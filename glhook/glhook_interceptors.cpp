#define NOGDI
#include <windows.h>
#undef NOGDI
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include "glhook_ipc_writer.h"
#include "glhook_state.h"
#include "glhook_overlay.h"
#include "glcapture_ipc.h"

// Real function pointers (defined in glhook_main.cpp)
extern void* (__stdcall *real_wglGetProcAddress)(const char*);
extern int   (__stdcall *real_wglMakeCurrent)(void*, void*);

extern void  (__stdcall *real_glBindTexture)(unsigned int, unsigned int);
extern void  (__stdcall *real_glClear)(unsigned int);
extern void  (__stdcall *real_glCullFace)(unsigned int);
extern void  (__stdcall *real_glDepthFunc)(unsigned int);
extern void  (__stdcall *real_glDisable)(unsigned int);
extern void  (__stdcall *real_glDrawArrays)(unsigned int, int, int);
extern void  (__stdcall *real_glDrawElements)(unsigned int, int, unsigned int, const void*);
extern void  (__stdcall *real_glEnable)(unsigned int);
extern void  (__stdcall *real_glFrontFace)(unsigned int);
extern void  (__stdcall *real_glGenTextures)(int, unsigned int*);
extern void  (__stdcall *real_glLoadMatrixf)(const float*);
extern void  (__stdcall *real_glMatrixMode)(unsigned int);
extern void  (__stdcall *real_glTexImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
extern void  (__stdcall *real_glBlendFunc)(unsigned int, unsigned int);
extern void  (__stdcall *real_glViewport)(int, int, int, int);

// Extension function pointers
typedef void   (__stdcall *PFN_glBufferData)(unsigned int, ptrdiff_t, const void*, unsigned int);
typedef void   (__stdcall *PFN_glBufferSubData)(unsigned int, ptrdiff_t, ptrdiff_t, const void*);
typedef void   (__stdcall *PFN_glGenBuffers)(int, unsigned int*);
typedef void   (__stdcall *PFN_glBindBuffer)(unsigned int, unsigned int);
typedef void   (__stdcall *PFN_glVertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void*);
typedef void   (__stdcall *PFN_glVertexAttribIPointer)(unsigned int, int, unsigned int, int, const void*);
typedef void   (__stdcall *PFN_glEnableVertexAttribArray)(unsigned int);
typedef void   (__stdcall *PFN_glDisableVertexAttribArray)(unsigned int);
typedef void   (__stdcall *PFN_glActiveTexture)(unsigned int);
typedef void   (__stdcall *PFN_glCompressedTexImage2D)(unsigned int, int, unsigned int, int, int, int, int, const void*);
typedef void   (__stdcall *PFN_glUniformMatrix4fv)(int, int, unsigned char, const float*);
typedef void   (__stdcall *PFN_glShaderSource)(unsigned int, int, const char**, const int*);
typedef void   (__stdcall *PFN_glUseProgram)(unsigned int);

extern PFN_glBufferData              real_glBufferData;
extern PFN_glBufferSubData           real_glBufferSubData;
extern PFN_glGenBuffers              real_glGenBuffers;
typedef void   (__stdcall *PFN_glDeleteBuffers)(int, const unsigned int*);
extern PFN_glDeleteBuffers           real_glDeleteBuffers;
typedef void (__stdcall *PFN_glDrawRangeElements)(unsigned int, unsigned int, unsigned int, int, unsigned int, const void*);
static PFN_glDrawRangeElements real_glDrawRangeElements = nullptr;
extern PFN_glBindBuffer              real_glBindBuffer;
extern PFN_glVertexAttribPointer     real_glVertexAttribPointer;
extern PFN_glVertexAttribIPointer    real_glVertexAttribIPointer;
extern PFN_glEnableVertexAttribArray real_glEnableVertexAttribArray;
extern PFN_glDisableVertexAttribArray real_glDisableVertexAttribArray;
extern PFN_glActiveTexture           real_glActiveTexture;
extern PFN_glCompressedTexImage2D    real_glCompressedTexImage2D;
extern PFN_glUniformMatrix4fv       real_glUniformMatrix4fv;
extern PFN_glShaderSource           real_glShaderSource;
extern PFN_glUseProgram             real_glUseProgram;

// Defined in glhook_main.cpp
extern void EnsureRealLoaded();
extern void ProxyLog(const char* fmt, ...);
extern HMODULE g_real_opengl32;

static bool g_capturing_frame = false;

// Forward declarations for hooked extension functions (defined below extern "C" block)
void __stdcall hooked_glBufferData(unsigned int, ptrdiff_t, const void*, unsigned int);
void __stdcall hooked_glBufferSubData(unsigned int, ptrdiff_t, ptrdiff_t, const void*);
void __stdcall hooked_glGenBuffers(int, unsigned int*);
void __stdcall hooked_glDeleteBuffers(int, const unsigned int*);
void __stdcall hooked_glDrawRangeElements(unsigned int, unsigned int, unsigned int, int, unsigned int, const void*);
void __stdcall hooked_glBindBuffer(unsigned int, unsigned int);
void __stdcall hooked_glVertexAttribPointer(unsigned int, int, unsigned int, unsigned char, int, const void*);
void __stdcall hooked_glVertexAttribIPointer(unsigned int, int, unsigned int, int, const void*);
void __stdcall hooked_glEnableVertexAttribArray(unsigned int);
void __stdcall hooked_glDisableVertexAttribArray(unsigned int);
void __stdcall hooked_glActiveTexture(unsigned int);
void __stdcall hooked_glCompressedTexImage2D(unsigned int, int, unsigned int, int, int, int, int, const void*);
void __stdcall hooked_glUniformMatrix4fv(int, int, unsigned char, const float*);
void __stdcall hooked_glShaderSource(unsigned int, int, const char**, const int*);
void __stdcall hooked_glUseProgram(unsigned int);

// ============================================================================
// Shadow copies of buffer contents
// ============================================================================
// Google Earth uploads a tile's vertex/index buffers once, when the tile loads,
// long before anyone presses Capture. So every upload is mirrored here, and
// during a capture each buffer a draw call uses is sent (once per capture)
// right before that draw record.

static std::unordered_map<uint32_t, std::vector<uint8_t>> g_shadow_buffers;
static std::unordered_set<uint32_t> g_sent_buffers;   // sent in the current capture
static SRWLOCK g_shadow_lock = SRWLOCK_INIT;           // uploads may come from a loader thread

static void ShadowBufferData(uint32_t id, const void* data, size_t size)
{
    if (id == 0) return;
    AcquireSRWLockExclusive(&g_shadow_lock);
    std::vector<uint8_t>& buf = g_shadow_buffers[id];
    if (data)
        buf.assign((const uint8_t*)data, (const uint8_t*)data + size);
    else
        buf.assign(size, 0);
    ReleaseSRWLockExclusive(&g_shadow_lock);
}

static void ShadowBufferSubData(uint32_t id, size_t offset, const void* data, size_t size)
{
    if (id == 0 || !data) return;
    AcquireSRWLockExclusive(&g_shadow_lock);
    std::vector<uint8_t>& buf = g_shadow_buffers[id];
    if (buf.size() < offset + size)
        buf.resize(offset + size, 0);
    memcpy(buf.data() + offset, data, size);
    ReleaseSRWLockExclusive(&g_shadow_lock);
}

static void ShadowDeleteBuffers(int n, const unsigned int* ids)
{
    if (!ids) return;
    AcquireSRWLockExclusive(&g_shadow_lock);
    for (int i = 0; i < n; i++)
    {
        g_shadow_buffers.erase(ids[i]);
        g_sent_buffers.erase(ids[i]);
    }
    ReleaseSRWLockExclusive(&g_shadow_lock);
}

// Send the shadow copy of one buffer as a normal CMD_BUFFER_DATA record.
static void EmitShadowBuffer(uint32_t id)
{
    if (id == 0 || id == 0xFFFFFFFFu || g_sent_buffers.count(id))
        return;
    AcquireSRWLockShared(&g_shadow_lock);
    auto it = g_shadow_buffers.find(id);
    if (it != g_shadow_buffers.end())
    {
        uint32_t size = (uint32_t)it->second.size();
        void* payload = g_ipc_writer.BeginRecord(CMD_BUFFER_DATA, sizeof(CmdBufferData) + size);
        if (payload)
        {
            CmdBufferData* cmd = (CmdBufferData*)payload;
            cmd->target = 0;
            cmd->size = size;
            cmd->usage = 0;
            cmd->buffer_id = id;
            if (size)
                memcpy(cmd + 1, it->second.data(), size);
            g_ipc_writer.EndRecord();
            g_sent_buffers.insert(id);
        }
    }
    ReleaseSRWLockShared(&g_shadow_lock);
}

// Buffers referenced by the draw call about to be recorded.
static void EmitDrawResources(bool indexed)
{
    if (indexed)
        EmitShadowBuffer(g_hook_state.bound_element_buffer);
    for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++)
    {
        if (g_hook_state.vertex_attribs[i].is_enabled)
            EmitShadowBuffer(g_hook_state.vertex_attribs[i].data_buffer_obj);
    }
}

