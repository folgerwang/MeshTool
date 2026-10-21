"""Install validated isolated building repairs without regenerating the city."""
import argparse,copy,json,shutil
from pathlib import Path
import numpy as np
import mtscene
from iterate_building_shells import local_group
from repair_stepped_building import geometry_digest
from compose_iteration import fingerprint
from validate_city_iteration import openings


def planarity(group,catalog):
    """Every wall triangle must belong to one of the continuous facade planes."""
    faces=catalog['faces'];normals=np.array([f['normal'] for f in faces]);origins=np.array([f['origin'] for f in faces])
    offsets=np.sum(normals*origins,axis=1);worst=0.;count=0
    for rec in catalog['semantic_meshes']:
        if rec['part']!='wall':continue
        mesh=group.meshes[rec['local_mesh_index']]
        for primitive,index in mesh.draw_calls:
            if primitive!=4:continue
            triangles=(mesh.vertices[index].astype(float)+mesh.translation).reshape(-1,3,3)
            if len(triangles):
                deviations=np.max(abs(triangles@normals.T-offsets),axis=1).min(axis=1)
                worst=max(worst,float(deviations.max()));count+=len(triangles)
    if not count:raise ValueError('No wall triangles to validate')
    if worst>1e-4:raise ValueError(f'Nonplanar wall triangles: {worst} m')
    return dict(wall_triangles_checked=count,max_plane_deviation_m=worst)


def install(base,assets,output):
    base,assets,output=map(Path,(base,assets,output));report=json.loads((assets/'iterations.json').read_text())
    records=[r for r in report['buildings'] if r['status']=='accepted']
    if not records:raise ValueError('No accepted repairs to install')
    scene=mtscene.load(base);source=mtscene.load(report['source']);route=scene.route_data
    selected={(r['batch'],r['group'],r['object_id']) for r in records};protected={};prefix={}
    for bi,b in enumerate(scene):
        for gi,g in enumerate(b.groups):
            protected[bi,gi]=fingerprint([m for m in g.meshes if (bi,gi,m.object_id) not in selected]);prefix[bi,gi]=list(g.textures)
    prepared=[]
    for rec in records:
        bi,gi,oid=rec['batch'],rec['group'],rec['object_id'];name=rec['building'];g=scene[bi].groups[gi]
        if g.objects[oid].name!=name or source[bi].groups[gi].objects[oid].name!=name:raise ValueError('Building identity mismatch')
        captured=local_group(source[bi].groups[gi],oid)
        if geometry_digest(captured.meshes)!=rec['source_geometry_sha256']:raise ValueError('Original capture changed since fitting')
        asset_scene=mtscene.load(assets/rec['asset']);asset=asset_scene[0].groups[0]
        if asset_scene[0].reference_pos!=scene[bi].reference_pos:raise ValueError('Repair coordinate reference mismatch')
        catalog=json.loads(Path(rec['catalog']).read_text());checks=dict(**openings(asset,catalog),**planarity(asset,catalog))
        prepared.append((rec,captured,asset,checks))
    output.mkdir(parents=True,exist_ok=False);validation=[]
    manifest=json.loads((base.parent/'manifest.json').read_text())
    for rec,captured,asset,checks in prepared:
        bi,gi,oid=rec['batch'],rec['group'],rec['object_id'];g=scene[bi].groups[gi];name=rec['building']
        original=[m for m in g.meshes if m.object_id==oid and m.model_variant==1]
        def append_group(group,variant):
            used=sorted({m.tex_index for m in group.meshes if m.tex_index<len(group.textures)})
            remap={t:len(g.textures)+i for i,t in enumerate(used)};g.textures.extend(group.textures[t] for t in used);result=[]
            for mesh in group.meshes:
                mesh=copy.copy(mesh);mesh.object_id=oid;mesh.model_variant=variant;mesh.tex_index=remap.get(mesh.tex_index,0xffffffff);result.append(mesh)
            return result
        if not original:original=append_group(captured,1)
        replacement=append_group(asset,2)
        g.meshes=[m for m in g.meshes if m.object_id!=oid]+original+replacement
        shutil.copytree(assets/name,output/name);shutil.copy2(assets/rec['asset'],output/rec['asset'])
        updated=copy.deepcopy(rec);updated.update(catalog=str((output/name/'tiles.json').resolve()),status='converted',variant=2,conversion_level='capture_supported_planar_envelope',validation=checks)
        manifest['buildings']=[updated if r['building']==name else r for r in manifest['buildings']]
        validation.append(dict(building=name,**checks))
    for bi,b in enumerate(scene):
        for gi,g in enumerate(b.groups):
            assert protected[bi,gi]==fingerprint([m for m in g.meshes if (bi,gi,m.object_id) not in selected])
            assert g.textures[:len(prefix[bi,gi])]==prefix[bi,gi]
    assert scene.route_data==route
    checks=dict(buildings=validation,unselected_geometry_and_materials_unchanged=True,route_unchanged=True,source_scene=str(base.resolve()))
    manifest.update(input_scene=str(base.resolve()),planar_repairs=checks)
    (output/'validation.json').write_text(json.dumps(checks,indent=2));(output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    (output/'iterations.json').write_text(json.dumps(report,indent=2));mtscene.save(output/'city.mtscene',scene)
    print(json.dumps(checks,indent=2),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('assets');p.add_argument('output');a=p.parse_args();install(a.base,a.assets,a.output)
