#pragma once

#include <GLFW/glfw3.h>

class VulkanRenderer;
class VulkanMeshRenderer;
class VulkanTextureManager;
class VulkanPipelineManager;
class MeshToolUI;
class InputHandler;
struct CameraController;
class ProcessManager;
class LiveCaptureProcessor;

class MeshToolApp {
public:
    MeshToolApp();
    ~MeshToolApp();

    bool Init();
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
    CameraController*      m_camera       = nullptr;
    ProcessManager*        m_processManager = nullptr;

    void ProcessPendingActions();
    void LoadConfig();
    void SaveConfig();
};
