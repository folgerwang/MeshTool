"""Reject false arches using a straight captured lintel and row peers."""
import copy
import numpy as np
from rules_config import RULES


def refine(image,openings,pitch):
    cfg=RULES['window_shape'];result=copy.deepcopy(openings);changes=[]
    gray=image.astype(float).mean(2);height,width=gray.shape
    for i,o in enumerate(openings):
        if o.get('kind')!='window' or o.get('shape')!='arched':continue
        if o.get('recovery') in ('visually_measured_capture','captured_storefront'):continue
        x,y,X,Y=o['bounds'];center=(y+Y)/2
        peers=sum(p.get('kind')=='window' and p.get('shape')=='rectangular'
                  and abs((p['bounds'][1]+p['bounds'][3])/2-center)<cfg['row_tolerance_m']
                  and abs((p['bounds'][3]-p['bounds'][1])-(Y-y))<cfg['height_tolerance_m']
                  for p in openings)
        if peers<cfg['min_rectangular_peers']:continue
        a,c=round(x/pitch),round(X/pitch);top=round(height-Y/pitch)
        trim=max(1,round((c-a)*.1));a=max(0,a+trim);c=min(width,c-trim)
        if c-a<5:continue
        radius=max(1,round(cfg['lintel_search_m']/pitch));step=max(1,round(cfg['edge_sample_m']/pitch))
        support=[]
        for row in range(max(step,top-radius),min(height-step,top+radius+1)):
            support.append(float(np.mean(gray[row-step,a:c]-gray[row+step,a:c]>cfg['contrast_min'])))
        score=max(support,default=0)
        if score<cfg['min_lintel_support']:continue
        result[i]['shape']='rectangular'
        evidence=dict(bounds=o['bounds'],before='arched',after='rectangular',lintel_support=score,rectangular_peers=peers)
        result[i]['shape_evidence']=evidence;changes.append(evidence)
    return result,changes
