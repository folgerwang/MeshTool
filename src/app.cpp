#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app.h"

#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <set>

#include <vulkan/vulkan.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "base.h"
#include "worlddata.h"
#include "meshdata.h"
#include "coretexture.h"
#include "scenefile.h"
#include "geoview.h"
#include <GeographicLib/LocalCartesian.hpp>
#include "coregeographic.h"
#include "viewcamera.h"
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
    m_camera = new ViewCamera();
    {
        // Start framed on a unit box at the origin; with zero distance the eye
        // would sit on the focus point and the view matrix would be degenerate.
        core::bounds3d unitBox;
        unitBox += core::vec3d(-1.0, -1.0, -1.0);
        unitBox += core::vec3d(1.0, 1.0, 1.0);
        FrameBounds(unitBox);
    }

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

    // ---- GE view link (GPS) ----
    m_geoServer = std::make_unique<GeoViewServer>();
    if (m_geoServer->Start())
        m_ui->geoViewServer = m_geoServer.get();
    else
        std::cerr << "Could not start the Google Earth view link; captures will not be georeferenced.\n";
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

        const double now = glfwGetTime();
        const double dt = (std::min)(now - m_lastFrameTime, 0.1);
        m_lastFrameTime = now;

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

        // Camera input after the UI so ImGui's mouse capture is current.
        ViewCamera::Rect vpRect;
        vpRect.x = m_ui->viewportX; vpRect.y = m_ui->viewportY;
        vpRect.w = m_ui->viewportW; vpRect.h = m_ui->viewportH;
        m_camera->Update(m_window, m_input->GetState().scrollDelta, vpRect,
                         ImGui::GetIO().WantCaptureMouse, dt);
        UpdateGeoReadout();

        ImGui::Render();

        // Draw 3D scene
        // Draw 3D only inside the UI's viewport rect (between toolbar, scene
        // panel and status bar), converted from ImGui to framebuffer pixels.
        auto& ctx = m_renderer->GetContext();
        const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
        const float fbW = (float)ctx.swapchainExtent.width;
        const float fbH = (float)ctx.swapchainExtent.height;
        float vx0 = std::clamp(m_ui->viewportX * fbScale.x, 0.0f, fbW);
        float vy0 = std::clamp(m_ui->viewportY * fbScale.y, 0.0f, fbH);
        float vx1 = std::clamp((m_ui->viewportX + m_ui->viewportW) * fbScale.x, vx0 + 1.0f, fbW);
        float vy1 = std::clamp((m_ui->viewportY + m_ui->viewportH) * fbScale.y, vy0 + 1.0f, fbH);
        if (vx1 <= vx0 || vy1 <= vy0) { vx0 = 0; vy0 = 0; vx1 = fbW; vy1 = fbH; }

        VkViewport viewport = {};
        viewport.x = vx0;
        viewport.y = vy0;
        viewport.width = vx1 - vx0;
        viewport.height = vy1 - vy0;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.offset = { int32_t(vx0), int32_t(vy0) };
        scissor.extent = { uint32_t(viewport.width), uint32_t(viewport.height) };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Camera-relative rendering: meshes are offset by -eye in double, so the
        // view matrix is a pure rotation and large world coordinates stay precise.
        float aspectX = viewport.width / std::max(viewport.height, 1.0f);
        float viewProj[16];
        m_camera->BuildViewProj(aspectX, viewProj);

        const core::vec3d eyePos = m_camera->Eye();
        MeshDrawFrame drawFrame;
        drawFrame.refPos[0] = eyePos.x;
        drawFrame.refPos[1] = eyePos.y;
        drawFrame.refPos[2] = eyePos.z;
        drawFrame.scale = 1.0f;
        m_meshRenderer->DrawBatchMeshes(cmd, g_world.mesh_data_batches, viewProj, drawFrame, true);

        // Render ImGui draw data
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        m_renderer->EndFrame();
    }

    vkDeviceWaitIdle(m_renderer->GetContext().device);
}

