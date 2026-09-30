#define NOGDI
#include "glhook_overlay.h"
#undef NOGDI
#include "glhook_ipc_writer.h"
#include "glhook_state.h"
#include "glcapture_ipc.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// We use OpenGL immediate mode (available in GE's compat context) to draw the overlay.
// These are resolved from the real opengl32.dll at init time.

typedef void   (__stdcall *PFN_void_uint)(unsigned int);
typedef void   (__stdcall *PFN_void_4f)(float, float, float, float);
typedef void   (__stdcall *PFN_void_2f)(float, float);
typedef void   (__stdcall *PFN_void_1i)(int);
typedef void   (__stdcall *PFN_void_2i)(int, int);
typedef void   (__stdcall *PFN_void_0)(void);
typedef void   (__stdcall *PFN_void_bool)(unsigned char);
typedef void   (__stdcall *PFN_void_4i)(int, int, int, int);
typedef void   (__stdcall *PFN_pushattrib)(unsigned int);
typedef void   (__stdcall *PFN_matmode)(unsigned int);
typedef void   (__stdcall *PFN_ortho)(double, double, double, double, double, double);
typedef void   (__stdcall *PFN_getiv)(unsigned int, int*);

static PFN_void_uint    p_glEnable = nullptr;
static PFN_void_uint    p_glDisable = nullptr;
static PFN_void_uint    p_glBegin = nullptr;
static PFN_void_0       p_glEnd = nullptr;
static PFN_void_4f      p_glColor4f = nullptr;
static PFN_void_2f      p_glVertex2f = nullptr;
static PFN_pushattrib   p_glPushAttrib = nullptr;
static PFN_void_0       p_glPopAttrib = nullptr;
static PFN_matmode      p_glMatrixMode = nullptr;
static PFN_void_0       p_glPushMatrix = nullptr;
static PFN_void_0       p_glPopMatrix = nullptr;
static PFN_void_0       p_glLoadIdentity = nullptr;
static PFN_ortho        p_glOrtho = nullptr;
static PFN_void_4f      p_glBlendFunc2 = nullptr;
static PFN_void_bool    p_glDepthMask = nullptr;
static PFN_getiv        p_glGetIntegerv = nullptr;
static PFN_void_4i      p_glScissor = nullptr;
static PFN_void_4i      p_glViewport = nullptr;

static bool g_overlay_initialized = false;
static bool g_f12_was_pressed = false;
static int  g_frame_number = 0;
static float g_capture_flash = 0.0f;

// Simple 5x7 bitmap font for digits and uppercase letters
// Each char is 5 columns x 7 rows, stored as 7 bytes (each byte = 5 bits)
static const unsigned char g_font[][7] = {
    // 0-9
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x04,0x04,0x04}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    // A-Z (indices 10-35)
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x01,0x01,0x01,0x01,0x01,0x11,0x0E}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
    {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    // Special: space(36), colon(37), period(38), minus(39), F(40=same as F above), hash(41)
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, // :
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // .
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
};

static int CharIndex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c == ' ') return 36;
    if (c == ':') return 37;
    if (c == '.') return 38;
    if (c == '-') return 39;
    return 36; // space for unknown
}

static void DrawChar(float x, float y, float scale, char c, float r, float g, float b, float a)
{
    int idx = CharIndex(c);
    if (idx < 0 || idx >= 40) idx = 36;

    for (int row = 0; row < 7; row++)
    {
        unsigned char bits = g_font[idx][row];
        for (int col = 0; col < 5; col++)
        {
            if (bits & (0x10 >> col))
            {
                float px = x + col * scale;
                float py = y + row * scale;
                p_glColor4f(r, g, b, a);
                p_glVertex2f(px,         py);
                p_glVertex2f(px + scale, py);
                p_glVertex2f(px + scale, py + scale);
                p_glVertex2f(px,         py + scale);
            }
        }
    }
}

static void DrawText(float x, float y, float scale, const char* text, float r, float g, float b, float a)
{
    p_glBegin(0x0007); // GL_QUADS
    while (*text)
    {
        DrawChar(x, y, scale, *text, r, g, b, a);
        x += 6.0f * scale;
        text++;
    }
    p_glEnd();
}

