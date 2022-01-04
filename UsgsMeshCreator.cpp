#include "assert.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include "opencv2/opencv.hpp"

#include <QLoggingCategory>
#include <QUrlQuery>
#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "GpaDumpAnalyzeTool.h"
#include "coregeographic.h"
#include "meshdata.h"
#include "meshtexture.h"
#include "worlddata.h"
#include "kmlfileparser.h"
#include "imagedownload.h"
#include <fbxsdk.h>
#include "hfa/hfa_p.h"
#include "hfa/hfa.h"

using namespace std;
namespace fs = experimental::filesystem::v1;

constexpr int TILE_SIZE = 512;
constexpr int MARGIN_SIZE = 22;
void loadMapTexture(FileDownloader *download, double lon_, double lat_, int width_, int height_, int zoom_level_)
{
    QString long_str = QString::number(lon_);
    QString lat_str = QString::number(lat_);
    QString zoom_str = QString::number(zoom_level_);
    QString width_str = QString::number(width_);
    QString height_str = QString::number(height_+MARGIN_SIZE*2);

    QString url_string = "https://maps.googleapis.com/maps/api/staticmap";
         url_string += "?center=" + lat_str + "," + long_str;
         url_string += "&zoom=" + zoom_str;
         url_string += "&size=" + width_str + "x" + height_str;
         url_string += "&maptype=satellite";
         url_string += "&key=AIzaSyAQSf40otiucpCT6HwYvIQuX2jqwhqOVsM";

    QString file_name = "lon@" + long_str + "_lat@" + lat_str + "_zoom@" + zoom_str;

    download->downloadFile(QUrl(url_string), file_name, QString("C:/Users/fenwang"));
}

core::vec2d project(double lon_, double lat_)
{
  double siny = std::sin(lat_ * core::PI / 180.0);

  // Truncating to 0.9999 effectively limits latitude to 89.189. This is
  // about a third of a tile past the edge of the world tile.
  siny = std::min(std::max(siny, -0.9999), 0.9999);

  return core::vec2d(
      TILE_SIZE * (0.5 + lon_ / 360.0),
      TILE_SIZE * (0.5 - std::log((1 + siny) / (1 - siny)) / (4 * core::PI)));
}

void createInfoWindowContent(double lon_, double lat_, int zoom, core::vec2d& pixel_coord_, core::vec2d& tile_coord_)
{
    double scale = 1 << zoom;

    core::vec2d worldCoordinate = project(lon_, lat_);

    pixel_coord_ = worldCoordinate * scale;
    tile_coord_ = worldCoordinate * scale / TILE_SIZE;
}

core::vec2d GetSubImageInfo(core::vec2i sample_idx, core::vec2i num_samples, core::vec2d min_idx, core::vec2d max_idx, core::vec2d ofs, core::vec2d pixel_size)
{
    core::vec2d img_index;
    img_index.x = sample_idx.x == 0 ? min_idx.x : (sample_idx.x == num_samples.x - 1 ? max_idx.x : sample_idx.x + floor(min_idx.x));
    img_index.y = sample_idx.y == 0 ? min_idx.y : (sample_idx.y == num_samples.y - 1 ? max_idx.y : sample_idx.y + floor(min_idx.y));

    return ofs + pixel_size * img_index;
}

float LookupAltitudeMap(double s_x, double s_y, const float* height_map, int nXSize)
{
    int x_low = int32_t(floor(s_x));
    int y_low = int32_t(floor(s_y));

    float h_00 = height_map[y_low * nXSize + x_low];
    float h_01 = height_map[y_low * nXSize + x_low + 1];
    float h_10 = height_map[(y_low + 1) * nXSize + x_low];
    float h_11 = height_map[(y_low + 1) * nXSize + x_low + 1];

    double s_x_ofs = s_x - double(x_low);
    double s_y_ofs = s_y - double(y_low);

    h_00 = h_00 * (1.0f - float(s_x_ofs)) + h_01 * float(s_x_ofs);
    h_10 = h_10 * (1.0f - float(s_x_ofs)) + h_11 * float(s_x_ofs);
    float h = h_00 * (1.0f - float(s_y_ofs)) + h_10 * float(s_y_ofs);

    return h;
}

