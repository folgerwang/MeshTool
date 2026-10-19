"""Deterministic, bounded facade fitting against saved capture geometry.

Four sides, four diagonal views, and top share fixed orthographic source framing.
Every view must pass absolute error and shape limits before a model is applied.
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

VIEW_NAMES = ('front', 'back', 'left', 'right', 'front_left', 'front_right',
              'back_left', 'back_right', 'top')
TOP_VIEW = VIEW_NAMES.index('top')
SIDE_VIEWS = tuple(i for i in range(len(VIEW_NAMES)) if i != TOP_VIEW)
ALL_VIEWS = tuple(range(len(VIEW_NAMES)))
MIN_SILHOUETTE_IOU = .90
MAX_DEPTH_FRACTION = .02
DEFAULT_MAX_ERROR = .20


class Cameras:
    def __init__(self, source, textures, resolution=512):
        self.resolution = resolution
        self.cache = {}
        tris, tex = self.collect(source, textures)
        p = tris.pos.reshape(-1, 3)
        self.center = (p.min(0) + p.max(0)) / 2
        self.span = max(float(np.linalg.norm(np.ptp(p, axis=0))) * 1.10, 1.)
        normals = np.cross(tris.pos[:,1]-tris.pos[:,0], tris.pos[:,2]-tris.pos[:,0])
        wall = np.abs(normals[:,2]) < .20*np.linalg.norm(normals, axis=1)
        outward=np.sum((tris.pos.mean(1)-self.center)*normals,axis=1)
        normals[outward<0]*=-1
        self.normals=normals
        self.wall=wall
        # Orient each building's side cameras to its dominant facade, rather
        # than assuming every building faces the world's cardinal axes.
        from facade import wall_directions
        lengths = np.linalg.norm(normals, axis=1)
        axes = wall_directions(normals[wall] / np.maximum(lengths[wall,None], 1e-12), lengths[wall])
        front = np.array(axes[0], dtype=float) if axes else np.array([0., -1., 0.])
        front[2] = 0
        front /= max(np.linalg.norm(front), 1e-12)
        right = np.array([-front[1], front[0], 0.])
        # Elevated diagonal cameras reveal corners, setbacks, and roof edges
        # that the straight-on views cannot show together.
        elevation = np.deg2rad(20)
        corners = [np.cos(elevation)*n/np.linalg.norm(n) + np.array([0.,0.,np.sin(elevation)])
                   for n in (front-right, front+right, -front-right, -front+right)]
        directions = (front, -front, -right, right, *corners, np.array([0., 0., 1.]))
        self.bases = []
        for i, n in enumerate(directions):
            u = right if i == TOP_VIEW else np.array([-n[1], n[0], 0.])
            u = u / np.linalg.norm(u)
            self.bases.append(np.stack([u, np.cross(n, u), n], axis=1))
        self.reference = self.render_tris(tris, tex)
        # Facade appearance is measured on walls; top appearance is measured
        # on the roof. Placeholder imagery never counts as valid evidence.
        self.valid = []
        for i, (image, depth, ids) in enumerate(self.reference):
            valid_tri = (~wall if i == TOP_VIEW else wall) & (tris.tex >= 0)
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
            for i in SIDE_VIEWS:
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
            depth_fraction = min(1., depth/self.span)
            loss = .40*(photo if photo is not None else 1.)+.20*structure+.25*(1-iou)+.15*depth_fraction
            scores.append(dict(view=VIEW_NAMES[len(scores)],loss=loss,photo=photo,structure=structure,
                               silhouette_iou=iou,depth_mae_m=depth,depth_fraction=depth_fraction,pixels=int(mask.sum())))
        return scores


def aggregate(scores, indices):
    return float(np.mean([scores[i]['loss'] for i in indices]))


def failures(scores, max_error=DEFAULT_MAX_ERROR):
    """Report every failed view; a good average cannot hide a bad roof or side."""
    result = []
    if len(scores) != len(VIEW_NAMES): return ['missing required camera views']
    for name, score in zip(VIEW_NAMES, scores):
        if score['pixels'] < 64 or score['photo'] is None:
            result.append(f'{name}: insufficient textured evidence')
        if not all(np.isfinite(score[k]) for k in ('loss', 'silhouette_iou', 'depth_fraction')):
            result.append(f'{name}: invalid comparison metrics')
            continue
        if score['loss'] > max_error: result.append(f'{name}: error {score["loss"]:.3f} > {max_error:.3f}')
        if score['silhouette_iou'] < MIN_SILHOUETTE_IOU: result.append(f'{name}: silhouette IoU below {MIN_SILHOUETTE_IOU:.2f}')
        if score['depth_fraction'] > MAX_DEPTH_FRACTION: result.append(f'{name}: depth error exceeds {MAX_DEPTH_FRACTION:.0%} of source span')
    return result


def accepts(baseline, candidate, regularized=False, max_error=DEFAULT_MAX_ERROR):
    # Keep the previous call signature for callers; acceptance is now an
    # absolute comparison with the capture, not a relative baseline gain.
    return not failures(candidate, max_error)


def rank(scores, max_error=DEFAULT_MAX_ERROR):
    """Drive the worst failing view toward acceptance, not just the average."""
    failed = failures(scores,max_error)
    if len(scores) != len(VIEW_NAMES) or any(s['pixels'] < 64 or s['photo'] is None or
            not all(np.isfinite(s[k]) for k in ('loss','silhouette_iou','depth_fraction')) for s in scores):
        return True, float('inf'), float('inf')
    violation = max(max(s['loss']/max_error,
                        (1-s['silhouette_iou'])/(1-MIN_SILHOUETTE_IOU),
                        s['depth_fraction']/MAX_DEPTH_FRACTION) for s in scores)
    return bool(failed), violation, aggregate(scores,ALL_VIEWS)


def fit(source, oid, texture_base, textures, finish, baseline, iterations=3,
        report_dir=None, progress=lambda *args:None, initial_parameters=None, max_error=DEFAULT_MAX_ERROR,
        details=lambda **fields:None):
    if not np.isfinite(max_error) or not 0 < max_error <= 1:
        raise ValueError('Maximum per-view error must be in (0, 1].')
    import facade
    details(phase='Rendering 9 reference views',iterations=iterations,threshold=max_error,views=len(VIEW_NAMES))
    cameras = Cameras(source,textures)
    original_new, original_tex, original_stats = baseline
    details(phase='Comparing initial PCG model')
    original_views = cameras.render(original_new,textures+original_tex)
    initial_scores = cameras.scores(original_views)
    best = baseline
    best_views, best_scores = original_views, initial_scores
    parameters = dict(initial_parameters or {})
    def telemetry(phase, iteration=0, candidate=0):
        failed = failures(best_scores,max_error)
        passed = sum(not any(f.startswith(name+':') for f in failed) for name in VIEW_NAMES)
        worst = max(best_scores,key=lambda s:s['loss'])
        details(phase=phase,iteration=iteration,candidate=candidate,budget=iterations*10+10,
                mean=aggregate(best_scores,ALL_VIEWS),worst=worst['loss'],worst_view=worst['view'],passed=passed)
    telemetry('Checking acceptance limits')
    history = [dict(iteration=0,parameters=dict(parameters),train=aggregate(initial_scores,ALL_VIEWS))]
    evaluated={json.dumps(parameters,sort_keys=True):history[0]['train']}
    # Roof colors must not bias the facade palette.
    samples = [cameras.reference[i][0][cameras.valid[i]] for i in SIDE_VIEWS if cameras.valid[i].any()]
    eligible = all(cameras.valid[i].sum()>=64 for i in ALL_VIEWS)
    if eligible and samples:
        pixels = np.concatenate(samples).astype(float)
        lum = pixels @ np.array([.2126,.7152,.0722])
        dark,light = np.percentile(lum,[30,70])
        colors = dict(wall_color=tuple(np.median(pixels[lum>=light],axis=0).round().astype(int).tolist()),
                      glass_color=tuple(np.median(pixels[lum<=dark],axis=0).round().astype(int).tolist()))
        count=0
        for iteration in range(iterations):
            if accepts(initial_scores, best_scores, max_error=max_error): break
            step = .5**iteration
            proposals = []
            if iteration==0:
                proposals=[dict(align_grid=True),colors,dict(face_colors=cameras.face_colors()),dict(capture_layout=True)]
                # Staggered rows are competing architectural hypotheses.
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
                telemetry('Generating and comparing candidate',iteration+1,count)
                progress(count,iterations*10+10,f"PCG 9-view fit {iteration+1}/{iterations}, candidate {count}, worst error {max(s['loss'] for s in best_scores):.1%}")
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
                    score=aggregate(scores,ALL_VIEWS)
                    evaluated[key]=score
                    history.append(dict(iteration=iteration+1,parameters=candidate,train=score,views=scores))
                    if rank(scores,max_error) < rank(best_scores,max_error):
                        best,best_views,best_scores,parameters=result,views,scores,candidate
                    telemetry('Candidate compared',iteration+1,count)
                except (ValueError,facade.shapely.errors.GEOSException) as error:
                    history.append(dict(iteration=iteration+1,parameters=candidate,error=str(error)))
            for proposal in proposals:
                evaluate({**parameters,**proposal})
                if accepts(initial_scores,best_scores,max_error=max_error): break
            dimensions.append(('bay_scale',1.,.20*step,.7,1.6))
            for name,default,delta,low,high in dimensions:
                if accepts(initial_scores,best_scores,max_error=max_error): break
                start=dict(parameters)
                for sign in (-1,1):
                    candidate={**start,name:float(np.clip(start.get(name,default)+sign*delta,low,high))}
                    evaluate(candidate)
                    if accepts(initial_scores,best_scores,max_error=max_error): break
            print(f"  nine-view fit iteration {iteration+1}/{iterations}: mean error {aggregate(best_scores,ALL_VIEWS):.2%}, worst {max(s['loss'] for s in best_scores):.2%}",flush=True)
    failed = failures(best_scores,max_error)
    accepted=eligible and not failed
    telemetry('All 9 views passed' if accepted else 'Limits not reached; keeping existing model',
              max((h['iteration'] for h in history),default=0),len(history)-1)
    report=dict(camera_count=len(VIEW_NAMES),resolution=cameras.resolution,center=cameras.center.tolist(),span=cameras.span,
                views=[dict(name=name,basis=basis.tolist()) for name,basis in zip(VIEW_NAMES,cameras.bases)],
                comparison_views=list(ALL_VIEWS),max_view_error=max_error,
                min_silhouette_iou=MIN_SILHOUETTE_IOU,max_depth_fraction=MAX_DEPTH_FRACTION,
                iterations=iterations,accepted=accepted,eligible=eligible,
                failed_checks=failed,
                reason=('all nine views meet the acceptance limits' if accepted else
                        'insufficient textured evidence in one or more required views' if not eligible else
                        'error limits not reached within the iteration budget; existing building retained'),
                metric='.40*unlit multiscale RGB error + .20*local structural error + .25*(1-silhouette IoU) + .15*depth MAE / source span',
                regularization=best[2].get('regularization'),
                baseline=initial_scores,candidate=best_scores,parameters=parameters,history=history)
    if report_dir:
        details(phase='Saving comparison images and report')
        destination=Path(report_dir);destination.mkdir(parents=True,exist_ok=True)
        (destination/'fit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        rows=[]
        for i,(ref,before,after) in enumerate(zip(cameras.reference,original_views,best_views)):
            images=[]
            for label,view in [('Capture',ref),('Baseline PCG',before),('Fitted candidate',after)]:
                image=cv2.cvtColor(view[0],cv2.COLOR_RGB2BGR)
                image=cv2.copyMakeBorder(image,24,0,0,0,cv2.BORDER_CONSTANT,value=(250,250,250))
                cv2.putText(image,f'{label} | {VIEW_NAMES[i]}',(6,17),cv2.FONT_HERSHEY_SIMPLEX,.42,(25,25,25),1,cv2.LINE_AA)
                cv2.imwrite(str(destination/f'{VIEW_NAMES[i]}_{label.lower().replace(" ","_")}.png'),image)
                images.append(image)
            rows.append(np.concatenate(images,axis=1))
        cv2.imwrite(str(destination/'cameras.png'),np.concatenate(rows,axis=0))
    print(f"  nine-view fit: {'accepted' if accepted else 'existing building retained'}; "
          f"mean error {aggregate(initial_scores,ALL_VIEWS):.2%} -> {aggregate(best_scores,ALL_VIEWS):.2%}; "
          f"worst view {max(s['loss'] for s in best_scores):.2%}, limit {max_error:.2%}",flush=True)
    chosen=best if accepted else baseline
    chosen[2]['multiview']=report
    return chosen
