#!/usr/bin/env python3
"""glb_tool.py — small GLB utilities for the aircraft and scenery models.

    python3 scripts/glb_tool.py pack <input.gltf> <output.glb>
        Pack a multi-file glTF (.gltf + .bin + external images) into one
        self-contained .glb: every external image becomes a bufferView.
        Used to bundle Blender / assimp exported aircraft models when Godot's
        GLTFDocument importer cannot run headless on the custom double build.

    python3 scripts/glb_tool.py inspect <file.glb>
        Print accessors, bufferViews, mesh primitives and extensions, and
        flag attribute accessors whose type does not match the glTF spec.

    python3 scripts/glb_tool.py validate <file.glb | directory>
        Check every accessor against its bufferView and the binary chunk
        (size mismatches are what make Godot reject a file).
"""
import json
import os
import struct
import sys
from pathlib import Path

MAGIC_GLTF = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
COMP_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
TYPE_N = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}
EXPECTED_ATTR = {
    "POSITION": "VEC3", "NORMAL": "VEC3", "TANGENT": "VEC4",
    "TEXCOORD_0": "VEC2", "TEXCOORD_1": "VEC2",
    "COLOR_0": "VEC4", "JOINTS_0": "VEC4", "WEIGHTS_0": "VEC4",
}


def read_glb(path):
    """(gltf dict, binary chunk bytes) of a .glb file."""
    data = Path(path).read_bytes()
    magic, _version, length = struct.unpack_from("<III", data, 0)
    if magic != MAGIC_GLTF:
        raise ValueError(f"{path}: not a GLB (magic {magic:#x})")
    gltf, blob = None, b""
    off = 12
    while off + 8 <= min(length, len(data)):
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8:off + 8 + clen]
        if ctype == CHUNK_JSON:
            gltf = json.loads(chunk.rstrip(b"\x00 "))
        elif ctype == CHUNK_BIN:
            blob = chunk
        off += 8 + clen
        if off % 4:
            off += 4 - (off % 4)
    if gltf is None:
        raise ValueError(f"{path}: no JSON chunk")
    return gltf, blob


def write_glb(path, gltf, blob):
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "
    blob = bytes(blob)
    while len(blob) % 4:
        blob += b"\x00"
    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", MAGIC_GLTF, 2, total))
        out.write(struct.pack("<II", len(json_bytes), CHUNK_JSON))
        out.write(json_bytes)
        out.write(struct.pack("<II", len(blob), CHUNK_BIN))
        out.write(blob)
    return total


# ── pack ─────────────────────────────────────────────────────────────────────

