"""Clean-scene terrain: ground, road, plants and water rebuilt as one textured
height field.

Everything that is not trusted terrain in the top-down raster is a hole: trees
and cars (removed first), buildings (terrain continues underneath, hidden),
small raised clutter on the surface (street furniture, people), and gaps in the
capture. Heights and the orthophoto are filled from the surrounding terrain,
then meshed on a regular grid, one mesh per (texture page, class) so road and
ground stay separate objects. The captured ground/road meshes are replaced by
these; trees keep their captured meshes, cars are gone.
"""
from dataclasses import dataclass, field

import cv2
import numpy as np
from scipy import ndimage

import mtscene
import raster

CLS_GROUND, CLS_ROAD, CLS_BUILDING, CLS_CAR, CLS_TREE, CLS_PLANTS, CLS_WATER = 1, 2, 3, 4, 5, 6, 7
TERRAIN = (CLS_GROUND, CLS_ROAD, CLS_PLANTS, CLS_WATER)
REMOVED = TERRAIN + (CLS_CAR,)          # captured meshes the terrain replaces
CLASS_NAME = {CLS_GROUND: "ground", CLS_ROAD: "road", CLS_PLANTS: "plants", CLS_WATER: "water"}
PAGE = 2048

CLUTTER_HEIGHT = 0.5      # m above the local lowest terrain
CLUTTER_WIDTH = 3.0       # m: window for that baseline; bumps narrower than this are clutter...
CLUTTER_MAX_M2 = 60.0     # ...if their footprint is this small (people, bins, parked-car rows and buses the
                          # segmenter left in a ground class; kiosks); taller than MISHEIGHT is never ground
COVER_GAP = 4.0           # m: capture gaps up to this wide are bridged
MISHEIGHT = 3.0           # m above the baseline: not ground at all (facade / roof piece labelled ground)


@dataclass
class Terrain:
    top: object                   # raster.TopRaster
    res: float
    height: np.ndarray            # float32[H, W] filled
    cls: np.ndarray               # int32[H, W] terrain class everywhere (nearest)
    cover: np.ndarray             # bool[H, W] where terrain is generated
    holes: np.ndarray             # bool[H, W] what was filled
    color_holes: np.ndarray = None  # bool[H, W] colour taken from neighbours (holes + dark overlay texels)
    stats: dict = field(default_factory=dict)

    def height_at(self, x, y):
        px, py = self.top.to_px(np.asarray(x, float), np.asarray(y, float))
        px = np.clip(px.astype(int), 0, self.height.shape[1] - 1)
        py = np.clip(py.astype(int), 0, self.height.shape[0] - 1)
        return self.height[py, px]

    def outline_min(self, polygon, spacing=0.5):
        """Lowest terrain along a footprint's outer ring (None if off the raster)."""
        polys = list(polygon.geoms) if hasattr(polygon, "geoms") else [polygon]
        pts = []
        for poly in polys:
            c = np.asarray(poly.exterior.coords)[:, :2]
            for a, b in zip(c[:-1], c[1:]):
                n = max(1, int(np.hypot(*(b - a)) / spacing))
                pts.append(a + (b - a) * np.linspace(0, 1, n, endpoint=False)[:, None])
        if not pts:
            return None
        pts = np.concatenate(pts)
        px, py = self.top.to_px(pts[:, 0], pts[:, 1])
        inside = (px >= 0) & (py >= 0) & (px < self.height.shape[1]) & (py < self.height.shape[0])
        if not inside.any():
            return None
        return float(self.height[py[inside].astype(int), px[inside].astype(int)].min())


def relabel_vehicles(tris, objects, log=print):
    """Buses, trucks and vans the segmenter made into a 'building' or 'tree':
    instance objects with a vehicle's footprint, height and elongation. They
    become cars, so the clean pass removes them and fills the surface."""
    n = len(objects)
    cls = np.array([o.cls for o in objects], np.int32)
    cand = np.flatnonzero(np.isin(cls, [CLS_BUILDING, CLS_TREE]))
    if not len(cand):
        return 0
    cen = tris.pos.reshape(-1, 3)
    oid = np.repeat(tris.obj, 3)
    changed = 0
    for i in cand:
        P = cen[oid == i]
        if len(P) < 12:
            continue
        lo, hi = np.percentile(P, 2, axis=0), np.percentile(P, 98, axis=0)
        height = hi[2] - lo[2]
        # Footprint along the object's own axes (a bus at 45 degrees to the grid).
        xy = P[:, :2] - P[:, :2].mean(0)
        _, _, vt = np.linalg.svd(xy, full_matrices=False)
        q = xy @ vt.T
        length = np.percentile(q[:, 0], 98) - np.percentile(q[:, 0], 2)
        width = np.percentile(q[:, 1], 98) - np.percentile(q[:, 1], 2)
        area = length * width
        small = 1.0 <= height <= 4.5 and area <= 60.0 and length <= 14.0
        if not small:
            continue
        # A 'building' this size is a vehicle, kiosk or shed; a 'tree' only if it
        # is bus-shaped or too low to be a tree.
        if cls[i] == CLS_TREE and not (length / max(width, 0.3) >= 1.6 or height < 2.5):
            continue
        objects[i].cls = CLS_CAR
        changed += 1
    if changed:
        log(f"  {changed} vehicle-sized building/tree objects relabelled as cars")
    return changed


