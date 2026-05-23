#!/usr/bin/env python3
"""Deep inspection of original vs converted GLB to find geometry issues."""

import json
import struct
import sys
from pathlib import Path

def parse_glb(data):
    magic, version, length = struct.unpack_from('<4sII', data, 0)
    offset = 12
    json_chunk = None
    bin_chunk = None
    while offset < length:
        chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A:
            json_chunk = json.loads(chunk_data)
        elif chunk_type == 0x004E4942:
            bin_chunk = bytearray(chunk_data)
        offset += 8 + chunk_len
    return json_chunk, bin_chunk

def inspect_glb(filepath):
    data = Path(filepath).read_bytes()
    gltf, bin_data = parse_glb(data)
    
    print(f"\n{'='*60}")
    print(f"FILE: {filepath}")
    print(f"{'='*60}")
    
    bvs = gltf.get('bufferViews', [])
    accs = gltf.get('accessors', [])
    meshes = gltf.get('meshes', [])
    
    print(f"\nBuffers: {len(gltf.get('buffers', []))}")
    for i, buf in enumerate(gltf.get('buffers', [])):
        print(f"  buffer[{i}]: byteLength={buf.get('byteLength')}, actual bin size={len(bin_data)}")
    
    print(f"\nBufferViews: {len(bvs)}")
    for i, bv in enumerate(bvs):
        stride_str = f", stride={bv['byteStride']}" if 'byteStride' in bv else ""
        target_str = f", target={bv.get('target', 'none')}" if 'target' in bv else ""
        print(f"  bv[{i}]: offset={bv.get('byteOffset',0)}, len={bv['byteLength']}{stride_str}{target_str}")
    
    print(f"\nAccessors: {len(accs)}")
    for i, acc in enumerate(accs):
        norm = ", normalized" if acc.get('normalized') else ""
        bv_idx = acc.get('bufferView', 'NONE')
        print(f"  acc[{i}]: bv={bv_idx}, offset={acc.get('byteOffset',0)}, "
              f"type={acc['type']}, comp={acc['componentType']}, count={acc['count']}{norm}")
        if 'min' in acc:
            print(f"          min={acc['min']}")
            print(f"          max={acc['max']}")
    
    print(f"\nMeshes: {len(meshes)}")
    for i, mesh in enumerate(meshes):
        print(f"  mesh[{i}]: '{mesh.get('name', '')}' — {len(mesh.get('primitives',[]))} primitives")
        for j, prim in enumerate(mesh.get('primitives', [])):
            attrs = prim.get('attributes', {})
            idx = prim.get('indices')
            mat = prim.get('material')
            mode = prim.get('mode', 4)  # default TRIANGLES
            print(f"    prim[{j}]: mode={mode}, indices=acc[{idx}], material={mat}")
            for attr_name, acc_idx in sorted(attrs.items()):
                acc = accs[acc_idx]
                print(f"      {attr_name}: acc[{acc_idx}] → bv[{acc.get('bufferView','?')}] "
                      f"{acc['type']} comp={acc['componentType']} count={acc['count']}")
    
    # Check for shared bufferViews (multiple accessors using same bv)
    bv_users = {}
    for i, acc in enumerate(accs):
        bv_idx = acc.get('bufferView')
        if bv_idx is not None:
            bv_users.setdefault(bv_idx, []).append(i)
    
    shared = {k: v for k, v in bv_users.items() if len(v) > 1}
    if shared:
        print(f"\nShared bufferViews:")
        for bv_idx, acc_list in shared.items():
            bv = bvs[bv_idx]
            print(f"  bv[{bv_idx}] (offset={bv.get('byteOffset',0)}, len={bv['byteLength']}, "
                  f"stride={bv.get('byteStride','none')}) used by accessors: {acc_list}")
    
    # Verify vertex data samples
    print(f"\nVertex data samples (first 3 vertices per POSITION accessor):")
    for i, acc in enumerate(accs):
        # Find which attributes use this accessor
        for mesh in meshes:
            for prim in mesh.get('primitives', []):
                for attr_name, acc_idx in prim.get('attributes', {}).items():
                    if acc_idx == i and attr_name == 'POSITION':
                        bv_idx = acc.get('bufferView')
                        bv = bvs[bv_idx]
                        bv_off = bv.get('byteOffset', 0)
                        acc_off = acc.get('byteOffset', 0)
                        stride = bv.get('byteStride', 12)  # VEC3 FLOAT = 12 bytes if no stride  
                        comp = acc['componentType']
                        
                        if comp == 5126:  # FLOAT
                            for v in range(min(3, acc['count'])):
                                base = bv_off + acc_off + v * stride
                                x, y, z = struct.unpack_from('<3f', bin_data, base)
                                print(f"    acc[{i}] v{v}: ({x:.4f}, {y:.4f}, {z:.4f})")
                        else:
                            print(f"    acc[{i}]: non-FLOAT positions (comp={comp})")
    
    # Check index data
    print(f"\nIndex data samples (first 12 indices per index accessor):")
    for i, acc in enumerate(accs):
        for mesh in meshes:
            for prim in mesh.get('primitives', []):
                if prim.get('indices') == i:
                    bv_idx = acc.get('bufferView')
                    bv = bvs[bv_idx]
                    bv_off = bv.get('byteOffset', 0)
                    acc_off = acc.get('byteOffset', 0)
                    comp = acc['componentType']
                    
                    if comp == 5123:  # USHORT
                        fmt, sz = 'H', 2
                    elif comp == 5125:  # UINT
                        fmt, sz = 'I', 4
                    elif comp == 5121:  # UBYTE
                        fmt, sz = 'B', 1
                    else:
                        print(f"    acc[{i}]: unknown index type {comp}")
                        continue
                    
                    indices = []
                    for j in range(min(12, acc['count'])):
                        val = struct.unpack_from(f'<{fmt}', bin_data, bv_off + acc_off + j * sz)[0]
                        indices.append(val)
                    print(f"    acc[{i}] (count={acc['count']}): {indices}")
                    # Also show max index value
                    max_idx = 0
                    for j in range(acc['count']):
                        val = struct.unpack_from(f'<{fmt}', bin_data, bv_off + acc_off + j * sz)[0]
                        max_idx = max(max_idx, val)
                    print(f"    max index value: {max_idx}")

    return gltf, bin_data


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: deep_inspect_glb.py <file.glb> [file2.glb]")
        sys.exit(1)
    
    for f in sys.argv[1:]:
        inspect_glb(f)
