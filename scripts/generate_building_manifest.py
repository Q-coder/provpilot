#!/usr/bin/env python3
"""Generate buildings/manifest.json from existing building GLB files.

Reads the hardcoded tile centers from building_loader.gd (in Godot coords)
and converts them back to absolute Swiss LV95 coordinates for the manifest.
"""

import json
import os
import glob

# The terrain center and building origin used when the GLBs were created
# Schaffhausen buildings were converted with origin E 2681968, N 1282869
# Terrain center is E 2682880, N 1283072
BUILDING_ORIGIN_E = 2681968
BUILDING_ORIGIN_N = 1282869
TERRAIN_CENTER_E = 2682880
TERRAIN_CENTER_N = 1283072

# Hardcoded from building_loader.gd (Godot-space centers, before BUILDING_ORIGIN_OFFSET)
# These were computed as: Godot X = BUILDING_ORIGIN_E - Swiss_E, Godot Z = Swiss_N - BUILDING_ORIGIN_N
SCHAFFHAUSEN_TILES = [
    {"id": "1031-14", "center_godot": (2903, 2631)},
    {"id": "1031-21", "center_godot": (-4528, 2615)},  # not in current array - skip missing
    {"id": "1031-22", "center_godot": (-8925, 2638)},  # not in current array - skip missing
    {"id": "1031-23", "center_godot": (-1451, 2597)},
    {"id": "1031-24", "center_godot": (-5892, 2628)},
    {"id": "1031-32", "center_godot": (2914, -363)},
    {"id": "1031-34", "center_godot": (2934, -3347)},
    {"id": "1031-41", "center_godot": (-1517, -363)},
    {"id": "1031-42", "center_godot": (-5844, -361)},
    {"id": "1031-43", "center_godot": (-1331, -3078)},
    {"id": "1031-44", "center_godot": (-5884, -3368)},
    {"id": "1032-11", "center_godot": (-10243, 2636)},
    {"id": "1032-13", "center_godot": (-10202, -384)},
    {"id": "1032-31", "center_godot": (-10239, -3373)},
]

def main():
    scenery_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "scenery", "buildings")
    
    # Find all GLB files in the buildings directory
    glb_files = set()
    for f in glob.glob(os.path.join(scenery_dir, "*.glb")):
        basename = os.path.basename(f)
        # Extract tile ID: "buildings_1031-14.glb" -> "1031-14"
        tile_id = basename.replace("buildings_", "").replace(".glb", "")
        glb_files.add(tile_id)
    
    print(f"Found {len(glb_files)} GLB files: {sorted(glb_files)}")
    
    tiles = {}
    for tile in SCHAFFHAUSEN_TILES:
        tile_id = tile["id"]
        glb_name = f"buildings_{tile_id}.glb"
        
        if tile_id not in glb_files:
            print(f"  Skipping {tile_id} — no GLB file found")
            continue
        
        gx, gz = tile["center_godot"]
        
        # Reverse the Godot coordinate conversion:
        # The GLBs were built with origin at BUILDING_ORIGIN_E/N
        # Godot X = BUILDING_ORIGIN_E - Swiss_E  =>  Swiss_E = BUILDING_ORIGIN_E - Godot_X
        # Godot Z = Swiss_N - BUILDING_ORIGIN_N  =>  Swiss_N = BUILDING_ORIGIN_N + Godot_Z
        swiss_e = BUILDING_ORIGIN_E - gx
        swiss_n = BUILDING_ORIGIN_N + gz
        
        tiles[tile_id] = {
            "file": glb_name,
            "center_e": swiss_e,
            "center_n": swiss_n,
        }
        print(f"  {tile_id}: Swiss E={swiss_e}, N={swiss_n}")
    
    manifest = {
        "format": "glb",
        "source": "swissBUILDINGS3D",
        "conversion_origin_e": BUILDING_ORIGIN_E,
        "conversion_origin_n": BUILDING_ORIGIN_N,
        "note": "GLB models are positioned relative to conversion_origin. "
                "At runtime, the loader computes: "
                "Godot X = conversion_origin_e - center_e + (TERRAIN_CENTER_E - conversion_origin_e), "
                "which simplifies to Godot X = TERRAIN_CENTER_E - center_e. "
                "This makes centers origin-agnostic.",
        "tiles": tiles,
    }
    
    manifest_path = os.path.join(scenery_dir, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"\nWrote {manifest_path} with {len(tiles)} tiles")


if __name__ == "__main__":
    main()
