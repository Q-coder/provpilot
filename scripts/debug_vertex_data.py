#!/usr/bin/env python3
"""Inspect the raw vertex data from extracted GLBs to debug geometry issues."""
import json, struct, sys
sys.path.insert(0, '.')
from scripts.convert_msfs_scenery import extract_glbs_from_bgl, parse_glb

from pathlib import Path
bgl_path = Path('scenery/custom/ps-airport-lspf-schmerlat/scenery/global/scenery/modelLib.BGL')
glbs = extract_glbs_from_bgl(bgl_path)
print(f'Total models: {len(glbs)}')

# Parse model 0 (simplest: 4 meshes)
offset, glb_data = glbs[0]
js, bd = parse_glb(glb_data)
accessors = js.get('accessors', [])
buffer_views = js.get('bufferViews', [])
meshes = js.get('meshes', [])

print(f'\nModel 0: {len(meshes)} meshes, {len(accessors)} accessors, {len(buffer_views)} bvs')
print(f'Buffer size: {len(bd)} bytes')

# Show ALL buffer views
print('\n=== BUFFER VIEWS ===')
for i, bv in enumerate(buffer_views):
    s = bv.get('byteStride', '-')
    t = bv.get('target', '-')
    print(f'  BV[{i}]: off={bv.get("byteOffset",0)} len={bv["byteLength"]} stride={s} target={t}')

# Show all accessors with their mesh primitive usage
print('\n=== ACCESSORS ===')
acc_usage = {}
for mi, mesh in enumerate(meshes):
    for pi, prim in enumerate(mesh.get('primitives', [])):
        for attr, ai in prim.get('attributes', {}).items():
            acc_usage.setdefault(ai, []).append(f'm{mi}p{pi}.{attr}')
        idx = prim.get('indices')
        if idx is not None:
            acc_usage.setdefault(idx, []).append(f'm{mi}p{pi}.INDICES')

for i, a in enumerate(accessors):
    usage = ', '.join(acc_usage.get(i, ['unused']))
    norm = ' NORM' if a.get('normalized') else ''
    print(f'  acc[{i}]: bv={a.get("bufferView")} type={a["type"]} comp={a["componentType"]} count={a["count"]} off={a.get("byteOffset",0)}{norm}  [{usage}]')

# For each interleaved buffer view, dump some raw position data
print('\n=== RAW POSITION DATA (first 5 vertices) ===')
for bv_idx, bv in enumerate(buffer_views):
    stride = bv.get('byteStride')
    if not stride:
        continue
    
    bv_off = bv.get('byteOffset', 0)
    bv_len = bv['byteLength']
    n_verts = bv_len // stride
    
    # Find POSITION accessor for this BV
    pos_accs = []
    for ai, a in enumerate(accessors):
        if a.get('bufferView') == bv_idx:
            names = acc_usage.get(ai, [])
            if any('POSITION' in n for n in names):
                pos_accs.append(ai)
    
    print(f'\n  BV[{bv_idx}] stride={stride} verts={n_verts}')
    
    # Dump raw bytes of first vertex to see actual layout
    print(f'  First 3 vertices raw bytes (stride={stride}):')
    for v in range(min(3, n_verts)):
        base = bv_off + v * stride
        raw = bd[base:base+stride]
        hex_str = ' '.join(f'{b:02x}' for b in raw)
        print(f'    v{v}: {hex_str}')
    
    # Interpret positions (first 12 bytes as 3 floats)
    print(f'  Positions (VEC3 FLOAT @ offset 0):')
    for v in range(min(5, n_verts)):
        base = bv_off + v * stride
        x, y, z = struct.unpack_from('<3f', bd, base)
        print(f'    v{v}: ({x:.4f}, {y:.4f}, {z:.4f})')
    
    # Show all components for first vertex
    print(f'  First vertex decoded (all stride={stride} components):')
    base = bv_off
    if stride >= 12:
        x, y, z = struct.unpack_from('<3f', bd, base)
        print(f'    @0  POSITION: ({x:.6f}, {y:.6f}, {z:.6f})')
    if stride >= 16:
        vals = struct.unpack_from('<4b', bd, base + 12)
        print(f'    @12 TANGENT(4xBYTE): {vals}')
    if stride >= 20:
        vals = struct.unpack_from('<4b', bd, base + 16)
        print(f'    @16 NORMAL(4xBYTE):  {vals}')
    if stride >= 24:
        vals = struct.unpack_from('<2H', bd, base + 20)
        # Interpret as half-float
        from scripts.convert_msfs_scenery import _half_to_float
        hf = [_half_to_float(v) for v in vals]
        print(f'    @20 TEXCOORD0(2xSHORT): raw={vals} half={hf}')
    if stride >= 28:
        vals = struct.unpack_from('<2H', bd, base + 24)
        hf = [_half_to_float(v) for v in vals]
        print(f'    @24 TEXCOORD1(2xSHORT): raw={vals} half={hf}')
    if stride >= 36:
        vals = struct.unpack_from('<4H', bd, base + 28)
        hf = [_half_to_float(v) for v in vals]
        print(f'    @28 COLOR0(4xUSHORT):   raw={vals} half={hf}')
    
    # Check: are there DIFFERENT strides?
    print(f'\n  All interleaved BVs and their strides:')
    for bvi2, bv2 in enumerate(buffer_views):
        s2 = bv2.get('byteStride')
        if s2:
            print(f'    BV[{bvi2}]: stride={s2} len={bv2["byteLength"]} verts={bv2["byteLength"]//s2}')

# Also check: how many distinct strides exist across ALL models?
print('\n\n=== STRIDES ACROSS ALL MODELS ===')
stride_counts = {}
for mi, (off, gdata) in enumerate(glbs):
    gj, _ = parse_glb(gdata)
    for bv in gj.get('bufferViews', []):
        s = bv.get('byteStride')
        if s:
            stride_counts[s] = stride_counts.get(s, 0) + 1
print(f'Distinct strides: {stride_counts}')

# Check: do any models have non-36 strides?
for mi, (off, gdata) in enumerate(glbs):
    gj, _ = parse_glb(gdata)
    for bvi, bv in enumerate(gj.get('bufferViews', [])):
        s = bv.get('byteStride')
        if s and s != 36:
            print(f'  Model [{mi}] BV[{bvi}]: NON-36 stride={s}')
