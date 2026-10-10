#!/usr/bin/env python3
"""convert_msfs_scenery.py — MSFS 2020 scenery package → Godot GLB models + placements.

    python3 scripts/convert_msfs_scenery.py <package dir | modelLib.BGL | model.gltf> \\
        --output <dir> [--max-texture 2048] [--only name,name] [--drop-vertex-colors]

What it does
  * Reads every model of the package's modelLib BGLs (RIFF "GLTF" forms: an XML
    ModelInfo with the GUID and name, then the glTF binary) and any loose
    .gltf/.bin source models.
  * Decodes Asobo's optimized vertex format: one interleaved buffer per model
    (POSITION float3, TANGENT/NORMAL as signed bytes, TEXCOORD_0/1 and COLOR_0
    as IEEE half floats), one shared index accessor per mesh that every
    primitive slices with extras.ASOBO_primitive {StartIndex, PrimitiveCount,
    BaseVertexIndex}. Triangles are rewound counter-clockwise about their vertex
    normals (MSFS front faces are clockwise; glTF's are counter-clockwise).
    Each mesh is rewritten with plain float32 attributes, compacted to the
    vertices its triangles use, and one index accessor per primitive.
  * Strips the ASOBO_* / MSFT_texture_dds extensions, converts the DDS textures
    (BC1/BC2/BC3/BC4/BC5, msfs_dds.py) to embedded PNGs, flips normal maps to
    the OpenGL convention Godot expects, keeps light nodes as empty nodes with
    their parameters in `extras.msfs_light`.
  * Parses the scenery placement BGLs (SceneryObject sections, LibraryObject
    records) and writes <output>/package.json with every placed model
    (lat/lon, LV95 E/N when pyproj is available, height above ground, heading,
    scale), an exclusion zone per placed model (its footprint disc: the
    buildings and trees of lower-priority scenery packs vanish there) and
    the GUIDs of objects that come from other libraries.

The output directory should be the `objects/` folder of a scenery library
area (`<area>/objects/package.json`): the sim then places the models and
applies the exclusions with the area's priority (docs/SCENERY_ARCHITECTURE.md).

Requires numpy (python3 on this Mac). Validate the result with
`scripts/glb_tool.py validate <dir>`.
"""
import argparse
import json
import os
import re
import struct
import sys
import uuid
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msfs_dds  # noqa: E402

GLTF_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
COMP_NP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
TYPE_N = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
INVISIBLE_MATERIAL_EXT = ("ASOBO_material_invisible", "ASOBO_material_environment_occluder", "ASOBO_material_fake_terrain")
BGL_MAGIC = 0x19920201
SECTION_SCENERY_OBJECT = 0x25
LIBRARY_OBJECT = 0x0B


# ── glTF binary helpers ──────────────────────────────────────────────────────

def parse_glb(blob: bytes) -> Tuple[dict, bytes]:
    magic, _ver, length = struct.unpack_from("<III", blob, 0)
    if magic != GLTF_MAGIC:
        raise ValueError("not a GLB")
    js, bin_ = None, b""
    off = 12
    while off + 8 <= length:
        clen, ctype = struct.unpack_from("<II", blob, off)
        chunk = blob[off + 8:off + 8 + clen]
        if ctype == CHUNK_JSON:
            js = json.loads(chunk.rstrip(b"\x00 "))
        elif ctype == CHUNK_BIN:
            bin_ = chunk
        off += 8 + clen
        off += (-off) % 4
    if js is None:
        raise ValueError("GLB without JSON chunk")
    return js, bin_


def build_glb(js: dict, bin_: bytes) -> bytes:
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((-len(jb)) % 4)
    bb = bytes(bin_) + b"\x00" * ((-len(bin_)) % 4)
    total = 12 + 8 + len(jb) + 8 + len(bb)
    return (struct.pack("<III", GLTF_MAGIC, 2, total) + struct.pack("<II", len(jb), CHUNK_JSON) + jb
            + struct.pack("<II", len(bb), CHUNK_BIN) + bb)


