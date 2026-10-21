"""Evaluate capture-supported closed building envelopes before replacing assets.

Each building gets several independent fits. Fit metrics are measured against
the original capture, including side silhouettes; the previous PCG is not a
training target. Rejected candidates never enter the city.
"""
import argparse, copy, json
from collections import defaultdict
from pathlib import Path
import numpy as np
import shapely
from shapely.ops import unary_union
import building, mtscene, raster, capture_appearance
from capture_tiles import refine_building
from solid_city import audit_shell
from stepped_shell import build, polygons
from repair_stepped_building import geometry_digest


def local_group(group, oid):
    own=[m for m in group.meshes if m.object_id==oid]
    own=[m for m in own if m.model_variant==1] or [m for m in own if m.model_variant==0]
    used=sorted({m.tex_index for m in own if m.tex_index<len(group.textures)})
    remap={v:i for i,v in enumerate(used)}; meshes=[]
    for m in own:
        m=copy.copy(m);m.object_id=0;m.model_variant=0;m.tex_index=remap.get(m.tex_index,0xffffffff);meshes.append(m)
    return mtscene.Group([group.textures[t] for t in used],meshes,[copy.copy(group.objects[oid])])


def envelope(model, merge_height):
    parts=[]
    for part in model.parts:
        if np.linalg.norm(part.plane[:2])>.15:
            raise ValueError('Sloping roof requires a separate roof reconstruction')
        z=float(np.asarray(part.polygon.exterior.coords)[:,:2].mean(0)@part.plane[:2]+part.plane[2])
        parts.append((z,part.polygon))
    parts.sort(key=lambda p:p[0]);clusters=[]
    for z,poly in parts:
        if not clusters or z-clusters[-1][0]>merge_height:clusters.append([z,[(z,poly)]])
        else:
            clusters[-1][1].append((z,poly))
            clusters[-1][0]=sum(a*p.area for a,p in clusters[-1][1])/sum(p.area for _,p in clusters[-1][1])
    if len(clusters)>10:raise ValueError('Too many unresolved roof levels')
    layers=[]
    for i,(z,_) in enumerate(clusters):
        region=unary_union([p for _,members in clusters[i:] for _,p in members])
        if i==0:region=model.footprint
        else:region=region.intersection(layers[-1][1])
        if not region.is_empty and z>model.ground_z+1:layers.append((z,region))
    return layers


def compare(source, candidate, resolution=.4):
    """Top and four side silhouette tests on a common fixed raster."""
    a=raster.collect(source);b=raster.collect(candidate)
    a.tex[:]=-1;b.tex[:]=-1;texture=np.zeros((1,4,4,3),np.uint8)
    lo=np.minimum(a.pos.min((0,1)),b.pos.min((0,1)))-1
    hi=np.maximum(a.pos.max((0,1)),b.pos.max((0,1)))+1
    bounds=[*lo[:2],*hi[:2]]
    original=raster.top_down(a,texture,resolution,bounds=bounds)
    result=raster.top_down(b,texture,resolution,bounds=bounds)
    x=np.isfinite(original.height);y=np.isfinite(result.height);both=x&y
    report=dict(footprint_iou=float(both.sum()/max(1,(x|y).sum())),
                height_fit=float((abs(original.height[both]-result.height[both])<1.5).mean()) if both.any() else 0,
                silhouette_miss=[],silhouette_excess=[])
    for angle in (0,np.pi/4,np.pi/2,3*np.pi/4):
        c,s=np.cos(angle),np.sin(angle)
        matrix=np.array([[c,s,0],[0,0,1],[s,-c,0]])
        ap=a.pos@matrix.T;bp=b.pos@matrix.T
        lower=np.minimum(ap.min((0,1)),bp.min((0,1)))-1;upper=np.maximum(ap.max((0,1)),bp.max((0,1)))+1
        bb=[*lower[:2],*upper[:2]]
        aa=raster.SceneTris(ap,a.uv,a.tex,a.obj,a.mesh);bbt=raster.SceneTris(bp,b.uv,b.tex,b.obj,b.mesh)
        aa=np.isfinite(raster.top_down(aa,texture,resolution,bounds=bb).height)
        bt=np.isfinite(raster.top_down(bbt,texture,resolution,bounds=bb).height)
        report['silhouette_miss'].append(float((aa&~bt).sum()/max(1,aa.sum())))
        report['silhouette_excess'].append(float((bt&~aa).sum()/max(1,bt.sum())))
    report['score']=report['footprint_iou']+report['height_fit']-max(report['silhouette_miss'])-.25*max(report['silhouette_excess'])
    report['accepted']=bool(report['footprint_iou']>=.9 and report['height_fit']>=.87 and max(report['silhouette_miss'])<=.09 and max(report['silhouette_excess'])<=.35)
    return report