// Byte size of glTexImage2D client data, honouring GL_UNPACK_ALIGNMENT /
// GL_UNPACK_ROW_LENGTH; 0 if unknown or if a pixel unpack buffer is bound
// (then `data` is an offset, not a pointer).
static uint32_t TexImageDataSize(int width, int height, unsigned int format, unsigned int type)
{
    typedef void (__stdcall *PFN_GetIntegerv)(unsigned int, int*);
    static PFN_GetIntegerv getIntegerv = (PFN_GetIntegerv)GetProcAddress(g_real_opengl32, "glGetIntegerv");
    if (!getIntegerv || width <= 0 || height <= 0)
        return 0;

    int unpack_buffer = 0;
    getIntegerv(0x88EF, &unpack_buffer);   // GL_PIXEL_UNPACK_BUFFER_BINDING
    if (unpack_buffer)
        return 0;

    int components = 0;
    switch (format)
    {
    case 0x1903: case 0x1906: case 0x1909: case 0x1902: case 0x8D94: components = 1; break;  // RED, ALPHA, LUMINANCE, DEPTH, RED_INTEGER
    case 0x190A: case 0x8227: components = 2; break;                                        // LUMINANCE_ALPHA, RG
    case 0x1907: case 0x80E0: components = 3; break;                                        // RGB, BGR
    case 0x1908: case 0x80E1: components = 4; break;                                        // RGBA, BGRA
    }

    int bytes_per_pixel = 0;
    switch (type)
    {
    case 0x1400: case 0x1401: bytes_per_pixel = components; break;              // BYTE, UNSIGNED_BYTE
    case 0x1402: case 0x1403: case 0x140B: bytes_per_pixel = components * 2; break;  // SHORT, UNSIGNED_SHORT, HALF_FLOAT
    case 0x1404: case 0x1405: case 0x1406: bytes_per_pixel = components * 4; break;  // INT, UNSIGNED_INT, FLOAT
    case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: bytes_per_pixel = 2; break;  // packed 16-bit
    case 0x8035: case 0x8367: case 0x8036: case 0x8368: bytes_per_pixel = 4; break;                            // packed 32-bit
    }
    if (bytes_per_pixel == 0)
        return 0;

    int alignment = 4, row_length = 0;
    getIntegerv(0x0CF5, &alignment);    // GL_UNPACK_ALIGNMENT
    getIntegerv(0x0CF2, &row_length);   // GL_UNPACK_ROW_LENGTH
    if (alignment <= 0) alignment = 1;

    uint64_t row = uint64_t(row_length > 0 ? row_length : width) * bytes_per_pixel;
    uint64_t row_aligned = (row + alignment - 1) / alignment * alignment;
    uint64_t total = row_aligned * uint64_t(height - 1) + uint64_t(width) * bytes_per_pixel;
    return total > 0xFFFFFFFFull ? 0 : uint32_t(total);
}

