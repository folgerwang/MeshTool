#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
#include <GeographicLib/LocalCartesian.hpp>
#include "meshdata.h"
#include "segment/segmenter.h"   // DecodeTexture

namespace
{
    const double kPi = 3.14159265358979323846;
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    const double kInf = std::numeric_limits<double>::infinity();

    // Sparse grid of point indices in the xy plane.
    struct PointGrid
    {
        double cell = 10.0;
        std::unordered_map<int64_t, std::vector<int>> cells;

        static int64_t Key(int cx, int cy) { return (int64_t(cx) << 32) ^ (int64_t(cy) & 0xffffffffLL); }
        int C(double v) const { return int(floor(v / cell)); }
        void Add(double x, double y, int idx) { cells[Key(C(x), C(y))].push_back(idx); }
        template<class F> void ForRange(double x0, double y0, double x1, double y1, F&& f) const
        {
            const int cx0 = C(x0), cy0 = C(y0), cx1 = C(x1), cy1 = C(y1);
            for (int cx = cx0; cx <= cx1; cx++)
                for (int cy = cy0; cy <= cy1; cy++)
                {
                    auto it = cells.find(Key(cx, cy));
                    if (it == cells.end()) continue;
                    for (int i : it->second) f(i);
                }
        }
    };

    struct Box2
    {
        double x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
        void Add(double x, double y) { x0 = (std::min)(x0, x); y0 = (std::min)(y0, y); x1 = (std::max)(x1, x); y1 = (std::max)(y1, y); }
        bool Overlaps(const Box2& o) const { return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1; }
        bool OverlapsBounds(const core::bounds3d& b) const
        {
            return x0 <= b.bb_max.x && b.bb_min.x <= x1 && y0 <= b.bb_max.y && b.bb_min.y <= y1;
        }
    };

    // Height of the triangle's plane at (x, y) when the point is inside it (seen from above).
    bool TriangleHeight(const core::vec3d& a, const core::vec3d& b, const core::vec3d& c, double x, double y, double& z)
    {
        const double bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y;
        const double d = bx * cy - cx * by;
        if (fabs(d) < 1e-12) return false;
        const double px = x - a.x, py = y - a.y;
        const double u = (px * cy - cx * py) / d;
        const double v = (bx * py - px * by) / d;
        if (u < -1e-9 || v < -1e-9 || u + v > 1.0 + 1e-9) return false;
        z = a.z + u * (b.z - a.z) + v * (c.z - a.z);
        return true;
    }

    // Every drawn mesh of the scene with its group, and the class of the
    // segmented object it belongs to (kObjUnknown when the scene is not
    // segmented).
    template<class F> void ForEachMesh(const std::vector<BatchMeshData*>& batches, F&& f)
    {
        for (BatchMeshData* batch : batches)
        {
            if (!batch) continue;
            for (GroupMeshData* group : batch->group_meshes)
            {
                if (!group) continue;
                for (MeshData* mesh : group->meshes)
                {
                    if (!mesh || !mesh->vertex_list || mesh->num_vertex <= 0) continue;
                    if (!IsMeshModelVisible(group, mesh)) continue;
                    ObjectClass cls = kObjUnknown;
                    if (mesh->object_id >= 0 && size_t(mesh->object_id) < group->objects.size())
                        cls = ObjectClass(group->objects[size_t(mesh->object_id)].cls);
                    f(mesh, group, cls);
                }
            }
        }
    }

    // Triangles of one draw call as index triples (strips unrolled).
    template<class F> void ForEachTriangle(const MeshData* mesh, const DrawCallInfo& dc, F&& f)
    {
        const int n = dc.get_index_count();
        const bool strip = dc.get_primitive_type() == kGlTriangleStrip;
        if (dc.get_primitive_type() != kGlTriangles && !strip) return;
        const int tris = strip ? n - 2 : n / 3;
        const uint32_t nv = uint32_t(mesh->num_vertex);
        for (int k = 0; k < tris; k++)
        {
            const int b = strip ? k : k * 3;
            const uint32_t i0 = dc.get_index(b), i1 = dc.get_index(b + 1), i2 = dc.get_index(b + 2);
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;   // strip restart or bad index
            if (i0 == i1 || i1 == i2 || i0 == i2) continue;   // degenerate
            f(i0, i1, i2);
        }
    }

    core::vec3d WorldVertex(const MeshData* mesh, uint32_t i)
    {
        const core::vec3f& v = mesh->vertex_list[i];
        return core::vec3d(v.x + mesh->translation.x, v.y + mesh->translation.y, v.z + mesh->translation.z);
    }

    double WrapAngle(double a)
    {
        while (a > kPi) a -= 2.0 * kPi;
        while (a < -kPi) a += 2.0 * kPi;
        return a;
    }

    // ---- mesh building -------------------------------------------------

    struct Builder
    {
        std::vector<core::vec3d> v;
        std::vector<uint32_t>    i;

        void Tri(const core::vec3d& a, const core::vec3d& b, const core::vec3d& c)
        {
            const uint32_t n = uint32_t(v.size());
            v.push_back(a); v.push_back(b); v.push_back(c);
            i.push_back(n); i.push_back(n + 1); i.push_back(n + 2);
        }
        void Quad(const core::vec3d& a, const core::vec3d& b, const core::vec3d& c, const core::vec3d& d)
        {
            const uint32_t n = uint32_t(v.size());
            v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
            i.push_back(n); i.push_back(n + 1); i.push_back(n + 2);
            i.push_back(n); i.push_back(n + 2); i.push_back(n + 3);
        }
        // Six faces between a bottom and a top quad (corners in matching order).
        void Frustum(const core::vec3d b[4], const core::vec3d t[4])
        {
            Quad(b[0], b[3], b[2], b[1]);
            Quad(t[0], t[1], t[2], t[3]);
            for (int k = 0; k < 4; k++)
            {
                const int k1 = (k + 1) % 4;
                Quad(b[k], b[k1], t[k1], t[k]);
            }
        }
        void Box(const core::vec3d& lo, const core::vec3d& hi)
        {
            const core::vec3d b[4] = { {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z} };
            const core::vec3d t[4] = { {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z} };
            Frustum(b, t);
        }
        // Cylinder along the x axis.
        void CylinderX(const core::vec3d& c, double radius, double halfWidth, int segments)
        {
            for (int k = 0; k < segments; k++)
            {
                const double a0 = 2.0 * kPi * k / segments, a1 = 2.0 * kPi * (k + 1) / segments;
                const core::vec3d r0(0.0, cos(a0) * radius, sin(a0) * radius);
                const core::vec3d r1(0.0, cos(a1) * radius, sin(a1) * radius);
                const core::vec3d l(c.x - halfWidth, c.y, c.z), r(c.x + halfWidth, c.y, c.z);
                Quad(l + r0, l + r1, r + r1, r + r0);
                Tri(l, l + r1, l + r0);
                Tri(r, r + r0, r + r1);
            }
        }

