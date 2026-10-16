"""Export a separate georeferenced terrain scene and future asset placements.

python tools/building_refine/terrain_scene.py capture.mtscene new_output_directory
The output directory must not exist. Source scene and meshes are never modified.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np
from scipy import ndimage

import mtscene
import raster
import terrain
from scipy.interpolate import LinearNDInterpolator
from scipy.spatial import QhullError


def bare_ground(height, candidates, res):
    """Reject elevated surface objects, then interpolate terrain, not canopy.

    Progressive openings reject broad mislabelled crowns/roofs as well as small
    clutter. Linear interpolation cannot overshoot its surrounding elevations.
    The distance map exposes where below-canopy ground is weakly constrained.
    """
    trusted=candidates & np.isfinite(height)
    if not trusted.any():raise ValueError('No measured ground samples')
    # Captures also contain low skirts / buried duplicate surfaces. Reject these
    # BEFORE opening: otherwise a low layer makes valid streets look elevated.
    # The slope allowance preserves gradual hills; only abrupt depressions are
    # removed. Rejected samples are subsequently reconstructed and marked inferred.
    for radius in (2.,6.,12.,24.,48.,96.):
        _,indices=ndimage.distance_transform_edt(~trusted,return_indices=True)
        filled=height[tuple(indices)]
        closed=ndimage.grey_closing(filled,size=(int(2*radius/res)|1,)*2)
        low=(closed-filled>max(1.5,.15*radius))
        # A crown in a small capture can make closing raise the entire outer
        # ground region. Only enclosed depressions have surrounding evidence;
        # never classify an edge-connected valley from this test alone.
        labels,_=ndimage.label(low)
        edge=np.unique(np.concatenate((labels[0],labels[-1],labels[:,0],labels[:,-1])))
        low &= ~np.isin(labels,edge)
        trusted &= ~low
        if not trusted.any():raise ValueError('Ground filtering rejected every sample')
    for radius in (2.,6.,12.,24.,48.):
        _,indices=ndimage.distance_transform_edt(~trusted,return_indices=True)
        filled=height[tuple(indices)]
        size=int(2*radius/res)|1
        opened=ndimage.grey_opening(filled,size=(size,size))
        rejected=trusted & (height-opened>max(.6,.12*radius))
        trusted &= ~rejected
        if not trusted.any():raise ValueError('Ground filtering rejected every sample')
    distance,indices=ndimage.distance_transform_edt(~trusted,return_indices=True)
    filled=height[tuple(indices)].astype(np.float32)
    yy,xx=np.nonzero(trusted)
    # Bound the triangulation cost; retain deterministic spatial coverage.
    stride=max(1,int(np.ceil(np.sqrt(len(xx)/60000))))
    select=(xx%stride==0)&(yy%stride==0)
    if select.sum()>=3:
        try:
            interp=LinearNDInterpolator(np.c_[xx[select],yy[select]],height[yy[select],xx[select]])
            hy,hx=np.nonzero(~trusted)
            values=interp(hx,hy);valid=np.isfinite(values)
            filled[hy[valid],hx[valid]]=values[valid]
        except QhullError:pass # narrow capture: nearest observed ground is safer
    filled[trusted]=height[trusted]
    return filled,trusted,(distance*res).astype(np.float32)


def original_group(group):
    archived={m.object_id for m in group.meshes if m.model_variant==1}
    meshes=[m for m in group.meshes if (m.model_variant==1 if m.object_id in archived else m.model_variant==0)]
    return mtscene.Group(group.textures,meshes,group.objects)


def rebuild_group(group,res=.5,step=2,log=print):
    group=original_group(group)
    tris=raster.collect(group)
    extent=np.ptp(tris.pos.reshape(-1,3)[:,:2],axis=0)
    if np.prod(np.maximum(1,np.ceil(extent/res)))>16_000_000:
        raise ValueError('Terrain raster exceeds 16 million pixels; use a coarser --resolution or a smaller source scene')
    cls_of=np.array([o.cls for o in group.objects]+[0],np.int32)
    if np.any((tris.obj < -1)|(tris.obj>=len(group.objects))):raise ValueError('Invalid source object IDs')
    tri_cls=cls_of[tris.obj]
    if not np.isin(tri_cls,terrain.TERRAIN).any():raise ValueError('No segmented ground, road, vegetation ground or water')
    log(f'Rasterizing {len(tris.pos):,} source triangles at {res:g} m/pixel')
    tex=raster.decode_textures(group)
    top=raster.top_down(tris,tex,res)
    classes=cls_of[top.obj]
    if not (np.isfinite(top.height)&np.isin(classes,terrain.TERRAIN)).any():
        raise ValueError('No visible terrain samples; cannot infer terrain from buildings alone')
    # Render only upward/horizontal terrain evidence. Tree and building meshes
    # never become elevation samples, even where they cover the entire view.
    cross=np.cross(tris.pos[:,1]-tris.pos[:,0],tris.pos[:,2]-tris.pos[:,0])
    length=np.linalg.norm(cross,axis=1)
    ground_tri=np.isin(tri_cls,terrain.TERRAIN)&(abs(cross[:,2])>.7*length)&(length>.001)
    if not ground_tri.any():raise ValueError('No horizontal ground triangles')
    subset=raster.SceneTris(tris.pos[ground_tri],tris.uv[ground_tri],tris.tex[ground_tri],tris.obj[ground_tri],tris.mesh[ground_tri])
    h,w=top.height.shape
    bounds=np.array([top.min_x,top.max_y-h*res,top.min_x+w*res,top.max_y])
    ground=raster.top_down(subset,tex,res,bounds=bounds)
    candidates=np.isfinite(ground.height)
    # Duplicate capture layers can classify the same roof/crown as both ground
    # and an asset. Reject ground samples at that asset's elevation even if the
    # ground-labelled triangle won the full-scene depth test.
    asset_tri=np.isin(tri_cls,(3,4,5))
    tree_cover=classes==5
    if asset_tri.any():
        assets=raster.SceneTris(tris.pos[asset_tri],tris.uv[asset_tri],tris.tex[asset_tri],tris.obj[asset_tri],tris.mesh[asset_tri])
        asset_top=raster.top_down(assets,tex,res,bounds=bounds)
        candidates &= ~(np.isfinite(asset_top.height)&(ground.height>=asset_top.height-1.))
        tree_cover |= cls_of[asset_top.obj]==5
    height,trusted,support_distance=bare_ground(ground.height,candidates,res)
    _,indices=ndimage.distance_transform_edt(~trusted,return_indices=True)
    ground_classes=cls_of[ground.obj];land=ground_classes[tuple(indices)]
    kernel=cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(int(terrain.COVER_GAP/res)|1,)*2)
    cover=cv2.morphologyEx(np.isfinite(top.height).astype(np.uint8),cv2.MORPH_CLOSE,kernel).astype(bool)
    holes=~trusted
    color_holes=holes|raster.placeholder_pixels(ground.color)
    stats=dict(measured_ground_m2=float((trusted&cover).sum()*res*res),
               inferred_ground_m2=float((holes&cover).sum()*res*res),
               removed_elevated_samples_m2=float((np.isfinite(ground.height)&holes&cover).sum()*res*res),
               tree_cover_m2=float((tree_cover&cover).sum()*res*res),
               inferred_under_trees_m2=float((tree_cover&holes&cover).sum()*res*res))
    log(f'Bare ground: {stats}')
    ter=terrain.Terrain(ground,res,height,land,cover,holes,color_holes,stats)
    if not np.isfinite(ter.height[ter.cover]).all():raise ValueError('Non-finite terrain elevations')
    color=terrain.fill_color(ground,ter,log=log)
    # Large unobserved plots must not inherit black building shadows from the
    # edge inpainting. Use nearby unshadowed ground as an explicitly synthetic
    # surface in their interior; retain captured roads and measured surface detail.
    luminance=ground.color.astype(np.float32).mean(axis=2)
    good=trusted & ~raster.placeholder_pixels(ground.color)
    if good.any():
        good &= luminance>=max(45.,float(np.percentile(luminance[good],30)))
    if good.any():
        _,ci=ndimage.distance_transform_edt(~good,return_indices=True)
        neutral=ground.color[tuple(ci)].astype(np.float32)
        neutral=ndimage.gaussian_filter(neutral,sigma=(2/res,2/res,0))
        depth=ndimage.distance_transform_edt(color_holes)*res
        blend=np.clip((depth-.5)/2.,0,1)[...,None]
        color=np.clip(color*(1-blend)+neutral*blend,0,255).astype(np.uint8)
    class_ids=sorted(int(k) for k in np.unique(ter.cls[ter.cover]) if k in terrain.TERRAIN)
    objects=[mtscene.SceneObject(terrain.CLASS_NAME[k],k) for k in class_ids]
    meshes,textures=terrain.build_meshes(ter,color,step,{k:i for i,k in enumerate(class_ids)},0)
    if not meshes:raise ValueError('No terrain cells can be generated from this capture')
    placements=[]
    for oid,obj in enumerate(group.objects):
        if obj.cls not in (terrain.CLS_BUILDING,terrain.CLS_TREE):continue
        own=tris.pos[tris.obj==oid].reshape(-1,3)
        if not len(own):continue
        lo,hi=own.min(0),own.max(0);xy=(lo[:2]+hi[:2])/2
        px,py=top.to_px(*xy);ix,iy=int(px),int(py)
        valid=0<=ix<ter.cover.shape[1] and 0<=iy<ter.cover.shape[0] and bool(ter.cover[iy,ix])
        placements.append(dict(source_object=oid,name=obj.name,kind='building' if obj.cls==3 else 'tree',
                               world_bounds=[lo.tolist(),hi.tolist()],world_anchor=[*xy.tolist(),float(lo[2])],
                               terrain_z=float(ter.height[iy,ix]) if valid else None,
                               terrain_inferred=bool(ter.holes[iy,ix]) if valid else None,
                               placement_applied=False))
    data=dict(height=ter.height,cover=ter.cover,inferred=ter.holes&ter.cover,surface_class=ter.cls,
              source_object=top.obj,source_class=classes,captured_height=top.height,tree_cover=tree_cover,
              ground_support_distance_m=support_distance,min_x=top.min_x,max_y=top.max_y,res=res)
    stats={**ter.stats,'triangles':sum(len(idx)//3 for m in meshes for prim,idx in m.draw_calls if prim==4),
           'classes':{terrain.CLASS_NAME[k]:int((ter.cover&(ter.cls==k)).sum())*res*res for k in class_ids},
           'height_range':[float(ter.height[ter.cover].min()),float(ter.height[ter.cover].max())],
           'resolution_m':res,'mesh_spacing_m':res*step,'placements':len(placements)}
    return mtscene.Group(textures,meshes,objects),data,color,placements,stats


def export_scene(source,destination,res=.5,step=2):
    source=Path(source).resolve();destination=Path(destination).resolve()
    if not np.isfinite(res) or res<.1:raise ValueError('Resolution must be finite and at least 0.1 m')
    if step<1 or terrain.PAGE%step:raise ValueError('Mesh step must be a positive divisor of 2048')
    if destination.exists():raise ValueError('Output directory already exists; use a new directory to preserve existing scenes')
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    batches=mtscene.load(source);result=mtscene.Scene(route_data=batches.route_data);records=[]
    # Preflight before creating output; avoid silently dropping unsupported data.
    if not any(not b.is_spline and b.groups for b in batches):raise ValueError('No captured mesh groups')
    destination.mkdir(parents=True)
    for bi,b in enumerate(batches):
        if b.is_spline:continue
        batch=copy.copy(b);batch.groups=[]
        for gi,g in enumerate(b.groups):
            print(f'Building terrain for batch {bi}, group {gi}',flush=True)
            generated,data,color,placements,stats=rebuild_group(g,res,step,lambda s:print(s,flush=True))
            prefix=f'{bi}_{gi}'
            np.savez_compressed(destination/f'{prefix}_terrain.npz',**data)
            cv2.imwrite(str(destination/f'{prefix}_surface.png'),cv2.cvtColor(color,cv2.COLOR_RGB2BGR))
            cv2.imwrite(str(destination/f'{prefix}_inferred.png'),data['inferred'].astype(np.uint8)*255)
            cv2.imwrite(str(destination/f'{prefix}_coverage.png'),data['cover'].astype(np.uint8)*255)
            low,high=stats['height_range'];span=max(high-low,1e-6)
            encoded=np.round(np.clip((data['height']-low)/span,0,1)*65535).astype(np.uint16)
            encoded[~data['cover']]=0
            cv2.imwrite(str(destination/f'{prefix}_height16.png'),encoded)
            batch.groups.append(generated)
            records.append(dict(source_batch=bi,source_group=gi,output_batch=len(result),output_group=len(batch.groups)-1,
                                georeferenced=b.is_georeferenced,reference_pos=list(b.reference_pos),
                                heightmap=dict(file=f'{prefix}_height16.png',coverage=f'{prefix}_coverage.png',
                                               min_z=low,range_z=span,decode='world_z = min_z + pixel / 65535 * range_z',
                                               min_x=float(data['min_x']),max_y=float(data['max_y']),resolution_m=res,
                                               grid='x = min_x + column * resolution_m; y = max_y - row * resolution_m'),
                                data=f'{prefix}_terrain.npz',stats=stats,placements=placements))
        result.append(batch)
    mtscene.save(destination/'terrain.mtscene',result)
    assert hashlib.sha256(source.read_bytes()).hexdigest()==digest,'Source unexpectedly changed'
    manifest=dict(version=1,source=str(source),source_sha256=digest,scene='terrain.mtscene',
                  coordinates='Original scene world coordinates in metres, Z-up; batch geographic references preserved. No recentering or asset placement applied.',
                  elevation_note='Ground obscured by buildings or trees is interpolated, not newly measured.',groups=records)
    (destination/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print(f'Saved {destination / "terrain.mtscene"}',flush=True)
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source');parser.add_argument('output_directory')
    parser.add_argument('--resolution',type=float,default=.5)
    parser.add_argument('--step',type=int,default=2,help='Grid step in pixels; must divide 2048')
    args=parser.parse_args()
    try:export_scene(args.source,args.output_directory,args.resolution,args.step)
    except (ValueError,OSError) as e:parser.exit(1,f'Terrain export failed: {e}\n')

if __name__=='__main__':main()
