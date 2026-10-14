"""Deterministic, bounded facade fitting against saved capture geometry.

Eight orthographic cameras share fixed framing. Even views drive coordinate
descent; odd views are a final acceptance check, never a search objective.
The software captures compare unlit texture/shape, not environment reflections.
No external capture, network, or ML models are needed.
"""
import copy
import json
from pathlib import Path
from types import SimpleNamespace

import cv2
import numpy as np

import raster


class Cameras:
    def __init__(self, source, textures, resolution=512):
        self.resolution = resolution
        self.cache = {}
        tris, tex = self.collect(source, textures)
        p = tris.pos.reshape(-1, 3)
        self.center = (p.min(0) + p.max(0)) / 2
        self.span = max(float(np.linalg.norm(np.ptp(p, axis=0))) * 1.10, 1.)
        self.bases = []
        for yaw in np.arange(8) * np.pi / 4 + .17:
            elevation = np.deg2rad(20)
            n = np.array([np.cos(yaw)*np.cos(elevation), np.sin(yaw)*np.cos(elevation), np.sin(elevation)])
            u = np.array([-np.sin(yaw), np.cos(yaw), 0.])
            self.bases.append(np.stack([u, np.cross(n, u), n], axis=1))
        self.reference = self.render_tris(tris, tex)
        # Fit only actual textured walls; roofs and placeholder imagery cannot
        # reward arbitrary facade colors. Shape checks still include every pixel.
        normals = np.cross(tris.pos[:,1]-tris.pos[:,0], tris.pos[:,2]-tris.pos[:,0])
        wall = np.abs(normals[:,2]) < .20*np.linalg.norm(normals, axis=1)
        outward=np.sum((tris.pos.mean(1)-self.center)*normals,axis=1)
        normals[outward<0]*=-1
        self.normals=normals
        self.wall=wall
        valid_tri = wall & (tris.tex >= 0)
        self.valid = []
        for image, depth, ids in self.reference:
            valid = (ids >= 0) & valid_tri[np.maximum(ids, 0)] & ~raster.placeholder_pixels(image)
            self.valid.append(cv2.erode(valid.astype(np.uint8), np.ones((3,3), np.uint8)).astype(bool))

    def face_colors(self):
        from facade import wall_directions
        normals=self.normals.copy()
        lengths=np.linalg.norm(normals,axis=1)
        normals/=np.maximum(lengths[:,None],1e-12)
        axes=wall_directions(normals[self.wall],lengths[self.wall])[:8]
        if not axes:return []
        dots=normals@np.asarray(axes).T
        labels=dots.argmax(1)
        confident=dots.max(1)>.90
        result=[]
        for j,axis in enumerate(axes):
            samples=[]
            for i in range(0,8,2):
                image,_,ids=self.reference[i]
                mask=self.valid[i] & (labels[np.maximum(ids,0)]==j) & confident[np.maximum(ids,0)]
                if mask.any():samples.append(image[mask])
            if not samples:continue
            pixels=np.concatenate(samples).astype(float)
            if len(pixels)<128:continue
            lum=pixels@np.array([.2126,.7152,.0722]);lo,hi=np.percentile(lum,[30,70])
            result.append(dict(normal=axis.tolist(),
                wall_color=np.median(pixels[lum>=hi],axis=0).round().astype(int).tolist(),
                glass_color=np.median(pixels[lum<=lo],axis=0).round().astype(int).tolist()))
        return result

    def collect(self, meshes, textures):
        used = sorted({m.tex_index for m in meshes if m.uvs is not None and 0 <= m.tex_index < len(textures)})
        remap = {old:i for i,old in enumerate(used)}
        parts = []
        for mesh in meshes:
            part = copy.copy(mesh)
            part.tex_index = remap.get(mesh.tex_index, 0xffffffff) if mesh.uvs is not None else 0xffffffff
            parts.append(part)
        decoded = []
        for old in used:
            texture = textures[old]
            key = id(texture)
            if key not in self.cache:
                self.cache[key] = (texture, raster.decode_textures(SimpleNamespace(textures=[texture]))[0])
            decoded.append(self.cache[key][1])
        group = SimpleNamespace(meshes=parts, textures=[textures[i] for i in used])
        return raster.collect(group), np.stack(decoded) if decoded else np.zeros((1,256,256,3),np.uint8)

    def render_tris(self, tris, textures):
        size = self.resolution
        result = []
        for basis in self.bases:
            scr = (tris.pos-self.center) @ basis
            scr[:,:,:2] *= size/self.span
            scr[:,:,1] *= -1
            scr[:,:,:2] += size/2
            depth = np.full((size,size), -np.inf, np.float32)
            image = np.full((size,size,3), 235, np.uint8)
            ids = np.full((size,size), -1, np.int32)
            raster.rasterize(scr, tris.uv, tris.tex, np.arange(len(tris.pos),dtype=np.int32),
                             textures, size, size, depth, image, ids, -1e12, 1e12)
            result.append((image,depth,ids))
        return result

    def render(self, meshes, textures):
        return self.render_tris(*self.collect(meshes,textures))

    def scores(self, views):
        scores = []
        for ref, actual, valid in zip(self.reference, views, self.valid):
            a,da,ia = ref
            b,db,ib = actual
            ma,mb = ia>=0,ib>=0
            union = ma|mb
            common = ma&mb
            iou = float(common.sum()/max(1,union.sum()))
            depth = float(np.mean(np.abs(da[common]-db[common]))) if common.any() else self.span
            mask = valid & mb
            if mask.sum() >= 64:
                # Two scales reduce subpixel aliasing while still penalizing
                # shifted window rows and incorrect opening proportions.
                aa,bb = a.astype(np.float32)/255,b.astype(np.float32)/255
                fine = float(np.abs(cv2.GaussianBlur(aa,(0,0),.65)-cv2.GaussianBlur(bb,(0,0),.65))[mask].mean())
                coarse = float(np.abs(cv2.GaussianBlur(aa,(0,0),2)-cv2.GaussianBlur(bb,(0,0),2))[mask].mean())
                photo = .6*fine+.4*coarse
                ga=cv2.cvtColor(aa,cv2.COLOR_RGB2GRAY);gb=cv2.cvtColor(bb,cv2.COLOR_RGB2GRAY)
                mean_a=cv2.GaussianBlur(ga,(0,0),1.5);mean_b=cv2.GaussianBlur(gb,(0,0),1.5)
                va=np.maximum(0,cv2.GaussianBlur(ga*ga,(0,0),1.5)-mean_a*mean_a)
                vb=np.maximum(0,cv2.GaussianBlur(gb*gb,(0,0),1.5)-mean_b*mean_b)
                covariance=cv2.GaussianBlur(ga*gb,(0,0),1.5)-mean_a*mean_b
                bound=np.sqrt(va*vb);covariance=np.clip(covariance,-bound,bound)
                structure=float((np.clip(1-(2*covariance+.0009)/(va+vb+.0009),0,2)*.5)[mask].mean())
            else:
                photo = None
                structure=0.
            loss = (photo if photo is not None else 0.)+.75*structure+2*(1-iou)+min(1.,depth/self.span)*2
            scores.append(dict(loss=loss,photo=photo,structure=structure,silhouette_iou=iou,depth_mae_m=depth,pixels=int(mask.sum())))
        return scores


