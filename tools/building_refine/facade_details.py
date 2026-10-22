"""Add inferred architectural trim to an existing, validated facade catalog.

The approved shell, window slots, materials and roof remain byte-for-byte intact.
Trim is clipped to actual wall triangles and kept clear of every opening/detail.
"""
import argparse
import copy
import hashlib
import json
import shutil
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
from shapely.geometry import Polygon, box
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

import mtscene
import raster
from building_rules import extrusion
from compose_iteration import fingerprint
from install_building_repairs import planarity
from solid_city import audit_shell
from validate_city_iteration import openings as validate_openings


def polygons(geometry):
    if geometry.is_empty:
        return
    if geometry.geom_type == 'Polygon':
        yield geometry
    elif hasattr(geometry, 'geoms'):
        for part in geometry.geoms:
            yield from polygons(part)


def face_details(boundary, instances, size):
    """Metric trim footprints; never bridge a window, courtyard or facade edge."""
    windows = [i for i in instances if i.get('kind') == 'window']
    if not windows:
        return []
    occupied = [box(*i['bounds']).buffer(.025, join_style=2)
                for i in instances if i.get('kind') and 'bounds' in i]
    # Keep the corner setback larger than the embedded back depth so adjacent
    # perpendicular facades cannot produce coincident trim edges.
    available = boundary.buffer(-.04, join_style=2).difference(unary_union(occupied))
    result = []

    def add(role, footprint, depth):
        nonlocal available
        accepted = []
        for polygon in polygons(footprint.intersection(available)):
            if polygon.area < .008 or min(np.ptp(np.asarray(polygon.exterior.coords), axis=0)) < .015:
                continue
            result.append((role, polygon, depth))
            accepted.append(polygon)
        if accepted:
            available = available.difference(unary_union(accepted).buffer(.012, join_style=2))

    rows = []
    for item in sorted(windows, key=lambda i: (i['bounds'][1], i['bounds'][0])):
        x, y, X, Y = item['bounds']
        # The U-shaped surround has a separate projecting sill below it.
        surround = box(x-.11, y+.02, X+.11, Y+.13).difference(box(x-.025, y-.2, X+.025, Y+.025))
        add('window_surround', surround, .085)
        add('window_sill', box(x-.15, y-.19, X+.15, y-.035), .20)
        row = next((r for r in rows if abs(np.mean(r, axis=0)[0]-y) < .25), None)
        if row is None:
            rows.append([(y, Y)])
        else:
            row.append((y, Y))
    width, height = size
    supported = sorted((min(y for y, Y in row), max(Y for y, Y in row))
                       for row in rows if len(row) >= 2)
    for lower, upper in zip(supported, supported[1:]):
        if upper[0]-lower[1] > .7:
            level = (lower[1]+upper[0])/2
            add('floor_course', box(0, level-.055, width, level+.055), .055)
    add('base_course', box(0, .16, width, .48), .065)
    add('cornice', box(0, height-.24, width, height-.075), .12)
    return result


