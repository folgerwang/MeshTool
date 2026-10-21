"""Refine one catalogued facade without rebuilding the rest of its building."""
import argparse
import copy
import hashlib
import json
import shutil
from collections import Counter
from pathlib import Path
import cv2
import numpy as np
import mtscene
import raster
from capture_tiles import refine_building
from solid_city import audit_shell
from rules_config import RULES, snapshot


def run(base,output,name,face_id,flush_windows=False,additions=None,scene=None,manifest=None,image_override=None):
    base,output=Path(base),Path(output);output.mkdir(parents=True,exist_ok=False)
    shared=scene is not None
    if scene is None:scene=mtscene.load(base/'city.mtscene')
    if manifest is None:manifest=json.loads((base/'manifest.json').read_text())
    shutil.copytree(base/name,output/name)
    directory=output/name;catalog=json.loads((directory/'tiles.json').read_text())
    face=next(f for f in catalog['faces'] if f['face_id']==face_id)
    prototypes={p['id']:p for p in catalog['prototypes']}
    image=cv2.cvtColor(cv2.imread(str(directory/f'face_{face_id:03d}_capture.png')),cv2.COLOR_BGR2RGB)
    if image_override is not None:image=image_override.copy()
    pitch=max(RULES['windows']['projection_texel_m'],max(face['size'])/2040)
    openings=[]
    for instance in catalog['instances']:
        if instance['face_id']!=face_id or instance.get('kind') not in ('window','door_candidate'):continue
        o=copy.deepcopy(instance);prototype=prototypes[o['prototype']]
        o['shape']=prototype.get('shape','rectangular');o['measured_members']=prototype.get('captured_members',[])
        x,y,X,Y=o['bounds'];o['pixel_rect']=[round(x/pitch),round(len(image)-Y/pitch),round(X/pitch),round(len(image)-y/pitch)]
        o['contrast']=20.
        if flush_windows and o['kind']=='window':o['surface_mode']='flush'
        openings.append(o)
    if additions:
        for item in additions:
            o=copy.deepcopy(item);o['kind']='window';o['surface_mode']='flush'
            x,y,X,Y=o['bounds'];o['pixel_rect']=[round(x/pitch),round(len(image)-Y/pitch),round(X/pitch),round(len(image)-y/pitch)]
            openings.append(o)
    h,w=image.shape[:2];H,W=(h+3)//4*4,(w+3)//4*4
    padded=cv2.copyMakeBorder(image,0,H-h,0,W-w,cv2.BORDER_REPLICATE)
    f_origin=np.array(face['origin']);u=np.array(face['u']);v=np.array(face['v']);n=np.array(face['normal'])
    width,height=face['size'];local=np.array([[0,0],[width,0],[width,height],[0,0],[width,height],[0,height]])
    for batch in scene:
        for g in batch.groups:
            oid=next((i for i,o in enumerate(g.objects) if o.name==name),None)
            if oid is None:continue
            own=[m for m in g.meshes if m.object_id==oid];texture=len(g.textures)
            g.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(W,H,raster.encode_dxt1(padded))]))
            points=f_origin+local[:,0,None]*u+local[:,1,None]*v;center=own[0].translation
            uv=np.c_[local[:,0]/pitch/W,(h-local[:,1]/pitch)/H]
            plane=mtscene.Mesh(texture,oid,center,(points-center).astype(np.float32),uv.astype(np.float32),None,[(4,np.arange(6,dtype=np.uint32))],0,0)
            work=output/'face_iteration';work.mkdir()
            rect_enabled=RULES['facade_rectification']['enabled']
            if additions:RULES['facade_rectification']['enabled']=False
            try:
                generated,report=refine_building([plane],g.objects[oid],g.textures,work,clean=True,capture_wall=catalog.get('captured_wall_material',True),opening_overrides={0:openings})
            finally:RULES['facade_rectification']['enabled']=rect_enabled
            patch=json.loads((work/'tiles.json').read_text())
            rectification=patch['faces'][0]['rectification']
            if not additions and not flush_windows and not patch['faces'][0].get('shape_changes') and (not rectification or rectification['status']!='rectified'):
                (output/'rejected_iteration.json').write_text(json.dumps(dict(building=name,face_id=face_id,reason=rectification),indent=2))
                print('No accepted correction; keep previous scene',rectification,flush=True)
                return False
            if sum(i.get('kind') in ('window','door_candidate') for i in patch['instances'])!=len(openings):raise ValueError('Opening count changed; reject iteration')
            if not all(any(p['id']==i['prototype'] and len(p['geometry'].get('frame',[])) for p in patch['prototypes']) for i in patch['instances'] if i.get('kind')=='window'):
                raise ValueError('Window without frame')
            keep=[];roles=[];old_roles={r['local_mesh_index']:r['part'] for r in catalog['semantic_meshes']}
            removed=0
            for mi,m in enumerate(own):
                calls=[]
                for primitive,indices in m.draw_calls:
                    if primitive!=4:calls.append((primitive,indices));continue
                    tri=indices.reshape(-1,3);rel=m.vertices[tri].astype(float)+m.translation-f_origin
                    xy=np.stack([rel@u,rel@v],axis=-1)
                    inside=(xy[:,:,0].min(1)>=-.002)&(xy[:,:,0].max(1)<=width+.002)&(xy[:,:,1].min(1)>=-.002)&(xy[:,:,1].max(1)<=height+.002)
                    # Select only this wall and its recessed frame/glass assembly.
                    on_plane=(np.max(rel@n,axis=1)<.002)&(np.min(rel@n,axis=1)>-.25)
                    hit=inside&on_plane
                    if additions and old_roles.get(mi)!='wall':hit[:]=False
                    removed+=int(hit.sum())
                    if (~hit).any():calls.append((primitive,tri[~hit].ravel()))
                if calls:
                    part=copy.copy(m);part.draw_calls=calls
                    if mi in old_roles:roles.append(dict(local_mesh_index=len(keep),part=old_roles[mi]))
                    keep.append(part)
            if additions:
                # Retain original window assemblies; select only new opening parts.
                new_generated=[];new_roles=[]
                for r in patch['semantic_meshes']:
                    mesh=generated[r['local_mesh_index']]
                    if r['part']!='wall':
                        calls=[]
                        for primitive,indices in mesh.draw_calls:
                            if primitive!=4:continue
                            tris=indices.reshape(-1,3);rel=mesh.vertices[tris].astype(float)+mesh.translation-f_origin
                            xy=np.stack([rel@u,rel@v],axis=-1);hit=np.zeros(len(tris),bool)
                            for added in additions:
                                x,y,X,Y=added['bounds'];hit|=(xy[:,:,0].min(1)>=x-.01)&(xy[:,:,0].max(1)<=X+.01)&(xy[:,:,1].min(1)>=y-.01)&(xy[:,:,1].max(1)<=Y+.01)
                            if hit.any():calls.append((primitive,tris[hit].ravel()))
                        if not calls:continue
                        mesh=copy.copy(mesh);mesh.draw_calls=calls
                    new_roles.append(dict(local_mesh_index=len(new_generated),part=r['part']));new_generated.append(mesh)
                generated=new_generated;patch['semantic_meshes']=new_roles
                patch['instances']=[i for i in patch['instances'] if not i.get('kind') or any(np.allclose(i['bounds'],a['bounds'],atol=.001) for a in additions)]
            for r in patch['semantic_meshes']:roles.append(dict(local_mesh_index=len(keep)+r['local_mesh_index'],part=r['part']))
            replacement=keep+generated
            before,after=audit_shell(own),audit_shell(replacement)
            if any(after[k]>before[k] for k in before):raise ValueError(f'Topology regression: {before} -> {after}')
            g.meshes=[m for m in g.meshes if m.object_id!=oid]+replacement
            generation=1
            existing_ids={p['id'] for p in catalog['prototypes']}
            while any(pid.startswith(f'iter_face_{face_id}_pass{generation}_') for pid in existing_ids):generation+=1
            prefix=f'iter_face_{face_id}_pass{generation}_'
            for p in patch['prototypes']:
                p['id']=prefix+p['id']
                if p.get('preview'):
                    preview=prefix+p['preview'];shutil.copy2(work/p['preview'],directory/preview);p['preview']=preview
            for i in patch['instances']:i['prototype']=prefix+i['prototype'];i['face_id']=face_id
            catalog['instances']=[i for i in catalog['instances'] if i['face_id']!=face_id or (additions and i.get('kind'))]+patch['instances']
            used={i['prototype'] for i in catalog['instances']};catalog['prototypes']=[p for p in catalog['prototypes'] if p['id'] in used]+patch['prototypes']
            replacement_face=patch['faces'][0];replacement_face['face_id']=face_id
            if additions:replacement_face['openings']=sum(i['face_id']==face_id and bool(i.get('kind')) for i in catalog['instances'])
            catalog['faces']=[replacement_face if f['face_id']==face_id else f for f in catalog['faces']]
            catalog['semantic_meshes']=roles;catalog['counts']=dict(Counter(i.get('kind','wall') for i in catalog['instances']))
            catalog['facade_iteration']=dict(face_id=face_id,removed_triangles=removed,before=before,after=after,rectification=replacement_face['rectification'])
            (directory/'tiles.json').write_text(json.dumps(catalog,indent=2))
            for suffix in ('capture','detections','source'):
                asset=work/f'face_000_{suffix}.png'
                if asset.exists():
                    label=f'inferred_{suffix}' if image_override is not None else suffix
                    shutil.copy2(asset,directory/f'face_{face_id:03d}_{label}.png')
            for r in manifest['buildings']:
                if r['building']==name:r.update(catalog=str(directory/'tiles.json'),counts=catalog['counts'],facade_iteration=catalog['facade_iteration'])
            print(catalog['facade_iteration'],flush=True)
    if shared:return catalog
    mtscene.save(output/'city.mtscene',scene)
    from building_review import isolate
    isolate(scene,name,directory/'pcg.mtscene')
    manifest['facade_iteration_parent']=dict(path=str(base.resolve()),sha256=hashlib.sha256((base/'city.mtscene').read_bytes()).hexdigest(),config=snapshot(output))
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('output');p.add_argument('--building',required=True);p.add_argument('--face',type=int,required=True);p.add_argument('--flush-windows',action='store_true');a=p.parse_args();run(a.base,a.output,a.building,a.face,a.flush_windows)
