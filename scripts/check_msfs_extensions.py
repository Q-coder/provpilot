#!/usr/bin/env python3
"""Check original MSFS primitives for per-primitive extensions/data that we're stripping."""
import json, struct, sys
from pathlib import Path

GLTF_MAGIC = b'glTF'

def extract_glbs_from_bgl(bgl_path):
    data = Path(bgl_path).read_bytes()
    results = []
    offset = 0
    while offset < len(data) - 12:
        pos = data.find(GLTF_MAGIC, offset)
        if pos == -1:
            break
        magic, version, length = struct.unpack_from('<4sII', data, pos)
        if version == 2 and length > 12 and pos + length <= len(data):
            results.append((pos, data[pos:pos+length]))
        offset = pos + 4
    return results

def parse_glb(data):
    offset = 12
    json_chunk = None
    while offset < len(data):
        chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A:
            text = chunk_data.rstrip(b'\x00 \t\n\r')
            json_chunk = json.loads(text)
        offset += 8 + chunk_len
    return json_chunk

bgl_path = Path('scenery/custom/ps-airport-lspf-schmerlat/scenery/global/scenery/modelLib.BGL')
glbs = extract_glbs_from_bgl(bgl_path)

# Model 1 is the Hangar
gltf = parse_glb(glbs[1][1])

# Show ALL primitive details including extensions
meshes = gltf.get('meshes', [])
print(f"=== ORIGINAL MSFS Model 1 (Hangar) ===")
print(f"Extensions used: {gltf.get('extensionsUsed', [])}")
print(f"Extensions required: {gltf.get('extensionsRequired', [])}")

for i, mesh in enumerate(meshes[:5]):  # First 5 meshes
    prims = mesh.get('primitives', [])
    print(f"\nmesh[{i}] '{mesh.get('name', '')}': {len(prims)} prims")
    if mesh.get('extensions'):
        print(f"  mesh extensions: {json.dumps(mesh['extensions'], indent=4)}")
    if mesh.get('extras'):
        print(f"  mesh extras: {json.dumps(mesh['extras'], indent=4)}")
    
    for j, prim in enumerate(prims):
        print(f"  prim[{j}]:")
        print(f"    indices: acc[{prim.get('indices')}]")
        print(f"    material: {prim.get('material')}")
        print(f"    mode: {prim.get('mode', 4)}")
        attrs = prim.get('attributes', {})
        for k, v in sorted(attrs.items()):
            print(f"    {k}: acc[{v}]")
        if prim.get('extensions'):
            print(f"    EXTENSIONS: {json.dumps(prim['extensions'], indent=4)}")
        if prim.get('extras'):
            print(f"    EXTRAS: {json.dumps(prim['extras'], indent=4)}")
        if prim.get('targets'):
            print(f"    TARGETS: {prim['targets']}")

# Also check materials for MSFS extensions
print(f"\n\n=== Material Extensions ===")
for i, mat in enumerate(gltf.get('materials', [])[:15]):
    ext = mat.get('extensions', {})
    extras = mat.get('extras', {})
    if ext or extras:
        print(f"\nmat[{i}] '{mat.get('name', '')}': ")
        if ext:
            print(f"  extensions: {json.dumps(ext, indent=4)}")
        if extras:
            print(f"  extras: {json.dumps(extras, indent=4)}")

# Check root extensions
print(f"\n\n=== Root Extensions ===")
root_ext = gltf.get('extensions', {})
if root_ext:
    print(json.dumps(root_ext, indent=2))
