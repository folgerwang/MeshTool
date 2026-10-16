"""Masked facade repair: observed horizontal repeats first, LaMa for residual gaps."""
import numpy as np
from scipy import ndimage
import facade_pattern
import refine


def repair(image,covered,pitch,model):
    covered=np.asarray(covered,bool)
    base=refine.fill_holes(image.copy(),covered)
    inferred=np.zeros_like(covered)
    report=dict(observed_fraction=float(covered.mean()),repeat_pixels=0,ml_pixels=0)
    if covered.mean()<.25 or min(image.shape[:2])<16 or covered.all():return base,inferred,report
    # Limit extrapolation to six metres of genuine capture support.
    missing=(~covered)&(ndimage.distance_transform_edt(~covered)*pitch<=6.)
    if missing.sum()<16:return base,inferred,report
    pattern=facade_pattern.image_pattern(image,covered,pitch)
    bay=pattern.get('bay',{})
    copied=np.zeros_like(covered)
    if bay.get('confidence',0)>=.65 and bay.get('cycles',0)>=4:
        step=round(bay['spacing']/pitch)
        for multiple in (1,-1,2,-2,3,-3):
            shift=multiple*step
            donor=np.roll(covered,shift,axis=1)
            if shift>0:donor[:,:shift]=False
            else:donor[:,shift:]=False
            selected=missing&~copied&donor
            base[selected]=np.roll(image,shift,axis=1)[selected];copied|=selected
    # Keep repeated-row phase: only horizontal donors from the same row are used.
    # LaMa sees those donors as context, and blends inferred seam pixels only.
    seam=copied&~ndimage.binary_erosion(copied,iterations=2)
    ml=(missing&~copied)|seam
    if ml.any():
        result=model.fill(base,ml,log=lambda message:None)
        base[ml]=result[ml]
    base[covered]=image[covered]
    inferred=missing
    report.update(repeat_pixels=int(copied.sum()),ml_pixels=int(ml.sum()),inferred_fraction=float(inferred.mean()),pattern=pattern)
    return base,inferred,report
