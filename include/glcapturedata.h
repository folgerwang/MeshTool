#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include "base.h"
#include "coremath.h"
#include "glfunctionlist.h"

// Structures shared between GLHookDLL and MeshTool for live OpenGL capture.
// Extracted from GpaDumpAnalyzeTool.cpp to enable both GPA dump and live capture paths.

enum BindBufferTypeList
{
    kGlArrayBufferIdx,
    kGlAtomicCounterBufferIdx,
    kGlCopyReadBufferIdx,
    kGlCopyWriteBufferIdx,
    kGlDispatchIndirectBufferIdx,
    kGlDrawIndirectBufferIdx,
    kGlElementArrayBufferIdx,
    kGlPixelPackBufferIdx,
    kGlPixelUnpackBufferIdx,
    kGlQueryBufferIdx,
    kGlShaderStorageBufferIdx,
    kGlTextureBufferIdx,
    kGlTransformFeedbackBufferIdx,
    kGlUniformBufferIdx,
    kGlNumBufferTypeIdx,
};

inline int32_t GetBindBufferTypeIndex(uint32_t buffer_type)
{
    switch (buffer_type)
    {
    case kGlArrayBuffer:              return kGlArrayBufferIdx;
    case kGlAtomicCounterBuffer:      return kGlAtomicCounterBufferIdx;
    case kGlCopyReadBuffer:           return kGlCopyReadBufferIdx;
    case kGlCopyWriteBuffer:          return kGlCopyWriteBufferIdx;
    case kGlDispatchIndirectBuffer:   return kGlDispatchIndirectBufferIdx;
    case kGlDrawIndirectBuffer:       return kGlDrawIndirectBufferIdx;
    case kGlElementArrayBuffer:       return kGlElementArrayBufferIdx;
    case kGlPixelPackBuffer:          return kGlPixelPackBufferIdx;
    case kGlPixelUnpackBuffer:        return kGlPixelUnpackBufferIdx;
    case kGlQueryBuffer:              return kGlQueryBufferIdx;
    case kGlShaderStorageBuffer:      return kGlShaderStorageBufferIdx;
    case kGlTextureBuffer:            return kGlTextureBufferIdx;
    case kGlTransformFeedbackBuffer:  return kGlTransformFeedbackBufferIdx;
    case kGlUniformBuffer:            return kGlUniformBufferIdx;
    default:                          return -1;
    }
}

struct VertexAttrib
{
    uint32_t    is_enabled;
    uint32_t    data_buffer_obj;
    uint32_t    num_elements;
    DataType    data_type;
    uint32_t    is_normalized;
    uint32_t    stride;
    uint32_t    start_offset;
};

struct DrawCallParameters
{
    uint32_t    element_buffer_obj;
    uint32_t    primitive_type;
    uint32_t    num_indexes;
    DataType    data_type;
    uint32_t    data_offset;
};

struct TextureSlotInfo
{
    uint32_t    index_in_list;
};

struct BufferDataInfo
{
    BufferType      buffer_type;
    uint32_t        buffer_obj_id;
    uint32_t        buffer_size;
    uint32_t        buffer_usage;
    union
    {
        uint32_t*   int_data_address;
        float*      float_data_address;
        uint16_t*   short_data_address;
        char*       byte_data_address;
    };
};

struct RenderingStates
{
    DrawCallParameters      draw_call_params;
    int32_t                 binding_buffer_list[kGlNumBufferTypeIdx];
    VertexAttrib            m_vertexStream[256];
    TextureSlotInfo         m_textureSlot[256];
    core::matrix4f          m_transform_matrix;
    core::matrix4d          m_first_inv_transform_matrix;

    RenderingStates()
    {
        for (int i = 0; i < 256; i++) m_textureSlot[i].index_in_list = INVALID_VALUE;
        for (int i = 0; i < 16; i++) m_vertexStream[i].is_enabled = 0;
        for (int i = 0; i < kGlNumBufferTypeIdx; i++) binding_buffer_list[i] = -1;
    }
};

inline uint32_t GetDataElementSize(uint32_t data_type)
{
    switch (data_type)
    {
    case kGlByte:
    case kGlUByte:
    case kGlUByte332:
    case kGlUByte332Rev:
        return 1;
    case kGlShort:
    case kGlUShort:
    case kGl2Bytes:
    case kGlUShort565:
    case kGlUShort565Rev:
    case kGlUShort4444:
    case kGlUShort4444Rev:
    case kGlUShort5551:
    case kGlUShort5551Rev:
        return 2;
    case kGl3Bytes:
        return 3;
    case kGlInt:
    case kGlUInt:
    case kGlFloat:
    case kGl4Bytes:
    case kGlUInt8888:
    case kGlUInt8888Rev:
    case kGlUInt1010102:
    case kGlUInt1010102Rev:
        return 4;
    case kGlDouble:
        return 8;
    }
    return 4;
}
