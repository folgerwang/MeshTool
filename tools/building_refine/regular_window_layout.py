"""Fit reviewed facade motifs and shared floor levels to capture observations."""
import numpy as np
from shapely.geometry import box


def fit_floors(catalog,faces,hint,count=4,tolerance=.7):
    """One robust vote per face/row prevents a densely sampled face dominating."""
    values=[];indices=[]
    for fid in faces:
        rows={}
        for item in catalog['instances']:
            if item.get('kind')!='window' or item['face_id']!=fid:continue
            _,y,_,Y=item['bounds'];center=(y+Y)/2
            index=round((center-hint[0])/hint[1])
            if 0<=index<count and abs(center-hint[0]-index*hint[1])<tolerance:
                rows.setdefault(index,[]).append(center)
        for index,row in rows.items():indices.append(index);values.append(float(np.median(row)))
    if len(set(indices))<3:raise ValueError('Insufficient observed floor levels')
    spacing,phase=np.polyfit(indices,values,1)
    if abs(spacing-hint[1])>.3:raise ValueError('Floor spacing conflicts with reviewed capture')
    return dict(first_center_m=float(phase),spacing_m=float(spacing),count=count,
                observation_rms_m=float(np.sqrt(np.mean((np.asarray(values)-phase-spacing*np.asarray(indices))**2))),
                face_row_observations=len(values))


def fit_columns(observations,bay_count):
    """Fit x = origin + bay * spacing + member * paired-window separation.

    Observations are reviewed (bay, member, center_m) triples. A single column
    uses one observation; simple equally spaced rows use member=0 throughout.
    """
    values=np.array(observations,float)
    if values.ndim!=2 or values.shape[1]!=3 or not len(values):raise ValueError('Invalid motif observations')
    paired=bool(values[:,1].max()>0)
    design=np.c_[np.ones(len(values)),values[:,:2]]
    active=[0]+([1] if len(set(values[:,0]))>1 else [])+([2] if paired else [])
    if np.linalg.matrix_rank(design[:,active])<len(active):raise ValueError('Ambiguous motif observations')
    coefficients=np.zeros(3);coefficients[active]=np.linalg.lstsq(design[:,active],values[:,2],rcond=None)[0]
    if bay_count>1 and coefficients[1]<=0:raise ValueError('Repeated bays need observed positive spacing')
    if paired and coefficients[2]<=0:raise ValueError('Paired windows need positive separation')
    centers=[float(coefficients@[1,bay,member]) for bay in range(bay_count) for member in range(2 if paired else 1)]
    return dict(first_center_m=float(coefficients[0]),bay_spacing_m=float(coefficients[1]),
                pair_spacing_m=float(coefficients[2]) if paired else None,centers_m=centers,
                observation_rms_m=float(np.sqrt(np.mean((design@coefficients-values[:,2])**2))))


def openings(floors,columns,width,height,face_size,image_shape,pitch):
    """Create exact metric slots; never shrink a corner-crossing window to fit."""
    if width<=0 or height<=0:raise ValueError('Window sizes must be positive')
    boundary=box(.12,.12,face_size[0]-.12,face_size[1]-.12)
    result=[];skipped=[]
    for row in range(floors['count']):
        y=floors['first_center_m']+row*floors['spacing_m']
        for column,x in enumerate(columns['centers_m']):
            bounds=[x-width/2,y-height/2,x+width/2,y+height/2]
            if not boundary.covers(box(*bounds)):
                skipped.append(dict(row=row,column=column,reason='crosses_facade_edge'));continue
            if any(box(*bounds).intersects(box(*o['bounds'])) for o in result):raise ValueError('Pattern windows overlap')
            a,b,c,d=bounds;h,w=image_shape[:2]
            rect=[round(a/pitch),round(h-d/pitch),round(c/pitch),round(h-b/pitch)]
            if not(0<=rect[0]<rect[2]<=w and 0<=rect[1]<rect[3]<=h):raise ValueError('Window sample leaves capture image')
            result.append(dict(kind='window',shape='rectangular',bounds=bounds,pixel_rect=rect,
                               confidence=.8,recovery='fitted_repeating_motif',layout_row=row,layout_column=column))
    return result,skipped


def profile_openings(floors,config,default_height,face_size,image_shape,pitch):
    """Fit independent reviewed window groups on the same facade/floor grid.

    Wide glazing and narrow stair/service windows need separate widths and
    vertical offsets; neither should force an artificial repeating bay spacing.
    Profiles without column_groups retain the original single-motif behavior.
    """
    groups=config.get('column_groups',[config])
    if not groups:raise ValueError('A reviewed facade needs window column groups')
    result=[];reports=[]
    for group in groups:
        columns=fit_columns(group['column_observations'],group['bay_count'])
        height=group.get('window_height_m',default_height)
        local=dict(floors,first_center_m=floors['first_center_m']+group.get('floor_offset_m',0.))
        items,skipped=openings(local,columns,group['window_width_m'],height,face_size,image_shape,pitch)
        for item in items:
            if any(box(*item['bounds']).intersects(box(*other['bounds'])) for other in result):
                raise ValueError('Reviewed column groups overlap')
        result.extend(items)
        reports.append(dict(columns=columns,width_m=group['window_width_m'],height_m=height,
            floor_offset_m=group.get('floor_offset_m',0.),slots=len(items),skipped=skipped))
    # Stable order keeps prototype generation and exact-slot verification reproducible.
    result.sort(key=lambda o:(o['layout_row'],o['bounds'][0]))
    return result,dict(groups=reports,slots=len(result)) if 'column_groups' in config else reports[0]
