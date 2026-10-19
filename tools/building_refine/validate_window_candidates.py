"""Validate AI window candidates against independently projected original capture."""
import argparse,json,copy,hashlib,shutil
from pathlib import Path
import cv2,numpy as np
from shapely.geometry import box
import mtscene,raster,refine,building
from capture_tiles import detect_openings
from iterate_facade import run as patch_face
from rules_config import RULES,snapshot

def validate(proposal,detected,coverage,pitch):
    shape=box(*proposal['bounds']); best=None
    for d in detected:
        if d.get('kind')!='window':continue
        other=box(*d['bounds']);score=shape.intersection(other).area/shape.union(other).area
        if best is None or score>best[0]:best=(score,d)
    if best is None or best[0]<RULES['window_candidate_validation']['min_iou']:return None,'no_matching_observed_opening'
    d=copy.deepcopy(best[1]);d['pixel_rect']=[int(x) for x in d['pixel_rect']];d['neighbors']=int(d.get('neighbors',0));a,b,c,e=d['pixel_rect'];known=float(coverage[b:e,a:c].mean())
    if known<RULES['window_candidate_validation']['min_observed_fraction']:return None,'insufficient_observed_pixels'
    d.update(kind='window',surface_mode='flush',validation=dict(iou=best[0],observed_fraction=known),recovery='original_capture_validated')
    return d,'accepted'

def run(base,source,output):
    base,source,output=map(Path,(base,source,output));output.mkdir(exist_ok=False)
    manifest=json.loads((base/'manifest.json').read_text());scene=mtscene.load(base/'city.mtscene');original=mtscene.load(source)
    digest=hashlib.sha256(source.read_bytes()).hexdigest();reports=[];records={r['building']:r for r in manifest['buildings']}
    jobs=sorted(manifest['ai_iteration']['buildings'],key=lambda r:r['building']!='building_334')
    for bi,batch in enumerate(scene):
      for gi,g in enumerate(batch.groups):
        sg=original[bi].groups[gi];src=raster.collect(sg);tex=raster.decode_textures(sg,size=1024);ids={o.name:i for i,o in enumerate(sg.objects)}
        for job in jobs:
          name=job['building'];folder=base/name
          if not (folder/'tiles.json').exists():continue
          catalog=json.loads((folder/'tiles.json').read_text());selected=np.isin(src.obj,[ids[n] for n in records[name].get('source_members',[name]) if n in ids])&(src.tex>=0)
          local=raster.SceneTris(*[getattr(src,k)[selected] for k in ('pos','uv','tex','obj','mesh')]);current=base
          for info in job['faces']:
            proposals=info.get('window_proposals',[])
            if not proposals:continue
            fid=info['face_id'];f=next(f for f in catalog['faces'] if f['face_id']==fid)
            face=building.Face(np.empty((0,3,3)),*[np.array(f[k]) for k in ('origin','u','v','normal')],f['size'],'wall');pitch=max(.08,max(face.size)/1020)
            image,_,covered=refine.project_face(face,local,tex,pitch,(-2.5,2.5),nearest_plane=True);covered &= ~raster.yellowish(image)
            image[~covered]=128;detected=detect_openings(image,pitch,float(face.origin[2]-catalog['building_base_z']))
            accepted=[];results=[];existing=[box(*o['bounds']) for o in catalog['instances'] if o['face_id']==fid and o.get('kind')]
            for proposal in proposals:
              d,reason=validate(proposal,detected,covered,pitch)
              if d and any(box(*d['bounds']).intersects(x) for x in existing):d=None;reason='overlaps_existing_opening'
              if d:accepted.append(d);existing.append(box(*d['bounds']))
              results.append(dict(proposal=proposal,status=reason,accepted=d))
            report=dict(building=name,face_id=fid,candidates=results,installed=0)
            if accepted:
              stage=output/f'{name}_face_{fid:03d}'
              saved_meshes=list(g.meshes);saved_tex=list(g.textures);saved_manifest=copy.deepcopy(manifest)
              try:
                updated=patch_face(current,stage,name,fid,additions=accepted,scene=scene,manifest=manifest)
                current=stage;catalog=updated;report['installed']=len(accepted);report['status']='installed'
              except Exception as e:
                g.meshes=saved_meshes;g.textures=saved_tex;manifest.clear();manifest.update(saved_manifest);report.update(status='geometry_rejected',error=str(e))
            reports.append(report)
            print(name,fid,'accepted',len(accepted),'installed',report['installed'],report.get('error',''),flush=True)
          if current!=base:
            shutil.copytree(current/name,output/name)
            for r in manifest['buildings']:
              if r['building']==name:r['catalog']=str(output/name/'tiles.json')
    assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
    manifest['validated_windows']=dict(config=snapshot(output),source_sha256=digest,reports=reports,total_installed=sum(r['installed'] for r in reports))
    mtscene.save(output/'city.mtscene',scene);(output/'manifest.json').write_text(json.dumps(manifest,indent=2,default=lambda x:x.item() if isinstance(x,np.generic) else x));(output/'validation.json').write_text(json.dumps(manifest['validated_windows'],indent=2,default=lambda x:x.item() if isinstance(x,np.generic) else x))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('source');p.add_argument('output');a=p.parse_args();run(a.base,a.source,a.output)
