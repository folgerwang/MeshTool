#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app.h"

#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <set>
#include <cmath>
#include <limits>
#include <ctime>
#include <filesystem>

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
#include "segmenter.h"
#include "refiner.h"
#include "qwen_client.h"   // WritePngRgb
#include "objectclass.h"
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
    m_pipeManager->Init(&ctx, m_texManager->GetBindlessLayout());

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
        UpdateGeFollow();
        UpdateSelection();
        DrawCaptureOverlay();

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
        // Check screenshots: one frame per colour mode, viewport only.
        UpdateAutoShots();
        if (m_ui->wantCheckScreenshots)
        {
            m_ui->wantCheckScreenshots = false;
            StartCheckScreenshots();
        }
        if (m_shotStage > 0 && !m_shotPending)
        {
            m_shotRect[0] = int(vx0); m_shotRect[1] = int(vy0);
            m_shotRect[2] = int(vx1 - vx0); m_shotRect[3] = int(vy1 - vy0);
            m_renderer->RequestReadback();
            m_shotPending = true;
        }
        drawFrame.classColors = m_shotStage == 1 ? true : m_shotStage == 2 ? false : m_ui->classColors;
        drawFrame.buildingColors = m_ui->buildingColors;
        drawFrame.captureColors = m_ui->debugCaptures;
        drawFrame.selGroup = m_selGroup;
        drawFrame.selObject = m_selObject;
        drawFrame.selMesh = m_selMesh;
        drawFrame.isolateSelection = m_ui->isolateSelection;
        drawFrame.selActual = m_ui->selectionView == MeshToolUI::kSelActual;
        {
            // Pulse between yellow and white so the selection stands out in any colour mode.
            float k = 0.5f + 0.5f * float(sin(glfwGetTime() * 5.0));
            drawFrame.selColor[0] = 1.0f;
            drawFrame.selColor[1] = 0.80f + 0.20f * k;
            drawFrame.selColor[2] = 0.05f + 0.75f * k;
        }
        drawFrame.classVisible = m_ui->classVisible;
        drawFrame.glass = m_ui->glass;
        drawFrame.glassOpacity = m_ui->glassOpacity;
        drawFrame.glassReflect = m_ui->glassReflect;
        m_meshRenderer->DrawBatchMeshes(cmd, g_world.mesh_data_batches, viewProj, drawFrame, true);

        // Render ImGui draw data - not on check-screenshot frames: dialogs,
        // their dimming and overlays would cover the scene being checked.
        if (!m_shotPending)
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        m_renderer->EndFrame();

        if (m_shotPending)
        {
            m_shotPending = false;
            std::vector<uint8_t> rgb;
            int fw = 0, fh = 0;
            if (m_renderer->TakeReadback(rgb, fw, fh))
            {
                int x0 = std::clamp(m_shotRect[0], 0, fw - 1), y0 = std::clamp(m_shotRect[1], 0, fh - 1);
                int w = std::clamp(m_shotRect[2], 1, fw - x0), h = std::clamp(m_shotRect[3], 1, fh - y0);
                std::vector<uint8_t> crop(size_t(w) * h * 3);
                for (int y = 0; y < h; y++)
                    memcpy(&crop[size_t(y) * w * 3], &rgb[(size_t(y0 + y) * fw + x0) * 3], size_t(w) * 3);
                std::string path = m_shotBase + (m_shotStage == 1 ? "_segment.png" : "_original.png");
                if (!WritePngRgb(path, w, h, crop.data()))
                {
                    m_ui->statusMessage = "Could not write " + path;
                    m_ui->statusTimeout = 8.0f;
                    m_shotStage = 0;
                }
                else if (m_shotStage == 1)
                    m_shotStage = 2;
                else
                {
                    m_shotStage = 0;
                    // Keep the segmentation summary when it is still showing.
                    std::string saved = "check screenshots: " + m_shotBase + "_segment.png / _original.png";
                    m_ui->statusMessage = m_ui->statusTimeout > 0.0f && !m_ui->statusMessage.empty()
                                              ? m_ui->statusMessage + "  |  " + saved : saved;
                    m_ui->statusTimeout = 15.0f;
                }
            }
            else
            {
                m_shotStage = 0;
                m_ui->statusMessage = "Screenshots are not supported by this display driver.";
                m_ui->statusTimeout = 8.0f;
            }
        }
    }

    vkDeviceWaitIdle(m_renderer->GetContext().device);
}

void MeshToolApp::StartCheckScreenshots(const std::string& base)
{
    if (m_shotStage > 0)
        return;
    if (!m_renderer->ReadbackSupported())
    {
        m_ui->statusMessage = "Screenshots are not supported by this display driver.";
        m_ui->statusTimeout = 8.0f;
        return;
    }
    // .\screenshots next to mesh_tool.cfg (run.bat starts MeshTool there).
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::absolute("screenshots", ec);
    std::filesystem::create_directories(dir, ec);
    char stamp[32];
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &lt);
    m_shotBase = base.empty() ? (dir / (std::string("check_") + stamp)).string() : base;
    m_shotStage = 1;
}

