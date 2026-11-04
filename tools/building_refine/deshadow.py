"""Remove cast shadows from captured textures.

Works in texture space, so roofs, walls and ground are all covered and no
geometry changes. A cast shadow is a large dark region that is bluer than the
lit surface around it (it is lit by the sky alone). Windows are dark and blue
too, but small and rectangular; whole facades turned from the sun have no lit
surface around them in their texture and stay as captured.

Each shadow region is brought to the level of the lit ring around it, per Lab
channel, with a feathered edge for the penumbra. Texture detail is kept: only
the level and contrast of the region change.
"""
import cv2
import numpy as np

TEXEL = .156            # metres per texel of a typical capture tile
OPEN_M = 1.0            # thinner dark parts are not cast shadows
MIN_AREA = 6.           # m2: smaller dark regions are left alone
RECT_FILL = .8          # a region this full of its box and under RECT_AREA m2 is an opening
RECT_AREA = 40.
RING_M = 1.5            # lit reference ring around a region
MIN_GAIN = 1.25         # lit ring must be this much brighter
MAX_GAIN = 3.5          # and no more than this (a darker material, not a shadow)
BLUE_SHIFT = 1.0        # the region must be at least this much bluer in Lab b
MAX_BLUE_SHIFT = 12.  # sky-lit shade is at most this much bluer in Lab b
MAX_HUE_SHIFT = 4.    # and keeps its Lab a (red-green) within this
MIN_SEPARATION = 25.    # grey levels between the dark and bright modes of a texture
FEATHER_M = .4
EDGE_M = 1.0          # a cast shadow reaches this close to the surface's edge
DEBUG = None


def shadow_regions(lab, valid, texel=TEXEL):
    """Connected shadow candidates as a label image (0 = none) and their count."""
    L = lab[..., 0]
    values = L[valid]
    if values.size < 400:
        return np.zeros(L.shape, np.int32), 0
    threshold, _ = cv2.threshold(values.reshape(-1, 1), 0, 255, cv2.THRESH_BINARY+cv2.THRESH_OTSU)
    dark, bright = values[values <= threshold], values[values > threshold]
    if not dark.size or not bright.size or bright.mean()-dark.mean() < MIN_SEPARATION:
        return np.zeros(L.shape, np.int32), 0
    mask = ((L <= threshold) & valid).astype(np.uint8)
    k = max(3, int(round(OPEN_M/texel)) | 1)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k)))
    return cv2.connectedComponents(mask, connectivity=8, ltype=cv2.CV_32S)[::-1]


