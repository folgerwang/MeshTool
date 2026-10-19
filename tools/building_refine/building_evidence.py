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


def check_triangles(p):
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
    accepted=(corner and fraction>=(.20 if planes>=3 else .50)) or rectangular
    return dict(accepted=bool(accepted),planes=planes,wall_fraction=fraction,
                reason='Supported facade planes' if accepted else 'Insufficient coherent wall planes; possible vegetation, terrain or incomplete capture')
