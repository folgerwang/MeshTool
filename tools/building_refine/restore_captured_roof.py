"""Restore a reviewed roof from archived capture without rebuilding lower facades."""
import argparse
import copy
import gc
import json
from collections import Counter
from pathlib import Path

import numpy as np

import mtscene
from compose_iteration import fingerprint
from repair_stepped_building import geometry_digest
from solid_city import audit_shell


def boundary_length(meshes):
    """Unpaired edge length is stable when a clipping plane splits an edge."""
    triangles = np.concatenate([(m.vertices[i].astype(float)+m.translation).reshape(-1, 3, 3)
                                for m in meshes for p, i in m.draw_calls if p == 4])
    points, inverse = np.unique(np.round(triangles.reshape(-1, 3), 4), axis=0, return_inverse=True)
    tri = inverse.reshape(-1, 3)
    edges = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
    edges.sort(axis=1)
    edges, counts = np.unique(edges, axis=0, return_counts=True)
    pairs = points[edges[counts == 1]]
    return float(np.linalg.norm(pairs[:, 1]-pairs[:, 0], axis=1).sum())


def clip_mesh(mesh, height, above):
    """Clip at world Z, preserving original vertices and interpolating UV/color."""
    vertices = list(mesh.vertices.astype(float))
    uv = list(mesh.uvs.astype(float)) if mesh.uvs is not None else None
    colors = list(mesh.colors) if mesh.colors is not None else None
    edge_cache = {}
    z = mesh.vertices[:, 2].astype(float)+mesh.translation[2]-height
    calls = []

    def intersection(a, b):
        if abs(z[a]) < 1e-10:
            return a
        if abs(z[b]) < 1e-10:
            return b
        key = tuple(sorted((a, b)))
        if key not in edge_cache:
            a, b = key
            t = z[a]/(z[a]-z[b])
            point = vertices[a]+t*(vertices[b]-vertices[a])
            point[2] = height-mesh.translation[2]
            edge_cache[key] = len(vertices)
            vertices.append(point)
            if uv is not None:
                uv.append(uv[a]+t*(uv[b]-uv[a]))
            if colors is not None:
                ca = np.array([(int(colors[a]) >> (8*k)) & 255 for k in range(4)])
                cb = np.array([(int(colors[b]) >> (8*k)) & 255 for k in range(4)])
                rgba = np.rint(ca+t*(cb-ca)).astype(int)
                colors.append(sum(int(c) << (8*k) for k, c in enumerate(rgba)))
        return edge_cache[key]

    for primitive, indices in mesh.draw_calls:
        if primitive != 4:
            raise ValueError('Roof restoration requires triangle draw calls')
        result = []
        for triangle in indices.reshape(-1, 3):
            distance = z[triangle]
            if np.all(distance == 0) and not above:
                continue
            inside = distance >= 0 if above else distance <= 0
            if inside.all():
                result.extend(triangle)
                continue
            if not inside.any():
                continue
            polygon = []
            for i in range(3):
                a, b = int(triangle[i]), int(triangle[(i+1) % 3])
                if inside[i]:
                    polygon.append(a)
                if inside[i] != inside[(i+1) % 3]:
                    polygon.append(intersection(a, b))
            polygon = list(dict.fromkeys(polygon))
            for i in range(1, len(polygon)-1):
                tri = [polygon[0], polygon[i], polygon[i+1]]
                a, b, c = [vertices[j] for j in tri]
                if np.linalg.norm(np.cross(b-a, c-a)) > 1e-8:
                    result.extend(tri)
        if result:
            calls.append((primitive, np.asarray(result, np.uint32)))
    if not calls:
        return None
    result = copy.copy(mesh)
    result.draw_calls = calls
    if len(vertices) != len(mesh.vertices):
        result.vertices = np.asarray(vertices, np.float32)
        result.uvs = np.asarray(uv, np.float32) if uv is not None else None
        result.colors = np.asarray(colors, np.uint32) if colors is not None else None
    return result


