"""Replace captured wall triangles with clipped, recessed facade modules.

Window layouts follow local wall planes. Roofs and non-wall source triangles
remain intact. Generated geometry is batched per material, not per window.
"""
from collections import defaultdict
import copy
from functools import lru_cache
import numpy as np
import cv2
import numba
import shapely
from shapely.geometry import Polygon, box
from shapely.ops import unary_union
import mtscene
import raster

BAY, FLOOR = 1.6, 3.4


def wall_directions(normals,weights):
    """Stable horizontal facade directions; neighboring corners stay separate."""
    axes=[];sums=[]
    for i in np.argsort(-np.asarray(weights)):
        n=np.array([normals[i][0],normals[i][1],0.],float)
        length=np.linalg.norm(n)
        if length<1e-8:continue
        n/=length
        match=next((j for j,a in enumerate(axes) if np.dot(n,a)>np.cos(np.deg2rad(8))),None)
        if match is None:axes.append(n);sums.append(n*weights[i])
        else:
            sums[match]+=n*weights[i]
            axes[match]=sums[match]/np.linalg.norm(sums[match])
    return axes


@numba.njit(cache=True)
def _surface_heights(points,xy,z,a,b,inv):
    """First supporting triangle, matching captured draw order at overlaps."""
    heights=np.empty(len(points),np.float64)
    for i in range(len(points)):
        found=False
        for j in range(len(xy)):
            if inv[j]==0:continue
            dx,dy=points[i,0]-xy[j,0,0],points[i,1]-xy[j,0,1]
            u=(dx*b[j,1]-dy*b[j,0])*inv[j]
            v=(a[j,0]*dy-a[j,1]*dx)*inv[j]
            if u>=-1e-5 and v>=-1e-5 and u+v<=1.00001:
                heights[i]=z[j,0]+u*(z[j,1]-z[j,0])+v*(z[j,2]-z[j,0])
                found=True;break
        if not found:
            best=np.inf
            for j in range(len(xy)):
                for k in range(3):
                    distance=(points[i,0]-xy[j,k,0])**2+(points[i,1]-xy[j,k,1])**2
                    if distance<best:best=distance;heights[i]=z[j,k]
    return heights


@numba.njit(cache=True)
def _window_rectangle(supported,xs,ys):
    best=np.zeros(4,np.float64);score=0.
    for first in range(len(ys)-1):
        columns=np.ones(len(xs)-1,np.bool_)
        for last in range(first,len(ys)-1):
            columns &= supported[last]
            height=ys[last+1]-ys[first]
            if height<.75:continue
            start=-1
            for col in range(len(xs)):
                if col<len(columns) and columns[col]:
                    if start<0:start=col
                elif start>=0:
                    width=xs[col]-xs[start]
                    area=(width-.13)*(height-.13)
                    if width>=.48 and area>score:
                        best[:]=xs[start],ys[first],xs[col],ys[last+1]
                        score=area
                    start=-1
    return best,score

def _polygons(g):
    if g.is_empty: return []
    if g.geom_type == 'Polygon': return [g]
    return [p for part in getattr(g,'geoms',[]) for p in _polygons(part)]


@lru_cache(maxsize=32)
def _textures(color=(139,148,151),finish="painted-concrete",glass_color=(52,86,108)):
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
            tint=np.array(glass_color)+rng.uniform(-10,10)
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
    bounds,score=_window_rectangle(supported,xs,ys)
    candidate=box(*bounds) if score>0 else Polygon()
    return candidate if tile.buffer(1e-6).covers(candidate) else Polygon()


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

def _unique_triangles(tris):
    """Drop duplicate capture triangles (LOD copies), whatever their vertex order."""
    if not len(tris):return tris
    key=np.sort(np.round(tris,3).reshape(len(tris),3,3).view([('x',float),('y',float),('z',float)]).reshape(len(tris),3),axis=1)
    flat=key.view(np.dtype((np.void,key.dtype.itemsize*3)))
    _,first=np.unique(flat,return_index=True)
    return tris[np.sort(first)]


