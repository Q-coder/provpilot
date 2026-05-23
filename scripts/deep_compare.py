#!/usr/bin/env python3
"""Deep compare: original MSFS GLB vs our converted vs MCX reference."""
import json, struct, sys
from pathlib import Path

def parse_glb(data):
    offset = 12
    json_chunk = None; bin_chunk = b''
    while offset < len(data):
        chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
        cd = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A: json_chunk = json.loads(cd.rstrip(b'\x00 '))
        elif chunk_type == 0x004E4942: bin_chunk = cd
        offset += 8 + chunk_len
    return json_chunk, bin_chunk

def half_to_float(h):
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        return (-1)**sign * (frac / 1024.0) * (2 ** -14) if frac else (-1)**sign * 0.0
    elif exp == 31:
        return float('inf') * (-1)**sign if frac == 0 else float('nan')
    return (-1)**sign * (1.0 + frac / 1024.0) * (2 ** (exp - 15))

# Load original BGL and extract the first GLB
bgl_path = Path('/Users/gery/code/provpilot/scenery/custom/ps-airport-lspf-schmerlat/scenery/global/scenery/modelLib.BGL')
bgl_data = bgl_path.read_bytes()
# Find first glTF
pos = bgl_data.find(b'glTF')
version = struct.unpack_from('<I', bgl_data, pos + 4)[0]
length = struct.unpack_from('<I', bgl_data, pos + 8)[0]
orig_glb = bgl_data[pos:pos+length]
orig_json, orig_bin = parse_glb(orig_glb)

print("=" * 70)
print("ORIGINAL MSFS GLB (from BGL)")
print("=" * 70)

# Show the ORIGINAL accessor/bufferView layout
orig_bvs = orig_json.get('bufferViews', [])
orig_accs = orig_json.get('accessors', [])
orig_meshes = orig_json.get('meshes', [])
orig_nodes = orig_json.get('nodes', [])

print(f"BufferViews: {len(orig_bvs)}, Accessors: {len(orig_accs)}")
print(f"Meshes: {len(orig_meshes)}, Nodes: {len(orig_nodes)}")

# Show all bufferViews
print(f"\nBufferViews:")
for i, bv in enumerate(orig_bvs):
    target = bv.get('target', '?')
    target_s = {34962: 'VERTEX', 34963: 'INDEX'}.get(target, str(target))
    print(f"  bv[{i}]: offset={bv.get('byteOffset',0)} length={bv['byteLength']}"
          f" stride={bv.get('byteStride','(none)')} target={target_s}")

# Show mesh[0] structure in detail
mesh0 = orig_meshes[0]
print(f"\nmesh[0] '{mesh0.get('name','')}': {len(mesh0['primitives'])} primitives")

for pi, prim in enumerate(mesh0['primitives']):
    asobo = prim.get('extras', {}).get('ASOBO_primitive', {})
    idx_acc_idx = prim.get('indices')
    idx_acc = orig_accs[idx_acc_idx] if idx_acc_idx is not None else None
    
    print(f"\n  prim[{pi}] mat={prim.get('material')}")
    if asobo:
        print(f"    ASOBO_primitive: StartIndex={asobo.get('StartIndex', 0)}, PrimitiveCount={asobo.get('PrimitiveCount')}")
    
    if idx_acc:
        idx_bv = orig_bvs[idx_acc['bufferView']]
        print(f"    indices: acc[{idx_acc_idx}] type={idx_acc['type']} comp={idx_acc['componentType']}"
              f" count={idx_acc['count']} byteOff={idx_acc.get('byteOffset',0)}")
        print(f"      -> bv[{idx_acc['bufferView']}]: offset={idx_bv.get('byteOffset',0)} length={idx_bv['byteLength']}")
    
    attrs = prim.get('attributes', {})
    for attr_name in ['POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0', 'TEXCOORD_1', 'COLOR_0']:
        if attr_name in attrs:
            acc_idx = attrs[attr_name]
            acc = orig_accs[acc_idx]
            bv_idx = acc.get('bufferView')
            bv = orig_bvs[bv_idx] if bv_idx is not None else {}
            print(f"    {attr_name}: acc[{acc_idx}] {acc['type']} comp={acc['componentType']}"
                  f" count={acc['count']} byteOff={acc.get('byteOffset',0)}"
                  f" norm={'Y' if acc.get('normalized') else 'N'}"
                  f" -> bv[{bv_idx}] stride={bv.get('byteStride','(none)')}")