def remove(rgb, valid=None, texel=TEXEL, exclude=None):
    """Returns (image without cast shadows, shadow mask, stats). exclude marks
    pixels that are never shadow (glazing); they keep their capture."""
    h, w = rgb.shape[:2]
    if valid is None:
        valid = np.ones((h, w), bool)
    lab = cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    candidates = valid if exclude is None else valid & ~exclude
    labels, count = shadow_regions(cv2.cvtColor(rgb, cv2.COLOR_RGB2LAB), candidates, texel)
    stats = dict(candidates=max(0, count-1), removed=0, area_m2=0.)
    if count <= 1:
        return rgb, np.zeros((h, w), bool), stats
    area = texel*texel
    k = max(3, int(round(RING_M/texel))*2+1)
    ring_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
    all_dark = labels > 0
    out = lab.copy()
    weight = np.zeros((h, w), np.float32)
    target = lab.copy()
    boxes = cv2.connectedComponentsWithStats((labels > 0).astype(np.uint8), 8, cv2.CV_32S)
    _, boxed, box_stats, _ = boxes
    for i in range(1, box_stats.shape[0]):
        x, y, bw, bh, pixels = box_stats[i]
        m_area = pixels*area
        if m_area < MIN_AREA:
            continue
        if m_area < RECT_AREA and pixels/(bw*bh) > RECT_FILL:
            continue
        pad = k+2
        x0, y0, x1, y1 = max(0, x-pad), max(0, y-pad), min(w, x+bw+pad), min(h, y+bh+pad)
        region = boxed[y0:y1, x0:x1] == i
        ring = cv2.dilate(region.astype(np.uint8), ring_kernel).astype(bool) & ~all_dark[y0:y1, x0:x1] & valid[y0:y1, x0:x1]
        if ring.sum() < .1*pixels and ring.sum() < 200:
            continue
        # A cast shadow comes from something beyond the surface (another mass,
        # a neighbour): it reaches the surface's edge. A dark patch with lit
        # surface all round is paint, a mural or a stain.
        reach = max(1, int(round(EDGE_M/texel)))
        grown = cv2.dilate(region.astype(np.uint8), np.ones((2*reach+1, 2*reach+1), np.uint8)).astype(bool)
        at_edge = (grown & ~valid[y0:y1, x0:x1]).any() or y-reach <= 0 or x-reach <= 0 or             y+bh+reach >= h or x+bw+reach >= w
        if not at_edge:
            continue
        crop = lab[y0:y1, x0:x1]
        lit, dark = crop[ring], crop[region]
        gain = np.median(lit[:, 0])/max(np.median(dark[:, 0]), 1)
        shift = np.median(lit[:, 2])-np.median(dark[:, 2])
        if DEBUG is not None:
            DEBUG.append((float(m_area), float(gain), float(shift), int(x), int(y), int(bw), int(bh)))
        hue = abs(np.median(lit[:, 1])-np.median(dark[:, 1]))
        chroma = lambda x, b_off=0.: np.median(np.hypot(x[:, 1]-128, x[:, 2]-128+b_off))
        hue = max(hue, abs(chroma(lit)-chroma(dark))/2)
        # Sunlight and skylight differ in level and a little in blue, not in
        # hue: a darker paint or a mural differs in hue or is far bluer.
        if not MIN_GAIN <= gain <= MAX_GAIN or not BLUE_SHIFT <= shift <= MAX_BLUE_SHIFT or hue > MAX_HUE_SHIFT:
            continue
        # Match level and contrast to the lit ring, per channel.
        fixed = crop.copy()
        # Lightness: the lit level and contrast. Colour: only the sky's blue
        # cast is taken off (Lab b shift); signs and paint keep their hue.
        a, b = dark[:, 0], lit[:, 0]
        scale = np.clip(np.std(b)/max(np.std(a), 1e-3), .5, 2.5)
        fixed[..., 0] = np.median(b)+(crop[..., 0]-np.median(a))*scale
        fixed[..., 2] = crop[..., 2]+shift
        target[y0:y1, x0:x1][region] = fixed[region]
        weight[y0:y1, x0:x1][region] = 1.
        stats['removed'] += 1
        stats['area_m2'] += float(m_area)
    if not stats['removed']:
        return rgb, np.zeros((h, w), bool), stats
    mask = weight > 0
    # Grow into the penumbra with the nearest corrected value, then feather.
    f = max(1, int(round(FEATHER_M/texel)))
    grown = cv2.dilate(mask.astype(np.uint8), np.ones((2*f+1, 2*f+1), np.uint8))
    offset = cv2.GaussianBlur((target-lab)*mask[..., None], (0, 0), f)/np.maximum(cv2.GaussianBlur(mask.astype(np.float32), (0, 0), f), 1e-3)[..., None]
    alpha = cv2.GaussianBlur(mask.astype(np.float32), (0, 0), f/2)
    inside = (target-lab)*mask[..., None]
    delta = np.where(mask[..., None], inside, offset*(grown[..., None] > 0))
    out = lab+delta*np.maximum(alpha, mask.astype(np.float32))[..., None]
    out = np.where(candidates[..., None], out, lab)
    result = cv2.cvtColor(np.clip(out, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB)
    return result, mask, stats


try:
    import numba
    _jit = numba.njit(cache=True)
except ImportError:      # pragma: no cover
    def _jit(f):
        return f


@_jit
def _uv_raster(uvpix, attr, out, hit):
    """Write per-texel plane coordinates of each triangle (uvpix in texels,
    attr the plane metres at its corners) into out[H, W, 2]; textures wrap."""
    H, W = out.shape[:2]
    for t in range(uvpix.shape[0]):
        x0, y0 = uvpix[t, 0, 0], uvpix[t, 0, 1]
        x1, y1 = uvpix[t, 1, 0], uvpix[t, 1, 1]
        x2, y2 = uvpix[t, 2, 0], uvpix[t, 2, 1]
        area = (x1-x0)*(y2-y0)-(x2-x0)*(y1-y0)
        if abs(area) < 1e-9:
            continue
        for py in range(int(np.floor(min(y0, y1, y2))), int(np.ceil(max(y0, y1, y2)))+1):
            cy = py+.5
            for px in range(int(np.floor(min(x0, x1, x2))), int(np.ceil(max(x0, x1, x2)))+1):
                cx = px+.5
                w0 = ((x1-cx)*(y2-cy)-(x2-cx)*(y1-cy))/area
                w1 = ((x2-cx)*(y0-cy)-(x0-cx)*(y2-cy))/area
                w2 = 1.-w0-w1
                if w0 < -.02 or w1 < -.02 or w2 < -.02:
                    continue
                yy, xx = py % H, px % W
                out[yy, xx, 0] = w0*attr[t, 0, 0]+w1*attr[t, 1, 0]+w2*attr[t, 2, 0]
                out[yy, xx, 1] = w0*attr[t, 0, 1]+w1*attr[t, 1, 1]+w2*attr[t, 2, 1]
                hit[yy, xx] = True


def correct_texture(image, uvpix, plane_xy, delta, pitch):
    """Add a plane-space colour correction (delta float[h, w, 3], plane metres
    at `pitch`) to the texels a set of triangles covers in one texture.
    image uint8[H, W, 3]; uvpix float[T, 3, 2] texels; plane_xy float[T, 3, 2]."""
    H, W = image.shape[:2]
    coords = np.zeros((H, W, 2), np.float64)
    hit = np.zeros((H, W), np.bool_)
    _uv_raster(uvpix.astype(np.float64), plane_xy.astype(np.float64), coords, hit)
    if not hit.any():
        return image, 0
    h, w = delta.shape[:2]
    cx = np.clip((coords[..., 0]/pitch).astype(int), 0, w-1)
    cy = np.clip((h-coords[..., 1]/pitch).astype(int), 0, h-1)
    add = np.where(hit[..., None], delta[cy, cx], 0)
    changed = hit & (np.abs(add).max(-1) > .5)
    out = np.clip(image.astype(np.float32)+add, 0, 255).astype(np.uint8)
    return out, int(changed.sum())
