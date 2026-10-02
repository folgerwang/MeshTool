#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <functional>
#include "glcapturedata.h"
#include "glcapture_ipc.h"
#include "coretexture.h"
#include "geoview.h"

struct MeshData;
struct GroupMeshData;
struct CaptureInfo;
struct BatchMeshData;

// LiveCaptureProcessor: reads captured GL commands from shared memory,
// maintains GL state, and extracts meshes at draw calls.

class LiveCaptureProcessor
{
public:
    LiveCaptureProcessor();
    ~LiveCaptureProcessor();

    void SetOutputBatch(BatchMeshData* batch) { m_output_batch = batch; }

    // Callbacks (replace Qt signals)
    std::function<void(int)>                m_on_frame_captured;
    std::function<void(const std::string&)> m_on_capture_error;
    std::function<void(bool)>               m_on_connection_changed;

    // keepMeshes = false still replays the frame (GL buffers and textures it
    // uploads are needed by later frames) but adds nothing to the scene.
    void processFrame(bool keepMeshes = true);

    // What the last processFrame() did to the scene. Meshes in `removed` were
    // taken out of the output batch and must be freed (GPU data + delete) by
    // the caller; meshes in `modified` changed their index lists, so cached GPU
    // data must be dropped. `group` is the group the capture went into: an
    // existing area when `merged`, else a new one appended to the batch.
    // Groups in `discarded` (overview captures) were taken out of the batch
    // and must be freed by the caller.
    struct FrameResult
    {
        bool new_data = false;
        bool merged = false;
        GroupMeshData* group = nullptr;
        std::vector<GroupMeshData*> discarded;
        std::vector<MeshData*> modified;
        std::vector<MeshData*> removed;
    };
    const FrameResult& LastResult() const { return m_last; }

    // Forget the areas being merged into (the app deleted or rebuilt them).
    void ResetMerge();

    // Google Earth's view (from the KML view link) for the next processFrame():
    // with it, captures are placed in real GPS coordinates.
    void SetGeoView(const GeoView& view) { m_geo_view = view; }

private:
    HANDLE              m_mapping;
    void*               m_shared_mem;
    GLCaptureHeader*    m_header;
    char*               m_ring_base;
    HANDLE              m_event_ready;

    RenderingStates     m_render_states;
    int32_t             m_current_bind_texture;
    int32_t             m_current_bind_buffer;
    int32_t             m_bind_buffer_list[kGlNumBufferTypeIdx];
    uint32_t            m_current_texture_slot;

    std::map<uint32_t, std::vector<char>>    m_buffer_store;
    std::map<uint32_t, core::Texture2DInfo*> m_texture_store;
    // Textures handed to captured groups. Those groups end up in g_world, which
    // keeps using them after this processor is gone, so they must not be freed here.
    std::set<core::Texture2DInfo*>           m_textures_handed_out;

    GroupMeshData*      m_current_group;
    BatchMeshData*      m_output_batch;

    std::vector<core::matrix4f> m_matrix_stack;
    bool                m_has_first_matrix;
    core::matrix4d      m_first_inv_transform_matrix;

    // Fixed-function matrices (glMatrixMode / glLoadMatrixf), row-vector layout.
    uint32_t            m_matrix_mode = 0x1700;     // GL_MODELVIEW
    core::matrix4f      m_modelview;
    core::matrix4f      m_projection;
    bool                m_has_modelview = false;
    uint32_t            m_last_uniform_location = 0xFFFFFFFF;
    uint32_t            m_current_program = 0;

    // Sum of area-weighted triangle normals (eye space) over the current frame.
    core::vec3d         m_up_accum;

    // Edge length (metres) of each GE tile mesh this frame, from its modelview
    // scale: Google Earth draws coarse parent tiles under finer children and
    // hides the overlap with the stencil buffer, so both end up captured.
    std::map<MeshData*, double> m_tile_size;
    // Scene position (metres) of each GE tile's origin, set when the tile is
    // placed. All meshes GE draws for one quadtree node (one per texture)
    // share its modelview, so origin + size identify the node across captures.
    std::map<MeshData*, core::vec3d> m_tile_origin;

    // Eye space -> ground frame: metres, Z up, Y pointing away from the GE
    // camera, centred on the first capture of a merged area.
    struct GroundFrame
    {
        core::vec3d origin, x_axis, y_axis, up;
        bool valid = false;
        bool geo = false;       // x/y/up are East/North/Up at the batch's GPS reference
    };

    // GE view at capture time (invalid if the KML view link never reported).
    GeoView             m_geo_view;
    // Eye space -> East/North/Up metres at (lat0, lon0, 0), from the GE view.
    GroundFrame GroundFrameFromGeo(double lon0, double lat0) const;
    static core::matrix4d FrameMatrix(const GroundFrame& f);   // row-vector [p,1] -> [ground / metres-per-unit, 1]
    // Logs how far the GE look-at point is from the captured surface at screen centre.
    void CheckLookAt(const GroupMeshData* group) const;

    // Tile identity across captures: hash of a tile's vertex + index data.
    std::map<MeshData*, uint64_t>       m_tile_key;

