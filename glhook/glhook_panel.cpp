#include "glhook_panel.h"
#include <cstdio>
#include <cmath>

// ============================================================================
// GL entry points (fixed-function subset + the state we have to reset)
// ============================================================================

typedef void(__stdcall*PFN_PushAttrib)(unsigned int);
typedef void(__stdcall*PFN_Void)();
typedef void(__stdcall*PFN_UInt)(unsigned int);
typedef void(__stdcall*PFN_Ortho)(double,double,double,double,double,double);
typedef void(__stdcall*PFN_Color4f)(float,float,float,float);
typedef void(__stdcall*PFN_Vertex2f)(float,float);
typedef void(__stdcall*PFN_GetIV)(unsigned int,int*);
typedef void(__stdcall*PFN_BlendFunc)(unsigned int,unsigned int);
typedef void(__stdcall*PFN_DepthMask)(unsigned char);
typedef void(__stdcall*PFN_Viewport)(int,int,int,int);
typedef void(__stdcall*PFN_PolygonMode)(unsigned int,unsigned int);
typedef void(__stdcall*PFN_BindFramebuffer)(unsigned int,unsigned int);

static struct
{
    bool                resolved;
    PFN_PushAttrib      PushAttrib;
    PFN_Void            PopAttrib;
    PFN_UInt            MatrixMode;
    PFN_Void            PushMatrix;
    PFN_Void            PopMatrix;
    PFN_Void            LoadIdentity;
    PFN_Ortho           Ortho;
    PFN_UInt            Begin;
    PFN_Void            End;
    PFN_Color4f         Color4f;
    PFN_Vertex2f        Vertex2f;
    PFN_UInt            Enable;
    PFN_UInt            Disable;
    PFN_GetIV           GetIntegerv;
    PFN_BlendFunc       BlendFunc;
    PFN_DepthMask       DepthMask;
    PFN_Viewport        Viewport;
    PFN_PolygonMode     PolygonMode;
    // extensions (need a current context)
    PFN_UInt            UseProgram;
    PFN_UInt            BindVertexArray;
    PFN_UInt            ActiveTexture;
    PFN_BindFramebuffer BindFramebuffer;
} gl;

static void ResolveGL(HMODULE opengl32, PFN_PanelGetProcAddress get_proc)
{
    if (gl.resolved)
        return;
    gl.PushAttrib   = (PFN_PushAttrib)GetProcAddress(opengl32, "glPushAttrib");
    gl.PopAttrib    = (PFN_Void)GetProcAddress(opengl32, "glPopAttrib");
    gl.MatrixMode   = (PFN_UInt)GetProcAddress(opengl32, "glMatrixMode");
    gl.PushMatrix   = (PFN_Void)GetProcAddress(opengl32, "glPushMatrix");
    gl.PopMatrix    = (PFN_Void)GetProcAddress(opengl32, "glPopMatrix");
    gl.LoadIdentity = (PFN_Void)GetProcAddress(opengl32, "glLoadIdentity");
    gl.Ortho        = (PFN_Ortho)GetProcAddress(opengl32, "glOrtho");
    gl.Begin        = (PFN_UInt)GetProcAddress(opengl32, "glBegin");
    gl.End          = (PFN_Void)GetProcAddress(opengl32, "glEnd");
    gl.Color4f      = (PFN_Color4f)GetProcAddress(opengl32, "glColor4f");
    gl.Vertex2f     = (PFN_Vertex2f)GetProcAddress(opengl32, "glVertex2f");
    gl.Enable       = (PFN_UInt)GetProcAddress(opengl32, "glEnable");
    gl.Disable      = (PFN_UInt)GetProcAddress(opengl32, "glDisable");
    gl.GetIntegerv  = (PFN_GetIV)GetProcAddress(opengl32, "glGetIntegerv");
    gl.BlendFunc    = (PFN_BlendFunc)GetProcAddress(opengl32, "glBlendFunc");
    gl.DepthMask    = (PFN_DepthMask)GetProcAddress(opengl32, "glDepthMask");
    gl.Viewport     = (PFN_Viewport)GetProcAddress(opengl32, "glViewport");
    gl.PolygonMode  = (PFN_PolygonMode)GetProcAddress(opengl32, "glPolygonMode");
    if (get_proc)
    {
        gl.UseProgram      = (PFN_UInt)get_proc("glUseProgram");
        gl.BindVertexArray = (PFN_UInt)get_proc("glBindVertexArray");
        gl.ActiveTexture   = (PFN_UInt)get_proc("glActiveTexture");
        gl.BindFramebuffer = (PFN_BindFramebuffer)get_proc("glBindFramebuffer");
    }
    gl.resolved = true;
}

