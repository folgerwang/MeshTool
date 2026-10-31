"""Conservative architectural evidence before turning a capture into facades.

Keep thresholds aligned with src/segment/building_evidence.h. A label or an
image-match score is not evidence of a building. Uncertain objects stay captured.
"""
import numpy as np


def check(meshes):
    parts=[m.vertices[i.reshape(-1,3)].astype(float)+m.translation
           for m in meshes for prim,i in m.draw_calls if prim==4 and len(i) and len(i)%3==0]
    if not parts:return dict(accepted=False,reason='No triangle geometry')
    return check_triangles(np.concatenate(parts))


SAMPLE_AREA=.25      # m2 of surface per coherence sample
MAX_SAMPLES=64       # per triangle


def _samples(p,area):
    """Points spread over each triangle (about one per SAMPLE_AREA), so a
    large triangle covers every voxel it crosses. Returns triangle index and
    position per sample; a fixed barycentric pattern keeps results exact."""
    counts=np.clip(np.ceil(area/SAMPLE_AREA).astype(np.int64),1,MAX_SAMPLES)
    owner=np.repeat(np.arange(len(p)),counts)
    k=np.arange(len(owner))-np.repeat(np.cumsum(counts)-counts,counts)
    # Low-discrepancy barycentrics folded into the triangle.
    u=(k*.7548776662+.5)%1;v=(k*.5698402910+.5)%1
    flip=u+v>1;u[flip]=1-u[flip];v[flip]=1-v[flip]
    q=p[owner]
    return owner,q[:,0]+u[:,None]*(q[:,1]-q[:,0])+v[:,None]*(q[:,2]-q[:,0])


def _neighbourhood_resultant(points,vectors,size):
    """Resultant length of unit vectors over each sample's 3x3x3 voxel block."""
    cell=np.floor(points/size).astype(np.int64)+(1<<20)
    key=(cell[:,0]<<42)|(cell[:,1]<<21)|cell[:,2]
    unique,inverse=np.unique(key,return_inverse=True);inverse=inverse.ravel()
    sums=np.stack([np.bincount(inverse,weights=v) for v in vectors.T],1)
    weight=np.bincount(inverse).astype(float)
    total=np.zeros_like(sums);mass=np.zeros_like(weight)
    for dx in (-1,0,1):
        for dy in (-1,0,1):
            for dz in (-1,0,1):
                shifted=unique+(dx<<42)+(dy<<21)+dz
                at=np.searchsorted(unique,shifted);at=np.minimum(at,len(unique)-1)
                hit=unique[at]==shifted
                total[hit]+=sums[at[hit]];mass[hit]+=weight[at[hit]]
    return (np.linalg.norm(total,axis=1)/np.maximum(mass,1e-12))[inverse]


def surface_coherence(p,normals,area,total):
    """Locally consistent walls and roofs, without requiring one clean plane.

    Fine, wrinkled capture meshes rarely form a single dense plane per wall,
    and top-down captures show little wall at all. Around every surface point
    a facade or a flat/pitched roof keeps one orientation; foliage does not.
    Opposite wall windings agree through the doubled horizontal angle. A roof
    also needs some coherent wall, so a sloped ground plane is not a building.
    """
    owner,points=_samples(p,area);share=(area/np.bincount(owner,minlength=len(p)))[owner]
    n=normals[owner];vertical=abs(n[:,2])<.25
    wall=0.;height=0.
    if vertical.any():
        angle=2*np.arctan2(n[vertical,1],n[vertical,0])
        r=_neighbourhood_resultant(points[vertical],np.stack([np.cos(angle),np.sin(angle)],1),1.5)
        good=r>.90;wall=float(share[vertical][good].sum())
        if good.any():height=float(np.ptp(points[vertical][good,2]))
    up=n*np.where(n[:,2:3]<0,-1.,1.);roofish=up[:,2]>=.25
    roof=0.;roof_total=float(share[roofish].sum())
    if roofish.any():
        r=_neighbourhood_resultant(points[roofish],up[roofish],2.)
        roof=float(share[roofish][r>.97].sum())
    accepted=((wall>=150 and wall>=.15*total and height>=6) or
              (roof>=100 and roof>=.30*roof_total and wall>=max(20.,.05*roof)) or
              (wall+roof>=600 and wall+roof>=.05*total and wall>=.05*(wall+roof)))
    return dict(coherent_wall=wall,coherent_roof=roof,coherent_wall_height=height,roof_area=roof_total,surface_area=float(total),accepted=bool(accepted))


BIRDVIEW_RES=.5        # m per pixel, as the segmenter's orthophoto
SMOOTH_RESIDUAL=.10    # m: local plane-fit roughness of a roof seen from above
RAISED=2.5             # m above ground


