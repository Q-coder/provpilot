#!/usr/bin/env python3
"""
Generate a low-resolution heightmap and color map for distant terrain rendering.

Reads all 42,400 high-res terrain tiles (1024x1024 @ 1m) from scenery/terrain/
and produces:
  1. A downsampled heightmap at 256m per pixel (float32 raw, little-endian)
  2. A color map based on elevation bands (PNG, RGB)
  3. A metadata JSON file

The output is used by distant_terrain.gd to render terrain beyond the Terrain3D
region grid, providing a horizon that extends 100-200km.

Usage:
    python scripts/generate_distant_terrain.py [--resolution 256] [--output scenery/terrain]

Tile orientation (from manifest):
  - pixel_0_0: "SE corner of Swiss tile"
  - col_direction: "East to West (Godot +X)"
  - row_direction: "South to North (Godot +Z)"
"""

import argparse
import json
import os
import struct
import sys
from pathlib import Path

import numpy as np
from tqdm import tqdm


def load_manifest(terrain_dir: str) -> dict:
    """Load the terrain manifest.json."""
    manifest_path = os.path.join(terrain_dir, "manifest.json")
    with open(manifest_path, "r") as f:
        return json.load(f)


def compute_grid_bounds(manifest: dict) -> dict:
    """Compute the Swiss coordinate bounds from all tiles."""
    tiles = manifest["tiles"]
    e_coords = []
    n_coords = []
    for tile_info in tiles.values():
        e_coords.append(tile_info["swiss_e"])
        n_coords.append(tile_info["swiss_n"])

    tile_size = manifest["tile_size_m"]
    return {
        "e_min": min(e_coords),
        "e_max": max(e_coords) + tile_size,  # +tile_size for upper bound
        "n_min": min(n_coords),
        "n_max": max(n_coords) + tile_size,
        "tile_size": tile_size,
    }


