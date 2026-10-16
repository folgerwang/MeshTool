"""Solid-first reconstruction: closed shells, clean roofs, no retained capture shards."""
import argparse,copy,json,hashlib
from pathlib import Path
from collections import Counter
import cv2,numpy as np,shapely
from shapely.geometry import MultiPoint,Polygon
from shapely.ops import unary_union
from scipy import ndimage
import mtscene,raster,building
from road_network import corridors


def polygons(geometry):
    if geometry.is_empty:return []
    if geometry.geom_type=='Polygon':return [geometry]
    return [p for g in getattr(geometry,'geoms',[]) for p in polygons(g)]


def atlas(color,stagger=False):
    n=256;y,x=np.mgrid[:n,:n];row=y//128
    # Two bays and two floors per repeating tile, allowing half-bay row shifts.
    u=(x+np.where((row%2==1)&stagger,64,0))%128;v=y%128
    image=np.empty((n,n,3),np.uint8);image[:]=color
    frame=(u>23)&(u<105)&(v>22)&(v<103);pane=(u>27)&(u<101)&(v>26)&(v<99)
    image[frame]=(55,59,60);image[pane]=(65,86,99)
    image[(v<6)]=(np.asarray(color)*.88).astype(np.uint8)
    return mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(n,n,raster.encode_dxt1(image))])


def shell(poly,bottom,top,oid,texture_base=0):
    poly=shapely.geometry.polygon.orient(poly,sign=1)
    center=np.array([poly.centroid.x,poly.centroid.y,(bottom+top)/2]);walls=[];wall_uv=[];roof=[]
    for ring in [poly.exterior,*poly.interiors]:
        xy=np.asarray(ring.coords);distance=0
        for a,b in zip(xy[:-1],xy[1:]):
            length=np.linalg.norm(b-a)
            if length<1e-6:continue
            p=np.array([[*a,bottom],[*b,bottom],[*b,top],[*a,top]])
            walls.append(p[[0,1,2,0,2,3]])
            # Each facade gets a complete number of bays/floors, no cut-off tiles.
            bays=max(1,round(length/2.6));floors=max(1,round((top-bottom)/3.4))
            uv=np.array([[0,0],[bays/2,0],[bays/2,floors/2],[0,floors/2]])
            wall_uv.append(uv[[0,1,2,0,2,3]])
    for triangle in building._triangulate(poly):
        xy=np.asarray(shapely.geometry.polygon.orient(triangle,sign=1).exterior.coords)[:3]
        roof.append(np.c_[xy,np.full(3,top)]);roof.append(np.c_[xy[::-1],np.full(3,bottom)])
    meshes=[]
    for ti,parts,uv in ((texture_base,walls,np.concatenate(wall_uv)),(texture_base+1,roof,None)):
        points=np.concatenate(parts)
        if uv is None:uv=(points[:,:2]-center[:2])/8
        meshes.append(mtscene.Mesh(ti,oid,center,(points-center).astype(np.float32),np.asarray(uv,np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))],0,0))
    return meshes


def audit_shell(meshes):
    triangles=np.concatenate([(m.vertices[idx].astype(float)+m.translation).reshape(-1,3,3) for m in meshes for prim,idx in m.draw_calls if prim==4])
    points=np.round(triangles.reshape(-1,3),4);_,inverse=np.unique(points,axis=0,return_inverse=True);tri=inverse.reshape(-1,3)
    edges=np.concatenate([tri[:,[0,1]],tri[:,[1,2]],tri[:,[2,0]]]);edges.sort(axis=1)
    _,counts=np.unique(edges,axis=0,return_counts=True)
    area=np.linalg.norm(np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]),axis=1)
    return dict(boundary_edges=int((counts==1).sum()),nonmanifold_edges=int((counts>2).sum()),degenerate_triangles=int((area<1e-8).sum()))