// ---------------------------------------------------------------------------
// Process UI-requested actions
// ---------------------------------------------------------------------------

void MeshToolApp::UpdateGeoReadout()
{
    // The scene's georeferenced batches share East/North/Up metres at their
    // reference point; convert the camera pivot back to WGS84.
    const BatchMeshData* geo = nullptr;
    for (const BatchMeshData* b : g_world.mesh_data_batches)
        if (b && b->is_georeferenced && !b->group_meshes.empty()) { geo = b; break; }

    std::string linkState;
    if (m_geoServer)
        linkState = m_geoServer->Latest().valid ? "   |   GE view link: receiving" : "   |   GE view link: waiting";

    if (!geo)
    {
        m_ui->geoText = linkState.empty() ? std::string() : "Not georeferenced" + linkState;
        return;
    }
    GeographicLib::LocalCartesian local(geo->reference_pos.y, geo->reference_pos.x, 0.0);
    double lat, lon, h;
    local.Reverse(m_camera->target.x, m_camera->target.y, m_camera->target.z, lat, lon, h);
    char buf[160];
    snprintf(buf, sizeof(buf), "Pivot  %.7f, %.7f   %.1f m", lat, lon, h);
    m_ui->geoText = std::string(buf) + linkState;
}

void MeshToolApp::DiscardOlderCaptures(BatchMeshData* batch)
{
    if (!batch || batch->group_meshes.size() < 2)
        return;

    // Earlier frames may still be in flight on the GPU.
    vkDeviceWaitIdle(m_renderer->GetContext().device);

    GroupMeshData* latest = batch->group_meshes.back();
    for (size_t g = 0; g + 1 < batch->group_meshes.size(); g++)
        ReleaseGroup(batch->group_meshes[g]);
    batch->group_meshes.assign(1, latest);
}

void MeshToolApp::ReleaseGroup(GroupMeshData* group)
{
    // Caller has waited for the GPU to go idle.
    if (!group)
        return;
    for (MeshData* mesh : group->meshes)
    {
        if (!mesh) continue;
        m_meshRenderer->ReleaseMesh(mesh);
        delete mesh;
    }
    for (core::Texture2DInfo* tex : group->loaded_textures)
    {
        auto it = m_texHandles.find(tex);
        if (it != m_texHandles.end())
        {
            m_texManager->ReleaseTexture(it->second);
            m_texHandles.erase(it);
        }
        delete tex;   // captured / loaded textures are owned by their group
    }
    delete group;
}

void MeshToolApp::ClearScene()
{
    vkDeviceWaitIdle(m_renderer->GetContext().device);
    BatchMeshData* live = m_ui ? m_ui->liveBatch : nullptr;
    if (m_ui && m_ui->captureProcessor)
        m_ui->captureProcessor->ResetMerge();   // the group it merges into is about to go
    for (BatchMeshData* batch : g_world.mesh_data_batches)
    {
        if (!batch) continue;
        for (GroupMeshData* group : batch->group_meshes)
            ReleaseGroup(group);
        batch->group_meshes.clear();
        batch->bbox_ws.Reset();
        batch->bbox_gps.Reset();
        if (batch != live)   // a running capture keeps writing into its batch
            delete batch;
    }
    g_world.mesh_data_batches.clear();
    RecomputeWorldBounds();
}

void MeshToolApp::OpenScene(const std::string& path)
{
    std::vector<BatchMeshData*> loaded;
    std::string error;
    if (!LoadScene(path, loaded, error))
    {
        m_ui->statusMessage = "Open failed: " + error;
        m_ui->statusTimeout = 10.0f;
        return;
    }

    ClearScene();
    size_t meshes = 0;
    for (BatchMeshData* batch : loaded)
    {
        for (GroupMeshData* group : batch->group_meshes)
        {
            UploadGroupTextures(group);
            meshes += group->meshes.size();
        }
        g_world.mesh_data_batches.push_back(batch);
    }
    RecomputeWorldBounds();
    FrameBounds(g_world.bbox_ws);

    m_ui->statusMessage = "Opened " + path + " (" + std::to_string(meshes) + " meshes).";
    m_ui->statusTimeout = 6.0f;
}

