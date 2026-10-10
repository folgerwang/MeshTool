"""Replace each segmented building with a clean, regularized model.

    python refine.py in.mtscene out.mtscene [--only building_012,building_040]
                     [--no-sam] [--texel 0.1] [--debug-dir dir]

Per building: outline from the height raster refined by SAM2 on the
orthophoto, roof planes by RANSAC + region growing, edges straightened onto
the building's main axes, then a prism model (flat/sloped roof parts, vertical
walls to the ground) textured by projecting the original captured mesh onto
each new face."""
import argparse
import copy
import math
import os
import sys
import time

import cv2
import numpy as np

import building
import glass
import mtscene
import raster
import terrain
import cull
import inpaint

CLS_GROUND, CLS_ROAD, CLS_BUILDING, CLS_CAR, CLS_TREE, CLS_PLANTS, CLS_WATER = 1, 2, 3, 4, 5, 6, 7
PAGE = 2048                     # atlas page size (px)
PAD = 2                         # texels of bleed around each face


def load_sam():
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    import torch
    from sam2.sam2_image_predictor import SAM2ImagePredictor
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    pred = SAM2ImagePredictor.from_pretrained("facebook/sam2.1-hiera-small", device=dev)
    return pred, torch


def ground_height(top, obj_cls, mask, res, base_z=None):
    """Robust ground level around a building: low percentile of ground-class
    pixels in a 2-12 m ring. base_z (where the building's own walls end) wins
    when the ring disagrees by more than 3 m - the ring then sees a plaza,
    podium roof or garden instead of the street the walls stand on."""
    k_in = int(2 / res) * 2 + 1
    k_out = int(12 / res) * 2 + 1
    m = mask.astype(np.uint8)
    ring = cv2.dilate(m, np.ones((k_out, k_out), np.uint8)).astype(bool) & \
           ~cv2.dilate(m, np.ones((k_in, k_in), np.uint8)).astype(bool)
    valid = np.isfinite(top)
    g = ring & valid & np.isin(obj_cls, [CLS_GROUND, CLS_ROAD, CLS_PLANTS, CLS_WATER])
    if g.sum() < 20:
        g = ring & valid
    ring_z = float(np.percentile(top[g], 20)) if g.sum() else None
    if base_z is None:
        return ring_z
    if ring_z is None or abs(ring_z - base_z) > 3.0:
        return base_z
    return ring_z


# ----------------------------------------------------------------------------
# Texture baking
# ----------------------------------------------------------------------------
def project_face(face, src, texs, texel, band):
    """Rasterize the original triangles into a face's frame. Keeps the outermost
    original surface whose distance from the face (along its outward normal)
    lies in band. Returns colour, depth, coverage mask."""
    w = max(1, int(math.ceil(face.size[0] / texel)))
    h = max(1, int(math.ceil(face.size[1] / texel)))
    P = src.pos - face.origin
    scr = np.empty_like(P)
    scr[..., 0] = (P @ face.u) / texel
    scr[..., 1] = h - (P @ face.v) / texel
    scr[..., 2] = P @ face.normal
    depth = np.full((h, w), -np.inf)
    color = np.zeros((h, w, 3), np.uint8)
    ids = np.full((h, w), -1, np.int32)
    raster.rasterize(scr, src.uv, src.tex, np.zeros(len(scr), np.int32), texs, w, h,
                     depth, color, ids, band[0], band[1])
    return color, depth, ids >= 0


ROOF_BAND = (-2.0, 6.0)     # rooftop equipment above a roof plane still paints onto it
WALL_BAND = (-2.5, 2.5)


def bake_face(face, src, texs, texel):
    """Original appearance on a face: uint8[h, w, 3] and the covered share."""
    band = ROOF_BAND if face.kind == "roof" else WALL_BAND
    color, _, covered = project_face(face, src, texs, texel, band)
    return fill_holes(color, covered), covered.mean()


class Surroundings:
    """What stands outside a wall, from the top-down raster."""

    def __init__(self, top, obj_cls, oid, ground_z):
        self.top, self.obj_cls, self.oid, self.ground_z = top, obj_cls, oid, ground_z

    def faces_building(self, f, gap=1.0):
        """Wall looks onto another building at least as tall (a party wall
        nobody photographed)."""
        a = f.origin[:2]
        u, n = f.u[:2], f.normal[:2]
        wall_top = f.origin[2] + f.size[1]
        hits = 0
        k = max(2, int(f.size[0] / 1.0))
        for s in range(k):
            x, y = a + u * (f.size[0] * (s + 0.5) / k) + n * gap
            px, py = self.top.to_px(x, y)
            px, py = int(px), int(py)
            if not (0 <= py < self.top.obj.shape[0] and 0 <= px < self.top.obj.shape[1]):
                continue
            # Hidden only where the neighbour stands as high as the wall.
            if self.obj_cls[py, px] == CLS_BUILDING and self.top.obj[py, px] != self.oid and \
                    self.top.height[py, px] > max(self.ground_z + 3.0, wall_top - 2.0):
                hits += 1
        return hits > k // 2


