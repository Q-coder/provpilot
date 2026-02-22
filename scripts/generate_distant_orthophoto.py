#!/usr/bin/env python3
"""
Generate a low-resolution orthophoto color map for the distant terrain.

Fetches SWISSIMAGE via the Swisstopo WMS service as a single request at
the distant terrain resolution (~256 m/pixel), producing a PNG that replaces
the procedural elevation-band color map used by distant_terrain.gd.

The output file is `distant_terrain_color.png` and is placed alongside the
existing distant_terrain.raw heightmap and distant_terrain_meta.json.

Usage:
    python scripts/generate_distant_orthophoto.py

    # Custom bounds (defaults from distant_terrain_meta.json):
    python scripts/generate_distant_orthophoto.py \\
        --terrain-dir scenery/terrain \\
        --output scenery/terrain/distant_terrain_color.png

Requirements:
    pip install requests Pillow
"""

import argparse
import json
import os
import sys
from io import BytesIO
from pathlib import Path

try:
    import requests
    from PIL import Image
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install requests Pillow")
    sys.exit(1)


# Swisstopo WMS endpoint for SWISSIMAGE
WMS_BASE = "https://wms.geo.admin.ch/"
WMS_LAYER = "ch.swisstopo.swissimage-product"

# Maximum pixels per WMS request dimension (Swisstopo limit is 4096)
MAX_WMS_SIZE = 2048


def fetch_wms_image(bbox, width, height, crs="EPSG:2056"):
    """
    Fetch an image from the Swisstopo WMS.

    bbox: (min_e, min_n, max_e, max_n) in the specified CRS
    width, height: output image dimensions in pixels
    """
    params = {
        "SERVICE": "WMS",
        "VERSION": "1.3.0",
        "REQUEST": "GetMap",
        "LAYERS": WMS_LAYER,
        "CRS": crs,
        "BBOX": f"{bbox[0]},{bbox[1]},{bbox[2]},{bbox[3]}",
        "WIDTH": str(width),
        "HEIGHT": str(height),
        "FORMAT": "image/jpeg",
        "STYLES": "",
    }

    print(f"  Fetching WMS: {width}×{height} px, "
          f"BBOX=[{bbox[0]:.0f}, {bbox[1]:.0f}, {bbox[2]:.0f}, {bbox[3]:.0f}]")

    resp = requests.get(WMS_BASE, params=params, timeout=120)
    resp.raise_for_status()

    content_type = resp.headers.get("Content-Type", "")
    if "image" not in content_type:
        # WMS error — likely returned XML
        print(f"  ERROR: WMS returned {content_type}")
        print(f"  Response: {resp.text[:500]}")
        return None

    img = Image.open(BytesIO(resp.content))
    return img


def fetch_tiled_wms(bbox, target_width, target_height, crs="EPSG:2056"):
    """
    Fetch a large area via multiple WMS requests if needed, then stitch.

    Splits into tiles if either dimension exceeds MAX_WMS_SIZE.
    """
    if target_width <= MAX_WMS_SIZE and target_height <= MAX_WMS_SIZE:
        return fetch_wms_image(bbox, target_width, target_height, crs)

    # Split into tiles
    tiles_x = (target_width + MAX_WMS_SIZE - 1) // MAX_WMS_SIZE
    tiles_y = (target_height + MAX_WMS_SIZE - 1) // MAX_WMS_SIZE

    e_min, n_min, e_max, n_max = bbox
    e_range = e_max - e_min
    n_range = n_max - n_min

    print(f"  Splitting into {tiles_x}×{tiles_y} WMS requests...")

    result = Image.new("RGB", (target_width, target_height))

    for ty in range(tiles_y):
        for tx in range(tiles_x):
            # Pixel range for this tile
            px_x0 = tx * MAX_WMS_SIZE
            px_x1 = min(px_x0 + MAX_WMS_SIZE, target_width)
            px_y0 = ty * MAX_WMS_SIZE
            px_y1 = min(px_y0 + MAX_WMS_SIZE, target_height)

            tile_w = px_x1 - px_x0
            tile_h = px_y1 - px_y0

            # Convert pixel range to geographic bounds
            # Note: WMS BBOX for EPSG:2056 is (min_e, min_n, max_e, max_n)
            # Pixel columns go W→E (left to right)
            # Pixel rows go N→S (top to bottom in image)
            tile_e_min = e_min + (px_x0 / target_width) * e_range
            tile_e_max = e_min + (px_x1 / target_width) * e_range
            # Image row 0 = top = north
            tile_n_max = n_max - (px_y0 / target_height) * n_range
            tile_n_min = n_max - (px_y1 / target_height) * n_range

            tile_bbox = (tile_e_min, tile_n_min, tile_e_max, tile_n_max)
            tile_img = fetch_wms_image(tile_bbox, tile_w, tile_h, crs)

            if tile_img:
                result.paste(tile_img, (px_x0, px_y0))
            else:
                print(f"  WARNING: Failed to fetch tile ({tx}, {ty})")

    return result


