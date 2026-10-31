"""Capture-faithful facade: the original wall image on a clean plane, with
recessed openings exactly where the capture shows windows and doors.

No module grid is imposed. Openings come from the rectified capture
(capture_tiles.detect_openings and its row-supported pattern recovery). The
wall keeps the captured finish (murals, trim, signage, colour) with the window
imagery inpainted away; the glass keeps the captured window pixels. Captured
lighting stays baked in: this reproduces the image, not a physical albedo.
"""
import cv2
import numpy as np
import shapely
from shapely.geometry import box
from shapely.ops import unary_union

import mtscene
import raster

PITCH = .08          # rectified capture resolution the opening rules are tuned for
MAX_TEXTURE = 2048   # texture edge limit; larger walls are sampled more coarsely


def _texture(image):
    h, w = image.shape[:2]
    scale = min(1., MAX_TEXTURE/max(h, w))
    if scale < 1:
        image = cv2.resize(image, (max(4, int(w*scale)), max(4, int(h*scale))), interpolation=cv2.INTER_AREA)
    h, w = image.shape[:2]
    image = cv2.copyMakeBorder(image, 0, (-h) % 4, 0, (-w) % 4, cv2.BORDER_REPLICATE)
    return mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1, raster.GL_COMPRESSED_RGB_S3TC_DXT1, 0,
                           [(image.shape[1], image.shape[0], raster.encode_dxt1(np.ascontiguousarray(image)))]), \
        (w/image.shape[1], h/image.shape[0])


def observe(cap, origin, u, v, n, size, region, ground_offset):
    """Rectified capture of one wall patch and the openings it shows.

    Returns None when too little of the patch was captured to reproduce it.
    Opening bounds are patch metres (x right, y up from the patch origin).
    """
    import facade_pattern
    from refine import fill_holes
    from capture_tiles import detect_openings, recover_patterned_windows
    from building_rules import captured_wall_material
    pitch = max(PITCH, float(max(size))/4096)
    image, mask = facade_pattern.project_patch(cap, origin, u, v, n, size, pitch)
    # Coverage counts the wall itself, not its bounding box: a small wall on
    # a large irregular plane is still fully captured.
    h, w = mask.shape
    ys, xs = (np.mgrid[:h, :w]+.5)*pitch
    inside = shapely.contains_xy(region, xs, h*pitch-ys)
    if mask.sum() < 400 or not inside.any() or mask[inside].mean() < .4:
        return None
    image, mask, extended = extend_base(image, mask, pitch)
    raw = fill_holes(image.copy(), mask)
    openings = recover_patterned_windows(raw, pitch, detect_openings(raw, pitch, ground_offset))
    kept = []
    for o in openings:
        shape = box(*o['bounds'])
        # An opening must lie on this wall and on captured pixels.
        if region.buffer(.05).covers(shape):
            x0, y0, x1, y1 = o['pixel_rect']
            # Foliage in front of a wall has dark gaps too; it is not glazing.
            if mask[y0:y1, x0:x1].mean() > .6 and vegetation(raw[y0:y1, x0:x1]) < .3:
                kept.append(dict(o, shape=shape))
    wall, _ = captured_wall_material(raw, kept, pitch)
    return dict(pitch=pitch, size=(image.shape[1]*pitch, image.shape[0]*pitch), raw=raw, wall=wall,
                openings=kept, coverage=float(mask.mean()), extended_m2=extended*pitch*pitch)


def vegetation(pixels):
    """Share of green-dominant pixels (trees and planting in front of walls)."""
    p = pixels.reshape(-1, 3).astype(int)
    if not len(p):
        return 0.
    return float(np.mean((p[:, 1] > p[:, 0]+6) & (p[:, 1] > p[:, 2]+2)))


def extend_base(image, mask, pitch, band_m=3.6):
    """Continue each column's lowest captured storey down an uncaptured base.

    Top-down captures often miss the lower floors that base recovery then
    completes. Repeating the lowest observed storey band keeps the completed
    base looking like the building instead of one flat hole-fill colour. Only
    the gap below a column's lowest captured pixel is filled, and only from at
    least one full band of captured pixels above it; the result is inferred.
    """
    h, w = mask.shape
    band = max(4, int(round(band_m/pitch)))
    covered = mask.astype(bool)
    lowest = np.where(covered.any(0), h-1-np.argmax(covered[::-1], 0), -1)
    image, mask = image.copy(), covered.copy()
    filled = 0
    for c in np.flatnonzero((lowest >= band) & (lowest < h-1)):
        top = lowest[c]-band+1
        if not covered[top:lowest[c]+1, c].mean() > .9 or vegetation(image[top:lowest[c]+1, c]) > .2:
            continue
        gap = np.arange(lowest[c]+1, h)
        source = top+(gap-lowest[c]-1) % band
        image[gap, c] = image[source, c]
        mask[gap, c] = covered[source, c]
        filled += len(gap)
    return image, mask, filled


def textures(observation):
    """(wall texture, raw texture, uv scale) for one observed patch."""
    wall, scale = _texture(observation['wall'])
    raw, _ = _texture(observation['raw'])
    return wall, raw, scale


def opening_union(observation):
    return unary_union([o['shape'] for o in observation['openings']]) if observation['openings'] else None