def _silhouette(pos, R, lo, res, shape):
    """Orthographic coverage mask of triangles pos[t, 3, 3] seen along rotation R."""
    P = pos @ R.T
    scr = np.empty_like(P)
    scr[..., 0] = (P[..., 0] - lo[0]) / res
    scr[..., 1] = (P[..., 1] - lo[1]) / res
    scr[..., 2] = P[..., 2]
    depth = np.full(shape, -np.inf)
    color = np.zeros(shape + (3,), np.uint8)
    ids = np.full(shape, -1, np.int32)
    n = len(pos)
    raster.rasterize(scr, np.zeros((n, 3, 2), np.float32), np.full(n, -1, np.int32), np.zeros(n, np.int32),
                     np.zeros((1, 256, 256, 3), np.uint8), shape[1], shape[0], depth, color, ids, -1e30, 1e30)
    return ids >= 0


def silhouette_miss(faces, own_pos, kept_pos, res=0.5):
    """Worst share, over five views (four oblique sides and straight down), of
    the captured building's silhouette the model does not cover. Catches
    missing walls you could see through, and gross shape errors."""
    model_pos = np.concatenate([f.tris for f in faces] + ([kept_pos] if len(kept_pos) else []))
    worst = 0.0
    views = [(0.0, -90.0)] + [(yaw, -35.0) for yaw in (0, 90, 180, 270)]
    for yaw, pitch in views:
        a, b = math.radians(yaw), math.radians(pitch)
        fwd = np.array([math.sin(a) * math.cos(b), math.cos(a) * math.cos(b), math.sin(b)])
        right = np.cross(fwd, [0, 0, 1.0]) if abs(fwd[2]) < 0.99 else np.array([1.0, 0, 0])
        right /= np.linalg.norm(right)
        up = np.cross(right, fwd)
        R = np.stack([right, up, -fwd])
        P = own_pos.reshape(-1, 3) @ R.T
        lo, hi = P.min(0) - 1.0, P.max(0) + 1.0
        shape = (int((hi[1] - lo[1]) / res) + 1, int((hi[0] - lo[0]) / res) + 1)
        orig = _silhouette(own_pos, R, lo, res, shape)
        # Thin captured details (antennas, railings, fins) are not modelled.
        orig = cv2.morphologyEx(orig.astype(np.uint8), cv2.MORPH_OPEN, np.ones((5, 5), np.uint8)).astype(bool)
        mod = _silhouette(model_pos, R, lo, res, shape)
        mod = cv2.dilate(mod.astype(np.uint8), np.ones((3, 3), np.uint8)).astype(bool)
        if orig.sum():
            worst = max(worst, float((orig & ~mod).sum() / orig.sum()))
    return worst


def blank_walls(faces, src, texs, around):
    """Area (m2) of steps between roof parts and courtyard walls with almost
    nothing captured to project onto them (would show as a flat fill colour,
    e.g. a wall where the capture has an open canopy)."""
    blank = 0.0
    for f in faces:
        if f.kind != "wall" or (f.exterior and f.edge >= 0) or around.faces_building(f):
            continue                       # outer walls: wall_fidelity covers them
        area = f.size[0] * f.size[1]
        if area < 15.0:
            continue
        _, _, cov = project_face(f, src, texs, 0.5, WALL_BAND)
        if cov.mean() < 0.3:
            blank += area * (1.0 - cov.mean())
    return blank


def wall_fidelity(model, src, texs, around):
    """Share of street-facing wall area the captured facade lies on (within 1.5 m)."""
    good = total = 0.0
    for f in building.model_faces(model):
        if f.kind != "wall" or not f.exterior or around.faces_building(f):
            continue
        area = f.size[0] * f.size[1]
        if area < 4.0:
            continue
        _, _, cov = project_face(f, src, texs, 0.5, (-1.5, 1.5))
        good += cov.mean() * area
        total += area
    return good / total if total else 1.0


def fill_holes(color, covered, fallback=(128, 128, 128)):
    """Small holes: inpaint. Large ones (walls nobody captured, e.g. against
    a neighbour): the face's mean colour, blended in, instead of long smears."""
    if covered.all():
        return color
    if not covered.any():
        color[:] = fallback
        return color
    holes = (~covered).astype(np.uint8)
    dist = cv2.distanceTransform(holes, cv2.DIST_L2, 3)
    out = cv2.inpaint(color, holes, 3, cv2.INPAINT_TELEA)
    if dist.max() > 6:
        mean = color[covered].mean(0)
        w = np.clip((dist - 3) / 6, 0, 1)[..., None]
        out = (out * (1 - w) + mean * w).astype(np.uint8)
    return out


class Atlas:
    """Shelf packer over PAGE x PAGE pages."""

    def __init__(self):
        self.pages = []           # images
        self.cursor = []          # (x, y, shelf_h)
        self.used = []            # (max x, max y) touched per page

    def add(self, img):
        h, w = img.shape[:2]
        H, W = h + 2 * PAD, w + 2 * PAD
        for pi, (x, y, sh) in enumerate(self.cursor):
            if x + W > PAGE:
                x, y, sh = 0, y + sh, 0
            if y + H <= PAGE:
                self._blit(pi, x, y, img)
                self.cursor[pi] = (x + W, y, max(sh, H))
                return pi, x + PAD, y + PAD
        self.pages.append(np.full((PAGE, PAGE, 3), 128, np.uint8))
        self.cursor.append((0, 0, 0))
        pi = len(self.pages) - 1
        self._blit(pi, 0, 0, img)
        self.cursor[pi] = (W, 0, H)
        return pi, PAD, PAD

    def _blit(self, pi, x, y, img):
        padded = cv2.copyMakeBorder(img, PAD, PAD, PAD, PAD, cv2.BORDER_REPLICATE)
        self.pages[pi][y:y + padded.shape[0], x:x + padded.shape[1]] = padded
        while len(self.used) <= pi:
            self.used.append((0, 0))
        ux, uy = self.used[pi]
        self.used[pi] = (max(ux, x + padded.shape[1]), max(uy, y + padded.shape[0]))

    def page_size(self, pi):
        """Used part of a page, rounded up to whole DXT blocks."""
        ux, uy = self.used[pi]
        return (ux + 3) // 4 * 4, (uy + 3) // 4 * 4


