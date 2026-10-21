"""Fit continuous planar building walls from roof outlines and wall evidence.

Minor rooftop equipment does not veto a predominantly flat roof. Missing
captured walls are completed from the supported roof boundary, rather than
treated as holes that a reconstruction must preserve.
"""
import numpy as np
import shapely
from shapely.geometry import Polygon
from shapely.ops import unary_union
from shapely.affinity import rotate
from stepped_shell import polygons


def weighted_median(values,weights):
    order=np.argsort(values);return float(values[order[np.searchsorted(np.cumsum(weights[order]),weights.sum()/2)]])


def wall_evidence(triangles):
    cross=np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]);area=np.linalg.norm(cross,axis=1)/2
    normal=cross/np.maximum(2*area[:,None],1e-12);center=triangles.mean(1)
    useful=(area>.02)&(abs(normal[:,2])<.3)
    if useful.sum()<4:raise ValueError('Not enough surviving wall evidence for planar fitting')
    angle=np.arctan2(normal[useful,1],normal[useful,0])
    moment=np.sum(area[useful]*np.exp(4j*angle))/area[useful].sum()
    if abs(moment)<.5:raise ValueError('Facade directions do not support an orthogonal envelope')
    return float(np.angle(moment)/4),center[useful],normal[useful],area[useful],float(abs(moment))


def straighten(region,angle,evidence=None,roof_z=None,bottom=None,tolerance=.8):
    """Intersect a small set of parallel/perpendicular fitted boundary lines."""
    local=rotate(region,-np.degrees(angle),origin=(0,0)).simplify(tolerance,preserve_topology=True)
    rotation=np.array([[np.cos(angle),-np.sin(angle)],[np.sin(angle),np.cos(angle)]])
    def ring(coords):
        xy=np.asarray(coords)[:,:2];lines=[]
        for a,b in zip(xy[:-1],xy[1:]):
            delta=b-a;length=float(np.linalg.norm(delta))
            if length<2*tolerance:continue
            tangent=int(np.argmax(abs(delta)));axis=1-tangent
            if abs(delta[tangent])/length<.9:
                if length<4*tolerance:continue
                raise ValueError('Long diagonal facade cannot be flattened to orthogonal planes')
            offset=float((a[axis]+b[axis])/2)
            if evidence is not None:
                centers,normals,areas=evidence;normal=rotation[:,axis];direction=rotation[:,tangent]
                distance=centers[:,:2]@normal;along=centers[:,:2]@direction
                good=(abs(normals[:,:2]@normal)>.93)&(abs(distance-offset)<1.5)&(along>min(a[tangent],b[tangent])-.5)&(along<max(a[tangent],b[tangent])+.5)
                if roof_z is not None:good &= centers[:,2]<roof_z-1
                if bottom is not None:good &= centers[:,2]>bottom+1
                if good.sum()>=3 and areas[good].sum()>max(2,length):offset=weighted_median(distance[good],areas[good])
            line=[axis,offset,length]
            if lines and lines[-1][0]==axis:
                old=lines[-1];total=old[2]+length;old[1]=(old[1]*old[2]+offset*length)/total;old[2]=total
            else:lines.append(line)
        if len(lines)>1 and lines[0][0]==lines[-1][0]:
            a,b=lines[0],lines.pop();total=a[2]+b[2];a[1]=(a[1]*a[2]+b[1]*b[2])/total;a[2]=total
        if len(lines)<4:raise ValueError('Insufficient independent facade boundaries')
        corners=[]
        for a,b in zip(lines,np.roll(np.array(lines),-1,axis=0)):
            point=np.zeros(2);point[int(a[0])]=a[1];point[int(b[0])]=b[1];corners.append(point@rotation.T)
        return corners
    result=[]
    for p in polygons(local):
        if p.area<8:continue
        holes=[ring(r.coords) for r in p.interiors if Polygon(r).area>25]
        candidate=Polygon(ring(p.exterior.coords),holes)
        if not candidate.is_valid:raise ValueError('Fitted wall intersections self-intersect')
        result.append(candidate)
    if not result:raise ValueError('No supported facade boundary')
    result=unary_union(result)
    agreement=result.intersection(region).area/max(1,result.union(region).area)
    if agreement<.88:raise ValueError(f'Planar boundary loses roof support: {agreement:.3f}')
    return shapely.set_precision(result,.01)


