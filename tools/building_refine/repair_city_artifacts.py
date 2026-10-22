"""Audit every active building and install only capture-supported artifact repairs.

The archived capture is the fitting target. Failed fits retain the existing
building and record the reason; a clean box is not a substitute for evidence.
"""
import argparse
import copy
import json
from collections import Counter
from pathlib import Path

import numpy as np
import shapely
import building
import capture_appearance
import mtscene
import raster
from capture_tiles import refine_building
from compose_iteration import fingerprint
from compose_city import texture_key
from facade_details import enhance
from install_building_repairs import planarity
from iterate_building_shells import compare, envelope, local_group
from planar_envelope import fit, assess
from repair_stepped_building import geometry_digest
from solid_city import audit_shell
from stepped_shell import build
from validate_city_iteration import openings


STYLE = dict(frame_width_m=.16, frame_rgb=[210, 204, 190],
             mullion_rgb=[196, 192, 180], glazing_rgb=[27, 54, 72],
             reveal_rgb=[42, 47, 50], door_leaf_rgb=[86, 63, 43])


def active(group, oid):
    own = [m for m in group.meshes if m.object_id == oid]
    return [m for m in own if m.model_variant == 2] or [m for m in own if m.model_variant == 0]


def remove_collapsed_triangles(meshes):
    """Discard zero-area finishing triangles without moving any vertex or UV."""
    removed = 0
    for mesh in meshes:
        calls = []
        for primitive, indices in mesh.draw_calls:
            if primitive != 4:
                calls.append((primitive, indices)); continue
            triangles = mesh.vertices[indices].astype(float).reshape(-1, 3, 3)
            area = np.linalg.norm(np.cross(triangles[:, 1]-triangles[:, 0],
                                          triangles[:, 2]-triangles[:, 0]), axis=1)
            keep = area >= 1e-8
            removed += int((~keep).sum())
            calls.append((primitive, indices.reshape(-1, 3)[keep].ravel()))
        mesh.draw_calls = calls
    return removed


def inspect(group, record):
    own = active(group, record['object_id'])
    if not own:
        return dict(status='no_active_geometry', issues=[])
    audit = audit_shell(own)
    issues = ['damaged_surface'] if any(audit.values()) else []
    checks = dict(audit=audit)
    try:
        catalog = json.loads(Path(record['catalog']).read_text())
        # Closed buildings also need real installed infill and flat-wall checks.
        if not issues:
            asset = mtscene.Group(group.textures, own, group.objects)
            checks.update(openings(asset, catalog))
            checks.update(planarity(asset, catalog))
    except (ValueError, KeyError, IndexError, OSError) as error:
        issues.append(str(error))
    return dict(status='needs_repair' if issues else 'passed', issues=issues, **checks)


def candidates(source):
    tris = raster.collect(source)
    tris.tex[:] = -1
    top = raster.top_down(tris, np.zeros((1, 4, 4, 3), np.uint8), .2)
    bottom = float(np.percentile(tris.pos[:, :, 2], .5))
    crop = dict(height=top.height, rgb=top.color, mask=np.isfinite(top.height),
                other_building=np.zeros_like(top.height, bool), origin=(top.min_x, top.max_y))
    model = building.build_model(crop, bottom, building.Params(
        res=.2, min_plane_m2=10, max_planes=16, simplify_m=.4))
    if model is None:
        raise ValueError('Insufficient roof support')
    trials = []
    accepted = []
    for mode in ('orthogonal', 'polygonal', 'stepped'):
        for tolerance in (.6, .9, 1.2):
            trial = dict(mode=mode, tolerance_m=tolerance)
            trials.append(trial)
            try:
                if mode == 'stepped':
                    layers = envelope(model, tolerance * 2)
                else:
                    layers, evidence = fit(model, tris.pos, tolerance, polygonal=mode == 'polygonal')
                    trial['fit'] = evidence
                points, _ = build(layers, bottom)
                center = points.mean(0)
                mesh = mtscene.Mesh(0xffffffff, 0, center, (points-center).astype(np.float32),
                                   None, None, [(4, np.arange(len(points), dtype=np.uint32))])
                audit = audit_shell([mesh])
                if any(audit.values()):
                    raise ValueError('Candidate envelope is not closed')
                candidate = mtscene.Group(list(source.textures), [mesh], copy.deepcopy(source.objects))
                metrics = compare(source, candidate)
                trial['capture_comparison'] = metrics
                if mode == 'stepped':
                    supported = metrics['accepted']
                else:
                    evidence = assess(source, candidate, layers)
                    trial['roof_evidence'] = evidence
                    supported = evidence['accepted'] and metrics['footprint_iou'] >= .88 and max(metrics['silhouette_excess']) <= .35
                trial['accepted'] = bool(supported)
                if supported:
                    accepted.append((metrics['score'], candidate, layers, trial))
            except (ValueError, shapely.errors.GEOSException) as error:
                trial.update(accepted=False, reason=str(error))
        # Prefer supported straight wall planes over a jagged polygonal fallback.
        if accepted:
            break
    return sorted(accepted, key=lambda x: x[0], reverse=True), trials


