"""Reader/writer for MeshTool .mtscene files (src/scenefile.cpp)."""
import struct
import json
from dataclasses import dataclass, field

import numpy as np

MAGIC = b"MTSCENE\0"
VERSION = 9   # 9: stable capture identity after route; 8: per-group tree planting JSON
MAT_CAPTURED, MAT_GLASS, MAT_INTERIOR = 0, 1, 2


class Scene(list):
    """List-compatible batches with an exact v6 driving-route payload."""
    def __init__(self, batches=(), route_data=b'\x00\x00\x00\x00', capture=None):
        super().__init__(batches)
        self.route_data=route_data
        self.capture = dict(capture or {})


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
    model_variant: int = 0 # 0 shared, 1 original, 2 refined
    lod_size: float = 0.0  # GE tile edge (metres): coarser draws behind finer; 0 = not a GE tile


@dataclass
class SceneObject:
    name: str
    cls: int


@dataclass
class Group:
    textures: list
    meshes: list
    objects: list
    tree_instances: list = field(default_factory=list)


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


def load(path, route=None):
    """Read a scene. `route`, if a dict, receives the driving route a version 6
    file carries (polyline of (lon, lat), from_name, to_name, from, to,
    length_m); it is left empty when the file has none."""
    with open(path, "rb") as f:
        r = _Reader(f.read())
    if bytes(r.take(8)) != MAGIC:
        raise ValueError("not a MeshTool scene file")
    version = r.pod("<I")
    if not 1 <= version <= VERSION:
        raise ValueError(f"unsupported scene version {version}")
    batches = Scene()
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
                variant = r.pod("<B") if version >= 5 else 0
                if variant > 2: raise ValueError("invalid model variant")
                lod = r.pod("<f") if version >= 7 else 0.0
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
                meshes.append(Mesh(tex, obj, tr, verts, uvs, cols, dcs, mat, variant, lod))
            objects = []
            if version >= 3:
                for _ in range(r.pod("<I")):
                    name = bytes(r.take(r.pod("<I"))).decode("utf-8", "replace")
                    objects.append(SceneObject(name, r.pod("<B")))
            trees = []
            if version >= 8:
                size = r.pod('<I')
                if size > 512*1024*1024: raise ValueError('invalid tree metadata size')
                payload = bytes(r.take(size))
                trees = json.loads(payload.decode('utf-8')) if payload else []
                if not isinstance(trees, list): raise ValueError('invalid tree instance records')
            groups.append(Group(textures, meshes, objects, trees))
        batches.append(Batch(is_spline, is_dump, georef, ref, groups))
    if version >= 6 and r.p < len(r.d):
        start = r.p
        points = r.pod("<I")
        if points > (1 << 22): raise ValueError("invalid route point count")
        polyline = [(r.pod("<d"), r.pod("<d")) for _ in range(points)]
        if points:
            text = lambda: bytes(r.take(r.pod("<I"))).decode("utf-8", "replace")
            from_name, to_name = text(), text()
            ends = [r.pod("<d") for _ in range(4)]
            length = r.pod("<d")
            if route is not None:
                route.update(polyline=polyline, from_name=from_name, to_name=to_name,
                             **{"from": (ends[0], ends[1]), "to": (ends[2], ends[3])}, length_m=length)
        batches.route_data = bytes(r.d[start:r.p])
    if version >= 9:
        for key in ('id', 'name', 'created_utc'):
            size = r.pod('<I')
            if size > 4096: raise ValueError('invalid capture identity size')
            batches.capture[key] = bytes(r.take(size)).decode('utf-8')
        if not batches.capture['id'] or not batches.capture['name']:
            raise ValueError('invalid capture identity')
    return batches


def save(path, batches, route=None):
    """Write v9 with capture identity, v8 with trees, otherwise v7; retain route bytes."""
    out = bytearray()
    w = lambda fmt, *v: out.extend(struct.pack(fmt, *v))
    out += MAGIC
    version = 8 if any(g.tree_instances for b in batches for g in b.groups) else 7
    capture = getattr(batches, 'capture', {})
    if capture:
        if not capture.get('id') or not capture.get('name'): raise ValueError('invalid capture identity')
        version = 9
    w("<I", version)
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
                w("<IIiBBf", n, m.tex_index & 0xFFFFFFFF, m.object_id, m.material, m.model_variant, float(m.lod_size))
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
            if version >= 8:
                payload = json.dumps(g.tree_instances, ensure_ascii=False, allow_nan=False).encode('utf-8')
                w('<I', len(payload))
                out += payload
    if route is None:
        out += getattr(batches,'route_data',b'\x00\x00\x00\x00')
    else:
        # Version 6 trailer: the driving route (0 points = none), as scenefile.cpp writes it.
        polyline = list(route.get("polyline", [])) if route else []
        if len(polyline) < 2: polyline = []
        w("<I", len(polyline))
        if polyline:
            for lon, lat in polyline:
                w("<dd", float(lon), float(lat))
            for key in ("from_name", "to_name"):
                nb = str(route.get(key, "")).encode("utf-8")
                w("<I", len(nb))
                out += nb
            a, b = route.get("from", polyline[0]), route.get("to", polyline[-1])
            w("<dddd", float(a[0]), float(a[1]), float(b[0]), float(b[1]))
            w("<d", float(route.get("length_m", 0.0)))
    if version >= 9:
        for key in ('id', 'name', 'created_utc'):
            payload = capture.get(key, '').encode('utf-8')
            if len(payload) > 4096: raise ValueError('invalid capture identity size')
            w('<I', len(payload)); out += payload
    with open(path, "wb") as f:
        f.write(out)
