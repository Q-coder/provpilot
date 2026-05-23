#!/usr/bin/env python3
"""
Generate an orthophoto-based color map for distant terrain rendering.

Reads the 41,474 orthophoto JPEG tiles (1024×1024 @ 1m) produced by
process_swissimage_orthophoto.py and downsamples them into a single PNG
matching the distant terrain grid (distant_terrain_meta.json).

This replaces the elevation-band color map (distant_terrain_color.png)
with real satellite/aerial imagery, eliminating the visible color
mismatch at the Terrain3D ↔ distant terrain transition.

The result is written as distant_terrain_color.png (overwriting the
elevation-band version).  The existing heightmap and metadata are
not modified.

Orientation handling:
  Orthophoto tiles:  pixel(0,0) = SE corner, cols E→W, rows S→N
  Distant terrain:   pixel(0,0) = SW corner, cols W→E, rows S→N
  The orthophoto tiles are flipped horizontally (cols reversed)
  before being placed into the output grid.

Usage:
    python scripts/generate_distant_color_ortho.py \\
        [--ortho-dir scenery/orthophoto] \\
        [--terrain-dir scenery/terrain]  \\
        [--output scenery/terrain/distant_terrain_color.png]
"""

import argparse
import json
import os
import sys

import numpy as np
from tqdm import tqdm
from PIL import Image


def main():
    parser = argparse.ArgumentParser(
        description="Generate orthophoto-based distant terrain color map"
    )
    parser.add_argument(
        "--ortho-dir",
        default="scenery/orthophoto",
        help="Path to orthophoto tile directory (default: scenery/orthophoto)",
    )
    parser.add_argument(
        "--terrain-dir",
        default="scenery/terrain",
        help="Path to terrain directory containing distant_terrain_meta.json "
             "(default: scenery/terrain)",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="Output PNG path (default: <terrain-dir>/distant_terrain_color.png)",
    )
    args = parser.parse_args()

    ortho_dir = args.ortho_dir
    terrain_dir = args.terrain_dir
    output_path = args.output or os.path.join(terrain_dir, "distant_terrain_color.png")

    # ── Load distant terrain metadata ───────────────────────────────────
    meta_path = os.path.join(terrain_dir, "distant_terrain_meta.json")
    with open(meta_path) as f:
        meta = json.load(f)

    out_cols = meta["cols"]
    out_rows = meta["rows"]
    resolution = meta["resolution_m"]
    e_min = meta["swiss_e_min"]
    n_min = meta["swiss_n_min"]
    e_max = meta["swiss_e_max"]
    n_max = meta["swiss_n_max"]

    print(f"Distant terrain grid: {out_cols}×{out_rows} at {resolution}m/px")
    print(f"Swiss E [{e_min}, {e_max}), N [{n_min}, {n_max})")

    # ── Load orthophoto manifest ────────────────────────────────────────
    ortho_manifest_path = os.path.join(ortho_dir, "manifest.json")
    with open(ortho_manifest_path) as f:
        ortho_manifest = json.load(f)

    tile_size = ortho_manifest["tile_size_m"]  # 1024
    pixels_per_tile = ortho_manifest["pixels_per_tile"]  # 1024
    ortho_tiles = ortho_manifest["tiles"]
    print(f"Orthophoto tiles: {len(ortho_tiles)} available, "
          f"{tile_size}m @ {pixels_per_tile}px")

    # Output pixels per tile (how many output pixels one 1024m tile covers)
    out_per_tile = tile_size // resolution

    # Source pixels to average per output pixel
    src_per_out = resolution  # 1m/pixel source → 128 pixels per output pixel

    # ── Allocate output ─────────────────────────────────────────────────
    # RGB color map matching the distant terrain grid.
    # Initialize to the fog/haze color so missing tiles blend naturally.
    color_map = np.full((out_rows, out_cols, 3), [179, 191, 217], dtype=np.uint8)

    # ── Process tiles ───────────────────────────────────────────────────
    skipped = 0
    processed = 0

    for tile_key, tile_info in tqdm(ortho_tiles.items(), desc="Processing tiles"):
        tile_path = os.path.join(ortho_dir, tile_info["file"])
        if not os.path.exists(tile_path):
            skipped += 1
            continue

        swiss_e = tile_info["swiss_e"]
        swiss_n = tile_info["swiss_n"]

        # Output pixel position for this tile's SW corner.
        # Distant terrain grid: col 0 = e_min (west), row 0 = n_min (south).
        out_col_base = (swiss_e - e_min) // resolution
        out_row_base = (swiss_n - n_min) // resolution

        # Quick bounds check (skip tiles outside the distant terrain grid)
        if (out_col_base + out_per_tile <= 0 or out_col_base >= out_cols or
                out_row_base + out_per_tile <= 0 or out_row_base >= out_rows):
            skipped += 1
            continue

        try:
            img = Image.open(tile_path)
        except Exception:
            skipped += 1
            continue

        tile_data = np.array(img)
        # tile_data shape: (1024, 1024, 3) — RGB
        # Orthophoto orientation: pixel(0,0) = SE corner
        #   rows: S→N (row 0 = south, same as distant terrain)
        #   cols: E→W (col 0 = east, opposite of distant terrain)
        # Flip columns to match distant terrain's W→E col direction.
        tile_data = np.flip(tile_data, axis=1)  # Now col 0 = west

        # Downsample: average each src_per_out × src_per_out block
        for dr in range(out_per_tile):
            for dc in range(out_per_tile):
                out_r = int(out_row_base) + dr
                out_c = int(out_col_base) + dc

                if out_r < 0 or out_r >= out_rows:
                    continue
                if out_c < 0 or out_c >= out_cols:
                    continue

                src_r0 = dr * src_per_out
                src_r1 = min(src_r0 + src_per_out, pixels_per_tile)
                src_c0 = dc * src_per_out
                src_c1 = min(src_c0 + src_per_out, pixels_per_tile)

                block = tile_data[src_r0:src_r1, src_c0:src_c1]
                if block.size > 0:
                    avg = block.reshape(-1, 3).mean(axis=0).astype(np.uint8)
                    color_map[out_r, out_c] = avg

        processed += 1

    print(f"Processed {processed} tiles, skipped {skipped}")

    # ── Save ────────────────────────────────────────────────────────────
    out_img = Image.fromarray(color_map, "RGB")
    out_img.save(output_path)
    file_size_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"Saved: {output_path} ({out_cols}×{out_rows}, {file_size_mb:.1f} MB)")


if __name__ == "__main__":
    main()
