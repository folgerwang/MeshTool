#pragma once

#include <cstdint>

// Shared memory IPC protocol between GLHookDLL and MeshTool.
// Both the proxy DLL and MeshTool include this header.

// "2": 64-bit ring offsets. A hook built for the old 32-bit layout opens the
// old name, finds nothing and stays "not connected" instead of misreading this.
#define GLCAPTURE_SHARED_MEM_NAME   "Local\\MeshToolGLCapture2"
#define GLCAPTURE_EVENT_READY       "Local\\MeshToolCaptureReady"
#define GLCAPTURE_EVENT_CONTROL     "Local\\MeshToolCaptureControl"

// 16 GB: a dense city frame with its textures passed 256 MB on a 4K screen.
// Only pages actually written use memory; ring offsets are 64-bit.
#define GLCAPTURE_SHARED_MEM_SIZE   (16ull * 1024 * 1024 * 1024)
#define GLCAPTURE_HEADER_SIZE       4096ull
#define GLCAPTURE_RING_SIZE         (GLCAPTURE_SHARED_MEM_SIZE - GLCAPTURE_HEADER_SIZE)

// Capture control flags (set by MeshTool in header)
#define GLCAPTURE_FLAG_ACTIVE       0x01    // Hook DLL should capture
#define GLCAPTURE_FLAG_FRAME_REQ    0x02    // Request capture of next frame
#define GLCAPTURE_FLAG_CONTINUOUS   0x04    // Capture every frame

// Status flags (set by hook DLL in header)
#define GLCAPTURE_STATUS_CONNECTED  0x01    // Hook DLL is loaded and connected
#define GLCAPTURE_STATUS_CAPTURING  0x02    // Currently capturing a frame
#define GLCAPTURE_STATUS_OVERFLOW   0x04    // Ring buffer overflowed

struct GLCaptureHeader
{
    // Written by producer (hook DLL)
    volatile uint64_t   write_offset;       // Current write position in ring buffer
    volatile uint32_t   frame_count;        // Total frames seen
    volatile uint32_t   status_flags;       // GLCAPTURE_STATUS_*

    // Written by consumer (MeshTool)
    volatile uint64_t   read_offset;        // Current read position in ring buffer
    volatile uint32_t   capture_flags;      // GLCAPTURE_FLAG_*
    uint32_t            _pad1;

    // Info (written once by hook DLL on init)
    uint32_t            gl_version_major;
    uint32_t            gl_version_minor;
    uint32_t            pid;                // Google Earth process ID

    // Written by consumer (MeshTool): may F12 capture now? (GLCAPTURE_GATE_*)
    volatile uint32_t   capture_gate;
    uint32_t            _reserved[52];      // Pad header to 256 bytes used, rest is spare
};

// capture_gate: while Google Earth follows MeshTool's viewport, a capture is
// only useful once GE has flown to the viewport camera and loaded the view.
enum : uint32_t
{
    GLCAPTURE_GATE_NONE  = 0,   // not following: F12 always captures
    GLCAPTURE_GATE_READY = 1,   // following, GE settled: F12 captures
    GLCAPTURE_GATE_WAIT  = 2,   // following, GE still moving/loading: F12 ignored
};

// Command record IDs
enum GLCaptureCmd : uint32_t
{
    CMD_NONE = 0,

    // Buffer operations
    CMD_GEN_BUFFERS         = 0x0100,
    CMD_BIND_BUFFER         = 0x0101,
    CMD_BUFFER_DATA         = 0x0102,
    CMD_BUFFER_SUB_DATA     = 0x0103,
    CMD_DELETE_BUFFERS      = 0x0104,

    // Vertex attribute operations
    CMD_VERTEX_ATTRIB_PTR   = 0x0200,
    CMD_VERTEX_ATTRIB_IPTR  = 0x0201,
    CMD_ENABLE_VERTEX_ATTRIB  = 0x0202,
    CMD_DISABLE_VERTEX_ATTRIB = 0x0203,
    CMD_BIND_VERTEX_ARRAY   = 0x0204,

    // Draw calls
    CMD_DRAW_ELEMENTS       = 0x0300,
    CMD_DRAW_ARRAYS         = 0x0301,

    // Texture operations
    CMD_GEN_TEXTURES        = 0x0400,
    CMD_BIND_TEXTURE        = 0x0401,
    CMD_ACTIVE_TEXTURE      = 0x0402,
    CMD_TEX_IMAGE_2D        = 0x0403,
    CMD_COMPRESSED_TEX_IMAGE_2D = 0x0404,
    CMD_DELETE_TEXTURES     = 0x0405,

    // Uniform / matrix operations
    CMD_UNIFORM_MATRIX_4FV  = 0x0500,
    CMD_LOAD_MATRIX_F       = 0x0501,
    CMD_MATRIX_MODE         = 0x0502,

    // Shader operations
    CMD_SHADER_SOURCE       = 0x0600,
    CMD_USE_PROGRAM         = 0x0601,

