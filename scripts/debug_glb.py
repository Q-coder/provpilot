#!/usr/bin/env python3
"""Debug script to inspect road GLB vertex positions."""
import struct, json, numpy as np

with open('scenery/roads/manifest.json') as f:
    m = json.load(f)

# Aircraft at Swiss E~2684138 N~1281473 (from Godot pos -1258, 618, -1599)
ac_e, ac_n = 2684138, 1281473
tiles_by_dist = []
for tid, info in m['tiles'].items():
    dist = ((info['center_e'] - ac_e)**2 + (info['center_n'] - ac_n)**2)**0.5
    tiles_by_dist.append((dist, tid, info))
tiles_by_dist.sort()

print('Closest tiles to aircraft:')
for dist, tid, info in tiles_by_dist[:5]:
    print(f'  {tid}: dist={dist:.0f}m center=({info["center_e"]}, {info["center_n"]}) segs={info["segments"]}')

# Read the closest GLB
closest = tiles_by_dist[0]
glb_path = f'scenery/roads/{closest[2]["file"]}'
print(f'\nExamining: {glb_path}')

with open(glb_path, 'rb') as f:
    magic = f.read(4)
    version = struct.unpack('<I', f.read(4))[0]
    total = struct.unpack('<I', f.read(4))[0]
    json_len = struct.unpack('<I', f.read(4))[0]
    json_type = struct.unpack('<I', f.read(4))[0]
    json_data = json.loads(f.read(json_len))
    bin_len = struct.unpack('<I', f.read(4))[0]
    bin_type = struct.unpack('<I', f.read(4))[0]
    bin_data = f.read(bin_len)

pos_acc = json_data['accessors'][1]
pos_bv = json_data['bufferViews'][1]
pos_offset = pos_bv['byteOffset']
pos_count = pos_acc['count']
pos_data = np.frombuffer(bin_data, dtype=np.float32, count=pos_count*3, offset=pos_offset).reshape(-1, 3)

print(f'Vertices: {pos_count}')
print(f'Position min: {pos_data.min(axis=0)}')
print(f'Position max: {pos_data.max(axis=0)}')
print(f'Position mean: {pos_data.mean(axis=0)}')
print(f'GLTF accessor min: {pos_acc["min"]}')
print(f'GLTF accessor max: {pos_acc["max"]}')
print()
print(f'Aircraft Godot pos: (-1258, 618, -1599)')
print(f'Road Godot X range: {pos_data[:,0].min():.1f} to {pos_data[:,0].max():.1f}')
print(f'Road Godot Y range: {pos_data[:,1].min():.1f} to {pos_data[:,1].max():.1f} (elevation)')
print(f'Road Godot Z range: {pos_data[:,2].min():.1f} to {pos_data[:,2].max():.1f}')
print(f'First 5 vertices:')
for v in pos_data[:5]:
    print(f'  ({v[0]:.1f}, {v[1]:.1f}, {v[2]:.1f})')

# Check normals
norm_acc = json_data['accessors'][2]
norm_bv = json_data['bufferViews'][2]
norm_offset = norm_bv['byteOffset']
norm_count = norm_acc['count']
norm_data = np.frombuffer(bin_data, dtype=np.float32, count=norm_count*3, offset=norm_offset).reshape(-1, 3)
print(f'\nNormal sample: {norm_data[0]}')

# Check indices
idx_acc = json_data['accessors'][0]
idx_bv = json_data['bufferViews'][0]
idx_count = idx_acc['count']
idx_data = np.frombuffer(bin_data, dtype=np.uint32, count=idx_count, offset=0)
print(f'Triangle count: {idx_count // 3}')
print(f'Index max: {idx_data.max()} (should be < {pos_count})')
print(f'First 6 indices: {idx_data[:6]}')
