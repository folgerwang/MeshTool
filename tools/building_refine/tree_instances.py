"""Capture-fitted placements of shared tree prototypes, plus native glTF instances."""
import copy,json
from pathlib import Path
import cv2
import numpy as np
from scipy import ndimage
from skimage.segmentation import watershed
import mtscene
from trees import points,surface,palette


def prototypes():
    """Unit crowns; placements scale their exact bounding boxes in metres."""
    result=[]
    for variant in range(3):
        pieces=[]
        # Overlapping rounded leaf clusters, with an open trunk beneath them.
        clusters=[(0,0,.67,.35,.38,.32)]
        for k in range(6):
            angle=2*np.pi*k/6+variant*.35
            clusters.append((.22*np.cos(angle),.22*np.sin(angle),.54+.09*np.sin(k*2.1+variant),.25,.25,.24))
        for x,y,z,rx,ry,rz in clusters:
            rings=[(z+rz*np.sin(a),rx*np.cos(a),ry*np.cos(a)) for a in np.linspace(-np.pi/2,np.pi/2,7)]
            mesh=surface(rings,0,np.zeros(3),0,segments=10)
            mesh.vertices[:,:2]+=np.array([x,y]);pieces.append(mesh.vertices)
        p=np.concatenate(pieces);low=p.min(0);high=p.max(0)
        p[:,:2]=(p[:,:2]-(low[:2]+high[:2])/2)/(high[:2]-low[:2])
        p[:,2]=.25+(p[:,2]-low[2])/(high[2]-low[2])*.75
        leaves=mtscene.Mesh(0,0,np.zeros(3),p.astype(np.float32),np.zeros((len(p),2),np.float32),None,[(4,np.arange(len(p),dtype=np.uint32))],0,2)
        trunk=surface([(0,0,0),(0,.025,.025),(.64,.014,.014),(.64,0,0)],0,np.zeros(3),1,segments=10)
        result.append([leaves,trunk])
    return result


def sample_ground(data,xy):
    px=(xy[...,0]-float(data['min_x']))/float(data['res'])
    py=(float(data['max_y'])-xy[...,1])/float(data['res'])
    return ndimage.map_coordinates(data['height'],[py.ravel(),px.ravel()],order=1,mode='nearest').reshape(px.shape)


def placements(meshes,data,min_crown=1.3):
    p=points(meshes)
    if len(p)<3:return [],'insufficient capture'
    ground=sample_ground(data,p[:,:2]);above=p[:,2]-ground
    # Segmented tree objects sometimes contain whole facades. Only plausible
    # above-ground canopy samples contribute to a tree placement.
    p=p[(above>1.)&(above<30.)]
    if len(p)<6:return [],'no canopy within 1–30 m of measured terrain'
    low=p.min(0);res=.5;xy=np.floor((p[:,:2]-low[:2])/res).astype(int)
    shape=tuple((xy.max(0)+3)[::-1]);height=np.full(shape,-np.inf)
    np.maximum.at(height,(xy[:,1]+1,xy[:,0]+1),p[:,2])
    mask=np.isfinite(height).astype(np.uint8)
    # Rasterize projected crown triangles as well as their vertices. Coarse
    # captures may have only a few vertices on an otherwise complete canopy.
    for mesh in meshes:
        world=mesh.vertices.astype(float)+mesh.translation
        above=world[:,2]-sample_ground(data,world[:,:2])
        for primitive,index in mesh.draw_calls:
            if primitive!=4:continue
            tri=index.reshape(-1,3)
            tri=tri[((above[tri]>1.)&(above[tri]<30.)).all(axis=1)]
            projected=np.rint((world[tri,:2]-low[:2])/res+1).astype(np.int32)
            for polygon in projected:cv2.fillConvexPoly(mask,polygon,1)
    mask=mask.astype(bool)
    mask=ndimage.binary_closing(mask,iterations=2)|mask
    mask=ndimage.binary_fill_holes(mask)
    labels,n=ndimage.label(mask)
    records=[]
    for label,region in enumerate(ndimage.find_objects(labels),1):
        if region is None:continue
        sub=labels[region]==label
        if sub.sum()*res*res<min_crown**2*.4:continue
        # Split contiguous street-tree groups at crown-distance peaks.
        dist=ndimage.distance_transform_edt(sub)
        peaks=(dist==ndimage.maximum_filter(dist,size=13))&(dist>=min(2.,dist.max()))&sub
        markers,num=ndimage.label(peaks)
        if not num:markers=np.zeros_like(sub,int);markers[np.unravel_index(np.argmax(dist),dist.shape)]=1
        split=watershed(-dist,markers,mask=sub)
        for k in range(1,int(split.max())+1):
            yy,xx=np.nonzero(split==k)
            if len(xx)*res*res<min_crown**2*.4:continue
            x=(xx+region[1].start-1)*res+low[0];y=(yy+region[0].start-1)*res+low[1]
            center=np.array([(x.min()+x.max())/2,(y.min()+y.max())/2])
            width=max(min_crown,x.max()-x.min()+res);depth=max(min_crown,y.max()-y.min()+res)
            # Use captured points inside this watershed region for the top.
            local=xy+1-np.array([region[1].start,region[0].start])
            hit=(local[:,0]>=0)&(local[:,0]<split.shape[1])&(local[:,1]>=0)&(local[:,1]<split.shape[0])
            ids=np.flatnonzero(hit);ids=ids[split[local[ids,1],local[ids,0]]==k]
            if len(ids)<3:continue
            original_center=center.copy()
            if 'source_class' in data:
                gx=int(round((center[0]-float(data['min_x']))/float(data['res'])))
                gy=int(round((float(data['max_y'])-center[1])/float(data['res'])))
                cl=data['source_class'];inside=0<=gy<cl.shape[0] and 0<=gx<cl.shape[1]
                if not inside:continue
                if cl[gy,gx] in (3,7):
                    radius=max(1,round(min(3.,width/2,depth/2)/float(data['res'])))
                    y0,y1=max(0,gy-radius),min(cl.shape[0],gy+radius+1)
                    x0,x1=max(0,gx-radius),min(cl.shape[1],gx+radius+1)
                    allowed=~np.isin(cl[y0:y1,x0:x1],[3,7])&data['cover'][y0:y1,x0:x1]
                    ay,ax=np.nonzero(allowed)
                    if not len(ax):continue
                    distances=(ax+x0-gx)**2+(ay+y0-gy)**2;nearest=int(np.argmin(distances))
                    if distances[nearest]>radius**2:continue
                    center=np.array([float(data['min_x'])+(ax[nearest]+x0)*float(data['res']),float(data['max_y'])-(ay[nearest]+y0)*float(data['res'])])
            base=float(sample_ground(data,center[None])[0]);top=float(np.quantile(p[ids,2],.98));h=top-base
            # Very tall thin remnants beside towers are facade-label leakage,
            # not credible broadleaf street crowns. Reject rather than shrink.
            if h<1.5 or h>22 or h>max(12.,4*min(width,depth)) or max(width,depth)>22:continue
            records.append(dict(position=[*center.tolist(),base],scale=[width,depth,h],captured_top_z=top,anchor_shift_m=float(np.linalg.norm(center-original_center))))
    return records,None if records else 'canopy too sparse or implausible'