    // A captured area: one group of the output batch that later captures
    // merge into. Its first capture's eye space is the area's reference.
    struct Area
    {
        GroupMeshData*                      group = nullptr;
        GroundFrame                         frame;        // reference eye space -> scene
        std::map<uint64_t, core::matrix4d>  ref_tile_mv;  // key -> modelview into the reference eye space
        std::map<uint64_t, MeshData*>       key_mesh;     // tiles present in the group
    };
    std::vector<Area>   m_areas;   // oldest first

    // Drops coarse-tile triangles whose ground footprint finer tiles cover,
    // which is what GE's stencil masking does on screen, and same-level tiles
    // that are copies of one already kept (merged captures overlap).
    void RemoveCoveredLods(Area& area);
    // Smallest tile edge (metres) among the meshes: the level of detail a
    // capture or area reaches, unaffected by tilt (horizon tiles are coarse).
    double FinestTile(const std::vector<MeshData*>& meshes) const;
    // Moves an area by d metres in the scene, its frame included.
    void ShiftArea(Area& area, const core::vec3d& d);
    // Moves areas captured without GPS off the others (they have no true place).
    void SeparateNoGpsAreas();

    FrameResult         m_last;

    GroundFrame ComputeGroundFrame(const GroupMeshData* group) const;
    // Moves a group's meshes from this frame's eye space (optionally via the
    // eye-to-reference-eye transform X) into ground frame `f`.
    void ApplyGroundFrame(GroupMeshData* group, const core::matrix4d* X, const GroundFrame& f);
    static core::vec3d ToGround(const core::vec3d& p_eye, const core::matrix4d* X, const GroundFrame& f);
    // Debug record of this frame's capture (camera, footprint) once its
    // meshes are in ground frame `f`; tags the meshes with `capture_id`.
    CaptureInfo DescribeCapture(const std::vector<MeshData*>& meshes, const core::matrix4d* X,
                                const GroundFrame& f, int32_t capture_id, const std::string& placement) const;
    // Finds X (this frame's eye space -> the area's reference eye space) from
    // tiles both contain; false if fewer than 3 agree.
    bool RegisterToReference(const GroupMeshData* group, const Area& area, core::matrix4d& X, int& support) const;
    void HandOutTextures(GroupMeshData* group, uint32_t index_offset);

    void ProcessRecord(const GLCaptureRecord* record, const char* payload);
    void HandleBindBuffer(const CmdBindBuffer* cmd);
    void HandleBufferData(const CmdBufferData* cmd, const char* data);
    void HandleBufferSubData(const CmdBufferSubData* cmd, const char* data);
    void HandleGenBuffers(const CmdGenBuffers* cmd, const uint32_t* ids);
    void HandleVertexAttribPointer(const CmdVertexAttribPointer* cmd);
    void HandleEnableVertexAttrib(const CmdEnableDisableVertexAttrib* cmd);
    void HandleDisableVertexAttrib(const CmdEnableDisableVertexAttrib* cmd);
    void HandleBindTexture(const CmdBindTexture* cmd);
    void HandleActiveTexture(const CmdActiveTexture* cmd);
    void HandleTexImage2D(const CmdTexImage2D* cmd, const char* data);
    void HandleCompressedTexImage2D(const CmdCompressedTexImage2D* cmd, const char* data);
    void HandleUniformMatrix4fv(const CmdUniformMatrix4fv* cmd);
    void HandleDrawElements(const CmdDrawElements* cmd, const char* payload);
    void HandleDrawArrays(const CmdDrawArrays* cmd, const char* payload);
    void HandleUseProgram(const CmdUseProgram* cmd);
    void ExtractMeshFromDrawCall(const RenderingStates& state);
};

// ProcessManager: launches Google Earth Pro and manages IPC lifecycle
class ProcessManager
{
public:
    ProcessManager();
    ~ProcessManager();

    // `kml_path`, if given, is opened by GE at startup (e.g. the GPS view link).
    bool StartGoogleEarth(const std::string& ge_path = "", const std::string& kml_path = "");
    // Launch Google Earth and fly to a KML file
    bool StartGoogleEarthWithKML(const std::string& kml_path);
    // Opens a KML in Google Earth: a running Google Earth (started by MeshTool
    // or not) loads it into its session, otherwise GE starts. Not tracked as
    // MeshTool's GE process and nothing is injected.
    bool OpenKmlInGoogleEarth(const std::string& kml_path);
    void StopGoogleEarth();
    bool IsRunning() const;

    // Generate a KML file that flies to the given GPS coordinate
    static std::string GenerateFlyToKML(double lon, double lat, double altitude,
                                         double heading, double tilt, double range);
    // Generate a KML from an address/place name (uses Google Earth's search)
    static std::string GenerateSearchKML(const std::string& address);

    bool CreateSharedMemory();
    void DestroySharedMemory();
    bool RequestFrameCapture();
    bool IsHookConnected() const;

    GLCaptureHeader* GetHeader() const { return m_header; }
    HANDLE GetReadyEvent() const { return m_event_ready; }

    // Callbacks
    std::function<void()>   m_on_process_started;
    std::function<void()>   m_on_process_stopped;
    std::function<void()>   m_on_hook_connected;

private:
    HANDLE              m_process;
    HANDLE              m_mapping;
    void*               m_shared_mem;
    GLCaptureHeader*    m_header;
    HANDLE              m_event_ready;
    HANDLE              m_event_control;

    std::string         m_ge_path;
};
