#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Tools > Refine Buildings: replaces segmented buildings with clean models
// (straight walls on the captured facades, planar roofs, glass facades).
// The work is done by tools/building_refine/refine.py (SAM2, CLIP), run as a
// child process on a saved copy of the scene; the result is a new scene file.
struct RefineSettings
{
    bool        noSam = false; // optional CLI outline-only mode
    bool        pcg = false;
    std::string target; // exact batch:group:object index; empty means batch operation
    bool        glass = true;       // detect curtain-wall facades and give them the glass material
    bool        clean = false;      // clean scene: remove cars/clutter, rebuild ground and roads under everything
    bool        cull = false;       // remove hidden surfaces (always on with clean)
    std::string workDir = "C:\\Users\\Public\\meshtool_segment";   // input/output scenes and log
};

struct RefineResult
{
    bool        ok = false;
    std::string error;
    std::string outputPath;         // refined scene, ready to open
    std::string summary;            // "Refined 42 of 361 buildings ..."
};

class BuildingRefiner
{
public:
    ~BuildingRefiner();

    // Refines the scene saved at inputPath in the background.
    bool Start(const std::string& inputPath, const RefineSettings& settings);
    void Cancel() { m_cancel = true; }
    bool Running() const { return m_running; }
    float Progress() const { return m_progress; }
    std::string Status() const;
    // The finished result (once Running() is false), else nullptr.
    std::unique_ptr<RefineResult> TakeResult();

    // refine.py next to the executable's source tree, or "" if not found.
    static std::string FindScript();

private:
    std::thread                     m_thread;
    std::atomic<bool>               m_running{ false };
    std::atomic<bool>               m_cancel{ false };
    std::atomic<float>              m_progress{ 0.0f };
    mutable std::mutex              m_mutex;
    std::string                     m_status;
    std::unique_ptr<RefineResult>   m_result;

    void Run(std::string inputPath, RefineSettings settings);
    void SetStatus(const std::string& s);
};
