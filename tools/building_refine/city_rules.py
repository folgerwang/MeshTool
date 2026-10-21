"""Apply shared construction with per-building capture evidence, not one style."""
import copy
import hashlib
import json
from pathlib import Path
from collections import Counter

import cv2
import numpy as np
import shapely
from scipy import ndimage
from shapely.geometry import box
from shapely.ops import unary_union

import mtscene,raster,building
import capture_appearance,envelope_repair
from building_rules import RULESET_VERSION
from capture_tiles import refine_building
from solid_city import audit_shell,shell,polygons
from rules_config import RULES,configure,snapshot


def regularize_heights(xy,z,valid,res,return_regions=False):
    """Robust planes remove scan spikes while retaining supported roof slopes."""
    config=RULES['roofs'];remaining=valid.copy();planes=[];members=[]
    local=xy-xy.mean(0);A=np.c_[local,np.ones(len(local))]
    minimum=max(6,int(config['min_plane_area_m2']/res**2),int(valid.sum()*config['min_plane_fraction']))
    rng=np.random.default_rng(7421)
    for _ in range(config['max_planes']):
        indices=np.flatnonzero(remaining)
        if len(indices)<minimum:break
        best=None;best_count=0
        for _ in range(config['ransac_trials']):
            sample=rng.choice(indices,3,replace=False)
            try:plane=np.linalg.solve(A[sample],z[sample])
            except np.linalg.LinAlgError:continue
            if np.linalg.norm(plane[:2])>np.tan(np.deg2rad(config['max_slope_degrees'])):continue
            hit=remaining&(abs(A@plane-z)<config['plane_tolerance_m'])
            if hit.sum()>best_count:best_count=int(hit.sum());best=hit
        if best is None or best_count<minimum:break
        plane=np.linalg.lstsq(A[best],z[best],rcond=None)[0]
        if np.linalg.norm(plane[:2])<np.tan(np.deg2rad(config['flat_degrees'])):
            plane=np.array([0.,0.,float(np.median(z[best]))])
        planes.append(plane);members.append(best);remaining[best]=False
    if not planes:
        result=(np.full(len(z),np.median(z[valid])),dict(planes=0,supported_fraction=0.,status='no_supported_plane'))
        if return_regions:return (*result,np.zeros(len(z),int),np.array([[0.,0.,np.median(z[valid])]]),xy.mean(0))
        return result
    labels=np.full(len(z),-1,int)
    for i,mask in enumerate(members):labels[mask]=i
    from scipy.spatial import cKDTree
    known=labels>=0
    _,nearest=cKDTree(xy[known]).query(xy[~known]);labels[~known]=labels[known][nearest]
    fitted=np.sum(A*np.asarray(planes)[labels],axis=1)
    fitted=np.clip(fitted,np.percentile(z[valid],2),np.percentile(z[valid],98))
    result=(fitted,dict(planes=len(planes),supported_fraction=float(known.mean()),status='regularized_capture_planes'))
    if return_regions:return (*result,labels,np.asarray(planes),xy.mean(0))
    return result


