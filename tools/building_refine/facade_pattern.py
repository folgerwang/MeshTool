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
    return result


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


def detect(source,textures,groups):
    from refine import project_face
    used=sorted({m.tex_index for m in source if m.uvs is not None and m.tex_index<len(textures)})
    if not used:return {}
    remap={old:i for i,old in enumerate(used)}
    meshes=[]
    for m in source:
        if m.tex_index not in remap or m.uvs is None:continue
        part=copy.copy(m);part.tex_index=remap[m.tex_index];meshes.append(part)
    group=SimpleNamespace(meshes=meshes,textures=[textures[i] for i in used])
    src=raster.collect(group);tex=raster.decode_textures(group)
    candidates={'floor':[],'bay':[],'group':[]}
    colors=[]
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
    result={}
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
