#!/usr/bin/env python3
"""Pack a multi-file glTF (.gltf + .bin + external PNG images) into a single
self-contained binary .glb. Embeds all external images as bufferViews.

Usage: python3 scripts/pack_glb.py <input.gltf> <output.glb>

Used to bundle Blender-exported aircraft models for ProvPilot when Godot's
GLTFDocument importer crashes in headless mode on this custom double build.
"""
import json
import os
import struct
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: pack_glb.py <input.gltf> <output.glb>")
        return 1
    in_path, out_path = sys.argv[1], sys.argv[2]
    base = os.path.dirname(in_path)

    with open(in_path) as f:
        g = json.load(f)

    # Load the single existing buffer (the .bin).
    buffers = g.get("buffers", [])
    if len(buffers) != 1:
        print("Expected exactly one buffer, got %d" % len(buffers))
        return 1
    bin_uri = buffers[0]["uri"]
    blob = bytearray(open(os.path.join(base, bin_uri), "rb").read())

    g.setdefault("bufferViews", [])

    def align4(b: bytearray) -> None:
        while len(b) % 4 != 0:
            b.append(0)

    # Embed each external image as a new bufferView appended to the blob.
    for img in g.get("images", []):
        uri = img.get("uri")
        if not uri:
            continue
        data = open(os.path.join(base, uri), "rb").read()
        align4(blob)
        offset = len(blob)
        blob += data
        ext = os.path.splitext(uri)[1].lower()
        img["mimeType"] = "image/jpeg" if ext in (".jpg", ".jpeg") else "image/png"
        g["bufferViews"].append({
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(data),
        })
        img["bufferView"] = len(g["bufferViews"]) - 1
        img.pop("uri", None)

    # All bufferViews now point at buffer 0; collapse to a single embedded buffer.
    align4(blob)
    g["buffers"] = [{"byteLength": len(blob)}]

    json_bytes = json.dumps(g, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "
    bin_chunk = bytes(blob)
    while len(bin_chunk) % 4 != 0:
        bin_chunk += b"\x00"

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_chunk)
    with open(out_path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, total))  # 'glTF', ver 2
        out.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))  # 'JSON'
        out.write(json_bytes)
        out.write(struct.pack("<II", len(bin_chunk), 0x004E4942))  # 'BIN\0'
        out.write(bin_chunk)

    print("Wrote %s (%d bytes, %d images embedded)" % (
        out_path, total, sum(1 for i in g.get("images", []) if "bufferView" in i)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
