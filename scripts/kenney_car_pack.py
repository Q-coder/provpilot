#!/usr/bin/env python3
"""Pack a Kenney Car Kit model (CC0, www.kenney.nl) into one self-contained GLB.

The kit's .glb files reference Textures/colormap.png beside them; the vehicle
loader wants a single file. The image becomes a bufferView.

    python3 scripts/kenney_car_pack.py "<kit>/Models/GLB format/sedan.glb" provpilot/aircraft/car/model/car.glb
"""
import json
import struct
import sys
from pathlib import Path


def main(src, dst):
    src = Path(src)
    data = src.read_bytes()
    magic, _ver, _len = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67, "not a GLB"
    jlen, jtype = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + jlen])
    off = 20 + jlen
    blen, _btype = struct.unpack_from("<II", data, off)
    binary = bytearray(data[off + 8:off + 8 + blen])
    for img in gltf.get("images", []):
        uri = img.pop("uri", None)
        if uri is None:
            continue
        png = (src.parent / uri).read_bytes()
        while len(binary) % 4:
            binary.append(0)
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(png)})
        img["bufferView"] = len(gltf["bufferViews"]) - 1
        img["mimeType"] = "image/png"
        binary += png
    while len(binary) % 4:
        binary.append(0)
    gltf["buffers"][0]["byteLength"] = len(binary)
    gltf["asset"]["copyright"] = "Kenney (www.kenney.nl), Car Kit, CC0"
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(binary), 0x004E4942) + bytes(binary)
    Path(dst).write_bytes(out)
    print(f"{dst}: {len(out)} bytes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