        std::unique_ptr<MeshData> Make(const core::vec3d& origin) const
        {
            auto m = std::make_unique<MeshData>();
            const int n = int(v.size());
            m->num_vertex = n;
            m->translation = origin;
            m->bbox_ws.Reset();
            if (n > 0)
            {
                m->vertex_list = std::make_unique<core::vec3f[]>(n);
                for (int k = 0; k < n; k++)
                {
                    m->vertex_list[k] = core::vec3f(float(v[k].x - origin.x), float(v[k].y - origin.y), float(v[k].z - origin.z));
                    m->bbox_ws += v[k];
                }
            }
            m->add_draw_call_list(kGlTriangles, int(i.size()), n);
            DrawCallInfo& dc = m->get_last_draw_call_info();
            for (uint32_t idx : i) dc.add_index(idx);
            return m;
        }
    };

    struct Mat4
    {
        float m[16];   // column-major
        static Mat4 Identity()
        {
            Mat4 r = {};
            r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
            return r;
        }
        static Mat4 Translate(double x, double y, double z)
        {
            Mat4 r = Identity();
            r.m[12] = float(x); r.m[13] = float(y); r.m[14] = float(z);
            return r;
        }
        static Mat4 RotZ(double a)
        {
            Mat4 r = Identity();
            const float c = float(cos(a)), s = float(sin(a));
            r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
            return r;
        }
        static Mat4 RotX(double a)
        {
            Mat4 r = Identity();
            const float c = float(cos(a)), s = float(sin(a));
            r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
            return r;
        }
        Mat4 operator*(const Mat4& b) const
        {
            Mat4 r = {};
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++)
                {
                    float acc = 0.0f;
                    for (int k = 0; k < 4; k++) acc += m[k * 4 + row] * b.m[c * 4 + k];
                    r.m[c * 4 + row] = acc;
                }
            return r;
        }
    };

    // Right-hand unit vector of a heading (radians clockwise from north).
    core::vec3d RightOf(double heading) { return core::vec3d(cos(heading), -sin(heading), 0.0); }
}

// ---------------------------------------------------------------------------
// Road profile
// ---------------------------------------------------------------------------

core::vec3d DriveRoad::PoseAt(double s, double* heading, double* pitch) const
{
    if (stations.empty())
    {
        if (heading) *heading = 0.0;
        if (pitch) *pitch = 0.0;
        return core::vec3d(0.0, 0.0, 0.0);
    }
    if (stations.size() == 1 || s <= stations.front().s)
    {
        if (heading) *heading = stations.front().heading;
        if (pitch) *pitch = 0.0;
        return stations.front().pos;
    }
    if (s >= stations.back().s)
    {
        if (heading) *heading = stations.back().heading;
        if (pitch) *pitch = 0.0;
        return stations.back().pos;
    }
    size_t hi = size_t(std::upper_bound(stations.begin(), stations.end(), s,
                                        [](double v, const RoadStation& st) { return v < st.s; }) - stations.begin());
    const RoadStation& a = stations[hi - 1];
    const RoadStation& b = stations[hi];
    const double ds = (std::max)(b.s - a.s, 1e-6);
    const double t = (s - a.s) / ds;
    if (heading) *heading = a.heading + WrapAngle(b.heading - a.heading) * t;
    if (pitch) *pitch = atan2(b.pos.z - a.pos.z, ds);
    return a.pos + (b.pos - a.pos) * t;
}

double DriveRoad::LiftAt(double s) const
{
    if (stations.empty()) return 0.0;
    if (s <= stations.front().s) return stations.front().lift;
    if (s >= stations.back().s) return stations.back().lift;
    size_t hi = size_t(std::upper_bound(stations.begin(), stations.end(), s,
                                        [](double v, const RoadStation& st) { return v < st.s; }) - stations.begin());
    const RoadStation& a = stations[hi - 1];
    const RoadStation& b = stations[hi];
    const double t = (s - a.s) / (std::max)(b.s - a.s, 1e-6);
    return a.lift + (b.lift - a.lift) * t;
}