def roof_surface(poly,old_top,bottom,ortho,texture,oid,center,res=None):
    """Closed roof sampled from captured upper surfaces, including slopes/steps.

    Interior capture gaps are filled from nearest measured roof samples. The
    existing shell provides a sealed fallback when evidence is insufficient.
    """
    res=RULES['roofs']['mesh_resolution_m'] if res is None else res
    poly=shapely.set_precision(poly,RULES['roofs']['footprint_precision_m'])
    minx,miny,maxx,maxy=poly.bounds
    xs=np.arange(np.floor(minx/res)*res,maxx,res)
    ys=np.arange(np.floor(miny/res)*res,maxy,res)
    cells=shapely.intersection([box(x,y,x+res,y+res) for y in ys for x in xs],poly)
    triangles=[]
    for cell in cells:
        for part in getattr(cell,'geoms',[cell]):
            if part.geom_type!='Polygon' or part.area<1e-7:continue
            triangles.extend(np.asarray(t.exterior.coords)[:3] for t in building._triangulate(part))
    if not triangles:return None,old_top,dict(status='no_surface')
    xy=np.round(np.concatenate(triangles),5)
    unique,inverse=np.unique(xy,axis=0,return_inverse=True)
    px=np.clip(((unique[:,0]-ortho.min_x)/ortho.res).astype(int),0,ortho.height.shape[1]-1)
    py=np.clip(((ortho.max_y-unique[:,1])/ortho.res).astype(int),0,ortho.height.shape[0]-1)
    heights=ortho.height[py,px].astype(float)
    valid=np.isfinite(heights)&(heights>max(bottom+2,old_top-RULES['roofs']['height_below_cap_m']))&(heights<old_top+RULES['roofs']['height_above_cap_m'])
    coverage=float(valid.mean())
    if valid.sum()<RULES['roofs']['min_samples'] or coverage<RULES['roofs']['min_coverage']:return None,old_top,dict(status='insufficient_roof_capture',coverage=coverage)
    from scipy.spatial import cKDTree
    if not valid.all():
        _,nearest=cKDTree(unique[valid]).query(unique[~valid]);heights[~valid]=heights[valid][nearest]
    heights,plane_report,labels,planes,plane_origin=regularize_heights(unique,heights,valid,res,True)
    if plane_report.get('supported_fraction',1)<RULES['roofs']['min_plane_support']:
        return None,old_top,dict(status='insufficient_plane_support',coverage=coverage,plane_fit=plane_report)
    # Eave is taken from the lower captured roof population, bounded to avoid
    # consuming whole upper floors in damaged/occluded captures.
    base=max(bottom+3,old_top-RULES['roofs']['max_eave_lowering_m'],min(old_top,float(np.percentile(heights[valid],RULES['roofs']['eave_percentile']))))
    heights=np.maximum(heights,base+.04)
    tri=inverse.reshape(-1,3)
    a,b=unique[tri[:,1]]-unique[tri[:,0]],unique[tri[:,2]]-unique[tri[:,0]]
    area=a[:,0]*b[:,1]-a[:,1]*b[:,0]
    reverse=area<0;tri[reverse]=tri[reverse][:,::-1]
    tri=tri[abs(area)>1e-8]
    # A heightfield interpolated across roof levels creates false ramps and
    # pyramids. Partition triangles into fitted planes and close each region
    # independently, giving observed level changes real vertical risers.
    region=np.array([np.bincount(labels[t],minlength=len(planes)).argmax() for t in tri])
    points=[];side_ranges=[];offset=0;regions=[]
    for rid,plane in enumerate(planes):
        chosen=tri[region==rid]
        if not len(chosen):continue
        polygon=unary_union([shapely.Polygon(unique[t]) for t in chosen])
        # Separate point-contact rings: otherwise two walls share one vertical
        # edge and make a non-manifold solid. The sealed shell below covers
        # these millimetre construction joints.
        polygon=polygon.buffer(-RULES['roofs']['region_joint_m'],join_style=2)
        for part in getattr(polygon,'geoms',[polygon]):
            if part.geom_type=='Polygon' and part.area>=RULES['roofs']['region_min_area_m2']:
                part=part.simplify(RULES['roofs']['region_simplify_m'],preserve_topology=True).intersection(poly)
                for section in getattr(part,'geoms',[part]):
                    if section.geom_type=='Polygon' and section.area>1e-7:regions.append((plane,section))
    if not regions:return None,old_top,dict(status='insufficient_roof_regions',coverage=coverage)
    for rid,(plane,part) in enumerate(regions):
        local_tri=np.concatenate([np.asarray(t.exterior.coords)[:3] for t in building._triangulate(part)])
        local_xy,idx=np.unique(np.round(local_tri,5),axis=0,return_inverse=True)
        chosen=idx.reshape(-1,3)
        a,b=local_xy[chosen[:,1]]-local_xy[chosen[:,0]],local_xy[chosen[:,2]]-local_xy[chosen[:,0]]
        area=a[:,0]*b[:,1]-a[:,1]*b[:,0]
        chosen[area<0]=chosen[area<0][:,::-1];chosen=chosen[abs(area)>1e-8]
        z=np.maximum(np.c_[local_xy-plane_origin,np.ones(len(local_xy))]@plane,base+.04)
        top=np.c_[local_xy,z]
        # Tiny buried offsets keep coincident region bottoms from becoming
        # non-manifold edges when the independently closed solids are audited.
        lower=np.c_[local_xy,np.full(len(local_xy),base-.025-rid*.002)]
        pieces=[top[chosen].reshape(-1,3),lower[chosen[:,::-1]].reshape(-1,3)]
        edges=np.concatenate([chosen[:,[0,1]],chosen[:,[1,2]],chosen[:,[2,0]]])
        _,first,count=np.unique(np.sort(edges,axis=1),axis=0,return_index=True,return_counts=True)
        start=offset+sum(len(p) for p in pieces)
        for a,b in edges[first[count==1]]:
            q=np.array([top[a],lower[a],lower[b],top[b]])
            pieces.append(q[[0,1,2,0,2,3]])
        piece=np.concatenate(pieces);points.append(piece)
        side_ranges.append((start,offset+len(piece)));offset+=len(piece)
    points=np.concatenate(points)
    uv=np.c_[(points[:,0]-ortho.min_x)/(ortho.res*ortho.height.shape[1]),(ortho.max_y-points[:,1])/(ortho.res*ortho.height.shape[0])]
    # Top-down photos have no observed vertical pixels. Sample one adjacent
    # roof color per riser instead of stretching the photograph down its wall.
    for start,end in side_ranges:
        for i in range(start,end,6):uv[i:i+6]=uv[i:i+6].mean(0)
    mesh=mtscene.Mesh(texture,oid,center,(points-center).astype(np.float32),uv.astype(np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))],0,0)
    audit=audit_shell([mesh])
    if any(audit.values()):return None,old_top,dict(status='topology_fallback',coverage=coverage,audit=audit)
    return mesh,base,dict(status='captured_height_surface',construction='closed_plane_regions',regions=len(regions),coverage=coverage,eave=base,height_range=[float(heights.min()),float(points[:,2].max())],grid_m=res,plane_fit=plane_report,audit=audit)


