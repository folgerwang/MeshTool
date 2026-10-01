#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <limits>
#include <sstream>

#include "imgui.h"
#include "imgui_internal.h"
#include "nfd.h"
#include "vk_texture_manager.h"

#include "base.h"
#include "worlddata.h"
#include "coregeographic.h"
#include "GpaDumpAnalyzeTool.h"
#include "livecaptureprocessor.h"
#include "objectclass.h"

// ---------------------------------------------------------------------------
// Custom dark theme inspired by Blender/RenderDoc/Unreal
// ---------------------------------------------------------------------------

void MeshToolUI::ApplyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();

    // Sizing
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 8.0f;

    // Rounding
    s.WindowRounding    = 4.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 3.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 4.0f;

    // Borders
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.TabBorderSize     = 0.0f;

    ImVec4* c = s.Colors;

    // ---- High-contrast dark theme ----
    // Panels are clearly lighter than the dark viewport.

    // Background - panels use a warm-tinted grey, distinct from cool-black viewport
    c[ImGuiCol_WindowBg]            = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.24f, 0.24f, 0.28f, 0.97f);

    // Borders - visible lines between regions
    c[ImGuiCol_Border]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_BorderShadow]        = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Frame (input fields, checkboxes) - darker inset look
    c[ImGuiCol_FrameBg]             = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);

    // Title bar
    c[ImGuiCol_TitleBg]             = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]    = ImVec4(0.16f, 0.16f, 0.19f, 0.50f);

    // Menu bar
    c[ImGuiCol_MenuBarBg]           = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_Tab]                 = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_TabHovered]          = ImVec4(0.35f, 0.55f, 0.85f, 0.80f);
    c[ImGuiCol_TabSelected]         = ImVec4(0.28f, 0.50f, 0.80f, 1.00f);

    // Accent (vivid blue)
    ImVec4 accent      = ImVec4(0.30f, 0.58f, 0.92f, 1.00f);
    ImVec4 accentHover = ImVec4(0.40f, 0.66f, 0.96f, 1.00f);
    ImVec4 accentPress = ImVec4(0.24f, 0.50f, 0.84f, 1.00f);

    // Buttons - raised look, clearly distinct from background
    c[ImGuiCol_Button]              = ImVec4(0.30f, 0.30f, 0.36f, 1.00f);
    c[ImGuiCol_ButtonHovered]       = accentHover;
    c[ImGuiCol_ButtonActive]        = accentPress;

    // Header (collapsing headers) - highlighted bar
    c[ImGuiCol_Header]              = ImVec4(0.28f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.34f, 0.34f, 0.42f, 1.00f);
    c[ImGuiCol_HeaderActive]        = accent;

    // Separator - visible divider
    c[ImGuiCol_Separator]           = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_SeparatorHovered]    = accentHover;
    c[ImGuiCol_SeparatorActive]     = accent;

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.36f, 0.36f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.46f, 0.46f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.60f, 1.00f);

    // Slider, checkbox
    c[ImGuiCol_SliderGrab]          = accent;
    c[ImGuiCol_SliderGrabActive]    = accentPress;
    c[ImGuiCol_CheckMark]           = accent;

    // Text - bright white for readability
    c[ImGuiCol_Text]                = ImVec4(0.95f, 0.95f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled]        = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);

    // Progress bar
    c[ImGuiCol_PlotHistogram]       = accent;

    // Resize grip
    c[ImGuiCol_ResizeGrip]          = ImVec4(0.30f, 0.30f, 0.36f, 0.40f);
    c[ImGuiCol_ResizeGripHovered]   = accentHover;
    c[ImGuiCol_ResizeGripActive]    = accent;

    // Modal dim
    c[ImGuiCol_ModalWindowDimBg]    = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void MeshToolUI::Init(VulkanTextureManager* texMgr)
{
    m_texMgr = texMgr;
    ApplyTheme();
    LoadIcons();
}

void MeshToolUI::LoadIcons()
{
    if (!m_texMgr) return;

    uint8_t pixels[icons::ICON_BYTES];
    for (int i = 0; i < icons::ICON_COUNT; i++)
    {
        icons::GetIconGenerator((icons::IconID)i)(pixels);
        m_iconHandles[i] = m_texMgr->UploadRGBA(pixels, icons::ICON_SIZE, icons::ICON_SIZE);
    }
    m_iconsLoaded = true;
}

// ---------------------------------------------------------------------------
// Main draw
// ---------------------------------------------------------------------------

void MeshToolUI::DrawUI()
{
    DrawMenuBar();
    DrawToolbar();
    DrawScenePanel();
    DrawViewport();
    DrawStatusBar();

    if (showRefPointDialog)     DrawRefPointDialog();
    if (showRegionSelectDialog) DrawRegionSelectDialog();
    if (showNavCaptureDialog)   DrawNavCaptureDialog();
    if (showSegmentDialog)      DrawSegmentDialog();
    if (showMessageBox)         DrawMessageBox();

    // Auto-capture timer
    if (m_pendingAutoCapture)
    {
        m_autoCaptureTimer -= ImGui::GetIO().DeltaTime;
        float remaining = m_autoCaptureTimer;

        if (remaining > 0)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Waiting for Google Earth to load... %.0fs remaining", remaining);
            statusMessage = msg;
            statusTimeout = 2.0f;
        }
        else
        {
            // Time to capture
            if (processManager && processManager->IsHookConnected())
            {
                wantCaptureFrame = true;
                m_autoCaptureCount++;

                if (m_autoCaptureCount >= m_autoCaptureMaxFrames)
                {
                    m_pendingAutoCapture = false;
                    SetStatus("Auto-capture complete. " + std::to_string(m_autoCaptureCount) + " frames captured.");
                }
                else
                {
                    // Wait a bit more for the next frame
                    m_autoCaptureTimer = 3.0f;
                    SetStatus("Captured frame " + std::to_string(m_autoCaptureCount) +
                              "/" + std::to_string(m_autoCaptureMaxFrames) + ", next in 3s...");
                }
            }
            else
            {
                // Hook not connected yet, keep waiting
                m_autoCaptureTimer = 2.0f;
                SetStatus("Waiting for hook DLL to connect...");
            }
        }
    }

    if (statusTimeout > 0)
    {
        statusTimeout -= ImGui::GetIO().DeltaTime;
        if (statusTimeout <= 0) { statusTimeout = 0; statusMessage.clear(); }
    }
}

