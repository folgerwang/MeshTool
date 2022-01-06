#include "assert.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include "opencv2/opencv.hpp"

#include <QMessageBox>
#include <QProgressBar>
#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "glfunctionlist.h"
#include "GpaDumpAnalyzeTool.h"
#include "meshdata.h"
#include "worlddata.h"
#include "kmlfileparser.h"
#include <fbxsdk.h>
#include "debugout.h"

#include <windows.h>
#pragma comment(lib, "rpcrt4.lib")

string GetUUID()
{
    UUID uuid;
    UuidCreate(&uuid);
    char *str;
    UuidToStringA(&uuid, reinterpret_cast<RPC_CSTR*>(&str));

    return string(str);
}

namespace fs = std::filesystem;

void AddMaterial(fbxsdk::FbxScene* pScene, fbxsdk::FbxNode* pMeshNode, size_t group_idx, size_t material_idx, const string& tex_name, const string& relative_tex_name)
{
    FbxString lMaterialName = "material_";
    FbxString lShadingName = "lambert";
    lMaterialName += int(group_idx);
    lMaterialName += "_";
    lMaterialName += int(material_idx);
    FbxDouble3 lBlack(0.0, 0.0, 0.0);
    FbxDouble3 lGrey(0.1, 0.1, 0.1);
    FbxDouble3 lDiffuseColor(0.8, 0.8, 0.8);
    fbxsdk::FbxSurfaceLambert *lMaterial = fbxsdk::FbxSurfaceLambert::Create(pScene, lMaterialName.Buffer());

    // Generate primary and secondary colors.
    lMaterial->Emissive.Set(lBlack);
    lMaterial->Ambient.Set(lGrey);
    lMaterial->Diffuse.Set(lDiffuseColor);
    lMaterial->TransparencyFactor.Set(0.0);
    lMaterial->ShadingModel.Set(lShadingName);

    fbxsdk::FbxFileTexture* lTexture = fbxsdk::FbxFileTexture::Create(pScene, "Diffuse Texture");

    // Set texture properties.
    lTexture->SetFileName(tex_name.c_str()); // Resource file is in current directory.
    lTexture->SetRelativeFileName(relative_tex_name.c_str());
    lTexture->SetTextureUse(fbxsdk::FbxTexture::eStandard);
    lTexture->SetMappingType(fbxsdk::FbxTexture::eUV);
    lTexture->SetMaterialUse(fbxsdk::FbxFileTexture::eModelMaterial);
    lTexture->SetSwapUV(false);
    lTexture->SetTranslation(0.0, 0.0);
    lTexture->SetScale(1.0, 1.0);
    lTexture->SetRotation(0.0, 0.0);

    // don't forget to connect the texture to the corresponding property of the material
    if (lMaterial)
    {
        lMaterial->Diffuse.ConnectSrcObject(lTexture);
    }

    auto mesh = pMeshNode->GetMesh();
    auto geo_element_material = mesh->GetElementMaterial(0);

    if (!geo_element_material) {
        geo_element_material = mesh->CreateElementMaterial();
    }

    // The material is mapped to the whole Nurbs
    geo_element_material->SetMappingMode(fbxsdk::FbxGeometryElement::eAllSame);

    // And the material is avalible in the Direct array
    geo_element_material->SetReferenceMode(fbxsdk::FbxGeometryElement::eDirect);

    //get the node of mesh, add material for it.
    pMeshNode->AddMaterial(lMaterial);
}

void ExportTextureFile(const string& file_name, const core::TextureFileInfo* tex_file_info)
{
    ofstream outFile;
    outFile.open(file_name, ofstream::binary);
    outFile.write(reinterpret_cast<const char*>(tex_file_info->memory.get()), tex_file_info->size);
    outFile.close();
}

void ExportJpgFile(const string& file_name, uint32_t w, uint32_t h, uint32_t channel_count, bool switch_channels, uint8_t* src_data)
{
    cv::Mat src_image(int32_t(h), int32_t(w), channel_count == 3 ? CV_8UC3 : CV_8UC4, src_data);
    vector<cv::Mat> src_channels;
    cv::split(src_image, src_channels);
    vector<cv::Mat> dst_channels;
    dst_channels.push_back(move(src_channels[switch_channels ? 2 : 0]));
    dst_channels.push_back(move(src_channels[1]));
    dst_channels.push_back(move(src_channels[switch_channels ? 0 : 2]));
    src_image.release();
    cv::Mat tmp_image;
    cv::merge(dst_channels, tmp_image);
    cv::Mat out_image;
    cv::flip(tmp_image, out_image, 0);
    tmp_image.release();

    cv::imwrite(file_name, out_image);
}

