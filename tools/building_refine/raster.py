"""Scene triangles, DXT1/3/5 decoding and a numba triangle rasterizer."""
from dataclasses import dataclass

import numba
import numpy as np


GL_TRIANGLES = 4
GL_COMPRESSED_RGB_S3TC_DXT1 = 0x83F0
GL_COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1
GL_COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2
GL_COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3


def decode_dxt1(width, height, data):
    """DXT1 blocks -> uint8[h, w, 3]."""
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    blocks = np.frombuffer(data, dtype="<u2", count=bw * bh * 4).reshape(bh, bw, 4)
    return _decode_color_blocks(blocks, width, height, always_four=False)


def decode_dxt5(width, height, data):
    """DXT3 / DXT5 blocks (8 bytes alpha + a DXT1 colour block) -> uint8[h, w, 3]; alpha dropped."""
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    blocks = np.frombuffer(data, dtype="<u2", count=bw * bh * 8).reshape(bh, bw, 8)[..., 4:8]
    return _decode_color_blocks(blocks, width, height, always_four=True)


def _decode_color_blocks(blocks, width, height, always_four):
    bh, bw = blocks.shape[:2]
    c0, c1 = blocks[..., 0].astype(np.int32), blocks[..., 1].astype(np.int32)
    bits = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 16)

    def rgb565(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)

    p0, p1 = rgb565(c0), rgb565(c1)
    four = np.ones_like(c0, bool)[..., None] if always_four else (c0 > c1)[..., None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], 2)                          # bh, bw, 4, 3
    shifts = (np.arange(16, dtype=np.uint32) * 2)
    codes = (bits[..., None] >> shifts) & 3                          # bh, bw, 16
    px = np.take_along_axis(palette, codes[..., None].astype(np.int64).repeat(3, -1), 2)
    px = px.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)
    return px[:height, :width].astype(np.uint8)


