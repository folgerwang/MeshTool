"""Explain facade repetition and separate finishes from observed wall details.

All coordinates are metric, X right and Y up. No captured pixels are used as
render textures. Material and decal semantics are hypotheses, recorded as such.
"""
import cv2
import numpy as np


def lattice(values, tolerance=.18, minimum=3):
    """Robust 1-D lattice fit allowing missing bays and rejecting outliers."""
    values=np.sort(np.asarray(values, float))
    if len(values)<minimum:return None
    gaps=np.diff(values);candidates=[]
    for gap in gaps[gaps>.6]:
        for divisor in (1,2,3):
            step=gap/divisor
            if step<.8:continue
            for anchor in values:
                residual=np.abs((values-anchor)/step-np.round((values-anchor)/step))*step
                inliers=residual<tolerance
                if inliers.sum()<minimum:continue
                indices=np.round((values[inliers]-anchor)/step).astype(int)
                # Prefer an observed fundamental, not an arbitrary subdivision.
                adjacency=int(np.sum(np.diff(indices)==1))
                score=(int(inliers.sum()),adjacency,-float(np.median(residual[inliers])),step)
                candidates.append((score,step,anchor,inliers))
    if not candidates:return None
    _,step,anchor,inliers=max(candidates,key=lambda x:x[0])
    ids=np.round((values[inliers]-anchor)/step)
    if len(np.unique(ids))<minimum:return None
    fit=np.polyfit(ids,values[inliers],1);step=float(fit[0]);phase=float(fit[1]%step)
    error=float(np.sqrt(np.mean((values[inliers]-np.polyval(fit,ids))**2)))
    return dict(spacing_m=step,phase_m=phase,rms_m=error,support=int(inliers.sum()),
                confidence=float(inliers.mean()*max(0.,1-error/tolerance)))


def pattern(openings):
    """Describe independently fitted rows; preserve stagger and mixed motifs."""
    rows=[]
    for index,item in sorted(enumerate(openings),key=lambda x:sum(x[1]['bounds'][1::2])):
        if item['kind']!='window':continue
        x,y,X,Y=item['bounds'];cy=(y+Y)/2
        row=next((r for r in rows if abs(np.median(r['ys'])-cy)<.35),None)
        if row is None:
            row=dict(ys=[],xs=[],members=[]);rows.append(row)
        row['ys'].append(cy);row['xs'].append((x+X)/2);row['members'].append(index)
    result=[]
    for row in rows:
        result.append(dict(center_y_m=float(np.median(row['ys'])),members=row['members'],
                           bay=lattice(row['xs'])))
    floor=lattice([r['center_y_m'] for r in result],tolerance=.25)
    supported=[r for r in result if r['bay']]
    cycle=None
    if floor and len(supported)>=6:
        step=float(np.median([r['bay']['spacing_m'] for r in supported]))
        indexed={};consistent=True
        for r in supported:
            if abs(r['bay']['spacing_m']-step)>.15:consistent=False;break
            i=round((r['center_y_m']-floor['phase_m'])/floor['spacing_m'])
            indexed[i]=np.exp(2j*np.pi*r['bay']['phase_m']/step)
        if consistent:
            aligned=abs(np.mean(list(indexed.values())))
            for n in (2,3,4):
                groups=[[v for i,v in indexed.items() if i%n==k] for k in range(n)]
                if any(len(g)<3 for g in groups):continue
                confidence=float(np.mean([abs(np.mean(g)) for g in groups]))
                if confidence>.9 and confidence>aligned+.18:
                    phase=np.angle([np.mean(g) for g in groups])/(2*np.pi)
                    cycle=dict(rows=n,offsets_bays=((phase-phase[0])%1).round(3).tolist(),confidence=confidence)
                    break
    return dict(rows=result,floor=floor,row_cycle=cycle,
                policy='Fit observed rows independently; no extrapolation into blank wall')


def wall_mask(image, known, openings, pitch):
    mask=np.asarray(known,bool).copy()
    for item in openings:
        x,y,X,Y=item['pixel_rect'];pad=max(2,round(.12/pitch))
        mask[max(0,y-pad):Y+pad,max(0,x-pad):X+pad]=False
    return mask


def details(image, mask, pitch):
    """Conservative colored wall-mark candidates, separate from dark openings.

    Semantic identity (sign, mural, vent, etc.) remains unresolved. Represent
    each supported mark by a flat color instance rather than copying imagery.
    """
    if mask.sum()<100:return []
    pixels=image[mask].astype(float);base=np.median(pixels,axis=0)
    color=image.astype(float);delta=color-base
    chroma=delta-delta.mean(2,keepdims=True)
    salient=(np.linalg.norm(chroma,axis=2)>42)&mask
    salient=cv2.morphologyEx(salient.astype(np.uint8),cv2.MORPH_OPEN,np.ones((3,3),np.uint8))
    count,labels,stats,_=cv2.connectedComponentsWithStats(salient,8);result=[]
    for i in range(1,count):
        x,y,w,h,area=stats[i]
        if not(.12<=w*pitch<=5 and .12<=h*pitch<=3 and area*pitch*pitch>=.06):continue
        if area/(w*h)<.65 or not mask[y:y+h,x:x+w].all():continue
        if x<2 or y<2 or x+w>=image.shape[1]-2 or y+h>=image.shape[0]-2:continue
        rgb=np.median(image[labels==i],axis=0).round().astype(int).tolist()
        result.append(dict(kind='wall_detail',bounds=[x*pitch,(len(image)-y-h)*pitch,(x+w)*pitch,(len(image)-y)*pitch],
                           rgb=rgb,confidence=.65,semantic_status='unclassified_colored_wall_mark'))
    return result