def read_accessor(js: dict, bin_: bytes, ai: int) -> np.ndarray:
    """Raw accessor values as (count, n) numpy array (interleaved buffers ok)."""
    a = js["accessors"][ai]
    bv = js["bufferViews"][a["bufferView"]]
    n = TYPE_N[a["type"]]
    dt = np.dtype(COMP_NP[a["componentType"]])
    elem = n * dt.itemsize
    stride = bv.get("byteStride", elem)
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    count = a["count"]
    raw = np.frombuffer(bin_, np.uint8, count * stride - (stride - elem), base)
    if stride == elem:
        return raw.view(dt).reshape(count, n)
    padded = np.zeros(count * stride, np.uint8)
    padded[:len(raw)] = raw
    return np.ascontiguousarray(padded.reshape(count, stride)[:, :elem]).view(dt).reshape(count, n)


def decode_attribute(name: str, a: dict, raw: np.ndarray) -> np.ndarray:
    """MSFS packed attribute -> float32 with the glTF meaning."""
    ct = a["componentType"]
    if ct == 5126:
        return raw.astype(np.float32)
    if ct in (5120, 5121):  # signed / unsigned bytes: normalized
        div = 127.0 if ct == 5120 else 255.0
        out = np.maximum(raw.astype(np.float32) / div, -1.0)
        if name == "NORMAL":
            out = out[:, :3]
            ln = np.linalg.norm(out, axis=1, keepdims=True)
            out = np.where(ln > 1e-6, out / np.maximum(ln, 1e-6), np.array([[0, 1, 0]], np.float32))
        elif name == "TANGENT":
            xyz = out[:, :3]
            ln = np.linalg.norm(xyz, axis=1, keepdims=True)
            xyz = np.where(ln > 1e-6, xyz / np.maximum(ln, 1e-6), np.array([[1, 0, 0]], np.float32))
            w = np.where(out[:, 3:4] < 0, -1.0, 1.0).astype(np.float32)
            out = np.concatenate([xyz, w], axis=1)
        return out.astype(np.float32)
    if ct in (5122, 5123):  # shorts: half floats for UVs and colours, normalized otherwise
        if name.startswith("TEXCOORD") or name.startswith("COLOR"):
            return raw.view(np.float16).astype(np.float32)
        div = 32767.0 if ct == 5122 else 65535.0
        return np.maximum(raw.astype(np.float32) / div, -1.0)
    return raw.astype(np.float32)


# ── BGL readers ──────────────────────────────────────────────────────────────

def iter_bgl_models(data: bytes):
    """(name, guid, glb bytes) for every RIFF GLTF form of a modelLib BGL."""
    off = 0
    while True:
        pos = data.find(b"RIFF", off)
        if pos < 0:
            return
        size = struct.unpack_from("<I", data, pos + 4)[0]
        form = data[pos + 8:pos + 12]
        end = pos + 8 + size
        if form == b"GLTF" and end <= len(data):
            name, guid, glb = None, None, None
            o = pos + 12
            while o + 8 <= end:
                cid = data[o:o + 4]
                csz = struct.unpack_from("<I", data, o + 4)[0]
                body = data[o + 8:o + 8 + csz]
                if cid == b"GXML":
                    m = re.search(rb'name="([^"]*)"', body)
                    g = re.search(rb'guid="\{([^}]*)\}"', body)
                    name = m.group(1).decode("utf-8", "replace") if m else None
                    guid = g.group(1).decode() if g else None
                elif cid == b"GLBD" and body[:4] == b"GLB\x00":
                    glen = struct.unpack_from("<I", body, 4)[0]
                    glb = body[8:8 + glen]
                o += 8 + csz + (csz & 1)
            if glb is not None:
                yield name or ("model_%08x" % pos), guid, glb
            off = end
        else:
            off = pos + 4