def lower_catalog(catalog, height, roles):
    """Trim instance geometry as well as meshes so the active catalog stays valid."""
    library = {p['id']: p for p in catalog['prototypes']}
    instances, prototypes = [], []
    for instance in catalog['instances']:
        prototype = copy.deepcopy(library[instance['prototype']])
        basis = np.asarray(instance['basis'])
        origin = np.asarray(instance['origin'])
        geometry = {}
        for role, points in prototype['geometry'].items():
            points = np.asarray(points, float)
            if not len(points):
                continue
            world = points@basis.T+origin
            mesh = mtscene.Mesh(0, 0, np.zeros(3), world, None, None,
                                [(4, np.arange(len(world), dtype=np.uint32))])
            clipped = clip_mesh(mesh, height, False)
            if clipped is not None:
                index = np.concatenate([i for _, i in clipped.draw_calls])
                geometry[role] = np.linalg.solve(basis, (clipped.vertices[index]-origin).T).T.tolist()
        if not geometry:
            continue
        prototype['id'] = f'retained_{len(prototypes)}'
        prototype['geometry'] = geometry
        prototype['clipped_below_world_z'] = height
        updated = copy.deepcopy(instance)
        updated['prototype'] = prototype['id']
        instances.append(updated)
        prototypes.append(prototype)
    face_ids = {i['face_id'] for i in instances}
    catalog['faces'] = [f for f in catalog['faces'] if f['face_id'] in face_ids]
    catalog['prototypes'], catalog['instances'] = prototypes, instances
    catalog['semantic_meshes'] = roles
    catalog['counts'] = dict(Counter(i.get('kind', 'wall') for i in instances))


