#pragma once

#include <string>
#include <vector>

struct BatchMeshData;
struct RoutePlan;

// Identity belongs to the captured scene, not its filename or processing stage.
struct SceneCapture
{
    std::string id;
    std::string name;
    std::string createdUtc; // identity creation time; not inferred historical capture time
};
SceneCapture NewSceneCapture();
bool SaveCaptureFolder(const std::string& parent, const std::vector<BatchMeshData*>& batches,
                       const SceneCapture& capture, std::string& scenePath, std::string& error,
                       const RoutePlan* route = nullptr);

// Native MeshTool scene file (.mtscene): every batch, group and mesh with its
// vertices, UVs, colors, index lists and world translation, plus each group's
// textures in their original (often DXT-compressed) form. Lossless, so a
// saved capture loads back exactly as it was captured. Segmentation is embedded
// in the same file: object names/classes and every mesh's object assignment,
// including materials from refinement. Object bounds are reconstructed on load;
// no separate segmentation export or model rerun is needed. Version 5 also
// embeds original/refined mesh variants for instant in-editor comparison.
//
// Version 6 also stores the driving route a route capture followed (polyline,
// end names, length), so a drive simulation can run on the saved scene.
// Version 8 preserves per-group tree planting records after tree removal.
// Version 9 adds the scene's permanent capture ID, name and identity creation UTC.
// With no capture identity, use v8 for planting records, otherwise v7.
//
// Loaded meshes come back without GPU resources (tex_id unset); the caller
// uploads textures and maps idx_in_texture_list -> tex_id.

// `route` (optional): saved after the meshes when it has a polyline; on load
// filled in when the file carries one (ok = true), else left untouched.
bool SaveScene(const std::string& path, const std::vector<BatchMeshData*>& batches, std::string& error,
               const RoutePlan* route = nullptr, const SceneCapture* capture = nullptr);
bool LoadScene(const std::string& path, std::vector<BatchMeshData*>& outBatches, std::string& error,
               RoutePlan* route = nullptr, SceneCapture* capture = nullptr, bool preserveMeshes = false);
