#!/usr/bin/env python3
"""Compare MCX reference export with our converter output."""
import json
import struct
import sys
from pathlib import Path

def load_gltf_and_bin(gltf_path):
    gltf_path = Path(gltf_path)
    with open(gltf_path) as f:
        gltf = json.load(f)
    bin_path = gltf_path.parent / gltf.get('buffers', [{}])[0].get('uri', '')
    bin_data = bin_path.read_bytes() if bin_path.exists() else b''
    return gltf, bin_data

def parse_glb(data):
    offset = 12
    json_chunk = None
    bin_chunk = b''
    while offset < len(data):
        chunk_len, chunk_type = struct.unpack_from('<II', data, offset)
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A:
            json_chunk = json.loads(chunk_data.rstrip(b'\x00 '))
        elif chunk_type == 0x004E4942:
            bin_chunk = chunk_data
        offset += 8 + chunk_len
    return json_chunk, bin_chunk

def inspect(label, gltf, bin_data):
    bvs = gltf.get('bufferViews', [])
    accs = gltf.get('accessors', [])
    meshes = gltf.get('meshes', [])
    mats = gltf.get('materials', [])
    imgs = gltf.get('images', [])
    nodes = gltf.get('nodes', [])

    print(f"\n{'='*60}")
    print(f"{label}")
    print(f"{'='*60}")
    print(f"Asset: {gltf.get('asset', {})}")
    print(f"Buffers: {len(gltf.get('buffers', []))}, bin_size={len(bin_data)}")
    print(f"BufferViews: {len(bvs)}, Accessors: {len(accs)}")
    print(f"Meshes: {len(meshes)}, Materials: {len(mats)}, Images: {len(imgs)}, Nodes: {len(nodes)}")

    # Show first mesh in detail
    for mi, mesh in enumerate(meshes[:3]):
        prims = mesh.get('primitives', [])
        print(f"\n  mesh[{mi}] '{mesh.get('name', '')}': {len(prims)} prims")
        for pi, prim in enumerate(prims[:6]):
            idx_acc = prim.get('indices')
            attrs = prim.get('attributes', {})
            mat = prim.get('material')
            
            idx_info = ""
            if idx_acc is not None:
                acc = accs[idx_acc]
                idx_info = f"count={acc['count']} comp={acc['componentType']}"
            
            print(f"    prim[{pi}]: mat={mat}, indices({idx_info})")
            for attr_name in ['POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0', 'TEXCOORD_1', 'COLOR_0']:
                if attr_name in attrs:
                    acc_idx = attrs[attr_name]
                    acc = accs[acc_idx]
                    bv_idx = acc.get('bufferView')
                    bv = bvs[bv_idx] if bv_idx is not None else {}
                    stride = bv.get('byteStride', '')
                    norm = ' normalized' if acc.get('normalized') else ''
                    print(f"      {attr_name}: {acc['type']} comp={acc['componentType']}"
                          f" count={acc['count']} bvStride={stride}{norm}")
        if len(prims) > 6:
            print(f"    ... +{len(prims)-6} more primitives")

    # Show vertex data samples for first primitive
    mesh0 = meshes[0]
    prim0 = mesh0['primitives'][0]
    pos_acc_idx = prim0['attributes'].get('POSITION')
    if pos_acc_idx is not None:
        acc = accs[pos_acc_idx]
        bv = bvs[acc['bufferView']]
        bv_off = bv.get('byteOffset', 0)
        acc_off = acc.get('byteOffset', 0)
        stride = bv.get('byteStride', 12)
        print(f"\n  First 5 POSITION values (mesh0/prim0):")
        for v in range(min(5, acc['count'])):
            base = bv_off + acc_off + v * stride
            x, y, z = struct.unpack_from('<3f', bin_data, base)
            print(f"    v{v}: ({x:.6f}, {y:.6f}, {z:.6f})")

    # Show first few indices
    idx_acc_idx = prim0.get('indices')
    if idx_acc_idx is not None:
        acc = accs[idx_acc_idx]
        bv = bvs[acc['bufferView']]
        bv_off = bv.get('byteOffset', 0)
        acc_off = acc.get('byteOffset', 0)
        comp = acc['componentType']
        fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
        sz = {5121: 1, 5123: 2, 5125: 4}.get(comp, 2)
        indices = []
        for i in range(min(18, acc['count'])):
            val = struct.unpack_from(f'<{fmt}', bin_data, bv_off + acc_off + i * sz)[0]
            indices.append(val)
        max_idx = 0
        for i in range(acc['count']):
            val = struct.unpack_from(f'<{fmt}', bin_data, bv_off + acc_off + i * sz)[0]
            max_idx = max(max_idx, val)
        print(f"\n  First indices (mesh0/prim0, count={acc['count']}): {indices}")
        print(f"  Max index: {max_idx}")

    # Show node transforms for first 5 mesh-bearing nodes
    print(f"\n  Nodes with meshes:")
    for ni, node in enumerate(nodes):
        mesh_idx = node.get('mesh')
        if mesh_idx is None:
            continue
        t = node.get('translation', [0,0,0])
        r = node.get('rotation')
        s = node.get('scale')
        mesh_name = meshes[mesh_idx].get('name', '') if mesh_idx < len(meshes) else ''
        parts = [f"T=[{t[0]:.2f},{t[1]:.2f},{t[2]:.2f}]"]
        if r: parts.append(f"R=[{r[0]:.3f},{r[1]:.3f},{r[2]:.3f},{r[3]:.3f}]")
        if s: parts.append(f"S=[{s[0]:.3f},{s[1]:.3f},{s[2]:.3f}]")
        print(f"    node[{ni}] '{node.get('name','')}' mesh={mesh_idx}({mesh_name}): {' '.join(parts)}")

    # Material summary
    print(f"\n  Materials:")
    for mi, mat in enumerate(mats[:10]):
        pbr = mat.get('pbrMetallicRoughness', {})
        bc = pbr.get('baseColorFactor', [1,1,1,1])
        bt = pbr.get('baseColorTexture')
        print(f"    mat[{mi}] '{mat.get('name','')}': bc={[round(x,3) for x in bc]}"
              f" tex={'yes' if bt else 'no'}")
    if len(mats) > 10:
        print(f"    ... +{len(mats)-10} more")


# ---- Main ----
mcx_gltf, mcx_bin = load_gltf_and_bin('/Users/gery/code/provpilot/schmerlat/schmerlat_hauptgebaeude.gltf')
inspect("MCX REFERENCE (correct)", mcx_gltf, mcx_bin)

our_glb = Path('/Users/gery/code/provpilot/provpilot/Assets/custom/ps-airport-lspf-schmerlat/converted/Hangar_rechts.glb').read_bytes()
our_gltf, our_bin = parse_glb(our_glb)
inspect("OUR CONVERTER OUTPUT", our_gltf, our_bin)