def iter_placements(data: bytes):
    """LibraryObject records of a scenery BGL: dicts with lat, lon, agl_m, heading_deg, scale, guid."""
    if len(data) < 0x38 or struct.unpack_from("<I", data, 0)[0] != BGL_MAGIC:
        return
    nsec = struct.unpack_from("<I", data, 0x14)[0]
    off = 0x38
    for _ in range(nsec):
        typ, flags, nsub, suboff, _subsize = struct.unpack_from("<IIIII", data, off)
        off += 20
        if typ != SECTION_SCENERY_OBJECT:
            continue
        ssz = ((flags & 0x10000) | 0x40000) >> 14
        for j in range(nsub):
            o = suboff + j * ssz
            if ssz == 16:
                _qmid, _nrec, roff, rsize = struct.unpack_from("<IIII", data, o)
            else:
                _q1, _q2, _nrec, roff, rsize = struct.unpack_from("<IIIII", data, o)
            p = roff
            while p + 4 <= roff + rsize:
                rid, rsz = struct.unpack_from("<HH", data, p)
                if rsz < 4:
                    break
                if rid == LIBRARY_OBJECT and rsz >= 64:
                    lon, lat, alt = struct.unpack_from("<IIi", data, p + 4)  # altitude is signed: sunk objects are negative
                    flags1, pitch, bank, hdg = struct.unpack_from("<HHHH", data, p + 16)
                    guid = uuid.UUID(bytes_le=data[p + 44:p + 60])
                    scale = struct.unpack_from("<f", data, p + 60)[0]
                    yield {
                        "lat": 90.0 - lat * (180.0 / (2 * 0x10000000)),
                        "lon": lon * (360.0 / (3 * 0x10000000)) - 180.0,
                        "agl_m": alt / 1000.0 if flags1 & 1 else None,
                        "elevation_m": None if flags1 & 1 else alt / 1000.0,
                        "heading_deg": hdg * 360.0 / 65536.0,
                        "pitch_deg": pitch * 360.0 / 65536.0 if pitch not in (0, 0xFFFF) else 0.0,
                        "scale": scale if 0.001 < scale < 1000.0 else 1.0,
                        "guid": str(guid),
                    }
                p += rsz


# ── textures ─────────────────────────────────────────────────────────────────

class TextureStore:
    def __init__(self, dirs: List[Path], max_size: int):
        self.files: Dict[str, Path] = {}
        for d in dirs:
            if d.is_dir():
                for f in d.iterdir():
                    if f.is_file():
                        self.files.setdefault(f.name.lower(), f)
        self.max_size = max_size
        self.cache: Dict[Tuple[str, bool], Optional[Tuple[bytes, str]]] = {}
        self.warnings: List[str] = []

    def resolve(self, uri: str) -> Optional[Path]:
        base = os.path.basename(uri)
        stem = base.rsplit(".", 1)[0] if "." in base else base
        for cand in (base, base + ".dds", stem + ".png.dds", stem + ".jpg.dds", stem + ".dds", stem + ".png", stem + ".jpg"):
            f = self.files.get(cand.lower())
            if f:
                return f
        return None

    def load(self, uri: str, is_normal: bool) -> Optional[Tuple[bytes, str]]:
        key = (uri.lower(), is_normal)
        if key in self.cache:
            return self.cache[key]
        f = self.resolve(uri)
        result = None
        if f is None:
            self.warnings.append("texture not found: %s" % uri)
        else:
            data = f.read_bytes()
            try:
                if data[:4] == b"DDS ":
                    rgba, fmt = msfs_dds.decode_dds(data)
                    if is_normal:
                        rgba = msfs_dds.normal_map_to_opengl(rgba, fmt in ("BC5", "BC5S"))
                    rgba = msfs_dds.downscale(rgba, self.max_size)
                    result = (msfs_dds.png_bytes(rgba), "image/png")
                elif data[:8] == b"\x89PNG\r\n\x1a\n":
                    result = (data, "image/png")
                elif data[:2] == b"\xff\xd8":
                    result = (data, "image/jpeg")
                else:
                    self.warnings.append("unknown image format: %s" % f.name)
            except msfs_dds.DdsError as e:
                self.warnings.append("%s: %s" % (f.name, e))
        self.cache[key] = result
        return result


# ── model conversion ─────────────────────────────────────────────────────────

