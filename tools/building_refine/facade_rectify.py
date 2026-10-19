"""Bounded repair of scan-warped window rows; never invent bays or openings."""
import copy
import cv2
import numpy as np
from scipy.spatial import Delaunay, QhullError
from rules_config import RULES


def regularize(openings):
    cfg=RULES['facade_rectification'];result=copy.deepcopy(openings)
    rows=[]
    for i,o in sorted(enumerate(openings),key=lambda p:(p[1]['bounds'][1]+p[1]['bounds'][3])/2):
        if o['kind']!='window' or o.get('recovery') in ('visually_measured_capture','captured_storefront'):continue
        bounds=np.array(o['bounds']);center=(bounds[1]+bounds[3])/2
        row=next((r for r in rows if abs(np.median(r[0])-center)<cfg['row_tolerance_m']),None)
        if row is None:rows.append(([center],[i]))
        else:row[0].append(center);row[1].append(i)
    for _,ids in rows:
        if len(ids)<cfg['min_row_members']:continue
        # Distinct tall/short motifs in a row retain their own lintel and sill.
        groups=[]
        for i in ids:
            b=np.array(openings[i]['bounds']);height=b[3]-b[1]
            group=next((g for g in groups if abs(np.median(g[0])-height)<cfg['height_tolerance_m']),None)
            if group is None:groups.append(([height],[i]))
            else:group[0].append(height);group[1].append(i)
        for _,members in groups:
            if len(members)<cfg['min_row_members']:continue
            boxes=np.array([openings[i]['bounds'] for i in members]);target=np.median(boxes[:,[1,3]],axis=0)
            for i in members:
                before=np.array(openings[i]['bounds']);after=before.copy()
                if np.max(abs(before[[1,3]]-target))>cfg['max_edge_shift_m']:continue
                after[[1,3]]=target
                # Preserve each row's horizontal phase and alternating bay widths.
                if any(j!=i and after[0]<other['bounds'][2] and after[2]>other['bounds'][0]
                       and after[1]<other['bounds'][3] and after[3]>other['bounds'][1]
                       for j,other in enumerate(result)):continue
                result[i]['bounds']=after.tolist()
    return result


def rectify(image, openings, pitch):
    """Move the captured material with the repaired frames using a bounded warp.

    Reject individual adjustments that would fold or excessively stretch the mapping. Facade borders stay fixed, so roofs,
    neighboring walls and architectural corners cannot move with a window row.
    """
    cfg=RULES['facade_rectification'];updated=regularize(openings);h,w=image.shape[:2]
    old=[];new=[]
    for a,b in zip(openings,updated):
        for target,item in ((old,a),(new,b)):
            x,y,X,Y=item['bounds'];target.extend([[x/pitch,h-Y/pitch],[X/pitch,h-Y/pitch],[X/pitch,h-y/pitch],[x/pitch,h-y/pitch]])
    changed=sum(not np.allclose(a['bounds'],b['bounds']) for a,b in zip(openings,updated))
    if not changed:return image,openings,dict(status='already_regular',changed_windows=0)
    # Boundary anchors every metre stop a long edge bending near an end window.
    border=[]
    for x in np.linspace(0,w-1,max(2,round(w*pitch)+1)):border.extend([[x,0],[x,h-1]])
    for y in np.linspace(0,h-1,max(2,round(h*pitch)+1)):border.extend([[0,y],[w-1,y]])
    old_all=np.array(old+border);new_all=np.array(new+border)
    rejected=set()
    def area(p):
        a=p[:,1]-p[:,0];b=p[:,2]-p[:,0];return a[:,0]*b[:,1]-a[:,1]*b[:,0]
    for attempt in range(len(openings)+1):
        _,unique=np.unique(np.round(new_all,5),axis=0,return_index=True)
        old=old_all[unique];new=new_all[unique]
        try:tri=Delaunay(new)
        except QhullError:return image,openings,dict(status='warp_triangulation_rejected',changed_windows=0)
        ratios=area(old[tri.simplices])/area(new[tri.simplices])
        bad=(ratios<cfg['min_warp_area_ratio'])|(ratios>cfg['max_warp_area_ratio'])
        if not bad.any():break
        owners=np.unique(unique[tri.simplices[bad]]//4)
        reject=[int(i) for i in owners if i<len(openings) and not np.allclose(openings[i]['bounds'],updated[i]['bounds'])]
        if not reject:return image,openings,dict(status='excessive_warp_rejected',changed_windows=0)
        for i in reject:
            updated[i]=copy.deepcopy(openings[i]);new_all[i*4:i*4+4]=old_all[i*4:i*4+4];rejected.add(i)
    changed=sum(not np.allclose(a['bounds'],b['bounds']) for a,b in zip(openings,updated))
    if not changed:return image,openings,dict(status='excessive_warp_rejected',changed_windows=0)
    yy,xx=np.indices((h,w));pixels=np.c_[xx.ravel(),yy.ravel()];simplex=tri.find_simplex(pixels)
    if (simplex<0).any():return image,openings,dict(status='incomplete_warp_rejected',changed_windows=0)
    weights=np.einsum('nij,nj->ni',tri.transform[simplex,:2],pixels-tri.transform[simplex,2])
    weights=np.c_[weights,1-weights.sum(1)]
    source=np.einsum('ni,nij->nj',weights,old[tri.simplices[simplex]])
    warped=cv2.remap(image,source[:,0].reshape(h,w).astype(np.float32),source[:,1].reshape(h,w).astype(np.float32),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE)
    for item in updated:
        x,y,X,Y=item['bounds'];item['pixel_rect']=[round(x/pitch),round(h-Y/pitch),round(X/pitch),round(h-y/pitch)]
    return warped,updated,dict(status='rectified',changed_windows=changed,max_displacement_m=float(np.linalg.norm(old-new,axis=1).max()*pitch),area_ratio_range=[float(ratios.min()),float(ratios.max())],rejected_window_adjustments=len(rejected))
