"""Replace captured wall triangles with clipped, recessed facade modules.

Window layouts follow local wall planes. Roofs and non-wall source triangles
remain intact. Generated geometry is batched per material, not per window.
"""
from collections import defaultdict
import copy
from functools import lru_cache
import numpy as np
import cv2
import shapely
from shapely.geometry import Polygon, box
from shapely.ops import unary_union
import mtscene
import raster

BAY, FLOOR = 1.6, 3.4

def _polygons(g):
    if g.is_empty: return []
    if g.geom_type == 'Polygon': return [g]
    return [p for part in getattr(g,'geoms',[]) for p in _polygons(part)]


@lru_cache(maxsize=32)
def _textures(color=(139,148,151),finish="painted-concrete"):
    # Seeded high-resolution micrograin and brushed finish, not enlarged capture.
    rng=np.random.default_rng(1741)
    n=512
    y,x=np.mgrid[:n,:n]
    grain=rng.normal(0,1.8,(n,n))
    brushed=(1.2*np.sin(y*.8)+.7*np.sin(y*2.3)) if finish=="metal" else np.zeros((n,n))
    if finish=="painted-concrete":grain+=cv2.GaussianBlur(rng.normal(0,8,(n,n)),(0,0),2.0)
    images=[]
    images.append(np.clip(np.array(color)+grain[:,:,None]+brushed[:,:,None],0,255).astype(np.uint8))
    # 8x8 pane swatches vary in tint, with subtle vertical gradients.
    panes=np.zeros((n,n,3),np.uint8)
    for row in range(8):
        for col in range(8):
            tint=np.array([52,86,108])+rng.uniform(-10,10)
            grad=np.linspace(6,-6,64)[:,None,None]
            panes[row*64:(row+1)*64,col*64:(col+1)*64]=np.clip(tint+grad,0,255)
    images.append(panes)
    # Dark backing retains a quiet suggestion of interior blinds.
    blinds=3*np.sin(y*.20)+rng.normal(0,.4,(n,n))
    images.append(np.clip(np.array([15,22,28])+blinds[:,:,None],0,255).astype(np.uint8))
    # Roof membrane: neutral grey derived from the cladding, with 1 m seams
    # (the roof is mapped at 4 m per repeat) and mottled wear.
    roof=np.array(color)*.42+28
    wear=cv2.GaussianBlur(rng.normal(0,14,(n,n)),(0,0),6.0)+rng.normal(0,2.5,(n,n))
    seams=np.where((x%128<2)|(y%128<2),-18.,0.)
    images.append(np.clip(roof+wear[:,:,None]+seams[:,:,None],0,255).astype(np.uint8))
    return [mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,
            [(n,n,raster.encode_dxt1(np.ascontiguousarray(im)))]) for im in images]

TEXTURE_COUNT=4
ROOF_REPEAT=4.0


def fit_window(tile, opening):
    """Largest supported rectangular boundary window on a bounded search grid.

    Every accepted grid cell is covered by the wall; holes and missing captured
    surfaces are never bridged. Interior bays keep their exact standard frame.
    """
    if tile.buffer(1e-6).covers(opening):return opening
    available=tile.intersection(opening)
    if available.is_empty or available.area<.40:return Polygon()
    x0,y0,x1,y1=available.bounds
    if x1-x0<.48 or y1-y0<.75:return Polygon()
    # Include component boundaries so a narrow side of an opening is not lost
    # just because it falls between fixed grid samples.
    bounds=[part.bounds for part in _polygons(available)[:16]]
    xs=np.unique(np.r_[np.linspace(x0,x1,13),[x for b in bounds for x in (b[0],b[2])]])
    ys=np.unique(np.r_[np.linspace(y0,y1,19),[y for b in bounds for y in (b[1],b[3])]])
    cells=shapely.box(xs[:-1][None,:],ys[:-1,None],xs[1:][None,:],ys[1:,None])
    supported=shapely.covers(available.buffer(1e-7),cells)
    best=None;score=0.
    for first in range(len(ys)-1):
        columns=np.ones(len(xs)-1,dtype=bool)
        for last in range(first,len(ys)-1):
            columns &= supported[last]
            height=ys[last+1]-ys[first]
            if height<.75:continue
            edges=np.diff(np.r_[False,columns,False].astype(np.int8))
            for a,b in zip(np.flatnonzero(edges==1),np.flatnonzero(edges==-1)):
                width=xs[b]-xs[a]
                if width<.48:continue
                # Prefer more glass area, with deterministic traversal for ties.
                area=(width-.13)*(height-.13)
                if area>score:
                    candidate=box(xs[a],ys[first],xs[b],ys[last+1])
                    if tile.buffer(1e-6).covers(candidate):best=candidate;score=area
    return best if best is not None else Polygon()


