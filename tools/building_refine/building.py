"""Regularize one building: footprint + roof planes from the top-down raster,
straightened polygons, prism mesh (roofs + vertical walls).

All 2D work happens in crop pixel coordinates (row 0 = north); polygons are
converted to world metres at the end."""
import math
from dataclasses import dataclass, field

import cv2
import numpy as np
import shapely
from shapely.geometry import MultiPolygon, Polygon
from shapely.ops import unary_union


@dataclass
class Params:
    res: float = 0.25                 # m / px (top raster)
    min_area_m2: float = 25.0         # smaller buildings are left alone
    plane_tol: float = 0.30           # m, pixel-to-plane distance when growing a roof plane
    min_plane_m2: float = 4.0         # smaller roof parts merge into neighbours
    flat_deg: float = 4.0             # roof planes flatter than this become horizontal
    wall_slope_deg: float = 60.0      # steeper raster pixels are facade, not roof
    snap_deg: float = 15.0            # edges within this of the main axes are straightened
    simplify_m: float = 0.6           # polygon simplification tolerance
    min_edge_m: float = 0.8           # shorter straightened edges are dropped
    max_planes: int = 40
    min_outline_iou: float = 0.80     # below this (or height fit) the original mesh is kept
    min_height_fit: float = 0.70
    min_wall_fidelity: float = 0.65   # outer wall area that must sit on the captured facade


@dataclass
class RoofPart:
    polygon: Polygon                  # world metres
    plane: np.ndarray                 # z = a*x + b*y + c (world metres)


@dataclass
class BuildingModel:
    ground_z: float
    footprint: Polygon                # world metres
    parts: list = field(default_factory=list)
    axis_deg: float = 0.0
    sam_iou: float = -1.0             # IoU between SAM and height masks, -1 = SAM unused
    surface: object = None            # (x, y) -> captured surface height or None (courtyard floors)
    outline_iou: float = 0.0          # model footprint vs captured outline
    height_fit: float = 0.0           # share of roof pixels the model matches within 1.5 m

    def top_z(self):
        """Highest roof point."""
        return max(float(np.max(np.asarray(q.polygon.exterior.coords) @ q.plane[:2] + q.plane[2]))
                   for q in self.parts)

    def acceptable(self, p):
        return self.outline_iou >= p.min_outline_iou and self.height_fit >= p.min_height_fit


# ----------------------------------------------------------------------------
# Masks
# ----------------------------------------------------------------------------
def clean_mask(mask, px):
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * px + 1, 2 * px + 1))
    m = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_OPEN, k)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, k)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(m, 8)
    if n <= 1:
        return m.astype(bool)
    keep = 1 + np.argmax(stats[1:, cv2.CC_STAT_AREA])
    # Keep every reasonably large part (L-shaped blocks split by a thin gap stay whole).
    big = stats[1:, cv2.CC_STAT_AREA] >= 0.1 * stats[keep, cv2.CC_STAT_AREA]
    return np.isin(lab, 1 + np.flatnonzero(big))


def refine_with_sam(predictor, rgb, mask, raised, other_building):
    """SAM2 on the orthophoto crop, prompted with the height mask's box and
    interior/exterior points. Returns (mask, iou with the height mask)."""
    ys, xs = np.nonzero(mask)
    box = np.array([xs.min(), ys.min(), xs.max() + 1, ys.max() + 1], np.float32)
    dist = cv2.distanceTransform(mask.astype(np.uint8), cv2.DIST_L2, 5)
    pos = []
    d = dist.copy()
    for _ in range(4):
        y, x = np.unravel_index(np.argmax(d), d.shape)
        if d[y, x] < 3:
            break
        pos.append((x, y))
        cv2.circle(d, (int(x), int(y)), int(max(6, dist[y, x])), 0, -1)
    neg = []
    ring = cv2.dilate(mask.astype(np.uint8), np.ones((15, 15), np.uint8)).astype(bool) & ~mask & ~raised
    ry, rx = np.nonzero(ring)
    if len(rx):
        sel = np.linspace(0, len(rx) - 1, min(4, len(rx))).astype(int)
        neg = list(zip(rx[sel], ry[sel]))
    pts = np.array(pos + neg, np.float32)
    lbl = np.array([1] * len(pos) + [0] * len(neg), np.int32)

    predictor.set_image(rgb)
    masks, scores, _ = predictor.predict(point_coords=pts if len(pts) else None,
                                         point_labels=lbl if len(pts) else None,
                                         box=box, multimask_output=True)
    best, best_iou = None, 0.0
    for m in masks.astype(bool):
        m = m & ~other_building
        iou = (m & mask).sum() / max(1, (m | mask).sum())
        if iou > best_iou:
            best, best_iou = m, iou
    return best, best_iou


