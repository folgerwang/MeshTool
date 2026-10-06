"""Reader/writer for MeshTool .mtscene files (src/scenefile.cpp)."""
import struct
from dataclasses import dataclass, field

import numpy as np

MAGIC = b"MTSCENE\0"
VERSION = 4
MAT_CAPTURED, MAT_GLASS, MAT_INTERIOR = 0, 1, 2


@dataclass
class Texture:
    internal_format: int = 0
    format: int = 0
    type: int = 0
    mips: list = field(default_factory=list)   # [(width, height, bytes)]


@dataclass
class Mesh:
    tex_index: int
    object_id: int
    translation: np.ndarray            # float64[3]
    vertices: np.ndarray               # float32[n, 3], relative to translation
    uvs: np.ndarray | None             # float32[n, 2]
    colors: np.ndarray | None          # uint32[n]
    draw_calls: list                   # [(primitive_type, uint32[count])]
    material: int = MAT_CAPTURED


@dataclass
class SceneObject:
    name: str
    cls: int


@dataclass
class Group:
    textures: list
    meshes: list
    objects: list


@dataclass
class Batch:
    is_spline: bool
    is_google_dump: bool
    is_georeferenced: bool
    reference_pos: tuple
    groups: list


class _Reader:
    def __init__(self, data):
        self.d, self.p = memoryview(data), 0

    def take(self, n):
        b = self.d[self.p:self.p + n]
        if len(b) != n:
            raise ValueError("truncated scene file")
        self.p += n
        return b

    def pod(self, fmt):
        s = struct.calcsize(fmt)
        return struct.unpack(fmt, self.take(s))[0]

    def array(self, dtype, count):
        dt = np.dtype(dtype)
        return np.frombuffer(self.take(dt.itemsize * count), dtype=dt).copy()


def load(path):
    with open(path, "rb") as f:
        r = _Reader(f.read())
    if bytes(r.take(8)) != MAGIC:
        raise ValueError("not a MeshTool scene file")
    version = r.pod("<I")
    if not 1 <= version <= VERSION:
        raise ValueError(f"unsupported scene version {version}")
    batches = []
    for _ in range(r.pod("<I")):
        is_spline = r.pod("<B") != 0
        is_dump = r.pod("<B") != 0
        georef = r.pod("<B") != 0 if version >= 2 else False
        ref = (r.pod("<d"), r.pod("<d"))
        groups = []
        for _ in range(r.pod("<I")):
            textures = []
            for _ in range(r.pod("<I")):
                t = Texture()
                levels = r.pod("<I")
                if levels:
                    t.internal_format, t.format, t.type = r.pod("<I"), r.pod("<I"), r.pod("<I")
                    for _ in range(levels):
                        w, h, size = r.pod("<I"), r.pod("<I"), r.pod("<I")
                        t.mips.append((w, h, bytes(r.take(size))))
                textures.append(t)
            meshes = []
            for _ in range(r.pod("<I")):
                n = r.pod("<I")
                tex = r.pod("<I")
                obj = r.pod("<i") if version >= 3 else -1
                mat = r.pod("<B") if version >= 4 else MAT_CAPTURED
                tr = np.array([r.pod("<d"), r.pod("<d"), r.pod("<d")])
                has_uv, has_color = r.pod("<B"), r.pod("<B")
                verts = r.array("<f4", n * 3).reshape(n, 3) if n else np.zeros((0, 3), np.float32)
                uvs = r.array("<f4", n * 2).reshape(n, 2) if n and has_uv else None
                cols = r.array("<u4", n) if n and has_color else None
                dcs = []
                for _ in range(r.pod("<I")):
                    prim = r.pod("<I")
                    count = r.pod("<I")
                    dcs.append((prim, r.array("<u4", count)))
                meshes.append(Mesh(tex, obj, tr, verts, uvs, cols, dcs, mat))
            objects = []
            if version >= 3:
                for _ in range(r.pod("<I")):
                    name = bytes(r.take(r.pod("<I"))).decode("utf-8", "replace")
                    objects.append(SceneObject(name, r.pod("<B")))
            groups.append(Group(textures, meshes, objects))
        batches.append(Batch(is_spline, is_dump, georef, ref, groups))
    return batches


def save(path, batches):
    out = bytearray()
    w = lambda fmt, *v: out.extend(struct.pack(fmt, *v))
    out += MAGIC
    w("<I", VERSION)
    w("<I", len(batches))
    for b in batches:
        w("<BBB", int(b.is_spline), int(b.is_google_dump), int(b.is_georeferenced))
        w("<dd", *b.reference_pos)
        w("<I", len(b.groups))
        for g in b.groups:
            w("<I", len(g.textures))
            for t in g.textures:
                w("<I", len(t.mips))
                if t.mips:
                    w("<III", t.internal_format, t.format, t.type)
                    for mw, mh, data in t.mips:
                        w("<III", mw, mh, len(data))
                        out.extend(data)
            w("<I", len(g.meshes))
            for m in g.meshes:
                n = len(m.vertices)
                w("<IIiB", n, m.tex_index & 0xFFFFFFFF, m.object_id, m.material)
                w("<ddd", *map(float, m.translation))
                has_uv = m.uvs is not None and n > 0
                has_col = m.colors is not None and n > 0
                w("<BB", int(has_uv), int(has_col))
                out += np.ascontiguousarray(m.vertices, "<f4").tobytes()
                if has_uv:
                    out += np.ascontiguousarray(m.uvs, "<f4").tobytes()
                if has_col:
                    out += np.ascontiguousarray(m.colors, "<u4").tobytes()
                w("<I", len(m.draw_calls))
                for prim, idx in m.draw_calls:
                    w("<II", prim, len(idx))
                    out += np.ascontiguousarray(idx, "<u4").tobytes()
            w("<I", len(g.objects))
            for o in g.objects:
                nb = o.name.encode("utf-8")
                w("<I", len(nb))
                out += nb
                w("<B", o.cls)
    with open(path, "wb") as f:
        f.write(out)