static void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    p_glBegin(0x0007); // GL_QUADS
    p_glColor4f(r, g, b, a);
    p_glVertex2f(x,     y);
    p_glVertex2f(x + w, y);
    p_glVertex2f(x + w, y + h);
    p_glVertex2f(x,     y + h);
    p_glEnd();
}

// ============================================================================

void OverlayInit()
{
    // Resolve GL functions from real DLL
    extern HMODULE g_real_opengl32;
    if (!g_real_opengl32) return;

    p_glEnable       = (PFN_void_uint)GetProcAddress(g_real_opengl32, "glEnable");
    p_glDisable      = (PFN_void_uint)GetProcAddress(g_real_opengl32, "glDisable");
    p_glBegin        = (PFN_void_uint)GetProcAddress(g_real_opengl32, "glBegin");
    p_glEnd          = (PFN_void_0)GetProcAddress(g_real_opengl32, "glEnd");
    p_glColor4f      = (PFN_void_4f)GetProcAddress(g_real_opengl32, "glColor4f");
    p_glVertex2f     = (PFN_void_2f)GetProcAddress(g_real_opengl32, "glVertex2f");
    p_glPushAttrib   = (PFN_pushattrib)GetProcAddress(g_real_opengl32, "glPushAttrib");
    p_glPopAttrib    = (PFN_void_0)GetProcAddress(g_real_opengl32, "glPopAttrib");
    p_glMatrixMode   = (PFN_matmode)GetProcAddress(g_real_opengl32, "glMatrixMode");
    p_glPushMatrix   = (PFN_void_0)GetProcAddress(g_real_opengl32, "glPushMatrix");
    p_glPopMatrix    = (PFN_void_0)GetProcAddress(g_real_opengl32, "glPopMatrix");
    p_glLoadIdentity = (PFN_void_0)GetProcAddress(g_real_opengl32, "glLoadIdentity");
    p_glOrtho        = (PFN_ortho)GetProcAddress(g_real_opengl32, "glOrtho");
    p_glDepthMask    = (PFN_void_bool)GetProcAddress(g_real_opengl32, "glDepthMask");
    p_glGetIntegerv  = (PFN_getiv)GetProcAddress(g_real_opengl32, "glGetIntegerv");
    p_glViewport     = (PFN_void_4i)GetProcAddress(g_real_opengl32, "glViewport");

    // Resolve glBlendFunc with correct cast
    p_glBlendFunc2   = (PFN_void_4f)GetProcAddress(g_real_opengl32, "glBlendFunc");

    g_overlay_initialized = true;
}

void OverlayCheckHotkey()
{
    // F12 to trigger capture
    bool f12_now = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12_now && !g_f12_was_pressed)
    {
        // F12 pressed - request capture
        if (g_ipc_writer.IsConnected())
        {
            GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
            if (hdr)
            {
                hdr->capture_flags |= GLCAPTURE_FLAG_ACTIVE | GLCAPTURE_FLAG_FRAME_REQ;
                g_capture_flash = 1.0f;
            }
        }
    }
    g_f12_was_pressed = f12_now;
}

