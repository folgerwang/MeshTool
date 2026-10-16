"""Restore scan-gap notches only where captured roof surfaces support a wall."""
import numpy as np
import json
from pathlib import Path
import shapely
from shapely.geometry import Polygon
from shapely.ops import unary_union
from capture_appearance import faces
from solid_city import shell,polygons,audit_shell
from rules_config import RULES


def footprint(meshes):
    return unary_union([Polygon(t[:,:2]) for m in meshes for f,_ in faces(m)
                        if f.normal[2]>.5 for t in f.tris])


def repair(own,source_triangles,excluded,city_bounds,name=None,source_hash=None):
    cfg=RULES['envelopes'];base=footprint(own)
    world=np.concatenate([m.vertices.astype(float)+m.translation for m in own])
    bottom,top=world[:,2].min(),world[:,2].max()
    levels={round(float(f.origin[2]),2) for m in own for f,_ in faces(m) if f.normal[2]>.5}
    if len(levels)!=1:return own,dict(status='preserve_stepped_envelope')
    profile=Path(__file__).parent/RULES['profiles']['facade_directory']/f'{name}_envelope.json'
    if name and profile.exists():
        measured=json.loads(profile.read_text())
        if measured['source_sha256']==source_hash:
            candidate=Polygon(measured['outer_xy'],measured.get('holes_xy',[])).intersection(city_bounds).difference(excluded.buffer(.03))
            meshes=[m for part in polygons(candidate) for m in shell(part,bottom,top,own[0].object_id,own[0].tex_index)]
            audit=audit_shell(meshes)
            if not any(audit.values()):
                return meshes,dict(status='measured_capture_envelope',profile=str(profile),original_area_m2=base.area,repaired_area_m2=candidate.area,audit=audit)
    p=source_triangles
    if not len(p):return own,dict(status='no_source_roof')
    n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);n/=np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-12)
    hit=(abs(n[:,2])>=cfg['roof_normal_min'])&(p[:,:,2].min(1)>top-cfg['roof_depth_m'])
    roof=unary_union([Polygon(t[:,:2]) for t in p[hit]])
    roof=roof.buffer(cfg['gap_close_m'],join_style=2).buffer(-cfg['gap_close_m'],join_style=2)
    roof=roof.intersection(city_bounds)
    support=roof.intersection(base).area/max(base.area,1e-9)
    if support<cfg['min_footprint_support']:
        return own,dict(status='insufficient_roof_outline',support=support)
    # Existing neighboring buildings retain their footprint. No convex hull:
    # courtyards and genuine setbacks must remain empty without roof evidence.
    added=roof.difference(base).difference(excluded.buffer(.03))
    added=unary_union([p for p in polygons(added) if p.area>=cfg['min_added_area_m2']])
    candidate=base.union(added).buffer(0).simplify(cfg['simplify_m'],preserve_topology=True)
    candidate=shapely.set_precision(candidate,.02).buffer(.03,join_style=2).buffer(-.03,join_style=2)
    gain=candidate.difference(base).area
    if gain<cfg['min_added_area_m2']:return own,dict(status='no_supported_missing_envelope',support=support)
    # A scan may split one roof into narrow disconnected strips. Permit a
    # larger recovery only when nearly all existing strips sit on the same
    # measured roof and the added footprint is itself captured, not a hull.
    consolidated=(len(polygons(roof))==1 and len(polygons(candidate))==1
                  and support>=cfg.get('fragment_min_support',.9)
                  and roof.intersection(candidate).area/max(candidate.area,1e-9)>=.9
                  and candidate.area<=base.area*cfg.get('fragment_max_area_ratio',3.5))
    if candidate.area>base.area*cfg['max_area_ratio'] and not consolidated:
        return own,dict(status='excessive_outline_change',support=support)
    meshes=[]
    for part in polygons(candidate):
        meshes+=shell(part,bottom,top,own[0].object_id,own[0].tex_index)
    audit=audit_shell(meshes)
    if any(audit.values()):return own,dict(status='envelope_topology_fallback',audit=audit)
    return meshes,dict(status='captured_roof_supported_envelope',support=support,added_area_m2=gain,
                       consolidated_parts=consolidated,parts_before=len(polygons(base)),parts_after=len(polygons(candidate)),
                       original_area_m2=base.area,repaired_area_m2=candidate.area,audit=audit)
