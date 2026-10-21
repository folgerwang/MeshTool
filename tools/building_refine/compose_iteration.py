"""Assemble accepted building iterations, clean terrain, road paint and tree instances."""
import argparse,copy,hashlib,json
from pathlib import Path
import numpy as np
from scipy import ndimage
import mtscene,raster,iterate_roads,tree_instances
from solid_city import audit_shell
from compose_city import texture_key
from repair_stepped_building import geometry_digest


def remove_reviewed_vehicles(group,profile):
    """Apply explicit visual labels only to exactly matching captured geometry."""
    selected=[]
    for rec in profile['objects']:
        ids=[i for i,o in enumerate(group.objects) if o.name==rec['name']]
        if len(ids)!=1:raise ValueError('Reviewed vehicle identity is absent or ambiguous')
        oid=ids[0];own=[m for m in group.meshes if m.object_id==oid]
        if geometry_digest(own)!=rec['expected_geometry_sha256']:
            raise ValueError('Reviewed vehicle geometry does not match this scene')
        selected.append(oid)
    for oid in selected:group.objects[oid].cls=4
    group.meshes=[m for m in group.meshes if m.object_id not in selected]
    return [group.objects[i].name for i in selected]


def compact_textures(group):
    used=sorted({m.tex_index for m in group.meshes if m.tex_index<len(group.textures)})
    unique={};textures=[];remap={}
    for old in used:
        key=texture_key(group.textures[old])
        if key not in unique:unique[key]=len(textures);textures.append(group.textures[old])
        remap[old]=unique[key]
    before=len(group.textures)
    for m in group.meshes:m.tex_index=remap.get(m.tex_index,0xffffffff)
    group.textures=textures
    return dict(before=before,after=len(textures),payloads_unchanged=True)


def clean_paving(group,data,color):
    """Replace baked cars/canopies in remaining ground photographs with finishes."""
    sample=(data['source_class']==1)&~data['inferred']&data['cover']
    rgb=color.astype(float);neutral=(rgb.max(2)-rgb.min(2)<45)&(rgb.mean(2)>85)&(rgb.mean(2)<200)
    base=np.median(rgb[sample&neutral],axis=0) if (sample&neutral).any() else np.array([151.,146.,137.])
    palettes={1:base,2:base,6:np.array([85.,104.,64.]),7:np.array([70.,92.,103.])}
    textures={};yy,xx=np.indices((256,256));rng=np.random.default_rng(11)
    for cls,shade in palettes.items():
        image=np.broadcast_to(shade,(256,256,3)).copy()+rng.integers(-3,4,(256,256,1))
        if cls in (1,2):image[(xx%64<1)|(yy%64<1)]-=7
        image=np.clip(image,0,255).astype(np.uint8);textures[cls]=len(group.textures)
        group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(256,256,raster.encode_dxt1(image))]))
    count=0
    for m in group.meshes:
        obj=group.objects[m.object_id]
        if obj.cls not in palettes or obj.name.startswith('PCG road asphalt'):continue
        m.tex_index=textures[obj.cls];m.colors=None;m.uvs=((m.vertices.astype(float)+m.translation)[:,:2]/8).astype(np.float32);count+=1
    return dict(meshes=count,concrete_rgb=base.tolist(),method='Procedural paving and ground finishes remove baked vehicle/canopy photographs; road paint remains separately reconstructed from capture evidence.')


def fingerprint(meshes):
    digest=hashlib.sha256()
    for m in meshes:
        digest.update(str((m.object_id,m.tex_index,m.model_variant,m.material)).encode())
        for a in (m.translation,m.vertices,m.uvs,m.colors):
            if a is not None:digest.update(a.tobytes())
        for p,i in m.draw_calls:digest.update(str(p).encode());digest.update(i.tobytes())
    return digest.hexdigest()


