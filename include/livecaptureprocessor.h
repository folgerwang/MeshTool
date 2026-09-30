#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <functional>
#include "glcapturedata.h"
#include "glcapture_ipc.h"
#include "coretexture.h"

struct MeshData;
struct GroupMeshData;
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

    void processFrame();

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

    GroupMeshData*      m_current_group;
    BatchMeshData*      m_output_batch;

    std::vector<core::matrix4f> m_matrix_stack;
    bool                m_has_first_matrix;
    core::matrix4d      m_first_inv_transform_matrix;

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

    bool StartGoogleEarth(const std::string& ge_path = "");
    // Launch Google Earth and fly to a KML file
    bool StartGoogleEarthWithKML(const std::string& kml_path);
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