def main():
    parser = argparse.ArgumentParser(
        description="Generate low-res orthophoto for distant terrain "
                    "from Swisstopo WMS"
    )
    parser.add_argument(
        "--terrain-dir", default="scenery/terrain",
        help="Path to terrain directory with distant_terrain_meta.json "
             "(default: scenery/terrain)"
    )
    parser.add_argument(
        "--output", default=None,
        help="Output PNG path (default: <terrain-dir>/distant_terrain_color.png)"
    )
    parser.add_argument(
        "--backup", action="store_true", default=True,
        help="Backup existing color map before overwriting (default: true)"
    )
    parser.add_argument(
        "--no-backup", action="store_false", dest="backup",
        help="Don't backup existing color map"
    )
    args = parser.parse_args()

    terrain_dir = Path(args.terrain_dir)

    # Load distant terrain metadata for bounds and dimensions
    meta_path = terrain_dir / "distant_terrain_meta.json"
    if not meta_path.exists():
        print(f"ERROR: Metadata not found: {meta_path}")
        print("Run generate_distant_terrain.py first.")
        return 1

    with open(meta_path) as f:
        meta = json.load(f)

    cols = meta["cols"]       # 1368
    rows = meta["rows"]       # 896
    e_min = meta["swiss_e_min"]  # 2484224
    e_max = meta["swiss_e_max"]  # 2834432
    n_min = meta["swiss_n_min"]  # 1069056
    n_max = meta["swiss_n_max"]  # 1298432
    resolution = meta["resolution_m"]  # 256

    print("=" * 60)
    print("Distant Terrain Orthophoto Generator")
    print("=" * 60)
    print(f"\nCoverage: {(e_max - e_min)/1000:.0f} km × "
          f"{(n_max - n_min)/1000:.0f} km")
    print(f"Grid: {cols} × {rows} pixels at {resolution} m/pixel")
    print(f"Source: Swisstopo WMS ({WMS_LAYER})")

    # Determine output path
    output_path = Path(args.output) if args.output else terrain_dir / "distant_terrain_color.png"

    # Backup existing color map
    if args.backup and output_path.exists():
        backup_path = output_path.with_suffix(".elevation.png")
        if not backup_path.exists():
            import shutil
            shutil.copy2(output_path, backup_path)
            print(f"\nBacked up existing color map to {backup_path}")

    # Fetch the orthophoto via WMS
    # The distant terrain heightmap orientation is:
    #   pixel_0_0 = SW corner (n_min, e_min)
    #   col = West → East
    #   row = South → North
    #
    # WMS returns images with standard orientation:
    #   pixel_0_0 = NW corner (top-left)
    #   col = West → East
    #   row = North → South (top to bottom)
    #
    # So we need to flip vertically after downloading.

    print(f"\n-- Fetching orthophoto from WMS --")
    bbox = (e_min, n_min, e_max, n_max)

    img = fetch_tiled_wms(bbox, cols, rows)
    if img is None:
        print("ERROR: Failed to fetch orthophoto from WMS")
        return 1

    print(f"  Received: {img.size[0]}×{img.size[1]}, mode={img.mode}")

    # Flip vertically: WMS returns North-up (row 0 = north),
    # but our grid expects row 0 = south.
    from PIL import ImageOps
    img = ImageOps.flip(img)

    # Ensure RGB
    if img.mode != "RGB":
        img = img.convert("RGB")

    # Save
    img.save(output_path, "PNG")
    file_size = output_path.stat().st_size
    print(f"\nSaved: {output_path} ({file_size / 1024:.0f} KB)")

    print(f"\n{'=' * 60}")
    print("DONE! The distant terrain will now use real aerial imagery.")
    print("No changes needed in distant_terrain.gd — it loads the same file.")
    print(f"{'=' * 60}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
