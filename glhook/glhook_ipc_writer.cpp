#include "glhook_ipc_writer.h"
#include <cstring>

IPCWriter g_ipc_writer;

IPCWriter::IPCWriter()
    : m_mapping(nullptr)
    , m_shared_mem(nullptr)
    , m_header(nullptr)
    , m_ring_base(nullptr)
    , m_event_ready(nullptr)
    , m_event_control(nullptr)
    , m_current_record_offset(0)
    , m_current_record_size(0)
{
}

IPCWriter::~IPCWriter()
{
    Shutdown();
}

bool IPCWriter::Init()
{
    // Open existing shared memory created by MeshTool
    m_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, GLCAPTURE_SHARED_MEM_NAME);
    if (!m_mapping)
    {
        // MeshTool hasn't created shared memory yet - not an error, just not connected
        return false;
    }

    m_shared_mem = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, GLCAPTURE_SHARED_MEM_SIZE);
    if (!m_shared_mem)
    {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }

    m_header = reinterpret_cast<GLCaptureHeader*>(m_shared_mem);
    m_ring_base = reinterpret_cast<char*>(m_shared_mem) + GLCAPTURE_HEADER_SIZE;

    // Open events
    m_event_ready = OpenEventA(EVENT_ALL_ACCESS, FALSE, GLCAPTURE_EVENT_READY);
    m_event_control = OpenEventA(EVENT_ALL_ACCESS, FALSE, GLCAPTURE_EVENT_CONTROL);

    // Set initial state
    m_header->write_offset = 0;
    m_header->frame_count = 0;
    m_header->pid = GetCurrentProcessId();
    m_header->status_flags = GLCAPTURE_STATUS_CONNECTED;

    return true;
}

void IPCWriter::Shutdown()
{
    if (m_header)
    {
        m_header->status_flags = 0;
    }

    if (m_shared_mem)
    {
        UnmapViewOfFile(m_shared_mem);
        m_shared_mem = nullptr;
        m_header = nullptr;
        m_ring_base = nullptr;
    }

    if (m_mapping)
    {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }

    if (m_event_ready)
    {
        CloseHandle(m_event_ready);
        m_event_ready = nullptr;
    }

    if (m_event_control)
    {
        CloseHandle(m_event_control);
        m_event_control = nullptr;
    }
}

bool IPCWriter::IsCaptureRequested() const
{
    if (!m_header) return false;
    return (m_header->capture_flags & (GLCAPTURE_FLAG_ACTIVE | GLCAPTURE_FLAG_FRAME_REQ | GLCAPTURE_FLAG_CONTINUOUS)) != 0;
}

bool IPCWriter::IsFrameRequested() const
{
    if (!m_header) return false;
    return (m_header->capture_flags & GLCAPTURE_FLAG_FRAME_REQ) != 0;
}

void IPCWriter::ClearFrameRequest()
{
    if (m_header)
    {
        m_header->capture_flags &= ~GLCAPTURE_FLAG_FRAME_REQ;
    }
}

uint32_t IPCWriter::AvailableSpace() const
{
    uint32_t write = m_header->write_offset;
    uint32_t read = m_header->read_offset;

    if (write >= read)
        return GLCAPTURE_RING_SIZE - (write - read) - 1;
    else
        return read - write - 1;
}

void* IPCWriter::BeginRecord(GLCaptureCmd cmd, uint32_t payload_size)
{
    if (!m_shared_mem || !IsCaptureRequested()) return nullptr;

    uint32_t total_size = sizeof(GLCaptureRecord) + payload_size;
    // Align to 4 bytes
    total_size = (total_size + 3) & ~3;

    if (AvailableSpace() < total_size + 8)  // Extra margin
    {
        m_header->status_flags |= GLCAPTURE_STATUS_OVERFLOW;
        return nullptr;
    }

    uint32_t write = m_header->write_offset;
    m_current_record_offset = write;
    m_current_record_size = total_size;

    // Handle wrap-around: if record doesn't fit at end, wrap to beginning
    if (write + total_size > GLCAPTURE_RING_SIZE)
    {
        // Write a skip marker at current position if there's room for a header
        if (GLCAPTURE_RING_SIZE - write >= sizeof(GLCaptureRecord))
        {
            GLCaptureRecord* skip = reinterpret_cast<GLCaptureRecord*>(m_ring_base + write);
            skip->cmd_id = CMD_NONE;
            skip->total_size = GLCAPTURE_RING_SIZE - write;
            skip->flags = 0;
        }
        write = 0;
        m_current_record_offset = 0;

        if (AvailableSpace() < total_size + 8)
        {
            m_header->status_flags |= GLCAPTURE_STATUS_OVERFLOW;
            return nullptr;
        }
    }

    // Write record header
    GLCaptureRecord* record = reinterpret_cast<GLCaptureRecord*>(m_ring_base + write);
    record->cmd_id = cmd;
    record->total_size = total_size;
    record->flags = 0;

    return m_ring_base + write + sizeof(GLCaptureRecord);
}

void IPCWriter::EndRecord()
{
    // Advance write offset atomically
    uint32_t new_offset = m_current_record_offset + m_current_record_size;
    if (new_offset >= GLCAPTURE_RING_SIZE)
        new_offset = 0;

    // Memory barrier before publishing
    MemoryBarrier();
    m_header->write_offset = new_offset;
}

bool IPCWriter::WriteRecord(GLCaptureCmd cmd, const void* payload, uint32_t payload_size)
{
    void* dest = BeginRecord(cmd, payload_size);
    if (!dest) return false;

    if (payload && payload_size > 0)
        memcpy(dest, payload, payload_size);

    EndRecord();
    return true;
}

void IPCWriter::SignalReady()
{
    if (m_event_ready)
        SetEvent(m_event_ready);
}

void IPCWriter::SetStatus(uint32_t flags)
{
    if (m_header)
        m_header->status_flags |= flags;
}

void IPCWriter::ClearStatus(uint32_t flags)
{
    if (m_header)
        m_header->status_flags &= ~flags;
}