# ----------------------------------------------------------------------------
# Roof planes
# ----------------------------------------------------------------------------
def fit_plane(x, y, z):
    A = np.stack([x, y, np.ones_like(x)], 1)
    p, *_ = np.linalg.lstsq(A, z, rcond=None)
    return p


def segment_planes(z, roof, p, rng):
    """Greedy RANSAC + region growing on roof pixels. Returns label image
    (-1 = none) and plane list in pixel units (z = a*px + b*py + c)."""
    H, W = z.shape
    labels = np.full((H, W), -1, np.int32)
    planes = []
    remaining = roof.copy()
    min_px = p.min_plane_m2 / p.res ** 2
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    for _ in range(p.max_planes):
        ys, xs = np.nonzero(remaining)
        if len(xs) < min_px:
            break
        zs = z[ys, xs]
        best_n, best = 0, None
        for _ in range(200):
            i = rng.choice(len(xs), 3, replace=False)
            try:
                pl = np.linalg.solve(np.stack([xs[i], ys[i], np.ones(3)], 1).astype(np.float64), zs[i])
            except np.linalg.LinAlgError:
                continue
            if math.hypot(pl[0], pl[1]) / p.res > math.tan(math.radians(p.wall_slope_deg)):
                continue
            n = (np.abs(pl[0] * xs + pl[1] * ys + pl[2] - zs) < p.plane_tol).sum()
            if n > best_n:
                best_n, best = n, pl
        if best is None or best_n < min_px:
            break
        # Grow from the largest connected inlier blob, refitting twice.
        pl = best
        for _ in range(3):
            inl = remaining & (np.abs(pl[0] * xx + pl[1] * yy + pl[2] - z) < p.plane_tol)
            n, lab, stats, _ = cv2.connectedComponentsWithStats(inl.astype(np.uint8), 4)
            if n <= 1:
                break
            k = 1 + np.argmax(stats[1:, cv2.CC_STAT_AREA])
            region = lab == k
            ry, rx = np.nonzero(region)
            pl = fit_plane(rx.astype(np.float64), ry.astype(np.float64), z[ry, rx].astype(np.float64))
        if region.sum() < min_px:
            remaining &= ~region   # never pick this blob again
            continue
        labels[region] = len(planes)
        planes.append(pl)
        remaining &= ~region
    return labels, planes


def fill_labels(labels, mask):
    """Give every mask pixel the label of its nearest labelled pixel."""
    if (labels >= 0).sum() == 0:
        return labels
    known = (labels < 0).astype(np.uint8)
    _, idx = cv2.distanceTransformWithLabels(known, cv2.DIST_L2, 5, labelType=cv2.DIST_LABEL_PIXEL)
    ys, xs = np.nonzero(labels >= 0)
    lut = np.full(idx.max() + 1, -1, np.int32)
    lut[idx[ys, xs]] = labels[ys, xs]
    out = lut[idx]
    out[~mask] = -1
    return out


def merge_similar(labels, planes, p, z):
    """Merge neighbouring planes that describe the same surface."""
    changed = True
    while changed and len(planes) > 1:
        changed = False
        for a in range(len(planes)):
            ma = labels == a
            if not ma.any():
                continue
            ring = cv2.dilate(ma.astype(np.uint8), np.ones((3, 3), np.uint8)).astype(bool) & ~ma
            for b in np.unique(labels[ring]):
                if b < 0 or b == a:
                    continue
                both = ma | (labels == b)
                ys, xs = np.nonzero(both)
                pl = fit_plane(xs.astype(np.float64), ys.astype(np.float64), z[ys, xs].astype(np.float64))
                rms = np.sqrt(np.mean((pl[0] * xs + pl[1] * ys + pl[2] - z[ys, xs]) ** 2))
                if rms < p.plane_tol * 0.6:
                    labels[labels == b] = a
                    planes[a] = pl
                    changed = True
                    break
            if changed:
                break
    # Compact ids.
    used = [i for i in range(len(planes)) if (labels == i).any()]
    remap = {o: n for n, o in enumerate(used)}
    out = np.full_like(labels, -1)
    for o, n in remap.items():
        out[labels == o] = n
    return out, [planes[i] for i in used]