def encode_dxt1(img):
    """uint8[h, w, 3] (h, w multiples of 4) -> DXT1 bytes. Endpoints = per-block
    luminance extremes; good enough for baked photo textures."""
    h, w, _ = img.shape
    blocks = img.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, 3).astype(np.int32)
    lum = blocks @ np.array([77, 150, 29])
    lo = np.take_along_axis(blocks, lum.argmin(-1)[..., None, None].repeat(3, -1), 2)[:, :, 0]
    hi = np.take_along_axis(blocks, lum.argmax(-1)[..., None, None].repeat(3, -1), 2)[:, :, 0]

    def to565(c):
        return ((c[..., 0] * 31 + 127) // 255 << 11) | ((c[..., 1] * 63 + 127) // 255 << 5) | ((c[..., 2] * 31 + 127) // 255)

    c0, c1 = to565(hi), to565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    hi, lo = np.where(swap[..., None], lo, hi), np.where(swap[..., None], hi, lo)
    same = c0 == c1
    # palette in 4-colour mode: 0 = hi, 1 = lo, 2 = 2/3 hi, 3 = 1/3 hi
    pal = np.stack([hi, lo, (2 * hi + lo) // 3, (hi + 2 * lo) // 3], 2)  # bh, bw, 4, 3
    d = ((blocks[:, :, :, None, :] - pal[:, :, None, :, :]) ** 2).sum(-1)  # bh, bw, 16, 4
    codes = d.argmin(-1).astype(np.uint32)
    codes[same] = 0
    bits = (codes << (np.arange(16, dtype=np.uint32) * 2)).sum(-1).astype(np.uint32)
    out = np.stack([c0.astype(np.uint16), c1.astype(np.uint16),
                    (bits & 0xFFFF).astype(np.uint16), (bits >> 16).astype(np.uint16)], -1)
    return out.astype("<u2").tobytes()


@dataclass
class SceneTris:
    pos: np.ndarray        # float64[t, 3, 3] world
    uv: np.ndarray         # float32[t, 3, 2]
    tex: np.ndarray        # int32[t] texture index (-1 none)
    obj: np.ndarray        # int32[t] object id (-1 none)
    mesh: np.ndarray       # int32[t] mesh index in the group
    dc: np.ndarray = None  # int32[t] draw call index in that mesh
    local: np.ndarray = None  # int32[t] triangle index in that draw call


def collect(group):
    pos, uv, tex, obj, mesh, dcs, local = [], [], [], [], [], [], []
    for mi, m in enumerate(group.meshes):
        for di, (prim, idx) in enumerate(m.draw_calls):
            if prim != GL_TRIANGLES or len(idx) < 3:
                continue
            idx = idx[: len(idx) // 3 * 3].reshape(-1, 3)
            dcs.append(np.full(len(idx), di, np.int32))
            local.append(np.arange(len(idx), dtype=np.int32))
            pos.append(m.vertices[idx].astype(np.float64) + m.translation)
            uv.append(m.uvs[idx] if m.uvs is not None else np.zeros((len(idx), 3, 2), np.float32))
            t = m.tex_index if m.tex_index < len(group.textures) else -1
            tex.append(np.full(len(idx), t, np.int32))
            obj.append(np.full(len(idx), m.object_id, np.int32))
            mesh.append(np.full(len(idx), mi, np.int32))
    return SceneTris(np.concatenate(pos), np.concatenate(uv), np.concatenate(tex),
                     np.concatenate(obj), np.concatenate(mesh), np.concatenate(dcs), np.concatenate(local))


def decode_textures(group):
    """All group textures as one uint8[n, 256, 256, 3] stack (resized if needed)."""
    import cv2
    out = np.zeros((max(1, len(group.textures)), 256, 256, 3), np.uint8)
    for i, t in enumerate(group.textures):
        if not t.mips:
            continue
        w, h, data = t.mips[0]
        if t.internal_format in (GL_COMPRESSED_RGB_S3TC_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT1):
            img = decode_dxt1(w, h, data)
        elif t.internal_format in (GL_COMPRESSED_RGBA_S3TC_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT5):
            img = decode_dxt5(w, h, data)
        elif len(data) >= w * h * 4:
            img = np.frombuffer(data, np.uint8, w * h * 4).reshape(h, w, 4)[..., :3]
        else:
            skipped = decode_textures.skipped = getattr(decode_textures, "skipped", 0) + 1
            if skipped == 1:
                print(f"texture format 0x{t.internal_format:X} not decoded: those tiles sample black", flush=True)
            continue
        out[i] = img if img.shape[:2] == (256, 256) else cv2.resize(img, (256, 256), interpolation=cv2.INTER_AREA)
    return out


@numba.njit(cache=True)
def _sample(texs, t, u, v):
    """Bilinear, wrapping (the captured tiles repeat their UVs)."""
    if t < 0:
        return 128, 128, 128
    fx = u * 256.0 - 0.5
    fy = v * 256.0 - 0.5
    x0 = int(np.floor(fx))
    y0 = int(np.floor(fy))
    ax = fx - x0
    ay = fy - y0
    x0 %= 256
    y0 %= 256
    x1 = (x0 + 1) % 256
    y1 = (y0 + 1) % 256
    r = g = b = 0.0
    for yy, wy in ((y0, 1.0 - ay), (y1, ay)):
        for xx, wx in ((x0, 1.0 - ax), (x1, ax)):
            w = wx * wy
            r += texs[t, yy, xx, 0] * w
            g += texs[t, yy, xx, 1] * w
            b += texs[t, yy, xx, 2] * w
    return np.uint8(r + 0.5), np.uint8(g + 0.5), np.uint8(b + 0.5)


@numba.njit(cache=True)
def rasterize(scr, uv, tex, tid, texs, W, H, depth, color, ids, zband_lo, zband_hi):
    """Rasterize triangles given in screen space scr[t, 3, 3] = (px, py, depth).
    Keeps the largest depth within [zband_lo, zband_hi] (nearest to the viewer).
    Writes depth[H, W], color[H, W, 3] (sampled from texs) and ids[H, W] = tid."""
    for t in range(scr.shape[0]):
        x0, y0, z0 = scr[t, 0, 0], scr[t, 0, 1], scr[t, 0, 2]
        x1, y1, z1 = scr[t, 1, 0], scr[t, 1, 1], scr[t, 1, 2]
        x2, y2, z2 = scr[t, 2, 0], scr[t, 2, 1], scr[t, 2, 2]
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-12:
            continue
        minx = max(int(np.floor(min(x0, x1, x2))), 0)
        maxx = min(int(np.ceil(max(x0, x1, x2))), W - 1)
        miny = max(int(np.floor(min(y0, y1, y2))), 0)
        maxy = min(int(np.ceil(max(y0, y1, y2))), H - 1)
        for py in range(miny, maxy + 1):
            cy = py + 0.5
            for px in range(minx, maxx + 1):
                cx = px + 0.5
                w0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) / area
                w1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) / area
                w2 = 1.0 - w0 - w1
                if w0 < -1e-6 or w1 < -1e-6 or w2 < -1e-6:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                if z < zband_lo or z > zband_hi or z <= depth[py, px]:
                    continue
                depth[py, px] = z
                u = w0 * uv[t, 0, 0] + w1 * uv[t, 1, 0] + w2 * uv[t, 2, 0]
                v = w0 * uv[t, 0, 1] + w1 * uv[t, 1, 1] + w2 * uv[t, 2, 1]
                r, g, b = _sample(texs, tex[t], u, v)
                color[py, px, 0] = r
                color[py, px, 1] = g
                color[py, px, 2] = b
                ids[py, px] = tid[t]


@numba.njit(cache=True)
def _tri_texture_stats(uv, tex, texs, out):
    """Per triangle, over a 10-point barycentric grid of its texture: mean
    brightness (max channel), its std, saturation of the mean colour, and the
    share of samples that are placeholder colours (near-black, or a bright
    saturated primary such as the yellow of Google Earth's missing-texture pattern)."""
    for t in range(len(tex)):
        if tex[t] < 0:
            out[t, 0] = 128.0
            continue
        n = 0
        s = s2 = sr = sg = sb = 0.0
        ph = 0
        for i in range(4):
            for j in range(4 - i):
                a = (i + 0.5) / 4.0
                b = (j + 0.5) / 4.0
                c = 1.0 - a - b
                if c < 0.0:
                    continue
                u = a * uv[t, 0, 0] + b * uv[t, 1, 0] + c * uv[t, 2, 0]
                v = a * uv[t, 0, 1] + b * uv[t, 1, 1] + c * uv[t, 2, 1]
                r, g, bl = _sample(texs, tex[t], u, v)
                m = max(r, max(g, bl))
                mn_ = min(r, min(g, bl))
                if m < 12 or (m > 100 and (m - mn_) > 0.8 * m):
                    ph += 1
                s += m
                s2 += m * m
                sr += r
                sg += g
                sb += bl
                n += 1
        mean = s / n
        var = s2 / n - mean * mean
        out[t, 0] = mean
        out[t, 1] = np.sqrt(var) if var > 0.0 else 0.0
        mx = max(sr, max(sg, sb)) / n
        mn = min(sr, min(sg, sb)) / n
        out[t, 2] = (mx - mn) / mx if mx > 0.0 else 0.0
        out[t, 3] = ph / n


def yellowish(c):
    """The yellow of Google Earth's missing-texture checker, also after
    resampling has blended it with its black cells (olive)."""
    c = c.astype(np.int32)
    return (c[..., 0] > 120) & (c[..., 1] > 110) & (c[..., 2] < 100) & ((c[..., 0] + c[..., 1]) // 2 - c[..., 2] > 60)


def placeholder_textures(texs, black=12, share=0.5):
    """Textures that are mostly placeholder: near-black (Google Earth's dark
    overlay pass on the ground tiles) or the yellow/black checker of imagery
    that had not streamed in. Such a texture may still carry real imagery in a
    corner, so this decides drawing order, not deletion."""
    flat = texs.reshape(len(texs), -1, 3)
    return ((flat.max(-1) < black) | yellowish(flat)).mean(1) > share


def triangle_texture_stats(tris, texs):
    """float64[t, 4]: mean brightness, std, saturation, placeholder-colour share."""
    st = np.zeros((len(tris.tex), 4), np.float64)
    _tri_texture_stats(tris.uv.astype(np.float64), tris.tex, texs, st)
    return st


def placeholder_pixels(color, black=12):
    """Pixels coloured like a placeholder: near-black, or the yellow of Google
    Earth's missing-texture pattern."""
    c = color.astype(np.int32)
    return (c.max(-1) < black) | yellowish(color)


@dataclass
class TopRaster:
    min_x: float
    max_y: float
    res: float
    height: np.ndarray     # float32[H, W], -inf empty
    color: np.ndarray      # uint8[H, W, 3]
    obj: np.ndarray        # int32[H, W] object id, -1 none
    tri: np.ndarray = None  # int32[H, W] triangle row in the SceneTris, -1 none

    def to_px(self, x, y):
        return (x - self.min_x) / self.res, (self.max_y - y) / self.res

    def to_world(self, px, py):
        return self.min_x + px * self.res, self.max_y - py * self.res


def top_down(tris, texs, res=0.25, depth_bias=None):
    """depth_bias: optional float[t] added to the triangles' heights for the
    depth test only (e.g. negative for Google Earth's dark overlay twins)."""
    lo, hi = tris.pos.reshape(-1, 3).min(0), tris.pos.reshape(-1, 3).max(0)
    W, H = int(np.ceil((hi[0] - lo[0]) / res)), int(np.ceil((hi[1] - lo[1]) / res))
    scr = np.empty(tris.pos.shape, np.float64)
    scr[..., 0] = (tris.pos[..., 0] - lo[0]) / res
    scr[..., 1] = (hi[1] - tris.pos[..., 1]) / res
    scr[..., 2] = tris.pos[..., 2]
    if depth_bias is not None:
        scr[..., 2] += depth_bias[:, None]
    depth = np.full((H, W), -np.inf)
    color = np.zeros((H, W, 3), np.uint8)
    ids = np.full((H, W), -1, np.int32)
    rasterize(scr, tris.uv, tris.tex, np.arange(len(tris.obj), dtype=np.int32), texs, W, H, depth, color, ids, -1e30, 1e30)
    obj = np.where(ids >= 0, tris.obj[np.maximum(ids, 0)], -1).astype(np.int32)
    if depth_bias is not None:
        hit = ids >= 0
        depth[hit] -= depth_bias[ids[hit]]
    return TopRaster(lo[0], hi[1], res, depth.astype(np.float32), color, obj, ids)
