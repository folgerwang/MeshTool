#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app.h"

#include <iostream>
#include <fstream>
#include <cstring>

#include <vulkan/vulkan.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "base.h"
#include "worlddata.h"
#include "coregeographic.h"
#include "oglcamera.h"
#include "GpaDumpAnalyzeTool.h"
#include "livecaptureprocessor.h"

#include "vk_renderer.h"
#include "vk_buffer.h"
#include "vk_texture_manager.h"
#include "vk_pipeline.h"
#include "vk_mesh_renderer.h"

#include "ui.h"
#include "input.h"

WorldData g_world;

MeshToolApp::MeshToolApp()  = default;
MeshToolApp::~MeshToolApp() = default;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool MeshToolApp::Init()
{
    // ---- GLFW ----
    if (!glfwInit())
    {
        std::cerr << "Failed to initialise GLFW.\n";
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    m_window = glfwCreateWindow(1600, 900, "MeshTool", nullptr, nullptr);
    if (!m_window)
    {
        std::cerr << "Failed to create GLFW window.\n";
        glfwTerminate();
        return false;
    }

    // ---- Vulkan renderer ----
    m_renderer = new VulkanRenderer();
    if (!m_renderer->Init(m_window))
    {
        std::cerr << "Failed to initialise Vulkan renderer.\n";
        delete m_renderer; m_renderer = nullptr;
        glfwDestroyWindow(m_window);
        glfwTerminate();
        return false;
    }

    auto& ctx = m_renderer->GetContext();

    // ---- Texture manager ----
    m_texManager = new VulkanTextureManager();
    // Create descriptor set layout for textures (1 combined image sampler)
    VkDescriptorSetLayoutBinding samplerBinding = {};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI = {};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &samplerBinding;

    VkDescriptorSetLayout texLayout;
    vkCreateDescriptorSetLayout(ctx.device, &layoutCI, nullptr, &texLayout);

    m_texManager->Init(&ctx, texLayout);

    // ---- Pipeline manager ----
    m_pipeManager = new VulkanPipelineManager();
    m_pipeManager->Init(&ctx, texLayout);

    // ---- Mesh renderer ----
    m_meshRenderer = new VulkanMeshRenderer();
    m_meshRenderer->Init(&ctx, m_texManager, m_pipeManager);

    // ---- Camera ----
    m_camera = new CameraController();

    // ---- Input (must init BEFORE ImGui so ImGui chains our callbacks) ----
    m_input = new InputHandler();
    m_input->Init(m_window);

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load font at native large size (NOT FontGlobalScale which causes blurry scaling)
    io.Fonts->AddFontDefault();  // keep default as fallback
    ImFontConfig fontCfg;
    fontCfg.SizePixels = 22.0f;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    io.FontDefault = io.Fonts->AddFontDefault(&fontCfg);

    ImGui::StyleColorsDark();

    // install_callbacks=true: ImGui saves our callbacks and chains to them
    ImGui_ImplGlfw_InitForVulkan(m_window, true);

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = ctx.instance;
    initInfo.PhysicalDevice = ctx.physicalDevice;
    initInfo.Device = ctx.device;
    initInfo.QueueFamily = ctx.graphicsFamily;
    initInfo.Queue = ctx.graphicsQueue;
    initInfo.DescriptorPool = ctx.descriptorPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = (uint32_t)ctx.swapchainImages.size();
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.RenderPass = ctx.renderPass;
    initInfo.Subpass = 0;

    ImGui_ImplVulkan_Init(&initInfo);

    ImGui_ImplVulkan_CreateFontsTexture();

    // ---- Process manager ----
    m_processManager = new ProcessManager();

    // ---- UI ----
    m_ui = new MeshToolUI();
    m_ui->processManager = m_processManager;
    m_ui->Init(m_texManager);

    // ---- Load config & init earth mesh ----
    LoadConfig();
    earth_mesh_init();

    std::cout << "MeshTool initialised successfully.\n";
    return true;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void MeshToolApp::MainLoop()
{
    while (!glfwWindowShouldClose(m_window))
    {
        glfwPollEvents();

        // Update input -> camera
        m_input->Update(m_camera);

        const InputState& st = m_input->GetState();
        if (st.windowWidth > 0 && st.windowHeight > 0)
        {
            float aspectX = float(st.windowWidth) / float(st.windowHeight);
            float tanFovY = tanf(3.14159265f / 8.0f);
            m_camera->UpdateOnPlanar(tanFovY, aspectX, st.windowWidth, st.windowHeight);
        }

        // Begin Vulkan frame
        if (!m_renderer->BeginFrame())
        {
            // Swapchain out of date, recreate
            int w, h;
            glfwGetFramebufferSize(m_window, &w, &h);
            if (w > 0 && h > 0)
                m_renderer->RecreateSwapchain(w, h);
            continue;
        }

        VkCommandBuffer cmd = m_renderer->GetCurrentCommandBuffer();

        // ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_ui->DrawUI();
        ProcessPendingActions();

        ImGui::Render();

        // Draw 3D scene
        auto& ctx = m_renderer->GetContext();
        VkViewport viewport = {};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = (float)ctx.swapchainExtent.width;
        viewport.height = (float)ctx.swapchainExtent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.offset = {0, 0};
        scissor.extent = ctx.swapchainExtent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Build view-proj matrix from camera (same logic as oglwidget.cpp paintGL)
        core::matrix4f projMat, viewMat;
        float fovY = 3.14159265f / 4.0f; // 45 degrees
        float aspectX = viewport.width / std::max(viewport.height, 1.0f);
        float farClip = 100.0f;

        core::vec3d eyePos = m_camera->cur_camera_info.focus_point +
            core::vec3d(0, 0, m_camera->cur_camera_info.dist_to_focus_point);
        auto refPosWs = eyePos;
        float scale = std::min(farClip / float(length(eyePos - m_camera->cur_camera_info.focus_point)) * 0.5f, 1.0f);
        if (scale != scale) scale = 1.0f; // NaN guard

        core::perspective(projMat, fovY, aspectX, 0.05f, farClip);
        auto eyeLocal = eyePos - refPosWs;
        auto lookAtLocal = m_camera->cur_camera_info.focus_point - refPosWs;
        core::lookAt(viewMat, core::vec3f(eyeLocal) * scale, core::vec3f(lookAtLocal) * scale, core::vec3f(0, 1.0f, 0));

        core::matrix4f worldScreenMat = projMat * viewMat;
        const float* viewProj = worldScreenMat.get_value();

        m_meshRenderer->DrawBatchMeshes(cmd, g_world.mesh_data_batches, viewProj, true);

        // Render ImGui draw data
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        m_renderer->EndFrame();
    }

    vkDeviceWaitIdle(m_renderer->GetContext().device);
}

// ---------------------------------------------------------------------------
// Process UI-requested actions
// ---------------------------------------------------------------------------

void MeshToolApp::ProcessPendingActions()
{
    if (!m_ui) return;

    if (m_ui->wantCaptureFrame)
    {
        m_ui->wantCaptureFrame = false;
        if (m_processManager && m_processManager->IsRunning())
        {
            m_processManager->RequestFrameCapture();
            HANDLE readyEvent = m_processManager->GetReadyEvent();
            if (readyEvent)
            {
                DWORD result = WaitForSingleObject(readyEvent, 10000);
                if (result == WAIT_OBJECT_0)
                {
                    if (m_ui->captureProcessor)
                        m_ui->captureProcessor->processFrame();
                }
                else
                {
                    m_ui->statusMessage = "Timed out waiting for frame capture.";
                    m_ui->statusTimeout = 5.0f;
                }
            }
        }
    }

    if (m_ui->wantStopCapture)
    {
        m_ui->wantStopCapture = false;
        if (m_processManager)
        {
            m_processManager->StopGoogleEarth();
            BatchMeshData* batch = m_ui->liveBatch;
            if (batch && !batch->group_meshes.empty())
            {
                g_world.mesh_data_batches.push_back(batch);
                g_world.bbox_ws += batch->bbox_ws;
                g_world.bbox_gps += batch->bbox_gps;
                m_ui->liveBatch = nullptr;
            }
            else
            {
                delete batch;
                m_ui->liveBatch = nullptr;
            }
            delete m_ui->captureProcessor;
            m_ui->captureProcessor = nullptr;
            m_processManager->DestroySharedMemory();
            m_ui->statusMessage = "Google Earth Pro stopped.";
            m_ui->statusTimeout = 5.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void MeshToolApp::LoadConfig()
{
    std::ifstream inFile("mesh_tool.cfg", std::ifstream::binary);
    if (!inFile) return;
    inFile.seekg(0, inFile.end);
    auto length = inFile.tellg();
    inFile.seekg(0, inFile.beg);
    if (length > 8)
    {
        inFile.read(reinterpret_cast<char*>(&g_world.reference_pos), sizeof(core::vec2d));
        inFile.read(reinterpret_cast<char*>(&g_world.scissor_bbox), sizeof(core::bounds2d));
    }
    inFile.close();
}

void MeshToolApp::SaveConfig()
{
    std::ofstream outFile("mesh_tool.cfg", std::ofstream::binary);
    if (!outFile) return;
    outFile.write(reinterpret_cast<char*>(&g_world.reference_pos), sizeof(core::vec2d));
    outFile.write(reinterpret_cast<char*>(&g_world.scissor_bbox), sizeof(core::bounds2d));
    outFile.close();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void MeshToolApp::Shutdown()
{
    SaveConfig();

    if (m_renderer)
        vkDeviceWaitIdle(m_renderer->GetContext().device);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (m_meshRenderer) { m_meshRenderer->Shutdown(); delete m_meshRenderer; m_meshRenderer = nullptr; }
    if (m_pipeManager)  { m_pipeManager->Shutdown();  delete m_pipeManager;  m_pipeManager = nullptr; }
    if (m_texManager)   { m_texManager->Shutdown();    delete m_texManager;   m_texManager = nullptr; }
    if (m_renderer)     { m_renderer->Shutdown();      delete m_renderer;     m_renderer = nullptr; }

    delete m_ui;              m_ui = nullptr;
    delete m_input;           m_input = nullptr;
    delete m_camera;          m_camera = nullptr;
    delete m_processManager;  m_processManager = nullptr;

    if (m_window)
    {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}