void ExportTextureList(const GroupMeshData* group_mesh_data,
                       const string& texture_root_path,
                       const uint32_t group_idx,
                       vector<string>& tex_name_list,
                       uint32_t num_total_items,
                       uint32_t num_items,
                       QProgressBar* progress_bar)
{
    tex_name_list.reserve(group_mesh_data->loaded_textures.size());
    tex_name_list.resize(group_mesh_data->loaded_textures.size());

    for (uint32_t i = 0; i < group_mesh_data->loaded_textures.size(); i++)
    {
        const core::Texture2DInfo* tex_info = group_mesh_data->loaded_textures[i];
        if (tex_info)
        {
            string tex_idx_string = to_string(group_idx) + "_" + to_string(i);
            bool decode_dxt1 = tex_info->m_format == kGLCmpsdRgbS3tcDxt1Ext || tex_info->m_format == kGLCmpsdRgbaS3tcDxt1Ext;

            if (decode_dxt1 || tex_info->m_format == kGlRgb || tex_info->m_format == kGlRgba)
            {
                uint8_t* src_tex_data = nullptr;
                unique_ptr<uint32_t[]> tmp_tex_data;
                uint32_t w = tex_info->m_mips[0].m_width;
                uint32_t h = tex_info->m_mips[0].m_height;
                uint32_t channel_count = tex_info->m_format == kGlRgb ? 3 : 4;

                uint8_t* src_img_data = reinterpret_cast<uint8_t*>(tex_info->m_mips[0].m_imageData.get());
                if (decode_dxt1)
                {
                    tmp_tex_data = make_unique<uint32_t[]>(w * h);
                    src_tex_data = reinterpret_cast<uint8_t*>(tmp_tex_data.get());
                    core::Dxt1Convertor::DecodeDxt1Texture(tmp_tex_data.get(), w, h, src_img_data);
                }
                else
                {
                    src_tex_data = src_img_data;
                }

                string texture_name = tex_idx_string + ".jpg";
                tex_name_list[i] = texture_root_path + "texture_" + texture_name;

                ExportJpgFile(tex_name_list[i], w, h, channel_count, !decode_dxt1, src_tex_data);
            }
            else
            {
                core::TextureFileInfo tex_file_info;
                if (tex_info->m_format == kGLCmpsdRgbaS3tcDxt3Ext || tex_info->m_format == kGLCmpsdRgbaS3tcDxt5Ext)
                {
                    core::ExportDdsImageFile(tex_info, &tex_file_info);
                    tex_name_list[i] = texture_root_path + "texture_" + tex_idx_string + ".dds";

                    ExportTextureFile(tex_name_list[i], &tex_file_info);
                }
                else
                {
                    core::output_debug_info("error", "wrong texture format : " + to_string(tex_info->m_format));
                }
            }

            progress_bar->setValue(int32_t(float(num_items + i) / float(num_total_items) * 100.0f));
        }
    }
}

