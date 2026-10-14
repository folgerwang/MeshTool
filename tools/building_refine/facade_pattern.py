"""Estimate repeated facade spacing from covered capture texture profiles."""
import copy
from types import SimpleNamespace
import numpy as np
import cv2
import raster


def period(profile,valid,res,limits):
    """Autocorrelation with contrast, overlap, and repeated-cycle requirements."""
    profile=np.asarray(profile,dtype=float);valid=np.asarray(valid,dtype=bool)
    if valid.sum()<32:return None
    # Keep the longest covered run: never interpolate across a large capture gap.
    edges=np.diff(np.r_[False,valid,False].astype(np.int8))
    runs=list(zip(np.flatnonzero(edges==1),np.flatnonzero(edges==-1)))
    begin,end=max(runs,key=lambda q:q[1]-q[0]);p=profile[begin:end]
    low=max(3,int(np.ceil(limits[0]/res)));high=min(int(limits[1]/res),len(p)//3)
    if high<=low or np.std(p)<2.:return None
    smooth=cv2.GaussianBlur(p.reshape(-1,1),(1,0),sigmaX=0,sigmaY=max(3.,high*.8)).ravel()
    signal=p-smooth
    if np.std(signal)<1.5:return None
    scores=[]
    for lag in range(low,high+1):
        a,b=signal[:-lag],signal[lag:]
        scores.append(float(np.dot(a,b)/max(np.linalg.norm(a)*np.linalg.norm(b),1e-9)))
    peaks=[i for i in range(1,len(scores)-1) if scores[i]>=scores[i-1] and scores[i]>=scores[i+1]]
    if not peaks:return None
    best=max(scores[i] for i in peaks)
    if best<.45 or best-np.median(scores)<.08:return None
    # Prefer the fundamental rather than its 2x harmonic when equally strong.
    peak=min(i for i in peaks if scores[i]>=best-.06)
    lag=low+peak
    phase=np.array([signal[k::lag].mean() for k in range(lag)]).argmax()
    return {'spacing':lag*res,'phase':(begin+phase)*res,'confidence':scores[peak],'cycles':len(p)/lag}


def image_pattern(image,mask,res):
    gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY).astype(float)
    result={}
    for axis,name,limits in ((0,'bay',(1.0,4.0)),(1,'floor',(2.6,5.2))):
        coverage=mask.mean(axis=axis)
        values=np.sum(gray*mask,axis=axis)/np.maximum(mask.sum(axis=axis),1)
        valid=coverage>.55
        if name=='floor':values=values[::-1];valid=valid[::-1]
        value=period(values,valid,res,limits)
        if value:result[name]=value
    # Averaging staggered rows can turn a bay into its half/quarter harmonic.
    # Recover horizontal spacing from individual floor bands before phase fit.
    if 'floor' in result:
        floor=result['floor']['spacing'];phase=result['floor']['phase']
        bottom=gray[::-1];covered=mask[::-1];bays=[]
        for z in np.arange(phase,len(gray)*res-floor,floor):
            a=int((z+.25*floor)/res);b=int((z+.8*floor)/res)
            if b-a<3:continue
            m=covered[a:b];values=(bottom[a:b]*m).sum(0)/np.maximum(m.sum(0),1)
            candidate=period(values,m.mean(0)>.8,res,(1.,4.))
            if candidate:bays.append(candidate)
        if len(bays)>=3:
            best=max(bays,key=lambda q:sum(abs(v['spacing']-q['spacing'])<.15 for v in bays))
            agree=[v for v in bays if abs(v['spacing']-best['spacing'])<.15]
            if len(agree)>=max(3,len(bays)*.7):
                result['bay']={**best,'row_samples':len(agree)}
    return result