void MeshToolApp::SaveSceneTo(const std::string& path)
{
    std::string error;
    if (SaveScene(path, g_world.mesh_data_batches, error))
        m_ui->statusMessage = "Saved " + path;
    else
        m_ui->statusMessage = "Save failed: " + error;
    m_ui->statusTimeout = 6.0f;
}

void MeshToolApp::RecomputeWorldBounds()
{
    g_world.bbox_ws.Reset();
    g_world.bbox_gps.Reset();
    for (const BatchMeshData* b : g_world.mesh_data_batches)
    {
        if (!b) continue;
        if (b->bbox_ws.b_valid)  g_world.bbox_ws += b->bbox_ws;
        if (b->bbox_gps.b_valid) g_world.bbox_gps += b->bbox_gps;
    }
}

void MeshToolApp::UploadGroupTextures(GroupMeshData* group)
{
    if (!group)
        return;

    // Upload each texture once (a merged group grows, so most may already be
    // on the GPU), then point meshes at the GPU handle.
    std::vector<uint32_t> handles(group->loaded_textures.size(), UINT32_MAX);
    for (size_t i = 0; i < group->loaded_textures.size(); i++)
    {
        const core::Texture2DInfo* info = group->loaded_textures[i];
        auto known = m_texHandles.find(info);
        if (known != m_texHandles.end())
        {
            handles[i] = known->second;
            continue;
        }
        if (!info || !info->m_levelCount || !info->m_mips[0].m_imageData)
            continue;

        // UploadTexture treats any format it doesn't know as RGBA8; skip data
        // that can't be that, rather than upload a mis-sized image.
        const uint32_t fmt = info->m_internalFormat;
        const bool known_compressed = (fmt >= 0x83F0 && fmt <= 0x83F3) || fmt == 0x9274;  // S3TC DXT1-5, ETC2 RGB8
        const uint64_t rgba_size = uint64_t(info->m_mips[0].m_width) * info->m_mips[0].m_height * 4;
        if (!known_compressed && uint64_t(info->m_mips[0].m_size) < rgba_size &&
            fmt != 0x8051 /* GL_RGB8, expanded on upload */)
            continue;

        try
        {
            handles[i] = m_texManager->UploadTexture(info);
            if (handles[i] != UINT32_MAX)
                m_texHandles[info] = handles[i];
        }
        catch (const std::exception& e) { std::cerr << "Texture upload failed: " << e.what() << "\n"; }
    }

    for (MeshData* mesh : group->meshes)
        if (mesh && mesh->idx_in_texture_list < handles.size())
            mesh->tex_id = handles[mesh->idx_in_texture_list];
}

void MeshToolApp::FrameBounds(const core::bounds3d& bbox)
{
    if (!bbox.b_valid)
        return;
    float aspectX = 16.0f / 9.0f;
    if (m_ui && m_ui->viewportH > 1.0f)
        aspectX = m_ui->viewportW / m_ui->viewportH;
    m_camera->Frame(bbox, aspectX);
}

