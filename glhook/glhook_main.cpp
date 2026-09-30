// Proxy opengl32.dll - forwards all GL calls to real system opengl32.dll
// Hooks: wglSwapBuffers (overlay), wglGetProcAddress (extension interception), wglMakeCurrent

#define NOGDI
#include <windows.h>
#undef NOGDI
#include <cstdio>
#include <cstring>
#include <cstdarg>

#include "glhook_ipc_writer.h"
#include "glhook_state.h"
#include "glhook_overlay.h"

HMODULE g_real_opengl32 = nullptr;

// ============================================================================
// Real function pointers for hooked GL 1.1 functions
// ============================================================================
typedef void   (__stdcall *PFN_glBindTexture_t)(unsigned int, unsigned int);
typedef void   (__stdcall *PFN_glClear_t)(unsigned int);
typedef void   (__stdcall *PFN_glCullFace_t)(unsigned int);
typedef void   (__stdcall *PFN_glDepthFunc_t)(unsigned int);
typedef void   (__stdcall *PFN_glDisable_t)(unsigned int);
typedef void   (__stdcall *PFN_glDrawArrays_t)(unsigned int, int, int);
typedef void   (__stdcall *PFN_glDrawElements_t)(unsigned int, int, unsigned int, const void*);
typedef void   (__stdcall *PFN_glEnable_t)(unsigned int);
typedef void   (__stdcall *PFN_glFrontFace_t)(unsigned int);
typedef void   (__stdcall *PFN_glGenTextures_t)(int, unsigned int*);
typedef void   (__stdcall *PFN_glLoadMatrixf_t)(const float*);
typedef void   (__stdcall *PFN_glMatrixMode_t)(unsigned int);
typedef void   (__stdcall *PFN_glTexImage2D_t)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
typedef void   (__stdcall *PFN_glBlendFunc_t)(unsigned int, unsigned int);
typedef void   (__stdcall *PFN_glViewport_t)(int, int, int, int);
typedef void*  (__stdcall *PFN_wglGetProcAddress_t)(const char*);
typedef int    (__stdcall *PFN_wglMakeCurrent_t)(void*, void*);

PFN_glBindTexture_t      real_glBindTexture = nullptr;
PFN_glClear_t            real_glClear = nullptr;
PFN_glCullFace_t         real_glCullFace = nullptr;
PFN_glDepthFunc_t        real_glDepthFunc = nullptr;
PFN_glDisable_t          real_glDisable = nullptr;
PFN_glDrawArrays_t       real_glDrawArrays = nullptr;
PFN_glDrawElements_t     real_glDrawElements = nullptr;
PFN_glEnable_t           real_glEnable = nullptr;
PFN_glFrontFace_t        real_glFrontFace = nullptr;
PFN_glGenTextures_t      real_glGenTextures = nullptr;
PFN_glLoadMatrixf_t      real_glLoadMatrixf = nullptr;
PFN_glMatrixMode_t       real_glMatrixMode = nullptr;
PFN_glTexImage2D_t       real_glTexImage2D = nullptr;
PFN_glBlendFunc_t        real_glBlendFunc = nullptr;
PFN_glViewport_t         real_glViewport = nullptr;
PFN_wglGetProcAddress_t  real_wglGetProcAddress = nullptr;
PFN_wglMakeCurrent_t     real_wglMakeCurrent = nullptr;

// Extension function pointers
typedef void (__stdcall *PFN_glBufferData)(unsigned int, ptrdiff_t, const void*, unsigned int);
typedef void (__stdcall *PFN_glBufferSubData)(unsigned int, ptrdiff_t, ptrdiff_t, const void*);
typedef void (__stdcall *PFN_glGenBuffers)(int, unsigned int*);
typedef void (__stdcall *PFN_glDeleteBuffers)(int, const unsigned int*);
typedef void (__stdcall *PFN_glBindBuffer)(unsigned int, unsigned int);
typedef void (__stdcall *PFN_glVertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void*);
typedef void (__stdcall *PFN_glVertexAttribIPointer)(unsigned int, int, unsigned int, int, const void*);
typedef void (__stdcall *PFN_glEnableVertexAttribArray)(unsigned int);
typedef void (__stdcall *PFN_glDisableVertexAttribArray)(unsigned int);
typedef void (__stdcall *PFN_glActiveTexture)(unsigned int);
typedef void (__stdcall *PFN_glCompressedTexImage2D)(unsigned int, int, unsigned int, int, int, int, int, const void*);
typedef void (__stdcall *PFN_glUniformMatrix4fv)(int, int, unsigned char, const float*);
typedef void (__stdcall *PFN_glShaderSource)(unsigned int, int, const char**, const int*);
typedef void (__stdcall *PFN_glUseProgram)(unsigned int);

PFN_glBufferData              real_glBufferData = nullptr;
PFN_glBufferSubData           real_glBufferSubData = nullptr;
PFN_glGenBuffers              real_glGenBuffers = nullptr;
PFN_glDeleteBuffers           real_glDeleteBuffers = nullptr;
PFN_glBindBuffer              real_glBindBuffer = nullptr;
PFN_glVertexAttribPointer     real_glVertexAttribPointer = nullptr;
PFN_glVertexAttribIPointer    real_glVertexAttribIPointer = nullptr;
PFN_glEnableVertexAttribArray real_glEnableVertexAttribArray = nullptr;
PFN_glDisableVertexAttribArray real_glDisableVertexAttribArray = nullptr;
PFN_glActiveTexture           real_glActiveTexture = nullptr;
PFN_glCompressedTexImage2D    real_glCompressedTexImage2D = nullptr;
PFN_glUniformMatrix4fv        real_glUniformMatrix4fv = nullptr;
PFN_glShaderSource            real_glShaderSource = nullptr;
PFN_glUseProgram              real_glUseProgram = nullptr;

