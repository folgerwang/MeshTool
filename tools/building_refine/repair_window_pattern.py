"""Rebuild only window slots on an approved, capture-bound building envelope."""
import argparse,copy,hashlib,json,shutil
from pathlib import Path
import cv2
import numpy as np
from shapely.geometry import shape
import mtscene,raster,capture_appearance
from capture_tiles import refine_building,capture_faces
from iterate_building_shells import local_group
from repair_stepped_building import geometry_digest
from stepped_shell import build
from regular_window_layout import fit_floors,profile_openings
from install_building_repairs import planarity
from validate_city_iteration import openings as validate_openings


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def repair(source,base,profile_path,output):
    source,base,profile_path,output=map(Path,(source,base,profile_path,output))
    profile=json.loads(profile_path.read_text());name=profile['building']
    manifest=json.loads((base.parent/'manifest.json').read_text());record=next(r for r in manifest['buildings'] if r['building']==name)
    folder=Path(record['catalog']).parent;catalog=json.loads((folder/'tiles.json').read_text())
    for file,key in [('tiles.json','catalog_sha256'),('layers.json','layers_sha256')]:
        if digest(folder/file)!=profile[key]:raise ValueError(f'Approved envelope/profile mismatch: {file}')
    scene=mtscene.load(source);bi,gi,oid=record['batch'],record['group'],record['object_id']
    group=local_group(scene[bi].groups[gi],oid)
    if group.objects[0].name!=name or geometry_digest(group.meshes)!=profile['source_geometry_sha256']:raise ValueError('Original capture mismatch')
    floor_faces={int(f) for f in profile.get('floor_fit_faces',profile['faces'])}
    if not floor_faces or not floor_faces.issubset({int(f) for f in profile['faces']}):raise ValueError('Floor-fit faces must be reviewed facades')
    floors=fit_floors(catalog,floor_faces,profile['floor_hint'],profile['floor_count'])
    overrides={f['face_id']:[] for f in catalog['faces']};patterns={}
    for key,config in profile['faces'].items():
        fid=int(key);image_path=folder/f'face_{fid:03d}_source.png'
        if digest(image_path)!=config['capture_png_sha256']:raise ValueError('Reviewed facade capture changed')
        image=cv2.imread(str(image_path));face=next(f for f in catalog['faces'] if f['face_id']==fid)
        # facade_image uses the configured metric pitch; confirm it against the
        # actual image extent rather than silently accepting a different scale.
        from rules_config import RULES
        pitch=max(RULES['windows']['projection_texel_m'],max(face['size'])/2040)
        if abs(image.shape[1]*pitch-face['size'][0])>pitch*2:raise ValueError('Unexpected capture pixel pitch')
        overrides[fid],patterns[key]=profile_openings(floors,config,profile['window_height_m'],face['size'],image.shape,pitch)
    layers=[(x['height'],shape(x['geometry'])) for x in json.loads((folder/'layers.json').read_text())]
    bottom=float(np.percentile(raster.collect(group).pos[:,:,2],.5))
    points,_=build(layers,bottom);center=points.mean(0)
    mesh=mtscene.Mesh(0xffffffff,0,center,(points-center).astype(np.float32),None,None,[(4,np.arange(len(points),dtype=np.uint32))])
    candidate=mtscene.Group(list(group.textures),[mesh],copy.deepcopy(group.objects));observed={}
    capture_appearance.bake(candidate,group,only={name},source_cache=(raster.collect(group),raster.decode_textures(group,size=256)),provenance=observed)
    # Polygon serialization can rotate ring starts and reorder faces. Match by
    # the approved geometric frame, never by incidental triangle enumeration.
    face_map={};new_id=0
    for mesh in candidate.meshes:
        for face,_ in capture_faces(mesh):
            if face.kind!='wall' or face.size[0]<1 or face.size[1]<1.5:continue
            matches=[f['face_id'] for f in catalog['faces'] if all(np.allclose(f[key],value,atol=1e-6,rtol=0) for key,value in [('origin',face.origin),('u',face.u),('v',face.v),('normal',face.normal),('size',face.size)])]
            if len(matches)!=1:raise ValueError('Rebuilt facade does not match approved geometry')
            face_map[matches[0]]=new_id;new_id+=1
    if len(face_map)!=len(catalog['faces']):raise ValueError('Approved facade missing from rebuilt shell')
    overrides={face_map[fid]:value for fid,value in overrides.items()}
    output.mkdir(parents=True,exist_ok=False);destination=output/name;destination.mkdir()
    generated,report=refine_building(candidate.meshes,candidate.objects[0],candidate.textures,destination,observed_masks=observed,clean=True,preserve_roofs=True,opening_overrides=overrides)
    candidate.meshes=generated;updated=json.loads((destination/'tiles.json').read_text())
    capture_errors={}
    for key,config in profile['faces'].items():
        old_image=cv2.imread(str(folder/f'face_{int(key):03d}_source.png'))
        new_image=cv2.imread(str(destination/f'face_{face_map[int(key)]:03d}_source.png'))
        if old_image.shape!=new_image.shape:raise ValueError('Regenerated capture dimensions changed')
        # Reordered coplanar triangles can change DXT block alignment during
        # reprojection. The original geometry and reviewed PNG remain hash-bound.
        error=float(np.mean(abs(old_image.astype(float)-new_image)))
        if error>6:raise ValueError('Regenerated facade differs from reviewed capture')
        capture_errors[key]=error
    if len(catalog['faces'])!=len(updated['faces']):raise ValueError('Facade count changed')
    for old in catalog['faces']:
        new=updated['faces'][face_map[old['face_id']]]
        for key in ('origin','u','v','normal','size'):
            if not np.allclose(old[key],new[key],atol=1e-6,rtol=0):raise ValueError(f'Approved shape changed: {key}')
    for fid,expected in overrides.items():
        actual=[i for i in updated['instances'] if i.get('kind')=='window' and i['face_id']==fid]
        if len(actual)!=len(expected):raise ValueError('Some pattern slots were lost during window cutting')
        for a,b in zip(sorted(actual,key=lambda o:tuple(o['bounds'])),sorted(expected,key=lambda o:tuple(o['bounds']))):
            if not np.allclose(a['bounds'],b['bounds'],atol=1e-7,rtol=0):raise ValueError('Window layout moved during finishing')
    # Compare the roof triangles to the approved saved asset as an additional
    # independent shape check, including roof levels and courtyard boundaries.
    previous=mtscene.load(base);old_group=previous[bi].groups[gi]
    own=[m for m in old_group.meshes if m.object_id==oid and m.model_variant==2]
    old_roofs=[own[r['local_mesh_index']] for r in catalog['semantic_meshes'] if r['part']=='roof']
    roof_records=[r for r in updated['semantic_meshes'] if r['part']=='roof']
    if len(old_roofs)!=len(roof_records):raise ValueError('Approved roof partition changed')
    # Keep the approved roof vertices, UVs, and texture payloads exactly.
    for old,rec in zip(old_roofs,roof_records):
        kept=copy.copy(old);kept.object_id=0;kept.model_variant=0
        if kept.tex_index<len(old_group.textures):
            candidate.textures.append(old_group.textures[kept.tex_index]);kept.tex_index=len(candidate.textures)-1
        generated[rec['local_mesh_index']]=kept
    new_roofs=[generated[r['local_mesh_index']] for r in roof_records]
    if geometry_digest(old_roofs)!=geometry_digest(new_roofs):raise ValueError('Approved roof geometry changed')
    checks=dict(**validate_openings(candidate,updated),**planarity(candidate,updated))
    checks.update(approved_facade_planes_unchanged=True,approved_roof_geometry_unchanged=True,exact_pattern_slots=True)
    layout=dict(floors=floors,faces=patterns,approved_to_output_face_ids=face_map,capture_reprojection_mean_absolute_rgb_error=capture_errors,validation=checks,profile=str(profile_path.resolve()),inference=profile['notes'])
    (destination/'pattern_fit.json').write_text(json.dumps(layout,indent=2));shutil.copy2(folder/'layers.json',destination/'layers.json')
    b=copy.copy(scene[bi]);b.groups=[candidate];mtscene.save(output/(name+'.mtscene'),mtscene.Scene([b]))
    rec=dict(building=name,batch=bi,group=gi,object_id=oid,status='accepted',source_geometry_sha256=profile['source_geometry_sha256'],counts=report['counts'],audit_after=checks['audit'],asset=name+'.mtscene',catalog=str((destination/'tiles.json').resolve()),repair_mode='regular_window_pattern_on_approved_envelope',pattern_fit=layout)
    (output/'iterations.json').write_text(json.dumps(dict(source=str(source.resolve()),buildings=[rec]),indent=2))
    print(json.dumps(dict(counts=report['counts'],floors=floors,validation=checks),indent=2),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('base');p.add_argument('profile');p.add_argument('output');a=p.parse_args();repair(a.source,a.base,a.profile,a.output)
