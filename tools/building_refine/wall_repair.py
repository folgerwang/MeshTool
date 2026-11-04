"""Repair distorted or missing parts of a captured wall from the wall itself.

Geometry alone cannot find these: a wavy ledge can sit closer to its plane than
a clean facade does. Facades repeat, so the texture can judge: rows and columns
of the rectified wall give a floor and bay period. A cell the capture barely
saw is where the mesh was guessed and distorted; it is rebuilt from the best
captured like cell of the same wall (same floor first), so nothing new is
designed, and its faces are flattened onto the wall. Walls without a clear
repeat are left as captured.
"""
import cv2
import numpy as np

MIN_PERIOD, MAX_PERIOD = 2.4, 6.0   # floor height (m)
MIN_BAY, MAX_BAY = 1.2, 9.0         # bay width (m)
MIN_REPEAT = .25                    # autocorrelation peak needed to trust a period
MIN_CELLS = 3                       # repeats along an axis
MIN_COVERAGE = .6                   # less captured than this is missing
SEARCH = .4                         # metres a donor may slide to line up


def period(profile, pitch, low, high):
    """Strongest repeat of a 1-D profile: (metres, autocorrelation peak) or None."""
    p = profile-np.mean(profile)
    n = len(p)
    a, b = int(low/pitch), min(int(high/pitch), n//MIN_CELLS)
    if b <= a+1 or np.std(p) < 1e-3:
        return None
    spectrum = np.fft.rfft(p, 2*n)
    corr = np.fft.irfft(spectrum*np.conj(spectrum))[:n]
    corr = corr/max(corr[0], 1e-6)*n/np.maximum(n-np.arange(n), 1)
    lags = np.arange(a, b)
    peaks = lags[(corr[lags] > corr[lags-1]) & (corr[lags] >= corr[lags+1])]
    if not len(peaks):
        return None
    best = corr[peaks].max()
    if best <= 0:
        return None
    # The fundamental: the shortest lag nearly as strong as the strongest.
    lag = peaks[corr[peaks] >= .8*best][0]
    return float(lag*pitch), float(corr[lag])


def grid(image, mask, pitch):
    """Cell edges (x, y in pixels) of a repeating wall, or None."""
    gray = cv2.cvtColor(image, cv2.COLOR_RGB2GRAY).astype(np.float32)
    m = mask.astype(np.float32)
    rows = (gray*m).sum(1)/np.maximum(m.sum(1), 1)
    cols = (gray*m).sum(0)/np.maximum(m.sum(0), 1)
    fy = period(rows, pitch, MIN_PERIOD, MAX_PERIOD)
    fx = period(cols, pitch, MIN_BAY, MAX_BAY)
    if fy is None or fx is None or fy[1] < MIN_REPEAT or fx[1] < MIN_REPEAT:
        return None
    px, py = fx[0]/pitch, fy[0]/pitch
    # Phase: cell edges fall on the brightest band (wall between windows).
    def phase(profile, step):
        bins = np.arange(len(profile)) % int(round(step))
        means = np.array([profile[bins == k].mean() for k in range(int(round(step)))])
        return int(np.argmax(np.convolve(np.r_[means, means], np.ones(3)/3, 'same')[:len(means)]))
    x0, y0 = phase(cols, px), phase(rows, py)
    xs = np.arange(x0, image.shape[1]-px+1, px).round().astype(int)
    ys = np.arange(y0, image.shape[0]-py+1, py).round().astype(int)
    if len(xs) < MIN_CELLS or len(ys) < MIN_CELLS:
        return None
    return xs, ys, int(round(px)), int(round(py))


def _ncc(a, b, mask=None):
    if mask is not None:
        if mask.sum() < 16:
            return 0.
        a, b = a[mask], b[mask]
    a, b = a-a.mean(), b-b.mean()
    return float((a*b).sum()/max(np.sqrt((a*a).sum()*(b*b).sum()), 1e-3))


def assess(image, mask, pitch):
    """Score every cell: dict(xs, ys, w, h, ncc[r, c], coverage[r, c], bad[r, c]) or None."""
    g = grid(image, mask, pitch)
    if g is None:
        return None
    xs, ys, w, h = g
    gray = cv2.cvtColor(image, cv2.COLOR_RGB2GRAY).astype(np.float32)
    tiles = np.stack([[gray[y:y+h, x:x+w] for x in xs] for y in ys])
    covers = np.stack([[mask[y:y+h, x:x+w] for x in xs] for y in ys])
    coverage = covers.mean((2, 3))
    usable = coverage > .9
    if usable.sum() < 4:
        return None
    consensus = np.median(tiles[usable], 0)
    ncc = np.array([[_ncc(tiles[r, c], consensus, covers[r, c]) for c in range(len(xs))] for r in range(len(ys))])
    typical = np.median(ncc[usable])
    if typical < .3:
        return None      # cells do not really repeat
    # Only cells the capture barely saw are rebuilt. A fully captured cell
    # unlike its neighbours (ncc) is as likely a mural, sign or entrance as a
    # distortion, and stays as captured. The ground floor (last image row) is
    # never rebuilt from other cells: entrances and the street meet it.
    bad = coverage < MIN_COVERAGE
    bad[-1] = False
    return dict(xs=xs, ys=ys, w=w, h=h, ncc=ncc, coverage=coverage, bad=bad, typical=float(typical))


def repair(image, mask, pitch):
    """(repaired image, repaired mask, [(x0, y0, x1, y1) repaired pixel rects]).
    Each bad cell is replaced by the best-matching good cell, nearest first."""
    a = assess(image, mask, pitch)
    if a is None:
        return image, mask, []
    out, cover = image.copy(), mask.copy()
    xs, ys, w, h = a['xs'], a['ys'], a['w'], a['h']
    good = np.argwhere(~a['bad'] & (a['coverage'] > .95) & (a['ncc'] >= a['typical']-.1))
    if not len(good):
        return image, mask, []
    rects = []
    for r, c in np.argwhere(a['bad']):
        # Same floor first (storefronts and parapets repeat along their own
        # floor), then the same column, then nearest.
        order = np.lexsort((np.abs(good[:, 1]-c), np.abs(good[:, 0]-r)))
        dr, dc = good[order[0]]
        y, x, sy, sx = ys[r], xs[c], ys[dr], xs[dc]
        out[y:y+h, x:x+w] = image[sy:sy+h, sx:sx+w]
        cover[y:y+h, x:x+w] = True
        rects.append((int(x), int(y), int(x+w), int(y+h)))
    return out, cover, rects
