#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "meshdata.h"

// Splits captured meshes into semantic objects (buildings, cars, trees,
// plants, water, road, ground) for later per-object AI detail enhancement.
//
// Pipeline (all in the scene's metric, Z-up frame):
//   1. Rasterize the meshes top-down: colour from the captured textures,
//      surface height and the triangle under every pixel.
//   2. Ground model by morphological opening of the height map; height above
//      ground per pixel.
//   3. Regions: raised areas split into instances by height continuity, ground
//      split into colour superpixels (SLIC) and merged.
//   4. Qwen (vision model via Ollama) labels numbered regions tile by tile
//      ("set of marks"), told which regions are raised and which are ground.
//   5. Each triangle takes the object of the region under it (walls join
//      their building via height above ground); meshes are then split.

struct SegmentSettings
{
    std::string server = "http://127.0.0.1:11434";
    std::string model = "qwen3.8:latest";
    double      metresPerPixel = 0.25;   // orthophoto resolution
    double      groundRadius = 40.0;     // m; buildings narrower than 2x this are lifted off the ground model
    double      raisedHeight = 1.2;      // m above ground to count as a raised object
    int         tileSize = 768;          // px per model request (Qwen3.x vision input)
    bool        useModel = true;         // false: geometry/colour fallback only (no Qwen)
    std::string debugDir = "C:\\Users\\Public\\meshtool_segment";
};

struct SegmentResult
{
    bool        ok = false;
    std::string error;
    std::string summary;

    // Objects across all segmented groups, and for every segmented triangle
    // list (mesh -> triangle index / 3) the object it belongs to, -1 = none.
    std::vector<SceneObject> objects;
    struct MeshAssign
    {
        GroupMeshData* group = nullptr;
        MeshData*      mesh = nullptr;
        std::vector<int32_t> triangleObject;
    };
    std::vector<MeshAssign> assignments;
};

class Segmenter
{
public:
    ~Segmenter();

    // Starts segmenting `groups` in the background. Their meshes must not
    // change until the job has finished and its result was applied.
    bool Start(const std::vector<GroupMeshData*>& groups, const SegmentSettings& settings);
    void Cancel() { m_cancel = true; }
    bool Running() const { return m_running; }
    float Progress() const { return m_progress; }
    std::string Status() const;
    // The finished result (once Running() is false), else nullptr.
    std::unique_ptr<SegmentResult> TakeResult();

private:
    std::thread                     m_thread;
    std::atomic<bool>               m_running{ false };
    std::atomic<bool>               m_cancel{ false };
    std::atomic<float>              m_progress{ 0.0f };
    mutable std::mutex              m_mutex;
    std::string                     m_status;
    std::unique_ptr<SegmentResult>  m_result;

    void Run(std::vector<GroupMeshData*> groups, SegmentSettings settings);
    void SetStatus(const std::string& s, float progress);
};

// Replaces each segmented mesh by one mesh per object it touches and fills
// the groups' object tables. Returns the replaced meshes; the caller frees
// their GPU data and deletes them.
std::vector<MeshData*> ApplySegmentation(const SegmentResult& result);
