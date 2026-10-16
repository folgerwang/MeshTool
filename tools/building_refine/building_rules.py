"""Shared building construction rules for city and individual-building generation.

Per-building evidence supplies dimensions/layout/materials; construction is shared.
"""
import numpy as np
import cv2
from shapely.geometry import Polygon, box
import building
import mtscene

from rules_config import RULES
RULESET_VERSION = RULES['ruleset']


def material_for_part(role):
    """Assign renderer materials by semantic part, never by sampled color."""
    return mtscene.MAT_GLASS if role in RULES['materials']['glass_parts'] else mtscene.MAT_CAPTURED


def roof_frame(footprint, deck_z, width=None, rise=None):
    """Raised, stepped roof coping; the deck remains below the frame.

    Width and rise come from each capture. Never flatten an observed coping
    onto the roof surface or represent it as a line drawn on that surface.
    """
    width=RULES['roofs']['frame_width_m'] if width is None else width
    rise=RULES['roofs']['frame_rise_m'] if rise is None else rise
    if width<=0 or rise<=0:
        raise ValueError('Roof frame width and rise must be positive')
    levels=[(0.,width,deck_z+.015,deck_z+rise*.45),
            (.055,width-.035,deck_z+rise*.45+.002,deck_z+rise)]
    result=[]
    for outer,inset,low,high in levels:
        ring=footprint.buffer(outer,join_style=2).difference(footprint.buffer(-inset,join_style=2))
        for poly in getattr(ring,'geoms',[ring]):
            if not poly.is_empty:result.append(extrusion(poly,low,high))
    return result

def planar(poly,z):
    if poly.is_empty:return np.empty((0,3))
    triangles=building._triangulate(poly)
    pieces=[]
    for tri in triangles:
        xy=np.asarray(tri.exterior.coords)[:3]
        a,b=xy[1]-xy[0],xy[2]-xy[0]
        if a[0]*b[1]-a[1]*b[0]<0:xy=xy[::-1]
        pieces.append(np.c_[xy,np.full(3,z)])
    return np.concatenate(pieces) if pieces else np.empty((0,3))


def outline(width,height,shape='rectangular'):
    if shape!='arched':return box(0,0,width,height)
    theta=np.linspace(0,np.pi,RULES['windows']['arch_segments'])
    crown=np.c_[width*(.5+.5*np.cos(theta)),height*(1-RULES['windows']['arch_crown_fraction']+RULES['windows']['arch_crown_fraction']*np.sin(theta))]
    return Polygon(np.vstack([[0,0],[width,0],crown]))


def opening_template(width,height,kind,shape='rectangular',frame_width=None):
    """Unit XY tile with physical depth; frame, reveal and glazing/door leaf."""
    border=min(RULES['windows']['frame_width_m'] if frame_width is None else frame_width,width*RULES['windows']['frame_max_fraction'],height*RULES['windows']['frame_max_fraction'])
    from shapely.affinity import scale
    physical=outline(width,height,shape)
    outer=scale(physical,xfact=1/width,yfact=1/height,origin=(0,0))
    inner=scale(physical.buffer(-border,join_style=2),xfact=1/width,yfact=1/height,origin=(0,0))
    depth=RULES['doors']['recess_m'] if kind=='door_candidate' else RULES['windows']['recess_m']
    ring=np.asarray(inner.exterior.coords)[:-1]
    a,b=ring[1]-ring[0],ring[2]-ring[0]
    if a[0]*b[1]-a[1]*b[0]<0:ring=ring[::-1]
    sides=[]
    for a,b in zip(ring,np.roll(ring,-1,axis=0)):
        p=np.array([[*a,0],[*b,0],[*b,-depth],[*a,-depth]])
        sides.append(p[[0,1,2,0,2,3]])
    return dict(frame=planar(outer.difference(inner),0),reveal=np.concatenate(sides),
                **{('door_leaf' if kind=='door_candidate' else 'glazing'):planar(inner,-depth)})


def extrusion(poly,back,front):
    """Closed local XYZ solid, including polygon holes."""
    pieces=[planar(poly,front),planar(poly,back).reshape(-1,3,3)[:,::-1].reshape(-1,3)]
    for ring in [poly.exterior,*poly.interiors]:
        xy=np.asarray(ring.coords)
        for a,b in zip(xy[:-1],xy[1:]):
            q=np.array([[*a,back],[*b,back],[*b,front],[*a,front]])
            pieces.append(q[[0,1,2,0,2,3]])
    return np.concatenate(pieces)


def beam(a,b,r=.045):
    a,b=np.array(a,float),np.array(b,float);axis=b-a;axis/=np.linalg.norm(axis)
    ref=np.array([0.,0.,1.]) if abs(axis[2])<.9 else np.array([1.,0.,0.])
    u=np.cross(axis,ref);u/=np.linalg.norm(u);v=np.cross(axis,u)
    angles=np.arange(6)*np.pi/3
    ring=r*(np.cos(angles)[:,None]*u+np.sin(angles)[:,None]*v)
    lo=a+ring;hi=b+ring;out=[]
    for i in range(6):
        j=(i+1)%6;out.extend([lo[i],lo[j],hi[j],lo[i],hi[j],hi[i]])
    for i in range(1,5):out.extend([lo[0],lo[i+1],lo[i],hi[0],hi[i],hi[i+1]])
    return np.array(out)



def captured_wall_material(image,openings,pitch,cornice_height=None):
    """Keep captured stone; remove opening imagery before mapping onto new walls.

    Inpainting only supplies the small margins around geometric openings. The
    windows themselves are holes in the wall, with separate procedural models.
    Captured lighting is retained; this is not a recovered physical albedo.
    """
    h,w=image.shape[:2];mask=np.zeros((h,w),np.uint8)
    for opening in openings:
        if opening.get('recovery') in ('captured_curtain_bay','captured_glazed_band'):
            # Narrow captured metal strips between adjacent bays are valid
            # material, not margins to erase during photographic cleanup.
            x0,y0,x1,y1=opening['pixel_rect'];mask[y0:y1,x0:x1]=255
            continue
        x,y,X,Y=opening['bounds'];pad=RULES['materials']['wall_padding_m']
        a=max(0,int((x-pad)/pitch));b=min(w,int(np.ceil((X+pad)/pitch)))
        c=max(0,int(h-(Y+pad)/pitch));d=min(h,int(np.ceil(h-(y-pad)/pitch)))
        mask[c:d,a:b]=255
    # Avoid restoring photographed roof/cornice ornaments behind the new trim.
    if cornice_height is not None:mask[:max(0,int(h-cornice_height/pitch))]=255
    mask[image.max(axis=2)<RULES['materials']['invalid_pixel_max']]=255
    if np.all(mask):
        return np.full_like(image,RULES['materials']['fallback_wall_rgb']),mask
    result=cv2.inpaint(image,mask,RULES['materials']['inpaint_radius_px'],cv2.INPAINT_TELEA)
    return result,mask