// ---------------------------------------------------------------------------
// Menu bar (top)
// ---------------------------------------------------------------------------

void MeshToolUI::DrawMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Scene...",      "Ctrl+O"))   ActionOpenScene();
            if (ImGui::MenuItem("Save Scene...",      "Ctrl+S", false, !g_world.mesh_data_batches.empty()))
                ActionSaveScene();
            ImGui::Separator();
            if (ImGui::MenuItem("Import USGS...",     "Ctrl+U"))   ActionImportUSGS();
            if (ImGui::MenuItem("Import KML...",      "Ctrl+K"))   ActionImportKML();
            ImGui::Separator();
            if (ImGui::MenuItem("Export Mesh...",      "Ctrl+E"))   ActionExport();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Capture"))
        {
            if (ImGui::MenuItem("Navigate & Capture...", "Ctrl+N"))  showNavCaptureDialog = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Launch Google Earth"))   ActionLiveCapture();
            if (ImGui::MenuItem("Capture Frame",  "F5"))  wantCaptureFrame = true;
            if (ImGui::MenuItem("Stop Capture"))          wantStopCapture = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings"))
        {
            if (ImGui::MenuItem("GPS Reference Point..."))
            {
                refLonInput = g_world.reference_pos.x;
                refLatInput = g_world.reference_pos.y;
                showRefPointDialog = true;
            }
            if (ImGui::MenuItem("Region Select..."))
            {
                regionLon0 = g_world.scissor_bbox.bb_min.x;
                regionLat0 = g_world.scissor_bbox.bb_min.y;
                regionLon1 = g_world.scissor_bbox.bb_max.x;
                regionLat1 = g_world.scissor_bbox.bb_max.y;
                showRegionSelectDialog = true;
            }
            ImGui::Separator();
            ImGui::MenuItem("Scene Panel", nullptr, &showScenePanel);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem("Segment Scene (AI)...", nullptr, false, !g_world.mesh_data_batches.empty()))
                showSegmentDialog = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Frame All", "F", false, !g_world.mesh_data_batches.empty()))
                wantFrameAll = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::BeginMenu("Viewport Controls"))
            {
                ImGui::TextDisabled("Maya style");
                ImGui::BulletText("Alt + Left drag     Orbit around pivot");
                ImGui::BulletText("Alt + Middle drag   Pan");
                ImGui::BulletText("Alt + Right drag    Dolly");
                ImGui::Separator();
                ImGui::TextDisabled("Unreal style");
                ImGui::BulletText("Right drag          Look around");
                ImGui::BulletText("Right + W/A/S/D     Fly, Q/E down/up");
                ImGui::BulletText("Right + Wheel       Fly speed, Shift = faster");
                ImGui::BulletText("Left drag           Move forward/back + turn");
                ImGui::BulletText("Middle / L+R drag   Pan");
                ImGui::Separator();
                ImGui::BulletText("Wheel               Zoom to pivot");
                ImGui::BulletText("F                   Frame all");
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem("About MeshTool", nullptr, false, false);
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------------
// Left toolbar (icon-style vertical strip)
// ---------------------------------------------------------------------------

// Image button helper: draws a Vulkan texture as an ImGui button
bool MeshToolUI::IconBtn(icons::IconID id, const char* tooltip, const ImVec4& tint)
{
    const float sz = 48.0f;
    bool clicked = false;

    if (m_iconsLoaded && m_texMgr)
    {
        void* texID = m_texMgr->GetImTextureID(m_iconHandles[id]);
        if (texID)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f, 0.16f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.30f, 0.50f, 0.75f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.20f, 0.40f, 0.65f, 1.0f));
            clicked = ImGui::ImageButton(tooltip, (ImTextureID)texID, ImVec2(sz, sz),
                                         ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint);
            ImGui::PopStyleColor(3);
        }
    }
    else
    {
        // Fallback text button
        clicked = ImGui::Button(tooltip, ImVec2(sz + 12, sz + 4));
    }

    if (ImGui::IsItemHovered() && tooltip)
        ImGui::SetTooltip("%s", tooltip);

    return clicked;
}

