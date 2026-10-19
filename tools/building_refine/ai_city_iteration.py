"""Local LaMa facade repair and missing-window proposals, with source masks.

Updates only supported gaps in existing PCG wall materials. Window proposals
remain reviewable evidence; they never become doors or automatic geometry.
"""
import argparse,copy,hashlib,json,shutil,time
from pathlib import Path
import cv2
import numpy as np
from scipy import ndimage
from shapely.geometry import box
import mtscene,raster,refine,building
from capture_tiles import decode,facade_image,detect_openings,recover_patterned_windows
from facade_inpaint import repair
from inpaint import LamaInpainter,WEIGHTS
from rules_config import RULES,snapshot


def wall_gap_mask(covered,openings,pitch):
    cfg=RULES['ai_facade'];mask=(~covered)&(ndimage.distance_transform_edt(~covered)*pitch<=cfg['max_gap_m'])
    h,w=mask.shape
    for o in openings:
        x,y,X,Y=o['bounds'];pad=cfg['opening_padding_m']
        a=max(0,int((x-pad)/pitch));b=min(w,int(np.ceil((X+pad)/pitch)))
        c=max(0,int(h-(Y+pad)/pitch));d=min(h,int(np.ceil(h-(y-pad)/pitch)))
        mask[c:d,a:b]=False
    return mask