void MeshToolApp::UpdateAutoShots()
{
    if (m_autoStage == 0)
        return;
    m_autoFrames++;
    if (m_autoStage == 1)
    {
        // Scene open (first frames) and drawn once.
        if (g_world.mesh_data_batches.empty() || m_autoFrames < 3)
        {
            if (m_autoFrames > 600)
            {
                fprintf(stderr, "--shots: no scene loaded\n");
                glfwSetWindowShouldClose(m_window, GLFW_TRUE);
                m_autoStage = 0;
            }
            return;
        }
        core::bounds3d box;
        if (m_auto.hasBox)
        {
            box += core::vec3d(m_auto.box[0], m_auto.box[1], m_auto.box[2]);
            box += core::vec3d(m_auto.box[3], m_auto.box[4], m_auto.box[5]);
        }
        for (const BatchMeshData* b : g_world.mesh_data_batches)
            if (b && !m_auto.hasBox)
                for (const GroupMeshData* g : b->group_meshes)
                {
                    if (!g) continue;
                    if (m_auto.frameObject.empty())
                        box += g->bbox_ws;
                    else
                        for (const SceneObject& o : g->objects)
                            if (o.name == m_auto.frameObject && o.bbox_ws.b_valid)
                                box += o.bbox_ws;
                }
        if (!box.b_valid)
            fprintf(stderr, "--shots: object %s not found, framing everything\n", m_auto.frameObject.c_str());
        if (!box.b_valid)
            for (const BatchMeshData* b : g_world.mesh_data_batches)
                if (b)
                    for (const GroupMeshData* g : b->group_meshes)
                        if (g) box += g->bbox_ws;
        m_camera->yaw = m_auto.yawDeg * 3.14159265358979 / 180.0;
        m_camera->pitch = m_auto.pitchDeg * 3.14159265358979 / 180.0;
        FrameBounds(box);
        m_camera->distance *= m_auto.zoom;
        m_ui->classColors = true;
        m_ui->glass = m_auto.glass;
        m_autoStage = 2;
        m_autoFrames = 0;
    }
    else if (m_autoStage == 2 && m_autoFrames >= 3)
    {
        StartCheckScreenshots(m_auto.prefix);
        m_autoStage = 3;
    }
    else if (m_autoStage == 3 && m_shotStage == 0 && !m_shotPending)
    {
        printf("Saved %s_segment.png / _original.png\n", m_auto.prefix.c_str());
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        m_autoStage = 0;
    }
}

// ---------------------------------------------------------------------------
// Process UI-requested actions
// ---------------------------------------------------------------------------

void MeshToolApp::OpenOnStart(const std::string& path)
{
    if (m_ui)
        m_ui->pendingOpenScenePath = path;   // opened on the first frame
}

const BatchMeshData* MeshToolApp::GeoreferencedBatch() const
{
    // The scene's georeferenced batches share East/North/Up metres at their
    // reference point.
    for (const BatchMeshData* b : g_world.mesh_data_batches)
        if (b && b->is_georeferenced && !b->group_meshes.empty())
            return b;
    return nullptr;
}

