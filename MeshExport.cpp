#include "assert.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <cfloat>
#include <cctype>
#include "stb_image_write.h"

// Qt removed - using progress.h interface
#include "coremath.h"
#include "corefile.h"
#include "coregeographic.h"
#include "glfunctionlist.h"
#include "GpaDumpAnalyzeTool.h"
#include "meshdata.h"
#include "worlddata.h"
#include "kmlfileparser.h"
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

void ExportTextureFile(const string& file_name, const core::TextureFileInfo* tex_file_info)
{
    ofstream outFile;
    outFile.open(file_name, ofstream::binary);
    outFile.write(reinterpret_cast<const char*>(tex_file_info->memory.get()), tex_file_info->size);
    outFile.close();
}

// src_data is bottom-up (GL order); RGB(A) when switch_channels, BGR(A) otherwise.
void ExportJpgFile(const string& file_name, uint32_t w, uint32_t h, uint32_t channel_count, bool switch_channels, uint8_t* src_data)
{
    uint32_t r_ofs = switch_channels ? 0 : 2;
    uint32_t b_ofs = switch_channels ? 2 : 0;
    vector<uint8_t> rgb(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t* src_row = src_data + size_t(h - 1 - y) * w * channel_count;
        uint8_t* dst_row = rgb.data() + size_t(y) * w * 3;
        for (uint32_t x = 0; x < w; x++)
        {
            const uint8_t* src = src_row + x * channel_count;
            dst_row[x * 3 + 0] = src[r_ofs];
            dst_row[x * 3 + 1] = src[1];
            dst_row[x * 3 + 2] = src[b_ofs];
        }
    }

    stbi_write_jpg(file_name.c_str(), int(w), int(h), 3, rgb.data(), 95);
}

void ExportTextureList(const GroupMeshData* group_mesh_data,
                       const string& texture_root_path,
                       const uint32_t group_idx,
                       vector<string>& tex_name_list,
                       uint32_t num_total_items,
                       uint32_t num_items,
                       IProgressCallback* progress)
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
            bool decode_dxt3 = tex_info->m_format == kGLCmpsdRgbaS3tcDxt3Ext;
            bool decode_dxt5 = tex_info->m_format == kGLCmpsdRgbaS3tcDxt5Ext;
            bool decode_dxt  = decode_dxt1 || decode_dxt3 || decode_dxt5;

            if (decode_dxt || tex_info->m_format == kGlRgb || tex_info->m_format == kGlRgba)
            {
                uint8_t* src_tex_data = nullptr;
                unique_ptr<uint32_t[]> tmp_tex_data;
                uint32_t w = tex_info->m_mips[0].m_width;
                uint32_t h = tex_info->m_mips[0].m_height;
                uint32_t channel_count = tex_info->m_format == kGlRgb ? 3 : 4;

                uint8_t* src_img_data = reinterpret_cast<uint8_t*>(tex_info->m_mips[0].m_imageData.get());
                if (decode_dxt)
                {
                    tmp_tex_data = make_unique<uint32_t[]>(w * h);
                    src_tex_data = reinterpret_cast<uint8_t*>(tmp_tex_data.get());
                    if (decode_dxt1)      core::Dxt1Convertor::DecodeDxt1Texture(tmp_tex_data.get(), w, h, src_img_data);
                    else if (decode_dxt3) core::DecodeDxt3Texture(tmp_tex_data.get(), w, h, src_img_data);
                    else                  core::DecodeDxt5Texture(tmp_tex_data.get(), w, h, src_img_data);
                }
                else
                {
                    src_tex_data = src_img_data;
                }

                string texture_name = tex_idx_string + ".jpg";
                tex_name_list[i] = texture_root_path + "texture_" + texture_name;

                ExportJpgFile(tex_name_list[i], w, h, channel_count, !decode_dxt, src_tex_data);
            }
            else
            {
                core::output_debug_info("error", "wrong texture format : " + to_string(tex_info->m_format));
            }

            progress->SetValue(int32_t(float(num_items + i) / float(num_total_items) * 100.0f));
        }
    }
}

