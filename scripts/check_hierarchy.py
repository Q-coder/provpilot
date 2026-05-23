#!/usr/bin/env python3
"""Check node hierarchy and scene graph of our converted GLB."""
import json, struct
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

glb = Path('/Users/gery/code/provpilot/provpilot/Assets/custom/ps-airport-lspf-schmerlat/converted/Hangar_rechts.glb').read_bytes()
gltf, bin_data = parse_glb(glb)

nodes = gltf.get('nodes', [])
scenes = gltf.get('scenes', [])

print("Scenes:")
for i, sc in enumerate(scenes):
    print(f"  scene[{i}] '{sc.get('name','')}': root_nodes={sc.get('nodes', [])}")

print(f"\nNode hierarchy ({len(nodes)} nodes):")
# Build parent map
parent_of = {}
for ni, node in enumerate(nodes):
    for child in node.get('children', []):
        parent_of[child] = ni

def print_node(idx, depth=0):
    node = nodes[idx]
    indent = "  " * depth
    mesh = node.get('mesh')
    t = node.get('translation', None)
    r = node.get('rotation', None) 
    s = node.get('scale', None)
    parts = []
    if t: parts.append(f"T=[{t[0]:.2f},{t[1]:.2f},{t[2]:.2f}]")
    if r: parts.append(f"R=[{r[0]:.3f},{r[1]:.3f},{r[2]:.3f},{r[3]:.3f}]")
    if s: parts.append(f"S=[{s[0]:.3f},{s[1]:.3f},{s[2]:.3f}]")
    mesh_str = f" mesh={mesh}" if mesh is not None else ""
    print(f"{indent}[{idx}] '{node.get('name','')}':{mesh_str} {' '.join(parts)}")
    for child in node.get('children', []):
        print_node(child, depth + 1)

# Print from scene roots
for sc in scenes:
    for root in sc.get('nodes', []):
        print_node(root)

# Also check: how many GLBs in the BGL?
bgl = Path('/Users/gery/code/provpilot/scenery/custom/ps-airport-lspf-schmerlat/scenery/global/scenery/modelLib.BGL').read_bytes()
magic = b'glTF'
count = 0
pos = 0
while True:
    p = bgl.find(magic, pos)
    if p == -1: break
    ver = struct.unpack_from('<I', bgl, p + 4)[0]
    length = struct.unpack_from('<I', bgl, p + 8)[0]
    if ver == 2 and length > 100 and p + length <= len(bgl):
        # Parse just the JSON to get mesh/node counts
        inner_json, _ = parse_glb(bgl[p:p+length])
        meshes = inner_json.get('meshes', [])
        inner_nodes = inner_json.get('nodes', [])
        first_node = inner_nodes[0].get('name', '?') if inner_nodes else '?'
        print(f"\nBGL GLB[{count}] at offset {p}: {length} bytes, "
              f"{len(meshes)} meshes, {len(inner_nodes)} nodes, first_node='{first_node}'")
        count += 1
        pos = p + length
    else:
        pos = p + 1
print(f"\nTotal GLBs in BGL: {count}")

# Find the one that matches Hangar_rechts
print("\n--- Looking for Hangar_rechts GLB ---")
pos = 0
while True:
    p = bgl.find(magic, pos)
    if p == -1: break
    ver = struct.unpack_from('<I', bgl, p + 4)[0]
    length = struct.unpack_from('<I', bgl, p + 8)[0]
    if ver == 2 and length > 100 and p + length <= len(bgl):
        inner_json, inner_bin = parse_glb(bgl[p:p+length])
        inner_nodes = inner_json.get('nodes', [])
        node_names = [n.get('name', '') for n in inner_nodes]
        if 'Hangar_rechts' in node_names:
            print(f"Found at offset {p}, {length} bytes")
            print(f"  Nodes: {len(inner_nodes)}")
            print(f"  Meshes: {len(inner_json.get('meshes', []))}")
            print(f"  Accessors: {len(inner_json.get('accessors', []))}")
            print(f"  BufferViews: {len(inner_json.get('bufferViews', []))}")
            
            # Show mesh[0] structure
            mesh0 = inner_json['meshes'][0]
            print(f"\n  mesh[0] '{mesh0.get('name','')}': {len(mesh0['primitives'])} prims")
            for pi, prim in enumerate(mesh0['primitives'][:5]):
                asobo = prim.get('extras', {}).get('ASOBO_primitive', {})
                idx_acc = inner_json['accessors'][prim['indices']]
                print(f"    prim[{pi}]: indices_count={idx_acc['count']}")
                if asobo:
                    print(f"      ASOBO: StartIndex={asobo.get('StartIndex',0)}, PrimCount={asobo.get('PrimitiveCount')}")
            
            # Show first 3 original indices
            idx_acc = inner_json['accessors'][mesh0['primitives'][0]['indices']]
            idx_bv = inner_json['bufferViews'][idx_acc['bufferView']]
            comp = idx_acc['componentType']
            fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
            sz = struct.calcsize(f'<{fmt}')
            bv_off = idx_bv.get('byteOffset', 0)
            acc_off = idx_acc.get('byteOffset', 0)
            orig_indices = []
            for i in range(min(18, idx_acc['count'])):
                val = struct.unpack_from(f'<{fmt}', inner_bin, bv_off + acc_off + i * sz)[0]
                orig_indices.append(val)
            print(f"\n  Original first 18 indices: {orig_indices}")
            
            # Read ASOBO-relative indices for prim[0]
            asobo_0 = mesh0['primitives'][0].get('extras', {}).get('ASOBO_primitive', {})
            if asobo_0:
                start = asobo_0.get('StartIndex', 0)
                pcount = asobo_0.get('PrimitiveCount', 0)
                asob_indices = []
                for i in range(min(18, pcount * 3)):
                    val = struct.unpack_from(f'<{fmt}', inner_bin, bv_off + acc_off + (start + i) * sz)[0]
                    asob_indices.append(val)
                print(f"  ASOBO prim[0] indices (start={start}, count={pcount*3}): {asob_indices}")
            
            # Read first 5 original positions
            pos_acc_idx = mesh0['primitives'][0]['attributes']['POSITION']
            pos_acc = inner_json['accessors'][pos_acc_idx]
            pos_bv = inner_json['bufferViews'][pos_acc['bufferView']]
            stride = pos_bv.get('byteStride', 12)
            print(f"\n  Original first 5 positions (stride={stride}):")
            for v in range(5):
                base = pos_bv.get('byteOffset', 0) + v * stride + pos_acc.get('byteOffset', 0)
                px, py, pz = struct.unpack_from('<3f', inner_bin, base)
                print(f"    v{v}: ({px:.6f}, {py:.6f}, {pz:.6f})")
            break
        pos = p + length
    else:
        pos = p + 1