void ExportFbxMeshFile(const string& file_name, const vector<BatchMeshData*>& batch_mesh_data, QProgressBar* progress_bar)
{
    const char* lFilename = file_name.c_str();

    size_t pos_0 = file_name.rfind('.');
    size_t pos_1 = file_name.rfind('/');
    size_t pos_2 = file_name.rfind('\\');
    bool found_pos_1 = pos_1 != string::npos;
    bool found_pos_2 = pos_2 != string::npos;
    bool found_path = found_pos_1 || found_pos_2;

    pos_1 = (found_pos_1 && found_pos_2) ? max(pos_1, pos_2) : (found_pos_2 ? pos_2 : pos_1);
    string folder_name = file_name.substr(pos_1 + 1, pos_0 - pos_1 - 1);
    string root_path_name = found_path ? file_name.substr(0, pos_1) : "";

    if (pos_0 == string::npos)
    {
        return;
    }

    string dump_folder_name = root_path_name == "" ? folder_name : (root_path_name + "/" + folder_name);
    if (!fs::exists(dump_folder_name))
    {
        fs::create_directory(dump_folder_name);
    }

    string textures_folder_name = dump_folder_name + "/textures";
    if (!fs::exists(textures_folder_name))
    {
        fs::create_directory(textures_folder_name);
    }

    // Initialize the SDK manager. This object handles all our memory management.
    fbxsdk::FbxManager* lSdkManager = fbxsdk::FbxManager::Create();

    // Create the IO settings object.
    fbxsdk::FbxIOSettings *ios = fbxsdk::FbxIOSettings::Create(lSdkManager, IOSROOT);
    lSdkManager->SetIOSettings(ios);

    // set some IOSettings options
    (*(lSdkManager->GetIOSettings())).SetBoolProp(IOSN_ASCIIFBX,           true);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_MATERIAL,        true);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_TEXTURE,         true);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_EMBEDDED,        true);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_SHAPE,           false);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_GOBO,            false);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_ANIMATION,       false);
    (*(lSdkManager->GetIOSettings())).SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);

    // create an empty scene
    fbxsdk::FbxScene* pScene = fbxsdk::FbxScene::Create(lSdkManager, "");
    FbxAxisSystem axis_system(FbxAxisSystem::EPreDefinedAxisSystem::eMayaZUp);
    pScene->GetGlobalSettings().SetAxisSystem(axis_system);
    pScene->GetGlobalSettings().SetSystemUnit(fbxsdk::FbxSystemUnit::m);

    // Create an importer using the SDK manager.
    fbxsdk::FbxExporter* lExporter = fbxsdk::FbxExporter::Create(lSdkManager, "");

    // Use the first argument as the filename for the importer.
    if (!lExporter->Initialize(lFilename, -1, lSdkManager->GetIOSettings())) {
        printf("Call to FbxExporter::Initialize() failed.\n");
        printf("Error returned: %s\n\n", lExporter->GetStatus().GetErrorString());
        exit(-1);
    }

    // Add the mesh node to the root node in the scene.
    fbxsdk::FbxNode *lRootNode = pScene->GetRootNode();

    uint32_t num_total_items = 0;
    for (uint32_t iMeshBatch = 0; iMeshBatch < batch_mesh_data.size(); iMeshBatch++)
    {
        for (uint32_t iMeshGroup = 0; iMeshGroup < batch_mesh_data[iMeshBatch]->group_meshes.size(); iMeshGroup++)
        {
            vector<string> tex_name_list;
            if (batch_mesh_data[iMeshBatch]->is_google_dump)
            {
                num_total_items += uint32_t(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->loaded_textures.size());
            }

            num_total_items += 1;
        }
    }

    uint32_t num_items = 0;
    for (uint32_t iMeshBatch = 0; iMeshBatch < batch_mesh_data.size(); iMeshBatch++)
    {
        if (!batch_mesh_data[iMeshBatch]->is_spline_mesh)
        {
            for (uint32_t iMeshGroup = 0; iMeshGroup < batch_mesh_data[iMeshBatch]->group_meshes.size(); iMeshGroup++)
            {
                vector<string> tex_name_list;
                if (batch_mesh_data[iMeshBatch]->is_google_dump)
                {
                    ExportTextureList(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup],
                                      textures_folder_name + "/" + folder_name + "_",
                                      iMeshGroup,
                                      tex_name_list,
                                      num_total_items,
                                      num_items,
                                      progress_bar);

                    num_items += uint32_t(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->loaded_textures.size());
                }

                for (uint32_t iMesh = 0; iMesh < batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->meshes.size(); iMesh++)
                {
                    const MeshData* mesh_data = batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->meshes[iMesh];
                    if (mesh_data)
                    {
                        string mesh_idx_string = to_string(iMeshGroup) + "_" + to_string(iMesh);
                        string mesh_node_name = folder_name + string("_meshNode_") + mesh_idx_string;
                        string mesh_name = folder_name + string("_mesh_") + mesh_idx_string;
                        // Create a node for our mesh in the scene.
                        fbxsdk::FbxNode* lMeshNode = fbxsdk::FbxNode::Create(pScene, mesh_node_name.c_str());

                        //lMeshNode->LclTranslation.Set(FbxDouble3(mesh_data->translation.x, mesh_data->translation.y, mesh_data->translation.z));
                        //lMeshNode->LclTranslation.Set(FbxDouble3(mesh_data->dumpped_matrix._41, mesh_data->dumpped_matrix._42, mesh_data->dumpped_matrix._43));
                        lMeshNode->LclTranslation.Set(FbxDouble3(0, 0, 0));

                        // Create a mesh.
                        fbxsdk::FbxMesh* lMesh = fbxsdk::FbxMesh::Create(pScene, mesh_name.c_str());
                        lMesh->InitControlPoints(mesh_data->num_vertex);
                        FbxVector4* lControlPoints = lMesh->GetControlPoints();
                        auto& vertex_list = mesh_data->vertex_list;
                        for (int i = 0; i < mesh_data->num_vertex; i++)
                        {
                            const core::vec3f& v = vertex_list[uint32_t(i)];
                            core::vec4d v1 = core::vec4d(v[0], v[1], v[2], 1.0f) * mesh_data->dumpped_matrix;

                            lControlPoints[i] = FbxVector4(double(v1[0]), double(v1[1]), double(v1[2]));
                        }

                        // Create layer 0 for the mesh if it does not already exist.
                        // This is where we will define our normals.
                        fbxsdk::FbxLayer* lLayer = lMesh->GetLayer(0);
                        if (lLayer == nullptr) {
                            lMesh->CreateLayer();
                            lLayer = lMesh->GetLayer(0);
                        }

                        // Create a uv layer.
                        fbxsdk::FbxLayerElementUV* lLayerElementUv = fbxsdk::FbxLayerElementUV::Create(lMesh, "diffuseUV");
                        lLayerElementUv->SetMappingMode(fbxsdk::FbxLayerElementUV::eByPolygonVertex);
                        lLayerElementUv->SetReferenceMode(fbxsdk::FbxLayerElementUV::eIndexToDirect);

                        for (int i = 0; i < mesh_data->num_vertex; i++)
                        {
                            core::vec2f v = mesh_data->uv_list[uint32_t(i)];
                            lLayerElementUv->GetDirectArray().Add(FbxVector2(double(v.x), double(v.y)));
                        }

                        std::vector<uint32_t> dst_index_list;
                        for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
                        {
                            const DrawCallInfo& draw_call_info = mesh_data->draw_call_list[i_draw];
                            if (draw_call_info.get_primitive_type() == kGlTriangleStrip)
                            {
                                bool flip = false;
                                for (int i = 2; i < draw_call_info.get_index_count(); i++)
                                {
                                    const uint32_t& i0 = draw_call_info.get_index(i-2);
                                    const uint32_t& i1 = draw_call_info.get_index(i-1);
                                    const uint32_t& i2 = draw_call_info.get_index(i);
                                    if (i0 != i1 && i1 != i2 && i2 != i0)
                                    {
                                        dst_index_list.push_back(flip ? i1 : i0);
                                        dst_index_list.push_back(flip ? i0 : i1);
                                        dst_index_list.push_back(i2);
                                    }

                                    flip = !flip;
                                }

                            }
                            else if (draw_call_info.get_primitive_type() == kGlTriangles)
                            {
                                for (int i_vert = 0; i_vert < draw_call_info.get_index_count(); i_vert++)
                                {
                                    dst_index_list.push_back(draw_call_info.get_index(i_vert));
                                }
                            }
                        }

                        int32_t num_index = int32_t(dst_index_list.size());
                        lMesh->ReservePolygonVertexCount(num_index);
                        lLayerElementUv->GetIndexArray().SetCount(num_index);
                        for (int32_t i = 0; i < num_index; i += 3)
                        {
                            int idx0 = int(dst_index_list[uint32_t(i + 0)]);
                            int idx1 = int(dst_index_list[uint32_t(i + 1)]);
                            int idx2 = int(dst_index_list[uint32_t(i + 2)]);
                            lMesh->BeginPolygon(-1, -1, -1, false);
                            lMesh->AddPolygon(idx0);
                            lMesh->AddPolygon(idx1);
                            lMesh->AddPolygon(idx2);
                            lLayerElementUv->GetIndexArray().SetAt(int32_t(i + 0), idx0);
                            lLayerElementUv->GetIndexArray().SetAt(int32_t(i + 1), idx1);
                            lLayerElementUv->GetIndexArray().SetAt(int32_t(i + 2), idx2);
                            lMesh->EndPolygon();
                        }

                        lLayer->SetUVs(lLayerElementUv);

                        // Set the node attribute of the mesh node.
                        lMeshNode->SetNodeAttribute(lMesh);

                        lMeshNode->SetShadingMode(fbxsdk::FbxNode::eTextureShading);

                        string tex_file_name = batch_mesh_data[iMeshBatch]->is_google_dump ? tex_name_list[mesh_data->idx_in_texture_list] : *mesh_data->tex_file_name.get();
                        auto relative_tex_name = tex_file_name.substr(tex_file_name.rfind('/')+1);
                        AddMaterial(pScene, lMeshNode, iMeshGroup, iMesh, tex_file_name, relative_tex_name);

                        lRootNode->AddChild(lMeshNode);
                    }
                }
                num_items++;
                progress_bar->setValue(int32_t(float(num_items) / float(num_total_items) * 100.0f));
            }
        }
    }

    // export the scene.
    lExporter->Export(pScene);

    // The file is imported; so get rid of the importer.
    lExporter->Destroy();

    // Destroy the SDK manager and all the other objects it was handling.
    lSdkManager->Destroy();
}

