#include <windows.h>
#include <cstring>
#include <cassert>
#include <fstream>
#include <string>
#include "glhook_injector.h"
#include "livecaptureprocessor.h"
#include <array>
#include <cstdarg>
#include <cstdio>
#include "meshdata.h"
#include "worlddata.h"
#include "debugout.h"

using namespace std;

// ---------------------------------------------------------------------------
// Capture diagnostics: C:\Users\Public\meshtool_capture.log
// ---------------------------------------------------------------------------
static void CapLog(const char* fmt, ...)
{
    FILE* f = fopen("C:\\Users\\Public\\meshtool_capture.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

static int g_logged_draws = 0;          // draw calls logged in the current frame
static const int kMaxLoggedDraws = 40;
static int g_meshes_from_frame = 0;

// Size in bytes of one component of a GL vertex attribute / index type; 0 if unsupported.
static uint32_t GLTypeSize(uint32_t type)
{
    switch (type)
    {
    case 0x1400: case 0x1401: return 1;   // BYTE, UNSIGNED_BYTE
    case 0x1402: case 0x1403: return 2;   // SHORT, UNSIGNED_SHORT
    case 0x1404: case 0x1405: case 0x1406: return 4;   // INT, UNSIGNED_INT, FLOAT
    case 0x140A: return 8;                // DOUBLE
    }
    return 0;
}

// One component converted to float the way GL feeds it to the shader.
static float ReadGLComponent(const char* p, uint32_t type, bool normalized)
{
    switch (type)
    {
    case 0x1400: { int8_t   v = *(const int8_t*)p;   return normalized ? (v / 127.0f < -1.0f ? -1.0f : v / 127.0f) : float(v); }
    case 0x1401: { uint8_t  v = *(const uint8_t*)p;  return normalized ? v / 255.0f : float(v); }
    case 0x1402: { int16_t  v = *(const int16_t*)p;  return normalized ? (v / 32767.0f < -1.0f ? -1.0f : v / 32767.0f) : float(v); }
    case 0x1403: { uint16_t v = *(const uint16_t*)p; return normalized ? v / 65535.0f : float(v); }
    case 0x1404: { int32_t  v = *(const int32_t*)p;  return normalized ? float(v / 2147483647.0) : float(v); }
    case 0x1405: { uint32_t v = *(const uint32_t*)p; return normalized ? float(v / 4294967295.0) : float(v); }
    case 0x1406: return *(const float*)p;
    case 0x140A: return float(*(const double*)p);
    }
    return 0.0f;
}

// Reads every vertex of an attribute stream from its buffer into `out`
// (up to 4 components each, missing ones = 0). Stride 0 means tightly packed.
static size_t ReadAttributeStream(const std::vector<char>& buf, const VertexAttrib& a, std::vector<std::array<float, 4>>& out)
{
    uint32_t comp_size = GLTypeSize(uint32_t(a.data_type));
    uint32_t comps = a.num_elements < 1 ? 1 : (a.num_elements > 4 ? 4 : a.num_elements);
    if (comp_size == 0)
        return 0;
    uint32_t elem_size = comp_size * comps;
    uint32_t stride = a.stride ? a.stride : elem_size;

    const char* p = buf.data() + a.start_offset;
    const char* end = buf.data() + buf.size();
    if (a.start_offset >= buf.size())
        return 0;
    for (; p + elem_size <= end; p += stride)
    {
        std::array<float, 4> v = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (uint32_t c = 0; c < comps; c++)
            v[c] = ReadGLComponent(p + c * comp_size, uint32_t(a.data_type), a.is_normalized != 0);
        out.push_back(v);
    }
    return out.size();
}

// ============================================================================
// Helper: convert matrix4f to matrix4d (same as in GpaDumpAnalyzeTool)
// ============================================================================
static void Matrix4fToMatrix4d(const core::matrix4f& src_mat, core::matrix4d& dst_mat)
{
    for (int r = 0; r < 4; r++)
    {
        core::vec4f row = src_mat.get_row(r);
        dst_mat.set_row(r, core::vec4d(double(row.x), double(row.y), double(row.z), double(row.w)));
    }
}

// ============================================================================
// LiveCaptureProcessor
// ============================================================================

LiveCaptureProcessor::LiveCaptureProcessor()
    : m_mapping(nullptr)
    , m_shared_mem(nullptr)
    , m_header(nullptr)
    , m_ring_base(nullptr)
    , m_event_ready(nullptr)
    , m_current_bind_texture(-1)
    , m_current_bind_buffer(-1)
    , m_current_texture_slot(0)
    , m_current_group(nullptr)
    , m_output_batch(nullptr)
    , m_has_first_matrix(false)
{
    memset(m_bind_buffer_list, -1, sizeof(m_bind_buffer_list));
}

LiveCaptureProcessor::~LiveCaptureProcessor()
{
    if (m_shared_mem) UnmapViewOfFile(m_shared_mem);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_event_ready) CloseHandle(m_event_ready);

    // Clean up texture store (except textures now owned by captured groups)
    for (auto& pair : m_texture_store)
        if (!m_textures_handed_out.count(pair.second))
            delete pair.second;
}

void LiveCaptureProcessor::processFrame()
{
    // Open shared memory if not already open
    if (!m_shared_mem)
    {
        m_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, GLCAPTURE_SHARED_MEM_NAME);
        if (!m_mapping)
        {
            if (m_on_capture_error) m_on_capture_error("Cannot open shared memory - is Google Earth running with hook?");
            return;
        }

        m_shared_mem = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, GLCAPTURE_SHARED_MEM_SIZE);
        if (!m_shared_mem)
        {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            if (m_on_capture_error) m_on_capture_error("Cannot map shared memory");
            return;
        }

        m_header = reinterpret_cast<GLCaptureHeader*>(m_shared_mem);
        m_ring_base = reinterpret_cast<char*>(m_shared_mem) + GLCAPTURE_HEADER_SIZE;

        m_event_ready = OpenEventA(EVENT_ALL_ACCESS, FALSE, GLCAPTURE_EVENT_READY);

        if (m_on_connection_changed) m_on_connection_changed(true);
    }

    // Check if hook is connected
    if (!(m_header->status_flags & GLCAPTURE_STATUS_CONNECTED))
    {
        if (m_on_capture_error) m_on_capture_error("Hook DLL not connected");
        return;
    }

    // Create a new group for this frame's meshes
    m_current_group = new GroupMeshData;
    g_logged_draws = 0;
    g_meshes_from_frame = 0;
    std::map<uint32_t, uint32_t> record_counts;
    CapLog("=== processFrame: read=%u write=%u status=0x%X\n",
           m_header->read_offset, m_header->write_offset, m_header->status_flags);
    m_has_first_matrix = false;
    m_matrix_stack.clear();

    // Read all available records from the ring buffer
    uint32_t read_pos = m_header->read_offset;
    uint32_t write_pos = m_header->write_offset;

    while (read_pos != write_pos)
    {
        // Bounds check
        if (read_pos >= GLCAPTURE_RING_SIZE)
        {
            read_pos = 0;
            continue;
        }

        const GLCaptureRecord* record = reinterpret_cast<const GLCaptureRecord*>(m_ring_base + read_pos);

        // Skip marker (wrap-around)
        if (record->cmd_id == CMD_NONE)
        {
            read_pos = 0;
            continue;
        }

        // Frame end - stop processing
        if (record->cmd_id == CMD_FRAME_END)
        {
            read_pos += record->total_size;
            if (read_pos >= GLCAPTURE_RING_SIZE)
                read_pos = 0;
            break;
        }

        const char* payload = m_ring_base + read_pos + sizeof(GLCaptureRecord);
        record_counts[record->cmd_id]++;
        ProcessRecord(record, payload);

        read_pos += record->total_size;
        if (read_pos >= GLCAPTURE_RING_SIZE)
            read_pos = 0;
    }

    // Update read position
    m_header->read_offset = read_pos;

    CapLog("  records:");
    for (auto& rc : record_counts)
        CapLog(" 0x%04X x%u", rc.first, rc.second);
    CapLog("\n  buffers known=%u textures known=%u meshes built=%d%s\n",
           unsigned(m_buffer_store.size()), unsigned(m_texture_store.size()), g_meshes_from_frame,
           (m_header->status_flags & GLCAPTURE_STATUS_OVERFLOW) ? "  (hook reported RING OVERFLOW)" : "");

    // Add captured meshes to output
    int mesh_count = (int)m_current_group->meshes.size();
    if (mesh_count > 0 && m_output_batch)
    {
        // Transfer textures from texture_store to group
        for (auto& pair : m_texture_store)
        {
            m_current_group->loaded_textures.push_back(pair.second);
            m_textures_handed_out.insert(pair.second);
        }
        // Don't clear texture_store - the pointers are now owned by the group

        m_output_batch->group_meshes.push_back(m_current_group);
        m_output_batch->bbox_ws += m_current_group->bbox_ws;
        m_output_batch->bbox_gps += m_current_group->bbox_gps;
        m_current_group = nullptr;

        if (m_on_frame_captured) m_on_frame_captured(mesh_count);
    }
    else
    {
        delete m_current_group;
        m_current_group = nullptr;

        if (m_on_frame_captured) m_on_frame_captured(0);
    }
}