// ---------------------------------------------------------------------------
// glTF 2.0 export (.gltf = JSON + .bin + texture files, .glb = single binary)
// ---------------------------------------------------------------------------

static string JsonEscape(const string& s)
{
    string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += char(c);
            }
        }
    }
    return out;
}

// Percent-encode a relative path for use as a glTF uri ('/' kept as separator).
static string UriEscape(const string& s)
{
    static const char* hex = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
        {
            out += char(c);
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

static string JsonNumber(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

static bool ReadWholeFile(const string& file_name, vector<uint8_t>& data)
{
    ifstream in(file_name, ifstream::binary);
    if (!in)
    {
        return false;
    }
    in.seekg(0, ios::end);
    data.resize(size_t(in.tellg()));
    in.seekg(0, ios::beg);
    in.read(reinterpret_cast<char*>(data.data()), streamsize(data.size()));
    return bool(in);
}

struct GltfBuilder
{
    vector<uint8_t> bin;
    vector<string>  buffer_views;
    vector<string>  accessors;
    vector<string>  images;
    vector<string>  textures;
    vector<string>  materials;
    vector<string>  meshes;
    vector<string>  nodes;
    map<string, int32_t> material_of_image;   // texture file -> material index (-1 = unusable)

    int32_t AddBufferView(const void* data, size_t size, int32_t target)
    {
        while (bin.size() % 4) bin.push_back(0);
        size_t offset = bin.size();
        bin.insert(bin.end(), reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
        string v = "{\"buffer\":0,\"byteOffset\":" + to_string(offset) + ",\"byteLength\":" + to_string(size);
        if (target)
        {
            v += ",\"target\":" + to_string(target);
        }
        buffer_views.push_back(v + "}");
        return int32_t(buffer_views.size() - 1);
    }

    int32_t AddAccessor(int32_t view, int32_t component_type, size_t count, const char* type, const string& min_max = "")
    {
        accessors.push_back("{\"bufferView\":" + to_string(view) + ",\"componentType\":" + to_string(component_type) +
                            ",\"count\":" + to_string(count) + ",\"type\":\"" + type + "\"" + min_max + "}");
        return int32_t(accessors.size() - 1);
    }

    // Material for a texture file; creates image/texture/material on first use.
    int32_t GetMaterial(const string& tex_file_name, bool embed, const string& gltf_dir, const string& textures_rel_dir)
    {
        auto it = material_of_image.find(tex_file_name);
        if (it != material_of_image.end())
        {
            return it->second;
        }

        int32_t material_idx = -1;
        string ext = fs::path(tex_file_name).extension().string();
        transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(tolower(c)); });
        const char* mime = (ext == ".png") ? "image/png" : ((ext == ".jpg" || ext == ".jpeg") ? "image/jpeg" : nullptr);

        if (mime && fs::exists(tex_file_name))
        {
            string image;
            if (embed)
            {
                vector<uint8_t> data;
                if (ReadWholeFile(tex_file_name, data))
                {
                    int32_t view = AddBufferView(data.data(), data.size(), 0);
                    image = "{\"bufferView\":" + to_string(view) + ",\"mimeType\":\"" + mime + "\"}";
                }
            }
            else
            {
                // .gltf: every texture lives in <name>/textures next to the .gltf
                std::error_code ec;
                fs::path src = fs::absolute(tex_file_name, ec);
                fs::path dst = fs::absolute(fs::path(gltf_dir) / textures_rel_dir / src.filename(), ec);
                if (!fs::equivalent(src, dst, ec))
                {
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                }
                image = "{\"uri\":\"" + JsonEscape(UriEscape(textures_rel_dir + "/" + src.filename().string())) + "\"}";
            }

            if (!image.empty())
            {
                images.push_back(image);
                textures.push_back("{\"sampler\":0,\"source\":" + to_string(images.size() - 1) + "}");
                materials.push_back("{\"name\":\"" + JsonEscape(fs::path(tex_file_name).stem().string()) + "\","
                                    "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + to_string(textures.size() - 1) + "},"
                                    "\"metallicFactor\":0,\"roughnessFactor\":1},"
                                    "\"doubleSided\":true,\"extensions\":{\"KHR_materials_unlit\":{}}}");
                material_idx = int32_t(materials.size() - 1);
            }
        }
        else
        {
            core::output_debug_info("gltf export", "skipping texture (missing or not png/jpg) : " + tex_file_name);
        }

        material_of_image[tex_file_name] = material_idx;
        return material_idx;
    }

    string BuildJson(const string& buffer_uri) const
    {
        auto join = [](const vector<string>& items)
        {
            string s = "[";
            for (size_t i = 0; i < items.size(); i++)
            {
                if (i) s += ",";
                s += items[i];
            }
            return s + "]";
        };

        // Root node: source data is Z-up, glTF is Y-up -> rotate -90 degrees around X.
        vector<string> all_nodes = nodes;
        string children;
        for (size_t i = 0; i < nodes.size(); i++)
        {
            children += (i ? "," : "") + to_string(i);
        }
        all_nodes.push_back("{\"name\":\"root\",\"rotation\":[-0.70710678118654757,0,0,0.70710678118654757],\"children\":[" + children + "]}");

        string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"MeshTool\"},";
        if (!materials.empty()) json += "\"extensionsUsed\":[\"KHR_materials_unlit\"],";
        json += "\"scene\":0,\"scenes\":[{\"nodes\":[" + to_string(all_nodes.size() - 1) + "]}],";
        json += "\"nodes\":" + join(all_nodes) + ",";
        if (!meshes.empty())    json += "\"meshes\":" + join(meshes) + ",";
        if (!materials.empty()) json += "\"materials\":" + join(materials) + ",";
        if (!textures.empty())
        {
            json += "\"textures\":" + join(textures) + ",";
            json += "\"images\":" + join(images) + ",";
            json += "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":33071,\"wrapT\":33071}],";
        }
        if (!accessors.empty())
        {
            json += "\"accessors\":" + join(accessors) + ",";
            json += "\"bufferViews\":" + join(buffer_views) + ",";
            json += "\"buffers\":[{\"byteLength\":" + to_string(bin.size());
            if (!buffer_uri.empty())
            {
                json += ",\"uri\":\"" + JsonEscape(UriEscape(buffer_uri)) + "\"";
            }
            json += "}],";
        }
        json.back() = '}';
        return json;
    }
};

static bool WriteGlb(const string& file_name, string json, vector<uint8_t> bin)
{
    while (json.size() % 4) json += ' ';
    while (bin.size() % 4) bin.push_back(0);

    uint32_t json_len = uint32_t(json.size());
    uint32_t bin_len  = uint32_t(bin.size());
    uint32_t total    = 12 + 8 + json_len + (bin_len ? 8 + bin_len : 0);
    uint32_t header[3]     = { 0x46546C67u, 2u, total };      // "glTF", version 2
    uint32_t json_chunk[2] = { json_len, 0x4E4F534Au };       // "JSON"
    uint32_t bin_chunk[2]  = { bin_len,  0x004E4942u };       // "BIN\0"

    ofstream out(file_name, ofstream::binary);
    if (!out)
    {
        return false;
    }
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(json_chunk), sizeof(json_chunk));
    out.write(json.data(), json_len);
    if (bin_len)
    {
        out.write(reinterpret_cast<const char*>(bin_chunk), sizeof(bin_chunk));
        out.write(reinterpret_cast<const char*>(bin.data()), bin_len);
    }
    return bool(out);
}

