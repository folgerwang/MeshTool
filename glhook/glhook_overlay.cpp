#define NOGDI
#include "glhook_overlay.h"
#undef NOGDI
#include "glhook_ipc_writer.h"
#include "glhook_state.h"
#include "glhook_panel.h"
#include "glcapture_ipc.h"

// The overlay itself is drawn by glhook_panel.cpp (shared with meshtool_hook.dll);
// this file adds the proxy's capture link status, capture progress and the F12 hotkey.

extern HMODULE g_real_opengl32;
extern void* (__stdcall *real_wglGetProcAddress)(const char*);

static bool     g_f12_was_pressed = false;
static int      g_frame_number = 0;
static float    g_capture_flash = 0.0f;
static uint32_t g_seen_captures = 0;
static volatile uint32_t g_drawn_gate = 0;   // capture_gate the panel last showed

// Keep Google Earth rendering while a capture is pending or the done-flash fades:
// a capture only starts at the next frame boundary, and GE renders on demand.
static bool NeedsRedraw()
{
    GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
    if (hdr && (hdr->capture_flags & GLCAPTURE_FLAG_FRAME_REQ))
        return true;
    if (hdr && (hdr->status_flags & GLCAPTURE_STATUS_CAPTURING))
        return true;
    // MeshTool turns the F12 hint to READY once GE has settled, i.e. while GE
    // is idle and draws nothing: draw a frame to show it.
    if (hdr && hdr->capture_gate != g_drawn_gate)
        return true;
    return g_capture_flash > 0.0f;
}

void OverlayInit()
{
}

void OverlayCheckHotkey()
{
    // F12 to trigger capture
    bool f12_now = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12_now && !g_f12_was_pressed)
    {
        if (g_ipc_writer.IsConnected())
        {
            GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
            // While GE is still flying to / loading MeshTool's viewport camera
            // the capture would be thrown away: ignore the key.
            if (hdr && hdr->capture_gate != GLCAPTURE_GATE_WAIT)
                hdr->capture_flags |= GLCAPTURE_FLAG_ACTIVE | GLCAPTURE_FLAG_FRAME_REQ;
        }
    }
    g_f12_was_pressed = f12_now;
}

void OverlayRender(void* hdc)
{
    if (!g_real_opengl32 || !real_wglGetProcAddress)
        return;

    static bool redraw_hooked = false;
    if (!redraw_hooked)
    {
        redraw_hooked = true;
        PanelSetRedrawRequest(NeedsRedraw);
    }

    g_frame_number++;
    OverlayCheckHotkey();

    // Flash once per finished capture
    if (g_capture_stats.captures_done != g_seen_captures)
    {
        g_seen_captures = g_capture_stats.captures_done;
        g_capture_flash = 1.0f;
    }

    PanelStatus status = {};
    status.link = g_ipc_writer.IsConnected() ? PANEL_CONNECTED : PANEL_WAITING;
    status.frame = g_frame_number;
    status.flash = g_capture_flash;
    status.frame_draws = int(g_capture_stats.last_frame_draws);

    GLCaptureHeader* hdr = g_ipc_writer.GetHeader();
    if (status.link == PANEL_CONNECTED && hdr)
    {
        if (hdr->status_flags & GLCAPTURE_STATUS_CAPTURING)
        {
            status.capture = PANEL_CAPTURE_CAPTURING;
            status.captured_draws = int(g_capture_stats.captured_draws);
            uint32_t expected = g_capture_stats.last_frame_draws;
            status.progress = expected ? float(g_capture_stats.captured_draws) / float(expected) : 0.0f;
        }
        else if (hdr->capture_flags & GLCAPTURE_FLAG_FRAME_REQ)
            status.capture = PANEL_CAPTURE_WAITING_FRAME;
        else if (g_capture_stats.captures_done > 0)
            status.capture = PANEL_CAPTURE_DONE;
        else
            status.capture = PANEL_CAPTURE_READY;

        status.last_draws = int(g_capture_stats.last_capture_draws);
        status.last_mb = float(g_capture_stats.last_capture_bytes) / (1024.0f * 1024.0f);

        uint64_t write = hdr->write_offset, read = hdr->read_offset;
        uint64_t used = write >= read ? write - read : GLCAPTURE_RING_SIZE - read + write;
        status.buffer_fill = float(used) / float(GLCAPTURE_RING_SIZE);
        status.overflow = (hdr->status_flags & GLCAPTURE_STATUS_OVERFLOW) != 0;
        status.capture_gate = hdr->capture_gate;
        g_drawn_gate = status.capture_gate;
    }

    PanelDraw(hdc, g_real_opengl32, real_wglGetProcAddress, status);

    if (g_capture_flash > 0.0f)
    {
        g_capture_flash -= 0.03f;
        if (g_capture_flash < 0.0f) g_capture_flash = 0.0f;
    }
}