void LiveCaptureProcessor::ProcessRecord(const GLCaptureRecord* record, const char* payload)
{
    switch (record->cmd_id)
    {
    case CMD_BIND_BUFFER:
        HandleBindBuffer(reinterpret_cast<const CmdBindBuffer*>(payload));
        break;
    case CMD_BUFFER_DATA:
        HandleBufferData(reinterpret_cast<const CmdBufferData*>(payload),
                         payload + sizeof(CmdBufferData));
        break;
    case CMD_BUFFER_SUB_DATA:
        HandleBufferSubData(reinterpret_cast<const CmdBufferSubData*>(payload),
                            payload + sizeof(CmdBufferSubData));
        break;
    case CMD_GEN_BUFFERS:
        HandleGenBuffers(reinterpret_cast<const CmdGenBuffers*>(payload),
                         reinterpret_cast<const uint32_t*>(payload + sizeof(CmdGenBuffers)));
        break;
    case CMD_VERTEX_ATTRIB_PTR:
    case CMD_VERTEX_ATTRIB_IPTR:
        HandleVertexAttribPointer(reinterpret_cast<const CmdVertexAttribPointer*>(payload));
        break;
    case CMD_ENABLE_VERTEX_ATTRIB:
        HandleEnableVertexAttrib(reinterpret_cast<const CmdEnableDisableVertexAttrib*>(payload));
        break;
    case CMD_DISABLE_VERTEX_ATTRIB:
        HandleDisableVertexAttrib(reinterpret_cast<const CmdEnableDisableVertexAttrib*>(payload));
        break;
    case CMD_BIND_TEXTURE:
        HandleBindTexture(reinterpret_cast<const CmdBindTexture*>(payload));
        break;
    case CMD_ACTIVE_TEXTURE:
        HandleActiveTexture(reinterpret_cast<const CmdActiveTexture*>(payload));
        break;
    case CMD_TEX_IMAGE_2D:
        HandleTexImage2D(reinterpret_cast<const CmdTexImage2D*>(payload),
                         payload + sizeof(CmdTexImage2D));
        break;
    case CMD_COMPRESSED_TEX_IMAGE_2D:
        HandleCompressedTexImage2D(reinterpret_cast<const CmdCompressedTexImage2D*>(payload),
                                    payload + sizeof(CmdCompressedTexImage2D));
        break;
    case CMD_UNIFORM_MATRIX_4FV:
        HandleUniformMatrix4fv(reinterpret_cast<const CmdUniformMatrix4fv*>(payload));
        break;
    case CMD_DRAW_ELEMENTS:
        HandleDrawElements(reinterpret_cast<const CmdDrawElements*>(payload), payload);
        break;
    case CMD_DRAW_ARRAYS:
        HandleDrawArrays(reinterpret_cast<const CmdDrawArrays*>(payload), payload);
        break;
    case CMD_USE_PROGRAM:
        HandleUseProgram(reinterpret_cast<const CmdUseProgram*>(payload));
        break;
    default:
        break;
    }
}

