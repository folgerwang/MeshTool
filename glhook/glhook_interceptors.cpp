#define NOGDI
#include <windows.h>
#undef NOGDI
#include <cstring>
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
extern void ResolveExtensions();
extern HMODULE g_real_opengl32;

static bool g_extensions_resolved = false;
static bool g_capturing_frame = false;

// Forward declarations for hooked extension functions (defined below extern "C" block)
void __stdcall hooked_glBufferData(unsigned int, ptrdiff_t, const void*, unsigned int);
void __stdcall hooked_glBufferSubData(unsigned int, ptrdiff_t, ptrdiff_t, const void*);
void __stdcall hooked_glGenBuffers(int, unsigned int*);
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
// GL 1.1 Intercepted Functions (exported via .def file)
// ============================================================================

extern "C" {

__declspec(dllexport) void __stdcall glBindTexture(unsigned int target, unsigned int texture)
{
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
    // glClear is used as frame boundary detection
    CmdClear cmd = { mask };
    g_ipc_writer.WriteRecord(CMD_CLEAR, &cmd, sizeof(cmd));

    // Frame boundary: if stencil clear, mark frame end
    if (mask & 0x0400) // GL_STENCIL_BUFFER_BIT
    {
        if (g_capturing_frame)
        {
            g_ipc_writer.WriteRecord(CMD_FRAME_END, nullptr, 0);
            g_ipc_writer.SignalReady();
            g_capturing_frame = false;
            g_ipc_writer.ClearStatus(GLCAPTURE_STATUS_CAPTURING);
        }

        // Check if new frame capture requested
        if (g_ipc_writer.IsFrameRequested())
        {
            g_capturing_frame = true;
            g_ipc_writer.SetStatus(GLCAPTURE_STATUS_CAPTURING);
            g_ipc_writer.ClearFrameRequest();

            GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
            if (hdr) hdr->frame_count++;
        }
    }

    real_glClear(mask);
}

__declspec(dllexport) void __stdcall glCullFace(unsigned int mode)
{
    CmdCullFace cmd = { mode };
    g_ipc_writer.WriteRecord(CMD_CULL_FACE, &cmd, sizeof(cmd));
    real_glCullFace(mode);
}

__declspec(dllexport) void __stdcall glDepthFunc(unsigned int func)
{
    CmdDepthFunc cmd = { func };
    g_ipc_writer.WriteRecord(CMD_DEPTH_FUNC, &cmd, sizeof(cmd));
    real_glDepthFunc(func);
}

__declspec(dllexport) void __stdcall glDisable(unsigned int cap)
{
    g_ipc_writer.WriteRecord(CMD_DISABLE, &cap, sizeof(cap));
    real_glDisable(cap);
}

__declspec(dllexport) void __stdcall glDrawArrays(unsigned int mode, int first, int count)
{
    if (g_capturing_frame)
    {
        // Write draw call with state snapshot
        uint32_t snapshot_size = sizeof(CmdDrawArrays) +
                                  MAX_VERTEX_ATTRIBS * sizeof(CmdVertexAttribSnapshot) +
                                  16 * sizeof(float) +  // transform matrix
                                  sizeof(uint32_t);      // texture slot

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
        }
    }

    real_glDrawArrays(mode, first, count);
}

__declspec(dllexport) void __stdcall glDrawElements(unsigned int mode, int count, unsigned int type, const void* indices)
{
    if (g_capturing_frame)
    {
        uint32_t snapshot_size = sizeof(CmdDrawElements) +
                                  MAX_VERTEX_ATTRIBS * sizeof(CmdVertexAttribSnapshot) +
                                  16 * sizeof(float) +
                                  sizeof(uint32_t);

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
        }
    }

    real_glDrawElements(mode, count, type, indices);
}

__declspec(dllexport) void __stdcall glEnable(unsigned int cap)
{
    g_ipc_writer.WriteRecord(CMD_ENABLE, &cap, sizeof(cap));
    real_glEnable(cap);
}

__declspec(dllexport) void __stdcall glFrontFace(unsigned int mode)
{
    CmdFrontFace cmd = { mode };
    g_ipc_writer.WriteRecord(CMD_FRONT_FACE, &cmd, sizeof(cmd));
    real_glFrontFace(mode);
}

__declspec(dllexport) void __stdcall glGenTextures(int n, unsigned int* textures)
{
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
    CmdLoadMatrixf cmd;
    cmd.mode = g_hook_state.matrix_mode;
    memcpy(cmd.matrix, m, 16 * sizeof(float));
    g_ipc_writer.WriteRecord(CMD_LOAD_MATRIX_F, &cmd, sizeof(cmd));

    real_glLoadMatrixf(m);
}

