"""Extract per-building facade tile prototypes/instances from registered capture textures.

The scene expands the instance library to ordinary meshes for the current renderer.
Window/door recess depths are inferred; positions and appearance come from capture.
"""
import argparse
import copy
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path

import cv2
import numpy as np
from scipy import ndimage
from shapely.geometry import Polygon, box
from shapely.ops import unary_union

import building
import mtscene
import raster
from capture_appearance import faces
from rules_config import RULES, configure, snapshot


def json_value(value):
    if isinstance(value,np.generic):return value.item()
    if isinstance(value,np.ndarray):return value.tolist()
    raise TypeError(f'Cannot serialize {type(value).__name__}')


def detect_openings(image, pitch, ground_offset=0.,_large=False):
    """Observed components, with row/column support; never impose a regular grid."""
    gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY).astype(np.float32)
    background=cv2.GaussianBlur(gray,(0,0),max(1.,(RULES['windows']['large_sigma_m'] if _large else RULES['windows']['background_sigma_m'])/pitch))
    contrast=background-gray
    dark=(contrast>RULES['windows']['contrast_min'])&(gray<RULES['windows']['gray_max'])
    # Bridge thin horizontal crossbars, but preserve narrow vertical mullions:
    # a square closing kernel merges adjacent windows into oversized tiles.
    dark=cv2.morphologyEx(dark.astype(np.uint8),cv2.MORPH_CLOSE,np.ones((3,3 if _large else 1),np.uint8))
    if _large:dark=ndimage.binary_fill_holes(dark).astype(np.uint8)
    count,labels,stats,centers=cv2.connectedComponentsWithStats(dark,8)
    candidates=[];height,width=gray.shape
    for i in range(1,count):
        x,y,w,h,area=stats[i];wm,hm=w*pitch,h*pitch
        if not(RULES['windows']['min_width_m']<=wm<=(RULES['windows']['large_max_width_m'] if _large else RULES['windows']['max_width_m']) and RULES['windows']['min_height_m']<=hm<=(RULES['windows']['large_max_height_m'] if _large else RULES['windows']['max_height_m']) and RULES['windows']['min_aspect']<=wm/hm<=(RULES['windows']['large_max_aspect'] if _large else RULES['windows']['max_aspect'])):continue
        if _large and hm<=4.5 and wm<=3.5:continue
        touches_base=_large and ground_offset<=RULES['doors']['max_sill_offset_m'] and height-y-h<=2
        if x<2 or y<2 or x+w>width-2 or (y+h>height-2 and not touches_base):continue
        fill=area/(w*h)
        if fill<RULES['windows']['min_fill']:continue
        region=labels[y:y+h,x:x+w]==i
        strength=float(np.median(contrast[y:y+h,x:x+w][region]))
        # Filling crossbar holes must not turn an outlined wall panel into a
        # glass opening. The component interior still needs glazing contrast.
        if strength<RULES['windows']['contrast_min']:continue
        widths=region.sum(1);top_width=float(np.median(widths[:max(2,round(h*.15))]));middle=float(np.median(widths[round(h*.35):max(round(h*.7),round(h*.35)+1)]))
        symmetry=float(np.mean(abs(region.astype(float)-region[:,::-1])))
        shape='arched' if top_width<.85*middle and middle>.7*w and symmetry<.2 else 'rectangular'
        bottom=(height-y-h)*pitch
        door=ground_offset+bottom<RULES['doors']['max_sill_offset_m'] and RULES['doors']['min_height_m']<=hm<=RULES['doors']['max_height_m'] and RULES['doors']['min_width_m']<=wm<=RULES['doors']['max_width_m']
        candidates.append(dict(rect=[x,y,w,h],center=centers[i],width=wm,height=hm,
                               contrast=strength,fill=float(fill),shape=shape,kind='door_candidate' if door else 'window'))
    result=[]
    for item in candidates:
        x,y=item['center'];neighbors=0
        for other in candidates:
            if other is item:continue
            dx,dy=abs(x-other['center'][0])*pitch,abs(y-other['center'][1])*pitch
            similar=abs(item['width']-other['width'])<max(.4,item['width']*.35) and abs(item['height']-other['height'])<max(.4,item['height']*.3)
            if similar and ((dy<.35 and .6<dx<(24 if _large else 9)) or (dx<.4 and 1.5<dy<6)):neighbors+=1
        if neighbors<1 and not(item['kind']=='door_candidate' and item['contrast']>25):continue
        if _large and neighbors<2:continue
        x,y,w,h=item['rect'];margin=max(1,round(RULES['windows']['opening_margin_m']/pitch))
        x0=max(1,x-margin);y0=max(1,y-margin);x1=min(width-1,x+w+margin);y1=min(height-1,y+h+margin)
        result.append(dict(bounds=[x0*pitch,(height-y1)*pitch,x1*pitch,(height-y0)*pitch],
                           pixel_rect=[x0,y0,x1,y1],kind=item['kind'],neighbors=neighbors,
                           confidence=float(min(.98,.55+.06*neighbors+.004*item['contrast'])),
                           contrast=item['contrast'],shape=item['shape']))
    # Margins must not overlap adjacent openings.
    accepted=[];occupied=Polygon()
    for item in sorted(result,key=lambda q:-q['confidence']):
        shape=box(*item['bounds'])
        if occupied.intersects(shape):continue
        accepted.append(item);occupied=unary_union([occupied,shape])
    if not _large:
        large=detect_openings(image,pitch,ground_offset,_large=True)
        footprints=unary_union([box(*o['bounds']) for o in large])
        accepted=large+[o for o in accepted if not footprints.intersects(box(*o['bounds']))]
    return sorted(accepted,key=lambda q:(q['bounds'][1],q['bounds'][0]))


