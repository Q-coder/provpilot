#!/usr/bin/env python3
"""
Generate orthophoto JPEG tiles for ProvPilot terrain via Swisstopo WMS.

For each 1024m output tile, sends a single WMS GetMap request to the
Swisstopo WMS service, which returns a 1024×1024 JPEG at 1 m/pixel.

This is far faster than downloading raw SWISSIMAGE GeoTIFF tiles:
  - No source tile discovery (no year-probing HEAD requests)
  - No merge step — one HTTP request per output tile
  - Automatically handles tile boundaries and resampling
  - Areas outside Switzerland return blacked-out images (skipped)

Fully resumable: existing JPEG files are skipped on re-run.

SwissTopo licensing: SWISSIMAGE has been released as Open Government Data
(OGD) since March 2021 — free for commercial and non-commercial use.

Requirements:
    pip install requests Pillow numpy tqdm
"""

import argparse
import json
import os
import sys
import time
from io import BytesIO
from pathlib import Path

try:
    import numpy as np
    import requests
    from PIL import Image, ImageOps
    from tqdm import tqdm
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install requests Pillow numpy tqdm")
    sys.exit(1)


# ── Configuration ────────────────────────────────────────────────────────────

TILE_SIZE = 1024      # Output tile size in meters (matches terrain)

# Swisstopo WMS endpoint for SWISSIMAGE
WMS_BASE = "https://wms.geo.admin.ch/"
WMS_LAYER = "ch.swisstopo.swissimage-product"

# WMS request timeout
WMS_TIMEOUT = 30

# Minimum non-black pixel fraction to keep a tile (skip empty/border tiles)
MIN_DATA_FRACTION = 0.05

# Rate limiting: minimum seconds between WMS requests
# (be nice to the free public service)
MIN_REQUEST_INTERVAL = 0.05


# ── WMS Fetching ─────────────────────────────────────────────────────────────

