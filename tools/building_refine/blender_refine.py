"""Clean captured buildings with Blender, keeping the captured look.

    python blender_refine.py IN.mtscene OUT.mtscene [--only NAME ...] [--blender G:/blender/blender.exe]

Unlike the PCG facade (facade.py) nothing is regenerated: every kept face is
the captured face with its captured texture, at the capture's finest level of
detail. Blender (blender_cleanup.py) welds tile seams and removes the obvious
capture artifacts: smeared faces whose texture collapses to a line, slivers,
and small floating fragments, and closes the small holes this leaves.

Windows: captured_glass finds the panes on every wall plane of the capture;
Blender replaces each window's captured faces with a reveal, a frame ring and
a pane set back behind it, textured from the wall's rectified capture. Panes
get the viewer's glass material with a dark interior behind them. The cleaned building is stored as the
refined variant (2) with the capture archived as the original (1), so the
viewer can still compare both.
"""
import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

import captured_facade
import captured_glass
import mtscene

BLENDER = 'G:/blender/blender.exe'
SMEAR_AREA = .05      # m2: smaller faces are not visible as smears
SMEAR_DENSITY = .02   # texel density below this share of the building median is a smear
BURIED = .5           # metres: faces wholly this far below the captured ground are hidden fragments
GROUND_CLASSES = (1, 2, 6, 7)
GLASS, BACKING = 1, 4 # blender_cleanup 'mat' of pane and backing faces
SMEARED, UNDERGROUND = 1, 2   # face flags handed to blender_cleanup


def triangles(meshes, textures):
    pos, uv, src, density = [], [], [], []
    for k, m in enumerate(meshes):
        w, h = textures[m.tex_index].mips[0][:2] if m.uvs is not None else (0, 0)
        for prim, idx in m.draw_calls:
            if prim != 4:
                continue
            i = idx.reshape(-1, 3)
            p = m.vertices[i].astype(np.float64)+m.translation
            t = m.uvs[i].astype(np.float64) if m.uvs is not None else np.zeros(i.shape+(2,))
            area = np.linalg.norm(np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]), axis=1)/2
            e1, e2 = t[:, 1]-t[:, 0], t[:, 2]-t[:, 0]
            texels = np.abs(e1[:, 0]*e2[:, 1]-e1[:, 1]*e2[:, 0])/2*w*h
            pos.append(p); uv.append(t); src.append(np.full(len(i), k, np.int32))
            density.append(texels/np.maximum(area, 1e-9))
            density[-1][area <= SMEAR_AREA] = np.inf
    if not pos:
        return None
    pos, uv, src, density = map(np.concatenate, (pos, uv, src, density))
    finite = density[np.isfinite(density)]
    median = np.median(finite) if len(finite) else 0
    return dict(pos=pos, uv=uv, src=src, bad=np.where(density < SMEAR_DENSITY*median, SMEARED, 0).astype(np.int32))


class Ground:
    """Captured ground height (terrain and road objects) around a point."""

    def __init__(self, group):
        from scipy.spatial import cKDTree
        pts = [m.vertices.astype(np.float64)+m.translation for m in group.meshes
               if 0 <= m.object_id < len(group.objects) and group.objects[m.object_id].cls in GROUND_CLASSES
               and m.model_variant != 2]
        self.points = np.concatenate(pts) if pts else np.zeros((0, 3))
        self.tree = cKDTree(self.points[:, :2]) if len(self.points) >= 8 else None

    def below(self, pos):
        """Faces whose highest corner lies under the ground next to them."""
        if self.tree is None:
            return np.zeros(len(pos), bool)
        dist, near = self.tree.query(pos.mean(1)[:, :2], k=8, distance_upper_bound=3.)
        found = np.isfinite(dist).all(1)
        z = np.full(len(pos), -np.inf)
        z[found] = np.median(self.points[near[found], 2], 1)
        return pos[:, :, 2].max(1) < z-BURIED


def rebuild(meshes, out, center, planes=(), plane_textures=()):
    """Refined meshes: the captured faces per source mesh, and per wall plane
    and level of detail the built windows (frame, reveal, glass pane and the
    interior backing behind it) on the plane's baked capture."""
    pos, uv = out['pos'], out['uv']
    mat = out['mat'] if 'mat' in out else np.zeros(len(pos), np.int32)
    plane = out['plane'] if 'plane' in out else np.full(len(pos), -1)

    def emit(template, p, t, tex_index, material):
        n = len(p)
        mesh = copy.copy(template)
        mesh.tex_index = tex_index
        mesh.vertices = (p.reshape(-1, 3)+center-template.translation).astype(np.float32)
        mesh.uvs = t.reshape(-1, 2).astype(np.float32) if template.uvs is not None else None
        mesh.colors = None if template.colors is None else np.repeat(template.colors[:1], 3*n)
        mesh.draw_calls = [(4, np.arange(3*n, dtype=np.uint32))]
        mesh.material = material
        mesh.model_variant = 2
        return mesh
    new = []
    for k, m in enumerate(meshes):
        sel = (out['src'] == k) & (mat == 0)
        if sel.any():
            new.append(emit(m, pos[sel], uv[sel], m.tex_index, m.material))
    built = mat != 0
    for pid, f in enumerate(planes):
        tex_index, scale = plane_textures[pid]
        here = built & (plane == pid)
        if not here.any():
            continue
        size = np.array(f['image_size'])
        mapped = np.stack([uv[..., 0]/size[0]*scale[0], (1-uv[..., 1]/size[1])*scale[1]], -1)
        lods = np.array([meshes[k].lod_size for k in out['src']])
        for lod in np.unique(lods[here]):
            part = here & (lods == lod)
            # A template at this level of detail keeps the windows in depth order with their wall.
            template = meshes[out['src'][np.flatnonzero(part)[0]]]
            glass = part & (mat == GLASS)
            backing = part & (mat == BACKING)
            other = part & (mat != GLASS) & (mat != BACKING)
            if other.any():
                new.append(emit(template, pos[other], mapped[other], tex_index, mtscene.MAT_CAPTURED))
            if glass.any():
                new.append(emit(template, pos[glass], mapped[glass], tex_index, mtscene.MAT_GLASS))
            if backing.any():
                new.append(emit(template, pos[backing], mapped[backing], tex_index, mtscene.MAT_INTERIOR))
    return new


