#pragma once

#include <cstdint>
#include <windows.h>

// In-process GL state tracking for the hook DLL.
// Tracks bound buffers, active texture unit, vertex attribs, etc.
// This state is snapshotted at draw call time and sent via IPC.

#define MAX_VERTEX_ATTRIBS 16
#define MAX_TEXTURE_UNITS 32

struct HookVertexAttrib
{
    uint32_t    is_enabled;
    uint32_t    data_buffer_obj;
    uint32_t    num_elements;
    uint32_t    data_type;
    uint32_t    is_normalized;
    uint32_t    stride;
    uint32_t    start_offset;
};

struct HookGLState
{
    // Buffer bindings
    uint32_t    bound_array_buffer;
    uint32_t    bound_element_buffer;
    uint32_t    bound_uniform_buffer;

    // Texture state
    uint32_t    active_texture_unit;    // 0-based (GL_TEXTURE0 = 0)
    uint32_t    bound_texture[MAX_TEXTURE_UNITS];

    // Vertex attributes
    HookVertexAttrib vertex_attribs[MAX_VERTEX_ATTRIBS];

    // Shader
    uint32_t    current_program;

    // Matrix state (legacy)
    uint32_t    matrix_mode;

    // Transform matrix (from glUniformMatrix4fv)
    float       transform_matrix[16];
    bool        has_transform_matrix;

    void Reset();
};

// Global hook state (per-thread would be ideal but GE uses single GL thread)
extern HookGLState g_hook_state;

void HookStateInit();