string InsertRename()
{
    string uuid = GetUUID();
    transform(uuid.begin(), uuid.end(), uuid.begin(), [](char c){ return std::toupper(c); });
    return "\trename -uid \"" + uuid + "\";\n";
}

void AddTransformToMa(const string& transform_type, const core::vec3d* t, const core::vec3d* r, string& body)
{
    body += "createNode transform -s -n \"" + transform_type + "\";\n";
    body += InsertRename();
    body += "\tsetAttr \".v\" no;\n";
    if (t)
    {
        body += "\tsetAttr \".t\" -type \"double3\" " + to_string(t->x) + " " + to_string(t->y) + " " + to_string(t->z) + " ;\n";
    }
    if (r)
    {
        body += "\tsetAttr \".r\" -type \"double3\" " + to_string(r->x) + " " + to_string(r->y) + " " + to_string(r->z) + " ;\n";
    }
}

void AddCameraToMa(const string transform_type, bool is_perspective, int64_t rnd,
                   double fl, int64_t ncp, int64_t fcp, double coi, int64_t ow, const core::vec3d* tp, int64_t o, string& body)
{
    body += "createNode camera -s -n \"" + transform_type + "Shape\" -p \"" + transform_type + "\";\n";
    body += InsertRename();
    body += "\tsetAttr -k off \".v\" no;\n";
    if (rnd >= 0)
    {
        body += "\tsetAttr \".rnd\" ";
        body += rnd == 0 ? "no;\n" : "yes;\n";
    }
    if (fl >= 0.0)
    {
        body += "\tsetAttr \".fl\" " + to_string(fl) + ";\n";
    }
    if (ncp >= 0)
    {
        body += "\tsetAttr \".ncp\" " + to_string(ncp) + ";\n";
    }
    if (fcp >= 0)
    {
        body += "\tsetAttr \".fcp\" " + to_string(fcp) + ";\n";
    }
    if (coi >= 0.0)
    {
        body += "\tsetAttr \".coi\" " + to_string(coi) + ";\n";
    }
    if (ow >= 0)
    {
        body += "\tsetAttr \".ow\" " + to_string(ow) + ";\n";
    }
    body += "\tsetAttr \".imn\" -type \"string\" \"" + transform_type + "\";\n";
    body += "\tsetAttr \".den\" -type \"string\" \"" + transform_type + "_depth\";\n";
    body += "\tsetAttr \".man\" -type \"string\" \"" + transform_type + "_mask\";\n";
    if (tp)
    {
        body += "\tsetAttr \".tp\" -type \"double3\" " + to_string(tp->x) + " " + to_string(tp->y) + " " + to_string(tp->z) + " ;\n";
    }
    body += "\tsetAttr \".hc\" -type \"string\" \"viewSet -p %camera\";\n";
    if (o >= 0)
    {
        body += "\tsetAttr \".o\" ";
        body += o == 0 ? "no\n" : "yes;\n";
    }
    body += "\tsetAttr \".ai_translator\" -type \"string\" "; body += is_perspective ? "\"perspective\";\n" : "\"orthographic\";\n";
}

