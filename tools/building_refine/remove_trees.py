"""Remove all tree-class meshes from every scene variant; retain other assets."""
import argparse
import gc
import hashlib
import json
from pathlib import Path

import mtscene
from compose_iteration import fingerprint
from compose_city import texture_key
from trees import TREE


def retained_state(scene):
    return [(fingerprint(g.meshes), [texture_key(t) for t in g.textures],
             [(o.name, o.cls) for o in g.objects], [m.lod_size for m in g.meshes])
            for b in scene for g in b.groups]


def remove(scene):
    records = []
    for bi, batch in enumerate(scene):
        for gi, group in enumerate(batch.groups):
            keep = [m for m in group.meshes if group.objects[m.object_id].cls != TREE]
            before = fingerprint(keep)
            for oid, obj in enumerate(group.objects):
                if obj.cls != TREE:
                    continue
                own = [m for m in group.meshes if m.object_id == oid]
                if own:
                    records.append(dict(batch=bi, group=gi, object_id=oid, name=obj.name,
                        meshes=len(own), vertices=sum(len(m.vertices) for m in own),
                        variants=sorted({m.model_variant for m in own})))
            group.meshes = keep
            if fingerprint(group.meshes) != before:
                raise ValueError('Non-tree meshes changed')
    return records


def run(source, output):
    source, output = Path(source), Path(output)
    if output.exists():
        raise ValueError('Use a new output directory')
    route = {}
    scene = mtscene.load(source, route)
    route_bytes = scene.route_data
    records = remove(scene)
    expected = retained_state(scene)
    output.mkdir(parents=True, exist_ok=False)
    mtscene.save(output/'city.mtscene', scene)
    del scene
    gc.collect()
    loaded = mtscene.load(output/'city.mtscene')
    remaining = sum(g.objects[m.object_id].cls == TREE
                    for b in loaded for g in b.groups for m in g.meshes)
    if remaining or retained_state(loaded) != expected or loaded.route_data != route_bytes:
        raise ValueError('Tree removal failed saved-scene preservation checks')
    report = dict(source=str(source.resolve()), removed_tree_objects=len(records),
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


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('output')
    args = parser.parse_args()
    run(args.source, args.output)
