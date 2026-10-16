"""Capture-registered, map-reviewed road corridor graph."""
import json
from pathlib import Path
import cv2
import numpy as np
from scipy import ndimage
from shapely.geometry import LineString,Point
from shapely.ops import unary_union


def recover_surface(seed,data,excluded=None,max_growth_m=14.,surface=None,junctions=()):
    """Recover connected carriageway evidence outside nominal map widths.

    The graph seeds connectivity; it is not the final road boundary. Elevation
    rejects road-labelled roofs, and actual building footprints are barriers.
    Bounded growth avoids flooding distant paved plots from noisy labels.
    """
    res=float(data['res']);excluded=np.zeros_like(seed) if excluded is None else excluded
    if 'captured_height' not in data:raise ValueError('Captured height is required for road boundary recovery')
    delta=data['captured_height']-data['height']
    at_ground=np.isfinite(delta)&(delta>=-1.)&(delta<=2.)
    observed=(data['source_class']==2)&data['cover']
    distance,indices=ndimage.distance_transform_edt(~seed,return_indices=True)
    near=distance*res<=max_growth_m
    evidence=observed&at_ground&near&~excluded
    if surface is not None:
        rgb=surface.astype(float);r,g,b=rgb.transpose(2,0,1)
        neutral=rgb.max(2)-rgb.min(2)<45
        seed_gray=rgb.mean(2)[seed&observed&at_ground&neutral]
        limit=float(np.clip(np.percentile(seed_gray,85),105,150)) if len(seed_gray) else 130.
        asphalt=neutral&(rgb.mean(2)<limit)
        paint=((r>80)&(r>1.4*g)&(r>1.25*b))|((r>130)&(g>110)&(b<g*.72))
        # Paint and cars interrupt asphalt evidence. Close only short gaps.
        appearance=ndimage.maximum_filter((asphalt|paint).astype(np.uint8),size=max(1,round(1.5/res)))>0
        evidence &= appearance
        # A street edge has sustained support along its tangent. A compact
        # paved forecourt adjoining a street must not become an extra lane.
        yy,xx=np.indices(seed.shape);dy=yy-indices[0];dx=xx-indices[1]
        length=np.maximum(distance,1.);ty=dx/length;tx=-dy/length
        support=np.zeros(seed.shape,float)
        for offset in np.linspace(-15/res,15/res,13):
            support+=ndimage.map_coordinates(evidence.astype(float),[yy+ty*offset,xx+tx*offset],order=0,mode='constant')/13
        junction_area=np.zeros(seed.shape,np.uint8)
        for junction in junctions:cv2.circle(junction_area,tuple(np.rint(junction['pixel']).astype(int)),round(14/res),1,-1)
        evidence &= (support>=.72)|(distance*res<=2.)|(junction_area>0)
    radius=max(1,round(1.5/res));y,x=np.mgrid[-radius:radius+1,-radius:radius+1]
    disk=x*x+y*y<=radius*radius
    # Small occlusions from cars/markings can bridge; buildings cannot.
    candidate=ndimage.binary_closing(seed|evidence,structure=disk)|(seed|evidence)
    candidate &= data['cover']&near&~excluded
    candidate &= at_ground|seed|((data['source_class']==4)&(delta<3.))
    grown=ndimage.binary_propagation(seed&candidate,mask=candidate)
    smooth=ndimage.gaussian_filter(grown.astype(float),.6/res)>.5
    grown=(smooth|seed)&data['cover']&~excluded
    # Retain only additions connected to the street graph after smoothing.
    grown=ndimage.binary_propagation(seed&grown,mask=grown)
    return grown,dict(method='Map graph seeds plus height, appearance and longitudinal capture support',
        max_growth_m=max_growth_m,observed_road_m2=float(observed.sum()*res**2),
        observed_outside_seed_m2=float((observed&~seed).sum()*res**2),
        rejected_elevated_or_buried_m2=float((observed&~at_ground).sum()*res**2),
        recovered_road_m2=float((grown&~seed).sum()*res**2),
        missed_valid_nearby_evidence_m2=float((evidence&~grown).sum()*res**2))


def building_mask(group,data):
    """Rasterize projected triangles, preserving concavities and courtyards."""
    mask=np.zeros(data['cover'].shape,np.uint8);res=float(data['res'])
    for mesh in group.meshes:
        if group.objects[mesh.object_id].cls!=3:continue
        world=mesh.vertices.astype(float)+mesh.translation
        xy=np.c_[(world[:,0]-float(data['min_x']))/res,(float(data['max_y'])-world[:,1])/res]
        for primitive,indices in mesh.draw_calls:
            if primitive!=4:continue
            tris=xy[indices.reshape(-1,3)]
            a,b=tris[:,1]-tris[:,0],tris[:,2]-tris[:,0]
            keep=abs(a[:,0]*b[:,1]-a[:,1]*b[:,0])>.01
            for tri in tris[keep]:cv2.fillConvexPoly(mask,np.rint(tri).astype(np.int32),1)
    return mask.astype(bool)


def corridors(profile,data):
    registration=profile['registration']
    if list(data['cover'].shape)!=registration['shape']:raise ValueError('Road profile raster dimensions mismatch')
    for key in ('min_x','max_y','res'):
        if not np.isclose(float(data[key]),registration[key],atol=1e-6,rtol=0):raise ValueError('Road profile registration mismatch')
    lines=[LineString(r['points']) for r in profile['roads']]
    # Snap junction endpoints to the measured adjoining street, not just to
    # existing vertices (a T junction often joins the middle of a segment).
    snapped=[]
    for i,line in enumerate(lines):
        coords=list(line.coords)
        for end in (0,-1):
            point=Point(coords[end]);others=[other for j,other in enumerate(lines) if j!=i]
            target=min(others,key=lambda other:other.distance(point))
            if target.distance(point)<=6/float(data['res']):coords[end]=target.interpolate(target.project(point)).coords[0]
        snapped.append(LineString(coords))
    road=np.zeros(data['cover'].shape,np.uint8)
    for line,record in zip(snapped,profile['roads']):
        points=np.rint(line.coords).astype(np.int32)
        cv2.polylines(road,[points],False,255,max(1,round(record['width_m']/float(data['res']))),lineType=cv2.LINE_8)
        radius=max(1,round(record['width_m']/float(data['res'])/2))
        for p in points:cv2.circle(road,tuple(p),radius,255,-1)
    road=(road>0)&data['cover']
    labels,count=ndimage.label(road);sizes=np.bincount(labels.ravel());sizes[0]=0
    # Capture clipping can leave edge slivers: report, don't invent connections
    # outside captured terrain or silently call a disconnected raster connected.
    components=[float(s*float(data['res'])**2) for s in sizes[1:]]
    topology=unary_union(snapped);edges=list(topology.geoms) if hasattr(topology,'geoms') else [topology]
    nodes={}
    for edge in edges:
        points=list(edge.coords)
        for a,b in ((points[0],points[1]),(points[-1],points[-2])):
            key=tuple(np.round(a,3));direction=np.asarray(b)-a
            nodes.setdefault(key,[]).append(float(np.arctan2(direction[1],direction[0])))
    junctions=[dict(pixel=list(p),approaches=angles) for p,angles in nodes.items() if len(angles)>=3]
    report=dict(profile_roads=len(snapped),raster_components=count,component_area_m2=components,
                junction_count=len(junctions),evidence=profile['evidence'],map_url=profile['map_url'],
                registered_centerlines=[dict(id=r['id'],width_m=r['width_m'],points=list(map(list,line.coords))) for r,line in zip(profile['roads'],snapped)])
    return road,junctions,report