void MeshToolUI::DrawToolbar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float toolbarW = 80.0f;
    const float statusH  = 26.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(toolbarW, vp->WorkSize.y - statusH));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.14f, 0.17f, 1.0f));
    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec4 white(1,1,1,1);

    // Capture group
    if (IconBtn(icons::ICON_NAVIGATE,"Navigate & Capture",  white)) showNavCaptureDialog = true;
    if (IconBtn(icons::ICON_LAUNCH,  "Launch Google Earth", white)) ActionLiveCapture();
    if (IconBtn(icons::ICON_CAPTURE, "Capture Frame (F5)",  white)) wantCaptureFrame = true;
    if (IconBtn(icons::ICON_STOP,    "Stop Capture",        white)) wantStopCapture = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Import group
    if (IconBtn(icons::ICON_IMPORT,  "Open Scene (Ctrl+O)", white)) ActionOpenScene();
    if (IconBtn(icons::ICON_TERRAIN, "Import USGS",        white)) ActionImportUSGS();
    if (IconBtn(icons::ICON_SPLINE,  "Import KML",         white)) ActionImportKML();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Export
    if (IconBtn(icons::ICON_EXPORT,  "Export Mesh",         white)) ActionExport();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Segmentation
    if (IconBtn(icons::ICON_SEGMENT, "Segment Scene (AI)",  white)) showSegmentDialog = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Settings
    if (IconBtn(icons::ICON_PIN,     "GPS Reference",       white))
    {
        refLonInput = g_world.reference_pos.x;
        refLatInput = g_world.reference_pos.y;
        showRefPointDialog = true;
    }
    if (IconBtn(icons::ICON_REGION,  "Region Select",       white))
    {
        regionLon0 = g_world.scissor_bbox.bb_min.x;
        regionLat0 = g_world.scissor_bbox.bb_min.y;
        regionLon1 = g_world.scissor_bbox.bb_max.x;
        regionLat1 = g_world.scissor_bbox.bb_max.y;
        showRegionSelectDialog = true;
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// Right scene panel (properties / info)
// ---------------------------------------------------------------------------

void MeshToolUI::DrawScenePanel()
{
    if (!showScenePanel) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float toolbarW = 80.0f;
    const float panelW   = 280.0f;
    const float statusH  = 26.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panelW, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(panelW, vp->WorkSize.y - statusH));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.20f, 0.20f, 0.24f, 1.0f));
    ImGui::Begin("Scene", &showScenePanel,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

    // --- GPS Reference ---
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.30f, 0.45f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.38f, 0.55f, 1.0f));
    if (ImGui::CollapsingHeader("GPS Reference", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Reference Point");
        ImGui::Text("Lon: %.8f", g_world.reference_pos.x);
        ImGui::Text("Lat: %.8f", g_world.reference_pos.y);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Scissor Region");
        ImGui::Text("Min: (%.6f, %.6f)", g_world.scissor_bbox.bb_min.x, g_world.scissor_bbox.bb_min.y);
        ImGui::Text("Max: (%.6f, %.6f)", g_world.scissor_bbox.bb_max.x, g_world.scissor_bbox.bb_max.y);
        ImGui::Unindent(8);
    }

    ImGui::Spacing();

    ImGui::PopStyleColor(2);

    // --- Scene Stats ---
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.38f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.45f, 0.30f, 1.0f));
    if (ImGui::CollapsingHeader("Scene Statistics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8);
        size_t totalMeshes = 0;
        size_t totalVertices = 0;
        size_t totalBatches = g_world.mesh_data_batches.size();

        for (auto* batch : g_world.mesh_data_batches)
        {
            for (auto* group : batch->group_meshes)
            {
                totalMeshes += group->meshes.size();
                for (auto* mesh : group->meshes)
                    totalVertices += mesh->num_vertex;
            }
        }

        ImGui::Text("Batches:   %zu", totalBatches);
        ImGui::Text("Meshes:    %zu", totalMeshes);
        ImGui::Text("Vertices:  %zu", totalVertices);

        if (totalBatches > 0)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "World Bounds");
            ImGui::Text("X: [%.2f, %.2f]", g_world.bbox_ws.bb_min.x, g_world.bbox_ws.bb_max.x);
            ImGui::Text("Y: [%.2f, %.2f]", g_world.bbox_ws.bb_min.y, g_world.bbox_ws.bb_max.y);
            ImGui::Text("Z: [%.2f, %.2f]", g_world.bbox_ws.bb_min.z, g_world.bbox_ws.bb_max.z);
        }
        ImGui::Unindent(8);
    }

    ImGui::Spacing();

    ImGui::PopStyleColor(2);

    DrawObjectsSection();

    // --- Batch list ---
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.40f, 0.28f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.48f, 0.34f, 0.22f, 1.0f));
    if (ImGui::CollapsingHeader("Mesh Batches"))
    {
        ImGui::Indent(8);
        for (size_t i = 0; i < g_world.mesh_data_batches.size(); i++)
        {
            auto* batch = g_world.mesh_data_batches[i];
            char label[64];
            snprintf(label, sizeof(label), "Batch %zu (%zu groups)", i, batch->group_meshes.size());

            if (ImGui::TreeNode(label))
            {
                ImGui::Text("Google Dump: %s", batch->is_google_dump ? "Yes" : "No");
                ImGui::Text("Spline Mesh: %s", batch->is_spline_mesh ? "Yes" : "No");

                for (size_t j = 0; j < batch->group_meshes.size(); j++)
                {
                    auto* group = batch->group_meshes[j];
                    char glabel[64];
                    snprintf(glabel, sizeof(glabel), "Group %zu (%zu meshes, %zu tex)", j,
                             group->meshes.size(), group->loaded_textures.size());
                    if (ImGui::TreeNode(glabel))
                    {
                        for (size_t k = 0; k < group->meshes.size(); k++)
                        {
                            auto* mesh = group->meshes[k];
                            ImGui::Text("  Mesh %zu: %d verts, %zu draws",
                                        k, mesh->num_vertex, mesh->draw_call_list.size());
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
        if (g_world.mesh_data_batches.empty())
            ImGui::TextDisabled("No data loaded");
        ImGui::Unindent(8);
    }

    ImGui::Spacing();

    ImGui::PopStyleColor(2);

    // --- Live Capture Status ---
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.35f, 0.20f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.42f, 0.26f, 0.45f, 1.0f));
    if (ImGui::CollapsingHeader("Live Capture"))
    {
        ImGui::Indent(8);
        bool running = processManager && processManager->IsRunning();
        bool hooked  = processManager && processManager->IsHookConnected();

        ImGui::TextColored(running ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           running ? "Google Earth: Running" : "Google Earth: Not running");
        ImGui::TextColored(hooked ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           hooked ? "Hook DLL: Connected" : "Hook DLL: Not connected");
        ImGui::Unindent(8);
    }

    // --- Progress ---
    float frac = progress.GetFraction();
    if (frac > 0.001f)
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Progress:");
        ImGui::ProgressBar(frac, ImVec2(-1, 0));
    }

    ImGui::PopStyleColor(2); // Live Capture header colors
    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
}

// ---------------------------------------------------------------------------
// Viewport (the 3D area)
// ---------------------------------------------------------------------------

void MeshToolUI::DrawViewport()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float toolbarW = 80.0f;
    const float panelW   = showScenePanel ? 280.0f : 0.0f;
    const float statusH  = 26.0f;

    float x = vp->WorkPos.x + toolbarW;
    float y = vp->WorkPos.y;
    float w = vp->WorkSize.x - toolbarW - panelW;
    float h = vp->WorkSize.y - statusH;

    viewportX = x;
    viewportY = y;
    viewportW = w;
    viewportH = h;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // No filled background here: ImGui is rendered after the 3D scene, so any
    // opaque fill would cover the meshes. The renderer's clear color is the
    // viewport background.

    // Navigation hint (bottom-left)
    {
        const char* nav = "Alt+LMB orbit  Alt+MMB pan  Alt+RMB dolly  |  RMB look + WASD/QE fly  |  Wheel zoom  F frame";
        float lineH = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(x + 10, y + h - lineH - 8), IM_COL32(110, 118, 145, 170), nav);
    }

    // Border
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(50, 52, 65, 200), 0.0f, 0, 1.0f);

    // Overlay text when empty
    if (g_world.mesh_data_batches.empty())
    {
        const char* hint = "Drag & drop files or use File > Import to load mesh data";
        ImVec2 textSize = ImGui::CalcTextSize(hint);
        dl->AddText(
            ImVec2(x + (w - textSize.x) * 0.5f, y + h * 0.8f - textSize.y * 0.5f),
            IM_COL32(140, 145, 170, 200), hint);
    }

    // Corner info overlay: GPS of the camera pivot (set by the app when the
    // scene is georeferenced), else the configured reference point.
    {
        char camInfo[160];
        if (!geoText.empty())
            snprintf(camInfo, sizeof(camInfo), "%s", geoText.c_str());
        else
            snprintf(camInfo, sizeof(camInfo), "Lon: %.4f  Lat: %.4f  (not georeferenced)",
                     g_world.reference_pos.x, g_world.reference_pos.y);
        dl->AddText(ImVec2(x + 10, y + 8), IM_COL32(120, 130, 160, 200), camInfo);
    }
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void MeshToolUI::DrawStatusBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float statusH = 26.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - statusH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, statusH));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.14f, 0.17f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

    char info[128];
    snprintf(info, sizeof(info), "%.1f FPS | %zu batches",
             ImGui::GetIO().Framerate, g_world.mesh_data_batches.size());
    const float infoW = ImGui::CalcTextSize(info).x;
    const ImGuiStyle& style = ImGui::GetStyle();

    if (segRunning || captureWaiting)
    {
        // Full-width progress bar across the bottom of the window.
        const float barH = statusH - 8.0f;
        float cancelW = 0.0f;
        if (segRunning)
            cancelW = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
        float barW = ImGui::GetContentRegionAvail().x - infoW - cancelW - 24.0f;
        char overlay[320];
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.25f, 0.60f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
        if (segRunning)
        {
            snprintf(overlay, sizeof(overlay), "%s   %d%%", segStatus.c_str(), int(segProgress * 100.0f + 0.5f));
            ImGui::ProgressBar(segProgress, ImVec2(barW, barH), overlay);
        }
        else
        {
            // Unknown duration: indeterminate (animated) bar.
            ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(barW, barH), "Waiting for Google Earth to capture the frame...");
        }
        ImGui::PopStyleColor(2);
        if (segRunning)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
                wantCancelSegment = true;
        }
    }
    else if (!statusMessage.empty())
        ImGui::TextUnformatted(statusMessage.c_str());
    else
        ImGui::TextDisabled("Ready");

    // Right-aligned info
    ImGui::SameLine(ImGui::GetWindowWidth() - infoW - 16);
    ImGui::TextDisabled("%s", info);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Segmented objects (scene panel)