def run(base,terrain,evidence,asset_dirs,output,vehicle_review=None):
    base,terrain,evidence,output=map(Path,(base,terrain,evidence,output))
    output.mkdir(parents=True,exist_ok=False);scene=mtscene.load(base);original_route=scene.route_data
    clean=mtscene.load(terrain/'terrain.mtscene');manifest=json.loads((base.parent/'manifest.json').read_text())
    reviewed=[]
    if vehicle_review:
        reviewed=remove_reviewed_vehicles(scene[0].groups[0],json.loads(Path(vehicle_review).read_text()))
        for rec in manifest['buildings']:
            if rec['building'] in reviewed:rec.update(status='removed_reviewed_vehicle',conversion_level='vehicle_label_corrected')
    accepted={};trials=[]
    for folder in map(Path,asset_dirs):
        report=json.loads((folder/'iterations.json').read_text());trials+=report['buildings']
        for rec in report['buildings']:
            if rec['status']=='accepted':accepted[rec['building']]=(folder,rec)
    protected={};archives={}
    for bi,b in enumerate(scene):
        if b.is_spline:continue
        for gi,g in enumerate(b.groups):
            for oid,o in enumerate(g.objects):
                if o.cls!=3:continue
                own=[m for m in g.meshes if m.object_id==oid]
                if o.name not in accepted:protected[bi,gi,oid]=fingerprint(own)
                archives[bi,gi,oid]=fingerprint([m for m in own if m.model_variant==1])
    for name,(folder,rec) in accepted.items():
        g=scene[rec['batch']].groups[rec['group']];oid=rec['object_id'];asset=mtscene.load(folder/rec['asset'])[0].groups[0]
        offset=len(g.textures);g.textures.extend(asset.textures);new=[]
        for m in asset.meshes:
            m=copy.copy(m);m.object_id=oid;m.model_variant=2
            if m.tex_index<len(asset.textures):m.tex_index+=offset
            new.append(m)
        audit=audit_shell(new)
        if any(audit.values()):raise ValueError(f'Invalid accepted building {name}')
        g.meshes=[m for m in g.meshes if m.object_id!=oid or m.model_variant==1]+new
        for r in manifest['buildings']:
            if r['building']==name:r.update(rec,variant=2,conversion_level='capture_supported_iterated_envelope')
    reports=[]
    terrain_manifest=json.loads((terrain/'manifest.json').read_text())
    if len(terrain_manifest['groups'])!=1 or (terrain_manifest['groups'][0]['source_batch'],terrain_manifest['groups'][0]['source_group'])!=(0,0):
        raise ValueError('This iteration requires one captured group; provide separately registered evidence for multi-group scenes')
    for rec in terrain_manifest['groups']:
        bi,gi=rec['source_batch'],rec['source_group'];g=scene[bi].groups[gi];ground=clean[rec['output_batch']].groups[rec['output_group']]
        # Replace captured roads/ground and any old inferred traffic controls.
        g.meshes=[m for m in g.meshes if g.objects[m.object_id].cls not in (1,2,6,7) and not g.objects[m.object_id].name.startswith('PCG inferred traffic')]
        tex_offset=len(g.textures);obj_offset=len(g.objects);g.textures.extend(ground.textures);g.objects.extend(ground.objects)
        for m in ground.meshes:
            m=copy.copy(m);m.object_id+=obj_offset
            if m.tex_index<len(ground.textures):m.tex_index+=tex_offset
            g.meshes.append(m)
        data=dict(np.load(terrain/rec['data']));road_evidence=dict(np.load(evidence/'evidence.npz'))
        excluded=data['source_class']==3
        print('Iterating roads and paint',flush=True)
        roads=iterate_roads.apply(g,data,road_evidence,excluded,output/'roads')
        import cv2
        color=cv2.cvtColor(cv2.imread(str(terrain/'0_0_surface.png')),cv2.COLOR_BGR2RGB)
        roads['surrounding_ground']=clean_paving(g,data,color)
        print('Replacing cars and trees',flush=True)
        trees=tree_instances.apply(g,data,output/'trees')
        reports.append(dict(batch=bi,group=gi,roads=roads,trees=dict(prototypes=trees['prototype_count'],instances=len(trees['instances']),removed_car_objects=trees['removed_car_objects'],source_tree_objects=len(trees['source_objects']),unresolved_tree_objects=sum(r['placements']==0 for r in trees['source_objects']))))
    for (bi,gi,oid),digest in protected.items():
        assert fingerprint([m for m in scene[bi].groups[gi].meshes if m.object_id==oid])==digest
    for (bi,gi,oid),digest in archives.items():
        assert fingerprint([m for m in scene[bi].groups[gi].meshes if m.object_id==oid and m.model_variant==1])==digest
    assert scene.route_data==original_route
    validation=dict(protected_buildings_verified=len(protected),archived_buildings_verified=len(archives),route_unchanged=True,reviewed_vehicle_removals=reviewed,
                    remaining_car_meshes=sum(g.objects[m.object_id].cls==4 for b in scene for g in b.groups for m in g.meshes),
                    remaining_captured_tree_meshes=sum(g.objects[m.object_id].cls==5 and m.model_variant!=2 for b in scene for g in b.groups for m in g.meshes),
                    accepted_buildings=len(accepted),buildings_attempted=len(trials),groups=reports)
    assert validation['remaining_car_meshes']==validation['remaining_captured_tree_meshes']==0
    validation['texture_compaction']=[compact_textures(g) for b in scene for g in b.groups]
    (output/'validation.json').write_text(json.dumps(validation,indent=2));(output/'building_iterations.json').write_text(json.dumps(trials,indent=2))
    manifest.update(input_scene=str(base.resolve()),city_iteration=validation)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print('Saving city',flush=True);mtscene.save(output/'city.mtscene',scene);print(json.dumps({k:v for k,v in validation.items() if k!='groups'}),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('terrain');p.add_argument('evidence');p.add_argument('output');p.add_argument('--assets',nargs='+',required=True);p.add_argument('--vehicle-review')
    a=p.parse_args();run(a.base,a.terrain,a.evidence,a.assets,a.output,a.vehicle_review)
