"""Reconstruct roads and inferred static signals from saved terrain evidence.
Usage: python road_scene.py COMPOSED_SCENE TERRAIN_MANIFEST NEW_DIRECTORY
Signals and markings are procedural estimates, not surveyed traffic controls.
"""
import argparse,copy,json,hashlib
from pathlib import Path
from collections import defaultdict
import cv2
import numpy as np
from scipy import ndimage
from skimage.morphology import skeletonize
import mtscene,raster
from pcg import _CORNERS,_TRI


def network(data):
    res=float(data['res'])
    road=(data['source_class']==2)&data['cover']
    road=ndimage.binary_closing(road,iterations=max(1,round(2/res)))
    road=ndimage.binary_opening(road,iterations=max(1,round(1.5/res)))
    labels,n=ndimage.label(~road);sizes=np.bincount(labels.ravel())
    edge=np.unique(np.r_[labels[0],labels[-1],labels[:,0],labels[:,-1]])
    fill=(sizes<40/res**2);fill[edge]=False;road|=fill[labels]
    labels,n=ndimage.label(road);sizes=np.bincount(labels.ravel());road&=sizes[labels]>100/res**2
    road &= data['cover'] & (data['source_class']!=3)
    sk=skeletonize(road)
    # Remove short dead ends caused by parked cars and ragged segmentation.
    for _ in range(5):
        degree=ndimage.convolve(sk.astype(np.int16),np.ones((3,3),np.int16))-sk
        remove=[]
        for y,x in zip(*np.nonzero(sk&(degree==1))):
            path=[];prev=None;cur=(y,x)
            while True:
                path.append(cur)
                yy,xx=cur
                neighbors=[(yy+dy,xx+dx) for dy in (-1,0,1) for dx in (-1,0,1)
                           if (dx or dy) and 0<=yy+dy<sk.shape[0] and 0<=xx+dx<sk.shape[1]
                           and sk[yy+dy,xx+dx] and (yy+dy,xx+dx)!=prev]
                if len(neighbors)!=1:break
                prev,cur=cur,neighbors[0]
                if len(path)*res>=20:break
            if len(path)*res<20:remove+=path[:-1]
        if not remove:break
        for y,x in remove:sk[y,x]=False
    degree=ndimage.convolve(sk.astype(np.int16),np.ones((3,3),np.int16))-sk
    labels,n=ndimage.label(ndimage.binary_dilation(sk&(degree>=3),iterations=max(1,round(3/res))))
    widths=ndimage.distance_transform_edt(road)*res
    junctions=[]
    for label in range(1,n+1):
        yy,xx=np.nonzero((labels==label)&sk&(degree>=3))
        if not len(xx):continue
        y,x=int(np.median(yy)),int(np.median(xx))
        radius=round(30/res)
        if not(radius<y<road.shape[0]-radius and radius<x<road.shape[1]-radius) or widths[y,x]<4:continue
        # Distinct street approaches on a 30 m circle reject tiny skeleton forks.
        angles=np.linspace(0,2*np.pi,180,endpoint=False)
        hits=road[np.round(y+radius*np.sin(angles)).astype(int),np.round(x+radius*np.cos(angles)).astype(int)]
        starts=np.flatnonzero(hits&~np.roll(hits,1))
        if len(starts)<3 or len(starts)>5:continue
        dirs=[]
        for start in starts:
            count=0
            while count<180 and hits[(start+count)%180]:count+=1
            if count>=7:dirs.append(float(angles[(start+count//2)%180]))
        if len(dirs)<3:continue
        if any(np.hypot(y-j['pixel'][1],x-j['pixel'][0])*res<45 for j in junctions):continue
        junctions.append(dict(pixel=[x,y],approaches=dirs))
    return road,sk,junctions


class Geometry:
    def __init__(self,group,data):
        self.group=group;self.data=data;self.parts=defaultdict(list)
    def z(self,xy):
        d=self.data;xy=np.asarray(xy)
        x=(xy[...,0]-float(d['min_x']))/float(d['res']);y=(float(d['max_y'])-xy[...,1])/float(d['res'])
        return ndimage.map_coordinates(d['height'],[y.ravel(),x.ravel()],order=1,mode='nearest').reshape(x.shape)
    def world(self,x,y):return np.array([float(self.data['min_x'])+x*float(self.data['res']),float(self.data['max_y'])-y*float(self.data['res'])])
    def strip(self,a,b,width,color,offset=.09):
        delta=b-a;length=np.linalg.norm(delta)
        if length<.01:return
        if length>.5:
            count=int(np.ceil(length/.5))
            for i in range(count):self.strip(a+delta*i/count,a+delta*(i+1)/count,width,color,offset)
            return
        n=np.array([-delta[1],delta[0]])/length*width/2
        xy=np.array([a-n,b-n,b+n,a+n]);p=np.c_[xy,self.z(xy)+offset]
        self.parts[color].append(p[[0,1,2,0,2,3]])
    def box(self,center,dims,color):self.parts[color].append(((_CORNERS*np.array(dims))+center)[_TRI])
    def finish(self,name,cls):
        oid=len(self.group.objects);self.group.objects.append(mtscene.SceneObject(name,cls))
        for color,parts in self.parts.items():
            p=np.concatenate(parts);center=p.mean(0);ti=len(self.group.textures)
            image=np.empty((4,4,3),np.uint8);image[:]=color
            self.group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(4,4,raster.encode_dxt1(image))]))
            self.group.meshes.append(mtscene.Mesh(ti,oid,center,(p-center).astype(np.float32),np.zeros((len(p),2),np.float32),None,[(4,np.arange(len(p),dtype=np.uint32))]))
        self.parts.clear()


def continuous_red_lanes(mask,road,res,scale=2):
    """Fit one constant-width stroke to related red evidence across occlusions.

    Merge only aligned pieces in the same road corridor. Parallel lanes and
    unrelated red objects must not merge merely because they are nearby.
    """
    labels,n=ndimage.label(mask&road);pieces=[]
    def fit(points):
        center=np.median(points,axis=0)
        _,_,axes=np.linalg.svd(points-center,full_matrices=False)
        direction=axes[0];normal=np.array([-direction[1],direction[0]])
        along=(points-center)@direction;across=(points-center)@normal
        low,high=np.percentile(along,[1,99])
        width=np.clip(np.percentile(across,90)-np.percentile(across,10),2.5/res,4.2/res)
        return center,direction,normal,low,high,width
    for k,region in enumerate(ndimage.find_objects(labels),1):
        if region is None:continue
        y,x=np.nonzero(labels[region]==k)
        if len(x)*res*res<3:continue
        points=np.c_[x+region[1].start,y+region[0].start].astype(float)
        c,d,v,lo,hi,width=fit(points)
        if (hi-lo)*res<5 or hi-lo<width*1.8:continue
        pieces.append(points)
    # Longest observations establish direction; fragments refine the same stroke.
    pieces.sort(key=len,reverse=True)
    changed=True
    while changed:
        changed=False
        for i in range(len(pieces)):
            c,d,v,lo,hi,width=fit(pieces[i])
            for j in range(i+1,len(pieces)):
                other=pieces[j];c2,d2,_,_,_,_=fit(other)
                if abs(d@d2)<np.cos(np.deg2rad(12)):continue
                if abs((c2-c)@v)*res>2.5:continue
                projection=(other-c)@d
                if max(lo-projection.max(),projection.min()-hi,0)*res>30:continue
                merged=np.concatenate((pieces[i],other));mc,md,mn,ml,mh,mw=fit(merged)
                # Sample the full stroke, including gap, on the road corridor.
                samples=mc+np.linspace(ml,mh,max(2,int(mh-ml)))[:,None]*md
                xy=np.rint(samples).astype(int)
                good=(xy[:,0]>=0)&(xy[:,0]<road.shape[1])&(xy[:,1]>=0)&(xy[:,1]<road.shape[0])
                if not good.all() or road[xy[:,1],xy[:,0]].mean()<.95:continue
                pieces[i]=merged;pieces.pop(j);changed=True;break
            if changed:break
    paint=np.zeros((road.shape[0]*scale,road.shape[1]*scale),np.uint8);lanes=[]
    for points in pieces:
        c,d,n,lo,hi,width=fit(points)
        a,b=c+d*lo,c+d*hi
        polygon=np.array([a-n*width/2,b-n*width/2,b+n*width/2,a+n*width/2])
        cv2.fillConvexPoly(paint,np.rint(polygon*scale).astype(np.int32),255,lineType=cv2.LINE_AA)
        lanes.append(dict(start_pixel=a.tolist(),end_pixel=b.tolist(),width_m=float(width*res),length_m=float((hi-lo)*res)))
    paint &= cv2.resize(road.astype(np.uint8)*255,(paint.shape[1],paint.shape[0]),interpolation=cv2.INTER_NEAREST)
    return paint,lanes


def repair_road_mask(road,data):
    """Close short capture gaps and enclosed vehicle islands, not city blocks."""
    res=float(data['res'])
    radius=max(1,round(3/res));yy,xx=np.mgrid[-radius:radius+1,-radius:radius+1]
    repaired=ndimage.binary_closing(road,structure=xx*xx+yy*yy<=radius*radius)
    repaired|=road
    labels,n=ndimage.label(~repaired);sizes=np.bincount(labels.ravel())
    edge=np.unique(np.r_[labels[0],labels[-1],labels[:,0],labels[:,-1]])
    fill=sizes*res*res<250;fill[edge]=False
    repaired|=fill[labels]
    return repaired&data['cover']


def paint_asphalt(texture,mask,color):
    # Paint retains subtle asphalt grain and anti-aliased edges. It is surface
    # colour in the same atlas, not an opaque raised strip or a second mesh.
    alpha=mask.astype(np.float32)[...,None]/255
    grain=texture.astype(np.float32).mean(2,keepdims=True)-61
    pigment=np.clip(np.asarray(color)+np.clip(grain,-2,2),0,255)
    texture[:]=np.clip(texture*(1-alpha)+pigment*alpha,0,255).astype(np.uint8)


def road_surface(group,data,road,surface=None):
    """Partition existing terrain triangles: each belongs to ground OR road.

    Exact shared vertices preserve the seam. Paint is baked into the asphalt
    atlas, so there is no second coplanar mesh and no depth-bias dependency.
    """
    res=float(data['res']);h,w=road.shape
    scale=2
    texture=np.full((h*scale,w*scale,3),(58,61,65),np.uint8)
    rng=np.random.default_rng(7)
    texture=np.clip(texture.astype(np.int16)+rng.integers(-2,3,texture.shape[:2]+(1,)),0,255).astype(np.uint8)
    counts={};lanes=[]
    if surface is not None:
        rgb=surface.astype(np.float32);r,g,b=rgb.transpose(2,0,1)
        masks={'red':(r>80)&(r>g*1.4)&(r>b*1.25),
               'yellow':(r>130)&(g>110)&(b<g*.72)&(r<g*1.6),
               'white':(rgb.min(2)>160)&(rgb.max(2)-rgb.min(2)<38)}
        inside=ndimage.binary_erosion(road,iterations=max(1,round(.5/res)))
        for name,color in (('red',(143,63,52)),('yellow',(228,195,81)),('white',(218,218,207))):
            if name=='red':
                paint,lanes=continuous_red_lanes(masks[name],road,res,scale)
                paint_asphalt(texture,paint,color);counts[name]=float((paint>0).sum()*(res/scale)**2)
                continue
            mask=masks[name]&inside
            labels,n=ndimage.label(mask);sizes=np.bincount(labels.ravel());paint=np.zeros(texture.shape[:2],np.uint8)
            for k,region in enumerate(ndimage.find_objects(labels),1):
                if region is None or sizes[k]<3:continue
                yy,xx=np.nonzero(labels[region]==k);xx+=region[1].start;yy+=region[0].start
                rect=cv2.minAreaRect(np.c_[xx,yy].astype(np.float32));length,width=sorted(rect[1],reverse=True)
                area=sizes[k]*res*res
                if name=='red':keep=area>=4 and length>width*2
                else:keep=.5<=area<=25 and length*res>=1.3 and width*res<=1.2 and length>max(.5,width)*3
                if not keep:continue
                # Fit straight stroke edges to paint evidence, not to guessed lanes.
                center,dims,angle=rect;dims=(max(dims[0],.6),max(dims[1],.6))
                polygon=np.rint(cv2.boxPoints((center,dims,angle))*scale).astype(np.int32)
                cv2.fillConvexPoly(paint,polygon,255,lineType=cv2.LINE_AA)
            paint &= cv2.resize(road.astype(np.uint8)*255,(w*scale,h*scale),interpolation=cv2.INTER_NEAREST)
            paint_asphalt(texture,paint,color);counts[name]=float((paint>0).sum()*(res/scale)**2)
    ph,pw=(h*scale+3)//4*4,(w*scale+3)//4*4
    texture=cv2.copyMakeBorder(texture,0,ph-h*scale,0,pw-w*scale,cv2.BORDER_REPLICATE)
    ti=len(group.textures)
    group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(pw,ph,raster.encode_dxt1(texture))]))
    oid=len(group.objects);group.objects.append(mtscene.SceneObject('PCG road asphalt and captured paint',2))
    output=[];road_count=0;terrain_count=0
    for mesh in group.meshes:
        if group.objects[mesh.object_id].cls not in (1,2,6,7):output.append(mesh);continue
        ground_draw=[];road_draw=[]
        world=mesh.vertices.astype(np.float64)+mesh.translation
        for primitive,indices in mesh.draw_calls:
            if primitive!=4:raise ValueError('Terrain must be triangulated before road partition')
            triangles=indices.reshape(-1,3);center=world[triangles].mean(1)
            x=np.clip(np.rint((center[:,0]-float(data['min_x']))/res).astype(int),0,w-1)
            y=np.clip(np.rint((float(data['max_y'])-center[:,1])/res).astype(int),0,h-1)
            selected=road[y,x]
            if (~selected).any():ground_draw.append((4,triangles[~selected].ravel()))
            if selected.any():road_draw.append((4,triangles[selected].ravel()))
            road_count+=int(selected.sum());terrain_count+=len(selected)
        if ground_draw:
            ground=copy.copy(mesh);ground.draw_calls=ground_draw;output.append(ground)
        if road_draw:
            asphalt=copy.copy(mesh);asphalt.object_id=oid;asphalt.tex_index=ti;asphalt.material=0
            asphalt.draw_calls=road_draw;asphalt.colors=None
            asphalt.uvs=np.c_[(world[:,0]-float(data['min_x']))/res*scale/pw,(float(data['max_y'])-world[:,1])/res*scale/ph].astype(np.float32)
            output.append(asphalt)
    group.meshes=output
    return dict(terrain_triangles=terrain_count,road_triangles=road_count,paint_m2=counts,red_lanes=lanes,
                seam='Exact original shared vertices; mutually exclusive terrain and road triangles',
                markings='Filtered captured paint baked into road atlas; no overlapping paint geometry')