def fetch_tile_wms(bbox, width, height, session, crs="EPSG:2056"):
    """
    Fetch a single tile from the Swisstopo WMS.

    bbox: (min_e, min_n, max_e, max_n) in EPSG:2056
    width, height: output image dimensions in pixels
    Returns: PIL Image or None on failure
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

    try:
        resp = session.get(WMS_BASE, params=params, timeout=WMS_TIMEOUT)
        resp.raise_for_status()

        content_type = resp.headers.get("Content-Type", "")
        if "image" not in content_type:
            return None

        img = Image.open(BytesIO(resp.content))
        return img

    except (requests.RequestException, Exception):
        return None


def is_tile_empty(img, threshold=MIN_DATA_FRACTION):
    """
    Check if a tile is mostly empty (outside Switzerland coverage).
    WMS returns white (255,255,255) for uncovered areas.
    Returns True if the tile should be skipped.
    """
    arr = np.asarray(img)
    # A pixel has real data if it's neither near-black nor near-white
    is_black = np.all(arr < 10, axis=-1)
    is_white = np.all(arr > 245, axis=-1)
    has_data = ~(is_black | is_white)
    fraction = has_data.sum() / has_data.size
    return fraction < threshold


# ── Tile Processing ──────────────────────────────────────────────────────────

def process_one_tile(ei, ni, tile_dir, session, jpeg_quality):
    """
    Fetch and save one orthophoto tile via WMS.

    Returns: (tile_key, file_size, status)
      status: "existing", "generated", "empty", "wms_error"
    """
    e_sw = ei * TILE_SIZE
    n_sw = ni * TILE_SIZE
    tile_key = f"{e_sw}_{n_sw}"
    tile_name = f"ortho_{tile_key}.jpg"
    tile_path = tile_dir / tile_name

    # Skip if already generated
    if tile_path.exists() and tile_path.stat().st_size > 0:
        return tile_key, tile_path.stat().st_size, "existing"

    # WMS BBOX: (min_e, min_n, max_e, max_n)
    bbox = (e_sw, n_sw, e_sw + TILE_SIZE, n_sw + TILE_SIZE)

    img = fetch_tile_wms(bbox, TILE_SIZE, TILE_SIZE, session)
    if img is None:
        return tile_key, None, "wms_error"

    # Check if tile is empty (outside Swiss coverage)
    if is_tile_empty(img):
        return tile_key, None, "empty"

    # WMS returns image with standard orientation:
    #   pixel(0,0) = NW corner, rows go N→S, cols go W→E
    #
    # Terrain3D / ProvPilot needs:
    #   pixel(0,0) = SE corner, rows go S→N, cols go E→W
    #
    # So: flip vertically (N→S becomes S→N) and flip horizontally (W→E becomes E→W)
    img = ImageOps.flip(img)    # vertical flip: rows S→N
    img = ImageOps.mirror(img)  # horizontal flip: cols E→W

    # Ensure RGB
    if img.mode != "RGB":
        img = img.convert("RGB")

    # Save
    img.save(tile_path, "JPEG", quality=jpeg_quality)
    return tile_key, tile_path.stat().st_size, "generated"


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Generate orthophoto JPEG tiles for ProvPilot via "
                    "Swisstopo WMS. Fully resumable.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Generate for all terrain tiles:
  %(prog)s --terrain-manifest scenery/terrain/manifest.json

  # Custom area:
  %(prog)s --center-e 2682880 --center-n 1283072 --size 40000

  # Resume after interruption (existing tiles are skipped):
  %(prog)s --terrain-manifest scenery/terrain/manifest.json
        """,
    )

    parser.add_argument("--terrain-manifest", type=str,
                        default="scenery/terrain/manifest.json",
                        help="Terrain manifest for coverage area")
    parser.add_argument("--center-e", type=int, default=None,
                        help="Override: center Easting in LV95")
    parser.add_argument("--center-n", type=int, default=None,
                        help="Override: center Northing in LV95")
    parser.add_argument("--size", type=int, default=None,
                        help="Override: area size in meters")
    parser.add_argument("--tile-dir", type=str, default="scenery/orthophoto",
                        help="Output directory (default: scenery/orthophoto)")
    parser.add_argument("--jpeg-quality", type=int, default=85,
                        help="JPEG quality 1-100 (default: 85)")

    args = parser.parse_args()

    # Determine bounds
    if args.center_e and args.center_n and args.size:
        half = args.size // 2
        west = args.center_e - half
        east = args.center_e + half
        south = args.center_n - half
        north = args.center_n + half
    else:
        manifest_path = Path(args.terrain_manifest)
        if not manifest_path.exists():
            print(f"ERROR: Terrain manifest not found: {manifest_path}")
            return 1
        with open(manifest_path) as f:
            tm = json.load(f)
        tg = tm["grid"]
        west = tg["ei_min"] * TILE_SIZE
        east = (tg["ei_max"] + 1) * TILE_SIZE
        south = tg["ni_min"] * TILE_SIZE
        north = (tg["ni_max"] + 1) * TILE_SIZE

    # Snap to tile grid
    west = (west // TILE_SIZE) * TILE_SIZE
    south = (south // TILE_SIZE) * TILE_SIZE
    east = ((east + TILE_SIZE - 1) // TILE_SIZE) * TILE_SIZE
    north = ((north + TILE_SIZE - 1) // TILE_SIZE) * TILE_SIZE

    ei_min = west // TILE_SIZE
    ei_max = east // TILE_SIZE - 1
    ni_min = south // TILE_SIZE
    ni_max = north // TILE_SIZE - 1
    n_e = ei_max - ei_min + 1
    n_n = ni_max - ni_min + 1
    total_tiles = n_e * n_n

    print("=" * 60)
    print("SWISSIMAGE Orthophoto Generator (WMS)")
    print("=" * 60)
    print(f"\nBounds: W={west:,} S={south:,} E={east:,} N={north:,}")
    print(f"Coverage: {(east-west)/1000:.0f} km × {(north-south)/1000:.0f} km")
    print(f"Tile grid: {n_e} × {n_n} = {total_tiles:,} tiles")
    print(f"Output: {TILE_SIZE}×{TILE_SIZE} px JPEG at 1 m/pixel")
    print(f"Source: Swisstopo WMS ({WMS_LAYER})")

    tile_dir = Path(args.tile_dir)
    tile_dir.mkdir(parents=True, exist_ok=True)

    # Count existing tiles
    existing_count = sum(1 for f in tile_dir.glob("ortho_*.jpg")
                         if f.stat().st_size > 0)
    remaining = total_tiles - existing_count

    if existing_count:
        print(f"\nResuming: {existing_count:,} tiles already done, "
              f"~{remaining:,} remaining")
    else:
        print(f"\nStarting fresh: {total_tiles:,} tiles to generate")

    # Process tiles
    session = requests.Session()
    generated = 0
    skipped_existing = 0
    skipped_empty = 0
    failed = 0
    total_bytes = 0
    start_time = time.time()
    last_request_time = 0

    tile_list = []
    for ei in range(ei_min, ei_max + 1):
        for ni in range(ni_min, ni_max + 1):
            tile_list.append((ei, ni))

    pbar = tqdm(tile_list, desc="Processing", unit="tile",
                miniters=10, dynamic_ncols=True)

    for idx, (ei, ni) in enumerate(pbar):
        # Rate limiting
        now = time.time()
        wait = MIN_REQUEST_INTERVAL - (now - last_request_time)
        if wait > 0:
            time.sleep(wait)

        tile_key, size, status = process_one_tile(
            ei, ni, tile_dir, session, args.jpeg_quality
        )
        last_request_time = time.time()

        if status == "existing":
            skipped_existing += 1
            total_bytes += size
        elif status == "generated":
            generated += 1
            total_bytes += size
        elif status == "empty":
            skipped_empty += 1
        else:
            failed += 1

        # Update progress
        if (idx + 1) % 50 == 0 or idx == len(tile_list) - 1:
            elapsed = time.time() - start_time
            processed = generated + skipped_empty + failed
            rate = processed / max(elapsed, 1) if processed > 0 else 1
            tiles_left = total_tiles - skipped_existing - idx - 1
            eta_h = tiles_left / max(rate, 0.01) / 3600
            pbar.set_postfix_str(
                f"new={generated} empty={skipped_empty} "
                f"skip={skipped_existing} fail={failed} "
                f"ETA={eta_h:.1f}h"
            )

    pbar.close()

    # Build manifest from all tiles on disk
    manifest = _build_manifest(tile_dir, ei_min, ei_max, ni_min, ni_max,
                               args.jpeg_quality)

    elapsed = time.time() - start_time
    total_mb = sum(
        t["size_bytes"] for t in manifest["tiles"].values()
    ) / (1024 * 1024)
    print(f"\n{'=' * 60}")
    print(f"DONE in {elapsed/60:.1f} minutes ({elapsed/3600:.1f} hours)")
    print(f"  Generated: {generated:,} new tiles")
    print(f"  Skipped (existing): {skipped_existing:,}")
    print(f"  Skipped (empty/outside CH): {skipped_empty:,}")
    print(f"  Failed: {failed:,}")
    print(f"  Total on disk: {manifest['tile_count']:,} tiles")
    print(f"  Total size: {total_mb:.0f} MB")
    print(f"  Directory: {tile_dir}")
    print(f"{'=' * 60}")
    return 0


def _build_manifest(tile_dir, ei_min, ei_max, ni_min, ni_max, jpeg_quality):
    """Scan tile directory and rebuild manifest.json."""
    manifest = {
        "tile_size_m": TILE_SIZE,
        "pixels_per_tile": TILE_SIZE,
        "pixel_resolution_m": 1.0,
        "format": "jpeg_rgb",
        "jpeg_quality": jpeg_quality,
        "orientation": {
            "pixel_0_0": "SE corner of Swiss tile",
            "col_direction": "East to West (Godot +X)",
            "row_direction": "South to North (Godot +Z)",
        },
        "grid": {
            "ei_min": ei_min,
            "ei_max": ei_max,
            "ni_min": ni_min,
            "ni_max": ni_max,
        },
        "tiles": {},
    }

    for f in sorted(tile_dir.glob("ortho_*.jpg")):
        parts = f.stem.replace("ortho_", "").split("_")
        if len(parts) != 2:
            continue
        try:
            e_sw = int(parts[0])
            n_sw = int(parts[1])
        except ValueError:
            continue

        ei = e_sw // TILE_SIZE
        ni = n_sw // TILE_SIZE
        tile_key = f"{e_sw}_{n_sw}"
        manifest["tiles"][tile_key] = {
            "swiss_e": e_sw,
            "swiss_n": n_sw,
            "tile_ei": ei,
            "tile_ni": ni,
            "file": f.name,
            "size_bytes": f.stat().st_size,
        }

    manifest["tile_count"] = len(manifest["tiles"])

    manifest_path = tile_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"Manifest: {manifest_path} ({manifest['tile_count']:,} tiles)")
    return manifest


if __name__ == "__main__":
    sys.exit(main())
