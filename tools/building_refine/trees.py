"""Replace segmented tree shapes, preserving captured meshes as original variants.

Uses only NumPy and the scene reader. No models, downloads, or species matching.
"""
import argparse
import copy
import json
from pathlib import Path

import numpy as np
import mtscene

TREE = 5
GROUND = (1, 2, 6)


def points(meshes):
    chunks = []
    for mesh in meshes:
        if not len(mesh.vertices): continue
        indices = [np.asarray(i, dtype=np.int64) for _, i in mesh.draw_calls if len(i)]
        if not indices: continue
        used = np.unique(np.concatenate(indices))
        used = used[(used >= 0) & (used < len(mesh.vertices))]
        p = mesh.vertices[used].astype(float) + mesh.translation
        chunks.append(p[np.isfinite(p).all(axis=1)])
    return np.concatenate(chunks) if chunks else np.empty((0, 3))


class GroundSamples:
    def __init__(self, group):
        # Bucket nearby terrain samples once, rather than scanning the whole
        # scene for every tree. Median nearby heights reject isolated outliers.
        terrain = [m for m in group.meshes if m.model_variant != 1 and
                   0 <= m.object_id < len(group.objects) and group.objects[m.object_id].cls in GROUND]
        triangles = []
        for mesh in terrain:
            world = mesh.vertices.astype(float)+mesh.translation
            for primitive, indices in mesh.draw_calls:
                idx = np.asarray(indices,dtype=np.int64)
                if primitive == 4: idx = idx[:len(idx)//3*3].reshape(-1,3)
                elif primitive == 5 and len(idx) >= 3: idx = np.lib.stride_tricks.sliding_window_view(idx,3)
                else: continue
                idx = idx[((idx >= 0) & (idx < len(world))).all(axis=1)]
                if len(idx): triangles.append(world[idx])
        self.triangles = np.concatenate(triangles) if triangles else np.empty((0,3,3))
        self.low = self.triangles[:,:,:2].min(axis=1) if len(self.triangles) else np.empty((0,2))
        self.high = self.triangles[:,:,:2].max(axis=1) if len(self.triangles) else np.empty((0,2))
        p = points(terrain)
        if len(p) > 250000: p = p[::int(np.ceil(len(p)/250000))]
        self.cells = {}
        for xyz in p:
            key = tuple(np.floor(xyz[:2]/4).astype(int))
            self.cells.setdefault(key, []).append(xyz)

    def height(self, center, radius, low, source_height):
        # Interpolate a directly supporting terrain triangle first. This also
        # works for a broad ground quad with no nearby vertex under the tree.
        lower_limit = low-max(5.,min(20.,source_height*2))
        hit = ((self.low <= center[:2]).all(axis=1) & (self.high >= center[:2]).all(axis=1))
        tri = self.triangles[hit]
        if len(tri):
            a,b,c = tri[:,0],tri[:,1],tri[:,2]
            denom = (b[:,1]-c[:,1])*(a[:,0]-c[:,0])+(c[:,0]-b[:,0])*(a[:,1]-c[:,1])
            good = np.abs(denom) > 1e-8
            safe = np.where(good,denom,1)
            wa = ((b[:,1]-c[:,1])*(center[0]-c[:,0])+(c[:,0]-b[:,0])*(center[1]-c[:,1]))/safe
            wb = ((c[:,1]-a[:,1])*(center[0]-c[:,0])+(a[:,0]-c[:,0])*(center[1]-c[:,1]))/safe
            wc = 1-wa-wb
            z = wa*a[:,2]+wb*b[:,2]+wc*c[:,2]
            z = z[good & (wa >= -1e-6) & (wb >= -1e-6) & (wc >= -1e-6) & (z >= lower_limit) & (z <= low+.5)]
            if len(z): return min(low,float(z.max())),True
        reach = max(3., min(12., radius*.75))
        a = np.floor((center[:2]-reach)/4).astype(int)
        b = np.floor((center[:2]+reach)/4).astype(int)
        samples = [p for x in range(a[0], b[0]+1) for y in range(a[1], b[1]+1)
                   for p in self.cells.get((x, y), [])]
        if not samples: return low, False
        p = np.asarray(samples)
        d = np.linalg.norm(p[:,:2]-center[:2], axis=1)
        p, d = p[d <= reach], d[d <= reach]
        if not len(p): return low, False
        z = float(np.median(p[np.argsort(d)[:16],2]))
        if lower_limit <= z <= low+.5:
            return min(low, z), True
        return low, False


def palette():
    yy, xx = np.indices((16, 16))
    textures = []
    for base, pattern in [((54, 108, 45), ((xx*7+yy*11)%13)-6),
                          ((103, 73, 43), ((xx//2)%3)*6-6)]:
        image = np.empty((16,16,4), np.uint8)
        image[:,:,:3] = np.clip(np.array(base)+pattern[:,:,None],0,255)
        image[:,:,3] = 255
        textures.append(mtscene.Texture(0x8058,0x1908,0x1401,[(16,16,image.tobytes())]))
    return textures


def surface(rings, oid, center, texture, segments=16):
    """Closed ring surface with pole fans, avoiding zero-area pole triangles."""
    vertices, uv = [], []
    def at(ring, j):
        z, rx, ry = ring
        angle = 2*np.pi*j/segments
        return np.array([rx*np.cos(angle), ry*np.sin(angle), z])
    def emit(a, b, c, coords):
        if np.linalg.norm(np.cross(b-a,c-a)) < 1e-10: return
        vertices.extend((a,b,c)); uv.extend(coords)
    for k in range(len(rings)-1):
        for j in range(segments):
            a,b = at(rings[k],j),at(rings[k],j+1)
            c,d = at(rings[k+1],j),at(rings[k+1],j+1)
            u0,u1=j/segments,(j+1)/segments
            v0,v1=k/(len(rings)-1),(k+1)/(len(rings)-1)
            emit(a,b,c,[(u0,v0),(u1,v0),(u0,v1)])
            emit(b,d,c,[(u1,v0),(u1,v1),(u0,v1)])
    if not vertices: raise ValueError('empty procedural surface')
    p = np.asarray(vertices,np.float32)
    return mtscene.Mesh(texture,oid,center.copy(),p,np.asarray(uv,np.float32),None,
                        [(4,np.arange(len(p),dtype=np.uint32))],0,2)


def build(source, oid, texture_base=0, shape='fitted', ground=None):
    p = points(source)
    if len(p) < 3: raise ValueError('no usable captured geometry')
    low, high = p.min(0), p.max(0)
    extent = high-low
    if extent[2] < .5 or min(extent[:2]) < .15:
        raise ValueError('tree bounds too small or flat to fit')
    center = (low+high)/2
    base, grounded = (ground.height(center,max(extent[:2])/2,low[2],extent[2])
                      if ground is not None else (float(low[2]),False))
    height = high[2]-base
    center[2] = base
    # A thin lower capture is a trunk, not part of the crown. If only the
    # canopy was captured, retain its lower edge and add a trunk below it.
    canopy_low = max(low[2],base+height*.18)
    upper = p[p[:,2] >= base+height*.25]
    if len(upper):
        q = np.quantile(upper[:,2],[.15,.85])
        lower = upper[upper[:,2] <= q[0]]
        if len(lower) >= 4 and max(np.ptp(lower[:,:2],axis=0)) < max(extent[:2])*.25:
            canopy_low = base+height*.32
    crown_height = high[2]-canopy_low
    radii = extent[:2]/2
    rings = []
    for k, angle in enumerate(np.linspace(-np.pi/2,np.pi/2,13)):
        z = canopy_low+(np.sin(angle)+1)*.5*crown_height
        factor = float(np.cos(angle))
        if shape == 'conifer': factor = min(1., (high[2]-z)/max(crown_height,1e-6)*1.25)
        if shape == 'fitted' and 0 < k < 12:
            band = p[np.abs(p[:,2]-z) <= crown_height*.12]
            if len(band) >= 4:
                radial = np.max(np.abs((band[:,:2]-center[:2])/radii),axis=1)
                factor = float(np.clip(np.quantile(radial,.9),.15,1.))
        if k in (0,12): factor = 0.
        rings.append((z-base, radii[0]*factor, radii[1]*factor))
    # Preserve captured crown widths exactly, independent of ring tessellation.
    scale = max(max(r[1]/radii[0],r[2]/radii[1]) for r in rings)
    rings = [(z,rx/scale,ry/scale) for z,rx,ry in rings]
    crown = surface(rings,oid,center,texture_base)
    trunk_radius = min(min(radii)*.15, max(.06,height*.022))
    trunk_top = min(height*.75,canopy_low-base+crown_height*.4)
    trunk = surface([(0,0,0),(0,trunk_radius,trunk_radius),
                     (trunk_top,trunk_radius*.6,trunk_radius*.6),(trunk_top,0,0)],
                    oid,center,texture_base+1,12)
    report = dict(position=center.tolist(),height_m=float(height),crown_size_m=[*map(float,extent[:2]),float(crown_height)],
                  captured_bounds=[low.tolist(),high.tolist()],ground_inferred=grounded,shape=shape,
                  triangles=sum(len(m.vertices)//3 for m in (crown,trunk)))
    return [crown,trunk],report


def apply(batches, target=None, shape='fitted', use_ground=True, progress=lambda *args:None, reporter=None):
    from refine_progress import Reporter
    reporter = reporter or Reporter(progress)
    records = [];done = 0
    targets = [(bi,gi,oi) for bi,b in enumerate(batches) if not b.is_spline
               for gi,g in enumerate(b.groups) for oi,o in enumerate(g.objects)
               if o.cls == TREE and (target is None or target == (bi,gi,oi))]
    if target is not None and not targets: raise ValueError('selected target is not a segmented tree')
    palettes = palette();grounds = {}
    for count,(bi,gi,oid) in enumerate(targets):
        group = batches[bi].groups[gi];name = group.objects[oid].name
        reporter.begin(name,len(targets),'Fitting procedural trunk and crown')
        original = [m for m in group.meshes if m.object_id == oid and m.model_variant == 1]
        source = original or [m for m in group.meshes if m.object_id == oid and m.model_variant != 2]
        key = (bi,gi)
        if use_ground and key not in grounds: grounds[key] = GroundSamples(group)
        existing = next((i for i in range(len(group.textures)-1) if group.textures[i:i+2] == palettes),None)
        tex = existing if existing is not None else len(group.textures)
        try:
            new,report = build(source,oid,tex,shape,grounds.get(key))
        except ValueError as error:
            records.append(dict(target=[bi,gi,oid],name=name,status='unchanged',reason=str(error)))
            reporter.finish(False,str(error))
            continue
        if not original:
            original = [copy.copy(m) for m in source]
            for m in original: m.model_variant = 1
        if existing is None: group.textures.extend(palettes)
        group.meshes = [m for m in group.meshes if m.object_id != oid]+original+new
        records.append(dict(target=[bi,gi,oid],name=name,status='replaced',**report));done += 1
        reporter.finish(True)
    return done,len(targets),records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--target');parser.add_argument('--shape',choices=['fitted','round','conifer'],default='fitted')
    parser.add_argument('--no-ground',action='store_true');parser.add_argument('--progress',action='store_true')
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve() or (args.output.exists() and args.input.samefile(args.output)):
        parser.error('input and output must differ; preserve the original capture')
    target = None
    if args.target:
        try:
            target = tuple(map(int,args.target.split(':')))
            if len(target) != 3 or min(target) < 0: raise ValueError()
        except ValueError: parser.error('--target must be nonnegative batch:group:object indices')
    def progress(done,total,message):
        if args.progress: print(f'@progress {done} {total} {message}',flush=True)
    scene = mtscene.load(args.input)
    from refine_progress import Reporter
    reporter = Reporter(progress,args.progress)
    try: done,total,records = apply(scene,target,args.shape,not args.no_ground,progress,reporter)
    except ValueError as error: parser.error(str(error))
    reporter.update(phase='Saving paired tree scene')
    mtscene.save(args.output,scene)
    args.output.with_suffix('.trees.json').write_text(json.dumps(dict(replaced=done,total=total,trees=records),indent=2),encoding='utf-8')
    progress(1,1,f'Replaced {done} of {total} trees; originals preserved')
    print(f'Refined {done} of {total} trees with procedural shapes; originals preserved; saved {args.output}',flush=True)
    return 0


if __name__ == '__main__': raise SystemExit(main())