void ExportMaMeshFile(const string& file_name, const vector<BatchMeshData*>& batch_mesh_data, QProgressBar* progress_bar)
{
    const char* lFilename = file_name.c_str();

    size_t pos_0 = file_name.rfind('.');
    size_t pos_1 = file_name.rfind('/');
    size_t pos_2 = file_name.rfind('\\');
    bool found_pos_1 = pos_1 != string::npos;
    bool found_pos_2 = pos_2 != string::npos;
    bool found_path = found_pos_1 || found_pos_2;

    pos_1 = (found_pos_1 && found_pos_2) ? max(pos_1, pos_2) : (found_pos_2 ? pos_2 : pos_1);
    string folder_name = file_name.substr(pos_1 + 1, pos_0 - pos_1 - 1);
    string root_path_name = found_path ? file_name.substr(0, pos_1) : "";

    if (pos_0 == string::npos)
    {
        return;
    }

    string dump_folder_name = root_path_name == "" ? folder_name : (root_path_name + "/" + folder_name);
    if (!fs::exists(dump_folder_name))
    {
        fs::create_directory(dump_folder_name);
    }

    string ma_file_body;
    ma_file_body += "//Maya ASCII 2018ff08 scene\n";
    ma_file_body += "//Name: smap_linear.ma\n";
    ma_file_body += "//Last modified: Tue, Aug 07, 2018 01:54:10 PM\n";
    ma_file_body += "//Codeset: 1252\n";

    ma_file_body += "requires maya \"2018ff08\";\n";
    ma_file_body += "requires \"stereoCamera\" \"10.0\";\n";
    ma_file_body += "currentUnit -l centimeter -a degree -t film;\n";

    ma_file_body += "fileInfo \"application\" \"maya\";\n";
    ma_file_body += "fileInfo \"product\" \"Maya 2018\";\n";
    ma_file_body += "fileInfo \"version\" \"2018\";\n";
    ma_file_body += "fileInfo \"cutIdentifier\" \"201804211841-f3d65dda2a\";\n";
    ma_file_body += "fileInfo \"osv\" \"Microsoft Windows 8 Enterprise Edition, 64-bit  (Build 9200)\\n\";\n";

    core::vec3d t(-64685.373113880691, 150716.75844239967, 102039.61971618992);
    core::vec3d r(63.261729616690658, 0, -118.60011936645681);
    AddTransformToMa("persp", &t, &r, ma_file_body);

    core::vec3d tp(52759.253291327972, 23153.513755010354, -54583.389675629369);
    AddCameraToMa("persp", true, -1, 34.999999999999993, 1000, 10000000, 197769.59287207728, -1, &tp, -1, ma_file_body);

    core::vec3d t_t(0, 0, 100000.09999999999);
    AddTransformToMa("top", &t_t, nullptr, ma_file_body);

    AddCameraToMa("top", false, 0, -1, -1, -1, 100000.09999999999, 30, nullptr, 1, ma_file_body);

    core::vec3d t_f(0, -100000.09999999999, 0);
    core::vec3d r_f(89.999999999999986, 0, 0);
    AddTransformToMa("front", &t_f, &r_f, ma_file_body);

    AddCameraToMa("front", false, 0, -1, -1, -1, 100000.09999999999, 30, nullptr, 1, ma_file_body);

    core::vec3d t_s(100000.09999999999, 0, 0);
    core::vec3d r_s(90, 4.7708320221952799e-14, 89.999999999999986);
    AddTransformToMa("side", &t_s, &r_s, ma_file_body);

    AddCameraToMa("side", false, 0, -1, -1, -1, 100000.09999999999, 30, nullptr, 1, ma_file_body);

    uint32_t num_total_items = 0;
    for (uint32_t iMeshBatch = 0; iMeshBatch < batch_mesh_data.size(); iMeshBatch++)
    {
        for (uint32_t iMeshGroup = 0; iMeshGroup < batch_mesh_data[iMeshBatch]->group_meshes.size(); iMeshGroup++)
        {
            vector<string> tex_name_list;
            if (batch_mesh_data[iMeshBatch]->is_google_dump)
            {
                num_total_items += uint32_t(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->loaded_textures.size());
            }

            num_total_items += 1;
        }
    }

    uint32_t item_index = 101;
    uint32_t num_items = 0;
    for (uint32_t iMeshBatch = 0; iMeshBatch < batch_mesh_data.size(); iMeshBatch++)
    {
        if (batch_mesh_data[iMeshBatch]->is_spline_mesh)
        {
            for (uint32_t iMeshGroup = 0; iMeshGroup < batch_mesh_data[iMeshBatch]->group_meshes.size(); iMeshGroup++)
            {
                for (uint32_t iMesh = 0; iMesh < batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->meshes.size(); iMesh++)
                {
                    const MeshData* mesh_data = batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->meshes[iMesh];
                    if (mesh_data)
                    {
                        for (uint32_t i_draw = 0; i_draw < mesh_data->draw_call_list.size(); i_draw++)
                        {
                            const DrawCallInfo& draw_call_info = mesh_data->draw_call_list[i_draw];
                            if (draw_call_info.is_ge_polygon())
                            {
                                string curve_name = "smap_linear:curve" + to_string(item_index);
                                string curve_shape_name = "smap_linear:curveShape" + to_string(item_index);
                                ma_file_body += "createNode transform -n \"" + curve_name + "\";\n";
                                ma_file_body += InsertRename();
                                ma_file_body += "createNode nurbsCurve -n \"" + curve_shape_name + "\" -p \"" + curve_name + "\";\n";
                                ma_file_body += InsertRename();
                                ma_file_body += "\tsetAttr -k off \".v\";\n";
                                ma_file_body += "\tsetAttr \".cc\" -type \"nurbsCurve\"\n";
                                ma_file_body += "\t1 " + to_string(mesh_data->num_vertex - 1) + " 0 no 3\n";
                                ma_file_body += "\t" + to_string(draw_call_info.get_index_count());
                                for (int32_t i_idx = 0; i_idx < draw_call_info.get_index_count(); i_idx += 20)
                                {
                                    if (i_idx > 0)
                                    {
                                        ma_file_body += "\t";
                                    }
                                    for (int32_t j_idx = 0; i_idx + j_idx < draw_call_info.get_index_count() && j_idx < 20; j_idx++)
                                    {
                                        ma_file_body += " " + to_string(draw_call_info.get_index(i_idx + j_idx));
                                    }
                                    ma_file_body += "\n";
                                }
                                auto& vertex_list = mesh_data->vertex_list;
                                ma_file_body += "\t" + to_string(mesh_data->num_vertex) + "\n";
                                for (uint32_t i_vert = 0; i_vert < uint32_t(mesh_data->num_vertex); i_vert++)
                                {
                                    ma_file_body += "\t" + to_string(vertex_list[i_vert].x * 100.0f) + " " +
                                                    to_string(vertex_list[i_vert].y * 100.0f) + " " +
                                                    to_string(vertex_list[i_vert].z * 100.0f) + "\n";
                                }
                                ma_file_body += "\t;\n";

                            }
                        }
                    }
                }
                num_items++;
                progress_bar->setValue(int32_t(float(num_items) / float(num_total_items) * 100.0f));
            }
        }
    }

    ma_file_body += "createNode lightLinker -s -n \"lightLinker1\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "\tsetAttr -s 2 \".lnk\";\n";
    ma_file_body += "\tsetAttr -s 2 \".slnk\";\n";
    ma_file_body += "createNode shapeEditorManager -n \"shapeEditorManager\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "createNode poseInterpolatorManager -n \"poseInterpolatorManager\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "createNode displayLayerManager -n \"layerManager\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "createNode displayLayer -n \"defaultLayer\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "createNode renderLayerManager -n \"renderLayerManager\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "createNode renderLayer -n \"defaultRenderLayer\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "\tsetAttr \".g\" yes;\n";
    ma_file_body += "createNode script -n \"sceneConfigurationScriptNode\";\n";
    ma_file_body += InsertRename();
    ma_file_body += "\tsetAttr \".b\" -type \"string\" \"playbackOptions -min 1 -max 120 -ast 1 -aet 200 \";\n";
    ma_file_body += "\tsetAttr \".st\" 6;\n";
    ma_file_body += "select -ne :time1;\n";
    ma_file_body += "\tsetAttr \".o\" 1;\n";
    ma_file_body += "\tsetAttr \".unw\" 1;\n";
    ma_file_body += "select -ne :hardwareRenderingGlobals;\n";
    ma_file_body += "\tsetAttr \".otfna\" -type \"stringArray\" 22 \"NURBS Curves\" \"NURBS Surfaces\" \"Polygons\" \"Subdiv Surface\" \"Particles\" \"Particle Instance\" \"Fluids\" \"Strokes\" \"Image Planes\" \"UI\" \"Lights\" \"Cameras\" \"Locators\" \"Joints\" \"IK Handles\" \"Deformers\" \"Motion Trails\" \"Components\" \"Hair Systems\" \"Follicles\" \"Misc. UI\" \"Ornaments\"  ;\n";
    ma_file_body += "\tsetAttr \".otfva\" -type \"Int32Array\" 22 0 1 1 1 1 1\n";
    ma_file_body += "\t\t1 1 1 0 0 0 0 0 0 0 0 0\n";
    ma_file_body += "\t\t0 0 0 0 ;\n";
    ma_file_body += "\tsetAttr \".fprt\" yes;\n";
    ma_file_body += "select -ne :renderPartition;\n";
    ma_file_body += "\tsetAttr -s 2 \".st\";\n";
    ma_file_body += "select -ne :renderGlobalsList1;\n";
    ma_file_body += "select -ne :defaultShaderList1;\n";
    ma_file_body += "\tsetAttr -s 4 \".s\";\n";
    ma_file_body += "select -ne :postProcessList1;\n";
    ma_file_body += "\tsetAttr -s 2 \".p\";\n";
    ma_file_body += "select -ne :defaultRenderingList1;\n";
    ma_file_body += "select -ne :initialShadingGroup;\n";
    ma_file_body += "\tsetAttr \".ro\" yes;\n";
    ma_file_body += "select -ne :initialParticleSE;\n";
    ma_file_body += "\tsetAttr \".ro\" yes;\n";
    ma_file_body += "select -ne :defaultRenderGlobals;\n";
    ma_file_body += "\tsetAttr \".ren\" -type \"string\" \"arnold\";\n";
    ma_file_body += "select -ne :defaultResolution;\n";
    ma_file_body += "\tsetAttr \".pa\" 1;\n";
    ma_file_body += "select -ne :hardwareRenderGlobals;\n";
    ma_file_body += "\tsetAttr \".ctrs\" 256;\n";
    ma_file_body += "\tsetAttr \".btrs\" 512;\n";
    ma_file_body += "relationship \"link\" \":lightLinker1\" \":initialShadingGroup.message\" \":defaultLightSet.message\";\n";
    ma_file_body += "relationship \"link\" \":lightLinker1\" \":initialParticleSE.message\" \":defaultLightSet.message\";\n";
    ma_file_body += "relationship \"shadowLink\" \":lightLinker1\" \":initialShadingGroup.message\" \":defaultLightSet.message\";\n";
    ma_file_body += "relationship \"shadowLink\" \":lightLinker1\" \":initialParticleSE.message\" \":defaultLightSet.message\";\n";
    ma_file_body += "connectAttr \"layerManager.dli[0]\" \"defaultLayer.id\";\n";
    ma_file_body += "connectAttr \"renderLayerManager.rlmi[0]\" \"defaultRenderLayer.rlid\";\n";
    ma_file_body += "connectAttr \"defaultRenderLayer.msg\" \":defaultRenderingList1.r\" -na;\n";
    ma_file_body += "// End of smap_linear.ma\n";

    ofstream out_file;
    out_file.open(lFilename, ofstream::binary);
    if (out_file)
    {
        out_file.write(ma_file_body.c_str(), int32_t(ma_file_body.length()));
        out_file.close();
    }
}