// ============================================================================
// Frames and capture boundaries
// ============================================================================
// One frame = everything between two wglSwapBuffers calls. (Stencil clears
// were used before, but Google Earth clears the stencil several times a frame.)

struct FrameCallCounts
{
    uint32_t draw_elements, draw_range_elements, draw_arrays, clears, stencil_clears;
};
static FrameCallCounts g_frame_calls = {};
static uint32_t g_swap_count = 0;

static void OnFrameBoundary()
{
    g_swap_count++;
    if (g_swap_count <= 3 || g_swap_count % 1000 == 0 || g_capturing_frame)
    {
        ProxyLog("frame %u: glDrawElements=%u glDrawRangeElements=%u glDrawArrays=%u glClear=%u (stencil %u)%s\n",
                 g_swap_count, g_frame_calls.draw_elements, g_frame_calls.draw_range_elements,
                 g_frame_calls.draw_arrays, g_frame_calls.clears, g_frame_calls.stencil_clears,
                 g_capturing_frame ? "  [captured]" : "");
    }
    g_frame_calls = {};

    g_capture_stats.last_frame_draws = g_capture_stats.draws_this_frame;
    g_capture_stats.draws_this_frame = 0;

    GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
    if (g_capturing_frame)
    {
        g_ipc_writer.WriteRecord(CMD_FRAME_END, nullptr, 0);
        g_ipc_writer.SetRecording(false);
        g_ipc_writer.SignalReady();
        g_capturing_frame = false;
        g_ipc_writer.ClearStatus(GLCAPTURE_STATUS_CAPTURING);

        uint32_t end = hdr ? hdr->write_offset : 0;
        uint32_t start = g_capture_stats.capture_start_offset;
        g_capture_stats.last_capture_bytes = end >= start ? end - start : GLCAPTURE_RING_SIZE - start + end;
        g_capture_stats.last_capture_draws = g_capture_stats.captured_draws;
        g_capture_stats.captures_done++;
        ProxyLog("capture %u done: %u of %u draw calls recorded, %u buffers sent, %u bytes%s\n",
                 g_capture_stats.captures_done, g_capture_stats.captured_draws, g_capture_stats.last_frame_draws,
                 (unsigned)g_sent_buffers.size(), g_capture_stats.last_capture_bytes,
                 (hdr && (hdr->status_flags & GLCAPTURE_STATUS_OVERFLOW)) ? "  (RING OVERFLOW - data dropped)" : "");
    }

    if (g_ipc_writer.IsFrameRequested())
    {
        g_capturing_frame = true;
        g_ipc_writer.SetStatus(GLCAPTURE_STATUS_CAPTURING);
        g_ipc_writer.ClearFrameRequest();
        if (hdr) hdr->frame_count++;
        g_capture_stats.captured_draws = 0;
        g_capture_stats.capture_start_offset = hdr ? hdr->write_offset : 0;
        g_sent_buffers.clear();
        g_ipc_writer.SetRecording(true);
        ProxyLog("capture started (%u shadowed buffers)\n", (unsigned)g_shadow_buffers.size());
    }
}

// ============================================================================
// GL 1.1 Intercepted Functions (exported via .def file)
// ============================================================================

