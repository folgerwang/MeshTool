"""Complete a missing facade with same-building captured bay motifs, explicitly inferred."""
import argparse,json,copy,hashlib
from pathlib import Path
import cv2,numpy as np
from shapely.geometry import box
import mtscene,raster,refine,building
from iterate_facade import run as patch
from rules_config import RULES,snapshot

def repeat_bay(donor,target,known,start,end):
    tile=donor[:,start:end];h,w=target.shape[:2]
    if donor.shape[0]!=h or end<=start:raise ValueError('Donor must share floor registration')
    scores=[]
    for phase in range(end-start):
        candidate=tile[:,(np.arange(w)+phase)%(end-start)]
        sample=known.copy();sample[:int(h*.05)]=False;sample[int(h*.9):]=False
        if not sample.any():raise ValueError('No observed edge pixels for phase alignment')
        scores.append(float(np.mean(abs(candidate[sample].astype(float)-target[sample].astype(float)))))
    phase=int(np.argmin(scores));result=tile[:,(np.arange(w)+phase)%(end-start)].copy();result[known]=target[known]
    return result,phase,scores[phase]

def run(base,source,out,name,target_id,donor_id,bay_start,bay_end):
    base,source,out=map(Path,(base,source,out));manifest=json.loads((base/'manifest.json').read_text());scene=mtscene.load(base/'city.mtscene');original=mtscene.load(source)
    digest=hashlib.sha256(source.read_bytes()).hexdigest();catalog=json.loads((base/name/'tiles.json').read_text());record=next(r for r in manifest['buildings'] if r['building']==name)
    g=original[0].groups[0];src=raster.collect(g);ids={o.name:i for i,o in enumerate(g.objects)};sel=np.isin(src.obj,[ids[n] for n in record.get('source_members',[name])])&(src.tex>=0);local=raster.SceneTris(*[getattr(src,k)[sel] for k in ('pos','uv','tex','obj','mesh')]);textures=raster.decode_textures(g,size=1024)
    def project(fid):
        f=next(f for f in catalog['faces'] if f['face_id']==fid);face=building.Face(np.empty((0,3,3)),*[np.array(f[k]) for k in ('origin','u','v','normal')],f['size'],'wall');pitch=max(RULES['windows']['projection_texel_m'],max(face.size)/2040);im,_,mask=refine.project_face(face,local,textures,pitch,(-2.5,2.5),nearest_plane=True);mask &= ~raster.yellowish(im);return f,im,mask,pitch
    target,im,mask,pitch=project(target_id);donor,di,dm,dp=project(donor_id)
    if abs(dp-pitch)>1e-8 or abs(target['origin'][2]-donor['origin'][2])>.01:raise ValueError('Different floor registration')
    start,end=round(bay_start/pitch),round(bay_end/pitch)
    if dm[:,start:end].mean()<RULES['surrounding_facade_completion']['min_donor_coverage']:raise ValueError('Insufficient donor capture')
    di=refine.fill_holes(di,dm);filled,phase,error=repeat_bay(di,im,mask,start,end)
    prototypes={p['id']:p for p in catalog['prototypes']};existing=[box(*i['bounds']) for i in catalog['instances'] if i['face_id']==target_id and i.get('kind')];added=[];period=(end-start)*pitch
    for o in catalog['instances']:
        if o['face_id']!=donor_id or o.get('kind')!='window':continue
        x,y,X,Y=o['bounds']
        if x<start*pitch or X>end*pitch:continue
        for k in range(-1,int(target['size'][0]/period)+2):
            a=x-start*pitch-phase*pitch+k*period;b=X-start*pitch-phase*pitch+k*period
            if a<RULES['surrounding_facade_completion']['min_edge_margin_m'] or b>target['size'][0]-RULES['surrounding_facade_completion']['min_edge_margin_m']:continue
            bounds=[a,y,b,Y];shape=box(*bounds)
            if any(shape.intersects(e) for e in existing):continue
            yy,YY=round(len(im)-Y/pitch),round(len(im)-y/pitch);xx,XX=round(a/pitch),round(b/pitch)
            if mask[yy:YY,xx:XX].mean()>RULES['surrounding_facade_completion']['max_target_observed_window_fraction']:continue
            proto=prototypes[o['prototype']];added.append(dict(bounds=bounds,kind='window',shape=proto.get('shape','rectangular'),confidence=RULES['surrounding_facade_completion']['inferred_window_confidence'],contrast=20.,surface_mode='flush',measured_members=proto.get('captured_members',[]),recovery='surrounding_capture_inference',donor_face=donor_id,inferred=True));existing.append(shape)
    if not added:raise ValueError('No inferred windows fit missing region')
    result=patch(base,out,name,target_id,additions=added,scene=scene,manifest=manifest,image_override=filled)
    provenance=dict(building=name,target_face=target_id,donor_face=donor_id,bay_interval_m=[start*pitch,end*pitch],phase_m=phase*pitch,observed_fraction=float(mask.mean()),donor_coverage=float(dm[:,start:end].mean()),boundary_rgb_error=error,inferred_windows=len(added),source_sha256=digest,classification='inferred_from_same_building_not_directly_observed',config=snapshot(out))
    result['surrounding_capture_inference']=provenance
    # Keep inferred status through the geometry generator's instance normalization.
    for i in result['instances']:
        if i['face_id']==target_id and i.get('kind') and any(np.allclose(i['bounds'],o['bounds']) for o in added):i.update(inferred=True,inference_source=dict(building=name,face_id=donor_id))
    (out/name/'tiles.json').write_text(json.dumps(result,indent=2));manifest['surrounding_capture_inference']=provenance
    for label,array in [('original',im),('inferred',filled),('mask',(~mask).astype(np.uint8)*255),('donor',di)]:cv2.imwrite(str(out/name/f'completion_{label}.png'),cv2.cvtColor(array,cv2.COLOR_RGB2BGR) if array.ndim==3 else array)
    mtscene.save(out/'city.mtscene',scene);(out/'manifest.json').write_text(json.dumps(manifest,indent=2));(out/'inference.json').write_text(json.dumps(provenance,indent=2));assert hashlib.sha256(source.read_bytes()).hexdigest()==digest;print(provenance)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('source');p.add_argument('output');p.add_argument('--building',required=True);p.add_argument('--target',type=int,required=True);p.add_argument('--donor',type=int,required=True);p.add_argument('--bay',nargs=2,type=float,required=True);a=p.parse_args();run(a.base,a.source,a.output,a.building,a.target,a.donor,*a.bay)
