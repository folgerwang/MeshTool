"""Measured building_080 roof assembly in the building's local XY frame.

Roof footprints are traced from the rectified original capture. Heights come
from the captured top surface: deck 50.8, mansard plateau 55.8, dome apex 58.4.
The missing wedge of the dome is completed by its observed rotational pattern.
"""
import numpy as np
from shapely.geometry import box, Point
from building_rules import extrusion, planar

ORIGIN=np.array([-156.99999946594238,107.29999946594239,0.])
U=np.array([.9875297819484838,.15743230216439072,0.])
V=np.array([-.15743230216439072,.9875297819484838,0.])


from building_rules import beam, roof_frame


def geometry():
    parts=[]
    def add(p,role):parts.append((p,role))
    # Raised mansard: sloped sides, complete plateau and underside.
    base=np.array([[20.4,2.0],[46.8,2.0],[46.8,37.0],[20.4,37.0]])
    top=np.array([[22.7,5.1],[44.4,5.1],[44.4,33.8],[22.7,33.8]])
    for i in range(4):
        j=(i+1)%4;q=np.array([[*base[i],51.6],[*base[j],51.6],[*top[j],55.8],[*top[i],55.8]])
        add(q[[0,1,2,0,2,3]],'roof_stone')
        # Capture shows three inset panels on each long eave and two on the end.
        count={0:3,1:3,2:4,3:0}[i]
        if i==3:count=0  # this side faces the dome and service structures
        for t in np.linspace(.16,.84,count):
            length=np.linalg.norm(base[j]-base[i]);half=1.6/length
            corners=[]
            for s,h in [(t-half,.20),(t+half,.20),(t+half,.81),(t-half,.81)]:
                xy=(1-h)*(base[i]*(1-s)+base[j]*s)+h*(top[i]*(1-s)+top[j]*s)
                corners.append([*xy,51.6+h*4.2+.025])
            q=np.array(corners);add(q[[0,1,2,0,2,3]],'roof_glass')
            for a,b in zip(q,np.roll(q,-1,axis=0)):add(beam(a,b,.065),'roof_metal')
            for f in (.33,.66):add(beam(q[0]*(1-f)+q[1]*f,q[3]*(1-f)+q[2]*f,.04),'roof_metal')
        # Fine standing seams on the slopes, separate from the skylight panels.
        for t in np.arange(.4,length if count else np.linalg.norm(base[j]-base[i]),.55)/np.linalg.norm(base[j]-base[i]):
            a=np.r_[base[i]*(1-t)+base[j]*t,51.63];b=np.r_[top[i]*(1-t)+top[j]*t,55.83]
            add(beam(a,b,.018),'roof_stone')
    add(planar(box(22.7,5.1,44.4,33.8),55.8),'roof_stone')
    # Observed rim peaks near 56.0 m while the adjacent deck is 55.7-55.8.
    # A stepped coping is actual geometry, not a coplanar line or flat cap.
    for frame in roof_frame(box(22.7,5.1,44.4,33.8),55.8,width=.34,rise=.23):
        add(frame,'trim')
    add(planar(box(20.4,2.0,46.8,37.0),51.6).reshape(-1,3,3)[:,::-1].reshape(-1,3),'roof_stone')
    # Roof deck seams and service strip follow the observed plateau.
    # Roof seams are shallow rectangular strips, not half-buried round tubes
    # that alias as dotted / uneven framing in the viewport.
    for x in np.arange(23.1,44.3,.8):
        add(extrusion(box(x-.018,5.5,x+.018,33.4),55.805,55.825),'roof_stone')
    add(extrusion(box(30.5,5.5,31.1,33.4),55.815,55.99),'roof_stone')
    add(extrusion(box(31.12,25.1,44.0,25.6),55.815,55.98),'roof_stone')
    # Service headhouse visible adjacent to the dome; measured height 59.7 m.
    add(extrusion(box(21.4,11.7,26.1,17.8),55.85,59.7),'roof_stone')
    add(extrusion(box(21.1,11.4,26.4,18.1),59.71,59.88),'trim')
    for x,y,w,h,z in [(33.5,28.6,2.1,1.5,56.65),(33.7,10.4,1.8,2.1,57.1),(6.0,34.3,5.2,2.3,54.0)]:
        add(extrusion(box(x,y,x+w,y+h),50.85 if y>34 else 55.85,z),'roof_stone')
    # Low perimeter pedestals visible in the original overhead capture.
    for y in (.65,37.7):
        for x in np.arange(1.2,48,3.4):
            add(extrusion(box(x-.38,y-.33,x+.38,y+.33),52.4,53.05),'trim')
            add(extrusion(box(x-.46,y-.39,x+.46,y+.39),53.06,53.18),'trim')
    for y in np.arange(3.0,37,3.4):
        add(extrusion(box(48.4,y-.38,49.0,y+.38),52.4,53.05),'trim')
    # Service paths / ducts seen curving around the west-side headhouse.
    for a,b in [([23.,18.4,56.65],[27.,18.4,56.65]),([27.,18.4,56.65],[28.,17.4,56.65]),
                ([28.,17.4,56.65],[28.,12.,56.65]),([28.,12.,56.65],[27.,11.,56.65]),
                ([27.,11.,56.65],[23.,11.,56.65])]:
        add(beam(a,b,.45),'roof_stone')
    add(extrusion(box(3.5,29.1,17.5,30.5),50.85,51.5),'roof_stone')
    for x in np.arange(4.,17.5,1.2):add(extrusion(box(x,29.15,x+.12,30.45),51.52,51.7),'roof_metal')
    # Full 24-spoke, seven-ring glazed dome; radial height observations fit
    # a nearly conical profile, not a generic hemisphere.
    cx,cy=11.2,19.;radius=9.2;sectors=48;apex=58.4;base_z=52.35
    angles=np.arange(sectors)*2*np.pi/sectors
    ring=np.c_[cx+radius*np.cos(angles),cy+radius*np.sin(angles),np.full(sectors,base_z)]
    for a,b in zip(ring,np.roll(ring,-1,axis=0)):add(np.array([[cx,cy,apex],a,b]),'roof_glass')
    add(planar(Point(cx,cy).buffer(radius,quad_segs=12),base_z).reshape(-1,3,3)[:,::-1].reshape(-1,3),'roof_glass')
    curb=Point(cx,cy).buffer(radius+.35,quad_segs=12).difference(Point(cx,cy).buffer(radius+.02,quad_segs=12))
    add(extrusion(curb,50.85,52.4),'trim')
    for angle in np.arange(24)*2*np.pi/24:
        add(beam([cx,cy,apex+.04],[cx+radius*np.cos(angle),cy+radius*np.sin(angle),base_z+.04],.055),'roof_metal')
    for r in np.linspace(radius/7,radius,7):
        z=apex-(apex-base_z)*r/radius+.04
        points=np.c_[cx+r*np.cos(angles),cy+r*np.sin(angles),np.full(sectors,z)]
        for a,b in zip(points,np.roll(points,-1,axis=0)):add(beam(a,b,.045),'roof_metal')
    # Convert roof-local points to the same world coordinates as the facade.
    return [(ORIGIN+p[:,0,None]*U+p[:,1,None]*V+p[:,2,None]*np.array([0.,0.,1.]),role) for p,role in parts]