// ============================================================================
// Command handlers - mirror GpaDumpAnalyzeTool's state machine
// ============================================================================

void LiveCaptureProcessor::HandleBindBuffer(const CmdBindBuffer* cmd)
{
    int32_t idx = GetBindBufferTypeIndex(cmd->target);
    if (idx >= 0)
    {
        m_bind_buffer_list[idx] = cmd->buffer;
        m_render_states.binding_buffer_list[idx] = cmd->buffer;
    }
    m_current_bind_buffer = cmd->buffer;
}

void LiveCaptureProcessor::HandleBufferData(const CmdBufferData* cmd, const char* data)
{
    uint32_t buf_id = cmd->buffer_id;
    if (buf_id == 0) return;

    // Store the buffer data
    auto& buf = m_buffer_store[buf_id];
    buf.resize(cmd->size);
    if (data && cmd->size > 0)
        memcpy(buf.data(), data, cmd->size);
}

void LiveCaptureProcessor::HandleBufferSubData(const CmdBufferSubData* cmd, const char* data)
{
    uint32_t buf_id = cmd->buffer_id;
    if (buf_id == 0) return;

    auto it = m_buffer_store.find(buf_id);
    if (it == m_buffer_store.end()) return;

    auto& buf = it->second;
    if (cmd->offset + cmd->size <= (uint32_t)buf.size() && data)
        memcpy(buf.data() + cmd->offset, data, cmd->size);
}

void LiveCaptureProcessor::HandleGenBuffers(const CmdGenBuffers* cmd, const uint32_t* ids)
{
    // Pre-create empty entries
    for (uint32_t i = 0; i < cmd->count; i++)
        m_buffer_store[ids[i]]; // creates empty vector
}

void LiveCaptureProcessor::HandleVertexAttribPointer(const CmdVertexAttribPointer* cmd)
{
    if (cmd->index < 256)
    {
        m_render_states.m_vertexStream[cmd->index].is_enabled =
            m_render_states.m_vertexStream[cmd->index].is_enabled; // preserve enabled state
        m_render_states.m_vertexStream[cmd->index].data_buffer_obj = cmd->bound_buffer;
        m_render_states.m_vertexStream[cmd->index].num_elements = cmd->size;
        m_render_states.m_vertexStream[cmd->index].data_type = (DataType)cmd->type;
        m_render_states.m_vertexStream[cmd->index].is_normalized = cmd->normalized;
        m_render_states.m_vertexStream[cmd->index].stride = cmd->stride;
        m_render_states.m_vertexStream[cmd->index].start_offset = cmd->offset;
    }
}

void LiveCaptureProcessor::HandleEnableVertexAttrib(const CmdEnableDisableVertexAttrib* cmd)
{
    if (cmd->index < 256)
        m_render_states.m_vertexStream[cmd->index].is_enabled = 1;
}

void LiveCaptureProcessor::HandleDisableVertexAttrib(const CmdEnableDisableVertexAttrib* cmd)
{
    if (cmd->index < 256)
        m_render_states.m_vertexStream[cmd->index].is_enabled = 0;
}

void LiveCaptureProcessor::HandleBindTexture(const CmdBindTexture* cmd)
{
    m_current_bind_texture = cmd->texture;

    // Track which texture is bound to which slot
    if (m_current_texture_slot < 256)
    {
        // Find or create index in texture list
        auto it = m_texture_store.find(cmd->texture);
        if (it != m_texture_store.end())
        {
            // Find index of this texture in the group's loaded_textures (if any)
            // For now, store the GL object ID as index
            m_render_states.m_textureSlot[m_current_texture_slot].index_in_list = cmd->texture;
        }
    }
}

