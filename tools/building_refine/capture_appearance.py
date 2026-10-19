"""Bake original capture appearance onto repaired geometry without moving vertices."""
import argparse,copy,json,hashlib
from collections import defaultdict
from pathlib import Path
import numpy as np
import mtscene,raster,building,refine


def faces(mesh):
    world=mesh.vertices.astype(float)+mesh.translation
    for primitive,indices in mesh.draw_calls:
        if primitive!=4:raise ValueError('Expected triangles')
        tri=indices.reshape(-1,3);p=world[tri];n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);n/=np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-12)
        # Walls are emitted as adjacent quad triangles by the solid-shell builder.
        wall=abs(n[:,2])<.5
        groups=[]
        used=set()
        for i in np.flatnonzero(wall):
            if i in used:continue
            pair=[i]
            if i+1<len(tri) and wall[i+1] and n[i]@n[i+1]>.999:pair.append(i+1)
            used.update(pair);groups.append(pair)
        for sign in (-1,1):
            selected=np.flatnonzero(n[:,2]*sign>.5)
            # Roof assemblies contain several levels/slopes in one mesh.
            # Projecting all of them at their mean height loses low decks.
            planes=defaultdict(list)
            for i in selected:
                key=(*np.round(n[i],3),round(float(n[i]@p[i].mean(0)),2))
                planes[key].append(i)
            groups.extend(planes.values())
        for selection in groups:
            selection=np.asarray(selection);points=p[selection];normal=n[selection[0]]
            if abs(normal[2])<.5:
                u=np.array([-normal[1],normal[0],0.]);v=np.array([0.,0.,1.]);kind='wall'
            else:
                u=np.array([1.,0.,-normal[0]/normal[2]]);u/=np.linalg.norm(u)
                v=np.cross(normal,u);kind='roof'
            flat=points.reshape(-1,3);a,b=flat@u,flat@v
            origin=u*a.min()+v*b.min()+normal*np.mean(flat@normal)
            yield building.Face(points,origin,u,v,normal,(max(.01,np.ptp(a)),max(.01,np.ptp(b))),kind),tri[selection].ravel()


