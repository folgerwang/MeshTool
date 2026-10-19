"""Recover continuous facade planes before laying out architectural modules.

Capture triangulation is evidence for the wall envelope, not a window boundary.
Only supported wall planes are rebuilt. Roofs and unsupported surfaces retain
their captured meshes; the caller archives the complete original separately.
"""
import copy
import numpy as np
import shapely
from shapely.geometry import Polygon
from shapely.ops import unary_union
import mtscene
from shapely.affinity import affine_transform


def roof_supported_envelope(planes, points, ground):
    """Complete four observed exterior walls only with a supported roof above.

    Unlike a hull of facade fragments, the roof must cover the proposed plan.
    Four broad, perpendicular wall observations and little interior wall area
    are required, so wings, courtyards and setbacks keep patch reconstruction.
    """
    from shapely.geometry import box
    if len(planes)<4:return None
    cross=np.cross(points[:,1]-points[:,0],points[:,2]-points[:,0])
    area=np.linalg.norm(cross,axis=1)/2;centers=points.mean(1)
    low=float(points[:,:,2].min());high=float(points[:,:,2].max());height=high-low
    if height<8:return None
    roof_ids=(abs(cross[:,2])>1.4*area)&(centers[:,2]>low+.65*height)
    roof=unary_union([Polygon(t[:,:2]) for t in points[roof_ids]])
    if roof.is_empty:return None
    rectangle=roof.minimum_rotated_rectangle
    if rectangle.geom_type!='Polygon' or rectangle.area<25:return None
    corners=np.asarray(rectangle.exterior.coords)[:-1]
    center=np.asarray(rectangle.centroid.coords[0]);selected=[]
    for a,b in zip(corners,np.roll(corners,-1,axis=0)):
        delta=b-a;length=np.linalg.norm(delta);n=np.r_[delta[1],-delta[0],0.]/length
        if n[:2]@((a+b)/2-center)<0:n=-n
        matches=[p for p in planes if p['n']@n>.995
                 and abs(p['offset']-a@n[:2])<max(2.,min(6.,.15*length))
                 and p['region'].bounds[2]-p['region'].bounds[0]>.7*length
                 and p['raw_area']>.08*length*height]
        if not matches:return None
        selected.append(max(matches,key=lambda p:p['raw_area']))
    # Intersect the measured planes, avoiding roof cornice overhang as a wall.
    n=np.array([p['n'][:2] for p in selected]);d=np.array([p['offset'] for p in selected])
    corners=np.array([np.linalg.solve(n[[i,(i+1)%4]],d[[i,(i+1)%4]]) for i in range(4)])
    outline=Polygon(corners)
    if not outline.is_valid or roof.intersection(outline).area<.82*outline.area:return None
    vertical=abs(cross[:,2])<.7*area
    interior=shapely.distance(shapely.points(centers[:,:2]),outline.boundary)>3.
    if area[vertical&interior].sum()>.15*area[vertical].sum():return None
    # The lowest supported eave closes the walls; taller roof structures stay
    # captured rather than being stretched into the exterior facade.
    eave=min(p['region'].bounds[3] for p in selected)
    if all(p['region'].area>.95*(p['region'].bounds[2]-p['region'].bounds[0])*(eave-ground)
           for p in selected):return None
    result=[]
    for p in selected:
        u=p['u'];edge=corners[abs(corners@p['n'][:2]-p['offset'])<1e-5];span=edge@u[:2]
        region=box(float(span.min()),ground,float(span.max()),eave)
        result.append(dict(p,region=region))
    return result


