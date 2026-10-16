"""Persistent building-by-building refinement queue; metrics never imply approval."""
import argparse
import hashlib
import json
from functools import lru_cache
from pathlib import Path
import numpy as np
from PIL import Image
from shapely.geometry import box
from shapely.ops import unary_union
from curtain_wall import detect
from rules_config import snapshot


@lru_cache(maxsize=8)
def _digest(path,size,mtime):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def scene_digest(path):
    path=Path(path).resolve();stat=path.stat()
    return _digest(str(path),stat.st_size,stat.st_mtime_ns)


def building_hashes(path):
    import mtscene
    result={}
    for batch in mtscene.load(path):
        for group in batch.groups:
            textures={}
            for oid,obj in enumerate(group.objects):
                if obj.cls!=3:continue
                h=hashlib.sha256(obj.name.encode())
                for m in group.meshes:
                    if m.object_id!=oid:continue
                    h.update(str((m.material,m.model_variant)).encode())
                    for a in (m.translation,m.vertices,m.uvs,m.colors):
                        h.update(b'none' if a is None else str((a.dtype,a.shape)).encode()+a.tobytes())
                    for primitive,indices in m.draw_calls:h.update(str(primitive).encode()+indices.tobytes())
                    if 0<=m.tex_index<len(group.textures):
                        if m.tex_index not in textures:
                            t=group.textures[m.tex_index];th=hashlib.sha256(str((t.internal_format,t.format,t.type)).encode())
                            for w,hh,payload in t.mips:th.update(str((w,hh)).encode()+payload)
                            textures[m.tex_index]=th.digest()
                        h.update(textures[m.tex_index])
                result[obj.name]=h.hexdigest()
    return result


def scan(city,output):
    city=Path(city).resolve();output=Path(output);output.mkdir(parents=True,exist_ok=False)
    config=snapshot(output)
    manifest=json.loads((city/'manifest.json').read_text());entries=[]
    baseline_rules=json.loads((city/'effective_rules.json').read_text())
    for record in manifest['buildings']:
        name=record['building'];folder=city/name;faces=[];missing=0.
        if (folder/'tiles.json').exists():
            catalog=json.loads((folder/'tiles.json').read_text())
            for face in catalog['faces']:
                path=folder/f'face_{face["face_id"]:03d}_capture.png'
                if not path.exists():continue
                im=np.array(Image.open(path).convert('RGB'))
                pitch=max(baseline_rules['windows']['projection_texel_m'],max(face['size'])/2040)
                proposals=detect(im,pitch)
                if not proposals:continue
                proposed=unary_union([box(*o['bounds']) for o in proposals])
                existing=unary_union([box(*i['bounds']) for i in catalog['instances'] if i.get('kind')=='window' and i['face_id']==face['face_id']])
                gap=proposed.difference(existing).area;missing+=gap
                faces.append(dict(face=face['face_id'],proposed_glazed_area_m2=proposed.area,missing_glazed_area_m2=gap,proposed_openings=len(proposals)))
        roof=[r['status'] for r in record.get('roof',[]) if r['status']!='captured_height_surface']
        tasks=[]
        if missing>30:tasks.append('fit_captured_glass_bands')
        if roof:tasks.append('repair_roof_evidence_or_fit')
        tasks.extend(['compare_windows_and_wall_materials','compare_roof_silhouette_and_details'])
        entries.append(dict(building=name,status='pending_iteration',priority=round(missing+len(roof)*100,2),
                            missing_glazed_area_m2=round(missing,2),curtain_faces=faces,roof_failures=roof,
                            tasks=tasks,history=[]))
        print(name,round(missing,1),'m2 missing candidate glazing',flush=True)
    entries.sort(key=lambda e:(e['building']!='building_469',-e['priority']))
    ledger=dict(schema=1,baseline=str(city),baseline_sha256=hashlib.sha256((city/'city.mtscene').read_bytes()).hexdigest(),
                config=config,acceptance='Human/agent visual comparison required; numerical gains alone cannot accept a building',buildings=entries)
    (output/'iterations.json').write_text(json.dumps(ledger,indent=2))
    selected=[e['building'] for e in entries if e['missing_glazed_area_m2']>30]
    (output/'curtain_candidates.txt').write_text(','.join(selected))