def analyse(top, obj_cls, res, color_holes=None, log=print):
    """Height field, texture-ready colour and class maps with all holes filled.
    color_holes: extra pixels whose colour (not height) is untrustworthy."""
    h = top.height.astype(np.float32)
    valid = np.isfinite(h)
    cls = np.where(obj_cls >= 0, obj_cls, 0).astype(np.int32)
    terrain = valid & np.isin(cls, TERRAIN)
    px_m2 = res * res

    # Clutter and misclassified heights: compare each terrain pixel with the
    # lowest trusted terrain within CLUTTER_WIDTH (erosion; non-terrain is +inf
    # so holes never take part). Small bumps are clutter (people, poles, bins;
    # cars have their own class); anything far above the baseline is a wall or
    # roof piece the segmenter put in a ground class, whatever its size.
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (int(CLUTTER_WIDTH / res) | 1,) * 2)
    h_e = np.where(terrain, h, 1e4).astype(np.float32)
    base = cv2.erode(h_e, k)
    rise = h - base
    bump = terrain & (rise > CLUTTER_HEIGHT)
    lab, n = ndimage.label(bump)
    if n:
        areas = np.bincount(lab.ravel())[1:] * px_m2
        small = np.flatnonzero(areas <= CLUTTER_MAX_M2) + 1
        clutter = np.isin(lab, small)
    else:
        clutter = np.zeros_like(bump)
    misheight = terrain & (rise > MISHEIGHT)
    clutter |= misheight

    holes = ~terrain | clutter
    kc = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (int(COVER_GAP / res) | 1,) * 2)
    cover = cv2.morphologyEx(valid.astype(np.uint8), cv2.MORPH_CLOSE, kc).astype(bool)

    # Heights: trusted terrain as captured (no smoothing: a filter window
    # straddling a facade would mix roof heights into the ground), holes filled.
    hs = np.where(valid, h, 0.0).astype(np.float32)
    hs[holes] = 0.0
    mask = holes.astype(np.uint8)
    height = cv2.inpaint(hs, mask, 3, cv2.INPAINT_TELEA)

    # Class everywhere: nearest trusted terrain pixel.
    if terrain.any():
        _, (iy, ix) = ndimage.distance_transform_edt(~(terrain & ~clutter), return_indices=True)
        cls_full = cls[iy, ix]
    else:
        cls_full = np.full_like(cls, CLS_GROUND)

    stats = {
        "terrain_m2": float((terrain & ~clutter).sum() * px_m2),
        "filled_under_trees_m2": float((valid & (cls == CLS_TREE) & cover).sum() * px_m2),
        "filled_under_cars_m2": float((valid & (cls == CLS_CAR) & cover).sum() * px_m2),
        "clutter_removed_m2": float(clutter.sum() * px_m2),
        "clutter_blobs": int(len(np.unique(lab[clutter & ~misheight])) if (clutter & ~misheight).any() else 0),
        "misheight_m2": float(misheight.sum() * px_m2),
        "filled_under_buildings_m2": float((valid & (cls == CLS_BUILDING) & cover).sum() * px_m2),
        "filled_gaps_m2": float((~valid & cover).sum() * px_m2),
    }
    log(f"  terrain: {stats['terrain_m2']:.0f} m2 captured; filled {stats['filled_under_trees_m2']:.0f} m2 under trees, "
        f"{stats['filled_under_cars_m2']:.0f} m2 under cars, {stats['clutter_removed_m2']:.0f} m2 of clutter "
        f"({stats['clutter_blobs']} blobs), {stats['misheight_m2']:.0f} m2 of wall/roof pieces labelled ground, "
        f"{stats['filled_under_buildings_m2']:.0f} m2 under buildings, "
        f"{stats['filled_gaps_m2']:.0f} m2 of capture gaps")
    ch = holes if color_holes is None else (holes | color_holes)
    stats["dark_overlay_m2"] = float(((ch & ~holes)).sum() * px_m2)
    if color_holes is not None:
        log(f"  terrain: {stats['dark_overlay_m2']:.0f} m2 of surface showed only Google Earth's dark overlay texture; recoloured from neighbours")
    return Terrain(top, res, height, cls_full, cover, holes, ch, stats)


