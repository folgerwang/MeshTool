// test_inject.cpp -- standalone tester for the injectable GL hook.
//
// Injects meshtool_hook.dll into an already-running process (Google Earth Pro
// by default) without going through the MeshTool UI. Useful for debugging the
// hook in isolation; check C:\Users\Public\meshtool_hook.log afterwards.
//
// Usage:
//   test_inject                         -> inject into googleearth.exe
//   test_inject <pid|process.exe>       -> inject into that process
//   test_inject <pid|process.exe> <dll> -> inject a specific DLL

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "glhook_injector.h"

static DWORD FindProcessByName(const char* exeName)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    // Use the explicit wide API: the project defines UNICODE (as 0), which
    // still makes the un-suffixed Toolhelp names map to their W versions.
    wchar_t wname[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, exeName, -1, wname, MAX_PATH);

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, wname) == 0)
            {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static std::string DefaultHookPath()
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos)
        dir = dir.substr(0, slash + 1);
    return dir + "meshtool_hook.dll";
}

int main(int argc, char** argv)
{
    const char* target = (argc > 1) ? argv[1] : "googleearth.exe";
    std::string dllPath = (argc > 2) ? argv[2] : DefaultHookPath();

    DWORD pid = 0;
    char* end = nullptr;
    unsigned long asNumber = strtoul(target, &end, 10);
    if (end && *end == '\0' && asNumber != 0)
        pid = static_cast<DWORD>(asNumber);
    else
        pid = FindProcessByName(target);

    if (pid == 0)
    {
        fprintf(stderr, "Process '%s' not found. Start it first.\n", target);
        return 1;
    }

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "Hook DLL not found: %s\n", dllPath.c_str());
        return 1;
    }

    printf("Injecting %s into PID %lu ...\n", dllPath.c_str(), static_cast<unsigned long>(pid));
    if (!InjectDLL(pid, dllPath))
    {
        fprintf(stderr, "Injection failed (error %lu). Try running as administrator.\n",
                static_cast<unsigned long>(GetLastError()));
        return 1;
    }

    printf("Injected OK. See C:\\Users\\Public\\meshtool_hook.log\n");
    return 0;
}