def run(base,source_path,output,only=None):
    base,source_path,output=map(Path,(base,source_path,output));output.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((base/'manifest.json').read_text());records={r['building']:r for r in manifest['buildings']}
    scene=mtscene.load(base/'city.mtscene');original=mtscene.load(source_path)
    cfg=RULES['ai_facade'];model=LamaInpainter(tile_size=512,margin=64);summaries=[]
    provenance=dict(source=str(source_path.resolve()),source_sha256=hashlib.sha256(source_path.read_bytes()).hexdigest(),base=str(base.resolve()),base_sha256=hashlib.sha256((base/'city.mtscene').read_bytes()).hexdigest(),model='LaMa TorchScript',weights_sha256=hashlib.sha256(Path(WEIGHTS).read_bytes()).hexdigest(),device=model.device,config=snapshot(output))
    (output/'run.json').write_text(json.dumps(provenance,indent=2));start=time.time()
    for bi,batch in enumerate(scene):
        for gi,g in enumerate(batch.groups):
            sg=original[bi].groups[gi];src=raster.collect(sg);source_textures=raster.decode_textures(sg,size=512)
            source_ids={o.name:i for i,o in enumerate(sg.objects)}
            for oid,obj in enumerate(g.objects):
                if obj.cls!=3 or (only and obj.name not in only):continue
                record=records.get(obj.name,{})
                own=[m for m in g.meshes if m.object_id==oid]
                folder=base/obj.name
                if not (folder/'tiles.json').exists():folder=Path(record.get('catalog','__missing__')).parent
                summary=dict(building=obj.name,faces=[],repaired_faces=0,proposals=0)
                if not own or not (folder/'tiles.json').exists():
                    summary['status']='no_geometry' if not own else 'no_facade_catalog';summaries.append(summary);continue
                catalog=json.loads((folder/'tiles.json').read_text());roles={r['local_mesh_index']:r['part'] for r in catalog['semantic_meshes']}
                directory=output/obj.name;directory.mkdir();decoded={}
                summary['input_catalog']=str((folder/'tiles.json').resolve())
                # Preserve observed facade images for subsequent PCG passes.
                for asset in folder.glob('*.png'):
                    shutil.copy2(asset,directory/asset.name)
                selected=np.isin(src.obj,[source_ids[n] for n in record.get('source_members',[obj.name]) if n in source_ids])&(src.tex>=0)
                local=raster.SceneTris(*[getattr(src,k)[selected] for k in ('pos','uv','tex','obj','mesh')])
                normals=np.cross(local.pos[:,1]-local.pos[:,0],local.pos[:,2]-local.pos[:,0]);normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-12)
                for f in catalog['faces']:
                    face=building.Face(np.empty((0,3,3)),*[np.array(f[k]) for k in ('origin','u','v','normal')],f['size'],'wall')
                    pitch=max(cfg['texel_m'],max(face.size)/1020)
                    aligned=abs(normals@face.normal)>.85
                    near=raster.SceneTris(*[getattr(local,k)[aligned] for k in ('pos','uv','tex','obj','mesh')])
                    image,_,covered=refine.project_face(face,near,source_textures,pitch,(-2.5,2.5),nearest_plane=True)
                    covered &= ~raster.yellowish(image)
                    info=dict(face_id=f['face_id'],observed_fraction=float(covered.mean()),status='unchanged')
                    summary['faces'].append(info)
                    if covered.mean()<cfg['min_observed_fraction']:
                        info['status']='insufficient_capture';continue
                    openings=[i for i in catalog['instances'] if i['face_id']==f['face_id'] and i.get('kind')]
                    missing=wall_gap_mask(covered,openings,pitch)
                    if missing.sum()<cfg['min_gap_pixels']:continue
                    # The model can fill only this bounded mask, with real source
                    # context. Restore observed pixels after every ML operation.
                    repaired,inferred,details=repair(image,covered,pitch,model)
                    assert np.array_equal(repaired[covered],image[covered])
                    detected=recover_patterned_windows(repaired,pitch,detect_openings(repaired,pitch,float(face.origin[2]-catalog['building_base_z'])))
                    proposals=[]
                    existing=[box(*o['bounds']) for o in openings]
                    for o in detected:
                        if o['kind']!='window' or any(box(*o['bounds']).intersects(p) for p in existing):continue
                        if o.get('recovery') not in ('bounded_row_gap','capture_template'):continue
                        proposals.append(dict(bounds=o['bounds'],shape=o['shape'],recovery=o['recovery'],status='inferred_requires_review'))
                    # An inferred window is not baked onto a solid wall.
                    missing &= wall_gap_mask(covered,proposals,pitch)
                    matches=[]
                    for mi,m in enumerate(own):
                        if roles.get(mi)!='wall' or m.uvs is None:continue
                        indices=np.concatenate([idx for prim,idx in m.draw_calls if prim==4]);points=m.vertices[indices].astype(float)+m.translation;relative=points-face.origin
                        if len(points)==0 or np.max(abs(relative@face.normal))>.003:continue
                        xy=np.c_[relative@face.u,relative@face.v]
                        if xy[:,0].min()<-.003 or xy[:,0].max()>face.size[0]+.003 or xy[:,1].min()<-.003 or xy[:,1].max()>face.size[1]+.003:continue
                        matches.append((mi,m,indices))
                    if not matches:info['status']='no_single_face_wall_mesh';continue
                    if not missing.any():info['status']='all_gaps_are_opening_proposals';continue
                    # Current wall material is the baseline: only masked pixels
                    # are edited, keeping previous successful material cleanup.
                    for mi,m,indices in matches:
                        if m.tex_index not in decoded:decoded[m.tex_index]=decode(g.textures[m.tex_index])
                        before,_,_=facade_image(m,face,indices,decoded[m.tex_index],pitch)
                        after=before.copy();after[missing]=repaired[missing]
                        assert np.array_equal(after[~missing],before[~missing])
                        h,w=after.shape[:2];H,W=(h+3)//4*4,(w+3)//4*4
                        padded=cv2.copyMakeBorder(after,0,H-h,0,W-w,cv2.BORDER_REPLICATE)
                        texture=len(g.textures);g.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(W,H,raster.encode_dxt1(padded))]))
                        relative=m.vertices.astype(float)+m.translation-face.origin
                        m.uvs=np.c_[relative@face.u/pitch/W,(h-relative@face.v/pitch)/H].astype(np.float32);m.tex_index=texture
                        for label,data in [('before',before),('after',after),('source',image),('inferred',missing.astype(np.uint8)*255)]:
                            cv2.imwrite(str(directory/f"face_{f['face_id']:03d}_{label}.png"),cv2.cvtColor(data,cv2.COLOR_RGB2BGR) if data.ndim==3 else data)
                    info.update(status='ai_gap_repaired',edited_pixels=int(missing.sum()),model=details,window_proposals=proposals)
                    summary['repaired_faces']+=1;summary['proposals']+=len(proposals)
                summary['status']='processed';summaries.append(summary)
                (directory/'ai_review.json').write_text(json.dumps(summary,indent=2))
                (directory/'tiles.json').write_text(json.dumps(catalog,indent=2));record['catalog']=str(directory/'tiles.json');record['ai_facade']=summary
                print(obj.name,'repaired',summary['repaired_faces'],'faces; proposals',summary['proposals'],flush=True)
                (output/'progress.json').write_text(json.dumps(dict(elapsed_s=time.time()-start,buildings=summaries),indent=2))
    mtscene.save(output/'city.mtscene',scene)
    assert hashlib.sha256(source_path.read_bytes()).hexdigest()==provenance['source_sha256']
    manifest['ai_iteration']=dict(**provenance,buildings=summaries,elapsed_s=time.time()-start)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('source');p.add_argument('output');p.add_argument('--only');a=p.parse_args();run(a.base,a.source,a.output,a.only.split(',') if a.only else None)