def stitch_internal_seams(groups):
    """Join near-parallel wall patches across shared diagonal mesh edges.

    Only actual shared edges qualify. Vertical corners, open boundaries and
    strongly folded surfaces are retained. A bounded plane-fit error prevents
    chains of triangles from flattening an entire curved building.
    """
    edges=defaultdict(set)
    for i,g in enumerate(groups):
        for tri in g['tri']:
            for a,b in ((0,1),(1,2),(2,0)):
                delta=tri[a]-tri[b]
                if abs(delta[2])<=.5 or np.linalg.norm(delta[:2])<=.5:continue
                key=tuple(sorted(tuple(np.round(p/.05).astype(np.int64)) for p in (tri[a],tri[b])))
                edges[key].add(i)
    parent=list(range(len(groups)));members={i:[i] for i in parent}
    def root(i):
        while parent[i]!=i:
            parent[i]=parent[parent[i]];i=parent[i]
        return i
    joined=0
    for ids in edges.values():
        if len(ids)!=2:continue
        a,b=map(root,sorted(ids))
        if a==b:continue
        items=members[a]+members[b]
        vertices=np.concatenate([np.asarray(groups[i]['tri']).reshape(-1,3) for i in items])
        anchor=vertices.mean(0)
        _,_,basis=np.linalg.svd(vertices-anchor,full_matrices=False);n=basis[-1]
        if any(abs(np.dot(n,groups[i]['n']))<.95 for i in items):continue
        tolerance=min(1.5,max(.10,np.linalg.norm(np.ptp(vertices,axis=0))*.015))
        if np.max(np.abs((vertices-anchor)@n))>tolerance:continue
        parent[b]=a;members[a]=items;del members[b];joined+=1
    result=[]
    for items in members.values():
        triangles=[t for i in items for t in groups[i]['tri']]
        if len(items)==1:result.append(groups[items[0]]);continue
        vertices=np.asarray(triangles).reshape(-1,3);anchor=vertices.mean(0)
        _,_,basis=np.linalg.svd(vertices-anchor,full_matrices=False);n=basis[-1]
        if np.dot(n,groups[items[0]]['n'])<0:n=-n
        result.append({'n':n,'anchor':anchor,'tri':triangles})
    return result,joined


