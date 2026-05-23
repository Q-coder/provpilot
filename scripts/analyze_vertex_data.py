#!/usr/bin/env python3
"""Analyze raw vertex data from MSFS-converted GLB to understand the packed format."""
import json, struct, math, sys

def parse_glb(path):
    data = open(path, 'rb').read()
    off = 12
    gltf = None
    bin_data = b''
    while off < len(data):
        cl = struct.unpack_from('<I', data, off)[0]
        ct = struct.unpack_from('<I', data, off+4)[0]
        if ct == 0x4E4F534A:
            gltf = json.loads(data[off+8:off+8+cl])
        elif ct == 0x004E4942:
            bin_data = data[off+8:off+8+cl]
        off += 8 + cl
        if off % 4: off += 4 - (off % 4)
    return gltf, bin_data

path = sys.argv[1] if len(sys.argv) > 1 else 'scenery/custom/ps-airport-lspf-schmerlat/converted/Hangar_rechts.glb'
gltf, bin_data = parse_glb(path)

bvs = gltf['bufferViews']
accs = gltf['accessors']

# Show the interleaved buffer view
bv0 = bvs[0]
stride = bv0.get('byteStride', 0)
print(f"BufferView 0: offset={bv0.get('byteOffset',0)} length={bv0['byteLength']} stride={stride}")

# Map component types
COMP_NAMES = {5120: 'BYTE', 5121: 'UBYTE', 5122: 'SHORT', 5123: 'USHORT', 5125: 'UINT', 5126: 'FLOAT'}
TYPE_COMPONENTS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}

# Show first mesh primitive's attributes
mesh0 = gltf['meshes'][0]
prim0 = mesh0['primitives'][0]
print(f"\nMesh 0, Primitive 0:")
print(f"  Indices accessor: {prim0.get('indices')}")
for attr, acc_idx in sorted(prim0['attributes'].items()):
    acc = accs[acc_idx]
    ct = COMP_NAMES.get(acc['componentType'], str(acc['componentType']))
    bo = acc.get('byteOffset', 0)
    norm = acc.get('normalized', False)
    print(f"  {attr:12s} -> acc[{acc_idx}] {acc['type']:6s} {ct:6s} byteOff={bo:4d} count={acc['count']} normalized={norm}")

# Layout within stride
print(f"\nVertex layout within stride={stride}:")
attrs_by_offset = []
for attr, acc_idx in prim0['attributes'].items():
    acc = accs[acc_idx]
    bo = acc.get('byteOffset', 0)
    ct = acc['componentType']
    comp_size = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}[ct]
    n_comps = TYPE_COMPONENTS[acc['type']]
    total = comp_size * n_comps
    attrs_by_offset.append((bo, attr, acc['type'], COMP_NAMES[ct], total))

attrs_by_offset.sort()
for bo, attr, atype, ct, size in attrs_by_offset:
    print(f"  offset {bo:3d}: {attr:12s} {atype} {ct} ({size} bytes)")

# Read and display first 5 vertices
bv_base = bv0.get('byteOffset', 0)
count = accs[0]['count']
print(f"\nFirst 5 vertices (of {count}):")
for v in range(min(5, count)):
    base = bv_base + v * stride
    # Position: FLOAT VEC3 at offset 0
    px, py, pz = struct.unpack_from('<fff', bin_data, base + 0)
    
    # Read all bytes from offset 12 to stride
    raw = bin_data[base:base+stride]
    hex_str = ' '.join(f'{b:02x}' for b in raw[12:])
    
    # Tangent: 4 bytes at offset 12
    tb = struct.unpack_from('<bbbb', bin_data, base + 12)
    # Normal: 4 bytes at offset 16
    nb = struct.unpack_from('<bbbb', bin_data, base + 16)
    # Texcoord0: 2 shorts at offset 20
    uv0 = struct.unpack_from('<hh', bin_data, base + 20)
    # Texcoord1: 2 shorts at offset 24
    uv1 = struct.unpack_from('<hh', bin_data, base + 24)
    # Color: 4 ushorts at offset 28
    col = struct.unpack_from('<HHHH', bin_data, base + 28)
    
    print(f"  V{v}: pos=({px:9.4f}, {py:9.4f}, {pz:9.4f})")
    print(f"       tan_raw=({tb[0]:4d},{tb[1]:4d},{tb[2]:4d},{tb[3]:4d})  normalized=({tb[0]/127:.3f},{tb[1]/127:.3f},{tb[2]/127:.3f},{tb[3]/127:.3f})")
    print(f"       nrm_raw=({nb[0]:4d},{nb[1]:4d},{nb[2]:4d},{nb[3]:4d})  normalized=({nb[0]/127:.3f},{nb[1]/127:.3f},{nb[2]/127:.3f},{nb[3]/127:.3f})")
    nl = math.sqrt(sum((x/127)**2 for x in nb[:3]))
    print(f"       nrm_len={nl:.3f}")
    print(f"       uv0=({uv0[0]:6d},{uv0[1]:6d}) -> ({uv0[0]/32767:.4f},{uv0[1]/32767:.4f})")
    print(f"       uv1=({uv1[0]:6d},{uv1[1]:6d}) -> ({uv1[0]/32767:.4f},{uv1[1]/32767:.4f})")
    print(f"       col=({col[0]:5d},{col[1]:5d},{col[2]:5d},{col[3]:5d})")
    print(f"       hex[12:]={hex_str}")

# Check if indices buffer view is separate
idx_acc = accs[prim0['indices']]
print(f"\nIndex accessor: bv={idx_acc.get('bufferView')} type={idx_acc['type']} comp={COMP_NAMES.get(idx_acc['componentType'])} count={idx_acc['count']}")
idx_bv = bvs[idx_acc['bufferView']]
print(f"  BufferView {idx_acc['bufferView']}: offset={idx_bv['byteOffset']} length={idx_bv['byteLength']} stride={idx_bv.get('byteStride','none')}")

# Read first few indices
idx_base = idx_bv['byteOffset'] + idx_acc.get('byteOffset', 0)
first_indices = struct.unpack_from('<6H', bin_data, idx_base)
print(f"  First indices: {first_indices}")