void MeshToolApp::ProcessPendingActions()
{
    if (!m_ui) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput)
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
            m_ui->ActionOpenScene();
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            m_ui->ActionSaveScene();
        else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
            m_ui->wantFrameAll = true;
    }

    if (!m_ui->pendingOpenScenePath.empty())
    {
        std::string path = std::move(m_ui->pendingOpenScenePath);
        m_ui->pendingOpenScenePath.clear();
        OpenScene(path);
    }
    if (!m_ui->pendingSaveScenePath.empty())
    {
        std::string path = std::move(m_ui->pendingSaveScenePath);
        m_ui->pendingSaveScenePath.clear();
        SaveSceneTo(path);
    }

    if (m_ui->wantFrameAll)
    {
        m_ui->wantFrameAll = false;
        core::bounds3d all;
        for (const auto* batch : g_world.mesh_data_batches)
            if (batch)
                for (const auto* group : batch->group_meshes)
                    if (group)
                        all += group->bbox_ws;
        FrameBounds(all);
    }

    if (m_ui->wantCaptureFrame)
    {
        m_ui->wantCaptureFrame = false;
        if (m_processManager && m_processManager->IsRunning())
        {
            // Non-blocking: the result is picked up below when the hook signals it.
            m_processManager->RequestFrameCapture();
            m_capturePending = true;
            m_captureRequestTime = glfwGetTime();
            m_ui->statusMessage = "Capturing frame...";
            m_ui->statusTimeout = 10.0f;
        }
    }

    // A finished capture, requested here or with F12 inside Google Earth.
    HANDLE readyEvent = m_processManager ? m_processManager->GetReadyEvent() : nullptr;
    if (readyEvent && m_ui->captureProcessor && WaitForSingleObject(readyEvent, 0) == WAIT_OBJECT_0)
    {
        m_capturePending = false;

        BatchMeshData* batch = m_ui->liveBatch;
        if (m_geoServer)
            m_ui->captureProcessor->SetGeoView(m_geoServer->Latest());
        m_ui->captureProcessor->processFrame();
        const LiveCaptureProcessor::FrameResult& result = m_ui->captureProcessor->LastResult();

        // Meshes the LOD filter emptied or trimmed may be on the GPU already.
        if (!result.removed.empty() || !result.modified.empty())
        {
            vkDeviceWaitIdle(m_renderer->GetContext().device);
            for (MeshData* mesh : result.removed)
            {
                m_meshRenderer->ReleaseMesh(mesh);
                delete mesh;
            }
            for (MeshData* mesh : result.modified)
                m_meshRenderer->ReleaseMesh(mesh);   // re-uploaded on next draw
        }

        // Show the capture right away, centred in the viewport. A capture next
        // to the previous one was merged into it; any other replaces it.
        if (batch && result.new_data && !batch->group_meshes.empty())
        {
            auto& batches = g_world.mesh_data_batches;
            if (std::find(batches.begin(), batches.end(), batch) == batches.end())
                batches.push_back(batch);

            if (!result.merged)
                DiscardOlderCaptures(batch);
            UploadGroupTextures(batch->group_meshes.back());

            batch->bbox_ws = batch->group_meshes.back()->bbox_ws;
            batch->bbox_gps = batch->group_meshes.back()->bbox_gps;
            RecomputeWorldBounds();
            FrameBounds(batch->bbox_ws);

            m_ui->statusMessage = std::string(result.merged ? "Merged capture into the neighbouring one: " : "Captured: ") +
                                  std::to_string(batch->group_meshes.back()->meshes.size()) + " meshes.";
            m_ui->statusTimeout = 6.0f;
        }
    }
    else if (m_capturePending && glfwGetTime() - m_captureRequestTime > 10.0)
    {
        m_capturePending = false;
        m_ui->statusMessage = "Timed out waiting for frame capture.";
        m_ui->statusTimeout = 5.0f;
    }

    if (m_ui->wantStopCapture)
    {
        m_ui->wantStopCapture = false;
        if (m_processManager)
        {
            m_processManager->StopGoogleEarth();
            BatchMeshData* batch = m_ui->liveBatch;
            auto& batches = g_world.mesh_data_batches;
            if (batch && std::find(batches.begin(), batches.end(), batch) != batches.end())
            {
                // Already in the scene since its first capture; g_world owns it now.
                m_ui->liveBatch = nullptr;
            }
            else if (batch && !batch->group_meshes.empty())
            {
                batches.push_back(batch);
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