extern "C" {

__declspec(dllexport) void __stdcall glBindTexture(unsigned int target, unsigned int texture)
{
    EnsureRealLoaded();
    // Update state
    uint32_t unit = g_hook_state.active_texture_unit;
    if (unit < MAX_TEXTURE_UNITS)
        g_hook_state.bound_texture[unit] = texture;

    // Write IPC record
    CmdBindTexture cmd = { target, texture };
    g_ipc_writer.WriteRecord(CMD_BIND_TEXTURE, &cmd, sizeof(cmd));

    real_glBindTexture(target, texture);
}

__declspec(dllexport) void __stdcall glClear(unsigned int mask)
{
    EnsureRealLoaded();
    g_frame_calls.clears++;
    if (mask & 0x0400) // GL_STENCIL_BUFFER_BIT
        g_frame_calls.stencil_clears++;

    CmdClear cmd = { mask };
    g_ipc_writer.WriteRecord(CMD_CLEAR, &cmd, sizeof(cmd));

    real_glClear(mask);
}

__declspec(dllexport) void __stdcall glCullFace(unsigned int mode)
{
    EnsureRealLoaded();
    CmdCullFace cmd = { mode };
    g_ipc_writer.WriteRecord(CMD_CULL_FACE, &cmd, sizeof(cmd));
    real_glCullFace(mode);
}

__declspec(dllexport) void __stdcall glDepthFunc(unsigned int func)
{
    EnsureRealLoaded();
    CmdDepthFunc cmd = { func };
    g_ipc_writer.WriteRecord(CMD_DEPTH_FUNC, &cmd, sizeof(cmd));
    real_glDepthFunc(func);
}

__declspec(dllexport) void __stdcall glDisable(unsigned int cap)
{
    EnsureRealLoaded();
    g_ipc_writer.WriteRecord(CMD_DISABLE, &cap, sizeof(cap));
    real_glDisable(cap);
}

__declspec(dllexport) void __stdcall glDrawArrays(unsigned int mode, int first, int count)
{
    EnsureRealLoaded();
    g_frame_calls.draw_arrays++;
    g_capture_stats.draws_this_frame++;
    if (g_capturing_frame)
    {
        // Write draw call with state snapshot
        uint32_t snapshot_size = sizeof(CmdDrawArrays) +
                                  MAX_VERTEX_ATTRIBS * sizeof(CmdVertexAttribSnapshot) +
                                  16 * sizeof(float) +  // transform matrix
                                  sizeof(uint32_t);      // texture slot

        EmitDrawResources(false);
        void* payload = g_ipc_writer.BeginRecord(CMD_DRAW_ARRAYS, snapshot_size);
        if (payload)
        {
            CmdDrawArrays* cmd = (CmdDrawArrays*)payload;
            cmd->mode = mode;
            cmd->first = first;
            cmd->count = count;
            cmd->num_attribs = MAX_VERTEX_ATTRIBS;

            // Write vertex attrib snapshot
            CmdVertexAttribSnapshot* attribs = (CmdVertexAttribSnapshot*)(cmd + 1);
            for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++)
            {
                attribs[i].is_enabled = g_hook_state.vertex_attribs[i].is_enabled;
                attribs[i].data_buffer_obj = g_hook_state.vertex_attribs[i].data_buffer_obj;
                attribs[i].num_elements = g_hook_state.vertex_attribs[i].num_elements;
                attribs[i].data_type = g_hook_state.vertex_attribs[i].data_type;
                attribs[i].is_normalized = g_hook_state.vertex_attribs[i].is_normalized;
                attribs[i].stride = g_hook_state.vertex_attribs[i].stride;
                attribs[i].start_offset = g_hook_state.vertex_attribs[i].start_offset;
            }

            // Write transform matrix
            float* matrix = (float*)(attribs + MAX_VERTEX_ATTRIBS);
            memcpy(matrix, g_hook_state.transform_matrix, 16 * sizeof(float));

            // Write active texture slot
            uint32_t* tex_slot = (uint32_t*)(matrix + 16);
            *tex_slot = g_hook_state.bound_texture[0];

            g_ipc_writer.EndRecord();
            g_capture_stats.captured_draws++;
        }
    }

    real_glDrawArrays(mode, first, count);
}

} // extern "C"

// Records an indexed draw (glDrawElements / glDrawRangeElements) while capturing.
static void RecordDrawElements(unsigned int mode, int count, unsigned int type, const void* indices)
{
    g_capture_stats.draws_this_frame++;
    if (g_capturing_frame)
    {
        uint32_t snapshot_size = sizeof(CmdDrawElements) +
                                  MAX_VERTEX_ATTRIBS * sizeof(CmdVertexAttribSnapshot) +
                                  16 * sizeof(float) +
                                  sizeof(uint32_t);

        EmitDrawResources(true);
        void* payload = g_ipc_writer.BeginRecord(CMD_DRAW_ELEMENTS, snapshot_size);
        if (payload)
        {
            CmdDrawElements* cmd = (CmdDrawElements*)payload;
            cmd->mode = mode;
            cmd->count = count;
            cmd->type = type;
            cmd->offset = (uint32_t)(uintptr_t)indices;  // When using VBOs, indices is an offset
            cmd->element_buffer = g_hook_state.bound_element_buffer;
            cmd->num_attribs = MAX_VERTEX_ATTRIBS;

            CmdVertexAttribSnapshot* attribs = (CmdVertexAttribSnapshot*)(cmd + 1);
            for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++)
            {
                attribs[i].is_enabled = g_hook_state.vertex_attribs[i].is_enabled;
                attribs[i].data_buffer_obj = g_hook_state.vertex_attribs[i].data_buffer_obj;
                attribs[i].num_elements = g_hook_state.vertex_attribs[i].num_elements;
                attribs[i].data_type = g_hook_state.vertex_attribs[i].data_type;
                attribs[i].is_normalized = g_hook_state.vertex_attribs[i].is_normalized;
                attribs[i].stride = g_hook_state.vertex_attribs[i].stride;
                attribs[i].start_offset = g_hook_state.vertex_attribs[i].start_offset;
            }

            float* matrix = (float*)(attribs + MAX_VERTEX_ATTRIBS);
            memcpy(matrix, g_hook_state.transform_matrix, 16 * sizeof(float));

            uint32_t* tex_slot = (uint32_t*)(matrix + 16);
            *tex_slot = g_hook_state.bound_texture[0];

            g_ipc_writer.EndRecord();
            g_capture_stats.captured_draws++;
        }
    }

}