def wall_cells(f):
    """Split a wall quad into ~3 m cells following its (possibly sloped) top.
    Returns (u edges, v edges, [(cell index, triangles float[2, 3, 3])])."""
    A0 = f.tris[0][0]
    vA = (f.tris[1][2] - A0) @ f.v
    vB = (f.tris[0][2] - A0) @ f.v
    L = f.size[0]
    ue, ve = glass.cell_grid(L, max(vA, vB))
    top = lambda s: vA + (vB - vA) * s / L
    pt = lambda s, v: A0 + f.u * s + f.v * v
    cells = []
    nu = len(ue) - 1
    for j in range(len(ve) - 1):
        for i in range(nu):
            s0, s1, v0, v1 = ue[i], ue[i + 1], ve[j], ve[j + 1]
            t0, t1 = min(v1, top(s0)), min(v1, top(s1))
            if max(t0, t1) <= v0 + 1e-3:
                continue
            t0, t1 = max(t0, v0), max(t1, v0)
            a, b, c, d = pt(s0, v0), pt(s1, v0), pt(s1, t1), pt(s0, t0)
            cells.append((j * nu + i, np.array([[a, b, c], [a, c, d]])))
    return ue, ve, cells


def model_meshes(model, faces, src, texs, texel, object_id, tex_base, classifier=None):
    """Bake faces into atlas pages; per page one captured mesh and, where CLIP
    finds curtain-wall glass on the walls, one glass mesh."""
    atlas = Atlas()
    per_page = {}                 # (page, material) -> [(points, texel coords)]
    coverage = []
    pending = []                  # walls waiting for the glass classifier
    crops = []
    for f in faces:
        t = texel
        while max(f.size) / t > PAGE - 2 * PAD - 2:
            t *= 1.5
        img, cov = bake_face(f, src, texs, t)
        coverage.append(cov)
        pi, x0, y0 = atlas.add(img)
        h = img.shape[0]
        to_tex = lambda P, f=f, t=t, x0=x0, y0=y0, h=h: np.stack(
            [((P - f.origin) @ f.u) / t + x0, h - ((P - f.origin) @ f.v) / t + y0], 1)
        if classifier is not None and f.kind == "wall" and f.size[1] >= 6.0 and f.size[0] >= 3.0:
            ue, ve, cells = wall_cells(f)
            first = len(crops)
            crops += glass.context_crops(img, t, ue, ve)
            pending.append((pi, to_tex, ue, ve, cells, first, f.normal))
            continue
        P = f.tris.reshape(-1, 3)
        per_page.setdefault((pi, mtscene.MAT_CAPTURED), []).append((P, to_tex(P)))

    glass_area = 0.0
    if pending:
        p = classifier.p_glass(crops)
        for pi, to_tex, ue, ve, cells, first, normal in pending:
            nu, nv = len(ue) - 1, len(ve) - 1
            decide = glass.smooth(p[first:first + nu * nv] > glass.THRESHOLD, nu, nv)
            if os.environ.get("GLASS_DEBUG"):
                pc = p[first:first + nu * nv]
                print(f"    wall {ue[-1]:5.1f} x {ve[-1]:5.1f} m: p mean {pc.mean():.2f} median {np.median(pc):.2f} "
                      f">thr {np.mean(pc > glass.THRESHOLD):.2f} smoothed {decide.mean():.2f}", flush=True)
            if decide.mean() < glass.FACADE_SHARE:
                decide[:] = False          # glass is a facade system, not a stray cell
            for ci, tri in cells:
                mat = mtscene.MAT_GLASS if decide[ci] else mtscene.MAT_CAPTURED
                P = tri.reshape(-1, 3)
                per_page.setdefault((pi, mat), []).append((P, to_tex(P)))
                if decide[ci]:
                    glass_area += (ue[1] - ue[0]) * (ve[1] - ve[0])
                    # Dark interior just behind the glass.
                    Q = P - normal * glass.INTERIOR_DEPTH
                    per_page.setdefault((pi, mtscene.MAT_INTERIOR), []).append((Q, to_tex(P)))

    center = np.array([model.footprint.centroid.x, model.footprint.centroid.y, model.ground_z])
    meshes, textures, page_tex = [], [], {}
    for (pi, mat), items in sorted(per_page.items()):
        pw, ph = atlas.page_size(pi)
        if pi not in page_tex:
            page_tex[pi] = tex_base + len(textures)
            textures.append(mtscene.Texture(
                raster.GL_COMPRESSED_RGB_S3TC_DXT1, raster.GL_COMPRESSED_RGB_S3TC_DXT1, 0,
                [(pw, ph, raster.encode_dxt1(np.ascontiguousarray(atlas.pages[pi][:ph, :pw])))]))
        P = np.concatenate([x[0] for x in items])
        uv = (np.concatenate([x[1] for x in items]) / [pw, ph]).astype(np.float32)
        verts = (P - center).astype(np.float32)
        idx = np.arange(len(P), dtype=np.uint32)
        meshes.append(mtscene.Mesh(page_tex[pi], object_id, center, verts, uv, None,
                                   [(raster.GL_TRIANGLES, idx)], mat))
    pages = [atlas.pages[i][:atlas.page_size(i)[1], :atlas.page_size(i)[0]] for i in range(len(atlas.pages))]
    return meshes, textures, float(np.mean(coverage)) if coverage else 0.0, pages, glass_area


