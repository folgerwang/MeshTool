#include <windows.h>
#include <cstring>
#include <cassert>
#include <fstream>
#include <string>
#include "glhook_injector.h"
#include "livecaptureprocessor.h"
#include <array>
#include <algorithm>
#include <tuple>
#include <cstdarg>
#include <cstdio>
#include "meshdata.h"
#include "worlddata.h"
#include "debugout.h"
#include <GeographicLib/LocalCartesian.hpp>
#include <GLFW/glfw3.h>

using namespace std;

// ---------------------------------------------------------------------------
// Capture diagnostics: C:\Users\Public\meshtool_capture.log
// ---------------------------------------------------------------------------
static void CapLog(const char* fmt, ...)
{
    FILE* f = fopen("C:\\Users\\Public\\meshtool_capture.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

// Google Earth's world unit is one Earth radius.
static const double kMetresPerUnit = 6378137.0;

static int g_logged_draws = 0;          // draw calls logged in the current frame
static const int kMaxLoggedDraws = 40;
static int g_meshes_from_frame = 0;

// Size in bytes of one component of a GL vertex attribute / index type; 0 if unsupported.
static uint32_t GLTypeSize(uint32_t type)
{
    switch (type)
    {
    case 0x1400: case 0x1401: return 1;   // BYTE, UNSIGNED_BYTE
    case 0x1402: case 0x1403: return 2;   // SHORT, UNSIGNED_SHORT
    case 0x1404: case 0x1405: case 0x1406: return 4;   // INT, UNSIGNED_INT, FLOAT
    case 0x140A: return 8;                // DOUBLE
    }
    return 0;
}

// One component converted to float the way GL feeds it to the shader.
static float ReadGLComponent(const char* p, uint32_t type, bool normalized)
{
    switch (type)
    {
    case 0x1400: { int8_t   v = *(const int8_t*)p;   return normalized ? (v / 127.0f < -1.0f ? -1.0f : v / 127.0f) : float(v); }
    case 0x1401: { uint8_t  v = *(const uint8_t*)p;  return normalized ? v / 255.0f : float(v); }
    case 0x1402: { int16_t  v = *(const int16_t*)p;  return normalized ? (v / 32767.0f < -1.0f ? -1.0f : v / 32767.0f) : float(v); }
    case 0x1403: { uint16_t v = *(const uint16_t*)p; return normalized ? v / 65535.0f : float(v); }
    case 0x1404: { int32_t  v = *(const int32_t*)p;  return normalized ? float(v / 2147483647.0) : float(v); }
    case 0x1405: { uint32_t v = *(const uint32_t*)p; return normalized ? float(v / 4294967295.0) : float(v); }
    case 0x1406: return *(const float*)p;
    case 0x140A: return float(*(const double*)p);
    }
    return 0.0f;
}

// Reads every vertex of an attribute stream from its buffer into `out`
// (up to 4 components each, missing ones = 0). Stride 0 means tightly packed.
static size_t ReadAttributeStream(const std::vector<char>& buf, const VertexAttrib& a, std::vector<std::array<float, 4>>& out)
{
    uint32_t comp_size = GLTypeSize(uint32_t(a.data_type));
    uint32_t comps = a.num_elements < 1 ? 1 : (a.num_elements > 4 ? 4 : a.num_elements);
    if (comp_size == 0)
        return 0;
    uint32_t elem_size = comp_size * comps;
    uint32_t stride = a.stride ? a.stride : elem_size;

    const char* p = buf.data() + a.start_offset;
    const char* end = buf.data() + buf.size();
    if (a.start_offset >= buf.size())
        return 0;
    for (; p + elem_size <= end; p += stride)
    {
        std::array<float, 4> v = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (uint32_t c = 0; c < comps; c++)
            v[c] = ReadGLComponent(p + c * comp_size, uint32_t(a.data_type), a.is_normalized != 0);
        out.push_back(v);
    }
    return out.size();
}

// ============================================================================
// Helper: convert matrix4f to matrix4d (same as in GpaDumpAnalyzeTool)
// ============================================================================
static void Matrix4fToMatrix4d(const core::matrix4f& src_mat, core::matrix4d& dst_mat)
{
    for (int r = 0; r < 4; r++)
    {
        core::vec4f row = src_mat.get_row(r);
        dst_mat.set_row(r, core::vec4d(double(row.x), double(row.y), double(row.z), double(row.w)));
    }
}

// ============================================================================
// LiveCaptureProcessor
// ============================================================================

LiveCaptureProcessor::LiveCaptureProcessor()
    : m_mapping(nullptr)
    , m_shared_mem(nullptr)
    , m_header(nullptr)
    , m_ring_base(nullptr)
    , m_event_ready(nullptr)
    , m_current_bind_texture(-1)
    , m_current_bind_buffer(-1)
    , m_current_texture_slot(0)
    , m_current_group(nullptr)
    , m_output_batch(nullptr)
    , m_has_first_matrix(false)
{
    memset(m_bind_buffer_list, -1, sizeof(m_bind_buffer_list));
}

LiveCaptureProcessor::~LiveCaptureProcessor()
{
    if (m_shared_mem) UnmapViewOfFile(m_shared_mem);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_event_ready) CloseHandle(m_event_ready);

    // Clean up texture store (except textures now owned by captured groups)
    for (auto& pair : m_texture_store)
        if (!m_textures_handed_out.count(pair.second))
            delete pair.second;
}

void LiveCaptureProcessor::processFrame(bool keepMeshes)
{
    // Open shared memory if not already open
    if (!m_shared_mem)
    {
        m_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, GLCAPTURE_SHARED_MEM_NAME);
        if (!m_mapping)
        {
            if (m_on_capture_error) m_on_capture_error("Cannot open shared memory - is Google Earth running with hook?");
            return;
        }

        m_shared_mem = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, GLCAPTURE_SHARED_MEM_SIZE);
        if (!m_shared_mem)
        {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            if (m_on_capture_error) m_on_capture_error("Cannot map shared memory");
            return;
        }

        m_header = reinterpret_cast<GLCaptureHeader*>(m_shared_mem);
        m_ring_base = reinterpret_cast<char*>(m_shared_mem) + GLCAPTURE_HEADER_SIZE;

        m_event_ready = OpenEventA(EVENT_ALL_ACCESS, FALSE, GLCAPTURE_EVENT_READY);

        if (m_on_connection_changed) m_on_connection_changed(true);
    }

    // Check if hook is connected
    if (!(m_header->status_flags & GLCAPTURE_STATUS_CONNECTED))
    {
        if (m_on_capture_error) m_on_capture_error("Hook DLL not connected");
        return;
    }

    m_last = FrameResult();
    uint32_t read_pos = m_header->read_offset;
    const uint32_t write_pos = m_header->write_offset;

    // Pass 1: find the end of the newest complete frame. More than one may be
    // queued (two captures can finish before we wake up: the ready event only
    // remembers one signal), and the newest is the one GE shows right now -
    // older ones would make every capture lag one behind. Records after the
    // last frame end belong to a capture still being written; leave them.
    uint32_t last_end = 0;
    int complete_frames = 0;
    for (uint32_t p = read_pos; p != write_pos;)
    {
        if (p >= GLCAPTURE_RING_SIZE) { p = 0; continue; }
        const GLCaptureRecord* rec = reinterpret_cast<const GLCaptureRecord*>(m_ring_base + p);
        if (rec->cmd_id == CMD_NONE) { p = 0; continue; }
        if (rec->total_size == 0) break;   // corrupt; don't spin
        p += rec->total_size;
        if (p >= GLCAPTURE_RING_SIZE) p = 0;
        if (rec->cmd_id == CMD_FRAME_END)
        {
            last_end = p;
            complete_frames++;
        }
    }
    CapLog("=== processFrame: read=%u write=%u status=0x%X, %d complete frame(s) queued\n",
           read_pos, write_pos, m_header->status_flags, complete_frames);
    if (complete_frames == 0)
        return;

    auto start_frame = [this]() {
        m_current_group = new GroupMeshData;
        g_logged_draws = 0;
        g_meshes_from_frame = 0;
        m_has_first_matrix = false;
        m_matrix_stack.clear();
        m_up_accum = core::vec3d(0.0, 0.0, 0.0);
        // m_tile_size / m_tile_key persist: the merged capture's meshes keep theirs.
    };
    start_frame();
    std::map<uint32_t, uint32_t> record_counts;

    // Pass 2: replay every queued record (GL state carries across frames) but
    // keep only the newest frame's meshes.
    while (read_pos != last_end)
    {
        if (read_pos >= GLCAPTURE_RING_SIZE) { read_pos = 0; continue; }
        const GLCaptureRecord* record = reinterpret_cast<const GLCaptureRecord*>(m_ring_base + read_pos);
        if (record->cmd_id == CMD_NONE) { read_pos = 0; continue; }
        if (record->total_size == 0) break;

        if (record->cmd_id == CMD_FRAME_END)
        {
            read_pos += record->total_size;
            if (read_pos >= GLCAPTURE_RING_SIZE) read_pos = 0;
            if (read_pos != last_end)
            {
                // An older queued frame: drop its meshes and start over.
                CapLog("  skipped an older queued frame (%d meshes)\n", int(m_current_group->meshes.size()));
                for (MeshData* mesh : m_current_group->meshes)
                {
                    m_tile_size.erase(mesh);
                    m_tile_origin.erase(mesh);
                    m_tile_key.erase(mesh);
                    delete mesh;
                }
                delete m_current_group;
                start_frame();
                record_counts.clear();
            }
            continue;
        }

        const char* payload = m_ring_base + read_pos + sizeof(GLCaptureRecord);
        record_counts[record->cmd_id]++;
        ProcessRecord(record, payload);

        read_pos += record->total_size;
        if (read_pos >= GLCAPTURE_RING_SIZE) read_pos = 0;
    }

    // Update read position
    m_header->read_offset = read_pos;
    CapLog("  records:");
    for (auto& rc : record_counts)
        CapLog(" 0x%04X x%u", rc.first, rc.second);
    CapLog("\n  buffers known=%u textures known=%u meshes built=%d%s\n",
           unsigned(m_buffer_store.size()), unsigned(m_texture_store.size()), g_meshes_from_frame,
           (m_header->status_flags & GLCAPTURE_STATUS_OVERFLOW) ? "  (hook reported RING OVERFLOW)" : "");

    // Add captured meshes to output
    int mesh_count = (int)m_current_group->meshes.size();
    if (keepMeshes && mesh_count > 0 && m_output_batch)
    {
        // Where this capture goes. Every capture is placed in one scene frame:
        // East/North/Up metres at the batch's GPS reference when GE reported
        // its view, otherwise a frame derived from the first capture's camera.
        // A capture that overlaps or borders one of the captured areas is
        // merged into it, through X (this frame's eye space -> the area's
        // reference eye space): from tiles both contain (exact) or else from
        // both GPS views. Any other capture starts a new area.
        const bool geo = m_geo_view.valid;
        CheckLookAt(m_current_group);

        // This capture's footprint in the scene when placed through X into frame f.
        auto footprint = [&](const core::matrix4d& Xm, const GroundFrame& f) {
            core::bounds3d box;
            for (MeshData* mesh : m_current_group->meshes)
            {
                if (!mesh->bbox_ws.b_valid) continue;
                for (int c = 0; c < 8; c++)
                {
                    core::vec3d p((c & 1) ? mesh->bbox_ws.bb_max.x : mesh->bbox_ws.bb_min.x,
                                  (c & 2) ? mesh->bbox_ws.bb_max.y : mesh->bbox_ws.bb_min.y,
                                  (c & 4) ? mesh->bbox_ws.bb_max.z : mesh->bbox_ws.bb_min.z);
                    box += ToGround(p, &Xm, f);
                }
            }
            return box;
        };
        // GPS placement only (shared tiles prove the overlap): does the capture
        // border the area, at a comparable level of detail?
        auto neighbours = [&](const core::bounds3d& probe_box, const Area& a) {
            const core::bounds3d& old_box = a.group->bbox_ws;
            const double kNeighbourGap = 100.0;   // metres between footprints that still count as neighbours
            double gap_x = (std::max)(probe_box.bb_min.x - old_box.bb_max.x, old_box.bb_min.x - probe_box.bb_max.x);
            double gap_y = (std::max)(probe_box.bb_min.y - old_box.bb_max.y, old_box.bb_min.y - probe_box.bb_max.y);
            // A frame grabbed while GE was still zoomed out (planet-sized
            // tiles) must not join a street-level area or vice versa. Compare
            // the finest tiles, not the footprints: a tilted view reaches the
            // horizon, so its footprint says little about the zoom.
            const double kMaxLevelGap = 5.0;   // levels of detail, each halves the tile edge
            double new_tile = FinestTile(m_current_group->meshes), old_tile = FinestTile(a.group->meshes);
            double levels = (new_tile > 0.0 && old_tile > 0.0) ? fabs(log2(new_tile / old_tile)) : 0.0;
            bool ok = probe_box.b_valid && old_box.b_valid && gap_x <= kNeighbourGap && gap_y <= kNeighbourGap &&
                      levels <= kMaxLevelGap;
            CapLog("  GPS placement vs area %d: footprint gap %.1f x %.1f m, finest tile %.1f vs %.1f m (%.1f levels) -> %s\n",
                   int(&a - m_areas.data()) + 1, (std::max)(gap_x, 0.0), (std::max)(gap_y, 0.0), new_tile, old_tile,
                   levels, ok ? "MERGE" : "no");
            return ok;
        };
        auto updateBatchBounds = [&]() {
            m_output_batch->bbox_ws.Reset();
            m_output_batch->bbox_gps.Reset();
            for (const GroupMeshData* g : m_output_batch->group_meshes)
            {
                if (g->bbox_ws.b_valid) m_output_batch->bbox_ws += g->bbox_ws;
                if (g->bbox_gps.b_valid) m_output_batch->bbox_gps += g->bbox_gps;
            }
        };

        // Which area the capture joins, and X: this frame's eye space -> that
        // area's reference eye space.
        Area* target = nullptr;
        core::matrix4d X;
        std::string placement;   // how X was found (debug view)

        // 1. Shared tiles: exact, and proof that the captures overlap, so the
        //    capture merges whatever its zoom or tilt. The best-supported area wins.
        int best_support = 0;
        for (Area& a : m_areas)
        {
            core::matrix4d Xa;
            int support = 0;
            if (RegisterToReference(m_current_group, a, Xa, support) && support > best_support)
            {
                best_support = support;
                target = &a;
                X = Xa;
            }
        }
        // GE's view in the scene frame (the batch's GPS reference), for GPS placement.
        GroundFrame geo_in_scene;
        if (geo && m_output_batch->is_georeferenced)
            geo_in_scene = GroundFrameFromGeo(m_output_batch->reference_pos.x, m_output_batch->reference_pos.y);
        if (target)
        {
            placement = "shared tiles (" + std::to_string(best_support) + ")";
            CapLog("  placement by shared tiles: %d agreeing -> MERGE into area %d\n",
                   best_support, int(target - m_areas.data()) + 1);
            if (geo_in_scene.valid && target->frame.geo)
            {
                // Both placements are available: how well does GPS agree with the exact one?
                core::matrix4d Xg = FrameMatrix(geo_in_scene) * inverse(FrameMatrix(target->frame));
                double dx = (X(3, 0) - Xg(3, 0)), dy = (X(3, 1) - Xg(3, 1)), dz = (X(3, 2) - Xg(3, 2));
                const double kDeg = 180.0 / 3.14159265358979323846;
                CapLog("  GPS vs shared-tile placement of the camera: %.2f m apart, rotation %.2f vs %.2f deg\n",
                       sqrt(dx * dx + dy * dy + dz * dz) * kMetresPerUnit,
                       atan2(Xg(0, 1), Xg(0, 0)) * kDeg, atan2(X(0, 1), X(0, 0)) * kDeg);
            }
        }
        // 2. GPS: the newest area the capture borders.
        else if (geo_in_scene.valid)
        {
            for (size_t k = m_areas.size(); k-- > 0;)
            {
                Area& a = m_areas[k];
                if (!a.frame.geo) continue;
                core::matrix4d Xa = FrameMatrix(geo_in_scene) * inverse(FrameMatrix(a.frame));
                if (neighbours(footprint(Xa, a.frame), a))
                {
                    target = &a;
                    X = Xa;
                    placement = "GPS";
                    break;
                }
            }
        }
        if (!target && !m_areas.empty())
            CapLog("  placement: no area shares tiles with it or borders it -> new area\n");

        if (target)
        {
            GroupMeshData* group = target->group;

            // Tiles the area already has are identical data: drop them.
            int duplicates = 0;
            for (size_t k = 0; k < m_current_group->meshes.size();)
            {
                MeshData* mesh = m_current_group->meshes[k];
                auto key = m_tile_key.find(mesh);
                if (key != m_tile_key.end() && target->key_mesh.count(key->second))
                {
                    m_tile_key.erase(mesh);
                    m_tile_size.erase(mesh);
                    m_tile_origin.erase(mesh);
                    delete mesh;   // never uploaded
                    m_current_group->meshes.erase(m_current_group->meshes.begin() + k);
                    duplicates++;
                    continue;
                }
                k++;
            }

            ApplyGroundFrame(m_current_group, &X, target->frame);
            CaptureInfo capture = DescribeCapture(m_current_group->meshes, &X, target->frame,
                                                  int32_t(group->captures.size()), placement);
            capture.duplicates = duplicates;
            group->captures.push_back(capture);
            HandOutTextures(m_current_group, uint32_t(group->loaded_textures.size()));
            for (core::Texture2DInfo* tex : m_current_group->loaded_textures)
                group->loaded_textures.push_back(tex);
            for (MeshData* mesh : m_current_group->meshes)
            {
                auto key = m_tile_key.find(mesh);
                if (key != m_tile_key.end())
                {
                    target->key_mesh[key->second] = mesh;
                    target->ref_tile_mv[key->second] = mesh->dumpped_matrix * X;
                }
                group->meshes.push_back(mesh);
            }
            int added = int(m_current_group->meshes.size());
            m_current_group->meshes.clear();
            m_current_group->loaded_textures.clear();
            delete m_current_group;
            m_current_group = nullptr;

            // Finer tiles from either capture now hide coarser ones from both.
            RemoveCoveredLods(*target);
            group->bbox_ws.Reset();
            for (MeshData* mesh : group->meshes)
                if (mesh->bbox_ws.b_valid) group->bbox_ws += mesh->bbox_ws;
            SeparateNoGpsAreas();
            updateBatchBounds();

            CapLog("  merged into area %d: %d tiles added, %d duplicates dropped, %u meshes total\n",
                   int(target - m_areas.data()) + 1, added, duplicates, unsigned(group->meshes.size()));
            m_last.new_data = true;
            m_last.merged = true;
            m_last.group = group;
            if (m_on_frame_captured) m_on_frame_captured(int(group->meshes.size()));
            return;
        }

        // New area. Areas are never replaced: GPS places them all in one scene
        // frame, and one captured without GPS (no true location) is set down
        // beside the rest. Their tiles stay known, so later captures can still
        // merge into any area. Only an overview (GE zoomed far out, e.g. while
        // flying in) is dropped by the next capture.
        const double kOverviewTile = 1000.0;   // metres: finest tile coarser than this = overview
        for (size_t k = 0; k < m_areas.size();)
        {
            double finest = FinestTile(m_areas[k].group->meshes);
            if (!(finest > kOverviewTile))
            {
                k++;
                continue;
            }
            CapLog("  dropping overview area %zu (finest tile %.0f m)\n", k + 1, finest);
            GroupMeshData* old = m_areas[k].group;
            for (MeshData* mesh : old->meshes)
            {
                m_tile_size.erase(mesh);
                m_tile_origin.erase(mesh);
                m_tile_key.erase(mesh);
            }
            auto& groups = m_output_batch->group_meshes;
            groups.erase(std::remove(groups.begin(), groups.end(), old), groups.end());
            m_last.discarded.push_back(old);   // the caller frees it
            m_areas.erase(m_areas.begin() + k);
        }
        if (m_output_batch->group_meshes.empty())
            m_output_batch->is_georeferenced = false;   // a fresh scene takes this capture's GPS origin

        GroundFrame frame;
        if (geo)
        {
            if (!m_output_batch->is_georeferenced)
            {
                // The scene's GPS origin is this capture's look-at point. Areas
                // captured without GPS keep their (arbitrary) place and are
                // moved aside below if they overlap.
                const double lon0 = m_geo_view.hasLookAt ? m_geo_view.laLon : m_geo_view.lookLon;
                const double lat0 = m_geo_view.hasLookAt ? m_geo_view.laLat : m_geo_view.lookLat;
                m_output_batch->reference_pos = core::vec2d(lon0, lat0);
                m_output_batch->is_georeferenced = true;
            }
            frame = GroundFrameFromGeo(m_output_batch->reference_pos.x, m_output_batch->reference_pos.y);
        }
        else
        {
            CapLog("  no GPS view from Google Earth (KML view link not loaded?): using a camera-based frame\n");
            frame = ComputeGroundFrame(m_current_group);
        }

        m_areas.push_back(Area());
        Area& area = m_areas.back();
        area.group = m_current_group;
        area.frame = frame;
        m_current_group->no_gps = !geo;
        for (MeshData* mesh : m_current_group->meshes)
        {
            auto key = m_tile_key.find(mesh);
            if (key != m_tile_key.end())
            {
                area.key_mesh[key->second] = mesh;
                area.ref_tile_mv[key->second] = mesh->dumpped_matrix;   // reference eye space = this frame's
            }
        }
        ApplyGroundFrame(m_current_group, nullptr, frame);
        m_current_group->captures.push_back(
            DescribeCapture(m_current_group->meshes, nullptr, frame, 0, geo ? "first capture (GPS)" : "first capture (no GPS)"));
        RemoveCoveredLods(area);
        HandOutTextures(m_current_group, 0);

        m_output_batch->group_meshes.push_back(m_current_group);
        m_last.group = m_current_group;
        m_current_group = nullptr;
        SeparateNoGpsAreas();
        updateBatchBounds();
        CapLog("  new area %d of %zu%s\n", int(m_areas.size()), m_areas.size(), geo ? "" : " (no GPS)");

        m_last.new_data = true;
        if (m_on_frame_captured) m_on_frame_captured(int(m_last.group->meshes.size()));
    }
    else
    {
        for (MeshData* mesh : m_current_group->meshes)
        {
            m_tile_size.erase(mesh);
            m_tile_origin.erase(mesh);
            m_tile_key.erase(mesh);
            delete mesh;
        }
        delete m_current_group;
        m_current_group = nullptr;

        if (m_on_frame_captured) m_on_frame_captured(0);
    }
}