def aggregate(scores, indices):
    return float(np.mean([scores[i]['loss'] for i in indices]))


def accepts(baseline, candidate, regularized=False):
    """Training gain must generalize, without a substantial individual-view loss."""
    return (aggregate(candidate,range(0,8,2)) < aggregate(baseline,range(0,8,2))-1e-5
            and aggregate(candidate,range(1,8,2)) <= aggregate(baseline,range(1,8,2))+1e-5
            and all(b['silhouette_iou'] >= a['silhouette_iou']-(.01 if regularized else .002)
                    and b['depth_mae_m'] <= a['depth_mae_m']+(.65 if regularized else .05)
                    and b['loss'] <= a['loss']+.015 for a,b in zip(baseline,candidate)))


def fit(source, oid, texture_base, textures, finish, baseline, iterations=3,
        report_dir=None, progress=lambda *args:None, initial_parameters=None):
    import facade
    cameras = Cameras(source,textures)
    original_new, original_tex, original_stats = baseline
    original_views = cameras.render(original_new,textures+original_tex)
    initial_scores = cameras.scores(original_views)
    best = baseline
    best_views, best_scores = original_views, initial_scores
    parameters = dict(initial_parameters or {})
    history = [dict(iteration=0,parameters=dict(parameters),train=aggregate(initial_scores,range(0,8,2)))]
    evaluated={json.dumps(parameters,sort_keys=True):history[0]['train']}
    # Color seeds are estimated exclusively from the training camera captures.
    samples = [view[0][mask] for i,(view,mask) in enumerate(zip(cameras.reference,cameras.valid)) if i%2==0 and mask.any()]
    eligible = all(cameras.valid[i].sum()>=64 for i in range(8))
    if eligible and samples:
        pixels = np.concatenate(samples).astype(float)
        lum = pixels @ np.array([.2126,.7152,.0722])
        dark,light = np.percentile(lum,[30,70])
        colors = dict(wall_color=tuple(np.median(pixels[lum>=light],axis=0).round().astype(int).tolist()),
                      glass_color=tuple(np.median(pixels[lum<=dark],axis=0).round().astype(int).tolist()))
        count=0
        for iteration in range(iterations):
            step = .5**iteration
            proposals = []
            if iteration==0:
                proposals=[dict(align_grid=True),colors,dict(face_colors=cameras.face_colors()),dict(capture_layout=True)]
                # Staggered rows are competing architectural hypotheses, not
                # random decoration. Held-out cameras must validate the winner.
                proposals += [dict(row_offsets=offsets) for offsets in
                              ([0.],[0.,.5],[.5,0.],[0.,0.,.5],[0.,.5,0.],[.5,0.,0.])]
            # Coordinate descent: every later coordinate starts at the winner
            # so distinct improvements accumulate across iterations.
            dimensions = [('sill_fraction',.65/original_stats['floor'],.18*step,.08,.55),
                          ('jamb_fraction',.10/original_stats['bay'],.10*step,.03,.30),
                          ('floor_offset',0.,.25*step,-1.,1.),
                          ('bay_offset',0.,.25*step,-1.,1.)]
            def evaluate(candidate):
                nonlocal best,best_views,best_scores,parameters,count
                count+=1
                progress(count,iterations*10+10,f"PCG camera fit iteration {iteration+1}/{iterations}, candidate {count}")
                key=json.dumps(candidate,sort_keys=True)
                if key in evaluated:
                    history.append(dict(iteration=iteration+1,parameters=candidate,train=evaluated[key],reused=True))
                    return
                try:
                    result=facade.build(source,oid,texture_base,textures,finish,candidate,
                                        None if candidate.get('regular_facades') else original_stats['pattern'])
                    if not result[2]['windows']:return
                    views=cameras.render(result[0],textures+result[1])
                    scores=cameras.scores(views)
                    score=aggregate(scores,range(0,8,2))
                    evaluated[key]=score
                    history.append(dict(iteration=iteration+1,parameters=candidate,train=score))
                    if score < aggregate(best_scores,range(0,8,2))-1e-5:
                        best,best_views,best_scores,parameters=result,views,scores,candidate
                except (ValueError,facade.shapely.errors.GEOSException) as error:
                    history.append(dict(iteration=iteration+1,parameters=candidate,error=str(error)))
            for proposal in proposals:evaluate({**parameters,**proposal})
            dimensions.append(('bay_scale',1.,.20*step,.7,1.6))
            for name,default,delta,low,high in dimensions:
                start=dict(parameters)
                for sign in (-1,1):
                    candidate={**start,name:float(np.clip(start.get(name,default)+sign*delta,low,high))}
                    evaluate(candidate)
            print(f"  camera fit iteration {iteration+1}/{iterations}: training loss {aggregate(best_scores,range(0,8,2)):.5f}",flush=True)
    accepted=eligible and accepts(initial_scores,best_scores,bool(best[2].get('regularization')))
    report=dict(camera_count=8,resolution=cameras.resolution,center=cameras.center.tolist(),span=cameras.span,
                yaw_degrees=(np.arange(8)*45+np.rad2deg(.17)).tolist(),elevation_degrees=20,
                training_views=[0,2,4,6],validation_views=[1,3,5,7],
                iterations=iterations,accepted=accepted,eligible=eligible,
                reason=('improved training and passed validation' if accepted else
                        'insufficient textured wall coverage' if not eligible else
                        'no generalizing improvement within the search budget'),
                metric='unlit multiscale RGB error + .75*local structural error + 2*(1-silhouette IoU) + 2*depth MAE / scene span',
                regularization=best[2].get('regularization'),
                baseline=initial_scores,candidate=best_scores,parameters=parameters,history=history)
    if report_dir:
        destination=Path(report_dir);destination.mkdir(parents=True,exist_ok=True)
        (destination/'fit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        rows=[]
        for i,(ref,before,after) in enumerate(zip(cameras.reference,original_views,best_views)):
            images=[]
            for label,view in [('Capture',ref),('Baseline PCG',before),('Fitted candidate',after)]:
                image=cv2.cvtColor(view[0],cv2.COLOR_RGB2BGR)
                image=cv2.copyMakeBorder(image,24,0,0,0,cv2.BORDER_CONSTANT,value=(250,250,250))
                cv2.putText(image,f'{label} | {i} '+('fit' if i%2==0 else 'check'),(6,17),cv2.FONT_HERSHEY_SIMPLEX,.42,(25,25,25),1,cv2.LINE_AA)
                images.append(image)
            rows.append(np.concatenate(images,axis=1))
        cv2.imwrite(str(destination/'cameras.png'),np.concatenate(rows,axis=0))
    print(f"  multi-camera fit: {'accepted' if accepted else 'baseline retained'}; "
          f"training {aggregate(initial_scores,range(0,8,2)):.5f} -> {aggregate(best_scores,range(0,8,2)):.5f}, "
          f"validation {aggregate(initial_scores,range(1,8,2)):.5f} -> {aggregate(best_scores,range(1,8,2)):.5f}",flush=True)
    chosen=best if accepted else baseline
    chosen[2]['multiview']=report
    return chosen