void PreLoadUSGSData(FileDownloader *download,
                     const vector<unique_ptr<string>>& map_name_list,
                     vector<MapInfo>& map_info_list)
{
    for (uint32_t i_map = 0; i_map < map_name_list.size(); i_map++)
    {
        HFAHandle	hHFA;
        int	nXSize, nYSize, nBands;
        const Eprj_MapInfo *psMapInfo;

        string* file_name = map_name_list[i_map].get();
        hHFA = HFAOpen(file_name->c_str(), "r");

        if (hHFA)
        {
            HFADumpDictionary(hHFA, stdout);
            HFADumpTree(hHFA, stdout);
            HFAGetRasterInfo(hHFA, &nXSize, &nYSize, &nBands);

            psMapInfo = HFAGetMapInfo(hHFA);
            if (psMapInfo)
            {
                Eprj_Coordinate ul_center = psMapInfo->upperLeftCenter;
                Eprj_Size pixel_size = psMapInfo->pixelSize;

                core::vec2d s_pixel_size(pixel_size.width, -pixel_size.height);
                core::vec2d img_ul_center(ul_center.x, ul_center.y);

                HFABand ** papoBandList = hHFA->papoBand;
                for (int b = 1; b <= nBands; b++)
                {
                    int	nDataType, nOverviews;
                    int nBlockXSize, nBlockYSize, nCompressionType;

                    HFAGetBandInfo(hHFA, b, &nDataType, &nBlockXSize, &nBlockYSize,
                        &nOverviews, &nCompressionType);

                    printf("Band %d: %dx%d tiles, type = %d\n",
                        b, nBlockXSize, nBlockYSize, nDataType);

                    int nBlocksPerRow = (nXSize + nBlockXSize - 1) / nBlockXSize;
                    int nBlocksPerColumn = (nYSize + nBlockYSize - 1) / nBlockYSize;

                    HFABand* poBand = papoBandList[b - 1];
                    int nDataBits = HFAGetDataTypeBits(poBand->nDataType);

                    double t_min = 100000.0;
                    double t_max = -100000.0;
                    uint8_t *pabyTile = new uint8_t[uint32_t(nBlockXSize * nBlockYSize * nDataBits / 8)];
                    MapInfo map_info;
                    map_info.alt_list = make_unique<float[]>(uint32_t(nXSize * nYSize));

                    if (pabyTile)
                    {
                        for (int b_y = 0; b_y < nBlocksPerColumn; b_y++)
                        {
                            for (int b_x = 0; b_x < nBlocksPerRow; b_x++)
                            {
                                if (poBand->GetRasterBlock(b_x, b_y, pabyTile) != CE_Failure)
                                {
                                    float* f_height = reinterpret_cast<float*>(pabyTile);
                                    for (int i = 0; i < nBlockYSize; i++)
                                    {
                                        int y = b_y * nBlockYSize + i;
                                        for (int j = 0; j < nBlockXSize; j++)
                                        {
                                            int x = b_x * nBlockXSize + j;
                                            if (y < nYSize && x < nXSize)
                                            {
                                                float h = f_height[i * nBlockXSize + j];
                                                map_info.alt_list[uint32_t(y * nXSize + x)] = h;
                                                t_min = min(t_min, double(h));
                                                t_max = max(t_max, double(h));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    map_info.ul_corner = img_ul_center;
                    map_info.pixel_size = s_pixel_size;
                    map_info.pixel_count = core::vec2i(nXSize, nYSize);

                    const core::vec2d& ul_corner = img_ul_center;
                    const core::vec2d& lr_corner = ul_corner + core::vec2d(s_pixel_size.x * (nXSize - 1), s_pixel_size.y * (nYSize - 1));

                    int zoom = 14;
                    core::vec2d pixel_coord_ul, tile_coord_ul;
                    createInfoWindowContent(ul_corner.x, ul_corner.y, zoom, pixel_coord_ul, tile_coord_ul);

                    core::vec2d pixel_coord_lr, tile_coord_lr;
                    createInfoWindowContent(lr_corner.x, lr_corner.y, zoom, pixel_coord_lr, tile_coord_lr);

                    core::vec2d pixel_ofs = (pixel_coord_lr - pixel_coord_ul);
                    double t = std::log2(std::max(pixel_ofs.x, pixel_ofs.y) / TILE_SIZE);

                    loadMapTexture(download, (ul_corner.x + lr_corner.x) * 0.5, (ul_corner.y + lr_corner.y) * 0.5, TILE_SIZE, TILE_SIZE, std::max(int(zoom - t), 1));

                    //QLoggingCategory category("image size");
                    //qCInfo(category) << img_ul_center.x << img_ul_center.y << s_pixel_size.x << s_pixel_size.y << s_pixel_size.x * nXSize << s_pixel_size.y * nYSize << t_min << t_max;

                    map_info_list.push_back(move(map_info));
                }
            }
        }
    }
}

void GenerateMeshData(const vector<MapInfo>& map_info_list,
                      const uint32_t& grid_size_by_angle,
                      BatchMeshData* batch_mesh_data)
{
    core::CoordinateTransformer gps_to_env_cnvt(batch_mesh_data->reference_pos.y, batch_mesh_data->reference_pos.x, 0.0);

    for (uint32_t i_map = 0; i_map < map_info_list.size(); i_map++)
    {
        GroupMeshData* group_mesh_data = new GroupMeshData;
        const core::vec2d& ul_corner = map_info_list[i_map].ul_corner;
        const core::vec2d& lr_corner = ul_corner + core::vec2d(map_info_list[i_map].pixel_size.x * (map_info_list[i_map].pixel_count.x - 1),
                                                               map_info_list[i_map].pixel_size.y * (map_info_list[i_map].pixel_count.y - 1));

        core::bounds2d bbox;
        bbox += ul_corner * grid_size_by_angle;
        bbox += lr_corner * grid_size_by_angle;

        core::bounds2i i_bbox;
        i_bbox.bb_min.x = int(ceil(bbox.bb_min.x));
        i_bbox.bb_min.y = int(ceil(bbox.bb_min.y));
        i_bbox.bb_max.x = int(floor(bbox.bb_max.x));
        i_bbox.bb_max.y = int(floor(bbox.bb_max.y));

        int num_y = i_bbox.bb_max.y - i_bbox.bb_min.y + 1;
        int num_x = i_bbox.bb_max.x - i_bbox.bb_min.x + 1;

        // triangle mesh.
        MeshData* render_block = new MeshData;
        render_block->num_vertex = num_y * num_x;
        render_block->vertex_list = make_unique<core::vec3f[]>(uint32_t(render_block->num_vertex));

        int idx = 0;
        for (int y = i_bbox.bb_min.y; y <= i_bbox.bb_max.y; y++)
        {
            for (int x = i_bbox.bb_min.x; x <= i_bbox.bb_max.x; x++)
            {
                core::GpsCoord gps_coord(double(x) / grid_size_by_angle, double(y) / grid_size_by_angle, 0.0);
                render_block->vertex_list[uint32_t(idx)] = gps_to_env_cnvt.lla_to_enu(gps_coord);
                render_block->uv_list[uint32_t(idx)] = core::vec2f(0, 0);
                render_block->bbox_ws += render_block->vertex_list[uint32_t(idx)];
                idx ++;
            }
        }

        // create mesh draw call list.
        {
            int num_index = (num_y - 1) * (num_x - 1) * 6;
            render_block->add_draw_call_list(kGlTriangles, num_index, render_block->num_vertex);
            DrawCallInfo& last_draw_call_info = render_block->get_last_draw_call_info();
            for (int y = 0; y < num_y - 1; y++)
            {
                for (int x = 0; x < num_x - 1; x++)
                {
                    last_draw_call_info.add_index(uint32_t(y * num_x + x));
                    last_draw_call_info.add_index(uint32_t(y * num_x + x + 1));
                    last_draw_call_info.add_index(uint32_t((y + 1) * num_x + x + 1));
                    last_draw_call_info.add_index(uint32_t(y * num_x + x));
                    last_draw_call_info.add_index(uint32_t((y + 1) * num_x + x + 1));
                    last_draw_call_info.add_index(uint32_t((y + 1) * num_x + x));
                }
            }
        }

        // create outlines.
        {
            int num_index = num_y * (num_x - 1) * 2 + num_x * (num_y - 1) * 2;
            render_block->add_draw_call_list(kGlLines, num_index, render_block->num_vertex);
            DrawCallInfo& last_draw_call_info = render_block->get_last_draw_call_info();
            for (int y = 0; y < num_y; y++)
            {
                for (int x = 0; x < num_x - 1; x++)
                {
                    last_draw_call_info.add_index(uint32_t(y * num_x + x));
                    last_draw_call_info.add_index(uint32_t(y * num_x + x + 1));
                }
            }

            for (int x = 0; x < num_x; x++)
            {
                for (int y = 0; y < num_y - 1; y++)
                {
                    last_draw_call_info.add_index(uint32_t(y * num_x + x));
                    last_draw_call_info.add_index(uint32_t((y + 1) * num_x + x));
                }
            }
        }

        // create quad lists.
        {
            int num_index = (num_y - 1) * (num_x - 1) * 4;
            render_block->add_draw_call_list(kGlQuads, num_index, render_block->num_vertex);
            render_block->patch_list.reserve(uint32_t(num_index / 4));
            render_block->patch_list.resize(uint32_t(num_index / 4));
            DrawCallInfo& last_draw_call_info = render_block->get_last_draw_call_info();
            last_draw_call_info.set_drawable(false);
            for (int y = 0; y < num_y - 1; y++)
            {
                for (int x = 0; x < num_x - 1; x++)
                {
                    last_draw_call_info.add_index(uint32_t(y * num_x + x));
                    last_draw_call_info.add_index(uint32_t(y * num_x + x + 1));
                    last_draw_call_info.add_index(uint32_t((y + 1) * num_x + x + 1));
                    last_draw_call_info.add_index(uint32_t((y + 1) * num_x + x));
                }
            }
        }

        // create patch mask lists.
        {
            int num_index = (num_y - 1) * (num_x - 1) * 6;
            render_block->add_draw_call_list(kGlTriangles, num_index, render_block->num_vertex);
            DrawCallInfo& last_draw_call_info = render_block->get_last_draw_call_info();
            last_draw_call_info.clear_index_buffer();
            last_draw_call_info.set_drawable(true);
            last_draw_call_info.set_drawable_patches(true);
        }

        group_mesh_data->meshes.push_back(render_block);
        group_mesh_data->bbox_ws += render_block->bbox_ws;

        batch_mesh_data->group_meshes.push_back(group_mesh_data);
        batch_mesh_data->bbox_ws += group_mesh_data->bbox_ws;
    }
}

struct ScissorMeshInfo
{
    core::bounds2d  bbox;
    int tex_id;
};

void DumpUSGSData(const vector<unique_ptr<string>>& file_name_list,
                  const vector<DumppedTextureInfo>& dumpped_texture_info_list,
                  bool split_texture,
                  BatchMeshData* batch_mesh_data)
{
    core::CoordinateTransformer gps_to_env_cnvt(batch_mesh_data->reference_pos.y, batch_mesh_data->reference_pos.x, 0.0);

    for (uint32_t i_map = 0; i_map < file_name_list.size(); i_map++)
    {
        HFAHandle	hHFA;
        int	nXSize, nYSize, nBands;
        const Eprj_MapInfo *psMapInfo;
        const Eprj_ProParameters *psProParameters;
        const Eprj_Datum *psDatum;

        string* file_name = file_name_list[i_map].get();
        hHFA = HFAOpen(file_name->c_str(), "r");

        if (hHFA)
        {
            HFADumpDictionary(hHFA, stdout);

            HFADumpTree(hHFA, stdout);

            HFAGetRasterInfo(hHFA, &nXSize, &nYSize, &nBands);

            psMapInfo = HFAGetMapInfo(hHFA);
            if (psMapInfo)
            {
                Eprj_Coordinate ul_center = psMapInfo->upperLeftCenter;
                //Eprj_Coordinate lr_center = psMapInfo->lowerRightCenter;

                Eprj_Size pixel_size = psMapInfo->pixelSize;
                vector<ScissorMeshInfo> scissor_mesh_info_list;

                core::vec2d s_pixel_size(pixel_size.width, -pixel_size.height);
                core::vec2d img_ul_center(ul_center.x, ul_center.y);

                HFABand ** papoBandList = hHFA->papoBand;
                for (int b = 1; b <= nBands; b++)
                {
                    int	nDataType, nColors, nOverviews;
                    double	*padfRed, *padfGreen, *padfBlue, *padfAlpha;
                    int nBlockXSize, nBlockYSize, nCompressionType;

                    HFAGetBandInfo(hHFA, b, &nDataType, &nBlockXSize, &nBlockYSize,
                        &nOverviews, &nCompressionType);

                    printf("Band %d: %dx%d tiles, type = %d\n",
                        b, nBlockXSize, nBlockYSize, nDataType);

                    int nBlocksPerRow = (nXSize + nBlockXSize - 1) / nBlockXSize;
                    int nBlocksPerColumn = (nYSize + nBlockYSize - 1) / nBlockYSize;

                    HFABand* poBand = papoBandList[b - 1];
                    int nDataBits = HFAGetDataTypeBits(poBand->nDataType);

                    double h_min = 0;
                    double h_max = 0;
                    double h_mean = 0;
                    double h_median = 0;
                    double h_mode = 0;
                    double h_stddev = 0;
                    HFAEntry *poStats = poBand->poNode->GetNamedChild("Statistics");
                    if (poStats)
                    {
                        h_min = poStats->GetDoubleField("minimum");
                        h_max = poStats->GetDoubleField("maximum");
                        h_mean = poStats->GetDoubleField("mean");
                        h_median = poStats->GetDoubleField("median");
                        h_mode = poStats->GetDoubleField("mode");
                        h_stddev = poStats->GetDoubleField("stddev");
                    }

                    double t_min = 100000.0;
                    double t_max = -100000.0;
                    uint8_t *pabyTile = new uint8_t[uint32_t(nBlockXSize * nBlockYSize * nDataBits / 8)];
                    float* height_map = new float[uint32_t(nXSize * nYSize)];

                    if (pabyTile)
                    {
                        GroupMeshData* group_mesh_data = new GroupMeshData;
                        for (uint32_t i_tex = 0; i_tex < dumpped_texture_info_list.size(); i_tex++)
                        {
                            group_mesh_data->texture_names.push_back(dumpped_texture_info_list[i_tex].file_name);
                        }

                        for (int b_y = 0; b_y < nBlocksPerColumn; b_y++)
                        {
                            for (int b_x = 0; b_x < nBlocksPerRow; b_x++)
                            {
                                if (poBand->GetRasterBlock(b_x, b_y, pabyTile) != CE_Failure)
                                {
                                    float* f_height = reinterpret_cast<float*>(pabyTile);
                                    for (int i = 0; i < nBlockYSize; i++)
                                    {
                                        int y = b_y * nBlockYSize + i;
                                        for (int j = 0; j < nBlockXSize; j++)
                                        {
                                            int x = b_x * nBlockXSize + j;
                                            if (y < nYSize && x < nXSize)
                                            {
                                                float h = f_height[i * nBlockXSize + j];
                                                height_map[y * nXSize + x] = h;
                                                t_min = min(t_min, double(h));
                                                t_max = max(t_max, double(h));
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        core::vec2d g = img_ul_center + s_pixel_size * core::vec2d(nXSize -1, nYSize -1);

                        core::bounds2d img_bbox;
                        img_bbox += img_ul_center;
                        img_bbox += g;

                        // if scissor rectangle not empty, then do intersect check.
                        if (batch_mesh_data->scissor_bbox.GetRadius() > 0.0001)
                        {
                            img_bbox = img_bbox ^ batch_mesh_data->scissor_bbox;
                        }

                        for (uint32_t i = 0; i < dumpped_texture_info_list.size(); i++)
                        {
                            core::bounds2d intersect_bbox = img_bbox ^ dumpped_texture_info_list[i].bbox;
                            if (intersect_bbox.b_valid)
                            {
                                ScissorMeshInfo scissor_mesh_info;
                                scissor_mesh_info.bbox = intersect_bbox;
                                scissor_mesh_info.tex_id = int32_t(i);

                                scissor_mesh_info_list.push_back(scissor_mesh_info);
                            }
                        }

                        const int NUM_SAMPLES_PER_BLOCK_X = 33;
                        const int NUM_SAMPLES_PER_BLOCK_Y = 33;
                        for (uint32_t i = 0; i < scissor_mesh_info_list.size(); i++)
                        {
                            ScissorMeshInfo& info = scissor_mesh_info_list[i];

                            cv::Mat src_tex;
                            if (split_texture)
                            {
                                src_tex = cv::imread(dumpped_texture_info_list[uint32_t(info.tex_id)].file_name.get()->c_str());
                            }

                            const DumppedTextureInfo* p_tex_info = &dumpped_texture_info_list[uint32_t(info.tex_id)];
                            string sub_img_name = *dumpped_texture_info_list[uint32_t(info.tex_id)].file_name.get();
                            sub_img_name = sub_img_name.substr(0, sub_img_name.rfind("."));

                            int w = src_tex.cols;
                            int h = src_tex.rows;

                            core::vec2d min_idx = (info.bbox.bb_min - img_ul_center) / s_pixel_size;
                            core::vec2d max_idx = (info.bbox.bb_max - img_ul_center) / s_pixel_size;

                            core::bounds2d img_bbox;
                            img_bbox += min_idx;
                            img_bbox += max_idx;

                            // y always decrease.
                            int i_min_x_idx = int32_t(floor(img_bbox.bb_min.x));
                            int i_min_y_idx = int32_t(floor(img_bbox.bb_min.y));
                            int i_max_x_idx = int32_t(ceil(img_bbox.bb_max.x));
                            int i_max_y_idx = int32_t(ceil(img_bbox.bb_max.y));

                            core::vec2i num_samples_per_block = core::vec2i(NUM_SAMPLES_PER_BLOCK_X, NUM_SAMPLES_PER_BLOCK_Y);
                            core::vec2i num_samples(i_max_x_idx - i_min_x_idx + 1, i_max_y_idx - i_min_y_idx + 1);
                            core::vec2i num_blocks = (num_samples - 1 + num_samples_per_block - 1) / num_samples_per_block;
                            core::vec2d tex_size(w, h);

                            uint32_t mesh_index = 0;
                            for (int b_y = 0; b_y < num_blocks.y; b_y++)
                            {
                                for (int b_x = 0; b_x < num_blocks.x; b_x++)
                                {
                                    core::vec2i block_start_ofs = core::vec2i(b_x, b_y) * num_samples_per_block;
                                    core::vec2i block_size = Min(num_samples_per_block + 1, num_samples - block_start_ofs);

                                    if (block_size.x > 1 && block_size.y > 1) // at least you need 2 lines to get triangles
                                    {
                                        MeshData* render_block = new MeshData;

                                        core::vec2i block_min_idx = block_start_ofs;
                                        core::vec2i block_max_idx = block_start_ofs + block_size - 1;

                                        core::vec2d pos_gps_0 = GetSubImageInfo(block_min_idx, num_samples, img_bbox.bb_min, img_bbox.bb_max, img_ul_center, s_pixel_size);
                                        core::vec2d pos_gps_1 = GetSubImageInfo(block_max_idx, num_samples, img_bbox.bb_min, img_bbox.bb_max, img_ul_center, s_pixel_size);

                                        core::vec2d bb_range = p_tex_info->bbox.bb_max - p_tex_info->bbox.bb_min;
                                        core::vec2d f_idx_in_tex_0 = (pos_gps_0 - p_tex_info->bbox.bb_min) / bb_range * tex_size;
                                        core::vec2d f_idx_in_tex_1 = (pos_gps_1 - p_tex_info->bbox.bb_min) / bb_range * tex_size;

                                        core::vec2i i_idx_in_tex_0, i_idx_in_tex_1;
                                        i_idx_in_tex_0.x = min(int32_t(floor(f_idx_in_tex_0.x)), w);
                                        i_idx_in_tex_0.y = min(int32_t(ceil(f_idx_in_tex_0.y)), h);
                                        i_idx_in_tex_1.x = min(int32_t(floor(f_idx_in_tex_1.x)), w);
                                        i_idx_in_tex_1.y = min(int32_t(ceil(f_idx_in_tex_1.y)), h);

                                        pos_gps_0 = core::vec2d(i_idx_in_tex_0.x, i_idx_in_tex_0.y) / tex_size * bb_range + p_tex_info->bbox.bb_min;
                                        pos_gps_1 = core::vec2d(i_idx_in_tex_1.x, i_idx_in_tex_1.y) / tex_size * bb_range + p_tex_info->bbox.bb_min;

                                        core::bounds2d bbox;
                                        if (split_texture)
                                        {
                                            bbox += pos_gps_0;
                                            bbox += pos_gps_1;

                                            core::bounds2i bbox_i;
                                            bbox_i += i_idx_in_tex_0;
                                            bbox_i += i_idx_in_tex_1;

                                            int32_t s_w = min(bbox_i.bb_max.x - bbox_i.bb_min.x + 1, w - bbox_i.bb_min.x);
                                            int32_t s_h = min(bbox_i.bb_max.y - bbox_i.bb_min.y + 1, h - bbox_i.bb_min.y);

                                            cv::Mat sub_img = cv::Mat(src_tex, cv::Rect(int(bbox_i.bb_min.x), int(h - bbox_i.bb_min.y - s_h), int(s_w), int(s_h)));

                                            string tex_file_name = sub_img_name + "_" + to_string(mesh_index) + ".png";
                                            render_block->tex_file_name = make_unique<string>(tex_file_name);

                                            cv::imwrite(tex_file_name, sub_img);
                                            sub_img.release();
                                        }
                                        else
                                        {
                                            bbox = p_tex_info->bbox;
                                            render_block->idx_in_texture_list = uint32_t(info.tex_id);
                                        }

                                        int num_triangle_strips = block_size.y - 1;
                                        render_block->num_vertex = block_size.x * block_size.y;
                                        int32_t num_index = block_size.x * 2 * num_triangle_strips + (num_triangle_strips - 1) * 2;
                                        render_block->vertex_list = make_unique<core::vec3f[]>(uint32_t(render_block->num_vertex));
                                        render_block->add_draw_call_list(kGlTriangleStrip, num_index, render_block->num_vertex);
                                        DrawCallInfo& last_draw_call_info = render_block->get_last_draw_call_info();
                                        render_block->uv_list = make_unique<core::vec2f[]>(uint32_t(render_block->num_vertex));
                                        render_block->idx_in_texture_list = uint32_t(info.tex_id);

                                        auto& vertex_list = render_block->vertex_list;

                                        int d_idx = 0;
                                        for (int i = 0; i < block_size.y; i++)
                                        {
                                            int y = block_start_ofs.y + i;
                                            for (int j = 0; j < block_size.x; j++)
                                            {
                                                int x = block_start_ofs.x + j;
                                                if (y < num_samples.y && x < num_samples.x)
                                                {
                                                    uint32_t idx = uint32_t(i * block_size.x + j);

                                                    double s_x = x == 0 ? img_bbox.bb_min.x : (x == num_samples.x - 1 ? img_bbox.bb_max.x : x + floor(img_bbox.bb_min.x));
                                                    double s_y = y == 0 ? img_bbox.bb_min.y : (y == num_samples.y - 1 ? img_bbox.bb_max.y : y + floor(img_bbox.bb_min.y));

                                                    float h = LookupAltitudeMap(s_x, s_y, height_map, nXSize);

                                                    double g_x = img_ul_center.x + s_pixel_size.x * s_x;
                                                    double g_y = img_ul_center.y + s_pixel_size.y * s_y;

                                                    vertex_list[idx] = gps_to_env_cnvt.lla_to_enu(core::GpsCoord(g_x, g_y, double(h)));

    #if USE_USGS_MAP_TEXTURE
                                                    core::vec2d utm_loc = FromGeographicCoord(core::vec2d(g_y, g_x), p_dumpped_tex->xml_info.zone);
                                                    utm_loc *= 1000.0; // convert km to m.
                                                    render_block->uv_list[idx] = p_dumpped_tex->tfw_info.FromUTMLocation(utm_loc, core::vec2f(1.0f / p_dumpped_tex->tex_body.cols, 1.0f / p_dumpped_tex->tex_body.rows));
    #else
                                                    double u = (g_x - bbox.bb_min.x) / (bbox.bb_max.x - bbox.bb_min.x);
                                                    double v = (g_y - bbox.bb_min.y) / (bbox.bb_max.y - bbox.bb_min.y);

                                                    u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
                                                    v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);

                                                    render_block->uv_list[idx] = core::vec2f(float(u), float(v));
    #endif
                                                    render_block->bbox_ws += vertex_list[idx];

                                                    if (i < num_triangle_strips)
                                                    {
                                                        last_draw_call_info.set_index(d_idx + 0, idx);
                                                        last_draw_call_info.set_index(d_idx + 1, idx + uint32_t(block_size.x));
                                                        d_idx += 2;
                                                    }
                                                }
                                            }

                                            if (i > 0 && i < num_triangle_strips)
                                            {
                                                last_draw_call_info.set_index(d_idx - (block_size.x + 1) * 2 + 1, last_draw_call_info.get_index(d_idx - (block_size.x + 1) * 2 + 2));
                                            }

                                            if (i < num_triangle_strips - 1)
                                            {
                                                last_draw_call_info.set_index(d_idx, last_draw_call_info.get_index(d_idx - 1));
                                                d_idx += 2;
                                            }
                                        }

                                        render_block->translation = render_block->bbox_ws.GetCentroid();
                                        core::vec3f center = render_block->translation;
                                        for (uint32_t i = 0; i < uint32_t(render_block->num_vertex); i++)
                                        {
                                            vertex_list[i] -= center;
                                        }

                                        group_mesh_data->meshes.push_back(render_block);
                                        group_mesh_data->bbox_ws += render_block->bbox_ws;
                                        mesh_index++;
                                    }
                                }
                            }

                            src_tex.release();
                        }

                        batch_mesh_data->group_meshes.push_back(group_mesh_data);
                        batch_mesh_data->bbox_ws += group_mesh_data->bbox_ws;
                    }

                    if (pabyTile)
                    {
                        delete[] pabyTile;
                        pabyTile = nullptr;
                    }

                    if (height_map)
                    {
                        delete[] height_map;
                        height_map = nullptr;
                    }

                    if (HFAGetPCT(hHFA, b, &nColors, &padfRed, &padfGreen, &padfBlue, &padfAlpha) == CE_None)
                    {
                        int	j;

                        for (j = 0; j < nColors; j++)
                        {
                            printf("PCT[%d] = %f,%f,%f %f\n",
                                j, padfRed[j], padfGreen[j],
                                padfBlue[j], padfAlpha[j]);
                        }
                    }
                }

                psProParameters = HFAGetProParameters(hHFA);

                psDatum = HFAGetDatum(hHFA);
            }

            HFAClose(hHFA);
        }
    }
}