LiveCaptureProcessor::GroundFrame LiveCaptureProcessor::GroundFrameFromGeo(double lon0, double lat0) const
{
    GroundFrame f;
    const GeoView& v = m_geo_view;
    if (!v.valid)
        return f;

    // GE's LookAt, in East/North/Up metres at the look-at point L (at altitude
    // 0, which is what `range` is measured from): the camera sits `range`
    // away, tilted `tilt` from vertical, looking along `heading`; GE cameras
    // don't roll, so screen-right stays horizontal. A link written by an older
    // MeshTool has no LookAt point; the terrain point is the same for tilt 0.
    const double la_lon = v.hasLookAt ? v.laLon : v.lookLon;
    const double la_lat = v.hasLookAt ? v.laLat : v.lookLat;
    const double deg = 3.14159265358979323846 / 180.0;
    const double h = v.heading * deg, t = v.tilt * deg;
    const core::vec3d right(cos(h), -sin(h), 0.0);
    const core::vec3d horiz(sin(h), cos(h), 0.0);
    const core::vec3d cam = core::vec3d(-sin(t) * horiz.x, -sin(t) * horiz.y, cos(t)) * v.range;
    const core::vec3d fwd(sin(t) * horiz.x, sin(t) * horiz.y, -cos(t));
    const core::vec3d eye_up = cross(right, fwd);
    const core::vec3d eye_z = fwd * -1.0;                       // eye space looks down -Z

    // L's ENU -> scene ENU at (lat0, lon0, 0): L's position T and rotation M.
    GeographicLib::LocalCartesian scene(lat0, lon0, 0.0);
    double tx, ty, tz;
    std::vector<double> M(9);
    scene.Forward(la_lat, la_lon, 0.0, tx, ty, tz, M);
    auto rotate = [&M](const core::vec3d& p) {
        return core::vec3d(M[0] * p.x + M[1] * p.y + M[2] * p.z,
                           M[3] * p.x + M[4] * p.y + M[5] * p.z,
                           M[6] * p.x + M[7] * p.y + M[8] * p.z);
    };

    // ground = metres * Rot * p_eye + trans, Rot's columns = eye axes in scene ENU.
    core::vec3d c0 = rotate(right), c1 = rotate(eye_up), c2 = rotate(eye_z);
    core::vec3d trans = core::vec3d(tx, ty, tz) + rotate(cam);
    f.x_axis = core::vec3d(c0.x, c1.x, c2.x);   // rows of Rot, as eye-space vectors
    f.y_axis = core::vec3d(c0.y, c1.y, c2.y);
    f.up     = core::vec3d(c0.z, c1.z, c2.z);
    // ground = metres * Rot * (p - origin)  =>  origin = -Rot^T * trans / metres,
    // and Rot^T's rows are Rot's columns.
    f.origin = core::vec3d(dot(c0, trans), dot(c1, trans), dot(c2, trans)) * (-1.0 / kMetresPerUnit);
    f.valid = true;
    f.geo = true;

    // Cross-check against GE's own camera position (its altitude reference
    // may differ, so horizontal and vertical are reported separately).
    double cx, cy, cz;
    scene.Forward(v.camLat, v.camLon, v.camAlt, cx, cy, cz);
    CapLog("  GPS view: look-at %.7f, %.7f%s  range %.1f m  heading %.2f  tilt %.2f  terrain %.1f m  (age %.1f s)\n"
           "  GPS camera check: computed vs GE camera differ %.2f m horizontally, %.2f m vertically\n",
           la_lat, la_lon, v.hasLookAt ? "" : " (terrain point: old view link)", v.range, v.heading, v.tilt,
           v.lookAlt, glfwGetTime() - v.time,
           sqrt((cx - trans.x) * (cx - trans.x) + (cy - trans.y) * (cy - trans.y)), cz - trans.z);
    return f;
}

