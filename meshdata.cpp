#include "meshdata.h"

bool CullingCore(const core::bounds3f& bbox, const core::matrix4f& world_proj_mat, float scale)
{
    core::vec4f p[8];
    core::vec3f p_ss[8];
    core::bounds3f bbox_scaled;
    bbox_scaled.bb_min = bbox.bb_min * scale;
    bbox_scaled.bb_max = bbox.bb_max * scale;
    p[0] = world_proj_mat * core::vec4f(bbox_scaled.bb_min.x, bbox_scaled.bb_min.y, bbox_scaled.bb_min.z, 1.0);
    p[1] = world_proj_mat * core::vec4f(bbox_scaled.bb_max.x, bbox_scaled.bb_min.y, bbox_scaled.bb_min.z, 1.0);
    p[2] = world_proj_mat * core::vec4f(bbox_scaled.bb_min.x, bbox_scaled.bb_max.y, bbox_scaled.bb_min.z, 1.0);
    p[3] = world_proj_mat * core::vec4f(bbox_scaled.bb_max.x, bbox_scaled.bb_max.y, bbox_scaled.bb_min.z, 1.0);
    p[4] = world_proj_mat * core::vec4f(bbox_scaled.bb_min.x, bbox_scaled.bb_min.y, bbox_scaled.bb_max.z, 1.0);
    p[5] = world_proj_mat * core::vec4f(bbox_scaled.bb_max.x, bbox_scaled.bb_min.y, bbox_scaled.bb_max.z, 1.0);
    p[6] = world_proj_mat * core::vec4f(bbox_scaled.bb_min.x, bbox_scaled.bb_max.y, bbox_scaled.bb_max.z, 1.0);
    p[7] = world_proj_mat * core::vec4f(bbox_scaled.bb_max.x, bbox_scaled.bb_max.y, bbox_scaled.bb_max.z, 1.0);

    for (int i = 0; i < 8; i++)
    {
        if (p[i].w != 0.0f)
        {
            p_ss[i].x = p[i].x / p[i].w;
            p_ss[i].y = p[i].y / p[i].w;
            p_ss[i].z = p[i].z / p[i].w;
        }
        else
        {
            p_ss[i].x = 0.0f;
            p_ss[i].y = 0.0f;
            p_ss[i].z = 0.0f;
        }
    }

    core::vec3f min_p = min(min(min(p_ss[0], p_ss[1]), min(p_ss[2], p_ss[3])), min(min(p_ss[4], p_ss[5]), min(p_ss[6], p_ss[7])));
    core::vec3f max_p = max(max(max(p_ss[0], p_ss[1]), max(p_ss[2], p_ss[3])), max(max(p_ss[4], p_ss[5]), max(p_ss[6], p_ss[7])));

    bool result = true;

    if (min_p.x >= 1.0f || min_p.y >= 1.0f || min_p.z >= 1.0f ||
        max_p.x <= -1.0f || max_p.y <= -1.0f || max_p.z <= 0.0f)
    {
        result = false;
    }

    return result;
}


void MeshData::draw(CoreGLSLProgram* program)
{
    program;
}


bool MeshData::culling(const core::vec3d& reference_pos, const core::matrix4f& world_proj_mat, float scale)
{
/*    core::vec3f boundingsphere_center = bbox.GetCentroid();
    float boundingsphere_radius = bbox.GetRadius();

    if (core::length(cam_pos - boundingsphere_center) > (max_draw_dist + boundingsphere_radius))
    {
        return false;
    }*/

    core::bounds3f local_bbox;
    local_bbox += bbox_ws.bb_min - reference_pos;
    local_bbox += bbox_ws.bb_max - reference_pos;

    return CullingCore(local_bbox, world_proj_mat, scale);
}

void PatchCutting(const PatchInfo& patch_info,
                  const vector<MapInfo>& map_list,
                  const vector<TexInfo>& tex_list,
                  MeshData& mesh_data)
{
    for (uint32_t i = 0; i < map_list.size(); i++)
    {

    }
}

void PatchesCutting(const vector<core::bounds2d>& patch_list,
                    const vector<core::bounds2d>& map_list,
                    const vector<core::bounds2d>& tex_list,
                    vector<PatchInfo>& patch_info_list)
{
    for (uint32_t i_patch = 0; i_patch < patch_list.size(); i_patch++)
    {
        vector<uint32_t> map_idx_list;
        vector<uint32_t> tex_idx_list;
        for (uint32_t i_map = 0; i_map < map_list.size(); i_map++)
        {
            core::bounds2d intersect_bbox = patch_list[i_patch] ^ map_list[i_map];
            if (intersect_bbox.b_valid)
            {
                map_idx_list.push_back(i_map);
            }
        }

        for (uint32_t i_tex = 0; i_tex < tex_list.size(); i_tex++)
        {
            core::bounds2d intersect_bbox = patch_list[i_patch] ^ tex_list[i_tex];
            if (intersect_bbox.b_valid)
            {
                tex_idx_list.push_back(i_tex);
            }
        }

        PatchInfo patch_info;
        if (map_idx_list.size() > 0)
        {
            patch_info.map_idx_list = make_unique<uint32_t[]>(map_idx_list.size());
            memcpy(patch_info.map_idx_list.get(), map_idx_list.data(), sizeof(uint32_t) * map_idx_list.size());
        }

        if (tex_idx_list.size() > 0)
        {
            patch_info.tex_idx_list = make_unique<uint32_t[]>(tex_idx_list.size());
            memcpy(patch_info.tex_idx_list.get(), tex_idx_list.data(), sizeof(uint32_t) * tex_idx_list.size());
        }

        patch_info_list.push_back(move(patch_info));
    }
}
