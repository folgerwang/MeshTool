"""Merge an explicitly selected repair pass into a city, retaining provenance."""
import argparse
import copy
import hashlib
import json
import shutil
from pathlib import Path
import mtscene


def window_layout_regressions(previous,candidate):
    from rules_config import RULES
    cfg=RULES['iteration_acceptance'];old={r['building']:r for r in previous};result={}
    for r in candidate:
        if r.get('status') in ('combined_into_building','excluded_nonbuilding_capture','not_selected'):continue
        before=old.get(r['building'],{}).get('counts',{}).get('window',0)
        after=r.get('counts',{}).get('window',0)
        if before>=cfg['min_previous_windows'] and before-after>=cfg['min_lost_windows'] and after<before*(1-cfg['max_window_loss_fraction']):
            result[r['building']]=dict(reason='window_layout_count_regression_requires_review',previous_windows=before,candidate_windows=after)
    return result


def merge(base,patch,output,only=None):
    base,patch,output=map(Path,(base,patch,output))
    bm=json.loads((base/'manifest.json').read_text());pm=json.loads((patch/'manifest.json').read_text())
    assert bm['inputs']==pm['inputs'],'Repair must use the same reference and capture'
    selected={b['building']:b for b in pm['buildings'] if b.get('status')!='not_selected' and (only is None or b['building'] in only)}
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
        record.setdefault('generation_config_sha256',(pm if name in selected else bm)['config']['config_sha256'])
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
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('patch');p.add_argument('output');p.add_argument('--preserve-window-layout',action='store_true');a=p.parse_args()
    retained={}
    if a.preserve_window_layout:
        bm=json.loads((Path(a.base)/'manifest.json').read_text());pm=json.loads((Path(a.patch)/'manifest.json').read_text())
        retained=window_layout_regressions(bm['buildings'],pm['buildings'])
    only={r['building'] for r in pm['buildings']}-set(retained) if a.preserve_window_layout else None
    merge(a.base,a.patch,a.output,only)
    if retained:
        path=Path(a.output)/'manifest.json';m=json.loads(path.read_text());m['retained_previous_buildings']=retained;path.write_text(json.dumps(m,indent=2))
