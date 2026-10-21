"""Iterate road boundaries and paint against registered capture evidence."""
import argparse,copy,json
from pathlib import Path
import cv2
import numpy as np
from scipy import ndimage
from scipy.interpolate import splprep,splev
from skimage.morphology import skeletonize
import mtscene,raster
from terrain_scene import original_group
from road_scene import road_surface
from road_finish import finish


def evidence(source,terrain_path,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    data=dict(np.load(terrain_path));scene=mtscene.load(source);group=original_group(scene[0].groups[0])
    ground=[m for m in group.meshes if group.objects[m.object_id].cls in (1,2,6,7)]
    used=sorted({m.tex_index for m in ground if m.tex_index<len(group.textures)});remap={v:i for i,v in enumerate(used)}
    meshes=[]
    for mesh in ground:
        m=copy.copy(mesh);m.tex_index=remap.get(m.tex_index,0xffffffff);meshes.append(m)
    g=mtscene.Group([group.textures[i] for i in used],meshes,group.objects)
    src=raster.collect(g);p=src.pos;norm=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);center=p.mean(1)
    ix=(center[:,0]-float(data['min_x']))/float(data['res']);iy=(float(data['max_y'])-center[:,1])/float(data['res'])
    height=ndimage.map_coordinates(data['height'],[iy,ix],order=1,mode='nearest')
    hit=(abs(norm[:,2])>np.linalg.norm(norm,axis=1)*.7)
    subset=raster.SceneTris(*[getattr(src,k)[hit] for k in ('pos','uv','tex','obj','mesh')])
    h,w=data['height'].shape;res=float(data['res'])/2
    bounds=[float(data['min_x']),float(data['max_y'])-h*float(data['res']),float(data['min_x'])+w*float(data['res']),float(data['max_y'])]
    print('Road evidence',len(subset.pos),'triangles',len(used),'textures',flush=True)
    top=raster.top_down(subset,raster.decode_textures(g,size=256),res,bounds=bounds)
    target=cv2.resize(data['height'],(top.height.shape[1],top.height.shape[0]),interpolation=cv2.INTER_LINEAR)
    valid=np.isfinite(top.height)&(abs(top.height-target)<1.5)
    cv2.imwrite(str(output/'capture.png'),cv2.cvtColor(top.color,cv2.COLOR_RGB2BGR))
    cls=np.array([o.cls for o in g.objects]+[0]);classes=cls[top.obj]
    np.savez_compressed(output/'evidence.npz',color=top.color,valid=valid,source_class=classes,res=res)
    print('Saved high-resolution road evidence',flush=True)


def disk(radius,res):
    r=max(1,round(radius/res));y,x=np.mgrid[-r:r+1,-r:r+1];return x*x+y*y<=r*r


def choose_mask(data,excluded,output):
    res=float(data['res']);cover=data['cover'];cls=data['source_class']
    delta=data['captured_height']-data['height']
    trusted=cover&~excluded&np.isfinite(delta)&(abs(delta)<.9)&~np.isin(cls,[3,4,5,7])
    positive=trusted&(cls==2)
    # Only recover under occluders when nearby terrain identifies the surface
    # as road; building footprints are never a permitted growth region.
    occluded=cover&~excluded&np.isin(cls,[4,5])&(data['surface_class']==2)
    records=[];candidates=[]
    for iteration,radius in enumerate((.5,1.,1.5),1):
        seed=positive|occluded
        road=ndimage.binary_closing(seed,structure=disk(radius,res))|seed
        holes,n=ndimage.label(~road);areas=np.bincount(holes.ravel())*res**2
        border=np.unique(np.r_[holes[0],holes[-1],holes[:,0],holes[:,-1]])
        fill=areas<20;fill[border]=False;road|=fill[holes]
        road=ndimage.gaussian_filter(road.astype(float),.5/res)>.5
        road &= cover&~excluded
        labels,n=ndimage.label(road);sizes=np.bincount(labels.ravel());sizes[0]=0
        road &= sizes[labels]*res**2>=20
        tp=(road&positive).sum();fp=(road&trusted&~positive).sum();fn=(~road&positive).sum()
        iou=float(tp/max(1,tp+fp+fn))
        perimeter=float((road^ndimage.binary_erosion(road)).sum()*res)
        score=iou-.02*perimeter/max(1,np.sqrt(road.sum()*res**2))
        record=dict(iteration=iteration,gap_radius_m=radius,observed_road_iou=iou,selection_score=score,boundary_length_m=perimeter,
                    captured_road_recall=float(tp/max(1,positive.sum())),building_overlap_m2=float((road&excluded).sum()*res**2),
                    road_area_m2=float(road.sum()*res**2),inferred_road_m2=float((road&~trusted).sum()*res**2))
        records.append(record);candidates.append((score,road,record))
        cv2.imwrite(str(Path(output)/f'road-mask-{iteration}.png'),road.astype(np.uint8)*255)
    _,road,record=max(candidates,key=lambda x:x[0]);return road,dict(iterations=records,selected_iteration=record['iteration'])