core::matrix4d LiveCaptureProcessor::FrameMatrix(const GroundFrame& f)
{
    // [g, 1] = [p, 1] * G with g = Rot * (p - origin)  (row vectors).
    core::matrix4d G;
    const core::vec3d ax[3] = { f.x_axis, f.y_axis, f.up };
    for (int k = 0; k < 3; k++)
    {
        for (int i = 0; i < 3; i++)
            G(k, i) = ax[i][k];
        G(k, 3) = 0.0;
    }
    for (int i = 0; i < 3; i++)
        G(3, i) = -dot(f.origin, ax[i]);
    G(3, 3) = 1.0;
    return G;
}

void LiveCaptureProcessor::CheckLookAt(const GroupMeshData* group) const
{
    // GE's look-at point is the terrain at screen centre, `range` metres down
    // the camera's -Z axis. Find the captured vertex closest to that ray.
    if (!m_geo_view.valid || !group)
        return;
    double best_angle = 1e9, best_depth = 0.0;
    for (const MeshData* mesh : group->meshes)
    {
        if (!mesh->vertex_list) continue;
        for (int i = 0; i < mesh->num_vertex; i++)
        {
            const core::vec3f& v = mesh->vertex_list[i];
            core::vec3d p = core::vec3d(v.x, v.y, v.z) + mesh->translation;
            if (p.z >= 0.0) continue;
            double angle = sqrt(p.x * p.x + p.y * p.y) / -p.z;
            if (angle < best_angle) { best_angle = angle; best_depth = -p.z * kMetresPerUnit; }
        }
    }
    if (best_angle >= 0.01)
        return;

    // Where GE says the screen-centre terrain is, seen from the camera that
    // GE's LookAt describes.
    const GeoView& v = m_geo_view;
    const double la_lon = v.hasLookAt ? v.laLon : v.lookLon;
    const double la_lat = v.hasLookAt ? v.laLat : v.lookLat;
    const double deg = 3.14159265358979323846 / 180.0;
    const double h = v.heading * deg, t = v.tilt * deg;
    GeographicLib::LocalCartesian at(la_lat, la_lon, 0.0);
    double ex, ey, ez;
    at.Forward(v.lookLat, v.lookLon, v.lookAlt, ex, ey, ez);
    double cx = -sin(t) * sin(h) * v.range, cy = -sin(t) * cos(h) * v.range, cz = cos(t) * v.range;
    double expected = sqrt((ex - cx) * (ex - cx) + (ey - cy) * (ey - cy) + (ez - cz) * (ez - cz));
    CapLog("  GPS look-at check: captured surface at screen centre %.2f m away, GE view says %.2f m (diff %.2f m)\n",
           best_depth, expected, best_depth - expected);
}

void LiveCaptureProcessor::ResetMerge()
{
    m_areas.clear();
    m_tile_size.clear();
    m_tile_origin.clear();
    m_tile_key.clear();
}

void LiveCaptureProcessor::HandOutTextures(GroupMeshData* group, uint32_t index_offset)
{
    // Hand the group the textures its meshes sample. Meshes recorded the GL
    // texture id; remap it to an index into the (eventual) loaded_textures,
    // which is what the renderer and exporters expect.
    std::map<uint32_t, uint32_t> gl_to_index;
    int textured = 0;
    for (MeshData* mesh : group->meshes)
    {
        uint32_t gl_id = mesh->idx_in_texture_list;
        mesh->idx_in_texture_list = INVALID_VALUE;
        auto st = m_texture_store.find(gl_id);
        if (gl_id == 0 || st == m_texture_store.end() || !st->second->m_levelCount ||
            !st->second->m_mips[0].m_imageData)
            continue;
        auto it = gl_to_index.find(gl_id);
        if (it == gl_to_index.end())
        {
            it = gl_to_index.emplace(gl_id, index_offset + uint32_t(group->loaded_textures.size())).first;
            group->loaded_textures.push_back(st->second);
            m_textures_handed_out.insert(st->second);
        }
        mesh->idx_in_texture_list = it->second;
        textured++;
    }
    // The group owns those now; a later capture re-sends them and must get
    // fresh objects rather than overwrite this group's.
    for (auto& g : gl_to_index)
        m_texture_store.erase(g.first);
    CapLog("  textures: %u used, %d of %u meshes textured\n",
           unsigned(gl_to_index.size()), textured, unsigned(group->meshes.size()));
}

bool LiveCaptureProcessor::RegisterToReference(const GroupMeshData* group, const Area& area, core::matrix4d& X,
                                               int& support) const
{
    // Each tile both captures contain gives the camera-to-camera transform:
    // p_eye_new * inv(MV_new) = p_tile, p_tile * MV_ref = p_eye_ref.
    std::vector<core::matrix4d> candidates;
    std::vector<std::pair<core::matrix4d, core::matrix4d>> pairs;   // (MV_new, MV_ref) per candidate
    for (const MeshData* mesh : group->meshes)
    {
        auto key = m_tile_key.find(const_cast<MeshData*>(mesh));
        if (key == m_tile_key.end()) continue;
        auto ref = area.ref_tile_mv.find(key->second);
        if (ref == area.ref_tile_mv.end()) continue;
        candidates.push_back(inverse(mesh->dumpped_matrix) * ref->second);
        pairs.emplace_back(mesh->dumpped_matrix, ref->second);
    }

    // Consensus on where the new camera sits in the reference eye space
    // (translation row); a hash collision or reused tile can't win the vote.
    const double kTolerance = 3e-7;   // Earth radii, about 2 m
    int best = -1, best_votes = 0;
    for (size_t i = 0; i < candidates.size(); i++)
    {
        int votes = 0;
        for (size_t j = 0; j < candidates.size(); j++)
        {
            double dx = candidates[i](3, 0) - candidates[j](3, 0);
            double dy = candidates[i](3, 1) - candidates[j](3, 1);
            double dz = candidates[i](3, 2) - candidates[j](3, 2);
            if (dx * dx + dy * dy + dz * dz < kTolerance * kTolerance)
                votes++;
        }
        if (votes > best_votes) { best_votes = votes; best = int(i); }
    }
    support = best_votes;
    if (best < 0 || best_votes < 3)
        return false;
    X = candidates[size_t(best)];

    // Check: every shared tile's corners (tile space is the unit cube),
    // placed through X, against where the reference capture has them. The
    // vote only compared camera positions; this also catches a wrong rotation.
    std::vector<double> residual;   // metres, worst corner per agreeing tile
    const core::vec3d bx = core::vec3d(candidates[size_t(best)](3, 0), candidates[size_t(best)](3, 1),
                                       candidates[size_t(best)](3, 2));
    for (size_t i = 0; i < candidates.size(); i++)
    {
        core::vec3d d = core::vec3d(candidates[i](3, 0), candidates[i](3, 1), candidates[i](3, 2)) - bx;
        if (dot(d, d) >= kTolerance * kTolerance)
            continue;
        double worst = 0.0;
        for (int c = 0; c < 8; c++)
        {
            core::vec4d corner((c & 1) ? 1.0 : 0.0, (c & 2) ? 1.0 : 0.0, (c & 4) ? 1.0 : 0.0, 1.0);
            core::vec4d a = corner * pairs[i].first * X;
            core::vec4d b = corner * pairs[i].second;
            core::vec3d e(a.x - b.x, a.y - b.y, a.z - b.z);
            worst = (std::max)(worst, length(e) * kMetresPerUnit);
        }
        residual.push_back(worst);
    }
    std::sort(residual.begin(), residual.end());
    // Rotation of X about the eye's view axis (in-plane rotation between the two cameras).
    const double yaw = atan2(X(0, 1), X(0, 0)) * 180.0 / 3.14159265358979323846;
    CapLog("  shared-tile check: %zu tiles, corner error median %.3f m, 90%% %.3f m, max %.3f m; "
           "camera rotation about view axis %.2f deg\n",
           residual.size(), residual.empty() ? 0.0 : residual[residual.size() / 2],
           residual.empty() ? 0.0 : residual[residual.size() * 9 / 10],
           residual.empty() ? 0.0 : residual.back(), yaw);
    return true;
}

