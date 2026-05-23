#!/usr/bin/env python3
"""Check mip manifest format and tile position consistency."""
import json, struct, sys, os
from pathlib import Path

SCENERY = Path(__file__).resolve().parent.parent / "scenery"

# Check manifest format
for mip in [2, 3, 4]:
    mf = json.load(open(SCENERY / "terrain" / f"mip{mip}" / "manifest.json"))
    print(f"mip{mip}: pixels_per_tile={mf['pixels_per_tile']}, pixel_res={mf['pixel_resolution_m']}m")
    
    # Find a tile near Lake Zürich (E~2683000, N~1247000)
    for key, t in mf["tiles"].items():
        if 2683000 <= t["swiss_e"] <= 2684000 and 1246000 <= t["swiss_n"] <= 1248000:
            print(f"  tile {key}: swiss_e={t['swiss_e']}, swiss_n={t['swiss_n']}, file={t['file']}")
            break

# Source manifest
m0 = json.load(open(SCENERY / "terrain" / "manifest.json"))
print(f"\nSource: pixels_per_tile={m0['pixels_per_tile']}")
for key, t in m0["tiles"].items():
    if 2683000 <= t["swiss_e"] <= 2684000 and 1246000 <= t["swiss_n"] <= 1248000:
        print(f"  tile {key}: swiss_e={t['swiss_e']}, swiss_n={t['swiss_n']}")
        break

# Verify a specific height value matches between source and mip2
# Sample point: center of Lake Zürich, E=2684000, N=1247000
TEST_E, TEST_N = 2684000, 1247000

def sample_from_source(swiss_e, swiss_n):
    m = json.load(open(SCENERY / "terrain" / "manifest.json"))
    for t in m["tiles"].values():
        te, tn = t["swiss_e"], t["swiss_n"]
        if te <= swiss_e < te + 1024 and tn <= swiss_n < tn + 1024:
            data = (SCENERY / "terrain" / t["file"]).read_bytes()
            px = m["pixels_per_tile"]
            # pixel(0,0) at SE corner, cols E->W, rows S->N
            col = int((te + 1024 - swiss_e) / 1.0)  # 1m/px
            row = int((swiss_n - tn) / 1.0)
            col = max(0, min(col, px-1))
            row = max(0, min(row, px-1))
            idx = (row * px + col) * 4
            return struct.unpack('<f', data[idx:idx+4])[0]
    return None

def sample_from_mip(mip_level, swiss_e, swiss_n):
    m = json.load(open(SCENERY / "terrain" / f"mip{mip_level}" / "manifest.json"))
    px = m["pixels_per_tile"]
    pres = m["pixel_resolution_m"]
    for t in m["tiles"].values():
        te, tn = t["swiss_e"], t["swiss_n"]
        if te <= swiss_e < te + 1024 and tn <= swiss_n < tn + 1024:
            data = (SCENERY / "terrain" / f"mip{mip_level}" / t["file"]).read_bytes()
            col = int((te + 1024 - swiss_e) / pres)
            row = int((swiss_n - tn) / pres)
            col = max(0, min(col, px-1))
            row = max(0, min(row, px-1))
            idx = (row * px + col) * 4
            return struct.unpack('<f', data[idx:idx+4])[0]
    return None

print(f"\nHeight at E={TEST_E}, N={TEST_N}:")
h0 = sample_from_source(TEST_E, TEST_N)
print(f"  Source (1m/px): {h0}")
for mip in [2, 3, 4]:
    h = sample_from_mip(mip, TEST_E, TEST_N)
    print(f"  mip{mip}: {h}")

# Now check the assembly math for Ring 0
# Simulate what _assemble_heightmap_texture does
print("\n--- Assembly simulation for Ring 0 ---")
terrain_center_e = 2684928.0  # from screenshot
terrain_center_n = 1252352.0
half_extent = 16384.0
pixel_res = 16.0
TEX_SIZE = 2048

tex_east = terrain_center_e + half_extent  # 2701312
tex_south = terrain_center_n - half_extent  # 1235968
tex_west = tex_east - TEX_SIZE * pixel_res  # 2668544
tex_north = tex_south + TEX_SIZE * pixel_res  # 1268736

print(f"Texture covers: E=[{tex_west:.0f}, {tex_east:.0f}], N=[{tex_south:.0f}, {tex_north:.0f}]")

# For our test point at E=2684000, N=1247000:
# What texture pixel should it land on?
tex_col = (tex_east - TEST_E) / pixel_res
tex_row = (TEST_N - tex_south) / pixel_res
print(f"Test point E={TEST_E}, N={TEST_N} -> texture pixel ({tex_col:.1f}, {tex_row:.1f})")
print(f"  UV = ({tex_col/TEX_SIZE:.4f}, {tex_row/TEX_SIZE:.4f})")

# What mesh vertex (in Godot coords) corresponds to this point?
godot_x = terrain_center_e - TEST_E  # = 928
godot_z = TEST_N - terrain_center_n  # = -5352
mesh_uv_x = (godot_x + half_extent) / (2 * half_extent)
mesh_uv_y = (godot_z + half_extent) / (2 * half_extent)
print(f"  Godot position: ({godot_x:.0f}, 0, {godot_z:.0f})")
print(f"  Mesh UV = ({mesh_uv_x:.4f}, {mesh_uv_y:.4f})")

# Check if they match
print(f"\n  UV match X: {'YES' if abs(tex_col/TEX_SIZE - mesh_uv_x) < 0.001 else 'NO - MISMATCH!'}")
print(f"  UV match Y: {'YES' if abs(tex_row/TEX_SIZE - mesh_uv_y) < 0.001 else 'NO - MISMATCH!'}")