def orient_outward(groups,all_tri,center):
    """Point each patch away from the building by ray parity.

    Horizontal rays leave the patch's largest triangles along its normal; an odd
    number of crossings of the building's other surfaces means that side is
    inside. Duplicate capture triangles are removed first, so they do not count
    twice. Patches whose rays give no majority keep the centre-outward rule.
    Returns the number of patches flipped against their winding.
    """
    unique=_unique_triangles(np.asarray(all_tri,dtype=np.float64))
    v0=unique[:,0];e1=unique[:,1]-v0;e2=unique[:,2]-v0
    def crossings(o,d):
        if not len(unique):return 0
        h=np.cross(d,e2);a=np.einsum('ij,ij->i',e1,h);valid=np.abs(a)>1e-9
        f=np.divide(1.,a,out=np.zeros_like(a),where=valid);s=o-v0
        u=f*np.einsum('ij,ij->i',s,h);q=np.cross(s,e1)
        v=f*np.einsum('ij,j->i',q,d);t=f*np.einsum('ij,ij->i',q,e2)
        return int((valid&(u>=-1e-6)&(v>=-1e-6)&(u+v<=1+1e-6)&(t>1e-3)).sum())
    angles=[np.radians(a) for a in (-30,-15,0,15,30)]
    flipped=0
    for g in groups:
        n=g['n'];inside=0;total=0
        samples=sorted(g['tri'],key=lambda t:-np.linalg.norm(np.cross(t[1]-t[0],t[2]-t[0])))[:4]
        for t in samples:
            c=t.mean(0)
            for r in angles:
                R=np.array([[np.cos(r),-np.sin(r),0],[np.sin(r),np.cos(r),0],[0,0,1]])
                d=R@n;total+=1;inside+=crossings(c+d*.02,d)%2
        if total and inside*2!=total:outward=inside*2<total
        else:outward=np.dot(np.mean([t.mean(0) for t in samples],axis=0)-center,n)>0
        if not outward:
            g['n']=-n;flipped+=1
    return flipped