// ============================================================================
void ProxyLog(const char* fmt, ...)
{
    FILE* f = fopen("C:\\Users\\Public\\meshtool_proxy.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

static bool LoadRealOpenGL()
{
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\opengl32.dll");
    g_real_opengl32 = LoadLibraryA(path);

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadRealOpenGL), &self);
    ProxyLog("LoadRealOpenGL: %s -> %p (proxy module %p)%s\n", path, (void*)g_real_opengl32, (void*)self,
             g_real_opengl32 == self ? "  ERROR: loaded ourselves" : "");
    if (g_real_opengl32 == self)
        g_real_opengl32 = nullptr;
    if (!g_real_opengl32) return false;

    real_glBindTexture    = (PFN_glBindTexture_t)GetProcAddress(g_real_opengl32, "glBindTexture");
    real_glClear          = (PFN_glClear_t)GetProcAddress(g_real_opengl32, "glClear");
    real_glCullFace       = (PFN_glCullFace_t)GetProcAddress(g_real_opengl32, "glCullFace");
    real_glDepthFunc      = (PFN_glDepthFunc_t)GetProcAddress(g_real_opengl32, "glDepthFunc");
    real_glDisable        = (PFN_glDisable_t)GetProcAddress(g_real_opengl32, "glDisable");
    real_glDrawArrays     = (PFN_glDrawArrays_t)GetProcAddress(g_real_opengl32, "glDrawArrays");
    real_glDrawElements   = (PFN_glDrawElements_t)GetProcAddress(g_real_opengl32, "glDrawElements");
    real_glEnable         = (PFN_glEnable_t)GetProcAddress(g_real_opengl32, "glEnable");
    real_glFrontFace      = (PFN_glFrontFace_t)GetProcAddress(g_real_opengl32, "glFrontFace");
    real_glGenTextures    = (PFN_glGenTextures_t)GetProcAddress(g_real_opengl32, "glGenTextures");
    real_glLoadMatrixf    = (PFN_glLoadMatrixf_t)GetProcAddress(g_real_opengl32, "glLoadMatrixf");
    real_glMatrixMode     = (PFN_glMatrixMode_t)GetProcAddress(g_real_opengl32, "glMatrixMode");
    real_glTexImage2D     = (PFN_glTexImage2D_t)GetProcAddress(g_real_opengl32, "glTexImage2D");
    real_glBlendFunc      = (PFN_glBlendFunc_t)GetProcAddress(g_real_opengl32, "glBlendFunc");
    real_glViewport       = (PFN_glViewport_t)GetProcAddress(g_real_opengl32, "glViewport");
    real_wglGetProcAddress = (PFN_wglGetProcAddress_t)GetProcAddress(g_real_opengl32, "wglGetProcAddress");
    real_wglMakeCurrent   = (PFN_wglMakeCurrent_t)GetProcAddress(g_real_opengl32, "wglMakeCurrent");
    return true;
}

void ResolveExtensions()
{
    if (!real_wglGetProcAddress) return;
    real_glBufferData = (PFN_glBufferData)real_wglGetProcAddress("glBufferData");
    real_glBufferSubData = (PFN_glBufferSubData)real_wglGetProcAddress("glBufferSubData");
    real_glGenBuffers = (PFN_glGenBuffers)real_wglGetProcAddress("glGenBuffers");
    real_glDeleteBuffers = (PFN_glDeleteBuffers)real_wglGetProcAddress("glDeleteBuffers");
    real_glBindBuffer = (PFN_glBindBuffer)real_wglGetProcAddress("glBindBuffer");
    real_glVertexAttribPointer = (PFN_glVertexAttribPointer)real_wglGetProcAddress("glVertexAttribPointer");
    real_glVertexAttribIPointer = (PFN_glVertexAttribIPointer)real_wglGetProcAddress("glVertexAttribIPointer");
    real_glEnableVertexAttribArray = (PFN_glEnableVertexAttribArray)real_wglGetProcAddress("glEnableVertexAttribArray");
    real_glDisableVertexAttribArray = (PFN_glDisableVertexAttribArray)real_wglGetProcAddress("glDisableVertexAttribArray");
    real_glActiveTexture = (PFN_glActiveTexture)real_wglGetProcAddress("glActiveTexture");
    real_glCompressedTexImage2D = (PFN_glCompressedTexImage2D)real_wglGetProcAddress("glCompressedTexImage2D");
    real_glUniformMatrix4fv = (PFN_glUniformMatrix4fv)real_wglGetProcAddress("glUniformMatrix4fv");
    real_glShaderSource = (PFN_glShaderSource)real_wglGetProcAddress("glShaderSource");
    real_glUseProgram = (PFN_glUseProgram)real_wglGetProcAddress("glUseProgram");
}

// ============================================================================
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        // Real DLL is loaded lazily on first GL call.
        HookStateInit();
        ProxyLog("proxy opengl32.dll attached, PID=%lu\n", GetCurrentProcessId());
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_ipc_writer.Shutdown();
        if (g_real_opengl32) { FreeLibrary(g_real_opengl32); g_real_opengl32 = nullptr; }
    }
    return TRUE;
}

// ============================================================================
// Pure forwarding for all non-hooked functions
// ============================================================================
// Called by every export (forwarded or intercepted) before touching real_* pointers.
void EnsureRealLoaded()
{
    if (g_real_opengl32) return;
    LoadRealOpenGL();
    g_ipc_writer.Init();
}

static void* _c[400] = {};
static void* R(int i, const char* n) { EnsureRealLoaded(); if (!_c[i]) _c[i]=(void*)GetProcAddress(g_real_opengl32,n); return _c[i]; }

