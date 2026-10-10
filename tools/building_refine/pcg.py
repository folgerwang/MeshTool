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


def captured_details(points, object_id):
    """Conservative fallback: preserve the shell; detail verified flat roof patches.

    No inferred footprint or wall alignment is needed. Near-horizontal captured
    triangles are grouped by elevation; projected triangle unions retain holes.
    Elevation spread is bounded below the equipment's clearance above the roof.
    """
    from types import SimpleNamespace
    import building
    points = np.asarray(points, dtype=np.float64)
    if not len(points): return [], 0
    points = points[np.isfinite(points).all(axis=(1,2))]
    if not len(points): return [], 0
    cross = np.cross(points[:,1]-points[:,0], points[:,2]-points[:,0])
    area2 = np.linalg.norm(cross, axis=1)
    flat = (area2 > .02) & (np.abs(cross[:,2]) >= .995*area2)
    flat &= np.ptp(points[:,:,2], axis=1) <= .08
    # Avoid ground fragments assigned to the building.
    flat &= points[:,:,2].min(1) > points[:,:,2].min() + 2.0
    roofs = points[flat]
    faces = []
    if len(roofs):
        levels = np.floor(roofs[:,:,2].mean(1)/.10).astype(np.int64)
        # Bound work and detail count on noisy captures.
        groups = sorted(np.unique(levels), key=lambda k: -area2[flat][levels==k].sum())[:32]
        for level in groups:
            patch = roofs[levels==level]
            if np.ptp(patch[:,:,2]) > .10: continue
            lo, hi = patch.min(axis=(0,1)), patch.max(axis=(0,1))
            origin = np.array([lo[0],lo[1],hi[2]])
            faces.append(building.Face(patch,origin,np.array([1.,0,0]),np.array([0.,1,0]),
                         np.array([0.,0,1]),tuple(hi[:2]-lo[:2]),kind='roof'))
    center = points.mean(axis=(0,1))
    model = SimpleNamespace(footprint=SimpleNamespace(centroid=SimpleNamespace(x=center[0],y=center[1])), ground_z=center[2])
    roof_meshes, roof_count = detail_meshes(faces,object_id,model)
    bands, band_count = captured_bands(points, object_id)
    # Consolidate attachments by finish, including roof equipment.
    merged = []
    for mat in sorted({m.material for m in roof_meshes+bands}):
        chunks=[m.vertices.astype(np.float64)+m.translation-center for m in roof_meshes+bands if m.material==mat]
        verts=np.concatenate(chunks).astype(np.float32)
        merged.append(mtscene.Mesh(0xffffffff,object_id,center,verts,None,None,
                      [(4,np.arange(len(verts),dtype=np.uint32))],mat))
    return merged, roof_count+band_count


def captured_bands(points, object_id, spacing=3.4):
    """Slice captured wall triangles into surface-following floor trim.

    Bands follow each local wall segment, leaving capture holes open. No inferred
    facade rectangle is used, so curved and stepped buildings retain their shape.
    """
    points=np.asarray(points,dtype=np.float64)
    if not len(points): return [],0
    cross=np.cross(points[:,1]-points[:,0],points[:,2]-points[:,0])
    lengths=np.linalg.norm(cross,axis=1)
    wall=(lengths>.02)&(np.abs(cross[:,2])<.2*lengths)
    triangles=points[wall]
    if not len(triangles): return [],0
    normals=cross[wall]/lengths[wall,None]
    center=points.mean(axis=(0,1))
    lo,hi=np.percentile(triangles[:,:,2],[3,97])
    levels=np.arange(lo+spacing,hi-.5,spacing)[:80]
    from shapely.geometry import LineString
    from shapely.ops import linemerge
    chunks=[]
    for z in levels:
        segments=[]
        active=(triangles[:,:,2].min(1)<z)&(triangles[:,:,2].max(1)>z)
        for tri in triangles[active]:
            hits=[]
            for i,j in ((0,1),(1,2),(2,0)):
                a,b=tri[i],tri[j]
                if (a[2]<=z<b[2]) or (b[2]<=z<a[2]):
                    hits.append(a+(b-a)*((z-a[2])/(b[2]-a[2])))
            if len(hits)!=2: continue
            # Weld shared triangle intersections without spanning capture gaps.
            a,b=np.round(np.asarray(hits)[:,:2]/.02)*.02
            if np.linalg.norm(b-a)>.02: segments.append(LineString([a,b]))
        if not segments: continue
        merged=unary_union(segments)
        if merged.geom_type!='LineString': merged=linemerge(merged)
        lines=[merged] if merged.geom_type=='LineString' else list(merged.geoms)
        for line in lines:
            if line.length<4.: continue
            xy=np.asarray(line.simplify(.025).coords)
            if len(xy)<2: continue
            delta=np.diff(xy,axis=0);length=np.linalg.norm(delta,axis=1)
            normals=np.stack([delta[:,1],-delta[:,0]],axis=1)/length[:,None]
            outward=np.sum(((xy[:-1]+xy[1:])/2-center[:2])*normals,axis=1)
            # Use one orientation per continuous contour, avoiding flipped joints.
            if np.sum(outward*length)<0: normals=-normals
            join=np.vstack([normals[0],(normals[:-1]+normals[1:])/2,normals[-1]])
            denom=np.sum(join*np.vstack([normals,normals[-1]]),axis=1)
            join/=np.maximum(denom[:,None],.5)
            if line.is_ring:
                n=(normals[-1]+normals[0])/2
                n/=max(np.dot(n,normals[0]),.5)
                join[0]=join[-1]=n
            for i in range(len(xy)-1):
                # Matched corner offsets make the bands continuous at triangle seams.
                verts=[]
                for j in (i,i+1):
                    for offset in (.09,.27):
                        for height in (-.11,.11):
                            verts.append([*(xy[j]+join[j]*offset),z+height])
                verts=np.asarray(verts)
                axes=np.stack([np.r_[delta[i],0],np.r_[normals[i],0],[0.,0,1.]])
                idx=_TRI if np.linalg.det(axes)>0 else _TRI.reshape(-1,3)[:,::-1].ravel()
                chunks.append(verts[idx]-center)
                if len(chunks)>=12000: break
            if len(chunks)>=12000: break
        if len(chunks)>=12000: break
    if not chunks: return [],0
    vertices=np.concatenate(chunks).astype(np.float32)
    return [mtscene.Mesh(0xffffffff,object_id,center,vertices,None,None,
            [(4,np.arange(len(vertices),dtype=np.uint32))],MAT_STONE)],len(chunks)


def reflective_facades(meshes, object_ids):
    """Explicit material override; split walls from roofs without changing geometry."""
    import copy
    result=[]
    for mesh in meshes:
        if mesh.object_id not in object_ids or mesh.model_variant!=2 or mesh.material not in (0,3):
            result.append(mesh); continue
        walls=[]; opaque=[]
        for prim,idx in mesh.draw_calls:
            if prim!=4 or len(idx)%3:
                opaque.append((prim,idx)); continue
            tri=idx.reshape(-1,3)
            p=mesh.vertices[tri].astype(np.float64)
            n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0])
            length=np.linalg.norm(n,axis=1)
            iswall=(length>1e-8)&(np.abs(n[:,2])<.3*length)
            if iswall.any(): walls.append((prim,tri[iswall].ravel().copy()))
            if (~iswall).any(): opaque.append((prim,tri[~iswall].ravel().copy()))
        for draws,material in ((opaque,mesh.material),(walls,mtscene.MAT_GLASS)):
            if not draws: continue
            part=copy.copy(mesh);part.draw_calls=draws;part.material=material
            result.append(part)
    return result