def generate_distant_heightmap(
    terrain_dir: str,
    output_dir: str,
    resolution: int = 256,
) -> None:
    """
    Generate a low-resolution heightmap from all terrain tiles.

    Each output pixel represents a resolution x resolution meter area.
    Height values are the average of the high-res source data.
    Missing tiles get height = 0 (sea level / below terrain).
    """
    print(f"Loading manifest from {terrain_dir}...")
    manifest = load_manifest(terrain_dir)
    tiles = manifest["tiles"]
    tile_size = manifest["tile_size_m"]  # 1024
    pixels_per_tile = manifest["pixels_per_tile"]  # 1024

    bounds = compute_grid_bounds(manifest)
    print(f"Swiss bounds: E [{bounds['e_min']}, {bounds['e_max']}), "
          f"N [{bounds['n_min']}, {bounds['n_max']})")
    print(f"Coverage: {(bounds['e_max'] - bounds['e_min']) / 1000:.0f} km × "
          f"{(bounds['n_max'] - bounds['n_min']) / 1000:.0f} km")

    # Output dimensions
    width_m = bounds["e_max"] - bounds["e_min"]
    height_m = bounds["n_max"] - bounds["n_min"]
    out_cols = width_m // resolution  # E direction
    out_rows = height_m // resolution  # N direction
    print(f"Output: {out_cols} × {out_rows} pixels at {resolution}m/pixel")

    # Pixels per tile in output
    # Each 1024m tile = 1024/256 = 4 output pixels
    out_per_tile = tile_size // resolution

    # Source pixels to average per output pixel
    # Each output pixel = 256m = 256 source pixels per side
    src_per_out = resolution  # Since source is 1m/pixel

    # Allocate output: float32 heightmap, initialized to NaN (no data)
    heightmap = np.full((out_rows, out_cols), np.nan, dtype=np.float32)

    print(f"Processing {len(tiles)} tiles...")
    for tile_key, tile_info in tqdm(tiles.items(), desc="Reading tiles"):
        tile_file = os.path.join(terrain_dir, tile_info["file"])
        if not os.path.exists(tile_file):
            continue

        # Read raw float32 heightmap (1024×1024)
        data = np.fromfile(tile_file, dtype=np.float32)
        if data.size != pixels_per_tile * pixels_per_tile:
            continue
        tile_data = data.reshape((pixels_per_tile, pixels_per_tile))

        # Tile position in Swiss coords
        swiss_e = tile_info["swiss_e"]
        swiss_n = tile_info["swiss_n"]

        # Output pixel position for this tile
        # E dimension: swiss_e is the tile's easting origin
        # N dimension: swiss_n is the tile's northing origin
        out_col_start = (swiss_e - bounds["e_min"]) // resolution
        out_row_start = (swiss_n - bounds["n_min"]) // resolution

        # The tile data orientation:
        # pixel_0_0 = SE corner, cols go E→W, rows go S→N
        # So tile_data[0, 0] = SE corner
        # tile_data[row, col]: row=0 is south, row increases northward
        #                      col=0 is east, col increases westward

        # Downsample: average each src_per_out × src_per_out block
        for dr in range(out_per_tile):
            for dc in range(out_per_tile):
                src_r0 = dr * src_per_out
                src_r1 = min(src_r0 + src_per_out, pixels_per_tile)
                src_c0 = dc * src_per_out
                src_c1 = min(src_c0 + src_per_out, pixels_per_tile)

                block = tile_data[src_r0:src_r1, src_c0:src_c1]
                if block.size > 0:
                    avg_h = np.mean(block)
                    # Map to output position
                    # Row in output: south-to-north, same as tile row direction
                    out_r = out_row_start + dr
                    # Col in output: the tile's col goes E→W (col0=east)
                    # In our output, col0 = westernmost (e_min), so we need
                    # to reverse the column direction
                    out_c = out_col_start + (out_per_tile - 1 - dc)

                    if 0 <= out_r < out_rows and 0 <= out_c < out_cols:
                        heightmap[out_r, out_c] = avg_h

    # Statistics on no-data areas
    nan_count = np.count_nonzero(np.isnan(heightmap))
    print(f"Pixels with no data: {nan_count} / {out_rows * out_cols} "
          f"({100 * nan_count / (out_rows * out_cols):.1f}%)")

    # Fill NaN holes by iteratively averaging valid neighbours (4-connected).
    # This closes gaps in both the heightmap and the derived color map,
    # preventing visible seams in the distant terrain mesh.
    heightmap = _fill_nan_heightmap(heightmap)

    # Compute stats on valid data only.
    valid = heightmap[~np.isnan(heightmap)]
    if valid.size > 0:
        print(f"Elevation range: {valid.min():.0f}m - {valid.max():.0f}m")
    print(f"Total data points: {valid.size}")

    # Save heightmap as raw float32
    os.makedirs(output_dir, exist_ok=True)
    heightmap_path = os.path.join(output_dir, "distant_terrain.raw")
    heightmap.tofile(heightmap_path)
    print(f"Saved heightmap: {heightmap_path} ({os.path.getsize(heightmap_path) / 1024 / 1024:.1f} MB)")

    # Generate color map based on elevation bands
    color_map = generate_color_map(heightmap)
    try:
        from PIL import Image
        img = Image.fromarray(color_map, "RGB")
        color_path = os.path.join(output_dir, "distant_terrain_color.png")
        img.save(color_path)
        print(f"Saved color map: {color_path}")
    except ImportError:
        print("WARNING: PIL/Pillow not available, skipping color map PNG")
        # Save as raw RGB instead
        color_path = os.path.join(output_dir, "distant_terrain_color.raw")
        color_map.tofile(color_path)
        print(f"Saved color map (raw RGB): {color_path}")

    # Save metadata
    metadata = {
        "format": "float32_raw_le",
        "resolution_m": resolution,
        "cols": int(out_cols),
        "rows": int(out_rows),
        "swiss_e_min": int(bounds["e_min"]),
        "swiss_e_max": int(bounds["e_max"]),
        "swiss_n_min": int(bounds["n_min"]),
        "swiss_n_max": int(bounds["n_max"]),
        "orientation": {
            "pixel_0_0": "SW corner (n_min, e_min)",
            "col_direction": "West to East (+E)",
            "row_direction": "South to North (+N)",
        },
        "elevation_min": float(np.nanmin(heightmap)) if valid.size > 0 else 0.0,
        "elevation_max": float(np.nanmax(heightmap)) if valid.size > 0 else 0.0,
        "heightmap_file": "distant_terrain.raw",
        "color_file": "distant_terrain_color.png",
    }
    meta_path = os.path.join(output_dir, "distant_terrain_meta.json")
    with open(meta_path, "w") as f:
        json.dump(metadata, f, indent=2)
    print(f"Saved metadata: {meta_path}")