bool ExportGltfMeshFile(const string& file_name, const vector<BatchMeshData*>& batch_mesh_data, IProgressCallback* progress)
{
    fs::path out_path(file_name);
    string ext = out_path.extension().string();
    transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(tolower(c)); });
    bool embed = ext == ".glb";

    string folder_name = out_path.stem().string();
    string gltf_dir = out_path.has_parent_path() ? out_path.parent_path().string() : ".";
    string textures_rel_dir = folder_name + "/textures";

    // Google dump textures are decoded to image files first: next to the .gltf,
    // or into a temp folder that is embedded and then deleted for .glb.
    std::error_code ec;
    fs::path textures_dir = embed ? fs::temp_directory_path(ec) / ("meshtool_glb_" + GetUUID())
                                  : fs::path(gltf_dir) / textures_rel_dir;
    fs::create_directories(textures_dir, ec);
    if (ec)
    {
        core::output_debug_info("gltf export", "cannot create folder : " + textures_dir.string());
        return false;
    }

    uint32_t num_total_items = 0;
    for (uint32_t iMeshBatch = 0; iMeshBatch < batch_mesh_data.size(); iMeshBatch++)
    {
        for (uint32_t iMeshGroup = 0; iMeshGroup < batch_mesh_data[iMeshBatch]->group_meshes.size(); iMeshGroup++)
        {
            if (batch_mesh_data[iMeshBatch]->is_google_dump)
            {
                num_total_items += uint32_t(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->loaded_textures.size());
            }

            num_total_items += 1;
        }
    }

    GltfBuilder gltf;
    uint32_t num_items = 0;
    for (uint32_t iMeshBatch = 0; iMeshBatch < batch_mesh_data.size(); iMeshBatch++)
    {
        if (batch_mesh_data[iMeshBatch]->is_spline_mesh)
        {
            continue;
        }

        for (uint32_t iMeshGroup = 0; iMeshGroup < batch_mesh_data[iMeshBatch]->group_meshes.size(); iMeshGroup++)
        {
            vector<string> tex_name_list;
            if (batch_mesh_data[iMeshBatch]->is_google_dump)
            {
                ExportTextureList(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup],
                                  (textures_dir / (folder_name + "_" + to_string(iMeshBatch) + "_")).string(),
                                  iMeshGroup,
                                  tex_name_list,
                                  num_total_items,
                                  num_items,
                                  progress);

                num_items += uint32_t(batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->loaded_textures.size());
            }

            for (uint32_t iMesh = 0; iMesh < batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->meshes.size(); iMesh++)
            {
                const MeshData* mesh_data = batch_mesh_data[iMeshBatch]->group_meshes[iMeshGroup]->meshes[iMesh];
                if (!mesh_data || mesh_data->num_vertex <= 0 || !mesh_data->vertex_list)
                {
                    continue;
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
                        for (int i_vert = 0; i_vert + 2 < draw_call_info.get_index_count(); i_vert += 3)
                        {
                            dst_index_list.push_back(draw_call_info.get_index(i_vert + 0));
                            dst_index_list.push_back(draw_call_info.get_index(i_vert + 1));
                            dst_index_list.push_back(draw_call_info.get_index(i_vert + 2));
                        }
                    }
                }

                if (dst_index_list.empty())
                {
                    continue;
                }

                // Positions stay float and mesh-local; the double-precision offset goes on the node.
                uint32_t num_vertex = uint32_t(mesh_data->num_vertex);
                const core::vec3f* positions = mesh_data->vertex_list.get();
                float min_p[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
                float max_p[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                vector<float> pos_data(num_vertex * 3);
                for (uint32_t i = 0; i < num_vertex; i++)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        float v = positions[i][c];
                        pos_data[i * 3 + c] = v;
                        min_p[c] = min(min_p[c], v);
                        max_p[c] = max(max_p[c], v);
                    }
                }
                string pos_min_max = ",\"min\":[" + JsonNumber(min_p[0]) + "," + JsonNumber(min_p[1]) + "," + JsonNumber(min_p[2]) + "]"
                                     ",\"max\":[" + JsonNumber(max_p[0]) + "," + JsonNumber(max_p[1]) + "," + JsonNumber(max_p[2]) + "]";

                int32_t pos_view = gltf.AddBufferView(pos_data.data(), pos_data.size() * sizeof(float), 34962);
                int32_t pos_acc  = gltf.AddAccessor(pos_view, 5126, num_vertex, "VEC3", pos_min_max);
                string attributes = "\"POSITION\":" + to_string(pos_acc);

                if (mesh_data->uv_list)
                {
                    // GL / FBX uv origin is bottom-left, glTF is top-left.
                    vector<float> uvs(num_vertex * 2);
                    for (uint32_t i = 0; i < num_vertex; i++)
                    {
                        uvs[i * 2 + 0] = mesh_data->uv_list[i].x;
                        uvs[i * 2 + 1] = 1.0f - mesh_data->uv_list[i].y;
                    }
                    int32_t uv_view = gltf.AddBufferView(uvs.data(), uvs.size() * sizeof(float), 34962);
                    attributes += ",\"TEXCOORD_0\":" + to_string(gltf.AddAccessor(uv_view, 5126, num_vertex, "VEC2"));
                }

                int32_t idx_view = gltf.AddBufferView(dst_index_list.data(), dst_index_list.size() * sizeof(uint32_t), 34963);
                int32_t idx_acc  = gltf.AddAccessor(idx_view, 5125, dst_index_list.size(), "SCALAR");

                string tex_file_name;
                if (batch_mesh_data[iMeshBatch]->is_google_dump)
                {
                    if (mesh_data->idx_in_texture_list < tex_name_list.size())
                    {
                        tex_file_name = tex_name_list[mesh_data->idx_in_texture_list];
                    }
                }
                else if (mesh_data->tex_file_name)
                {
                    tex_file_name = *mesh_data->tex_file_name;
                }
                int32_t material_idx = tex_file_name.empty() ? -1 : gltf.GetMaterial(tex_file_name, embed, gltf_dir, textures_rel_dir);

                string mesh_idx_string = to_string(iMeshBatch) + "_" + to_string(iMeshGroup) + "_" + to_string(iMesh);
                string primitive = "{\"attributes\":{" + attributes + "},\"indices\":" + to_string(idx_acc) + ",\"mode\":4";
                if (material_idx >= 0)
                {
                    primitive += ",\"material\":" + to_string(material_idx);
                }
                primitive += "}";
                gltf.meshes.push_back("{\"name\":\"" + JsonEscape(folder_name + "_mesh_" + mesh_idx_string) + "\",\"primitives\":[" + primitive + "]}");

                const core::vec3d& t = mesh_data->translation;
                gltf.nodes.push_back("{\"name\":\"" + JsonEscape(folder_name + "_meshNode_" + mesh_idx_string) + "\",\"mesh\":" + to_string(gltf.meshes.size() - 1) +
                                     ",\"translation\":[" + JsonNumber(t.x) + "," + JsonNumber(t.y) + "," + JsonNumber(t.z) + "]}");
            }
            num_items++;
            progress->SetValue(int32_t(float(num_items) / float(num_total_items) * 100.0f));
        }
    }

    bool ok;
    if (embed)
    {
        ok = WriteGlb(file_name, gltf.BuildJson(""), gltf.bin);
        fs::remove_all(textures_dir, ec);
    }
    else
    {
        string bin_name = folder_name + ".bin";
        ofstream bin_out((fs::path(gltf_dir) / bin_name).string(), ofstream::binary);
        bin_out.write(reinterpret_cast<const char*>(gltf.bin.data()), streamsize(gltf.bin.size()));
        ofstream json_out(file_name, ofstream::binary);
        json_out << gltf.BuildJson(bin_name);
        ok = bool(bin_out) && bool(json_out);
    }

    if (!ok)
    {
        core::output_debug_info("gltf export", "failed to write : " + file_name);
    }
    return ok;
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

void ExportMaMeshFile(const string& file_name, const vector<BatchMeshData*>& batch_mesh_data, IProgressCallback* progress)
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
                progress->SetValue(int32_t(float(num_items) / float(num_total_items) * 100.0f));
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
