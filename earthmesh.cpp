#include "coremath.h"
#include "coregeographic.h"
#include "coretexture.h"
#include "meshdata.h"
#include "worlddata.h"
#include "oglwidget.h"
#include "opencv2/opencv.hpp"
#include <QLoggingCategory>
#include "debugout.h"

uint32_t get_index(int lon, int lat)
{
    if (lat == -90)
    {
        return 0;
    }
    else if (lat == 90)
    {
        return 1 + 179 * (360 + 1);
    }
    else
    {
        return uint32_t(1 + (lat - (-89)) * (360 + 1) + lon);
    }
}

void earth_mesh_init()
{
    if (!g_world.earth_mesh)
    {
        g_world.earth_mesh = new MeshData;
        g_world.earth_mesh->num_vertex = 2 + 179 * (360 + 1);
        g_world.earth_mesh->vertex_list = make_unique<core::vec3f[]>(uint32_t(g_world.earth_mesh->num_vertex));
        g_world.earth_mesh->uv_list = make_unique<core::vec2f[]>(uint32_t(g_world.earth_mesh->num_vertex));

        // filling earth vertex stream.
        uint32_t idx = get_index(0, -90);
        core::vec3d pos = core::GetCartesianPosition(-90, -180, 0, false);
        g_world.earth_mesh->vertex_list[idx] = pos;
        g_world.earth_mesh->bbox_ws += g_world.earth_mesh->vertex_list[idx];
        for (int lat = -89; lat <= 89; lat++)
        {
            for (int lon = 0; lon <= 360; lon++)
            {
                idx = get_index(lon, lat);
                pos = core::GetCartesianPosition(lat, lon - 180, 0, false);
                g_world.earth_mesh->vertex_list[idx] = pos;
                g_world.earth_mesh->bbox_ws += g_world.earth_mesh->vertex_list[idx];
                g_world.earth_mesh->uv_list[idx] = core::vec2f(lon / 360.f, 1.0f - (lat + 90) / 180.0f);
            }
        }
        idx = get_index(0, 90);
        pos = core::GetCartesianPosition(90, -180, 0, false);
        g_world.earth_mesh->vertex_list[idx] = pos;
        g_world.earth_mesh->bbox_ws += g_world.earth_mesh->vertex_list[idx];

        int32_t num_index = 360 * 3 * 2 + 178 * 360 * 3 * 2;
        g_world.earth_mesh->add_draw_call_list(kGlTriangles, num_index, g_world.earth_mesh->num_vertex);
        DrawCallInfo& last_draw_call_info = g_world.earth_mesh->get_last_draw_call_info();

        for (int lon = 0; lon < 360; lon++)
        {
            last_draw_call_info.add_index(get_index(0, -90));
            last_draw_call_info.add_index(get_index(lon + 1, -89));
            last_draw_call_info.add_index(get_index(lon, -89));
        }

        for (int lat = -89; lat < 89; lat++)
        {
            for (int lon = 0; lon < 360; lon++)
            {
                uint32_t idx00 = get_index(lon, lat);
                uint32_t idx01 = get_index(lon, lat + 1);
                uint32_t idx10 = get_index(lon + 1, lat);
                uint32_t idx11 = get_index(lon + 1, lat + 1);
                last_draw_call_info.add_index(idx00);
                last_draw_call_info.add_index(idx11);
                last_draw_call_info.add_index(idx01);
                last_draw_call_info.add_index(idx00);
                last_draw_call_info.add_index(idx10);
                last_draw_call_info.add_index(idx11);
            }
        }

        for (int lon = 0; lon < 360; lon++)
        {
            last_draw_call_info.add_index(get_index(0, 90));
            last_draw_call_info.add_index(get_index(lon, 89));
            last_draw_call_info.add_index(get_index(lon + 1, 89));
        }

//        QLoggingCategory category("bbox");
//        qCInfo(category) << g_world.earth_mesh->bbox_ws.bb_min.x << g_world.earth_mesh->bbox_ws.bb_min.y << g_world.earth_mesh->bbox_ws.bb_min.z <<
//                            g_world.earth_mesh->bbox_ws.bb_max.x << g_world.earth_mesh->bbox_ws.bb_max.y << g_world.earth_mesh->bbox_ws.bb_max.z;

        g_world.earth_tex_info = make_unique<core::Texture2DInfo>();
        LoadTextureFromFile("land_ocean_ice_8192.png", g_world.earth_tex_info.get());
    }
}


