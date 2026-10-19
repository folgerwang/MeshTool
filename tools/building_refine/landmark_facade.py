"""Evidence-led, single-building facade study; never a city-wide style override.

The measured profile is in rectified capture pixels (8 cm/pixel). Dimensions
and repetition are observed; recess depths, masonry joints and trim profiles
are estimates. Optional captured wall materials remove opening imagery before
projection; procedural frames, glazing and trim remain separate geometry.
"""
import argparse
import copy
import hashlib
import json
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np
from shapely.affinity import translate
from shapely.geometry import box
from shapely.ops import unary_union

import mtscene
import raster
from capture_appearance import faces
from capture_tiles import outline, planar, opening_template, facade_image, decode
from solid_city import audit_shell
from rules_config import RULES,configure,snapshot


# Main elevations, measured independently rather than copied around the building.
PROFILE = {int(k):v for k,v in json.loads((Path(__file__).parent/RULES['profiles']['building_080']).read_text())['facades'].items()}


def measured_openings(face_id, height=498):
    """Semantic rows: whole windows, never individual dark panes or dentils."""
    if face_id not in PROFILE:
        return []
    profile=PROFILE[face_id];result=[]
    def add(cx,y0,y1,width,shape,role):
        bounds=[(cx-width/2)*.08,(height-y1)*.08,(cx+width/2)*.08,(height-y0)*.08]
        result.append(dict(bounds=bounds,shape=shape,role=role,kind='window',source='measured_capture_row'))
    for cx in profile['upper']:
        add(cx,130,151,14,'rectangular','upper')
    for cx in profile['centers']:
        add(cx,194,269,30,'arched','tall_arch')
        add(cx,290,324,40,'rectangular','triple')
        # These are glazed ground-floor bays. Door function is not inferred
        # from every tall opening; upper floors can never become entrances.
        add(cx,365,493,profile['ground_width'],'arched','ground_arch')
    return result


from building_rules import extrusion, RULESET_VERSION, material_for_part


def masonry():
    """Low-contrast ashlar, 3.2 x 3.2 metres; joints are inferred, not surveyed."""
    rng=np.random.default_rng(80);n=512
    pixels=np.full((n,n,3),(205,205,201),float)
    for row in range(8):
        for col in range(-1,3):
            x0=col*256+(128 if row%2 else 0)
            pixels[row*64:(row+1)*64,max(0,x0):min(n,x0+256)]+=rng.uniform(-2.5,2.5)
        pixels[row*64:row*64+1]*=.94
        for x in range(128 if row%2 else 0,n,256):pixels[row*64:(row+1)*64,x:x+1]*=.94
    # No high-frequency noise: the capture shows smooth dressed masonry.
    return np.clip(pixels,0,255).astype(np.uint8)


from building_rules import captured_wall_material


