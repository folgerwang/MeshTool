"""Blender side of blender_refine: clean captured building meshes in place and
rebuild their windows.

Run by blender_refine.py as
    blender -b --factory-startup --python blender_cleanup.py -- JOBDIR
JOBDIR holds one NPZ per building (pos float[T,3,3] in metres about the
building centre, uv float[T,3,2], src int[T] source mesh, bad int[T] faces the
driver flagged: 1 smeared, 2 underground) and optionally *_glass.json (wall
planes with their pane rectangles, captured_glass.detect). It gets one
*_out.npz back with the same arrays for the result, plus mat int[T] (0 wall,
1 glass, 2 frame, 3 reveal, 4 dark backing) and plane int[T] (window plane, -1 if none), and
a stats JSON.

The captured surface is kept and no texture is changed. Only obvious capture
artifacts go: smeared faces, faces buried under the ground, degenerate
slivers and small floating fragments. Holes left by removed faces are closed
only where they are small and lie inside a single source mesh, so the fill
can reuse that mesh's texture coordinates.

Windows are rebuilt in place: the captured faces inside each window's frame
ring are replaced by a reveal into the wall, a frame ring and a pane set back
behind it. Their UVs are plane metres; the driver maps them to the wall's
rectified capture, so they look as captured, now with depth. The pane gets
the viewer's glass material.
"""
import json
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector

WELD = .02            # metres: GE tile seams are this close
SLIVER = .01          # metres: faces thinner than this are degenerate
ISLAND_AREA = 2.0     # m2: loose parts smaller than this are fragments
HOLE_EDGES = 12       # boundary loops up to this many edges are closed...
HOLE_PERIMETER = 8.0  # ...when also shorter than this many metres
NEAR = .8             # metres: faces this close to a wall plane carry its windows
FRAME = .08           # captured frame ring around each pane (m), as captured_glass.FRAME
REVEAL = .10          # the window opening is set back this far into the wall (m)
PANE_SETBACK = .05    # and the pane this much further, behind its frame (m)
CORNER_REACH = .4     # metres: wall vertices this close set a window corner's depth
CORNER_SPREAD = .15   # and it stays this close to the window's own depth (m)
FLANGE = .03          # metres the reveal laps over the wall around an opening
CELL = 2.0            # metres: plane grid used to find the faces near a pane
BACKING_DEPTH = .35   # metres behind the wall
BACKING_GROW = .2     # metres past the opening on each side
WALL, GLASS, FRAME_FACE, REVEAL_FACE, BACKING = 0, 1, 2, 3, 4   # 'mat' face attribute


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


