"""Replace each segmented building with a clean, regularized model.

    python refine.py in.mtscene out.mtscene [--only building_012,building_040]
                     [--no-sam] [--texel 0.1] [--debug-dir dir]

Per building: outline from the height raster refined by SAM2 on the
orthophoto, roof planes by RANSAC + region growing, edges straightened onto
the building's main axes, then a prism model (flat/sloped roof parts, vertical
walls to the ground) textured by projecting the original captured mesh onto
each new face."""
import argparse
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
        """Wall looks onto another building (a party wall nobody photographed)."""
        a = f.origin[:2]
        u, n = f.u[:2], f.normal[:2]
        hits = 0
        k = max(2, int(f.size[0] / 1.0))
        for s in range(k):
            x, y = a + u * (f.size[0] * (s + 0.5) / k) + n * gap
            px, py = self.top.to_px(x, y)
            px, py = int(px), int(py)
            if not (0 <= py < self.top.obj.shape[0] and 0 <= px < self.top.obj.shape[1]):
                continue
            if self.obj_cls[py, px] == CLS_BUILDING and self.top.obj[py, px] != self.oid and \
                    self.top.height[py, px] > self.ground_z + 3.0:
                hits += 1
        return hits > k // 2


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


def absorb_fragments(group, tris, tri_cls, buildings, models, tol=1.5, share=0.8):
    """Unrefined building objects that lie (almost) entirely inside or against a
    refined model - facade slivers the segmenter split off - are dropped: the
    model's walls now stand where they were."""
    import shapely
    if not models:
        return []
    zones = [(shapely.prepared.prep(m.footprint.buffer(tol)), m.ground_z - 1.0,
              m.top_z() + 1.0, m) for m in models.values()]
    out = []
    for oid in buildings:
        if oid in models:
            continue
        sel = tris.obj == oid
        if not sel.any():
            continue
        cen = tris.pos[sel].mean(1)
        inside = np.zeros(len(cen), bool)
        for prep, z0, z1, m in zones:
            b = m.footprint.bounds
            cand = (cen[:, 0] > b[0] - tol) & (cen[:, 0] < b[2] + tol) & (cen[:, 1] > b[1] - tol) &                    (cen[:, 1] < b[3] + tol) & (cen[:, 2] > z0) & ~inside
            if not cand.any():
                continue
            idx = np.flatnonzero(cand)
            hit = shapely.contains_xy(m.footprint.buffer(tol), cen[idx, 0], cen[idx, 1])
            inside[idx[hit]] = True
        if inside.mean() >= share:
            out.append(oid)
    return out


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--only", help="comma-separated building names")
    ap.add_argument("--no-sam", action="store_true", help="outline from height only")
    ap.add_argument("--no-glass", action="store_true", help="do not detect glass facades")
    ap.add_argument("--texel", type=float, default=0.08, help="baked texture resolution (m per texel)")
    ap.add_argument("--debug-dir", help="write per-building debug images here")
    ap.add_argument("--progress", action="store_true",
                    help="print '@progress <done> <total> <message>' lines (for MeshTool)")
    args = ap.parse_args()

    def progress(done, total, msg):
        if args.progress:
            print(f"@progress {done} {total} {msg}", flush=True)

    t0 = time.time()
    progress(0, 1, "Loading scene...")
    batches = mtscene.load(args.input)
    progress(0, 1, "Loading SAM2 and CLIP...")
    params = building.Params()
    predictor = torch = None
    if not args.no_sam:
        try:
            predictor, torch = load_sam()
        except Exception as e:  # noqa: BLE001
            print(f"SAM2 unavailable ({e}); outlines from height only", flush=True)
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
    for batch in batches:
        if batch.is_spline:
            continue
        for group in batch.groups:
            buildings = [i for i, o in enumerate(group.objects) if o.cls == CLS_BUILDING
                         and (only is None or o.name in only)]
            if not buildings:
                continue
            progress(0, len(buildings), "Rendering the scene from above...")
            tris = raster.collect(group)
            texs = raster.decode_textures(group)
            top = raster.top_down(tris, texs, params.res)
            cls_of = np.array([o.cls for o in group.objects] + [0], np.int32)
            obj_cls = cls_of[top.obj]                     # -1 -> last entry (0)
            tri_cls = cls_of[tris.obj]
            print(f"scene: {len(tris.obj)} triangles, {len(buildings)} buildings, "
                  f"raster {top.color.shape[1]}x{top.color.shape[0]} ({time.time() - t0:.0f} s)", flush=True)

            replaced, models = {}, {}
            visible = np.bincount(top.obj[top.obj >= 0].ravel(), minlength=len(group.objects)) * params.res ** 2
            fragment_ids = np.array([i for i, o in enumerate(group.objects)
                                     if o.cls == CLS_BUILDING and visible[i] < 20.0], np.int32)
            new_textures = []
            margin = int(10 / params.res)
            for bi, oid in enumerate(buildings):
                name = group.objects[oid].name
                total += 1
                progress(bi, len(buildings), f"Refining {name} ({bi + 1} of {len(buildings)})")
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
                near_sel = np.all((cen >= lo) & (cen <= hi), 1) & ~np.isin(tri_cls, [CLS_CAR, CLS_TREE, CLS_PLANTS])
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
                faces = building.model_faces(model)
                meshes, textures, cov, pages, glass_m2 = model_meshes(
                    model, faces, src, texs, args.texel, oid, len(group.textures) + len(new_textures), classifier)
                new_textures += textures
                replaced[oid] = meshes
                models[oid] = model
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

            absorbed = absorb_fragments(group, tris, tri_cls, buildings, models)
            for oid in absorbed:
                replaced.setdefault(oid, [])
            if absorbed:
                print(f"  absorbed {len(absorbed)} facade fragments into refined buildings", flush=True)
            if replaced:
                group.meshes = [m for m in group.meshes if m.object_id not in replaced] + \
                               [m for ms in replaced.values() for m in ms]
                group.textures += new_textures

    progress(1, 1, "Saving the refined scene...")
    mtscene.save(args.output, batches)
    print(f"Refined {done} of {total} buildings in {time.time() - t0:.0f} s; saved {args.output}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
