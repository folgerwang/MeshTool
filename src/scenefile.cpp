#include "scenefile.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include "meshdata.h"
#include "coretexture.h"

namespace
{
    const char     kMagic[8] = { 'M', 'T', 'S', 'C', 'E', 'N', 'E', '\0' };
    const uint32_t kVersion = 5;          // 2: per-batch georeferenced flag, 3: segmented objects, 4: mesh material
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
        return m;
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

bool SaveScene(const std::string& path, const std::vector<BatchMeshData*>& batches, std::string& error)
{
    Writer w;
    w.f = fopen(path.c_str(), "wb");
    if (!w.f) { error = "Cannot open file for writing."; return false; }

    w.bytes(kMagic, sizeof(kMagic));
    w.pod(kVersion);

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
        }
    }

    bool ok = w.ok;
    if (fclose(w.f) != 0) ok = false;
    if (!ok) { error = "Write failed (disk full?)."; remove(path.c_str()); }
    return ok;
}

bool LoadScene(const std::string& path, std::vector<BatchMeshData*>& outBatches, std::string& error)
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
            if (g->bbox_ws.b_valid)
                b->bbox_ws += g->bbox_ws;
        }
    }
    fclose(r.f);

    if (!r.ok)
    {
        DeleteBatches(batches);
        error = "The file is truncated or corrupt.";
        return false;
    }
    outBatches.insert(outBatches.end(), batches.begin(), batches.end());
    return true;
}
