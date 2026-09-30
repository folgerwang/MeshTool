// Absolute minimum proxy opengl32.dll - ONLY forwards, no hooks at all.
// If this crashes GE, the issue is in the forwarding mechanism itself.
// If this works, the issue is in our hook/IPC/overlay code.

#define NOGDI
#include <windows.h>
#undef NOGDI
#include <cstdio>

static HMODULE g_real = nullptr;
static bool g_logged = false;

static void EnsureLoaded()
{
    if (g_real) return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\opengl32.dll");
    g_real = LoadLibraryA(path);

    if (!g_logged)
    {
        g_logged = true;
        FILE* f = fopen("C:\\meshtool_hook.log", "a");
        if (f) { fprintf(f, "MINIMAL proxy: EnsureLoaded real=%p PID=%lu\n", g_real, GetCurrentProcessId()); fclose(f); }
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        // Do NOT call LoadLibrary here - it can deadlock or crash inside DllMain.
        // The real DLL is loaded lazily on first GL function call.
        FILE* f = fopen("C:\\meshtool_hook.log", "w");
        if (f) { fprintf(f, "MINIMAL proxy DllMain ATTACH PID=%lu\n", GetCurrentProcessId()); fclose(f); }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_real) { FreeLibrary(g_real); g_real = nullptr; }
    }
    return TRUE;
}

static void* _c[400] = {};
static void* R(int i, const char* n) { EnsureLoaded(); if(!_c[i]) _c[i]=(void*)GetProcAddress(g_real,n); return _c[i]; }