// ============================================================================
// 5x7 bitmap font and quad helpers (inside one glBegin(GL_QUADS) block)
// ============================================================================

static const unsigned char g_font[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x04,0x04,0x04},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x01,0x01,0x01,0x01,0x01,0x11,0x0E},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x00,0x04,0x04,0x00,0x04,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10},{0x18,0x19,0x02,0x04,0x08,0x13,0x03},
};

static int CharIdx(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c == ':') return 37;
    if (c == '.') return 38;
    if (c == '-') return 39;
    if (c == '/') return 40;
    if (c == '%') return 41;
    return 36;  // space / unknown
}

static void Rect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    gl.Color4f(r, g, b, a);
    gl.Vertex2f(x, y); gl.Vertex2f(x + w, y); gl.Vertex2f(x + w, y + h); gl.Vertex2f(x, y + h);
}

// Top-left origin: glyph row 0 is the top row.
static void Text(float x, float y, float scale, const char* text, float r, float g, float b, float a)
{
    gl.Color4f(r, g, b, a);
    for (; *text; text++, x += 6.0f * scale)
    {
        int idx = CharIdx(*text);
        for (int row = 0; row < 7; row++)
        {
            unsigned char bits = g_font[idx][row];
            for (int col = 0; col < 5; col++)
            {
                if (bits & (0x10 >> col))
                {
                    float px = x + col * scale, py = y + row * scale;
                    gl.Vertex2f(px, py); gl.Vertex2f(px + scale, py);
                    gl.Vertex2f(px + scale, py + scale); gl.Vertex2f(px, py + scale);
                }
            }
        }
    }
}

// ============================================================================
// Hover: 50% by default, opaque while the mouse is over the panel
// ============================================================================

static const float kPanelX = 20.0f;      // top-left corner, client pixels
static const float kPanelY = 20.0f;
static const float kPanelW = 640.0f;      // fits "FRAME nnnnn  nnnn DRAWS" + activity dots, and buffer % + F12 hint
static const float kPanelHBase    = 190.0f;  // title, link, frame counter
static const float kPanelHCapture = 290.0f;  // + capture state, progress bar, buffer
static volatile float g_panelH = kPanelHBase;
static const float kIdleOpacity  = 0.8f;
static const float kHoverOpacity = 1.0f;
static const float kFadeSeconds  = 0.15f;   // idle <-> hover transition time
static float g_panelOpacity = kIdleOpacity;  // written by the render thread only
static volatile HWND g_renderWnd = nullptr;  // window the panel is drawn into

// Mouse over the panel, and the render view is the window actually under the
// cursor (not covered by a menu or another window).
static bool IsMouseOverPanel(HWND wnd)
{
    POINT screen;
    if (!wnd || !GetCursorPos(&screen) || WindowFromPoint(screen) != wnd)
        return false;
    POINT pt = screen;
    if (!ScreenToClient(wnd, &pt))
        return false;
    return pt.x >= kPanelX && pt.x < kPanelX + kPanelW &&
           pt.y >= kPanelY && pt.y < kPanelY + g_panelH;
}

