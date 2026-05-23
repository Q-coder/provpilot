#!/usr/bin/env python3
"""Export first few meshes from our GLB as OBJ for visual inspection,
and also do geometry sanity checks."""
import json, struct, math
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

def read_accessor_floats(gltf, bin_data, acc_idx, expected_comps=3):
    """Read an accessor's data as floats."""
    acc = gltf['accessors'][acc_idx]
    bv = gltf['bufferViews'][acc['bufferView']]
    bv_off = bv.get('byteOffset', 0)
    acc_off = acc.get('byteOffset', 0)
    stride = bv.get('byteStride', expected_comps * 4)
    count = acc['count']
    comp = acc['componentType']
    
    result = []
    for i in range(count):
        base = bv_off + acc_off + i * stride
        if comp == 5126:  # FLOAT
            vals = struct.unpack_from(f'<{expected_comps}f', bin_data, base)
        else:
            vals = tuple([0.0] * expected_comps)
        result.append(vals)
    return result

def read_accessor_indices(gltf, bin_data, acc_idx):
    """Read index accessor."""
    acc = gltf['accessors'][acc_idx]
    bv = gltf['bufferViews'][acc['bufferView']]
    bv_off = bv.get('byteOffset', 0)
    acc_off = acc.get('byteOffset', 0)
    comp = acc['componentType']
    fmt = {5121: 'B', 5123: 'H', 5125: 'I'}.get(comp, 'H')
    sz = struct.calcsize(f'<{fmt}')
    
    result = []
    for i in range(acc['count']):
        val = struct.unpack_from(f'<{fmt}', bin_data, bv_off + acc_off + i * sz)[0]
        result.append(val)
    return result

def apply_transform(positions, translation, rotation, scale):
    """Apply TRS transform to positions."""
    # Apply scale
    scaled = [(p[0]*scale[0], p[1]*scale[1], p[2]*scale[2]) for p in positions]
    
    # Apply rotation (quaternion)
    if rotation:
        qx, qy, qz, qw = rotation
        result = []
        for x, y, z in scaled:
            # Quaternion rotation: q * v * q^-1
            # Using the formula for rotating a vector by a quaternion
            ix = qw*x + qy*z - qz*y
            iy = qw*y + qz*x - qx*z
            iz = qw*z + qx*y - qy*x
            iw = -qx*x - qy*y - qz*z
            rx = ix*qw + iw*(-qx) + iy*(-qz) - iz*(-qy)
            ry = iy*qw + iw*(-qy) + iz*(-qx) - ix*(-qz)
            rz = iz*qw + iw*(-qz) + ix*(-qy) - iy*(-qx)
            result.append((rx, ry, rz))
        scaled = result
    
    # Apply translation
    tx, ty, tz = translation
    return [(p[0]+tx, p[1]+ty, p[2]+tz) for p in scaled]

# Load our converted GLB
glb = Path('/Users/gery/code/provpilot/provpilot/Assets/custom/ps-airport-lspf-schmerlat/converted/Hangar_rechts.glb').read_bytes()
gltf, bin_data = parse_glb(glb)

nodes = gltf['nodes']
meshes = gltf['meshes']
accs = gltf['accessors']
bvs = gltf['bufferViews']

# Export OBJ for first 5 mesh nodes (with transforms)
obj_lines = ["# Converted Hangar_rechts.glb - geometry check\n"]
vertex_offset = 0

print("=" * 60)
print("GEOMETRY SANITY CHECK")
print("=" * 60)

