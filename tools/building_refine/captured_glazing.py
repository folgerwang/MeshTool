"""Transfer observed pane appearance onto existing PCG glass, without remeshing.

Only the building's explicitly associated source objects vote. Unsupported panes
retain their previous material. This is captured appearance, not recovered PBR
albedo: baked reflections remain in the texture under the native glass shader.
"""
import argparse
import copy
import hashlib
import json
import shutil
from pathlib import Path

import cv2
import numpy as np
import building
import mtscene
import raster
import refine
from rules_config import RULES, snapshot


def pane_components(points):
    """Join triangles along shared edges so a pane is never half retextured."""
    parent=list(range(len(points)));edges={}
    def root(i):
        while parent[i]!=i:
            parent[i]=parent[parent[i]];i=parent[i]
        return i
    for i,triangle in enumerate(np.round(points,6)):
        vertices=[tuple(p) for p in triangle]
        for a,b in ((0,1),(1,2),(2,0)):
            edge=tuple(sorted((vertices[a],vertices[b])))
            if edge in edges:parent[root(i)]=root(edges[edge])
            else:edges[edge]=i
    return np.array([root(i) for i in range(len(points))])


def complete_panes(ids, components):
    accepted=np.zeros(len(components),bool);accepted[ids]=True
    partial=np.unique(components[~accepted])
    return ids[~np.isin(components[ids],partial)]


def covered_triangles(xy, mask, minimum):
    """Measure all rasterized pixels, including capture holes inside a pane."""
    result=[]
    for triangle in xy:
        low=np.floor(triangle.min(0)).astype(int);high=np.ceil(triangle.max(0)).astype(int)
        if np.any(low<0) or high[0]>=mask.shape[1] or high[1]>=mask.shape[0]:
            result.append(False);continue
        region=np.zeros((high[1]-low[1]+1,high[0]-low[0]+1),np.uint8)
        cv2.fillConvexPoly(region,np.round(triangle-low).astype(np.int32),1)
        samples=mask[low[1]:high[1]+1,low[0]:high[0]+1][region>0]
        result.append(bool(samples.size and samples.mean()>=minimum))
    return np.asarray(result,bool)


def transfer(own, textures, catalog, source, source_textures, directory):
    cfg=RULES['captured_glazing'];directory=Path(directory)
    normals=np.cross(source.pos[:,1]-source.pos[:,0],source.pos[:,2]-source.pos[:,0])
    normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-12)
    pending={};components={}
    for mi,m in enumerate(own):
        if m.material!=1:continue
        if any(primitive!=4 for primitive,_ in m.draw_calls):continue
        triangles=np.concatenate([indices.reshape(-1,3) for _,indices in m.draw_calls])
        pending[mi]=[triangles,np.ones(len(triangles),bool)]
        components[mi]=pane_components(m.vertices[triangles])
    replacements={};reports=[]
    for f in catalog['faces']:
        if f.get('window_finish')=='procedural':
            reports.append(dict(face_id=f['face_id'],mapped_glass_triangles=0,status='preserve_procedural_glass'))
            continue
        if (f.get('rectification') or {}).get('status')=='rectified':
            reports.append(dict(face_id=f['face_id'],mapped_glass_triangles=0,status='preserve_rectified_capture_glass'))
            continue
        face=building.Face(np.empty((0,3,3)),*[np.array(f[k]) for k in ('origin','u','v','normal')],f['size'],'wall')
        selected=abs(normals@face.normal)>=cfg['normal_alignment']
        near=raster.SceneTris(*[getattr(source,k)[selected] for k in ('pos','uv','tex','obj','mesh')])
        pitch=max(cfg['texel_m'],max(face.size)/2040)
        rgb,_,mask=refine.project_face(face,near,source_textures,pitch,(-cfg['depth_band_m'],cfg['depth_band_m']),nearest_plane=True)
        mask &= ~raster.yellowish(rgb)
        mapped=[]
        for mi,(triangles,available) in pending.items():
            m=own[mi];points=m.vertices[triangles].astype(float)+m.translation
            relative=points-face.origin
            n=np.cross(points[:,1]-points[:,0],points[:,2]-points[:,0]);n/=np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-12)
            hit=available & (np.max(abs(relative@face.normal),axis=1)<=cfg['pane_plane_tolerance_m']) & (abs(n@face.normal)>.999)
            ids=np.flatnonzero(hit)
            if not len(ids):continue
            xy=np.stack([relative[ids]@face.u/pitch,rgb.shape[0]-relative[ids]@face.v/pitch],axis=-1)
            ids=ids[covered_triangles(xy,mask,cfg['min_observed_fraction'])]
            ids=complete_panes(ids,components[mi])
            if len(ids):mapped.append((mi,ids))
        count=sum(len(ids) for _,ids in mapped)
        reports.append(dict(face_id=f['face_id'],capture_coverage=float(mask.mean()),mapped_glass_triangles=count))
        if not count:continue
        rgb=refine.fill_holes(rgb,mask)
        h,w=rgb.shape[:2];H,W=(h+3)//4*4,(w+3)//4*4
        padded=cv2.copyMakeBorder(rgb,0,H-h,0,W-w,cv2.BORDER_REPLICATE)
        texture=len(textures)
        textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(W,H,raster.encode_dxt1(padded))]))
        cv2.imwrite(str(directory/f"face_{f['face_id']:03d}_glass_evidence.png"),cv2.cvtColor(rgb,cv2.COLOR_RGB2BGR))
        cv2.imwrite(str(directory/f"face_{f['face_id']:03d}_glass_observed.png"),mask.astype(np.uint8)*255)
        for mi,ids in mapped:
            triangles,available=pending[mi];available[ids]=False
            m=copy.copy(own[mi]);relative=m.vertices.astype(float)+m.translation-face.origin
            m.uvs=np.stack([relative@face.u/pitch/W,(h-relative@face.v/pitch)/H],axis=-1).astype(np.float32)
            m.draw_calls=[(4,triangles[ids].ravel())];m.tex_index=texture;m.colors=None
            replacements.setdefault(mi,[]).append(m)
    result=[];roles=[];old_roles={r['local_mesh_index']:r['part'] for r in catalog['semantic_meshes']}
    for mi,m in enumerate(own):
        pieces=replacements.get(mi,[])
        if mi not in pending or pending[mi][1].all():pieces=[m]
        elif pending[mi][1].any():
            fallback=copy.copy(m);triangles,available=pending[mi];fallback.draw_calls=[(4,triangles[available].ravel())];pieces=pieces+[fallback]
        for piece in pieces:
            if mi in old_roles:roles.append(dict(local_mesh_index=len(result),part=old_roles[mi]))
            result.append(piece)
    catalog['semantic_meshes']=roles
    catalog['captured_glazing']=dict(faces=reports,policy='observed_capture_on_existing_native_glass',geometry='unchanged')
    return result,catalog['captured_glazing']