extern "C" {

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef double GLclampd;
typedef float GLclampf;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef unsigned char GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort;
typedef void GLvoid;

#define X __declspec(dllexport)

// All non-hooked GL functions: pure forward via function pointer
// Glmf* stubs (ordinals 1-6)
X void __stdcall GlmfBeginGlsBlock(GLint a,GLint b){typedef void(__stdcall*F)(GLint,GLint);((F)R(370,"GlmfBeginGlsBlock"))(a,b);}
X void __stdcall GlmfCloseMetaFile(GLint a){typedef void(__stdcall*F)(GLint);((F)R(371,"GlmfCloseMetaFile"))(a);}
X void __stdcall GlmfEndGlsBlock(GLint a,GLint b){typedef void(__stdcall*F)(GLint,GLint);((F)R(372,"GlmfEndGlsBlock"))(a,b);}
X void __stdcall GlmfEndPlayback(GLint a){typedef void(__stdcall*F)(GLint);((F)R(373,"GlmfEndPlayback"))(a);}
X GLint __stdcall GlmfInitPlayback(void*a,void*b,void*c){typedef GLint(__stdcall*F)(void*,void*,void*);return((F)R(374,"GlmfInitPlayback"))(a,b,c);}
X GLint __stdcall GlmfPlayGlsRecord(GLint a,GLint b,void*c,void*d){typedef GLint(__stdcall*F)(GLint,GLint,void*,void*);return((F)R(375,"GlmfPlayGlsRecord"))(a,b,c,d);}

X void __stdcall glAccum(GLenum a,GLfloat b){typedef void(__stdcall*F)(GLenum,GLfloat);((F)R(0,"glAccum"))(a,b);}
X void __stdcall glAlphaFunc(GLenum a,GLclampf b){typedef void(__stdcall*F)(GLenum,GLclampf);((F)R(1,"glAlphaFunc"))(a,b);}
X GLboolean __stdcall glAreTexturesResident(GLsizei a,const GLuint*b,GLboolean*c){typedef GLboolean(__stdcall*F)(GLsizei,const GLuint*,GLboolean*);return((F)R(2,"glAreTexturesResident"))(a,b,c);}
X void __stdcall glArrayElement(GLint a){typedef void(__stdcall*F)(GLint);((F)R(3,"glArrayElement"))(a);}
X void __stdcall glBegin(GLenum a){typedef void(__stdcall*F)(GLenum);((F)R(4,"glBegin"))(a);}
// glBindTexture - HOOKED
X void __stdcall glBitmap(GLsizei a,GLsizei b,GLfloat c,GLfloat d,GLfloat e,GLfloat f,const GLubyte*g){typedef void(__stdcall*F)(GLsizei,GLsizei,GLfloat,GLfloat,GLfloat,GLfloat,const GLubyte*);((F)R(6,"glBitmap"))(a,b,c,d,e,f,g);}
// glBlendFunc - HOOKED
X void __stdcall glCallList(GLuint a){typedef void(__stdcall*F)(GLuint);((F)R(8,"glCallList"))(a);}
X void __stdcall glCallLists(GLsizei a,GLenum b,const GLvoid*c){typedef void(__stdcall*F)(GLsizei,GLenum,const GLvoid*);((F)R(9,"glCallLists"))(a,b,c);}
// glClear - HOOKED
X void __stdcall glClearAccum(GLfloat a,GLfloat b,GLfloat c,GLfloat d){typedef void(__stdcall*F)(GLfloat,GLfloat,GLfloat,GLfloat);((F)R(11,"glClearAccum"))(a,b,c,d);}
X void __stdcall glClearColor(GLclampf a,GLclampf b,GLclampf c,GLclampf d){typedef void(__stdcall*F)(GLclampf,GLclampf,GLclampf,GLclampf);((F)R(12,"glClearColor"))(a,b,c,d);}
X void __stdcall glClearDepth(GLclampd a){typedef void(__stdcall*F)(GLclampd);((F)R(13,"glClearDepth"))(a);}
X void __stdcall glClearIndex(GLfloat a){typedef void(__stdcall*F)(GLfloat);((F)R(14,"glClearIndex"))(a);}
X void __stdcall glClearStencil(GLint a){typedef void(__stdcall*F)(GLint);((F)R(15,"glClearStencil"))(a);}
X void __stdcall glClipPlane(GLenum a,const GLdouble*b){typedef void(__stdcall*F)(GLenum,const GLdouble*);((F)R(16,"glClipPlane"))(a,b);}

// For brevity, remaining GL functions use a generic passthrough.
// We forward ALL remaining calls through GetProcAddress on first use.

#define G1V(name,T1,i) X void __stdcall name(T1 a){typedef void(__stdcall*F)(T1);((F)R(i,#name))(a);}
#define G2V(name,T1,T2,i) X void __stdcall name(T1 a,T2 b){typedef void(__stdcall*F)(T1,T2);((F)R(i,#name))(a,b);}
#define G3V(name,T1,T2,T3,i) X void __stdcall name(T1 a,T2 b,T3 c){typedef void(__stdcall*F)(T1,T2,T3);((F)R(i,#name))(a,b,c);}
#define G4V(name,T1,T2,T3,T4,i) X void __stdcall name(T1 a,T2 b,T3 c,T4 d){typedef void(__stdcall*F)(T1,T2,T3,T4);((F)R(i,#name))(a,b,c,d);}
#define G0V(name,i) X void __stdcall name(){typedef void(__stdcall*F)();((F)R(i,#name))();}
#define G0R(name,RT,i) X RT __stdcall name(){typedef RT(__stdcall*F)();return((F)R(i,#name))();}
#define G1R(name,RT,T1,i) X RT __stdcall name(T1 a){typedef RT(__stdcall*F)(T1);return((F)R(i,#name))(a);}

G3V(glColor3b,GLbyte,GLbyte,GLbyte,17) G1V(glColor3bv,const GLbyte*,18)
G3V(glColor3d,GLdouble,GLdouble,GLdouble,19) G1V(glColor3dv,const GLdouble*,20)
G3V(glColor3f,GLfloat,GLfloat,GLfloat,21) G1V(glColor3fv,const GLfloat*,22)
G3V(glColor3i,GLint,GLint,GLint,23) G1V(glColor3iv,const GLint*,24)
G3V(glColor3s,GLshort,GLshort,GLshort,25) G1V(glColor3sv,const GLshort*,26)
G3V(glColor3ub,GLubyte,GLubyte,GLubyte,27) G1V(glColor3ubv,const GLubyte*,28)
G3V(glColor3ui,GLuint,GLuint,GLuint,29) G1V(glColor3uiv,const GLuint*,30)
G3V(glColor3us,GLushort,GLushort,GLushort,31) G1V(glColor3usv,const GLushort*,32)
G4V(glColor4b,GLbyte,GLbyte,GLbyte,GLbyte,33) G1V(glColor4bv,const GLbyte*,34)
G4V(glColor4d,GLdouble,GLdouble,GLdouble,GLdouble,35) G1V(glColor4dv,const GLdouble*,36)
G4V(glColor4f,GLfloat,GLfloat,GLfloat,GLfloat,37) G1V(glColor4fv,const GLfloat*,38)
G4V(glColor4i,GLint,GLint,GLint,GLint,39) G1V(glColor4iv,const GLint*,40)
G4V(glColor4s,GLshort,GLshort,GLshort,GLshort,41) G1V(glColor4sv,const GLshort*,42)
G4V(glColor4ub,GLubyte,GLubyte,GLubyte,GLubyte,43) G1V(glColor4ubv,const GLubyte*,44)
G4V(glColor4ui,GLuint,GLuint,GLuint,GLuint,45) G1V(glColor4uiv,const GLuint*,46)
G4V(glColor4us,GLushort,GLushort,GLushort,GLushort,47) G1V(glColor4usv,const GLushort*,48)
G4V(glColorMask,GLboolean,GLboolean,GLboolean,GLboolean,49) G2V(glColorMaterial,GLenum,GLenum,50)
G4V(glColorPointer,GLint,GLenum,GLsizei,const GLvoid*,51)
X void __stdcall glCopyPixels(GLint a,GLint b,GLsizei c,GLsizei d,GLenum e){typedef void(__stdcall*F)(GLint,GLint,GLsizei,GLsizei,GLenum);((F)R(52,"glCopyPixels"))(a,b,c,d,e);}
X void __stdcall glCopyTexImage1D(GLenum a,GLint b,GLenum c,GLint d,GLint e,GLsizei f,GLint g){typedef void(__stdcall*F)(GLenum,GLint,GLenum,GLint,GLint,GLsizei,GLint);((F)R(53,"glCopyTexImage1D"))(a,b,c,d,e,f,g);}
X void __stdcall glCopyTexImage2D(GLenum a,GLint b,GLenum c,GLint d,GLint e,GLsizei f,GLsizei g,GLint h){typedef void(__stdcall*F)(GLenum,GLint,GLenum,GLint,GLint,GLsizei,GLsizei,GLint);((F)R(54,"glCopyTexImage2D"))(a,b,c,d,e,f,g,h);}
X void __stdcall glCopyTexSubImage1D(GLenum a,GLint b,GLint c,GLint d,GLint e,GLsizei f){typedef void(__stdcall*F)(GLenum,GLint,GLint,GLint,GLint,GLsizei);((F)R(55,"glCopyTexSubImage1D"))(a,b,c,d,e,f);}
X void __stdcall glCopyTexSubImage2D(GLenum a,GLint b,GLint c,GLint d,GLint e,GLint f,GLsizei g,GLsizei h){typedef void(__stdcall*F)(GLenum,GLint,GLint,GLint,GLint,GLint,GLsizei,GLsizei);((F)R(56,"glCopyTexSubImage2D"))(a,b,c,d,e,f,g,h);}
// glCullFace - HOOKED
X void __stdcall glDebugEntry(GLenum a,GLenum b){typedef void(__stdcall*F)(GLenum,GLenum);((F)R(376,"glDebugEntry"))(a,b);}
G2V(glDeleteLists,GLuint,GLsizei,58) G2V(glDeleteTextures,GLsizei,const GLuint*,59)
// glDepthFunc - HOOKED
G1V(glDepthMask,GLboolean,61) G2V(glDepthRange,GLclampd,GLclampd,62)
// glDisable - HOOKED
G1V(glDisableClientState,GLenum,64)
// glDrawArrays - HOOKED
G1V(glDrawBuffer,GLenum,66)
// glDrawElements - HOOKED
X void __stdcall glDrawPixels(GLsizei a,GLsizei b,GLenum c,GLenum d,const GLvoid*e){typedef void(__stdcall*F)(GLsizei,GLsizei,GLenum,GLenum,const GLvoid*);((F)R(68,"glDrawPixels"))(a,b,c,d,e);}
G1V(glEdgeFlag,GLboolean,69) G2V(glEdgeFlagPointer,GLsizei,const GLvoid*,70) G1V(glEdgeFlagv,const GLboolean*,71)
// glEnable - HOOKED
G1V(glEnableClientState,GLenum,73) G0V(glEnd,74) G0V(glEndList,75)
G1V(glEvalCoord1d,GLdouble,76) G1V(glEvalCoord1dv,const GLdouble*,77) G1V(glEvalCoord1f,GLfloat,78) G1V(glEvalCoord1fv,const GLfloat*,79)
G2V(glEvalCoord2d,GLdouble,GLdouble,80) G1V(glEvalCoord2dv,const GLdouble*,81) G2V(glEvalCoord2f,GLfloat,GLfloat,82) G1V(glEvalCoord2fv,const GLfloat*,83)
G3V(glEvalMesh1,GLenum,GLint,GLint,84)
X void __stdcall glEvalMesh2(GLenum a,GLint b,GLint c,GLint d,GLint e){typedef void(__stdcall*F)(GLenum,GLint,GLint,GLint,GLint);((F)R(85,"glEvalMesh2"))(a,b,c,d,e);}
G1V(glEvalPoint1,GLint,86) G2V(glEvalPoint2,GLint,GLint,87)
G3V(glFeedbackBuffer,GLsizei,GLenum,GLfloat*,88) G0V(glFinish,89) G0V(glFlush,90)
G2V(glFogf,GLenum,GLfloat,91) G2V(glFogfv,GLenum,const GLfloat*,92) G2V(glFogi,GLenum,GLint,93) G2V(glFogiv,GLenum,const GLint*,94)
// glFrontFace - HOOKED
X void __stdcall glFrustum(GLdouble a,GLdouble b,GLdouble c,GLdouble d,GLdouble e,GLdouble f){typedef void(__stdcall*F)(GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble);((F)R(96,"glFrustum"))(a,b,c,d,e,f);}
G1R(glGenLists,GLuint,GLsizei,97)
// glGenTextures - HOOKED
G2V(glGetBooleanv,GLenum,GLboolean*,99) G2V(glGetClipPlane,GLenum,GLdouble*,100) G2V(glGetDoublev,GLenum,GLdouble*,101)
G0R(glGetError,GLenum,102) G2V(glGetFloatv,GLenum,GLfloat*,103) G2V(glGetIntegerv,GLenum,GLint*,104)
G3V(glGetLightfv,GLenum,GLenum,GLfloat*,105) G3V(glGetLightiv,GLenum,GLenum,GLint*,106)
G3V(glGetMapdv,GLenum,GLenum,GLdouble*,107) G3V(glGetMapfv,GLenum,GLenum,GLfloat*,108) G3V(glGetMapiv,GLenum,GLenum,GLint*,109)
G3V(glGetMaterialfv,GLenum,GLenum,GLfloat*,110) G3V(glGetMaterialiv,GLenum,GLenum,GLint*,111)
G2V(glGetPixelMapfv,GLenum,GLfloat*,112) G2V(glGetPixelMapuiv,GLenum,GLuint*,113) G2V(glGetPixelMapusv,GLenum,GLushort*,114)
G2V(glGetPointerv,GLenum,GLvoid**,115) G1V(glGetPolygonStipple,GLubyte*,116)
G1R(glGetString,const GLubyte*,GLenum,117)
G3V(glGetTexEnvfv,GLenum,GLenum,GLfloat*,118) G3V(glGetTexEnviv,GLenum,GLenum,GLint*,119)
G3V(glGetTexGendv,GLenum,GLenum,GLdouble*,120) G3V(glGetTexGenfv,GLenum,GLenum,GLfloat*,121) G3V(glGetTexGeniv,GLenum,GLenum,GLint*,122)
X void __stdcall glGetTexImage(GLenum a,GLint b,GLenum c,GLenum d,GLvoid*e){typedef void(__stdcall*F)(GLenum,GLint,GLenum,GLenum,GLvoid*);((F)R(123,"glGetTexImage"))(a,b,c,d,e);}
G4V(glGetTexLevelParameterfv,GLenum,GLint,GLenum,GLfloat*,124) G4V(glGetTexLevelParameteriv,GLenum,GLint,GLenum,GLint*,125)
G3V(glGetTexParameterfv,GLenum,GLenum,GLfloat*,126) G3V(glGetTexParameteriv,GLenum,GLenum,GLint*,127)
G2V(glHint,GLenum,GLenum,128) G1V(glIndexMask,GLuint,129) G3V(glIndexPointer,GLenum,GLsizei,const GLvoid*,130)
G1V(glIndexd,GLdouble,131) G1V(glIndexdv,const GLdouble*,132) G1V(glIndexf,GLfloat,133) G1V(glIndexfv,const GLfloat*,134)
G1V(glIndexi,GLint,135) G1V(glIndexiv,const GLint*,136) G1V(glIndexs,GLshort,137) G1V(glIndexsv,const GLshort*,138)
G1V(glIndexub,GLubyte,139) G1V(glIndexubv,const GLubyte*,140) G0V(glInitNames,141) G3V(glInterleavedArrays,GLenum,GLsizei,const GLvoid*,142)
G1R(glIsEnabled,GLboolean,GLenum,143) G1R(glIsList,GLboolean,GLuint,144) G1R(glIsTexture,GLboolean,GLuint,145)
G2V(glLightModelf,GLenum,GLfloat,146) G2V(glLightModelfv,GLenum,const GLfloat*,147) G2V(glLightModeli,GLenum,GLint,148) G2V(glLightModeliv,GLenum,const GLint*,149)
G3V(glLightf,GLenum,GLenum,GLfloat,150) G3V(glLightfv,GLenum,GLenum,const GLfloat*,151) G3V(glLighti,GLenum,GLenum,GLint,152) G3V(glLightiv,GLenum,GLenum,const GLint*,153)
G2V(glLineStipple,GLint,GLushort,154) G1V(glLineWidth,GLfloat,155) G1V(glListBase,GLuint,156) G0V(glLoadIdentity,157)
G1V(glLoadMatrixd,const GLdouble*,158)
// glLoadMatrixf - HOOKED
G1V(glLoadName,GLuint,160) G1V(glLogicOp,GLenum,161)
X void __stdcall glMap1d(GLenum a,GLdouble b,GLdouble c,GLint d,GLint e,const GLdouble*f){typedef void(__stdcall*F)(GLenum,GLdouble,GLdouble,GLint,GLint,const GLdouble*);((F)R(162,"glMap1d"))(a,b,c,d,e,f);}
X void __stdcall glMap1f(GLenum a,GLfloat b,GLfloat c,GLint d,GLint e,const GLfloat*f){typedef void(__stdcall*F)(GLenum,GLfloat,GLfloat,GLint,GLint,const GLfloat*);((F)R(163,"glMap1f"))(a,b,c,d,e,f);}
X void __stdcall glMap2d(GLenum t,GLdouble u1,GLdouble u2,GLint us,GLint uo,GLdouble v1,GLdouble v2,GLint vs,GLint vo,const GLdouble*p){typedef void(__stdcall*F)(GLenum,GLdouble,GLdouble,GLint,GLint,GLdouble,GLdouble,GLint,GLint,const GLdouble*);((F)R(164,"glMap2d"))(t,u1,u2,us,uo,v1,v2,vs,vo,p);}
X void __stdcall glMap2f(GLenum t,GLfloat u1,GLfloat u2,GLint us,GLint uo,GLfloat v1,GLfloat v2,GLint vs,GLint vo,const GLfloat*p){typedef void(__stdcall*F)(GLenum,GLfloat,GLfloat,GLint,GLint,GLfloat,GLfloat,GLint,GLint,const GLfloat*);((F)R(165,"glMap2f"))(t,u1,u2,us,uo,v1,v2,vs,vo,p);}
G3V(glMapGrid1d,GLint,GLdouble,GLdouble,166) G3V(glMapGrid1f,GLint,GLfloat,GLfloat,167)
X void __stdcall glMapGrid2d(GLint a,GLdouble b,GLdouble c,GLint d,GLdouble e,GLdouble f){typedef void(__stdcall*F)(GLint,GLdouble,GLdouble,GLint,GLdouble,GLdouble);((F)R(168,"glMapGrid2d"))(a,b,c,d,e,f);}
X void __stdcall glMapGrid2f(GLint a,GLfloat b,GLfloat c,GLint d,GLfloat e,GLfloat f){typedef void(__stdcall*F)(GLint,GLfloat,GLfloat,GLint,GLfloat,GLfloat);((F)R(169,"glMapGrid2f"))(a,b,c,d,e,f);}
G3V(glMaterialf,GLenum,GLenum,GLfloat,170) G3V(glMaterialfv,GLenum,GLenum,const GLfloat*,171) G3V(glMateriali,GLenum,GLenum,GLint,172) G3V(glMaterialiv,GLenum,GLenum,const GLint*,173)
// glMatrixMode - HOOKED
G1V(glMultMatrixd,const GLdouble*,175) G1V(glMultMatrixf,const GLfloat*,176) G2V(glNewList,GLuint,GLenum,177)
G3V(glNormal3b,GLbyte,GLbyte,GLbyte,178) G1V(glNormal3bv,const GLbyte*,179) G3V(glNormal3d,GLdouble,GLdouble,GLdouble,180) G1V(glNormal3dv,const GLdouble*,181)
G3V(glNormal3f,GLfloat,GLfloat,GLfloat,182) G1V(glNormal3fv,const GLfloat*,183) G3V(glNormal3i,GLint,GLint,GLint,184) G1V(glNormal3iv,const GLint*,185)
G3V(glNormal3s,GLshort,GLshort,GLshort,186) G1V(glNormal3sv,const GLshort*,187)
G3V(glNormalPointer,GLenum,GLsizei,const GLvoid*,377)
X void __stdcall glOrtho(GLdouble a,GLdouble b,GLdouble c,GLdouble d,GLdouble e,GLdouble f){typedef void(__stdcall*F)(GLdouble,GLdouble,GLdouble,GLdouble,GLdouble,GLdouble);((F)R(188,"glOrtho"))(a,b,c,d,e,f);}
G1V(glPassThrough,GLfloat,189) G3V(glPixelMapfv,GLenum,GLsizei,const GLfloat*,190) G3V(glPixelMapuiv,GLenum,GLsizei,const GLuint*,191) G3V(glPixelMapusv,GLenum,GLsizei,const GLushort*,192)
G2V(glPixelStoref,GLenum,GLfloat,193) G2V(glPixelStorei,GLenum,GLint,194) G2V(glPixelTransferf,GLenum,GLfloat,195) G2V(glPixelTransferi,GLenum,GLint,196)
G2V(glPixelZoom,GLfloat,GLfloat,197) G1V(glPointSize,GLfloat,198) G2V(glPolygonMode,GLenum,GLenum,199) G2V(glPolygonOffset,GLfloat,GLfloat,200)
G1V(glPolygonStipple,const GLubyte*,201) G0V(glPopAttrib,202) G0V(glPopClientAttrib,203) G0V(glPopMatrix,204) G0V(glPopName,205)
G3V(glPrioritizeTextures,GLsizei,const GLuint*,const GLclampf*,206) G1V(glPushAttrib,GLbitfield,207) G1V(glPushClientAttrib,GLbitfield,208)
G0V(glPushMatrix,209) G1V(glPushName,GLuint,210)
G2V(glRasterPos2d,GLdouble,GLdouble,211) G1V(glRasterPos2dv,const GLdouble*,212) G2V(glRasterPos2f,GLfloat,GLfloat,213) G1V(glRasterPos2fv,const GLfloat*,214)
G2V(glRasterPos2i,GLint,GLint,215) G1V(glRasterPos2iv,const GLint*,216) G2V(glRasterPos2s,GLshort,GLshort,217) G1V(glRasterPos2sv,const GLshort*,218)
G3V(glRasterPos3d,GLdouble,GLdouble,GLdouble,219) G1V(glRasterPos3dv,const GLdouble*,220) G3V(glRasterPos3f,GLfloat,GLfloat,GLfloat,221) G1V(glRasterPos3fv,const GLfloat*,222)
G3V(glRasterPos3i,GLint,GLint,GLint,223) G1V(glRasterPos3iv,const GLint*,224) G3V(glRasterPos3s,GLshort,GLshort,GLshort,225) G1V(glRasterPos3sv,const GLshort*,226)
G4V(glRasterPos4d,GLdouble,GLdouble,GLdouble,GLdouble,227) G1V(glRasterPos4dv,const GLdouble*,228) G4V(glRasterPos4f,GLfloat,GLfloat,GLfloat,GLfloat,229) G1V(glRasterPos4fv,const GLfloat*,230)
G4V(glRasterPos4i,GLint,GLint,GLint,GLint,231) G1V(glRasterPos4iv,const GLint*,232) G4V(glRasterPos4s,GLshort,GLshort,GLshort,GLshort,233) G1V(glRasterPos4sv,const GLshort*,234)
G1V(glReadBuffer,GLenum,235)
X void __stdcall glReadPixels(GLint a,GLint b,GLsizei c,GLsizei d,GLenum e,GLenum f,GLvoid*g){typedef void(__stdcall*F)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,GLvoid*);((F)R(236,"glReadPixels"))(a,b,c,d,e,f,g);}
G4V(glRectd,GLdouble,GLdouble,GLdouble,GLdouble,237) G2V(glRectdv,const GLdouble*,const GLdouble*,238)
G4V(glRectf,GLfloat,GLfloat,GLfloat,GLfloat,239) G2V(glRectfv,const GLfloat*,const GLfloat*,240)
G4V(glRecti,GLint,GLint,GLint,GLint,241) G2V(glRectiv,const GLint*,const GLint*,242)
G4V(glRects,GLshort,GLshort,GLshort,GLshort,243) G2V(glRectsv,const GLshort*,const GLshort*,244)
G1R(glRenderMode,GLint,GLenum,245)
G4V(glRotated,GLdouble,GLdouble,GLdouble,GLdouble,246) G4V(glRotatef,GLfloat,GLfloat,GLfloat,GLfloat,247)
G3V(glScaled,GLdouble,GLdouble,GLdouble,248) G3V(glScalef,GLfloat,GLfloat,GLfloat,249)
G4V(glScissor,GLint,GLint,GLsizei,GLsizei,250) G2V(glSelectBuffer,GLsizei,GLuint*,251)
G1V(glShadeModel,GLenum,252) G3V(glStencilFunc,GLenum,GLint,GLuint,253) G1V(glStencilMask,GLuint,254) G3V(glStencilOp,GLenum,GLenum,GLenum,255)
G1V(glTexCoord1d,GLdouble,256) G1V(glTexCoord1dv,const GLdouble*,257) G1V(glTexCoord1f,GLfloat,258) G1V(glTexCoord1fv,const GLfloat*,259)
G1V(glTexCoord1i,GLint,260) G1V(glTexCoord1iv,const GLint*,261) G1V(glTexCoord1s,GLshort,262) G1V(glTexCoord1sv,const GLshort*,263)
G2V(glTexCoord2d,GLdouble,GLdouble,264) G1V(glTexCoord2dv,const GLdouble*,265) G2V(glTexCoord2f,GLfloat,GLfloat,266) G1V(glTexCoord2fv,const GLfloat*,267)
G2V(glTexCoord2i,GLint,GLint,268) G1V(glTexCoord2iv,const GLint*,269) G2V(glTexCoord2s,GLshort,GLshort,270) G1V(glTexCoord2sv,const GLshort*,271)
G3V(glTexCoord3d,GLdouble,GLdouble,GLdouble,272) G1V(glTexCoord3dv,const GLdouble*,273) G3V(glTexCoord3f,GLfloat,GLfloat,GLfloat,274) G1V(glTexCoord3fv,const GLfloat*,275)
G3V(glTexCoord3i,GLint,GLint,GLint,276) G1V(glTexCoord3iv,const GLint*,277) G3V(glTexCoord3s,GLshort,GLshort,GLshort,278) G1V(glTexCoord3sv,const GLshort*,279)
G4V(glTexCoord4d,GLdouble,GLdouble,GLdouble,GLdouble,280) G1V(glTexCoord4dv,const GLdouble*,281) G4V(glTexCoord4f,GLfloat,GLfloat,GLfloat,GLfloat,282) G1V(glTexCoord4fv,const GLfloat*,283)
G4V(glTexCoord4i,GLint,GLint,GLint,GLint,284) G1V(glTexCoord4iv,const GLint*,285) G4V(glTexCoord4s,GLshort,GLshort,GLshort,GLshort,286) G1V(glTexCoord4sv,const GLshort*,287)
G4V(glTexCoordPointer,GLint,GLenum,GLsizei,const GLvoid*,288)
G3V(glTexEnvf,GLenum,GLenum,GLfloat,289) G3V(glTexEnvfv,GLenum,GLenum,const GLfloat*,290) G3V(glTexEnvi,GLenum,GLenum,GLint,291) G3V(glTexEnviv,GLenum,GLenum,const GLint*,292)
G3V(glTexGend,GLenum,GLenum,GLdouble,293) G3V(glTexGendv,GLenum,GLenum,const GLdouble*,294) G3V(glTexGenf,GLenum,GLenum,GLfloat,295) G3V(glTexGenfv,GLenum,GLenum,const GLfloat*,296)
G3V(glTexGeni,GLenum,GLenum,GLint,297) G3V(glTexGeniv,GLenum,GLenum,const GLint*,298)
X void __stdcall glTexImage1D(GLenum a,GLint b,GLint c,GLsizei d,GLint e,GLenum f,GLenum g,const GLvoid*h){typedef void(__stdcall*F)(GLenum,GLint,GLint,GLsizei,GLint,GLenum,GLenum,const GLvoid*);((F)R(299,"glTexImage1D"))(a,b,c,d,e,f,g,h);}
// glTexImage2D - HOOKED
G3V(glTexParameterf,GLenum,GLenum,GLfloat,301) G3V(glTexParameterfv,GLenum,GLenum,const GLfloat*,302) G3V(glTexParameteri,GLenum,GLenum,GLint,303) G3V(glTexParameteriv,GLenum,GLenum,const GLint*,304)
X void __stdcall glTexSubImage1D(GLenum a,GLint b,GLint c,GLsizei d,GLenum e,GLenum f,const GLvoid*g){typedef void(__stdcall*F)(GLenum,GLint,GLint,GLsizei,GLenum,GLenum,const GLvoid*);((F)R(305,"glTexSubImage1D"))(a,b,c,d,e,f,g);}
X void __stdcall glTexSubImage2D(GLenum a,GLint b,GLint c,GLint d,GLsizei e,GLsizei f,GLenum g,GLenum h,const GLvoid*i){typedef void(__stdcall*F)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const GLvoid*);((F)R(306,"glTexSubImage2D"))(a,b,c,d,e,f,g,h,i);}
G3V(glTranslated,GLdouble,GLdouble,GLdouble,307) G3V(glTranslatef,GLfloat,GLfloat,GLfloat,308)
G2V(glVertex2d,GLdouble,GLdouble,309) G1V(glVertex2dv,const GLdouble*,310) G2V(glVertex2f,GLfloat,GLfloat,311) G1V(glVertex2fv,const GLfloat*,312)
G2V(glVertex2i,GLint,GLint,313) G1V(glVertex2iv,const GLint*,314) G2V(glVertex2s,GLshort,GLshort,315) G1V(glVertex2sv,const GLshort*,316)
G3V(glVertex3d,GLdouble,GLdouble,GLdouble,317) G1V(glVertex3dv,const GLdouble*,318) G3V(glVertex3f,GLfloat,GLfloat,GLfloat,319) G1V(glVertex3fv,const GLfloat*,320)
G3V(glVertex3i,GLint,GLint,GLint,321) G1V(glVertex3iv,const GLint*,322) G3V(glVertex3s,GLshort,GLshort,GLshort,323) G1V(glVertex3sv,const GLshort*,324)
G4V(glVertex4d,GLdouble,GLdouble,GLdouble,GLdouble,325) G1V(glVertex4dv,const GLdouble*,326) G4V(glVertex4f,GLfloat,GLfloat,GLfloat,GLfloat,327) G1V(glVertex4fv,const GLfloat*,328)
G4V(glVertex4i,GLint,GLint,GLint,GLint,329) G1V(glVertex4iv,const GLint*,330) G4V(glVertex4s,GLshort,GLshort,GLshort,GLshort,331) G1V(glVertex4sv,const GLshort*,332)
G4V(glVertexPointer,GLint,GLenum,GLsizei,const GLvoid*,333)
// glViewport - HOOKED

