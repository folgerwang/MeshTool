// Injectable GL hook DLL.
// Loaded into Google Earth via CreateRemoteThread + LoadLibrary.
// Hooks wglSwapBuffers by patching the real opengl32.dll's IAT or using
// a trampoline hook on the function itself.
//
// This DLL does NOT export any GL functions - it's not a proxy.
// Instead it patches function pointers after being injected.

#define NOGDI
#include <windows.h>
#undef NOGDI
#include <cstdio>
#include <cstring>
#include <cmath>

#include "glhook_panel.h"

// ============================================================================
// Trampoline hook: overwrite first bytes of target function with a JMP to ours
// ============================================================================

struct TrampolineHook
{
    void* target;           // Original function address
    void* hook;             // Our replacement
    unsigned char saved[16]; // Saved original bytes
    bool installed;

    TrampolineHook() : target(nullptr), hook(nullptr), installed(false) { memset(saved, 0, sizeof(saved)); }

    bool Install(void* targetFunc, void* hookFunc)
    {
        target = targetFunc;
        hook = hookFunc;

        // Save original bytes
        memcpy(saved, target, 14);

        // Write a 64-bit absolute JMP: FF 25 00 00 00 00 [8-byte address]
        DWORD oldProtect;
        if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        unsigned char jmp[14];
        jmp[0] = 0xFF;
        jmp[1] = 0x25;
        jmp[2] = 0x00;
        jmp[3] = 0x00;
        jmp[4] = 0x00;
        jmp[5] = 0x00;
        memcpy(jmp + 6, &hook, 8);

        memcpy(target, jmp, 14);
        VirtualProtect(target, 14, oldProtect, &oldProtect);

        installed = true;
        return true;
    }

    void CallOriginal_SwapBuffers(void* hdc, int* result)
    {
        // Temporarily restore original bytes, call, then re-patch
        DWORD oldProtect;
        VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(target, saved, 14);
        VirtualProtect(target, 14, oldProtect, &oldProtect);

        typedef int(__stdcall*F)(void*);
        *result = ((F)target)(hdc);

        VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
        unsigned char jmp[14];
        jmp[0] = 0xFF; jmp[1] = 0x25;
        jmp[2] = 0x00; jmp[3] = 0x00; jmp[4] = 0x00; jmp[5] = 0x00;
        memcpy(jmp + 6, &hook, 8);
        memcpy(target, jmp, 14);
        VirtualProtect(target, 14, oldProtect, &oldProtect);
    }
};

static TrampolineHook g_swapHook;
static HMODULE g_opengl32 = nullptr;
static int g_frameCount = 0;

// GL function pointers for overlay rendering
typedef void(__stdcall*PFN_PushAttrib)(unsigned int);
typedef void(__stdcall*PFN_PopAttrib)();
typedef void(__stdcall*PFN_MatMode)(unsigned int);
typedef void(__stdcall*PFN_PushMat)();
typedef void(__stdcall*PFN_PopMat)();
typedef void(__stdcall*PFN_LoadId)();
typedef void(__stdcall*PFN_Ortho)(double,double,double,double,double,double);
typedef void(__stdcall*PFN_Begin)(unsigned int);
typedef void(__stdcall*PFN_End)();
typedef void(__stdcall*PFN_Color4f)(float,float,float,float);
typedef void(__stdcall*PFN_Vertex2f)(float,float);
typedef void(__stdcall*PFN_Disable)(unsigned int);
typedef void(__stdcall*PFN_Enable)(unsigned int);
typedef void(__stdcall*PFN_GetIV)(unsigned int,int*);
typedef void(__stdcall*PFN_BlendFunc)(unsigned int,unsigned int);
typedef void(__stdcall*PFN_DepthMask)(unsigned char);