def bake(group,source,texel=.08,inpainter=None,provenance=None,only=None,source_cache=None):
    src,textures=source_cache if source_cache is not None else (raster.collect(source),raster.decode_textures(source,size=512))
    # Nearby segmentation fragments are useful, but trees/ground must never paint walls.
    building_ids=np.array([i for i,o in enumerate(source.objects) if o.cls==3])
    useful_texture=np.array([np.mean(t.max(axis=2)>=12)>.01 for t in textures])
    valid_source=np.isin(src.obj,building_ids)&(src.tex>=0)&useful_texture[np.clip(src.tex,0,len(textures)-1)]
    tri_min=src.pos.min(1);tri_max=src.pos.max(1)
    reports=[];result=[]
    for oid,obj in enumerate(group.objects):
        own=[m for m in group.meshes if m.object_id==oid]
        if obj.cls!=3 or (obj.name=='building_367' and only is None) or (only is not None and obj.name not in only) or not own:result+=own;continue
        world=np.concatenate([m.vertices.astype(float)+m.translation for m in own]);lo,hi=world.min(0)-8,world.max(0)+8
        selected=valid_source&np.all(tri_max>=lo,axis=1)&np.all(tri_min<=hi,axis=1)
        subset=raster.SceneTris(src.pos[selected],src.uv[selected],src.tex[selected],src.obj[selected],src.mesh[selected])
        normals=np.cross(subset.pos[:,1]-subset.pos[:,0],subset.pos[:,2]-subset.pos[:,0]);normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-12)
        atlas=refine.Atlas();pieces=defaultdict(list);samples=[];mask_pages=[]
        for mi,mesh in enumerate(own):
            for face,indices in faces(mesh):
                pitch=max(texel,max(face.size)/(refine.PAGE-2*refine.PAD-4))
                aligned=abs(normals@face.normal)>.65
                near=raster.SceneTris(subset.pos[aligned],subset.uv[aligned],subset.tex[aligned],subset.obj[aligned],subset.mesh[aligned])
                image,depth,covered=refine.project_face(face,near,textures,pitch,(-6.,6.),nearest_plane=face.kind=='wall')
                # Dark windows/doorways are real evidence. Reject empty textures
                # above, not individual black pixels in otherwise useful imagery.
                covered &= ~raster.yellowish(image)
                if face.normal[2]<-.5:covered[:]=False
                coverage=float(covered.mean());repair_report=None
                if inpainter is not None and face.kind=='wall':
                    from facade_inpaint import repair
                    image,inferred,repair_report=repair(image,covered,pitch,inpainter)
                else:image=refine.fill_holes(image,covered)
                page,x,y=atlas.add(image);height=image.shape[0]
                if provenance is not None:
                    while len(mask_pages)<=page:mask_pages.append(np.zeros((refine.PAGE,refine.PAGE),np.uint8))
                    mask_pages[page][y:y+height,x:x+image.shape[1]]=covered.astype(np.uint8)
                points=mesh.vertices[indices].astype(float)+mesh.translation
                uv=np.c_[(points-face.origin)@face.u/pitch+x,height-(points-face.origin)@face.v/pitch+y]
                pieces[mi,page].append((indices,uv));samples.append(dict(kind=face.kind,area=float(np.prod(face.size)),coverage=coverage,repair=repair_report))
        mapping={}
        for pi in range(len(atlas.pages)):
            w,h=atlas.page_size(pi);mapping[pi]=len(group.textures)
            if provenance is not None:provenance[str(mapping[pi])]=mask_pages[pi][:h,:w]
            group.textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(w,h,raster.encode_dxt1(np.ascontiguousarray(atlas.pages[pi][:h,:w])))]))
        for (mi,page),items in pieces.items():
            mesh=copy.copy(own[mi]);w,h=atlas.page_size(page);mesh.uvs=np.zeros((len(mesh.vertices),2),np.float32)
            for indices,uv in items:mesh.uvs[indices]=uv/[w,h]
            mesh.draw_calls=[(4,np.concatenate([indices for indices,uv in items]))];mesh.tex_index=mapping[page];mesh.material=0;result.append(mesh)
        area=sum(s['area'] for s in samples if s['kind']=='wall')
        cov=sum(s['area']*s['coverage'] for s in samples if s['kind']=='wall')/max(area,1e-9)
        reports.append(dict(name=obj.name,wall_capture_coverage=cov,faces=samples))
        print(f'{len(reports)} {obj.name}: wall capture coverage {cov:.1%}',flush=True)
    group.meshes=result
    return reports


def main(source_path,solid_path,out,lama=False):
    out=Path(out)
    if out.exists():raise ValueError('Use a new output directory')
    hashes={str(Path(p).resolve()):hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in (source_path,solid_path)}
    source=mtscene.load(source_path);scene=mtscene.load(solid_path);reports=[];inpainter=None;provenance={}
    if lama:
        from inpaint import LamaInpainter
        inpainter=LamaInpainter(tile_size=512,margin=64,log=lambda s:print(s,flush=True))
    for bi,b in enumerate(scene):
        for gi,g in enumerate(b.groups):
            masks={};reports+=bake(g,source[bi].groups[gi],inpainter=inpainter,provenance=masks)
            provenance.update({f'{bi}_{gi}_{key}':value for key,value in masks.items()})
    out.mkdir(parents=True);mtscene.save(out/'city_capture_appearance.mtscene',scene)
    np.savez_compressed(out/'observed_masks.npz',**provenance)
    for p,digest in hashes.items():
        if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=digest:raise RuntimeError('Input changed during bake: '+p)
    (out/'manifest.json').write_text(json.dumps(dict(input_sha256=hashes,source=str(Path(source_path).resolve()),solid_scene=str(Path(solid_path).resolve()),buildings=reports,ml_repair=lama,note='Only UVs, textures and draw-call grouping changed. With --lama, inferred pixels use same-row repeats and LaMa; observed_masks.npz retains original evidence. Otherwise missing areas use color fill. No damaged geometry is restored.'),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('solid');p.add_argument('output');p.add_argument('--lama',action='store_true');a=p.parse_args();main(a.source,a.solid,a.output,a.lama)