extern "C" {
#define X __declspec(dllexport)
typedef unsigned int E; typedef int I; typedef float F; typedef double D;
typedef unsigned char B; typedef short S; typedef unsigned short US;
typedef unsigned int U; typedef signed char SB; typedef void V;

#define G0V(n,i) X void __stdcall n(){typedef void(__stdcall*T)();((T)R(i,#n))();}
#define G0R(n,RT,i) X RT __stdcall n(){typedef RT(__stdcall*T)();return((T)R(i,#n))();}
#define G1V(n,A,i) X void __stdcall n(A a){typedef void(__stdcall*T)(A);((T)R(i,#n))(a);}
#define G1R(n,RT,A,i) X RT __stdcall n(A a){typedef RT(__stdcall*T)(A);return((T)R(i,#n))(a);}
#define G2V(n,A,A2,i) X void __stdcall n(A a,A2 b){typedef void(__stdcall*T)(A,A2);((T)R(i,#n))(a,b);}
#define G2R(n,RT,A,A2,i) X RT __stdcall n(A a,A2 b){typedef RT(__stdcall*T)(A,A2);return((T)R(i,#n))(a,b);}
#define G3V(n,A,A2,A3,i) X void __stdcall n(A a,A2 b,A3 c){typedef void(__stdcall*T)(A,A2,A3);((T)R(i,#n))(a,b,c);}
#define G3R(n,RT,A,A2,A3,i) X RT __stdcall n(A a,A2 b,A3 c){typedef RT(__stdcall*T)(A,A2,A3);return((T)R(i,#n))(a,b,c);}
#define G4V(n,A,A2,A3,A4,i) X void __stdcall n(A a,A2 b,A3 c,A4 d){typedef void(__stdcall*T)(A,A2,A3,A4);((T)R(i,#n))(a,b,c,d);}
#define G4R(n,RT,A,A2,A3,A4,i) X RT __stdcall n(A a,A2 b,A3 c,A4 d){typedef RT(__stdcall*T)(A,A2,A3,A4);return((T)R(i,#n))(a,b,c,d);}
#define G5V(n,A,A2,A3,A4,A5,i) X void __stdcall n(A a,A2 b,A3 c,A4 d,A5 e){typedef void(__stdcall*T)(A,A2,A3,A4,A5);((T)R(i,#n))(a,b,c,d,e);}
#define G5R(n,RT,A,A2,A3,A4,A5,i) X RT __stdcall n(A a,A2 b,A3 c,A4 d,A5 e){typedef RT(__stdcall*T)(A,A2,A3,A4,A5);return((T)R(i,#n))(a,b,c,d,e);}
#define G6V(n,A,A2,A3,A4,A5,A6,i) X void __stdcall n(A a,A2 b,A3 c,A4 d,A5 e,A6 f){typedef void(__stdcall*T)(A,A2,A3,A4,A5,A6);((T)R(i,#n))(a,b,c,d,e,f);}
#define G7V(n,A,A2,A3,A4,A5,A6,A7,i) X void __stdcall n(A a,A2 b,A3 c,A4 d,A5 e,A6 f,A7 g){typedef void(__stdcall*T)(A,A2,A3,A4,A5,A6,A7);((T)R(i,#n))(a,b,c,d,e,f,g);}
#define G8V(n,A,A2,A3,A4,A5,A6,A7,A8,i) X void __stdcall n(A a,A2 b,A3 c,A4 d,A5 e,A6 f,A7 g,A8 h){typedef void(__stdcall*T)(A,A2,A3,A4,A5,A6,A7,A8);((T)R(i,#n))(a,b,c,d,e,f,g,h);}
#define G9V(n,A,A2,A3,A4,A5,A6,A7,A8,A9,i) X void __stdcall n(A a,A2 b,A3 c,A4 d,A5 e,A6 f,A7 g,A8 h,A9 j){typedef void(__stdcall*T)(A,A2,A3,A4,A5,A6,A7,A8,A9);((T)R(i,#n))(a,b,c,d,e,f,g,h,j);}
#define G10V(n,A,A2,A3,A4,A5,A6,A7,A8,A9,A10,i) X void __stdcall n(A a,A2 b,A3 c,A4 d,A5 e,A6 f,A7 g,A8 h,A9 j,A10 k){typedef void(__stdcall*T)(A,A2,A3,A4,A5,A6,A7,A8,A9,A10);((T)R(i,#n))(a,b,c,d,e,f,g,h,j,k);}
#define G8R(n,RT,A,A2,A3,A4,A5,A6,A7,A8,i) X RT __stdcall n(A a,A2 b,A3 c,A4 d,A5 e,A6 f,A7 g,A8 h){typedef RT(__stdcall*T)(A,A2,A3,A4,A5,A6,A7,A8);return((T)R(i,#n))(a,b,c,d,e,f,g,h);}

// Glmf* stubs (ordinals 1-6) + glDebugEntry (ordinal 65) - forward to real DLL
G2V(GlmfBeginGlsBlock,I,I,370) G1V(GlmfCloseMetaFile,I,371) G2V(GlmfEndGlsBlock,I,I,372)
G1V(GlmfEndPlayback,I,373)
X int __stdcall GlmfInitPlayback(V*a,V*b,V*c){typedef int(__stdcall*T)(V*,V*,V*);return((T)R(374,"GlmfInitPlayback"))(a,b,c);}
X int __stdcall GlmfPlayGlsRecord(I a,I b,V*c,V*d){typedef int(__stdcall*T)(I,I,V*,V*);return((T)R(375,"GlmfPlayGlsRecord"))(a,b,c,d);}

// All GL+WGL exports - pure forwarding, no hooks (ordinals shifted by +6 for Glmf*)
G2V(glAccum,E,F,0) G2V(glAlphaFunc,E,F,1) G3R(glAreTexturesResident,B,I,const U*,B*,2) G1V(glArrayElement,I,3) G1V(glBegin,E,4)
G2V(glBindTexture,E,U,5) G7V(glBitmap,I,I,F,F,F,F,const B*,6) G2V(glBlendFunc,E,E,7) G1V(glCallList,U,8) G3V(glCallLists,I,E,const V*,9)
G1V(glClear,U,10) G4V(glClearAccum,F,F,F,F,11) G4V(glClearColor,F,F,F,F,12) G1V(glClearDepth,D,13) G1V(glClearIndex,F,14) G1V(glClearStencil,I,15)
G2V(glClipPlane,E,const D*,16) G3V(glColor3b,SB,SB,SB,17) G1V(glColor3bv,const SB*,18) G3V(glColor3d,D,D,D,19) G1V(glColor3dv,const D*,20)
G3V(glColor3f,F,F,F,21) G1V(glColor3fv,const F*,22) G3V(glColor3i,I,I,I,23) G1V(glColor3iv,const I*,24) G3V(glColor3s,S,S,S,25) G1V(glColor3sv,const S*,26)
G3V(glColor3ub,B,B,B,27) G1V(glColor3ubv,const B*,28) G3V(glColor3ui,U,U,U,29) G1V(glColor3uiv,const U*,30) G3V(glColor3us,US,US,US,31) G1V(glColor3usv,const US*,32)
G4V(glColor4b,SB,SB,SB,SB,33) G1V(glColor4bv,const SB*,34) G4V(glColor4d,D,D,D,D,35) G1V(glColor4dv,const D*,36)
G4V(glColor4f,F,F,F,F,37) G1V(glColor4fv,const F*,38) G4V(glColor4i,I,I,I,I,39) G1V(glColor4iv,const I*,40)
G4V(glColor4s,S,S,S,S,41) G1V(glColor4sv,const S*,42) G4V(glColor4ub,B,B,B,B,43) G1V(glColor4ubv,const B*,44)
G4V(glColor4ui,U,U,U,U,45) G1V(glColor4uiv,const U*,46) G4V(glColor4us,US,US,US,US,47) G1V(glColor4usv,const US*,48)
G4V(glColorMask,B,B,B,B,49) G2V(glColorMaterial,E,E,50) G4V(glColorPointer,I,E,I,const V*,51)
G5V(glCopyPixels,I,I,I,I,E,52) G7V(glCopyTexImage1D,E,I,E,I,I,I,I,53) G8V(glCopyTexImage2D,E,I,E,I,I,I,I,I,54)
G6V(glCopyTexSubImage1D,E,I,I,I,I,I,55) G8V(glCopyTexSubImage2D,E,I,I,I,I,I,I,I,56)
G1V(glCullFace,E,57)
G2V(glDebugEntry,E,E,376)
G2V(glDeleteLists,U,I,58) G2V(glDeleteTextures,I,const U*,59)
G1V(glDepthFunc,E,60) G1V(glDepthMask,B,61) G2V(glDepthRange,D,D,62) G1V(glDisable,E,63) G1V(glDisableClientState,E,64)
G3V(glDrawArrays,E,I,I,65) G1V(glDrawBuffer,E,66) G4V(glDrawElements,E,I,E,const V*,67) G5V(glDrawPixels,I,I,E,E,const V*,68)
G1V(glEdgeFlag,B,69) G2V(glEdgeFlagPointer,I,const V*,70) G1V(glEdgeFlagv,const B*,71) G1V(glEnable,E,72) G1V(glEnableClientState,E,73)
G0V(glEnd,74) G0V(glEndList,75) G1V(glEvalCoord1d,D,76) G1V(glEvalCoord1dv,const D*,77) G1V(glEvalCoord1f,F,78) G1V(glEvalCoord1fv,const F*,79)
G2V(glEvalCoord2d,D,D,80) G1V(glEvalCoord2dv,const D*,81) G2V(glEvalCoord2f,F,F,82) G1V(glEvalCoord2fv,const F*,83)
G3V(glEvalMesh1,E,I,I,84) G5V(glEvalMesh2,E,I,I,I,I,85) G1V(glEvalPoint1,I,86) G2V(glEvalPoint2,I,I,87)
G3V(glFeedbackBuffer,I,E,F*,88) G0V(glFinish,89) G0V(glFlush,90) G2V(glFogf,E,F,91) G2V(glFogfv,E,const F*,92) G2V(glFogi,E,I,93) G2V(glFogiv,E,const I*,94)
G1V(glFrontFace,E,95) G6V(glFrustum,D,D,D,D,D,D,96) G1R(glGenLists,U,I,97) G2V(glGenTextures,I,U*,98)
G2V(glGetBooleanv,E,B*,99) G2V(glGetClipPlane,E,D*,100) G2V(glGetDoublev,E,D*,101) G0R(glGetError,E,102) G2V(glGetFloatv,E,F*,103) G2V(glGetIntegerv,E,I*,104)
G3V(glGetLightfv,E,E,F*,105) G3V(glGetLightiv,E,E,I*,106) G3V(glGetMapdv,E,E,D*,107) G3V(glGetMapfv,E,E,F*,108) G3V(glGetMapiv,E,E,I*,109)
G3V(glGetMaterialfv,E,E,F*,110) G3V(glGetMaterialiv,E,E,I*,111) G2V(glGetPixelMapfv,E,F*,112) G2V(glGetPixelMapuiv,E,U*,113) G2V(glGetPixelMapusv,E,US*,114)
G2V(glGetPointerv,E,V**,115) G1V(glGetPolygonStipple,B*,116) G1R(glGetString,const B*,E,117)
G3V(glGetTexEnvfv,E,E,F*,118) G3V(glGetTexEnviv,E,E,I*,119) G3V(glGetTexGendv,E,E,D*,120) G3V(glGetTexGenfv,E,E,F*,121) G3V(glGetTexGeniv,E,E,I*,122)
G5V(glGetTexImage,E,I,E,E,V*,123) G4V(glGetTexLevelParameterfv,E,I,E,F*,124) G4V(glGetTexLevelParameteriv,E,I,E,I*,125)
G3V(glGetTexParameterfv,E,E,F*,126) G3V(glGetTexParameteriv,E,E,I*,127) G2V(glHint,E,E,128) G1V(glIndexMask,U,129) G3V(glIndexPointer,E,I,const V*,130)
G1V(glIndexd,D,131) G1V(glIndexdv,const D*,132) G1V(glIndexf,F,133) G1V(glIndexfv,const F*,134) G1V(glIndexi,I,135) G1V(glIndexiv,const I*,136)
G1V(glIndexs,S,137) G1V(glIndexsv,const S*,138) G1V(glIndexub,B,139) G1V(glIndexubv,const B*,140) G0V(glInitNames,141) G3V(glInterleavedArrays,E,I,const V*,142)
G1R(glIsEnabled,B,E,143) G1R(glIsList,B,U,144) G1R(glIsTexture,B,U,145)
G2V(glLightModelf,E,F,146) G2V(glLightModelfv,E,const F*,147) G2V(glLightModeli,E,I,148) G2V(glLightModeliv,E,const I*,149)
G3V(glLightf,E,E,F,150) G3V(glLightfv,E,E,const F*,151) G3V(glLighti,E,E,I,152) G3V(glLightiv,E,E,const I*,153)
G2V(glLineStipple,I,US,154) G1V(glLineWidth,F,155) G1V(glListBase,U,156) G0V(glLoadIdentity,157) G1V(glLoadMatrixd,const D*,158) G1V(glLoadMatrixf,const F*,159)
G1V(glLoadName,U,160) G1V(glLogicOp,E,161) G6V(glMap1d,E,D,D,I,I,const D*,162) G6V(glMap1f,E,F,F,I,I,const F*,163)
G10V(glMap2d,E,D,D,I,I,D,D,I,I,const D*,164) G10V(glMap2f,E,F,F,I,I,F,F,I,I,const F*,165)
G3V(glMapGrid1d,I,D,D,166) G3V(glMapGrid1f,I,F,F,167) G6V(glMapGrid2d,I,D,D,I,D,D,168) G6V(glMapGrid2f,I,F,F,I,F,F,169)
G3V(glMaterialf,E,E,F,170) G3V(glMaterialfv,E,E,const F*,171) G3V(glMateriali,E,E,I,172) G3V(glMaterialiv,E,E,const I*,173)
G1V(glMatrixMode,E,174) G1V(glMultMatrixd,const D*,175) G1V(glMultMatrixf,const F*,176) G2V(glNewList,U,E,177)
G3V(glNormal3b,SB,SB,SB,178) G1V(glNormal3bv,const SB*,179) G3V(glNormal3d,D,D,D,180) G1V(glNormal3dv,const D*,181)
G3V(glNormal3f,F,F,F,182) G1V(glNormal3fv,const F*,183) G3V(glNormal3i,I,I,I,184) G1V(glNormal3iv,const I*,185) G3V(glNormal3s,S,S,S,186) G1V(glNormal3sv,const S*,187) G3V(glNormalPointer,E,I,const V*,377)
G6V(glOrtho,D,D,D,D,D,D,188) G1V(glPassThrough,F,189) G3V(glPixelMapfv,E,I,const F*,190) G3V(glPixelMapuiv,E,I,const U*,191) G3V(glPixelMapusv,E,I,const US*,192)
G2V(glPixelStoref,E,F,193) G2V(glPixelStorei,E,I,194) G2V(glPixelTransferf,E,F,195) G2V(glPixelTransferi,E,I,196) G2V(glPixelZoom,F,F,197)
G1V(glPointSize,F,198) G2V(glPolygonMode,E,E,199) G2V(glPolygonOffset,F,F,200) G1V(glPolygonStipple,const B*,201)
G0V(glPopAttrib,202) G0V(glPopClientAttrib,203) G0V(glPopMatrix,204) G0V(glPopName,205) G3V(glPrioritizeTextures,I,const U*,const F*,206)
G1V(glPushAttrib,U,207) G1V(glPushClientAttrib,U,208) G0V(glPushMatrix,209) G1V(glPushName,U,210)
G2V(glRasterPos2d,D,D,211) G1V(glRasterPos2dv,const D*,212) G2V(glRasterPos2f,F,F,213) G1V(glRasterPos2fv,const F*,214) G2V(glRasterPos2i,I,I,215) G1V(glRasterPos2iv,const I*,216) G2V(glRasterPos2s,S,S,217) G1V(glRasterPos2sv,const S*,218)
G3V(glRasterPos3d,D,D,D,219) G1V(glRasterPos3dv,const D*,220) G3V(glRasterPos3f,F,F,F,221) G1V(glRasterPos3fv,const F*,222) G3V(glRasterPos3i,I,I,I,223) G1V(glRasterPos3iv,const I*,224) G3V(glRasterPos3s,S,S,S,225) G1V(glRasterPos3sv,const S*,226)
G4V(glRasterPos4d,D,D,D,D,227) G1V(glRasterPos4dv,const D*,228) G4V(glRasterPos4f,F,F,F,F,229) G1V(glRasterPos4fv,const F*,230) G4V(glRasterPos4i,I,I,I,I,231) G1V(glRasterPos4iv,const I*,232) G4V(glRasterPos4s,S,S,S,S,233) G1V(glRasterPos4sv,const S*,234)
G1V(glReadBuffer,E,235) G7V(glReadPixels,I,I,I,I,E,E,V*,236) G4V(glRectd,D,D,D,D,237) G2V(glRectdv,const D*,const D*,238) G4V(glRectf,F,F,F,F,239) G2V(glRectfv,const F*,const F*,240) G4V(glRecti,I,I,I,I,241) G2V(glRectiv,const I*,const I*,242) G4V(glRects,S,S,S,S,243) G2V(glRectsv,const S*,const S*,244)
G1R(glRenderMode,I,E,245) G4V(glRotated,D,D,D,D,246) G4V(glRotatef,F,F,F,F,247) G3V(glScaled,D,D,D,248) G3V(glScalef,F,F,F,249) G4V(glScissor,I,I,I,I,250) G2V(glSelectBuffer,I,U*,251)
G1V(glShadeModel,E,252) G3V(glStencilFunc,E,I,U,253) G1V(glStencilMask,U,254) G3V(glStencilOp,E,E,E,255)
G1V(glTexCoord1d,D,256) G1V(glTexCoord1dv,const D*,257) G1V(glTexCoord1f,F,258) G1V(glTexCoord1fv,const F*,259) G1V(glTexCoord1i,I,260) G1V(glTexCoord1iv,const I*,261) G1V(glTexCoord1s,S,262) G1V(glTexCoord1sv,const S*,263)
G2V(glTexCoord2d,D,D,264) G1V(glTexCoord2dv,const D*,265) G2V(glTexCoord2f,F,F,266) G1V(glTexCoord2fv,const F*,267) G2V(glTexCoord2i,I,I,268) G1V(glTexCoord2iv,const I*,269) G2V(glTexCoord2s,S,S,270) G1V(glTexCoord2sv,const S*,271)
G3V(glTexCoord3d,D,D,D,272) G1V(glTexCoord3dv,const D*,273) G3V(glTexCoord3f,F,F,F,274) G1V(glTexCoord3fv,const F*,275) G3V(glTexCoord3i,I,I,I,276) G1V(glTexCoord3iv,const I*,277) G3V(glTexCoord3s,S,S,S,278) G1V(glTexCoord3sv,const S*,279)
G4V(glTexCoord4d,D,D,D,D,280) G1V(glTexCoord4dv,const D*,281) G4V(glTexCoord4f,F,F,F,F,282) G1V(glTexCoord4fv,const F*,283) G4V(glTexCoord4i,I,I,I,I,284) G1V(glTexCoord4iv,const I*,285) G4V(glTexCoord4s,S,S,S,S,286) G1V(glTexCoord4sv,const S*,287)
G4V(glTexCoordPointer,I,E,I,const V*,288) G3V(glTexEnvf,E,E,F,289) G3V(glTexEnvfv,E,E,const F*,290) G3V(glTexEnvi,E,E,I,291) G3V(glTexEnviv,E,E,const I*,292)
G3V(glTexGend,E,E,D,293) G3V(glTexGendv,E,E,const D*,294) G3V(glTexGenf,E,E,F,295) G3V(glTexGenfv,E,E,const F*,296) G3V(glTexGeni,E,E,I,297) G3V(glTexGeniv,E,E,const I*,298)
G8V(glTexImage1D,E,I,I,I,I,E,E,const V*,299) G9V(glTexImage2D,E,I,I,I,I,I,E,E,const V*,300)
G3V(glTexParameterf,E,E,F,301) G3V(glTexParameterfv,E,E,const F*,302) G3V(glTexParameteri,E,E,I,303) G3V(glTexParameteriv,E,E,const I*,304)
G7V(glTexSubImage1D,E,I,I,I,E,E,const V*,305) G9V(glTexSubImage2D,E,I,I,I,I,I,E,E,const V*,306)
G3V(glTranslated,D,D,D,307) G3V(glTranslatef,F,F,F,308)
G2V(glVertex2d,D,D,309) G1V(glVertex2dv,const D*,310) G2V(glVertex2f,F,F,311) G1V(glVertex2fv,const F*,312) G2V(glVertex2i,I,I,313) G1V(glVertex2iv,const I*,314) G2V(glVertex2s,S,S,315) G1V(glVertex2sv,const S*,316)
G3V(glVertex3d,D,D,D,317) G1V(glVertex3dv,const D*,318) G3V(glVertex3f,F,F,F,319) G1V(glVertex3fv,const F*,320) G3V(glVertex3i,I,I,I,321) G1V(glVertex3iv,const I*,322) G3V(glVertex3s,S,S,S,323) G1V(glVertex3sv,const S*,324)
G4V(glVertex4d,D,D,D,D,325) G1V(glVertex4dv,const D*,326) G4V(glVertex4f,F,F,F,F,327) G1V(glVertex4fv,const F*,328) G4V(glVertex4i,I,I,I,I,329) G1V(glVertex4iv,const I*,330) G4V(glVertex4s,S,S,S,S,331) G1V(glVertex4sv,const S*,332)
G4V(glVertexPointer,I,E,I,const V*,333) G4V(glViewport,I,I,I,I,334)

// WGL - all use void* for handles
G2R(wglChoosePixelFormat,I,V*,const V*,340) G3R(wglCopyContext,I,V*,V*,U,341) G1R(wglCreateContext,V*,V*,342) G2R(wglCreateLayerContext,V*,V*,I,343) G1R(wglDeleteContext,I,V*,344)
G5R(wglDescribeLayerPlane,I,V*,I,I,U,V*,345) G4R(wglDescribePixelFormat,I,V*,I,U,V*,346) G0R(wglGetCurrentContext,V*,347) G0R(wglGetCurrentDC,V*,348)
G1R(wglGetDefaultProcAddress,V*,const char*,349) G5R(wglGetLayerPaletteEntries,I,V*,I,I,I,V*,350) G1R(wglGetPixelFormat,I,V*,351)
G1R(wglGetProcAddress,V*,const char*,352) G2R(wglMakeCurrent,I,V*,V*,353) G3R(wglRealizeLayerPalette,I,V*,I,I,354)
G5R(wglSetLayerPaletteEntries,I,V*,I,I,I,const V*,355) G3R(wglSetPixelFormat,I,V*,I,const V*,356)
G2R(wglShareLists,I,V*,V*,357) // wglSwapBuffers - HOOKED for overlay
X int __stdcall wglSwapBuffers(V* hdc)
{
    EnsureLoaded();

    // Simple overlay: draw colored border to confirm hook works
    typedef void(__stdcall*PFN_PushAttrib)(U);
    typedef void(__stdcall*PFN_PopAttrib)();
    typedef void(__stdcall*PFN_MatMode)(E);
    typedef void(__stdcall*PFN_PushMat)();
    typedef void(__stdcall*PFN_PopMat)();
    typedef void(__stdcall*PFN_LoadId)();
    typedef void(__stdcall*PFN_Ortho)(D,D,D,D,D,D);
    typedef void(__stdcall*PFN_Begin)(E);
    typedef void(__stdcall*PFN_End)();
    typedef void(__stdcall*PFN_Color4f)(F,F,F,F);
    typedef void(__stdcall*PFN_Vertex2f)(F,F);
    typedef void(__stdcall*PFN_Disable)(E);
    typedef void(__stdcall*PFN_Enable)(E);
    typedef void(__stdcall*PFN_GetIV)(E,I*);
    typedef void(__stdcall*PFN_BlendFunc)(E,E);

    static PFN_PushAttrib pPushAttrib = nullptr;
    static PFN_PopAttrib pPopAttrib = nullptr;
    static PFN_MatMode pMatMode = nullptr;
    static PFN_PushMat pPushMat = nullptr;
    static PFN_PopMat pPopMat = nullptr;
    static PFN_LoadId pLoadId = nullptr;
    static PFN_Ortho pOrtho = nullptr;
    static PFN_Begin pBegin = nullptr;
    static PFN_End pEnd = nullptr;
    static PFN_Color4f pColor4f = nullptr;
    static PFN_Vertex2f pVertex2f = nullptr;
    static PFN_Disable pDisable = nullptr;
    static PFN_Enable pEnable = nullptr;
    static PFN_GetIV pGetIV = nullptr;
    static PFN_BlendFunc pBlendFunc = nullptr;

    if (!pPushAttrib) {
        pPushAttrib = (PFN_PushAttrib)GetProcAddress(g_real, "glPushAttrib");
        pPopAttrib = (PFN_PopAttrib)GetProcAddress(g_real, "glPopAttrib");
        pMatMode = (PFN_MatMode)GetProcAddress(g_real, "glMatrixMode");
        pPushMat = (PFN_PushMat)GetProcAddress(g_real, "glPushMatrix");
        pPopMat = (PFN_PopMat)GetProcAddress(g_real, "glPopMatrix");
        pLoadId = (PFN_LoadId)GetProcAddress(g_real, "glLoadIdentity");
        pOrtho = (PFN_Ortho)GetProcAddress(g_real, "glOrtho");
        pBegin = (PFN_Begin)GetProcAddress(g_real, "glBegin");
        pEnd = (PFN_End)GetProcAddress(g_real, "glEnd");
        pColor4f = (PFN_Color4f)GetProcAddress(g_real, "glColor4f");
        pVertex2f = (PFN_Vertex2f)GetProcAddress(g_real, "glVertex2f");
        pDisable = (PFN_Disable)GetProcAddress(g_real, "glDisable");
        pEnable = (PFN_Enable)GetProcAddress(g_real, "glEnable");
        pGetIV = (PFN_GetIV)GetProcAddress(g_real, "glGetIntegerv");
        pBlendFunc = (PFN_BlendFunc)GetProcAddress(g_real, "glBlendFunc");
    }

    if (pPushAttrib && pBegin) {
        I vp[4]; pGetIV(0x0BA2, vp);
        F w = (F)vp[2], h = (F)vp[3];

        pPushAttrib(0x000FFFFF);
        pMatMode(0x1701); pPushMat(); pLoadId(); pOrtho(0, w, h, 0, -1, 1);
        pMatMode(0x1700); pPushMat(); pLoadId();
        pDisable(0x0B71); // depth test
        pEnable(0x0BE2);  // blend
        pBlendFunc(0x0302, 0x0303);
        pDisable(0x0DE1); // texture

        // Draw "MESHTOOL" panel background
        pBegin(0x0007); // QUADS
        pColor4f(0.0f, 0.0f, 0.0f, 0.7f);
        pVertex2f(10, 10); pVertex2f(250, 10); pVertex2f(250, 70); pVertex2f(10, 70);
        // Blue accent bar
        pColor4f(0.2f, 0.6f, 1.0f, 0.9f);
        pVertex2f(10, 10); pVertex2f(14, 10); pVertex2f(14, 70); pVertex2f(10, 70);
        // Green status dot
        pColor4f(0.2f, 0.9f, 0.3f, 1.0f);
        pVertex2f(20, 40); pVertex2f(30, 40); pVertex2f(30, 50); pVertex2f(20, 50);
        pEnd();

        pMatMode(0x1701); pPopMat();
        pMatMode(0x1700); pPopMat();
        pPopAttrib();
    }

    // Forward to real SwapBuffers
    typedef int(__stdcall*PFN_Swap)(V*);
    static PFN_Swap pSwap = nullptr;
    if (!pSwap) pSwap = (PFN_Swap)GetProcAddress(g_real, "wglSwapBuffers");
    return pSwap(hdc);
} G2R(wglSwapLayerBuffers,I,V*,U,359) G2R(wglSwapMultipleBuffers,unsigned long,U,const V*,360)
G4R(wglUseFontBitmapsA,I,V*,unsigned long,unsigned long,unsigned long,361) G4R(wglUseFontBitmapsW,I,V*,unsigned long,unsigned long,unsigned long,362)
G8R(wglUseFontOutlinesA,I,V*,unsigned long,unsigned long,unsigned long,F,F,I,V*,363) G8R(wglUseFontOutlinesW,I,V*,unsigned long,unsigned long,unsigned long,F,F,I,V*,364)

} // extern "C"
