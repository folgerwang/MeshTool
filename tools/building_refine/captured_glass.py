"""Window panes where a captured building shows them, for blender_refine.

The captured mesh and texture stay as they are; this only says where the
glass panes of each wall plane are, in plane metres. Facades put their
windows in floors and columns: on the rectified capture of a wall, rows with
many locally dark pixels are window bands, and inside a band, dark column
runs are panes. Mullions, transoms and the wall between windows are lighter
and stay wall, so a curtain wall yields one pane per light and a punched
facade one per window (or per sash).

Blender (blender_cleanup.py) cuts the mesh along each pane and the frame ring
around it, sets the opening back into the wall, the pane further back behind
its frame, and gives the pane the viewer's glass material.
"""
import cv2
import numpy as np
import shapely

import facade_pattern

ANGLE = np.radians(10)   # faces within this of a plane's normal belong to it
OFFSET = .6              # ...and within this distance of it (m)
MIN_AREA = 25.           # m2: smaller vertical planes are not facades
BIN_ANGLE, BIN_OFFSET = np.radians(5), .5
PITCH = .08              # rectified capture resolution (m per pixel)
DARK = 8.                # grey levels below the local wall that count as glazing
FRAME = .08              # captured frame ring kept around each pane (m)


def wall_planes(pos, min_area=MIN_AREA):
    """Vertical planes of a building: [dict(n, d, faces)] with n horizontal
    outward unit normals and d the offset (n . x) of the plane."""
    cross = np.cross(pos[:, 1]-pos[:, 0], pos[:, 2]-pos[:, 0])
    area = np.linalg.norm(cross, axis=1)/2
    unit = cross/np.maximum(2*area, 1e-12)[:, None]
    # Captured meshes wind faces either way: fold both sides of a wall
    # together and orient each plane outward once it is found.
    wound = unit.copy()
    unit = np.where(((unit[:, 1] < 0) | ((unit[:, 1] == 0) & (unit[:, 0] < 0)))[:, None], -unit, unit)
    center = pos.mean(1)
    vertical = (np.abs(unit[:, 2]) < .2) & (area > 1e-4)
    theta = np.arctan2(unit[:, 1], unit[:, 0])
    nt = int(round(2*np.pi/BIN_ANGLE))
    tb = np.floor((theta+np.pi)/BIN_ANGLE).astype(int) % nt
    axis = (np.arange(nt)+.5)*BIN_ANGLE-np.pi
    reach = np.abs(center[:, :2]).max()+1 if len(center) else 1
    nd = int(np.ceil(2*reach/BIN_OFFSET))+1
    d_bin = (center[:, 0]*np.cos(axis[tb])+center[:, 1]*np.sin(axis[tb])+reach)/BIN_OFFSET
    db = np.clip(d_bin.astype(int), 0, nd-1)
    remaining = vertical.copy()
    planes = []
    while remaining.any():
        acc = np.zeros((nt, nd))
        np.add.at(acc, (tb[remaining], db[remaining]), area[remaining])
        smooth = sum(np.roll(np.roll(acc, i, 0), j, 1) for i in (-1, 0, 1) for j in (-1, 0, 1))
        t, k = np.unravel_index(np.argmax(smooth), smooth.shape)
        if smooth[t, k] < min_area:
            break
        h = np.array([np.cos(axis[t]), np.sin(axis[t])])
        d0 = (k+.5)*BIN_OFFSET-reach
        for tolerance in (1., OFFSET):
            members = remaining & (unit[:, :2]@h > np.cos(ANGLE)) & (np.abs(center[:, :2]@h-d0) < tolerance)
            if not members.any():
                break
            h = (unit[members, :2]*area[members, None]).sum(0)
            h /= np.linalg.norm(h)
            dist = center[members, :2]@h
            order = np.argsort(dist)
            cum = np.cumsum(area[members][order])
            d0 = dist[order][np.searchsorted(cum, cum[-1]/2)]
        members = remaining & (unit[:, :2]@h > np.cos(ANGLE)) & (np.abs(center[:, :2]@h-d0) < OFFSET)
        if area[members].sum() < min_area:
            # The peak was spread over several planes: retire its neighbourhood.
            near = (np.minimum((tb-t) % nt, (t-tb) % nt) <= 1) & (np.abs(db-k) <= 1)
            remaining &= ~near
            continue
        remaining &= ~members
        # Outward: the way most of its faces are wound (captures face outward
        # in the main); away from the building's centre when the winding is
        # split. Courtyard walls face the centre, so winding comes first.
        votes = (wound[members, :2]@h*area[members]).sum()
        if (votes < 0) if abs(votes) > .3*area[members].sum() else (d0 < 0):
            h, d0 = -h, -d0
        planes.append(dict(n=np.array([h[0], h[1], 0.]), d=float(d0), faces=np.flatnonzero(members)))
    return planes


