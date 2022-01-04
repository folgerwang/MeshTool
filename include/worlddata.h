#pragma once
#include "meshdata.h"

struct WorldData
{
    core::vec2d         reference_pos; // tmp data.
    core::bounds2d      scissor_bbox; // tmp data.
    core::bounds3f      bbox_ws;
    core::bounds3d      bbox_gps;
    vector<BatchMeshData*> mesh_data_batches;
    MeshData*           earth_mesh;
    unique_ptr<core::Texture2DInfo> earth_tex_info;

    WorldData() : reference_pos(core::vec2d(0.0, 0.0)),
                  earth_mesh(nullptr),
                  earth_tex_info(nullptr)
    {}
};

extern WorldData g_world;