def recover_patterned_windows(image,pitch,openings):
    """Recover repeated capture motifs and bounded gaps within each observed row.

    No global grid: each row retains its own phase, including half-bay shifts.
    Doors are excluded from both templates and generated candidates.
    """
    gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY)
    recovered=list(openings);height,width=gray.shape
    occupied=[box(*o['bounds']) for o in recovered]
    templates=[]
    for o in sorted(openings,key=lambda x:-x['confidence']):
        if o['kind']!='window':continue
        x,y,X,Y=o['pixel_rect'];w,h=X-x,Y-y
        if w<5 or h<7 or h*pitch>5:continue
        if any(abs(w-a)<3 and abs(h-b)<3 for a,b in templates):continue
        templates.append((w,h))
        if len(templates)>RULES['pattern']['max_templates']:break
        template=gray[y:Y,x:X]
        if template.std()<RULES['pattern']['min_template_std']:continue
        score=cv2.matchTemplate(gray,template,cv2.TM_CCOEFF_NORMED)
        peaks=(score>RULES['pattern']['match_threshold'])&(score==ndimage.maximum_filter(score,size=(max(3,h//2),max(3,w//2))))
        yy,xx=np.nonzero(peaks)
        for j in np.argsort(score[yy,xx])[::-1][:RULES['pattern']['max_matches_per_template']]:
            a,b=int(xx[j]),int(yy[j]);bounds=[a*pitch,(height-b-h)*pitch,(a+w)*pitch,(height-b)*pitch]
            shape=box(*bounds)
            if a<2 or b<2 or a+w>=width-2 or b+h>=height-2:continue
            if any(shape.intersects(other) for other in occupied):continue
            # Correlation alone also matches masonry and cornice shadows.
            # Require comparable absolute tone/contrast to the source glazing.
            sample=gray[b:b+h,a:a+w]
            if abs(float(np.median(sample))-float(np.median(template)))>RULES['pattern']['max_template_tone_difference']:continue
            if sample.std()<template.std()*RULES['pattern']['min_template_contrast_ratio']:continue
            row_support=column_support=0
            for seed in openings:
                if seed['kind']!='window':continue
                sx,sy,sX,sY=seed['pixel_rect'];sw,sh=sX-sx,sY-sy
                if abs(sw-w)*pitch>max(.3,w*pitch*.25) or abs(sh-h)*pitch>max(.3,h*pitch*.25):continue
                if abs((sy+sY)/2-(b+h/2))*pitch<RULES['pattern']['row_tolerance_m']:row_support+=1
                if abs((sx+sX)/2-(a+w/2))*pitch<RULES['pattern']['row_tolerance_m']:column_support+=1
            # Texture similarity in foliage/scan noise is not architectural
            # evidence. Require an independently observed row or column.
            if max(row_support,column_support)<RULES['pattern']['min_template_seed_support']:continue
            candidate=dict(bounds=bounds,pixel_rect=[a,b,a+w,b+h],kind='window',shape=o['shape'],confidence=float(score[b,a])*.85,neighbors=2,contrast=o['contrast'],recovery='capture_template')
            recovered.append(candidate);occupied.append(shape)
    rows=[]
    for o in sorted((o for o in recovered if o['kind']=='window'),key=lambda o:o['bounds'][1]):
        center=(o['bounds'][1]+o['bounds'][3])/2
        row=next((r for r in rows if abs(r[0]-center)<RULES['pattern']['row_tolerance_m']),None)
        if row is None:rows.append([center,[o]])
        else:row[1].append(o)
    for _,row in rows:
        if len(row)<RULES['pattern']['min_row_members']:continue
        row.sort(key=lambda o:o['bounds'][0]);centers=np.array([(o['bounds'][0]+o['bounds'][2])/2 for o in row])
        steps=np.diff(centers)
        from facade_layout import lattice
        fitted=lattice(centers)
        if fitted is None:continue
        spacing=fitted['spacing_m']
        if spacing<RULES['pattern']['min_bay_m']:continue
        regular=abs(steps/spacing-np.round(steps/spacing))<RULES['pattern']['spacing_tolerance']
        if regular.mean()<RULES['pattern']['min_regular_fraction']:continue
        for left,right,gap in zip(row,row[1:],steps):
            count=round(gap/spacing)
            if count<2 or count>RULES['pattern']['max_gap_bays'] or abs(gap/spacing-count)>RULES['pattern']['spacing_tolerance']:continue
            if abs((left['bounds'][3]-left['bounds'][1])-(right['bounds'][3]-right['bounds'][1]))>RULES['pattern']['max_height_difference_m']:continue
            for k in range(1,count):
                bounds=np.array(left['bounds']);bounds[[0,2]]+=gap*k/count
                shape=box(*bounds)
                if any(shape.intersects(other) for other in occupied):continue
                a,b,A,B=bounds;rect=[round(a/pitch),round(height-B/pitch),round(A/pitch),round(height-b/pitch)]
                # Repetition proposes a location; captured contrast must confirm it.
                ra,rb,rc,rd=rect;sample=gray[rb:rd,ra:rc]
                lx,ly,lX,lY=left['pixel_rect'];reference=gray[ly:lY,lx:lX]
                if not sample.size or not reference.size:continue
                if abs(float(np.median(sample))-float(np.median(reference)))>RULES['pattern']['max_template_tone_difference']:continue
                if sample.std()<max(2.,reference.std()*RULES['pattern']['min_template_contrast_ratio']):continue
                recovered.append(dict(left,bounds=bounds.tolist(),pixel_rect=rect,confidence=.55,recovery='bounded_row_gap',kind='window'));occupied.append(shape)
    return recovered


def decode(texture):
    w,h,data=texture.mips[0]
    if texture.internal_format in (raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGBA_S3TC_DXT1):return raster.decode_dxt1(w,h,data)
    if texture.internal_format in (raster.GL_COMPRESSED_RGBA_S3TC_DXT3,raster.GL_COMPRESSED_RGBA_S3TC_DXT5):return raster.decode_dxt5(w,h,data)
    raise ValueError('Unsupported facade texture format')


def facade_image(mesh,face,indices,texture,pitch=None):
    pitch=RULES['windows']['projection_texel_m'] if pitch is None else pitch
    world=mesh.vertices[indices].astype(float)+mesh.translation
    xy=np.c_[(world-face.origin)@face.u,(world-face.origin)@face.v,np.ones(len(world))]
    affine=np.linalg.lstsq(xy,mesh.uvs[indices],rcond=None)[0]
    pitch=max(pitch,max(face.size)/2040)
    w,h=np.ceil(np.array(face.size)/pitch).astype(int)
    yy,xx=np.mgrid[:h,:w]
    uv=np.stack([(xx+.5)*pitch,(h-yy-.5)*pitch,np.ones_like(xx)],axis=-1)@affine
    image=cv2.remap(texture,(uv[:,:,0]*texture.shape[1]-.5).astype(np.float32),
                    (uv[:,:,1]*texture.shape[0]-.5).astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    return image,affine,pitch


from building_rules import planar, outline, opening_template, RULESET_VERSION, material_for_part


def capture_faces(mesh):
    """Metric wall frames; merge coplanar patches sharing one affine UV map."""
    groups=[];lookup={}
    for face,indices in faces(mesh):
        if face.kind=='wall' and np.linalg.norm(face.normal)>1e-12:
            n=face.normal/np.linalg.norm(face.normal)
            u=np.array([-n[1],n[0],0.]);u/=np.linalg.norm(u)
            v=np.cross(n,u)
            points=face.tris.reshape(-1,3)
            x,y=points@u,points@v
            origin=u*x.min()+v*y.min()+n*np.mean(points@n)
            face=building.Face(face.tris,origin,u,v,n,(max(.01,np.ptp(x)),max(.01,np.ptp(y))),'wall')
            # Fit relative to the mesh translation for georeferenced scenes.
            local=mesh.vertices[indices].astype(float)
            design=np.c_[local@u,local@v,np.ones(len(local))]
            uv=np.linalg.lstsq(design,mesh.uvs[indices],rcond=None)[0]
            error=float(np.max(np.abs(design@uv-mesh.uvs[indices])))
            key=(*np.round(n,6),round(float(np.mean(local@n)),4),*np.round(uv.ravel(),5))
            if error<1e-6 and key in lookup:
                target=groups[lookup[key]];base=target[0]
                points=face.tris.reshape(-1,3)
                if np.max(np.abs((points-base.origin)@base.normal))<1e-5:
                    base_local=points-mesh.translation
                    design=np.c_[base_local@base.u,base_local@base.v,np.ones(len(points))]
                    if np.max(np.abs(design@target[2]-mesh.uvs[indices]))<1e-6:
                        target[1].append(indices);target[3].append(face.tris);continue
            if error<1e-6:lookup[key]=len(groups)
            groups.append([face,[indices],uv,[face.tris]])
        else:groups.append([face,[indices],None,[face.tris]])
    for face,indices,_,triangles in groups:
        if len(triangles)>1:
            points=np.concatenate(triangles);flat=points.reshape(-1,3)
            x,y=flat@face.u,flat@face.v
            origin=face.u*x.min()+face.v*y.min()+face.normal*np.mean(flat@face.normal)
            face=building.Face(points,origin,face.u,face.v,face.normal,(np.ptp(x),np.ptp(y)),'wall')
        yield face,np.concatenate(indices)


def refine_building(own,obj,textures,directory,observed_masks=None,clean=False,capture_wall=False,opening_overrides=None,support_levels=None,preserve_roofs=False,write_previews=True,window_style=None):
    """support_levels maps face IDs to measured patio/landing world-Z levels."""
    window_style=copy.deepcopy(window_style or {})
    allowed={'frame_width_m','frame_rgb','mullion_rgb','glazing_rgb','reveal_rgb','door_leaf_rgb'}
    if set(window_style)-allowed:raise ValueError('Unknown window style setting')
    for key,value in window_style.items():
        if key=='frame_width_m':
            if not isinstance(value,(int,float)) or not np.isfinite(value) or not 0<value<=.4:raise ValueError('Invalid window frame width')
        elif not isinstance(value,(list,tuple)) or len(value)!=3 or any(not isinstance(v,int) or not 0<=v<=255 for v in value):
            raise ValueError('Invalid window style RGB')
    library=[];instances=[];reports=[];parts=defaultdict(list);untouched=[];decoded={};style_keys={};preserved=defaultdict(list);native_parts=[]
    # One translation for all new parts avoids independent float32 seam rounding.
    center=own[0].translation.copy()
    bottom=min(float((m.vertices+m.translation)[:,2].min()) for m in own)
    def save_preview(path,image):
        if write_previews:cv2.imwrite(str(path),cv2.cvtColor(image,cv2.COLOR_RGB2BGR))
    def emit(local,face,affine,texture,semantic):
        if not len(local):return
        world=face.origin+local[:,0,None]*face.u+local[:,1,None]*face.v+local[:,2,None]*face.normal
        uv=np.c_[local[:,:2],np.ones(len(local))]@affine
        if clean:
            texture=palette[semantic];uv=np.zeros_like(uv)
            if semantic=='wall' and face_finish_texture is not None:
                texture=face_finish_texture;uv=np.c_[local[:,0]/finish_size[0],1-local[:,1]/finish_size[1]]
            if capture_wall and semantic=='wall':uv=np.c_[local[:,0]/wall_size[0],1-local[:,1]/wall_size[1]]
            if capture_wall and semantic=='glazing' and face_glass_texture is not None and 'glazing_rgb' not in window_style:
                texture=face_glass_texture;uv=np.c_[local[:,0]/wall_size[0],1-local[:,1]/wall_size[1]]
        parts[texture,semantic].append((world,uv))
    material_cache={}
    def material(color):
        key=tuple(int(v) for v in color)
        if key not in material_cache:
            image=np.full((4,4,3),key,np.uint8)
            material_cache[key]=len(textures)
            textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(4,4,raster.encode_dxt1(image))]))
        return material_cache[key]
    palette={}
    face_id=0
    for mesh_index,mesh in enumerate(own):
        if clean and mesh.material in (mtscene.MAT_GLASS,mtscene.MAT_INTERIOR):
            # A previous PCG pass may already have resolved window assemblies.
            # Their glass/interior triangles are not opaque wall observations.
            kept=copy.copy(mesh)
            color=(45,61,72) if mesh.material==mtscene.MAT_GLASS else (28,32,36)
            if mesh.material==mtscene.MAT_GLASS and mesh.tex_index<len(textures):
                sampled=decode(textures[mesh.tex_index])
                color=np.clip(np.median(sampled.reshape(-1,3),axis=0),RULES['materials']['glass_min'],RULES['materials']['glass_max'])
            kept.tex_index=material(color);kept.uvs=np.zeros((len(kept.vertices),2),np.float32);kept.colors=None
            native_parts.append((kept,'glazing' if mesh.material==mtscene.MAT_GLASS else 'interior'))
            continue
        if mesh.uvs is None or mesh.tex_index>=len(textures):untouched.append(mesh);continue
        if mesh.tex_index not in decoded:decoded[mesh.tex_index]=decode(textures[mesh.tex_index])
        retained=[];roof_indices=[]
        for face,indices in capture_faces(mesh):
            if preserve_roofs and face.kind!='wall':
                roof_indices.extend(indices);continue
            if face.kind!='wall' or face.size[0]<1. or face.size[1]<1.5:
                retained.extend(indices);continue
            image,affine,pitch=facade_image(mesh,face,indices,decoded[mesh.tex_index])
            levels=[bottom]+list((support_levels or {}).get(face_id,[]))
            openings=detect_openings(image,pitch,float(face.origin[2]-bottom))
            # Elevated doors require explicit supporting patio/landing evidence.
            for level in levels[1:]:
                for candidate in detect_openings(image,pitch,float(face.origin[2]-level)):
                    if candidate['kind']!='door_candidate':continue
                    sill=float(face.origin[2]+candidate['bounds'][1])
                    if not level-.01<=sill<=level+RULES['doors']['max_sill_offset_m']:continue
                    candidate['support_level_z']=float(level)
                    openings=[o for o in openings if not box(*o['bounds']).intersects(box(*candidate['bounds']))]
                    openings.append(candidate)
            if clean:openings=recover_patterned_windows(image,pitch,openings)
            if clean:
                from curtain_wall import detect as detect_curtain
                curtain=detect_curtain(image,pitch)
                if not RULES['curtain_wall']['allow_unresolved_glass_strips']:
                    curtain=[o for o in curtain if o['recovery']!='captured_glazed_band']
                if curtain:
                    area=unary_union([box(*o['bounds']) for o in curtain])
                    openings=curtain+[o for o in openings if not area.intersects(box(*o['bounds']))]
            # Exact-image binding prevents local measurements leaking into a
            # different capture that happens to reuse the same object number.
            if clean:
                from storefront import detect as detect_storefront
                storefronts=detect_storefront(image,pitch,float(face.origin[2]-bottom))
                if storefronts:
                    occupied=unary_union([box(*o['bounds']) for o in storefronts])
                    openings=storefronts+[o for o in openings if not occupied.intersects(box(*o['bounds']))]
            profile_path=Path(__file__).parent/RULES['profiles']['facade_directory']/(obj.name+'_facades.json')
            if profile_path.exists():
                profile=json.loads(profile_path.read_text())
                measured=profile.get('faces',{}).get(str(face_id))
                exact=measured and measured['image_sha256']==hashlib.sha256(image.tobytes()).hexdigest()
                anchored=measured and measured.get('anchor_sha256')==hashlib.sha256(image[-200:].tobytes()).hexdigest() and measured.get('reference_width_px')==image.shape[1]
                if exact or anchored:
                    region=measured.get('replace_region_px')
                    if region:
                        ra,rb,rc,rd=region
                        shift=0 if exact else len(image)-measured['reference_height_px']
                        rb+=shift;rd+=shift
                        openings=[o for o in openings if o['pixel_rect'][2]<=ra or o['pixel_rect'][0]>=rc or o['pixel_rect'][3]<=rb or o['pixel_rect'][1]>=rd]
                    else:openings=[]
                    for rect in measured.get('window_rects_px',[])+measured.get('storefront_rects_px',[]):
                        a,b,c,d=rect
                        if not exact:
                            shift=len(image)-measured['reference_height_px'];b+=shift;d+=shift
                        if b<1 or d>=len(image):continue
                        openings.append(dict(kind='window',shape='rectangular',pixel_rect=[a,b,c,d],
                            bounds=[a*pitch,(len(image)-d)*pitch,c*pitch,(len(image)-b)*pitch],
                            confidence=1.,recovery='captured_storefront' if rect in measured.get('storefront_rects_px',[]) else 'visually_measured_capture'))
                        if rect in measured.get('storefront_rects_px',[]) and measured.get('storefront_members'):
                            openings[-1]['measured_members']=measured['storefront_members'][measured['storefront_rects_px'].index(rect)]
            if opening_overrides is not None and face_id in opening_overrides:
                openings=copy.deepcopy(opening_overrides[face_id])
            from window_shape import refine as refine_shapes
            openings,shape_changes=refine_shapes(image,openings,pitch)
            if observed_masks is not None and str(mesh.tex_index) in observed_masks:
                evidence=observed_masks[str(mesh.tex_index)]
                yy,xx=np.indices(image.shape[:2]);uv=np.stack([(xx+.5)*pitch,(len(image)-yy-.5)*pitch,np.ones_like(xx)],axis=-1)@affine
                known=cv2.remap(evidence,(uv[:,:,0]*evidence.shape[1]-.5).astype(np.float32),(uv[:,:,1]*evidence.shape[0]-.5).astype(np.float32),cv2.INTER_NEAREST,borderMode=cv2.BORDER_CONSTANT)>0
            else:known=np.ones(image.shape[:2],bool)
            # Rectangular projection bounds include pixels outside triangular,
            # stepped, or courtyard faces. They must never vote as wall evidence.
            covered=np.zeros(image.shape[:2],np.uint8)
            for triangle in face.tris:
                rel=triangle-face.origin
                pixels=np.c_[rel@face.u/pitch,len(image)-rel@face.v/pitch]
                cv2.fillConvexPoly(covered,np.round(pixels).astype(np.int32),1)
            known &= covered.astype(bool)
            for item in openings:
                x0,y0,x1,y1=item['pixel_rect'];item['observed_fraction']=float(known[y0:y1,x0:x1].mean())
                item['confidence']*=.5+.5*item['observed_fraction']
            # Never turn hallucinated ground-floor imagery into a door instance.
            openings=[o for o in openings if o['kind']!='door_candidate' or o['observed_fraction']>=RULES['doors']['min_observed_fraction']
                or (o.get('recovery')=='reviewed_door_capture' and 'support_level_z' in o)]
            xy=(face.tris.reshape(-1,3)-face.origin)@np.stack([face.u,face.v],axis=1)
            boundary=unary_union([Polygon(t) for t in xy.reshape(-1,3,2)])
            openings=[o for o in openings if boundary.buffer(-.025).covers(box(*o['bounds']))]
            deviation=float(np.max(np.abs((face.tris.reshape(-1,3)-face.origin)@face.normal)))
            # Adjacent capture triangles can have similar normals while lying
            # off the same plane. Cutting planar holes would move their seams.
            if deviation>1e-5:openings=[]
            rectification=None;face_glass_texture=None;face_finish_texture=None
            if clean and RULES['facade_rectification']['enabled']:
                from facade_rectify import rectify
                save_preview(directory/f'face_{face_id:03d}_source.png',image)
                image,openings,rectification=rectify(image,openings,pitch)
            from facade_layout import pattern,wall_mask,finish,details
            wall_pixels=wall_mask(image,known,openings,pitch)
            marks=details(image,wall_pixels,pitch) if clean and not capture_wall else []
            marks=[m for m in marks if boundary.buffer(-.025).covers(box(*m['bounds']))]
            finish_pixels=wall_pixels.copy()
            for mark in marks:
                x,y,X,Y=mark['bounds'];finish_pixels[round(len(image)-Y/pitch):round(len(image)-y/pitch),round(x/pitch):round(X/pitch)]=False
            wall_finish=finish(image,finish_pixels,pitch)
            if clean:
                from facade_pattern import panel_color
                estimate=panel_color(image,finish_pixels)
                # Missed glazing can dominate the median of a partly observed
                # facade. Prefer the robust cladding sample over dark pane tone.
                wall_color=estimate['rgb'] if estimate else wall_finish['rgb']
                wall_finish['render_rgb']=list(wall_color)
                # Captured pixels estimate color only; no photographic underlay survives.
                samples=[]
                for opening in openings:
                    ax,ay,bx,by=opening['pixel_rect'];crop=image[ay:by,ax:bx]
                    if crop.size:samples.append(np.percentile(crop.reshape(-1,3),25,axis=0))
                glass=np.clip(np.median(samples,axis=0),RULES['materials']['glass_min'],RULES['materials']['glass_max']) if samples else (45,61,72)
                palette=dict(wall=material(wall_color),frame=material(np.clip(np.array(wall_color)*RULES['materials']['frame_color_factor'],35,190)),
                             reveal=material(np.clip(np.array(wall_color)*RULES['materials']['reveal_color_factor'],20,125)),glazing=material(glass),door_leaf=material((56,49,42)))
                # Frames can be bright metal/stone even on a dark wall. Sample
                # the captured perimeter instead of always darkening masonry.
                frame_samples=[]
                for opening in openings:
                    ax,ay,bx,by=opening['pixel_rect']
                    rim=image[max(0,ay-1):min(len(image),by+1),max(0,ax-1):min(image.shape[1],bx+1)]
                    if min(rim.shape[:2])<3:continue
                    edge=np.concatenate([rim[0],rim[-1],rim[1:-1,0],rim[1:-1,-1]])
                    if opening.get('recovery') in ('captured_curtain_bay','captured_glazed_band'):
                        edge=np.concatenate([rim[1:-1,0],rim[1:-1,-1]])
                    frame_samples.append(np.percentile(edge,RULES['materials']['frame_sample_percentile'],axis=0))
                if frame_samples:palette['frame']=material(np.median(frame_samples,axis=0))
                if wall_finish['kind']=='tile_candidate' and not capture_wall:
                    from facade_layout import procedural_tiles
                    tile=procedural_tiles(wall_finish,image.shape,pitch)
                    th,tw=tile.shape[:2];finish_size=(tw*pitch,th*pitch)
                    face_finish_texture=len(textures)
                    textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(tw,th,raster.encode_dxt1(tile))]))
            if capture_wall:
                from building_rules import captured_wall_material
                finish,_=captured_wall_material(image,openings,pitch)
                fh,fw=finish.shape[:2];wall_size=(fw*pitch,fh*pitch)
                finish=cv2.resize(finish,((fw+3)//4*4,(fh+3)//4*4))
                fh,fw=finish.shape[:2];palette['wall']=len(textures)
                textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(fw,fh,raster.encode_dxt1(finish))]))
                if RULES['captured_glazing']['enabled'] and ((rectification and rectification['status']=='rectified') or any(o.get('surface_mode')=='flush' for o in openings)):
                    glass_image=cv2.resize(image,(fw,fh));face_glass_texture=len(textures)
                    textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(fw,fh,raster.encode_dxt1(glass_image))]))
            report=dict(face_id=face_id,origin=face.origin.tolist(),u=face.u.tolist(),v=face.v.tolist(),normal=face.normal.tolist(),size=list(face.size),openings=len(openings),observed_fraction=float(known.mean()))
            report['shape_changes']=shape_changes
            report['rectification']=rectification
            report['window_finish']='captured' if face_glass_texture is not None else ('procedural' if clean else 'captured')
            report['pattern']=pattern(openings)
            report['wall_finish']=wall_finish
            report['wall_details']=len(marks)
            report['plane_deviation_m']=deviation
            report['geometry_policy']='cut_planar_openings' if openings else 'preserve_source_triangles'
            reports.append(report)
            for mark in marks:
                from building_rules import extrusion
                x,y,X,Y=mark['bounds'];w,h=X-x,Y-y
                key=('wall_detail',round(w,2),round(h,2),tuple(mark['rgb']))
                if key not in style_keys:
                    pid=f'wall_detail_{len(style_keys):03d}'
                    template={'wall_detail':extrusion(box(0,0,1,1),.002,.006)}
                    style_keys[key]=(pid,template)
                    library.append(dict(id=pid,kind='wall_detail',rgb=mark['rgb'],geometry={k:v.tolist() for k,v in template.items()},semantic_status=mark['semantic_status']))
                pid,template=style_keys[key];palette['wall_detail']=material(mark['rgb'])
                local=template['wall_detail'].copy();local[:,0]=x+local[:,0]*w;local[:,1]=y+local[:,1]*h
                emit(local,face,affine,mesh.tex_index,'wall_detail')
                instances.append(dict(prototype=pid,face_id=face_id,kind='wall_detail',bounds=mark['bounds'],origin=(face.origin+face.u*x+face.v*y).tolist(),basis=np.stack([face.u*w,face.v*h,face.normal],axis=1).tolist(),confidence=mark['confidence'],semantic_status=mark['semantic_status']))
            save_preview(directory/f'face_{face_id:03d}_capture.png',image)
            if not openings:
                save_preview(directory/f'face_{face_id:03d}_detections.png',image)
                basis=np.stack([face.u,face.v,face.normal],axis=1)
                local=np.linalg.solve(basis,(face.tris.reshape(-1,3)-face.origin).T).T
                pid=f'wall_{face_id}'
                library.append(dict(id=pid,kind='wall',geometry={'wall':local.tolist()},appearance='Captured wall; no confidently detected openings'))
                instances.append(dict(prototype=pid,face_id=face_id,origin=face.origin.tolist(),basis=np.stack([face.u,face.v,face.normal],axis=1).tolist(),texture_index=mesh.tex_index,uv_transform=affine.tolist()))
                if clean:
                    texture=face_finish_texture if face_finish_texture is not None else palette['wall']
                    if capture_wall or face_finish_texture is not None:
                        size=wall_size if capture_wall else finish_size
                        uv=np.c_[local[:,0]/size[0],1-local[:,1]/size[1]]
                    else:uv=np.zeros((len(indices),2),np.float32)
                    preserved[mesh_index,texture].append((indices,uv))
                else:retained.extend(indices)
                face_id+=1;continue
            from shapely.affinity import translate
            holes=[translate(outline(o['bounds'][2]-o['bounds'][0],o['bounds'][3]-o['bounds'][1],o['shape']),o['bounds'][0],o['bounds'][1]) for o in openings]
            wall=planar(boundary.difference(unary_union(holes)),0)
            emit(wall,face,affine,mesh.tex_index,'wall')
            wall_id=f'wall_{face_id}'
            library.append(dict(id=wall_id,kind='wall',geometry={'wall':wall.tolist()},appearance='Captured facade; per-instance UV transform'))
            instances.append(dict(prototype=wall_id,face_id=face_id,origin=face.origin.tolist(),basis=np.stack([face.u,face.v,face.normal],axis=1).tolist(),texture_index=mesh.tex_index,uv_transform=affine.tolist()))
            debug=image.copy()
            for item in openings:
                x0,y0,x1,y1=item['bounds'];w,h=x1-x0,y1-y0
                px0,py0,px1,py1=item['pixel_rect'];crop=image[py0:py1,px0:px1]
                color=np.median(crop.reshape(-1,3),axis=0)
                from window_detail import measure, geometry
                curtain_part=item.get('recovery') in ('captured_curtain_bay','captured_glazed_band')
                members=measure(crop,w,h,both_polarities=RULES['window_members']['detect_dark_members']) if item['kind']!='door_candidate' and item.get('recovery')!='captured_curtain_bay' else []
                if 'measured_members' in item:members=copy.deepcopy(item['measured_members'])
                if item.get('recovery')=='captured_storefront':
                    from storefront import measure_members
                    members=item.get('measured_members',measure_members(crop,w,h))
                detail_key=tuple((m['axis'],round(m['position'],2),round(m['width_m'],2)) for m in members)
                frame_width=RULES['curtain_wall']['frame_width_m'] if curtain_part else None
                if item['kind'] in ('window','door_candidate'):frame_width=window_style.get('frame_width_m',frame_width)
                key=(item['kind'],item['shape'],round(w/.25),round(h/.25),*(np.round(color/40).astype(int)),detail_key,curtain_part,item.get('surface_mode'),frame_width)
                if key not in style_keys:
                    template=opening_template(w,h,item['kind'],item['shape'],frame_width,item.get('surface_mode'));pid=f'{item["kind"]}_{len(style_keys):03d}'
                    depth=RULES['doors']['recess_m'] if item['kind']=='door_candidate' else (RULES['windows']['flush_recess_m'] if item.get('surface_mode')=='flush' else RULES['windows']['recess_m'])
                    bars=geometry(w,h,item['shape'],members,depth)
                    if len(bars):template['mullion']=bars
                    style_keys[key]=(pid,template)
                    save_preview(directory/(pid+'.png'),crop)
                    library.append(dict(id=pid,kind=item['kind'],shape=item['shape'],reference_size_m=[w,h],captured_members=members,geometry={k:v.tolist() for k,v in template.items()},preview=pid+'.png' if write_previews else None,depth_inferred=True))
                pid,template=style_keys[key]
                previous_palette=palette.copy()
                if clean:
                    palette['mullion']=palette['frame'];palette['interior']=material((28,32,36))
                if clean and members and RULES['materials']['sample_measured_frame_color']:
                    from window_detail import member_color
                    measured_color=member_color(crop,w,h,members)
                    if measured_color is not None:palette['mullion']=material(measured_color)
                elif clean and item.get('recovery')=='captured_storefront':
                    palette['frame']=material(np.percentile(crop.reshape(-1,3),20,axis=0))
                if clean and item['kind'] in ('window','door_candidate'):
                    for part in ('frame','mullion','glazing','reveal','door_leaf'):
                        if part+'_rgb' in window_style:palette[part]=material(window_style[part+'_rgb'])
                for semantic,points in template.items():
                    local=points.copy();local[:,0]=x0+local[:,0]*w;local[:,1]=y0+local[:,1]*h
                    emit(local,face,affine,mesh.tex_index,semantic)
                palette.clear();palette.update(previous_palette)
                origin=face.origin+face.u*x0+face.v*y0
                support=item.get('support_level_z',bottom)
                if item['kind']=='door_candidate' and not(support-.01<=origin[2]<=support+RULES['doors']['max_sill_offset_m']):
                    raise ValueError('Door instance must meet the building base or an evidenced landing')
                transform=np.array([[w,0,0],[0,h,0],[x0,y0,1.]])@affine
                instances.append(dict(prototype=pid,face_id=face_id,kind=item['kind'],surface_mode=item.get('surface_mode'),origin=origin.tolist(),basis=np.stack([face.u*w,face.v*h,face.normal],axis=1).tolist(),texture_index=mesh.tex_index,uv_transform=transform.tolist(),bounds=item['bounds'],confidence=item['confidence'],observed_fraction=item['observed_fraction'],recovery=item.get('recovery','detected'),semantic_status='inferred_ground_level_opening' if item['kind']=='door_candidate' else ('ml_repaired_opening' if item['observed_fraction']<.8 else 'repeated_capture_opening')))
                if item['kind']=='door_candidate':
                    instances[-1]['support_level_z']=float(support)
                    instances[-1]['semantic_status']='inferred_patio_opening' if 'support_level_z' in item else 'inferred_ground_level_opening'
                    if item.get('recovery')=='reviewed_door_capture':instances[-1]['semantic_status']='reviewed_ground_level_door'
                cv2.rectangle(debug,(px0,py0),(px1,py1),(255,130,20) if item['kind']=='door_candidate' else (40,240,70),1)
            save_preview(directory/f'face_{face_id:03d}_capture.png',image)
            save_preview(directory/f'face_{face_id:03d}_detections.png',debug)
            face_id+=1
        if retained:
            kept=copy.copy(mesh);kept.draw_calls=[(4,np.asarray(retained,np.uint32))]
            if clean and not capture_wall:
                # Roof caps and tiny unclassified shell sections also lose photo imagery.
                kept.tex_index=material((125,124,119));kept.uvs=np.zeros((len(kept.vertices),2),np.float32);kept.colors=None;kept.material=0
            untouched.append(kept)
        if roof_indices:
            kept=copy.copy(mesh);kept.draw_calls=[(4,np.asarray(roof_indices,np.uint32))]
            native_parts.append((kept,'roof'))
    meshes=list(untouched);semantic_meshes=[]
    for mesh,semantic in native_parts:
        semantic_meshes.append(dict(local_mesh_index=len(meshes),part=semantic))
        meshes.append(mesh)
    for (mesh_index,texture),pieces in preserved.items():
        # Keep original float32 vertices AND translation; rebasing all fragments
        # at one new center can itself break seams through rounding.
        mesh=copy.copy(own[mesh_index]);mesh.tex_index=texture;mesh.material=material_for_part('wall');mesh.colors=None
        mesh.uvs=np.zeros((len(mesh.vertices),2),np.float32)
        for indices,uv in pieces:mesh.uvs[indices]=uv
        mesh.draw_calls=[(4,np.concatenate([indices for indices,uv in pieces]))]
        semantic_meshes.append(dict(local_mesh_index=len(meshes),part='wall'))
        meshes.append(mesh)
    for (texture,semantic),pieces in parts.items():
        world=np.concatenate([p for p,uv in pieces]);uv=np.concatenate([uv for p,uv in pieces])
        semantic_meshes.append(dict(local_mesh_index=len(meshes),part=semantic))
        meshes.append(mtscene.Mesh(texture,own[0].object_id,center,(world-center).astype(np.float32),uv.astype(np.float32),None,[(4,np.arange(len(world),dtype=np.uint32))],material_for_part(semantic),0))
    if clean and not capture_wall:
        for mesh in untouched:
            mesh.tex_index=material((125,124,119));mesh.uvs=np.zeros((len(mesh.vertices),2),np.float32);mesh.colors=None;mesh.material=0
    counts=Counter(i.get('kind','wall') for i in instances)
    catalog=dict(captured_wall_material=capture_wall,ruleset=RULESET_VERSION,clean_procedural_materials=clean,building=obj.name,building_base_z=bottom,door_rule=dict(max_sill_offset_m=RULES['doors']['max_sill_offset_m'],min_observed_fraction=RULES['doors']['min_observed_fraction'],reference='overall_building_base'),prototypes=library,instances=instances,faces=reports,semantic_meshes=semantic_meshes,
                 counts=dict(counts),note=('Captured wall finish with procedural frames and native glass.' if capture_wall else ('Clean procedural wall/frame/glazing materials; capture UVs are reference metadata only.' if clean else 'UV transforms retain individual capture variation.'))+' Renderer export expands shared prototypes; GPU instancing is not implemented. Door semantics and recess depths are inferred.')
    catalog['retained_native_parts']=dict(Counter(role for _,role in native_parts))
    if window_style:catalog['window_style']=window_style
    (directory/'tiles.json').write_text(json.dumps(catalog,indent=2,default=json_value))
    usage=Counter(i['prototype'] for i in instances)
    return meshes,dict(building=obj.name,counts=dict(counts),prototypes=len(library),instances=len(instances),
                       reused_opening_prototypes=sum(n>1 and not key.startswith('wall_') for key,n in usage.items()),
                       inferred_windows=sum(i.get('semantic_status')=='ml_repaired_opening' for i in instances),
                       arched_prototypes=sum(p.get('shape')=='arched' for p in library),catalog=str(directory/'tiles.json'))