def fit_roofs(own,ortho,texture):
    own=copy.deepcopy(own);roof_parts=[];reports=[]
    bottom=min((m.vertices+m.translation)[:,2].min() for m in own)
    for mesh in own:
        world=mesh.vertices.astype(float)+mesh.translation
        for primitive,indices in mesh.draw_calls:
            if primitive!=4:continue
            p=world[indices].reshape(-1,3,3);n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);n/=np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-12)
            selected=(n[:,2]>.99)&(np.ptp(p[:,:,2],axis=1)<.001)
            for level in np.unique(np.round(p[selected,:,2].mean(1),3)):
                keep=selected&(abs(p[:,:,2].mean(1)-level)<.002)
                poly=unary_union([shapely.Polygon(t[:,:2]) for t in p[keep]])
                for part in getattr(poly,'geoms',[poly]):
                    if part.geom_type!='Polygon':continue
                    roof,base,report=roof_surface(part,level,bottom,ortho,texture,mesh.object_id,own[0].translation)
                    reports.append(report)
                    if roof is None:continue
                    roof_parts.append(roof)
                    # Lower the matching cap and wall tops without moving the
                    # UV mapping on the unchanged lower wall.
                    for target in own:
                        wp=target.vertices.astype(float)+target.translation
                        matching=(abs(wp[:,2]-level)<.002)&shapely.intersects_xy(part.buffer(.002),wp[:,0],wp[:,1])
                        if target.uvs is not None and matching.any():
                            # Per-triangle old world -> UV interpolation preserves
                            # wall texture scale when an eave is lowered.
                            for prim,idx in target.draw_calls:
                                if prim!=4:continue
                                for tri in idx.reshape(-1,3):
                                    hit=matching[tri]
                                    if not hit.any():continue
                                    old=wp[tri];nn=np.cross(old[1]-old[0],old[2]-old[0])
                                    if abs(nn[2])>.5*np.linalg.norm(nn):continue
                                    basis=np.c_[old,np.ones(3)];coef=np.linalg.lstsq(basis,target.uvs[tri],rcond=None)[0]
                                    moved=old.copy();moved[hit,2]=base
                                    target.uvs[tri[hit]]=(np.c_[moved,np.ones(3)]@coef)[hit]
                        target.vertices[matching,2]=base-target.translation[2]
    return own,roof_parts,reports