total_degen = 0
for ni, node in enumerate(nodes[:20]):
    mesh_idx = node.get('mesh')
    if mesh_idx is None:
        continue
    
    mesh = meshes[mesh_idx]
    mesh_name = mesh.get('name', f'mesh_{mesh_idx}')
    node_name = node.get('name', f'node_{ni}')
    
    t = node.get('translation', [0,0,0])
    r = node.get('rotation', None)
    s = node.get('scale', [1,1,1])
    
    for pi, prim in enumerate(mesh['primitives']):
        attrs = prim.get('attributes', {})
        pos_acc = attrs.get('POSITION')
        idx_acc = prim.get('indices')
        if pos_acc is None or idx_acc is None:
            continue
        
        # Read positions
        n_comps = 3
        positions = read_accessor_floats(gltf, bin_data, pos_acc, n_comps)
        indices = read_accessor_indices(gltf, bin_data, idx_acc)
        
        # Apply node transform
        world_positions = apply_transform(positions, t, r, s)
        
        # Check for degenerate triangles
        degen_count = 0
        for ti in range(0, len(indices), 3):
            if ti + 2 >= len(indices):
                break
            i0, i1, i2 = indices[ti], indices[ti+1], indices[ti+2]
            if i0 >= len(positions) or i1 >= len(positions) or i2 >= len(positions):
                print(f"  ERROR: index out of range in {node_name}/prim{pi}")
                continue
            p0, p1, p2 = world_positions[i0], world_positions[i1], world_positions[i2]
            # Check for degenerate (zero-area) triangle
            e1 = (p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2])
            e2 = (p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2])
            cross = (e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0])
            area = math.sqrt(cross[0]**2 + cross[1]**2 + cross[2]**2) / 2.0
            if area < 1e-10:
                degen_count += 1
        
        # Bounding box
        mins = [float('inf')]*3
        maxs = [float('-inf')]*3
        for p in world_positions:
            for c in range(3):
                mins[c] = min(mins[c], p[c])
                maxs[c] = max(maxs[c], p[c])
        extent = [maxs[c] - mins[c] for c in range(3)]
        
        total_degen += degen_count
        print(f"  {node_name}/prim{pi}: {len(positions)} verts, {len(indices)//3} tris"
              f", {degen_count} degenerate"
              f", extent=({extent[0]:.1f}, {extent[1]:.1f}, {extent[2]:.1f})")
        
        # Write OBJ
        obj_lines.append(f"g {node_name}_prim{pi}\n")
        for p in world_positions:
            obj_lines.append(f"v {p[0]:.6f} {p[1]:.6f} {p[2]:.6f}\n")
        for ti in range(0, len(indices), 3):
            if ti + 2 >= len(indices):
                break
            i0, i1, i2 = indices[ti], indices[ti+1], indices[ti+2]
            obj_lines.append(f"f {i0+1+vertex_offset} {i1+1+vertex_offset} {i2+1+vertex_offset}\n")
        vertex_offset += len(world_positions)

print(f"\nTotal degenerate triangles: {total_degen}")

# Save OBJ
obj_path = Path('/Users/gery/code/provpilot/scripts/hangar_rechts_check.obj')
obj_path.write_text(''.join(obj_lines))
print(f"\nOBJ saved to: {obj_path}")
print(f"Total vertices: {vertex_offset}")

# Also save MCX reference as OBJ for comparison
print("\n\nMCX REFERENCE:")
mcx_gltf_path = Path('/Users/gery/code/provpilot/schmerlat/schmerlat_hauptgebaeude.gltf')
with open(mcx_gltf_path) as f:
    mcx_gltf = json.load(f)
mcx_bin = (mcx_gltf_path.parent / mcx_gltf['buffers'][0]['uri']).read_bytes()

mcx_obj = ["# MCX reference - schmerlat_hauptgebaeude\n"]
mcx_offset = 0
for mi, mesh in enumerate(mcx_gltf['meshes'][:5]):
    for pi, prim in enumerate(mesh['primitives']):
        attrs = prim.get('attributes', {})
        pos_acc = attrs.get('POSITION')
        idx_acc_idx = prim.get('indices')
        if pos_acc is None or idx_acc_idx is None:
            continue
        
        positions = read_accessor_floats(mcx_gltf, mcx_bin, pos_acc, 3)
        indices = read_accessor_indices(mcx_gltf, mcx_bin, idx_acc_idx)
        
        # MCX nodes all at origin, no transforms to apply
        mins = [float('inf')]*3
        maxs = [float('-inf')]*3
        for p in positions:
            for c in range(3):
                mins[c] = min(mins[c], p[c])
                maxs[c] = max(maxs[c], p[c])
        extent = [maxs[c] - mins[c] for c in range(3)]
        
        name = mesh.get('name', f'mesh_{mi}')
        print(f"  {name}/prim{pi}: {len(positions)} verts, {len(indices)//3} tris"
              f", extent=({extent[0]:.1f}, {extent[1]:.1f}, {extent[2]:.1f})")