def run(base, output, only=None):
    base, output = Path(base), Path(output)
    output.mkdir(parents=True, exist_ok=False)
    scene = mtscene.load(base)
    manifest = json.loads((base.parent/'manifest.json').read_text())
    records = [r for r in manifest['buildings'] if r.get('status') != 'removed_reviewed_vehicle']
    if only is not None:
        records = [r for r in records if r['building'] in only]
        if {r['building'] for r in records} != set(only):
            raise ValueError('Requested building is absent from the manifest')
    report = dict(source=str(base.resolve()), buildings=[])
    print('Auditing', len(records), 'buildings', flush=True)
    for number, record in enumerate(records, 1):
        g = scene[record['batch']].groups[record['group']]
        oid = next(i for i, obj in enumerate(g.objects) if obj.name == record['building'])
        record['object_id'] = oid
        rec = dict(building=record['building'], batch=record['batch'], group=record['group'], object_id=oid,
                   before=inspect(g, record), status='unchanged')
        report['buildings'].append(rec)
        if number % 50 == 0:
            print('Audited', number, flush=True)
    (output/'audit_before.json').write_text(json.dumps(report, indent=2))
    originals = {(bi, gi): fingerprint([m for m in g.meshes if m.model_variant == 1])
                 for bi, b in enumerate(scene) for gi, g in enumerate(b.groups)}
    prefixes = {(bi, gi): [texture_key(t) for t in g.textures] for bi, b in enumerate(scene) for gi, g in enumerate(b.groups)}
    routes = scene.route_data
    wanted = [r for r in report['buildings'] if r['before']['status'] == 'needs_repair']
    targets = {(r['batch'], r['group'], r['object_id']) for r in wanted}
    protected = {(bi, gi): fingerprint([m for m in g.meshes if (bi, gi, m.object_id) not in targets])
                 for bi, b in enumerate(scene) for gi, g in enumerate(b.groups)}
    trees_before = sum(g.objects[m.object_id].cls == 5 for b in scene for g in b.groups for m in g.meshes)
    print('Buildings needing repair:', len(wanted), flush=True)
    selected = set()
    preserved = {(r['batch'], r['group'], r['object_id']): fingerprint(active(scene[r['batch']].groups[r['group']], r['object_id'])) for r in report['buildings']}
    for number, rec in enumerate(wanted, 1):
        name = rec['building']
        print(number, '/', len(wanted), name, 'fitting', flush=True)
        g = scene[rec['batch']].groups[rec['group']]
        oid = rec['object_id']
        source = local_group(g, oid)
        rec['source_geometry_sha256'] = geometry_digest(source.meshes)
        try:
            # The individually restored roof is an explicit measured exception.
            # A generic shell must not discard its accepted captured geometry.
            if name == 'building_278':
                raise ValueError('Restored captured roof is protected; remaining lower facade needs a measured envelope')
            options, rec['trials'] = candidates(source)
            if not options:
                raise ValueError('No envelope passed capture shape and roof evidence checks')
            installed = False
            for attempt, (_, candidate, layers, trial) in enumerate(options):
                folder = output/name/f'candidate_{attempt}'
                folder.mkdir(parents=True)
                try:
                    observed = {}
                    capture_appearance.bake(candidate, source, only={name},
                        source_cache=(raster.collect(source), raster.decode_textures(source, size=256)), provenance=observed)
                    generated, _ = refine_building(candidate.meshes, candidate.objects[0], candidate.textures,
                        folder, observed_masks=observed, clean=True, preserve_roofs=True,
                        write_previews=True, window_style=STYLE)
                    candidate.meshes = generated
                    removed = remove_collapsed_triangles(candidate.meshes)
                    catalog = json.loads((folder/'tiles.json').read_text())
                    checks = dict(**openings(candidate, catalog), **planarity(candidate, catalog))
                    detail = enhance(candidate, catalog)
                    checks = detail['validation']
                    (folder/'tiles.json').write_text(json.dumps(catalog, indent=2))
                    (folder/'layers.json').write_text(json.dumps([dict(height=z, geometry=shapely.geometry.mapping(p)) for z, p in layers]))
                    batch = copy.copy(scene[rec['batch']]); batch.groups = [candidate]
                    asset_path = output/(name+'.mtscene')
                    mtscene.save(asset_path, mtscene.Scene([batch]))
                    saved = mtscene.load(asset_path)[0].groups[0]
                    checks.update(openings(saved, catalog)); checks.update(planarity(saved, catalog))
                    rec.update(status='repaired', selected_trial=trial, validation=checks,
                        catalog=str((folder/'tiles.json').resolve()), counts=catalog['counts'], asset=asset_path.name,
                        collapsed_finishing_triangles_removed=removed)
                    used = sorted({m.tex_index for m in saved.meshes if m.tex_index < len(saved.textures)})
                    remap = {t:len(g.textures)+i for i, t in enumerate(used)}
                    g.textures.extend(saved.textures[t] for t in used)
                    replacement = []
                    for mesh in saved.meshes:
                        mesh = copy.copy(mesh); mesh.object_id = oid; mesh.model_variant = 2
                        mesh.tex_index = remap.get(mesh.tex_index, 0xffffffff); replacement.append(mesh)
                    # Original captures remain available in variant 1.
                    if not any(m.object_id == oid and m.model_variant == 1 for m in g.meshes):
                        for m in active(g, oid):
                            m.model_variant = 1
                        originals[rec['batch'], rec['group']] = fingerprint([m for m in g.meshes if m.model_variant == 1])
                    g.meshes = [m for m in g.meshes if m.object_id != oid or m.model_variant == 1]+replacement
                    updated = next(r for r in manifest['buildings'] if r['building'] == name)
                    updated.update(status='converted', variant=2, conversion_level='capture_supported_city_artifact_repair',
                        audit_before=rec['before']['audit'], audit_after=checks['audit'], catalog=rec['catalog'],
                        counts=catalog['counts'], validation=checks, source_geometry_sha256=rec['source_geometry_sha256'])
                    selected.add((rec['batch'], rec['group'], oid))
                    installed = True
                    break
                except (ValueError, shapely.errors.GEOSException) as error:
                    trial['finishing_rejected'] = str(error)
            if not installed:
                raise ValueError('Capture fit passed, but opening or finished geometry validation failed')
        except (ValueError, shapely.errors.GEOSException) as error:
            rec.update(status='needs_measured_review', reason=str(error))
        report['counts'] = dict(Counter(r['status'] for r in report['buildings']))
        (output/'repair_report.json').write_text(json.dumps(report, indent=2))
        print(name, rec['status'], rec.get('counts', rec.get('reason', '')), flush=True)
    for bi, b in enumerate(scene):
        for gi, g in enumerate(b.groups):
            assert protected[bi, gi] == fingerprint([m for m in g.meshes if (bi, gi, m.object_id) not in targets])
            assert prefixes[bi, gi] == [texture_key(t) for t in g.textures[:len(prefixes[bi, gi])]]
            assert originals[bi, gi] == fingerprint([m for m in g.meshes if m.model_variant == 1])
    for key, value in preserved.items():
        if key not in selected:
            bi, gi, oid = key
            assert value == fingerprint(active(scene[bi].groups[gi], oid))
    assert routes == scene.route_data
    trees_after = sum(g.objects[m.object_id].cls == 5 for b in scene for g in b.groups for m in g.meshes)
    assert trees_after == trees_before
    report['preservation'] = dict(unselected_buildings_unchanged=True, archived_captures_unchanged=True,
                                 existing_texture_payloads_unchanged=True, terrain_and_other_objects_unchanged=True,
                                 route_unchanged=True, remaining_tree_meshes=trees_after)
    manifest.update(input_scene=str(base.resolve()), city_artifact_repair=str((output/'repair_report.json').resolve()))
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2))
    mtscene.save(output/'city.mtscene', scene)
    saved = mtscene.load(output/'city.mtscene')
    for bi, b in enumerate(scene):
        for gi, g in enumerate(b.groups):
            assert fingerprint(g.meshes) == fingerprint(saved[bi].groups[gi].meshes)
    assert saved.route_data == routes
    report['saved_city_matches'] = True
    (output/'repair_report.json').write_text(json.dumps(report, indent=2))
    print('Saved', output/'city.mtscene', report['counts'], flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base'); parser.add_argument('output')
    parser.add_argument('--only', help='Comma-separated building names for a focused retry')
    args = parser.parse_args()
    run(args.base, args.output, set(args.only.split(',')) if args.only else None)