# ----------------------------------------------------------------------------
# Facade slices
# ----------------------------------------------------------------------------
def slice_inside(tris_px, z, shape, barrier, close_px=5):
    """Cross-section of the captured mesh at height z, as a filled mask.
    tris_px: float[t, 3, 3] = (px, py, world z). barrier: pixels the outside
    flood may not cross (other buildings, so a missing party wall does not
    leak). Returns None when the section does not close."""
    zs = tris_px[..., 2]
    span = (zs.min(1) < z) & (zs.max(1) > z)
    t = tris_px[span]
    if len(t) == 0:
        return None
    pts = []
    for a, c in ((0, 1), (1, 2), (2, 0)):
        za, zc = t[:, a, 2], t[:, c, 2]
        cross = (za - z) * (zc - z) < 0
        w = np.where(cross, (z - za) / np.where(cross, zc - za, 1), np.nan)
        p = t[:, a, :2] + (t[:, c, :2] - t[:, a, :2]) * w[:, None]
        pts.append(p)
    pts = np.stack(pts, 1)                                # t, 3, 2 (nan where no crossing)
    lines = np.zeros(shape, np.uint8)
    for tri in pts:
        q = tri[~np.isnan(tri[:, 0])]
        if len(q) >= 2:
            cv2.line(lines, tuple(np.round(q[0]).astype(int)), tuple(np.round(q[1]).astype(int)), 1, 1)
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close_px, close_px))
    walls = cv2.morphologyEx(lines, cv2.MORPH_CLOSE, k) | barrier.astype(np.uint8)
    flood = walls.copy()
    h, w = shape
    ff = np.zeros((h + 2, w + 2), np.uint8)
    outside = np.zeros(shape, bool)
    for y, x in [(0, 0), (0, w - 1), (h - 1, 0), (h - 1, w - 1)] + \
                [(0, x) for x in range(0, w, 8)] + [(h - 1, x) for x in range(0, w, 8)] + \
                [(y, 0) for y in range(0, h, 8)] + [(y, w - 1) for y in range(0, h, 8)]:
        if flood[y, x] == 0:
            cv2.floodFill(flood, ff, (x, y), 2)
    outside = flood == 2
    inside = ~outside & ~barrier
    return inside


def fit_to_facades(labels, planes, zs, ground_z, crop, p):
    """Move each roof part's outline onto the captured facade below it: keep
    the part only where the mesh cross-section just under its roof is solid,
    and let it grow (up to 2 m) where the facade stands outside the roof edge.
    Pixels a part gives up fall to the lower parts."""
    tris = crop.get("tris_px")
    if tris is None or not len(planes):
        return labels
    H, W = labels.shape
    barrier = crop["other_building"]
    grow = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (17, 17))      # 2 m
    tops = {i: float(np.median(zs[labels == i])) for i in range(len(planes)) if (labels == i).any()}
    order = sorted(tops, key=lambda i: -tops[i])
    out = np.full_like(labels, -1)
    pool = np.zeros((H, W), bool)
    for i in order:
        region = labels == i
        drop = max(1.5, min(4.0, 0.25 * (tops[i] - ground_z)))
        inside = slice_inside(tris, tops[i] - drop, (H, W), barrier)
        if inside is None or (inside & region).sum() < 0.3 * region.sum() or inside.sum() > 4 * region.sum() + pool.sum():
            keep = region & (out < 0)                 # section unusable: keep the roof outline
        else:
            cand = (cv2.dilate(region.astype(np.uint8), grow).astype(bool) | pool) & inside & (out < 0)
            n, lab = cv2.connectedComponents(cand.astype(np.uint8), connectivity=4)
            # Only the pieces that hold part of the roof region itself.
            ids = np.unique(lab[region & cand])
            keep = np.isin(lab, ids[ids > 0])
        out[keep] = i
        pool = (pool | (region & ~keep)) & (out < 0)
    return out