LiveCaptureProcessor::GroundFrame LiveCaptureProcessor::ComputeGroundFrame(const GroupMeshData* group) const
{
    GroundFrame f;
    if (!group || !group->bbox_ws.b_valid)
        return f;

    // Eye space: GE camera at the origin, looking down -Z, screen-up = +Y.
    f.origin = group->bbox_ws.GetCentroid();
    f.up = core::vec3d(0.0, 1.0, 0.0);
    double up_len = length(m_up_accum);
    if (up_len > 0.0)
    {
        f.up = m_up_accum / up_len;
        if (dot(f.up, f.origin) > 0.0)   // the camera (eye-space origin) is above the ground
            f.up = f.up * -1.0;
    }

    // Y axis: screen-up laid onto the ground plane, i.e. "away from the camera"
    // when GE is tilted and screen-up when it looks straight down.
    f.y_axis = core::vec3d(0.0, 1.0, 0.0) - f.up * f.up.y;
    if (length(f.y_axis) < 1e-6)
        f.y_axis = core::vec3d(0.0, 0.0, -1.0) - f.up * -f.up.z;
    f.y_axis = normalize(f.y_axis);
    f.x_axis = cross(f.y_axis, f.up);
    f.valid = true;

    CapLog("  ground frame: up=(%.4f %.4f %.4f) origin=(%.6g %.6g %.6g) units\n",
           f.up.x, f.up.y, f.up.z, f.origin.x, f.origin.y, f.origin.z);
    return f;
}

core::vec3d LiveCaptureProcessor::ToGround(const core::vec3d& p_eye, const core::matrix4d* X, const GroundFrame& f)
{
    core::vec3d p = p_eye;
    if (X)
    {
        core::vec4d q = core::vec4d(p.x, p.y, p.z, 1.0) * (*X);
        p = core::vec3d(q.x, q.y, q.z);
    }
    core::vec3d d = p - f.origin;
    return core::vec3d(dot(d, f.x_axis), dot(d, f.y_axis), dot(d, f.up)) * kMetresPerUnit;
}

CaptureInfo LiveCaptureProcessor::DescribeCapture(const std::vector<MeshData*>& meshes, const core::matrix4d* X,
                                                  const GroundFrame& f, int32_t capture_id,
                                                  const std::string& placement) const
{
    CaptureInfo info;
    info.placement = placement;
    info.tiles_added = int(meshes.size());
    for (MeshData* mesh : meshes)
    {
        mesh->capture_id = capture_id;
        if (mesh->bbox_ws.b_valid)
            info.footprint += mesh->bbox_ws;
    }

    // GE's camera is the eye-space origin, looking down -Z.
    info.eye = ToGround(core::vec3d(0.0, 0.0, 0.0), X, f);
    core::vec3d dir = ToGround(core::vec3d(0.0, 0.0, -1e-5), X, f) - info.eye;   // ~64 m ahead
    double len = length(dir);
    dir = len > 0.0 ? dir / len : core::vec3d(0.0, 0.0, -1.0);

    // Look target: where that ray meets the capture's mid height, or (looking
    // at the horizon) a point as far out as the capture reaches.
    const double ground_z = info.footprint.b_valid ? info.footprint.GetCentroid().z : 0.0;
    double reach = info.footprint.b_valid ? length(info.footprint.GetDiagonal()) * 0.5 : 100.0;
    if (dir.z < -0.02)
        reach = (std::min)((info.eye.z - ground_z) / -dir.z, reach * 4.0);
    info.target = info.eye + dir * reach;

    CapLog("  capture #%d (%s): camera %.1f, %.1f, %.1f  looking at %.1f, %.1f, %.1f  %d tiles\n",
           capture_id + 1, placement.c_str(), info.eye.x, info.eye.y, info.eye.z,
           info.target.x, info.target.y, info.target.z, info.tiles_added);
    return info;
}

void LiveCaptureProcessor::ApplyGroundFrame(GroupMeshData* group, const core::matrix4d* X, const GroundFrame& f)
{
    if (!group || !f.valid)
        return;

    auto to_ground = [&](const core::vec3d& p) { return ToGround(p, X, f); };

    group->bbox_ws.Reset();
    for (MeshData* mesh : group->meshes)
    {
        if (!mesh || !mesh->vertex_list || mesh->num_vertex <= 0)
            continue;
        if (m_tile_size.count(mesh))
        {
            core::vec4d o = mesh->dumpped_matrix.get_row(3);   // tile-space origin in eye space
            m_tile_origin[mesh] = to_ground(core::vec3d(o.x, o.y, o.z));
        }
        uint32_t n = uint32_t(mesh->num_vertex);
        std::vector<core::vec3d> pos(n);
        mesh->bbox_ws.Reset();
        for (uint32_t i = 0; i < n; i++)
        {
            const core::vec3f& v = mesh->vertex_list[i];
            pos[i] = to_ground(core::vec3d(v.x, v.y, v.z) + mesh->translation);
            mesh->bbox_ws += pos[i];
        }
        mesh->translation = mesh->bbox_ws.GetCentroid();
        for (uint32_t i = 0; i < n; i++)
            mesh->vertex_list[i] = core::vec3f(pos[i] - mesh->translation);
        group->bbox_ws += mesh->bbox_ws;
    }

    if (group->bbox_ws.b_valid)
    {
        core::vec3d d = group->bbox_ws.GetDiagonal();
        CapLog("  ground frame extent: %.1f x %.1f x %.1f m\n", d.x, d.y, d.z);
    }
}
void LiveCaptureProcessor::ShiftArea(Area& area, const core::vec3d& d)
{
    GroupMeshData* g = area.group;
    auto shift = [&d](core::bounds3d& b) {
        if (b.b_valid) { b.bb_min = b.bb_min + d; b.bb_max = b.bb_max + d; }
    };
    for (MeshData* mesh : g->meshes)
    {
        mesh->translation = mesh->translation + d;   // vertices are relative to it: no re-upload
        shift(mesh->bbox_ws);
        auto o = m_tile_origin.find(mesh);
        if (o != m_tile_origin.end()) o->second = o->second + d;
    }
    shift(g->bbox_ws);
    for (SceneObject& obj : g->objects)
        shift(obj.bbox_ws);
    for (CaptureInfo& c : g->captures)
    {
        c.eye = c.eye + d;
        c.target = c.target + d;
        shift(c.footprint);
    }
    // Later captures placed into this area land shifted too.
    area.frame.origin = area.frame.origin -
        (area.frame.x_axis * d.x + area.frame.y_axis * d.y + area.frame.up * d.z) * (1.0 / kMetresPerUnit);
}

void LiveCaptureProcessor::SeparateNoGpsAreas()
{
    // GPS areas stay where GPS put them; each no-GPS area (no true location)
    // that overlaps what is placed so far moves to its +X side.
    const double kGap = 50.0;   // metres between a moved area and the rest
    core::bounds3d placed;
    for (const Area& a : m_areas)
        if (!a.group->no_gps && a.group->bbox_ws.b_valid)
            placed += a.group->bbox_ws;
    for (Area& a : m_areas)
    {
        const core::bounds3d& b = a.group->bbox_ws;
        if (!a.group->no_gps || !b.b_valid)
            continue;
        bool overlaps = placed.b_valid && b.bb_min.x < placed.bb_max.x + kGap && b.bb_max.x > placed.bb_min.x - kGap &&
                        b.bb_min.y < placed.bb_max.y + kGap && b.bb_max.y > placed.bb_min.y - kGap;
        if (overlaps)
        {
            core::vec3d d(placed.bb_max.x + kGap - b.bb_min.x,
                          (placed.bb_min.y + placed.bb_max.y - b.bb_min.y - b.bb_max.y) * 0.5, 0.0);
            CapLog("  no-GPS area %d moved %.0f m east, %.0f m north to sit beside the rest\n",
                   int(&a - m_areas.data()) + 1, d.x, d.y);
            ShiftArea(a, d);
        }
        placed += a.group->bbox_ws;
    }
}

double LiveCaptureProcessor::FinestTile(const std::vector<MeshData*>& meshes) const
{
    double finest = 0.0;
    for (MeshData* mesh : meshes)
    {
        auto it = m_tile_size.find(mesh);
        if (it != m_tile_size.end() && it->second > 0.0 && (finest == 0.0 || it->second < finest))
            finest = it->second;
    }
    return finest;
}