__declspec(dllexport) void __stdcall glMatrixMode(unsigned int mode)
{
    g_hook_state.matrix_mode = mode;
    g_ipc_writer.WriteRecord(CMD_MATRIX_MODE, &mode, sizeof(mode));
    real_glMatrixMode(mode);
}

__declspec(dllexport) void __stdcall glTexImage2D(unsigned int target, int level, int internalformat,
                                                    int width, int height, int border,
                                                    unsigned int format, unsigned int type, const void* data)
{
    // Calculate data size for IPC
    uint32_t pixel_size = 4; // Approximate - RGBA8 most common
    uint32_t data_size = data ? (width * height * pixel_size) : 0;

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
    CmdBlendFunc cmd = { sfactor, dfactor };
    g_ipc_writer.WriteRecord(CMD_BLEND_FUNC, &cmd, sizeof(cmd));
    real_glBlendFunc(sfactor, dfactor);
}

__declspec(dllexport) void __stdcall glViewport(int x, int y, int width, int height)
{
    CmdViewport cmd = { (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height };
    g_ipc_writer.WriteRecord(CMD_VIEWPORT, &cmd, sizeof(cmd));
    real_glViewport(x, y, width, height);
}

// ============================================================================
// wglGetProcAddress - critical hook for intercepting extension functions
// ============================================================================

__declspec(dllexport) void* __stdcall wglGetProcAddress(const char* name)
{
    // Resolve real extension pointers on first call
    if (!g_extensions_resolved)
    {
        ResolveExtensions();
        g_extensions_resolved = true;
    }

    // Return our hooked version for functions we intercept
    if (strcmp(name, "glBufferData") == 0)            return (void*)hooked_glBufferData;
    if (strcmp(name, "glBufferSubData") == 0)          return (void*)hooked_glBufferSubData;
    if (strcmp(name, "glGenBuffers") == 0)             return (void*)hooked_glGenBuffers;
    if (strcmp(name, "glBindBuffer") == 0)             return (void*)hooked_glBindBuffer;
    if (strcmp(name, "glVertexAttribPointer") == 0)    return (void*)hooked_glVertexAttribPointer;
    if (strcmp(name, "glVertexAttribIPointer") == 0)   return (void*)hooked_glVertexAttribIPointer;
    if (strcmp(name, "glEnableVertexAttribArray") == 0)  return (void*)hooked_glEnableVertexAttribArray;
    if (strcmp(name, "glDisableVertexAttribArray") == 0) return (void*)hooked_glDisableVertexAttribArray;
    if (strcmp(name, "glActiveTexture") == 0)          return (void*)hooked_glActiveTexture;
    if (strcmp(name, "glCompressedTexImage2D") == 0)   return (void*)hooked_glCompressedTexImage2D;
    if (strcmp(name, "glUniformMatrix4fv") == 0)       return (void*)hooked_glUniformMatrix4fv;
    if (strcmp(name, "glShaderSource") == 0)           return (void*)hooked_glShaderSource;
    if (strcmp(name, "glUseProgram") == 0)             return (void*)hooked_glUseProgram;

    // Also handle ARB/EXT variants
    if (strcmp(name, "glBufferDataARB") == 0)          return (void*)hooked_glBufferData;
    if (strcmp(name, "glBufferSubDataARB") == 0)       return (void*)hooked_glBufferSubData;
    if (strcmp(name, "glGenBuffersARB") == 0)          return (void*)hooked_glGenBuffers;
    if (strcmp(name, "glBindBufferARB") == 0)          return (void*)hooked_glBindBuffer;
    if (strcmp(name, "glActiveTextureARB") == 0)       return (void*)hooked_glActiveTexture;
    if (strcmp(name, "glCompressedTexImage2DARB") == 0) return (void*)hooked_glCompressedTexImage2D;

    // For everything else, return the real function
    return real_wglGetProcAddress(name);
}

__declspec(dllexport) int __stdcall wglMakeCurrent(void* hdc, void* hglrc)
{
    int result = real_wglMakeCurrent(hdc, hglrc);

    if (result && hglrc && !g_extensions_resolved)
    {
        // GL context is now current - resolve extension functions
        ResolveExtensions();
        g_extensions_resolved = true;

        // Try to connect IPC if not already connected
        if (!g_ipc_writer.IsConnected())
            g_ipc_writer.Init();
    }

    return result;
}

// ============================================================================
// wglSwapBuffers - render overlay then present
// ============================================================================

__declspec(dllexport) int __stdcall wglSwapBuffers(void* hdc)
{
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
