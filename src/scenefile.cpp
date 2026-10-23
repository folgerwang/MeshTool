#include "scenefile.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <random>
#include <ctime>
#include <cctype>
#include <unordered_map>
#include "meshdata.h"
#include "coretexture.h"
#include "route.h"

SceneCapture NewSceneCapture()
{
    SceneCapture capture;
    std::random_device random;
    char part[9];
    for (int i = 0; i < 4; ++i)
    {
        snprintf(part, sizeof(part), "%08x", uint32_t(random()));
        capture.id += part;
    }
    const time_t now = time(nullptr);
    tm utc{};
    gmtime_s(&utc, &now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
    capture.name = std::string("capture_") + stamp + "_" + capture.id.substr(0, 8);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    capture.createdUtc = stamp;
    return capture;
}

namespace
{
    std::string JsonString(const std::string& value)
    {
        std::string out = "\"";
        for (unsigned char ch : value)
        {
            if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
            else if (ch < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", ch); out += escaped; }
            else out += char(ch);
        }
        return out + "\"";
    }
}

bool SaveCaptureFolder(const std::string& parent, const std::vector<BatchMeshData*>& batches,
                       const SceneCapture& capture, std::string& scenePath, std::string& error,
                       const RoutePlan* route)
{
    namespace fs = std::filesystem;
    // Names are a single portable path component, never paths supplied by metadata.
    if (capture.id.empty() || capture.name.empty() || capture.name.size() > 120 ||
        capture.name.rfind("capture_", 0) != 0 ||
        capture.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
    { error = "Invalid capture identity or folder name."; return false; }
    const fs::path dir = fs::path(parent) / capture.name;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec || !fs::create_directory(dir, ec))
    { error = "Capture folder already exists or cannot be created. Choose another parent folder."; return false; }
    scenePath = (dir / (capture.name + ".mtscene")).string();
    if (!SaveScene(scenePath, batches, error, route, &capture)) return false;
    const std::string identity = "{\"id\":" + JsonString(capture.id) + ",\"name\":" +
        JsonString(capture.name) + ",\"created_utc\":" + JsonString(capture.createdUtc) + "}";
    const std::string treeName = capture.name + ".trees.json";
    std::ofstream trees(dir / treeName, std::ios::binary);
    trees << "{\n  \"schema_version\":1,\n  \"capture\":" << identity
          << ",\n  \"scene_file\":" << JsonString(fs::path(scenePath).filename().string())
          << ",\n  \"units\":\"metres\",\n  \"instances\":[";
    bool first = true;
    for (const auto* b : batches) if (b) for (const auto* g : b->group_meshes) if (g)
    {
        const auto& json = g->tree_instances_json;
        const auto begin = json.find_first_not_of(" \r\n\t"), end = json.find_last_not_of(" \r\n\t");
        if (begin == std::string::npos) continue;
        if (json[begin] != '[' || json[end] != ']')
        { error = "Invalid embedded tree catalog; capture folder is incomplete."; return false; }
        const auto body = json.substr(begin + 1, end - begin - 1);
        if (body.find_first_not_of(" \r\n\t") == std::string::npos) continue;
        if (!first) trees << ',';
        trees << body;
        first = false;
    }
    trees << "]\n}\n";
    trees.close();
    if (!trees) { error = "Cannot write tree catalog; capture folder is incomplete."; return false; }
    // Written last: its presence marks a completed bundle. Paths are portable and relative.
    std::ofstream manifest(dir / "capture.json", std::ios::binary);
    manifest << "{\n  \"schema_version\":1,\n  \"capture\":" << identity
             << ",\n  \"scene_file\":" << JsonString(fs::path(scenePath).filename().string())
             << ",\n  \"trees_file\":" << JsonString(treeName)
             << ",\n  \"tree_records_embedded\":true\n}\n";
    manifest.close();
    if (!manifest) { error = "Cannot write capture manifest; capture folder is incomplete."; return false; }
    return true;
}

namespace
{
    const char     kMagic[8] = { 'M', 'T', 'S', 'C', 'E', 'N', 'E', '\0' };
    const uint32_t kVersion = 9;          // 9: capture identity; 8: per-group planting records
                                          // 7: mesh lod_size (GE tile edge, the LOD depth order),
                                          // 5: model variants, 6: driving route
    const uint32_t kOldestVersion = 1;

    // Sanity limits so a corrupt file fails cleanly instead of allocating wildly.
    const uint32_t kMaxCount = 50u * 1000u * 1000u;
    const uint32_t kMaxBytes = 512u * 1024u * 1024u;

    struct Writer
    {
        FILE* f = nullptr;
        bool ok = true;
        void bytes(const void* p, size_t n) { if (ok && n && fwrite(p, 1, n, f) != n) ok = false; }
        template<class T> void pod(const T& v) { bytes(&v, sizeof(T)); }
    };

    struct Reader
    {
        FILE* f = nullptr;
        bool ok = true;
        void bytes(void* p, size_t n) { if (ok && n && fread(p, 1, n, f) != n) ok = false; }
        template<class T> T pod() { T v{}; bytes(&v, sizeof(T)); return v; }
        uint32_t count(uint32_t limit = kMaxCount)
        {
            uint32_t n = pod<uint32_t>();
            if (n > limit) ok = false;
            return ok ? n : 0;
        }
    };

    void WriteTexture(Writer& w, const core::Texture2DInfo* tex)
    {
        uint32_t levels = 0;
        if (tex)
            while (levels < tex->m_levelCount && levels < 15 && tex->m_mips[levels].m_imageData)
                levels++;
        w.pod(levels);
        if (!levels)
            return;
        w.pod(tex->m_internalFormat);
        w.pod(tex->m_format);
        w.pod(tex->m_type);
        for (uint32_t l = 0; l < levels; l++)
        {
            const core::Texture2DSurfaceInfo& m = tex->m_mips[l];
            w.pod(m.m_width);
            w.pod(m.m_height);
            w.pod(m.m_size);
            w.bytes(m.m_imageData.get(), m.m_size);
        }
    }

    core::Texture2DInfo* ReadTexture(Reader& r)
    {
        uint32_t levels = r.count(15);
        core::Texture2DInfo* tex = new core::Texture2DInfo();
        tex->m_objectId = 0;
        tex->m_levelCount = 0;
        tex->m_internalFormat = tex->m_format = tex->m_type = 0;
        if (!levels)
            return tex;
        tex->m_internalFormat = r.pod<uint32_t>();
        tex->m_format = r.pod<uint32_t>();
        tex->m_type = r.pod<uint32_t>();
        for (uint32_t l = 0; l < levels && r.ok; l++)
        {
            core::Texture2DSurfaceInfo& m = tex->m_mips[l];
            m.m_width = r.pod<uint32_t>();
            m.m_height = r.pod<uint32_t>();
            m.m_size = r.count(kMaxBytes);
            if (!r.ok) break;
            m.m_imageData = std::make_unique<char[]>(m.m_size ? m.m_size : 1);
            r.bytes(m.m_imageData.get(), m.m_size);
            tex->m_levelCount = l + 1;
        }
        return tex;
    }

    void WriteMesh(Writer& w, const MeshData* m)
    {
        uint32_t n = m->num_vertex > 0 && m->vertex_list ? uint32_t(m->num_vertex) : 0;
        w.pod(n);
        w.pod(m->idx_in_texture_list);
        w.pod(m->object_id);
        w.pod(m->material);
        w.pod(m->model_variant);
        w.pod(m->lod_size);
        w.pod(m->translation.x); w.pod(m->translation.y); w.pod(m->translation.z);
        uint8_t has_uv = (n && m->uv_list) ? 1 : 0;
        uint8_t has_color = (n && m->color_list) ? 1 : 0;
        w.pod(has_uv);
        w.pod(has_color);
        if (n)
            w.bytes(m->vertex_list.get(), sizeof(core::vec3f) * n);
        if (has_uv)
            w.bytes(m->uv_list.get(), sizeof(core::vec2f) * n);
        if (has_color)
            w.bytes(m->color_list.get(), sizeof(uint32_t) * n);

        w.pod(uint32_t(m->draw_call_list.size()));
        for (const DrawCallInfo& dc : m->draw_call_list)
        {
            w.pod(uint32_t(dc.get_primitive_type()));
            uint32_t count = uint32_t(dc.get_index_count() > 0 ? dc.get_index_count() : 0);
            w.pod(count);
            for (uint32_t i = 0; i < count; i++)
                w.pod(dc.get_index(int(i)));
        }
    }

    MeshData* ReadMesh(Reader& r, uint32_t version)
    {
        MeshData* m = new MeshData;
        uint32_t n = r.count();
        m->num_vertex = int32_t(n);
        m->idx_in_texture_list = r.pod<uint32_t>();
        if (version >= 3)
            m->object_id = r.pod<int32_t>();
        if (version >= 4)
            m->material = r.pod<uint8_t>();
        if (version >= 5)
        {
            m->model_variant = r.pod<uint8_t>();
            if (m->model_variant > 2) r.ok = false;
        }
        if (version >= 7)
        {
            m->lod_size = r.pod<float>();
            if (!(m->lod_size >= 0.0f && m->lod_size < 1e6f)) m->lod_size = 0.0f;
        }
        double t[3] = { r.pod<double>(), r.pod<double>(), r.pod<double>() };
        m->translation = core::vec3d(t[0], t[1], t[2]);
        uint8_t has_uv = r.pod<uint8_t>();
        uint8_t has_color = r.pod<uint8_t>();
        if (n && r.ok)
        {
            m->vertex_list = std::make_unique<core::vec3f[]>(n);
            r.bytes(m->vertex_list.get(), sizeof(core::vec3f) * n);
            if (has_uv)
            {
                m->uv_list = std::make_unique<core::vec2f[]>(n);
                r.bytes(m->uv_list.get(), sizeof(core::vec2f) * n);
            }
            if (has_color)
            {
                m->color_list = std::make_unique<uint32_t[]>(n);
                r.bytes(m->color_list.get(), sizeof(uint32_t) * n);
            }
        }

        uint32_t num_dc = r.count(1u << 16);
        for (uint32_t d = 0; d < num_dc && r.ok; d++)
        {
            uint32_t prim = r.pod<uint32_t>();
            uint32_t count = r.count();
            if (!r.ok) break;
            m->add_draw_call_list(PrimitiveType(prim), int32_t(count), int32_t(n));
            DrawCallInfo& dc = m->get_last_draw_call_info();
            for (uint32_t i = 0; i < count && r.ok; i++)
            {
                uint32_t idx = r.pod<uint32_t>();
                dc.add_index(idx < n ? idx : 0);
            }
        }

        // World bounds from the vertices actually stored.
        m->bbox_ws.Reset();
        for (uint32_t i = 0; i < n && m->vertex_list; i++)
        {
            const core::vec3f& v = m->vertex_list[i];
            m->bbox_ws += core::vec3d(v.x, v.y, v.z) + m->translation;
        }
        // Scenes saved before version 7 lost the tile edge, and without it
        // the stacked tiles of overlapping captures z-fight (the renderer
        // draws a coarser tile behind a finer one by its lod_size). Estimate
        // it for the captured meshes from the footprint, rounded to the
        // power of two GE's quadtree levels are near: a tile the LOD filter
        // trimmed may come out a level too fine, which is still better than
        // no order at all.
        if (version < 7 && m->bbox_ws.b_valid && m->material == kMatCaptured && m->model_variant == 0 &&
            !m->draw_call_list.empty() && m->draw_call_list[0].is_ge_mesh())
        {
            // No upper bound: the overview tiles of a capture zoomed far out
            // span kilometres, and they of all tiles must draw behind the
            // detailed ones (the renderer caps the push distance).
            const double edge = (std::max)(m->bbox_ws.bb_max.x - m->bbox_ws.bb_min.x,
                                           m->bbox_ws.bb_max.y - m->bbox_ws.bb_min.y);
            if (edge >= 1.0)
                m->lod_size = float(pow(2.0, floor(log2(edge) + 0.5)));
        }
        return m;
    }

    // A captured scene can hold the same tile several times over: the same
    // vertices, the same triangles, the same place, only under another
    // texture slot (a later capture brought its own copy of the texture).
    // Drawn twice at the same depth, the copies fight for pixels, and they
    // cost memory and draw time for nothing. Keep the first of each.
    void DropDuplicateMeshes(GroupMeshData* g)
    {
        struct Key
        {
            int32_t  object;
            uint8_t  material, variant;
            uint32_t vertices, indices;
            int64_t  tx, ty, tz;
            uint64_t vhash, ihash;
            bool operator==(const Key& o) const
            {
                return object == o.object && material == o.material && variant == o.variant &&
                       vertices == o.vertices && indices == o.indices && tx == o.tx && ty == o.ty && tz == o.tz &&
                       vhash == o.vhash && ihash == o.ihash;
            }
        };
        struct Hash
        {
            size_t operator()(const Key& k) const
            {
                uint64_t h = k.vhash ^ (k.ihash * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.vertices) << 32) ^ k.indices;
                h ^= uint64_t(k.tx) * 0xBF58476D1CE4E5B9ull;
                h ^= uint64_t(k.ty) * 0x94D049BB133111EBull;
                return size_t(h);
            }
        };
        auto fnv = [](const void* data, size_t bytes, uint64_t h) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
            return h;
        };
        std::unordered_map<Key, const MeshData*, Hash> seen;
        std::vector<MeshData*> kept;
        kept.reserve(g->meshes.size());
        for (MeshData* m : g->meshes)
        {
            if (!m || !m->vertex_list || m->num_vertex <= 0 || m->draw_call_list.empty())
            {
                kept.push_back(m);
                continue;
            }
            Key k;
            k.object = m->object_id;
            k.material = m->material;
            k.variant = m->model_variant;
            k.vertices = uint32_t(m->num_vertex);
            k.tx = int64_t(floor(m->translation.x * 1000.0));
            k.ty = int64_t(floor(m->translation.y * 1000.0));
            k.tz = int64_t(floor(m->translation.z * 1000.0));
            k.vhash = fnv(m->vertex_list.get(), sizeof(core::vec3f) * size_t(m->num_vertex), 0xCBF29CE484222325ull);
            k.indices = 0;
            k.ihash = 0xCBF29CE484222325ull;
            for (const DrawCallInfo& dc : m->draw_call_list)
            {
                k.indices += uint32_t(dc.get_index_count());
                const uint32_t prim = uint32_t(dc.get_primitive_type());
                k.ihash = fnv(&prim, sizeof(prim), k.ihash);
                k.ihash = fnv(dc.get_index_buffer(), size_t(dc.get_index_count()) * (dc.get_index_type() == kGlUInt ? 4 : 2), k.ihash);
            }
            auto it = seen.find(k);
            if (it != seen.end())
            {
                const MeshData* first = it->second;
                bool same = memcmp(first->vertex_list.get(), m->vertex_list.get(), sizeof(core::vec3f) * size_t(m->num_vertex)) == 0 &&
                            first->draw_call_list.size() == m->draw_call_list.size();
                for (size_t d = 0; same && d < m->draw_call_list.size(); d++)
                {
                    const DrawCallInfo& a = first->draw_call_list[d];
                    const DrawCallInfo& b = m->draw_call_list[d];
                    same = a.get_index_count() == b.get_index_count() && a.get_index_type() == b.get_index_type() &&
                           memcmp(a.get_index_buffer(), b.get_index_buffer(),
                                  size_t(a.get_index_count()) * (a.get_index_type() == kGlUInt ? 4 : 2)) == 0;
                }
                if (same)
                {
                    delete m;
                    continue;
                }
            }
            seen.emplace(k, m);
            kept.push_back(m);
        }
        g->meshes.swap(kept);
    }

    void DeleteBatches(std::vector<BatchMeshData*>& batches)
    {
        for (BatchMeshData* b : batches)
        {
            if (!b) continue;
            for (GroupMeshData* g : b->group_meshes)
            {
                if (!g) continue;
                for (MeshData* m : g->meshes) delete m;
                for (core::Texture2DInfo* t : g->loaded_textures) delete t;
                delete g;
            }
            delete b;
        }
        batches.clear();
    }
}

