#!/usr/bin/env python3
"""Compare original vs converted vertex data to find corruption."""
import json, struct, sys
sys.path.insert(0, '.')
from pathlib import Path
from scripts.convert_msfs_scenery import extract_glbs_from_bgl, parse_glb, convert_glb, _half_to_float

bgl_path = Path('scenery/custom/ps-airport-lspf-schmerlat/scenery/global/scenery/modelLib.BGL')
glbs = extract_glbs_from_bgl(bgl_path)

# Use model 0 (simplest: 4 meshes, fence/bar)
offset, glb_data = glbs[0]
orig_json, orig_bin = parse_glb(glb_data)

import copy
conv_json = copy.deepcopy(orig_json)
conv_bin = bytes(orig_bin)

# Run conversion (without real textures, just the vertex conversion)
conv_json, conv_bin = convert_glb(conv_json, conv_bin, Path('.'), 0)

print("=== ORIGINAL ===")
orig_bvs = orig_json.get('bufferViews', [])
orig_accs = orig_json.get('accessors', [])
print(f"Accessors: {len(orig_accs)}, BVs: {len(orig_bvs)}")

print("\n=== CONVERTED ===")
conv_bvs = conv_json.get('bufferViews', [])
conv_accs = conv_json.get('accessors', [])
print(f"Accessors: {len(conv_accs)}, BVs: {len(conv_bvs)}")

# Build mesh primitive map for converted
meshes = conv_json.get('meshes', [])
print(f"\nMeshes: {len(meshes)}")
for mi, mesh in enumerate(meshes):
    print(f"\n  Mesh {mi}: {mesh.get('name', 'unnamed')}")
    for pi, prim in enumerate(mesh.get('primitives', [])):
        print(f"    Primitive {pi}:")
        for attr, ai in prim.get('attributes', {}).items():
            a = conv_accs[ai]
            bvi = a['bufferView']
            bv = conv_bvs[bvi]
            print(f"      {attr}: acc[{ai}] type={a['type']} comp={a['componentType']} count={a['count']} bv={bvi} bvOff={bv.get('byteOffset',0)} bvLen={bv['byteLength']} stride={bv.get('byteStride', '-')}")
        idx_ai = prim.get('indices')
        if idx_ai is not None:
            a = conv_accs[idx_ai]
            bvi = a['bufferView']
            bv = conv_bvs[bvi]
            print(f"      INDICES: acc[{idx_ai}] type={a['type']} comp={a['componentType']} count={a['count']} bv={bvi} bvOff={bv.get('byteOffset',0)} bvLen={bv['byteLength']} accOff={a.get('byteOffset',0)}")

# Compare positions for mesh 0 prim 0
print("\n=== POSITION COMPARISON: mesh 0, prim 0 ===")
# Original: acc[0], BV[0] stride=36
orig_acc = orig_accs[0]
orig_bv = orig_bvs[orig_acc['bufferView']]
stride = orig_bv['byteStride']
bv_off = orig_bv.get('byteOffset', 0)
acc_off = orig_acc.get('byteOffset', 0)
count = orig_acc['count']

print(f"Original: acc[0] count={count}, BV off={bv_off} stride={stride} acc_off={acc_off}")
orig_positions = []
for v in range(count):
    base = bv_off + v * stride + acc_off
    x, y, z = struct.unpack_from('<3f', orig_bin, base)
    orig_positions.append((x, y, z))

# Converted: acc[0] should still be POSITION
conv_acc = conv_accs[0]
conv_bv = conv_bvs[conv_acc['bufferView']]
conv_bv_off = conv_bv.get('byteOffset', 0)
conv_stride = conv_bv.get('byteStride', 0)
conv_acc_off = conv_acc.get('byteOffset', 0)
print(f"Converted: acc[0] count={conv_acc['count']}, BV off={conv_bv_off} stride={conv_stride} acc_off={conv_acc_off} comp={conv_acc['componentType']} type={conv_acc['type']}")

conv_positions = []
elem_size = 12  # VEC3 FLOAT = 3*4
for v in range(conv_acc['count']):
    if conv_stride:
        base = conv_bv_off + v * conv_stride + conv_acc_off
    else:
        base = conv_bv_off + conv_acc_off + v * elem_size
    x, y, z = struct.unpack_from('<3f', conv_bin, base)
    conv_positions.append((x, y, z))

print(f"\nFirst 10 positions comparison:")
for i in range(min(10, count)):
    ox, oy, oz = orig_positions[i]
    cx, cy, cz = conv_positions[i]
    match = "OK" if abs(ox-cx) < 0.001 and abs(oy-cy) < 0.001 and abs(oz-cz) < 0.001 else "MISMATCH!"
    print(f"  v{i:3d}: orig=({ox:9.4f},{oy:9.4f},{oz:9.4f})  conv=({cx:9.4f},{cy:9.4f},{cz:9.4f})  {match}")

