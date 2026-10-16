#pragma once

#include <GLFW/glfw3.h>
#include <string>
#include <map>
#include <memory>
#include <cstdint>
#include <future>
#include "route.h"
#include "sim.h"
#include "coremath.h"

class VulkanRenderer;
class VulkanMeshRenderer;
class VulkanTextureManager;
class VulkanPipelineManager;
class MeshToolUI;
class InputHandler;
class ViewCamera;
class VideoWriter;
class ProcessManager;
class LiveCaptureProcessor;
struct MeshDrawFrame;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
struct GroupMeshData;
struct MeshData;
struct BatchMeshData;

namespace core {
    template<class T> struct bounds3;
    typedef bounds3<double> bounds3d;
    struct Texture2DInfo;
}

class MeshToolApp {
public:
    MeshToolApp();
    ~MeshToolApp();

    bool Init();
    void OpenOnStart(const std::string& path);   // scene file given on the command line

    // Unattended check screenshots (MeshTool scene.mtscene --shots prefix ...):
    // once the scene is open, aim the camera (frame the named object, or the
    // whole scene), save <prefix>_segment.png / _original.png and exit.
    struct AutoShots
    {
        std::string prefix;
        std::string frameObject;     // object name, e.g. "building_012"; empty = everything
        double      yawDeg = 30.0;   // view direction (0 looks north, +90 east)
        double      pitchDeg = -35.0;
        double      zoom = 1.0;      // < 1 moves the camera closer than the framing distance
        bool        originalModels = false; // screenshot comparison mode
        bool        hasBox = false;  // frame this world box instead (same camera across scenes)
        double      box[6] = {};     // min x, y, z, max x, y, z
        bool        glass = true;    // draw glass facades as glass (false: as captured)
        bool        ortho = false;   // bird's-eye orthographic view instead of yaw/pitch
        double      sim = -1.0;      // >= 0: start the drive simulation, park the car this far along (0..1), chase camera
        std::string video;           // with sim: record the drive from there to this MP4 instead of screenshots ...
        double      videoSeconds = 10.0;   // ... for this long (or until the car arrives), then exit
        bool        heightCheck = true;   // --sim: check the car's height (off: the bare road profile)
        std::string roadCsv;              // --sim: write the recovered road's stations to this file
    };
    void SetAutoShots(const AutoShots& shots) { m_auto = shots; m_autoStage = 1; }
    // MeshTool.exe --capture-route "from" "to" [...]: start a route capture as
    // soon as the window is up; with `savePath`, save the scene and check
    // screenshots when it ends.
    struct AutoRoute
    {
        bool        enabled = false;
        std::string from, to, savePath;
        double      spacing = 100.0, height = 100.0, tilt = 0.0, dwell = 4.0;
        bool        lookBack = false;
    };
    void SetAutoRoute(const AutoRoute& route) { m_autoRoute = route; }
    void MainLoop();
    void Shutdown();

private:
    GLFWwindow* m_window = nullptr;

    VulkanRenderer*        m_renderer     = nullptr;
    VulkanMeshRenderer*    m_meshRenderer = nullptr;
    VulkanTextureManager*  m_texManager   = nullptr;
    VulkanPipelineManager* m_pipeManager  = nullptr;
    MeshToolUI*            m_ui           = nullptr;
    InputHandler*          m_input        = nullptr;
    ViewCamera*            m_camera       = nullptr;
    double                 m_lastFrameTime = 0.0;      // glfwGetTime() of the previous frame

    // GPU texture handle for each uploaded texture (owned by its group).
    std::map<const core::Texture2DInfo*, uint32_t> m_texHandles;

    // Receives Google Earth's view (GPS) through a KML NetworkLink.
    std::unique_ptr<class GeoViewServer> m_geoServer;

    // Tools > Refine Buildings: refine.py on a saved copy of the scene.
    std::unique_ptr<class BuildingRefiner> m_refiner;
    void UpdateRefine();
    // Background scene segmentation (Tools > Segment Scene).
    std::unique_ptr<class Segmenter> m_segmenter;
    void UpdateSegmentation();

    // Check screenshots: the 3D viewport saved twice - class colours, then
    // original textures - to .\screenshots\check_<time>_*.png.
    // Taken when segmentation finishes and from Tools > Save Check Screenshots.
    void StartCheckScreenshots(const std::string& base = std::string());
    AutoShots m_auto;
    int  m_autoStage = 0;          // 0 off, 1 wait for scene, 2 settle, 3 shooting
    int  m_autoFrames = 0;
    bool m_autoSimParked = false;
    void UpdateAutoShots();
    int  m_shotStage = 0;          // 0 idle, 1 class colours, 2 original colours
    bool m_shotPending = false;    // readback requested for this frame
    std::string m_shotBase;        // path prefix
    int  m_shotRect[4] = {};       // viewport in framebuffer pixels: x, y, w, h

    void UpdateGeoReadout();   // GPS of the camera pivot -> m_ui->geoText
    const BatchMeshData* GeoreferencedBatch() const;   // the batch whose GPS frame the scene uses, or null

