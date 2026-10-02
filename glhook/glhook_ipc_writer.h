#pragma once

#include <windows.h>
#include <cstdint>
#include "glcapture_ipc.h"

class IPCWriter
{
public:
    IPCWriter();
    ~IPCWriter();

    bool Init();
    void Shutdown();
    bool IsConnected() const { return m_shared_mem != nullptr; }

    // Check if MeshTool has requested capture
    bool IsCaptureRequested() const;
    bool IsFrameRequested() const;
    void ClearFrameRequest();

    // Write a command record to the ring buffer
    // Returns pointer to payload area, or nullptr if not enough space
    void* BeginRecord(GLCaptureCmd cmd, uint32_t payload_size);
    void EndRecord();

    // Convenience: write a complete record in one call
    bool WriteRecord(GLCaptureCmd cmd, const void* payload, uint32_t payload_size);

    // Signal MeshTool that data is available
    void SignalReady();

    // Update status
    void SetStatus(uint32_t flags);
    void ClearStatus(uint32_t flags);

    GLCaptureHeader* GetHeader() { return m_header; }

    // Records are only written while recording (i.e. during a captured frame),
    // so the ring does not fill up with GL traffic between captures.
    void SetRecording(bool on) { m_recording = on; }
    bool IsRecording() const { return m_recording; }

private:
    HANDLE              m_mapping;
    void*               m_shared_mem;
    GLCaptureHeader*    m_header;
    char*               m_ring_base;

    HANDLE              m_event_ready;
    HANDLE              m_event_control;

    uint64_t            m_current_record_offset;
    uint32_t            m_current_record_size;
    bool                m_recording = false;

    uint64_t AvailableSpace() const;
};

extern IPCWriter g_ipc_writer;