def semantic_review(scene,out,reports):
    """Diagnostic colors only; the main export keeps captured materials."""
    review=copy.deepcopy(scene);catalogs={r['building']:r['catalog'] for r in reports if 'catalog' in r}
    colors=dict(wall=(170,160,145),roof=(125,124,119),wall_detail=(220,60,180),frame=(220,220,210),mullion=(200,200,190),interior=(28,32,36),reveal=(55,60,65),glazing=(30,140,210),door_leaf=(245,120,30))
    for batch in review:
        for group in batch.groups:
            textures={}
            for role,color in colors.items():
                texture=np.full((4,4,3),color,np.uint8);textures[role]=len(group.textures)
                group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(4,4,raster.encode_dxt1(texture))]))
            for oid,obj in enumerate(group.objects):
                if obj.name not in catalogs:continue
                catalog=json.loads(Path(catalogs[obj.name]).read_text());own=[m for m in group.meshes if m.object_id==oid]
                roles={r['local_mesh_index']:r['part'] for r in catalog['semantic_meshes']}
                for mi,mesh in enumerate(own):
                    mesh.tex_index=textures[roles.get(mi,'wall')];mesh.material=0;mesh.colors=None
                    mesh.uvs=np.zeros((len(mesh.vertices),2),np.float32)
    mtscene.save(out/'semantic_review.mtscene',review)