// ---------------------------------------------------------------------------

void MeshToolUI::DrawObjectsSection()
{
    size_t instances[kObjClassCount] = {};
    size_t total = 0;
    for (const BatchMeshData* batch : g_world.mesh_data_batches)
        for (const GroupMeshData* group : batch->group_meshes)
            for (const SceneObject& o : group->objects)
            {
                instances[o.cls < kObjClassCount ? o.cls : kObjUnknown]++;
                total++;
            }
    if (total == 0)
        return;

    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.34f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.42f, 0.48f, 1.0f));
    if (ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8);
        ImGui::Checkbox("Colour by class", &classColors);
        ImGui::Spacing();
        for (int c = 1; c < kObjClassCount; c++)
        {
            const ObjectClassInfo& info = GetObjectClassInfo(ObjectClass(c));
            ImGui::PushID(c);
            ImGui::Checkbox("##vis", &classVisible[c]);
            ImGui::SameLine();
            ImGui::ColorButton("##col", ImVec4(info.color[0], info.color[1], info.color[2], 1.0f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(16, 16));
            ImGui::SameLine();
            if (IsInstanceClass(ObjectClass(c)))
                ImGui::Text("%-9s %zu", info.name, instances[c]);
            else
                ImGui::Text("%-9s %s", info.name, instances[c] ? "area" : "-");
            ImGui::PopID();
        }
        // Meshes outside any object (e.g. not yet segmented captures).
        ImGui::Checkbox("##vis0", &classVisible[kObjUnknown]);
        ImGui::SameLine();
        ImGui::TextDisabled("unassigned");
        ImGui::Unindent(8);
    }
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

