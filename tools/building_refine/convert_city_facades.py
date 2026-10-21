"""Convert active city building variants, preserving archived originals and roofs."""
import argparse
import copy
import hashlib
import json
import time
from collections import Counter
from pathlib import Path
import numpy as np
import mtscene
from capture_tiles import refine_building,decode
from capture_appearance import faces
from rules_config import snapshot
from solid_city import audit_shell


def conservative_finish(meshes,textures):
    """Change only opaque material colors when a geometric proposal is rejected."""
    import raster
    result=[];cache={}
    for source in meshes:
        mesh=copy.copy(source)
        # Preserve native assemblies and untextured geometry.
        if source.material not in (mtscene.MAT_GLASS,mtscene.MAT_INTERIOR) and 0<=source.tex_index<len(textures):
            walls=[];roofs=[]
            for face,indices in faces(source):
                (walls if face.kind=='wall' else roofs).extend(indices)
            if roofs:
                roof=copy.copy(source);roof.draw_calls=[(4,np.asarray(roofs,np.uint32))];result.append(roof)
            if not walls:continue
            mesh.draw_calls=[(4,np.asarray(walls,np.uint32))]
            if source.tex_index not in cache:
                rgb=np.median(decode(textures[source.tex_index]).reshape(-1,3),axis=0).astype(np.uint8)
                flat=np.full((4,4,3),rgb,np.uint8);cache[source.tex_index]=len(textures)
                textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(4,4,raster.encode_dxt1(flat))]))
            mesh.tex_index=cache[source.tex_index];mesh.uvs=np.zeros((len(mesh.vertices),2),np.float32);mesh.colors=None
        result.append(mesh)
    return result


def run(source,output):
    source,output=Path(source).resolve(),Path(output).resolve()
    output.mkdir(parents=True,exist_ok=False)
    start=time.monotonic();digest=hashlib.sha256(source.read_bytes()).hexdigest()
    scene=mtscene.load(source);records=[];config=snapshot(output)
    total=sum(o.cls==3 for b in scene for g in b.groups for o in g.objects)
    print(f'Loaded {total} building objects from {source}',flush=True)
    for bi,batch in enumerate(scene):
        for gi,group in enumerate(batch.groups):
            by_object={}
            for mesh in group.meshes:by_object.setdefault(mesh.object_id,[]).append(mesh)
            converted={}
            for oid,obj in enumerate(group.objects):
                if obj.cls!=3:continue
                meshes=by_object.get(oid,[])
                target=2 if any(m.model_variant==2 for m in meshes) else 0
                active=[m for m in meshes if m.model_variant==target]
                record=dict(building=obj.name,batch=bi,group=gi,object_id=oid,variant=target)
                if not active:
                    record['status']='no_active_geometry';records.append(record);continue
                folder=output/f'{bi}_{gi}_{obj.name}';folder.mkdir()
                texture_start=len(group.textures);before=audit_shell(active)
                try:
                    result,report=refine_building(active,obj,group.textures,folder,clean=True,preserve_roofs=True,write_previews=False)
                    after=audit_shell(result)
                    if any(after[k]>before[k] for k in before):
                        raise ValueError(f'Topology regression: {before} -> {after}')
                    record.update(report,status='converted',audit_before=before,audit_after=after)
                except Exception as error:
                    del group.textures[texture_start:]
                    # Retain exact topology and label this separately from an
                    # accepted opening reconstruction. Keep rejected diagnostics.
                    record.update(status='material_only_fallback',reason=str(error),audit_before=before)
                    try:result=conservative_finish(active,group.textures)
                    except Exception as fallback_error:
                        del group.textures[texture_start:];result=active
                        record.update(status='original_retained',fallback_reason=str(fallback_error))
                    record['audit_after']=audit_shell(result)
                for mesh in result:mesh.model_variant=target
                converted[oid]=[m for m in meshes if m.model_variant!=target]+result
                records.append(record)
                summary=dict(total=total,processed=len(records),elapsed_s=round(time.monotonic()-start,1),statuses=dict(Counter(r['status'] for r in records)))
                (output/'progress.json').write_text(json.dumps(summary,indent=2))
                (output/'building_results.json').write_text(json.dumps(records,indent=2))
                print(f'{len(records)}/{total} {obj.name}: {record["status"]} {record.get("counts",{})}',flush=True)
            group.meshes=[m for oid in range(len(group.objects)) for m in converted.get(oid,by_object.get(oid,[]))]
    print('Saving converted city...',flush=True)
    mtscene.save(output/'city.mtscene',scene)
    manifest=dict(input_scene=str(source),input_sha256=digest,config=config,buildings=records,
                  total=total,statuses=dict(Counter(r['status'] for r in records)),elapsed_s=round(time.monotonic()-start,1))
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    if hashlib.sha256(source.read_bytes()).hexdigest()!=digest:raise ValueError('Source scene changed')
    print(json.dumps({k:v for k,v in manifest.items() if k not in ('buildings','config')},indent=2),flush=True)


