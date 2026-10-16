"""Recover continuous glazed bays only where repeated captured floor bands exist."""
import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy.signal import find_peaks
from rules_config import RULES


def detect(image,pitch):
    cfg=RULES['curtain_wall'];h,w=image.shape[:2]
    if w*pitch<cfg['min_facade_width_m'] or h*pitch<cfg['min_facade_height_m']:return []
    gray=image.astype(float).mean(2)
    trim=max(1,int(w*.1))
    signal=np.median(gray[:,trim:-trim],axis=1)
    detail=gaussian_filter1d(signal,.7)-gaussian_filter1d(signal,max(2,1/pitch))
    peaks,_=find_peaks(detail,prominence=cfg['band_prominence'],distance=cfg['min_floor_m']/pitch)
    pairs=[(int(a),int(b)) for a,b in zip(peaks[:-1],peaks[1:]) if cfg['min_floor_m']<=(b-a)*pitch<=cfg['max_floor_m']]
    if len(pairs)<cfg['min_supported_rows']:return []
    # Independent vertical evidence: no arbitrary bay width is imposed.
    xsignal=np.mean(abs(np.diff(gray,axis=1)),axis=0)
    xdetail=gaussian_filter1d(xsignal,.6)-gaussian_filter1d(xsignal,max(2,.5/pitch))
    columns,_=find_peaks(xdetail,prominence=cfg['mullion_prominence'],distance=cfg['min_bay_m']/pitch)
    gaps=np.diff(columns)*pitch
    usable=gaps[(gaps>=cfg['min_bay_m'])&(gaps<=cfg['max_bay_m'])]
    reliable=len(columns)>=cfg['min_supported_bays'] and len(usable)>=cfg['min_supported_bays']-1
    if reliable:
        bay=np.median(usable)
        reliable=np.mean(abs(usable/bay-np.round(usable/bay))<.25)>=.65
    # A continuous strip is supported by observed floor bands even when narrow
    # mullions cannot be resolved. Do not invent a universal bay grid.
    if not reliable:columns=np.array([1,w-2])
    margin=max(1,round(cfg['frame_margin_m']/pitch));result=[]
    for top,bottom in pairs:
        interior=image[top+margin:bottom-margin,trim:-trim].astype(float)
        if not interior.size:continue
        dark=interior.mean(2)<cfg['glass_max_intensity']
        neutral=interior[:,:,2]>=interior[:,:,0]*cfg['min_blue_red_ratio']
        if np.mean(dark&neutral)<cfg['min_glass_fraction']:continue
        for left,right in zip(columns[:-1],columns[1:]):
            if reliable and not cfg['min_bay_m']<=(right-left)*pitch<=cfg['max_bay_m']:continue
            a=int(left)+margin;c=int(right)-margin;b=top+margin;d=bottom-margin
            search=max(2,round(cfg['band_search_m']/pitch))
            local_lines=[]
            for line in (top,bottom):
                start=max(0,line-search);stop=min(h,line+search+1)
                local_lines.append(start+int(np.argmax(np.median(gray[start:stop,a:c],axis=1))))
            floor_margin=max(2,round(cfg['floor_margin_m']/pitch))
            b=local_lines[0]+floor_margin;d=local_lines[1]-floor_margin
            if c-a<3 or d-b<5:continue
            crop=image[b:d,a:c].astype(float)
            if np.mean((crop.mean(2)<cfg['glass_max_intensity'])&(crop[:,:,2]>=crop[:,:,0]*cfg['min_blue_red_ratio']))<cfg['min_glass_fraction']:continue
            middle=float(np.median(crop.mean(2)))
            supported=True
            for line in (top,bottom):
                band=gray[max(0,line-search):min(h,line+search+1),a:c]
                if np.max(np.median(band,axis=1))-middle<cfg['local_band_contrast']:supported=False
            if not supported:continue
            result.append(dict(bounds=[a*pitch,(h-d)*pitch,c*pitch,(h-b)*pitch],pixel_rect=[a,b,c,d],
                               kind='window',shape='rectangular',confidence=.8,contrast=float(detail[top]),
                               recovery='captured_curtain_bay' if reliable else 'captured_glazed_band',neighbors=len(pairs)))
    return result