// Google Earth only redraws when something changes, so the hover state alone
// would never trigger a frame. Poll the mouse and request a repaint whenever
// the hover state flips or a fade is still in progress.
static bool (*volatile g_needsRedraw)() = nullptr;

void PanelSetRedrawRequest(bool (*needs_redraw)())
{
    g_needsRedraw = needs_redraw;
}

static DWORD WINAPI HoverWatchThread(LPVOID)
{
    bool last_hover = false;
    for (;;)
    {
        Sleep(30);
        HWND wnd = g_renderWnd;
        if (!wnd)
            continue;
        if (!IsWindow(wnd))
        {
            g_renderWnd = nullptr;
            continue;
        }
        bool hover = IsMouseOverPanel(wnd);
        float target = hover ? kHoverOpacity : kIdleOpacity;
        bool (*needs_redraw)() = g_needsRedraw;
        if (hover != last_hover || fabsf(g_panelOpacity - target) > 0.001f || (needs_redraw && needs_redraw()))
            InvalidateRect(wnd, nullptr, FALSE);
        last_hover = hover;
    }
}

static float UpdateOpacity(HWND wnd)
{
    static DWORD last_tick = 0;
    DWORD now = GetTickCount();
    float dt = last_tick ? float(now - last_tick) / 1000.0f : 0.0f;
    last_tick = now;

    float target = IsMouseOverPanel(wnd) ? kHoverOpacity : kIdleOpacity;
    float step = dt / kFadeSeconds * (kHoverOpacity - kIdleOpacity);
    if (g_panelOpacity < target)
        g_panelOpacity = (g_panelOpacity + step < target) ? g_panelOpacity + step : target;
    else
        g_panelOpacity = (g_panelOpacity - step > target) ? g_panelOpacity - step : target;
    return g_panelOpacity;
}

// ============================================================================

