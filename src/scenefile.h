#pragma once

#include <string>
#include <vector>

struct BatchMeshData;

// Native MeshTool scene file (.mtscene): every batch, group and mesh with its
// vertices, UVs, colors, index lists and world translation, plus each group's
// textures in their original (often DXT-compressed) form. Lossless, so a
// saved capture loads back exactly as it was captured. Segmentation is embedded
// in the same file: object names/classes and every mesh's object assignment,
// including materials from refinement. Object bounds are reconstructed on load;
// no separate segmentation export or model rerun is needed. Version 5 also
// embeds original/refined mesh variants for instant in-editor comparison.
//
// Loaded meshes come back without GPU resources (tex_id unset); the caller
// uploads textures and maps idx_in_texture_list -> tex_id.

bool SaveScene(const std::string& path, const std::vector<BatchMeshData*>& batches, std::string& error);
bool LoadScene(const std::string& path, std::vector<BatchMeshData*>& outBatches, std::string& error);