def retry_fallbacks(output):
    """Retry rejected proposals after a fix, replacing only their active variants."""
    output=Path(output).resolve();manifest=json.loads((output/'manifest.json').read_text())
    source=mtscene.load(manifest['input_scene']);scene=mtscene.load(output/'city.mtscene')
    for record in manifest['buildings']:
        if record['status'] not in ('material_only_fallback','original_retained'):continue
        bi,gi,oid,variant=(record[k] for k in ('batch','group','object_id','variant'))
        original=source[bi].groups[gi];group=scene[bi].groups[gi]
        active=[copy.copy(m) for m in original.meshes if m.object_id==oid and m.model_variant==variant]
        remap={}
        for mesh in active:
            if 0<=mesh.tex_index<len(original.textures):
                if mesh.tex_index not in remap:
                    remap[mesh.tex_index]=len(group.textures);group.textures.append(original.textures[mesh.tex_index])
                mesh.tex_index=remap[mesh.tex_index]
        before=audit_shell(active);texture_start=len(group.textures)
        folder=output/f'{bi}_{gi}_{record["building"]}'
        previous=record.get('reason');result=None
        try:
            result,report=refine_building(active,group.objects[oid],group.textures,folder,clean=True,preserve_roofs=True,write_previews=False)
            after=audit_shell(result)
            if any(after[k]>before[k] for k in before):raise ValueError(f'Topology regression: {before} -> {after}')
            record.update(report,status='converted',audit_before=before,audit_after=after,retry_resolved=previous)
            record.pop('reason',None);record.pop('fallback_reason',None)
        except Exception as error:
            del group.textures[texture_start:]
            result=conservative_finish(active,group.textures)
            record.update(status='material_only_fallback',reason=str(error),audit_after=audit_shell(result))
        for mesh in result:mesh.model_variant=variant
        group.meshes=[m for m in group.meshes if not(m.object_id==oid and m.model_variant==variant)]+result
        print(record['building'],record['status'],record.get('reason',''),flush=True)
    temporary=output/'city_retry.mtscene';mtscene.save(temporary,scene);temporary.replace(output/'city.mtscene')
    manifest['statuses']=dict(Counter(r['status'] for r in manifest['buildings']))
    manifest['retry_completed']=True
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    (output/'building_results.json').write_text(json.dumps(manifest['buildings'],indent=2))
    (output/'progress.json').write_text(json.dumps(dict(total=manifest['total'],processed=len(manifest['buildings']),
        statuses=manifest['statuses'],retry_completed=True),indent=2))
    print(json.dumps(manifest['statuses']),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('source');parser.add_argument('output',nargs='?');parser.add_argument('--retry-fallbacks',action='store_true')
    args=parser.parse_args()
    if args.retry_fallbacks:retry_fallbacks(args.source)
    elif args.output:run(args.source,args.output)
    else:parser.error('output is required for conversion')