def build(source, oid, texture_base, source_textures=None, wall_finish="painted-concrete",
          parameters=None, detected_pattern=None):
    """Return replacement meshes, textures, and metrics; no input mesh mutation."""
    empty={'windows':0,'replaced_triangles':0,'patches':0}
    if not source or not any(len(m.vertices) for m in source):
        return [],[],empty
    parameters=parameters or {}
    if parameters.get('regular_facades',True):
        import regular_facade
        import facade_pattern
        proxy,planes,regular_stats=regular_facade.rebuild(source,parameters.get('plane_tolerance',.8),parameters.get('base_recovery'))
        if planes:
            pattern=detected_pattern if detected_pattern is not None else (facade_pattern.detect(source,source_textures,planes) if source_textures else {})
            result=build(proxy,oid,texture_base,source_textures,wall_finish,
                         {**parameters,'regular_facades':False,'align_grid':True},pattern)
            result[2]['regularization']=regular_stats
            return result
    center=np.mean([m.translation+np.mean(m.vertices,axis=0) for m in source if len(m.vertices)],axis=0)
    wall_tri=[]; retained=[]; roof_tri=[]; parts=[]
    for m in source:
        # Recovered planes own the walls. Keep the captured roof, including
        # domes, skylights and their textures, out of the material replacement.
        keep=getattr(m,'pcg_keep',False)
        for prim,idx in m.draw_calls:
            if prim!=4 or len(idx)%3:
                parts.append((m,prim,idx,None,None,None));continue
            triangles=idx.reshape(-1,3)
            p=m.vertices[triangles].astype(np.float64)+m.translation
            n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);length=np.linalg.norm(n,axis=1)
            finite=np.isfinite(p).all(axis=(1,2))
            wall=(length>.02)&(np.abs(n[:,2])<.20*length)&finite
            if keep:
                parts.append((m,prim,triangles,p,np.zeros(len(p),bool),wall|((length>.02)&finite)));continue
            wall_tri.extend(p[wall]);parts.append((m,prim,triangles,p,wall,(length>.02)&finite))
    if not wall_tri:return [],[],empty
    wall_pts=np.asarray(wall_tri).reshape(-1,3);zmin=wall_pts[:,2].min()
    # Non-wall surfaces above the base (roof caps, setbacks, roof clutter) are
    # re-surfaced; fragments near the ground keep their captured texture.
    by_mesh=defaultdict(list)
    for m,prim,triangles,p,wall,valid in parts:
        if p is None:by_mesh[id(m)].append((prim,triangles.copy()));continue
        if (getattr(m,'pcg_keep',False) and m.uvs is not None
                and source_textures is not None and 0<=m.tex_index<len(source_textures)):
            by_mesh[id(m)].append((prim,triangles.ravel().copy()));continue
        n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);length=np.linalg.norm(n,axis=1)
        horizontal=np.abs(n[:,2])>=.20*np.maximum(length,1e-12)
        roof=valid&~wall&horizontal&(p[:,:,2].mean(1)>=zmin+2.0)
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
    # Provisional outward direction from the capture winding; each patch is
    # then checked by ray parity against the building's own surfaces below.
    for t in wall_tri:
        n=np.cross(t[1]-t[0],t[2]-t[0]);n/=np.linalg.norm(n)
        match=None
        for g in groups:
            if np.dot(n,g['n'])>.98 and np.max(np.abs((t-g['anchor'])@g['n']))<.60:
                match=g;break
        if match is None:
            match={'n':n,'anchor':t[0],'tri':[]};groups.append(match)
        match['tri'].append(t)
    all_tri=np.concatenate([p[v] for _,_,_,p,_,v in parts if p is not None]) if parts else np.zeros((0,3,3))
    flipped=orient_outward(groups,all_tri,center)
    capture_groups=groups
    groups,stitched_seams=stitch_internal_seams(groups)
    import facade_pattern
    cap=facade_pattern.capture(source,source_textures) if source_textures else None
    if detected_pattern is not None:pattern=detected_pattern
    else:pattern=facade_pattern.detect(source,source_textures,capture_groups,cap) if cap else {}
    reference=facade_pattern.reference_luminance(pattern)
    color=tuple(pattern.get('appearance',{}).get('rgb',(139,148,151)))
    color=tuple(parameters.get('wall_color',color))
    glass_color=tuple(parameters.get('glass_color',(52,86,108)))
    panel_material=4 if wall_finish=='metal' else 3
    bay_nominal=pattern.get('bay',{}).get('spacing',BAY)*parameters.get('bay_scale',1.)
    floor=pattern.get('floor',{}).get('spacing',FLOOR)
    floor_anchor=pattern.get('floor',{}).get('anchor_z',center[2])
    group_floors=pattern.get('group',{}).get('floors',0)
    band_anchor=pattern.get('group',{}).get('anchor_z',floor_anchor)
    floor_offset=parameters.get('floor_offset',0.);bay_offset=parameters.get('bay_offset',0.)
    sill_fraction=parameters.get('sill_fraction');jamb_fraction=parameters.get('jamb_fraction')
    axes=wall_directions([g['n'] for g in groups],
        [sum(np.linalg.norm(np.cross(t[1]-t[0],t[2]-t[0])) for t in g['tri']) for g in groups]) if parameters.get('align_grid') else []
    palettes=parameters.get('face_colors',[])
    # The building's own palette owns the first atlas so the roof membrane
    # always sits at texture_base+3; per-face palettes append further atlases.
    atlases={(color,glass_color):texture_base};generated_textures=list(_textures(color,wall_finish,glass_color))
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
        if axes:
            direction=max(axes,key=lambda a:np.dot(a,n))
            if np.dot(direction,n)>np.cos(np.deg2rad(12)):n=direction
        local_color,local_glass=color,glass_color
        if palettes:
            palette=max(palettes,key=lambda p:np.dot(p['normal'],n))
            if np.dot(palette['normal'],n)>.90:
                local_color=tuple(palette['wall_color']);local_glass=tuple(palette['glass_color'])
        atlas_key=(local_color,local_glass)
        if atlas_key not in atlases:
            atlases[atlas_key]=texture_base+len(generated_textures)
            generated_textures.extend(_textures(local_color,wall_finish,local_glass))
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
        # Global floor datum aligns levels across the different facade patches.
        floor_phase=((origin[2]-floor_anchor)/max(v[2],.9)-floor_offset*floor)%floor
        bay_phase=(bay_offset%1.)*bay
        if axes:bay_phase=((origin-center)@u+bay_offset*bay)%bay
        row_offsets=parameters.get('row_offsets',[0.]);row_anchor=floor_anchor
        # Per-face captured phases anchor the grid where detection found them.
        if pattern.get('faces') and (parameters.get('capture_layout') or any('rows' in f for f in pattern['faces'])):
            face=max(pattern['faces'],key=lambda f:np.dot(f['normal'],n))
            if np.dot(face['normal'],n)>.97:
                if 'bay' in face:
                    bay_phase=((origin-np.array(face['origin']))@u-face['bay']['phase']+bay_offset*bay)%bay
                if 'floor' in face:
                    floor_phase=(origin[2]-face['origin'][2]-face['floor']['phase']-floor_offset*floor)%floor
                if 'rows' in face and 'row_offsets' not in parameters:
                    row_offsets=face['rows']['offsets']
                    row_anchor=face['rows']['anchor_z']
                    bay_phase=((origin-np.array(face['origin']))@u-face['rows']['phase']+bay_offset*bay)%bay
        if not isinstance(row_offsets,(list,tuple)) or not row_offsets or not np.isfinite(row_offsets).all():
            raise ValueError('row_offsets must be a non-empty list of finite bay fractions')
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
                       'classes':classes['columns'],'spans':spans,'columns':columns,'stats':classes,
                       'floor_phase':floor_phase,'bay_phase':bay_phase,'row_offsets':row_offsets,
                       'row_anchor':row_anchor,'texture':atlases[atlas_key]})
    # Ground-level columns: the lowest module row on the building base that is
    # tall enough for a storefront, above a short plinth stub where one exists.
    def ground_row(f,col):
        span=f['spans'][col]
        if span is None or f['classes'][col]=='pier' or f['origin'][2]+span[1]>zmin+1.4:return None
        phase=f['floor_phase']
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
        xy=np.asarray(xy,dtype=np.float64)
        heights=_surface_heights(xy,surface_xy,surface_depth,surface_a,surface_b,surface_inv)
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
    def sill_height(default,strong):
        # A fitted sill replaces the module's default but keeps the stronger
        # spandrel at the detected multi-floor cadence.
        if sill_fraction is None:return default
        return sill_fraction*floor+(.25 if strong else 0.)
    for fi,f in enumerate(frames):
        origin,u,v,n,xy,t,region=f['origin'],f['u'],f['v'],f['n'],f['xy'],f['t'],f['region']
        surface_xy=xy
        surface_depth=(t-origin)@n
        surface_a=xy[:,1]-xy[:,0];surface_b=xy[:,2]-xy[:,0]
        det=surface_a[:,0]*surface_b[:,1]-surface_a[:,1]*surface_b[:,0]
        surface_inv=np.divide(1.,det,out=np.zeros_like(det),where=np.abs(det)>1e-9)
        area_source+=region.area
        x0,y0,x1,y1=region.bounds
        bay=f['bay'];columns=f['columns'];wall_texture=f['texture']
        floor_phase=f['floor_phase'];row_offsets=f['row_offsets']
        for row,cy in enumerate(np.arange(-floor_phase,y1,floor)):
            # Staggered rows shift the whole column grid by a bay fraction, with
            # row indices counted from the building datum, not from the patch.
            row_level=int(round((origin[2]+cy-f['row_anchor'])/floor-floor_offset))
            shift=float(row_offsets[row_level%len(row_offsets)])%1.
            row_phase=(f['bay_phase']-shift*bay)%bay
            if row_phase<1e-9 or bay-row_phase<1e-9:row_phase=0.
            starts=np.arange(-row_phase,x1,bay);starts=starts[starts<x1-1e-6]
            for ci,cx in enumerate(starts):
                # The nominal column whose centre holds this cell supplies its
                # captured class and extent even when the row is shifted.
                col=int(np.clip(np.floor((cx+bay/2)/bay),0,columns-1))
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
                # Stronger spandrels repeat at the observed multi-floor cadence,
                # from the band's own captured datum.
                band_level=int(round((origin[2]+cy-band_anchor)/floor))
                strong=group_floors and band_level%group_floors==0
                # Solid corner posts where a patch ends: glass never runs into a
                # corner or an open edge without a mullion-sized pier.
                left=.25 if ci==0 else 0.;right=.25 if ci==len(starts)-1 else 0.
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
                    sill=sill_height(.50 if strong else .32,strong)
                    jamb=.05 if jamb_fraction is None else jamb_fraction*bay
                    opening=box(cx+jamb+left,cy+sill,cx+bay-jamb-right,cy+floor-.10);counts['glazed']+=1
                else:
                    # Cladding reads as solid: punched windows between wide piers.
                    sill=sill_height(1.15 if strong else .95,strong)
                    jamb=.30 if jamb_fraction is None else jamb_fraction*bay
                    opening=box(cx+jamb+left*.6,cy+sill,cx+bay-jamb-right*.6,cy+floor-.25);counts['clad']+=1
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
                swatch=((level%max(1,group_floors))*11+ci*7)%64
                args=(origin,u,v,n,cx,cy,swatch)
                panel_depth=.06 if kind=='pier' else 0.
                # Continuous dark backing covers panel seams and window openings.
                emit(cell,-.55,2,wall_texture+2,'tile',*args)
                emit(tile.difference(outer),panel_depth,panel_material,wall_texture,'tile',*args)
                sides(tile.difference(outer),panel_depth,-.055,panel_material,origin,u,v,n,wall_texture)
                if top:
                    # Parapet coping: a proud metal cap along every patch top edge.
                    cap_strip=cell.intersection(box(cx,ct-.22,cx+bay,ct))
                    emit(cap_strip,panel_depth+.06,4,0xffffffff,'tile',*args)
                    sides(cap_strip,panel_depth+.06,panel_depth,4,origin,u,v,n)
                if not inner.is_empty:
                    glazing=inner.difference(door) if door is not None else inner
                    emit(outer.difference(inner),.035,4,0xffffffff,'tile',*args)
                    sides(inner,.035,-.14,4,origin,u,v,n)
                    emit(glazing,-.14,1,wall_texture+1,'pane',*args)
                    if door is not None:
                        # Entrance: deeper recessed doors under a projecting canopy.
                        leaf=door.buffer(-.05,join_style=2)
                        emit(door.difference(leaf),-.14,4,0xffffffff,'tile',*args)
                        sides(leaf,-.14,-.40,4,origin,u,v,n)
                        emit(leaf,-.40,1,wall_texture+1,'pane',*args)
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
    return meshes,generated_textures,{'windows':windows,'tiles':tile_count,'patches':len(groups),
        'replaced_triangles':len(wall_tri),'roof_triangles':len(roof_pts),'source_area':area_source,'covered_area':area_new,
        'pattern':pattern,'bay':bay_nominal,'floor':floor,'wall_color':color,'wall_finish':wall_finish,'stitched_seams':stitched_seams,
        'modules':dict(counts),'sampled_patches':sampled,'entrance':entrance is not None,'flipped_patches':flipped}