# Count mismatches
mismatches = 0
for i in range(count):
    ox, oy, oz = orig_positions[i]
    cx, cy, cz = conv_positions[i]
    if abs(ox-cx) > 0.001 or abs(oy-cy) > 0.001 or abs(oz-cz) > 0.001:
        mismatches += 1
print(f"\nPosition mismatches: {mismatches}/{count}")

# Also compare indices
print("\n=== INDEX COMPARISON ===")
# Original indices: acc[6]
orig_idx_acc = orig_accs[6]
orig_idx_bv = orig_bvs[orig_idx_acc['bufferView']]
idx_off = orig_idx_bv.get('byteOffset', 0) + orig_idx_acc.get('byteOffset', 0)
idx_count = orig_idx_acc['count']
orig_indices = list(struct.unpack_from(f'<{idx_count}H', orig_bin, idx_off))

# Converted indices: acc[6] (should be unchanged)  
conv_idx_acc = conv_accs[6]
conv_idx_bv = conv_bvs[conv_idx_acc['bufferView']]
conv_idx_off = conv_idx_bv.get('byteOffset', 0) + conv_idx_acc.get('byteOffset', 0)
conv_idx_count = conv_idx_acc['count']
conv_indices = list(struct.unpack_from(f'<{conv_idx_count}H', conv_bin, conv_idx_off))

if orig_indices == conv_indices:
    print(f"Indices match ({idx_count} indices)")
else:
    print(f"INDICES MISMATCH!")
    for i in range(min(20, idx_count)):
        if orig_indices[i] != conv_indices[i]:
            print(f"  [{i}]: orig={orig_indices[i]} conv={conv_indices[i]}")

# Now verify: does the GLB we write match what we validate?
from scripts.convert_msfs_scenery import build_glb
glb_output = build_glb(conv_json, conv_bin)
reparse_json, reparse_bin = parse_glb(glb_output)

print("\n=== RE-PARSED VERIFICATION ===")
reparse_accs = reparse_json.get('accessors', [])
reparse_bvs = reparse_json.get('bufferViews', [])
rp_acc = reparse_accs[0]
rp_bv = reparse_bvs[rp_acc['bufferView']]

print(f"Re-parsed acc[0]: bv={rp_acc['bufferView']} type={rp_acc['type']} comp={rp_acc['componentType']} count={rp_acc['count']}")
print(f"Re-parsed BV: off={rp_bv.get('byteOffset',0)} len={rp_bv['byteLength']} stride={rp_bv.get('byteStride','-')}")

rp_positions = []
rp_bv_off = rp_bv.get('byteOffset', 0)
for v in range(rp_acc['count']):
    base = rp_bv_off + v * 12
    x, y, z = struct.unpack_from('<3f', reparse_bin, base)
    rp_positions.append((x, y, z))

print(f"\nFirst 10 re-parsed positions:")
for i in range(min(10, count)):
    ox, oy, oz = orig_positions[i]
    rx, ry, rz = rp_positions[i]
    match = "OK" if abs(ox-rx) < 0.001 and abs(oy-ry) < 0.001 and abs(oz-rz) < 0.001 else "MISMATCH!"
    print(f"  v{i:3d}: orig=({ox:9.4f},{oy:9.4f},{oz:9.4f})  re-parsed=({rx:9.4f},{ry:9.4f},{rz:9.4f})  {match}")

# Also compare normals
print("\n=== NORMAL COMPARISON: mesh 0, prim 0 ===")
# Original: acc[2] NORMAL VEC4 BYTE at offset 16
orig_norm_acc = orig_accs[2]
print(f"Original normal: type={orig_norm_acc['type']} comp={orig_norm_acc['componentType']} off={orig_norm_acc.get('byteOffset',0)}")

# Converted: acc[2] should be the deinterleaved normal
conv_norm_acc = conv_accs[2]
conv_norm_bv = conv_bvs[conv_norm_acc['bufferView']]
print(f"Converted normal: type={conv_norm_acc['type']} comp={conv_norm_acc['componentType']} bv={conv_norm_acc['bufferView']} bvOff={conv_norm_bv.get('byteOffset',0)} bvLen={conv_norm_bv['byteLength']}")

print(f"\nFirst 5 normals:")
for v in range(min(5, count)):
    # Original: read from interleaved
    base = bv_off + v * stride + 16  # normal at stride offset 16
    nb = struct.unpack_from('<4b', orig_bin, base)
    orig_n = [max(b/127.0, -1.0) for b in nb[:3]]  # Only first 3 for VEC3
    
    # Converted
    n_comps = 3 if conv_norm_acc['type'] == 'VEC3' else 4
    cbase = conv_norm_bv.get('byteOffset', 0) + v * n_comps * 4
    conv_n = struct.unpack_from(f'<{n_comps}f', conv_bin, cbase)
    
    print(f"  v{v}: orig_bytes={nb} -> ({orig_n[0]:.4f},{orig_n[1]:.4f},{orig_n[2]:.4f})  conv=({conv_n[0]:.4f},{conv_n[1]:.4f},{conv_n[2]:.4f})")