def close_supported_envelope(planes, points, ground):
    """Close context-completed convex towers using their observed upper outline.

    Roof/interior triangles do not vote on facade support. Require 85 percent
    of upper wall area within two metres of the outline; concave buildings and
    poorly supported captures fall back to independent wall patches.
    """
    from shapely.geometry import box
    cross=np.cross(points[:,1]-points[:,0],points[:,2]-points[:,0])
    area=np.linalg.norm(cross,axis=1);centers=points.mean(1)
    low=float(points[:,:,2].min());top=float(points[:,:,2].max());height=top-low
    mask=(abs(cross[:,2])<.35*area)&(centers[:,2]>low+.55*height)&(centers[:,2]<low+.92*height)
    if mask.sum()<8:return None
    outline=shapely.MultiPoint(points[mask,:,:2].reshape(-1,2)).convex_hull.simplify(.6,preserve_topology=True)
    if outline.geom_type!='Polygon':return None
    distances=shapely.distance(shapely.points(centers[mask,:2]),outline.boundary)
    if np.average(distances<2.,weights=area[mask])<.85:return None
    result=[];coords=list(outline.exterior.coords);center=np.array(outline.centroid.coords[0])
    for a,b in zip(coords[:-1],coords[1:]):
        a=np.array(a);b=np.array(b);delta=b-a;length=np.linalg.norm(delta)
        if length<1:return None
        n=np.array([delta[1],-delta[0],0.])/length
        if n[:2]@((a+b)/2-center)<0:n=-n
        u=np.cross([0.,0.,1.],n);offset=float(a@n[:2]);span=np.array([a,b])@u[:2]
        match=max(planes,key=lambda p:p['n']@n)
        wall_top=match['region'].bounds[3]
        region=box(float(span.min()),ground,float(span.max()),wall_top)
        result.append(dict(n=n,u=u,v=np.array([0.,0.,1.]),offset=offset,region=region,raw_area=region.area))
    return result if len(result)>=4 else None


def resolve_overlaps(planes, tolerance):
    """Give overlapping near-parallel wall fits to the best-supported plane.

    Compare actual projected regions, not bounding boxes, so setbacks outside
    the supported wall footprint and openings survive.
    """
    accepted=[];trimmed=0.
    for plane in sorted(planes,key=lambda p:-p['raw_area']):
        region=plane['region']
        for strong in accepted:
            cosine=float(plane['n']@strong['n'])
            if cosine<-.98 or abs(cosine)<1e-4:continue
            # On this wall, strong-wall coordinates are (a*x+b, z).
            a=float(plane['u']@strong['u'])
            b=float((plane['offset']*plane['n'])@strong['u'])
            projected=affine_transform(strong['region'],[1/a,0,0,1,-b/a,0])
            slope=float(plane['u']@strong['n'])
            depth=float(plane['offset']*(plane['n']@strong['n'])-strong['offset'])
            if abs(slope)<1e-8:
                if abs(depth)>tolerance*4:continue
            else:
                # At an angled corner trim only the portion protruding through
                # the stronger wall, stopping exactly at their intersection.
                low=-tolerance*4 if cosine>=.9 else 0.
                x0,x1=sorted([(low-depth)/slope,(tolerance*4-depth)/slope])
                from shapely.geometry import box
                projected=projected.intersection(box(x0,region.bounds[1]-1,x1,region.bounds[3]+1))
            before=region.area
            region=region.difference(projected)
            trimmed+=before-region.area
            if region.is_empty:break
        if not region.is_empty:
            plane=dict(plane,region=region);accepted.append(plane)
    return accepted,trimmed