def frame(plane, pos):
    """Plane frame: origin at the lower left (seen from outside), u right, v up."""
    n = plane['n']
    u = np.array([-n[1], n[0], 0.])
    v = np.array([0., 0., 1.])
    p = pos[plane['faces']].reshape(-1, 3)
    s, z = p@u, p[:, 2]
    origin = n*plane['d']+u*s.min()+v*z.min()
    size = (float(s.max()-s.min()), float(z.max()-z.min()))
    tri = pos[plane['faces']]-origin
    polygons = shapely.polygons(np.stack([tri@u, tri@v], -1))
    region = shapely.union_all(shapely.buffer(polygons[shapely.area(polygons) > 1e-6], .05))
    return dict(origin=origin, u=u, v=v, n=n, size=size, region=region)


def _runs(flags):
    d = np.diff(np.r_[0, flags.astype(int), 0])
    return list(zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1)))


def panes(image, mask, pitch, dark_min=DARK):
    """Glass panes on a rectified wall: [(x0, y0, x1, y1)] pixel rectangles."""
    gray = cv2.cvtColor(image, cv2.COLOR_RGB2GRAY).astype(np.float32)
    m = mask.astype(np.float32)
    sigma = max(1., 2./pitch)
    wall = cv2.GaussianBlur(gray*m, (0, 0), sigma)/np.maximum(cv2.GaussianBlur(m, (0, 0), sigma), 1e-3)
    dark = ((wall-gray) > dark_min) & mask
    h, w = gray.shape
    covered = mask.sum(1)
    rows = np.convolve(np.where(covered > 0, dark.sum(1)/np.maximum(covered, 1), 0), np.ones(3)/3, 'same')
    wide = covered > .2*w
    if not wide.any():
        return []
    level = max(.12, .5*np.percentile(rows[wide], 90))
    found = []
    for y0, y1 in _runs(rows > level):
        if not .6 <= (y1-y0)*pitch <= 6:
            continue
        band = mask[y0:y1].sum(0)
        cols = np.where(band > 0, dark[y0:y1].sum(0)/np.maximum(band, 1), 0)
        for x0, x1 in _runs(cols > .5):
            if not .3 <= (x1-x0)*pitch <= 4.5:
                continue
            inside = np.flatnonzero(dark[y0:y1, x0:x1].mean(1) > .5)
            if len(inside) < 2:
                continue
            a, b = y0+inside[0], y0+inside[-1]+1
            if mask[a:b, x0:x1].mean() < .8 or dark[a:b, x0:x1].mean() < .55:
                continue
            found.append((x0, a, x1, b))
    # A pane under .8 m is a transom or sill light only next to a full pane.
    tall = [r for r in found if (r[3]-r[1])*pitch >= .8]
    def beside(r):
        return any(min(r[2], t[2])-max(r[0], t[0]) > .5*(r[2]-r[0]) and
                   min(abs(r[1]-t[3]), abs(t[1]-r[3]))*pitch < .3 for t in tall)
    found = tall+[r for r in found if (r[3]-r[1])*pitch < .8 and beside(r)]
    # Windows repeat: a pane needs a like-sized pane in its row or column.
    def repeated(r):
        w, h = r[2]-r[0], r[3]-r[1]
        for t in found:
            if t is r or abs((t[2]-t[0])-w) > max(.4/pitch, .35*w) or abs((t[3]-t[1])-h) > max(.4/pitch, .3*h):
                continue
            if abs(t[1]-r[1])*pitch < .35 or abs((t[0]+t[2])-(r[0]+r[2]))*pitch/2 < .4:
                return True
        return False
    return [r for r in found if repeated(r)]