def apply(batches,target=None,only=None,progress=lambda *args:None,wall_finish="painted-concrete",
          fit_iterations=0,debug_dir=None,fit_max_error=.20,reporter=None):
    from refine_progress import Reporter
    reporter = reporter or Reporter(progress)
    planned = sum(1 for bi,b in enumerate(batches) if not b.is_spline and (target is None or bi==target[0])
                  for gi,g in enumerate(b.groups) if target is None or gi==target[1]
                  for i,o in enumerate(g.objects) if o.cls==3 and (target is None or i==target[2])
                  and (only is None or o.name in only))
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
                reporter.begin(name,planned)
                original=[m for m in group.meshes if m.object_id==oid and m.model_variant==1]
                source=original or [m for m in group.meshes if m.object_id==oid and m.material in (0,1,2,3)]
                base=len(group.textures)
                try:
                    import base_recovery
                    recovery=base_recovery.infer(source,group)
                    parameters={'base_recovery':recovery} if recovery else {}
                    if recovery:
                        print(f"  {name}: completing lower walls to local ground {recovery['ground_z']:.2f} m using {len(recovery['planes'])} supported facade spans",flush=True)
                    new,textures,stats=build(source,oid,base,group.textures,wall_finish,parameters=parameters)
                    if fit_iterations and stats['windows']:
                        import multiview
                        from pathlib import Path
                        report_dir=Path(debug_dir)/f'{bi}_{gi}_{oid}' if debug_dir else None
                        new,textures,stats=multiview.fit(source,oid,base,group.textures,wall_finish,
                            (new,textures,stats),fit_iterations,report_dir,lambda *args:None,initial_parameters=parameters,
                            max_error=fit_max_error,details=reporter.update)
                        if not stats['multiview']['accepted']:
                            print(f"  {name}: nine-view comparison rejected; existing building unchanged: "
                                  + '; '.join(stats['multiview']['failed_checks']),flush=True)
                            reporter.finish(False,'Error or evidence limits not met; unchanged')
                            continue
                except (ValueError,shapely.errors.GEOSException) as e:
                    print(f"  {name}: facade unchanged ({e})",flush=True)
                    reporter.finish(False,str(e));continue
                if not stats['windows']:
                    print(f"  {name}: no usable wall modules; unchanged",flush=True)
                    reporter.finish(False,'No usable wall modules');continue
                if not original:
                    original=[copy.copy(m) for m in source]
                    for m in original:m.model_variant=1
                # A building may carry several palettes, so its atlas run is a
                # multiple of TEXTURE_COUNT; compare the whole run.
                existing=next((i for i in atlas_candidates if group.textures[i:i+len(textures)]==textures),None)
                if existing is None:
                    group.textures+=textures;atlas_candidates.append(base)
                else:
                    for mesh in new:
                        if base<=mesh.tex_index<base+len(textures):mesh.tex_index=existing+mesh.tex_index-base
                group.meshes=[m for m in group.meshes if m.object_id!=oid]+original+new
                done+=1
                reporter.finish(True)
                if stats.get('regularization'):
                    regular=stats['regularization']
                    print(f"  {name}: recovered {regular['planes']} continuous facade planes from {regular['replaced_capture_triangles']} capture triangles",flush=True)
                    if regular.get('recovered_base_area',0)>0:
                        print(f"  {name}: added {regular['recovered_base_area']:.1f} square metres of lower facade from context and inferred wall continuation",flush=True)
                print(f"  {name}: stitched {stats['stitched_seams']} internal diagonal patch joins; {stats['flipped_patches']} patch(es) turned outward by ray parity",flush=True)
                rgb=stats['wall_color'];estimated='capture color estimate' if 'appearance' in stats['pattern'] else 'default color'
                print(f"  {name}: {wall_finish}, RGB {rgb}, {estimated}",flush=True)
                for axis,default in (('bay',BAY),('floor',FLOOR)):
                    found=stats['pattern'].get(axis)
                    if found:
                        label='inferred floor subdivision' if found.get('derived') else f'detected {axis} repeat'
                        print(f"  {name}: {label} {found['spacing']:.2f} m, correlation {found['confidence']:.2f}, {found['samples']} agreeing patch(es)",flush=True)
                    else:print(f"  {name}: no reliable {axis} pattern; using {stats[axis]:.2f} m (default {default:.2f} m)",flush=True)
                    if found and abs(stats[axis]-found['spacing'])>.001:
                        print(f"  {name}: fitted {axis} spacing {stats[axis]:.2f} m",flush=True)
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