void LiveCaptureProcessor::RemoveCoveredLods(Area& area_info)
{
    GroupMeshData* group = area_info.group;
    if (!group)
        return;

    struct Item { MeshData* mesh; double size; int level; };
    std::vector<Item> items;
    core::bounds3d area;
    for (MeshData* mesh : group->meshes)
    {
        auto it = m_tile_size.find(mesh);
        if (it == m_tile_size.end() || !(it->second > 0.0) || !mesh->vertex_list ||
            mesh->draw_call_list.empty() || !mesh->draw_call_list[0].is_ge_mesh() || !mesh->bbox_ws.b_valid)
            continue;
        items.push_back({ mesh, it->second, int(floor(log2(it->second) + 0.5)) });
        area += mesh->bbox_ws;
    }
    if (items.size() < 2)
        return;
    // Stable: within a level, tiles keep the group's order (older captures
    // first), so of two copies of a tile the one already shown survives.
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.size < b.size; });

    // Ground coverage grid (XY, metres), fine enough for the finest tiles.
    double cell = items.front().size / 8.0;
    const double minX = area.bb_min.x, minY = area.bb_min.y;
    const double spanX = area.bb_max.x - minX, spanY = area.bb_max.y - minY;
    const double kMaxCells = 4096.0;
    cell = (std::max)(cell, (std::max)(spanX, spanY) / kMaxCells);
    const int nx = (std::max)(1, int(ceil(spanX / cell)) + 1);
    const int ny = (std::max)(1, int(ceil(spanY / cell)) + 1);
    std::vector<uint8_t> covered(size_t(nx) * size_t(ny), 0);

    auto cellIndex = [&](double x, double y) -> int64_t {
        int cx = int((x - minX) / cell), cy = int((y - minY) / cell);
        if (cx < 0 || cy < 0 || cx >= nx || cy >= ny) return -1;
        return int64_t(cy) * nx + cx;
    };

    // Visits the grid cells whose centres lie inside the triangle's XY
    // projection; a triangle too thin to contain any (e.g. a wall) visits the
    // cell under its centroid instead.
    auto forEachCell = [&](const core::vec3d& a, const core::vec3d& b, const core::vec3d& c, auto&& fn) {
        double x0 = (std::min)({ a.x, b.x, c.x }), x1 = (std::max)({ a.x, b.x, c.x });
        double y0 = (std::min)({ a.y, b.y, c.y }), y1 = (std::max)({ a.y, b.y, c.y });
        int cx0 = (std::max)(0, int(floor((x0 - minX) / cell - 0.5)));
        int cx1 = (std::min)(nx - 1, int(ceil((x1 - minX) / cell - 0.5)));
        int cy0 = (std::max)(0, int(floor((y0 - minY) / cell - 0.5)));
        int cy1 = (std::min)(ny - 1, int(ceil((y1 - minY) / cell - 0.5)));
        double area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        int visited = 0;
        if (area2 != 0.0)
        {
            for (int cy = cy0; cy <= cy1; cy++)
                for (int cx = cx0; cx <= cx1; cx++)
                {
                    double px = minX + (cx + 0.5) * cell, py = minY + (cy + 0.5) * cell;
                    double w0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
                    double w1 = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x);
                    double w2 = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x);
                    bool inside = area2 > 0.0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                    if (inside)
                    {
                        fn(int64_t(cy) * nx + cx);
                        visited++;
                    }
                }
        }
        if (visited == 0)
        {
            int64_t ci = cellIndex((a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0);
            if (ci >= 0) fn(ci);
        }
    };

    auto worldPos = [](const MeshData* m, uint32_t i) {
        const core::vec3f& v = m->vertex_list[i];
        return core::vec3d(v.x, v.y, v.z) + m->translation;
    };

    // Same quadtree node captured by two captures (data not byte-identical,
    // e.g. split over other textures): keep the older capture's meshes. A
    // node is its tile origin at this level; within one capture a node's
    // meshes are all kept (GE draws one per texture).
    const double kNodeQuantum = 0.5;   // metres; one node's origin agrees across captures to far less
    auto nodeKey = [&](const core::vec3d& o) {
        return std::make_tuple(int64_t(floor(o.x / kNodeQuantum)), int64_t(floor(o.y / kNodeQuantum)),
                               int64_t(floor(o.z / kNodeQuantum)));
    };

    size_t tris_before = 0, tris_after = 0;
    int same_level_copies = 0;
    for (size_t i = 0; i < items.size();)
    {
        size_t j = i;
        while (j < items.size() && items[j].level == items[i].level)
            j++;

        // Filter this level against everything finer, then add it to the coverage.
        std::map<std::tuple<int64_t, int64_t, int64_t>, int32_t> node_capture;   // node -> capture that owns it
        for (size_t k = i; k < j; k++)
        {
            MeshData* m = items[k].mesh;
            DrawCallInfo& dc = m->draw_call_list[0];
            int n = dc.get_index_count();

            auto origin = m_tile_origin.find(m);
            if (origin != m_tile_origin.end() && m->capture_id >= 0)
            {
                auto owner = node_capture.emplace(nodeKey(origin->second), m->capture_id).first;
                if (owner->second != m->capture_id)
                {
                    tris_before += size_t(n / 3);
                    dc.clear_index_buffer();   // emptied: removed below
                    same_level_copies++;
                    continue;
                }
            }

            std::vector<uint32_t> kept;
            kept.reserve(size_t(n));
            for (int t = 0; t + 2 < n; t += 3)
            {
                uint32_t i0 = dc.get_index(t), i1 = dc.get_index(t + 1), i2 = dc.get_index(t + 2);
                tris_before++;
                if (i == 0)   // the finest level is only thinned of copies
                {
                    kept.insert(kept.end(), { i0, i1, i2 });
                    continue;
                }
                int total = 0, hit = 0;
                forEachCell(worldPos(m, i0), worldPos(m, i1), worldPos(m, i2),
                            [&](int64_t ci) { total++; hit += covered[size_t(ci)]; });
                // Keep unless finer tiles cover all of it: removing a partly
                // covered triangle leaves a hole. Where it overlaps a finer
                // tile, the renderer draws the finer one in front (LOD depth order).
                if (total == 0 || hit < total)
                    kept.insert(kept.end(), { i0, i1, i2 });
            }
            if (kept.size() != size_t(n))
            {
                dc.clear_index_buffer();
                for (uint32_t idx : kept)
                    dc.add_index(idx);
                if (!kept.empty())
                    m_last.modified.push_back(m);   // its GPU copy is stale now
            }
            tris_after += kept.size() / 3;
        }
        for (size_t k = i; k < j; k++)
        {
            MeshData* m = items[k].mesh;
            const DrawCallInfo& dc = m->draw_call_list[0];
            for (int t = 0; t + 2 < dc.get_index_count(); t += 3)
                forEachCell(worldPos(m, dc.get_index(t)), worldPos(m, dc.get_index(t + 1)), worldPos(m, dc.get_index(t + 2)),
                            [&](int64_t ci) { covered[size_t(ci)] = 1; });
        }
        i = j;
    }

    // Drop meshes left empty; shrink the bounds of the rest to what remains.
    int removed_meshes = 0;
    group->bbox_ws.Reset();
    for (size_t k = 0; k < group->meshes.size();)
    {
        MeshData* m = group->meshes[k];
        if (m_tile_size.count(m) && !m->draw_call_list.empty() && m->draw_call_list[0].is_ge_mesh())
        {
            const DrawCallInfo& dc = m->draw_call_list[0];
            if (dc.get_index_count() == 0)
            {
                // The caller frees it: it may already have GPU data.
                auto key = m_tile_key.find(m);
                if (key != m_tile_key.end())
                {
                    auto km = area_info.key_mesh.find(key->second);
                    if (km != area_info.key_mesh.end() && km->second == m)
                        area_info.key_mesh.erase(km);   // a later capture may bring it back; the LOD filter will judge again
                    m_tile_key.erase(key);
                }
                m_tile_size.erase(m);
                m_tile_origin.erase(m);
                m_last.removed.push_back(m);
                group->meshes.erase(group->meshes.begin() + k);
                removed_meshes++;
                continue;
            }
            m->bbox_ws.Reset();
            for (int t = 0; t < dc.get_index_count(); t++)
                m->bbox_ws += worldPos(m, dc.get_index(t));
        }
        if (m->bbox_ws.b_valid)
            group->bbox_ws += m->bbox_ws;
        k++;
    }

    CapLog("  LOD filter: levels %d..%d (tile %.1f..%.1f m), %zu -> %zu triangles, %d meshes removed "
           "(%d same-level copies) (grid %dx%d, cell %.2f m)\n",
           items.front().level, items.back().level, items.front().size, items.back().size,
           tris_before, tris_after, removed_meshes, same_level_copies, nx, ny, cell);
}

void LiveCaptureProcessor::ProcessRecord(const GLCaptureRecord* record, const char* payload)
{
    switch (record->cmd_id)
    {
    case CMD_BIND_BUFFER:
        HandleBindBuffer(reinterpret_cast<const CmdBindBuffer*>(payload));
        break;
    case CMD_BUFFER_DATA:
        HandleBufferData(reinterpret_cast<const CmdBufferData*>(payload),
                         payload + sizeof(CmdBufferData));
        break;
    case CMD_BUFFER_SUB_DATA:
        HandleBufferSubData(reinterpret_cast<const CmdBufferSubData*>(payload),
                            payload + sizeof(CmdBufferSubData));
        break;
    case CMD_GEN_BUFFERS:
        HandleGenBuffers(reinterpret_cast<const CmdGenBuffers*>(payload),
                         reinterpret_cast<const uint32_t*>(payload + sizeof(CmdGenBuffers)));
        break;
    case CMD_VERTEX_ATTRIB_PTR:
    case CMD_VERTEX_ATTRIB_IPTR:
        HandleVertexAttribPointer(reinterpret_cast<const CmdVertexAttribPointer*>(payload));
        break;
    case CMD_ENABLE_VERTEX_ATTRIB:
        HandleEnableVertexAttrib(reinterpret_cast<const CmdEnableDisableVertexAttrib*>(payload));
        break;
    case CMD_DISABLE_VERTEX_ATTRIB:
        HandleDisableVertexAttrib(reinterpret_cast<const CmdEnableDisableVertexAttrib*>(payload));
        break;
    case CMD_BIND_TEXTURE:
        HandleBindTexture(reinterpret_cast<const CmdBindTexture*>(payload));
        break;
    case CMD_ACTIVE_TEXTURE:
        HandleActiveTexture(reinterpret_cast<const CmdActiveTexture*>(payload));
        break;
    case CMD_TEX_IMAGE_2D:
        HandleTexImage2D(reinterpret_cast<const CmdTexImage2D*>(payload),
                         payload + sizeof(CmdTexImage2D));
        break;
    case CMD_COMPRESSED_TEX_IMAGE_2D:
        HandleCompressedTexImage2D(reinterpret_cast<const CmdCompressedTexImage2D*>(payload),
                                    payload + sizeof(CmdCompressedTexImage2D));
        break;
    case CMD_UNIFORM_MATRIX_4FV:
        HandleUniformMatrix4fv(reinterpret_cast<const CmdUniformMatrix4fv*>(payload));
        break;
    case CMD_DRAW_ELEMENTS:
        HandleDrawElements(reinterpret_cast<const CmdDrawElements*>(payload), payload);
        break;
    case CMD_DRAW_ARRAYS:
        HandleDrawArrays(reinterpret_cast<const CmdDrawArrays*>(payload), payload);
        break;
    case CMD_USE_PROGRAM:
        HandleUseProgram(reinterpret_cast<const CmdUseProgram*>(payload));
        break;
    case CMD_MATRIX_MODE:
        m_matrix_mode = *reinterpret_cast<const uint32_t*>(payload);
        break;
    case CMD_LOAD_MATRIX_F:
    {
        const CmdLoadMatrixf* cmd = reinterpret_cast<const CmdLoadMatrixf*>(payload);
        core::matrix4f mat;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                mat(r, c) = cmd->matrix[r * 4 + c];
        if (cmd->mode == 0x1700)        // GL_MODELVIEW
        {
            m_modelview = mat;
            m_has_modelview = true;
        }
        else if (cmd->mode == 0x1701)   // GL_PROJECTION
            m_projection = mat;
        break;
    }
    default:
        break;
    }
}

// ============================================================================
// Command handlers - mirror GpaDumpAnalyzeTool's state machine
// ============================================================================

void LiveCaptureProcessor::HandleBindBuffer(const CmdBindBuffer* cmd)
{
    int32_t idx = GetBindBufferTypeIndex(cmd->target);
    if (idx >= 0)
    {
        m_bind_buffer_list[idx] = cmd->buffer;
        m_render_states.binding_buffer_list[idx] = cmd->buffer;
    }
    m_current_bind_buffer = cmd->buffer;
}