def inside_model(model, pos, tol=1.0):
    """Triangles (float[t, 3, 3]) whose centre lies in a refined model's volume:
    footprint grown by tol, between the ground and just above the roof."""
    import shapely
    cen = pos.mean(1)
    zone = model.footprint.buffer(tol, join_style="mitre")
    hit = (cen[:, 2] > model.ground_z - 1.0) & (cen[:, 2] < model.top_z() + 1.5)
    idx = np.flatnonzero(hit)
    if len(idx):
        hit[idx] = shapely.contains_xy(zone, cen[idx, 0], cen[idx, 1])
    return hit


def absorb_fragments(tris, buildings, models, share=0.8):
    """Unrefined building objects that lie (almost) entirely inside or against a
    refined model - facade slivers the segmenter split off. Returns
    {object id: indices of its triangles inside a model} for those; the rest
    of their triangles stay as captured."""
    if not models:
        return {}
    out = {}
    for oid in buildings:
        if oid in models:
            continue
        rows = np.flatnonzero(tris.obj == oid)
        if not len(rows):
            continue
        inside = np.zeros(len(rows), bool)
        for m in models.values():
            inside |= inside_model(m, tris.pos[rows], tol=1.5)
        if inside.mean() >= share:
            out[oid] = rows[inside]
    return out


