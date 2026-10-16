"""Lighten asphalt and fit shared road/terrain boundary vertices to cubic splines."""
import argparse,copy,hashlib,json
from collections import defaultdict
from pathlib import Path
import numpy as np
from scipy.interpolate import splprep,splev
import mtscene,raster

CONFIG=Path(__file__).with_suffix('.json')

def smooth_boundary(points,triangles,road,cfg):
    edges=np.concatenate([triangles[:,[0,1]],triangles[:,[1,2]],triangles[:,[2,0]]])
    labels=np.tile(road,3);edges.sort(axis=1)
    unique,inverse,count=np.unique(edges,axis=0,return_inverse=True,return_counts=True)
    road_count=np.bincount(inverse,weights=labels,minlength=len(unique))
    seam=unique[(count==2)&(road_count==1)]
    graph=defaultdict(list)
    for a,b in seam:graph[int(a)].append(int(b));graph[int(b)].append(int(a))
    visited=set();paths=[]
    starts=sorted(graph,key=lambda k:len(graph[k])==2)
    for start in starts:
        for nxt in graph[start]:
            edge=tuple(sorted((start,nxt)))
            if edge in visited:continue
            path=[start];prev,cur=start,nxt;visited.add(edge)
            while True:
                path.append(cur)
                if cur==start or len(graph[cur])!=2:break
                other=next(v for v in graph[cur] if v!=prev)
                edge=tuple(sorted((cur,other)))
                if edge in visited:break
                visited.add(edge);prev,cur=cur,other
            paths.append(path)
    target=points.copy();fitted=0
    for path in paths:
        if len(path)<6:continue
        ids=np.array(path);p=points[ids];closed=path[0]==path[-1]
        distance=np.r_[0,np.cumsum(np.linalg.norm(np.diff(p[:,:2],axis=0),axis=1))]
        if np.any(np.diff(distance)<1e-6):continue
        u=distance/distance[-1]
        try:
            tck,_=splprep(p[:,:2].T,u=u,s=len(p)*cfg['spline_error_m']**2,per=closed,k=3)
            xy=np.asarray(splev(u,tck)).T
        except ValueError:continue
        delta=xy-p[:,:2];length=np.linalg.norm(delta,axis=1)
        delta*=np.minimum(1,cfg['max_edge_shift_m']/np.maximum(length,1e-9))[:,None]
        if not closed:delta[[0,-1]]=0
        target[ids,:2]=p[:,:2]+delta;fitted+=1
    def area(q):
        t=q[triangles,:2];a=t[:,1]-t[:,0];b=t[:,2]-t[:,0]
        return a[:,0]*b[:,1]-a[:,1]*b[:,0]
    original=area(points);delta=target-points
    # Local damping keeps narrow terrain/road triangles from folding while
    # moving each shared vertex identically on both sides of the seam.
    for _ in range(24):
        current=area(points+delta)
        bad=(abs(original)>1e-8)&(current* np.sign(original)<abs(original)*cfg['minimum_triangle_area_ratio'])
        if not bad.any():break
        delta[np.unique(triangles[bad])]*=.5
    if bad.any():raise ValueError('Road boundary smoothing would invert triangles')
    return points+delta,dict(boundary_edges=len(seam),spline_paths=fitted,
        moved_vertices=int((np.linalg.norm(delta,axis=1)>1e-5).sum()),max_shift_m=float(np.linalg.norm(delta,axis=1).max()))

def finish(group,cfg):
    road_ids={i for i,o in enumerate(group.objects) if o.name.startswith('PCG road asphalt')}
    meshes=[m for m in group.meshes if group.objects[m.object_id].cls in (1,2,6,7)]
    if not road_ids or not meshes:return {}
    arrays=[m.vertices.astype(float)+m.translation for m in meshes]
    points,inverse=np.unique(np.round(np.concatenate(arrays),5),axis=0,return_inverse=True)
    triangles=[];labels=[];offset=0;maps=[]
    for m,p in zip(meshes,arrays):
        ids=inverse[offset:offset+len(p)];maps.append(ids);offset+=len(p)
        for primitive,index in m.draw_calls:
            if primitive!=4:raise ValueError('Expected terrain triangles')
            triangles.append(ids[index].reshape(-1,3));labels.extend([m.object_id in road_ids]*(len(index)//3))
    moved,report=smooth_boundary(points,np.concatenate(triangles),np.array(labels),cfg)
    displacement=moved-points
    for m,old,ids in zip(meshes,arrays,maps):m.vertices=(old+displacement[ids]-m.translation).astype(np.float32)
    textures={m.tex_index for m in meshes if m.object_id in road_ids}
    for ti in textures:
        t=copy.deepcopy(group.textures[ti]);mips=[]
        for w,h,payload in t.mips:
            if t.internal_format!=raster.GL_COMPRESSED_RGB_S3TC_DXT1:raise ValueError('Expected DXT1 asphalt')
            im=raster.decode_dxt1(w,h,payload);rgb=im.astype(float)
            distance=np.linalg.norm(rgb-np.array(cfg['previous_asphalt_rgb']),axis=2)
            weight=np.clip((25-distance)/10,0,1)
            rgb+=weight[...,None]*(np.array(cfg['asphalt_rgb'])-cfg['previous_asphalt_rgb'])
            mips.append((w,h,raster.encode_dxt1(np.clip(rgb,0,255).astype(np.uint8))))
        t.mips=mips;new=len(group.textures);group.textures.append(t)
        for m in meshes:
            if m.object_id in road_ids and m.tex_index==ti:m.tex_index=new
    report['asphalt_rgb']=cfg['asphalt_rgb'];return report

def run(source,output):
    source,output=Path(source),Path(output)
    output.mkdir(parents=True,exist_ok=False);cfg=json.loads(CONFIG.read_text())
    digest=hashlib.sha256(source.read_bytes()).hexdigest();scene=mtscene.load(source)
    reports=[finish(g,cfg) for b in scene for g in b.groups]
    mtscene.save(output/'city.mtscene',scene)
    assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
    (output/'road_finish.json').write_text(json.dumps(cfg,indent=2))
    (output/'manifest.json').write_text(json.dumps(dict(source=str(source.resolve()),source_sha256=digest,groups=reports),indent=2))
    print(reports)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('output');a=p.parse_args();run(a.source,a.output)
