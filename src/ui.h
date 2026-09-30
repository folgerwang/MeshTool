#pragma once

#include <string>
#include <vector>
#include "imgui.h"
#include "progress.h"
#include "icons.h"

struct WorldData;
struct BatchMeshData;
class ProcessManager;
class LiveCaptureProcessor;
class VulkanTextureManager;

class MeshToolUI {
public:
    void Init(VulkanTextureManager* texMgr = nullptr);
    void DrawUI();

    AtomicProgress progress;
    std::string statusMessage;
    float statusTimeout = 0;

    ProcessManager* processManager = nullptr;
    LiveCaptureProcessor* captureProcessor = nullptr;
    BatchMeshData* liveBatch = nullptr;

    bool wantCaptureFrame = false;
    bool wantStopCapture = false;

    // Viewport rect (set each frame, read by app for 3D rendering)
    float viewportX = 0, viewportY = 0, viewportW = 100, viewportH = 100;

private:
    // Modal state
    bool showRefPointDialog = false;
    double refLonInput = 0, refLatInput = 0;

    bool showRegionSelectDialog = false;
    double regionLon0 = 0, regionLat0 = 0;
    double regionLon1 = 0, regionLat1 = 0;

    bool showMessageBox = false;
    std::string messageBoxTitle;
    std::string messageBoxText;

    bool regionSelectMode = false;

    // Navigate & Capture dialog
    bool showNavCaptureDialog = false;
    int  navMode = 0;           // 0 = GPS coordinates, 1 = address/place
    double navLon = -122.4194;  // Default: San Francisco
    double navLat = 37.7749;
    double navAlt = 0.0;
    double navHeading = 0.0;
    double navTilt = 45.0;      // 45 deg tilt for good 3D view
    double navRange = 500.0;    // 500m viewing distance
    char   navAddress[256] = "San Francisco, CA";
    int    navCaptureDelay = 15; // seconds to wait for GE to load before capture
    bool   navAutoCapture = true;

    // Scene info panel
    bool showScenePanel = true;

    // Icon textures
    VulkanTextureManager* m_texMgr = nullptr;
    uint32_t m_iconHandles[icons::ICON_COUNT] = {};
    bool m_iconsLoaded = false;

    void ApplyTheme();
    void LoadIcons();
    bool IconBtn(icons::IconID id, const char* tooltip, const ImVec4& tint);
    void DrawMenuBar();
    void DrawToolbar();
    void DrawScenePanel();
    void DrawViewport();
    void DrawStatusBar();
    void DrawRefPointDialog();
    void DrawRegionSelectDialog();
    void DrawNavCaptureDialog();
    void DrawMessageBox();

    void ShowMessage(const std::string& title, const std::string& text);
    void SetStatus(const std::string& msg, float timeoutSeconds = 5.0f);

    // Action helpers
    void ActionImportGEDump();
    void ActionImportUSGS();
    void ActionImportKML();
    void ActionImportFBX();
    void ActionExport();
    void ActionLiveCapture();
    void ActionNavCapture();  // Navigate to location & auto-capture

    // Pending auto-capture state
    bool  m_pendingAutoCapture = false;
    float m_autoCaptureTimer = 0;
    int   m_autoCaptureCount = 0;    // number of frames captured so far
    int   m_autoCaptureMaxFrames = 3; // capture multiple frames for better coverage
};