def find_panes(source, textures, data, center):
    """Wall planes with their panes (captured_glass), in building-centred
    metres, and each plane's rectified capture."""
    keep = data['bad'] == 0
    planes = [captured_glass.frame(p, data['pos'][keep]) for p in captured_glass.wall_planes(data['pos'][keep])]
    cap = captured_glass.capture(source, textures)
    if cap is None:
        return []
    return [dict(origin=list(map(float, f['origin'])), u=list(map(float, f['u'])), v=list(map(float, f['v'])),
                 n=list(map(float, f['n'])), size=list(f['size']), rects=[list(map(float, r)) for r in f['rects']],
                 image=f['image'], image_size=[f['image'].shape[1]*f['pitch'], f['image'].shape[0]*f['pitch']])
            for f in captured_glass.detect(cap, planes, center)]


def run(batches, only=None, blender=BLENDER, job=None, log=print, windows=True):
    jobs = []
    work = Path(job or tempfile.mkdtemp(prefix='blender_refine_'))
    work.mkdir(parents=True, exist_ok=True)
    for bi, batch in enumerate(batches):
        if batch.is_spline:
            continue
        for gi, group in enumerate(batch.groups):
            ground = None
            for oid, obj in enumerate(group.objects):
                if obj.cls != 3 or (only and obj.name not in only and str(oid) not in only):
                    continue
                original = [m for m in group.meshes if m.object_id == oid and m.model_variant == 1]
                source = original or [m for m in group.meshes if m.object_id == oid and m.material in (0, 1, 2, 3)]
                data = triangles(source, group.textures)
                if data is None:
                    continue
                ground = ground or Ground(group)
                data['bad'][(data['bad'] == 0) & ground.below(data['pos'])] = UNDERGROUND
                center = data['pos'].reshape(-1, 3).mean(0)
                data['pos'] = data['pos']-center
                name = f'{bi}_{gi}_{oid}'
                np.savez(work/f'{name}_in.npz', **data)
                planes = find_panes(source, group.textures, data, center) if windows else []
                (work/f'{name}_glass.json').write_text(json.dumps(
                    [{k: v for k, v in f.items() if k != 'image'} for f in planes]))
                jobs.append((name, group, oid, source, bool(original), center, planes))
    if not jobs:
        return {}
    log(f'blender_refine: cleaning {len(jobs)} building(s) in {work}')
    subprocess.run([blender, '-b', '--factory-startup', '--python', str(Path(__file__).with_name('blender_cleanup.py')),
                    '--', str(work)], check=True, stdout=subprocess.DEVNULL if log is None else None)
    report = json.loads((work/'report.json').read_text())
    for name, group, oid, source, archived, center, planes in jobs:
        out = dict(np.load(work/f'{name}_out.npz'))
        plane_textures = []
        for f in planes:
            texture, scale = captured_facade._texture(f['image'])
            plane_textures.append((len(group.textures), scale))
            group.textures.append(texture)
        new = rebuild(source, out, center, planes, plane_textures)
        original = source if archived else [copy.copy(m) for m in source]
        for m in original:
            m.model_variant = 1
        group.meshes = [m for m in group.meshes if m.object_id != oid]+original+new
        s = report[name]
        log(f'  {group.objects[oid].name}: {s["input"]} -> {s["output"]} faces; removed {s["smear"]} smeared, {s["underground"]} underground, '
            f'{s["sliver"]} sliver, {s["fragments"]} fragment(s) ({s["fragment_area"]:.1f} m2); closed {s["filled"]} hole face(s); '
            f'rebuilt {s["panes"]} window pane(s) with frames on {s["window_planes"]} wall plane(s)')
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('input'); ap.add_argument('output')
    ap.add_argument('--only', nargs='*', help='object names or indices')
    ap.add_argument('--blender', default=BLENDER)
    ap.add_argument('--job', help='keep the Blender exchange files here')
    ap.add_argument('--no-windows', action='store_true', help='clean only; keep the captured windows flat')
    a = ap.parse_args()
    batches = mtscene.load(a.input, {})
    run(batches, set(a.only) if a.only else None, a.blender, a.job, windows=not a.no_windows)
    mtscene.save(a.output, batches, {})


if __name__ == '__main__':
    sys.exit(main())
