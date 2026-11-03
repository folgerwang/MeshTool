"""Blender side of blender_refine: clean captured building meshes in place.

Run by blender_refine.py as
    blender -b --factory-startup --python blender_cleanup.py -- JOBDIR
JOBDIR holds one NPZ per building (pos float[T,3,3] in metres about the
building centre, uv float[T,3,2], src int[T] source mesh, bad int[T] faces the
driver flagged: 1 smeared, 2 underground) and gets one *_out.npz back with the same arrays
for the cleaned triangles plus a stats JSON.

The captured surface is kept: no face is moved and no texture is changed.
Only obvious capture artifacts go: smeared faces, faces buried under the
ground, degenerate slivers and small floating fragments. Holes left by removed faces are closed only where
they are small and lie inside a single source mesh, so the fill can reuse
that mesh's texture coordinates.
"""
import json
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np

WELD = .02            # metres: GE tile seams are this close
SLIVER = .01          # metres: faces thinner than this are degenerate
ISLAND_AREA = 2.0     # m2: loose parts smaller than this are fragments
HOLE_EDGES = 12       # boundary loops up to this many edges are closed...
HOLE_PERIMETER = 8.0  # ...when also shorter than this many metres


def build(data):
    pos, uv, src, bad = data['pos'], data['uv'], data['src'], data['bad']
    bm = bmesh.new()
    s_layer = bm.faces.layers.int.new('src')
    b_layer = bm.faces.layers.int.new('bad')
    uv_layer = bm.loops.layers.uv.new('UVMap')
    verts = [bm.verts.new(p) for p in pos.reshape(-1, 3)]
    for t in range(len(pos)):
        try:
            f = bm.faces.new(verts[3*t:3*t+3])
        except ValueError:
            continue
        f[s_layer] = int(src[t]); f[b_layer] = int(bad[t])
        for loop, k in zip(f.loops, range(3)):
            loop[uv_layer].uv = uv[t, k]
    return bm, s_layer, b_layer, uv_layer


def islands(bm):
    seen, out = set(), []
    for f in bm.faces:
        if f in seen:
            continue
        stack, part = [f], []
        seen.add(f)
        while stack:
            g = stack.pop(); part.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h not in seen:
                        seen.add(h); stack.append(h)
        out.append(part)
    return out


def close_holes(bm, s_layer, uv_layer, stats):
    boundary = {e for e in bm.edges if e.is_boundary}
    loops, used = [], set()
    for e in boundary:
        if e in used:
            continue
        # Walk one boundary loop.
        chain, v, cur = [e], e.verts[1], e
        used.add(e)
        ok = True
        while v is not e.verts[0]:
            nxt = [x for x in v.link_edges if x in boundary and x not in used]
            if len(nxt) != 1 or len(chain) > HOLE_EDGES:
                ok = False; break
            cur = nxt[0]; used.add(cur); chain.append(cur); v = cur.other_vert(v)
        if ok and 3 <= len(chain) <= HOLE_EDGES:
            loops.append(chain)
    for chain in loops:
        if sum(e.calc_length() for e in chain) > HOLE_PERIMETER:
            continue
        sources = {f[s_layer] for e in chain for f in e.link_faces}
        if len(sources) != 1:
            continue
        # UV of each rim vertex from the source face around it.
        rim = {}
        for e in chain:
            for f in e.link_faces:
                for loop in f.loops:
                    if loop.vert in e.verts:
                        rim.setdefault(loop.vert, loop[uv_layer].uv.copy())
        result = bmesh.ops.holes_fill(bm, edges=chain, sides=HOLE_EDGES)
        for f in result['faces']:
            f[s_layer] = next(iter(sources))
            for loop in f.loops:
                if loop.vert in rim:
                    loop[uv_layer].uv = rim[loop.vert]
            stats['filled'] += 1


def clean(data):
    bm, s_layer, b_layer, uv_layer = build(data)
    stats = dict(input=len(bm.faces), smear=0, sliver=0, fragments=0, fragment_area=0., filled=0)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=WELD)
    # Flags from the driver: 1 smeared texture, 2 under the captured ground.
    flagged = [f for f in bm.faces if f[b_layer]]
    stats['smear'] = sum(f[b_layer] == 1 for f in flagged)
    stats['underground'] = sum(f[b_layer] == 2 for f in flagged)
    bmesh.ops.delete(bm, geom=flagged, context='FACES')
    slivers = []
    for f in bm.faces:
        longest = max(e.calc_length() for e in f.edges)
        if longest <= 1e-6 or 2*f.calc_area()/longest < SLIVER:
            slivers.append(f)
    stats['sliver'] = len(slivers)
    bmesh.ops.delete(bm, geom=slivers, context='FACES')
    for part in islands(bm):
        area = sum(f.calc_area() for f in part)
        if area < ISLAND_AREA:
            stats['fragments'] += 1; stats['fragment_area'] += area
            bmesh.ops.delete(bm, geom=part, context='FACES')
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    close_holes(bm, s_layer, uv_layer, stats)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    pos = np.array([[v.co[:] for v in f.verts] for f in bm.faces], np.float64).reshape(-1, 3, 3)
    uv = np.array([[loop[uv_layer].uv[:] for loop in f.loops] for f in bm.faces], np.float64).reshape(-1, 3, 2)
    src = np.array([f[s_layer] for f in bm.faces], np.int32)
    stats['output'] = len(src)
    bm.free()
    return dict(pos=pos, uv=uv, src=src), stats


def main():
    job = Path(sys.argv[sys.argv.index('--')+1])
    report = {}
    for path in sorted(job.glob('*_in.npz')):
        name = path.name[:-len('_in.npz')]
        out, stats = clean(dict(np.load(path)))
        np.savez(job/f'{name}_out.npz', **out)
        report[name] = stats
        print(f'blender_cleanup {name}: {stats}', flush=True)
    (job/'report.json').write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