def run(source,manifest_path,output,limit=30,skip=0,only=None,planar=False,polygonal=False):
    planar=planar or polygonal
    output=Path(output);output.mkdir(parents=True,exist_ok=False)
    manifest=json.loads(Path(manifest_path).read_text());scene=mtscene.load(source)
    ranked=sorted(manifest['buildings'],key=lambda r:r.get('audit_after',{}).get('boundary_edges',0),reverse=True)
    wanted=[r for r in ranked if (r['building'] in only if only else r.get('audit_after',{}).get('boundary_edges',0)>0)][skip:skip+limit]
    records=[]
    for number,record in enumerate(wanted,1):
        name=record['building'];g=scene[record['batch']].groups[record['group']]
        oid=next(i for i,o in enumerate(g.objects) if o.name==name)
        source_group=local_group(g,oid);rec=dict(building=name,batch=record['batch'],group=record['group'],object_id=oid,source_geometry_sha256=geometry_digest(source_group.meshes),iterations=[],status='preserved_previous')
        records.append(rec);print(number,len(wanted),name,'fitting',flush=True)
        try:
            src=raster.collect(source_group);src.tex[:]=-1
            resolution=.2 if planar else .35
            top=raster.top_down(src,np.zeros((1,4,4,3),np.uint8),resolution)
            bottom=float(np.percentile(src.pos[:,:,2],.5))
            crop=dict(height=top.height,rgb=top.color,mask=np.isfinite(top.height),other_building=np.zeros_like(top.height,bool),origin=(top.min_x,top.max_y))
            candidates=[]
            for step,(simplify,merge) in enumerate(((.35,1.2),(.6,2.),(.9,3.)),1):
                trial=dict(iteration=step,simplify_m=simplify,roof_merge_m=merge);rec['iterations'].append(trial)
                try:
                    model=building.build_model(crop,bottom,building.Params(res=resolution,min_plane_m2=10 if planar else 12,max_planes=16,simplify_m=.4 if planar else simplify))
                    if model is None:raise ValueError('Insufficient roof support')
                    if planar:
                        from planar_envelope import fit,assess
                        tolerance=(.6,.9,1.2)[step-1]
                        layers,fit_report=fit(model,src.pos,tolerance,polygonal=polygonal);trial.update(planar_fit=fit_report,facade_tolerance_m=tolerance)
                    else:layers=envelope(model,merge)
                    points,_=build(layers,bottom);center=points.mean(0)
                    mesh=mtscene.Mesh(0xffffffff,0,center,(points-center).astype(np.float32),None,None,[(4,np.arange(len(points),dtype=np.uint32))])
                    audit=audit_shell([mesh]);trial['audit']=audit
                    if any(audit.values()):raise ValueError('Envelope topology failed')
                    candidate=mtscene.Group(list(source_group.textures),[mesh],copy.deepcopy(source_group.objects))
                    metrics=compare(source_group,candidate);trial.update(metrics)
                    if planar:
                        trial['raw_capture_silhouette_accepted']=metrics['accepted']
                        trial['planar_evidence']=assess(source_group,candidate,layers)
                        trial['accepted']=trial['planar_evidence']['accepted']
                    if trial['accepted']:candidates.append((metrics['score'],candidate,layers,step))
                except (ValueError,shapely.errors.GEOSException) as e:trial.update(accepted=False,reason=str(e))
            if candidates:
                _,candidate,layers,step=max(candidates,key=lambda c:c[0]);folder=output/name;folder.mkdir()
                observed={}
                capture_appearance.bake(candidate,source_group,only={name},source_cache=(raster.collect(source_group),raster.decode_textures(source_group,size=256)),provenance=observed)
                generated,report=refine_building(candidate.meshes,candidate.objects[0],candidate.textures,folder,observed_masks=observed,clean=True,preserve_roofs=True,write_previews=bool(only))
                audit=audit_shell(generated)
                if any(audit.values()):raise ValueError(f'Opening topology failed: {audit}')
                candidate.meshes=generated;b=copy.copy(scene[record['batch']]);b.groups=[candidate]
                mtscene.save(output/(name+'.mtscene'),mtscene.Scene([b]))
                rec.update(status='accepted',selected_iteration=step,audit_after=audit,counts=report['counts'],asset=name+'.mtscene',catalog=str((folder/'tiles.json').resolve()),repair_mode='planar_roof_supported' if planar else 'stepped_envelope')
                (folder/'layers.json').write_text(json.dumps([dict(height=z,geometry=shapely.geometry.mapping(p)) for z,p in layers]))
        except (ValueError,shapely.errors.GEOSException) as e:rec['reason']=str(e)
        (output/'iterations.json').write_text(json.dumps(dict(source=str(Path(source).resolve()),screened_buildings=len(ranked),attempted=len(records),buildings=records),indent=2))
        print(name,rec['status'],rec.get('counts',rec.get('reason','capture fit rejected')),flush=True)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('manifest');p.add_argument('output');p.add_argument('--limit',type=int,default=30);p.add_argument('--skip',type=int,default=0)
    p.add_argument('--only',help='Comma-separated building names');p.add_argument('--planar',action='store_true',help='Fit continuous flat walls, allowing roof-supported completion of missing captured walls')
    p.add_argument('--polygonal',action='store_true',help='Fit flat walls along the captured roof boundary, preserving oblique facades and courtyards')
    a=p.parse_args();run(a.source,a.manifest,a.output,a.limit,a.skip,set(a.only.split(',')) if a.only else None,a.planar,a.polygonal)