def main(scene_path,out,only=None,review=False,clean=False,preserve_roofs=False):
    out=Path(out)
    if out.exists():raise ValueError('Use a new output directory')
    path=Path(scene_path).resolve();digest=hashlib.sha256(path.read_bytes()).hexdigest()
    scene=mtscene.load(path);out.mkdir(parents=True);reports=[];config_record=snapshot(out)
    provenance_path=path.parent/'observed_masks.npz'
    provenance_digest=hashlib.sha256(provenance_path.read_bytes()).hexdigest() if provenance_path.exists() else None
    provenance=np.load(provenance_path) if provenance_digest else None
    for bi,batch in enumerate(scene):
        for gi,group in enumerate(batch.groups):
            observed={k.split('_',2)[2]:provenance[k] for k in provenance.files if k.startswith(f'{bi}_{gi}_')} if provenance is not None else None
            output=[]
            for oid,obj in enumerate(group.objects):
                own=[m for m in group.meshes if m.object_id==oid]
                if obj.cls!=3 or not own:output+=own;continue
                if obj.name=='building_367' or (only and obj.name not in only):
                    output+=own;reports.append(dict(building=obj.name,status='existing_refined_preserved' if obj.name=='building_367' else 'not_selected'));continue
                directory=out/obj.name;directory.mkdir()
                meshes,report=refine_building(own,obj,group.textures,directory,observed,clean,preserve_roofs=preserve_roofs)
                from solid_city import audit_shell
                before,after=audit_shell(own),audit_shell(meshes)
                if any(after[key]>before[key] for key in before):
                    raise ValueError(f'{obj.name}: facade tiles damaged shell topology: {before} -> {after}')
                report.update(audit_before=before,audit_after=after)
                output+=meshes;reports.append(report)
                print(f'{obj.name}: {report["counts"]}, {report["prototypes"]} prototypes',flush=True)
            group.meshes=output
    mtscene.save(out/'city_tiles.mtscene',scene)
    if review:semantic_review(scene,out,reports)
    assert hashlib.sha256(path.read_bytes()).hexdigest()==digest
    if provenance_digest:assert hashlib.sha256(provenance_path.read_bytes()).hexdigest()==provenance_digest
    (out/'manifest.json').write_text(json.dumps(dict(config=config_record,input_scene=str(path),input_sha256=digest,observed_masks_sha256=provenance_digest,buildings=reports),indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('scene');p.add_argument('output');p.add_argument('--only');p.add_argument('--semantic-review',action='store_true');p.add_argument('--clean-materials',action='store_true');p.add_argument('--preserve-roofs',action='store_true');p.add_argument('--rules');a=p.parse_args();configure(a.rules)
    main(a.scene,a.output,set(a.only.split(',')) if a.only else None,a.semantic_review,a.clean_materials,a.preserve_roofs)
