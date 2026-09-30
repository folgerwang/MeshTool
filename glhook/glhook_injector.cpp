#include "glhook_injector.h"
#include <cstdio>

bool InjectDLL(DWORD processId, const std::string& dllPath)
{
    // Open target process
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE, processId);

    if (!hProcess)
    {
        fprintf(stderr, "InjectDLL: OpenProcess failed, error=%lu\n", GetLastError());
        return false;
    }

    // Allocate memory in target process for the DLL path string
    size_t pathLen = dllPath.size() + 1;
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
    {
        fprintf(stderr, "InjectDLL: VirtualAllocEx failed, error=%lu\n", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    // Write DLL path into target process memory
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), pathLen, nullptr))
    {
        fprintf(stderr, "InjectDLL: WriteProcessMemory failed, error=%lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Get LoadLibraryA address (same in all processes)
    FARPROC loadLibAddr = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!loadLibAddr)
    {
        fprintf(stderr, "InjectDLL: GetProcAddress(LoadLibraryA) failed\n");
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Create remote thread that calls LoadLibraryA(dllPath)
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadLibAddr, remoteMem, 0, nullptr);

    if (!hThread)
    {
        fprintf(stderr, "InjectDLL: CreateRemoteThread failed, error=%lu\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Wait for the remote thread to finish loading our DLL
    WaitForSingleObject(hThread, 10000);

    // Cleanup
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    fprintf(stderr, "InjectDLL: Successfully injected %s into PID %lu\n", dllPath.c_str(), processId);
    return true;
}