def smooth_raised(top,above_ground,res=BIRDVIEW_RES):
    """Pixels of a smooth surface standing above the ground, seen from above.

    A 2.5 m window fits a plane to the visible height; roofs (flat or
    pitched) fit within centimetres, tree crowns do not. top is NaN where
    nothing was captured.
    """
    from scipy import ndimage
    valid=np.isfinite(top).astype(float);z=np.nan_to_num(top)
    H,W=top.shape;y,x=np.mgrid[:H,:W].astype(float)
    k=2*max(1,int(round(1.25/res)))+1
    n=ndimage.uniform_filter(valid,k)+1e-9
    m=lambda a:ndimage.uniform_filter(a*valid,k)/n
    mx,my,mz=m(x),m(y),m(z)
    cxx=m(x*x)-mx*mx;cyy=m(y*y)-my*my;cxy=m(x*y)-mx*my
    cxz=m(x*z)-mx*mz;cyz=m(y*z)-my*mz;czz=m(z*z)-mz*mz
    det=cxx*cyy-cxy*cxy+1e-9
    b=(cxz*cyy-cyz*cxy)/det;c=(cyz*cxx-cxz*cxy)/det
    residual=np.sqrt(np.maximum(czz-b*cxz-c*cyz,0))
    return (n>.9)&(np.nan_to_num(above_ground)>RAISED)&(residual<SMOOTH_RESIDUAL)


def own_birdview(p,res=BIRDVIEW_RES):
    """(footprint m2, smooth raised roof m2) from the object's own triangles.

    Without the scene's ground model the object's lowest captured surfaces
    stand in for local ground.
    """
    import raster
    lo=p.reshape(-1,3).min(0);hi=p.reshape(-1,3).max(0)
    W=max(1,int(np.ceil((hi[0]-lo[0])/res)));H=max(1,int(np.ceil((hi[1]-lo[1])/res)))
    if W*H>16_000_000:return None
    scr=np.empty(p.shape);scr[...,0]=(p[...,0]-lo[0])/res;scr[...,1]=(hi[1]-p[...,1])/res;scr[...,2]=p[...,2]
    depth=np.full((H,W),-np.inf);ids=np.full((H,W),-1,np.int32)
    raster.rasterize(scr,np.zeros((len(p),3,2),np.float32),np.full(len(p),-1,np.int32),np.arange(len(p),dtype=np.int32),
                     np.zeros((1,4,4,3),np.uint8),W,H,depth,np.zeros((H,W,3),np.uint8),ids,-1e30,1e30)
    top=np.where(np.isfinite(depth),depth,np.nan)
    if not np.isfinite(top).any():return None
    ground=float(np.percentile(p[...,2],2))
    smooth=smooth_raised(top,top-ground,res)
    return float(np.isfinite(top).sum()*res*res),float(smooth.sum()*res*res)


def birdview_accepts(footprint,smooth,walls):
    """A smooth raised roof covering enough of the object from above, standing
    on some vertical surface (a sloped ground plane has none)."""
    return smooth>=40 and smooth>=.20*footprint and walls>=max(10.,.10*smooth)


def check_triangles(p,birdview=None):
    p=p[np.isfinite(p).all(axis=(1,2))]
    cross=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);area=np.linalg.norm(cross,axis=1)/2
    keep=area>.01;p=p[keep];area=area[keep];cross=cross[keep]
    if not len(p):return dict(accepted=False,reason='No usable surface area')
    normals=cross/(2*area[:,None]);centers=p.mean(1);total=float(area.sum())
    vertical=abs(normals[:,2])<.25
    seeds=np.flatnonzero(vertical);seeds=seeds[np.argsort(-area[seeds],kind='stable')][:64]
    used=np.zeros(len(p),bool);support=0.;planes=0;directions=[];rectangular=False
    for seed in seeds:
        if used[seed]:continue
        n=normals[seed].copy();n[2]=0.;n/=np.linalg.norm(n);offset=centers[seed]@n
        hit=vertical&~used&(abs(normals@n)>.97)&(np.max(abs(p@n-offset),axis=1)<1.2)
        if hit.sum()<2:continue
        u=np.array([-n[1],n[0],0.]);u/=np.linalg.norm(u)
        width=float(np.ptp(p[hit]@u));height=float(np.ptp(p[hit,:,2]));a=float(area[hit].sum())
        # A single tilted triangle or a sparse branch is not a wall panel.
        if width<3 or height<3 or a<12 or a<.55*width*height:continue
        used|=hit;support+=a;planes+=1
        directions.append(n)
        rectangular |= a>=.85*width*height and a>=.60*total
    fraction=support/total
    corner=any(abs(a@b)<.9 for a in directions for b in directions)
    # Weak support needs at least three planes; a pair of incidental patches
    # on a tree or cliff must not establish the whole object as a building.
    planar=(corner and fraction>=(.20 if planes>=3 else .50)) or rectangular
    coherence=surface_coherence(p,normals,area,total)
    coherent=coherence.pop('accepted')
    if birdview is None and not planar and not coherent:birdview=own_birdview(p)
    walls=float(area[abs(normals[:,2])<.25].sum())
    roof_seen=birdview is not None and birdview_accepts(*birdview,walls)
    accepted=planar or coherent or roof_seen
    return dict(accepted=bool(accepted),planes=planes,wall_fraction=fraction,**coherence,
                smooth_roof=birdview[1] if birdview else None,
                reason='Supported facade planes' if planar else 'Locally coherent walls and roofs' if coherent else 'Smooth raised roof seen from above' if accepted else 'Insufficient coherent wall planes; possible vegetation, terrain or incomplete capture')