def roof_zones(poly,own,bottom,top):
    if poly.area<1200 or top-bottom<35:return [(poly,top)]
    source=mtscene.Group([],own,[]);tris=raster.collect(source);tris.tex[:]=-1
    ortho=raster.top_down(tris,np.zeros((1,256,256,3),np.uint8),1.)
    valid=np.isfinite(ortho.height)&(ortho.height>bottom+2)
    if valid.sum()<30:return [(poly,top)]
    _,indices=ndimage.distance_transform_edt(~valid,return_indices=True)
    heights=ndimage.median_filter(ortho.height[tuple(indices)],size=5)
    h,w=heights.shape;mask=np.zeros((h,w),np.uint8)
    def pixels(ring):
        points=np.asarray(ring.coords);return np.rint(np.c_[points[:,0]-ortho.min_x,ortho.max_y-points[:,1]]).astype(np.int32)
    cv2.fillPoly(mask,[pixels(poly.exterior)],1)
    for ring in poly.interiors:cv2.fillPoly(mask,[pixels(ring)],0)
    values=heights[mask>0].astype(np.float32)
    if len(values)<30 or np.percentile(values,90)-np.percentile(values,10)<12:return [(poly,top)]
    cv2.setRNGSeed(17)
    _,_,centers=cv2.kmeans(values[:,None],3,None,(cv2.TERM_CRITERIA_EPS+cv2.TERM_CRITERIA_MAX_ITER,50,.1),3,cv2.KMEANS_PP_CENTERS)
    levels=sorted(float(v) for v in centers.ravel());levels=[v for i,v in enumerate(levels) if i==0 or v-levels[i-1]>7]
    assignment=np.argmin(abs(heights[...,None]-np.array(levels)),axis=2)
    remaining=poly;zones=[]
    for k in range(len(levels)-1,0,-1):
        contours,_=cv2.findContours(((assignment==k)&(mask>0)).astype(np.uint8),cv2.RETR_EXTERNAL,cv2.CHAIN_APPROX_SIMPLE)
        shapes=[]
        for contour in contours:
            if len(contour)<3:continue
            xy=contour[:,0,:];shape=Polygon(np.c_[ortho.min_x+xy[:,0],ortho.max_y-xy[:,1]]).buffer(0).simplify(.8,preserve_topology=True)
            if shape.area>40:shapes+=polygons(shape)
        if not shapes:continue
        region=remaining.intersection(unary_union(shapes))
        zones.extend((part,max(bottom+2,levels[k])) for part in polygons(region) if part.area>25)
        remaining=remaining.difference(region)
    zones.extend((part,max(bottom+2,levels[0])) for part in polygons(remaining) if part.area>25)
    return zones or [(poly,top)]


