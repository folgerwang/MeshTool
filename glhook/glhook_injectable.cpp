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

// 5x7 bitmap font for text
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
};

static int CharIdx(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='Z') return c-'A'+10;
    if (c>='a'&&c<='z') return c-'a'+10;
    if (c==' ') return 36; if (c==':') return 37; if (c=='.') return 38; if (c=='-') return 39;
    return 36;
}

static void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    pColor4f(r, g, b, a);
    pVertex2f(x, y); pVertex2f(x+w, y); pVertex2f(x+w, y+h); pVertex2f(x, y+h);
}

static void DrawText(float x, float y, float scale, const char* text, float r, float g, float b)
{
    while (*text) {
        int idx = CharIdx(*text);
        if (idx >= 0 && idx < 40) {
            for (int row = 0; row < 7; row++) {
                unsigned char bits = g_font[idx][row];
                for (int col = 0; col < 5; col++) {
                    if (bits & (0x10 >> col)) {
                        float px = x + col * scale;
                        float py = y + row * scale;
                        pColor4f(r, g, b, 1.0f);
                        pVertex2f(px, py); pVertex2f(px+scale, py);
                        pVertex2f(px+scale, py+scale); pVertex2f(px, py+scale);
                    }
                }
            }
        }
        x += 6.0f * scale;
        text++;
    }
}

// ============================================================================
// Our hooked wglSwapBuffers
// ============================================================================

static int __stdcall Hooked_wglSwapBuffers(void* hdc)
{
    g_frameCount++;
    ResolveGLFuncs();

    // Draw overlay
    if (pPushAttrib && pBegin)
    {
        int vp[4]; pGetIV(0x0BA2, vp);
        float w = (float)vp[2], h = (float)vp[3];

        pPushAttrib(0x000FFFFF); // ALL_ATTRIB_BITS

        // Setup clean 2D state - disable EVERYTHING that could interfere
        pDisable(0x0B71);  // GL_DEPTH_TEST
        pDisable(0x0DE1);  // GL_TEXTURE_2D
        pDisable(0x0B50);  // GL_LIGHTING
        pDisable(0x0B44);  // GL_CULL_FACE
        pDisable(0x0C11);  // GL_SCISSOR_TEST
        pDisable(0x0B90);  // GL_STENCIL_TEST
        pDisable(0x0BC0);  // GL_ALPHA_TEST
        pDisable(0x0BE0);  // GL_FOG
        pDepthMask(0);
        pEnable(0x0BE2);   // GL_BLEND
        pBlendFunc(0x0302, 0x0303); // SRC_ALPHA, ONE_MINUS_SRC_ALPHA

        // Use bottom-left origin (standard OpenGL)
        pMatMode(0x1701); pPushMat(); pLoadId(); pOrtho(0, w, 0, h, -1, 1);
        pMatMode(0x1700); pPushMat(); pLoadId();

        // Panel position (top-left of viewport, LARGE so it's unmistakable)
        float px = 20.0f;
        float py = h - 20.0f;
        float pw = w * 0.35f;  // 35% of viewport width
        if (pw < 500) pw = 500;
        float ph = 180.0f;
        float sc = 5.0f;  // large font

        pBegin(0x0007); // GL_QUADS

        // Panel background
        DrawRect(px, py - ph, pw, ph, 0.0f, 0.0f, 0.0f, 0.82f);

        // Blue accent bar
        DrawRect(px, py - ph, 6, ph, 0.25f, 0.6f, 1.0f, 0.95f);

        // Title: MESHTOOL
        DrawText(px + 20, py - 50, sc, "MESHTOOL", 0.3f, 0.75f, 1.0f);

        // Connection status
        DrawRect(px + 20, py - 90, 18, 18, 0.2f, 0.95f, 0.3f, 1.0f); // green dot
        DrawText(px + 46, py - 90, sc*0.7f, "CONNECTED", 0.7f, 0.9f, 0.7f);

        // Frame counter
        char frameBuf[32];
        sprintf(frameBuf, "FRAME %d", g_frameCount);
        DrawText(px + 20, py - 130, sc*0.7f, frameBuf, 0.6f, 0.6f, 0.7f);

        // F12 hotkey
        DrawText(px + 20, py - 165, sc*0.65f, "F12: CAPTURE", 0.45f, 0.45f, 0.55f);

        // Animated activity dots
        int dots = (g_frameCount / 20) % 4;
        for (int i = 0; i < dots; i++)
            DrawRect(px + pw - 100 + i*20, py - 128, 12, 12, 0.3f, 0.7f, 1.0f, 0.9f);

        // F12 flash
        bool f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12) {
            DrawRect(0, 0, w, 6, 0.3f, 0.8f, 1.0f, 0.95f);
            DrawRect(0, h-6, w, 6, 0.3f, 0.8f, 1.0f, 0.95f);
            DrawRect(0, 0, 6, h, 0.3f, 0.8f, 1.0f, 0.95f);
            DrawRect(w-6, 0, 6, h, 0.3f, 0.8f, 1.0f, 0.95f);
        }

        pEnd();

        pMatMode(0x1701); pPopMat();
        pMatMode(0x1700); pPopMat();
        pPopAttrib();
    }

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
