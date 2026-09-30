#pragma once

#include <string>
#include "coremath.h"
#include "meshtexture.h"
#include "meshdata.h"
#include "progress.h"

struct GroupMeshData;
struct BatchMeshData;
class FileDownloader;

void earth_mesh_init();
void CreateMeshFromDumpFile(const string& dump_file_name, GroupMeshData* group_mesh_data);
void DumpGoogleEarthMeshes(const vector<string>& file_name_list, BatchMeshData* batch_mesh_data, IProgressCallback* progress);
void DumpKmlSplineMeshes(const vector<string>& file_name_list, BatchMeshData* batch_mesh_data, IProgressCallback* progress);
bool ExportGltfMeshFile(const string& file_name, const vector<BatchMeshData*>& batch_mesh_data, IProgressCallback* progress);
void ExportMaMeshFile(const string& file_name, const vector<BatchMeshData*>& batch_mesh_data, IProgressCallback* progress);
void DumpUSGSTexture(const vector<shared_ptr<string>>& file_name_list,
                     vector<DumppedTextureInfo>& dumpped_texture_info_list);
void DumpUSGSData(const vector<unique_ptr<string>>& file_name_list,
                  const vector<DumppedTextureInfo>& dumpped_texture_info_list,
                  bool split_texture,
                  BatchMeshData* batch_mesh_data);

void PreLoadUSGSData(FileDownloader *download,
                     const vector<unique_ptr<string>>& map_name_list,
                     vector<MapInfo>& map_info_list);

void GenerateMeshData(const vector<MapInfo>& map_info_list,
                      const uint32_t& grid_size_by_angle,
                      BatchMeshData* batch_mesh_data);
