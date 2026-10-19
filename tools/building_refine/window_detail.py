"""Measure straight internal window members from the individual captured opening.

Only long, narrow light members supported by dark panes on BOTH sides qualify.
These measurements are evidence, not a universal grid imposed on every building.
"""
import numpy as np
from scipy.ndimage import gaussian_filter1d, label
from shapely.geometry import box
from shapely.ops import unary_union
from shapely.affinity import scale
from building_rules import outline, extrusion
from rules_config import RULES


def measure(crop, width, height, both_polarities=False):
    if both_polarities:
        result=measure(crop,width,height)
        for member in measure(255-crop,width,height):
            extent=width if member["axis"]=="x" else height
            if not any(m["axis"]==member["axis"] and abs(m["position"]-member["position"])*extent<.12 for m in result):
                result.append(member)
        return result
    rules=RULES['window_members']
    if min(crop.shape[:2]) < rules['min_pixels']:
        return []
    gray = crop.astype(float).mean(axis=2)
    result = []
    for axis, extent in ((0, width), (1, height)):
        image = gray if axis == 0 else gray.T
        n = image.shape[1]
        margin = max(2, round(image.shape[0] * rules['sample_margin_fraction']))
        image = image[margin:-margin]
        if not image.size:
            continue
        offset = max(2, round(n * rules['neighbor_offset_fraction']))
        contrast = image - np.maximum(np.roll(image, offset, 1), np.roll(image, -offset, 1))
        support = (contrast > rules['contrast_min']).mean(axis=0)
        score = gaussian_filter1d(support, .5)
        mask = score > rules['min_support']
        edge = max(offset + 1, round(n * rules['edge_fraction']))
        mask[:edge] = False
        mask[-edge:] = False
        labels, count = label(mask)
        for k in range(1, count + 1):
            pixels = np.flatnonzero(labels == k)
            size = len(pixels) / n * extent
            if not rules['min_width_m'] <= size <= rules['max_width_m']:
                continue
            position = (pixels.mean() + .5) / n
            # Image vertical coordinates run downward; model coordinates upward.
            if axis == 1:
                position = 1 - position
            result.append(dict(axis='x' if axis == 0 else 'y', position=float(position),
                               width_m=float(size), support=float(support[pixels].mean())))
    return result


def geometry(width, height, shape, members, depth):
    interior = outline(width, height, shape).buffer(-.07, join_style=2)
    bars = []
    for m in members:
        size = m['width_m']; pos = m['position']
        region = (box(pos*width-size/2, 0, pos*width+size/2, height)
                  if m['axis'] == 'x' else
                  box(0, pos*height-size/2, width, pos*height+size/2))
        bars.append(region.intersection(interior))
    if not bars:
        return np.empty((0, 3))
    # Snap sub-millimetre clipping slivers before extrusion. Curved crowns
    # can otherwise create duplicate vertices and zero-area side triangles.
    from shapely import set_precision
    merged = set_precision(unary_union(bars), RULES['window_members']['construction_precision_m'])
    pieces = []
    for poly in getattr(merged, 'geoms', [merged]):
        if poly.is_empty or poly.geom_type != 'Polygon':
            continue
        unit = scale(poly, xfact=1/width, yfact=1/height, origin=(0, 0))
        pieces.append(extrusion(unit, -depth*.85, -min(.015,depth*.25)))
    return np.concatenate(pieces) if pieces else np.empty((0, 3))


def member_color(crop, width, height, members):
    """Sample the measured bars themselves, rather than adjacent masonry."""
    samples=[]
    h,w=crop.shape[:2]
    for m in members:
        if m['axis']=='x':
            center=m['position']*w;half=max(.5,m['width_m']/width*w/2)
            strip=crop[round(h*.15):round(h*.85),max(0,int(center-half)):min(w,int(center+half)+1)]
        else:
            center=(1-m['position'])*h;half=max(.5,m['width_m']/height*h/2)
            strip=crop[max(0,int(center-half)):min(h,int(center+half)+1),round(w*.15):round(w*.85)]
        if strip.size:samples.append(strip.reshape(-1,3))
    return np.median(np.concatenate(samples),axis=0) if samples else None