def row_pattern(image,mask,res,bay,floor,floor_phase=0.):
    """Estimate a repeated 2-4 floor sequence of horizontal window phases.

    Missing pixels never vote. Require three observed cycles and consistent
    phase within each row class before preferring staggering to aligned rows.
    """
    gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY).astype(float)[::-1]
    mask=np.asarray(mask,bool)[::-1]
    rows=[];phases=[]
    x=(np.arange(gray.shape[1])+.5)*res
    if len(x)*res<bay*4:return None
    for row in range(int(np.floor(-floor_phase/floor)),int(np.ceil((len(gray)*res-floor_phase)/floor))):
        low=int(np.ceil((floor_phase+(row+.25)*floor)/res))
        high=int(np.floor((floor_phase+(row+.8)*floor)/res))
        if low<0 or high>len(gray) or high-low<3:continue
        m=mask[low:high];valid=m.mean(0)>.8
        if valid.mean()<.65:continue
        signal=(gray[low:high]*m).sum(0)/np.maximum(m.sum(0),1)
        signal-=cv2.GaussianBlur(signal.reshape(1,-1),(0,1),sigmaX=max(2.,bay/res)).ravel()
        values=signal[valid];values=values-values.mean()
        if np.std(values)<3:continue
        coefficient=np.mean(values*np.exp(-2j*np.pi*x[valid]/bay))
        if abs(coefficient)*np.sqrt(2)/np.std(values)<.4:continue
        rows.append(row);phases.append(-np.angle(coefficient)/(2*np.pi))
    if len(rows)<6:return None
    rows=np.array(rows);phasors=np.exp(2j*np.pi*np.array(phases))
    aligned=abs(phasors.mean())
    for cycle in (2,3,4):
        classes=[phasors[rows%cycle==i] for i in range(cycle)]
        if any(len(c)<3 for c in classes):continue
        consistency=float(np.mean([abs(c.mean()) for c in classes]))
        if consistency<.9 or consistency<aligned+.18:continue
        phase=np.angle([c.mean() for c in classes])/(2*np.pi)
        offsets=(phase-phase[0])%1
        offsets=np.where(np.abs(offsets-np.round(offsets*4)/4)<.06,np.round(offsets*4)/4,offsets)%1
        return dict(offsets=np.round(offsets,3).tolist(),phase=float((phase[0]%1)*bay),
                    anchor_z=float(floor_phase),confidence=consistency,observed_rows=len(rows))
    return None


def panel_color(image,mask):
    """Robust solid-panel color estimate; avoid dark glazing and clipped highlights."""
    pixels=image[mask].astype(float)
    if len(pixels)<64:return None
    lum=pixels@np.array([.2126,.7152,.0722])
    usable=(lum>25)&(lum<235)
    pixels=pixels[usable];lum=lum[usable]
    if len(pixels)<64:return None
    lo,hi=np.percentile(lum,[65,95])
    samples=pixels[(lum>=lo)&(lum<=hi)]
    # Prefer neutral bright structural panels over saturated sky reflections.
    saturation=np.ptp(samples,axis=1)/np.maximum(samples.max(axis=1),1)
    neutral=samples[saturation<.25]
    if len(neutral)>max(32,len(samples)*.25):samples=neutral
    color=np.median(samples,axis=0)
    # Capture contains baked shading. Mild compensation prevents lighting twice
    # from turning painted panels nearly black; this is an albedo estimate.
    color=np.clip(color*1.12,35,230)
    return {'rgb':tuple(int(round(x)) for x in color),'samples':len(samples)}


def capture(source,textures):
    """Decoded capture of one building: only the textures its meshes reference."""
    used=sorted({m.tex_index for m in source if m.uvs is not None and m.tex_index<len(textures)})
    if not used:return None
    remap={old:i for i,old in enumerate(used)}
    meshes=[]
    for m in source:
        if m.tex_index not in remap or m.uvs is None:continue
        part=copy.copy(m);part.tex_index=remap[m.tex_index];meshes.append(part)
    group=SimpleNamespace(meshes=meshes,textures=[textures[i] for i in used])
    return SimpleNamespace(src=raster.collect(group),tex=raster.decode_textures(group))


