"""Capture-supported ground-floor display bays, independent of upper-floor grids."""
import cv2
import numpy as np
from scipy import ndimage
from rules_config import RULES


def detect(image,pitch,ground_offset=0.):
    cfg=RULES['storefront'];h,w=image.shape[:2]
    available=cfg['search_height_m']-ground_offset
    if available<cfg['min_height_m']:return []
    start=max(0,h-int(available/pitch));rgb=image[start:].astype(float)
    gray=rgb.mean(2)
    threshold=min(cfg['max_gray'],float(np.percentile(gray,80))-cfg['wall_contrast'])
    dark=(gray<threshold)&(rgb[:,:,2]>=rgb[:,:,0]*cfg['min_blue_red_ratio'])
    kernel=max(1,round(cfg['bridge_frame_m']/pitch))
    mask=cv2.morphologyEx(dark.astype(np.uint8),cv2.MORPH_CLOSE,np.ones((kernel,kernel),np.uint8))
    mask=ndimage.binary_fill_holes(mask).astype(np.uint8)
    count,labels,stats,_=cv2.connectedComponentsWithStats(mask,8);result=[]
    for i in range(1,count):
        x,y,ww,hh,area=map(int,stats[i]);W,H=ww*pitch,hh*pitch;sill=ground_offset+(len(gray)-y-hh)*pitch
        if not(cfg['min_width_m']<=W<=cfg['max_width_m'] and cfg['min_height_m']<=H<=cfg['max_height_m'] and 0<=sill<=cfg['max_sill_m']):continue
        if x<2 or x+ww>w-2 or y==0 or area/(ww*hh)<cfg['min_fill']:continue
        # Require bright jambs on both sides. Shadow/vegetation swaths without
        # an architectural border cannot create an opening.
        pad=max(2,round(.18/pitch));left=gray[y:y+hh,max(0,x-pad):x];right=gray[y:y+hh,x+ww:min(w,x+ww+pad)]
        middle=gray[y:y+hh,x:x+ww]
        if not left.size or not right.size:continue
        contrast=min(float(np.median(left)),float(np.median(right)))-float(np.median(middle))
        if contrast<cfg['wall_contrast']:continue
        if float(middle.std())<cfg['min_texture_std']:continue
        a,b,c,d=x,y+start,x+ww,y+start+hh
        members=measure_members(image[b:d,a:c],W,H)
        # This automatic pass targets gridded display windows. A dark rectangle
        # alone could be a garage door, vegetation, or a shadow.
        if {m['axis'] for m in members}!={'x','y'}:continue
        result.append(dict(bounds=[a*pitch,(h-d)*pitch,c*pitch,(h-b)*pitch],pixel_rect=[a,b,c,d],
            kind='window',shape='rectangular',confidence=.85,contrast=contrast,neighbors=0,recovery='captured_storefront'))
    return result


def measure_members(crop,width,height):
    # Storefronts often have dark steel bars against brighter display glass;
    # upper-floor measurement only detects light bars. Keep either polarity.
    from window_detail import measure
    members=measure(crop,width,height)
    for m in measure(255-crop,width,height):
        if not any(a['axis']==m['axis'] and abs(a['position']-m['position'])* (width if m['axis']=='x' else height)<.12 for a in members):members.append(m)
    return members
