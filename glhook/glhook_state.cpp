#include "glhook_state.h"
#include <cstring>

HookGLState g_hook_state;
CaptureStats g_capture_stats = {};

void HookGLState::Reset()
{
    bound_array_buffer = 0;
    bound_element_buffer = 0;
    bound_uniform_buffer = 0;
    active_texture_unit = 0;
    memset(bound_texture, 0, sizeof(bound_texture));
    memset(vertex_attribs, 0, sizeof(vertex_attribs));
    current_program = 0;
    matrix_mode = 0x1700; // GL_MODELVIEW
    memset(transform_matrix, 0, sizeof(transform_matrix));
    // Identity matrix
    transform_matrix[0] = 1.0f;
    transform_matrix[5] = 1.0f;
    transform_matrix[10] = 1.0f;
    transform_matrix[15] = 1.0f;
    has_transform_matrix = false;
}

void HookStateInit()
{
    g_hook_state.Reset();
}