def apply(group,data,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    library=prototypes();tex=len(group.textures);group.textures.extend(palette())
    instances=[];records=[];replacement=[]
    for oid,obj in enumerate(group.objects):
        if obj.cls!=5:continue
        own=[m for m in group.meshes if m.object_id==oid]
        source=[m for m in own if m.model_variant==1] or [m for m in own if m.model_variant!=2]
        fits,reason=placements(source,data)
        records.append(dict(object=obj.name,placements=len(fits),reason=reason))
        for fit in fits:
            variant=len(instances)%len(library);instance=dict(source_object=obj.name,prototype=variant,**fit);instances.append(instance)
            for proto in library[variant]:
                m=copy.copy(proto);m.object_id=oid;m.translation=np.array(fit['position']);m.vertices=(proto.vertices*np.array(fit['scale'])).astype(np.float32);m.tex_index=tex+proto.tex_index
                replacement.append(m)
    removed_cars=sum(o.cls==4 and any(m.object_id==i for m in group.meshes) for i,o in enumerate(group.objects))
    group.meshes=[m for m in group.meshes if group.objects[m.object_id].cls not in (4,5)]+replacement
    # Captured trees and cars remain in the untouched source scene; this output
    # contains only replacement trees, including when switching model variants.
    export_gltf(library,instances,output/'trees.gltf')
    catalog=dict(prototype_count=len(library),instances=instances,source_objects=records,removed_car_objects=removed_cars,
                 preview='MeshTool v7 expands instances to meshes; trees.gltf uses shared meshes and per-node transforms')
    (output/'tree_instances.json').write_text(json.dumps(catalog,indent=2))
    return catalog


def export_gltf(library,instances,path):
    path=Path(path);blob=bytearray();views=[];accessors=[];meshes=[]
    for variant,parts in enumerate(library):
        primitives=[]
        for m in parts:
            p=np.asarray(m.vertices,dtype='<f4');tri=p.reshape(-1,3,3)
            normals=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]);normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-9)
            attrs={}
            for name,arr in [('POSITION',p),('NORMAL',np.repeat(normals,3,axis=0).astype('<f4'))]:
                view=len(views);views.append(dict(buffer=0,byteOffset=len(blob),byteLength=arr.nbytes,target=34962));blob.extend(arr.tobytes())
                acc=dict(bufferView=view,componentType=5126,count=len(arr),type='VEC3')
                if name=='POSITION':acc.update(min=arr.min(0).tolist(),max=arr.max(0).tolist())
                attrs[name]=len(accessors);accessors.append(acc)
            primitives.append(dict(attributes=attrs,material=m.tex_index,mode=4))
        meshes.append(dict(name=f'Tree prototype {variant}',primitives=primitives))
    # Root rotates the Z-up scene into glTF's Y-up coordinate system.
    nodes=[dict(name='Z-up city coordinates',rotation=[-2**-.5,0,0,2**-.5],children=list(range(1,len(instances)+1)))]
    nodes += [dict(name=f"{i['source_object']} instance {n}",mesh=i['prototype'],translation=i['position'],scale=i['scale']) for n,i in enumerate(instances)]
    doc=dict(asset=dict(version='2.0',generator='MeshTool capture-fitted tree instances'),scene=0,scenes=[dict(nodes=[0])],nodes=nodes,meshes=meshes,
             buffers=[dict(uri=path.with_suffix('.bin').name,byteLength=len(blob))],bufferViews=views,accessors=accessors,
             materials=[dict(name='Leaves',pbrMetallicRoughness=dict(baseColorFactor=[.17,.32,.10,1],metallicFactor=0,roughnessFactor=.9)),dict(name='Bark',pbrMetallicRoughness=dict(baseColorFactor=[.3,.19,.10,1],metallicFactor=0,roughnessFactor=1))])
    path.write_text(json.dumps(doc));path.with_suffix('.bin').write_bytes(blob)
