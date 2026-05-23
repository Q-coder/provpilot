#!/usr/bin/env python3
"""Extract raw GLBs from BGL without conversion, then inspect their structure."""

import json
import struct
import sys
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
    magic, version, length = struct.unpack_from('<4sII', data, 0)
    offset = 12
    json_chunk = None
    bin_chunk = None
    while offset < length:
        chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A:
            # JSON chunk may have padding - find actual JSON end
            text = chunk_data.rstrip(b'\x00 \t\n\r')
            json_chunk = json.loads(text)
        elif chunk_type == 0x004E4942:
            bin_chunk = bytearray(chunk_data)
        offset += 8 + chunk_len
    return json_chunk, bin_chunk

def find_bgl(scenery_dir):
    scenery_dir = Path(scenery_dir)
    for p in scenery_dir.rglob('*.BGL'):
        return p
    for p in scenery_dir.rglob('*.bgl'):
        return p
    return None

def inspect_original(gltf, bin_data, model_idx):
    bvs = gltf.get('bufferViews', [])
    accs = gltf.get('accessors', [])
    meshes = gltf.get('meshes', [])

    print(f"\n--- ORIGINAL Model {model_idx} ---")
    print(f"BufferViews: {len(bvs)}")
    for i, bv in enumerate(bvs):
        stride_str = f", stride={bv['byteStride']}" if 'byteStride' in bv else ""
        print(f"  bv[{i}]: offset={bv.get('byteOffset',0)}, len={bv['byteLength']}{stride_str}")

    print(f"\nAccessors: {len(accs)}")
    for i, acc in enumerate(accs):
        norm = ", normalized" if acc.get('normalized') else ""
        print(f"  acc[{i}]: bv={acc.get('bufferView','NONE')}, offset={acc.get('byteOffset',0)}, "
              f"type={acc['type']}, comp={acc['componentType']}, count={acc['count']}{norm}")

    print(f"\nMeshes: {len(meshes)}")
    for i, mesh in enumerate(meshes):
        prims = mesh.get('primitives', [])
        print(f"  mesh[{i}]: '{mesh.get('name', '')}' — {len(prims)} primitives")
        for j, prim in enumerate(prims):
            attrs = prim.get('attributes', {})
            idx = prim.get('indices')
            mat = prim.get('material')
            mode = prim.get('mode', 4)
            print(f"    prim[{j}]: mode={mode}, indices=acc[{idx}], material={mat}")
            for attr_name, acc_idx in sorted(attrs.items()):
                acc = accs[acc_idx]
                print(f"      {attr_name}: acc[{acc_idx}] (bv={acc.get('bufferView','?')}, "
                      f"offset={acc.get('byteOffset', 0)}, type={acc['type']}, "
                      f"comp={acc['componentType']}, count={acc['count']})")

if __name__ == '__main__':
    scenery_dir = sys.argv[1] if len(sys.argv) > 1 else 'scenery/custom/ps-airport-lspf-schmerlat'
    bgl_path = find_bgl(scenery_dir)
    if not bgl_path:
        print(f"No BGL found in {scenery_dir}")
        sys.exit(1)

    print(f"BGL: {bgl_path}")
    glbs = extract_glbs_from_bgl(bgl_path)
    print(f"Found {len(glbs)} GLBs")
    
    # Find the Hangar model (the large complex one)
    for idx, (offset, glb_data) in enumerate(glbs):
        gltf, bin_data = parse_glb(glb_data)
        if not gltf:
            continue
        meshes = gltf.get('meshes', [])
        # Show models with multiple primitives (likely the hangar)
        total_prims = sum(len(m.get('primitives', [])) for m in meshes)
        mesh_names = [m.get('name', '') for m in meshes[:3]]
        if total_prims > 5 or idx < 3:
            print(f"\nModel {idx}: {len(glb_data):,} bytes, {len(meshes)} meshes, "
                  f"{total_prims} prims, names={mesh_names}")
            inspect_original(gltf, bin_data, idx)
            if total_prims > 20:  # Only detailed for complex models
                break
