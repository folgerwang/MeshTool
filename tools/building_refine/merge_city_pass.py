"""Merge an explicitly selected repair pass into a city, retaining provenance."""
import argparse
import copy
import hashlib
import json
import shutil
from pathlib import Path
import mtscene


def merge(base,patch,output):
    base,patch,output=map(Path,(base,patch,output))
    bm=json.loads((base/'manifest.json').read_text());pm=json.loads((patch/'manifest.json').read_text())
    assert bm['inputs']==pm['inputs'],'Repair must use the same reference and capture'
    selected={b['building']:b for b in pm['buildings'] if b.get('status')!='not_selected'}
    city=mtscene.load(base/'city.mtscene');repair=mtscene.load(patch/'city.mtscene')
    assert len(city)==len(repair) and city.route_data==repair.route_data
    for before,after in zip(city,repair):
        assert len(before.groups)==len(after.groups)
        for group,rg in zip(before.groups,after.groups):
            assert group.objects==rg.objects
            ids={i for i,o in enumerate(group.objects) if o.name in selected}
            own=[copy.copy(m) for m in rg.meshes if m.object_id in ids];textures={}
            for mesh in own:
                ti=mesh.tex_index
                if 0<=ti<len(rg.textures):
                    if ti not in textures:
                        textures[ti]=len(group.textures);group.textures.append(rg.textures[ti])
                    mesh.tex_index=textures[ti]
            group.meshes=[m for m in group.meshes if m.object_id not in ids]+own
    output.mkdir(parents=True,exist_ok=False)
    reports=[]
    for previous in bm['buildings']:
        name=previous['building'];parent=patch if name in selected else base
        record=copy.deepcopy(selected.get(name,previous))
        if (parent/name).exists():shutil.copytree(parent/name,output/name)
        record['generation_config_sha256']=(pm if name in selected else bm)['config']['config_sha256']
        if 'catalog' in record:record['catalog']=str(output/name/'tiles.json')
        reports.append(record)
    shutil.copy2(patch/'effective_rules.json',output/'effective_rules.json')
    if (patch/'measured_profile_snapshot').exists():shutil.copytree(patch/'measured_profile_snapshot',output/'measured_profile_snapshot')
    manifest=copy.deepcopy(pm);manifest['buildings']=reports
    manifest['passes']=[dict(path=str(p),scene_sha256=hashlib.sha256((p/'city.mtscene').read_bytes()).hexdigest()) for p in (base,patch)]
    manifest['repaired_buildings']=sorted(selected)
    mtscene.save(output/'city.mtscene',city)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('patch');p.add_argument('output');a=p.parse_args();merge(a.base,a.patch,a.output)