def finish(image, mask, pitch):
    """Estimate wall palette and require intersecting repeated joints for tile."""
    if mask.sum()<64:return dict(kind='unknown',rgb=[164,160,151],confidence=0.)
    pixels=image[mask].astype(float)
    rgb=np.median(pixels,axis=0).round().astype(int).tolist()
    gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY).astype(float)
    from facade_pattern import period
    joints={}
    for axis,name in ((0,'vertical'),(1,'horizontal')):
        # Persistent dark lines across the remaining wall, not window rhythms.
        sums=mask.sum(axis=axis);profile=(gray*mask).sum(axis=axis)/np.maximum(sums,1)
        fit=period(-profile,sums>max(8,mask.shape[axis]*.65),pitch,(.15,1.2))
        if fit and fit['confidence']>.7:joints[name]=fit
    tiled=len(joints)==2
    return dict(kind='tile_candidate' if tiled else 'paint_or_render',rgb=rgb,
                confidence=.7 if tiled else .45,joints=joints,
                inference='Joint repetition is a material hypothesis; color is sampled without capture imagery')


def procedural_tiles(description, shape, pitch):
    """Synthesize flat cladding and mortar using only the fitted metric joints."""
    h,w=shape[:2];h=(h+3)//4*4;w=(w+3)//4*4
    result=np.full((h,w,3),description['rgb'],np.uint8)
    grout=np.clip(np.array(description['rgb'])*.8,0,255).astype(np.uint8)
    for axis,name in ((1,'vertical'),(0,'horizontal')):
        joint=description['joints'][name];length=result.shape[axis]
        positions=(np.arange(length)+.5)*pitch
        distance=np.abs((positions-joint['phase']+joint['spacing']/2)%joint['spacing']-joint['spacing']/2)
        hit=distance<max(.012,pitch*.55)
        if axis==1:result[:,hit]=grout
        else:result[hit]=grout
    return result


def main():
    """Repeatable single-image analysis for capture/algorithm review iterations."""
    import argparse
    import json
    from pathlib import Path
    from capture_tiles import detect_openings,recover_patterned_windows
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image');parser.add_argument('output');parser.add_argument('--pitch',type=float,required=True)
    parser.add_argument('--ground-offset',type=float,default=0.)
    args=parser.parse_args()
    if args.pitch<=0:parser.error('--pitch must be positive')
    image=cv2.imread(args.image)
    if image is None:parser.error('Cannot read capture image')
    image=cv2.cvtColor(image,cv2.COLOR_BGR2RGB)
    out=Path(args.output);out.mkdir(parents=True,exist_ok=False)
    detected=detect_openings(image,args.pitch,args.ground_offset)
    openings=recover_patterned_windows(image,args.pitch,detected)
    mask=wall_mask(image,np.ones(image.shape[:2],bool),openings,args.pitch)
    marks=details(image,mask,args.pitch)
    for mark in marks:
        x,y,X,Y=mark['bounds'];mask[round(len(image)-Y/args.pitch):round(len(image)-y/args.pitch),round(x/args.pitch):round(X/args.pitch)]=False
    appearance=finish(image,mask,args.pitch)
    preview=procedural_tiles(appearance,image.shape,args.pitch)[:image.shape[0],:image.shape[1]] if appearance['kind']=='tile_candidate' else np.full_like(image,appearance['rgb'])
    for o in openings:
        x,y,X,Y=o['pixel_rect'];color=(56,49,42) if o['kind']=='door_candidate' else (45,61,72)
        cv2.rectangle(preview,(x,y),(X-1,Y-1),tuple(int(c) for c in color),-1)
        cv2.rectangle(preview,(x,y),(X-1,Y-1),(130,128,120),max(1,round(.07/args.pitch)))
    for mark in marks:
        x,y,X,Y=mark['bounds'];a,b,c,d=round(x/args.pitch),round(len(image)-Y/args.pitch),round(X/args.pitch),round(len(image)-y/args.pitch)
        preview[b:d,a:c]=mark['rgb']
    report=dict(pattern=pattern(openings),wall_finish=appearance,openings=openings,wall_details=marks,
                metrics=dict(detected=len(detected),recovered=len(openings)-len(detected),window_count=sum(o['kind']=='window' for o in openings)))
    (out/'analysis.json').write_text(json.dumps(report,indent=2))
    cv2.imwrite(str(out/'procedural_preview.png'),cv2.cvtColor(preview,cv2.COLOR_RGB2BGR))


if __name__=='__main__':main()