def project_patch(cap,origin,u,v,n,size,res):
    """Captured colour and coverage of one wall patch in its tiling frame."""
    from refine import project_face
    face=SimpleNamespace(origin=origin,u=u,v=v,normal=n,size=size)
    image,_,mask=project_face(face,cap.src,cap.tex,res,(-.8,.8))
    return image,mask


def reference_luminance(pattern):
    """Raw captured luminance of the cladding (before albedo compensation)."""
    rgb=pattern.get('appearance',{}).get('rgb')
    if not rgb:return 120.
    return float(np.dot(rgb,[.2126,.7152,.0722])/1.12)


def column_classes(image,mask,res,bay,reference,count):
    """Classify each bay column of a patch from the capture: glazed, clad or pier.

    Column means are split in two when the patch has real contrast; otherwise
    the whole patch is one class judged against the building's cladding
    luminance (blue, dark surfaces are glazing). Uncovered columns follow their
    nearest sampled neighbour. A lone clad bay between glazing is a pier.
    """
    classes=['clad']*count
    if count<=0:return {'columns':classes,'sampled':0,'glazing':0.,'contrast':0.}
    gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY).astype(float)
    blue=image[:,:,2].astype(float)-image[:,:,0].astype(float)
    means=np.full(count,np.nan);blues=np.full(count,np.nan)
    for j in range(count):
        a=int(round(j*bay/res));b=max(a+1,int(round((j+1)*bay/res)))
        m=mask[:,a:b]
        if m.sum()<40:continue
        means[j]=gray[:,a:b][m].mean();blues[j]=np.median(blue[:,a:b][m])
    known=np.isfinite(means)
    if not known.any():return {'columns':classes,'sampled':0,'glazing':0.,'contrast':0.}
    values=means[known]
    lo,hi=np.percentile(values,[10,90]);contrast=float(hi-lo)
    glazed=np.zeros(count,bool)
    if contrast>25 and known.sum()>=4:
        c0,c1=values.min(),values.max()
        for _ in range(20):
            near=np.abs(values-c0)<=np.abs(values-c1)
            if near.all() or (~near).all():break
            c0,c1=values[near].mean(),values[~near].mean()
        dark=np.abs(means-c0)<=np.abs(means-c1)
        if c1<.62*reference:glazed=known.copy()
        elif c0<.78*reference:glazed=known&dark
    else:
        med=float(np.median(values));bl=float(np.nanmedian(blues))
        if med<.65*reference or (med<.85*reference and bl>20):glazed=known.copy()
    idx=np.flatnonzero(known)
    for j in range(count):
        if not known[j]:glazed[j]=glazed[idx[np.argmin(np.abs(idx-j))]]
    classes=['glazed' if g else 'clad' for g in glazed]
    def neighbours(j):
        return (classes[j-1] if j>0 else None,classes[j+1] if j+1<count else None)
    for j in range(count):
        left,right=neighbours(j)
        if classes[j]=='glazed' and left=='clad' and right=='clad':classes[j]='clad'
    for j in range(count):
        left,right=neighbours(j)
        if classes[j]=='clad' and left=='glazed' and right=='glazed':classes[j]='pier'
    return {'columns':classes,'sampled':int(known.sum()),
            'glazing':float(np.mean([c=='glazed' for c in classes])),'contrast':contrast}


