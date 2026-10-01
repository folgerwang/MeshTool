#pragma once

#include <string>
#include <vector>

struct BatchMeshData;

// Native MeshTool scene file (.mtscene): every batch, group and mesh with its
// vertices, UVs, colors, index lists and world translation, plus each group's
// textures in their original (often DXT-compressed) form. Lossless, so a
// saved capture loads back exactly as it was captured.
//
// Loaded meshes come back without GPU resources (tex_id unset); the caller
// uploads textures and maps idx_in_texture_list -> tex_id.

bool SaveScene(const std::string& path, const std::vector<BatchMeshData*>& batches, std::string& error);
bool LoadScene(const std::string& path, std::vector<BatchMeshData*>& outBatches, std::string& error);