# Now read actual vertex data from original using stride layout
print(f"\n\n--- Raw vertex data from ORIGINAL mesh[0]/prim[0] ---")
prim0 = mesh0['primitives'][0]
pos_acc_idx = prim0['attributes']['POSITION']
pos_acc = orig_accs[pos_acc_idx]
pos_bv = orig_bvs[pos_acc['bufferView']]
stride = pos_bv.get('byteStride', 12)
bv_off = pos_bv.get('byteOffset', 0)

print(f"POSITION accessor: bv={pos_acc['bufferView']} offset={pos_acc.get('byteOffset',0)} stride={stride}")

# Read first 5 vertices using the interleaved layout
for v in range(min(5, pos_acc['count'])):
    base = bv_off + v * stride + pos_acc.get('byteOffset', 0)
    # POSITION at offset 0 (relative to acc)
    px, py, pz = struct.unpack_from('<3f', orig_bin, base)
    print(f"  v{v} POSITION: ({px:.6f}, {py:.6f}, {pz:.6f})")
    
    # If stride=36, read other attrs
    if stride == 36:
        tang = struct.unpack_from('<4b', orig_bin, base + 12)
        norm = struct.unpack_from('<4b', orig_bin, base + 16)
        tc0_raw = struct.unpack_from('<2H', orig_bin, base + 20)
        tc1_raw = struct.unpack_from('<2H', orig_bin, base + 24)
        col_raw = struct.unpack_from('<4H', orig_bin, base + 28)
        
        tang_f = [max(t/127.0, -1.0) for t in tang]
        norm_f = [max(n/127.0, -1.0) for n in norm]
        tc0_f = [half_to_float(t) for t in tc0_raw]
        tc1_f = [half_to_float(t) for t in tc1_raw]
        col_f = [half_to_float(c) for c in col_raw]
        
        print(f"        TANGENT: raw={tang} -> {[f'{x:.4f}' for x in tang_f]}")
        print(f"        NORMAL:  raw={norm} -> {[f'{x:.4f}' for x in norm_f]}")
        print(f"        TC0:     raw={tc0_raw} -> {[f'{x:.4f}' for x in tc0_f]}")
        print(f"        TC1:     raw={tc1_raw} -> {[f'{x:.4f}' for x in tc1_f]}")
        print(f"        COLOR:   raw={col_raw} -> {[f'{x:.4f}' for x in col_f]}")

# Read original indices
idx_acc_idx = prim0.get('indices')
if idx_acc_idx is not None:
    idx_acc = orig_accs[idx_acc_idx]
    idx_bv = orig_bvs[idx_acc['bufferView']]
    comp = idx_acc['componentType']
    fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
    sz = {5121: 1, 5123: 2, 5125: 4}.get(comp, 2)
    bv_off = idx_bv.get('byteOffset', 0)
    acc_off = idx_acc.get('byteOffset', 0)
    
    # Read first 18 indices (6 triangles)
    indices = []
    for i in range(min(18, idx_acc['count'])):
        val = struct.unpack_from(f'<{fmt}', orig_bin, bv_off + acc_off + i * sz)[0]
        indices.append(val)
    print(f"\n  Original indices (first 18, comp={comp}): {indices}")
    
    # Check ASOBO_primitive
    asobo = prim0.get('extras', {}).get('ASOBO_primitive', {})
    if asobo:
        start = asobo.get('StartIndex', 0)
        pcount = asobo.get('PrimitiveCount', 0)
        print(f"  ASOBO: StartIndex={start}, PrimitiveCount={pcount}")
        print(f"  Expected index range: [{start}..{start + pcount*3 - 1}]")
        
        # Read the actual sub-range
        sub_indices = []
        for i in range(min(18, pcount*3)):
            val = struct.unpack_from(f'<{fmt}', orig_bin, bv_off + acc_off + (start + i) * sz)[0]
            sub_indices.append(val)
        print(f"  Sub-range indices (first 18): {sub_indices}")

# Now load our converted GLB and compare
print("\n" + "=" * 70)
print("OUR CONVERTED GLB")  
print("=" * 70)

our_glb = Path('/Users/gery/code/provpilot/provpilot/Assets/custom/ps-airport-lspf-schmerlat/converted/Hangar_rechts.glb').read_bytes()
our_json, our_bin = parse_glb(our_glb)