# ----------------------------------------------------------------------------
# Polygon regularization
# ----------------------------------------------------------------------------
def dominant_axis(mask):
    """Main building direction (radians, in [0, pi/2)) from boundary edges."""
    cs, _ = cv2.findContours(mask.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    hist = np.zeros(90)
    for c in cs:
        c = cv2.approxPolyDP(c, 1.5, True)[:, 0, :].astype(np.float64)
        d = np.roll(c, -1, 0) - c
        length = np.hypot(d[:, 0], d[:, 1])
        ang = (np.degrees(np.arctan2(d[:, 1], d[:, 0])) % 90).astype(int) % 90
        np.add.at(hist, ang, length)
    hist = sum(np.roll(hist, s) * w for s, w in [(-2, 1), (-1, 2), (0, 3), (1, 2), (2, 1)])
    return math.radians(float(np.argmax(hist)))


def _line_intersect(p1, d1, p2, d2):
    den = d1[0] * d2[1] - d1[1] * d2[0]
    if abs(den) < 1e-9:
        return None
    t = ((p2[0] - p1[0]) * d2[1] - (p2[1] - p1[1]) * d2[0]) / den
    return p1 + t * d1


def regularize_ring(contour, axis, p):
    """Straighten a closed pixel contour: simplify, snap edges near the main
    axes onto them (fit through the contour points they cover), re-intersect."""
    eps = p.simplify_m / p.res
    approx = cv2.approxPolyDP(contour.reshape(-1, 1, 2).astype(np.int32), eps, True)[:, 0, :]
    if len(approx) < 3:
        return None
    pts = contour.astype(np.float64)
    # Which contour points belong to which simplified edge.
    idx = [int(np.argmin(np.hypot(*(pts - a).T))) for a in approx.astype(np.float64)]
    snap = math.radians(p.snap_deg)
    lines = []                         # (point on line, unit direction, weight)
    n = len(approx)
    for i in range(n):
        a, b = approx[i].astype(np.float64), approx[(i + 1) % n].astype(np.float64)
        i0, i1 = idx[i], idx[(i + 1) % n]
        seg = pts[i0:i1 + 1] if i1 >= i0 else np.concatenate([pts[i0:], pts[:i1 + 1]])
        if len(seg) < 2:
            seg = np.stack([a, b])
        d = b - a
        L = math.hypot(*d)
        if L < 1e-6:
            continue
        ang = math.atan2(d[1], d[0])
        k = round((ang - axis) / (math.pi / 2))
        target = axis + k * math.pi / 2
        if abs(ang - target) < snap:
            u = np.array([math.cos(target), math.sin(target)])
        else:
            u = d / L
        nrm = np.array([-u[1], u[0]])
        off = float(np.median(seg @ nrm))
        lines.append([nrm * off, u, L, a])     # a = junction with the previous line
    # Merge consecutive lines that are (nearly) the same.
    merged = True
    while merged and len(lines) > 3:
        merged = False
        for i in range(len(lines)):
            j = (i + 1) % len(lines)
            pi, ui, li, ai = lines[i]
            pj, uj, lj, _ = lines[j]
            if abs(ui[0] * uj[1] - ui[1] * uj[0]) < 0.02 and ui @ uj > 0:
                nrm = np.array([-ui[1], ui[0]])
                if abs((pj - pi) @ nrm) < p.min_edge_m / p.res:
                    off = ((pi @ nrm) * li + (pj @ nrm) * lj) / (li + lj)
                    lines[i] = [nrm * off, ui, li + lj, ai]
                    del lines[j]
                    merged = True
                    break
    if len(lines) < 3:
        return None
    verts = []
    for i in range(len(lines)):
        p1, d1, _, _ = lines[i - 1]
        p2, d2, _, c = lines[i]
        x = _line_intersect(p1, d1, p2, d2)
        if x is None:
            # Parallel neighbours (a step): jog across at the junction.
            verts.append(p1 + d1 * ((c - p1) @ d1))
            verts.append(p2 + d2 * ((c - p2) @ d2))
        else:
            verts.append(x)
    poly = Polygon(verts)
    if not poly.is_valid:
        poly = shapely.make_valid(poly)
        if not isinstance(poly, (Polygon, MultiPolygon)):
            poly = unary_union([g for g in getattr(poly, "geoms", []) if isinstance(g, (Polygon, MultiPolygon))])
    raw = Polygon(approx.astype(np.float64))
    if poly.is_empty or not raw.is_valid or abs(poly.area - raw.area) > 0.25 * raw.area:
        return raw if raw.is_valid else None
    return poly


def mask_to_polygon(mask, axis, p):
    cs, hier = cv2.findContours(mask.astype(np.uint8), cv2.RETR_CCOMP, cv2.CHAIN_APPROX_NONE)
    if not cs:
        return None
    polys = []
    for i, c in enumerate(cs):
        if hier[0][i][3] != -1 or cv2.contourArea(c) < p.min_plane_m2 / p.res ** 2:
            continue
        outer = regularize_ring(c[:, 0, :] + 0.5, axis, p)   # +0.5: pixel centres
        if outer is None:
            continue
        child = hier[0][i][2]
        while child != -1:
            if cv2.contourArea(cs[child]) >= p.min_plane_m2 / p.res ** 2:
                hole = regularize_ring(cs[child][:, 0, :] + 0.5, axis, p)
                if hole is not None:
                    outer = outer.difference(hole)
            child = hier[0][child][0]
        polys.append(outer)
    if not polys:
        return None
    return unary_union(polys).buffer(0)


def _open(g, r):
    """Remove slivers thinner than 2r, keeping square corners square."""
    return g.buffer(-r, join_style="mitre", mitre_limit=4.0).buffer(r, join_style="mitre", mitre_limit=4.0)


def _polys(g):
    if g is None or g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    return [x for x in getattr(g, "geoms", []) if isinstance(x, Polygon) and not x.is_empty]


# ----------------------------------------------------------------------------
# Whole building
# ----------------------------------------------------------------------------
def build_model(crop, ground_z, p, predictor=None, rng=None):
    """crop: dict with height (H, W), rgb, mask (this building), other_building,
    origin (world x of px 0, world y of row 0)."""
    rng = rng or np.random.default_rng(0)
    z, mask = crop["height"].astype(np.float64), crop["mask"]
    hag = z - ground_z
    raised = np.isfinite(hag) & (hag > 1.5)
    model_mask = clean_mask(mask & raised, 2)
    if model_mask.sum() * p.res ** 2 < p.min_area_m2:
        return None

    sam_iou = -1.0
    if predictor is not None:
        sam, sam_iou = refine_with_sam(predictor, crop["rgb"], model_mask, raised, crop["other_building"])
        if sam is not None and sam_iou > 0.7:
            # SAM decides the outline where the height says "raised".
            grown = cv2.dilate(model_mask.astype(np.uint8), np.ones((5, 5), np.uint8)).astype(bool)
            model_mask = clean_mask(sam & raised & grown, 2)

    axis = dominant_axis(model_mask)
    footprint = mask_to_polygon(model_mask, axis, p)
    if footprint is None or footprint.area * p.res ** 2 < p.min_area_m2:
        return None

    # Roof planes on non-facade pixels.
    zs = cv2.medianBlur(np.where(np.isfinite(z), z, ground_z).astype(np.float32), 5).astype(np.float64)
    gy, gx = np.gradient(zs)
    slope = np.degrees(np.arctan(np.hypot(gx, gy) / p.res))
    roof = model_mask & (slope < p.wall_slope_deg)
    roof = cv2.erode(roof.astype(np.uint8), np.ones((3, 3), np.uint8)).astype(bool)
    labels, planes = segment_planes(zs, roof, p, rng)
    if not planes:
        ys, xs = np.nonzero(model_mask)
        labels = np.where(model_mask, 0, -1).astype(np.int32)
        planes = [np.array([0.0, 0.0, float(np.percentile(zs[ys, xs], 70))])]
    labels = fill_labels(labels, model_mask)
    labels, planes = merge_similar(labels, planes, p, zs)

    # Flatten near-horizontal planes.
    flat = math.tan(math.radians(p.flat_deg)) * p.res
    for i, pl in enumerate(planes):
        if math.hypot(pl[0], pl[1]) < flat:
            ys, xs = np.nonzero(labels == i)
            planes[i] = np.array([0.0, 0.0, float(np.median(zs[ys, xs]))])

    # Walls where the captured facades are, not where the roof edge is.
    labels = fit_to_facades(labels, planes, zs, ground_z, crop, p)
    model_mask = clean_mask(labels >= 0, 2) if (labels >= 0).any() else model_mask
    labels[~model_mask] = -1
    footprint = mask_to_polygon(model_mask, axis, p)
    if footprint is None or footprint.area * p.res ** 2 < p.min_area_m2:
        return None

    # Part polygons: taller parts claim overlaps, gaps go to the part that owns
    # most of their pixels.
    order = sorted(range(len(planes)), key=lambda i: -np.median(zs[labels == i]))
    parts, taken = {}, Polygon()
    for i in order:
        poly = mask_to_polygon(labels == i, axis, p)
        if poly is None:
            continue
        poly = _open(poly.intersection(footprint).difference(taken), 0.5)
        poly = poly.simplify(1.5, preserve_topology=True)      # boolean-op jaggies (px)
        if poly.area * p.res ** 2 < p.min_plane_m2:
            continue
        parts[i] = poly
        taken = taken.union(poly)
    if not parts:
        return None
    for gap in _polys(_open(footprint.difference(taken), 0.3)):
        gm = np.zeros_like(model_mask, np.uint8)
        cv2.fillPoly(gm, [np.asarray(gap.exterior.coords, np.float64).round().astype(np.int32)], 1)
        votes = np.bincount(labels[(gm > 0) & (labels >= 0)], minlength=len(planes))
        cand = [i for i in np.argsort(-votes) if i in parts]
        if not cand:
            cand = list(parts)
        k = cand[0]
        parts[k] = parts[k].union(gap).buffer(0)

    # How well the model explains the capture: outline IoU and the share of
    # roof pixels whose height the model reproduces within 1.5 m.
    H, W = model_mask.shape
    model_z = np.full((H, W), np.nan)
    for i, poly in parts.items():
        for g in _polys(poly):
            pm = np.zeros((H, W), np.uint8)
            cv2.fillPoly(pm, [np.asarray(g.exterior.coords).round().astype(np.int32)], 1)
            for r in g.interiors:
                cv2.fillPoly(pm, [np.asarray(r.coords).round().astype(np.int32)], 0)
            yy, xx = np.nonzero(pm)
            a, b, c = planes[i]
            model_z[yy, xx] = a * xx + b * yy + c
    covered = ~np.isnan(model_z)
    iou = (covered & model_mask).sum() / max(1, (covered | model_mask).sum())
    inside = covered & roof
    fit = (np.abs(model_z[inside] - zs[inside]) < 1.5).mean() if inside.any() else 0.0

    # Pixel -> world.
    ox, oy, res = crop["origin"][0], crop["origin"][1], p.res
    to_world = lambda g: shapely.transform(g, lambda c: np.stack([ox + c[:, 0] * res, oy - c[:, 1] * res], 1))
    model = BuildingModel(ground_z=float(ground_z), footprint=to_world(footprint),
                          axis_deg=math.degrees(axis), sam_iou=float(sam_iou),
                          outline_iou=float(iou), height_fit=float(fit))
    for i, poly in parts.items():
        a, b, c = planes[i]
        # z = a*px + b*py + c, px = (x-ox)/res, py = (oy-y)/res
        wp = np.array([a / res, -b / res, c - a * ox / res + b * oy / res])
        for g in _polys(to_world(poly)):
            model.parts.append(RoofPart(g, wp))
    return model


# ----------------------------------------------------------------------------
# Mesh
# ----------------------------------------------------------------------------
@dataclass
class Face:
    """A planar face to texture: triangles (world) and its 2D frame."""
    tris: np.ndarray        # float64[t, 3, 3]
    origin: np.ndarray      # frame origin (world)
    u: np.ndarray           # unit vectors spanning the face plane
    v: np.ndarray
    normal: np.ndarray      # outward
    size: tuple             # extent along u, v (m)
    kind: str = "roof"      # "roof" or "wall"
    part: int = -1          # index into model.parts
    edge: int = -1          # wall: edge index in the part's CCW outer ring (-1 = hole ring)
    exterior: bool = False  # wall: stands on the ground (not on a lower part)


def _ring_ccw(coords):
    c = np.asarray(coords)[:-1]
    area = 0.5 * np.sum(c[:, 0] * np.roll(c[:, 1], -1) - np.roll(c[:, 0], -1) * c[:, 1])
    return c if area > 0 else c[::-1]


def _triangulate(poly):
    for g in (poly, poly.buffer(0), poly.simplify(0.05).buffer(0), Polygon(poly.exterior)):
        try:
            return _polys(shapely.constrained_delaunay_triangles(g))
        except shapely.errors.GEOSException:
            continue
    return []


def _part_at(parts, x, y, skip):
    pt = shapely.Point(x, y)
    for j, q in enumerate(parts):
        if j != skip and q.polygon.distance(pt) < 1e-3:
            return j
    return -1


def model_faces(model, wall_step=1.0):
    """Roof faces per part; walls from each part's roof edge down to whatever
    is outside that edge (a lower part's roof or the ground). Walls next to a
    taller part are skipped: that part's wall covers them."""
    faces = []
    g = model.ground_z
    zf = lambda pl, x, y: pl[0] * x + pl[1] * y + pl[2]
    for pi_, part in enumerate(model.parts):
        pl = part.plane
        ts = []
        for t in _triangulate(part.polygon):
            c = np.asarray(t.exterior.coords)[:3]
            if Polygon(c).area < 1e-6:
                continue
            ts.append([[x, y, zf(pl, x, y)] for x, y in c])
        if ts:
            ts = np.array(ts)
            nz = np.cross(ts[:, 1] - ts[:, 0], ts[:, 2] - ts[:, 0])[:, 2]
            ts[nz < 0] = ts[nz < 0][:, ::-1]
            n = np.array([-pl[0], -pl[1], 1.0]); n /= np.linalg.norm(n)
            u = np.cross([0, 1.0, 0], n) if abs(n[1]) < 0.9 else np.cross(n, [1.0, 0, 0])
            u /= np.linalg.norm(u)
            v = np.cross(n, u)
            P = ts.reshape(-1, 3)
            lo_u, lo_v = (P @ u).min(), (P @ v).min()
            origin = u * lo_u + v * lo_v + n * (P @ n).mean()
            faces.append(Face(ts, origin, u, v, n, ((P @ u).max() - lo_u, (P @ v).max() - lo_v),
                              "roof", pi_))

        rings = [_ring_ccw(part.polygon.exterior.coords)] +                 [_ring_ccw(r.coords)[::-1] for r in part.polygon.interiors]
        for ri, ring in enumerate(rings):
            for i in range(len(ring)):
                a, b = ring[i], ring[(i + 1) % len(ring)]
                d = b - a
                L = math.hypot(*d)
                if L < 0.05:
                    continue
                u = np.array([d[0] / L, d[1] / L, 0.0])
                n = np.array([u[1], -u[0], 0.0])          # outward for a CCW ring
                # Bottom of the wall along the edge, in steps.
                k = max(1, int(round(L / wall_step)))
                bottoms, on_ground = [], []
                for s in range(k):
                    m = a + d * ((s + 0.5) / k)
                    o = m + n[:2] * 0.3
                    j = _part_at(model.parts, o[0], o[1], pi_)
                    top_z = zf(pl, *m)
                    if j >= 0:
                        bz = zf(model.parts[j].plane, *m)
                    elif ri > 0 and model.surface is not None:
                        # Courtyard / light well: down to its captured floor.
                        o2 = m + n[:2] * 1.0
                        sz = model.surface(o2[0], o2[1])
                        bz = g if sz is None else min(max(g, sz), top_z)
                    else:
                        bz = g
                    bottoms.append(bz if bz < top_z - 0.2 else None)
                    on_ground.append(j < 0 and ri == 0)
                # One quad per run of steps with (nearly) the same bottom.
                s = 0
                while s < k:
                    if bottoms[s] is None:
                        s += 1
                        continue
                    e = s
                    while e + 1 < k and bottoms[e + 1] is not None and abs(bottoms[e + 1] - bottoms[s]) < 0.3:
                        e += 1
                    bz = min(bottoms[s:e + 1])
                    pa, pb = a + d * (s / k), a + d * ((e + 1) / k)
                    ta, tb = zf(pl, *pa), zf(pl, *pb)
                    A0, B0 = np.array([pa[0], pa[1], bz]), np.array([pb[0], pb[1], bz])
                    A1, B1 = np.array([pa[0], pa[1], ta]), np.array([pb[0], pb[1], tb])
                    tris = np.array([[A0, B0, B1], [A0, B1, A1]])
                    faces.append(Face(tris, A0, u, np.array([0, 0, 1.0]), n,
                                      (math.hypot(*(pb - pa)), max(ta, tb) - bz), "wall", pi_,
                                      i if ri == 0 else -1, all(on_ground[s:e + 1])))
                    s = e + 1
    return faces