void LiveCaptureProcessor::HandleBufferData(const CmdBufferData* cmd, const char* data)
{
    uint32_t buf_id = cmd->buffer_id;
    if (buf_id == 0) return;

    // Store the buffer data
    auto& buf = m_buffer_store[buf_id];
    buf.resize(cmd->size);
    if (data && cmd->size > 0)
        memcpy(buf.data(), data, cmd->size);
}

void LiveCaptureProcessor::HandleBufferSubData(const CmdBufferSubData* cmd, const char* data)
{
    uint32_t buf_id = cmd->buffer_id;
    if (buf_id == 0) return;

    auto it = m_buffer_store.find(buf_id);
    if (it == m_buffer_store.end()) return;

    auto& buf = it->second;
    if (cmd->offset + cmd->size <= (uint32_t)buf.size() && data)
        memcpy(buf.data() + cmd->offset, data, cmd->size);
}

void LiveCaptureProcessor::HandleGenBuffers(const CmdGenBuffers* cmd, const uint32_t* ids)
{
    // Pre-create empty entries
    for (uint32_t i = 0; i < cmd->count; i++)
        m_buffer_store[ids[i]]; // creates empty vector
}

void LiveCaptureProcessor::HandleVertexAttribPointer(const CmdVertexAttribPointer* cmd)
{
    if (cmd->index < 256)
    {
        m_render_states.m_vertexStream[cmd->index].is_enabled =
            m_render_states.m_vertexStream[cmd->index].is_enabled; // preserve enabled state
        m_render_states.m_vertexStream[cmd->index].data_buffer_obj = cmd->bound_buffer;
        m_render_states.m_vertexStream[cmd->index].num_elements = cmd->size;
        m_render_states.m_vertexStream[cmd->index].data_type = (DataType)cmd->type;
        m_render_states.m_vertexStream[cmd->index].is_normalized = cmd->normalized;
        m_render_states.m_vertexStream[cmd->index].stride = cmd->stride;
        m_render_states.m_vertexStream[cmd->index].start_offset = cmd->offset;
    }
}

void LiveCaptureProcessor::HandleEnableVertexAttrib(const CmdEnableDisableVertexAttrib* cmd)
{
    if (cmd->index < 256)
        m_render_states.m_vertexStream[cmd->index].is_enabled = 1;
}

void LiveCaptureProcessor::HandleDisableVertexAttrib(const CmdEnableDisableVertexAttrib* cmd)
{
    if (cmd->index < 256)
        m_render_states.m_vertexStream[cmd->index].is_enabled = 0;
}

void LiveCaptureProcessor::HandleBindTexture(const CmdBindTexture* cmd)
{
    m_current_bind_texture = cmd->texture;

    // Track which texture is bound to which slot
    if (m_current_texture_slot < 256)
    {
        // Find or create index in texture list
        auto it = m_texture_store.find(cmd->texture);
        if (it != m_texture_store.end())
        {
            // Find index of this texture in the group's loaded_textures (if any)
            // For now, store the GL object ID as index
            m_render_states.m_textureSlot[m_current_texture_slot].index_in_list = cmd->texture;
        }
    }
}

void LiveCaptureProcessor::HandleActiveTexture(const CmdActiveTexture* cmd)
{
    m_current_texture_slot = cmd->texture - 0x84c0; // GL_TEXTURE0
}

void LiveCaptureProcessor::HandleTexImage2D(const CmdTexImage2D* cmd, const char* data)
{
    uint32_t tex_id = cmd->texture_id;
    if (tex_id == 0) return;

    auto it = m_texture_store.find(tex_id);
    core::Texture2DInfo* tex = nullptr;
    if (it == m_texture_store.end())
    {
        tex = new core::Texture2DInfo();
        tex->m_objectId = tex_id;
        tex->m_levelCount = 0;
        m_texture_store[tex_id] = tex;
    }
    else
    {
        tex = it->second;
    }

    tex->m_internalFormat = cmd->internalformat;
    tex->m_format = cmd->format;
    tex->m_type = cmd->type;

    uint32_t level = cmd->level;
    if (level < 15)
    {
        tex->m_mips[level].m_width = cmd->width;
        tex->m_mips[level].m_height = cmd->height;
        tex->m_mips[level].m_size = cmd->data_size;

        if (cmd->data_size > 0 && data)
        {
            tex->m_mips[level].m_imageData = make_unique<char[]>(cmd->data_size);
            memcpy(tex->m_mips[level].m_imageData.get(), data, cmd->data_size);
        }

        if (level + 1 > tex->m_levelCount)
            tex->m_levelCount = level + 1;
    }
}

void LiveCaptureProcessor::HandleCompressedTexImage2D(const CmdCompressedTexImage2D* cmd, const char* data)
{
    uint32_t tex_id = cmd->texture_id;
    if (tex_id == 0) return;

    auto it = m_texture_store.find(tex_id);
    core::Texture2DInfo* tex = nullptr;
    if (it == m_texture_store.end())
    {
        tex = new core::Texture2DInfo();
        tex->m_objectId = tex_id;
        tex->m_levelCount = 0;
        m_texture_store[tex_id] = tex;
    }
    else
    {
        tex = it->second;
    }

    tex->m_internalFormat = cmd->internalformat;
    tex->m_format = cmd->internalformat; // For compressed, format = internalformat
    tex->m_type = 0;

    uint32_t level = cmd->level;
    if (level < 15)
    {
        tex->m_mips[level].m_width = cmd->width;
        tex->m_mips[level].m_height = cmd->height;
        tex->m_mips[level].m_size = cmd->imageSize;

        if (cmd->imageSize > 0 && data)
        {
            tex->m_mips[level].m_imageData = make_unique<char[]>(cmd->imageSize);
            memcpy(tex->m_mips[level].m_imageData.get(), data, cmd->imageSize);
        }

        if (level + 1 > tex->m_levelCount)
            tex->m_levelCount = level + 1;
    }
}

void LiveCaptureProcessor::HandleUniformMatrix4fv(const CmdUniformMatrix4fv* cmd)
{
    core::matrix4f mat;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            mat(r, c) = cmd->matrix[r * 4 + c];

    m_render_states.m_transform_matrix = mat;
    m_matrix_stack.push_back(mat);
    m_last_uniform_location = cmd->location;

    if (!m_has_first_matrix)
    {
        core::matrix4d mat_d;
        Matrix4fToMatrix4d(mat, mat_d);
        m_first_inv_transform_matrix = inverse(mat_d);
        m_render_states.m_first_inv_transform_matrix = m_first_inv_transform_matrix;
        m_has_first_matrix = true;
    }
}

void LiveCaptureProcessor::HandleDrawElements(const CmdDrawElements* cmd, const char* payload)
{
    // Reconstruct RenderingStates from the snapshot in the draw call record
    RenderingStates draw_state = m_render_states;

    draw_state.draw_call_params.element_buffer_obj = cmd->element_buffer;
    draw_state.draw_call_params.primitive_type = cmd->mode;
    draw_state.draw_call_params.num_indexes = cmd->count;
    draw_state.draw_call_params.data_type = (DataType)cmd->type;
    draw_state.draw_call_params.data_offset = cmd->offset;

    // Read vertex attrib snapshot
    const CmdVertexAttribSnapshot* attribs =
        reinterpret_cast<const CmdVertexAttribSnapshot*>(payload + sizeof(CmdDrawElements));

    for (uint32_t i = 0; i < cmd->num_attribs && i < 256; i++)
    {
        draw_state.m_vertexStream[i].is_enabled = attribs[i].is_enabled;
        draw_state.m_vertexStream[i].data_buffer_obj = attribs[i].data_buffer_obj;
        draw_state.m_vertexStream[i].num_elements = attribs[i].num_elements;
        draw_state.m_vertexStream[i].data_type = (DataType)attribs[i].data_type;
        draw_state.m_vertexStream[i].is_normalized = attribs[i].is_normalized;
        draw_state.m_vertexStream[i].stride = attribs[i].stride;
        draw_state.m_vertexStream[i].start_offset = attribs[i].start_offset;
    }

    // Read transform matrix
    const float* matrix = reinterpret_cast<const float*>(attribs + cmd->num_attribs);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            draw_state.m_transform_matrix(r, c) = matrix[r * 4 + c];

    // Read texture slot
    const uint32_t* tex_slot = reinterpret_cast<const uint32_t*>(matrix + 16);
    draw_state.m_textureSlot[0].index_in_list = *tex_slot;

    draw_state.m_first_inv_transform_matrix = m_first_inv_transform_matrix;

    ExtractMeshFromDrawCall(draw_state);
}

void LiveCaptureProcessor::HandleDrawArrays(const CmdDrawArrays* cmd, const char* payload)
{
    RenderingStates draw_state = m_render_states;

    draw_state.draw_call_params.element_buffer_obj = INVALID_VALUE;
    draw_state.draw_call_params.primitive_type = cmd->mode;
    draw_state.draw_call_params.num_indexes = cmd->count;
    draw_state.draw_call_params.data_type = (DataType)-1;
    draw_state.draw_call_params.data_offset = cmd->first;

    const CmdVertexAttribSnapshot* attribs =
        reinterpret_cast<const CmdVertexAttribSnapshot*>(payload + sizeof(CmdDrawArrays));

    for (uint32_t i = 0; i < cmd->num_attribs && i < 256; i++)
    {
        draw_state.m_vertexStream[i].is_enabled = attribs[i].is_enabled;
        draw_state.m_vertexStream[i].data_buffer_obj = attribs[i].data_buffer_obj;
        draw_state.m_vertexStream[i].num_elements = attribs[i].num_elements;
        draw_state.m_vertexStream[i].data_type = (DataType)attribs[i].data_type;
        draw_state.m_vertexStream[i].is_normalized = attribs[i].is_normalized;
        draw_state.m_vertexStream[i].stride = attribs[i].stride;
        draw_state.m_vertexStream[i].start_offset = attribs[i].start_offset;
    }

    const float* matrix = reinterpret_cast<const float*>(attribs + cmd->num_attribs);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            draw_state.m_transform_matrix(r, c) = matrix[r * 4 + c];

    const uint32_t* tex_slot = reinterpret_cast<const uint32_t*>(matrix + 16);
    draw_state.m_textureSlot[0].index_in_list = *tex_slot;

    draw_state.m_first_inv_transform_matrix = m_first_inv_transform_matrix;

    ExtractMeshFromDrawCall(draw_state);
}

void LiveCaptureProcessor::HandleUseProgram(const CmdUseProgram* cmd)
{
    m_current_program = cmd->program;
}

static void LogMatrix(const char* name, const core::matrix4f& m)
{
    CapLog("    %s:", name);
    for (int r = 0; r < 4; r++)
        CapLog(" [%.6g %.6g %.6g %.6g]", m(r, 0), m(r, 1), m(r, 2), m(r, 3));
    CapLog("\n");
}

// ============================================================================
// ExtractMeshFromDrawCall - adapted from CreateObjectFile in GpaDumpAnalyzeTool.cpp
// Uses buffer_store instead of DataZoneInfo for buffer access
// ============================================================================