our_mesh0 = our_json['meshes'][0]
our_prim0 = our_mesh0['primitives'][0]
our_accs = our_json['accessors']
our_bvs = our_json['bufferViews']

# Read our converted POSITION data
our_pos_idx = our_prim0['attributes']['POSITION']
our_pos_acc = our_accs[our_pos_idx]
our_pos_bv = our_bvs[our_pos_acc['bufferView']]
our_bv_off = our_pos_bv.get('byteOffset', 0)
our_stride = our_pos_bv.get('byteStride', 12)

print(f"\nmesh[0]/prim[0] POSITION: count={our_pos_acc['count']}, stride={our_stride}")
for v in range(5):
    base = our_bv_off + our_pos_acc.get('byteOffset', 0) + v * our_stride
    px, py, pz = struct.unpack_from('<3f', our_bin, base)
    print(f"  v{v}: ({px:.6f}, {py:.6f}, {pz:.6f})")

# Read our indices
our_idx_idx = our_prim0.get('indices')
if our_idx_idx is not None:
    our_idx_acc = our_accs[our_idx_idx]
    our_idx_bv = our_bvs[our_idx_acc['bufferView']]
    comp = our_idx_acc['componentType']
    fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
    sz = {5121: 1, 5123: 2, 5125: 4}.get(comp, 2)
    bv_off = our_idx_bv.get('byteOffset', 0)
    acc_off = our_idx_acc.get('byteOffset', 0)
    
    indices = []
    for i in range(min(18, our_idx_acc['count'])):
        val = struct.unpack_from(f'<{fmt}', our_bin, bv_off + acc_off + i * sz)[0]
        indices.append(val)
    print(f"\n  Our indices (first 18, comp={comp}): {indices}")
    
    # Also check max index and compare to vertex count
    max_idx = 0
    for i in range(our_idx_acc['count']):
        val = struct.unpack_from(f'<{fmt}', our_bin, bv_off + acc_off + i * sz)[0]
        max_idx = max(max_idx, val)
    print(f"  Max index: {max_idx}, vertex count: {our_pos_acc['count']}")

# Check how many accessors share the same bufferView (vertex sharing)
print(f"\n--- Accessor sharing analysis ---")
bv_to_accs = {}
for i, acc in enumerate(our_accs):
    bv = acc.get('bufferView')
    bv_to_accs.setdefault(bv, []).append(i)

# Find vertex bufferViews (non-index)
for pi, prim in enumerate(our_mesh0['primitives'][:3]):
    attrs = prim.get('attributes', {})
    pos_bv = our_accs[attrs['POSITION']]['bufferView']
    idx_bv = our_accs[prim['indices']]['bufferView'] if 'indices' in prim else None
    print(f"  prim[{pi}]: POSITION bv={pos_bv}, indices bv={idx_bv}")

# Check if all prims in mesh0 share exact same POSITION accessor
all_pos_accs = set()
for prim in our_mesh0['primitives']:
    all_pos_accs.add(prim['attributes']['POSITION'])
print(f"\n  Unique POSITION accessors in mesh[0]: {all_pos_accs}")

# Key question: do the index values in each prim actually reference valid vertices?
print(f"\n--- Index range per primitive (mesh[0]) ---")
for pi, prim in enumerate(our_mesh0['primitives']):
    idx_acc = our_accs[prim['indices']]
    idx_bv = our_bvs[idx_acc['bufferView']]
    comp = idx_acc['componentType']
    fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
    sz = struct.calcsize(f'<{fmt}')
    bv_off = idx_bv.get('byteOffset', 0)
    acc_off = idx_acc.get('byteOffset', 0)
    
    min_idx, max_idx = 99999, 0
    for i in range(idx_acc['count']):
        val = struct.unpack_from(f'<{fmt}', our_bin, bv_off + acc_off + i * sz)[0]
        min_idx = min(min_idx, val)
        max_idx = max(max_idx, val)
    
    vtx_count = our_accs[prim['attributes']['POSITION']]['count']
    ok = "OK" if max_idx < vtx_count else "OUT OF RANGE!"
    print(f"  prim[{pi}]: {idx_acc['count']} indices, range=[{min_idx}..{max_idx}], vtx_count={vtx_count} {ok}")