def paint_masks(rgb,road,valid,res,threshold):
    im=rgb.astype(float);r,g,b=im.transpose(2,0,1);gray=im.mean(2)
    # Reject bright cars using the geometric ground mask before colour tests.
    support=road&valid
    local=ndimage.percentile_filter(gray,percentile=35,size=9)
    candidates=dict(red=(r>75)&(r>g*1.35)&(r>b*1.22),
                    green=(g>65)&(g>r*1.15)&(g>b*1.12),
                    yellow=(r>120)&(g>100)&(b<g*.72)&(r<g*1.7),
                    white=(im.min(2)>threshold)&(im.max(2)-im.min(2)<42)&(gray-local>12))
    result={}
    for name,raw in candidates.items():
        mask=raw&support;labels,_=ndimage.label(mask);sizes=np.bincount(labels.ravel());keep=np.zeros(len(sizes),bool)
        for k,region in enumerate(ndimage.find_objects(labels),1):
            if region is None:continue
            yy,xx=np.nonzero(labels[region]==k);area=sizes[k]*res**2
            if name in ('red','green'):ok=area>=.6
            else:
                rect=cv2.minAreaRect(np.c_[xx,yy].astype(np.float32));length,width=sorted(rect[1],reverse=True)
                # Preserve curved marks, lettering, arrows and crosswalk bars.
                ok=.08<=area<=100 and (length*res>=.7 or len(xx)>=5) and width*res<=5
            keep[k]=ok
        result[name]=keep[labels]
    return result


def choose_paint(data,evidence,road,output):
    rgb=evidence['color'];h,w=rgb.shape[:2];res=float(evidence['res'])
    road_hi=cv2.resize(road.astype(np.uint8),(w,h),interpolation=cv2.INTER_NEAREST)>0
    inferred=cv2.resize(data['inferred'].astype(np.uint8),(w,h),interpolation=cv2.INTER_NEAREST)>0
    assets=cv2.resize(np.isin(data['source_class'],[3,4,5]).astype(np.uint8),(w,h),interpolation=cv2.INTER_NEAREST)>0
    valid=evidence['valid']&~assets
    reference=paint_masks(rgb,road_hi,valid,res,165)
    records=[];candidates=[]
    for iteration,threshold in enumerate((165,145,125),1):
        masks=paint_masks(rgb,road_hi,valid,res,threshold)
        # Allow faded extensions only when connected to brighter paint. This
        # avoids increasing recall by painting arbitrary pale asphalt patches.
        labels,n=ndimage.label(masks['white']);supported=np.unique(labels[reference['white']]);supported=supported[supported>0]
        masks['white'] &= np.isin(labels,supported)
        target=reference['white'];detected=masks['white'];near=ndimage.binary_dilation(target,iterations=2)
        recall=float((detected&target).sum()/max(1,target.sum()))
        unsupported=float((detected&~near).sum()/max(1,detected.sum()))
        score=recall-unsupported+.05*min(1.,float((detected&near&~target).sum()/max(1,target.sum())))
        records.append(dict(iteration=iteration,white_threshold=threshold,bright_paint_recall=recall,unsupported_fraction=unsupported,score=score,
                            paint_area_m2={k:float(v.sum()*res**2) for k,v in masks.items()}))
        candidates.append((score,masks,iteration))
    _,masks,selected=max(candidates,key=lambda c:c[0])
    # Repair short occlusions in coloured lanes only along supported strokes.
    # The fit must stay on the road and near the measured coloured pixels.
    from road_scene import continuous_red_lanes
    recovered={}
    for name in ('red','green'):
        fitted,lanes=continuous_red_lanes(masks[name],road_hi,res,scale=1)
        near=ndimage.distance_transform_edt(~masks[name])*res<=5.
        additions=(fitted>0)&near&road_hi&~assets
        recovered[name]=dict(added_area_m2=float((additions&~masks[name]).sum()*res**2),strokes=len(lanes))
        masks[name]|=additions
    # Estimate asphalt palette only from observed, neutral, unpainted ground.
    neutral=rgb.max(2).astype(int)-rgb.min(2)<35
    gray=rgb.mean(2);sample=road_hi&valid&neutral&(gray>40)&(gray<155)
    color=np.median(rgb[sample],axis=0) if sample.any() else np.array([94,97,99])
    texture=np.broadcast_to(color,rgb.shape).copy().astype(float)
    texture+=np.random.default_rng(7).integers(-2,3,rgb.shape[:2])[...,None]
    pigments=dict(red=(150,66,54),green=(62,116,91),yellow=(226,194,83),white=(220,220,210))
    for name,mask in masks.items():
        alpha=mask.astype(float)[...,None]
        texture=texture*(1-alpha)+np.array(pigments[name])*alpha
        cv2.imwrite(str(Path(output)/(name+'-paint-mask.png')),mask.astype(np.uint8)*255)
    texture=np.clip(texture,0,255).astype(np.uint8)
    cv2.imwrite(str(Path(output)/'road-material.png'),cv2.cvtColor(texture,cv2.COLOR_RGB2BGR))
    return texture,dict(iterations=records,selected_iteration=selected,asphalt_rgb=color.tolist(),bounded_lane_recovery=recovered,note='Paint recovered from visible ground; short coloured-lane gaps inferred only along supported strokes.')