class Builder:
    """Collects bufferViews / accessors / images of the output GLB."""

    def __init__(self):
        self.bin = bytearray()
        self.views: List[dict] = []
        self.accessors: List[dict] = []
        self.images: List[dict] = []
        self.textures: List[dict] = []
        self.image_index: Dict[Tuple[str, bool], int] = {}
        self.texture_index: Dict[Tuple[int, int], int] = {}

    def add_view(self, data: bytes, target: Optional[int] = None) -> int:
        self.bin += b"\x00" * ((-len(self.bin)) % 4)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.views.append(view)
        return len(self.views) - 1

    def add_accessor(self, arr: np.ndarray, ctype: int, atype: str, target: int, minmax: bool = False) -> int:
        vi = self.add_view(np.ascontiguousarray(arr).tobytes(), target)
        acc = {"bufferView": vi, "componentType": ctype, "count": int(arr.shape[0]), "type": atype}
        if minmax:
            acc["min"] = [float(x) for x in arr.min(axis=0)]
            acc["max"] = [float(x) for x in arr.max(axis=0)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def add_image(self, uri: str, is_normal: bool, store: TextureStore) -> Optional[int]:
        key = (uri.lower(), is_normal)
        if key in self.image_index:
            return self.image_index[key]
        loaded = store.load(uri, is_normal)
        if loaded is None:
            self.image_index[key] = None
            return None
        data, mime = loaded
        vi = self.add_view(data)
        self.images.append({"name": os.path.basename(uri), "mimeType": mime, "bufferView": vi})
        self.image_index[key] = len(self.images) - 1
        return self.image_index[key]

    def add_texture(self, image: int, sampler: int) -> int:
        key = (image, sampler)
        if key not in self.texture_index:
            self.textures.append({"source": image, "sampler": sampler})
            self.texture_index[key] = len(self.textures) - 1
        return self.texture_index[key]


def source_image_uri(js: dict, tex_index: int) -> Optional[str]:
    tex = js.get("textures", [])[tex_index]
    src = tex.get("source")
    if src is None:
        src = tex.get("extensions", {}).get("MSFT_texture_dds", {}).get("source")
    if src is None:
        return None
    return js.get("images", [])[src].get("uri")


def convert_material(mat: dict, js: dict, b: Builder, store: TextureStore, report: dict) -> dict:
    out = {"name": mat.get("name", "material")}
    for k in ("alphaMode", "alphaCutoff", "doubleSided", "emissiveFactor"):
        if k in mat:
            out[k] = mat[k]
    pbr_in = mat.get("pbrMetallicRoughness", {})
    pbr = {}
    for k in ("baseColorFactor", "metallicFactor", "roughnessFactor"):
        if k in pbr_in:
            pbr[k] = pbr_in[k]

    def tex_info(info: dict, is_normal: bool) -> Optional[dict]:
        uri = source_image_uri(js, info["index"])
        if uri is None:
            return None
        img = b.add_image(uri, is_normal, store)
        if img is None:
            return None
        out_info = {"index": b.add_texture(img, 0)}
        if info.get("texCoord"):
            out_info["texCoord"] = info["texCoord"]
        if "scale" in info:
            out_info["scale"] = info["scale"]
        if "strength" in info:
            out_info["strength"] = info["strength"]
        report["textures"].add(uri)
        return out_info

    for k in ("baseColorTexture", "metallicRoughnessTexture"):
        if k in pbr_in:
            ti = tex_info(pbr_in[k], False)
            if ti:
                pbr[k] = ti
    if pbr:
        out["pbrMetallicRoughness"] = pbr
    for k in ("normalTexture", "occlusionTexture", "emissiveTexture"):
        if k in mat:
            ti = tex_info(mat[k], k == "normalTexture")
            if ti:
                out[k] = ti
    # a base-colour texture with alpha but no alpha mode: MSFS treats it as opaque, so does glTF
    return out


def convert_model(js: dict, bin_: bytes, store: TextureStore, drop_vertex_colors: bool,
                  keep_nonuniform_scale: bool = False) -> Tuple[dict, bytes, dict]:
    report = {"meshes": 0, "primitives": 0, "triangles": 0, "vertices": 0, "textures": set(), "warnings": [], "nonuniform_nodes": 0}
    b = Builder()
    invisible = {i for i, m in enumerate(js.get("materials", []))
                 if any(e in m.get("extensions", {}) for e in INVISIBLE_MATERIAL_EXT)}
    materials = [convert_material(m, js, b, store, report) for m in js.get("materials", [])]
    uses_uv1 = any(info.get("texCoord") == 1 for m in js.get("materials", [])
                   for info in [m.get("normalTexture", {}), m.get("occlusionTexture", {}), m.get("emissiveTexture", {}),
                                m.get("pbrMetallicRoughness", {}).get("baseColorTexture", {}),
                                m.get("pbrMetallicRoughness", {}).get("metallicRoughnessTexture", {})])
    meshes_out = []
    for mesh in js.get("meshes", []):
        prims = mesh.get("primitives", [])
        if not prims:
            meshes_out.append({"name": mesh.get("name", ""), "primitives": []})
            continue
        # every primitive of a mesh shares the vertex accessors and the index accessor
        attrs0 = prims[0]["attributes"]
        idx_acc = js["accessors"][prims[0]["indices"]]
        all_idx = read_accessor(js, bin_, prims[0]["indices"]).reshape(-1).astype(np.uint32)
        slices = []
        for p in prims:
            if p.get("mode", 4) != 4 or p.get("material") in invisible:
                continue
            if p["attributes"] != attrs0 or p["indices"] != prims[0]["indices"]:
                # a primitive with its own accessors: read it separately
                own = read_accessor(js, bin_, p["indices"]).reshape(-1).astype(np.uint32)
                slices.append((p, own, p["attributes"]))
                continue
            ex = p.get("extras", {}).get("ASOBO_primitive", {})
            start = int(ex.get("StartIndex", 0))
            count = int(ex.get("PrimitiveCount", (idx_acc["count"] - start) // 3)) * 3
            # a mesh with more than 65535 vertices keeps 16-bit indices and
            # offsets them per primitive (Kjeller's club house: the walls were
            # drawn with the wrong vertices without this)
            base = int(ex.get("BaseVertexIndex", 0))
            slices.append((p, all_idx[start:start + count] + base, attrs0))
        prims_out = []
        groups: Dict[Tuple[Tuple[str, int], ...], List[Tuple[dict, np.ndarray]]] = {}
        for p, ids, attrs in slices:
            groups.setdefault(tuple(sorted(attrs.items())), []).append((p, ids))
        for attrs_key, members in groups.items():
            attrs = dict(attrs_key)
            used = np.unique(np.concatenate([ids for _, ids in members]))
            nverts = js["accessors"][attrs["POSITION"]]["count"]
            used = used[used < nverts]
            remap = np.full(nverts, -1, np.int64)
            remap[used] = np.arange(len(used))
            new_attrs = {}
            pos_vals = nrm_vals = None
            for name, ai in attrs.items():
                if name == "TEXCOORD_1" and not uses_uv1:
                    continue
                if name.startswith("COLOR") and drop_vertex_colors:
                    continue
                raw = read_accessor(js, bin_, ai)[used]
                vals = decode_attribute(name, js["accessors"][ai], raw)
                if name == "POSITION":
                    pos_vals = vals.astype(np.float64)
                elif name == "NORMAL":
                    nrm_vals = vals.astype(np.float64)
                if name.startswith("COLOR"):
                    if np.all(np.abs(vals - 1.0) < 1e-3):
                        continue  # white everywhere: no tint
                    vals = np.clip(vals, 0.0, 1.0)
                atype = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[vals.shape[1]]
                new_attrs[name] = b.add_accessor(vals.astype(np.float32), 5126, atype, 34962, minmax=(name == "POSITION"))
            report["vertices"] += len(used)
            for p, ids in members:
                tri = remap[ids]
                if tri.size == 0 or (tri < 0).any():
                    continue
                tri = fix_winding(tri, pos_vals, nrm_vals, report)
                if len(used) < 65536:
                    ia = b.add_accessor(tri.astype(np.uint16), 5123, "SCALAR", 34963)
                else:
                    ia = b.add_accessor(tri.astype(np.uint32), 5125, "SCALAR", 34963)
                po = {"attributes": new_attrs, "indices": ia, "mode": 4}
                if "material" in p:
                    po["material"] = p["material"]
                prims_out.append(po)
                report["primitives"] += 1
                report["triangles"] += tri.size // 3
        meshes_out.append({"name": mesh.get("name", ""), "primitives": prims_out})
        report["meshes"] += 1
    nodes_out = []
    for n in js.get("nodes", []):
        no = {}
        for k in ("name", "translation", "rotation", "scale", "matrix", "children", "mesh"):
            if k in n:
                no[k] = n[k]
        if "scale" in no and not keep_nonuniform_scale:
            # MSFS has no non-uniform node scale: the sim (and ModelConverterX)
            # apply the mean of the three components uniformly. Verified on
            # Schmerlat: every single-material node matches the MCX export to
            # the millimetre with this rule, and the literal glTF scale gives
            # absurd shapes (a 2 mm flagpole, a 37 m wide 5 m tall hangar).
            k = float(sum(no["scale"]) / 3.0)
            if any(abs(c - k) > 1e-6 for c in no["scale"]):
                report["nonuniform_nodes"] += 1
            no["scale"] = [k, k, k]
        light = n.get("extensions", {}).get("ASOBO_macro_light")
        if light:
            no["extras"] = {"msfs_light": light}
        if "mesh" in no and not meshes_out[no["mesh"]]["primitives"]:
            del no["mesh"]
        nodes_out.append(no)
    b.bin += b"\x00" * ((-len(b.bin)) % 4)
    out = {
        "asset": {"version": "2.0", "generator": "ProVPilot convert_msfs_scenery.py"},
        "scene": js.get("scene", 0),
        "scenes": js.get("scenes", [{"nodes": list(range(len(nodes_out)))}]),
        "nodes": nodes_out,
        "meshes": meshes_out,
        "materials": materials,
        "accessors": b.accessors,
        "bufferViews": b.views,
        "buffers": [{"byteLength": len(b.bin)}],
    }
    if b.images:
        out["images"] = b.images
        out["textures"] = b.textures
        out["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
    report["warnings"] = list(store.warnings)
    store.warnings.clear()
    report["footprint_radius_m"] = footprint_radius(out, b)
    return out, bytes(b.bin), report


def fix_winding(tri: np.ndarray, pos: Optional[np.ndarray], nrm: Optional[np.ndarray], report: dict) -> np.ndarray:
    """Wind every triangle counter-clockwise about its vertex normals.

    MSFS stores clockwise front faces (DirectX convention, like its normal
    maps): all but one of the 60 Schmerlat and Kjeller models have their
    triangles wound against their vertex normals. glTF front faces are
    counter-clockwise, so a single-sided material got its outside culled and
    its inside drawn (Kjeller's McDonald's: no roof from above, the walls
    seen from within, mirrored lettering). The vertex normals are the
    authority for which side is out: flip every triangle whose face normal
    opposes them; a degenerate triangle follows the primitive's majority.
    """
    t3 = tri.reshape(-1, 3)
    if pos is None or nrm is None or len(t3) == 0:
        return tri
    p = pos[t3]
    fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    d = (fn * nrm[t3].mean(axis=1)).sum(axis=1)
    flip = d < 0
    undecided = d == 0
    if undecided.any():
        flip = flip | (undecided & (flip.sum() > (~flip & ~undecided).sum()))
    if flip.any():
        t3 = t3.copy()
        t3[flip] = t3[flip][:, [0, 2, 1]]
        report["flipped"] = report.get("flipped", 0) + int(flip.sum())
    return t3.reshape(-1)


def quat_matrix(q) -> np.ndarray:
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def footprint_radius(js: dict, b: "Builder") -> float:
    """Largest horizontal (XZ) distance of a vertex from the model origin, metres."""
    best = 0.0
    for n in js.get("nodes", []):
        if "mesh" not in n:
            continue
        s = np.array(n.get("scale", [1, 1, 1]), float)
        r = quat_matrix(n.get("rotation", [0, 0, 0, 1]))
        t = np.array(n.get("translation", [0, 0, 0]), float)
        for p in js["meshes"][n["mesh"]]["primitives"]:
            acc = js["accessors"][p["attributes"]["POSITION"]]
            lo, hi = np.array(acc["min"]), np.array(acc["max"])
            corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
            w = (corners * s) @ r.T + t
            best = max(best, float(np.sqrt(w[:, 0] ** 2 + w[:, 2] ** 2).max()))
    return best


# ── driver ───────────────────────────────────────────────────────────────────

def texture_dirs_for(package: Path, model_path: Path) -> List[Path]:
    dirs = [model_path.parent / "texture", model_path.parent.parent / "texture", model_path.parent]
    if package.is_dir():
        for d in package.rglob("*"):
            if d.is_dir() and d.name.lower() in ("texture", "textures"):
                dirs.append(d)
    seen, out = set(), []
    for d in dirs:
        r = d.resolve()
        if r not in seen and d.is_dir():
            seen.add(r)
            out.append(d)
    return out


def lv95_converter():
    try:
        from pyproj import Transformer
        t = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
        return lambda lat, lon: t.transform(lon, lat)
    except Exception:  # pyproj missing: E/N stay empty
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="MSFS package directory, a modelLib .BGL or a source .gltf")
    ap.add_argument("--output", "-o", required=True, help="output directory (GLBs + package.json)")
    ap.add_argument("--max-texture", type=int, default=2048, help="downscale textures larger than this (default 2048)")
    ap.add_argument("--only", default="", help="comma-separated model names to convert")
    ap.add_argument("--drop-vertex-colors", action="store_true", help="never write COLOR_0")
    ap.add_argument("--exclusion-margin", type=float, default=3.0,
                    help="metres added to each model's footprint radius for its exclusion zone (default 3)")
    ap.add_argument("--exclusion-max-radius", type=float, default=120.0,
                    help="a model with a larger footprint radius gets no automatic exclusion zone (default 120): such "
                         "models are sparse detail sets spread over a whole airport, and a disc around them would wipe "
                         "out every building of the lower tiers for kilometres (FSDT Zurich: the town of Kloten)")
    ap.add_argument("--keep-nonuniform-scale", action="store_true",
                    help="keep node scales as stored instead of the uniform mean MSFS applies")
    args = ap.parse_args()
    src = Path(args.input)
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    only = {s.strip() for s in args.only.split(",") if s.strip()}
    package = src if src.is_dir() else src.parent

    models: List[Tuple[str, Optional[str], dict, bytes, Path]] = []
    bgl_files: List[Path] = []
    gltf_files: List[Path] = []
    if src.is_dir():
        bgl_files = sorted(p for p in src.rglob("*") if p.suffix.lower() == ".bgl")
        gltf_files = sorted(p for p in src.rglob("*.gltf") if not is_converter_output(p))
    elif src.suffix.lower() == ".bgl":
        bgl_files = [src]
    elif src.suffix.lower() == ".gltf":
        gltf_files = [src]
    placements = []
    for f in bgl_files:
        data = f.read_bytes()
        n_models = 0
        for name, guid, glb in iter_bgl_models(data):
            js, bin_ = parse_glb(glb)
            models.append((name, guid, js, bin_, f))
            n_models += 1
        n_placed = 0
        for rec in iter_placements(data):
            placements.append(rec)
            n_placed += 1
        print("%s: %d models, %d placements" % (f.relative_to(package) if f.is_relative_to(package) else f, n_models, n_placed))
    for f in gltf_files:
        js = json.loads(f.read_text(encoding="utf-8"))
        if js.get("asset", {}).get("generator", "").startswith("ProVPilot"):
            continue
        uri = js.get("buffers", [{}])[0].get("uri")
        bin_ = (f.parent / uri).read_bytes() if uri and (f.parent / uri).exists() else b""
        models.append((f.stem, None, js, bin_, f))

    to_lv95 = lv95_converter()
    guid_names: Dict[str, str] = {}
    radii: Dict[str, float] = {}
    written = []
    for name, guid, js, bin_, path in models:
        if guid:
            guid_names[guid.lower()] = name
        if only and name not in only:
            continue
        store = TextureStore(texture_dirs_for(package, path), args.max_texture)
        out_js, out_bin, rep = convert_model(js, bin_, store, args.drop_vertex_colors, args.keep_nonuniform_scale)
        glb_path = out_dir / (safe_name(name) + ".glb")
        glb_path.write_bytes(build_glb(out_js, out_bin))
        written.append(name)
        radii[name] = rep["footprint_radius_m"]
        print("  %-32s %3d meshes %4d prims %8d tris %8d verts %2d textures %2d scale-fixed nodes %6d rewound -> %s (%.1f MB)" % (
            name, rep["meshes"], rep["primitives"], rep["triangles"], rep["vertices"], len(rep["textures"]),
            rep["nonuniform_nodes"], rep.get("flipped", 0), glb_path.name, glb_path.stat().st_size / 1e6))
        for w in rep["warnings"]:
            print("     ! " + w)

    objects, unresolved = [], {}
    for rec in placements:
        nm = guid_names.get(rec["guid"].lower())
        if nm is None:
            unresolved[rec["guid"]] = unresolved.get(rec["guid"], 0) + 1
            continue
        obj = {"model": safe_name(nm) + ".glb", "name": nm, "lat": round(rec["lat"], 7), "lon": round(rec["lon"], 7),
               "agl_m": rec["agl_m"], "elevation_m": rec["elevation_m"], "heading_deg": round(rec["heading_deg"], 2),
               "pitch_deg": round(rec["pitch_deg"], 2), "scale": round(rec["scale"], 4)}
        obj["footprint_radius_m"] = round(radii.get(nm, 0.0) * obj["scale"], 2)
        if to_lv95:
            e, n = to_lv95(rec["lat"], rec["lon"])
            obj["position_e"] = round(e, 2)
            obj["position_n"] = round(n, 2)
        objects.append(obj)
    # Exclusion zones: the buildings and trees of lower-priority packs are
    # removed under every placed model (footprint disc + margin). Edit by
    # hand for a whole airfield perimeter: {"kinds": "...", "polygon": [[e, n], ...]}.
    # A model wider than --exclusion-max-radius is a sparse detail set (apron
    # lamps, people, signs over a whole airport): it gets exclusion_radius_m 0,
    # which the sim honours when it rewrites the pack, and a note below.
    exclusions = []
    sparse = set()
    for obj in objects:
        if "position_e" not in obj or obj["footprint_radius_m"] <= 0.5:
            continue
        if obj["footprint_radius_m"] > args.exclusion_max_radius:
            obj["exclusion_radius_m"] = 0
            sparse.add(obj["name"])
            continue
        exclusions.append({"kinds": "buildings,vegetation,objects",
                           "circle": [obj["position_e"], obj["position_n"], round(obj["footprint_radius_m"] + args.exclusion_margin, 1)],
                           "object": obj["name"]})
    if sparse:
        print("no exclusion zone (footprint over %.0f m, a sparse detail model; set exclusion_radius_m in package.json "
              "or draw a polygon by hand): %s" % (args.exclusion_max_radius, ", ".join(sorted(sparse))))
    if placements or objects:
        lat0 = sum(o["lat"] for o in objects) / len(objects) if objects else None
        lon0 = sum(o["lon"] for o in objects) / len(objects) if objects else None
        pkg = {
            "schema_version": 2,
            "name": package.name,
            "source": "MSFS package %s, converted by scripts/convert_msfs_scenery.py" % package.name,
            "notes": ("lat/lon WGS84; position_e/n LV95 (EPSG:2056); agl_m = height above ground where the "
                      "placement said so, else elevation_m is absolute; heading_deg true, clockwise; the models are "
                      "glTF Y-up; the model's +Z faces the heading (checked against the LSPF orthophoto)"),
            "origin": {"lat": lat0, "lon": lon0},
            "objects": objects,
            "exclusions": exclusions,
            "unresolved_library_objects": [{"guid": g, "count": c} for g, c in sorted(unresolved.items(), key=lambda x: -x[1])],
        }
        if to_lv95 and objects:
            e, n = to_lv95(lat0, lon0)
            pkg["origin_e"] = round(e, 1)
            pkg["origin_n"] = round(n, 1)
        (out_dir / "package.json").write_text(json.dumps(pkg, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print("package.json: %d placed objects of this package, %d placements of other libraries (%d GUIDs)" % (
            len(objects), sum(unresolved.values()), len(unresolved)))
    print("%d models written to %s" % (len(written), out_dir))
    return 0


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or "model"


def is_converter_output(p: Path) -> bool:
    try:
        head = p.open("rb").read(4096)
        return b"ProVPilot convert_msfs_scenery" in head or b"ModelConverterX" in head
    except OSError:
        return False


if __name__ == "__main__":
    sys.exit(main())