def detect(source,textures,groups,cap=None):
    from refine import project_face
    cap=cap or capture(source,textures)
    if cap is None:return {}
    src,tex=cap.src,cap.tex
    candidates={'floor':[],'bay':[],'group':[]}
    colors=[];faces=[]
    largest=sorted(groups,key=lambda g:-sum(np.linalg.norm(np.cross(t[1]-t[0],t[2]-t[0])) for t in g['tri']))[:8]
    for g in largest:
        n=g['n'];u=np.cross([0.,0.,1.],n);u/=np.linalg.norm(u);v=np.cross(n,u)
        t=np.asarray(g['tri']);anchor=g['anchor']
        uv=np.stack([(t-anchor)@u,(t-anchor)@v],axis=-1)
        lo,hi=uv.min(axis=(0,1)),uv.max(axis=(0,1));size=hi-lo
        if size[0]<4 or size[1]<12:continue
        origin=anchor+u*lo[0]+v*lo[1]
        res=max(.15,float(max(size))/1024)
        face=SimpleNamespace(origin=origin,u=u,v=v,normal=n,size=size)
        image,_,mask=project_face(face,src,tex,res,(-.8,.8))
        color=panel_color(image,mask)
        if color:colors.append(color)
        found=image_pattern(image,mask,res)
        if found:
            # Per-face phases let the window grid follow the capture on that face.
            record=dict(normal=n.tolist(),origin=origin.tolist(),**{k:dict(v) for k,v in found.items()})
            if 'floor' in found and 'bay' in found:
                rows=row_pattern(image,mask,res,found['bay']['spacing'],found['floor']['spacing'],found['floor']['phase'])
                if rows:
                    rows['anchor_z']=float(origin[2]+rows['anchor_z']*v[2])
                    record['rows']=rows
            faces.append(record)
        # Wide bright/dark groups survive low-resolution captures even when
        # individual floors do not. Sample half-width strips to avoid averaging
        # together facades with different illumination or occlusion.
        gray=cv2.cvtColor(image,cv2.COLOR_RGB2GRAY).astype(float)
        group_values=[]
        for fraction in (0.,.25,.5):
            a=int(fraction*image.shape[1]);b=int((fraction+.5)*image.shape[1])
            m=mask[:,a:b]
            values=(gray[:,a:b]*m).sum(1)/np.maximum(m.sum(1),1)
            candidate=period(values[::-1],(m.mean(1)>.65)[::-1],res,(6.,15.))
            if candidate:group_values.append(candidate)
        if group_values:found['group']=max(group_values,key=lambda q:q['confidence'])
        for name,value in found.items():
            value['anchor_z']=float(origin[2]+value['phase']*v[2])
            value['weight']=value['confidence']*min(value['cycles'],15)*float(mask.mean())
            candidates[name].append(value)
    result={'faces':faces} if faces else {}
    if colors:
        samples=np.asarray([c['rgb'] for c in colors],float)
        light=samples@np.array([.2126,.7152,.0722])
        neutral=np.ptp(samples,axis=1)/np.maximum(samples.max(axis=1),1)<.25
        structural=neutral & (light>=np.median(light))
        # Discard glass-heavy shadow faces when other patches show the cladding.
        if structural.sum()>=2:samples=samples[structural]
        else:samples=samples[light>=np.percentile(light,60)]
        rgb=np.median(samples,axis=0)
        result['appearance']={'rgb':tuple(int(round(x)) for x in rgb),'patches':len(colors),'estimated':True}
    for name,values in candidates.items():
        if not values:continue
        # Use the spacing supported by the most agreeing facade samples.
        best=max(values,key=lambda q:sum(v['weight'] for v in values if abs(v['spacing']-q['spacing'])<.22))
        agree=[v for v in values if abs(v['spacing']-best['spacing'])<.22]
        chosen=dict(best);chosen['spacing']=float(np.average([v['spacing'] for v in agree],weights=[v['weight'] for v in agree]))
        chosen['samples']=len(agree)
        if name=='group' and len(agree)<2:continue
        result[name]=chosen
    if 'group' in result:
        group=result['group']
        reference=result.get('floor',{}).get('spacing',3.4)
        count=max(2,int(round(group['spacing']/reference)))
        spacing=group['spacing']/count
        if 2.6<=spacing<=5.2 and ('floor' not in result or abs(spacing-reference)<.25):
            result['group']['floors']=count
            if 'floor' not in result:
                result['floor']={**group,'spacing':spacing,'derived':True}
    return result