void PanelDraw(void* hdc, HMODULE opengl32, PFN_PanelGetProcAddress get_proc, const PanelStatus& status)
{
    if (!opengl32)
        return;
    ResolveGL(opengl32, get_proc);
    if (!gl.PushAttrib || !gl.Begin || !gl.GetIntegerv)
        return;

    HWND wnd = WindowFromDC((HDC)hdc);
    RECT rc = {};
    if (!wnd || !GetClientRect(wnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return;

    if (!g_renderWnd)
    {
        static bool watcher_started = false;
        if (!watcher_started)
        {
            watcher_started = true;
            CreateThread(nullptr, 0, HoverWatchThread, nullptr, 0, nullptr);
        }
    }
    g_renderWnd = wnd;

    float w = float(rc.right);
    float h = float(rc.bottom);
    float a = UpdateOpacity(wnd);

    // State not covered by glPushAttrib. Google Earth leaves its shader program
    // bound at SwapBuffers time; with it active, fixed-function quads go
    // through GE's shader and end up off-screen.
    int prevProgram = 0, prevVao = 0, prevActiveTex = 0, prevDrawFbo = 0;
    gl.GetIntegerv(0x8B8D, &prevProgram);    // GL_CURRENT_PROGRAM
    gl.GetIntegerv(0x85B5, &prevVao);        // GL_VERTEX_ARRAY_BINDING
    gl.GetIntegerv(0x84E0, &prevActiveTex);  // GL_ACTIVE_TEXTURE
    gl.GetIntegerv(0x8CA6, &prevDrawFbo);    // GL_DRAW_FRAMEBUFFER_BINDING

    gl.PushAttrib(0x000FFFFF);  // GL_ALL_ATTRIB_BITS

    if (gl.UseProgram)      gl.UseProgram(0);
    if (gl.BindVertexArray) gl.BindVertexArray(0);
    if (gl.ActiveTexture)   gl.ActiveTexture(0x84C0);          // GL_TEXTURE0
    if (gl.BindFramebuffer) gl.BindFramebuffer(0x8CA9, 0);      // GL_DRAW_FRAMEBUFFER -> window
    if (gl.Viewport)        gl.Viewport(0, 0, rc.right, rc.bottom);
    if (gl.PolygonMode)     gl.PolygonMode(0x0408, 0x1B02);     // GL_FRONT_AND_BACK, GL_FILL

    gl.Disable(0x0B71);  // GL_DEPTH_TEST
    gl.Disable(0x0DE1);  // GL_TEXTURE_2D
    gl.Disable(0x0B50);  // GL_LIGHTING
    gl.Disable(0x0B44);  // GL_CULL_FACE
    gl.Disable(0x0C11);  // GL_SCISSOR_TEST
    gl.Disable(0x0B90);  // GL_STENCIL_TEST
    gl.Disable(0x0BC0);  // GL_ALPHA_TEST
    gl.Disable(0x0BE0);  // GL_FOG
    gl.DepthMask(0);
    gl.Enable(0x0BE2);   // GL_BLEND
    gl.BlendFunc(0x0302, 0x0303);  // SRC_ALPHA, ONE_MINUS_SRC_ALPHA

    // Top-left origin, client pixels
    gl.MatrixMode(0x1701); gl.PushMatrix(); gl.LoadIdentity(); gl.Ortho(0, w, h, 0, -1, 1);  // GL_PROJECTION
    gl.MatrixMode(0x1700); gl.PushMatrix(); gl.LoadIdentity();                              // GL_MODELVIEW

    bool has_capture_rows = status.capture != PANEL_CAPTURE_NONE;
    g_panelH = has_capture_rows ? kPanelHCapture : kPanelHBase;
    float px = kPanelX, py = kPanelY, pw = kPanelW, ph = g_panelH;
    float sc = 5.0f;

    gl.Begin(0x0007);  // GL_QUADS

    Rect(px, py, pw, ph, 0.0f, 0.0f, 0.0f, 0.85f * a);    // background
    Rect(px, py, 6, ph, 0.25f, 0.6f, 1.0f, a);            // accent bar
    Text(px + 20, py + 18, sc, "MESHTOOL", 0.3f, 0.75f, 1.0f, a);

    // Link status
    switch (status.link)
    {
    case PANEL_CONNECTED:
        Rect(px + 20, py + 72, 18, 18, 0.2f, 0.95f, 0.3f, a);
        Text(px + 46, py + 72, sc * 0.7f, "CONNECTED", 0.7f, 0.9f, 0.7f, a);
        break;
    case PANEL_WAITING:
        Rect(px + 20, py + 72, 18, 18, 0.9f, 0.3f, 0.25f, a);
        Text(px + 46, py + 72, sc * 0.7f, "WAITING FOR MESHTOOL", 0.9f, 0.65f, 0.6f, a);
        break;
    default:
        Rect(px + 20, py + 72, 18, 18, 0.55f, 0.55f, 0.6f, a);
        Text(px + 46, py + 72, sc * 0.7f, "OVERLAY ONLY", 0.7f, 0.7f, 0.75f, a);
        break;
    }

    // Frame counter (+ draw calls per frame) and activity dots
    char buf[64];
    if (has_capture_rows)
        sprintf(buf, "FRAME %d  %d DRAWS", status.frame, status.frame_draws);
    else
        sprintf(buf, "FRAME %d", status.frame);
    Text(px + 20, py + 110, sc * 0.7f, buf, 0.6f, 0.6f, 0.7f, a);
    int dots = (status.frame / 20) % 4;
    for (int i = 0; i < dots; i++)
        Rect(px + pw - 100 + i * 20, py + 114, 12, 12, 0.3f, 0.7f, 1.0f, 0.9f * a);

    if (has_capture_rows)
    {
        // Capture state line
        float cr = 0.7f, cg = 0.7f, cb = 0.75f;
        float bar = 0.0f;
        switch (status.capture)
        {
        case PANEL_CAPTURE_WAITING_FRAME:
            sprintf(buf, "WAITING FOR FRAME...");
            cr = 1.0f; cg = 0.8f; cb = 0.3f;
            break;
        case PANEL_CAPTURE_CAPTURING:
            sprintf(buf, "CAPTURE %d%%  %d/%d", int(status.progress * 100.0f + 0.5f), status.captured_draws, status.frame_draws);
            cr = 1.0f; cg = 0.9f; cb = 0.3f;
            bar = status.progress;
            break;
        case PANEL_CAPTURE_DONE:
            sprintf(buf, "DONE %d DRAWS %.1f MB", status.last_draws, status.last_mb);
            cr = 0.5f; cg = 0.95f; cb = 0.6f;
            bar = 1.0f;
            break;
        default:
            sprintf(buf, "READY");
            break;
        }
        Text(px + 20, py + 150, sc * 0.65f, buf, cr, cg, cb, a);

        // Progress bar
        float bx = px + 20, by = py + 186, bw = pw - 40, bh = 16;
        Rect(bx, by, bw, bh, 0.2f, 0.2f, 0.25f, a);
        if (status.capture == PANEL_CAPTURE_WAITING_FRAME)
        {
            // indeterminate: a block sliding back and forth
            float t = float(GetTickCount() % 1200) / 600.0f;
            float pos = t < 1.0f ? t : 2.0f - t;
            Rect(bx + pos * (bw - 80), by, 80, bh, 1.0f, 0.8f, 0.3f, a);
        }
        else if (bar > 0.0f)
        {
            if (bar > 1.0f) bar = 1.0f;
            Rect(bx, by, bw * bar, bh, 0.3f, 0.75f, 1.0f, a);
        }

        // Shared buffer usage + hotkey hint
        sprintf(buf, status.overflow ? "BUFFER FULL %d%%" : "BUFFER %d%%", int(status.buffer_fill * 100.0f + 0.5f));
        if (status.overflow)
            Text(px + 20, py + 222, sc * 0.6f, buf, 1.0f, 0.35f, 0.3f, a);
        else
            Text(px + 20, py + 222, sc * 0.6f, buf, 0.5f, 0.5f, 0.6f, a);
        Text(px + pw - 20 - 12 * 6.0f * sc * 0.6f, py + 222, sc * 0.6f, "F12: CAPTURE", 0.45f, 0.45f, 0.55f, a);

        // Shared buffer bar (thin)
        Rect(bx, py + 254, bw, 6, 0.2f, 0.2f, 0.25f, a);
        float fill = status.buffer_fill > 1.0f ? 1.0f : status.buffer_fill;
        if (fill > 0.0f)
        {
            if (status.overflow)
                Rect(bx, py + 254, bw * fill, 6, 1.0f, 0.35f, 0.3f, a);
            else
                Rect(bx, py + 254, bw * fill, 6, 0.45f, 0.45f, 0.55f, a);
        }
    }

    // Capture feedback: cyan border around the whole view
    if (status.flash > 0.0f)
    {
        float fa = status.flash;
        Rect(0, 0, w, 6, 0.3f, 0.8f, 1.0f, fa);
        Rect(0, h - 6, w, 6, 0.3f, 0.8f, 1.0f, fa);
        Rect(0, 0, 6, h, 0.3f, 0.8f, 1.0f, fa);
        Rect(w - 6, 0, 6, h, 0.3f, 0.8f, 1.0f, fa);
    }

    gl.End();

    gl.MatrixMode(0x1701); gl.PopMatrix();
    gl.MatrixMode(0x1700); gl.PopMatrix();
    gl.PopAttrib();

    if (gl.BindFramebuffer) gl.BindFramebuffer(0x8CA9, (unsigned int)prevDrawFbo);
    if (gl.ActiveTexture)   gl.ActiveTexture((unsigned int)prevActiveTex);
    if (gl.BindVertexArray) gl.BindVertexArray((unsigned int)prevVao);
    if (gl.UseProgram)      gl.UseProgram((unsigned int)prevProgram);
}
