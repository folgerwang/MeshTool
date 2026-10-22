"""Repair one capture-bound stepped envelope and cut procedural window openings."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import numpy as np
from shapely.geometry import Polygon,shape
import mtscene,raster,capture_appearance
from capture_tiles import refine_building,capture_faces
from rules_config import RULES
from rules_config import snapshot
from solid_city import audit_shell
from stepped_shell import build


def geometry_digest(meshes):
    digest=hashlib.sha256()
    for mesh in meshes:
        for primitive,index in mesh.draw_calls:
            if primitive==4:digest.update((mesh.vertices[index].astype(float)+mesh.translation).tobytes())
    return digest.hexdigest()


def reviewed_openings(meshes,facades,ground_z=None):
    """Bind measured slots to facade frames, including stepped roof wall bands."""
    from shapely.geometry import box
    result={};frames=[]
    for mesh in meshes:
        for face,_ in capture_faces(mesh):
            if face.kind=='wall' and face.size[0]>=1 and face.size[1]>=1.5:
                frames.append(face)
    for fid,face in enumerate(frames):
        result[fid]=[]
        config=facades.get(str(fid))
        if config is None:continue
        for key in ('origin','u','v','normal','size'):
            if not np.allclose(config[key],getattr(face,key),atol=1e-6,rtol=0):
                raise ValueError(f'Reviewed facade frame changed: {fid}/{key}')
        pitch=max(RULES['windows']['projection_texel_m'],max(face.size)/2040)
        w,h=np.ceil(np.array(face.size)/pitch).astype(int)
        for slot in config['openings']:
            x,y,X,Y=map(float,slot['bounds']);shape=box(x,y,X,Y)
            kind=slot.get('kind','window')
            if kind not in ('window','door_candidate'):raise ValueError('Unsupported reviewed opening kind')
            boundary=box(.12,.03 if kind=='door_candidate' else .12,face.size[0]-.12,face.size[1]-.12)
            if X<=x or Y<=y or not boundary.covers(shape):
                raise ValueError(f'Reviewed window leaves facade {fid}')
            if any(shape.intersects(box(*o['bounds'])) for o in result[fid]):
                raise ValueError(f'Reviewed windows overlap on facade {fid}')
            item=dict(kind=kind,shape=slot.get('shape','rectangular'),bounds=[x,y,X,Y],
                pixel_rect=[round(x/pitch),round(h-Y/pitch),round(X/pitch),round(h-y/pitch)],
                confidence=1.,recovery='visually_measured_capture')
            if kind=='door_candidate':
                if ground_z is None or not -.01<=face.origin[2]+y-ground_z<=RULES['doors']['max_sill_offset_m']:
                    raise ValueError('Reviewed door must meet the building base')
                item.update(support_level_z=float(ground_z),recovery='reviewed_door_capture')
            if 'measured_members' in slot:item['measured_members']=slot['measured_members']
            result[fid].append(item)
    if set(facades)-{str(i) for i in range(len(frames))}:
        raise ValueError('Reviewed facade is absent from rebuilt shell')
    return result


def repair(base,profile_path,output):
    base,profile_path,output=Path(base).resolve(),Path(profile_path).resolve(),Path(output).resolve()
    output.mkdir(parents=True,exist_ok=False);profile=json.loads(profile_path.read_text());name=profile['building']
    scene=mtscene.load(base);manifest_path=base.parent/'manifest.json'
    manifest=json.loads(manifest_path.read_text()) if manifest_path.exists() else dict(buildings=[])
    found=False
    for bi,batch in enumerate(scene):
        for gi,group in enumerate(batch.groups):
            oid=next((i for i,obj in enumerate(group.objects) if obj.name==name and obj.cls==3),None)
            if oid is None:continue
            if found:raise ValueError('Building name is ambiguous across scene groups')
            found=True;own=[m for m in group.meshes if m.object_id==oid]
            captured=[m for m in own if m.model_variant==1] or own
            if geometry_digest(captured)!=profile['source_geometry_sha256']:
                raise ValueError('Measured envelope profile does not match captured building geometry')
            used=sorted({m.tex_index for m in captured if 0<=m.tex_index<len(group.textures)})
            remap={old:i for i,old in enumerate(used)};source_meshes=[]
            for mesh in captured:
                mesh=copy.copy(mesh);mesh.object_id=0;mesh.model_variant=0;mesh.tex_index=remap.get(mesh.tex_index,mesh.tex_index);source_meshes.append(mesh)
            source=mtscene.Group([group.textures[i] for i in used],source_meshes,[copy.copy(group.objects[oid])])
            layers=[(item['height'],shape(item['geometry']) if 'geometry' in item else Polygon(item['outer_xy'],item.get('holes_xy',[]))) for item in profile['layers']]
            points,_=build(layers,profile['ground_z']);center=captured[0].translation.copy()
            shell=mtscene.Mesh(0,0,center,(points-center).astype(np.float32),np.zeros((len(points),2),np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))])
            before=audit_shell([shell])
            if any(before.values()):raise ValueError(f'Envelope is not closed: {before}')
            candidate=mtscene.Group(list(source.textures),[shell],copy.deepcopy(source.objects));observed={}
            evidence=capture_appearance.bake(candidate,source,only={name},source_cache=(raster.collect(source),raster.decode_textures(source,size=256)),provenance=observed)
            folder=output/name;folder.mkdir()
            overrides=reviewed_openings(candidate.meshes,profile['facades'],profile['ground_z']) if 'facades' in profile else None
            result,report=refine_building(candidate.meshes,candidate.objects[0],candidate.textures,folder,observed_masks=observed,clean=True,preserve_roofs=True,
                capture_wall=profile.get('captured_wall_material',False),opening_overrides=overrides,window_style=profile.get('window_style'))
            after=audit_shell(result)
            if any(after.values()):raise ValueError(f'Window cuts damaged closed envelope: {after}')
            candidate.meshes=result
            if overrides is not None:
                from validate_city_iteration import openings
                from install_building_repairs import planarity
                catalog=json.loads((folder/'tiles.json').read_text())
                for fid,config in profile['facades'].items():
                    if config.get('capture_png_sha256') and hashlib.sha256((folder/f'face_{int(fid):03d}_source.png').read_bytes()).hexdigest()!=config['capture_png_sha256']:
                        raise ValueError(f'Reviewed facade capture changed: {fid}')
                for fid,expected in overrides.items():
                    actual=[i for i in catalog['instances'] if i.get('kind') in ('window','door_candidate') and i['face_id']==fid]
                    if len(actual)!=len(expected) or any(a['kind']!=b['kind'] or not np.allclose(a['bounds'],b['bounds'],atol=1e-7,rtol=0)
                        for a,b in zip(sorted(actual,key=lambda o:tuple(o['bounds'])),sorted(expected,key=lambda o:tuple(o['bounds'])))):
                        raise ValueError(f'Reviewed windows changed during cutting: {fid}')
                report['reviewed_window_validation']=dict(**openings(candidate,catalog),**planarity(candidate,catalog))
            if profile.get('facade_details'):
                from facade_details import enhance
                catalog=json.loads((folder/'tiles.json').read_text())
                report['facade_detailing']=enhance(candidate,catalog)
                (folder/'tiles.json').write_text(json.dumps(catalog,indent=2))
                report.update(counts=catalog['counts'],prototypes=len(catalog['prototypes']),instances=len(catalog['instances']))
                after=audit_shell(result)
            preview_batch=copy.copy(batch);preview_batch.groups=[candidate]
            mtscene.save(output/(name+'.mtscene'),mtscene.Scene([preview_batch]))
            active=[m for m in own if m.model_variant!=1]
            used=sorted({m.tex_index for m in result if 0<=m.tex_index<len(candidate.textures)})
            remap={old:len(group.textures)+i for i,old in enumerate(used)}
            group.textures.extend(candidate.textures[i] for i in used)
            replacement=[]
            for mesh in result:
                mesh=copy.copy(mesh);mesh.object_id=oid;mesh.model_variant=2;mesh.tex_index=remap.get(mesh.tex_index,mesh.tex_index);replacement.append(mesh)
            group.meshes=[m for m in group.meshes if m.object_id!=oid or m.model_variant==1]+replacement
            report.update(status='converted',variant=2,batch=bi,group=gi,object_id=oid,
                conversion_level='capture_supported_stepped_envelope',audit_before=audit_shell(active),audit_after=after,
                window_wall_cutouts=True,profile=str(profile_path),source_geometry_sha256=profile['source_geometry_sha256'],
                capture=evidence,shell_audit=before)
            records=manifest.setdefault('buildings',[])
            records[:]=[report if r['building']==name else r for r in records]
            if not any(r['building']==name for r in records):records.append(report)
            (output/'repair_report.json').write_text(json.dumps(report,indent=2))
            print(name,report['counts'],after,flush=True)
    if not found:raise ValueError('Building is absent from the city')
    manifest.update(input_scene=str(base),input_sha256=hashlib.sha256(base.read_bytes()).hexdigest(),
        config=snapshot(output),single_building_repair=name,
        repair_profile_sha256=hashlib.sha256(profile_path.read_bytes()).hexdigest())
    (output/'measured_envelope.json').write_bytes(profile_path.read_bytes())
    mtscene.save(output/'city.mtscene',scene)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    (output/'building_results.json').write_text(json.dumps(manifest['buildings'],indent=2))
    print('Saved',output/'city.mtscene',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('base');parser.add_argument('profile');parser.add_argument('output')
    args=parser.parse_args();repair(args.base,args.profile,args.output)