void LiveCaptureProcessor::HandleActiveTexture(const CmdActiveTexture* cmd)
{
    m_current_texture_slot = cmd->texture - 0x84c0; // GL_TEXTURE0
}

void LiveCaptureProcessor::HandleTexImage2D(const CmdTexImage2D* cmd, const char* data)
{
    uint32_t tex_id = cmd->texture_id;
    if (tex_id == 0) return;

    auto it = m_texture_store.find(tex_id);
    core::Texture2DInfo* tex = nullptr;
    if (it == m_texture_store.end())
    {
        tex = new core::Texture2DInfo();
        tex->m_objectId = tex_id;
        tex->m_levelCount = 0;
        m_texture_store[tex_id] = tex;
    }
    else
    {
        tex = it->second;
    }

    tex->m_internalFormat = cmd->internalformat;
    tex->m_format = cmd->format;
    tex->m_type = cmd->type;

    uint32_t level = cmd->level;
    if (level < 15)
    {
        tex->m_mips[level].m_width = cmd->width;
        tex->m_mips[level].m_height = cmd->height;
        tex->m_mips[level].m_size = cmd->data_size;

        if (cmd->data_size > 0 && data)
        {
            tex->m_mips[level].m_imageData = make_unique<char[]>(cmd->data_size);
            memcpy(tex->m_mips[level].m_imageData.get(), data, cmd->data_size);
        }

        if (level + 1 > tex->m_levelCount)
            tex->m_levelCount = level + 1;
    }
}

void LiveCaptureProcessor::HandleCompressedTexImage2D(const CmdCompressedTexImage2D* cmd, const char* data)
{
    uint32_t tex_id = cmd->texture_id;
    if (tex_id == 0) return;

    auto it = m_texture_store.find(tex_id);
    core::Texture2DInfo* tex = nullptr;
    if (it == m_texture_store.end())
    {
        tex = new core::Texture2DInfo();
        tex->m_objectId = tex_id;
        tex->m_levelCount = 0;
        m_texture_store[tex_id] = tex;
    }
    else
    {
        tex = it->second;
    }

    tex->m_internalFormat = cmd->internalformat;
    tex->m_format = cmd->internalformat; // For compressed, format = internalformat
    tex->m_type = 0;

    uint32_t level = cmd->level;
    if (level < 15)
    {
        tex->m_mips[level].m_width = cmd->width;
        tex->m_mips[level].m_height = cmd->height;
        tex->m_mips[level].m_size = cmd->imageSize;

        if (cmd->imageSize > 0 && data)
        {
            tex->m_mips[level].m_imageData = make_unique<char[]>(cmd->imageSize);
            memcpy(tex->m_mips[level].m_imageData.get(), data, cmd->imageSize);
        }

        if (level + 1 > tex->m_levelCount)
            tex->m_levelCount = level + 1;
    }
}

void LiveCaptureProcessor::HandleUniformMatrix4fv(const CmdUniformMatrix4fv* cmd)
{
    core::matrix4f mat;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            mat(r, c) = cmd->matrix[r * 4 + c];

    m_render_states.m_transform_matrix = mat;
    m_matrix_stack.push_back(mat);

    if (!m_has_first_matrix)
    {
        core::matrix4d mat_d;
        Matrix4fToMatrix4d(mat, mat_d);
        m_first_inv_transform_matrix = inverse(mat_d);
        m_render_states.m_first_inv_transform_matrix = m_first_inv_transform_matrix;
        m_has_first_matrix = true;
    }
}

void LiveCaptureProcessor::HandleDrawElements(const CmdDrawElements* cmd, const char* payload)
{
    // Reconstruct RenderingStates from the snapshot in the draw call record
    RenderingStates draw_state = m_render_states;

    draw_state.draw_call_params.element_buffer_obj = cmd->element_buffer;
    draw_state.draw_call_params.primitive_type = cmd->mode;
    draw_state.draw_call_params.num_indexes = cmd->count;
    draw_state.draw_call_params.data_type = (DataType)cmd->type;
    draw_state.draw_call_params.data_offset = cmd->offset;

    // Read vertex attrib snapshot
    const CmdVertexAttribSnapshot* attribs =
        reinterpret_cast<const CmdVertexAttribSnapshot*>(payload + sizeof(CmdDrawElements));

    for (uint32_t i = 0; i < cmd->num_attribs && i < 256; i++)
    {
        draw_state.m_vertexStream[i].is_enabled = attribs[i].is_enabled;
        draw_state.m_vertexStream[i].data_buffer_obj = attribs[i].data_buffer_obj;
        draw_state.m_vertexStream[i].num_elements = attribs[i].num_elements;
        draw_state.m_vertexStream[i].data_type = (DataType)attribs[i].data_type;
        draw_state.m_vertexStream[i].is_normalized = attribs[i].is_normalized;
        draw_state.m_vertexStream[i].stride = attribs[i].stride;
        draw_state.m_vertexStream[i].start_offset = attribs[i].start_offset;
    }

    // Read transform matrix
    const float* matrix = reinterpret_cast<const float*>(attribs + cmd->num_attribs);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            draw_state.m_transform_matrix(r, c) = matrix[r * 4 + c];

    // Read texture slot
    const uint32_t* tex_slot = reinterpret_cast<const uint32_t*>(matrix + 16);
    draw_state.m_textureSlot[0].index_in_list = *tex_slot;

    draw_state.m_first_inv_transform_matrix = m_first_inv_transform_matrix;

    ExtractMeshFromDrawCall(draw_state);
}