    // Google Earth follows the viewport camera (m_ui->geFollowViewport): the
    // camera is sent once it has rested briefly and differs from the last sent.
    void UpdateGeFollow();
    core::vec3d m_followPrevEye, m_followPrevFwd;   // last frame's camera
    core::vec3d m_followSentEye, m_followSentFwd;   // camera last sent to GE
    double      m_followSentLat = 0.0, m_followSentLon = 0.0, m_followSentAlt = 0.0;   // ... as GE gets it
    // GE's reported camera and since when it hasn't changed (GE reports its
    // view about once a second, moving or not).
    double      m_geCamLat = 0.0, m_geCamLon = 0.0, m_geCamAlt = 0.0;
    double      m_geCamTime = -1.0, m_geStillSince = 0.0;
    double      m_followMoveTime = 0.0;             // glfwGetTime() of the last camera change
    bool        m_followSent = false;
    bool        m_followWasOn = false;
    // While following: may a capture be taken now? (GE has flown to the
    // viewport camera and come to rest.) Reason in m_ui->captureGate when not.
    bool        CaptureSettled();
    // Capture > Route Capture: Google Earth is flown along a route between
    // two addresses through the follow link and a capture is taken at every
    // waypoint once the view has settled.
    struct RouteCapture
    {
        enum State { kIdle, kPlanning, kLaunching, kFlying, kSettling, kCapturing };
        State  state = kIdle;
        std::future<RoutePlan> planning;
        RoutePlan plan;
        size_t next = 0;             // waypoint to fly to
        int    captured = 0;
        double stateTime = 0.0;      // glfwGetTime() the state was entered
        double settledTime = -1.0;   // when the gate first opened for this waypoint
        bool   connectSent = false;  // links re-sent to a Google Earth that was not polling
        std::string lastStatus;
    } m_route;
    void UpdateRouteCapture();
    AutoRoute m_autoRoute;
    bool      m_autoRouteStarted = false;
    // The driving route the scene was captured along: set when a route
    // capture has planned it, saved in the scene file, restored on open.
    RoutePlan m_sceneRoute;

    // Tools > Drive Simulation: a car drives the scene route at a set speed
    // over a road recovered from the mesh (see sim.h).
    struct DriveSim
    {
        bool   active = false, playing = false;
        double s = 0.0;              // metres along the road
        double elapsed = 0.0;        // seconds driven
        double wheelSpin = 0.0;      // radians
        double camYaw = 0.0;         // chase camera, smoothed
        bool   camInit = false;
        // Height check every `simHeightCheckM` metres (see CheckRoadHeight):
        // the lift over the profile eases towards the last check's result.
        double lift = 0.0, liftTarget = 0.0;
        double nextCheckAt = 0.0;    // metres along the road
        bool   liftSnap = true;      // apply the next check at once (start, restart, park)
        int    lifted = 0;           // checks that moved the car
        HeightCheck::Kind liftKind = HeightCheck::kNone;
        double liftTrees = 0.0;      // the tree height the last check subtracted
        DriveRoadParams params;
        DriveRoad  road;
        CarMeshes  car;
        SkyDome    sky;
        uint32_t   skyTexture = 0xFFFFFFFF;
        CanopyCut  cut;
        int        canopyRemoved = 0;
        int        baked = 0;        // height checks that moved the profile before the canopy cut
        std::future<RoutePlan> planning;   // route looked up for a scene saved without one
        bool   planningRoute = false;
    } m_sim;
    void StartDriveSim();
    void StopDriveSim(bool restoreCanopy);
    void UpdateDriveSim(double dt);
    void DrawDriveSim(VkCommandBuffer cmd, const float* viewProj, const MeshDrawFrame& frame);
    // Record video: while the car drives, every frame's viewport is read back
    // and encoded (H.264 MP4). The simulation then steps at the video's frame
    // rate instead of wall-clock time, so the video is smooth however slow
    // the readback makes rendering.
    struct VideoCapture
    {
        std::unique_ptr<VideoWriter> writer;
        bool   recording = false;
        bool   pending = false;      // readback requested for this frame
        int    rect[4] = {};         // viewport in framebuffer pixels: x, y, w, h (even)
        int    fps = 30;
        std::string path;
    } m_video;
    void StartVideo(const std::string& path);
    void StopVideo();
    void UpdateVideoFrame();         // after EndFrame: take the readback and encode it
    ProcessManager*        m_processManager = nullptr;

    bool                   m_capturePending = false;     // Capture Frame requested, result not in yet
    double                 m_captureRequestTime = 0.0;   // glfwGetTime() of the request

    void ProcessPendingActions();

    // Debug view of an object clicked in the viewport: a segmented object
    // (group + object index) or, in an unsegmented scene, a single mesh.
    GroupMeshData* m_selGroup = nullptr;
    int32_t        m_selObject = -1;
    bool           m_objectMenuClick = false; // RMB click, cancelled by drag/navigation
    MeshData*      m_selMesh = nullptr;
    void UpdateSelection();                   // clicks, validity, UI requests, overlay
    void PickAt(float mouseX, float mouseY);  // ray-cast the visible meshes
    void ClearSelection();
    void DeleteSelection();   // removes the selected mesh / object's meshes from the scene
    bool SelectionBounds(core::bounds3d& box, int* meshes = nullptr, size_t* triangles = nullptr) const;
    void DrawSelectionOverlay();
    struct OverlayView;                       // scene -> viewport pixels for ImGui overlays
    void DrawCaptureOverlay();                // debug: each capture's camera, view ray, footprint, path
    void FrameBounds(const core::bounds3d& bbox);   // point the planar camera at bbox
    void UploadGroupTextures(GroupMeshData* group); // GPU-upload a captured group's textures
    void RecomputeWorldBounds();
    void ReleaseGroup(GroupMeshData* group);         // free GPU data and delete (GPU must be idle)
    void ClearScene();
    void OpenScene(const std::string& path, bool keepCamera = false);         // replaces the scene with a .mtscene file
    void SaveSceneTo(const std::string& path);
    void LoadConfig();
    void SaveConfig();
};
