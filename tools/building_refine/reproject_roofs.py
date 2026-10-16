"""Rebake each fitted roof plane at its own height without changing geometry."""
import argparse,hashlib,json,shutil
import numpy as np
from pathlib import Path
import mtscene,raster
from capture_appearance import bake
from rules_config import RULES,snapshot


def run(candidate,source,output):
    candidate,source,output=map(Path,(candidate,source,output))
    shutil.copytree(candidate,output,ignore=shutil.ignore_patterns('city.mtscene','verification.json','review_index.json','pcg.mtscene','capture.mtscene','profile.json','visual_findings.json'))
    scene=mtscene.load(candidate/'city.mtscene');original=mtscene.load(source)
    manifest=json.loads((output/'manifest.json').read_text())
    reports={r['building']:r for r in manifest['buildings']}
    classification=Path(__file__).parent/RULES['profiles']['classification_file']
    excluded={}
    if classification.exists():
        profile=json.loads(classification.read_text())
        if hashlib.sha256(source.read_bytes()).hexdigest()==profile['source_sha256']:
            excluded={r['building']:r for r in profile['objects'] if r['action']=='omit_generated_building'}
        shutil.copy2(classification,output/'measured_profile_snapshot'/classification.name)
    manifest['parent_config']=manifest['config']
    manifest['config']=snapshot(output)
    manifest['config']['profile_hashes']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (output/'measured_profile_snapshot').glob('*.json')}
    for bi,b in enumerate(scene):
        for gi,g in enumerate(b.groups):
            sg=original[bi].groups[gi];cache=(raster.collect(sg),raster.decode_textures(sg,size=512));result=[]
            for oid,obj in enumerate(g.objects):
                own=[m for m in g.meshes if m.object_id==oid]
                if obj.name in excluded:
                    reports[obj.name]['classification_correction']=excluded[obj.name]
                    reports[obj.name]['status']='excluded_nonbuilding_capture'
                    reports[obj.name]['counts']={};reports[obj.name]['roof']=[]
                    reports[obj.name]['glass_meshes']=0
                    print(obj.name,'excluded:',excluded[obj.name]['reason'],flush=True)
                    continue
                catalog=output/obj.name/'tiles.json'
                if obj.cls!=3 or not catalog.exists():result+=own;continue
                data=json.loads(catalog.read_text());roles=data['semantic_meshes']
                if not roles:result+=own;continue
                count=max(r['local_mesh_index'] for r in roles)+1
                roofs=own[count:]
                if not roofs:result+=own;continue
                if reports[obj.name].get('roof_projection'):
                    result+=own;continue
                spans=[r['height_range'][1]-r['height_range'][0] for r in reports[obj.name].get('roof',[]) if 'height_range' in r]
                sloped=False
                for mesh in roofs:
                    for primitive,indices in mesh.draw_calls:
                        if primitive!=4:continue
                        p=(mesh.vertices[indices].astype(float)+mesh.translation).reshape(-1,3,3)
                        n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);nz=n[:,2]/np.maximum(np.linalg.norm(n,axis=1),1e-12)
                        sloped|=bool(np.any((nz>.5)&(nz<.999)))
                if spans and max(spans)<3. and not sloped:
                    # All fitted levels fit comfortably inside the existing
                    # +/-6 m projection band. Their captured textures are valid.
                    reports[obj.name]['roof_projection_repair']='not_required_flat_height_span_below_3m'
                    result+=own;continue
                temporary=mtscene.Group(g.textures,roofs,g.objects)
                report=bake(temporary,sg,only={obj.name},source_cache=cache)
                reports[obj.name]['roof_projection']=report
                result+=own[:count]+temporary.meshes
            g.meshes=result
    mtscene.save(output/'city.mtscene',scene)
    manifest['roof_projection_parent']=dict(path=str(candidate.resolve()),sha256=hashlib.sha256((candidate/'city.mtscene').read_bytes()).hexdigest(),method='separate_coplanar_roof_faces')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('candidate');p.add_argument('source');p.add_argument('output');a=p.parse_args();run(a.candidate,a.source,a.output)