def recover_roof_deck(existing_roofs,captured,top,oid,texture):
    """Fill a missing deck only inside a well-supported captured roof outline."""
    from shapely.geometry import Polygon
    n=np.cross(captured[:,1]-captured[:,0],captured[:,2]-captured[:,0])
    area=np.linalg.norm(n,axis=1)/2
    nz=np.abs(n[:,2])/np.maximum(area*2,1e-12)
    z=captured[:,:,2].mean(1)
    valid=(nz>RULES['roofs']['deck_normal_min'])&(z>top-RULES['roofs']['height_below_cap_m'])&(area>1e-5)
    if valid.sum()<RULES['roofs']['min_samples']:return [],dict(status='insufficient_deck_capture')
    p=captured[valid];weights=area[valid];heights=z[valid]
    bins=np.round(heights/RULES['roofs']['deck_height_bin_m']).astype(int);labels,inv=np.unique(bins,return_inverse=True)
    sums=np.bincount(inv,weights=weights);mode=labels[sums.argmax()]*RULES['roofs']['deck_height_bin_m']
    near=abs(heights-mode)<RULES['roofs']['deck_height_tolerance_m']
    height=float(np.average(heights[near],weights=weights[near]))
    outline=unary_union([Polygon(t[:,:2]) for t in p])
    gap=RULES['envelopes']['gap_close_m']
    outline=outline.buffer(gap,join_style=2).buffer(-gap,join_style=2)
    outline=shapely.set_precision(outline,.02)
    parts=[part.simplify(RULES['roofs']['deck_simplify_m'],preserve_topology=True) for part in polygons(outline) if part.area>=RULES['roofs']['min_plane_area_m2']]
    outline=unary_union(parts)
    if outline.is_empty:return [],dict(status='insufficient_deck_outline')
    covered=envelope_repair.footprint(existing_roofs) if existing_roofs else Polygon()
    missing=outline.difference(covered).area/outline.area
    if missing<RULES['roofs']['deck_missing_fraction_min']:return [],dict(status='existing_deck_present',missing_fraction=missing)
    meshes=[m for part in parts for m in shell(part,height-RULES['roofs']['deck_thickness_m'],height,oid,texture)]
    audit=audit_shell(meshes)
    if any(audit.values()):return [],dict(status='deck_topology_rejected',audit=audit)
    return meshes,dict(status='captured_deck',height=height,area_m2=outline.area,missing_fraction=missing,audit=audit)