// WGL functions (use void* for all handle types)
X int __stdcall wglChoosePixelFormat(void*a,const void*b){typedef int(__stdcall*F)(void*,const void*);return((F)R(340,"wglChoosePixelFormat"))(a,b);}
X int __stdcall wglCopyContext(void*a,void*b,unsigned int c){typedef int(__stdcall*F)(void*,void*,unsigned int);return((F)R(341,"wglCopyContext"))(a,b,c);}
X void* __stdcall wglCreateContext(void*a){typedef void*(__stdcall*F)(void*);return((F)R(342,"wglCreateContext"))(a);}
X void* __stdcall wglCreateLayerContext(void*a,int b){typedef void*(__stdcall*F)(void*,int);return((F)R(343,"wglCreateLayerContext"))(a,b);}
X int __stdcall wglDeleteContext(void*a){typedef int(__stdcall*F)(void*);return((F)R(344,"wglDeleteContext"))(a);}
X int __stdcall wglDescribeLayerPlane(void*a,int b,int c,unsigned int d,void*e){typedef int(__stdcall*F)(void*,int,int,unsigned int,void*);return((F)R(345,"wglDescribeLayerPlane"))(a,b,c,d,e);}
X int __stdcall wglDescribePixelFormat(void*a,int b,unsigned int c,void*d){typedef int(__stdcall*F)(void*,int,unsigned int,void*);return((F)R(346,"wglDescribePixelFormat"))(a,b,c,d);}
X void* __stdcall wglGetCurrentContext(){typedef void*(__stdcall*F)();return((F)R(347,"wglGetCurrentContext"))();}
X void* __stdcall wglGetCurrentDC(){typedef void*(__stdcall*F)();return((F)R(348,"wglGetCurrentDC"))();}
X void* __stdcall wglGetDefaultProcAddress(const char*a){typedef void*(__stdcall*F)(const char*);return((F)R(349,"wglGetDefaultProcAddress"))(a);}
X int __stdcall wglGetLayerPaletteEntries(void*a,int b,int c,int d,void*e){typedef int(__stdcall*F)(void*,int,int,int,void*);return((F)R(350,"wglGetLayerPaletteEntries"))(a,b,c,d,e);}
X int __stdcall wglGetPixelFormat(void*a){typedef int(__stdcall*F)(void*);return((F)R(351,"wglGetPixelFormat"))(a);}
// wglGetProcAddress - HOOKED
// wglMakeCurrent - HOOKED
X int __stdcall wglRealizeLayerPalette(void*a,int b,int c){typedef int(__stdcall*F)(void*,int,int);return((F)R(355,"wglRealizeLayerPalette"))(a,b,c);}
X int __stdcall wglSetLayerPaletteEntries(void*a,int b,int c,int d,const void*e){typedef int(__stdcall*F)(void*,int,int,int,const void*);return((F)R(356,"wglSetLayerPaletteEntries"))(a,b,c,d,e);}
X int __stdcall wglSetPixelFormat(void*a,int b,const void*c){typedef int(__stdcall*F)(void*,int,const void*);return((F)R(357,"wglSetPixelFormat"))(a,b,c);}
X int __stdcall wglShareLists(void*a,void*b){typedef int(__stdcall*F)(void*,void*);return((F)R(358,"wglShareLists"))(a,b);}
// wglSwapBuffers - HOOKED
X int __stdcall wglSwapLayerBuffers(void*a,unsigned int b){typedef int(__stdcall*F)(void*,unsigned int);return((F)R(359,"wglSwapLayerBuffers"))(a,b);}
X unsigned long __stdcall wglSwapMultipleBuffers(unsigned int a,const void*b){typedef unsigned long(__stdcall*F)(unsigned int,const void*);return((F)R(360,"wglSwapMultipleBuffers"))(a,b);}
X int __stdcall wglUseFontBitmapsA(void*a,unsigned long b,unsigned long c,unsigned long d){typedef int(__stdcall*F)(void*,unsigned long,unsigned long,unsigned long);return((F)R(361,"wglUseFontBitmapsA"))(a,b,c,d);}
X int __stdcall wglUseFontBitmapsW(void*a,unsigned long b,unsigned long c,unsigned long d){typedef int(__stdcall*F)(void*,unsigned long,unsigned long,unsigned long);return((F)R(362,"wglUseFontBitmapsW"))(a,b,c,d);}
X int __stdcall wglUseFontOutlinesA(void*a,unsigned long b,unsigned long c,unsigned long d,float e,float f,int g,void*h){typedef int(__stdcall*F)(void*,unsigned long,unsigned long,unsigned long,float,float,int,void*);return((F)R(363,"wglUseFontOutlinesA"))(a,b,c,d,e,f,g,h);}
X int __stdcall wglUseFontOutlinesW(void*a,unsigned long b,unsigned long c,unsigned long d,float e,float f,int g,void*h){typedef int(__stdcall*F)(void*,unsigned long,unsigned long,unsigned long,float,float,int,void*);return((F)R(364,"wglUseFontOutlinesW"))(a,b,c,d,e,f,g,h);}

} // extern "C"
