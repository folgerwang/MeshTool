#pragma once

// MeshTool status panel drawn inside Google Earth's OpenGL frame.
// Shared by the proxy opengl32.dll (capture) and meshtool_hook.dll (overlay only).

#define NOGDI
#include <windows.h>
#undef NOGDI

enum PanelLink
{
    PANEL_OVERLAY_ONLY,  // meshtool_hook.dll: no capture link at all
    PANEL_WAITING,       // proxy, MeshTool's shared memory not found yet
    PANEL_CONNECTED,     // proxy, connected to MeshTool
};

enum PanelCapture
{
    PANEL_CAPTURE_NONE,           // no capture rows (overlay-only DLL)
    PANEL_CAPTURE_READY,          // connected, nothing requested yet
    PANEL_CAPTURE_WAITING_FRAME,  // requested, waiting for the next frame boundary
    PANEL_CAPTURE_CAPTURING,      // recording draw calls of the current frame
    PANEL_CAPTURE_DONE,           // last capture finished (stats below)
};

struct PanelStatus
{
    PanelLink    link;
    PanelCapture capture;
    float        progress;        // 0..1 while capturing (draws so far / previous frame's draws)
    int          captured_draws;  // while capturing
    int          frame_draws;     // draw calls in the previous frame
    int          last_draws;      // PANEL_CAPTURE_DONE: draws in the finished capture
    float        last_mb;         // PANEL_CAPTURE_DONE: data size of the finished capture
    float        buffer_fill;     // 0..1 shared ring buffer usage
    bool         overflow;        // ring buffer overflowed, records were dropped
    int          frame;
    float        flash;           // 0..1, cyan border around the view (capture feedback)
};

typedef void* (__stdcall *PFN_PanelGetProcAddress)(const char*);

// Optional: polled every 30 ms by the panel's watcher thread; return true to
// make Google Earth render another frame (it only redraws on demand), e.g.
// while a capture is waiting for its frame.
void PanelSetRedrawRequest(bool (*needs_redraw)());

// Call from inside wglSwapBuffers, with Google Earth's context current.
// opengl32 is the real system opengl32.dll and get_proc its real
// wglGetProcAddress, so none of the panel's GL calls go through any hook.
// 80% opacity by default, opaque while the mouse is over the panel.
void PanelDraw(void* hdc, HMODULE opengl32, PFN_PanelGetProcAddress get_proc, const PanelStatus& status);