def refine_preserved_roofs(own,group,source,obj,source_cache):
    """Refresh supported roof finishes without replacing calibrated facades."""
    from capture_appearance import faces
    roof=[];other=[]
    bottom=min((m.vertices+m.translation)[:,2].min() for m in own)
    top=max((m.vertices+m.translation)[:,2].max() for m in own)
    for mesh in own:
        selected=[]
        for face,indices in faces(mesh):
            area=np.linalg.norm(np.cross(face.tris[:,1]-face.tris[:,0],face.tris[:,2]-face.tris[:,0]),axis=1).sum()/2
            if face.normal[2]>.5 and face.origin[2]>max(bottom+2,top-RULES['roofs']['height_below_cap_m']) and area>=RULES['roofs']['min_plane_area_m2'] and area/max(np.prod(face.size),1e-9)>RULES['roofs']['deck_min_face_fill']:
                selected.extend(tuple(t) for t in indices.reshape(-1,3))
        selected=set(selected)
        if not selected:other.append(mesh);continue
        r=copy.copy(mesh);r.draw_calls=[];kept=copy.copy(mesh);kept.draw_calls=[]
        for primitive,index in mesh.draw_calls:
            if primitive!=4:kept.draw_calls.append((primitive,index));continue
            tri=index.reshape(-1,3);mask=np.array([tuple(t) in selected for t in tri])
            if mask.any():r.draw_calls.append((primitive,tri[mask].ravel()))
            if (~mask).any():kept.draw_calls.append((primitive,tri[~mask].ravel()))
        if r.draw_calls:roof.append(r)
        if kept.draw_calls:other.append(kept)
    # Calibrated facade generators may leave no main deck beneath roof equipment.
    src,_=source_cache
    sid=next(i for i,o in enumerate(source.objects) if o.name==obj.name)
    captured=src.pos[src.obj==sid]
    deck,deck_report=recover_roof_deck(roof,captured,top,own[0].object_id,own[0].tex_index)
    if deck:roof+=deck
    if not roof:return own,dict(status='no_large_roof_faces')
    temporary=mtscene.Group(group.textures,roof,group.objects)
    evidence=capture_appearance.bake(temporary,source,only={obj.name},source_cache=source_cache)
    samples=[f for r in evidence for f in r['faces']]
    coverage=sum(f['area']*f['coverage'] for f in samples)/max(sum(f['area'] for f in samples),1e-9)
    if coverage<RULES['roofs']['min_coverage']:return own,dict(status='insufficient_roof_material_capture',coverage=coverage)
    return other+temporary.meshes,dict(status='captured_roof_finish',coverage=coverage,geometry='captured_deck_recovered' if deck else 'unchanged',deck=deck_report,faces=len(samples))


