#include "refiner.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

namespace
{
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    std::string Quote(const std::string& s) { return "\"" + s + "\""; }
}

BuildingRefiner::~BuildingRefiner()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

std::string BuildingRefiner::FindScript()
{
    std::vector<fs::path> candidates;
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
    {
        // build/Release/MeshTool.exe -> <repo>/tools/building_refine/refine.py
        fs::path dir = fs::path(exe).parent_path();
        for (int up = 0; up < 4 && !dir.empty(); up++, dir = dir.parent_path())
            candidates.push_back(dir / "tools" / "building_refine" / "refine.py");
    }
    candidates.push_back(fs::current_path() / "tools" / "building_refine" / "refine.py");
    std::error_code ec;
    for (const fs::path& p : candidates)
        if (fs::exists(p, ec))
            return p.string();
    return std::string();
}

bool BuildingRefiner::Start(const std::string& inputPath, const RefineSettings& settings)
{
    if (m_running)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancel = false;
    m_progress = 0.0f;
    m_result.reset();
    m_running = true;
    SetStatus("Starting refine.py...");
    m_thread = std::thread(&BuildingRefiner::Run, this, inputPath, settings);
    return true;
}

std::string BuildingRefiner::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void BuildingRefiner::SetStatus(const std::string& s)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = s;
}

std::unique_ptr<RefineResult> BuildingRefiner::TakeResult()
{
    if (m_running)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::move(m_result);
}

void BuildingRefiner::Run(std::string inputPath, RefineSettings settings)
{
    auto result = std::make_unique<RefineResult>();
    auto finish = [&](bool ok, const std::string& error) {
        result->ok = ok;
        result->error = error;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result = std::move(result);
        }
        m_running = false;
    };

    const std::string script = FindScript();
    if (script.empty())
        return finish(false, "tools\\building_refine\\refine.py not found next to MeshTool.");

    // MESHTOOL_PYTHON picks the interpreter (default: python on PATH).
    const char* py = getenv("MESHTOOL_PYTHON");
    const std::string python = py && *py ? py : "python";
    // refine.py runs in its own folder: give it absolute paths.
    std::error_code ec;
    const fs::path work = fs::absolute(settings.workDir, ec);
    inputPath = fs::absolute(inputPath, ec).string();
    result->outputPath = (work / "refined.mtscene").string();
    const std::string logPath = (work / "refine.log").string();
    std::string cmd = Quote(python) + " -u " + Quote(script) + " " + Quote(inputPath) + " " +
                      Quote(result->outputPath) + " --progress";
    if (!settings.glass)
        cmd += " --no-glass";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return finish(false, "Cannot create a pipe for refine.py.");
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring wcmd = Widen(cmd);
    const std::wstring wdir = Widen(fs::path(script).parent_path().string());
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        wdir.c_str(), &si, &pi))
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return finish(false, "Cannot start Python (" + python + "). Install Python or set MESHTOOL_PYTHON.");
    }
    CloseHandle(writePipe);
    CloseHandle(pi.hThread);

    FILE* log = fopen(logPath.c_str(), "w");
    std::string pending, lastLine, summary;
    std::vector<std::string> tail;
    char buf[4096];
    for (;;)
    {
        if (m_cancel)
        {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr))
            break;                                      // process exited, pipe closed
        if (avail == 0)
        {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
            {
                PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr);
                if (avail == 0) break;
            }
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) || got == 0)
            break;
        if (log) fwrite(buf, 1, got, log);
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos)
        {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // "@progress <done> <total> <message>"
            if (line.rfind("@progress ", 0) == 0)
            {
                std::istringstream ss(line.substr(10));
                int done = 0, total = 0;
                ss >> done >> total;
                std::string msg;
                std::getline(ss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                if (total > 0) m_progress = float(done) / float(total);
                SetStatus(msg);
                continue;
            }
            if (line.rfind("Refined ", 0) == 0)
                summary = line;
            if (!line.empty())
            {
                tail.push_back(line);
                if (tail.size() > 4) tail.erase(tail.begin());
            }
        }
    }
    if (log) fclose(log);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    if (m_cancel)
        return finish(false, "Cancelled.");
    if (code != 0 || summary.empty())
    {
        std::string why = tail.empty() ? "no output" : tail.back();
        return finish(false, "refine.py failed (" + why + "); log: " + logPath);
    }
    result->summary = summary;
    m_progress = 1.0f;
    finish(true, "");
}