def _fill_nan_heightmap(heightmap: np.ndarray) -> np.ndarray:
    """
    Fill NaN holes by iteratively averaging valid 4-connected neighbours.

    Each pass fills the outermost ring of NaN pixels that border valid data.
    Runs until no more fillable pixels remain.  Uses numpy shifts so each
    pass over the ~1.2M pixel grid takes only a few milliseconds.
    """
    mask = np.isnan(heightmap)
    total_nan = np.count_nonzero(mask)
    if total_nan == 0:
        print("No NaN pixels to fill")
        return heightmap

    hm = heightmap.copy()
    filled_total = 0
    pass_num = 0

    while True:
        # For each NaN pixel, compute the sum and count of valid 4-neighbours
        valid = ~np.isnan(hm)
        valid_f = valid.astype(np.float32)

        # Shifted arrays (pad edges with 0 / False)
        def shift(arr, dr, dc):
            result = np.zeros_like(arr)
            sr = slice(max(0, -dr), arr.shape[0] - max(0, dr))
            sc = slice(max(0, -dc), arr.shape[1] - max(0, dc))
            tr = slice(max(0, dr), arr.shape[0] + min(0, dr))
            tc = slice(max(0, dc), arr.shape[1] + min(0, dc))
            result[tr, tc] = arr[sr, sc]
            return result

        # Zero out NaN values for summation
        hm_safe = np.where(valid, hm, 0.0)

        n_sum = (shift(hm_safe, -1, 0) + shift(hm_safe, 1, 0) +
                 shift(hm_safe, 0, -1) + shift(hm_safe, 0, 1))
        n_cnt = (shift(valid_f, -1, 0) + shift(valid_f, 1, 0) +
                 shift(valid_f, 0, -1) + shift(valid_f, 0, 1))

        fillable = np.isnan(hm) & (n_cnt > 0)
        n_fill = np.count_nonzero(fillable)
        if n_fill == 0:
            break

        hm[fillable] = n_sum[fillable] / n_cnt[fillable]
        filled_total += n_fill
        pass_num += 1

    remaining = np.count_nonzero(np.isnan(hm))
    print(f"Filled {filled_total} NaN pixels in {pass_num} passes "
          f"({remaining} unfillable)")
    return hm


def generate_color_map(heightmap: np.ndarray) -> np.ndarray:
    """
    Generate an RGB color map from elevation data.

    Color bands (Swiss terrain):
      0-300m:    Water/lowland blue (#4a90c4)
      300-600m:  Valley green (#5b8c3e)
      600-1000m: Forest green (#3d7a28)
      1000-1500m: Alpine meadow (#7ab648)
      1500-2000m: Sub-alpine (#a89060)
      2000-2800m: Rock gray (#8c8c7a)
      2800-3500m: High alpine gray (#b0a898)
      3500m+:    Snow white (#e8e8e8)
    """
    rows, cols = heightmap.shape
    color = np.zeros((rows, cols, 3), dtype=np.uint8)

    # Define elevation bands and colors (RGB)
    # Swiss terrain lowest point: ~193m (Lake Maggiore)
    # Darker greens to match realistic aerial/satellite appearance
    bands = [
        (0,    400,  (58, 95, 42)),     # Low valley — dark olive green
        (400,  700,  (48, 88, 35)),     # Valley/forest — dark forest green
        (700,  1200, (40, 78, 30)),     # Forest — deep green
        (1200, 1800, (82, 120, 48)),    # Alpine meadow — muted green
        (1800, 2400, (138, 120, 82)),   # Sub-alpine brown
        (2400, 3000, (130, 130, 115)),  # Rock gray
        (3000, 3800, (170, 162, 148)),  # High alpine
        (3800, 5000, (228, 228, 228)),  # Snow
    ]

    for low, high, rgb in bands:
        mask = (heightmap >= low) & (heightmap < high)
        # Add slight variation based on actual elevation within band
        t = np.clip((heightmap - low) / max(high - low, 1), 0, 1)
        for c in range(3):
            color[mask, c] = np.clip(
                rgb[c] + (t[mask] * 20 - 10).astype(np.int16),
                0, 255
            ).astype(np.uint8)

    # No-data areas (NaN): set to fog/sky color so they blend into the horizon
    no_data = np.isnan(heightmap)
    color[no_data] = [179, 191, 217]  # Matches fog_color in shader

    return color


def main():
    parser = argparse.ArgumentParser(
        description="Generate low-resolution distant terrain data"
    )
    parser.add_argument(
        "--terrain-dir",
        default="scenery/terrain",
        help="Path to terrain tile directory (default: scenery/terrain)",
    )
    parser.add_argument(
        "--output",
        default="scenery/terrain",
        help="Output directory (default: scenery/terrain, co-located with tiles)",
    )
    parser.add_argument(
        "--resolution",
        type=int,
        default=256,
        choices=[64, 128, 256, 512],
        help="Meters per output pixel (default: 256)",
    )
    args = parser.parse_args()

    generate_distant_heightmap(
        terrain_dir=args.terrain_dir,
        output_dir=args.output,
        resolution=args.resolution,
    )


if __name__ == "__main__":
    main()