def run(reference,source,landmark,output,only=None,fallback=None,procedural_facades=False):
    output=Path(output);output.mkdir(parents=True,exist_ok=False);config_record=snapshot(output)
    import shutil
    profile_dir=Path(__file__).parent/RULES['profiles']['facade_directory']
    profile_copy=output/'measured_profile_snapshot';profile_copy.mkdir()
    config_record['profile_hashes']={}
    for profile in profile_dir.glob('*.json'):
        shutil.copy2(profile,profile_copy/profile.name)
        config_record['profile_hashes'][profile.name]=hashlib.sha256(profile.read_bytes()).hexdigest()
    paths=[Path(p) for p in (reference,source,landmark)]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    calibrated_source=hashes[str(paths[1])]==RULES['profiles']['source_capture_sha256']
    classification_path=Path(__file__).parent/RULES['profiles']['classification_file']
    excluded={}
    if classification_path.exists():
        classification=json.loads(classification_path.read_text())
        if classification['source_sha256']==hashes[str(paths[1])]:
            excluded={r['building']:r for r in classification['objects'] if r['action']=='omit_generated_building'}
    scene=mtscene.load(reference);original=mtscene.load(source);example=mtscene.load(landmark)
    associations={};aliases={};assembly_modes={}
    group_profile=profile_dir/'building_groups.json'
    if group_profile.exists():
        membership=json.loads(group_profile.read_text())
        if membership['source_sha256']==hashes[str(paths[1])]:
            for assembly in membership['groups']:
                leader=assembly['building'];members=assembly['members']
                if only and not set(members)&only:continue
                associations[leader]=members
                assembly_modes[leader]=assembly.get('assembly_mode','combined_envelope')
                aliases.update({name:leader for name in members if name!=leader})
                if only:only.update(members)
    fallback_scene=mtscene.load(Path(fallback)/'city.mtscene') if fallback else None
    fallback_reports={r['building']:r for r in json.loads((Path(fallback)/'manifest.json').read_text())['buildings']} if fallback else {}
    reports=[]
    for bi,batch in enumerate(scene):
        for gi,group in enumerate(batch.groups):
            sg=original[bi].groups[gi];src=raster.collect(sg)
            n=np.cross(src.pos[:,1]-src.pos[:,0],src.pos[:,2]-src.pos[:,0]);n/=np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-12)
            buildings=np.array([o.cls==3 and o.name not in excluded for o in sg.objects]);selected=buildings[np.clip(src.obj,0,len(buildings)-1)]&(abs(n[:,2])>RULES['envelopes']['roof_normal_min'])
            roofsrc=raster.SceneTris(src.pos[selected],src.uv[selected],src.tex[selected],src.obj[selected],src.mesh[selected])
            print('Rasterizing original roof surfaces',flush=True)
            decoded=raster.decode_textures(sg,size=512)
            ortho=raster.top_down(roofsrc,decoded,RULES['roofs']['raster_resolution_m'])
            h,w=ortho.color.shape[:2];im=cv2.resize(ortho.color,((w+3)//4*4,(h+3)//4*4));ih,iw=im.shape[:2]
            texture=len(group.textures);group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(iw,ih,raster.encode_dxt1(im))]))
            output_meshes=[]
            for leader,members in associations.items():
                ids={i for i,o in enumerate(group.objects) if o.name in members}
                oid=next(i for i,o in enumerate(group.objects) if o.name==leader)
                if assembly_modes.get(leader)=='captured_roof_attachment':
                    # A thin terrace patch is not evidence for an independent full-height volume.
                    group.meshes=[m for m in group.meshes if m.object_id not in ids or m.object_id==oid]
                    continue
                pieces=[m for m in group.meshes if m.object_id in ids]
                if not pieces:continue
                world=np.concatenate([m.vertices.astype(float)+m.translation for m in pieces])
                outline=envelope_repair.footprint(pieces).buffer(0)
                combined=[m for part in polygons(outline) for m in shell(part,world[:,2].min(),world[:,2].max(),oid,pieces[0].tex_index)]
                group.meshes=[m for m in group.meshes if m.object_id not in ids]+combined
            envelopes={i:envelope_repair.footprint([m for m in group.meshes if m.object_id==i]) for i,o in enumerate(group.objects) if o.cls==3}
            envelope_tops={i:max((m.vertices[:,2]+m.translation[2]).max() for m in group.meshes if m.object_id==i)
                           for i in envelopes if any(m.object_id==i for m in group.meshes)}
            all_world=np.concatenate([m.vertices.astype(float)+m.translation for m in group.meshes])
            city_bounds=box(*all_world[:,:2].min(0),*all_world[:,:2].max(0))
            source_ids={o.name:i for i,o in enumerate(sg.objects)}
            for oid,obj in enumerate(group.objects):
                own=[m for m in group.meshes if m.object_id==oid]
                if obj.name in aliases:
                    reports.append(dict(building=obj.name,status='combined_into_building',combined_into=aliases[obj.name],roof=[],counts={}));continue
                if obj.cls!=3 or not own:output_meshes+=own;continue
                if only and obj.name not in only:
                    output_meshes+=own;reports.append(dict(building=obj.name,status='not_selected'));continue
                if assembly_modes.get(obj.name)=='captured_roof_attachment':
                    if fallback_scene is None:raise ValueError('Roof attachment requires a validated parent fallback scene')
                    fg=fallback_scene[bi].groups[gi];fid=next(i for i,o in enumerate(fg.objects) if o.name==obj.name)
                    meshes=[]
                    for parent,ids_to_copy in [(fg,{fid}),(sg,{i for i,o in enumerate(sg.objects) if o.name in associations[obj.name] and o.name!=obj.name})]:
                        remap={}
                        for original_mesh in parent.meshes:
                            if original_mesh.object_id not in ids_to_copy:continue
                            m=copy.deepcopy(original_mesh);m.object_id=oid
                            if 0<=m.tex_index<len(parent.textures):
                                if m.tex_index not in remap:remap[m.tex_index]=len(group.textures);group.textures.append(parent.textures[m.tex_index])
                                m.tex_index=remap[m.tex_index]
                            meshes.append(m)
                    output_meshes+=meshes
                    report=copy.deepcopy(fallback_reports[obj.name]);report['source_members']=associations[obj.name]
                    report['assembly']=dict(mode='captured_roof_attachment',parent_geometry='preserved',roof_patch='captured_geometry_retained')
                    directory=output/obj.name
                    if (Path(fallback)/obj.name).exists():shutil.copytree(Path(fallback)/obj.name,directory)
                    if 'catalog' in report:report['catalog']=str(directory/'tiles.json')
                    reports.append(report);print(obj.name,'attached captured roof patch without extruding walls',flush=True)
                    continue
                if obj.name in excluded:
                    reports.append(dict(building=obj.name,status='excluded_nonbuilding_capture',classification_correction=excluded[obj.name],counts={},roof=[]));continue
                structure_path=profile_dir/(obj.name+'_structure.json')
                if structure_path.exists():
                    structure=json.loads(structure_path.read_text())
                    if structure['source_sha256']==hashes[str(paths[1])]:
                        from open_structure import reconstruct
                        meshes,report=reconstruct(structure,own,obj,group.textures,sg,(src,decoded))
                        output_meshes+=meshes;reports.append(report);continue
                if calibrated_source and obj.name=='building_080':
                    eg=example[0].groups[0];offset=len(group.textures);group.textures.extend(eg.textures)
                    for m in eg.meshes:
                        if eg.objects[m.object_id].name!='building_080':continue
                        m=copy.deepcopy(m);m.object_id=oid;m.tex_index+=offset;output_meshes.append(m)
                    reports.append(dict(building=obj.name,status='calibrated_profile',ruleset=RULESET_VERSION));continue
                if calibrated_source and obj.name in RULES['profiles']['preserve_existing']:
                    own,roof_finish=refine_preserved_roofs(own,group,sg,obj,(src,decoded))
                    output_meshes+=own;reports.append(dict(building=obj.name,status='existing_refined_preserved',roof_finish=roof_finish,glass_meshes=sum(m.material==1 for m in own)));continue
                directory=output/obj.name;directory.mkdir()
                neighbor_footprints=unary_union([p for i,p in envelopes.items() if i!=oid and
                    (obj.name not in associations or envelope_tops.get(i,float('inf'))>=envelope_tops[oid]-12.)])
                observed_masks={}
                source_members=associations.get(obj.name,[obj.name])
                own,envelope_report=envelope_repair.repair(own,src.pos[np.isin(src.obj,[source_ids.get(n,-1) for n in source_members])],neighbor_footprints,city_bounds,obj.name,hashes[str(paths[1])])
                if obj.name in associations or envelope_report['status'] in ('captured_roof_supported_envelope','measured_capture_envelope'):
                    temporary=mtscene.Group(group.textures,own,group.objects)
                    capture_appearance.bake(temporary,sg,only={obj.name},source_cache=(src,decoded),provenance=observed_masks)
                    own=temporary.meshes
                    envelopes[oid]=envelope_repair.footprint(own)
                fitted,roofs,roof_report=fit_roofs(own,ortho,texture)
                original_audit,fitted_audit=audit_shell(own),audit_shell(fitted)
                if any(fitted_audit[k]>original_audit[k] for k in original_audit):
                    fitted=own;roofs=[];roof_report=[dict(status='shell_topology_fallback',before=original_audit,after=fitted_audit)]
                if roofs:
                    temporary=mtscene.Group(group.textures,roofs,group.objects)
                    capture_appearance.bake(temporary,sg,only={obj.name},source_cache=(src,decoded))
                    roofs=temporary.meshes
                meshes,report=refine_building(fitted,obj,group.textures,directory,observed_masks=observed_masks,clean=True,capture_wall=not procedural_facades)
                before,after=audit_shell(fitted),audit_shell(meshes)
                if any(after[k]>before[k] for k in before):
                    if fallback_scene is None:raise ValueError(f'{obj.name}: broken shell {after}')
                    fg=fallback_scene[bi].groups[gi]
                    fid=next(i for i,o in enumerate(fg.objects) if o.name==obj.name)
                    retained=[copy.deepcopy(m) for m in fg.meshes if m.object_id==fid]
                    texture_map={}
                    for m in retained:
                        m.object_id=oid
                        if 0<=m.tex_index<len(fg.textures):
                            if m.tex_index not in texture_map:
                                texture_map[m.tex_index]=len(group.textures);group.textures.append(fg.textures[m.tex_index])
                            m.tex_index=texture_map[m.tex_index]
                    output_meshes+=retained
                    rejected=dict(reason='candidate_topology_regression',before=before,after=after)
                    report=copy.deepcopy(fallback_reports[obj.name]);report['rejected_iteration']=rejected
                    report['fallback_scene']=str(Path(fallback)/'city.mtscene')
                    # Keep rejected measurements separate from the retained geometry's catalog.
                    directory.rename(output/(obj.name+'_rejected'))
                    if (Path(fallback)/obj.name).exists():shutil.copytree(Path(fallback)/obj.name,directory)
                    if 'catalog' in report:report['catalog']=str(directory/'tiles.json')
                    reports.append(report);print(obj.name,'retained previous valid building:',rejected,flush=True)
                    continue
                report.update(roof=roof_report,envelope=envelope_report,audit=after,glass_meshes=sum(m.material==1 for m in meshes))
                if obj.name in associations:report['source_members']=source_members
                if RULES['captured_glazing']['enabled'] and not procedural_facades:
                    from captured_glazing import transfer
                    selected=np.isin(src.obj,[source_ids.get(n,-1) for n in source_members])&(src.tex>=0)
                    evidence_source=raster.SceneTris(*[getattr(src,k)[selected] for k in ('pos','uv','tex','obj','mesh')])
                    catalog=json.loads((directory/'tiles.json').read_text())
                    meshes,report['captured_glazing']=transfer(meshes,group.textures,catalog,evidence_source,decoded,directory)
                    (directory/'tiles.json').write_text(json.dumps(catalog,indent=2))
                output_meshes+=meshes+roofs;reports.append(report)
                print(obj.name,report['counts'],'roofs',len(roofs),flush=True)
            group.meshes=output_meshes
    mtscene.save(output/'city.mtscene',scene)
    for p in paths:assert hashlib.sha256(p.read_bytes()).hexdigest()==hashes[str(p)]
    (output/'manifest.json').write_text(json.dumps(dict(config=config_record,ruleset=RULESET_VERSION,inputs=hashes,buildings=reports,procedural_facades=procedural_facades,
        limitations=['Capture-derived roofs use fitted closed plane regions; thin ornaments and roof glazing are not fully classified','Missing roof samples use nearest supported planes; fallback shells are explicitly reported','Window detection is conservative and may miss or misclassify openings','Captured wall and roof finishes retain baked lighting']),indent=2))


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('reference');p.add_argument('source');p.add_argument('landmark');p.add_argument('output');p.add_argument('--rules');p.add_argument('--only');p.add_argument('--fallback');p.add_argument('--procedural-facades',action='store_true',help='Use sampled palettes and procedural wall/window materials');a=p.parse_args();configure(a.rules)
    run(a.reference,a.source,a.landmark,a.output,set(a.only.split(',')) if a.only else None,a.fallback,a.procedural_facades)