def drop_triangles(group, tris, drop):
    """The group's meshes without the triangles flagged in drop (rows of tris);
    meshes left empty disappear."""
    hit = {}
    for row in np.flatnonzero(drop):
        hit.setdefault(int(tris.mesh[row]), []).append((int(tris.dc[row]), int(tris.local[row])))
    out = []
    for mi, m in enumerate(group.meshes):
        if mi not in hit:
            out.append(m)
            continue
        gone = {}
        for di, ti in hit[mi]:
            gone.setdefault(di, set()).add(ti)
        dcs = []
        for di, (prim, idx) in enumerate(m.draw_calls):
            if di in gone:
                t = idx[: len(idx) // 3 * 3].reshape(-1, 3)
                keep = np.ones(len(t), bool)
                keep[list(gone[di])] = False
                idx = t[keep].ravel()
            if len(idx):
                dcs.append((prim, idx))
        if dcs:
            m.draw_calls = dcs
            out.append(m)
    return out


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--wall-finish", choices=("painted-concrete","metal"), default="painted-concrete", help="opaque panel PBR finish; color is estimated from the original capture")
    ap.add_argument("--tiled-facade", action="store_true", help="replace walls with textured facade tiles and modeled recessed windows")
    ap.add_argument("--reflective-glass", action="store_true", help="explicitly assign reflective glass to refined wall triangles")
    ap.add_argument("--pcg", action="store_true", help="add procedural architectural details and PBR finishes")
    ap.add_argument("--target", help="exact selected batch:group:object indices")
    ap.add_argument("--only", help="comma-separated building names")
    ap.add_argument("--no-sam", action="store_true", help="outline from height only")
    ap.add_argument("--no-glass", action="store_true", help="do not detect glass facades")
    ap.add_argument("--texel", type=float, default=0.08, help="baked texture resolution (m per texel)")
    ap.add_argument("--clean", action="store_true",
                    help="clean scene: remove cars and surface clutter, rebuild ground/road/plants/water as a "
                         "filled height field (holes under trees, cars and buildings filled from their "
                         "surroundings), set refined walls down onto it; trees are kept")
    ap.add_argument("--terrain-step", type=float, default=0.5, help="terrain grid step (m) with --clean")
    ap.add_argument("--no-lama", action="store_true",
                    help="with --clean: geometric hole fill instead of LaMa inpainting")
    ap.add_argument("--cull", action="store_true",
                    help="remove hidden surfaces: triangles no view from above the horizon sees (terrain under "
                         "buildings, walls against neighbours, duplicate tiles); on with --clean")
    ap.add_argument("--debug-dir", help="write per-building debug images here")
    ap.add_argument("--progress", action="store_true",
                    help="print '@progress <done> <total> <message>' lines (for MeshTool)")
    args = ap.parse_args()
    if args.tiled_facade and (args.clean or args.cull):
        ap.error("tiled facade replacement cannot be combined with scene cleanup")
    target = None
    if args.target:
        try:
            target = tuple(map(int, args.target.split(":")))
            if len(target) != 3 or min(target) < 0: raise ValueError()
        except ValueError:
            ap.error("--target must be nonnegative batch:group:object indices")
        if args.clean or args.cull:
            ap.error("scene cleanup cannot be combined with selected-building refinement")
    elif args.only and (args.clean or args.cull):
        ap.error("scene cleanup is available only when refining all buildings")

    def progress(done, total, msg):
        if args.progress:
            print(f"@progress {done} {total} {msg}", flush=True)

    t0 = time.time()
    progress(0, 1, "Loading scene...")
    batches = mtscene.load(args.input)
    if target is not None:
        try:
            obj = batches[target[0]].groups[target[1]].objects[target[2]]
            if obj.cls != CLS_BUILDING: raise IndexError()
        except IndexError:
            ap.error("selected target is missing or is not a segmented building")
    if args.tiled_facade:
        import facade
        done,total=facade.apply(batches,target,set(args.only.split(",")) if args.only else None,progress,args.wall_finish)
        progress(98,100,"Saving tiled facades...")
        mtscene.save(args.output,batches)
        progress(100,100,"Tiled facade replacement complete")
        print(f"Refined {done} of {total} buildings with tiled window facades in {time.time()-t0:.0f} s; saved {args.output}",flush=True)
        return 0
    progress(0, 1, "Loading SAM2 and CLIP...")
    params = building.Params()
    predictor = torch = None
    if not args.no_sam:
        try:
            predictor, torch = load_sam()
        except Exception as e:  # noqa: BLE001
            print(f"SAM2 unavailable ({e}); outlines from height only", flush=True)
    inpainter = None
    if args.clean and not args.no_lama:
        try:
            inpainter = inpaint.LamaInpainter(log=lambda s: print(s, flush=True))
        except Exception as e:  # noqa: BLE001
            print(f"LaMa unavailable ({e}); geometric hole fill", flush=True)
    classifier = None
    if not args.no_glass:
        try:
            classifier = glass.GlassClassifier()
        except Exception as e:  # noqa: BLE001
            print(f"CLIP unavailable ({e}); no glass detection", flush=True)
    only = set(args.only.split(",")) if args.only else None
    if args.debug_dir:
        os.makedirs(args.debug_dir, exist_ok=True)

    total = done = 0
    for batch_index, batch in enumerate(batches):
        if target is not None and batch_index != target[0]: continue
        if batch.is_spline:
            continue
        for group_index, group in enumerate(batch.groups):
            if target is not None and group_index != target[1]: continue
            buildings = [i for i, o in enumerate(group.objects) if o.cls == CLS_BUILDING
                         and (only is None or o.name in only)
                         and (target is None or i == target[2])]
            if not buildings and not args.clean:
                continue
            # Keep archival originals outside every fitting/culling pass. They
            # share the scene's textures and are written back after refinement.
            originals = [m for m in group.meshes if m.model_variant == 1]
            group.meshes = [m for m in group.meshes if m.model_variant != 1]
            archived_objects = {m.object_id for m in originals}
            # Exclude previous procedural attachments from fitting and replace
            # them only when that building's new fit succeeds. This prevents
            # stacked details on repeated Apply while preserving rejected fits.
            previous_details = []
            if args.pcg:
                previous_details = [m for m in group.meshes if m.object_id in buildings and m.material in (4,5,6)]
                group.meshes = [m for m in group.meshes if not (m.object_id in buildings and m.material in (4,5,6))]
            local_bounds = None
            selected_area = len(buildings) == 1 and (target is not None or only is not None)
            if selected_area:
                own_meshes = [m for m in group.meshes if m.object_id == buildings[0] and len(m.vertices)]
                if not own_meshes:
                    raise ValueError("Selected building has no mesh geometry")
                lo = np.min([m.vertices[:,:2].min(0)+m.translation[:2] for m in own_meshes],axis=0)
                hi = np.max([m.vertices[:,:2].max(0)+m.translation[:2] for m in own_meshes],axis=0)
                local_bounds = np.r_[lo-20.0,hi+20.0]  # ground ring, occluders, texture context
            progress(1,100,"Collecting nearby triangles..." if selected_area else "Collecting scene triangles...")
            tris = raster.collect(group, bounds=local_bounds)
            texture_group = group
            if selected_area:
                from types import SimpleNamespace
                used = np.unique(tris.tex[tris.tex >= 0])
                remap = np.full(len(group.textures),-1,np.int32)
                remap[used] = np.arange(len(used),dtype=np.int32)
                valid = tris.tex >= 0
                tris.tex[valid] = remap[tris.tex[valid]]
                texture_group = SimpleNamespace(textures=[group.textures[i] for i in used])
                print(f"Selected area: {local_bounds[2]-local_bounds[0]:.1f} x {local_bounds[3]-local_bounds[1]:.1f} m, "
                      f"{len(tris.obj):,} nearby triangles, {len(used)} of {len(group.textures)} textures",flush=True)
            progress(3,100,f"Decoding {len(texture_group.textures)} nearby textures..." if selected_area else "Decoding scene textures...")
            texs = raster.decode_textures(texture_group,progress=lambda n,total: progress(3+int(7*n/max(1,total)),100,f"Decoding textures {n+1}/{total}..."))
            if args.clean:
                terrain.relabel_vehicles(tris, group.objects, log=lambda s: print(s, flush=True))
                buildings = [i for i, o in enumerate(group.objects) if o.cls == CLS_BUILDING
                             and (only is None or o.name in only)
                         and (target is None or i == target[2])]
            # Google Earth draws a dark overlay pass over the ground tiles: same
            # triangles, near-black texture. Drawn last they lose every depth tie
            # to their textured twins; where they are the only geometry their
            # colour is not used.
            progress(10,100,f"Checking textures on {len(tris.obj):,} triangles...")
            ph_tex = raster.placeholder_textures(texs)
            demote = (tris.tex >= 0) & ph_tex[np.clip(tris.tex, 0, len(ph_tex) - 1)]   # lose depth ties
            if demote.any():
                order = np.argsort(demote, kind="stable")
                tris = raster.SceneTris(tris.pos[order], tris.uv[order], tris.tex[order], tris.obj[order],
                                        tris.mesh[order], tris.dc[order], tris.local[order])
                demote = demote[order]
            # Triangles that both use such a texture and sample its pattern: not
            # used to texture anything.
            dark_tri = demote & (raster.triangle_texture_stats(tris, texs)[:, 3] >= 0.2)
            print(f"  {int(demote.sum())} of {len(demote)} triangles ({demote.mean():.0%}) use a placeholder texture "
                  f"({int(ph_tex.sum())} textures); {int(dark_tri.sum())} show its pattern", flush=True)
            top = raster.top_down(tris, texs, params.res, depth_bias=np.where(demote, -0.05, 0.0) if demote.any() else None,
                                  bounds=local_bounds, progress=lambda n,total,w,h: progress(15+int(20*n/max(1,total)),100,
                                  f"Rendering {'selected area' if selected_area else 'scene'} {w}x{h}: {n:,}/{total:,} triangles"))
            # Colour not to trust: pixels won by a pattern-showing triangle, or by
            # any placeholder-texture triangle where the colour itself looks like one.
            won = np.maximum(top.tri, 0)
            dark_px = (top.tri >= 0) & (dark_tri[won] | (demote[won] & raster.placeholder_pixels(top.color)))
            cls_of = np.array([o.cls for o in group.objects] + [0], np.int32)
            obj_cls = cls_of[top.obj]                     # -1 -> last entry (0)
            tri_cls = cls_of[tris.obj]
            print(f"scene: {len(tris.obj)} triangles, {len(buildings)} buildings, "
                  f"raster {top.color.shape[1]}x{top.color.shape[0]} ({time.time() - t0:.0f} s)", flush=True)
            ter = None
            if args.clean:
                progress(0, max(1, len(buildings)), "Rebuilding the terrain...")
                ter = terrain.analyse(top, obj_cls, params.res, color_holes=dark_px, log=lambda s: print(s, flush=True))
                if args.debug_dir:
                    cv2.imwrite(os.path.join(args.debug_dir, "terrain_holes.png"), ter.holes.astype(np.uint8) * 255)

            replaced, models = {}, {}
            drop = np.zeros(len(tris.obj), bool)          # captured triangles a model replaces
            visible = np.bincount(top.obj[top.obj >= 0].ravel(), minlength=len(group.objects)) * params.res ** 2
            fragment_ids = np.array([i for i, o in enumerate(group.objects)
                                     if o.cls == CLS_BUILDING and visible[i] < 20.0], np.int32)
            new_textures = []
            margin = int(10 / params.res)
            for bi, oid in enumerate(buildings):
                name = group.objects[oid].name
                total += 1
                progress(35+int(60*bi/max(1,len(buildings))),100,f"Refining {name} ({bi + 1} of {len(buildings)})")
                full = top.obj == oid
                if not full.any():
                    print(f"  {name}: not visible from above, kept", flush=True)
                    continue
                ys, xs = np.nonzero(full)
                y0, y1 = max(0, ys.min() - margin), min(full.shape[0], ys.max() + margin + 1)
                x0, x1 = max(0, xs.min() - margin), min(full.shape[1], xs.max() + margin + 1)
                sl = (slice(y0, y1), slice(x0, x1))
                sel = tris.obj == oid
                base_z = float(np.percentile(tris.pos[sel][..., 2], 1))
                gz = ground_height(top.height[sl], obj_cls[sl], full[sl], params.res, base_z)
                if gz is None:
                    print(f"  {name}: no ground around it, kept", flush=True)
                    continue
                # Buildings at the capture border were never seen from all sides.
                near = cv2.dilate(full[sl].astype(np.uint8), np.ones((9, 9), np.uint8)).astype(bool)
                edge = near.copy(); edge[1:-1, 1:-1] = False
                touches_edge = (y0 == 0 and near[0].any()) or (x0 == 0 and near[:, 0].any()) or                                (y1 == full.shape[0] and near[-1].any()) or (x1 == full.shape[1] and near[:, -1].any())
                if touches_edge or (~np.isfinite(top.height[sl]) & near).sum() > 0.02 * near.sum():
                    print(f"  {name}: at the edge of the capture, kept", flush=True)
                    continue
                ox, oy = top.to_world(x0, y0)
                # The building's own triangles plus facade slivers the segmenter
                # split off (building objects barely visible from above).
                fsel = sel | np.isin(tris.obj, fragment_ids)
                fpos = tris.pos[fsel]
                fpos = fpos[np.all((fpos[..., 0] >= ox) & (fpos[..., 0] <= ox + (x1 - x0) * params.res) &
                                   (fpos[..., 1] <= oy) & (fpos[..., 1] >= oy - (y1 - y0) * params.res), 1)]
                tris_px = np.stack([(fpos[..., 0] - ox) / params.res, (oy - fpos[..., 1]) / params.res, fpos[..., 2]], -1)
                crop = dict(tris_px=tris_px, height=top.height[sl], rgb=np.ascontiguousarray(top.color[sl]), mask=full[sl],
                            other_building=(obj_cls[sl] == CLS_BUILDING) & ~full[sl], origin=(ox, oy))
                try:
                    if torch is not None:
                        with torch.inference_mode(), torch.autocast("cuda", dtype=torch.bfloat16):
                            model = building.build_model(crop, gz, params, predictor)
                    else:
                        model = building.build_model(crop, gz, params, None)
                except Exception as e:  # noqa: BLE001
                    print(f"  {name}: failed ({e!r}), kept", flush=True)
                    continue
                if model is None:
                    print(f"  {name}: too small or no clear outline, kept", flush=True)
                    continue
                if not model.acceptable(params):
                    print(f"  {name}: model does not fit the capture (outline iou {model.outline_iou:.2f}, "
                          f"height fit {model.height_fit:.0%}), kept", flush=True)
                    continue
                lo = np.array([*model.footprint.bounds[:2], -np.inf]) - [5, 5, 0]
                hi = np.array([*model.footprint.bounds[2:], np.inf]) + [5, 5, 0]
                cen = tris.pos.mean(1)
                near_sel = np.all((cen >= lo) & (cen <= hi), 1) & ~np.isin(tri_cls, [CLS_CAR, CLS_TREE, CLS_PLANTS]) & ~dark_tri
                src = raster.SceneTris(tris.pos[near_sel], tris.uv[near_sel], tris.tex[near_sel],
                                       tris.obj[near_sel], tris.mesh[near_sel])
                def surface(x, y, top=top):
                    px, py = top.to_px(x, y)
                    px, py = int(px), int(py)
                    if 0 <= py < top.height.shape[0] and 0 <= px < top.height.shape[1] and np.isfinite(top.height[py, px]):
                        return float(top.height[py, px])
                    return None
                model.surface = surface
                around = Surroundings(top, obj_cls, oid, gz)
                fid = wall_fidelity(model, src, texs, around)
                if fid < params.min_wall_fidelity:
                    print(f"  {name}: walls do not match the captured facades ({fid:.0%}), kept", flush=True)
                    continue
                if ter is not None:
                    # Walls down onto the rebuilt terrain: no crack at the base.
                    omin = ter.outline_min(model.footprint)
                    if omin is not None:
                        model.ground_z = min(model.ground_z, omin - 0.15)
                faces = building.model_faces(model)
                own_rows = np.flatnonzero(sel)
                own_inside = inside_model(model, tris.pos[own_rows])
                # Captured triangles outside the model (pieces of neighbours the
                # segmenter gave this object) stay as captured.
                miss = silhouette_miss(faces, tris.pos[own_rows], tris.pos[own_rows[~own_inside]])
                blank = blank_walls(faces, src, texs, around)
                if os.environ.get("CHECK_DEBUG"):
                    print(f"    check {name}: silhouette miss {miss:.3f}, blank walls {blank:.0f} m2", flush=True)
                if miss > params.max_silhouette_miss:
                    print(f"  {name}: model leaves gaps in the captured silhouette ({miss:.0%}), kept", flush=True)
                    continue
                if blank > params.max_blank_wall_m2:
                    print(f"  {name}: {blank:.0f} m2 of walls with nothing captured on them, kept", flush=True)
                    continue
                meshes, textures, cov, pages, glass_m2 = model_meshes(
                    model, faces, src, texs, args.texel, oid, len(group.textures) + len(new_textures), classifier)
                if args.pcg:
                    import pcg
                    for m in meshes:
                        if m.material == mtscene.MAT_CAPTURED: m.material = pcg.MAT_FACADE
                    details, n_parts = pcg.detail_meshes(faces, oid, model)
                    meshes += details
                    print(f"  PCG: {n_parts} architectural parts in {len(details)} material meshes", flush=True)
                new_textures += textures
                replaced[oid] = meshes
                models[oid] = model
                drop[own_rows[own_inside]] = True
                done += 1
                ntri = sum(len(m.vertices) // 3 for m in meshes)
                print(f"  [{bi + 1}/{len(buildings)}] {name}: {len(model.parts)} roof parts, {ntri} triangles "
                      f"(was {int(sel.sum())}), height {model.top_z() - gz:.0f} m, "
                      f"fit {model.outline_iou:.2f}/{model.height_fit:.0%}/walls {fid:.0%}, sam iou {model.sam_iou:.2f}, "
                      f"texture coverage {cov:.0%}, glass {glass_m2:.0f} m2",
                      flush=True)
                if args.debug_dir:
                    dbg = crop["rgb"][..., ::-1].copy()
                    to_px = lambda c: np.stack([(c[:, 0] - ox) / params.res, (oy - c[:, 1]) / params.res], 1)
                    for part in model.parts:
                        pts = to_px(np.asarray(part.polygon.exterior.coords)).round().astype(np.int32)
                        cv2.polylines(dbg, [pts], True, (0, 255, 255), 1)
                    fp = to_px(np.asarray(building._polys(model.footprint)[0].exterior.coords))
                    cv2.polylines(dbg, [fp.round().astype(np.int32)], True, (0, 0, 255), 1)
                    cv2.imwrite(os.path.join(args.debug_dir, f"{name}_plan.png"),
                                cv2.resize(dbg, None, fx=3, fy=3, interpolation=cv2.INTER_NEAREST))
                    cv2.imwrite(os.path.join(args.debug_dir, f"{name}_atlas0.png"), pages[0][..., ::-1])

            # PCG can improve a captured shell even when rebuilding it is unsafe.
            # Existing comparison pairs stay untouched on rejected reapplications.
            fallback_objects = set()
            if args.pcg:
                import pcg
                for oid in buildings:
                    if oid in replaced: continue
                    if oid in archived_objects:
                        # Regenerate fallback attachments from an unchanged captured
                        # shell, but retain an already reconstructed model on failure.
                        old=[m for m in originals if m.object_id==oid]
                        current=[m for m in group.meshes if m.object_id==oid]
                        same=len(old)==len(current) and all(
                            np.array_equal(a.vertices,b.vertices) and np.array_equal(a.translation,b.translation)
                            and len(a.draw_calls)==len(b.draw_calls)
                            and all(pa==pb and np.array_equal(ia,ib) for (pa,ia),(pb,ib) in zip(a.draw_calls,b.draw_calls))
                            for a,b in zip(old,current))
                        if not same: continue
                    own = tris.pos[tris.obj == oid]
                    if not len(own) or not np.isfinite(own).all(): continue
                    details, n_parts = pcg.captured_details(own, oid)
                    sources = [m for m in group.meshes if m.object_id == oid]
                    if not details and not any(m.material == mtscene.MAT_CAPTURED for m in sources): continue
                    replaced[oid] = details  # keep all captured triangles and textures
                    fallback_objects.add(oid)
                    done += 1
                    print(f"  PCG fallback {group.objects[oid].name}: preserved captured geometry; "
                          f"PBR finishes, {n_parts} surface-following parts in {len(details)} meshes", flush=True)

            absorbed = absorb_fragments(tris, buildings, models)
            for rows in absorbed.values():
                drop[rows] = True
            if absorbed:
                print(f"  absorbed {len(absorbed)} facade fragments into refined buildings", flush=True)
            if ter is not None:
                # Captured triangles with a placeholder texture lying on the rebuilt
                # terrain (ground clutter, mislabelled pieces): the terrain shows instead.
                low = dark_tri & ~np.isin(tri_cls, list(terrain.REMOVED))
                if low.any():
                    cen = tris.pos[low].mean(1)
                    above = cen[:, 2] - ter.height_at(cen[:, 0], cen[:, 1])
                    idx = np.flatnonzero(low)[above < 1.5]
                    drop[idx] = True
                    print(f"  {len(idx)} placeholder-textured triangles at ground level dropped", flush=True)
            # Snapshot only successfully refined buildings, before their source
            # draw calls are modified. Repeated refinement keeps the first original.
            for oid in replaced:
                if oid not in archived_objects:
                    for source in group.meshes + previous_details:
                        if source.object_id != oid: continue
                        original = copy.copy(source)
                        original.draw_calls = [(prim, idx.copy()) for prim, idx in source.draw_calls]
                        original.model_variant = 1
                        originals.append(original)
            if replaced or drop.any():
                group.meshes = drop_triangles(group, tris, drop) + [m for ms in replaced.values() for m in ms]
                group.textures += new_textures
            group.meshes += [m for m in previous_details if m.object_id not in replaced]
            if ter is not None:
                progress(len(buildings), max(1, len(buildings)), "Meshing the terrain...")
                color = terrain.fill_color(top, ter, inpainter, log=lambda s: print(s, flush=True))
                if args.debug_dir:
                    cv2.imwrite(os.path.join(args.debug_dir, "terrain_color.png"), color[..., ::-1])
                step_px = max(1, int(round(args.terrain_step / params.res)))
                while terrain.PAGE % step_px:
                    step_px -= 1
                terrain.clean_group(group, ter, color, step_px, log=lambda s: print(s, flush=True))
            if args.cull or args.clean:
                progress(len(buildings), max(1, len(buildings)), "Removing hidden surfaces...")
                # Refined models and the terrain win coincident pixels over captured
                # meshes; glass interiors are never culled; trees never occlude.
                model_ids = {id(m) for ms in replaced.values() for m in ms}
                n_obj = len(group.objects)
                cls_of = lambda m: group.objects[m.object_id].cls if 0 <= m.object_id < n_obj else 0
                pri, occ, prot = [], [], set()
                # Meshes mostly made of placeholder-textured triangles lose coincident pixels.
                cur = raster.collect(group)
                ph_all = raster.placeholder_textures(texs if len(texs) >= len(group.textures) else raster.decode_textures(group))
                ph = (cur.tex >= 0) & ph_all[np.clip(cur.tex, 0, len(ph_all) - 1)]
                nm = len(group.meshes)
                dark_share = np.bincount(cur.mesh[ph], minlength=nm) / np.maximum(np.bincount(cur.mesh, minlength=nm), 1)
                for mi, m in enumerate(group.meshes):
                    is_model = id(m) in model_ids or (args.clean and cls_of(m) in terrain.TERRAIN)
                    dark = dark_share[mi] > 0.5 and not is_model
                    pri.append(0 if is_model else (2 if dark else 1))
                    occ.append(cls_of(m) != CLS_TREE)
                    if m.material == mtscene.MAT_INTERIOR:
                        prot.add(mi)
                before, after = cull.cull_group(group, prot, pri, occ, log=lambda s: print(s, flush=True))
                print(f"  hidden surfaces: {before - after} of {before} triangles removed ({(before - after) / max(before, 1):.1%})",
                      flush=True)

            for mesh in group.meshes:
                if mesh.object_id in replaced: mesh.model_variant = 2
                if mesh.object_id in fallback_objects and mesh.material == mtscene.MAT_CAPTURED:
                    mesh.material = pcg.MAT_FACADE
            if args.reflective_glass:
                import pcg
                group.meshes = pcg.reflective_facades(group.meshes, set(buildings))
            group.meshes += originals

    progress(98, 100, "Saving the refined scene...")
    mtscene.save(args.output, batches)
    progress(100, 100, "Refinement complete")
    print(f"Refined {done} of {total} buildings{' and rebuilt the terrain' if args.clean else ''} in "
          f"{time.time() - t0:.0f} s; saved {args.output}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
