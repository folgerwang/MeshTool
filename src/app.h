#pragma once

#include <GLFW/glfw3.h>
#include <string>
#include <map>
#include <memory>
#include <cstdint>

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

    void UpdateGeoReadout();   // GPS of the camera pivot -> m_ui->geoText
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
    void FrameBounds(const core::bounds3d& bbox);   // point the planar camera at bbox
    void UploadGroupTextures(GroupMeshData* group); // GPU-upload a captured group's textures
    void DiscardOlderCaptures(BatchMeshData* batch); // keep only the newest captured group
    void RecomputeWorldBounds();
    void ReleaseGroup(GroupMeshData* group);         // free GPU data and delete (GPU must be idle)
    void ClearScene();
    void OpenScene(const std::string& path);         // replaces the scene with a .mtscene file
    void SaveSceneTo(const std::string& path);
    void LoadConfig();
    void SaveConfig();
};