def build(source, oid, texture_base, source_textures=None, wall_finish="painted-concrete"):
    """Return replacement meshes, textures, and metrics; no input mesh mutation."""
    empty={'windows':0,'replaced_triangles':0,'patches':0}
    if not source or not any(len(m.vertices) for m in source):
        return [],[],empty
    center=np.mean([m.translation+np.mean(m.vertices,axis=0) for m in source if len(m.vertices)],axis=0)
    wall_tri=[]; retained=[]; roof_tri=[]; parts=[]
    for m in source:
        for prim,idx in m.draw_calls:
            if prim!=4 or len(idx)%3:
                parts.append((m,prim,idx,None,None,None));continue
            triangles=idx.reshape(-1,3)
            p=m.vertices[triangles].astype(np.float64)+m.translation
            n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);length=np.linalg.norm(n,axis=1)
            finite=np.isfinite(p).all(axis=(1,2))
            wall=(length>.02)&(np.abs(n[:,2])<.20*length)&finite
            wall_tri.extend(p[wall]);parts.append((m,prim,triangles,p,wall,(length>.02)&finite))
    if not wall_tri:return [],[],empty
    wall_pts=np.asarray(wall_tri).reshape(-1,3);zmin=wall_pts[:,2].min()
    # Non-wall surfaces above the base (roof caps, setbacks, roof clutter) are
    # re-surfaced; fragments near the ground keep their captured texture.
    by_mesh=defaultdict(list)
    for m,prim,triangles,p,wall,valid in parts:
        if p is None:by_mesh[id(m)].append((prim,triangles.copy()));continue
        roof=valid&~wall&(p[:,:,2].mean(1)>=zmin+2.0)
        roof_tri.append(p[roof])
        rest=~wall&~roof
        if rest.any():by_mesh[id(m)].append((prim,triangles[rest].ravel().copy()))
    for m in source:
        if by_mesh.get(id(m)):
            part=copy.copy(m);part.draw_calls=by_mesh[id(m)];part.model_variant=2;retained.append(part)
    # Group coplanar triangles with a strict geometric tolerance; do not flatten
    # the building to an inferred rectangular footprint.
    groups=[]
    wall_tri.sort(key=lambda t:-np.linalg.norm(np.cross(t[1]-t[0],t[2]-t[0])))
    # Outward direction: the capture's own winding when it is consistent (so a
    # recessed slot faces into the slot, not into the building); otherwise
    # away from the building centre.
    walls=np.asarray(wall_tri);cross=np.cross(walls[:,1]-walls[:,0],walls[:,2]-walls[:,0])
    area=np.linalg.norm(cross,axis=1);outward=np.einsum('ij,ij->i',walls.mean(1)-center,cross)>0
    winding=area[outward].sum()>=.85*area.sum()
    for t in wall_tri:
        n=np.cross(t[1]-t[0],t[2]-t[0]);n/=np.linalg.norm(n)
        if not winding and np.dot(t.mean(0)-center,n)<0:n=-n
        match=None
        for g in groups:
            if np.dot(n,g['n'])>.98 and np.max(np.abs((t-g['anchor'])@g['n']))<.60:
                match=g;break
        if match is None:
            match={'n':n,'anchor':t[0],'tri':[]};groups.append(match)
        match['tri'].append(t)
    capture_groups=groups
    groups,stitched_seams=stitch_internal_seams(groups)
    import facade_pattern
    cap=facade_pattern.capture(source,source_textures) if source_textures else None
    pattern=facade_pattern.detect(source,source_textures,capture_groups,cap) if cap else {}
    reference=facade_pattern.reference_luminance(pattern)
    color=tuple(pattern.get('appearance',{}).get('rgb',(139,148,151)))
    panel_material=4 if wall_finish=='metal' else 3
    bay_nominal=bay=pattern.get('bay',{}).get('spacing',BAY)
    floor=pattern.get('floor',{}).get('spacing',FLOOR)
    floor_anchor=pattern.get('floor',{}).get('anchor_z',center[2])
    group_floors=pattern.get('group',{}).get('floors',0)
    batches=defaultdict(list); windows=0; tile_count=0; area_source=0.; area_new=0.
    counts=defaultdict(int)
    # Every patch frame first: the entrance goes on the widest ground-level patch.
    frames=[]
    for g in groups:
        # Fit one plane to a coherent patch, smoothing sub-metre capture noise.
        vertices=np.asarray(g['tri']).reshape(-1,3)
        anchor=vertices.mean(0)
        _,_,basis=np.linalg.svd(vertices-anchor,full_matrices=False)
        n=basis[-1]
        if np.dot(n,g['n'])<0:n=-n
        # A small or degenerate patch keeps its source normal.
        if np.dot(n,g['n'])<.99:n=g['n'];anchor=g['anchor']
        u=np.cross([0.,0.,1.],n);u/=np.linalg.norm(u);v=np.cross(n,u)
        t=np.asarray(g['tri'])
        xy=np.stack([(t-anchor)@u,(t-anchor)@v],axis=-1)
        lo=xy.min(axis=(0,1));origin=anchor+u*lo[0]+v*lo[1]
        xy-=lo
        region=shapely.make_valid(unary_union([Polygon(q) for q in xy]))
        x0,y0,x1,y1=region.bounds
        # An integer number of bays spans each patch, so no patch ends with a
        # sliver window; the bay stretches or shrinks by less than half a module.
        columns=max(1,int(round(x1/bay_nominal)))
        bay=x1/columns
        if cap is not None and x1>=bay_nominal and y1>=1.:
            res=max(.15,float(max(x1,y1))/1024)
            image,mask=facade_pattern.project_patch(cap,origin,u,v,n,(x1,y1),res)
            classes=facade_pattern.column_classes(image,mask,res,bay,reference,columns)
        else:
            classes={'columns':['clad']*columns,'sampled':0,'glazing':0.,'contrast':0.}
        # Per-column extents define base stubs, storefronts and the parapet line.
        spans=[]
        for col in range(columns):
            strip=region.intersection(box(col*bay,y0-1,(col+1)*bay,y1+1))
            spans.append(strip.bounds if not strip.is_empty else None)
        frames.append({'origin':origin,'u':u,'v':v,'n':n,'xy':xy,'t':t,'region':region,'bay':bay,
                       'classes':classes['columns'],'spans':spans,'columns':columns,'stats':classes})
    # Ground-level columns: the lowest module row on the building base that is
    # tall enough for a storefront, above a short plinth stub where one exists.
    def ground_row(f,col):
        span=f['spans'][col]
        if span is None or f['classes'][col]=='pier' or f['origin'][2]+span[1]>zmin+1.4:return None
        phase=(f['origin'][2]-floor_anchor)%floor
        cy=-phase+floor*np.floor((span[1]+phase)/floor+1e-9)
        if min(cy+floor,span[3])-span[1]<1.2:cy+=floor
        return cy if min(cy+floor,span[3])-max(cy,span[1])>=2.4 else None
    for f in frames:
        f['ground']={col:cy for col in range(f['columns']) if (cy:=ground_row(f,col)) is not None}
    entrance=None
    best=max(((len(f['ground']),i) for i,f in enumerate(frames)),default=(0,None))
    if best[0]>=3:
        cols=sorted(frames[best[1]]['ground'])
        entrance=(best[1],cols[len(cols)//2])
    def world(xy,depth):
        # Keep the original surface's position while sharing one window grid
        # across its triangulation. This avoids cracks at neighboring patches.
        xy=np.asarray(xy)
        p=xy[:,None,:]-surface_xy[None,:,0,:]
        v1=(p[:,:,0]*surface_b[None,:,1]-p[:,:,1]*surface_b[None,:,0])*surface_inv
        v2=(surface_a[None,:,0]*p[:,:,1]-surface_a[None,:,1]*p[:,:,0])*surface_inv
        inside=(v1>=-1e-5)&(v2>=-1e-5)&(v1+v2<=1.00001)&(surface_inv!=0)
        choice=inside.argmax(axis=1);rows=np.arange(len(xy))
        heights=surface_depth[choice,0]+v1[rows,choice]*(surface_depth[choice,1]-surface_depth[choice,0])+v2[rows,choice]*(surface_depth[choice,2]-surface_depth[choice,0])
        heights[~inside.any(axis=1)]=np.nan
        # Only numerical boundary points should fall outside the projected union.
        missing=np.isnan(heights)
        if missing.any():
            flat=surface_xy.reshape(-1,2);z=surface_depth.ravel()
            nearest=np.argmin(((xy[missing,None,:]-flat[None,:,:])**2).sum(2),axis=1)
            heights[missing]=z[nearest]
        return origin+xy[:,0,None]*u+xy[:,1,None]*v+(heights+depth)[:,None]*n

    def emit(poly,depth,mat,tex,uv_mode,origin,u,v,n,cx,cy,swatch):
        for region in _polygons(poly):
            if region.area<1e-7:continue
            pane_center=pane_normal=None
            if mat==mtscene.MAT_GLASS:
                # One physical pane must have one plane; warped captured quads
                # otherwise produce a diagonal reflection split between its triangles.
                corners=world(np.asarray(region.exterior.coords)[:-1],depth)
                pane_center=corners.mean(0)
                _,_,basis=np.linalg.svd(corners-pane_center,full_matrices=False)
                pane_normal=basis[-1]
            for tri in _polygons(shapely.constrained_delaunay_triangles(region)):
                xy=np.asarray(tri.exterior.coords)[:3]
                a,b=xy[1]-xy[0],xy[2]-xy[0]
                signed=a[0]*b[1]-a[1]*b[0]
                if abs(signed)<1e-9:continue
                if signed<0:xy=xy[::-1]
                pos=world(xy,depth)
                if pane_normal is not None:
                    pos-=((pos-pane_center)@pane_normal)[:,None]*pane_normal
                uv=np.clip((xy-[cx,cy])/[bay,floor],0,1)
                if uv_mode=='pane':uv=(uv*.90+.05+[swatch%8,swatch//8])/8
                batches[(mat,tex)].append((pos,uv))
    def sides(poly,front,back,mat,origin,u,v,n,tex=0xffffffff):
        for region in _polygons(poly):
            for ring in [region.exterior,*region.interiors]:
                xy=np.asarray(ring.coords)
                for a,b in zip(xy[:-1],xy[1:]):
                    face=world(np.array([a,b]),front)
                    rear=world(np.array([a,b]),back)
                    points=np.array([face[0],face[1],rear[1],rear[0]])
                    batches[(mat,tex)].append((points[[0,1,2,0,2,3]],np.zeros((6,2))))
    def slab(xa,xb,ya,yb,reach,mat,n):
        # Horizontal box from the wall surface outward: the entrance canopy.
        inner=world(np.array([[xa,ya],[xb,ya],[xa,yb],[xb,yb]]),.0)
        corners=np.concatenate([inner,inner+n*reach])
        order=[0,1,3,0,3,2, 4,6,7,4,7,5, 0,4,5,0,5,1, 2,3,7,2,7,6, 0,2,6,0,6,4, 1,5,7,1,7,3]
        batches[(mat,0xffffffff)].append((corners[order],np.zeros((36,2))))
    for fi,f in enumerate(frames):
        origin,u,v,n,xy,t,region=f['origin'],f['u'],f['v'],f['n'],f['xy'],f['t'],f['region']
        surface_xy=xy
        surface_depth=(t-origin)@n
        surface_a=xy[:,1]-xy[:,0];surface_b=xy[:,2]-xy[:,0]
        det=surface_a[:,0]*surface_b[:,1]-surface_a[:,1]*surface_b[:,0]
        surface_inv=np.divide(1.,det,out=np.zeros_like(det),where=np.abs(det)>1e-9)
        area_source+=region.area
        x0,y0,x1,y1=region.bounds
        bay=f['bay'];columns=f['columns']
        # Global floor datum aligns levels across the different facade patches.
        floor_phase=(origin[2]-floor_anchor)%floor
        for row,cy in enumerate(np.arange(-floor_phase,y1,floor)):
            for col in range(columns):
                cx=col*bay
                cell=region.intersection(box(cx,cy,cx+bay,cy+floor))
                if cell.area<1e-5:continue
                tile_count+=1
                if tile_count>18000:raise ValueError('Facade exceeds the 18,000-module limit; original walls retained')
                area_new+=cell.area
                tile=cell.intersection(box(cx+.012,cy+.012,cx+bay-.012,cy+floor-.012))
                level=int(round((origin[2]+cy-floor_anchor)/floor))
                kind=f['classes'][col] if col<len(f['classes']) else 'clad'
                span=f['spans'][col] if col<len(f['spans']) else None
                cb,ct=cell.bounds[1],cell.bounds[3]
                top=span is not None and ct>=span[3]-.02
                base=span is not None and cb<=span[1]+.02 and origin[2]+cb<=zmin+1.4
                # Stronger spandrels repeat at the observed multi-floor cadence.
                strong=group_floors and level%group_floors==0
                # Solid corner posts where a patch ends: glass never runs into a
                # corner or an open edge without a mullion-sized pier.
                left=.25 if col==0 else 0.;right=.25 if col==columns-1 else 0.
                door=None;canopy=False
                if kind=='pier':
                    opening=Polygon();counts['piers']+=1
                elif col in f['ground'] and abs(f['ground'][col]-cy)<1e-6:
                    # Ground floor: storefront glazing from a low sill to a transom.
                    opening=box(cx+.06+left,cb+.12,cx+bay-.06-right,cy+floor-.30);counts['storefront']+=1
                    if entrance==(fi,col):door=True
                elif base and ct-cb<1.2:
                    opening=Polygon();counts['plinths']+=1        # solid plinth, no sliver window
                elif top and ct-cb<.7*floor:
                    opening=Polygon();counts['parapets']+=1       # solid parapet, no sliver window
                elif kind=='glazed':
                    sill=.50 if strong else .32
                    opening=box(cx+.05+left,cy+sill,cx+bay-.05-right,cy+floor-.10);counts['glazed']+=1
                else:
                    # Cladding reads as solid: punched windows between wide piers.
                    sill=1.15 if strong else .95
                    opening=box(cx+.30+left*.6,cy+sill,cx+bay-.30-right*.6,cy+floor-.25);counts['clad']+=1
                frame=.045 if kind=='glazed' else .065
                # Boundary modules get supported smaller rectangles instead of missing panes.
                outer=fit_window(tile,opening) if not opening.is_empty else Polygon()
                inner=outer.buffer(-frame,join_style=2)
                if inner.area<.20:outer=Polygon();inner=Polygon()
                else:windows+=1
                if door is not None:
                    # Entrance doors fill the lower part of the storefront pane.
                    lo_y,hi_y=inner.bounds[1],inner.bounds[3]
                    door=box(cx+.14,lo_y,cx+bay-.14,min(lo_y+2.4,hi_y-.4)).intersection(inner) if not inner.is_empty else None
                    if door is not None and door.area>=1.:canopy=True
                    else:door=None
                swatch=((level%max(1,group_floors))*11+col*7)%64
                args=(origin,u,v,n,cx,cy,swatch)
                panel_depth=.06 if kind=='pier' else 0.
                # Continuous dark backing covers panel seams and window openings.
                emit(cell,-.55,2,texture_base+2,'tile',*args)
                emit(tile.difference(outer),panel_depth,panel_material,texture_base,'tile',*args)
                sides(tile.difference(outer),panel_depth,-.055,panel_material,origin,u,v,n,texture_base)
                if top:
                    # Parapet coping: a proud metal cap along every patch top edge.
                    cap_strip=cell.intersection(box(cx,ct-.22,cx+bay,ct))
                    emit(cap_strip,panel_depth+.06,4,0xffffffff,'tile',*args)
                    sides(cap_strip,panel_depth+.06,panel_depth,4,origin,u,v,n)
                if not inner.is_empty:
                    glazing=inner.difference(door) if door is not None else inner
                    emit(outer.difference(inner),.035,4,0xffffffff,'tile',*args)
                    sides(inner,.035,-.14,4,origin,u,v,n)
                    emit(glazing,-.14,1,texture_base+1,'pane',*args)
                    if door is not None:
                        # Entrance: deeper recessed doors under a projecting canopy.
                        leaf=door.buffer(-.05,join_style=2)
                        emit(door.difference(leaf),-.14,4,0xffffffff,'tile',*args)
                        sides(leaf,-.14,-.40,4,origin,u,v,n)
                        emit(leaf,-.40,1,texture_base+1,'pane',*args)
                        counts['entrance']+=1
                if canopy:
                    yc=min(cb+2.9,cy+floor-.36)
                    xa,xb=max(x0,cx-.75*bay),min(x1,cx+1.75*bay)
                    slab(xa,xb,yc-.14,yc,1.4,4,n)
    meshes=list(retained)
    # Roof caps and other non-wall surfaces keep their captured geometry under
    # a clean membrane texture; low fragments near the base stay captured.
    roof_pts=np.concatenate(roof_tri) if roof_tri else np.zeros((0,3,3))
    if len(roof_pts):
        pos=roof_pts.reshape(-1,3)
        uv=((pos[:,:2]-center[:2])/ROOF_REPEAT).astype(np.float32)
        meshes.append(mtscene.Mesh(texture_base+3,oid,center,(pos-center).astype(np.float32),uv,None,
            [(4,np.arange(len(pos),dtype=np.uint32))],5,2))
    for (mat,tex),pieces in sorted(batches.items()):
        pos=np.concatenate([q[0] for q in pieces]);uv=np.concatenate([q[1] for q in pieces]).astype(np.float32)
        meshes.append(mtscene.Mesh(tex,oid,center,(pos-center).astype(np.float32),uv,None,
            [(4,np.arange(len(pos),dtype=np.uint32))],mat,2))
    sampled=sum(1 for f in frames if f['stats']['sampled'])
    return meshes,_textures(color,wall_finish),{'windows':windows,'tiles':tile_count,'patches':len(groups),
        'replaced_triangles':len(wall_tri),'roof_triangles':len(roof_pts),'source_area':area_source,'covered_area':area_new,
        'pattern':pattern,'bay':bay_nominal,'floor':floor,'wall_color':color,'wall_finish':wall_finish,'stitched_seams':stitched_seams,
        'modules':dict(counts),'sampled_patches':sampled,'entrance':entrance is not None,'orientation':'winding' if winding else 'centre'}


def apply(batches,target=None,only=None,progress=lambda *args:None,wall_finish="painted-concrete"):
    done=total=0
    for bi,batch in enumerate(batches):
        if batch.is_spline or (target is not None and bi!=target[0]):continue
        for gi,group in enumerate(batch.groups):
            if target is not None and gi!=target[1]:continue
            objects=[i for i,o in enumerate(group.objects) if o.cls==3 and
                     (target is None or i==target[2]) and (only is None or o.name in only)]
            # Only inspect generated atlas candidates: original captures often have
            # thousands of textures. Use size/format before comparing payload bytes.
            atlas_candidates=[i for i,t in enumerate(group.textures) if i+TEXTURE_COUNT-1<len(group.textures)
                              and t.mips and t.mips[0][:2]==(512,512)
                              and t.internal_format==raster.GL_COMPRESSED_RGB_S3TC_DXT1]
            for oi,oid in enumerate(objects):
                total+=1;name=group.objects[oid].name
                progress(oi,max(1,len(objects)),f"Building tiled facade: {name}")
                original=[m for m in group.meshes if m.object_id==oid and m.model_variant==1]
                source=original or [m for m in group.meshes if m.object_id==oid and m.material in (0,1,2,3)]
                base=len(group.textures)
                try:
                    new,textures,stats=build(source,oid,base,group.textures,wall_finish)
                except (ValueError,shapely.errors.GEOSException) as e:
                    print(f"  {name}: facade unchanged ({e})",flush=True);continue
                if not stats['windows']:
                    print(f"  {name}: no usable wall modules; unchanged",flush=True);continue
                if not original:
                    original=[copy.copy(m) for m in source]
                    for m in original:m.model_variant=1
                existing=next((i for i in atlas_candidates if group.textures[i:i+TEXTURE_COUNT]==textures),None)
                if existing is None:
                    group.textures+=textures;atlas_candidates.append(base)
                else:
                    for mesh in new:
                        if base<=mesh.tex_index<base+TEXTURE_COUNT:mesh.tex_index=existing+mesh.tex_index-base
                group.meshes=[m for m in group.meshes if m.object_id!=oid]+original+new
                done+=1
                print(f"  {name}: stitched {stats['stitched_seams']} internal diagonal patch joins; patches face outward by capture {stats['orientation']}",flush=True)
                rgb=stats['wall_color'];estimated='capture color estimate' if 'appearance' in stats['pattern'] else 'default color'
                print(f"  {name}: {wall_finish}, RGB {rgb}, {estimated}",flush=True)
                for axis,default in (('bay',BAY),('floor',FLOOR)):
                    found=stats['pattern'].get(axis)
                    if found:
                        label='inferred floor subdivision' if found.get('derived') else f'detected {axis} repeat'
                        print(f"  {name}: {label} {found['spacing']:.2f} m, correlation {found['confidence']:.2f}, {found['samples']} agreeing patch(es)",flush=True)
                    else:print(f"  {name}: no reliable {axis} pattern; using {default:.2f} m",flush=True)
                if 'group' in stats['pattern']:
                    pattern_group=stats['pattern']['group']
                    print(f"  {name}: detected facade group {pattern_group['spacing']:.2f} m; {pattern_group.get('floors','unknown')} floors per repeat",flush=True)
                modules=stats['modules']
                print(f"  {name}: capture-typed modules on {stats['sampled_patches']} of {stats['patches']} patches: "
                      f"{modules.get('glazed',0)} glazed, {modules.get('clad',0)} clad, {modules.get('piers',0)} pier, "
                      f"{modules.get('storefront',0)} storefront ({'entrance with canopy' if stats['entrance'] else 'no entrance'}), "
                      f"{modules.get('parapets',0)} parapet, {modules.get('plinths',0)} plinth",flush=True)
                print(f"  {name}: replaced {stats['replaced_triangles']} wall triangles with "
                      f"{stats['tiles']} tiles / {stats['windows']} recessed windows; "
                      f"{stats['roof_triangles']} roof triangles re-surfaced; "
                      f"{len(new)} meshes including retained geometry",flush=True)
    return done,total