    // Frame / state
    CMD_CLEAR               = 0x0700,
    CMD_FRAME_END           = 0x0701,
    CMD_VIEWPORT            = 0x0702,
    CMD_ENABLE              = 0x0703,
    CMD_DISABLE             = 0x0704,

    // Render state
    CMD_CULL_FACE           = 0x0800,
    CMD_FRONT_FACE          = 0x0801,
    CMD_DEPTH_FUNC          = 0x0802,
    CMD_DEPTH_MASK          = 0x0803,
    CMD_BLEND_FUNC          = 0x0804,
};

// Each command record in the ring buffer starts with this header
struct GLCaptureRecord
{
    GLCaptureCmd    cmd_id;         // Command type
    uint32_t        total_size;     // Total record size including this header and payload
    uint32_t        flags;          // Reserved
};

// Payload structures for each command type
// These follow the GLCaptureRecord header in the ring buffer

struct CmdGenBuffers
{
    uint32_t    count;
    // Followed by count * uint32_t buffer IDs
};

struct CmdBindBuffer
{
    uint32_t    target;     // GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, etc.
    uint32_t    buffer;     // Buffer object ID
};

struct CmdBufferData
{
    uint32_t    target;
    uint32_t    size;
    uint32_t    usage;
    uint32_t    buffer_id;  // Currently bound buffer object ID
    // Followed by 'size' bytes of buffer data
};

struct CmdBufferSubData
{
    uint32_t    target;
    uint32_t    offset;
    uint32_t    size;
    uint32_t    buffer_id;
    // Followed by 'size' bytes of data
};

struct CmdVertexAttribPointer
{
    uint32_t    index;
    uint32_t    size;       // num components (1-4)
    uint32_t    type;       // GL data type
    uint32_t    normalized;
    uint32_t    stride;
    uint32_t    offset;     // pointer/offset value
    uint32_t    bound_buffer; // ARRAY_BUFFER bound at call time
};

struct CmdEnableDisableVertexAttrib
{
    uint32_t    index;
};

struct CmdDrawElements
{
    uint32_t    mode;       // primitive type
    uint32_t    count;      // number of indices
    uint32_t    type;       // index data type
    uint32_t    offset;     // offset into element buffer
    uint32_t    element_buffer; // bound ELEMENT_ARRAY_BUFFER
    // Snapshot of current state follows:
    uint32_t    num_attribs;    // number of VertexAttrib entries
    // Followed by num_attribs * CmdVertexAttribSnapshot
    // Followed by transform matrix (16 floats)
    // Followed by texture slot info (active texture unit + bound texture index)
};

struct CmdVertexAttribSnapshot
{
    uint32_t    is_enabled;
    uint32_t    data_buffer_obj;
    uint32_t    num_elements;
    uint32_t    data_type;
    uint32_t    is_normalized;
    uint32_t    stride;
    uint32_t    start_offset;
};

struct CmdDrawArrays
{
    uint32_t    mode;
    uint32_t    first;
    uint32_t    count;
    uint32_t    num_attribs;
    // Same snapshot format as CmdDrawElements
};

struct CmdGenTextures
{
    uint32_t    count;
    // Followed by count * uint32_t texture IDs
};

struct CmdBindTexture
{
    uint32_t    target;
    uint32_t    texture;
};

struct CmdActiveTexture
{
    uint32_t    texture;    // GL_TEXTURE0 + n
};

struct CmdTexImage2D
{
    uint32_t    target;
    uint32_t    level;
    uint32_t    internalformat;
    uint32_t    width;
    uint32_t    height;
    uint32_t    border;
    uint32_t    format;
    uint32_t    type;
    uint32_t    data_size;
    uint32_t    texture_id; // Currently bound texture
    // Followed by data_size bytes of pixel data
};

struct CmdCompressedTexImage2D
{
    uint32_t    target;
    uint32_t    level;
    uint32_t    internalformat;
    uint32_t    width;
    uint32_t    height;
    uint32_t    border;
    uint32_t    imageSize;
    uint32_t    texture_id;
    // Followed by imageSize bytes of compressed data
};

struct CmdUniformMatrix4fv
{
    uint32_t    location;
    uint32_t    count;
    uint32_t    transpose;
    float       matrix[16];
};

struct CmdLoadMatrixf
{
    uint32_t    mode;   // current matrix mode
    float       matrix[16];
};

struct CmdShaderSource
{
    uint32_t    shader;
    uint32_t    length;
    // Followed by 'length' bytes of shader source
};

struct CmdUseProgram
{
    uint32_t    program;
};

struct CmdClear
{
    uint32_t    mask;
};

struct CmdViewport
{
    uint32_t    x, y, width, height;
};

struct CmdCullFace
{
    uint32_t    mode;
};

struct CmdFrontFace
{
    uint32_t    mode;
};

struct CmdDepthFunc
{
    uint32_t    func;
};

struct CmdBlendFunc
{
    uint32_t    sfactor;
    uint32_t    dfactor;
};
