#!/usr/bin/env python3
"""Inspect a GLB file's accessor types and mesh primitive extensions."""
import json, struct, sys

def parse_glb(path):
    data = open(path, 'rb').read()
    off = 12
    gltf = None
    while off < len(data):
        clen = struct.unpack_from('<I', data, off)[0]
        ctype = struct.unpack_from('<I', data, off+4)[0]
        if ctype == 0x4E4F534A:
            gltf = json.loads(data[off+8:off+8+clen])
        off += 8 + clen
        if off % 4:
            off += 4 - (off % 4)
    return gltf

path = sys.argv[1] if len(sys.argv) > 1 else 'scenery/custom/ps-airport-lspf-schmerlat/converted/Hangar_rechts.glb'
g = parse_glb(path)

# Check accessors
print('=== ACCESSORS ===')
for i, acc in enumerate(g.get('accessors', [])):
    bv = acc.get('bufferView', '?')
    bo = acc.get('byteOffset', 0)
    print(f'  [{i:3d}] type={acc["type"]:6s} comp={acc.get("componentType","?")} '
          f'count={acc.get("count","?")} bv={bv} byteOff={bo} sparse={"sparse" in acc}')
    if 'extensions' in acc:
        print(f'        extensions: {list(acc["extensions"].keys())}')

# Check bufferViews
print()
print('=== BUFFER VIEWS ===')
for i, bv in enumerate(g.get('bufferViews', [])):
    print(f'  [{i:3d}] offset={bv.get("byteOffset",0)} length={bv.get("byteLength","?")} '
          f'stride={bv.get("byteStride","none")} target={bv.get("target","none")}')

# Check meshes
print()
print('=== MESH PRIMITIVES ===')
for i, mesh in enumerate(g.get('meshes', [])):
    for j, prim in enumerate(mesh.get('primitives', [])):
        attrs = prim.get('attributes', {})
        ext = prim.get('extensions', {})
        idx = prim.get('indices', 'none')
        mode = prim.get('mode', 4)
        print(f'  mesh[{i}].prim[{j}] mode={mode} indices={idx} attrs={attrs}')
        if ext:
            for k, v in ext.items():
                print(f'    ext {k}: {json.dumps(v)[:200]}')

# Check extensions
print()
print('=== EXTENSIONS ===')
print('  used:', g.get('extensionsUsed', []))
print('  required:', g.get('extensionsRequired', []))

# Check for unusual accessor patterns
print()
print('=== ANALYSIS ===')
for i, mesh in enumerate(g.get('meshes', [])):
    for j, prim in enumerate(mesh.get('primitives', [])):
        attrs = prim.get('attributes', {})
        for attr_name, acc_idx in attrs.items():
            acc = g['accessors'][acc_idx]
            expected = {
                'POSITION': 'VEC3', 'NORMAL': 'VEC3', 'TANGENT': 'VEC4',
                'TEXCOORD_0': 'VEC2', 'TEXCOORD_1': 'VEC2',
                'COLOR_0': 'VEC4', 'JOINTS_0': 'VEC4', 'WEIGHTS_0': 'VEC4',
            }
            exp = expected.get(attr_name)
            actual = acc['type']
            if exp and actual != exp:
                print(f'  MISMATCH mesh[{i}].prim[{j}] {attr_name}: '
                      f'expected {exp}, got {actual} (accessor {acc_idx})')
