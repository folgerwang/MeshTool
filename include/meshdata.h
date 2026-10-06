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
    int32_t         object_id = -1;     // index into the owning group's objects, -1 = none
    int32_t         capture_id = -1;    // index into the owning group's captures, -1 = unknown
    float           lod_size = 0.0f;    // GE tile edge (metres): coarser draws behind finer; 0 = not a GE tile
    uint8_t         material = 0;       // MeshMaterial

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

// Surface material of a mesh. Captured meshes are kCaptured; Refine Buildings
// marks curtain-wall facades as kGlass (translucent, reflective).
enum MeshMaterial : uint8_t
{
    kMatCaptured = 0,
    kMatGlass = 1,
    kMatInterior = 2,   // dark backing just behind glass, so it never shows an empty shell
};

// Semantic class of a segmented scene object.
enum ObjectClass : uint8_t
{
    kObjUnknown = 0,
    kObjGround,         // sidewalk, plaza, parking, bare ground
    kObjRoad,
    kObjBuilding,
    kObjCar,
    kObjTree,
    kObjPlants,         // grass, shrubs, low vegetation
    kObjWater,
    kObjClassCount
};

// One segmented object (a building, a tree, all road surface, ...). Its
// geometry is the group's meshes whose object_id is this object's index.
struct SceneObject
{
    string          name;               // e.g. "building_012", "road"
    ObjectClass     cls = kObjUnknown;
    core::bounds3d  bbox_ws;
};

// One live capture merged into a group (debug view): where GE's camera was
// and how the capture was placed. Its tiles are the meshes with this capture_id.
struct CaptureInfo
{
    core::vec3d     eye;                // GE camera, scene coordinates
    core::vec3d     target;             // where the camera looked, on the captured ground
    core::bounds3d  footprint;          // the capture's tiles as placed
    string          placement;          // "first capture", "shared tiles (12)", "GPS"
    int             tiles_added = 0;    // tiles this capture brought in
    int             duplicates = 0;     // tiles it had that the group already held
};

struct GroupMeshData
{
    core::bounds3d          bbox_ws;
    core::bounds3d          bbox_gps;
    vector<MeshData*>       meshes;
    vector<core::Texture2DInfo*> loaded_textures;
    vector<shared_ptr<string>> texture_names;
    vector<SceneObject>     objects;    // filled by segmentation
    vector<CaptureInfo>     captures;   // filled by live capture, in capture order
    bool                    no_gps = false;   // captured without GPS: set beside the GPS areas, not at its true place

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
    bool                    is_georeferenced = false;  // bbox/vertices are East/North/Up metres at reference_pos
    core::vec2d             reference_pos;             // x = longitude, y = latitude (degrees)
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