class PlaneGrid:
    """Faces of one wall plane bucketed by plane coordinates, kept up to date
    while cuts replace faces."""

    def __init__(self, o, u, v):
        self.o, self.u, self.v, self.cells, self.where = o, u, v, {}, {}

    def add(self, f):
        s = [(p.co-self.o).dot(self.u) for p in f.verts]
        t = [(p.co-self.o).dot(self.v) for p in f.verts]
        keys = [(i, j) for i in range(int(min(s)//CELL), int(max(s)//CELL)+1)
                for j in range(int(min(t)//CELL), int(max(t)//CELL)+1)]
        for k in keys:
            self.cells.setdefault(k, set()).add(f)
        self.where[f] = keys

    def remove(self, f):
        for k in self.where.pop(f, ()):
            self.cells[k].discard(f)

    def near(self, s0, t0, s1, t1):
        out = set()
        for i in range(int(s0//CELL), int(s1//CELL)+1):
            for j in range(int(t0//CELL), int(t1//CELL)+1):
                out |= self.cells.get((i, j), set())
        return [f for f in out if f.is_valid]


def plane_xy(p, o, u, v):
    q = p-o
    return q.dot(u), q.dot(v)


def overlaps(f, o, u, v, rect, eps=1e-3):
    xy = [plane_xy(p.co, o, u, v) for p in f.verts]
    return (min(x for x, _ in xy) < rect[2]-eps and max(x for x, _ in xy) > rect[0]+eps and
            min(y for _, y in xy) < rect[3]-eps and max(y for _, y in xy) > rect[1]+eps)


def add_face(bm, verts, xys, src, mat, pid, toward, layers):
    """New face whose UVs are plane metres (the driver maps them to the plane's
    baked capture), its normal turned toward 'toward'."""
    s_layer, m_layer, p_layer, uv_layer = layers
    try:
        f = bm.faces.new(verts)
    except ValueError:
        print('blender_cleanup: window face not built', len(verts), flush=True)
        return None
    f.normal_update()
    if f.normal.dot(toward) < 0:
        f.normal_flip()
    corner = dict(zip(verts, xys))
    for loop in f.loops:
        loop[uv_layer].uv = corner[loop.vert]
    f[s_layer], f[m_layer], f[p_layer] = src, mat, pid
    return f


def inset(corner, rect, d=FRAME/2):
    """A point of a rectangle's outline moved d toward its inside."""
    return (corner[0]+(d if corner[0] <= rect[0]+1e-6 else -d), corner[1]+(d if corner[1] <= rect[1]+1e-6 else -d))


def outside(corner, rect, d=FRAME/2):
    """A pane corner moved d out into its frame."""
    return (corner[0]+(-d if corner[0] <= rect[0]+1e-6 else d), corner[1]+(-d if corner[1] <= rect[1]+1e-6 else d))


def rebuild_window(bm, grid, pid, o, u, v, n, panes, outer, layers):
    """Replace the captured faces inside a window's opening with a built
    window: reveal into the wall, frame with its mullions, panes set back
    behind the frame. Returns False where the capture does not cover it."""
    s_layer, m_layer, p_layer, uv_layer = layers
    faces = [f for f in grid.near(*outer) if overlaps(f, o, u, v, outer)]
    if not faces:
        return False
    # Cut along the four outer edges only.
    for axis, value in ((u, outer[0]), (u, outer[2]), (v, outer[1]), (v, outer[3])):
        cross = [f for f in faces if min((p.co-o).dot(axis) for p in f.verts) < value-1e-4
                 and max((p.co-o).dot(axis) for p in f.verts) > value+1e-4]
        if not cross:
            continue
        geom = cross+list({e for f in cross for e in f.edges})+list({p for f in cross for p in f.verts})
        result = bmesh.ops.bisect_plane(bm, geom=geom, plane_co=o+axis*value, plane_no=axis, dist=1e-5)
        for f in cross:
            grid.remove(f)
        new = [g for g in result['geom'] if isinstance(g, bmesh.types.BMFace) and g.is_valid]
        for f in new:
            if f not in grid.where:
                f[p_layer] = pid
                grid.add(f)
        faces = [f for f in set(faces) | set(new) if f.is_valid and overlaps(f, o, u, v, outer)]
    inside = []
    for f in faces:
        x, y = plane_xy(f.calc_center_median(), o, u, v)
        if outer[0] < x < outer[2] and outer[1] < y < outer[3]:
            inside.append(f)
    if sum(f.calc_area() for f in inside) < .85*(outer[2]-outer[0])*(outer[3]-outer[1]):
        return False
    # The window takes the source mesh (level of detail) that covered it most.
    share = {}
    for f in inside:
        share[f[s_layer]] = share.get(f[s_layer], 0)+f.calc_area()
    src = max(share, key=share.get)
    # Every vertex on the frame edge joins the reveal: also those whose outer
    # neighbour is a face off this plane, or there would be a gap beside it.
    rim_verts = {p for f in faces if f.is_valid for p in f.verts}
    for f in inside:
        grid.remove(f)
    bmesh.ops.delete(bm, geom=inside, context='FACES_ONLY')
    # The wall stays as captured; the window sits at its local depth, measured
    # on the vertices around the opening (a ledge vertex must not set it).
    eps = 2e-3
    rim = []
    for vert in rim_verts:
        if not vert.is_valid:
            continue
        x, y = plane_xy(vert.co, o, u, v)
        if outer[0]-eps <= x <= outer[2]+eps and outer[1]-eps <= y <= outer[3]+eps and                 min(abs(x-outer[0]), abs(x-outer[2]), abs(y-outer[1]), abs(y-outer[3])) < eps:
            rim.append((x, y, -(vert.co-o).dot(n)))
    base = float(np.median([r[2] for r in rim])) if rim else 0.

    def front_depth(x, y):
        near = [r[2] for r in rim if abs(r[0]-x)+abs(r[1]-y) < CORNER_REACH]
        depth = float(np.median(near)) if near else base
        return min(base+CORNER_SPREAD, max(base-CORNER_SPREAD, depth))
    x0, y0, x1, y1 = outer
    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]

    def point(x, y, depth):
        return o+u*x+v*y-n*depth

    center = point((outer[0]+outer[2])/2, (outer[1]+outer[3])/2, 0.)
    # Reveal: one quad per side from the wall opening back to the
    # recessed frame. Captures overlap in layers, so the opening's vertices do
    # not form one clean loop to stitch to; its rectangle closes it regardless.
    # The reveal's mouth reaches a little past the opening and in front of the
    # wall, so it laps over the captured edge instead of leaving a crack.
    cx, cy = (x0+x1)/2, (y0+y1)/2
    front = [bm.verts.new(point(x+FLANGE*np.sign(x-cx), y+FLANGE*np.sign(y-cy), front_depth(x, y)-FLANGE))
             for x, y in corners]
    back = [bm.verts.new(point(x, y, base+REVEAL)) for x, y in corners]
    for k in range(4):
        j = (k+1) % 4
        mid = ((corners[k][0]+corners[j][0])/2, (corners[k][1]+corners[j][1])/2)
        # Textured with the captured frame band just inside the opening: an
        # edge-on texel line would draw the dark pane edge down every reveal.
        inward = [inset(corners[k], outer), inset(corners[j], outer)]
        add_face(bm, [front[k], front[j], back[j], back[k]], [corners[k], corners[j], inward[1], inward[0]],
                 src, REVEAL_FACE, pid, center-point(*mid, 0.), layers)
    # The frame at the reveal depth: the opening minus its panes (ring and
    # mullions), on a grid of the pane edges so every edge is shared and no
    # crack shows the backing. Then each pane is set back behind the frame.
    xs = sorted({x0, x1} | {p[0] for p in panes} | {p[2] for p in panes})
    ys = sorted({y0, y1} | {p[1] for p in panes} | {p[3] for p in panes})
    level = {}

    def at(x, y):
        key = (round(x, 5), round(y, 5))
        if key not in level:
            level[key] = bm.verts.new(point(x, y, base+REVEAL))
        return level[key]
    for k, (x, y) in enumerate(corners):
        level[(round(x, 5), round(y, 5))] = back[k]
    for i in range(len(xs)-1):
        for j in range(len(ys)-1):
            cx, cy = (xs[i]+xs[i+1])/2, (ys[j]+ys[j+1])/2
            if any(p[0] < cx < p[2] and p[1] < cy < p[3] for p in panes):
                continue
            xy = [(xs[i], ys[j]), (xs[i+1], ys[j]), (xs[i+1], ys[j+1]), (xs[i], ys[j+1])]
            add_face(bm, [at(*c) for c in xy], xy, src, FRAME_FACE, pid, n, layers)
    for pane in panes:
        px0, py0, px1, py1 = pane
        inner_xy = [(px0, py0), (px1, py0), (px1, py1), (px0, py1)]
        # Frame-level vertices along each pane edge (a grid line may split it).
        glass_verts = [bm.verts.new(point(x, y, base+REVEAL+PANE_SETBACK)) for x, y in inner_xy]
        pane_center = point((px0+px1)/2, (py0+py1)/2, base+REVEAL)
        for k in range(4):
            j = (k+1) % 4
            a, b = inner_xy[k], inner_xy[j]
            if abs(a[1]-b[1]) < 1e-9:
                edge = [(x, a[1]) for x in xs if min(a[0], b[0])-1e-6 <= x <= max(a[0], b[0])+1e-6]
            else:
                edge = [(a[0], y) for y in ys if min(a[1], b[1])-1e-6 <= y <= max(a[1], b[1])+1e-6]
            edge.sort(key=lambda c: (c[0]-a[0])*(b[0]-a[0])+(c[1]-a[1])*(b[1]-a[1]))
            mid = ((a[0]+b[0])/2, (a[1]+b[1])/2)
            add_face(bm, [at(*c) for c in edge]+[glass_verts[j], glass_verts[k]],
                     edge+[outside(b, pane), outside(a, pane)], src, FRAME_FACE, pid,
                     pane_center-point(*mid, base+REVEAL), layers)
        add_face(bm, glass_verts, inner_xy, src, GLASS, pid, n, layers)
    # Dark backing wider than the opening: where the wall bulges past the
    # window's rectangle a crack reads as shadow, not as the ground behind.
    grown = [(x0-BACKING_GROW, y0-BACKING_GROW), (x1+BACKING_GROW, y0-BACKING_GROW),
             (x1+BACKING_GROW, y1+BACKING_GROW), (x0-BACKING_GROW, y1+BACKING_GROW)]
    add_face(bm, [bm.verts.new(point(x, y, base+BACKING_DEPTH)) for x, y in grown], grown, src, BACKING, pid, n, layers)
    return True


def openings(rects):
    """Group panes whose frame rectangles touch into one window opening:
    [(panes, outer rectangle)]."""
    parent = list(range(len(rects)))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for i, a in enumerate(rects):
        for j in range(i):
            b = rects[j]
            if a[4] <= b[6]+1e-3 and b[4] <= a[6]+1e-3 and a[5] <= b[7]+1e-3 and b[5] <= a[7]+1e-3:
                parent[root(i)] = root(j)
    groups = {}
    for i, r in enumerate(rects):
        groups.setdefault(root(i), []).append(r)
    out = []
    for members in groups.values():
        outer = (min(r[4] for r in members), min(r[5] for r in members), max(r[6] for r in members), max(r[7] for r in members))
        panes = [tuple(r[:4]) for r in members]
        # A ragged group would leave captured wall inside its box; keep those
        # panes as separate windows.
        if sum((r[6]-r[4])*(r[7]-r[5]) for r in members) < .9*(outer[2]-outer[0])*(outer[3]-outer[1]):
            out += [([tuple(r[:4])], tuple(r[4:8])) for r in members]
        else:
            out.append((panes, outer))
    return out


def windows(bm, planes, layers, stats):
    s_layer, m_layer, p_layer, uv_layer = layers
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    faces = list(bm.faces)
    if not faces or not planes:
        return
    centers = np.array([f.calc_center_median()[:] for f in faces])
    normals = np.array([f.normal[:] for f in faces])
    for pid, plane in enumerate(planes):
        o, u, v, n = (Vector(plane[k]) for k in ('origin', 'u', 'v', 'n'))
        su, sv = plane['size']
        q = centers-np.array(o[:])
        s, t, d = q@np.array(u[:]), q@np.array(v[:]), q@np.array(n[:])
        pick = np.flatnonzero((np.abs(normals@np.array(n[:])) > .8) & (np.abs(d) < NEAR) &
                              (s > -1) & (s < su+1) & (t > -1) & (t < sv+1))
        grid = PlaneGrid(o, u, v)
        for i in pick:
            f = faces[i]
            if f.is_valid and f[p_layer] < 0:
                f[p_layer] = pid
                grid.add(f)
        if not grid.where:
            continue
        built = 0
        for panes, outer in openings(plane['rects']):
            if rebuild_window(bm, grid, pid, o, u, v, n, panes, outer, layers):
                built += len(panes)
        stats['panes'] += built
        stats['window_planes'] += built > 0
        merge_pieces(bm, list(grid.where), m_layer)
    bmesh.ops.delete(bm, geom=[e for e in bm.edges if not e.link_faces], context='EDGES')
    bmesh.ops.delete(bm, geom=[p for p in bm.verts if not p.link_faces], context='VERTS')


def merge_pieces(bm, faces, m_layer):
    """Join the coplanar wall pieces the cuts left back into larger faces,
    never across a texture seam or a change of source mesh."""
    s_layer = bm.faces.layers.int['src']
    faces = {f for f in faces if f.is_valid and f[m_layer] == WALL}
    edges = [e for e in {e for f in faces for e in f.edges} if len(e.link_faces) == 2 and
             all(g in faces for g in e.link_faces) and
             e.link_faces[0][s_layer] == e.link_faces[1][s_layer]]
    if edges:
        bmesh.ops.dissolve_limit(bm, angle_limit=.002, use_dissolve_boundaries=False, verts=[], edges=edges,
                                 delimit={'UV'})


def clean(data, planes=None):
    bm, s_layer, b_layer, uv_layer = build(data)
    m_layer = bm.faces.layers.int.new('mat')
    p_layer = bm.faces.layers.int.new('plane')
    for f in bm.faces:
        f[p_layer] = -1
    stats = dict(input=len(bm.faces), smear=0, sliver=0, fragments=0, fragment_area=0., filled=0,
                 window_planes=0, panes=0)
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
    stats['open_edges_before'] = sum(e.is_boundary for e in bm.edges)
    windows(bm, planes or [], (s_layer, m_layer, p_layer, uv_layer), stats)
    stats['open_edges_after'] = sum(e.is_boundary for e in bm.edges)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    pos = np.array([[v.co[:] for v in f.verts] for f in bm.faces], np.float64).reshape(-1, 3, 3)
    uv = np.array([[loop[uv_layer].uv[:] for loop in f.loops] for f in bm.faces], np.float64).reshape(-1, 3, 2)
    src = np.array([f[s_layer] for f in bm.faces], np.int32)
    mat = np.array([f[m_layer] for f in bm.faces], np.int32)
    plane = np.array([f[p_layer] for f in bm.faces], np.int32)
    stats['output'] = len(src)
    bm.free()
    return dict(pos=pos, uv=uv, src=src, mat=mat, plane=plane), stats


def main():
    job = Path(sys.argv[sys.argv.index('--')+1])
    report = {}
    for path in sorted(job.glob('*_in.npz')):
        name = path.name[:-len('_in.npz')]
        glass = job/f'{name}_glass.json'
        planes = json.loads(glass.read_text()) if glass.exists() else None
        out, stats = clean(dict(np.load(path)), planes)
        np.savez(job/f'{name}_out.npz', **out)
        report[name] = stats
        print(f'blender_cleanup {name}: {stats}', flush=True)
    (job/'report.json').write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
