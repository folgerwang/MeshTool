"""Metric architectural details on fitted capture faces (MeshTool is Z-up).

Parts are consolidated per finish rather than creating an object for every
mullion. Detail surfaces stand clear of captured facades. Fit checks stay in
refine.py; the building silhouette and projected facade textures are retained.
"""
from collections import defaultdict

import numpy as np
from shapely.geometry import Polygon
from shapely.ops import unary_union

import mtscene

MAT_FACADE, MAT_METAL, MAT_ROOF, MAT_STONE = 3, 4, 5, 6
_CORNERS = np.array([[x, y, z] for x in (-.5, .5) for y in (-.5, .5) for z in (-.5, .5)])
_TRI = np.array([0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5,
                 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6,
                 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3])


def detail_meshes(faces, object_id, model):
    parts = defaultdict(list)
    count = 0
    seen = set()
    def box(center, axes, dims, mat):
        nonlocal count
        center, axes, dims = np.asarray(center), np.asarray(axes), np.asarray(dims)
        if not np.isfinite(center).all() or np.min(dims) <= 0: return
        key = (mat, tuple(np.round(center, 4)), tuple(np.round(dims, 4)), tuple(np.round(axes.ravel(), 4)))
        if key in seen: return
        seen.add(key)
        verts = (_CORNERS * dims) @ axes + center
        idx = _TRI if np.linalg.det(axes) > 0 else _TRI.reshape(-1, 3)[:, ::-1].ravel()
        parts[mat].append(verts[idx])
        count += 1

    for f in faces:
        if f.kind != 'wall' or not f.exterior: continue
        w, h = f.size
        if w < 1 or h < 2: continue
        # Wall faces can be stepped or triangular. Only place details inside
        # their actual triangulated region, including courtyard openings.
        uv = (f.tris - f.origin) @ np.stack([f.u, f.v], axis=1)
        region = unary_union([Polygon(t) for t in uv])
        axes = np.stack([f.u, f.normal, f.v])
        def wallbox(x, z, width, height, depth=.10, mat=MAT_METAL, inset=.035):
            rectangle = Polygon([(x-width/2,z-height/2), (x+width/2,z-height/2),
                                 (x+width/2,z+height/2), (x-width/2,z+height/2)])
            if region.buffer(.001).covers(rectangle):
                box(f.origin + f.u*x + f.v*z + f.normal*(inset+depth/2), axes,
                    [width, depth, height], mat)
        # Top coping and base course. Floor bands occupy different vertical
        # intervals from the mullions, avoiding overlapping coplanar joints.
        wallbox(w/2, h-.12, max(.1,w-.08), .18, .18, MAT_STONE)
        wallbox(w/2, .14, max(.1,w-.08), .20, .14, MAT_STONE)
        floors = min(80, max(1, int(round(h/3.4))))
        step = h/floors
        for row in range(1, floors):
            wallbox(w/2, row*step, max(.1,w-.08), .10, .16)
        bays = min(80, max(1, int(round(w/2.8))))
        for col in range(1, bays):
            for row in range(floors):
                low = max(.29, row*step+.075)
                high = min(h-.24, (row+1)*step-.075)
                if high > low:
                    wallbox(col*w/bays, (low+high)/2, .065, high-low)
        # Rainwater downpipe clear of the trim at one end of a facade.
        if h < 25 and w > 4:
            wallbox(.32, h/2-.035, .09, max(.1,h-.67), .10, inset=.28)
            wallbox(w/2, h-.32, max(.1,w-.6), .10, .16, inset=.28)

    # Equipment only on near-flat roof parts and fully inside their outline.
    for f in faces:
        if f.kind != 'roof' or f.normal[2] < .97: continue
        uv = (f.tris-f.origin) @ np.stack([f.u, f.v], axis=1)
        region = unary_union([Polygon(t) for t in uv]).buffer(-.7)
        if region.is_empty: continue
        p = region.representative_point()
        width, length = 1.5, 2.1
        outline = Polygon([(p.x-width/2,p.y-length/2), (p.x+width/2,p.y-length/2),
                           (p.x+width/2,p.y+length/2), (p.x-width/2,p.y+length/2)])
        if not region.covers(outline): continue
        axes = np.stack([f.u, f.v, f.normal])
        c = f.origin + p.x*f.u + p.y*f.v
        box(c + f.normal*.16, axes, [width+.2,length+.2,.25], MAT_ROOF)
        box(c + f.normal*.68, axes, [width,length,.75], MAT_METAL)
        box(c + f.normal*1.10, axes, [width+.12,length+.12,.08], MAT_STONE)
        for z in np.linspace(.40,.95,6):
            box(c + f.v*(length/2+.045) + f.normal*z, axes, [width-.15,.05,.035], MAT_ROOF)

    center = np.array([model.footprint.centroid.x, model.footprint.centroid.y, model.ground_z])
    meshes = []
    for mat, chunks in sorted(parts.items()):
        p = (np.concatenate(chunks)-center).astype(np.float32)
        meshes.append(mtscene.Mesh(0xffffffff, object_id, center, p, None, None,
                                  [(4,np.arange(len(p),dtype=np.uint32))], mat))
    return meshes, count
