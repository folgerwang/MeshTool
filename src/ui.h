#pragma once

#include <string>
#include <vector>
#include "imgui.h"
#include "progress.h"
#include "icons.h"
#include "segmenter.h"

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
    class GeoViewServer* geoViewServer = nullptr;   // GE reports its view (GPS) here
    std::string     geoText;                         // GPS readout for the viewport corner
    LiveCaptureProcessor* captureProcessor = nullptr;
    BatchMeshData* liveBatch = nullptr;

    bool wantCaptureFrame = false;
    bool captureWaiting = false;    // set by the app while a requested capture hasn't arrived
    bool wantStopCapture = false;
    bool wantFrameAll = false;      // point the camera at everything loaded

    // Segmentation (Tools > Segment Scene): settings and requests for the app,
    // progress reported back by it.
    SegmentSettings segSettings;
    bool  wantStartSegment = false;
    bool  wantCancelSegment = false;
    bool  segRunning = false;
    float segProgress = 0.0f;
    std::string segStatus;
    // Viewing segmented objects.
    bool  classColors = false;
    bool  buildingColors = true;   // with classColors: a distinct colour per building
    bool  classVisible[kObjClassCount] = { true, true, true, true, true, true, true, true };

    // Object clicked in the viewport (filled by the app; active = something selected).
    struct SelectionInfo
    {
        bool        active = false;
        std::string name;           // object name, or "mesh" for an unsegmented mesh
        std::string className;
        int         meshes = 0;
        size_t      triangles = 0;
        double      size[3] = {};   // bounding box extent, metres
        double      hit[3] = {};    // clicked point, scene coordinates
        std::string gpsText;        // clicked point in WGS84 when georeferenced
    };
    SelectionInfo selection;
    bool  isolateSelection = false;
    enum SelectionView { kSelSegment = 0, kSelActual = 1 };
    int   selectionView = kSelSegment;   // T toggles: highlighted segment / actual textured object
    bool  wantFrameSelection = false;
    bool  wantClearSelection = false;

    // Scene file requests, chosen in a file dialog and carried out by the app.
    std::string pendingOpenScenePath;
    std::string pendingSaveScenePath;
    void ActionOpenScene();
    void ActionSaveScene();
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

    // Segment Scene dialog
    bool showSegmentDialog = false;
    char segServer[256] = "http://127.0.0.1:11434";
    char segModel[128] = "qwen3.8:latest";
    void DrawSegmentDialog();
    void DrawObjectsSection();
    void DrawSelectionSection();

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
    void ActionImportUSGS();
    void ActionImportKML();
    void ActionExport();
    void ActionLiveCapture();
    void ActionNavCapture();  // Navigate to location & auto-capture

    // Pending auto-capture state
    bool  m_pendingAutoCapture = false;
    float m_autoCaptureTimer = 0;
    int   m_autoCaptureCount = 0;    // number of frames captured so far
    int   m_autoCaptureMaxFrames = 3; // capture multiple frames for better coverage
};
