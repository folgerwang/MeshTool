#pragma once
#include "coreprimitive.h"
#include "coretexture.h"
#include "glfunctionlist.h"
#include "debugout.h"

struct DrawCallInfo
{
private:
    PrimitiveType   primitive_type_;
    bool            drawable_;
    bool            drawable_patches_;
    int             max_index_count_;
    int				num_index_;
    uint32_t        index_type_;
    uint32_t        index_size_;
    unique_ptr<uint32_t[]> index_list_;

public:
    DrawCallInfo() : primitive_type_(kGlTriangles),
                     drawable_(true),
                     drawable_patches_(false),
                     max_index_count_(0),
                     num_index_(0),
                     index_type_(kGlUShort),
                     index_size_(2)
    {
    }

    void set_primitive_type(PrimitiveType prim_type)
    {
        primitive_type_ = prim_type;
    }

    PrimitiveType get_primitive_type() const
    {
        return primitive_type_;
    }

    void set_drawable(bool drawable)
    {
        drawable_ = drawable;
    }

    bool is_drawable() const
    {
        return drawable_;
    }

    void set_drawable_patches(bool drawable_pathes)
    {
        drawable_patches_ = drawable_pathes;
    }

    bool is_drawable_patches() const
    {
        return drawable_patches_;
    }

/*    bool is_polygon() const
    {
        return primitive_type_ == kGlTriangles ||
               primitive_type_ == kGlTriangleStrip ||
               primitive_type_ == kGlTriangleFan ||
               primitive_type_ == kGlQuads ||
               primitive_type_ == kGlQuadStrip ||
               primitive_type_ == kGlPolygon;
    }*/

    bool is_ge_mesh() const {
        return primitive_type_ == kGlTriangles;
    }

    bool is_ge_polygon() const {
        return primitive_type_ == kGlTriangleStrip;
    }

    void clear_index_buffer()
    {
        num_index_ = 0;
    }

    int get_index_count() const
    {
        return num_index_;
    }

    uint8_t* get_index_buffer() const
    {
        return reinterpret_cast<uint8_t*>(index_list_.get());
    }

    uint32_t get_index_type() const
    {
        return index_type_;
    }

    void allocate_index_buffer(int index_count, int max_index = 65535)
    {
        max_index_count_ = index_count;
        index_type_ = max_index > 0xffff ? kGlUInt : kGlUShort;
        index_size_ = index_type_ == kGlUInt ? 4 : 2;
        index_list_ = make_unique<uint32_t[]>((uint32_t(index_count) * index_size_ + 3) / 4);
    }

    uint32_t get_index(int idx) const
    {
        if (index_type_ == kGlUInt)
        {
            return index_list_[uint32_t(idx)];
        }
        else
        {
            return reinterpret_cast<const uint16_t*>(index_list_.get())[idx];
        }
    }

    void set_index(int idx, uint32_t index)
    {
        if (index_type_ == kGlUInt)
        {
            index_list_[uint32_t(idx)] = index;
        }
        else
        {
            uint16_t* index_16_list = reinterpret_cast<uint16_t*>(index_list_.get());
            index_16_list[idx] = uint16_t(index);
        }

        num_index_ = max(idx, num_index_);
    }

    void add_index(uint32_t idx)
    {
        if (index_type_ == kGlUInt)
        {
            index_list_[uint32_t(num_index_)] = idx;
        }
        else
        {
            uint16_t* index_16_list = reinterpret_cast<uint16_t*>(index_list_.get());
            index_16_list[num_index_] = uint16_t(idx);
        }

        num_index_++;
    }
};

struct MeshData : public core::Primitive
{
    int				num_vertex;
    uint32_t		idx_in_texture_list;
    uint32_t        tex_id;
    unique_ptr<string> tex_file_name;
    core::vec3d		translation;
    core::matrix4d	dumpped_matrix;
    vector<bool>    patch_list;
    unique_ptr<core::vec3f[]> vertex_list;
    unique_ptr<core::GpsCoord[]> gps_vert_list;
    unique_ptr<core::vec2f[]> uv_list;
    unique_ptr<uint32_t[]> color_list;
    vector<DrawCallInfo> draw_call_list;

    MeshData() : num_vertex(0),
                 idx_in_texture_list(INVALID_VALUE),
                 tex_id(INVALID_VALUE),
                 translation(core::vec3f(0, 0, 0)),
                 vertex_list(nullptr),
                 gps_vert_list(nullptr),
                 uv_list(nullptr),
                 color_list(nullptr)
    {}

    virtual ~MeshData()
    {
    }

    bool culling(const core::vec3d& reference_pos, const core::matrix4f& world_proj_mat, float scale);

    void draw(CoreGLSLProgram* program);

    DrawCallInfo& get_last_draw_call_info()
    {
        return draw_call_list.back();
    }

    void add_draw_call_list(PrimitiveType prim_type, int index_count, int max_index = 65535)
    {
        DrawCallInfo* lastest_draw_call = new DrawCallInfo;
        lastest_draw_call->set_primitive_type(prim_type);
        lastest_draw_call->allocate_index_buffer(index_count, max_index);

        draw_call_list.push_back(move(*lastest_draw_call));
    }
};

struct GroupMeshData
{
    core::bounds3d          bbox_ws;
    core::bounds3d          bbox_gps;
    vector<MeshData*>       meshes;
    vector<core::Texture2DInfo*> loaded_textures;
    vector<shared_ptr<string>> texture_names;

    void remove_item(uint32_t index)
    {
        if (index < meshes.size())
        {
            SAFE_DELETE(meshes[index]);
            meshes.erase(meshes.begin() + index);
        }
    }
};

struct BatchMeshData
{
    bool                    is_spline_mesh;
    bool                    is_texture_loaded;
    bool                    is_google_dump;
    core::vec2d             reference_pos;
    core::bounds2d          scissor_bbox;
    core::bounds3d          bbox_ws;
    core::bounds3d          bbox_gps;
    vector<GroupMeshData*>  group_meshes;

    BatchMeshData() : is_spline_mesh(false),
                      is_texture_loaded(false),
                      is_google_dump(false){}
};

struct PatchInfo
{
    core::bounds2d           bbox;
    unique_ptr<uint32_t[]>   map_idx_list;
    unique_ptr<uint32_t[]>   tex_idx_list;
};

struct MapInfo
{
    core::bounds2d           bbox;
    core::vec2d              ul_corner;
    core::vec2d              pixel_size;
    core::vec2i              pixel_count;
    core::vec2i              size;
    unique_ptr<float[]>      alt_list;
};

struct TexInfo
{
    core::bounds2d           bbox;
    core::vec2i              size;
    unique_ptr<uint8_t[]>    channel_list;
};

void PatchesCutting(const vector<core::bounds2d>& patch_list,
                    const vector<core::bounds2d>& map_list,
                    const vector<core::bounds2d>& tex_list,
                    vector<PatchInfo>& patch_info_list);