void LiveCaptureProcessor::HandleDrawArrays(const CmdDrawArrays* cmd, const char* payload)
{
    RenderingStates draw_state = m_render_states;

    draw_state.draw_call_params.element_buffer_obj = INVALID_VALUE;
    draw_state.draw_call_params.primitive_type = cmd->mode;
    draw_state.draw_call_params.num_indexes = cmd->count;
    draw_state.draw_call_params.data_type = (DataType)-1;
    draw_state.draw_call_params.data_offset = cmd->first;

    const CmdVertexAttribSnapshot* attribs =
        reinterpret_cast<const CmdVertexAttribSnapshot*>(payload + sizeof(CmdDrawArrays));

    for (uint32_t i = 0; i < cmd->num_attribs && i < 256; i++)
    {
        draw_state.m_vertexStream[i].is_enabled = attribs[i].is_enabled;
        draw_state.m_vertexStream[i].data_buffer_obj = attribs[i].data_buffer_obj;
        draw_state.m_vertexStream[i].num_elements = attribs[i].num_elements;
        draw_state.m_vertexStream[i].data_type = (DataType)attribs[i].data_type;
        draw_state.m_vertexStream[i].is_normalized = attribs[i].is_normalized;
        draw_state.m_vertexStream[i].stride = attribs[i].stride;
        draw_state.m_vertexStream[i].start_offset = attribs[i].start_offset;
    }

    const float* matrix = reinterpret_cast<const float*>(attribs + cmd->num_attribs);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            draw_state.m_transform_matrix(r, c) = matrix[r * 4 + c];

    const uint32_t* tex_slot = reinterpret_cast<const uint32_t*>(matrix + 16);
    draw_state.m_textureSlot[0].index_in_list = *tex_slot;

    draw_state.m_first_inv_transform_matrix = m_first_inv_transform_matrix;

    ExtractMeshFromDrawCall(draw_state);
}

void LiveCaptureProcessor::HandleUseProgram(const CmdUseProgram* cmd)
{
    // Track current program (for shader-related state)
}

// ============================================================================
// ExtractMeshFromDrawCall - adapted from CreateObjectFile in GpaDumpAnalyzeTool.cpp
// Uses buffer_store instead of DataZoneInfo for buffer access
// ============================================================================

static uint16_t read_ushort(const char* ptr)
{
    return *(const uint16_t*)ptr;
}

static float read_float(const char* ptr)
{
    return *(const float*)ptr;
}