def enhance(group, catalog):
    if catalog.get('facade_detailing'):
        raise ValueError('Facade already detailed; start from the undetailed asset')
    before = fingerprint(group.meshes)
    old_count = len(group.meshes)
    prototypes = {p['id']: p for p in catalog['prototypes']}
    parts = defaultdict(list)
    counts = Counter()
    reuse = {}
    max_overlap = 0.
    skipped_collapsed_trim = 0
    center = group.meshes[0].translation.copy()
    for face in catalog['faces']:
        fid = face['face_id']
        instances = [i for i in catalog['instances'] if i['face_id'] == fid]
        wall = []
        for instance in instances:
            prototype = prototypes[instance['prototype']]
            if prototype.get('kind') != 'wall':
                continue
            points = np.asarray(prototype['geometry']['wall'])
            wall.extend(Polygon(tri[:, :2]) for tri in points.reshape(-1, 3, 3))
        if not wall or face.get('plane_deviation_m', 0) > 1e-4:
            continue
        boundary = unary_union(wall)
        basis = np.stack([face['u'], face['v'], face['normal']], axis=1)
        origin = np.asarray(face['origin'])
        wall_rgb = np.asarray(face['wall_finish'].get('render_rgb', face['wall_finish']['rgb']))
        openings = unary_union([box(*i['bounds']) for i in instances
                                if i.get('kind') in ('window', 'door_candidate')])
        for role, polygon, depth in face_details(boundary, instances, face['size']):
            overlap = polygon.intersection(openings).area
            max_overlap = max(max_overlap, overlap)
            if overlap > 1e-8:
                raise ValueError('Facade trim obstructs an opening')
            # Embed the back slightly in the wall; independent closed solids
            # avoid coplanar surfaces and do not alter the approved shell.
            local = extrusion(orient(polygon, sign=1), -.025, depth)
            world = local@basis.T+origin
            # Boolean clipping can leave tiny sliver edges. Validate each trim
            # solid at its actual export precision before adding it to the
            # catalog, so an ornamental sliver cannot invalidate the building.
            probe = mtscene.Mesh(0, 0, center, (world-center).astype(np.float32),
                None, None, [(4, np.arange(len(world), dtype=np.uint32))])
            if any(audit_shell([probe]).values()):
                skipped_collapsed_trim += 1
                continue
            rgb = tuple(np.clip(wall_rgb*.78 if role == 'base_course' else wall_rgb+22, 25, 220).astype(int))
            parts[role, rgb].append(world)
            x, y, X, Y = polygon.bounds
            normalized = local - [x, y, 0]
            key = (role, hashlib.sha256(np.round(normalized, 6).tobytes()).hexdigest())
            if key not in reuse:
                pid = f'facade_detail_{len(reuse):04d}'
                reuse[key] = pid
                catalog['prototypes'].append(dict(id=pid, kind='facade_detail', role=role,
                    geometry={role: normalized.tolist()}, depth_inferred=True))
            catalog['instances'].append(dict(prototype=reuse[key], face_id=fid,
                kind='facade_detail', role=role, bounds=[x, y, X, Y],
                origin=(origin+basis@np.array([x, y, 0])).tolist(), basis=basis.tolist(),
                semantic_status='inferred_architectural_trim', rgb=list(map(int, rgb))))
            counts[role] += 1
    center = group.meshes[0].translation.copy()
    for (role, rgb), pieces in parts.items():
        points = np.concatenate(pieces)
        texture = len(group.textures)
        swatch = np.full((4, 4, 3), rgb, dtype=np.uint8)
        group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,
            raster.GL_COMPRESSED_RGB_S3TC_DXT1, 0, [(4, 4, raster.encode_dxt1(swatch))]))
        catalog['semantic_meshes'].append(dict(local_mesh_index=len(group.meshes), part=role))
        group.meshes.append(mtscene.Mesh(texture, group.meshes[0].object_id, center,
            (points-center).astype(np.float32), np.zeros((len(points), 2), np.float32),
            None, [(4, np.arange(len(points), dtype=np.uint32))]))
    if before != fingerprint(group.meshes[:old_count]):
        raise ValueError('Existing building geometry changed')
    checks = dict(**validate_openings(group, catalog), **planarity(group, catalog))
    checks.update(existing_meshes_unchanged=True, max_trim_opening_overlap_m2=max_overlap)
    catalog['counts'] = dict(Counter(i.get('kind', 'wall') for i in catalog['instances']))
    catalog['facade_detailing'] = dict(version=1, counts=dict(counts), validation=checks,
        skipped_collapsed_trim=skipped_collapsed_trim,
        inference='Trim style and depths are inferred, not measured from the capture. Existing window locations, shell and roof are retained.')
    return catalog['facade_detailing']


def run(base, output, name):
    base, output = Path(base), Path(output)
    manifest = json.loads((base/'manifest.json').read_text())
    record = next(r for r in manifest['buildings'] if r['building'] == name)
    folder = Path(record['catalog']).parent
    asset = base/(name+'.mtscene')
    scene = mtscene.load(asset)
    if len(scene) != 1 or len(scene[0].groups) != 1:
        raise ValueError('Expected an isolated building asset')
    group = scene[0].groups[0]
    catalog = json.loads((folder/'tiles.json').read_text())
    if catalog['building'] != name or group.objects[0].name != name:
        raise ValueError('Asset/catalog building identity mismatch')
    detail = enhance(group, catalog)
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(folder, output/name)
    (output/name/'tiles.json').write_text(json.dumps(catalog, indent=2))
    (output/name/'facade_details.json').write_text(json.dumps(detail, indent=2))
    mtscene.save(output/asset.name, scene)
    # Validate the serialized float32 geometry, not just in-memory results.
    validate_openings(mtscene.load(output/asset.name)[0].groups[0], catalog)
    previous = json.loads((base/'iterations.json').read_text())
    updated = copy.deepcopy(record)
    updated.update(status='accepted', asset=asset.name, catalog=str((output/name/'tiles.json').resolve()),
        counts=catalog['counts'], audit_after=detail['validation']['audit'],
        repair_mode='inferred_facade_details_on_approved_envelope', facade_detailing=detail)
    report = dict(source=previous['source'], base_scene=str((base/'city.mtscene').resolve()),
                  input_asset_sha256=hashlib.sha256(asset.read_bytes()).hexdigest(), buildings=[updated])
    (output/'iterations.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(detail, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base')
    parser.add_argument('output')
    parser.add_argument('--building', required=True)
    args = parser.parse_args()
    run(args.base, args.output, args.building)