def wall_pixels(cap, f, center, pitch):
    """Rectified capture of a wall plane and the pixels that show the wall
    itself: ground, roofs and ledges crossing the plane are not wall."""
    from types import SimpleNamespace
    import raster
    face = SimpleNamespace(origin=f['origin']+center, u=f['u'], v=f['v'], normal=f['n'], size=f['size'])
    w = max(1, int(np.ceil(face.size[0]/pitch)))
    h = max(1, int(np.ceil(face.size[1]/pitch)))
    P = cap.src.pos-face.origin
    scr = np.empty_like(P)
    scr[..., 0] = (P@face.u)/pitch
    scr[..., 1] = h-(P@face.v)/pitch
    scr[..., 2] = P@face.normal
    depth = np.full((h, w), -np.inf)
    color = np.zeros((h, w, 3), np.uint8)
    ids = np.full((h, w), -1, np.int32)
    raster.rasterize(scr, cap.src.uv, cap.src.tex, np.arange(len(scr), dtype=np.int32), cap.tex, w, h,
                     depth, color, ids, -.8, .8, False)
    cross = np.cross(cap.src.pos[:, 1]-cap.src.pos[:, 0], cap.src.pos[:, 2]-cap.src.pos[:, 0])
    # Steep faces only: window reveals and wrinkles stay, ground and roofs go.
    facing = np.abs(cross[:, 2]) < .6*np.linalg.norm(cross, axis=1)
    wall = (ids >= 0) & facing[np.maximum(ids, 0)]
    return color, wall, np.where(ids >= 0, depth, -np.inf)


def detect(cap, planes, center):
    """Captured wall planes with their panes: [dict(frame..., rects, image,
    mask, pitch)], rects as captured_glass.framed in plane metres."""
    from refine import fill_holes
    found = []
    for f in planes:
        if f['size'][0] < 2 or f['size'][1] < 2.5:
            continue
        pitch = max(PITCH, max(f['size'])/4096)
        image, mask, depth = wall_pixels(cap, f, center, pitch)
        h, w = mask.shape
        ys, xs = (np.mgrid[:h, :w]+.5)*pitch
        inside = shapely.contains_xy(f['region'], xs, h*pitch-ys)
        if mask.sum() < 400 or not inside.any() or mask[inside].mean() < .4:
            continue
        raw = fill_holes(image.copy(), mask)
        mask = mask & inside
        rects = []
        for x0, y0, x1, y1 in panes(image, mask, pitch):
            rects.append((x0*pitch, (h-y1)*pitch, x1*pitch, (h-y0)*pitch))
        found.append(dict(f, rects=framed(rects), image=raw, mask=mask, depth=depth, pitch=pitch))
    return found


def framed(rects, width=FRAME):
    """(pane, outer) rectangles: the frame ring around each pane is FRAME wide,
    or half the gap to the next pane where mullions are narrower."""
    out = []
    for i, (u0, v0, u1, v1) in enumerate(rects):
        side = [width]*4    # left, bottom, right, top
        for j, (a0, b0, a1, b1) in enumerate(rects):
            if j == i:
                continue
            if min(v1, b1) > max(v0, b0):          # same row: left / right neighbours
                if a1 <= u0:
                    side[0] = min(side[0], (u0-a1)/2)
                if a0 >= u1:
                    side[2] = min(side[2], (a0-u1)/2)
            if min(u1, a1) > max(u0, a0):          # same column: below / above
                if b1 <= v0:
                    side[1] = min(side[1], (v0-b1)/2)
                if b0 >= v1:
                    side[3] = min(side[3], (b0-v1)/2)
        side = [max(.01, x) for x in side]
        out.append((u0, v0, u1, v1, u0-side[0], v0-side[1], u1+side[2], v1+side[3]))
    return out


def capture(meshes, textures):
    return facade_pattern.capture(meshes, textures)