void MeshToolApp::UpdateGeoReadout()
{
    // Convert the camera pivot back to WGS84.
    const BatchMeshData* geo = GeoreferencedBatch();

    std::string linkState;
    if (m_geoServer)
        linkState = m_geoServer->Latest().valid ? "   |   GE view link: receiving" : "   |   GE view link: waiting";
    if (m_geoServer && m_ui->geFollowViewport)
        linkState += glfwGetTime() - m_geoServer->LastFollowPoll() < 3.0
                         ? "   |   GE follow: on"
                         : "   |   GE follow: GE not polling (start GE from MeshTool)";

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

void MeshToolApp::UpdateGeFollow()
{
    const bool on = m_ui->geFollowViewport && m_geoServer;
    if (on && !m_followWasOn)
        m_followSent = false;   // just switched on: send the current view
    m_followWasOn = on;
    if (!on)
        return;

    const double now = glfwGetTime();
    const core::vec3d eye = m_camera->Eye(), fwd = m_camera->Forward();
    auto moved = [](const core::vec3d& e0, const core::vec3d& f0, const core::vec3d& e1, const core::vec3d& f1,
                    double metres, double cosAngle) {
        core::vec3d d = e1 - e0;
        return dot(d, d) > metres * metres || dot(f0, f1) < cosAngle;
    };
    if (moved(m_followPrevEye, m_followPrevFwd, eye, fwd, 1e-4, 1.0 - 1e-12))
        m_followMoveTime = now;
    m_followPrevEye = eye;
    m_followPrevFwd = fwd;

    // Like GE's own onStop: send once the camera rests, so GE flies once per
    // move instead of restarting its flight every frame.
    const double kRest = 0.25;   // seconds
    if (now - m_followMoveTime < kRest)
        return;
    if (m_followSent && !moved(m_followSentEye, m_followSentFwd, eye, fwd, 0.05, cos(0.05 * 3.14159265358979323846 / 180.0)))
        return;

    const BatchMeshData* geo = GeoreferencedBatch();
    if (!geo)
        return;   // no GPS frame: nothing to tell GE

    // Eye to WGS84; heading and tilt in the eye's own East/North/Up (the
    // scene's axes drift from it away from the reference point).
    GeographicLib::LocalCartesian scene(geo->reference_pos.y, geo->reference_pos.x, 0.0);
    double lat, lon, h;
    scene.Reverse(eye.x, eye.y, eye.z, lat, lon, h);
    const core::vec3d ahead = eye + fwd * 10.0;
    double lat2, lon2, h2;
    scene.Reverse(ahead.x, ahead.y, ahead.z, lat2, lon2, h2);
    GeographicLib::LocalCartesian at(lat, lon, h);
    double fe, fn, fu;
    at.Forward(lat2, lon2, h2, fe, fn, fu);
    const double kDeg = 180.0 / 3.14159265358979323846;
    const double len = sqrt(fe * fe + fn * fn + fu * fu);
    const double heading = atan2(fe, fn) * kDeg;
    const double tilt = acos((std::max)(-1.0, (std::min)(1.0, -fu / len))) * kDeg;   // 0 = straight down

    m_geoServer->SetFollowCamera(lon, lat, h, heading, tilt);
    char msg[160];
    snprintf(msg, sizeof(msg), "GE follow: camera %.7f, %.7f  %.0f m  heading %.1f  tilt %.1f", lat, lon, h, heading, tilt);
    m_ui->statusMessage = msg;
    m_ui->statusTimeout = 3.0f;
    m_followSentEye = eye;
    m_followSentFwd = fwd;
    m_followSentLat = lat;
    m_followSentLon = lon;
    m_followSentAlt = h;
    m_followSent = true;
}

bool MeshToolApp::CaptureSettled()
{
    m_ui->captureGate.clear();
    if (!m_ui->geFollowViewport || !m_geoServer || !GeoreferencedBatch())
        return true;   // not following (no GPS scene yet: the first capture must be possible)

    // GE needs time to stream the tiles of the new view after it stops.
    const double kSettle = 1.5;      // seconds after GE is at the camera
    const double kNoReport = 4.0;    // GE reports a view only when it moved and stopped
    const double now = glfwGetTime();
    double delivered = 0.0;
    const bool pending = m_geoServer->FollowPending(&delivered);
    if (now - m_followMoveTime < 0.25 || !m_followSent || pending)
    {
        m_ui->captureGate = pending ? "camera not yet picked up by Google Earth" : "viewport camera moving";
        static double lastLog = 0.0;
        if (now - lastLog > 2.0)
        {
            lastLog = now;
            if (FILE* f = fopen("C:\\Users\\Public\\meshtool_capture.log", "a"))
            {
                fprintf(f, "  [gate] now %.2f  moved %.2f s ago  sent %d  pending %d  last GE poll %.2f s ago\n",
                        now, now - m_followMoveTime, int(m_followSent), int(pending),
                        now - m_geoServer->LastFollowPoll());
                fclose(f);
            }
        }
        return false;
    }

    // When did GE's camera stop moving? Each report that differs from the
    // previous one restarts the clock.
    const GeoView view = m_geoServer->Latest();
    auto metresApart = [](double lat0, double lon0, double alt0, double lat1, double lon1, double alt1) {
        GeographicLib::LocalCartesian at(lat0, lon0, alt0);
        double e, n, u;
        at.Forward(lat1, lon1, alt1, e, n, u);
        return sqrt(e * e + n * n + u * u);
    };
    if (view.valid && view.time != m_geCamTime)
    {
        if (m_geCamTime < 0.0 ||
            metresApart(m_geCamLat, m_geCamLon, m_geCamAlt, view.camLat, view.camLon, view.camAlt) > 0.2)
            m_geStillSince = view.time;
        m_geCamLat = view.camLat;
        m_geCamLon = view.camLon;
        m_geCamAlt = view.camAlt;
        m_geCamTime = view.time;
    }

    // GE is at the camera once its camera is the one sent (since it stopped
    // there); if it rests elsewhere (e.g. kept above terrain), once it has
    // rested kNoReport; with no reports at all, kNoReport after delivery.
    double atCamera = -1.0;   // glfwGetTime() GE was known to be at the camera
    if (view.valid &&
        metresApart(m_followSentLat, m_followSentLon, m_followSentAlt, view.camLat, view.camLon, view.camAlt) < 2.0)
        atCamera = (std::max)(m_geStillSince, delivered);
    else if (view.valid && view.time > delivered && m_geStillSince > delivered && now - m_geStillSince >= kNoReport)
        atCamera = m_geStillSince + kNoReport - kSettle;
    else if (!(view.valid && view.time > delivered) && now - delivered >= kNoReport)
        atCamera = delivered + kNoReport - kSettle;

    // Diagnostics (capture log): what the gate sees, every 2 s while waiting.
    static double lastLog = 0.0;
    if (now - lastLog > 2.0 && (atCamera < 0.0 || now - atCamera < kSettle))
    {
        lastLog = now;
        if (FILE* f = fopen("C:\\Users\\Public\\meshtool_capture.log", "a"))
        {
            fprintf(f, "  [gate] now %.2f  moved %.2f s ago  sent %d  delivered %.2f  view valid %d time %.2f  "
                       "cam %.7f, %.7f, %.1f  sent cam %.7f, %.7f, %.1f  atCamera %.2f\n",
                    now, now - m_followMoveTime, int(m_followSent), delivered, int(view.valid), view.time,
                    view.camLat, view.camLon, view.camAlt, m_followSentLat, m_followSentLon, m_followSentAlt, atCamera);
            fclose(f);
        }
    }
    if (atCamera < 0.0)
    {
        m_ui->captureGate = "Google Earth flying to the viewport camera";
        return false;
    }
    if (now - atCamera < kSettle)
    {
        m_ui->captureGate = "Google Earth loading the view";
        return false;
    }
    return true;
}

void MeshToolApp::ReleaseGroup(GroupMeshData* group)
{
    // Caller has waited for the GPU to go idle.
    if (!group)
        return;
    if (group == m_selGroup)
        ClearSelection();
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

void MeshToolApp::UpdateSegmentation()
{
    if (!m_segmenter)
        m_segmenter = std::make_unique<Segmenter>();

    if (m_ui->wantCancelSegment)
    {
        m_ui->wantCancelSegment = false;
        m_segmenter->Cancel();
    }

    if (m_ui->wantStartSegment)
    {
        m_ui->wantStartSegment = false;
        if (m_segmenter->Running())
            return;

        // Segment the batch with the most geometry, plus any batch in the
        // same coordinate frame (same GPS origin); spline batches are not
        // captured surfaces.
        const BatchMeshData* primary = nullptr;
        size_t best = 0;
        for (const BatchMeshData* b : g_world.mesh_data_batches)
        {
            if (!b || b->is_spline_mesh) continue;
            size_t n = 0;
            for (const GroupMeshData* g : b->group_meshes)
                for (const MeshData* m : g->meshes) n += size_t(std::max(m->num_vertex, 0));
            if (n > best) { best = n; primary = b; }
        }
        std::vector<GroupMeshData*> groups;
        std::vector<BatchMeshData*> input;
        for (BatchMeshData* b : g_world.mesh_data_batches)
        {
            if (!b || !primary || b->is_spline_mesh) continue;
            bool same_frame = b == primary ||
                (b->is_georeferenced && primary->is_georeferenced &&
                 b->reference_pos.x == primary->reference_pos.x && b->reference_pos.y == primary->reference_pos.y);
            if (same_frame)
            {
                groups.insert(groups.end(), b->group_meshes.begin(), b->group_meshes.end());
                input.push_back(b);
            }
        }
        if (groups.empty())
        {
            m_ui->statusMessage = "Nothing to segment.";
            m_ui->statusTimeout = 5.0f;
            return;
        }
        // The exact input, for re-running offline:
        //   MeshTool --segment <debugDir>\last_input.mtscene out.mtscene
        {
            std::error_code ec;
            std::filesystem::create_directories(m_ui->segSettings.debugDir, ec);
            std::string err;
            SaveScene((std::filesystem::path(m_ui->segSettings.debugDir) / "last_input.mtscene").string(), input, err);
        }
        m_segmenter->Start(groups, m_ui->segSettings);
    }

    m_ui->segRunning = m_segmenter->Running();
    if (m_ui->segRunning)
    {
        m_ui->segProgress = m_segmenter->Progress();
        m_ui->segStatus = m_segmenter->Status();
        return;
    }

    std::unique_ptr<SegmentResult> result = m_segmenter->TakeResult();
    if (!result)
        return;
    if (!result->ok)
    {
        m_ui->statusMessage = "Segmentation: " + result->error;
        m_ui->statusTimeout = 12.0f;
        return;
    }

    // Split meshes into per-object meshes; the old ones may be on the GPU.
    vkDeviceWaitIdle(m_renderer->GetContext().device);
    ClearSelection();   // object indices and meshes are about to change
    std::vector<MeshData*> replaced = ApplySegmentation(*result);
    for (MeshData* m : replaced)
    {
        m_meshRenderer->ReleaseMesh(m);
        delete m;
    }
    std::set<GroupMeshData*> touched;
    for (const SegmentResult::MeshAssign& a : result->assignments)
        touched.insert(a.group);
    for (GroupMeshData* g : touched)
        UploadGroupTextures(g);   // new meshes reuse the uploaded textures
    // The capture merger tracks tiles by mesh; those meshes are gone.
    if (m_ui->captureProcessor)
        m_ui->captureProcessor->ResetMerge();

    m_ui->classColors = true;
    m_ui->statusMessage = result->summary;
    m_ui->statusTimeout = 15.0f;
    StartCheckScreenshots();
}

void MeshToolApp::UpdateRefine()
{
    if (!m_refiner)
        m_refiner = std::make_unique<BuildingRefiner>();

    if (m_ui->wantCancelRefine)
    {
        m_ui->wantCancelRefine = false;
        m_refiner->Cancel();
    }

    if (m_ui->wantStartRefine)
    {
        m_ui->wantStartRefine = false;
        if (m_refiner->Running() || (m_segmenter && m_segmenter->Running()))
            return;
        RefineSettings settings;
        settings.glass = m_ui->refineGlass;
        settings.clean = m_ui->refineClean;
        settings.cull = m_ui->refineCull;
        settings.workDir = m_ui->segSettings.debugDir;
        std::error_code ec;
        std::filesystem::create_directories(settings.workDir, ec);
        const std::string input = (std::filesystem::path(settings.workDir) / "refine_input.mtscene").string();
        std::string err;
        if (!SaveScene(input, g_world.mesh_data_batches, err))
        {
            m_ui->statusMessage = "Refine Buildings: cannot save the scene for refine.py: " + err;
            m_ui->statusTimeout = 10.0f;
            return;
        }
        m_refiner->Start(input, settings);
    }

    m_ui->refineRunning = m_refiner->Running();
    if (m_ui->refineRunning)
    {
        m_ui->refineProgress = m_refiner->Progress();
        m_ui->refineStatus = m_refiner->Status();
        return;
    }

    std::unique_ptr<RefineResult> result = m_refiner->TakeResult();
    if (!result)
        return;
    if (!result->ok)
    {
        m_ui->statusMessage = "Refine Buildings: " + result->error;
        m_ui->statusTimeout = 15.0f;
        return;
    }
    vkDeviceWaitIdle(m_renderer->GetContext().device);
    ClearSelection();
    OpenScene(result->outputPath, /*keepCamera=*/true);
    m_ui->classColors = false;
    m_ui->statusMessage = result->summary + " - save it with File > Save Scene.";
    m_ui->statusTimeout = 15.0f;
}

void MeshToolApp::OpenScene(const std::string& path, bool keepCamera)
{
    if ((m_segmenter && m_segmenter->Running()) || (m_refiner && m_refiner->Running()))
    {
        m_ui->statusMessage = "Wait for segmentation or refinement to finish (or cancel it) before opening a scene.";
        m_ui->statusTimeout = 6.0f;
        return;
    }
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
    if (!keepCamera)
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
    // Only textures some mesh samples: the LOD filter and duplicate removal
    // drop many captured meshes, and their textures need no GPU memory.
    std::vector<uint32_t> handles(group->loaded_textures.size(), UINT32_MAX);
    std::vector<bool> used(group->loaded_textures.size(), false);
    for (const MeshData* mesh : group->meshes)
        if (mesh && mesh->idx_in_texture_list < used.size())
            used[mesh->idx_in_texture_list] = true;
    for (size_t i = 0; i < group->loaded_textures.size(); i++)
    {
        if (!used[i])
            continue;
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

// ---------------------------------------------------------------------------
// Object selection (click in the viewport) and its debug view
// ---------------------------------------------------------------------------

namespace {

// Ray vs axis-aligned box; true when it enters the box before maxT.
bool RayHitsBox(const core::vec3d& o, const core::vec3d& d, const core::bounds3d& b, double maxT)
{
    double t0 = 0.0, t1 = maxT;
    const double ro[3] = { o.x, o.y, o.z }, rd[3] = { d.x, d.y, d.z };
    const double lo[3] = { b.bb_min.x, b.bb_min.y, b.bb_min.z }, hi[3] = { b.bb_max.x, b.bb_max.y, b.bb_max.z };
    const double margin = 1e-3;
    for (int a = 0; a < 3; a++)
    {
        if (fabs(rd[a]) < 1e-12)
        {
            if (ro[a] < lo[a] - margin || ro[a] > hi[a] + margin) return false;
            continue;
        }
        double inv = 1.0 / rd[a];
        double ta = (lo[a] - margin - ro[a]) * inv, tb = (hi[a] + margin - ro[a]) * inv;
        if (ta > tb) std::swap(ta, tb);
        t0 = (std::max)(t0, ta);
        t1 = (std::min)(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

// Moller-Trumbore, both faces; distance along d, or -1 for a miss.
double RayTriangle(const core::vec3d& o, const core::vec3d& d,
                   const core::vec3d& a, const core::vec3d& b, const core::vec3d& c)
{
    core::vec3d e1 = b - a, e2 = c - a;
    core::vec3d p = cross(d, e2);
    double det = dot(e1, p);
    if (fabs(det) < 1e-12) return -1.0;
    double inv = 1.0 / det;
    core::vec3d s = o - a;
    double u = dot(s, p) * inv;
    if (u < 0.0 || u > 1.0) return -1.0;
    core::vec3d q = cross(s, e1);
    double v = dot(d, q) * inv;
    if (v < 0.0 || u + v > 1.0) return -1.0;
    double t = dot(e2, q) * inv;
    return t > 0.0 ? t : -1.0;
}

ObjectClass MeshClass(const GroupMeshData* group, const MeshData* mesh)
{
    if (mesh->object_id >= 0 && size_t(mesh->object_id) < group->objects.size())
        return group->objects[size_t(mesh->object_id)].cls;
    return kObjUnknown;
}

size_t TriangleCount(const MeshData* mesh)
{
    size_t n = 0;
    for (const DrawCallInfo& dc : mesh->draw_call_list)
    {
        int idx = dc.get_index_count();
        n += size_t(dc.get_primitive_type() == kGlTriangleStrip ? (std::max)(idx - 2, 0) : idx / 3);
    }
    return n;
}

} // namespace

void MeshToolApp::ClearSelection()
{
    m_selGroup = nullptr;
    m_selObject = -1;
    m_selMesh = nullptr;
    if (m_ui)
        m_ui->selection = MeshToolUI::SelectionInfo();
}

bool MeshToolApp::SelectionBounds(core::bounds3d& box, int* meshes, size_t* triangles) const
{
    box.Reset();
    if (!m_selGroup)
        return false;
    int count = 0;
    size_t tris = 0;
    for (const MeshData* mesh : m_selGroup->meshes)
    {
        if (!mesh || (m_selMesh ? mesh != m_selMesh : mesh->object_id != m_selObject))
            continue;
        if (mesh->bbox_ws.b_valid) box += mesh->bbox_ws;
        count++;
        tris += TriangleCount(mesh);
    }
    if (meshes) *meshes = count;
    if (triangles) *triangles = tris;
    return count > 0 && box.b_valid;
}

void MeshToolApp::PickAt(float mouseX, float mouseY)
{
    const double vw = (std::max)(m_ui->viewportW, 1.0f), vh = (std::max)(m_ui->viewportH, 1.0f);
    const double ndcX = 2.0 * (mouseX - m_ui->viewportX) / vw - 1.0;
    const double ndcY = 1.0 - 2.0 * (mouseY - m_ui->viewportY) / vh;   // +1 = top
    const double tanHalf = tan(m_camera->fovY * 0.5);
    const core::vec3d eye = m_camera->Eye();
    core::vec3d dir = m_camera->Forward() + m_camera->Right() * (ndcX * tanHalf * vw / vh) +
                      m_camera->Up() * (ndcY * tanHalf);
    dir = dir * (1.0 / length(dir));

    double best = std::numeric_limits<double>::max();
    BatchMeshData* bestBatch = nullptr;
    GroupMeshData* bestGroup = nullptr;
    MeshData* bestMesh = nullptr;
    for (BatchMeshData* batch : g_world.mesh_data_batches)
    {
        if (!batch) continue;
        for (GroupMeshData* group : batch->group_meshes)
        {
            if (!group) continue;
            for (MeshData* mesh : group->meshes)
            {
                // Only what is drawn can be clicked.
                if (!mesh || !mesh->vertex_list || mesh->num_vertex <= 0) continue;
                if (!m_ui->classVisible[MeshClass(group, mesh)]) continue;
                if (m_ui->isolateSelection && m_selGroup &&
                    !(group == m_selGroup && (m_selMesh ? mesh == m_selMesh : mesh->object_id == m_selObject)))
                    continue;
                if (mesh->bbox_ws.b_valid && !RayHitsBox(eye, dir, mesh->bbox_ws, best)) continue;

                const core::vec3d o = eye - mesh->translation;   // ray in mesh-local coordinates
                auto vert = [&](uint32_t i) {
                    const core::vec3f& v = mesh->vertex_list[i];
                    return core::vec3d(v.x, v.y, v.z);
                };
                for (const DrawCallInfo& dc : mesh->draw_call_list)
                {
                    if (!dc.is_drawable()) continue;
                    const int n = dc.get_index_count();
                    const bool strip = dc.get_primitive_type() == kGlTriangleStrip;
                    const int tris = strip ? n - 2 : n / 3;
                    for (int k = 0; k < tris; k++)
                    {
                        const int b = strip ? k : k * 3;
                        uint32_t i0 = dc.get_index(b), i1 = dc.get_index(b + 1), i2 = dc.get_index(b + 2);
                        if (i0 >= uint32_t(mesh->num_vertex) || i1 >= uint32_t(mesh->num_vertex) ||
                            i2 >= uint32_t(mesh->num_vertex))
                            continue;   // strip restart or bad index
                        double t = RayTriangle(o, dir, vert(i0), vert(i1), vert(i2));
                        if (t > 0.0 && t < best)
                        {
                            best = t;
                            bestBatch = batch;
                            bestGroup = group;
                            bestMesh = mesh;
                        }
                    }
                }
            }
        }
    }

    if (!bestMesh)
    {
        ClearSelection();   // clicked empty space
        return;
    }

    m_selGroup = bestGroup;
    if (bestMesh->object_id >= 0 && size_t(bestMesh->object_id) < bestGroup->objects.size())
    {
        m_selObject = bestMesh->object_id;
        m_selMesh = nullptr;
    }
    else
    {
        m_selObject = -1;
        m_selMesh = bestMesh;
    }

    MeshToolUI::SelectionInfo& info = m_ui->selection;
    info = MeshToolUI::SelectionInfo();
    info.active = true;
    info.name = m_selMesh ? "mesh (not segmented)" : bestGroup->objects[size_t(m_selObject)].name;
    info.className = m_selMesh ? "-" : GetObjectClassInfo(MeshClass(bestGroup, bestMesh)).name;
    core::bounds3d box;
    SelectionBounds(box, &info.meshes, &info.triangles);
    if (box.b_valid)
    {
        info.size[0] = box.bb_max.x - box.bb_min.x;
        info.size[1] = box.bb_max.y - box.bb_min.y;
        info.size[2] = box.bb_max.z - box.bb_min.z;
    }
    const core::vec3d hit = eye + dir * best;
    info.hit[0] = hit.x; info.hit[1] = hit.y; info.hit[2] = hit.z;
    if (bestBatch->is_georeferenced && !bestGroup->no_gps)
    {
        GeographicLib::LocalCartesian local(bestBatch->reference_pos.y, bestBatch->reference_pos.x, 0.0);
        double lat, lon, h;
        local.Reverse(hit.x, hit.y, hit.z, lat, lon, h);
        char buf[96];
        snprintf(buf, sizeof(buf), "%.7f, %.7f   %.1f m", lat, lon, h);
        info.gpsText = buf;
    }
    if (bestMesh->capture_id >= 0 && size_t(bestMesh->capture_id) < bestGroup->captures.size())
        info.captureText = "#" + std::to_string(bestMesh->capture_id + 1) + "  " +
                           bestGroup->captures[size_t(bestMesh->capture_id)].placement;
}

void MeshToolApp::UpdateSelection()
{
    if (m_ui->wantClearSelection)
    {
        m_ui->wantClearSelection = false;
        ClearSelection();
    }

    // A click (press and release without dragging) in the viewport, not over
    // UI and not part of a camera move.
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !io.KeyAlt &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f)
    {
        const ImVec2 p = io.MouseClickedPos[ImGuiMouseButton_Left];
        if (p.x >= m_ui->viewportX && p.y >= m_ui->viewportY &&
            p.x < m_ui->viewportX + m_ui->viewportW && p.y < m_ui->viewportY + m_ui->viewportH)
            PickAt(p.x, p.y);
    }

    // Still in the scene? Captures and scene loads replace groups and meshes.
    if (m_selGroup)
    {
        bool found = false;
        for (const BatchMeshData* batch : g_world.mesh_data_batches)
            if (batch && std::find(batch->group_meshes.begin(), batch->group_meshes.end(), m_selGroup) != batch->group_meshes.end())
                found = true;
        if (found && m_selMesh)
            found = std::find(m_selGroup->meshes.begin(), m_selGroup->meshes.end(), m_selMesh) != m_selGroup->meshes.end();
        if (found && !m_selMesh)
            found = m_selObject >= 0 && size_t(m_selObject) < m_selGroup->objects.size();
        if (!found)
            ClearSelection();
    }
    if (!m_selGroup)
    {
        m_ui->wantFrameSelection = false;
        return;
    }

    if (m_ui->wantFrameSelection)
    {
        m_ui->wantFrameSelection = false;
        core::bounds3d box;
        if (SelectionBounds(box))
            FrameBounds(box);
    }

    DrawSelectionOverlay();
}

// Scene point -> viewport pixels, for overlays drawn with ImGui over the 3D
// view (background list: over the scene, under the UI windows).
struct MeshToolApp::OverlayView
{
    float vx, vy, vw, vh;
    float vp[16];
    core::vec3d eye;
    ImDrawList* dl;
    static constexpr double kNearW = 1e-3;

    struct Clip { double x, y, z, w; };

    OverlayView(const MeshToolUI& ui, ViewCamera& camera)
        : vx(ui.viewportX), vy(ui.viewportY),
          vw((std::max)(ui.viewportW, 1.0f)), vh((std::max)(ui.viewportH, 1.0f)),
          eye(camera.Eye()), dl(ImGui::GetBackgroundDrawList())
    {
        camera.BuildViewProj(vw / vh, vp);
        dl->PushClipRect(ImVec2(vx, vy), ImVec2(vx + vw, vy + vh), true);
    }
    ~OverlayView() { dl->PopClipRect(); }

    // Camera-relative point -> clip space (column-major matrix).
    Clip ToClip(const core::vec3d& p) const
    {
        const double v[3] = { p.x - eye.x, p.y - eye.y, p.z - eye.z };
        double out[4];
        for (int r = 0; r < 4; r++)
            out[r] = vp[r] * v[0] + vp[4 + r] * v[1] + vp[8 + r] * v[2] + vp[12 + r];
        return Clip{ out[0], out[1], out[2], out[3] };
    }
    ImVec2 ToScreen(const Clip& c) const
    {
        return ImVec2(vx + float((c.x / c.w * 0.5 + 0.5) * vw), vy + float((c.y / c.w * 0.5 + 0.5) * vh));
    }
    // False if the point is behind the camera.
    bool Project(const core::vec3d& p, ImVec2& out) const
    {
        Clip c = ToClip(p);
        if (c.w < kNearW) return false;
        out = ToScreen(c);
        return true;
    }
    void Line(const core::vec3d& a, const core::vec3d& b, ImU32 col, float thickness) const
    {
        Clip ca = ToClip(a), cb = ToClip(b);
        if (ca.w < kNearW && cb.w < kNearW) return;
        if (ca.w < kNearW || cb.w < kNearW)
        {
            // Cut the part behind the camera.
            double t = (kNearW - ca.w) / (cb.w - ca.w);
            Clip m{ ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t, ca.z + (cb.z - ca.z) * t, kNearW };
            (ca.w < kNearW ? ca : cb) = m;
        }
        dl->AddLine(ToScreen(ca), ToScreen(cb), col, thickness);
    }
    // Text in a dark box centred above `p`.
    void Label(ImVec2 p, ImU32 col, const char* text) const
    {
        ImVec2 ts = ImGui::CalcTextSize(text);
        p.x -= ts.x * 0.5f;
        p.y -= ts.y + 10.0f;
        dl->AddRectFilled(ImVec2(p.x - 6, p.y - 3), ImVec2(p.x + ts.x + 6, p.y + ts.y + 3), IM_COL32(15, 15, 22, 210), 4.0f);
        dl->AddText(p, col, text);
    }
};

void MeshToolApp::DrawSelectionOverlay()
{
    core::bounds3d box;
    if (!SelectionBounds(box))
        return;

    const OverlayView view(*m_ui, *m_camera);
    const ImU32 lineCol = IM_COL32(255, 220, 30, 230);

    const core::vec3d lo = box.bb_min, hi = box.bb_max;
    core::vec3d c[8];
    for (int i = 0; i < 8; i++)
        c[i] = core::vec3d((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
    for (const auto& e : edges)
        view.Line(c[e[0]], c[e[1]], lineCol, 2.0f);

    // Label above the box.
    ImVec2 top;
    if (view.Project(core::vec3d((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, hi.z), top))
    {
        const MeshToolUI::SelectionInfo& info = m_ui->selection;
        char text[192];
        snprintf(text, sizeof(text), "%s  [%s]  %.1f x %.1f x %.1f m  (%s)", info.name.c_str(), info.className.c_str(),
                 info.size[0], info.size[1], info.size[2],
                 m_ui->selectionView == MeshToolUI::kSelActual ? "actual" : "segment");
        view.Label(top, IM_COL32(255, 225, 60, 255), text);
    }
}

void MeshToolApp::DrawCaptureOverlay()
{
    if (!m_ui->debugCaptures)
        return;

    const OverlayView view(*m_ui, *m_camera);
    auto color = [](size_t k, int alpha) {
        float rgb[3];
        DistinctColor(int32_t(k), rgb);
        return IM_COL32(int(rgb[0] * 255), int(rgb[1] * 255), int(rgb[2] * 255), alpha);
    };

    for (const BatchMeshData* batch : g_world.mesh_data_batches)
        for (const GroupMeshData* group : batch->group_meshes)
        {
            const std::vector<CaptureInfo>& caps = group->captures;
            for (size_t k = 0; k < caps.size(); k++)
            {
                const CaptureInfo& c = caps[k];
                const ImU32 col = color(k, 235);

                // Footprint outline at the capture's base height.
                if (c.footprint.b_valid)
                {
                    const core::vec3d& lo = c.footprint.bb_min;
                    const core::vec3d& hi = c.footprint.bb_max;
                    const core::vec3d r[4] = { core::vec3d(lo.x, lo.y, lo.z), core::vec3d(hi.x, lo.y, lo.z),
                                               core::vec3d(hi.x, hi.y, lo.z), core::vec3d(lo.x, hi.y, lo.z) };
                    for (int e = 0; e < 4; e++)
                        view.Line(r[e], r[(e + 1) % 4], color(k, 150), 1.5f);
                }

                // Path from the previous camera, then this camera's view ray.
                if (k > 0)
                    view.Line(caps[k - 1].eye, c.eye, IM_COL32(235, 235, 245, 200), 2.0f);
                view.Line(c.eye, c.target, col, 1.5f);

                ImVec2 p;
                if (view.Project(c.target, p))
                    view.dl->AddCircle(p, 4.0f, col, 0, 1.5f);
                if (view.Project(c.eye, p))
                {
                    view.dl->AddCircleFilled(p, 6.0f, col);
                    view.dl->AddCircle(p, 6.0f, IM_COL32(15, 15, 22, 255), 0, 1.5f);
                    char text[96];
                    snprintf(text, sizeof(text), "#%zu %s", k + 1, c.placement.c_str());
                    view.Label(p, col, text);
                }
            }
        }
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
        else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
            m_ui->geFollowViewport = !m_ui->geFollowViewport;
        else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T, false) && m_ui->selection.active)
            m_ui->selectionView = m_ui->selectionView == MeshToolUI::kSelActual ? MeshToolUI::kSelSegment
                                                                                : MeshToolUI::kSelActual;
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

    const bool captureSettled = CaptureSettled();
    // Tell the hook, for its F12 hint and to ignore F12 while GE settles.
    if (GLCaptureHeader* hdr = m_processManager ? m_processManager->GetHeader() : nullptr)
        hdr->capture_gate = !m_ui->geFollowViewport || !GeoreferencedBatch() ? GLCAPTURE_GATE_NONE
                            : captureSettled                                 ? GLCAPTURE_GATE_READY
                                                                             : GLCAPTURE_GATE_WAIT;
    if (m_ui->wantCaptureFrame)
    {
        m_ui->wantCaptureFrame = false;
        if (!captureSettled)
        {
            m_ui->statusMessage = "Capture held back: " + m_ui->captureGate + " - try again in a moment.";
            m_ui->statusTimeout = 4.0f;
        }
        else if (m_processManager && m_processManager->IsRunning())
        {
            // Non-blocking: the result is picked up below when the hook signals it.
            m_processManager->RequestFrameCapture();
            m_capturePending = true;
            m_captureRequestTime = glfwGetTime();
            m_ui->statusMessage = "Capturing frame...";
            m_ui->statusTimeout = 10.0f;
        }
    }

    UpdateSegmentation();

    UpdateRefine();
    const bool segmenting = m_segmenter && m_segmenter->Running();

    // A finished capture, requested here or with F12 inside Google Earth.
    // While segmentation reads the meshes, captures stay queued in the hook's
    // buffer (the event stays signalled) and are picked up afterwards.
    HANDLE readyEvent = m_processManager ? m_processManager->GetReadyEvent() : nullptr;
    if (!segmenting && readyEvent && m_ui->captureProcessor && WaitForSingleObject(readyEvent, 0) == WAIT_OBJECT_0)
    {
        m_capturePending = false;

        BatchMeshData* batch = m_ui->liveBatch;
        if (m_geoServer)
            m_ui->captureProcessor->SetGeoView(m_geoServer->Latest());
        // While following, a capture (F12 in GE) taken before GE settled on
        // the viewport camera is replayed but not kept.
        m_ui->captureProcessor->processFrame(captureSettled);
        if (!captureSettled)
        {
            m_ui->statusMessage = "Capture ignored: " + m_ui->captureGate + ". Press F12 once it settles.";
            m_ui->statusTimeout = 5.0f;
        }
        const LiveCaptureProcessor::FrameResult& result = m_ui->captureProcessor->LastResult();
        if (result.dropped)
        {
            m_ui->statusMessage = "Capture dropped: it overflowed the capture buffer (old hook - run deploy_hook.bat).";
            m_ui->statusTimeout = 8.0f;
        }

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

        // Overview captures (GE zoomed far out) the next capture replaced.
        if (!result.discarded.empty())
        {
            vkDeviceWaitIdle(m_renderer->GetContext().device);
            for (GroupMeshData* group : result.discarded)
                ReleaseGroup(group);
        }

        // Show the capture right away, centred in the viewport. A capture that
        // overlaps or borders an area was merged into it; any other is a new
        // area beside the others.
        if (batch && result.new_data && result.group)
        {
            auto& batches = g_world.mesh_data_batches;
            if (std::find(batches.begin(), batches.end(), batch) == batches.end())
                batches.push_back(batch);

            UploadGroupTextures(result.group);

            RecomputeWorldBounds();
            // With GE following the viewport, reframing would fly GE away
            // from where the user is capturing: keep the camera.
            if (!m_ui->geFollowViewport)
                FrameBounds(result.group->bbox_ws);

            m_ui->statusMessage = std::string(result.merged ? "Merged capture into the area it overlaps: "
                                              : batch->group_meshes.size() > 1 ? "Captured a new area: " : "Captured: ") +
                                  std::to_string(result.group->meshes.size()) + " meshes." +
                                  (result.truncated ? "  (Partly recorded: the view overflowed the capture buffer.)" : "");
            m_ui->statusTimeout = 6.0f;
        }
    }
    else if (!segmenting && m_capturePending && glfwGetTime() - m_captureRequestTime > 10.0)
    {
        m_capturePending = false;
        m_ui->statusMessage = "Timed out waiting for frame capture.";
        m_ui->statusTimeout = 5.0f;
    }
    m_ui->captureWaiting = m_capturePending;

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
    m_segmenter.reset();   // cancels and joins a running segmentation

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
