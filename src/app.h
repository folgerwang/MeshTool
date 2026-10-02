#pragma once

#include <GLFW/glfw3.h>
#include <string>
#include <map>
#include <memory>
#include <cstdint>
#include "coremath.h"

class VulkanRenderer;
class VulkanMeshRenderer;
class VulkanTextureManager;
class VulkanPipelineManager;
class MeshToolUI;
class InputHandler;
class ViewCamera;
class ProcessManager;
class LiveCaptureProcessor;
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
    };
    void SetAutoShots(const AutoShots& shots) { m_auto = shots; m_autoStage = 1; }
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
    double      m_followMoveTime = 0.0;             // glfwGetTime() of the last camera change
    bool        m_followSent = false;
    bool        m_followWasOn = false;
    ProcessManager*        m_processManager = nullptr;

    bool                   m_capturePending = false;     // Capture Frame requested, result not in yet
    double                 m_captureRequestTime = 0.0;   // glfwGetTime() of the request

    void ProcessPendingActions();

    // Debug view of an object clicked in the viewport: a segmented object
    // (group + object index) or, in an unsegmented scene, a single mesh.
    GroupMeshData* m_selGroup = nullptr;
    int32_t        m_selObject = -1;
    MeshData*      m_selMesh = nullptr;
    void UpdateSelection();                   // clicks, validity, UI requests, overlay
    void PickAt(float mouseX, float mouseY);  // ray-cast the visible meshes
    void ClearSelection();
    bool SelectionBounds(core::bounds3d& box, int* meshes = nullptr, size_t* triangles = nullptr) const;
    void DrawSelectionOverlay();
    struct OverlayView;                       // scene -> viewport pixels for ImGui overlays
    void DrawCaptureOverlay();                // debug: each capture's camera, view ray, footprint, path
    void FrameBounds(const core::bounds3d& bbox);   // point the planar camera at bbox
    void UploadGroupTextures(GroupMeshData* group); // GPU-upload a captured group's textures
    void RecomputeWorldBounds();
    void ReleaseGroup(GroupMeshData* group);         // free GPU data and delete (GPU must be idle)
    void ClearScene();
    void OpenScene(const std::string& path);         // replaces the scene with a .mtscene file
    void SaveSceneTo(const std::string& path);
    void LoadConfig();
    void SaveConfig();
};