def rebuild(group,data,surface=None,profile=None):
    road,sk,junctions=network(data);g=Geometry(group,data);res=float(data['res'])
    # Smooth ragged capture edges before partitioning, without covering buildings.
    road=(ndimage.gaussian_filter(road.astype(float),sigma=1.5/res)>.5)&data['cover']&(data['source_class']!=3)
    road=repair_road_mask(road,data)
    from road_network import corridors,building_mask,recover_surface
    excluded=building_mask(group,data)
    road &= ~excluded
    network_report=None
    if profile is not None:
        road,junctions,network_report=corridors(profile,data)
        road &= ~excluded
        road,recovery=recover_surface(road,data,excluded,surface=surface,junctions=junctions)
        network_report['surface_recovery']=recovery
        labels,count=ndimage.label(road);sizes=np.bincount(labels.ravel());sizes[0]=0
        tiny=(sizes*res*res<=1.)&(sizes>0)
        if count:tiny[np.argmax(sizes)]=False
        network_report['discarded_edge_slivers_m2']=float(sizes[tiny].sum()*res*res)
        road[tiny[labels]]=False
        labels,count=ndimage.label(road);sizes=np.bincount(labels.ravel())[1:]
        network_report['final_components']=count
        network_report['final_component_area_m2']=(sizes*res*res).tolist()
        if count!=1:raise ValueError(f'Map road network remains disconnected: {count} components')
    surface_report=road_surface(group,data,road,surface)
    surface_report['network']=network_report
    distance=ndimage.distance_transform_edt(road)*res
    signals=[]
    for j in junctions:
        c=g.world(*j['pixel'])
        for angle in j['approaches']:
            direction=np.array([np.cos(angle),-np.sin(angle)]);side=np.array([-direction[1],direction[0]])
            approach=c+direction*16
            px=round((approach[0]-float(data['min_x']))/res);py=round((float(data['max_y'])-approach[1])/res)
            half=float(np.clip(distance[py,px],3,10))
            # Walk across the carriageway to its actual edge; distance-transform
            # width alone underestimates width on an off-centre skeleton branch.
            edge=half
            for offset in np.arange(half,22.,res):
                trial=approach+side*offset
                tx=round((trial[0]-float(data['min_x']))/res);ty=round((float(data['max_y'])-trial[1])/res)
                if not(0<=ty<road.shape[0] and 0<=tx<road.shape[1]) or not road[ty,tx]:
                    edge=offset;break
            pole=approach+side*(edge+.8)
            ix=round((pole[0]-float(data['min_x']))/res);iy=round((float(data['max_y'])-pole[1])/res)
            if not (0<=iy<road.shape[0] and 0<=ix<road.shape[1] and data['cover'][iy,ix]) or road[iy,ix] or data['source_class'][iy,ix]==3:continue
            z=float(g.z(pole));g.box([*pole,z+2.4],[.18,.18,4.8],(75,78,76))
            g.box([*pole,z+.12],[.45,.45,.24],(140,140,132))
            head= pole-direction*.3
            g.box([*head,z+4.15],[.42,.42,1.15],(35,37,34))
            # Static red signal faces the approach. Signal state is not simulated.
            for dz,color in ((.34,(225,45,28)),(0,(83,65,25)),(-.34,(25,64,39))):
                lamp=head+direction*.24;g.box([*lamp,z+4.15+dz],[.18,.18,.20],color)
            signals.append([float(pole[0]),float(pole[1]),z])
    g.finish('PCG inferred traffic signals',0)
    from road_finish import finish,CONFIG
    surface_report['finish']=finish(group,json.loads(CONFIG.read_text()))
    return dict(road_area_m2=float(road.sum()*res*res),junctions=junctions,signal_positions=signals,
                signal_count=len(signals),surface=surface_report,note='Road graph and height-validated capture evidence determine coverage; paint is filtered from the captured surface. Static signal placements are inferred, not surveyed. No traffic simulation.'),road