static uint16_t read_ushort(const char* ptr)
{
    return *(const uint16_t*)ptr;
}

static float read_float(const char* ptr)
{
    return *(const float*)ptr;
}


void LiveCaptureProcessor::ExtractMeshFromDrawCall(const RenderingStates& state)
{
    bool has_mesh_data_texture = state.m_vertexStream[0].is_enabled &&
                                  state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1 &&
                                  state.m_vertexStream[3].is_enabled &&
                                  state.m_vertexStream[3].data_buffer_obj != (uint32_t)-1 &&
                                  state.draw_call_params.data_type != (DataType)-1 &&
                                  state.draw_call_params.element_buffer_obj != (uint32_t)-1;

    bool has_draw_data = state.m_vertexStream[0].is_enabled &&
                          state.m_vertexStream[0].data_buffer_obj != (uint32_t)-1;

    bool is_ge_polygon = state.draw_call_params.primitive_type == kGlTriangleStrip;
    bool is_ge_mesh = state.draw_call_params.primitive_type == kGlTriangles;

    if (!has_mesh_data_texture && !(has_draw_data && is_ge_polygon))
    {
        if (g_logged_draws < kMaxLoggedDraws)
        {
            g_logged_draws++;
            CapLog("  draw mode=0x%X count=%u skipped: needs attrib 0 + attrib 3 + index buffer (triangles), or attrib 0 (strip); enabled:",
                   state.draw_call_params.primitive_type, state.draw_call_params.num_indexes);
            for (int i = 0; i < 16; i++)
                if (state.m_vertexStream[i].is_enabled) CapLog(" %d", i);
            CapLog("\n");
        }
        return;
    }

    // Extract vertex positions from buffer store
    vector<core::vec3f> position_data;
    vector<core::vec2f> texture_coord_data;
    vector<uint32_t> color_data;
    vector<uint32_t> index_data;

    bool log_this = g_logged_draws < kMaxLoggedDraws;
    if (log_this)
    {
        g_logged_draws++;
        CapLog("  draw mode=0x%X count=%u index_type=0x%X element_buf=%u offset=%u\n",
               state.draw_call_params.primitive_type, state.draw_call_params.num_indexes,
               uint32_t(state.draw_call_params.data_type), state.draw_call_params.element_buffer_obj,
               state.draw_call_params.data_offset);
        for (int i = 0; i < 16; i++)
        {
            const VertexAttrib& va = state.m_vertexStream[i];
            if (!va.is_enabled) continue;
            auto bit = m_buffer_store.find(va.data_buffer_obj);
            CapLog("    attrib %d: buf=%u (%s, %u bytes) comps=%u type=0x%X norm=%u stride=%u offset=%u\n",
                   i, va.data_buffer_obj, bit != m_buffer_store.end() ? "have data" : "NO DATA",
                   bit != m_buffer_store.end() ? unsigned(bit->second.size()) : 0u,
                   va.num_elements, uint32_t(va.data_type), va.is_normalized, va.stride, va.start_offset);
        }
    }

    std::vector<std::array<float, 4>> stream;

    // Stream 0: positions
    if (state.m_vertexStream[0].is_enabled && state.m_vertexStream[0].data_buffer_obj != INVALID_VALUE)
    {
        auto it = m_buffer_store.find(state.m_vertexStream[0].data_buffer_obj);
        if (it != m_buffer_store.end())
        {
            stream.clear();
            ReadAttributeStream(it->second, state.m_vertexStream[0], stream);
            position_data.reserve(stream.size());
            for (const auto& v : stream)
                position_data.push_back(core::vec3f(v[0], v[1], v[2]));
        }
    }

    // Stream 3: texture coordinates
    if (state.m_vertexStream[3].is_enabled && state.m_vertexStream[3].data_buffer_obj != INVALID_VALUE)
    {
        auto it = m_buffer_store.find(state.m_vertexStream[3].data_buffer_obj);
        if (it != m_buffer_store.end())
        {
            stream.clear();
            ReadAttributeStream(it->second, state.m_vertexStream[3], stream);
            texture_coord_data.reserve(stream.size());
            for (const auto& v : stream)
                texture_coord_data.push_back(core::vec2f(v[0], v[1]));
        }
    }

    // Stream 2: colors (for polygon/triangle strip), raw bytes packed into RGBA
    if (is_ge_polygon && state.m_vertexStream[2].is_enabled && state.m_vertexStream[2].data_buffer_obj != INVALID_VALUE)
    {
        auto it = m_buffer_store.find(state.m_vertexStream[2].data_buffer_obj);
        if (it != m_buffer_store.end() && state.m_vertexStream[2].start_offset < it->second.size())
        {
            const char* start = it->second.data() + state.m_vertexStream[2].start_offset;
            const char* end = it->second.data() + it->second.size();
            uint32_t num_el = state.m_vertexStream[2].num_elements > 4 ? 4 : state.m_vertexStream[2].num_elements;
            uint32_t stride = state.m_vertexStream[2].stride ? state.m_vertexStream[2].stride : num_el;

            while (num_el > 0 && start + num_el <= end)
            {
                uint32_t color = 0;
                for (uint32_t i = 0; i < num_el; i++)
                    color |= ((uint32_t)(uint8_t)start[i]) << (8 * i);
                color_data.push_back(color);
                start += stride;
            }
        }
    }

    // Index data, in the draw call's index type
    uint32_t index_size = GLTypeSize(uint32_t(state.draw_call_params.data_type));
    bool supported_index = state.draw_call_params.data_type == DataType(0x1401) ||
                           state.draw_call_params.data_type == DataType(0x1403) ||
                           state.draw_call_params.data_type == DataType(0x1405);
    if (supported_index && state.draw_call_params.element_buffer_obj != INVALID_VALUE &&
        (state.draw_call_params.primitive_type == kGlTriangles ||
         state.draw_call_params.primitive_type == kGlTriangleStrip ||
         state.draw_call_params.primitive_type == kGlLineStrip))
    {
        auto it = m_buffer_store.find(state.draw_call_params.element_buffer_obj);
        if (it != m_buffer_store.end() && state.draw_call_params.data_offset < it->second.size())
        {
            const char* start = it->second.data() + state.draw_call_params.data_offset;
            const char* end = it->second.data() + it->second.size();
            index_data.reserve(state.draw_call_params.num_indexes);
            for (uint32_t i = 0; i < state.draw_call_params.num_indexes && start + index_size <= end; i++, start += index_size)
            {
                if (index_size == 1)      index_data.push_back(*(const uint8_t*)start);
                else if (index_size == 2) index_data.push_back(read_ushort(start));
                else                      index_data.push_back(*(const uint32_t*)start);
            }
        }
    }

    if (log_this)
    {
        CapLog("    -> %u positions, %u uvs, %u indices\n",
               unsigned(position_data.size()), unsigned(texture_coord_data.size()), unsigned(index_data.size()));
        CapLog("    program=%u uniform_loc=%d has_modelview=%d\n",
               m_current_program, int(m_last_uniform_location), int(m_has_modelview));
        LogMatrix("uniform   ", state.m_transform_matrix);
        LogMatrix("modelview ", m_modelview);
        LogMatrix("projection", m_projection);
        if (!position_data.empty())
        {
            const core::vec3f& p0 = position_data[0];
            core::vec3f pmin = p0, pmax = p0;
            for (const auto& p : position_data)
            {
                pmin = core::vec3f((std::min)(pmin.x, p.x), (std::min)(pmin.y, p.y), (std::min)(pmin.z, p.z));
                pmax = core::vec3f((std::max)(pmax.x, p.x), (std::max)(pmax.y, p.y), (std::max)(pmax.z, p.z));
            }
            CapLog("    raw pos range: (%.6g %.6g %.6g) - (%.6g %.6g %.6g)\n",
                   pmin.x, pmin.y, pmin.z, pmax.x, pmax.y, pmax.z);
        }
    }

    // Deduplicate vertices (same as CreateObjectFile)
    vector<int32_t> index_match_table(position_data.size(), -1);
    vector<int32_t> new_index_match_table;
    new_index_match_table.reserve(position_data.size());

    for (uint32_t i = 0; i < index_data.size(); i++)
    {
        if (index_data[i] < index_match_table.size() && index_match_table[index_data[i]] < 0)
        {
            index_match_table[index_data[i]] = int32_t(new_index_match_table.size());
            new_index_match_table.push_back(index_data[i]);
        }
    }

    if (new_index_match_table.size() == 0)
    {
        if (g_logged_draws <= kMaxLoggedDraws)
            CapLog("    -> no mesh (no usable indices/positions)\n");
        return;
    }
    g_meshes_from_frame++;

    // Create MeshData
    MeshData* mesh_data = new MeshData;
    mesh_data->num_vertex = int32_t(new_index_match_table.size());
    uint32_t num_vertex = uint32_t(mesh_data->num_vertex);

    // Transformation into a common space. Google Earth loads each tile's
    // modelview with glLoadMatrixf, which takes its quantized vertices straight
    // to eye space (units of one Earth radius). Its glUniformMatrix4fv is
    // modelview * projection, so it is only a fallback for programs without a
    // fixed-function modelview.
    if (m_has_modelview)
    {
        Matrix4fToMatrix4d(m_modelview, mesh_data->dumpped_matrix);
        // Vertices are quantized to 0..1 per tile, so the X axis scale is the
        // tile's edge length - its level of detail.
        if (is_ge_mesh)
        {
            core::vec4d ax = mesh_data->dumpped_matrix.get_row(0);
            m_tile_size[mesh_data] = sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z) * kMetresPerUnit;
            mesh_data->lod_size = float(m_tile_size[mesh_data]);

            // Identity of the tile across captures: its vertex and index data
            // (GL buffer ids get reused, the content does not).
            uint64_t key = 1469598103934665603ull;
            auto fnv = [&key](const void* p, size_t n) {
                const uint8_t* b = static_cast<const uint8_t*>(p);
                for (size_t i = 0; i < n; i++) { key ^= b[i]; key *= 1099511628211ull; }
            };
            auto vb = m_buffer_store.find(state.m_vertexStream[0].data_buffer_obj);
            if (vb != m_buffer_store.end())
                fnv(vb->second.data(), vb->second.size());
            fnv(&state.m_vertexStream[0].start_offset, sizeof(state.m_vertexStream[0].start_offset));
            fnv(index_data.data(), index_data.size() * sizeof(uint32_t));
            m_tile_key[mesh_data] = key;
        }
    }
    else
    {
        Matrix4fToMatrix4d(state.m_transform_matrix, mesh_data->dumpped_matrix);
        mesh_data->dumpped_matrix *= state.m_first_inv_transform_matrix;
    }

    // Positions
    if (position_data.size() > 0)
    {
        mesh_data->vertex_list = make_unique<core::vec3f[]>(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
            mesh_data->vertex_list[i] = position_data[uint32_t(new_index_match_table[i])];
    }

    // UVs
    if (texture_coord_data.size() > 0)
    {
        mesh_data->uv_list = make_unique<core::vec2f[]>(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
        {
            uint32_t src_idx = uint32_t(new_index_match_table[i]);
            if (src_idx < texture_coord_data.size())
                mesh_data->uv_list[i] = texture_coord_data[src_idx];
        }
    }

    // Colors
    if (color_data.size() > 0)
    {
        mesh_data->color_list = make_unique<uint32_t[]>(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
        {
            uint32_t src_idx = uint32_t(new_index_match_table[i]);
            if (src_idx < color_data.size())
                mesh_data->color_list[i] = color_data[src_idx];
        }
    }

    // Draw calls with remapped indices
    if (index_data.size() > 0)
    {
        if (is_ge_polygon)
        {
            mesh_data->add_draw_call_list(kGlTriangleStrip, int32_t(index_data.size()), int32_t(num_vertex));
            DrawCallInfo& dc = mesh_data->get_last_draw_call_info();
            for (uint32_t i = 0; i < index_data.size(); i++)
            {
                if (index_data[i] < index_match_table.size())
                    dc.add_index(uint32_t(index_match_table[index_data[i]]));
            }
        }
        else if (is_ge_mesh)
        {
            mesh_data->add_draw_call_list(kGlTriangles, int32_t(index_data.size()), int32_t(num_vertex));
            DrawCallInfo& dc = mesh_data->get_last_draw_call_info();
            for (uint32_t i = 0; i + 2 < index_data.size(); i += 3)
            {
                if (index_data[i + 0] < index_match_table.size() &&
                    index_data[i + 1] < index_match_table.size() &&
                    index_data[i + 2] < index_match_table.size())
                {
                    dc.add_index(uint32_t(index_match_table[index_data[i + 0]]));
                    dc.add_index(uint32_t(index_match_table[index_data[i + 1]]));
                    dc.add_index(uint32_t(index_match_table[index_data[i + 2]]));
                }
            }
        }
    }

    // Texture reference
    mesh_data->idx_in_texture_list = state.m_textureSlot[0].index_in_list;

    // Bake vertices into world space (row vectors times dumpped_matrix, in
    // double), stored relative to the mesh centre as the KML-referenced GPA
    // import does: world position = vertex_list[i] + translation. The renderer
    // and the exporters both rely on that layout.
    mesh_data->bbox_ws.Reset();
    if (mesh_data->vertex_list)
    {
        std::vector<core::vec3d> pos_ws(num_vertex);
        for (uint32_t i = 0; i < num_vertex; i++)
        {
            pos_ws[i] = core::vec3d(core::vec4d(mesh_data->vertex_list[i], 1.0) * mesh_data->dumpped_matrix);
            mesh_data->bbox_ws += pos_ws[i];
        }
        mesh_data->translation = mesh_data->bbox_ws.GetCentroid();
        for (uint32_t i = 0; i < num_vertex; i++)
            mesh_data->vertex_list[i] = core::vec3f(pos_ws[i] - mesh_data->translation);

        // Area-weighted surface normal, used later to find "up" for the frame.
        // Walls of closed shapes cancel out, so the ground dominates.
        if (is_ge_mesh && !mesh_data->draw_call_list.empty())
        {
            for (uint32_t i = 0; i + 2 < index_data.size(); i += 3)
            {
                uint32_t a = index_data[i], b = index_data[i + 1], c = index_data[i + 2];
                if (a >= index_match_table.size() || b >= index_match_table.size() || c >= index_match_table.size())
                    continue;
                const core::vec3d& pa = pos_ws[uint32_t(index_match_table[a])];
                const core::vec3d& pb = pos_ws[uint32_t(index_match_table[b])];
                const core::vec3d& pc = pos_ws[uint32_t(index_match_table[c])];
                m_up_accum += cross(pb - pa, pc - pa);
            }
        }
    }
    m_current_group->bbox_ws += mesh_data->bbox_ws;

    // Add to current group
    m_current_group->meshes.push_back(mesh_data);
}

// ============================================================================
// ProcessManager
// ============================================================================

ProcessManager::ProcessManager()
    : m_process(nullptr)
    , m_mapping(nullptr)
    , m_shared_mem(nullptr)
    , m_header(nullptr)
    , m_event_ready(nullptr)
    , m_event_control(nullptr)
    , m_ge_path("C:\\Program Files\\Google\\Google Earth Pro\\client\\googleearth.exe")
{
}

ProcessManager::~ProcessManager()
{
    StopGoogleEarth();
    DestroySharedMemory();
}

bool ProcessManager::CreateSharedMemory()
{
    // Create shared memory
    m_mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, GLCAPTURE_SHARED_MEM_SIZE, GLCAPTURE_SHARED_MEM_NAME);

    if (!m_mapping)
        return false;

    m_shared_mem = MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, GLCAPTURE_SHARED_MEM_SIZE);
    if (!m_shared_mem)
    {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }

    // Initialize header
    memset(m_shared_mem, 0, GLCAPTURE_SHARED_MEM_SIZE);
    m_header = reinterpret_cast<GLCaptureHeader*>(m_shared_mem);

    // Create events
    m_event_ready = CreateEventA(nullptr, FALSE, FALSE, GLCAPTURE_EVENT_READY);
    m_event_control = CreateEventA(nullptr, FALSE, FALSE, GLCAPTURE_EVENT_CONTROL);

    return true;
}