def run(base, profile_path, output):
    base, profile_path, output = map(Path, (base, profile_path, output))
    if output.exists():
        raise ValueError('Use a new output directory')
    profile = json.loads(profile_path.read_text())
    name, height = profile['building'], profile['roof_start_z']
    scene = mtscene.load(base)
    manifest = json.loads((base.parent/'manifest.json').read_text())
    record = next(r for r in manifest['buildings'] if r['building'] == name)
    bi, gi, oid = record['batch'], record['group'], record['object_id']
    group = scene[bi].groups[gi]
    source = [m for m in group.meshes if m.object_id == oid and m.model_variant == 1]
    active = [m for m in group.meshes if m.object_id == oid and m.model_variant != 1]
    if group.objects[oid].name != name or geometry_digest(source) != profile['source_geometry_sha256']:
        raise ValueError('Reviewed original building mismatch')
    if geometry_digest(active) != profile['active_geometry_sha256']:
        raise ValueError('Reviewed active building mismatch')
    protected = {(b, g): fingerprint([m for m in part.meshes if (b, g, m.object_id) != (bi, gi, oid)])
                 for b, batch in enumerate(scene) for g, part in enumerate(batch.groups)}
    original = fingerprint(source)
    route = scene.route_data
    catalog = json.loads(Path(record['catalog']).read_text())
    old_roles = {r['local_mesh_index']: r['part'] for r in catalog['semantic_meshes']}
    replacement, roles = [], []
    retained = []
    for i, mesh in enumerate(active):
        clipped = clip_mesh(mesh, height, False)
        if clipped is not None:
            roles.append(dict(local_mesh_index=len(replacement), part=old_roles.get(i, 'retained_capture')))
            replacement.append(clipped)
            retained.append(clipped)
    roof_count = 0
    for i, mesh in enumerate(source):
        if i in profile.get('exclude_source_meshes', []):
            continue
        clipped = clip_mesh(mesh, height, True)
        if clipped is not None:
            clipped.model_variant = 2
            roles.append(dict(local_mesh_index=len(replacement), part='roof'))
            replacement.append(clipped)
            roof_count += 1
    if not roof_count:
        raise ValueError('No captured roof surfaces selected')
    before, after = audit_shell(active), audit_shell(replacement)
    lengths = dict(before=boundary_length(active), after=boundary_length(replacement))
    # This is a local restoration on an already open scan. Cutting through
    # existing open edges subdivides them, so raw edge counts are not invariant.
    # Record both counts and lengths; reject increased open-edge length or more
    # nonmanifold/degenerate geometry. A closed input must remain fully closed.
    if (any(after[k] > before[k] for k in ('nonmanifold_edges', 'degenerate_triangles'))
            or lengths['after'] > lengths['before']+.001
            or (before['boundary_edges'] == 0 and after['boundary_edges'] != 0)):
        raise ValueError(f'Topology regression: {before} -> {after}')
    lower_catalog(catalog, height, roles)
    group.meshes = [m for m in group.meshes if m.object_id != oid or m.model_variant == 1]+replacement
    for (b, g), expected in protected.items():
        if fingerprint([m for m in scene[b].groups[g].meshes if (b, g, m.object_id) != (bi, gi, oid)]) != expected:
            raise ValueError('Unselected geometry changed')
    if fingerprint(source) != original or scene.route_data != route:
        raise ValueError('Archive or route changed')
    report = dict(building=name, roof_start_z=height, restored_roof_meshes=roof_count,
        excluded_source_meshes=profile.get('exclude_source_meshes', []),
        audit_before=before, audit_after=after, lower_facades_preserved=True,
        open_boundary_length_m=lengths,
        other_objects_unchanged=True, archived_capture_unchanged=True, materials_unchanged=True,
        remaining_tree_meshes=sum(g.objects[m.object_id].cls == 5 for b in scene for g in b.groups for m in g.meshes),
        source=str(base.resolve()), profile=str(profile_path.resolve()),
        limitation='Captured roof geometry and textures are restored. Existing scan defects below the roofline remain; the whole building is not watertight.')
    catalog['roof_restoration'] = report
    output.mkdir(parents=True)
    folder = output/name
    folder.mkdir()
    (folder/'tiles.json').write_text(json.dumps(catalog, indent=2))
    (output/'roof_repair.json').write_text(json.dumps(report, indent=2))
    (output/'profile.json').write_text(json.dumps(profile, indent=2))
    record.update(catalog=str((folder/'tiles.json').resolve()), roof_restoration=report,
                  audit_after=after, counts=catalog['counts'], conversion_level='captured_roof_restored')
    manifest.update(input_scene=str(base.resolve()), single_building_repair=name)
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2))
    used = sorted({m.tex_index for m in replacement if m.tex_index < len(group.textures)})
    remap = {t: i for i, t in enumerate(used)}
    meshes = []
    for mesh in replacement:
        mesh = copy.copy(mesh)
        mesh.tex_index, mesh.object_id, mesh.model_variant = remap.get(mesh.tex_index, mesh.tex_index), 0, 0
        meshes.append(mesh)
    batch = copy.copy(scene[bi])
    batch.groups = [mtscene.Group([group.textures[t] for t in used], meshes, [group.objects[oid]])]
    mtscene.save(output/(name+'.mtscene'), mtscene.Scene([batch]))
    expected = fingerprint(replacement)
    mtscene.save(output/'city.mtscene', scene)
    del scene, group, replacement, source, active, retained, meshes, batch
    gc.collect()
    saved = mtscene.load(output/'city.mtscene')
    target = [m for m in saved[bi].groups[gi].meshes if m.object_id == oid and m.model_variant != 1]
    if fingerprint(target) != expected or saved.route_data != route:
        raise ValueError('Saved repair differs from validated geometry')
    for (b, g), digest in protected.items():
        if fingerprint([m for m in saved[b].groups[g].meshes if (b, g, m.object_id) != (bi, gi, oid)]) != digest:
            raise ValueError('Saved scene changed an unselected object')
    (output/'reload_validation.json').write_text(json.dumps(dict(installed_geometry_matches=True,
        other_objects_unchanged=True, route_unchanged=True, audit=audit_shell(target)), indent=2))
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base')
    parser.add_argument('profile')
    parser.add_argument('output')
    args = parser.parse_args()
    run(args.base, args.profile, args.output)