def polygonal_boundary(region,tolerance=.8):
    """Keep oblique facades and courtyards while removing capture-scale zigzags."""
    result=[]
    for part in polygons(shapely.make_valid(region)):
        if part.area<8:continue
        # Small gaps in a photographed roof are not evidence for a courtyard.
        part=Polygon(part.exterior,[r.coords for r in part.interiors if Polygon(r).area>25])
        candidate=part.simplify(tolerance,preserve_topology=True)
        if not candidate.is_valid:raise ValueError('Invalid simplified facade boundary')
        result.append(candidate)
    if not result:raise ValueError('No supported facade boundary')
    result=unary_union(result)
    agreement=result.intersection(region).area/max(1,result.union(region).area)
    if agreement<.88:raise ValueError(f'Planar boundary loses roof support: {agreement:.3f}')
    return shapely.set_precision(result,.01)


def fit(model,triangles,tolerance=.8,polygonal=False):
    if polygonal:
        angle=centers=normals=area=confidence=None
    else:
        angle,centers,normals,area,confidence=wall_evidence(triangles)
    total=sum(p.polygon.area for p in model.parts)
    flat=[p for p in model.parts if np.linalg.norm(p.plane[:2])<.08 and p.polygon.area>=8]
    share=sum(p.polygon.area for p in flat)/max(1,total)
    if share<.8:raise ValueError(f'Main roof is not predominantly flat: {share:.3f}')
    clusters=[]
    for p in sorted(flat,key=lambda p:p.plane[2]):
        z=float(p.plane[2])
        if not clusters or z-clusters[-1][0]>.65:clusters.append([z,[p]])
        else:
            clusters[-1][1].append(p);parts=clusters[-1][1];clusters[-1][0]=sum(q.plane[2]*q.polygon.area for q in parts)/sum(q.polygon.area for q in parts)
    main=[(z,parts) for z,parts in clusters if sum(p.polygon.area for p in parts)>=max(30,total*.035)]
    if not main:raise ValueError('No dominant roof planes')
    evidence=(centers,normals,area)
    footprint=polygonal_boundary(model.footprint,tolerance) if polygonal else straighten(model.footprint,angle,evidence,main[-1][0],model.ground_z,tolerance)
    layers=[]
    for i,(z,parts) in enumerate(main):
        if i==0:region=footprint
        else:
            # Assign noisy roof details to the closest supporting roof level.
            threshold=(main[i-1][0]+z)/2
            high=[]
            for p in model.parts:
                center=np.array(p.polygon.centroid.coords)[0]
                if float(center@p.plane[:2]+p.plane[2])>=threshold:high.append(p.polygon)
            region=unary_union(high).buffer(.6,join_style=2).buffer(-.6,join_style=2)
            region=(polygonal_boundary(region,tolerance) if polygonal else straighten(region,angle,None,tolerance=tolerance)).intersection(layers[-1][1])
        layers.append((float(z),region))
    return layers,dict(boundary_mode='polygonal' if polygonal else 'orthogonal',axis_degrees=None if angle is None else float(np.degrees(angle)),wall_direction_confidence=confidence,dominant_flat_roof_fraction=share,
                      ignored_roof_detail_fraction=1-share,wall_segments=sum(len(p.exterior.coords)-1+sum(len(r.coords)-1 for r in p.interiors) for _,region in layers for p in polygons(region)))


def assess(source,candidate,layers,resolution=.25):
    """Score reliable roof evidence without penalizing completion of missing walls.

This is only for envelopes that already passed fit()'s flat-roof and
boundary-support checks. Roof equipment above the structural roof is
reported as simplified; it is not mistaken for a required wavy wall boundary.
"""
    import raster
    a=raster.collect(source);b=raster.collect(candidate);a.tex[:]=-1;b.tex[:]=-1
    low=np.minimum(a.pos.min((0,1)),b.pos.min((0,1)))-1
    high=np.maximum(a.pos.max((0,1)),b.pos.max((0,1)))+1
    bounds=[*low[:2],*high[:2]];texture=np.zeros((1,4,4,3),np.uint8)
    capture=raster.top_down(a,texture,resolution,bounds=bounds)
    result=raster.top_down(b,texture,resolution,bounds=bounds)
    heights=[z for z,_ in layers]
    observed=np.isfinite(capture.height)&(capture.height>=min(heights)-.75)&(capture.height<=max(heights)+1.5)
    generated=np.isfinite(result.height);both=observed&generated
    coverage=float(both.sum()/max(1,observed.sum()))
    height_fit=float((abs(capture.height[both]-result.height[both])<1.5).mean()) if both.any() else 0
    excess=float((generated&~np.isfinite(capture.height)).sum()/max(1,generated.sum()))
    return dict(observed_roof_coverage=coverage,roof_height_fit=height_fit,footprint_excess=excess,
                accepted=bool(coverage>=.92 and height_fit>=.9 and excess<=.05),
                basis='Dominant roof planes and fitted facade directions; missing capture walls may be completed.')
