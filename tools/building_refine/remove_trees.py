"""Save planting records, then remove tree-class meshes from every variant."""
import argparse
import gc
import hashlib
import json
import copy
from pathlib import Path
import numpy as np

import mtscene
from capture_identity import ensure_capture
from compose_iteration import fingerprint
from compose_city import texture_key
from trees import TREE


def retained_state(scene):
    return [(fingerprint(g.meshes), [texture_key(t) for t in g.textures],
             [(o.name, o.cls) for o in g.objects], [m.lod_size for m in g.meshes],
             copy.deepcopy(g.tree_instances))
            for b in scene for g in b.groups]


def bounds(meshes):
    """Only drawn vertices belong to a split object; buffers may hold a whole tile."""
    points = []
    for mesh in meshes:
        indices = [np.asarray(index) for _, index in mesh.draw_calls if len(index)]
        if not indices: continue
        indices = np.unique(np.concatenate(indices))
        if indices.min() < 0 or indices.max() >= len(mesh.vertices):
            raise ValueError('Invalid tree vertex index; geometry was not removed')
        points.append(mesh.vertices[indices].astype(float)+mesh.translation)
    if not points: raise ValueError('Tree has no drawn vertices; geometry was not removed')
    points = np.concatenate(points)
    if not np.isfinite(points).all():
        raise ValueError('Non-finite tree coordinates; geometry was not removed')
    return dict(min=points.min(0).tolist(), max=points.max(0).tolist())


def remove(scene):
    records = []
    pending = []
    for bi, batch in enumerate(scene):
        for gi, group in enumerate(batch.groups):
            for oid, obj in enumerate(group.objects):
                if obj.cls != TREE:
                    continue
                own = [m for m in group.meshes if m.object_id == oid]
                if own:
                    variants = sorted({m.model_variant for m in own})
                    chosen = [0, 1] if 1 in variants else ([0] if 0 in variants else [2])
                    bbox = bounds([m for m in own if m.model_variant in chosen])
                    low, high = np.array(bbox['min']), np.array(bbox['max'])
                    capture_id = getattr(scene, 'capture', {}).get('id', '')
                    identity = json.dumps([capture_id, batch.reference_pos, bi, gi, oid, obj.name, bbox], sort_keys=True)
                    record = dict(schema_version=1, instance_id=hashlib.sha256(identity.encode()).hexdigest()[:24],
                        batch=bi, group=gi, object_id=oid, name=obj.name,
                        meshes=len(own), vertices=sum(len(m.vertices) for m in own),
                        variants=variants, bounds_source_variants=chosen,
                        bbox=bbox, size_m=(high-low).tolist(),
                        position=[float((low[0]+high[0])/2), float((low[1]+high[1])/2), float(low[2])],
                        position_basis='bbox_xy_center_and_lowest_vertex',
                        geometry_source='procedural' if chosen == [2] else 'captured_or_shared',
                        ground_verified=False, trunk_position_measured=False,
                        coordinate_system='local_east_north_up_m' if batch.is_georeferenced else 'scene_xyz_m_z_up',
                        reference_lon_lat=list(batch.reference_pos), is_georeferenced=batch.is_georeferenced,
                        variant_bounds={str(v):bounds([m for m in own if m.model_variant==v]) for v in variants},
                        geometry_removed=True, planting_status='unplanted')
                    if capture_id: record['capture_id'] = capture_id
                    records.append(record); pending.append((group, record))
    # Validate all records before changing any group. Never lose a tree if its
    # position cannot be recorded. Existing records survive repeated removals.
    for group, record in pending:
        if not any(r.get('instance_id') == record['instance_id'] for r in group.tree_instances):
            group.tree_instances.append(copy.deepcopy(record))
    for batch in scene:
        for group in batch.groups:
            keep = [m for m in group.meshes if not (0 <= m.object_id < len(group.objects)
                    and group.objects[m.object_id].cls == TREE)]
            before = fingerprint(keep)
            group.meshes = keep
            if fingerprint(group.meshes) != before:
                raise ValueError('Non-tree meshes changed')
    return records


def run(source, output, progress=False):
    source, output = Path(source), Path(output)
    if output.exists():
        raise ValueError('Use a new output directory')
    route = {}
    if progress: print('@progress 0 4 Loading segmented scene', flush=True)
    scene = mtscene.load(source, route)
    capture = copy.deepcopy(ensure_capture(scene, source))
    route_bytes = scene.route_data
    records = remove(scene)
    expected = retained_state(scene)
    output.mkdir(parents=True, exist_ok=False)
    instances = [dict(r, batch=bi, group=gi) for bi,b in enumerate(scene)
                 for gi,g in enumerate(b.groups) for r in g.tree_instances]
    catalog = dict(schema_version=1, capture=capture, source=str(source.resolve()), instances=instances,
        units='metres', purpose='Plant replacement trees using retained positions and canopy bounds',
        placement_note='Position is an approximate bbox base, not a measured trunk or terrain elevation. Merged segmented crowns remain one instance.')
    (output/'tree_instances.json').write_text(json.dumps(catalog, indent=2, allow_nan=False), encoding='utf-8')
    if progress: print('@progress 2 4 Saving tree-free scene and planting records', flush=True)
    mtscene.save(output/'city.mtscene', scene)
    del scene
    gc.collect()
    loaded = mtscene.load(output/'city.mtscene')
    remaining = sum(0 <= m.object_id < len(g.objects) and g.objects[m.object_id].cls == TREE
                    for b in loaded for g in b.groups for m in g.meshes)
    if remaining or retained_state(loaded) != expected or loaded.route_data != route_bytes or loaded.capture != capture:
        raise ValueError('Tree removal failed saved-scene preservation checks')
    report = dict(source=str(source.resolve()), capture=capture, removed_tree_objects=len(records),
        saved_tree_instances=len(instances), planting_catalog=str((output/'tree_instances.json').resolve()),
        planting_records_embedded=True,
        removed_tree_meshes=sum(r['meshes'] for r in records), remaining_tree_meshes=remaining,
        non_tree_meshes_and_materials_unchanged=True, object_ids_unchanged=True,
        ground_and_buildings_unchanged=True, route_unchanged=True,
        route_points=len(route.get('polyline', [])),
        route_sha256=hashlib.sha256(route_bytes).hexdigest(), objects=records,
        policy='Remove tree-class geometry from all variants. Retain ground surfaces, object IDs and texture payloads.')
    (output/'tree_removal.json').write_text(json.dumps(report, indent=2))
    manifest_path = source.parent/'manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        manifest.update(input_scene=str(source.resolve()), tree_removal=report,
                        vegetation_policy='No tree meshes in any model variant')
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k != 'objects'}, indent=2), flush=True)
    if progress: print('@progress 4 4 Tree removal and planting records verified', flush=True)
    print(f'Removed {len(records)} trees; retained {len(instances)} planting records in the scene and {output / "tree_instances.json"}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('output')
    parser.add_argument('--progress', action='store_true')
    args = parser.parse_args()
    run(args.source, args.output, args.progress)
