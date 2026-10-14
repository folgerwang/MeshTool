"""Read-only context evidence for completing the lower facade of a segmented tower.

Neighbors remain separate objects. Only coplanar lower-wall fragments and local
horizontal ground samples contribute; unsupported lower walls are left alone.
"""
import numpy as np
import shapely
from shapely.geometry import Polygon, box, LineString
from shapely.ops import unary_union


def triangles(mesh):
    for prim, idx in mesh.draw_calls:
        if prim == 4 and len(idx) % 3 == 0:
            yield mesh.vertices[idx.reshape(-1, 3)].astype(float) + mesh.translation


def infer(source, group):
    import regular_facade
    import facade
    if not source:
        return {}
    _, planes, _ = regular_facade.rebuild(source)
    if not planes:
        return {}
    own = np.concatenate([p.reshape(-1, 3) for m in source for p in triangles(m)])
    lo, hi = own.min(0), own.max(0)
    height = hi[2] - lo[2]
    if height < 8:
        return {}
    footprint = shapely.MultiPoint(own[:, :2]).convex_hull
    ring = footprint.buffer(5).difference(footprint.buffer(-1))
    oid = source[0].object_id
    originals = {m.object_id for m in group.meshes if m.model_variant == 1}
    samples, weights, nearby = [], [], []
    for m in group.meshes:
        if m.object_id == oid or m.model_variant == 2:
            continue
        if m.object_id in originals and m.model_variant != 1:
            continue
        if not 0 <= m.object_id < len(group.objects):
            continue
        cls = group.objects[m.object_id].cls
        if cls not in (1, 2, 3):
            continue
        for p in triangles(m):
            c = p.mean(1)
            cross = np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0])
            area = np.linalg.norm(cross, axis=1)/2
            valid = (area > .02) & np.isfinite(p).all(axis=(1, 2))
            valid &= (p[:, :, :2].max(1) >= lo[:2]-5).all(1) & (p[:, :, :2].min(1) <= hi[:2]+5).all(1)
            valid &= (p[:, :, 2].min(1) < lo[2]+min(35, height*.3)) & (p[:, :, 2].max(1) > lo[2]-12)
            ids = np.flatnonzero(valid)
            for i in ids:
                n = cross[i]/(2*area[i])
                if cls in (1, 2) and abs(n[2]) > .9 and lo[2]-12 <= c[i, 2] <= lo[2]+8:
                    a = Polygon(p[i, :, :2]).intersection(ring).area
                    if a > .02:
                        samples.append(float(c[i, 2]));weights.append(a)
                if abs(n[2]) < .4:
                    nearby.append((p[i], n, m.object_id))
    if not samples or sum(weights) < 20:
        return {}
    samples, weights = np.array(samples), np.array(weights)
    bins = np.floor(samples/.5).astype(int);start = bins.min()
    hist = np.bincount(bins-start, weights=weights)
    # A plaza and a lower street can both surround a tower. Use the lowest
    # well-supported level, rather than stopping the building at the plaza.
    support=np.convolve(hist,np.ones(3),mode='full')[1:len(hist)+1]
    candidates=np.flatnonzero(support>=max(20.,.2*weights.sum()))
    if not len(candidates):
        return {}
    peak = (int(candidates[0])+start+.5)*.5
    good = abs(samples-peak) < 1
    if weights[good].sum() < .2*weights.sum():
        return {}
    ground = float(np.average(samples[good], weights=weights[good]))
    plans=[]
    for plane in planes:
        n=plane['n'];u=np.cross([0.,0.,1.],n);offset=float(plane['anchor']@n)
        p=np.array(plane['tri']);xy=np.stack([p@u,p[:,:,2]],axis=-1)
        region=unary_union([Polygon(t) for t in xy])
        left,bottom,right,top=region.bounds
        # Neighboring podiums can hide many floors. Sample the stable wall
        # above that occlusion, still below the building's midpoint.
        cutoff=ground+min(60.,height*.45)
        if top < cutoff+4 or right-left < 3:
            continue
        band=box(left,ground,right,cutoff)
        evidence=[];donors=set()
        for q,normal,donor in nearby:
            if abs(normal@n)<.9 or np.max(abs(q@n-offset))>1.25:
                continue
            projected=Polygon(np.stack([q@u,q[:,2]],axis=-1)).intersection(band)
            if projected.area>.05:
                evidence.append(projected);donors.add(int(donor))
        supported=unary_union([region]+evidence)
        # Require the same facade span in three higher cross-sections. This
        # continues walls downward without connecting separate wings or gaps.
        strips=[]
        for x in np.arange(left,right,.5):
            end=min(x+.5,right);mid=(x+end)/2
            if all(region.covers(shapely.Point(mid,cutoff+d)) for d in (0.,2.,4.)):
                strips.append(box(x,ground,end,cutoff))
        completion=unary_union(strips+[supported.intersection(band)]).difference(region)
        if completion.is_empty or completion.area<1 or completion.area>region.area*.65:
            continue
        polygons=[list(poly.exterior.coords) for poly in facade._polygons(completion) if poly.area>.05]
        if polygons:
            observed=float(unary_union(evidence).difference(region).area) if evidence else 0.
            plans.append(dict(normal=n.tolist(),offset=offset,polygons=polygons,
                              donor_objects=sorted(donors),observed_area=observed,
                              inferred_area=max(0.,float(completion.area)-observed),
                              added_area=float(completion.area)))
    return dict(ground_z=ground,ground_support_area=float(weights[good].sum()),planes=plans) if plans else {}