extern "C" {

__declspec(dllexport) void __stdcall glDrawElements(unsigned int mode, int count, unsigned int type, const void* indices)
{
    EnsureRealLoaded();
    g_frame_calls.draw_elements++;
    RecordDrawElements(mode, count, type, indices);
    real_glDrawElements(mode, count, type, indices);
}

__declspec(dllexport) void __stdcall glEnable(unsigned int cap)
{
    EnsureRealLoaded();
    g_ipc_writer.WriteRecord(CMD_ENABLE, &cap, sizeof(cap));
    real_glEnable(cap);
}

__declspec(dllexport) void __stdcall glFrontFace(unsigned int mode)
{
    EnsureRealLoaded();
    CmdFrontFace cmd = { mode };
    g_ipc_writer.WriteRecord(CMD_FRONT_FACE, &cmd, sizeof(cmd));
    real_glFrontFace(mode);
}

__declspec(dllexport) void __stdcall glGenTextures(int n, unsigned int* textures)
{
    EnsureRealLoaded();
    real_glGenTextures(n, textures);

    // Write after real call so we capture the generated IDs
    uint32_t payload_size = sizeof(CmdGenTextures) + n * sizeof(uint32_t);
    void* payload = g_ipc_writer.BeginRecord(CMD_GEN_TEXTURES, payload_size);
    if (payload)
    {
        CmdGenTextures* cmd = (CmdGenTextures*)payload;
        cmd->count = n;
        memcpy(cmd + 1, textures, n * sizeof(uint32_t));
        g_ipc_writer.EndRecord();
    }
}

__declspec(dllexport) void __stdcall glLoadMatrixf(const float* m)
{
    EnsureRealLoaded();
    CmdLoadMatrixf cmd;
    cmd.mode = g_hook_state.matrix_mode;
    memcpy(cmd.matrix, m, 16 * sizeof(float));
    g_ipc_writer.WriteRecord(CMD_LOAD_MATRIX_F, &cmd, sizeof(cmd));

    real_glLoadMatrixf(m);
}

__declspec(dllexport) void __stdcall glMatrixMode(unsigned int mode)
{
    EnsureRealLoaded();
    g_hook_state.matrix_mode = mode;
    g_ipc_writer.WriteRecord(CMD_MATRIX_MODE, &mode, sizeof(mode));
    real_glMatrixMode(mode);
}

__declspec(dllexport) void __stdcall glTexImage2D(unsigned int target, int level, int internalformat,
                                                    int width, int height, int border,
                                                    unsigned int format, unsigned int type, const void* data)
{
    EnsureRealLoaded();
    // Only computed while recording: it queries GL state.
    uint32_t data_size = (data && g_ipc_writer.IsRecording()) ? TexImageDataSize(width, height, format, type) : 0;

    uint32_t payload_size = sizeof(CmdTexImage2D) + data_size;
    void* payload = g_ipc_writer.BeginRecord(CMD_TEX_IMAGE_2D, payload_size);
    if (payload)
    {
        CmdTexImage2D* cmd = (CmdTexImage2D*)payload;
        cmd->target = target;
        cmd->level = level;
        cmd->internalformat = internalformat;
        cmd->width = width;
        cmd->height = height;
        cmd->border = border;
        cmd->format = format;
        cmd->type = type;
        cmd->data_size = data_size;
        uint32_t unit = g_hook_state.active_texture_unit;
        cmd->texture_id = (unit < MAX_TEXTURE_UNITS) ? g_hook_state.bound_texture[unit] : 0;

        if (data && data_size > 0)
            memcpy(cmd + 1, data, data_size);

        g_ipc_writer.EndRecord();
    }

    real_glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
}

__declspec(dllexport) void __stdcall glBlendFunc(unsigned int sfactor, unsigned int dfactor)
{
    EnsureRealLoaded();
    CmdBlendFunc cmd = { sfactor, dfactor };
    g_ipc_writer.WriteRecord(CMD_BLEND_FUNC, &cmd, sizeof(cmd));
    real_glBlendFunc(sfactor, dfactor);
}

__declspec(dllexport) void __stdcall glViewport(int x, int y, int width, int height)
{
    EnsureRealLoaded();
    CmdViewport cmd = { (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height };
    g_ipc_writer.WriteRecord(CMD_VIEWPORT, &cmd, sizeof(cmd));
    real_glViewport(x, y, width, height);
}

// ============================================================================
// wglGetProcAddress - critical hook for intercepting extension functions
// ============================================================================

__declspec(dllexport) void* __stdcall wglGetProcAddress(const char* name)
{
    EnsureRealLoaded();
    if (!real_wglGetProcAddress || !name)
        return nullptr;

    // Ask the driver first. Without a current context (Google Earth probes at
    // startup) or for an unsupported function this is null, and we must return
    // null too -- handing out our wrapper would make the app call a wrapper
    // whose real pointer is null.
    void* real = real_wglGetProcAddress(name);

    // Log each distinct name once (tells us which entry points GE really uses).
    static std::unordered_set<std::string> logged_names;
    if (logged_names.insert(name).second)
        ProxyLog("wglGetProcAddress(%s) -> %p\n", name, real);

    if (!real)
        return nullptr;

    // Functions we intercept: remember the driver pointer from this lookup and
    // return our wrapper. ARB/EXT names share the core wrapper and slot.
    struct Hook { const char* name; void** real_slot; void* wrapper; };
    static const Hook hooks[] = {
        { "glBufferData",               (void**)&real_glBufferData,               (void*)hooked_glBufferData },
        { "glBufferDataARB",            (void**)&real_glBufferData,               (void*)hooked_glBufferData },
        { "glBufferSubData",            (void**)&real_glBufferSubData,            (void*)hooked_glBufferSubData },
        { "glBufferSubDataARB",         (void**)&real_glBufferSubData,            (void*)hooked_glBufferSubData },
        { "glGenBuffers",               (void**)&real_glGenBuffers,               (void*)hooked_glGenBuffers },
        { "glGenBuffersARB",            (void**)&real_glGenBuffers,               (void*)hooked_glGenBuffers },
        { "glDeleteBuffers",            (void**)&real_glDeleteBuffers,            (void*)hooked_glDeleteBuffers },
        { "glDrawRangeElements",        (void**)&real_glDrawRangeElements,        (void*)hooked_glDrawRangeElements },
        { "glDrawRangeElementsEXT",     (void**)&real_glDrawRangeElements,        (void*)hooked_glDrawRangeElements },
        { "glDeleteBuffersARB",         (void**)&real_glDeleteBuffers,            (void*)hooked_glDeleteBuffers },
        { "glBindBuffer",               (void**)&real_glBindBuffer,               (void*)hooked_glBindBuffer },
        { "glBindBufferARB",            (void**)&real_glBindBuffer,               (void*)hooked_glBindBuffer },
        { "glVertexAttribPointer",      (void**)&real_glVertexAttribPointer,      (void*)hooked_glVertexAttribPointer },
        { "glVertexAttribIPointer",     (void**)&real_glVertexAttribIPointer,     (void*)hooked_glVertexAttribIPointer },
        { "glEnableVertexAttribArray",  (void**)&real_glEnableVertexAttribArray,  (void*)hooked_glEnableVertexAttribArray },
        { "glDisableVertexAttribArray", (void**)&real_glDisableVertexAttribArray, (void*)hooked_glDisableVertexAttribArray },
        { "glActiveTexture",            (void**)&real_glActiveTexture,            (void*)hooked_glActiveTexture },
        { "glActiveTextureARB",         (void**)&real_glActiveTexture,            (void*)hooked_glActiveTexture },
        { "glCompressedTexImage2D",     (void**)&real_glCompressedTexImage2D,     (void*)hooked_glCompressedTexImage2D },
        { "glCompressedTexImage2DARB",  (void**)&real_glCompressedTexImage2D,     (void*)hooked_glCompressedTexImage2D },
        { "glUniformMatrix4fv",         (void**)&real_glUniformMatrix4fv,         (void*)hooked_glUniformMatrix4fv },
        { "glShaderSource",             (void**)&real_glShaderSource,             (void*)hooked_glShaderSource },
        { "glUseProgram",               (void**)&real_glUseProgram,               (void*)hooked_glUseProgram },
    };
    for (const Hook& h : hooks)
    {
        if (strcmp(name, h.name) == 0)
        {
            *h.real_slot = real;
            return h.wrapper;
        }
    }
    return real;
}

__declspec(dllexport) int __stdcall wglMakeCurrent(void* hdc, void* hglrc)
{
    EnsureRealLoaded();
    int result = real_wglMakeCurrent(hdc, hglrc);

    // Connect to MeshTool's shared memory; retried until it exists, so MeshTool
    // may also be started after Google Earth.
    if (result && hglrc && !g_ipc_writer.IsConnected())
        g_ipc_writer.Init();

    return result;
}

// ============================================================================
// wglSwapBuffers - render overlay then present
// ============================================================================

__declspec(dllexport) int __stdcall wglSwapBuffers(void* hdc)
{
    EnsureRealLoaded();

    // The frame is complete: finish a running capture, start a requested one.
    // (The overlay below uses the real GL entry points, so it is not recorded.)
    OnFrameBoundary();

    // Render MeshTool overlay on top of Google Earth's frame
    OverlayRender(hdc);

    // Forward to real SwapBuffers
    typedef int (__stdcall *PFN_wglSwapBuffers)(void*);
    static PFN_wglSwapBuffers real_wglSwapBuffers = nullptr;
    if (!real_wglSwapBuffers)
        real_wglSwapBuffers = (PFN_wglSwapBuffers)GetProcAddress(g_real_opengl32, "wglSwapBuffers");

    return real_wglSwapBuffers(hdc);
}

} // extern "C"

// ============================================================================
// Extension function interceptors (returned by hooked wglGetProcAddress)
// ============================================================================

void __stdcall hooked_glBufferData(unsigned int target, ptrdiff_t size, const void* data, unsigned int usage)
{
    // Determine which buffer is bound
    uint32_t buffer_id = 0;
    if (target == 0x8892) // GL_ARRAY_BUFFER
        buffer_id = g_hook_state.bound_array_buffer;
    else if (target == 0x8893) // GL_ELEMENT_ARRAY_BUFFER
        buffer_id = g_hook_state.bound_element_buffer;

    ShadowBufferData(buffer_id, data, (size_t)size);

    // Write buffer data to IPC
    uint32_t payload_size = sizeof(CmdBufferData) + (data ? (uint32_t)size : 0);
    void* payload = g_ipc_writer.BeginRecord(CMD_BUFFER_DATA, payload_size);
    if (payload)
    {
        CmdBufferData* cmd = (CmdBufferData*)payload;
        cmd->target = target;
        cmd->size = (uint32_t)size;
        cmd->usage = usage;
        cmd->buffer_id = buffer_id;

        if (data && size > 0)
            memcpy(cmd + 1, data, (size_t)size);

        g_ipc_writer.EndRecord();
        g_sent_buffers.insert(buffer_id);   // the reader now has the latest contents
    }

    real_glBufferData(target, size, data, usage);
}

void __stdcall hooked_glBufferSubData(unsigned int target, ptrdiff_t offset, ptrdiff_t size, const void* data)
{
    uint32_t buffer_id = 0;
    if (target == 0x8892)
        buffer_id = g_hook_state.bound_array_buffer;
    else if (target == 0x8893)
        buffer_id = g_hook_state.bound_element_buffer;

    ShadowBufferSubData(buffer_id, (size_t)offset, data, (size_t)size);

    uint32_t payload_size = sizeof(CmdBufferSubData) + (data ? (uint32_t)size : 0);
    void* payload = g_ipc_writer.BeginRecord(CMD_BUFFER_SUB_DATA, payload_size);
    if (payload)
    {
        CmdBufferSubData* cmd = (CmdBufferSubData*)payload;
        cmd->target = target;
        cmd->offset = (uint32_t)offset;
        cmd->size = (uint32_t)size;
        cmd->buffer_id = buffer_id;

        if (data && size > 0)
            memcpy(cmd + 1, data, (size_t)size);

        g_ipc_writer.EndRecord();
    }

    real_glBufferSubData(target, offset, size, data);
}

void __stdcall hooked_glGenBuffers(int n, unsigned int* buffers)
{
    real_glGenBuffers(n, buffers);

    uint32_t payload_size = sizeof(CmdGenBuffers) + n * sizeof(uint32_t);
    void* payload = g_ipc_writer.BeginRecord(CMD_GEN_BUFFERS, payload_size);
    if (payload)
    {
        CmdGenBuffers* cmd = (CmdGenBuffers*)payload;
        cmd->count = n;
        memcpy(cmd + 1, buffers, n * sizeof(uint32_t));
        g_ipc_writer.EndRecord();
    }
}

void __stdcall hooked_glDrawRangeElements(unsigned int mode, unsigned int start, unsigned int end, int count, unsigned int type, const void* indices)
{
    g_frame_calls.draw_range_elements++;
    RecordDrawElements(mode, count, type, indices);
    real_glDrawRangeElements(mode, start, end, count, type, indices);
}

void __stdcall hooked_glDeleteBuffers(int n, const unsigned int* buffers)
{
    ShadowDeleteBuffers(n, buffers);
    real_glDeleteBuffers(n, buffers);
}

void __stdcall hooked_glBindBuffer(unsigned int target, unsigned int buffer)
{
    // Update state
    if (target == 0x8892)       // GL_ARRAY_BUFFER
        g_hook_state.bound_array_buffer = buffer;
    else if (target == 0x8893)  // GL_ELEMENT_ARRAY_BUFFER
        g_hook_state.bound_element_buffer = buffer;
    else if (target == 0x8a11)  // GL_UNIFORM_BUFFER
        g_hook_state.bound_uniform_buffer = buffer;

    CmdBindBuffer cmd = { target, buffer };
    g_ipc_writer.WriteRecord(CMD_BIND_BUFFER, &cmd, sizeof(cmd));

    real_glBindBuffer(target, buffer);
}

void __stdcall hooked_glVertexAttribPointer(unsigned int index, int size, unsigned int type,
                                             unsigned char normalized, int stride, const void* pointer)
{
    if (index < MAX_VERTEX_ATTRIBS)
    {
        g_hook_state.vertex_attribs[index].data_buffer_obj = g_hook_state.bound_array_buffer;
        g_hook_state.vertex_attribs[index].num_elements = size;
        g_hook_state.vertex_attribs[index].data_type = type;
        g_hook_state.vertex_attribs[index].is_normalized = normalized;
        g_hook_state.vertex_attribs[index].stride = stride;
        g_hook_state.vertex_attribs[index].start_offset = (uint32_t)(uintptr_t)pointer;
    }

    CmdVertexAttribPointer cmd;
    cmd.index = index;
    cmd.size = size;
    cmd.type = type;
    cmd.normalized = normalized;
    cmd.stride = stride;
    cmd.offset = (uint32_t)(uintptr_t)pointer;
    cmd.bound_buffer = g_hook_state.bound_array_buffer;
    g_ipc_writer.WriteRecord(CMD_VERTEX_ATTRIB_PTR, &cmd, sizeof(cmd));

    real_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

void __stdcall hooked_glVertexAttribIPointer(unsigned int index, int size, unsigned int type,
                                              int stride, const void* pointer)
{
    if (index < MAX_VERTEX_ATTRIBS)
    {
        g_hook_state.vertex_attribs[index].data_buffer_obj = g_hook_state.bound_array_buffer;
        g_hook_state.vertex_attribs[index].num_elements = size;
        g_hook_state.vertex_attribs[index].data_type = type;
        g_hook_state.vertex_attribs[index].is_normalized = 0;
        g_hook_state.vertex_attribs[index].stride = stride;
        g_hook_state.vertex_attribs[index].start_offset = (uint32_t)(uintptr_t)pointer;
    }

    CmdVertexAttribPointer cmd;
    cmd.index = index;
    cmd.size = size;
    cmd.type = type;
    cmd.normalized = 0;
    cmd.stride = stride;
    cmd.offset = (uint32_t)(uintptr_t)pointer;
    cmd.bound_buffer = g_hook_state.bound_array_buffer;
    g_ipc_writer.WriteRecord(CMD_VERTEX_ATTRIB_IPTR, &cmd, sizeof(cmd));

    real_glVertexAttribIPointer(index, size, type, stride, pointer);
}

void __stdcall hooked_glEnableVertexAttribArray(unsigned int index)
{
    if (index < MAX_VERTEX_ATTRIBS)
        g_hook_state.vertex_attribs[index].is_enabled = 1;

    CmdEnableDisableVertexAttrib cmd = { index };
    g_ipc_writer.WriteRecord(CMD_ENABLE_VERTEX_ATTRIB, &cmd, sizeof(cmd));

    real_glEnableVertexAttribArray(index);
}

void __stdcall hooked_glDisableVertexAttribArray(unsigned int index)
{
    if (index < MAX_VERTEX_ATTRIBS)
        g_hook_state.vertex_attribs[index].is_enabled = 0;

    CmdEnableDisableVertexAttrib cmd = { index };
    g_ipc_writer.WriteRecord(CMD_DISABLE_VERTEX_ATTRIB, &cmd, sizeof(cmd));

    real_glDisableVertexAttribArray(index);
}

void __stdcall hooked_glActiveTexture(unsigned int texture)
{
    g_hook_state.active_texture_unit = texture - 0x84c0; // GL_TEXTURE0

    CmdActiveTexture cmd = { texture };
    g_ipc_writer.WriteRecord(CMD_ACTIVE_TEXTURE, &cmd, sizeof(cmd));

    real_glActiveTexture(texture);
}

void __stdcall hooked_glCompressedTexImage2D(unsigned int target, int level, unsigned int internalformat,
                                              int width, int height, int border, int imageSize, const void* data)
{
    uint32_t payload_size = sizeof(CmdCompressedTexImage2D) + (data ? imageSize : 0);
    void* payload = g_ipc_writer.BeginRecord(CMD_COMPRESSED_TEX_IMAGE_2D, payload_size);
    if (payload)
    {
        CmdCompressedTexImage2D* cmd = (CmdCompressedTexImage2D*)payload;
        cmd->target = target;
        cmd->level = level;
        cmd->internalformat = internalformat;
        cmd->width = width;
        cmd->height = height;
        cmd->border = border;
        cmd->imageSize = imageSize;
        uint32_t unit = g_hook_state.active_texture_unit;
        cmd->texture_id = (unit < MAX_TEXTURE_UNITS) ? g_hook_state.bound_texture[unit] : 0;

        if (data && imageSize > 0)
            memcpy(cmd + 1, data, imageSize);

        g_ipc_writer.EndRecord();
    }

    real_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}

void __stdcall hooked_glUniformMatrix4fv(int location, int count, unsigned char transpose, const float* value)
{
    // Store the transform matrix in hook state
    if (value && count >= 1)
    {
        memcpy(g_hook_state.transform_matrix, value, 16 * sizeof(float));
        g_hook_state.has_transform_matrix = true;
    }

    CmdUniformMatrix4fv cmd;
    cmd.location = location;
    cmd.count = count;
    cmd.transpose = transpose;
    if (value)
        memcpy(cmd.matrix, value, 16 * sizeof(float));
    else
        memset(cmd.matrix, 0, 16 * sizeof(float));

    g_ipc_writer.WriteRecord(CMD_UNIFORM_MATRIX_4FV, &cmd, sizeof(cmd));

    real_glUniformMatrix4fv(location, count, transpose, value);
}

void __stdcall hooked_glShaderSource(unsigned int shader, int count, const char** string, const int* length)
{
    // Calculate total source length
    uint32_t total_len = 0;
    for (int i = 0; i < count; i++)
    {
        if (length && length[i] > 0)
            total_len += length[i];
        else if (string[i])
            total_len += (uint32_t)strlen(string[i]);
    }

    uint32_t payload_size = sizeof(CmdShaderSource) + total_len;
    void* payload = g_ipc_writer.BeginRecord(CMD_SHADER_SOURCE, payload_size);
    if (payload)
    {
        CmdShaderSource* cmd = (CmdShaderSource*)payload;
        cmd->shader = shader;
        cmd->length = total_len;

        char* dest = (char*)(cmd + 1);
        for (int i = 0; i < count; i++)
        {
            uint32_t len = 0;
            if (length && length[i] > 0)
                len = length[i];
            else if (string[i])
                len = (uint32_t)strlen(string[i]);

            if (len > 0 && string[i])
            {
                memcpy(dest, string[i], len);
                dest += len;
            }
        }

        g_ipc_writer.EndRecord();
    }

    real_glShaderSource(shader, count, string, length);
}

void __stdcall hooked_glUseProgram(unsigned int program)
{
    g_hook_state.current_program = program;

    CmdUseProgram cmd = { program };
    g_ipc_writer.WriteRecord(CMD_USE_PROGRAM, &cmd, sizeof(cmd));

    real_glUseProgram(program);
}

// ResolveExtensions is defined in glhook_main.cpp