def fill_color(top, terrain, inpainter=None, log=print):
    """Orthophoto with the holes filled. With a learned inpainter (LaMa) the
    fill continues lane markings, kerbs and paving across the hole; without
    one, a two-scale geometric fill. Pixels of placeholder yellow never seed
    the fill; where they are genuine surface (lane markings) they are put back."""
    color = np.ascontiguousarray(top.color)
    yellow = raster.yellowish(color)
    mask = terrain.color_holes | yellow
    keep = yellow & ~terrain.color_holes
    if inpainter is not None:
        try:
            # Dilate a little: the rim pixels of a removed object are blends of
            # it and the surface, and the model should not continue those.
            k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
            grown = cv2.dilate(mask.astype(np.uint8), k).astype(bool) & terrain.cover
            filled = inpainter.fill(color, grown, log=log)
            filled[keep] = color[keep]
            return filled
        except Exception as e:  # noqa: BLE001
            log(f"  LaMa inpainting failed ({e!r}); geometric fill instead")
    m8 = mask.astype(np.uint8)
    # Fine fill for the first metre inside a hole (continues edges), coarse fill
    # (1/4 resolution: 3 m of context) deeper in, so a few shadowed pixels at the
    # rim do not colour a whole filled area.
    fine = cv2.inpaint(color, m8, 3, cv2.INPAINT_TELEA)
    small = cv2.resize(color, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA)
    msmall = (cv2.resize(m8, (small.shape[1], small.shape[0]), interpolation=cv2.INTER_AREA) > 0).astype(np.uint8)
    coarse = cv2.resize(cv2.inpaint(small, msmall, 3, cv2.INPAINT_TELEA), (color.shape[1], color.shape[0]),
                        interpolation=cv2.INTER_LINEAR)
    depth = cv2.distanceTransform(m8, cv2.DIST_L2, 3) * terrain.res      # m into the hole
    w = np.clip((depth - 0.5) / 1.0, 0.0, 1.0)[..., None]                # 0 at the rim .. 1 from 1.5 m in
    filled = (fine * (1 - w) + coarse * w).astype(np.uint8)
    filled[~mask] = color[~mask]
    filled[keep] = color[keep]
    return filled