bool SaveScene(const std::string& path, const std::vector<BatchMeshData*>& batches, std::string& error,
               const RoutePlan* route, const SceneCapture* capture)
{
    if (capture && !capture->id.empty() && (capture->name.empty() || capture->id.size() > 4096 ||
        capture->name.size() > 4096 || capture->createdUtc.size() > 4096))
    { error = "Invalid capture identity."; return false; }
    Writer w;
    w.f = fopen(path.c_str(), "wb");
    if (!w.f) { error = "Cannot open file for writing."; return false; }

    w.bytes(kMagic, sizeof(kMagic));
    uint32_t version = 7;
    for (const auto* b : batches) if (b) for (const auto* g : b->group_meshes)
        if (g && !g->tree_instances_json.empty()) version = 8;
    if (capture && !capture->id.empty()) version = 9;
    w.pod(version);

    uint32_t num_batches = 0;
    for (const BatchMeshData* b : batches) if (b) num_batches++;
    w.pod(num_batches);
    for (const BatchMeshData* b : batches)
    {
        if (!b) continue;
        w.pod(uint8_t(b->is_spline_mesh ? 1 : 0));
        w.pod(uint8_t(b->is_google_dump ? 1 : 0));
        w.pod(uint8_t(b->is_georeferenced ? 1 : 0));
        w.pod(b->reference_pos.x); w.pod(b->reference_pos.y);

        uint32_t num_groups = 0;
        for (const GroupMeshData* g : b->group_meshes) if (g) num_groups++;
        w.pod(num_groups);
        for (const GroupMeshData* g : b->group_meshes)
        {
            if (!g) continue;
            w.pod(uint32_t(g->loaded_textures.size()));
            for (const core::Texture2DInfo* t : g->loaded_textures)
                WriteTexture(w, t);

            uint32_t num_meshes = 0;
            for (const MeshData* m : g->meshes) if (m) num_meshes++;
            w.pod(num_meshes);
            for (const MeshData* m : g->meshes)
                if (m) WriteMesh(w, m);

            w.pod(uint32_t(g->objects.size()));
            for (const SceneObject& o : g->objects)
            {
                w.pod(uint32_t(o.name.size()));
                w.bytes(o.name.data(), o.name.size());
                w.pod(uint8_t(o.cls));
            }
            if (version >= 8)
            {
                w.pod(uint32_t(g->tree_instances_json.size()));
                w.bytes(g->tree_instances_json.data(), g->tree_instances_json.size());
            }
        }
    }

    // Version 6: the driving route (none = 0 points).
    {
        const bool has = route && route->polyline.size() >= 2;
        w.pod(uint32_t(has ? route->polyline.size() : 0));
        if (has)
        {
            for (const auto& p : route->polyline) { w.pod(p.first); w.pod(p.second); }
            auto str = [&](const std::string& s) { w.pod(uint32_t(s.size())); w.bytes(s.data(), s.size()); };
            str(route->fromName);
            str(route->toName);
            w.pod(route->fromLon); w.pod(route->fromLat); w.pod(route->toLon); w.pod(route->toLat);
            w.pod(route->lengthM);
        }
    }

    if (version >= 9)
    {
        for (const auto* value : { &capture->id, &capture->name, &capture->createdUtc })
        {
            w.pod(uint32_t(value->size()));
            w.bytes(value->data(), value->size());
        }
    }
    bool ok = w.ok;
    if (fclose(w.f) != 0) ok = false;
    if (!ok) { error = "Write failed (disk full?)."; remove(path.c_str()); }
    return ok;
}