def cmd_pack(in_path, out_path):
    base = os.path.dirname(in_path)
    with open(in_path) as f:
        g = json.load(f)
    buffers = g.get("buffers", [])
    if len(buffers) != 1:
        print(f"Expected exactly one buffer, got {len(buffers)}")
        return 1
    blob = bytearray(open(os.path.join(base, buffers[0]["uri"]), "rb").read())
    g.setdefault("bufferViews", [])

    def align4():
        while len(blob) % 4:
            blob.append(0)

    embedded = 0
    for img in g.get("images", []):
        uri = img.get("uri")
        if not uri:
            continue
        data = open(os.path.join(base, uri), "rb").read()
        align4()
        g["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob += data
        ext = os.path.splitext(uri)[1].lower()
        img["mimeType"] = "image/jpeg" if ext in (".jpg", ".jpeg") else "image/png"
        img["bufferView"] = len(g["bufferViews"]) - 1
        img.pop("uri", None)
        embedded += 1
    align4()
    g["buffers"] = [{"byteLength": len(blob)}]
    total = write_glb(out_path, g, blob)
    print(f"Wrote {out_path} ({total} bytes, {embedded} images embedded)")
    return 0


# ── inspect ──────────────────────────────────────────────────────────────────

def cmd_inspect(path):
    g, _blob = read_glb(path)
    print("=== ACCESSORS ===")
    for i, acc in enumerate(g.get("accessors", [])):
        print(f'  [{i:3d}] type={acc["type"]:6s} comp={acc.get("componentType", "?")} '
              f'count={acc.get("count", "?")} bv={acc.get("bufferView", "?")} '
              f'byteOff={acc.get("byteOffset", 0)} sparse={"sparse" in acc}')
        if "extensions" in acc:
            print(f'        extensions: {list(acc["extensions"].keys())}')
    print("\n=== BUFFER VIEWS ===")
    for i, bv in enumerate(g.get("bufferViews", [])):
        print(f'  [{i:3d}] offset={bv.get("byteOffset", 0)} length={bv.get("byteLength", "?")} '
              f'stride={bv.get("byteStride", "none")} target={bv.get("target", "none")}')
    print("\n=== MESH PRIMITIVES ===")
    mismatches = []
    for i, mesh in enumerate(g.get("meshes", [])):
        for j, prim in enumerate(mesh.get("primitives", [])):
            attrs = prim.get("attributes", {})
            print(f'  mesh[{i}].prim[{j}] mode={prim.get("mode", 4)} indices={prim.get("indices", "none")} attrs={attrs}')
            for k, v in prim.get("extensions", {}).items():
                print(f"    ext {k}: {json.dumps(v)[:200]}")
            for name, acc_idx in attrs.items():
                exp = EXPECTED_ATTR.get(name)
                actual = g["accessors"][acc_idx]["type"]
                if exp and actual != exp:
                    mismatches.append(f"  MISMATCH mesh[{i}].prim[{j}] {name}: expected {exp}, got {actual} (accessor {acc_idx})")
    print("\n=== EXTENSIONS ===")
    print("  used:", g.get("extensionsUsed", []))
    print("  required:", g.get("extensionsRequired", []))
    print("\n=== ANALYSIS ===")
    for m in mismatches:
        print(m)
    if not mismatches:
        print("  attribute accessor types match the spec")
    return 0


# ── validate ─────────────────────────────────────────────────────────────────

def validate_glb(path):
    gltf, blob = read_glb(path)
    accessors = gltf.get("accessors", [])
    bvs = gltf.get("bufferViews", [])
    bufs = gltf.get("buffers", [])
    errors = []
    if bufs and bufs[0].get("byteLength") != len(blob):
        errors.append(f"  buffer[0].byteLength={bufs[0].get('byteLength')} != actual bin_size={len(blob)}")
    for i, acc in enumerate(accessors):
        bv_idx = acc.get("bufferView")
        if bv_idx is None:
            continue
        if bv_idx >= len(bvs):
            errors.append(f"  acc[{i}]: bufferView={bv_idx} out of range (only {len(bvs)} bvs)")
            continue
        bv = bvs[bv_idx]
        elem = COMP_SIZE.get(acc["componentType"], 4) * TYPE_N.get(acc["type"], 1)
        count = acc["count"]
        acc_offset = acc.get("byteOffset", 0)
        stride = bv.get("byteStride", elem)
        required = (count - 1) * stride + elem + acc_offset if count > 0 else 0
        bv_length = bv["byteLength"]
        bv_offset = bv.get("byteOffset", 0)
        if required > bv_length:
            errors.append(f'  acc[{i}] type={acc["type"]} comp={acc["componentType"]} count={count} bv={bv_idx} '
                          f"acc_off={acc_offset}: needs {required} but bv has {bv_length} (bv_off={bv_offset}, stride={stride})")
        if bv_offset + bv_length > len(blob):
            errors.append(f"  acc[{i}] bv[{bv_idx}]: bv_offset({bv_offset})+bv_length({bv_length})={bv_offset + bv_length} > bin_size({len(blob)})")
    return errors


def cmd_validate(target):
    target = Path(target)
    paths = [target] if target.is_file() else sorted(target.glob("*.glb"))
    total = 0
    for p in paths:
        errors = validate_glb(p)
        if errors:
            print(f"ERRORS in {p.name}:")
            print("\n".join(errors))
            total += len(errors)
        else:
            print(f"OK: {p.name}")
    print(f"\n{total} errors found" if total else f"\nAll {len(paths)} files OK")
    return 1 if total else 0


def main(argv):
    if len(argv) >= 4 and argv[1] == "pack":
        return cmd_pack(argv[2], argv[3])
    if len(argv) >= 3 and argv[1] == "inspect":
        return cmd_inspect(argv[2])
    if len(argv) >= 2 and argv[1] == "validate":
        return cmd_validate(argv[2] if len(argv) > 2 else ".")
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