void MeshToolUI::DrawSegmentDialog()
{
    ImGui::OpenPopup("Segment Scene");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Segment Scene", &showSegmentDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
            "Split the scene into buildings, trees, cars, road, plants, water and ground.");
        ImGui::TextDisabled("A vision model labels a top-down render; the 3D mesh gives the exact outlines.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (segRunning)
        {
            ImGui::ProgressBar(segProgress, ImVec2(460, 0));
            ImGui::TextUnformatted(segStatus.c_str());
            ImGui::TextDisabled("Captures wait until segmentation finishes.");
            ImGui::Spacing();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
                wantCancelSegment = true;
            ImGui::SameLine();
            if (ImGui::Button("Hide", ImVec2(120, 0)))
            {
                showSegmentDialog = false;
                ImGui::CloseCurrentPopup();
            }
        }
        else
        {
            ImGui::Checkbox("Use vision model", &segSettings.useModel);
            ImGui::BeginDisabled(!segSettings.useModel);
            ImGui::InputText("Server (Ollama)", segServer, sizeof(segServer));
            ImGui::InputText("Model", segModel, sizeof(segModel));
            ImGui::EndDisabled();
            if (!segSettings.useModel)
                ImGui::TextDisabled("Without the model, classes come from colour and height only.");

            ImGui::Spacing();
            ImGui::InputDouble("Resolution (m/pixel)", &segSettings.metresPerPixel, 0.05, 0.25, "%.2f");
            ImGui::InputDouble("Ground search radius (m)", &segSettings.groundRadius, 5, 20, "%.0f");
            ImGui::InputDouble("Raised above ground (m)", &segSettings.raisedHeight, 0.1, 0.5, "%.1f");
            segSettings.metresPerPixel = std::clamp(segSettings.metresPerPixel, 0.05, 5.0);
            segSettings.groundRadius = std::clamp(segSettings.groundRadius, 5.0, 500.0);
            segSettings.raisedHeight = std::clamp(segSettings.raisedHeight, 0.3, 20.0);
            ImGui::TextDisabled("The radius must exceed half the width of the largest building.");
            ImGui::TextDisabled("Debug images and log: %s", segSettings.debugDir.c_str());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Segment", ImVec2(140, 0)))
            {
                segSettings.server = segServer;
                segSettings.model = segModel;
                wantStartSegment = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(140, 0)))
            {
                showSegmentDialog = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void MeshToolUI::DrawRefPointDialog()
{
    ImGui::OpenPopup("GPS Reference Point");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("GPS Reference Point", &showRefPointDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Set the GPS reference coordinate:");
        ImGui::Spacing();
        ImGui::InputDouble("Longitude", &refLonInput, 0, 0, "%.10f");
        ImGui::InputDouble("Latitude",  &refLatInput, 0, 0, "%.10f");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply", ImVec2(130, 30)))
        {
            bool valid = true;
            std::string err;
            if (std::abs(refLonInput) > 180.0) { err += "Longitude out of range. "; valid = false; }
            if (std::abs(refLatInput) > 90.0)  { err += "Latitude out of range.";   valid = false; }

            if (valid)
            {
                g_world.reference_pos = core::vec2d(refLonInput, refLatInput);
                std::ofstream f("mesh_tool.cfg", std::ofstream::binary);
                if (f) { f.write((char*)&g_world.reference_pos, sizeof(core::vec2d));
                         f.write((char*)&g_world.scissor_bbox, sizeof(core::bounds2d)); f.close(); }
                showRefPointDialog = false;
                ImGui::CloseCurrentPopup();
            }
            else ShowMessage("Validation Error", err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(130, 30))) { showRefPointDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void MeshToolUI::DrawRegionSelectDialog()
{
    ImGui::OpenPopup("Region Select");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Region Select", &showRegionSelectDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Define the scissor GPS region:");
        ImGui::Spacing();

        ImGui::Text("Min Corner:");
        ImGui::InputDouble("Longitude##0", &regionLon0, 0, 0, "%.10f");
        ImGui::InputDouble("Latitude##0",  &regionLat0, 0, 0, "%.10f");
        ImGui::Spacing();
        ImGui::Text("Max Corner:");
        ImGui::InputDouble("Longitude##1", &regionLon1, 0, 0, "%.10f");
        ImGui::InputDouble("Latitude##1",  &regionLat1, 0, 0, "%.10f");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply", ImVec2(130, 30)))
        {
            bool valid = true;
            std::string err;
            if (std::abs(regionLon0) > 180.0) { err += "Min lon out of range. "; valid = false; }
            if (std::abs(regionLat0) > 90.0)  { err += "Min lat out of range. "; valid = false; }
            if (std::abs(regionLon1) > 180.0) { err += "Max lon out of range. "; valid = false; }
            if (std::abs(regionLat1) > 90.0)  { err += "Max lat out of range. "; valid = false; }
            if (valid && std::max(std::abs(regionLon0-regionLon1), std::abs(regionLat0-regionLat1)) < 1e-30)
            { err = "Corners are identical."; valid = false; }

            if (valid)
            {
                g_world.scissor_bbox.Reset();
                g_world.scissor_bbox += core::vec2d(regionLon0, regionLat0);
                g_world.scissor_bbox += core::vec2d(regionLon1, regionLat1);
                std::ofstream f("mesh_tool.cfg", std::ofstream::binary);
                if (f) { f.write((char*)&g_world.reference_pos, sizeof(core::vec2d));
                         f.write((char*)&g_world.scissor_bbox, sizeof(core::bounds2d)); f.close(); }
                showRegionSelectDialog = false;
                ImGui::CloseCurrentPopup();
            }
            else ShowMessage("Validation Error", err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(130, 30))) { showRegionSelectDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Navigate & Capture dialog
// ---------------------------------------------------------------------------

void MeshToolUI::DrawNavCaptureDialog()
{
    ImGui::OpenPopup("Navigate & Capture");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Navigate & Capture", &showNavCaptureDialog, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f),
            "Navigate Google Earth to a location and capture 3D mesh data.");
        ImGui::Spacing();

        // Mode selection
        ImGui::Text("Location Mode:");
        ImGui::RadioButton("GPS Coordinates", &navMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Address / Place Name", &navMode, 1);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (navMode == 0)
        {
            // GPS coordinate input
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Target Coordinates:");
            ImGui::InputDouble("Longitude##nav", &navLon, 0.001, 0.01, "%.8f");
            ImGui::InputDouble("Latitude##nav",  &navLat, 0.001, 0.01, "%.8f");

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Camera View:");
            ImGui::InputDouble("Altitude (m)",  &navAlt, 10, 100, "%.1f");
            ImGui::InputDouble("Heading (deg)", &navHeading, 5, 15, "%.1f");
            ImGui::SliderScalar("Tilt (deg)", ImGuiDataType_Double, &navTilt,
                               &(const double&)(0.0), &(const double&)(89.0), "%.1f");
            ImGui::InputDouble("Range (m)",     &navRange, 50, 200, "%.0f");

            // Quick presets
            ImGui::Spacing();
            ImGui::Text("Presets:");
            if (ImGui::SmallButton("Close-up (200m)"))   { navRange = 200;  navTilt = 60; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Street (500m)"))     { navRange = 500;  navTilt = 50; }
            ImGui::SameLine();
            if (ImGui::SmallButton("District (2km)"))    { navRange = 2000; navTilt = 45; }
            ImGui::SameLine();
            if (ImGui::SmallButton("City (10km)"))       { navRange = 10000; navTilt = 30; }
        }
        else
        {
            // Address input
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Search Location:");
            ImGui::InputText("Address", navAddress, sizeof(navAddress));
            ImGui::TextDisabled("Examples: \"Eiffel Tower, Paris\", \"1600 Pennsylvania Ave\"");

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Camera View:");
            ImGui::SliderScalar("Tilt (deg)", ImGuiDataType_Double, &navTilt,
                               &(const double&)(0.0), &(const double&)(89.0), "%.1f");
            ImGui::InputDouble("Range (m)", &navRange, 50, 200, "%.0f");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Capture settings
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Capture Settings:");
        ImGui::SliderInt("Wait time (sec)", &navCaptureDelay, 5, 60,
                         "%d seconds for GE to load");
        ImGui::Checkbox("Auto-capture after load", &navAutoCapture);

        if (navAutoCapture)
        {
            ImGui::SliderInt("Frames to capture", &m_autoCaptureMaxFrames, 1, 10);
            ImGui::TextDisabled("Multiple frames may capture more mesh detail.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Buttons
        // Size both buttons to fit the longer label at the current font size.
        const ImGuiStyle& style = ImGui::GetStyle();
        float bw = (std::max)(ImGui::CalcTextSize("Launch & Capture").x,
                              ImGui::CalcTextSize("Cancel").x) + style.FramePadding.x * 4.0f;
        float bh = ImGui::GetFrameHeight() + style.FramePadding.y * 2.0f;
        float spacing = (std::max)((ImGui::GetContentRegionAvail().x - bw * 2) / 3, style.ItemSpacing.x);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + spacing);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.50f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.60f, 0.30f, 1.0f));
        if (ImGui::Button("Launch & Capture", ImVec2(bw, bh)))
        {
            showNavCaptureDialog = false;
            ImGui::CloseCurrentPopup();
            ActionNavCapture();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Cancel", ImVec2(bw, bh)))
        {
            showNavCaptureDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------

void MeshToolUI::DrawMessageBox()
{
    ImGui::OpenPopup(messageBoxTitle.c_str());
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(messageBoxTitle.c_str(), &showMessageBox, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s", messageBoxText.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button("OK", ImVec2(std::min(w, 130.0f), 28)))
        { showMessageBox = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void MeshToolUI::ShowMessage(const std::string& title, const std::string& text)
{
    messageBoxTitle = title;
    messageBoxText = text;
    showMessageBox = true;
}

void MeshToolUI::SetStatus(const std::string& msg, float timeoutSeconds)
{
    statusMessage = msg;
    statusTimeout = timeoutSeconds;
}

// ---------------------------------------------------------------------------
// Action implementations (extracted from old button handlers)
// ---------------------------------------------------------------------------

void MeshToolUI::ActionOpenScene()
{
    nfdchar_t* outPath = nullptr;
    if (NFD_OpenDialog("mtscene", nullptr, &outPath) == NFD_OKAY && outPath)
    {
        pendingOpenScenePath = outPath;
        free(outPath);
    }
}

void MeshToolUI::ActionSaveScene()
{
    if (g_world.mesh_data_batches.empty())
    {
        ShowMessage("Save Scene", "Nothing to save.");
        return;
    }
    nfdchar_t* outPath = nullptr;
    if (NFD_SaveDialog("mtscene", nullptr, &outPath) == NFD_OKAY && outPath)
    {
        std::string fileName(outPath);
        free(outPath);
        // nfd does not append the extension
        size_t dot = fileName.rfind('.');
        size_t slash = fileName.find_last_of("/\\");
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            fileName += ".mtscene";
        pendingSaveScenePath = fileName;
    }
}
void MeshToolUI::ActionImportUSGS()
{
    nfdpathset_t pathSet;
    nfdresult_t result = NFD_OpenDialogMultiple("img", nullptr, &pathSet);
    if (result == NFD_OKAY)
    {
        size_t count = NFD_PathSet_GetCount(&pathSet);
        if (count > 0)
        {
            std::vector<std::unique_ptr<std::string>> mapFiles;
            for (size_t i = 0; i < count; i++)
            {
                nfdchar_t* p = NFD_PathSet_GetPath(&pathSet, i);
                if (p) mapFiles.push_back(std::make_unique<std::string>(p));
            }
            NFD_PathSet_Free(&pathSet);

            std::vector<std::shared_ptr<std::string>> texFiles;
            std::vector<DumppedTextureInfo> texInfo;
            DumpUSGSTexture(texFiles, texInfo);

            BatchMeshData* batch = new BatchMeshData;
            batch->reference_pos = g_world.reference_pos;
            batch->scissor_bbox = g_world.scissor_bbox;
            batch->is_google_dump = false;
            batch->is_spline_mesh = false;

            std::vector<MapInfo> mapInfo;
            PreLoadUSGSData(nullptr, mapFiles, mapInfo);
            GenerateMeshData(mapInfo, 256, batch);

            g_world.mesh_data_batches.push_back(batch);
            g_world.bbox_ws += batch->bbox_ws;
            wantFrameAll = true;
            SetStatus("USGS data imported.");
        }
        else NFD_PathSet_Free(&pathSet);
    }
}

void MeshToolUI::ActionImportKML()
{
    nfdpathset_t pathSet;
    nfdresult_t result = NFD_OpenDialogMultiple("kml", nullptr, &pathSet);
    if (result == NFD_OKAY)
    {
        size_t count = NFD_PathSet_GetCount(&pathSet);
        if (count > 0)
        {
            std::vector<std::string> files;
            for (size_t i = 0; i < count; i++)
            {
                nfdchar_t* p = NFD_PathSet_GetPath(&pathSet, i);
                if (p) files.push_back(p);
            }
            NFD_PathSet_Free(&pathSet);

            BatchMeshData* batch = new BatchMeshData;
            batch->reference_pos = g_world.reference_pos;
            batch->scissor_bbox = g_world.scissor_bbox;
            batch->is_google_dump = false;
            batch->is_spline_mesh = true;

            progress.Reset();
            DumpKmlSplineMeshes(files, batch, &progress);
            g_world.mesh_data_batches.push_back(batch);
            g_world.bbox_ws += batch->bbox_ws;
            g_world.bbox_gps += batch->bbox_gps;
            wantFrameAll = true;
            SetStatus("KML splines imported.");
        }
        else NFD_PathSet_Free(&pathSet);
    }
}

void MeshToolUI::ActionExport()
{
    if (g_world.mesh_data_batches.empty())
    {
        ShowMessage("Export", "No mesh data to export.");
        return;
    }

    nfdchar_t* outPath = nullptr;
    if (NFD_SaveDialog("glb;gltf;ma", nullptr, &outPath) == NFD_OKAY && outPath)
    {
        std::string fileName(outPath);
        free(outPath);

        // nfd does not append the extension -- default to .glb
        size_t dot = fileName.rfind('.');
        size_t slash = fileName.find_last_of("/\\");
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            fileName += ".glb";

        std::string ext = fileName.substr(fileName.rfind('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        progress.Reset();
        bool ok = true;
        if (ext == ".ma")
            ExportMaMeshFile(fileName, g_world.mesh_data_batches, &progress);
        else if (ext == ".glb" || ext == ".gltf")
            ok = ExportGltfMeshFile(fileName, g_world.mesh_data_batches, &progress);
        else
        {
            ShowMessage("Export", "Unsupported file type '" + ext + "'. Use .glb, .gltf or .ma.");
            ok = false;
        }
        progress.Reset();
        if (ok) SetStatus("Exported to " + fileName);
        else if (ext == ".glb" || ext == ".gltf") ShowMessage("Export", "Failed to write " + fileName);
    }
}

void MeshToolUI::ActionNavCapture()
{
    if (!processManager) { ShowMessage("Error", "Process manager not initialised."); return; }

    // Stop any existing capture
    if (processManager->IsRunning())
        processManager->StopGoogleEarth();

    // Setup shared memory
    if (!processManager->CreateSharedMemory())
    { ShowMessage("Error", "Failed to create shared memory for IPC."); return; }

    // Create batch
    liveBatch = new BatchMeshData;
    liveBatch->reference_pos = core::vec2d(navLon, navLat);
    liveBatch->scissor_bbox = g_world.scissor_bbox;
    liveBatch->is_google_dump = true;
    liveBatch->is_spline_mesh = false;

    // Also update the global reference position
    g_world.reference_pos = core::vec2d(navLon, navLat);

    // Create capture processor
    delete captureProcessor;
    captureProcessor = new LiveCaptureProcessor();
    captureProcessor->SetOutputBatch(liveBatch);
    captureProcessor->m_on_frame_captured = [this](int n) {
        SetStatus(n > 0 ? "Captured " + std::to_string(n) + " meshes." : "No meshes in frame.");
    };
    captureProcessor->m_on_capture_error = [this](const std::string& e) { SetStatus("Error: " + e, 10.0f); };
    captureProcessor->m_on_connection_changed = [this](bool c) {
        if (c) SetStatus("Hook connected. Waiting for Google Earth to load scene...");
    };

    // Generate KML and launch. With the view server running, the same file
    // carries the NetworkLink through which GE reports its view (GPS).
    std::string kmlPath;
    if (geoViewServer && geoViewServer->Port())
    {
        std::ostringstream extra;
        extra.precision(10);
        if (navMode == 0)
        {
            extra << "  <LookAt>\n"
                  << "    <longitude>" << navLon << "</longitude>\n"
                  << "    <latitude>" << navLat << "</latitude>\n"
                  << "    <altitude>" << navAlt << "</altitude>\n"
                  << "    <heading>" << navHeading << "</heading>\n"
                  << "    <tilt>" << navTilt << "</tilt>\n"
                  << "    <range>" << navRange << "</range>\n"
                  << "    <altitudeMode>relativeToGround</altitudeMode>\n"
                  << "  </LookAt>\n";
        }
        else
        {
            extra << "  <Placemark><name>" << navAddress << "</name></Placemark>\n";
        }
        kmlPath = geoViewServer->WriteKml("meshtool_flyto.kml", extra.str());
    }
    else if (navMode == 0)
    {
        kmlPath = ProcessManager::GenerateFlyToKML(navLon, navLat, navAlt, navHeading, navTilt, navRange);
    }
    else
    {
        kmlPath = ProcessManager::GenerateSearchKML(std::string(navAddress));
    }

    if (kmlPath.empty())
    { ShowMessage("Error", "Failed to generate KML file."); return; }

    if (!processManager->StartGoogleEarth("", kmlPath))
    {
        ShowMessage("Error",
            "Failed to launch Google Earth Pro.\n\n"
            "Ensure the proxy opengl32.dll is in:\n"
            "C:\\Program Files\\Google\\Google Earth Pro\\client\\");
        delete liveBatch; liveBatch = nullptr;
        delete captureProcessor; captureProcessor = nullptr;
        return;
    }

    // Setup auto-capture timer
    if (navAutoCapture)
    {
        m_pendingAutoCapture = true;
        m_autoCaptureTimer = (float)navCaptureDelay;
        m_autoCaptureCount = 0;
        SetStatus("Google Earth launched. Auto-capture in " + std::to_string(navCaptureDelay) + " seconds...");
    }
    else
    {
        SetStatus("Google Earth launched. Click 'Capture Frame' when ready.");
    }
}

void MeshToolUI::ActionLiveCapture()
{
    if (!processManager) { ShowMessage("Error", "Process manager not initialised."); return; }
    if (processManager->IsRunning()) { ShowMessage("Live Capture", "Google Earth Pro is already running."); return; }

    if (!processManager->CreateSharedMemory())
    { ShowMessage("Live Capture", "Failed to create shared memory for IPC."); return; }

    liveBatch = new BatchMeshData;
    liveBatch->reference_pos = g_world.reference_pos;
    liveBatch->scissor_bbox = g_world.scissor_bbox;
    liveBatch->is_google_dump = true;
    liveBatch->is_spline_mesh = false;

    delete captureProcessor;
    captureProcessor = new LiveCaptureProcessor();
    captureProcessor->SetOutputBatch(liveBatch);
    captureProcessor->m_on_frame_captured = [this](int n) {
        SetStatus(n > 0 ? "Captured " + std::to_string(n) + " meshes." : "No meshes in frame.");
    };
    captureProcessor->m_on_capture_error = [this](const std::string& e) { SetStatus("Error: " + e, 10.0f); };
    captureProcessor->m_on_connection_changed = [this](bool c) { SetStatus(c ? "Hook connected." : "Hook disconnected."); };

    std::string viewKml = geoViewServer ? geoViewServer->WriteKml("meshtool_view.kml") : std::string();
    if (!processManager->StartGoogleEarth("", viewKml))
    {
        ShowMessage("Live Capture", "Failed to launch Google Earth Pro.\nEnsure proxy opengl32.dll is in the client directory.");
        delete liveBatch; liveBatch = nullptr;
        delete captureProcessor; captureProcessor = nullptr;
    }
    else
    {
        SetStatus("Google Earth Pro launched. Navigate to area, then Capture Frame (F5).");
    }
}