def rebuild(source, tolerance=.8, recovery=None):
    import facade
    entries=[]
    for mi,m in enumerate(source):
        for di,(prim,indices) in enumerate(m.draw_calls):
            if prim!=4 or len(indices)%3:continue
            idx=indices.reshape(-1,3)
            p=m.vertices[idx].astype(float)+m.translation
            normals=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);area=np.linalg.norm(normals,axis=1)
            for ti in np.flatnonzero((area>.02)&np.isfinite(p).all(axis=(1,2))):
                entries.append((mi,di,int(ti),p[ti],normals[ti]/area[ti],area[ti]/2))
    if not entries:return source,[],{}
    points=np.array([e[3] for e in entries]);normals=np.array([e[4] for e in entries]);weights=np.array([e[5] for e in entries])
    centers=points.mean(1);center=(points.min((0,1))+points.max((0,1)))/2
    # A single captured wall has its center on that wall. Sub-metre scan noise
    # must not flip alternate tiles onto opposing facade planes.
    normals[np.sum((centers-center)*normals,axis=1)<-tolerance]*=-1
    vertical=np.abs(normals[:,2])<.35
    axes=facade.wall_directions(normals[vertical],weights[vertical])
    axes=[a for a in axes if weights[vertical & (normals@a>.94)].sum()>max(8.,weights[vertical].sum()*.035)][:8]
    angles=np.arctan2(normals[vertical,1],normals[vertical,0])
    harmonic=np.sum(weights[vertical]*np.exp(4j*angles))/max(weights[vertical].sum(),1e-9)
    if abs(harmonic)>.8:
        angle=np.angle(harmonic)/4
        axes=[np.array([np.cos(angle+i*np.pi/2),np.sin(angle+i*np.pi/2),0.]) for i in range(4)]
    if not axes:return source,[],{}
    alignment=normals@np.array(axes).T
    labels=alignment.argmax(1)
    remaining=vertical & (alignment.max(1)>.88)
    planes=[]
    for ai,n in enumerate(axes):
        u=np.cross([0.,0.,1.],n);v=np.array([0.,0.,1.])
        for _ in range(12):
            ids=np.flatnonzero(remaining & (labels==ai))
            if not len(ids):break
            depths=centers[ids]@n
            # A weighted histogram finds actual planes rather than averaging
            # the front wall, setbacks and an unrelated parallel wall together.
            low=float(depths.min());bins=np.floor((depths-low)/.25).astype(int)
            hist=np.bincount(bins,weights=weights[ids])
            smoothed=np.convolve(hist,np.ones(5),mode='full')[2:2+len(hist)]
            seed=low+(int(smoothed.argmax())+.5)*.25
            members=ids[np.abs(depths-seed)<=tolerance]
            if not len(members):break
            order=np.argsort(centers[members]@n);ordered=members[order]
            cumulative=np.cumsum(weights[ordered]);offset=float(centers[ordered[np.searchsorted(cumulative,cumulative[-1]/2)]]@n)
            distance=np.max(np.abs(points[ids]@n-offset),axis=1)
            members=ids[(distance<=tolerance*3)&(np.abs(depths-offset)<=tolerance*2)]
            if not len(members):remaining[ids[np.argmin(np.abs(depths-seed))]]=False;continue
            remaining[members]=False
            xy=np.stack([points[members]@u,points[members]@v],axis=-1)
            raw=shapely.make_valid(unary_union([Polygon(t) for t in xy]))
            if raw.area<max(12,weights[vertical].sum()*.0075) or np.ptp(xy[:,:,1])<4 or np.ptp(xy[:,:,0])<2:continue
            # Close sub-module capture cracks, preserving large openings and
            # disconnected wings. Simplification removes triangle-sized notches.
            region=raw.buffer(.35,join_style=2).buffer(-.35,join_style=2).simplify(.18,preserve_topology=True)
            if region.is_empty:continue
            pieces=[]
            for part in facade._polygons(region):
                if part.area<2:continue
                # Near-convex wall boundaries should not inherit scan scratches.
                if part.convex_hull.area<=part.area*1.025 and not part.interiors:part=part.convex_hull
                pieces.append(part)
            region=unary_union(pieces)
            if region.is_empty:continue
            planes.append(dict(n=n,u=u,v=v,offset=offset,region=region,raw_area=raw.area))
    if not planes:return source,[],{}
    added_area=0.
    for plane in planes:
        for patch in (recovery or {}).get('planes',[]):
            if np.dot(plane['n'],patch['normal'])>.9999 and abs(plane['offset']-patch['offset'])<.05:
                before=plane['region'].area
                plane['region']=unary_union([plane['region']]+[Polygon(p) for p in patch['polygons']])
                added_area+=plane['region'].area-before
    replacement_regions=planes
    ground=(recovery or {}).get('ground_z',float(points[:,:,2].min()))
    envelope=roof_supported_envelope(planes,points,ground)
    envelope_method='roof_supported' if envelope else None
    if not envelope and recovery and recovery.get('planes'):
        envelope=close_supported_envelope(planes,points,recovery['ground_z'])
        if envelope:envelope_method='upper_walls'
    if envelope:planes=envelope
    if envelope:overlap_area=0.
    else:planes,overlap_area=resolve_overlaps(planes,tolerance)
    # Remove captured wrinkles and duplicate patches lying on each fitted wall,
    # including tilted little triangles which originally escaped wall detection.
    removed=np.zeros(len(entries),bool)
    # Ownership trimming must not bring replaced capture triangles back.
    for plane in replacement_regions:
        n,u,v,offset=plane['n'],plane['u'],plane['v'],plane['offset']
        near=(np.max(np.abs(points@n-offset),axis=1)<=tolerance*3)&(np.abs(normals[:,2])<.75)
        ids=np.flatnonzero(near)
        if len(ids):
            inside=shapely.covers(plane['region'].buffer(.25),shapely.points(centers[ids]@u,centers[ids]@v))
            removed[ids[inside]]=True
    if envelope:
        # The closed shell owns the facade; only the captured roof remains.
        eave=min(p['region'].bounds[3] for p in envelope)
        roof_detail=(abs(normals[:,2])>=.35)&(centers[:,2]>ground+.65*(eave-ground))
        removed |= (centers[:,2]<eave-1.)&~roof_detail
        removed[roof_detail]=False
    groups=[];proxies=[]
    for plane in planes:
        n,u,v,offset=plane['n'],plane['u'],plane['v'],plane['offset']
        # Scan-shaped tails cannot hold even one window jamb. Suppress these
        # sub-module strips rather than generating long, disconnected fins.
        region=plane['region'].buffer(-.65,join_style=2).buffer(.65,join_style=2).intersection(plane['region'])
        pieces=facade._polygons(region)
        if pieces:
            largest=max(p.area for p in pieces)
            region=unary_union([p for p in pieces if p.area>=max(2.,largest*.03)])
        triangles=[]
        for poly in facade._polygons(shapely.constrained_delaunay_triangles(region)):
            xy=np.asarray(poly.exterior.coords)[:3]
            p=xy[:,0,None]*u+xy[:,1,None]*v+offset*n
            if np.dot(np.cross(p[1]-p[0],p[2]-p[0]),n)<0:p=p[::-1]
            triangles.append(p)
        if not triangles:continue
        p=np.asarray(triangles)
        groups.append(dict(n=n,anchor=offset*n,tri=list(p)))
        tr=p.reshape(-1,3).mean(0)
        proxies.append(mtscene.Mesh(0xffffffff,source[0].object_id,tr,(p.reshape(-1,3)-tr).astype(np.float32),None,None,
                                   [(4,np.arange(p.size//3,dtype=np.uint32))]))
    if envelope_method=='roof_supported':
        # A structural deck underneath the captured roof closes scan holes
        # without flattening or retexturing the dome and rooftop equipment.
        corners=[]
        for i,p in enumerate(envelope):
            q=envelope[(i+1)%len(envelope)]
            corners.append(np.linalg.solve(np.array([p['n'][:2],q['n'][:2]]),[p['offset'],q['offset']]))
        roof_mask=(abs(normals[:,2])>.7)&(centers[:,2]>ground+.65*(eave-ground))
        order=np.flatnonzero(roof_mask);order=order[np.argsort(centers[order,2])]
        cumulative=np.cumsum(weights[order])
        deck_z=min(eave-.3,float(centers[order[np.searchsorted(cumulative,cumulative[-1]*.1)],2])-.15)
        deck=[]
        for t in facade._polygons(shapely.constrained_delaunay_triangles(Polygon(corners))):
            xyz=np.column_stack([np.asarray(t.exterior.coords)[:3],np.full(3,deck_z)])
            if np.cross(xyz[1]-xyz[0],xyz[2]-xyz[0])[2]<0:xyz=xyz[::-1]
            deck.extend(xyz)
        xyz=np.array(deck);tr=xyz.mean(0)
        proxies.append(mtscene.Mesh(0xffffffff,source[0].object_id,tr,(xyz-tr).astype(np.float32),None,None,
                                   [(4,np.arange(len(xyz),dtype=np.uint32))]))
    drops={}
    for i in np.flatnonzero(removed):
        mi,di,ti=entries[i][:3];drops.setdefault((mi,di),set()).add(ti)
    kept=[]
    for mi,m in enumerate(source):
        calls=[]
        for di,(prim,idx) in enumerate(m.draw_calls):
            excluded=drops.get((mi,di))
            if excluded:
                tri=idx.reshape(-1,3);mask=np.ones(len(tri),bool);mask[list(excluded)]=False
                idx=tri[mask].ravel()
            if len(idx):calls.append((prim,idx.copy()))
        if calls:
            part=copy.copy(m);part.draw_calls=calls;part.pcg_keep=True;kept.append(part)
    return kept+proxies,groups,dict(planes=len(groups),replaced_capture_triangles=int(removed.sum()),recovered_base_area=added_area,removed_overlap_area=overlap_area,closed_envelope=bool(envelope),envelope_method=envelope_method)