static PFN_PushAttrib pPushAttrib;
static PFN_PopAttrib pPopAttrib;
static PFN_MatMode pMatMode;
static PFN_PushMat pPushMat;
static PFN_PopMat pPopMat;
static PFN_LoadId pLoadId;
static PFN_Ortho pOrtho;
static PFN_Begin pBegin;
static PFN_End pEnd;
static PFN_Color4f pColor4f;
static PFN_Vertex2f pVertex2f;
static PFN_Disable pDisable;
static PFN_Enable pEnable;
static PFN_GetIV pGetIV;
static PFN_BlendFunc pBlendFunc;
static PFN_DepthMask pDepthMask;

static void ResolveGLFuncs()
{
    if (pPushAttrib) return;
    pPushAttrib = (PFN_PushAttrib)GetProcAddress(g_opengl32, "glPushAttrib");
    pPopAttrib  = (PFN_PopAttrib)GetProcAddress(g_opengl32, "glPopAttrib");
    pMatMode    = (PFN_MatMode)GetProcAddress(g_opengl32, "glMatrixMode");
    pPushMat    = (PFN_PushMat)GetProcAddress(g_opengl32, "glPushMatrix");
    pPopMat     = (PFN_PopMat)GetProcAddress(g_opengl32, "glPopMatrix");
    pLoadId     = (PFN_LoadId)GetProcAddress(g_opengl32, "glLoadIdentity");
    pOrtho      = (PFN_Ortho)GetProcAddress(g_opengl32, "glOrtho");
    pBegin      = (PFN_Begin)GetProcAddress(g_opengl32, "glBegin");
    pEnd        = (PFN_End)GetProcAddress(g_opengl32, "glEnd");
    pColor4f    = (PFN_Color4f)GetProcAddress(g_opengl32, "glColor4f");
    pVertex2f   = (PFN_Vertex2f)GetProcAddress(g_opengl32, "glVertex2f");
    pDisable    = (PFN_Disable)GetProcAddress(g_opengl32, "glDisable");
    pEnable     = (PFN_Enable)GetProcAddress(g_opengl32, "glEnable");
    pGetIV      = (PFN_GetIV)GetProcAddress(g_opengl32, "glGetIntegerv");
    pBlendFunc  = (PFN_BlendFunc)GetProcAddress(g_opengl32, "glBlendFunc");
    pDepthMask  = (PFN_DepthMask)GetProcAddress(g_opengl32, "glDepthMask");
}

// ============================================================================
// Our hooked wglSwapBuffers
// ============================================================================

// Debug: draw a solid red block with a scissored clear instead of the panel.
// A clear ignores shaders, matrices, blending and the context profile, so if
// this is not visible the hook is not being called (or not on the window's FBO).
#define OVERLAY_DEBUG_RED_BLOCK 0

typedef void(__stdcall*PFN_Scissor)(int,int,int,int);
typedef void(__stdcall*PFN_ClearColor)(float,float,float,float);
typedef void(__stdcall*PFN_Clear)(unsigned int);
typedef void(__stdcall*PFN_ColorMask)(unsigned char,unsigned char,unsigned char,unsigned char);
typedef void(__stdcall*PFN_GetFloatv)(unsigned int,float*);
typedef unsigned char(__stdcall*PFN_IsEnabled)(unsigned int);
typedef const unsigned char*(__stdcall*PFN_GetString)(unsigned int);
typedef void(__stdcall*PFN_BindFramebuffer)(unsigned int,unsigned int);
typedef void(__stdcall*PFN_UseProgram)(unsigned int);
typedef void*(__stdcall*PFN_wglGetProcAddress)(const char*);
typedef void*(__stdcall*PFN_wglGetCurrentContext)();

