"""Closed stepped envelopes from nested, capture-supported roof footprints."""
import numpy as np
import shapely
from collections import Counter
from shapely.geometry import Polygon
from shapely.ops import unary_union
from building_rules import planar


def polygons(geometry):
    if geometry.is_empty:return []
    if geometry.geom_type=='Polygon':return [geometry]
    return [p for g in getattr(geometry,'geoms',[]) for p in polygons(g)]


def build(layers,bottom):
    """layers is [(roof_z, footprint at that height)], ordered bottom to top.

    Node every roof/wall boundary with the same XY vertices. Independent prism
    meshes would leave internal faces and T-junctions at tower/podium joins.
    Returns world-space triangles and a semantic role per triangle.
    """
    if not layers or bottom>=layers[0][0]:raise ValueError('Invalid stepped envelope heights')
    if any(a[0]>=b[0] for a,b in zip(layers,layers[1:])):raise ValueError('Roof heights must increase')
    normalized=[]
    for height,region in layers:
        # Snap inputs once, then perform overlays in floating precision. Keeping
        # a fixed precision model during differences can move new intersection
        # vertices off an existing sloping boundary and leave hairline seams.
        region=shapely.set_precision(shapely.set_precision(shapely.make_valid(region),.01),0)
        if region.is_empty:continue
        if normalized and region.difference(normalized[-1][1].buffer(.015,join_style=2)).area>.02:
            raise ValueError('Upper footprint must be contained in the lower footprint')
        if normalized:region=region.intersection(normalized[-1][1])
        normalized.append((float(height),region))
    roofs=[]
    for i,(height,region) in enumerate(normalized):
        above=normalized[i+1][1] if i+1<len(normalized) else Polygon()
        roofs.append((height,region.difference(above)))
    geometries=[r for _,r in normalized]+[r for _,r in roofs]
    vertices=np.unique(np.round(np.concatenate([np.asarray(r.coords)[:,:2] for g in geometries for p in polygons(g) for r in [p.exterior,*p.interiors]]),8),axis=0)
    def ring(coords):
        xy=np.asarray(coords)[:,:2];result=[]
        for a,b in zip(xy[:-1],xy[1:]):
            axis=b-a;length=float(axis@axis)
            if length<1e-12:continue
            t=(vertices-a)@axis/length
            dist=np.linalg.norm(vertices-a-t[:,None]*axis,axis=1)
            hit=(t>=-1e-8)&(t<1-1e-8)&(dist<1e-7)
            ids=np.flatnonzero(hit);ids=ids[np.argsort(t[ids])]
            for point in vertices[ids]:
                if not result or np.linalg.norm(point-result[-1])>1e-7:result.append(point)
        if len(result)>1 and np.linalg.norm(result[0]-result[-1])<1e-7:result.pop()
        return result
    def noded(region):
        return [Polygon(ring(p.exterior.coords),[ring(r.coords) for r in p.interiors]) for p in polygons(region)]
    triangles=[];roles=[]
    def append(points,role):
        if len(points):triangles.append(points);roles.extend([role]*(len(points)//3))
    for poly in noded(normalized[0][1]):
        append(planar(poly,bottom).reshape(-1,3,3)[:,::-1].reshape(-1,3),'base')
    low=float(bottom)
    for (height,region),(_,roof) in zip(normalized,roofs):
        for poly in noded(region):
            poly=shapely.geometry.polygon.orient(poly,sign=1)
            for boundary in [poly.exterior,*poly.interiors]:
                xy=np.asarray(boundary.coords)
                for a,b in zip(xy[:-1],xy[1:]):
                    if np.linalg.norm(b-a)<1e-7:continue
                    q=np.array([[*a,low],[*b,low],[*b,height],[*a,height]])
                    append(q[[0,1,2,0,2,3]],'wall')
        for poly in noded(roof):append(planar(poly,height),'roof')
        low=height
    points=np.concatenate(triangles)
    # Boolean roof clipping can leave a tiny triangular sliver untriangulated.
    # Close it only when the complete loop lies in a measured horizontal roof.
    tri=np.round(points.reshape(-1,3,3),8)
    edges=Counter(tuple(sorted((tuple(a),tuple(b)))) for t in tri for a,b in zip(t,np.roll(t,-1,axis=0)))
    boundary=[e for e,count in edges.items() if count==1]
    neighbors={}
    for a,b in boundary:
        neighbors.setdefault(a,set()).add(b);neighbors.setdefault(b,set()).add(a)
    filled=set();caps=[]
    for a,adjacent in neighbors.items():
        if len(adjacent)!=2:continue
        b,c=sorted(adjacent);key=tuple(sorted((a,b,c)))
        if key in filled or c not in neighbors.get(b,set()):continue
        if max(a[2],b[2],c[2])-min(a[2],b[2],c[2])>1e-7:continue
        patch=Polygon([a[:2],b[:2],c[:2]])
        if patch.area<=1e-8 or patch.area>1.:continue
        if not any(abs(z-a[2])<1e-7 and region.buffer(1e-6).covers(patch) for z,region in normalized):continue
        cap=np.array([a,b,c]);normal=np.cross(cap[1]-cap[0],cap[2]-cap[0])
        if normal[2]<0:cap=cap[::-1]
        caps.append(cap);roles.append('roof');filled.add(key)
    if caps:points=np.concatenate([points,*caps])
    return points,roles