void LiveCaptureProcessor::ExtractMeshFromDrawCall(const RenderingStates& state)
{
    bool has_mesh_data_texture = state.m_vertexStream[0].is_enabled &&
                                  state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1 &&
                                  state.m_vertexStream[3].is_enabled &&
                                  state.m_vertexStream[3].data_buffer_obj != (uint32_t)-1 &&
                                  state.draw_call_params.data_type != (DataType)-1 &&
                                  state.draw_call_params.element_buffer_obj != (uint32_t)-1;

    bool has_draw_data = state.m_vertexStream[0].is_enabled &&
                          state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1;

    bool is_ge_polygon = state.draw_call_params.primitive_type == kGlTriangleStrip;
    bool is_ge_mesh = state.draw_call_params.primitive_type == kGlTriangles;

    if (!has_mesh_data_texture && !(has_draw_data && is_ge_polygon))
    {
        if (g_logged_draws < kMaxLoggedDraws)
        {
            g_logged_draws++;
            CapLog("  draw mode=0x%X count=%u skipped: needs attrib 0 + attrib 3 + index buffer (triangles), or attrib 0 (strip); enabled:",
                   state.draw_call_params.primitive_type, state.draw_call_params.num_indexes);
            for (int i = 0; i < 16; i++)
                if (state.m_vertexStream[i].is_enabled) CapLog(" %d", i);
            CapLog("\n");
        }
        return;
    }

    // Extract vertex positions from buffer store
    vector<core::vec3f> position_data;
    vector<core::vec2f> texture_coord_data;
    vector<uint32_t> color_data;
    vector<uint32_t> index_data;

    bool log_this = g_logged_draws < kMaxLoggedDraws;
    if (log_this)
    {
        g_logged_draws++;
        CapLog("  draw mode=0x%X count=%u index_type=0x%X element_buf=%u offset=%u\n",
               state.draw_call_params.primitive_type, state.draw_call_params.num_indexes,
               uint32_t(state.draw_call_params.data_type), state.draw_call_params.element_buffer_obj,
               state.draw_call_params.data_offset);
        for (int i = 0; i < 16; i++)
        {
            const VertexAttrib& va = state.m_vertexStream[i];
            if (!va.is_enabled) continue;
            auto bit = m_buffer_store.find(va.data_buffer_obj);
            CapLog("    attrib %d: buf=%u (%s, %u bytes) comps=%u type=0x%X norm=%u stride=%u offset=%u\n",
                   i, va.data_buffer_obj, bit != m_buffer_store.end() ? "have data" : "NO DATA",
                   bit != m_buffer_store.end() ? unsigned(bit->second.size()) : 0u,
                   va.num_elements, uint32_t(va.data_type), va.is_normalized, va.stride, va.start_offset);
        }
    }

    std::vector<std::array<float, 4>> stream;

    // Stream 0: positions
    if (state.m_vertexStream[0].is_enabled && state.m_vertexStream[0].data_buffer_obj != INVALID_VALUE)
    {
        auto it = m_buffer_store.find(state.m_vertexStream[0].data_buffer_obj);
        if (it != m_buffer_store.end())
        {
            stream.clear();
            ReadAttributeStream(it->second, state.m_vertexStream[0], stream);
            position_data.reserve(stream.size());
            for (const auto& v : stream)
                position_data.push_back(core::vec3f(v[0], v[1], v[2]));
        }
    }

    // Stream 3: texture coordinates
    if (state.m_vertexStream[3].is_enabled && state.m_vertexStream[3].data_buffer_obj != INVALID_VALUE)
    {
        auto it = m_buffer_store.find(state.m_vertexStream[3].data_buffer_obj);
        if (it != m_buffer_store.end())
        {
            stream.clear();
            ReadAttributeStream(it->second, state.m_vertexStream[3], stream);
            texture_coord_data.reserve(stream.size());
            for (const auto& v : stream)
                texture_coord_data.push_back(core::vec2f(v[0], v[1]));
        }
    }

    // Stream 2: colors (for polygon/triangle strip), raw bytes packed into RGBA
    if (is_ge_polygon && state.m_vertexStream[2].is_enabled && state.m_vertexStream[2].data_buffer_obj != INVALID_VALUE)
    {
        auto it = m_buffer_store.find(state.m_vertexStream[2].data_buffer_obj);
        if (it != m_buffer_store.end() && state.m_vertexStream[2].start_offset < it->second.size())
        {
            const char* start = it->second.data() + state.m_vertexStream[2].start_offset;
            const char* end = it->second.data() + it->second.size();
            uint32_t num_el = state.m_vertexStream[2].num_elements > 4 ? 4 : state.m_vertexStream[2].num_elements;
            uint32_t stride = state.m_vertexStream[2].stride ? state.m_vertexStream[2].stride : num_el;

            while (num_el > 0 && start + num_el <= end)
            {
                uint32_t color = 0;
                for (uint32_t i = 0; i < num_el; i++)
                    color |= ((uint32_t)(uint8_t)start[i]) << (8 * i);
                color_data.push_back(color);
                start += stride;
            }
        }
    }

    // Index data, in the draw call's index type
    uint32_t index_size = GLTypeSize(uint32_t(state.draw_call_params.data_type));
    bool supported_index = state.draw_call_params.data_type == DataType(0x1401) ||
                           state.draw_call_params.data_type == DataType(0x1403) ||
                           state.draw_call_params.data_type == DataType(0x1405);
    if (supported_index && state.draw_call_params.element_buffer_obj != INVALID_VALUE &&
        (state.draw_call_params.primitive_type == kGlTriangles ||
         state.draw_call_params.primitive_type == kGlTriangleStrip ||
         state.draw_call_params.primitive_type == kGlLineStrip))
    {
        auto it = m_buffer_store.find(state.draw_call_params.element_buffer_obj);
        if (it != m_buffer_store.end() && state.draw_call_params.data_offset < it->second.size())
        {
            const char* start = it->second.data() + state.draw_call_params.data_offset;
            const char* end = it->second.data() + it->second.size();
            index_data.reserve(state.draw_call_params.num_indexes);
            for (uint32_t i = 0; i < state.draw_call_params.num_indexes && start + index_size <= end; i++, start += index_size)
            {
                if (index_size == 1)      index_data.push_back(*(const uint8_t*)start);
                else if (index_size == 2) index_data.push_back(read_ushort(start));
                else                      index_data.push_back(*(const uint32_t*)start);
            }
        }
    }

    if (log_this)
        CapLog("    -> %u positions, %u uvs, %u indices\n",
               unsigned(position_data.size()), unsigned(texture_coord_data.size()), unsigned(index_data.size()));

    // Deduplicate vertices (same as CreateObjectFile)
    vector<int32_t> index_match_table(position_data.size(), -1);
    vector<int32_t> new_index_match_table;
    new_index_match_table.reserve(position_data.size());

    for (uint32_t i = 0; i < index_data.size(); i++)
    {
        if (index_data[i] < index_match_table.size() && index_match_table[index_data[i]] < 0)
        {
            index_match_table[index_data[i]] = int32_t(new_index_match_table.size());
            new_index_match_table.push_back(index_data[i]);
        }
    }

    if (new_index_match_table.size() == 0)
    {
        if (g_logged_draws <= kMaxLoggedDraws)
            CapLog("    -> no mesh (no usable indices/positions)\n");
        return;
    }
    g_meshes_from_frame++;

    // Create MeshData
    MeshData* mesh_data = new MeshData;
    mesh_data->num_vertex = int32_t(new_index_match_table.size());
    uint32_t num_vertex = uint32_t(mesh_data->num_vertex);

    // Apply transformation matrix
    Matrix4fToMatrix4d(state.m_transform_matrix, mesh_data->dumpped_matrix);
    mesh_data->dumpped_matrix *= state.m_first_inv_transform_matrix;

    // Positions
    if (position_data.size() > 0)
    {
        mesh_data->vertex_list = make_unique<core::vec3f[]>(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
            mesh_data->vertex_list[i] = position_data[uint32_t(new_index_match_table[i])];
    }

    // UVs
    if (texture_coord_data.size() > 0)
    {
        mesh_data->uv_list = make_unique<core::vec2f[]>(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
        {
            uint32_t src_idx = uint32_t(new_index_match_table[i]);
            if (src_idx < texture_coord_data.size())
                mesh_data->uv_list[i] = texture_coord_data[src_idx];
        }
    }

    // Colors
    if (color_data.size() > 0)
    {
        mesh_data->color_list = make_unique<uint32_t[]>(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
        {
            uint32_t src_idx = uint32_t(new_index_match_table[i]);
            if (src_idx < color_data.size())
                mesh_data->color_list[i] = color_data[src_idx];
        }
    }

    // Draw calls with remapped indices
    if (index_data.size() > 0)
    {
        if (is_ge_polygon)
        {
            mesh_data->add_draw_call_list(kGlTriangleStrip, int32_t(index_data.size()), int32_t(num_vertex));
            DrawCallInfo& dc = mesh_data->get_last_draw_call_info();
            for (uint32_t i = 0; i < index_data.size(); i++)
            {
                if (index_data[i] < index_match_table.size())
                    dc.add_index(uint32_t(index_match_table[index_data[i]]));
            }
        }
        else if (is_ge_mesh)
        {
            mesh_data->add_draw_call_list(kGlTriangles, int32_t(index_data.size()), int32_t(num_vertex));
            DrawCallInfo& dc = mesh_data->get_last_draw_call_info();
            for (uint32_t i = 0; i + 2 < index_data.size(); i += 3)
            {
                if (index_data[i + 0] < index_match_table.size() &&
                    index_data[i + 1] < index_match_table.size() &&
                    index_data[i + 2] < index_match_table.size())
                {
                    dc.add_index(uint32_t(index_match_table[index_data[i + 0]]));
                    dc.add_index(uint32_t(index_match_table[index_data[i + 1]]));
                    dc.add_index(uint32_t(index_match_table[index_data[i + 2]]));
                }
            }
        }
    }

    // Texture reference
    mesh_data->idx_in_texture_list = state.m_textureSlot[0].index_in_list;

    // World bounds and translation, as the GPA dump import does without a KML
    // reference: vertices are row vectors transformed by dumpped_matrix (the
    // same matrix the renderer uses as model matrix).
    mesh_data->bbox_ws.Reset();
    if (mesh_data->vertex_list)
    {
        for (uint32_t i = 0; i < num_vertex; i++)
        {
            core::vec4d pos_ws = core::vec4d(mesh_data->vertex_list[i], 1.0) * mesh_data->dumpped_matrix;
            mesh_data->bbox_ws += core::vec3d(pos_ws);
        }
    }
    mesh_data->translation = mesh_data->dumpped_matrix.get_row(3);
    m_current_group->bbox_ws += mesh_data->bbox_ws;

    // Add to current group
    m_current_group->meshes.push_back(mesh_data);
}

// ============================================================================
// ProcessManager
// ============================================================================

ProcessManager::ProcessManager()
    : m_process(nullptr)
    , m_mapping(nullptr)
    , m_shared_mem(nullptr)
    , m_header(nullptr)
    , m_event_ready(nullptr)
    , m_event_control(nullptr)
    , m_ge_path("C:\\Program Files\\Google\\Google Earth Pro\\client\\googleearth.exe")
{
}

ProcessManager::~ProcessManager()
{
    StopGoogleEarth();
    DestroySharedMemory();
}

bool ProcessManager::CreateSharedMemory()
{
    // Create shared memory
    m_mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, GLCAPTURE_SHARED_MEM_SIZE, GLCAPTURE_SHARED_MEM_NAME);

    if (!m_mapping)
        return false;

    m_shared_mem = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, GLCAPTURE_SHARED_MEM_SIZE);
    if (!m_shared_mem)
    {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }

    // Initialize header
    memset(m_shared_mem, 0, GLCAPTURE_SHARED_MEM_SIZE);
    m_header = reinterpret_cast<GLCaptureHeader*>(m_shared_mem);

    // Create events
    m_event_ready = CreateEventA(nullptr, FALSE, FALSE, GLCAPTURE_EVENT_READY);
    m_event_control = CreateEventA(nullptr, FALSE, FALSE, GLCAPTURE_EVENT_CONTROL);

    return true;
}

void ProcessManager::DestroySharedMemory()
{
    if (m_shared_mem)
    {
        UnmapViewOfFile(m_shared_mem);
        m_shared_mem = nullptr;
        m_header = nullptr;
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

bool ProcessManager::StartGoogleEarth(const std::string& ge_path)
{
    if (!ge_path.empty())
        m_ge_path = ge_path;

    if (!m_mapping && !CreateSharedMemory())
        return false;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::string cmd = m_ge_path;

    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                         0, nullptr, nullptr, &si, &pi))
    {
        return false;
    }

    m_process = pi.hProcess;
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    // With the proxy opengl32.dll deployed next to googleearth.exe, the proxy
    // does the capture and draws its own overlay; injecting the overlay hook as
    // well would patch the proxy's wglSwapBuffers and draw a second overlay.
    std::string proxyPath = m_ge_path.substr(0, m_ge_path.find_last_of("\\/") + 1) + "opengl32.dll";
    if (GetFileAttributesA(proxyPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (m_on_process_started) m_on_process_started();
        return true;
    }

    // Wait for GE to initialize OpenGL, then inject our hook DLL
    Sleep(5000);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string hookDllPath(exePath);
    size_t lastSlash = hookDllPath.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        hookDllPath = hookDllPath.substr(0, lastSlash + 1);
    hookDllPath += "meshtool_hook.dll";

    InjectDLL(pid, hookDllPath);

    if (m_on_process_started) m_on_process_started();
    return true;
}

void ProcessManager::StopGoogleEarth()
{
    if (m_process)
    {
        TerminateProcess(m_process, 0);
        CloseHandle(m_process);
        m_process = nullptr;
        if (m_on_process_stopped) m_on_process_stopped();
    }
}

bool ProcessManager::IsRunning() const
{
    if (!m_process) return false;
    DWORD exit_code;
    GetExitCodeProcess(m_process, &exit_code);
    return exit_code == STILL_ACTIVE;
}

bool ProcessManager::RequestFrameCapture()
{
    if (!m_header) return false;
    m_header->capture_flags |= GLCAPTURE_FLAG_ACTIVE | GLCAPTURE_FLAG_FRAME_REQ;
    return true;
}

bool ProcessManager::IsHookConnected() const
{
    if (!m_header) return false;
    return (m_header->status_flags & GLCAPTURE_STATUS_CONNECTED) != 0;
}

bool ProcessManager::StartGoogleEarthWithKML(const std::string& kml_path)
{
    if (!m_mapping && !CreateSharedMemory())
        return false;

    // Launch Google Earth Pro with the KML file as argument
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::string cmd = "\"" + m_ge_path + "\" \"" + kml_path + "\"";

    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                         0, nullptr, nullptr, &si, &pi))
    {
        return false;
    }

    m_process = pi.hProcess;
    CloseHandle(pi.hThread);

    if (m_on_process_started) m_on_process_started();
    return true;
}

