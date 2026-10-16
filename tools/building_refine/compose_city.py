"""Compose checkpointed PCG buildings into the existing road/terrain scene."""
import argparse,copy,hashlib,json
from collections import Counter
from pathlib import Path
import numpy as np
import mtscene


def texture_key(texture):
    h=hashlib.sha256(str((texture.internal_format,texture.format,texture.type)).encode())
    for w,hgt,payload in texture.mips:
        h.update(str((w,hgt)).encode());h.update(payload)
    return h.digest()


def compose(scene_path,asset_directory,destination):
    scene_path=Path(scene_path).resolve();asset_directory=Path(asset_directory).resolve();destination=Path(destination).resolve()
    if destination.exists():raise ValueError('Use a new output directory')
    manifest=json.loads((asset_directory/'manifest.json').read_text())
    original_hash=hashlib.sha256(Path(manifest['source']).read_bytes()).hexdigest()
    if original_hash!=manifest['source_sha256']:raise ValueError('Original capture hash mismatch')
    identities=[(r['batch'],r['group'],r['object']) for r in manifest['buildings']]
    if len(set(identities))!=len(identities):raise ValueError('Duplicate building identities in asset manifest')
    scene_hash=hashlib.sha256(scene_path.read_bytes()).hexdigest();scene=mtscene.load(scene_path)
    records=[];indices={}
    for bi,batch in enumerate(scene):
      for gi,group in enumerate(batch.groups):
        remap={};objects=[]
        for i,o in enumerate(group.objects):
            if o.cls!=3:remap[i]=len(objects);objects.append(o)
        kept=[]
        for mesh in group.meshes:
            if mesh.object_id not in remap:continue
            m=copy.copy(mesh);m.object_id=remap[mesh.object_id];kept.append(m)
        group.meshes=kept;group.objects=objects
        indices[bi,gi]={texture_key(t):i for i,t in enumerate(group.textures)}
    for record in manifest['buildings']:
        bi,gi=record['batch'],record['group'];batch=scene[bi];group=batch.groups[gi]
        asset_path=asset_directory/record['asset'];asset=mtscene.load(asset_path)
        if asset[0].reference_pos!=batch.reference_pos:raise ValueError('Asset coordinate reference mismatch')
        source=asset[0].groups[0];oid=len(group.objects);group.objects.append(copy.copy(source.objects[0]))
        if len(source.objects)!=1 or source.objects[0].name!=record['name'] or source.objects[0].cls!=3:
            raise ValueError('PCG asset identity mismatch')
        remap={}
        for ti,texture in enumerate(source.textures):
            key=texture_key(texture)
            if key not in indices[bi,gi]:indices[bi,gi][key]=len(group.textures);group.textures.append(texture)
            remap[ti]=indices[bi,gi][key]
        bounds=[]
        for mesh in source.meshes:
            m=copy.copy(mesh);m.object_id=oid;m.model_variant=0
            if m.tex_index!=0xffffffff:m.tex_index=remap[m.tex_index]
            group.meshes.append(m);world=m.vertices.astype(float)+m.translation
            bounds.extend([world.min(0),world.max(0)])
        bounds=np.array(bounds)
        records.append(dict(**record,output_object=oid,world_bounds=[bounds.min(0).tolist(),bounds.max(0).tolist()],asset_sha256=hashlib.sha256(asset_path.read_bytes()).hexdigest()))
    if hashlib.sha256(scene_path.read_bytes()).hexdigest()!=scene_hash:raise ValueError('Input scene changed')
    destination.mkdir(parents=True);mtscene.save(destination/'city_pcg.mtscene',scene)
    report=dict(input_scene=str(scene_path),input_sha256=scene_hash,source_sha256=original_hash,buildings=records,
                methods=dict(Counter(r['method'] for r in records)),building_count=len(records),
                coordinates='Original world coordinates retained; road and terrain geometry preserved. No captured building fallback meshes.')
    (destination/'manifest.json').write_text(json.dumps(report,indent=2));print(report['methods'],flush=True)
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('scene');p.add_argument('assets');p.add_argument('output');a=p.parse_args();compose(a.scene,a.assets,a.output)