bool LoadScene(const std::string& path, std::vector<BatchMeshData*>& outBatches, std::string& error,
               RoutePlan* route, SceneCapture* capture, bool preserveMeshes)
{
    Reader r;
    r.f = fopen(path.c_str(), "rb");
    if (!r.f) { error = "Cannot open file."; return false; }

    char magic[8] = {};
    r.bytes(magic, sizeof(magic));
    if (!r.ok || memcmp(magic, kMagic, sizeof(kMagic)) != 0)
    {
        fclose(r.f);
        error = "Not a MeshTool scene file.";
        return false;
    }
    uint32_t version = r.pod<uint32_t>();
    if (version < kOldestVersion || version > kVersion)
    {
        fclose(r.f);
        error = "Unsupported scene file version " + std::to_string(version) + ".";
        return false;
    }

    std::vector<BatchMeshData*> batches;
    uint32_t num_batches = r.count(1u << 16);
    for (uint32_t bi = 0; bi < num_batches && r.ok; bi++)
    {
        BatchMeshData* b = new BatchMeshData;
        batches.push_back(b);
        b->is_spline_mesh = r.pod<uint8_t>() != 0;
        b->is_google_dump = r.pod<uint8_t>() != 0;
        if (version >= 2)
            b->is_georeferenced = r.pod<uint8_t>() != 0;
        double rx = r.pod<double>(), ry = r.pod<double>();
        b->reference_pos = core::vec2d(rx, ry);

        uint32_t num_groups = r.count(1u << 20);
        for (uint32_t gi = 0; gi < num_groups && r.ok; gi++)
        {
            GroupMeshData* g = new GroupMeshData;
            b->group_meshes.push_back(g);
            uint32_t num_tex = r.count(1u << 20);
            for (uint32_t ti = 0; ti < num_tex && r.ok; ti++)
                g->loaded_textures.push_back(ReadTexture(r));
            uint32_t num_meshes = r.count(1u << 24);
            for (uint32_t mi = 0; mi < num_meshes && r.ok; mi++)
            {
                MeshData* m = ReadMesh(r, version);
                g->meshes.push_back(m);
                if (m->bbox_ws.b_valid)
                    g->bbox_ws += m->bbox_ws;
            }
            // Named capture snapshots must round-trip without changing geometry.
            // Keep the old capture cleanup only for legacy interactive imports.
            if (!preserveMeshes && version < 9) DropDuplicateMeshes(g);
            if (version >= 3)
            {
                uint32_t num_objects = r.count(1u << 24);
                for (uint32_t oi = 0; oi < num_objects && r.ok; oi++)
                {
                    SceneObject o;
                    uint32_t len = r.count(4096);
                    o.name.resize(len);
                    r.bytes(o.name.data(), len);
                    uint8_t cls = r.pod<uint8_t>();
                    o.cls = cls < kObjClassCount ? ObjectClass(cls) : kObjUnknown;
                    g->objects.push_back(std::move(o));
                }
                // Object bounds are derived, not stored.
                for (MeshData* m : g->meshes)
                {
                    if (m->object_id >= int32_t(g->objects.size())) m->object_id = -1;
                    if (m->object_id >= 0)
                    {
                        auto& object = g->objects[size_t(m->object_id)];
                        if (m->bbox_ws.b_valid) object.bbox_ws += m->bbox_ws;
                        object.hasOriginalModel = object.hasOriginalModel || m->model_variant == 1;
                        object.hasRefinedModel = object.hasRefinedModel || m->model_variant == 2;
                    }
                }
            }
            if (version >= 8)
            {
                uint32_t len = r.count(kMaxBytes);
                g->tree_instances_json.resize(len);
                r.bytes(g->tree_instances_json.data(), len);
            }
            if (g->bbox_ws.b_valid)
                b->bbox_ws += g->bbox_ws;
        }
    }
    RoutePlan loadedRoute;
    if (version >= 6 && r.ok)
    {
        uint32_t points = r.count(1u << 22);
        for (uint32_t i = 0; i < points && r.ok; i++)
        {
            double lon = r.pod<double>(), lat = r.pod<double>();
            loadedRoute.polyline.emplace_back(lon, lat);
        }
        if (points)
        {
            auto str = [&]() {
                uint32_t len = r.count(4096);
                std::string s(len, ' ');
                r.bytes(s.data(), len);
                return s;
            };
            loadedRoute.fromName = str();
            loadedRoute.toName = str();
            loadedRoute.fromLon = r.pod<double>(); loadedRoute.fromLat = r.pod<double>();
            loadedRoute.toLon = r.pod<double>();   loadedRoute.toLat = r.pod<double>();
            loadedRoute.lengthM = r.pod<double>();
            loadedRoute.ok = r.ok && loadedRoute.polyline.size() >= 2;
        }
    }
    SceneCapture loadedCapture;
    if (version >= 9 && r.ok)
    {
        for (auto* value : { &loadedCapture.id, &loadedCapture.name, &loadedCapture.createdUtc })
        {
            uint32_t len = r.count(4096);
            value->resize(len);
            r.bytes(value->data(), len);
        }
        if (loadedCapture.id.empty() || loadedCapture.name.empty()) r.ok = false;
    }
    fclose(r.f);

    if (!r.ok)
    {
        DeleteBatches(batches);
        error = "The file is truncated or corrupt.";
        return false;
    }
    outBatches.insert(outBatches.end(), batches.begin(), batches.end());
    if (capture) *capture = std::move(loadedCapture);
    if (route && loadedRoute.ok)
        *route = std::move(loadedRoute);
    return true;
}