void ProcessManager::DestroySharedMemory()
{
    if (m_shared_mem)
    {
        UnmapViewOfFile(m_shared_mem);
        m_shared_mem = nullptr;
        m_header = nullptr;
    }
    if (m_mapping)
    {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
    if (m_event_ready)
    {
        CloseHandle(m_event_ready);
        m_event_ready = nullptr;
    }
    if (m_event_control)
    {
        CloseHandle(m_event_control);
        m_event_control = nullptr;
    }
}

bool ProcessManager::StartGoogleEarth(const std::string& ge_path, const std::string& kml_path)
{
    if (!ge_path.empty())
        m_ge_path = ge_path;

    if (!m_mapping && !CreateSharedMemory())
        return false;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::string cmd = "\"" + m_ge_path + "\"";
    if (!kml_path.empty())
        cmd += " \"" + kml_path + "\"";

    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                         0, nullptr, nullptr, &si, &pi))
    {
        return false;
    }

    m_process = pi.hProcess;
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    // With the proxy opengl32.dll deployed next to googleearth.exe, the proxy
    // does the capture and draws its own overlay; injecting the overlay hook as
    // well would patch the proxy's wglSwapBuffers and draw a second overlay.
    std::string proxyPath = m_ge_path.substr(0, m_ge_path.find_last_of("\\/") + 1) + "opengl32.dll";
    if (GetFileAttributesA(proxyPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (m_on_process_started) m_on_process_started();
        return true;
    }

    // Wait for GE to initialize OpenGL, then inject our hook DLL
    Sleep(5000);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string hookDllPath(exePath);
    size_t lastSlash = hookDllPath.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        hookDllPath = hookDllPath.substr(0, lastSlash + 1);
    hookDllPath += "meshtool_hook.dll";

    InjectDLL(pid, hookDllPath);

    if (m_on_process_started) m_on_process_started();
    return true;
}

void ProcessManager::StopGoogleEarth()
{
    if (m_process)
    {
        TerminateProcess(m_process, 0);
        CloseHandle(m_process);
        m_process = nullptr;
        if (m_on_process_stopped) m_on_process_stopped();
    }
}

bool ProcessManager::IsRunning() const
{
    if (!m_process) return false;
    DWORD exit_code;
    GetExitCodeProcess(m_process, &exit_code);
    return exit_code == STILL_ACTIVE;
}

bool ProcessManager::RequestFrameCapture()
{
    if (!m_header) return false;
    m_header->capture_flags |= GLCAPTURE_FLAG_ACTIVE | GLCAPTURE_FLAG_FRAME_REQ;
    return true;
}

bool ProcessManager::IsHookConnected() const
{
    if (!m_header) return false;
    return (m_header->status_flags & GLCAPTURE_STATUS_CONNECTED) != 0;
}

bool ProcessManager::OpenKmlInGoogleEarth(const std::string& kml_path)
{
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + m_ge_path + "\" \"" + kml_path + "\"";
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool ProcessManager::StartGoogleEarthWithKML(const std::string& kml_path)
{
    if (!m_mapping && !CreateSharedMemory())
        return false;

    // Launch Google Earth Pro with the KML file as argument
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::string cmd = "\"" + m_ge_path + "\" \"" + kml_path + "\"";

    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                         0, nullptr, nullptr, &si, &pi))
    {
        return false;
    }

    m_process = pi.hProcess;
    CloseHandle(pi.hThread);

    if (m_on_process_started) m_on_process_started();
    return true;
}

std::string ProcessManager::GenerateFlyToKML(double lon, double lat, double altitude,
                                              double heading, double tilt, double range)
{
    // Write a temporary KML file that flies Google Earth to the specified location
    std::string tmpPath = "meshtool_flyto.kml";

    std::ofstream f(tmpPath);
    if (!f) return "";

    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
      << "<Document>\n"
      << "  <name>MeshTool Capture Target</name>\n"
      << "  <Placemark>\n"
      << "    <name>Capture Location</name>\n"
      << "    <LookAt>\n"
      << "      <longitude>" << lon << "</longitude>\n"
      << "      <latitude>" << lat << "</latitude>\n"
      << "      <altitude>" << altitude << "</altitude>\n"
      << "      <heading>" << heading << "</heading>\n"
      << "      <tilt>" << tilt << "</tilt>\n"
      << "      <range>" << range << "</range>\n"
      << "      <altitudeMode>relativeToGround</altitudeMode>\n"
      << "    </LookAt>\n"
      << "    <Point>\n"
      << "      <coordinates>" << lon << "," << lat << "," << altitude << "</coordinates>\n"
      << "    </Point>\n"
      << "  </Placemark>\n"
      << "</Document>\n"
      << "</kml>\n";

    f.close();

    // Return absolute path
    char fullPath[MAX_PATH];
    GetFullPathNameA(tmpPath.c_str(), MAX_PATH, fullPath, nullptr);
    return std::string(fullPath);
}

std::string ProcessManager::GenerateSearchKML(const std::string& address)
{
    // Google Earth Pro can open a KML with a Placemark whose name is the search query.
    // Alternatively, we can use the "flytoview" query parameter in a NetworkLink.
    // The simplest approach: use the Google Earth "search" feature via a KML NetworkLink.
    std::string tmpPath = "meshtool_search.kml";

    std::ofstream f(tmpPath);
    if (!f) return "";

    // Use a Placemark with the address as the description.
    // Google Earth will geocode the address in the Placemark name.
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
      << "<Document>\n"
      << "  <name>MeshTool Search</name>\n"
      << "  <Placemark>\n"
      << "    <name>" << address << "</name>\n"
      << "    <description>MeshTool capture target</description>\n"
      << "    <Point>\n"
      << "      <coordinates>0,0,0</coordinates>\n"
      << "    </Point>\n"
      << "  </Placemark>\n"
      << "</Document>\n"
      << "</kml>\n";

    f.close();

    char fullPath[MAX_PATH];
    GetFullPathNameA(tmpPath.c_str(), MAX_PATH, fullPath, nullptr);
    return std::string(fullPath);
}