def run(base,source_path,output,names=None):
    base,source_path,output=map(Path,(base,source_path,output))
    output.mkdir(parents=True,exist_ok=False)
    inputs={str(p.resolve()):hashlib.sha256(p.read_bytes()).hexdigest() for p in (base/'city.mtscene',source_path)}
    scene=mtscene.load(base/'city.mtscene');source=mtscene.load(source_path)
    manifest=json.loads((base/'manifest.json').read_text());reports={r['building']:r for r in manifest['buildings']}
    summary=[]
    for bi,batch in enumerate(scene):
        for gi,g in enumerate(batch.groups):
            sg=source[bi].groups[gi];src=raster.collect(sg);decoded=raster.decode_textures(sg,size=512)
            source_ids={o.name:i for i,o in enumerate(sg.objects)};result=[]
            for oid,obj in enumerate(g.objects):
                own=[m for m in g.meshes if m.object_id==oid];directory=base/obj.name
                if obj.cls!=3 or (names is not None and obj.name not in names) or not (directory/'tiles.json').exists():result+=own;continue
                shutil.copytree(directory,output/obj.name)
                catalog=json.loads((directory/'tiles.json').read_text());report=reports[obj.name]
                members=report.get('source_members',[obj.name]);ids=[source_ids[n] for n in members if n in source_ids]
                selected=np.isin(src.obj,ids)&(src.tex>=0)
                subset=raster.SceneTris(*[getattr(src,k)[selected] for k in ('pos','uv','tex','obj','mesh')])
                updated,evidence=transfer(own,g.textures,catalog,subset,decoded,output/obj.name)
                result+=updated;report['captured_glazing']=evidence;report['catalog']=str(output/obj.name/'tiles.json')
                (output/obj.name/'tiles.json').write_text(json.dumps(catalog,indent=2))
                count=sum(f['mapped_glass_triangles'] for f in evidence['faces'])
                summary.append(dict(building=obj.name,mapped_glass_triangles=count))
                print(obj.name,count,'glass triangles retain captured appearance',flush=True)
            g.meshes=result
    mtscene.save(output/'city.mtscene',scene)
    manifest['captured_glazing_pass']=dict(inputs=inputs,config=snapshot(output),buildings=summary,
        code_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    for p,digest in inputs.items():
        if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=digest:raise RuntimeError('Input modified: '+p)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('source');p.add_argument('output');p.add_argument('--only');a=p.parse_args()
    run(a.base,a.source,a.output,a.only.split(',') if a.only else None)