def register(ledger,candidate):
    path=Path(ledger);data=json.loads(path.read_text());candidate=Path(candidate).resolve()
    manifest=json.loads((candidate/'manifest.json').read_text())
    digest=scene_digest(candidate/'city.mtscene');contents=building_hashes(candidate/'city.mtscene')
    reports={r['building']:r for r in manifest['buildings']}
    for entry in data['buildings']:
        r=reports[entry['building']]
        entry['latest_candidate']=dict(scene=str(candidate/'city.mtscene'),config_hash=manifest['config']['config_sha256'],
            scene_sha256=digest,building_content_sha256=contents[entry['building']],counts=r.get('counts',{}),roof=r.get('roof',[]),stage='generated_pending_comparison')
        previous=next((h for h in reversed(entry['history']) if h.get('building_content_sha256')==contents[entry['building']]),None)
        if previous:
            entry['latest_candidate']['stage']='visually_reviewed';entry['status']=previous['status']
        else:entry['status']='generated_pending_comparison'
    data.setdefault('generation_passes',[]).append(dict(path=str(candidate),scene_sha256=digest))
    path.write_text(json.dumps(data,indent=2))


def summary(ledger):
    entries=json.loads(Path(ledger).read_text())['buildings']
    pending=[e for e in entries if e['status']!='accepted']
    # Complete a round across buildings rather than endlessly tuning one tower.
    pending.sort(key=lambda e:(len(e['history']),-e['priority']))
    print(json.dumps(dict(total=len(entries),generated=sum('latest_candidate' in e for e in entries),
        visually_reviewed=sum(bool(e['history']) for e in entries),accepted=sum(e['status']=='accepted' for e in entries),
        latest_candidate_reviewed=sum(e.get('latest_candidate',{}).get('stage')=='visually_reviewed' for e in entries),
        next_buildings=[dict(building=e['building'],tasks=e['tasks'],iterations=len(e['history'])) for e in pending[:10]]),indent=2))


def record(ledger,building,candidate,status,notes,images):
    path=Path(ledger);data=json.loads(path.read_text());scene=Path(candidate).resolve()
    evidence=[]
    for im in images:
        p=Path(im).resolve();Image.open(p).verify()
        evidence.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
    if not evidence:raise ValueError('An iteration requires rendered comparison evidence')
    entry=next(e for e in data['buildings'] if e['building']==building)
    entry['history'].append(dict(iteration=len(entry['history'])+1,candidate=str(scene),
        scene_sha256=scene_digest(scene),status=status,notes=notes,images=evidence))
    entry['status']=status
    if entry.get('latest_candidate',{}).get('scene_sha256')==entry['history'][-1]['scene_sha256']:
        entry['latest_candidate']['stage']='visually_reviewed'
        entry['history'][-1]['building_content_sha256']=entry['latest_candidate'].get('building_content_sha256')
    path.write_text(json.dumps(data,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();sub=p.add_subparsers(dest='command',required=True)
    s=sub.add_parser('scan');s.add_argument('city');s.add_argument('output')
    s=sub.add_parser('record');s.add_argument('ledger');s.add_argument('building');s.add_argument('candidate');s.add_argument('status',choices=['improved_needs_review','needs_more_work','rejected','accepted']);s.add_argument('notes');s.add_argument('images',nargs='+')
    s=sub.add_parser('register');s.add_argument('ledger');s.add_argument('candidate')
    s=sub.add_parser('summary');s.add_argument('ledger')
    a=p.parse_args()
    if a.command=='scan':scan(a.city,a.output)
    elif a.command=='register':register(a.ledger,a.candidate)
    elif a.command=='summary':summary(a.ledger)
    else:record(a.ledger,a.building,a.candidate,a.status,a.notes,a.images)