void OverlayRender(void* hdc)
{
    if (!g_overlay_initialized)
        OverlayInit();
    if (!g_overlay_initialized || !p_glBegin)
        return;

    g_frame_number++;

    // Log first few frames to confirm overlay is running
    if (g_frame_number <= 3)
    {
        FILE* f = fopen("C:\\meshtool_hook.log", "a");
        if (f) { fprintf(f, "OverlayRender frame %d, viewport %dx%d\n", g_frame_number, 0, 0); fclose(f); }
    }

    OverlayCheckHotkey();

    // Get viewport size
    int viewport[4];
    p_glGetIntegerv(0x0BA2, viewport); // GL_VIEWPORT

    int vw = viewport[2];
    int vh = viewport[3];
    if (vw <= 0 || vh <= 0) return;

    // Save GL state
    p_glPushAttrib(0x000FFFFF); // GL_ALL_ATTRIB_BITS

    // Setup 2D orthographic projection
    p_glMatrixMode(0x1701); // GL_PROJECTION
    p_glPushMatrix();
    p_glLoadIdentity();
    p_glOrtho(0, vw, vh, 0, -1, 1); // top-left origin

    p_glMatrixMode(0x1700); // GL_MODELVIEW
    p_glPushMatrix();
    p_glLoadIdentity();

    p_glDisable(0x0B71); // GL_DEPTH_TEST
    p_glDepthMask(0);
    p_glEnable(0x0BE2);  // GL_BLEND

    // Cast glBlendFunc properly (it takes 2 uints, not 4 floats)
    typedef void (__stdcall *PFN_blendfunc)(unsigned int, unsigned int);
    PFN_blendfunc realBlendFunc = (PFN_blendfunc)p_glBlendFunc2;
    realBlendFunc(0x0302, 0x0303); // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA

    p_glDisable(0x0DE1); // GL_TEXTURE_2D

    // ---- Draw overlay panel (top-left corner) ----
    float panelX = 10.0f;
    float panelY = 10.0f;
    float panelW = 280.0f;
    float panelH = 110.0f;
    float scale  = 2.5f; // font pixel scale

    // Panel background
    DrawRect(panelX, panelY, panelW, panelH, 0.0f, 0.0f, 0.0f, 0.70f);

    // Accent bar on left
    DrawRect(panelX, panelY, 4.0f, panelH, 0.2f, 0.6f, 1.0f, 0.9f);

    // Title
    float tx = panelX + 12.0f;
    float ty = panelY + 8.0f;
    DrawText(tx, ty, scale, "MESHTOOL", 0.3f, 0.7f, 1.0f, 1.0f);

    // Connection status
    ty += 22.0f;
    bool connected = g_ipc_writer.IsConnected();
    bool capturing = false;
    if (connected)
    {
        GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
        capturing = hdr && (hdr->status_flags & GLCAPTURE_STATUS_CAPTURING);
    }

    if (connected)
    {
        DrawRect(tx, ty + 3.0f, 8.0f, 8.0f, 0.2f, 0.9f, 0.3f, 1.0f); // green dot
        DrawText(tx + 14.0f, ty, scale, "CONNECTED", 0.8f, 0.9f, 0.8f, 1.0f);
    }
    else
    {
        DrawRect(tx, ty + 3.0f, 8.0f, 8.0f, 0.7f, 0.2f, 0.2f, 1.0f); // red dot
        DrawText(tx + 14.0f, ty, scale, "WAITING", 0.9f, 0.6f, 0.6f, 1.0f);
    }

    // Capture status / frame count
    ty += 22.0f;
    if (capturing)
    {
        DrawText(tx, ty, scale, "CAPTURING...", 1.0f, 0.9f, 0.3f, 1.0f);
    }
    else
    {
        char frameBuf[32];
        sprintf(frameBuf, "FRAME: %d", g_frame_number);
        DrawText(tx, ty, scale, frameBuf, 0.7f, 0.7f, 0.75f, 1.0f);
    }

    // Hotkey hint
    ty += 22.0f;
    DrawText(tx, ty, scale, "F12: CAPTURE", 0.5f, 0.5f, 0.6f, 0.8f);

    // Capture flash effect (brief white border flash when F12 pressed)
    if (g_capture_flash > 0.0f)
    {
        float a = g_capture_flash;
        // Flash border around the whole screen
        DrawRect(0, 0, (float)vw, 4, 0.3f, 0.8f, 1.0f, a);           // top
        DrawRect(0, (float)vh - 4, (float)vw, 4, 0.3f, 0.8f, 1.0f, a); // bottom
        DrawRect(0, 0, 4, (float)vh, 0.3f, 0.8f, 1.0f, a);            // left
        DrawRect((float)vw - 4, 0, 4, (float)vh, 0.3f, 0.8f, 1.0f, a); // right

        g_capture_flash -= 0.03f;
        if (g_capture_flash < 0.0f) g_capture_flash = 0.0f;
    }

    // Restore GL state
    p_glMatrixMode(0x1701); // GL_PROJECTION
    p_glPopMatrix();
    p_glMatrixMode(0x1700); // GL_MODELVIEW
    p_glPopMatrix();

    p_glPopAttrib();
}