DriveRoad BuildDriveRoad(const std::vector<std::pair<double, double>>& polyline, double refLon, double refLat,
                         const std::vector<BatchMeshData*>& batches, const DriveRoadParams& p)
{
    DriveRoad road;
    if (polyline.size() < 2)
    {
        road.error = "the route has no polyline";
        return road;
    }

    // Route to scene coordinates (East/North metres).
    GeographicLib::LocalCartesian local(refLat, refLon, 0.0);
    std::vector<core::vec2d> pts;
    for (const auto& ll : polyline)
    {
        double x, y, z;
        local.Forward(ll.second, ll.first, 0.0, x, y, z);
        if (!pts.empty() && fabs(pts.back().x - x) < 1e-3 && fabs(pts.back().y - y) < 1e-3) continue;
        pts.push_back(core::vec2d(x, y));
    }
    if (pts.size() < 2)
    {
        road.error = "the route has no length";
        return road;
    }

    // Stations every `stationStep` metres along the polyline.
    const double step = (std::max)(0.05, p.stationStep);
    const size_t per2m = std::max<size_t>(1, size_t(2.0 / step + 0.5));   // stations in 2 m
    std::vector<core::vec2d> centre;
    double travelled = 0.0, nextAt = 0.0;
    for (size_t i = 0; i + 1 < pts.size(); i++)
    {
        const double dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
        const double len = sqrt(dx * dx + dy * dy);
        if (len <= 0.0) continue;
        while (nextAt <= travelled + len)
        {
            const double t = (nextAt - travelled) / len;
            centre.push_back(core::vec2d(pts[i].x + dx * t, pts[i].y + dy * t));
            nextAt += step;
        }
        travelled += len;
    }
    {
        const core::vec2d& e = pts.back();
        if (centre.empty() || hypot(e.x - centre.back().x, e.y - centre.back().y) > 0.25 * step)
            centre.push_back(e);
    }
    if (centre.size() < 2)
    {
        road.error = "the route is shorter than one station";
        return road;
    }
    // Round the polyline's corners a little so the car does not snap its heading.
    for (int pass = 0; pass < 2; pass++)
    {
        std::vector<core::vec2d> sm = centre;
        for (size_t i = 1; i + 1 < centre.size(); i++)
        {
            sm[i].x = 0.25 * centre[i - 1].x + 0.5 * centre[i].x + 0.25 * centre[i + 1].x;
            sm[i].y = 0.25 * centre[i - 1].y + 0.5 * centre[i].y + 0.25 * centre[i + 1].y;
        }
        centre.swap(sm);
    }

    const size_t n = centre.size();
    road.stations.resize(n);
    for (size_t i = 0; i < n; i++)
    {
        RoadStation& st = road.stations[i];
        st.pos = core::vec3d(centre[i].x, centre[i].y, 0.0);
        st.s = i == 0 ? 0.0 : road.stations[i - 1].s + hypot(centre[i].x - centre[i - 1].x, centre[i].y - centre[i - 1].y);
        const size_t i0 = i == 0 ? 0 : i - 1, i1 = i + 1 < n ? i + 1 : i;
        st.heading = atan2(centre[i1].x - centre[i0].x, centre[i1].y - centre[i0].y);
    }
    road.length = road.stations.back().s;

    // Sample points across the inner part of the road at every station. The
    // band stays well inside the carriageway, so a ditch or a bank beside a
    // route line that sits a little off the real road does not count. Beside
    // the road, a few samples per side (past the asphalt edge) see the trees
    // standing next to it: where the road is exposed, their height over it
    // is the tree height.
    const int kLateral = 7, kSide = 3;
    const double bandHalf = (std::min)(0.3 * p.roadWidth, 2.5);
    const double sideFrom = 0.5 * p.roadWidth + 1.5, sideStep = 1.5;
    const size_t bandCount = n * kLateral, total = bandCount + n * 2 * kSide;
    std::vector<double> sx(total), sy(total), top(total, -kInf), low(total, kInf);
    PointGrid grid;
    grid.cell = 4.0;
    Box2 sampleBox;
    for (size_t i = 0; i < n; i++)
    {
        const core::vec3d right = RightOf(road.stations[i].heading);
        auto put = [&](size_t idx, double off) {
            sx[idx] = road.stations[i].pos.x + right.x * off;
            sy[idx] = road.stations[i].pos.y + right.y * off;
            grid.Add(sx[idx], sy[idx], int(idx));
            sampleBox.Add(sx[idx], sy[idx]);
        };
        for (int k = 0; k < kLateral; k++)
            put(i * kLateral + k, bandHalf * (2.0 * k / (kLateral - 1) - 1.0));
        for (int k = 0; k < kSide; k++)
        {
            put(bandCount + i * 2 * kSide + k, sideFrom + sideStep * k);
            put(bandCount + i * 2 * kSide + kSide + k, -(sideFrom + sideStep * k));
        }
    }

    // One pass over the scene's triangles: each sample keeps its highest and
    // lowest hit (a bridge deck over water, a canopy over ground). Segmented
    // tree objects do not count on the road band; segmented buildings do not
    // count beside it (only trees measure the tree height).
    ForEachMesh(batches, [&](MeshData* mesh, GroupMeshData*, ObjectClass cls) {
        if (mesh->bbox_ws.b_valid && !sampleBox.OverlapsBounds(mesh->bbox_ws)) return;
        const bool isTree = cls == kObjTree, isBuilding = cls == kObjBuilding;
        for (const DrawCallInfo& dc : mesh->draw_call_list)
        {
            if (!dc.is_drawable()) continue;
            ForEachTriangle(mesh, dc, [&](uint32_t i0, uint32_t i1, uint32_t i2) {
                const core::vec3d a = WorldVertex(mesh, i0), b = WorldVertex(mesh, i1), c = WorldVertex(mesh, i2);
                Box2 tb;
                tb.Add(a.x, a.y); tb.Add(b.x, b.y); tb.Add(c.x, c.y);
                if (!tb.Overlaps(sampleBox)) return;
                grid.ForRange(tb.x0, tb.y0, tb.x1, tb.y1, [&](int idx) {
                    const bool band = size_t(idx) < bandCount;
                    if (band ? isTree : isBuilding) return;
                    double z;
                    if (!TriangleHeight(a, b, c, sx[idx], sy[idx], z)) return;
                    if (z > top[idx]) top[idx] = z;
                    if (z < low[idx]) low[idx] = z;
                });
            });
        }
    });

    // Per station and layer (the top surface, and a distinct lower layer when
    // there is one under it): the height across the band (median) and how
    // uneven it is (range).
    std::vector<double> s(n), zTop(n, kNaN), zLow(n, kNaN);
    std::vector<bool> roughTop(n, true), roughLow(n, true);
    {
        std::vector<double> t, l;
        for (size_t i = 0; i < n; i++)
        {
            s[i] = road.stations[i].s;
            t.clear(); l.clear();
            for (int k = 0; k < kLateral; k++)
            {
                const size_t idx = i * kLateral + k;
                if (top[idx] > -kInf) t.push_back(top[idx]);
                if (low[idx] < kInf && low[idx] < top[idx] - 1.0) l.push_back(low[idx]);
            }
            if (t.size() < 4)
            {
                road.stations[i].missing = true;
                road.stations[i].surfaceZ = kNaN;
                road.missing++;
                continue;
            }
            road.sampled++;
            std::sort(t.begin(), t.end());
            zTop[i] = t[t.size() / 2];
            roughTop[i] = t.back() - t.front() > p.roughness;
            road.stations[i].surfaceZ = zTop[i];
            road.stations[i].rough = roughTop[i];
            if (l.size() >= 4)
            {
                std::sort(l.begin(), l.end());
                zLow[i] = l[l.size() / 2];
                roughLow[i] = l.back() - l.front() > p.roughness;
            }
        }
    }
    if (road.sampled == 0)
    {
        road.error = "no captured surface under the route (is the scene the one captured along it?)";
        return road;
    }
    // Bumpy along the route as well: a road is flat from one station to the next.
    auto bumpy = [&](const std::vector<double>& zz, std::vector<bool>& rough) {
        std::vector<bool> out = rough;
        for (size_t i = per2m; i + per2m < n; i++)
            if (!rough[i] && !std::isnan(zz[i - per2m]) && !std::isnan(zz[i + per2m]) &&
                fabs(zz[i] - 0.5 * (zz[i - per2m] + zz[i + per2m])) > 0.6 * p.roughness)
                out[i] = true;
        rough.swap(out);
    };
    bumpy(zTop, roughTop);
    bumpy(zLow, roughLow);

    // Runs: stretches of smooth stations without a step between neighbours,
    // in either layer. Tree tops are bumpy and sit metres above the road;
    // the road itself, bridge decks included, is flat and continuous.
    struct Run { size_t first, last; bool low; };
    std::vector<Run> runs;
    auto buildRuns = [&](const std::vector<double>& zz, const std::vector<bool>& rough, bool lowLayer) {
        for (size_t i = 0; i < n; i++)
        {
            if (rough[i] || std::isnan(zz[i])) continue;
            size_t j = i;
            while (j + 1 < n && !rough[j + 1] && !std::isnan(zz[j + 1]) &&
                   fabs(zz[j + 1] - zz[j]) <= p.maxGrade * (s[j + 1] - s[j]) + 0.15 &&
                   (j + 1 < i + per2m || fabs(zz[j + 1] - zz[j + 1 - per2m]) <= p.maxGrade * (s[j + 1] - s[j + 1 - per2m]) + 0.3))
                j++;
            if (s[j] - s[i] >= 4.0) runs.push_back(Run{ i, j, lowLayer });
            i = j;
        }
    };
    buildRuns(zTop, roughTop, false);
    buildRuns(zLow, roughLow, true);
    if (runs.empty())
    {
        road.error = "no flat road surface found along the route (only trees or gaps)";
        return road;
    }
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.first < b.first; });

    // Choose the chain of runs that is the road: the one covering the most
    // length with the fewest unexplained steps between consecutive runs (a
    // step beyond what a road climb over the gap allows costs as much as
    // `kStepCost` metres of coverage). Tree tops smooth enough to form runs
    // step up and down again, so they lose unless very long; water or a hole
    // under a missing bridge deck steps down and loses the same way. Runs may
    // not overlap, so a lower layer (ground seen through the trees) competes
    // with the canopy above it and wins by joining the road without a step.
    const double kStepCost = 60.0;   // metres of coverage per metre of step
    const double kClimb = 0.12;      // a road may climb this much per metre of gap
    const double kJoinCost = 15.0;   // per join: a short run after a long gap (a smooth patch of tree top
                                     // that the climb allowance would let in) is not worth taking
    auto zAt = [&](const Run& r, size_t i) { return r.low ? zLow[i] : zTop[i]; };
    std::vector<double> best(runs.size(), 0.0);
    std::vector<int> prev(runs.size(), -1);
    for (size_t j = 0; j < runs.size(); j++)
    {
        const Run& rj = runs[j];
        const double len = (s[rj.last] - s[rj.first]) * (rj.low ? 0.9 : 1.0);   // prefer the top when both fit
        best[j] = len;
        for (size_t i = 0; i < j; i++)
        {
            const Run& ri = runs[i];
            if (ri.last >= rj.first) continue;   // overlapping (the other layer)
            const double gap = s[rj.first] - s[ri.last];
            const double step = (std::max)(0.0, fabs(zAt(rj, rj.first) - zAt(ri, ri.last)) - p.treeHeight - kClimb * gap);
            const double cand = best[i] - kStepCost * step - kJoinCost + len;
            if (cand > best[j]) { best[j] = cand; prev[j] = int(i); }
        }
    }
    std::vector<double> z(n, kNaN);
    {
        size_t bestRun = 0;
        for (size_t j = 1; j < runs.size(); j++) if (best[j] > best[bestRun]) bestRun = j;
        for (int j = int(bestRun); j >= 0; j = prev[size_t(j)])
        {
            const Run& r = runs[size_t(j)];
            for (size_t i = r.first; i <= r.last; i++) z[i] = zAt(r, i);
        }
    }
    for (size_t i = 0; i < n; i++)
        if (std::isnan(z[i]) && !road.stations[i].missing)
        {
            road.stations[i].tree = true;   // canopy, or clutter on the road (parked cars)
            road.rejected++;
        }
    // The tree height: at the kept stations (the road is exposed there) the
    // surface beside the road that stands more than the canopy step above it
    // is a tree; the median rise over the road is what the height check
    // subtracts from the canopy top where trees cover the road. Anything far
    // taller is a building or a wall and does not count. Trees differ along
    // a route, so every station gets the median of the rises seen within
    // 100 m of it (300 m when few were seen there, the whole route when
    // still few).
    {
        const double kMaxTreeRise = 40.0, kLocal = 100.0;
        const size_t kEnough = 12;
        std::vector<double> rises, riseS;   // rise, and the station distance it was seen at (ascending)
        for (size_t i = 0; i < n; i++)
        {
            if (std::isnan(z[i])) continue;
            for (int k = 0; k < 2 * kSide; k++)
            {
                const double tz = top[bandCount + i * 2 * kSide + size_t(k)];
                if (tz == -kInf) continue;
                const double rise = tz - z[i];
                if (rise > p.treeHeight && rise < kMaxTreeRise)
                {
                    rises.push_back(rise);
                    riseS.push_back(s[i]);
                }
            }
        }
        road.canopySamples = int(rises.size());
        if (rises.empty())
        {
            road.canopyHeight = kNaN;
            for (size_t i = 0; i < n; i++) road.stations[i].treeHeight = kNaN;
        }
        else
        {
            std::vector<double> tmp;
            auto medianOf = [&](std::vector<double>& v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            tmp = rises;
            road.canopyHeight = medianOf(tmp);
            // Evaluated every 5 m, held in between.
            const size_t stride = std::max<size_t>(1, size_t(5.0 / step + 0.5));
            for (size_t i = 0; i < n; i += stride)
            {
                auto within = [&](double radius) {
                    const size_t a = size_t(std::lower_bound(riseS.begin(), riseS.end(), s[i] - radius) - riseS.begin());
                    const size_t b = size_t(std::upper_bound(riseS.begin(), riseS.end(), s[i] + radius) - riseS.begin());
                    tmp.assign(rises.begin() + ptrdiff_t(a), rises.begin() + ptrdiff_t(b));
                };
                within(kLocal);
                if (tmp.size() < kEnough) within(3.0 * kLocal);
                const double v = tmp.size() >= kEnough ? medianOf(tmp) : road.canopyHeight;
                for (size_t j = i; j < n && j < i + stride; j++) road.stations[j].treeHeight = v;
            }
        }
    }
    // Fill the dropped and missing stations between the kept ones. Start
    // from a straight chord across each gap, then let the profile follow the
    // captured surface wherever that stays near it: the local median of the
    // top surface (a road the flatness test rejected for clutter or a band
    // straddling its edge), else the surface floor (the ground seen through
    // holes in the canopy, the lowest top surface within a few metres).
    // Repeating this a few times lets the profile climb a hill through the
    // canopy holes from both ends. A long stretch of solid canopy, where
    // neither comes near, follows the canopy floor lowered by the tree height
    // seen where the road enters and leaves it. What remains stays straight.
    std::vector<bool> underCanopy(n, false);   // filled from the surface: smoothed harder
    std::vector<bool> seenSurface(n, false);   // filled from a surface near the profile (not a canopy guess)
    {
        // Local median of the top surface over +-2 m, and its floor over +-8 m.
        std::vector<double> zMed(n, kNaN), zFloor(n, kNaN);
        {
            std::vector<double> win;
            for (size_t i = 0; i < n; i++)
            {
                if (std::isnan(zTop[i])) continue;
                win.clear();
                for (size_t j = i >= per2m ? i - per2m : 0; j <= i + per2m && j < n; j++)
                    if (!std::isnan(zTop[j])) win.push_back(zTop[j]);
                std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
                zMed[i] = win[win.size() / 2];
            }
            const size_t half = std::max<size_t>(1, size_t(8.0 / step + 0.5));
            std::deque<size_t> window;   // indices with increasing zTop (monotonic queue)
            size_t next = 0;
            for (size_t i = 0; i < n; i++)
            {
                for (; next < n && next <= i + half; next++)
                {
                    if (std::isnan(zTop[next])) continue;
                    while (!window.empty() && zTop[window.back()] >= zTop[next]) window.pop_back();
                    window.push_back(next);
                }
                while (!window.empty() && window.front() + half < i) window.pop_front();
                if (!window.empty()) zFloor[i] = zTop[window.front()];
            }
        }
        // Canopy height over the road at a gap's edge: the median rise of the
        // surface floor over the road height 15..25 m into the gap (nearer the
        // edge the floor still sees the road), counting only stations up in
        // the trees.
        auto edgeOffset = [&](size_t from, int dir, double roadZ) {
            std::vector<double> offs;
            for (size_t j = from, count = 0; j < n && count < 5 * per2m; j = size_t(int(j) + dir), count++)
            {
                if (!std::isnan(zFloor[j]) && zFloor[j] - roadZ > p.treeHeight) offs.push_back(zFloor[j] - roadZ);
                if (dir < 0 && j == 0) break;
            }
            if (offs.size() < per2m / 2 + 1) return kNaN;
            std::sort(offs.begin(), offs.end());
            return offs[offs.size() / 2];
        };
        // Gaps between kept stations; the stretches before the first and
        // after the last kept station are gaps with one side open (chord
        // flat from the kept side).
        struct Gap { size_t prev, next; };   // kept stations either side (n = none)
        std::vector<Gap> gaps;
        {
            size_t prev = n;
            for (size_t i = 0; i < n; i++)
            {
                if (std::isnan(z[i])) continue;
                if (prev == n) { if (i > 0) gaps.push_back(Gap{ n, i }); }
                else if (i > prev + 1) gaps.push_back(Gap{ prev, i });
                prev = i;
            }
            if (prev + 1 < n) gaps.push_back(Gap{ prev, n });
        }
        // The straight chords, the starting reference.
        std::vector<double> ref = z;
        for (const Gap& g : gaps)
        {
            const size_t a = g.prev == n ? g.next : g.prev, b = g.next == n ? g.prev : g.next;
            const size_t from = g.prev == n ? 0 : g.prev + 1, to = g.next == n ? n : g.next;
            for (size_t j = from; j < to; j++)
            {
                const double t = a == b ? 0.0 : (s[j] - s[a]) / (std::max)(s[b] - s[a], 1e-6);
                ref[j] = z[a] + (z[b] - z[a]) * t;
            }
        }
        const double kLongGap = 40.0;
        const size_t inset = size_t(15.0 / step + 0.5);
        for (int pass = 0; pass < 3; pass++)
        {
            for (const Gap& g : gaps)
            {
                const bool closed = g.prev != n && g.next != n;
                const size_t prev = g.prev, i = g.next;
                const size_t from = prev == n ? 0 : prev + 1, to = i == n ? n : i;
                const double gap = closed ? s[i] - s[prev] : 0.0;
                double offA = kNaN, offB = kNaN;
                if (pass == 0 && closed && gap > kLongGap && prev + 1 + inset < i)
                {
                    offA = edgeOffset(prev + 1 + inset, +1, z[prev]);
                    offB = edgeOffset(i - 1 - inset, -1, z[i]);
                }
                const bool canopy = !std::isnan(offA) && !std::isnan(offB);
                for (size_t j = from; j < to; j++)
                {
                    const double base = ref[j];
                    const double t = closed ? (s[j] - s[prev]) / (std::max)(gap, 1e-6) : 0.0;
                    seenSurface[j] = false;
                    if (!std::isnan(zMed[j]) && fabs(zMed[j] - base) <= p.treeHeight)
                    {
                        z[j] = zMed[j];
                        underCanopy[j] = true;
                        seenSurface[j] = true;
                    }
                    else if (!std::isnan(zFloor[j]) && fabs(zFloor[j] - base) <= p.treeHeight)
                    {
                        z[j] = zFloor[j];
                        underCanopy[j] = true;
                        seenSurface[j] = true;
                    }
                    else if (canopy && !std::isnan(zFloor[j]) && zFloor[j] - base > p.treeHeight)
                    {
                        z[j] = zFloor[j] - (offA * (1.0 - t) + offB * t);
                        underCanopy[j] = true;
                    }
                    else
                        z[j] = base;
                }
            }
            if (pass + 1 < 3)
            {
                // Next pass measures against this pass's result, lightly smoothed.
                const double sigma = (std::max)(1.0, p.smoothSigma);
                for (size_t i = 0; i < n; i++)
                {
                    double acc = 0.0, wsum = 0.0;
                    for (size_t j = i; j < n && s[j] - s[i] <= 2.0 * sigma; j++)
                    {
                        const double w = exp(-0.5 * (s[j] - s[i]) * (s[j] - s[i]) / (sigma * sigma));
                        acc += w * z[j]; wsum += w;
                    }
                    for (size_t j = i; j-- > 0 && s[i] - s[j] <= 2.0 * sigma;)
                    {
                        const double w = exp(-0.5 * (s[j] - s[i]) * (s[j] - s[i]) / (sigma * sigma));
                        acc += w * z[j]; wsum += w;
                    }
                    ref[i] = wsum > 0.0 ? acc / wsum : z[i];
                }
            }
        }
    }
    for (size_t i = 0; i < n; i++)
        if (underCanopy[i]) { road.stations[i].guided = true; road.guided++; }
    for (size_t i = 0; i < n; i++) road.stations[i].seen = seenSurface[i];

    // Smoothing: light everywhere, heavy where the profile follows the bumpy
    // canopy top, blended over 10 m so the two meet without a step.
    if (p.smoothSigma > 0.0)
    {
        auto gaussian = [&](const std::vector<double>& in, double sigma) {
            std::vector<double> out(n);
            for (size_t i = 0; i < n; i++)
            {
                double acc = 0.0, wsum = 0.0;
                for (size_t j = i; j < n && s[j] - s[i] <= 3.0 * sigma; j++)
                {
                    const double w = exp(-0.5 * (s[j] - s[i]) * (s[j] - s[i]) / (sigma * sigma));
                    acc += w * in[j]; wsum += w;
                }
                for (size_t j = i; j-- > 0 && s[i] - s[j] <= 3.0 * sigma;)
                {
                    const double w = exp(-0.5 * (s[j] - s[i]) * (s[j] - s[i]) / (sigma * sigma));
                    acc += w * in[j]; wsum += w;
                }
                out[i] = wsum > 0.0 ? acc / wsum : in[i];
            }
            return out;
        };
        const std::vector<double> light = gaussian(z, p.smoothSigma);
        bool anyCanopy = false;
        for (size_t i = 0; i < n; i++) anyCanopy = anyCanopy || underCanopy[i];
        if (!anyCanopy)
            z = light;
        else
        {
            const std::vector<double> heavy = gaussian(z, 3.0 * p.smoothSigma);
            // Weight 1 under the canopy, fading to 0 over 10 m outside it.
            const double fade = 10.0;
            std::vector<double> w(n, 0.0);
            double lastCanopy = -1e30;
            for (size_t i = 0; i < n; i++)
            {
                if (underCanopy[i]) lastCanopy = s[i];
                w[i] = (std::max)(w[i], (std::max)(0.0, 1.0 - (s[i] - lastCanopy) / fade));
            }
            lastCanopy = 1e30;
            for (size_t i = n; i-- > 0;)
            {
                if (underCanopy[i]) lastCanopy = s[i];
                w[i] = (std::max)(w[i], (std::max)(0.0, 1.0 - (lastCanopy - s[i]) / fade));
            }
            for (size_t i = 0; i < n; i++) z[i] = w[i] * heavy[i] + (1.0 - w[i]) * light[i];
        }
    }
    for (size_t i = 0; i < n; i++) road.stations[i].pos.z = z[i];
    road.ok = true;
    return road;
}

