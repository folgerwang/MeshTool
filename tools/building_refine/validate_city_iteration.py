"""Check that every fitted window/door in accepted assets cuts the wall."""
import argparse,json
from pathlib import Path
import numpy as np
import shapely
from shapely.geometry import Polygon
from shapely.affinity import translate
import mtscene
from building_rules import outline
from solid_city import audit_shell


def filled_openings(group,catalog):
    """Check installed frame and infill triangles, rather than prototype presence."""
    parts=[group.meshes[r['local_mesh_index']] for r in catalog['semantic_meshes'] if r['part'] in ('frame','glazing','door_leaf')]
    triangles=[(m.vertices[ix].astype(float)+m.translation).reshape(-1,3,3) for m in parts for prim,ix in m.draw_calls if prim==4]
    triangles=np.concatenate(triangles) if triangles else np.empty((0,3,3))
    surfaces={};prototypes={p['id']:p for p in catalog['prototypes']}
    for face in catalog['faces']:
        rel=triangles-np.array(face['origin']);depth=rel@np.array(face['normal'])
        # Frames and glazing lie on parallel planes just behind this facade.
        # Exclude the opposite side of a narrow wing and perpendicular reveals.
        hit=(np.ptp(depth,axis=1)<1e-4)&(depth.min(axis=1)>-.5)&(depth.max(axis=1)<.01)
        xy=np.stack([rel[hit]@np.array(face['u']),rel[hit]@np.array(face['v'])],axis=-1)
        polys=[Polygon(t) for t in xy if Polygon(t).area>1e-10]
        surfaces[face['face_id']]=(polys,shapely.STRtree(polys))
    worst=0.;count=0
    for instance in catalog['instances']:
        if instance.get('kind') not in ('window','door_candidate'):continue
        x,y,X,Y=instance['bounds'];prototype=prototypes[instance['prototype']]
        target=translate(outline(X-x,Y-y,prototype['shape']),x,y).buffer(-.002)
        polys,tree=surfaces[instance['face_id']];indices=tree.query(target,predicate='intersects')
        covered=shapely.union_all([polys[i] for i in indices])
        missing=target.difference(covered).area;worst=max(worst,missing);count+=1
        if missing>.001:raise ValueError(f'Unfilled opening on facade {instance["face_id"]}: {missing:.6f} m2')
    return dict(filled_openings_checked=count,max_unfilled_opening_m2=float(worst))


def openings(group,catalog):
    walls=[group.meshes[r['local_mesh_index']] for r in catalog['semantic_meshes'] if r['part']=='wall']
    parts=[m.vertices[i].astype(float)+m.translation for m in walls for primitive,i in m.draw_calls if primitive==4]
    triangles=np.concatenate(parts).reshape(-1,3,3) if parts else np.empty((0,3,3))
    prototypes={p['id']:p for p in catalog['prototypes']};by_face={}
    for f in catalog['faces']:
        rel=triangles-np.array(f['origin']);hit=np.max(abs(rel@np.array(f['normal'])),axis=1)<1e-4
        xy=np.stack([rel[hit]@np.array(f['u']),rel[hit]@np.array(f['v'])],axis=-1)
        polygons=[Polygon(t) for t in xy];by_face[f['face_id']]=(polygons,shapely.STRtree(polygons))
    worst=0.;count=0
    for instance in catalog['instances']:
        if instance.get('kind') not in ('window','door_candidate'):continue
        x,y,X,Y=instance['bounds'];prototype=prototypes[instance['prototype']]
        hole=translate(outline(X-x,Y-y,prototype['shape']),x,y).buffer(-.01)
        polys,tree=by_face[instance['face_id']];indices=tree.query(hole,predicate='intersects')
        area=sum(polys[i].intersection(hole).area for i in indices);worst=max(worst,area);count+=1
        if area>.001:raise ValueError(f'Wall obstructs opening: {area} m2')
        required=('frame','reveal','door_leaf') if instance['kind']=='door_candidate' else ('frame','reveal','glazing')
        if not all(prototype['geometry'].get(k) for k in required):
            raise ValueError('Incomplete opening assembly')
    fill=filled_openings(group,catalog)
    audit=audit_shell(group.meshes)
    if any(audit.values()):raise ValueError(f'Open building envelope: {audit}')
    return dict(openings_checked=count,max_wall_overlap_m2=worst,audit=audit,**fill)


def run(folders,output):
    results=[]
    for folder in map(Path,folders):
        records=json.loads((folder/'iterations.json').read_text())['buildings']
        for r in records:
            if r['status']!='accepted':continue
            group=mtscene.load(folder/r['asset'])[0].groups[0]
            catalog=json.loads(Path(r['catalog']).read_text());report=openings(group,catalog)
            results.append(dict(building=r['building'],**report));print(r['building'],report['openings_checked'],flush=True)
    report=dict(buildings=results,total_openings_checked=sum(r['openings_checked'] for r in results),max_wall_overlap_m2=max([r['max_wall_overlap_m2'] for r in results],default=0))
    Path(output).write_text(json.dumps(report,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('output');p.add_argument('folders',nargs='+');a=p.parse_args();run(a.folders,a.output)
