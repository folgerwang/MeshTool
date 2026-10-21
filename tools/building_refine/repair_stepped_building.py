"""Repair one capture-bound stepped envelope and cut procedural window openings."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import numpy as np
from shapely.geometry import Polygon,shape
import mtscene,raster,capture_appearance
from capture_tiles import refine_building
from rules_config import snapshot
from solid_city import audit_shell
from stepped_shell import build


def geometry_digest(meshes):
    digest=hashlib.sha256()
    for mesh in meshes:
        for primitive,index in mesh.draw_calls:
            if primitive==4:digest.update((mesh.vertices[index].astype(float)+mesh.translation).tobytes())
    return digest.hexdigest()


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
            result,report=refine_building(candidate.meshes,candidate.objects[0],candidate.textures,folder,observed_masks=observed,clean=True,preserve_roofs=True)
            after=audit_shell(result)
            if any(after.values()):raise ValueError(f'Window cuts damaged closed envelope: {after}')
            candidate.meshes=result
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