def restore_ground(group,clean):
    """Reset prior road partition from its saved terrain, preserving placed assets."""
    group.meshes=[m for m in group.meshes if group.objects[m.object_id].cls not in (1,2,6,7)
                  and group.objects[m.object_id].name!='PCG inferred traffic signals']
    offset=len(group.textures);object_offset=len(group.objects)
    group.textures.extend(copy.deepcopy(clean.textures));group.objects.extend(copy.deepcopy(clean.objects))
    for original in clean.meshes:
        mesh=copy.copy(original);mesh.object_id+=object_offset
        if mesh.tex_index<len(clean.textures):mesh.tex_index+=offset
        group.meshes.append(mesh)


def export(scene_path,manifest_path,destination,network_path=None,replace_existing=False):
    scene_path=Path(scene_path).resolve();manifest_path=Path(manifest_path).resolve();destination=Path(destination).resolve()
    if destination.exists():raise ValueError('Use a new output directory')
    manifest=json.loads(manifest_path.read_text());scene=mtscene.load(scene_path);records=[]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (scene_path,manifest_path)}
    profile=None;capture=None;clean=None;masks=[]
    if network_path:
        network_path=Path(network_path).resolve();profile=json.loads(network_path.read_text())
        if profile['source_sha256']!=manifest['source_sha256']:raise ValueError('Road network belongs to a different capture')
        hashes[str(network_path)]=hashlib.sha256(network_path.read_bytes()).hexdigest()
    for record in manifest['groups']:
        batch=scene[record['output_batch']]
        if list(batch.reference_pos)!=record['reference_pos']:raise ValueError('Coordinate reference mismatch')
        group=batch.groups[record['output_group']]
        if any(o.name.startswith('PCG road asphalt') for o in group.objects):
            if not replace_existing:raise ValueError('Use --replace-existing to rebuild roads from saved terrain')
            clean_path=manifest_path.parent/manifest['scene']
            hashes[str(clean_path)]=hashlib.sha256(clean_path.read_bytes()).hexdigest()
            if clean is None:clean=mtscene.load(clean_path)
            restore_ground(group,clean[record['output_batch']].groups[record['output_group']])
        data_path=manifest_path.parent/record['data']
        hashes[str(data_path)]=hashlib.sha256(data_path.read_bytes()).hexdigest()
        with np.load(data_path) as loaded:data={k:loaded[k] for k in loaded.files}
        if profile is not None and 'captured_height' not in data:
            source_path=Path(manifest['source']);digest=hashlib.sha256(source_path.read_bytes()).hexdigest()
            if digest!=manifest['source_sha256']:raise ValueError('Original capture fingerprint mismatch')
            hashes[str(source_path)]=digest
            if capture is None:capture=mtscene.load(source_path)
            source=capture[record['source_batch']].groups[record['source_group']]
            triangles=raster.collect(source);triangles.tex[:]=-1
            top=raster.top_down(triangles,np.zeros((1,256,256,3),np.uint8),float(data['res']))
            if top.height.shape!=data['cover'].shape or not np.allclose([top.min_x,top.max_y],[data['min_x'],data['max_y']]):raise ValueError('Capture height registration mismatch')
            data['captured_height']=top.height
        surface_path=manifest_path.parent/(Path(record['data']).stem.replace('_terrain','_surface')+'.png')
        surface=cv2.imread(str(surface_path))
        if surface is None:raise ValueError('Captured terrain surface image is required for road paint')
        hashes[str(surface_path)]=hashlib.sha256(surface_path.read_bytes()).hexdigest()
        report,mask=rebuild(group,data,cv2.cvtColor(surface,cv2.COLOR_BGR2RGB),profile);records.append(report);masks.append(mask)
    destination.mkdir(parents=True);mtscene.save(destination/'roads_pcg.mtscene',scene)
    for i,mask in enumerate(masks):cv2.imwrite(str(destination/f'road_mask_{i}.png'),mask.astype(np.uint8)*255)
    (destination/'manifest.json').write_text(json.dumps(dict(input_sha256=hashes,terrain_manifest=str(manifest_path),groups=records),indent=2))
    for p,digest in hashes.items():
        assert hashlib.sha256(Path(p).read_bytes()).hexdigest()==digest
    print(json.dumps([dict(road_area_m2=r['road_area_m2'],junctions=len(r['junctions']),signals=r['signal_count']) for r in records]),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('scene');p.add_argument('terrain_manifest');p.add_argument('output_directory');p.add_argument('--network');p.add_argument('--replace-existing',action='store_true');a=p.parse_args();export(a.scene,a.terrain_manifest,a.output_directory,a.network,a.replace_existing)