def build_meshes(terrain, color, step_px, object_of_cls, tex_base):
    """Grid mesh of the terrain. step_px must divide PAGE. object_of_cls maps a
    terrain class to its object index. Returns (meshes, textures)."""
    assert PAGE % step_px == 0, "grid step must divide the page size"
    top = terrain.top
    H, W = terrain.height.shape
    gx = np.arange(0, W, step_px)
    gy = np.arange(0, H, step_px)
    nx, ny = len(gx), len(gy)
    Z = terrain.height[gy[:, None], gx[None, :]]
    cover_v = terrain.cover[gy[:, None], gx[None, :]]

    # Cells: all four corners covered. Class at the cell centre.
    cell_ok = cover_v[:-1, :-1] & cover_v[1:, :-1] & cover_v[:-1, 1:] & cover_v[1:, 1:]
    cy = np.minimum(gy[:-1] + step_px // 2, H - 1)
    cx = np.minimum(gx[:-1] + step_px // 2, W - 1)
    cell_cls = terrain.cls[cy[:, None], cx[None, :]]
    # Diagonal with the smaller height change (keeps curbs and steps crisp).
    d1 = np.abs(Z[:-1, :-1] - Z[1:, 1:])
    d2 = np.abs(Z[1:, :-1] - Z[:-1, 1:])
    use_d1 = d1 <= d2

    # World vertex positions (pixel corners).
    wx, wy = top.to_world(gx.astype(np.float64), gy.astype(np.float64))
    VX, VY = np.meshgrid(wx, wy)
    centre = np.array([float(VX.mean()), float(VY.mean()), float(np.median(Z[cover_v])) if cover_v.any() else 0.0])

    meshes, textures = [], []
    pages_x = (W + PAGE - 1) // PAGE
    pages_y = (H + PAGE - 1) // PAGE
    for py_ in range(pages_y):
        for px_ in range(pages_x):
            x0, y0 = px_ * PAGE, py_ * PAGE
            pw, ph = min(PAGE, W - x0), min(PAGE, H - y0)
            pw4, ph4 = (pw + 3) // 4 * 4, (ph + 3) // 4 * 4
            # Vertex index range whose pixel lies in [x0, x0 + pw]; the right/bottom
            # edge vertex of a page may coincide with the next page's first.
            i0, i1 = x0 // step_px, min(nx - 1, (x0 + pw) // step_px)
            j0, j1 = y0 // step_px, min(ny - 1, (y0 + ph) // step_px)
            ci = slice(i0, i1)      # cells whose min corner is in this page
            cj = slice(j0, j1)
            ok = cell_ok[cj, ci]
            if not ok.any():
                continue
            img = cv2.copyMakeBorder(color[y0:y0 + ph, x0:x0 + pw], 0, ph4 - ph, 0, pw4 - pw, cv2.BORDER_REPLICATE)
            tex_index = tex_base + len(textures)
            textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1, raster.GL_COMPRESSED_RGB_S3TC_DXT1, 0,
                                            [(pw4, ph4, raster.encode_dxt1(np.ascontiguousarray(img)))]))
            # Page vertex arrays.
            vi = np.arange(i0, i1 + 1)
            vj = np.arange(j0, j1 + 1)
            P = np.stack([VX[vj[:, None], vi[None, :]], VY[vj[:, None], vi[None, :]], Z[vj[:, None], vi[None, :]]], -1)
            UV = np.stack([(gx[vi][None, :] - x0) / pw4 * np.ones((len(vj), 1)),
                           (gy[vj][:, None] - y0) / ph4 * np.ones((1, len(vi)))], -1)
            nvi = len(vi)
            vid = lambda jj, ii: (jj - j0) * nvi + (ii - i0)
            JJ, II = np.meshgrid(np.arange(j0, j1), np.arange(i0, i1), indexing="ij")
            a, b, c, d = vid(JJ, II), vid(JJ, II + 1), vid(JJ + 1, II), vid(JJ + 1, II + 1)   # tl, tr, bl, br
            u1 = use_d1[cj, ci]
            # CCW seen from above (x right, y up in world; rows go down in y).
            t_a = np.where(u1[..., None], np.stack([a, c, d], -1), np.stack([a, c, b], -1))
            t_b = np.where(u1[..., None], np.stack([a, d, b], -1), np.stack([b, c, d], -1))
            tris_all = np.concatenate([t_a[..., None, :], t_b[..., None, :]], -2)   # [cj, ci, 2, 3]
            ccls = cell_cls[cj, ci]
            for k, oid in object_of_cls.items():
                sel = ok & (ccls == k)
                if not sel.any():
                    continue
                idx = tris_all[sel].reshape(-1)
                used, inv = np.unique(idx, return_inverse=True)
                verts = (P.reshape(-1, 3)[used] - centre).astype(np.float32)
                uvs = UV.reshape(-1, 2)[used].astype(np.float32)
                meshes.append(mtscene.Mesh(tex_index, oid, centre, verts, uvs, None,
                                           [(raster.GL_TRIANGLES, inv.astype(np.uint32))], mtscene.MAT_CAPTURED))
    return meshes, textures


def clean_group(group, terrain, color, step_px, log=print):
    """Replace the group's captured terrain and car meshes by the rebuilt
    terrain; drop the car objects. Trees and buildings are untouched."""
    n = len(group.objects)
    cls_of = np.array([o.cls for o in group.objects], np.int32)
    removed = set(np.flatnonzero(np.isin(cls_of, REMOVED)).tolist())
    kept = [m for m in group.meshes if not (0 <= m.object_id < n and m.object_id in removed)]
    n_dropped = len(group.meshes) - len(kept)

    object_of_cls = {}
    for k in TERRAIN:
        hits = np.flatnonzero(cls_of == k)
        if len(hits):
            object_of_cls[k] = int(hits[0])
        elif (terrain.cls[terrain.cover] == k).any():
            group.objects.append(mtscene.SceneObject(CLASS_NAME[k], k))
            object_of_cls[k] = len(group.objects) - 1
    meshes, textures = build_meshes(terrain, color, step_px, object_of_cls, len(group.textures))
    group.textures += textures
    group.meshes = kept + meshes

    # Car objects out; renumber the rest.
    cars = [i for i in range(n) if group.objects[i].cls == CLS_CAR]
    if cars:
        remap, new_objects = {}, []
        for i, o in enumerate(group.objects):
            if i in cars:
                continue
            remap[i] = len(new_objects)
            new_objects.append(o)
        for m in group.meshes:
            m.object_id = remap.get(m.object_id, -1) if m.object_id >= 0 else -1
        group.objects = new_objects
    ntri = sum(len(dc[1]) // 3 for m in meshes for dc in m.draw_calls)
    log(f"  terrain mesh: {ntri} triangles in {len(meshes)} meshes on {len(textures)} texture page(s); "
        f"replaced {n_dropped} captured ground/road/car meshes; removed {len(cars)} car objects")