static void DrawDebugRedBlock(void* hdc)
{
    static PFN_Scissor         pScissor;
    static PFN_ClearColor      pClearColor;
    static PFN_Clear           pClear;
    static PFN_ColorMask       pColorMask;
    static PFN_GetFloatv       pGetFloatv;
    static PFN_IsEnabled       pIsEnabled;
    static PFN_GetString       pGetString;
    static PFN_BindFramebuffer pBindFramebuffer;
    static PFN_wglGetCurrentContext pWglGetCurrentContext;
    if (!pClear)
    {
        pScissor    = (PFN_Scissor)GetProcAddress(g_opengl32, "glScissor");
        pClearColor = (PFN_ClearColor)GetProcAddress(g_opengl32, "glClearColor");
        pClear      = (PFN_Clear)GetProcAddress(g_opengl32, "glClear");
        pColorMask  = (PFN_ColorMask)GetProcAddress(g_opengl32, "glColorMask");
        pGetFloatv  = (PFN_GetFloatv)GetProcAddress(g_opengl32, "glGetFloatv");
        pIsEnabled  = (PFN_IsEnabled)GetProcAddress(g_opengl32, "glIsEnabled");
        pGetString  = (PFN_GetString)GetProcAddress(g_opengl32, "glGetString");
        pWglGetCurrentContext = (PFN_wglGetCurrentContext)GetProcAddress(g_opengl32, "wglGetCurrentContext");
        // Extension entry points need a current context, which we have inside SwapBuffers.
        PFN_wglGetProcAddress pWglGPA = (PFN_wglGetProcAddress)GetProcAddress(g_opengl32, "wglGetProcAddress");
        if (pWglGPA) pBindFramebuffer = (PFN_BindFramebuffer)pWglGPA("glBindFramebuffer");
    }
    if (!pClear || !pScissor || !pGetIV) return;

    // Log GL state for the first few frames
    if (g_frameCount <= 3 || g_frameCount == 100)
    {
        int vp[4] = {}, sc[4] = {}, drawFbo = -1, readFbo = -1, program = -1, drawBuffer = -1;
        pGetIV(0x0BA2, vp);           // GL_VIEWPORT
        pGetIV(0x0C10, sc);           // GL_SCISSOR_BOX
        pGetIV(0x8CA6, &drawFbo);     // GL_DRAW_FRAMEBUFFER_BINDING
        pGetIV(0x8CAA, &readFbo);     // GL_READ_FRAMEBUFFER_BINDING
        pGetIV(0x8B8D, &program);     // GL_CURRENT_PROGRAM
        pGetIV(0x0C01, &drawBuffer);  // GL_DRAW_BUFFER
        RECT rc = {};
        HWND wnd = WindowFromDC((HDC)hdc);
        if (wnd) GetClientRect(wnd, &rc);
        FILE* f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a");
        if (f)
        {
            fprintf(f, "frame %d: tid=%lu hdc=%p hwnd=%p client=%ldx%ld ctx=%p\n", g_frameCount, GetCurrentThreadId(), hdc, wnd,
                    rc.right - rc.left, rc.bottom - rc.top, pWglGetCurrentContext ? pWglGetCurrentContext() : nullptr);
            fprintf(f, "  GL_VERSION=%s  RENDERER=%s\n", pGetString ? (const char*)pGetString(0x1F02) : "?", pGetString ? (const char*)pGetString(0x1F01) : "?");
            fprintf(f, "  viewport=%d,%d %dx%d  scissor=%d,%d %dx%d enabled=%d  drawFBO=%d readFBO=%d program=%d drawBuffer=0x%X\n",
                    vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3], pIsEnabled ? pIsEnabled(0x0C11) : -1,
                    drawFbo, readFbo, program, drawBuffer);
            fclose(f);
        }
    }

    // Save the state we touch
    int prevDrawFbo = 0, prevScissor[4] = {};
    float prevClear[4] = {};
    unsigned char prevScissorOn = pIsEnabled ? pIsEnabled(0x0C11) : 0;
    pGetIV(0x8CA6, &prevDrawFbo);
    pGetIV(0x0C10, prevScissor);
    if (pGetFloatv) pGetFloatv(0x0C22, prevClear);  // GL_COLOR_CLEAR_VALUE

    // Draw into the window's default framebuffer, top-left corner
    if (pBindFramebuffer) pBindFramebuffer(0x8CA9, 0);  // GL_DRAW_FRAMEBUFFER
    RECT rc = {};
    HWND wnd = WindowFromDC((HDC)hdc);
    if (wnd) GetClientRect(wnd, &rc);
    int winH = rc.bottom > 0 ? rc.bottom : 600;
    const int size = 300;

    pEnable(0x0C11);  // GL_SCISSOR_TEST
    pScissor(40, winH - 40 - size, size, size);
    if (pColorMask) pColorMask(1, 1, 1, 1);
    pClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    pClear(0x00004000);  // GL_COLOR_BUFFER_BIT

    // Restore
    pClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    pScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
    if (!prevScissorOn) pDisable(0x0C11);
    if (pBindFramebuffer) pBindFramebuffer(0x8CA9, (unsigned int)prevDrawFbo);
}

