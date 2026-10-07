"""Hidden-surface removal: delete triangles no view from above the horizon sees.

Orthographic views from many directions rasterize triangle ids with a depth
test. A triangle survives if some view sees it (or its centroid lies on the
visible surface in some view - small triangles between pixel centres).
Coincident surfaces, the z-fighting case, are settled by priority: triangles
are drawn high priority first, and a later triangle only takes a pixel if it is
closer by more than `eps`, so exactly one of two coincident triangles survives.
Trees are tested but never occlude, so what is under a tree stays for when
trees are hidden in the viewer. Undersides seen only from below are lost.
"""
import math

import numba
import numpy as np

import raster

RES = 0.3          # m per pixel in the views
EPS = 0.25         # m: depth tolerance (coincident / noise)
ELEVATIONS = (90.0, 60.0, 40.0, 22.0, 10.0, 3.0)   # degrees above the horizon; 90 = straight down
AZIMUTHS = 12


@numba.njit(cache=True)
def _raster(scr, order, eps, depth, ids, write, seen):
    """scr: float64[t, 3, 3] (x px, y px, depth m) per vertex; order: triangle ids
    in drawing order. write: update depth/ids; else only mark `seen` where the
    triangle is at or in front of the stored surface."""
    H, W = depth.shape
    for oi in range(len(order)):
        t = order[oi]
        x0, y0, z0 = scr[t, 0, 0], scr[t, 0, 1], scr[t, 0, 2]
        x1, y1, z1 = scr[t, 1, 0], scr[t, 1, 1], scr[t, 1, 2]
        x2, y2, z2 = scr[t, 2, 0], scr[t, 2, 1], scr[t, 2, 2]
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if area == 0.0:
            continue
        xmin = max(0, int(math.floor(min(x0, min(x1, x2)))))
        xmax = min(W - 1, int(math.ceil(max(x0, max(x1, x2)))))
        ymin = max(0, int(math.floor(min(y0, min(y1, y2)))))
        ymax = min(H - 1, int(math.ceil(max(y0, max(y1, y2)))))
        if xmin > xmax or ymin > ymax:
            continue
        inv = 1.0 / area
        for py in range(ymin, ymax + 1):
            cy = py + 0.5
            for px in range(xmin, xmax + 1):
                cx = px + 0.5
                w0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) * inv
                w1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) * inv
                w2 = 1.0 - w0 - w1
                if w0 < 0.0 or w1 < 0.0 or w2 < 0.0:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                if write:
                    if z < depth[py, px] - eps:
                        depth[py, px] = z
                        ids[py, px] = t
                else:
                    if z <= depth[py, px] + eps:
                        seen[t] = True


@numba.njit(cache=True)
def _centroid_test(scr, cand, eps, depth, seen):
    H, W = depth.shape
    for i in range(len(cand)):
        t = cand[i]
        cx = (scr[t, 0, 0] + scr[t, 1, 0] + scr[t, 2, 0]) / 3.0
        cy = (scr[t, 0, 1] + scr[t, 1, 1] + scr[t, 2, 1]) / 3.0
        cz = (scr[t, 0, 2] + scr[t, 1, 2] + scr[t, 2, 2]) / 3.0
        px, py = int(cx), int(cy)
        if 0 <= px < W and 0 <= py < H and cz <= depth[py, px] + eps:
            seen[t] = True


def view_directions():
    dirs = [(0.0, 0.0, -1.0)]
    for el in ELEVATIONS:
        if el >= 90.0:
            continue
        for k in range(AZIMUTHS):
            az = 2 * math.pi * k / AZIMUTHS
            e = math.radians(el)
            dirs.append((math.cos(e) * math.cos(az), math.cos(e) * math.sin(az), -math.sin(e)))   # looking along d
    return np.array(dirs)


def hidden(pos, priority, occluder, res=RES, eps=EPS, log=print):
    """pos: float64[t, 3, 3] world triangles; priority: int (lower drawn first);
    occluder: bool[t] (False = tested, never hides others). Returns bool[t] hidden."""
    T = len(pos)
    seen = np.zeros(T, bool)
    order_all = np.argsort(priority, kind="stable").astype(np.int64)
    occ_order = order_all[occluder[order_all]]
    non_occ = np.flatnonzero(~occluder).astype(np.int64)
    centre = pos.reshape(-1, 3).mean(0)
    for vi, d in enumerate(view_directions()):
        d = d / np.linalg.norm(d)
        up = np.array([0.0, 0.0, 1.0]) if abs(d[2]) < 0.99 else np.array([0.0, 1.0, 0.0])
        right = np.cross(d, up); right /= np.linalg.norm(right)
        vup = np.cross(right, d)
        R = np.stack([right, vup, d])                      # rows: view x, view y, depth along d
        P = (pos - centre) @ R.T
        xy = P[..., :2]
        lo = xy.reshape(-1, 2).min(0)
        scr = np.empty_like(P)
        scr[..., 0] = (xy[..., 0] - lo[0]) / res
        scr[..., 1] = (xy[..., 1] - lo[1]) / res
        scr[..., 2] = P[..., 2]
        W = int(scr[..., 0].max()) + 2
        H = int(scr[..., 1].max()) + 2
        depth = np.full((H, W), np.inf, np.float64)
        ids = np.full((H, W), -1, np.int64)
        _raster(scr, occ_order, eps, depth, ids, True, seen)
        hit = ids[ids >= 0]
        seen[hit] = True
        _raster(scr, non_occ, eps, depth, ids, False, seen)
        # Small triangles that fell between pixel centres but sit on the surface.
        cand = np.flatnonzero(~seen).astype(np.int64)
        _centroid_test(scr, cand, eps, depth, seen)
    return ~seen


def cull_group(group, protected, priority_of_mesh, occluder_of_mesh, log=print):
    """Delete hidden triangles from the group's meshes. protected: set of mesh
    indices never culled; priority_of_mesh / occluder_of_mesh: per mesh index.
    Returns (triangles before, after)."""
    tris = raster.collect(group)
    T = len(tris.obj)
    pri = np.array([priority_of_mesh[m] for m in range(len(group.meshes))], np.int32)[tris.mesh]
    occ = np.array([occluder_of_mesh[m] for m in range(len(group.meshes))], bool)[tris.mesh]
    drop = hidden(tris.pos, pri, occ, log=log)
    prot = np.array([m in protected for m in range(len(group.meshes))], bool)[tris.mesh]
    drop &= ~prot
    # Apply.
    keep_rows = np.flatnonzero(~drop)
    per = {}
    for r in keep_rows:
        per.setdefault((int(tris.mesh[r]), int(tris.dc[r])), []).append(int(tris.local[r]))
    out = []
    for mi, m in enumerate(group.meshes):
        dcs = []
        for di, (prim, idx) in enumerate(m.draw_calls):
            if prim != raster.GL_TRIANGLES:
                dcs.append((prim, idx))
                continue
            loc = per.get((mi, di))
            if not loc:
                continue
            t = idx[: len(idx) // 3 * 3].reshape(-1, 3)
            dcs.append((prim, t[np.array(sorted(loc))].ravel()))
        if dcs:
            m.draw_calls = dcs
            out.append(m)
    group.meshes = out
    return T, int((~drop).sum())