def build(scene_path,output,details=False,roof=False,capture_wall=False):
    path=Path(scene_path);digest=hashlib.sha256(path.read_bytes()).hexdigest()
    scene=mtscene.load(path);report=[]
    for batch in scene:
        for group in batch.groups:
            ids=[i for i,o in enumerate(group.objects) if o.name=='building_080']
            own=[m for m in group.meshes if m.object_id in ids]
            if not own:continue
            center=own[0].translation.copy();parts=defaultdict(list);base_parts=[]
            active_wall=None;wall_size=None
            textures={}
            for role,color in dict(interior=(28,32,36),wall=(205,205,201),trim=(218,218,211),frame=(62,67,65),glass=(43,48,51),roof=(145,141,131),reveal=(145,142,132),cornice_shadow=(96,99,96),roof_stone=(173,171,159),roof_glass=(43,58,67),roof_metal=(126,135,135)).items():
                image=masonry() if role=='wall' and details else np.full((4,4,3),color,np.uint8)
                h,w=image.shape[:2];textures[role]=len(group.textures)
                group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(w,h,raster.encode_dxt1(image))]))
            def emit(points,face,role,base=False):
                if not len(points):return
                world=face.origin+points[:,0,None]*face.u+points[:,1,None]*face.v+points[:,2,None]*face.normal
                uv=points[:,:2]/3.2 if role=='wall' and details else np.zeros((len(points),2))
                if role=='wall' and active_wall is not None:
                    uv=np.c_[points[:,0]/wall_size[0],1-points[:,1]/wall_size[1]]
                    role=active_wall
                parts[role].append((world,uv))
                if base:base_parts.append(world)
            fid=0
            for mesh in own:
                for face,indices in faces(mesh):
                    if face.kind!='wall':
                        world=face.tris.reshape(-1,3).copy();world[:,2]=np.minimum(world[:,2],50.8 if roof else 16.59716293575893+35.8);parts['roof'].append((world,np.zeros((len(world),2))));base_parts.append(world);continue
                    width,height=face.size
                    height=min(height,50.8-face.origin[2] if roof else 35.8)
                    windows=measured_openings(fid)
                    windows=[w for w in windows if box(.025,.025,width-.025,height-.025).covers(box(*w['bounds']))]
                    if capture_wall:
                        image,_,pitch=facade_image(mesh,face,indices,decode(group.textures[mesh.tex_index]))
                        image,mask=captured_wall_material(image,windows,pitch,cornice_height=30.6)
                        h,w=image.shape[:2];wall_size=(w*pitch,h*pitch)
                        # DXT dimensions must be multiples of four. Resize the
                        # image, keeping UV dimensions in physical metres.
                        image=cv2.resize(image,((w+3)//4*4,(h+3)//4*4),interpolation=cv2.INTER_LINEAR)
                        h,w=image.shape[:2];active_wall=f'captured_wall_{fid}'
                        textures[active_wall]=len(group.textures)
                        group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(w,h,raster.encode_dxt1(image))]))
                        if fid in PROFILE:
                            preview=Path(output).parent;preview.mkdir(parents=True,exist_ok=True)
                            cv2.imwrite(str(preview/f'wall_material_{fid:02d}.png'),cv2.cvtColor(image,cv2.COLOR_RGB2BGR))
                    holes=[]
                    for w in windows:
                        x,y,X,Y=w['bounds'];holes.append(translate(outline(X-x,Y-y,w['shape']),x,y))
                    boundary=box(0,0,width,height)
                    emit(planar(boundary.difference(unary_union(holes)),0),face,'wall',True)
                    for w,hole in zip(windows,holes):
                        x,y,X,Y=w['bounds'];ww,hh=X-x,Y-y
                        for role,points in opening_template(ww,hh,'window',w['shape']).items():
                            p=points.copy();p[:,0]=x+p[:,0]*ww;p[:,1]=y+p[:,1]*hh
                            # Stronger visible recess without photographed shadows.
                            p[:,2]*=3
                            emit(p,face,dict(frame='frame',reveal='reveal',glazing='glass',interior='interior')[role],True)
                        # Interior mullions divide complete assemblies, rather than
                        # making a separate wall hole for each photographed pane.
                        inset=hole.buffer(-.10,join_style=2)
                        mullions=[]
                        fractions=(1/3,2/3) if w['role'] in ('triple','tall_arch') else (.5,)
                        for f in fractions:
                            strip=box(x+ww*f-.055,y+.10,x+ww*f+.055,Y-.10).intersection(inset)
                            if not strip.is_empty:mullions.append(strip)
                        levels=([.32,.63] if w['role']=='ground_arch' else ([.72] if w['role']=='tall_arch' else [.45]))
                        for f in levels:
                            strip=box(x+.1,y+hh*f-.055,X-.1,y+hh*f+.055).intersection(inset)
                            if not strip.is_empty:mullions.append(strip)
                        bars=unary_union(mullions)
                        for poly in getattr(bars,'geoms',[bars]):
                            if poly.area>1e-6:emit(extrusion(poly,-.17,-.05),face,'trim' if w['role']=='tall_arch' and details else 'frame')
                        if details and w['role']=='tall_arch':
                            for f in (1/6,.5,5/6):
                                small=translate(outline(ww/3-.13,hh*.23,'arched'),x+ww*f-(ww/3-.13)/2,y+hh*.70)
                                tracery=small.difference(small.buffer(-.075)).intersection(inset)
                                if not tracery.is_empty:emit(extrusion(tracery,-.15,-.025),face,'trim')
                        if details and w['role']=='ground_arch':
                            # The capture has inset rectangular sash assemblies
                            # below the fanlight, not three empty horizontal panes.
                            for low,high in ((.35,.59),(.02,.29)):
                                panel=box(x+ww*.21,y+hh*low,X-ww*.21,y+hh*high)
                                rim=panel.difference(panel.buffer(-.14))
                                emit(extrusion(rim,-.16,-.018),face,'trim')
                            emit(extrusion(box(x+.12,y+hh*.64,X-.12,y+hh*.64+.14),-.16,-.025),face,'trim')
                        if details:
                            surround=hole.buffer(.19,join_style=2).difference(hole.buffer(.015,join_style=2))
                            emit(extrusion(surround,.015,.17),face,'trim')
                            emit(extrusion(box(x-.27,y-.22,X+.27,y-.06),.025,.30),face,'trim')
                    if roof:
                        emit(extrusion(box(.002,height,width-.002,35.8),-.45,-.002),face,'wall')
                    if details and fid in PROFILE:
                        # Cornice is masonry ornament, not glazing: three stepped
                        # fascia courses and a repeated console/dentil band.
                        for low,high,projection in [(30.6,30.78,.16),(33.7,33.95,.22),(35.2,35.5,.36),(35.5,35.8,.49)]:
                            emit(extrusion(box(.02,low,width-.02,high),.025,projection),face,'trim')
                        emit(extrusion(box(.025,30.85,width-.025,33.65),.003,.012),face,'cornice_shadow')
                        for x in np.arange(.28,width-.25,.86):
                            emit(extrusion(box(x-.11,30.85,x+.11,33.6),.025,.21),face,'trim')
                            emit(extrusion(box(x-.20,33.18,x+.20,33.53),.23,.35),face,'trim')
                        # Shallow corner pilasters and base string course.
                        for x in (.03,width-.62):
                            emit(extrusion(box(x,.3,x+.58,30.5),.025,.13),face,'trim')
                        emit(extrusion(box(.02,.12,width-.02,.34),.025,.22),face,'trim')
                    report.append(dict(face=fid,windows=len(windows),profile=PROFILE.get(fid),openings=windows));fid+=1
            if roof:
                from landmark_roof import geometry
                for points,role in geometry():parts[role].append((points,np.zeros((len(points),2))))
            def mesh_from(points,uv,tex,role='wall'):
                return mtscene.Mesh(tex,ids[0],center,(points-center).astype(np.float32),uv.astype(np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))],material_for_part(role),0)
            meshes=[mesh_from(np.concatenate([p for p,u in pieces]),np.concatenate([u for p,u in pieces]),textures[role],role) for role,pieces in parts.items()]
            base=np.concatenate(base_parts);audit=audit_shell([mesh_from(base,np.zeros((len(base),2)),0)])
            if any(audit.values()):raise ValueError(f'Broken structural envelope: {audit}')
            group.meshes=[m for m in group.meshes if m.object_id not in ids]+meshes
    output=Path(output);output.parent.mkdir(parents=True,exist_ok=True)
    config_record=snapshot(output.parent)
    mtscene.save(output,scene)
    assert hashlib.sha256(path.read_bytes()).hexdigest()==digest
    output.with_suffix('.json').write_text(json.dumps(dict(config=config_record,ruleset=RULESET_VERSION,source=str(path),source_sha256=digest,details=details,captured_wall_material=capture_wall,roof_reconstructed=roof,structural_audit=audit,faces=report,
        limitations=['Manually measured building-specific facade profile','Trim depth and uncaptured details inferred','Captured wall finish retains baked lighting' if capture_wall else 'Masonry joints inferred','Roof equipment and unobserved rear elevations remain approximate']),indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('scene');p.add_argument('output');p.add_argument('--details',action='store_true');p.add_argument('--roof',action='store_true');p.add_argument('--capture-wall',action='store_true');p.add_argument('--rules');a=p.parse_args();configure(a.rules)
    build(a.scene,a.output,a.details,a.roof,a.capture_wall)