def rebuild(source_path,base_path,terrain_manifest,profile_path,out):
    out=Path(out)
    if out.exists():raise ValueError('Use a new directory')
    paths=[Path(p).resolve() for p in (source_path,base_path,terrain_manifest,profile_path)]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    src=mtscene.load(source_path);scene=mtscene.load(base_path);manifest=json.loads(Path(terrain_manifest).read_text());profile=json.loads(Path(profile_path).read_text())
    reports=[]
    for record in manifest['groups']:
        sg=src[record['source_batch']].groups[record['source_group']];group=scene[record['output_batch']].groups[record['output_group']]
        data=np.load(Path(terrain_manifest).parent/record['data']);res=float(data['res'])
        road,_,_=corridors(profile,data)
        contours,_=cv2.findContours(road.astype(np.uint8),cv2.RETR_EXTERNAL,cv2.CHAIN_APPROX_SIMPLE)
        # Convert road raster with holes so city blocks are not cut away.
        contours,hierarchy=cv2.findContours(road.astype(np.uint8),cv2.RETR_CCOMP,cv2.CHAIN_APPROX_SIMPLE)
        def world(contour):
            xy=contour[:,0,:];return np.c_[float(data['min_x'])+xy[:,0]*res,float(data['max_y'])-xy[:,1]*res]
        road_polys=[]
        if hierarchy is not None:
            for i,c in enumerate(contours):
                if hierarchy[0,i,3]!=-1 or len(c)<3:continue
                holes=[world(contours[j]) for j in range(len(contours)) if hierarchy[0,j,3]==i and len(contours[j])>=3]
                road_polys.append(Polygon(world(c),holes).buffer(0))
        road_region=unary_union(road_polys).buffer(-.6)
        # Preserve the accepted refined tower already in the road scene.
        occupied=[]
        for oid,o in enumerate(group.objects):
            if o.cls==3:
                points=np.concatenate([m.vertices+m.translation for m in group.meshes if m.object_id==oid])
                occupied.append(shapely.concave_hull(MultiPoint(np.unique(np.round(points[:,:2],2),axis=0)),ratio=.25))
        records=[]
        for oid,obj in enumerate(sg.objects):
            if obj.cls!=3:continue
            if any(o.name==obj.name and o.cls==3 for o in group.objects):
                reports.append(dict(name=obj.name,status='preserved_refined'));continue
            own=[m for m in sg.meshes if m.object_id==oid]
            points=np.concatenate([m.vertices.astype(float)+m.translation for m in own])
            xy=np.unique(np.round(points[:,:2],1),axis=0)
            if len(xy)<3:continue
            footprint=shapely.concave_hull(MultiPoint(xy),ratio=.18,allow_holes=False)
            if footprint.geom_type!='Polygon':
                reports.append(dict(name=obj.name,status='unresolved_thin_fragment'));continue
            footprint=footprint.simplify(.55,preserve_topology=True).buffer(0)
            px=np.clip(np.rint((points[:,0]-float(data['min_x']))/res).astype(int),0,road.shape[1]-1)
            py=np.clip(np.rint((float(data['max_y'])-points[:,1])/res).astype(int),0,road.shape[0]-1)
            bottom=float(np.percentile(data['height'][py,px],10))-.15;top=float(np.percentile(points[:,2],95))
            records.append(dict(oid=oid,name=obj.name,footprint=footprint,bottom=bottom,top=top,own=own))
        # Larger structures absorb duplicate fragment footprints before small parts.
        records.sort(key=lambda r:r['footprint'].area,reverse=True)
        for rec in records:
            poly=rec['footprint'].difference(road_region)
            if occupied:poly=poly.difference(unary_union(occupied))
            poly=shapely.set_precision(poly,.02).simplify(.04,preserve_topology=True)
            parts=[p for p in polygons(poly) if p.area>=25 and p.minimum_rotated_rectangle.area>0 and p.area/p.length>1.25]
            if rec['top']-rec['bottom']<2 or not parts:
                reports.append(dict(name=rec['name'],status='absorbed_or_unresolved_fragment'));continue
            oid=len(group.objects);group.objects.append(mtscene.SceneObject(rec['name'],3))
            palette=[(165,159,146),(146,153,153),(174,163,149),(139,148,151),(165,169,170)]
            color=palette[rec['oid']%len(palette)];ti=len(group.textures);group.textures.append(atlas(color,rec['oid']%4==0))
            roof=np.full((16,16,3),(92,96,96),np.uint8);group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(16,16,raster.encode_dxt1(roof))]))
            audits=[]
            zones=[]
            for part in parts:zones+=roof_zones(part,rec['own'],rec['bottom'],rec['top'])
            for zone,height in zones:
                zone=shapely.set_precision(zone,.02).simplify(.04,preserve_topology=True)
                for part in polygons(zone):
                    if part.area<1:continue
                    meshes=shell(part,rec['bottom'],height,oid,ti);audit=audit_shell(meshes)
                    if any(audit.values()):raise ValueError(f'Invalid closed shell {rec["name"]}: {audit}')
                    group.meshes+=meshes;occupied.append(part);audits.append(audit)
            reports.append(dict(name=rec['name'],status='closed_shell',parts=len(audits),roof_levels=sorted(set(round(z,2) for _,z in zones)),height=rec['top']-rec['bottom'],audits=audits))
        data.close()
    for p,digest in hashes.items():
        if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=digest:raise ValueError('Input changed')
    out.mkdir(parents=True);mtscene.save(out/'city_solid.mtscene',scene)
    report=dict(input_sha256=hashes,counts=dict(Counter(r['status'] for r in reports)),buildings=reports,
                note='Solid-first approximation. Roof shapes and window patterns are simplified; fragment records are retained for later reconstruction rather than rendered as broken independent buildings.')
    (out/'manifest.json').write_text(json.dumps(report,indent=2));print(report['counts'],flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('base');p.add_argument('terrain');p.add_argument('profile');p.add_argument('output');a=p.parse_args();rebuild(a.source,a.base,a.terrain,a.profile,a.output)