HeightCheck CheckRoadHeight(const DriveRoad& road, double s, double window, double tolerance, double treeHeight)
{
    HeightCheck r;
    if (!road.ok || road.stations.size() < 2) return r;
    const auto& st = road.stations;
    const size_t lo = size_t(std::lower_bound(st.begin(), st.end(), s - window,
                                              [](const RoadStation& a, double v) { return a.s < v; }) - st.begin());
    const size_t hi = size_t(std::upper_bound(st.begin(), st.end(), s + window,
                                              [](double v, const RoadStation& a) { return v < a.s; }) - st.begin());
    if (hi <= lo) return r;
    // How much of the road is exposed over the window (stations whose
    // surface the profile follows), and the surface and tree height over a
    // wider stretch (the canopy top is bumpy: a short window would send the
    // guess up and down from one check to the next).
    size_t exposed = 0;
    for (size_t i = lo; i < hi; i++)
        if (!st[i].missing && (!st[i].tree || st[i].seen)) exposed++;   // the road, or the ground through the trees, is in view
    const size_t count = hi - lo;
    if (exposed * 2 >= count) return r;   // the road is in view from above: the profile is on it
    const double wide = (std::max)(window, 20.0);
    const size_t wlo = size_t(std::lower_bound(st.begin(), st.end(), s - wide,
                                               [](const RoadStation& a, double v) { return a.s < v; }) - st.begin());
    const size_t whi = size_t(std::upper_bound(st.begin(), st.end(), s + wide,
                                               [](double v, const RoadStation& a) { return v < a.s; }) - st.begin());
    std::vector<double> all, trees;
    for (size_t i = wlo; i < whi; i++)
    {
        if (st[i].missing || std::isnan(st[i].surfaceZ)) continue;
        all.push_back(st[i].surfaceZ);
        if (!std::isnan(st[i].treeHeight)) trees.push_back(st[i].treeHeight);
    }
    if (all.size() * 2 < whi - wlo || trees.empty()) return r;   // nothing to guess from
    auto median = [](std::vector<double>& v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const double top = median(all);
    const double carZ = road.PoseAt(s, nullptr, nullptr).z;
    if (top - carZ <= treeHeight) return r;   // not under trees: clutter on the road, which the profile follows
    // Trees over the road: the road is the tree height under their tops.
    const double trees_ = median(trees);
    double guess = top - trees_;
    // A road climbs only so fast: the guess stays within 12% of the distance
    // to the nearest exposed road of the chord between the exposed road on
    // either side (flat from one side when the other is never exposed).
    {
        const double kClimb = 0.12, kReach = 2000.0;
        auto isExposed = [&](size_t i) { return !st[i].missing && (!st[i].tree || st[i].seen); };
        size_t a = lo, b = hi;
        bool hasA = false, hasB = false;
        for (size_t i = lo; i-- > 0 && s - st[i].s <= kReach;)
            if (isExposed(i)) { a = i; hasA = true; break; }
        for (size_t i = hi; i < st.size() && st[i].s - s <= kReach; i++)
            if (isExposed(i)) { b = i; hasB = true; break; }
        if (hasA || hasB)
        {
            double chord, dist;
            if (hasA && hasB)
            {
                const double t = (s - st[a].s) / (std::max)(st[b].s - st[a].s, 1e-6);
                chord = st[a].pos.z + (st[b].pos.z - st[a].pos.z) * t;
                dist = (std::min)(s - st[a].s, st[b].s - s);
            }
            else if (hasA) { chord = st[a].pos.z; dist = s - st[a].s; }
            else { chord = st[b].pos.z; dist = st[b].s - s; }
            const double allow = kClimb * dist + 0.3;
            r.chordZ = chord;
            if (guess > chord + allow) { guess = chord + allow; r.clamped = true; }
            if (guess < chord - allow) { guess = chord - allow; r.clamped = true; }
        }
    }
    const double d = guess - carZ;
    if (fabs(d) > tolerance)
    {
        r.kind = HeightCheck::kUnderCanopy;
        r.lift = d;
        r.surfaceZ = top;
        r.treeHeight = trees_;
    }
    return r;
}

int ApplyHeightChecks(DriveRoad& road, double every, double tolerance, double treeHeight)
{
    if (!road.ok || road.stations.size() < 2) return 0;
    every = (std::max)(1.0, every);
    // All checks first, on the profile as recovered.
    std::vector<double> at, lift;
    int changed = 0;
    for (double s = 0.0;; s += every)
    {
        s = (std::min)(s, road.length);
        const HeightCheck c = CheckRoadHeight(road, s, 0.5 * every, tolerance, treeHeight);
        at.push_back(s);
        lift.push_back(c.lift);
        if (c.kind != HeightCheck::kNone) changed++;
        if (s >= road.length) break;
    }
    // Then the profile moves by the lift, interpolated between the checks.
    size_t k = 0;
    for (RoadStation& st : road.stations)
    {
        while (k + 1 < at.size() && at[k + 1] < st.s) k++;
        double v = lift[k];
        if (k + 1 < at.size())
        {
            const double t = (std::min)(1.0, (std::max)(0.0, (st.s - at[k]) / (std::max)(at[k + 1] - at[k], 1e-6)));
            v = lift[k] + (lift[k + 1] - lift[k]) * t;
        }
        st.lift = v;
    }
    // Smoothed over the check distance, so the car does not pitch up and
    // down between one guess and the next.
    {
        const size_t n = road.stations.size();
        const double sigma = every;
        std::vector<double> sm(n);
        for (size_t i = 0; i < n; i++)
        {
            const double si = road.stations[i].s;
            double acc = 0.0, wsum = 0.0;
            for (size_t j = i; j < n && road.stations[j].s - si <= 3.0 * sigma; j++)
            {
                const double d = road.stations[j].s - si, w = exp(-0.5 * d * d / (sigma * sigma));
                acc += w * road.stations[j].lift; wsum += w;
            }
            for (size_t j = i; j-- > 0 && si - road.stations[j].s <= 3.0 * sigma;)
            {
                const double d = road.stations[j].s - si, w = exp(-0.5 * d * d / (sigma * sigma));
                acc += w * road.stations[j].lift; wsum += w;
            }
            sm[i] = wsum > 0.0 ? acc / wsum : road.stations[i].lift;
        }
        for (size_t i = 0; i < n; i++)
        {
            road.stations[i].lift = sm[i];
            road.stations[i].pos.z += sm[i];
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Car
// ---------------------------------------------------------------------------

CarMeshes BuildCarMeshes()
{
    CarMeshes car;
    const core::vec3d o(0.0, 0.0, 0.0);
    {
        // Lower body with a sloping bonnet and boot, then the cabin on top.
        Builder b;
        const core::vec3d lo[4] = { {-0.90, -2.25, 0.35}, {0.90, -2.25, 0.35}, {0.90, 2.25, 0.35}, {-0.90, 2.25, 0.35} };
        const core::vec3d hi[4] = { {-0.88, -2.15, 0.95}, {0.88, -2.15, 0.95}, {0.88, 2.05, 0.90}, {-0.88, 2.05, 0.90} };
        b.Frustum(lo, hi);
        const core::vec3d clo[4] = { {-0.82, -1.55, 0.94}, {0.82, -1.55, 0.94}, {0.82, 0.95, 0.89}, {-0.82, 0.95, 0.89} };
        const core::vec3d chi[4] = { {-0.74, -1.25, 1.48}, {0.74, -1.25, 1.48}, {0.74, 0.35, 1.48}, {-0.74, 0.35, 1.48} };
        b.Frustum(clo, chi);
        car.body = b.Make(o);
    }
    {
        // Windows: the cabin's sides, slightly outset, stopping short of the roof.
        Builder b;
        const core::vec3d clo[4] = { {-0.84, -1.58, 1.00}, {0.84, -1.58, 1.00}, {0.84, 0.99, 0.96}, {-0.84, 0.99, 0.96} };
        const core::vec3d chi[4] = { {-0.76, -1.28, 1.40}, {0.76, -1.28, 1.40}, {0.76, 0.38, 1.40}, {-0.76, 0.38, 1.40} };
        for (int k = 0; k < 4; k++)
        {
            const int k1 = (k + 1) % 4;
            b.Quad(clo[k], clo[k1], chi[k1], chi[k]);
        }
        car.glass = b.Make(o);
    }
    {
        // Underbody, bumpers and side skirts.
        Builder b;
        b.Box(core::vec3d(-0.70, -2.30, 0.24), core::vec3d(0.70, 2.30, 0.40));   // narrower than the body: the wheels show
        b.Box(core::vec3d(-0.84, -2.32, 0.40), core::vec3d(0.84, -2.26, 0.62));   // rear bumper
        b.Box(core::vec3d(-0.84, 2.26, 0.40), core::vec3d(0.84, 2.32, 0.62));     // front bumper
        car.dark = b.Make(o);
    }
    {
        Builder b;
        b.Box(core::vec3d(-0.80, 2.20, 0.62), core::vec3d(-0.40, 2.30, 0.80));    // headlights
        b.Box(core::vec3d(0.40, 2.20, 0.62), core::vec3d(0.80, 2.30, 0.80));
        car.lights = b.Make(o);
    }
    {
        Builder b;
        b.CylinderX(o, car.wheelRadius, 0.12, 16);
        car.wheel = b.Make(o);
    }
    car.wheelOffsets[0] = core::vec3d(-0.86, 1.45, car.wheelRadius);   // tyres stand 8 cm proud of the body
    car.wheelOffsets[1] = core::vec3d(0.86, 1.45, car.wheelRadius);
    car.wheelOffsets[2] = core::vec3d(-0.86, -1.45, car.wheelRadius);
    car.wheelOffsets[3] = core::vec3d(0.86, -1.45, car.wheelRadius);
    return car;
}

SkyDome BuildSkyDome()
{
    SkyDome sky;
    // Sphere, 24 x 12 segments, UV v = latitude (0 nadir .. 1 zenith).
    const int segs = 24, rings = 12;
    auto m = std::make_unique<MeshData>();
    const int nv = (segs + 1) * (rings + 1);
    m->num_vertex = nv;
    m->vertex_list = std::make_unique<core::vec3f[]>(size_t(nv));
    m->uv_list = std::make_unique<core::vec2f[]>(size_t(nv));
    for (int r = 0; r <= rings; r++)
    {
        const double lat = -kPi / 2 + kPi * r / rings;
        for (int sgm = 0; sgm <= segs; sgm++)
        {
            const double lon = 2.0 * kPi * sgm / segs;
            const int k = r * (segs + 1) + sgm;
            m->vertex_list[size_t(k)] = core::vec3f(float(cos(lat) * cos(lon)), float(cos(lat) * sin(lon)), float(sin(lat)));
            m->uv_list[size_t(k)] = core::vec2f(float(sgm) / segs, 1.0f - float(r) / rings);
        }
    }
    std::vector<uint32_t> idx;
    for (int r = 0; r < rings; r++)
        for (int sgm = 0; sgm < segs; sgm++)
        {
            const uint32_t a = uint32_t(r * (segs + 1) + sgm), b = a + 1, cc = a + uint32_t(segs + 1), d = cc + 1;
            idx.insert(idx.end(), { a, cc, d, a, d, b });
        }
    m->add_draw_call_list(kGlTriangles, int(idx.size()), nv);
    for (uint32_t k : idx) m->get_last_draw_call_info().add_index(k);
    m->bbox_ws.Reset();
    m->bbox_ws += core::vec3d(-1.0, -1.0, -1.0);
    m->bbox_ws += core::vec3d(1.0, 1.0, 1.0);
    sky.mesh = std::move(m);

    // Gradient rows, top row first (texture v = 1 at the zenith is row 0).
    const uint32_t rows = 64;
    sky.gradientHeight = rows;
    sky.gradient.resize(size_t(rows) * 4);
    const double zenith[3] = { 0.22, 0.42, 0.78 }, horizon[3] = { 0.76, 0.84, 0.92 }, haze[3] = { 0.52, 0.55, 0.56 },
                 ground[3] = { 0.34, 0.36, 0.35 };
    for (uint32_t r = 0; r < rows; r++)
    {
        const double v = 1.0 - (r + 0.5) / rows;   // 1 zenith .. 0 nadir
        double rgb[3];
        for (int ch = 0; ch < 3; ch++)
        {
            if (v >= 0.5)
            {
                const double t = pow((v - 0.5) / 0.5, 0.6);   // pale band hugging the horizon
                rgb[ch] = horizon[ch] + (zenith[ch] - horizon[ch]) * t;
            }
            else
            {
                const double t = (0.5 - v) / 0.5;
                rgb[ch] = haze[ch] + (ground[ch] - haze[ch]) * t;
            }
        }
        for (int ch = 0; ch < 3; ch++) sky.gradient[size_t(r) * 4 + size_t(ch)] = uint8_t(rgb[ch] * 255.0 + 0.5);
        sky.gradient[size_t(r) * 4 + 3] = 255;
    }
    return sky;
}

void CarModelMatrix(const core::vec3d& posRelEye, double heading, double pitch,
                    const core::vec3d& local, double spin, float out[16])
{
    // Car space: +y forward. A heading h (clockwise from north) turns +y onto
    // (sin h, cos h), which is a rotation about z by -h; pitch tilts the nose
    // up about the car's x axis.
    const Mat4 m = Mat4::Translate(posRelEye.x, posRelEye.y, posRelEye.z) * Mat4::RotZ(-heading) * Mat4::RotX(pitch) *
                   Mat4::Translate(local.x, local.y, local.z) * Mat4::RotX(spin);
    std::copy(m.m, m.m + 16, out);
}

// ---------------------------------------------------------------------------
// Canopy cut
// ---------------------------------------------------------------------------

int CanopyCut::Apply(const DriveRoad& road, const DriveRoadParams& p, const std::vector<BatchMeshData*>& batches)
{
    Restore();
    if (!road.ok || road.stations.size() < 2) return 0;
    const size_t n = road.stations.size();
    const double half = (std::max)(p.roadWidth * 0.5 + p.shoulderWidth, p.clearWidth * 0.5);

    PointGrid grid;
    grid.cell = 20.0;
    Box2 corridorBox;
    double lowestRoad = kInf;
    double maxStep = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        const core::vec3d& c = road.stations[i].pos;
        if (i + 1 < n)
        {
            grid.Add(c.x, c.y, int(i));
            maxStep = (std::max)(maxStep, road.stations[i + 1].s - road.stations[i].s);
        }
        corridorBox.Add(c.x - half, c.y - half);
        corridorBox.Add(c.x + half, c.y + half);
        lowestRoad = (std::min)(lowestRoad, c.z);
    }
    // Lateral distance from (x, y) to the centre line and the road height there.
    auto corridor = [&](double x, double y, double& roadZ) {
        double best = kInf;
        const double r = half + maxStep;
        grid.ForRange(x - r, y - r, x + r, y + r, [&](int i) {
            const core::vec3d& a = road.stations[i].pos;
            const core::vec3d& b = road.stations[i + 1].pos;
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double len2 = dx * dx + dy * dy;
            double t = len2 > 0.0 ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0.0;
            t = (std::min)(1.0, (std::max)(0.0, t));
            const double px = a.x + dx * t, py = a.y + dy * t;
            const double d = hypot(x - px, y - py);
            if (d < best)
            {
                best = d;
                roadZ = a.z + (b.z - a.z) * t;
            }
        });
        return best <= half;
    };

    // Black faces: the parts of a tree lump no photo covered are textured
    // black. A corridor triangle is black when its texture is black at its
    // corners, edge midpoints and centre (a face only partly black is the
    // edge of the photographed part, and stays). Textures decode on first
    // use, once each.
    std::unordered_map<const core::Texture2DInfo*, DecodedTexture> decoded;
    const uint32_t kBlack = 20;   // per channel, 0..255: DXT compression leaves black a little grey
    auto blackAt = [&](const DecodedTexture& tex, double u, double v) {
        u -= floor(u); v -= floor(v);
        const int tx = (std::min)(tex.w - 1, (std::max)(0, int(u * tex.w)));
        const int ty = (std::min)(tex.h - 1, (std::max)(0, int(v * tex.h)));
        const uint32_t c = tex.rgb[size_t(ty) * size_t(tex.w) + size_t(tx)];
        return ((c >> 16) & 0xFF) <= kBlack && ((c >> 8) & 0xFF) <= kBlack && (c & 0xFF) <= kBlack;
    };

    int removed = 0;
    m_blackRemoved = 0;
    ForEachMesh(batches, [&](MeshData* mesh, GroupMeshData* group, ObjectClass) {
        if (mesh->bbox_ws.b_valid && !corridorBox.OverlapsBounds(mesh->bbox_ws)) return;
        const bool aboveClear = !mesh->bbox_ws.b_valid || mesh->bbox_ws.bb_max.z >= lowestRoad + p.clearHeight;
        const DecodedTexture* tex = nullptr;
        if (p.cutBlack && mesh->uv_list && mesh->idx_in_texture_list < group->loaded_textures.size())
        {
            const core::Texture2DInfo* info = group->loaded_textures[mesh->idx_in_texture_list];
            auto it = decoded.find(info);
            if (it == decoded.end())
            {
                DecodedTexture d;
                if (!DecodeTexture(info, d)) d.w = 0;
                it = decoded.emplace(info, std::move(d)).first;
            }
            if (it->second.w > 0) tex = &it->second;
        }
        if (!aboveClear && !tex) return;
        std::vector<DrawCallBackup> backups;
        for (size_t d = 0; d < mesh->draw_call_list.size(); d++)
        {
            DrawCallInfo& dc = mesh->draw_call_list[d];
            if (!dc.is_drawable()) continue;
            std::vector<uint32_t> kept;
            bool any = false;
            ForEachTriangle(mesh, dc, [&](uint32_t i0, uint32_t i1, uint32_t i2) {
                const core::vec3d a = WorldVertex(mesh, i0), b = WorldVertex(mesh, i1), c = WorldVertex(mesh, i2);
                const double centreZ = (a.z + b.z + c.z) / 3.0;
                double roadZ = 0.0;
                const bool inCorridor = (aboveClear || tex) &&
                                        corridor((a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, roadZ);
                // Judged by the triangle's centre: the big tree-side triangles
                // that reach down to the ground go too, or they wall the
                // chase camera in. A bank beside the road keeps its lower part.
                if (inCorridor && centreZ > lowestRoad + p.clearHeight && centreZ > roadZ + p.clearHeight)
                {
                    any = true;
                    removed++;
                    return;
                }
                if (inCorridor && tex)
                {
                    const core::vec2f& u0 = mesh->uv_list[i0];
                    const core::vec2f& u1 = mesh->uv_list[i1];
                    const core::vec2f& u2 = mesh->uv_list[i2];
                    const bool black =
                        blackAt(*tex, u0.x, u0.y) && blackAt(*tex, u1.x, u1.y) && blackAt(*tex, u2.x, u2.y) &&
                        blackAt(*tex, 0.5 * (u0.x + u1.x), 0.5 * (u0.y + u1.y)) &&
                        blackAt(*tex, 0.5 * (u1.x + u2.x), 0.5 * (u1.y + u2.y)) &&
                        blackAt(*tex, 0.5 * (u2.x + u0.x), 0.5 * (u2.y + u0.y)) &&
                        blackAt(*tex, (u0.x + u1.x + u2.x) / 3.0, (u0.y + u1.y + u2.y) / 3.0);
                    if (black)
                    {
                        any = true;
                        m_blackRemoved++;
                        return;
                    }
                }
                kept.push_back(i0); kept.push_back(i1); kept.push_back(i2);
            });
            if (!any) continue;
            DrawCallBackup bk;
            bk.drawCall = d;
            bk.primitiveType = uint32_t(dc.get_primitive_type());
            bk.indexType = dc.get_index_type();
            bk.indices.resize(size_t(dc.get_index_count()));
            for (int k = 0; k < dc.get_index_count(); k++) bk.indices[size_t(k)] = dc.get_index(k);
            backups.push_back(std::move(bk));
            dc.set_primitive_type(kGlTriangles);
            dc.clear_index_buffer();
            dc.allocate_index_buffer(int(kept.size()), bk.indexType == kGlUInt ? 0x10000 : 0xffff);
            for (uint32_t k : kept) dc.add_index(k);
        }
        if (!backups.empty())
            m_backups.push_back(Backup{ mesh, std::move(backups) });
    });
    return removed;
}

std::vector<MeshData*> CanopyCut::Restore()
{
    std::vector<MeshData*> touched = Touched();
    for (Backup& b : m_backups)
    {
        for (DrawCallBackup& bk : b.drawCalls)
        {
            if (bk.drawCall >= b.mesh->draw_call_list.size()) continue;
            DrawCallInfo& dc = b.mesh->draw_call_list[bk.drawCall];
            dc.set_primitive_type(PrimitiveType(bk.primitiveType));
            dc.clear_index_buffer();
            dc.allocate_index_buffer(int(bk.indices.size()), bk.indexType == kGlUInt ? 0x10000 : 0xffff);
            for (uint32_t k : bk.indices) dc.add_index(k);
        }
    }
    m_backups.clear();
    return touched;
}

std::vector<MeshData*> CanopyCut::Touched() const
{
    std::vector<MeshData*> out;
    for (const Backup& b : m_backups) out.push_back(b.mesh);
    return out;
}