def centerlines(road,data):
    """Trace skeleton graph, then fit bounded cubic curves without moving nodes."""
    # Remove short dangling branches created by boundary noise before fitting.
    res=float(data['res']);sk=skeletonize(road)
    for _ in range(8):
        pixels=set(map(tuple,np.argwhere(sk)))
        graph={p:[(p[0]+dy,p[1]+dx) for dy in (-1,0,1) for dx in (-1,0,1) if (dy or dx) and (p[0]+dy,p[1]+dx) in pixels] for p in pixels}
        remove=[]
        for start in [p for p in graph if len(graph[p])==1]:
            path=[start];prev=start;cur=graph[start][0]
            while len(graph[cur])==2 and len(path)*res<12:
                path.append(cur);nxt=next(p for p in graph[cur] if p!=prev);prev,cur=cur,nxt
            if len(path)*res<12:remove+=path
        if not remove:break
        for y,x in remove:sk[y,x]=False
    pixels=set(map(tuple,np.argwhere(sk)));graph={}
    for y,x in pixels:
        graph[y,x]=[(y+dy,x+dx) for dy in (-1,0,1) for dx in (-1,0,1) if (dy or dx) and (y+dy,x+dx) in pixels]
    visited=set();paths=[];res=float(data['res'])
    for start in sorted(graph,key=lambda p:len(graph[p])==2):
        for nxt in graph[start]:
            key=tuple(sorted((start,nxt)))
            if key in visited:continue
            visited.add(key);path=[start];prev,cur=start,nxt
            while True:
                path.append(cur)
                if cur==start or len(graph[cur])!=2:break
                other=next(v for v in graph[cur] if v!=prev);key=tuple(sorted((cur,other)))
                if key in visited:break
                visited.add(key);prev,cur=cur,other
            if len(path)*res<5:continue
            p=np.array(path,dtype=float);q=p.copy()
            if len(p)>4:
                try:
                    tck,u=splprep(p.T,s=len(p)*(.25/res)**2,k=3,per=np.array_equal(p[0],p[-1]));fit=np.array(splev(u,tck)).T
                    delta=fit-p;distance=np.linalg.norm(delta,axis=1);delta*=np.minimum(1,.5/res/np.maximum(distance,1e-9))[:,None];q=p+delta;q[[0,-1]]=p[[0,-1]]
                    ids=np.rint(q).astype(int)
                    if not road[ids[:,0],ids[:,1]].all():q=p
                except ValueError:pass
            xy=np.c_[float(data['min_x'])+q[:,1]*res,float(data['max_y'])-q[:,0]*res]
            z=ndimage.map_coordinates(data['height'],q.T,order=1,mode='nearest')
            paths.append(dict(points=np.c_[xy,z].tolist(),max_fit_shift_m=float(np.linalg.norm(q-p,axis=1).max()*res)))
    return paths


def apply(group,data,evidence,excluded,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    road,mask_report=choose_mask(data,excluded,output)
    texture,paint_report=choose_paint(data,evidence,road,output)
    surface_report=road_surface(group,data,road,None)
    road_id=len(group.objects)-1;mesh=next(m for m in group.meshes if m.object_id==road_id);ti=mesh.tex_index
    h,w=texture.shape[:2];H,W=(h+3)//4*4,(w+3)//4*4
    texture=cv2.copyMakeBorder(texture,0,H-h,0,W-w,cv2.BORDER_REPLICATE)
    group.textures[ti]=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(W,H,raster.encode_dxt1(texture))])
    # road_surface emits the same 2x atlas dimensions as the evidence raster.
    cfg=dict(previous_asphalt_rgb=paint_report['asphalt_rgb'],asphalt_rgb=paint_report['asphalt_rgb'],spline_error_m=.25,max_edge_shift_m=.6,minimum_triangle_area_ratio=.2)
    surface_report['spline_boundary']=finish(group,cfg)
    curves=centerlines(road,data)
    (output/'road_splines.json').write_text(json.dumps(dict(coordinates='Original world metres, Z-up',curves=curves),indent=2))
    report=dict(boundary=mask_report,paint=paint_report,surface=surface_report,centerline_count=len(curves))
    (output/'iterations.json').write_text(json.dumps(report,indent=2));return report


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('terrain');p.add_argument('output');a=p.parse_args();evidence(a.source,a.terrain,a.output)