std::string ProcessManager::GenerateFlyToKML(double lon, double lat, double altitude,
                                              double heading, double tilt, double range)
{
    // Write a temporary KML file that flies Google Earth to the specified location
    std::string tmpPath = "meshtool_flyto.kml";

    std::ofstream f(tmpPath);
    if (!f) return "";

    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
      << "<Document>\n"
      << "  <name>MeshTool Capture Target</name>\n"
      << "  <Placemark>\n"
      << "    <name>Capture Location</name>\n"
      << "    <LookAt>\n"
      << "      <longitude>" << lon << "</longitude>\n"
      << "      <latitude>" << lat << "</latitude>\n"
      << "      <altitude>" << altitude << "</altitude>\n"
      << "      <heading>" << heading << "</heading>\n"
      << "      <tilt>" << tilt << "</tilt>\n"
      << "      <range>" << range << "</range>\n"
      << "      <altitudeMode>relativeToGround</altitudeMode>\n"
      << "    </LookAt>\n"
      << "    <Point>\n"
      << "      <coordinates>" << lon << "," << lat << "," << altitude << "</coordinates>\n"
      << "    </Point>\n"
      << "  </Placemark>\n"
      << "</Document>\n"
      << "</kml>\n";

    f.close();

    // Return absolute path
    char fullPath[MAX_PATH];
    GetFullPathNameA(tmpPath.c_str(), MAX_PATH, fullPath, nullptr);
    return std::string(fullPath);
}

std::string ProcessManager::GenerateSearchKML(const std::string& address)
{
    // Google Earth Pro can open a KML with a Placemark whose name is the search query.
    // Alternatively, we can use the "flytoview" query parameter in a NetworkLink.
    // The simplest approach: use the Google Earth "search" feature via a KML NetworkLink.
    std::string tmpPath = "meshtool_search.kml";

    std::ofstream f(tmpPath);
    if (!f) return "";

    // Use a Placemark with the address as the description.
    // Google Earth will geocode the address in the Placemark name.
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
      << "<Document>\n"
      << "  <name>MeshTool Search</name>\n"
      << "  <Placemark>\n"
      << "    <name>" << address << "</name>\n"
      << "    <description>MeshTool capture target</description>\n"
      << "    <Point>\n"
      << "      <coordinates>0,0,0</coordinates>\n"
      << "    </Point>\n"
      << "  </Placemark>\n"
      << "</Document>\n"
      << "</kml>\n";

    f.close();

    char fullPath[MAX_PATH];
    GetFullPathNameA(tmpPath.c_str(), MAX_PATH, fullPath, nullptr);
    return std::string(fullPath);
}