static int __stdcall Hooked_wglSwapBuffers(void* hdc)
{
    g_frameCount++;
    ResolveGLFuncs();

#if OVERLAY_DEBUG_RED_BLOCK
    DrawDebugRedBlock(hdc);
#else
    // This DLL has no capture link, so the panel says so.
    PanelStatus status = {};
    status.link = PANEL_OVERLAY_ONLY;
    status.frame = g_frameCount;
    PanelDraw(hdc, g_opengl32, (PFN_PanelGetProcAddress)GetProcAddress(g_opengl32, "wglGetProcAddress"), status);
#endif

    // Call original wglSwapBuffers
    int result;
    g_swapHook.CallOriginal_SwapBuffers(hdc, &result);
    return result;
}

// ============================================================================
// DLL entry point - install hooks
// ============================================================================

static DWORD WINAPI HookThread(LPVOID param)
{
    // Wait a moment for GE to fully initialize OpenGL
    Sleep(3000);

    FILE* f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a");
    if (f) { fprintf(f, "HookThread started, PID=%lu\n", GetCurrentProcessId()); fclose(f); }

    // Get the real opengl32.dll that's already loaded
    g_opengl32 = GetModuleHandleA("opengl32.dll");
    if (!g_opengl32)
    {
        if (f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a")) { fprintf(f, "opengl32.dll not loaded in process!\n"); fclose(f); }
        return 1;
    }

    // Get wglSwapBuffers address
    void* swapAddr = (void*)GetProcAddress(g_opengl32, "wglSwapBuffers");
    if (!swapAddr)
    {
        if (f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a")) { fprintf(f, "wglSwapBuffers not found!\n"); fclose(f); }
        return 1;
    }

    f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a");
    if (f) { fprintf(f, "opengl32=%p, wglSwapBuffers=%p\n", g_opengl32, swapAddr); fclose(f); }

    // Install trampoline hook
    if (!g_swapHook.Install(swapAddr, (void*)Hooked_wglSwapBuffers))
    {
        if (f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a")) { fprintf(f, "Hook install FAILED!\n"); fclose(f); }
        return 1;
    }

    f = fopen("C:\\Users\\Public\\meshtool_hook.log", "a");
    if (f) { fprintf(f, "wglSwapBuffers hooked successfully!\n"); fclose(f); }

    // Google Earth only renders on demand: repaint its windows so the overlay
    // shows up now instead of on the next camera move.
    EnumWindows([](HWND wnd, LPARAM) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(wnd, &pid);
        if (pid == GetCurrentProcessId())
            RedrawWindow(wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return TRUE;
    }, 0);

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);

        FILE* f = fopen("C:\\Users\\Public\\meshtool_hook.log", "w");
        if (f) { fprintf(f, "Injectable hook DLL loaded, PID=%lu\n", GetCurrentProcessId()); fclose(f); }

        // Start hook installation on a separate thread (can't do heavy work in DllMain)
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
