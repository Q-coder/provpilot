#!/usr/bin/env python3
"""
Process SwissTopo swissALTI3D tiles into a single R16 heightmap for Godot/Terrain3D.

Downloads tiles directly from the SwissTopo STAC API (data.geo.admin.ch),
merges them, crops to the desired bounds, and converts to R16 format.

The R16 output is oriented for use with the ProvPilot coordinate convention:
  Godot X = TERRAIN_CENTER_E - Swiss_E   (+X = West)
  Godot Z = Swiss_N - TERRAIN_CENTER_N   (+Z = North)

Usage:
    # List tiles needed for the Schaffhausen area:
    python scripts/process_swisstopo_terrain.py \\
        --center-e 2681968 --center-n 1282869 --size 8000 \\
        --list-tiles

    # Download, process and generate R16 file:
    python scripts/process_swisstopo_terrain.py \\
        --center-e 2681968 --center-n 1282869 --size 8000 \\
        --output provpilot/schaffhausen.r16

    # Verify an existing R16 file against known landmarks:
    python scripts/process_swisstopo_terrain.py \\
        --center-e 2681968 --center-n 1282869 --size 8000 \\
        --verify provpilot/schaffhausen.r16

Requirements:
    pip install rasterio numpy requests tqdm
"""

import argparse
import json
import os
import sys
from pathlib import Path

try:
    import numpy as np
    import rasterio
    from rasterio.merge import merge
    from rasterio.transform import from_bounds as transform_from_bounds
    from rasterio.warp import reproject, Resampling
    import requests
    from tqdm import tqdm
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install rasterio numpy requests tqdm")
    sys.exit(1)


# ── SwissTopo STAC API ──────────────────────────────────────────────────────

STAC_COLLECTION = "ch.swisstopo.swissalti3d"
STAC_BASE = "https://data.geo.admin.ch/api/stac/v0.9"
DATA_BASE = "https://data.geo.admin.ch/ch.swisstopo.swissalti3d"

# SwissTopo tiles cover 1km x 1km each, named by SW corner in km.
# e.g. swissalti3d_2019_2682-1283 covers E 2682000..2683000, N 1283000..1284000
# Resolution: 0.5m -> 2000x2000 pixels per tile


def get_tile_ids_for_bounds(west, south, east, north):
    """Return list of (easting_km, northing_km) tile coordinates covering the bounds."""
    west_km = west // 1000
    south_km = south // 1000
    east_km = (east - 1) // 1000
    north_km = (north - 1) // 1000

    tiles = []
    for e_km in range(west_km, east_km + 1):
        for n_km in range(south_km, north_km + 1):
            tiles.append((e_km, n_km))
    return tiles


def discover_tile_urls(tiles, resolution=0.5):
    """
    Discover actual download URLs for tiles.

    SwissTopo tiles may have different vintage years (2019-2024).
    We try direct URL construction with common years.

    Returns dict: {(e_km, n_km): url}
    """
    found = {}
    for e_km, n_km in tqdm(tiles, desc="Discovering tiles"):
        url = _construct_tile_url(e_km, n_km, resolution)
        if url:
            found[(e_km, n_km)] = url
        else:
            # Try STAC API search as fallback
            url = _stac_search_tile(e_km, n_km, resolution)
            if url:
                found[(e_km, n_km)] = url
            else:
                print(f"  WARNING: No tile found for {e_km}-{n_km}")
    return found


def _construct_tile_url(e_km, n_km, resolution):
    """Try to construct a direct download URL, testing common years."""
    res_str = "0.5" if resolution == 0.5 else str(int(resolution))
    for year in [2024, 2023, 2022, 2021, 2020, 2019]:
        tile_id = f"swissalti3d_{year}_{e_km}-{n_km}"
        filename = f"{tile_id}_{res_str}_2056_5728.tif"
        url = f"{DATA_BASE}/{tile_id}/{filename}"
        try:
            resp = requests.head(url, timeout=10, allow_redirects=True)
            if resp.status_code == 200:
                return url
        except requests.RequestException:
            continue
    return None


def _stac_search_tile(e_km, n_km, resolution):
    """Search STAC API for a tile covering specific coordinates."""
    center_e = e_km * 1000 + 500
    center_n = n_km * 1000 + 500
    lat, lon = lv95_to_wgs84_approx(center_e, center_n)
    delta = 0.005

    url = (f"{STAC_BASE}/collections/{STAC_COLLECTION}/items"
           f"?bbox={lon-delta},{lat-delta},{lon+delta},{lat+delta}&limit=10")
    try:
        resp = requests.get(url, timeout=30)
        resp.raise_for_status()
        data = resp.json()
        res_str = "0.5" if resolution == 0.5 else str(int(resolution))

        for feature in data.get("features", []):
            fid = feature["id"]
            parts = fid.split("_")
            if len(parts) >= 3:
                coord_part = parts[-1]
                try:
                    fe, fn = coord_part.split("-")
                    if int(fe) == e_km and int(fn) == n_km:
                        for asset_key, asset in feature.get("assets", {}).items():
                            if (asset.get("eo:gsd") == resolution and
                                    asset_key.endswith(".tif") and
                                    "image/tiff" in asset.get("type", "")):
                                return asset["href"]
                except (ValueError, IndexError):
                    continue
    except requests.RequestException as e:
        print(f"  STAC search failed: {e}")
    return None


def lv95_to_wgs84_approx(easting, northing):
    """Approximate conversion from Swiss LV95 to WGS84 (lat, lon)."""
    y_aux = (easting - 2600000) / 1000000
    x_aux = (northing - 1200000) / 1000000

    lat = (16.9023892 + 3.238272 * x_aux
           - 0.270978 * y_aux ** 2
           - 0.002528 * x_aux ** 2
           - 0.0447 * y_aux ** 2 * x_aux
           - 0.0140 * x_aux ** 3) * 100 / 36

    lon = (2.6779094 + 4.728982 * y_aux
           + 0.791484 * y_aux * x_aux
           + 0.1306 * y_aux * x_aux ** 2
           - 0.0436 * y_aux ** 3) * 100 / 36

    return lat, lon


# ── Download ─────────────────────────────────────────────────────────────────

def download_tile(url, output_dir):
    """Download a single tile, using cache if available."""
    filename = url.split("/")[-1]
    output_path = output_dir / filename
    if output_path.exists() and output_path.stat().st_size > 0:
        return output_path

    resp = requests.get(url, stream=True, timeout=120)
    resp.raise_for_status()
    with open(output_path, "wb") as f:
        for chunk in resp.iter_content(chunk_size=65536):
            f.write(chunk)
    return output_path


# ── GeoTIFF Processing ──────────────────────────────────────────────────────

def inspect_geotiff(path):
    """Read and print GeoTIFF metadata for debugging."""
    with rasterio.open(path) as src:
        t = src.transform
        info = {
            "path": str(path.name),
            "size": f"{src.width}x{src.height}",
            "crs": str(src.crs),
            "bounds": f"W={src.bounds.left:.0f} S={src.bounds.bottom:.0f} E={src.bounds.right:.0f} N={src.bounds.top:.0f}",
            "resolution": f"{src.res[0]:.2f}m x {src.res[1]:.2f}m",
            "dtype": str(src.dtypes[0]),
            "nodata": src.nodata,
            "col_dir": "East (standard)" if t.a > 0 else "West (UNUSUAL!)",
            "row_dir": "South (standard)" if t.e < 0 else "North (UNUSUAL!)",
        }
        return info


def merge_and_crop_tiles(tif_paths, west, south, east, north,
                         output_path, target_resolution=1.0):
    """Merge multiple GeoTIFF tiles, crop to bounds, and resample."""
    print(f"\nMerging {len(tif_paths)} tiles...")

    datasets = []
    for p in tif_paths:
        try:
            ds = rasterio.open(p)
            datasets.append(ds)
        except Exception as e:
            print(f"  WARNING: Could not open {p}: {e}")

    if not datasets:
        raise RuntimeError("No valid GeoTIFF tiles to merge")

    crs = datasets[0].crs

    # Merge all tiles, cropping to desired bounds
    mosaic, mosaic_transform = merge(datasets, bounds=(west, south, east, north))

    for ds in datasets:
        ds.close()

    print(f"  Merged size: {mosaic.shape[2]}x{mosaic.shape[1]} pixels")
    print(f"  Merged bounds: W={west}, S={south}, E={east}, N={north}")

    target_width = int((east - west) / target_resolution)
    target_height = int((north - south) / target_resolution)
    data = mosaic[0]

    # Resample if needed
    if data.shape[1] != target_width or data.shape[0] != target_height:
        print(f"  Resampling {data.shape[1]}x{data.shape[0]} -> {target_width}x{target_height}")
        print(f"  ({abs(mosaic_transform.a):.2f}m -> {target_resolution:.2f}m per pixel)")

        dst_transform = transform_from_bounds(west, south, east, north,
                                              target_width, target_height)
        dst_data = np.zeros((target_height, target_width), dtype=data.dtype)

        reproject(
            source=data,
            destination=dst_data,
            src_transform=mosaic_transform,
            src_crs=crs,
            dst_transform=dst_transform,
            dst_crs=crs,
            resampling=Resampling.bilinear,
        )
        data = dst_data
        mosaic_transform = dst_transform

    out_meta = {
        "driver": "GTiff",
        "height": data.shape[0],
        "width": data.shape[1],
        "count": 1,
        "dtype": data.dtype,
        "crs": crs,
        "transform": mosaic_transform,
        "nodata": -9999,
    }

    with rasterio.open(output_path, "w", **out_meta) as dst:
        dst.write(data, 1)

    print(f"  Written merged GeoTIFF: {output_path}")
    return output_path


# ── R16 Conversion ──────────────────────────────────────────────────────────

def geotiff_to_r16(input_path, output_path, center_e, center_n,
                   verify_landmarks=True):
    """
    Convert a merged/cropped GeoTIFF to R16 format for Terrain3D.

    The R16 is oriented for the ProvPilot coordinate convention:
      Godot X = CENTER_E - Swiss_E   (+X = West)
      Godot Z = Swiss_N - CENTER_N   (+Z = North)

    Terrain3D maps R16 data as:
      pixel(col, row) -> world(import_position.x + col, import_position.z + row)

    So pixel (0,0) is at most negative world X (= easternmost Swiss E)
    and most negative world Z (= southernmost Swiss N).

    GeoTIFF standard orientation:
      row 0 = North, rows increase southward
      col 0 = West,  cols increase eastward

    Required transforms to match ProvPilot convention:
      flipud: row 0 (North) -> last row (maps to +Z = North in Godot)
      fliplr: col 0 (West) -> last col (maps to +X = West in Godot)

    Returns dict with Terrain3D import settings.
    """
    with rasterio.open(input_path) as src:
        data = src.read(1).astype(np.float64)
        transform = src.transform

        # ── Verify GeoTIFF orientation ──
        print("\n=== GeoTIFF Orientation Check ===")
        print(f"  Transform: {transform}")
        a_dir = "East (standard)" if transform.a > 0 else "West (UNUSUAL!)"
        e_dir = "South (standard)" if transform.e < 0 else "North (UNUSUAL!)"
        print(f"  Pixel width (a):  {transform.a:+.4f}  -> {a_dir}")
        print(f"  Pixel height (e): {transform.e:+.4f}  -> {e_dir}")

        if transform.a <= 0 or transform.e >= 0:
            print("  WARNING: Non-standard orientation detected!")
            print("  The flip logic assumes standard GeoTIFF (a>0, e<0).")
            print("  Results may be incorrect!")

        # ── Handle nodata ──
        nodata_mask = None
        if src.nodata is not None:
            nodata_mask = (data == src.nodata)
            data[nodata_mask] = 0
            nodata_count = int(nodata_mask.sum())
            if nodata_count > 0:
                print(f"  NoData pixels: {nodata_count} ({100*nodata_count/data.size:.2f}%)")

        data[data < 0] = 0

        # ── Elevation statistics ──
        valid = data[data > 0]
        if len(valid) == 0:
            raise RuntimeError("No valid elevation data found!")

        min_elev = float(valid.min())
        max_elev = float(valid.max())
        mean_elev = float(valid.mean())

        print(f"\n=== Elevation Statistics ===")
        print(f"  Min:  {min_elev:.2f}m")
        print(f"  Max:  {max_elev:.2f}m")
        print(f"  Mean: {mean_elev:.2f}m")
        print(f"  Range: {max_elev - min_elev:.2f}m")

        # ── Verify against known landmarks BEFORE flipping ──
        if verify_landmarks:
            print(f"\n=== Landmark Elevation Check (in GeoTIFF) ===")
            _verify_landmarks_geotiff(data, transform, center_e, center_n)

        # ── R16 encoding ──
        r16_max = float(np.ceil(max_elev / 10) * 10)
        print(f"\n=== R16 Encoding ===")
        print(f"  r16_max = {r16_max}")
        print(f"  Formula: raw_uint16 = elevation / {r16_max} * 65535")
        print(f"  Terrain3D decodes: elev = raw / 65535 * {r16_max}")

        data_r16 = np.clip(data / r16_max * 65535, 0, 65535).astype(np.uint16)

        # ── Apply coordinate system flips ──
        #
        # GeoTIFF (standard):              After flips (R16 for ProvPilot):
        #   (0,0)=NW  (0,W)=NE             (0,0)=SE  (0,W)=SW
        #   (H,0)=SW  (H,W)=SE             (H,0)=NE  (H,W)=NW
        #
        # Terrain3D pixel(col,row) -> world(import_x+col, import_z+row)
        # With import_position=(-4000,-4000):
        #   pixel(0,0) -> world(-4000,-4000) -> Swiss(center_e+4000, center_n-4000) = SE
        #   This matches the flipped (0,0)=SE!
        #
        print(f"\n=== Applying Coordinate Transforms ===")
        print(f"  flipud: row 0 was North -> now at bottom (+Z = North in Godot)")
        print(f"  fliplr: col 0 was West  -> now at right  (+X = West in Godot)")

        data_r16 = np.flipud(data_r16)
        data_r16 = np.fliplr(data_r16)

        # ── Corner verification ──
        print(f"\n=== Corner Verification (after flips) ===")
        se_val = data_r16[0, 0] / 65535.0 * r16_max
        sw_val = data_r16[0, -1] / 65535.0 * r16_max
        ne_val = data_r16[-1, 0] / 65535.0 * r16_max
        nw_val = data_r16[-1, -1] / 65535.0 * r16_max
        print(f"  pixel(0,0)     = {se_val:.1f}m  (SE corner)")
        print(f"  pixel(0,W-1)   = {sw_val:.1f}m  (SW corner)")
        print(f"  pixel(H-1,0)   = {ne_val:.1f}m  (NE corner)")
        print(f"  pixel(H-1,W-1) = {nw_val:.1f}m  (NW corner)")

        # ── Write R16 (little-endian uint16, row-major) ──
        if sys.byteorder != "little":
            data_r16 = data_r16.byteswap()
        data_r16.tofile(output_path)

        # ── Import settings ──
        width = data_r16.shape[1]
        height = data_r16.shape[0]
        import_x = -(width // 2)
        import_y = -(height // 2)

        settings = {
            "r16_file": str(output_path),
            "r16_size": f"Vector2i({width}, {height})",
            "r16_range": f"Vector2(0, {r16_max})",
            "import_position": f"Vector2i({import_x}, {import_y})",
            "height_offset": 0,
            "import_scale": 1.0,
            "vertex_spacing": 1.0,
            "min_elevation": min_elev,
            "max_elevation": max_elev,
            "r16_max": r16_max,
            "width": width,
            "height": height,
        }

        print(f"\n=== R16 Output ===")
        print(f"  File: {output_path}")
        print(f"  Size: {width}x{height} pixels")
        fsize = width * height * 2
        print(f"  File size: {fsize:,} bytes ({fsize / 1048576:.1f} MB)")
        print(f"  Raw value range: {data_r16.min()} - {data_r16.max()}")
        print(f"\n=== Terrain3D Import Settings ===")
        print(f'  height_file_name = "res://{output_path.name}"')
        print(f"  r16_size = {settings['r16_size']}")
        print(f"  r16_range = {settings['r16_range']}")
        print(f"  import_position = {settings['import_position']}")
        print(f"  height_offset = 0")
        print(f"  import_scale = 1.0")

        return settings


# ── Landmark Verification ────────────────────────────────────────────────────

# Known landmarks in the Schaffhausen area (Swiss LV95, elevation m ASL)
LANDMARKS_SCHAFFHAUSEN = [
    {"name": "Schmerlat Airfield (runway)", "e": 2681968, "n": 1282869,
     "elev_approx": 440, "tolerance": 20},
    {"name": "Hallau village center", "e": 2679800, "n": 1283600,
     "elev_approx": 430, "tolerance": 30},
    {"name": "Rhine near Schaffhausen", "e": 2688500, "n": 1283000,
     "elev_approx": 395, "tolerance": 20},
    {"name": "Randen hilltop area", "e": 2684000, "n": 1285000,
     "elev_approx": 700, "tolerance": 80},
    {"name": "Rheinau bend area", "e": 2686000, "n": 1280000,
     "elev_approx": 380, "tolerance": 30},
]


def _verify_landmarks_geotiff(data, transform, center_e, center_n):
    """Check elevation at known landmarks in the GeoTIFF (before flipping)."""
    half_w = data.shape[1] // 2
    half_h = data.shape[0] // 2
    west = center_e - half_w
    north = center_n + half_h

    for lm in LANDMARKS_SCHAFFHAUSEN:
        # GeoTIFF standard: col = (E - W_bound) / pixel_width
        #                   row = (N_bound - N) / pixel_height
        if abs(transform.a) > 0 and abs(transform.e) > 0:
            col = int((lm["e"] - transform.c) / transform.a)
            row = int((lm["n"] - transform.f) / transform.e)
        else:
            continue

        if 0 <= row < data.shape[0] and 0 <= col < data.shape[1]:
            actual = data[row, col]
            diff = abs(actual - lm["elev_approx"])
            status = "OK" if diff <= lm["tolerance"] else "MISMATCH"
            print(f"  {lm['name']}: {actual:.1f}m "
                  f"(expected ~{lm['elev_approx']}m, diff {diff:.1f}m) [{status}]")
        else:
            print(f"  {lm['name']}: outside bounds (col={col}, row={row})")


def verify_r16(r16_path, center_e, center_n, size, r16_max):
    """
    Verify an existing R16 file by checking known landmark elevations.

    R16 pixel mapping (after flips applied during creation):
      pixel(col, row) -> world(import_x + col, import_z + row)

      world_x = -(size/2) + col        Swiss_E = center_e - world_x
      world_z = -(size/2) + row        Swiss_N = center_n + world_z

    So:
      col = center_e + size/2 - swiss_e
      row = swiss_n - center_n + size/2
    """
    file_size = r16_path.stat().st_size
    expected_bytes = size * size * 2

    print(f"\n=== Verifying R16 File ===")
    print(f"  File: {r16_path}")
    print(f"  File size: {file_size:,} bytes")
    print(f"  Expected: {expected_bytes:,} bytes ({size}x{size} uint16)")

    if file_size != expected_bytes:
        print(f"  SIZE MISMATCH! File has {file_size // 2} values, expected {size * size}")
        if file_size % 2 != 0:
            print(f"  File size is ODD - corrupt data!")
            return
        actual_side = int(np.sqrt(file_size // 2))
        print(f"  Actual size if square: {actual_side}x{actual_side}")
        size = actual_side

    data = np.fromfile(r16_path, dtype="<u2").reshape(size, size)

    print(f"  Raw value range: {data.min()} - {data.max()}")
    print(f"  r16_max: {r16_max}")

    elevations = data.astype(np.float64) / 65535.0 * r16_max
    valid = elevations[elevations > 0]
    if len(valid) > 0:
        print(f"  Elevation range: {valid.min():.1f}m - {valid.max():.1f}m")
        print(f"  Mean elevation: {valid.mean():.1f}m")

    half = size // 2

    print(f"\n=== Landmark Verification ===")
    for lm in LANDMARKS_SCHAFFHAUSEN:
        col = center_e + half - lm["e"]
        row = lm["n"] - center_n + half

        if 0 <= row < size and 0 <= col < size:
            raw = data[row, col]
            actual = raw / 65535.0 * r16_max
            diff = abs(actual - lm["elev_approx"])
            status = "OK" if diff <= lm["tolerance"] else "MISMATCH"
            print(f"  {lm['name']}:")
            print(f"    Swiss ({lm['e']}, {lm['n']}) -> R16 pixel ({col}, {row})")
            print(f"    Elevation: {actual:.1f}m (expected ~{lm['elev_approx']}m, "
                  f"diff {diff:.1f}m) [{status}]")
        else:
            print(f"  {lm['name']}: outside R16 bounds (col={col}, row={row})")

    print(f"\n=== Corner Elevations ===")
    corners = [
        ("pixel(0,0)     = SE corner", 0, 0,
         center_e + half, center_n - half),
        (f"pixel(0,{size-1})   = SW corner", 0, size - 1,
         center_e - half + 1, center_n - half),
        (f"pixel({size-1},0)   = NE corner", size - 1, 0,
         center_e + half, center_n + half - 1),
        (f"pixel({size-1},{size-1}) = NW corner", size - 1, size - 1,
         center_e - half + 1, center_n + half - 1),
    ]
    for label, row, col, se, sn in corners:
        raw = data[row, col]
        elev = raw / 65535.0 * r16_max
        print(f"  {label}: {elev:.1f}m  (Swiss E={se}, N={sn})")


# ── Tile Generation for Streaming ────────────────────────────────────────────

def generate_tiles(merged_path, tile_dir, tile_size=1024):
    """
    Split a merged GeoTIFF into individual float32 tiles for Terrain3D streaming.

    Each tile is tile_size × tile_size pixels (1m/pixel), stored as raw
    little-endian float32 (elevation in meters ASL).

    Tiles are aligned to a global grid in Swiss LV95 coordinates:
      tile (ei, ni) covers E: [ei*1024, (ei+1)*1024), N: [ni*1024, (ni+1)*1024)

    The pixel data is oriented for Terrain3D / ProvPilot:
      pixel(col=0, row=0) = SE corner of the Swiss tile
      columns go East→West  (Godot +X)
      rows go South→North   (Godot +Z)

    Returns: manifest dict
    """
    tile_dir = Path(tile_dir)
    tile_dir.mkdir(parents=True, exist_ok=True)

    with rasterio.open(merged_path) as src:
        data = src.read(1).astype(np.float32)
        transform = src.transform

        # Handle nodata
        if src.nodata is not None:
            data[data == src.nodata] = 0
        data[data < 0] = 0

        # Mosaic bounds in Swiss coordinates
        w_bound = transform.c
        n_bound = transform.f
        pixel_w = abs(transform.a)
        pixel_h = abs(transform.e)
        e_bound = w_bound + data.shape[1] * pixel_w
        s_bound = n_bound - data.shape[0] * pixel_h

        print(f"\n=== Tile Generation ===")
        print(f"  Mosaic bounds: W={w_bound:.0f} S={s_bound:.0f} "
              f"E={e_bound:.0f} N={n_bound:.0f}")
        print(f"  Mosaic size: {data.shape[1]}x{data.shape[0]} px "
              f"at {pixel_w}m/pixel")

        # Determine tile grid range
        ei_min = int(np.floor(w_bound / tile_size))
        ei_max = int(np.ceil(e_bound / tile_size)) - 1
        ni_min = int(np.floor(s_bound / tile_size))
        ni_max = int(np.ceil(n_bound / tile_size)) - 1

        n_e = ei_max - ei_min + 1
        n_n = ni_max - ni_min + 1
        total_tiles = n_e * n_n
        print(f"  Tile grid: E[{ei_min}..{ei_max}] ({n_e}), "
              f"N[{ni_min}..{ni_max}] ({n_n})")
        print(f"  Total tiles to generate: {total_tiles}")
        print(f"  Coverage: {n_e * tile_size / 1000:.1f}km x "
              f"{n_n * tile_size / 1000:.1f}km")

        manifest = {
            "tile_size_m": tile_size,
            "pixels_per_tile": tile_size,
            "pixel_resolution_m": float(pixel_w),
            "format": "float32_raw_le",
            "bytes_per_tile": tile_size * tile_size * 4,
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
            "region_mapping": {
                "description": (
                    "Given floating origin (CENTER_E, CENTER_N) aligned to "
                    "tile_size boundaries: "
                    "region_x = CENTER_E/tile_size - tile_ei - 1, "
                    "region_z = tile_ni - CENTER_N/tile_size"
                ),
            },
            "tiles": {},
        }

        tile_count = 0
        for ei in tqdm(range(ei_min, ei_max + 1), desc="Generating tiles"):
            for ni in range(ni_min, ni_max + 1):
                e_sw = ei * tile_size
                n_sw = ni * tile_size

                # Pixel coordinates in merged GeoTIFF array
                # GeoTIFF: col increases East, row increases South
                col_start = int(round((e_sw - w_bound) / pixel_w))
                col_end = col_start + tile_size
                row_start = int(round((n_bound - n_sw - tile_size) / pixel_h))
                row_end = row_start + tile_size

                # Skip tiles entirely outside the mosaic
                if (col_end <= 0 or col_start >= data.shape[1] or
                        row_end <= 0 or row_start >= data.shape[0]):
                    continue

                # Extract with zero-padding where data is missing
                tile_data = np.zeros((tile_size, tile_size), dtype=np.float32)

                sr0 = max(0, row_start)
                sr1 = min(data.shape[0], row_end)
                sc0 = max(0, col_start)
                sc1 = min(data.shape[1], col_end)

                dr0 = sr0 - row_start
                dr1 = dr0 + (sr1 - sr0)
                dc0 = sc0 - col_start
                dc1 = dc0 + (sc1 - sc0)

                tile_data[dr0:dr1, dc0:dc1] = data[sr0:sr1, sc0:sc1]

                # Apply coordinate transforms for Terrain3D / ProvPilot
                # GeoTIFF: rows N→S, cols W→E
                # Target:  rows S→N, cols E→W  (pixel(0,0) = SE corner)
                tile_data = np.flipud(tile_data)
                tile_data = np.fliplr(tile_data)

                # Save as little-endian float32
                tile_data = tile_data.astype("<f4")
                tile_name = f"tile_{e_sw}_{n_sw}.raw"
                tile_path = tile_dir / tile_name
                tile_data.tofile(tile_path)

                # Elevation stats
                valid = tile_data[tile_data > 0]
                min_elev = float(valid.min()) if len(valid) > 0 else 0.0
                max_elev = float(tile_data.max())

                manifest["tiles"][f"{e_sw}_{n_sw}"] = {
                    "swiss_e": int(e_sw),
                    "swiss_n": int(n_sw),
                    "tile_ei": int(ei),
                    "tile_ni": int(ni),
                    "file": tile_name,
                    "min_elev": round(min_elev, 1),
                    "max_elev": round(max_elev, 1),
                }
                tile_count += 1

        manifest["tile_count"] = tile_count
        manifest_path = tile_dir / "manifest.json"
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)

        total_mb = tile_count * tile_size * tile_size * 4 / (1024 * 1024)
        print(f"\n  Generated {tile_count} tiles "
              f"({total_mb:.0f} MB total)")
        print(f"  Manifest: {manifest_path}")

        return manifest


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Process SwissTopo swissALTI3D tiles into R16 for Terrain3D",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List tiles for Schaffhausen area:
  %(prog)s --center-e 2681968 --center-n 1282869 --size 8000 --list-tiles

  # Full download + processing:
  %(prog)s --center-e 2681968 --center-n 1282869 --size 8000 \\
      --output provpilot/schaffhausen.r16

  # Verify existing R16:
  %(prog)s --center-e 2681968 --center-n 1282869 --size 8000 \\
      --verify provpilot/schaffhausen.r16

  # Generate streaming tiles for a 20km area:
  %(prog)s --center-e 2685000 --center-n 1283000 --size 20000 \\
      --generate-tiles --tile-dir provpilot/terrain_tiles
        """,
    )

    parser.add_argument("--center-e", type=int, required=True,
                        help="Center Easting in LV95 (e.g. 2681968)")
    parser.add_argument("--center-n", type=int, required=True,
                        help="Center Northing in LV95 (e.g. 1282869)")
    parser.add_argument("--size", type=int, default=8000,
                        help="Terrain size in meters (default: 8000)")
    parser.add_argument("--resolution", type=float, default=1.0,
                        help="Target resolution m/pixel (default: 1.0)")
    parser.add_argument("--tile-resolution", type=float, default=0.5,
                        choices=[0.5, 2.0],
                        help="SwissTopo tile resolution (default: 0.5)")
    parser.add_argument("--output", type=str,
                        help="Output R16 file path")
    parser.add_argument("--cache-dir", type=str, default="./terrain_cache",
                        help="Tile cache directory (default: ./terrain_cache)")
    parser.add_argument("--list-tiles", action="store_true",
                        help="Only list required tiles")
    parser.add_argument("--verify", type=str,
                        help="Verify an existing R16 file")
    parser.add_argument("--skip-download", action="store_true",
                        help="Use cached tiles only")
    parser.add_argument("--r16-max", type=float, default=None,
                        help="Override r16_max for --verify")
    parser.add_argument("--generate-tiles", action="store_true",
                        help="Generate individual float32 tiles for streaming "
                             "instead of a single R16")
    parser.add_argument("--tile-dir", type=str,
                        help="Output directory for streaming tiles "
                             "(with --generate-tiles)")
    parser.add_argument("--tile-size", type=int, default=1024,
                        help="Tile size in meters, must match Terrain3D "
                             "region_size (default: 1024)")

    args = parser.parse_args()

    half = args.size // 2
    west = args.center_e - half
    east = args.center_e + half
    south = args.center_n - half
    north = args.center_n + half

    # Snap bounds to tile grid for tile generation
    if args.generate_tiles:
        ts = args.tile_size
        west = (west // ts) * ts
        south = (south // ts) * ts
        east = ((east + ts - 1) // ts) * ts
        north = ((north + ts - 1) // ts) * ts

    print("=" * 60)
    print("SwissTopo Terrain Processor for ProvPilot / Terrain3D")
    print("=" * 60)
    print(f"\nTerrain Center: E {args.center_e:,} / N {args.center_n:,}")
    print(f"Size: {args.size}m x {args.size}m ({args.size / 1000:.1f}km)")
    print(f"Bounds: W={west:,} S={south:,} E={east:,} N={north:,}")
    print(f"Target resolution: {args.resolution}m/pixel")
    output_px = int(args.size / args.resolution)
    print(f"Output pixels: {output_px}x{output_px}")

    tiles = get_tile_ids_for_bounds(west, south, east, north)
    print(f"\nRequired tiles: {len(tiles)}")

    # ── List tiles ──
    if args.list_tiles:
        print("\nTile grid (easting-northing km):")
        e_values = sorted(set(t[0] for t in tiles))
        n_values = sorted(set(t[1] for t in tiles), reverse=True)
        for n_km in n_values:
            row_str = f"  N={n_km}: "
            row_str += " ".join(f"{e_km}-{n_km}" for e_km in e_values
                                if (e_km, n_km) in tiles)
            print(row_str)
        return 0

    # ── Verify existing R16 ──
    if args.verify:
        r16_path = Path(args.verify)
        if not r16_path.exists():
            print(f"\nERROR: R16 file not found: {r16_path}")
            return 1

        r16_max = args.r16_max
        if r16_max is None:
            r16_max = 810.0  # Default for Schaffhausen
            print(f"\n  Using default r16_max = {r16_max} (override with --r16-max)")

        file_pixels = r16_path.stat().st_size // 2
        target_size = int(np.sqrt(file_pixels))
        verify_r16(r16_path, args.center_e, args.center_n, target_size, r16_max)
        return 0

    # ── Full processing pipeline ──
    if not args.output and not args.generate_tiles:
        print("\nERROR: --output or --generate-tiles is required "
              "(or use --list-tiles / --verify)")
        return 1

    cache_dir = Path(args.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    # Step 1: Discover & download tiles
    if not args.skip_download:
        print("\n-- Step 1: Discovering tile URLs via SwissTopo API --")
        tile_urls = discover_tile_urls(tiles, args.tile_resolution)
        print(f"  Found {len(tile_urls)}/{len(tiles)} tiles")

        if len(tile_urls) < len(tiles):
            missing = set(tiles) - set(tile_urls.keys())
            print(f"  Missing: {[f'{e}-{n}' for e, n in sorted(missing)]}")

        if not tile_urls:
            print("ERROR: No tiles found!")
            return 1

        url_map_path = cache_dir / "tile_urls.json"
        with open(url_map_path, "w") as f:
            json.dump({f"{k[0]}-{k[1]}": v for k, v in tile_urls.items()},
                      f, indent=2)
        print(f"  Tile URLs saved to {url_map_path}")

        print("\n-- Step 2: Downloading tiles --")
        tif_paths = []
        for (e_km, n_km), url in tqdm(tile_urls.items(), desc="Downloading"):
            try:
                path = download_tile(url, cache_dir)
                tif_paths.append(path)
            except Exception as e:
                print(f"\n  ERROR downloading {e_km}-{n_km}: {e}")
    else:
        print("\n-- Using cached tiles --")
        tif_paths = sorted(cache_dir.glob("swissalti3d_*.tif"))
        print(f"  Found {len(tif_paths)} cached tiles")
        if not tif_paths:
            print("ERROR: No cached tiles found!")
            return 1

    # Step 3: Inspect first tile
    print("\n-- Step 3: Inspecting tile format --")
    info = inspect_geotiff(tif_paths[0])
    for k, v in info.items():
        print(f"  {k}: {v}")

    # Step 4: Merge, crop, resample
    print("\n-- Step 4: Merging and cropping --")
    merged_path = cache_dir / "merged_terrain.tif"
    merge_and_crop_tiles(
        tif_paths, west, south, east, north,
        merged_path,
        target_resolution=args.resolution,
    )

    if args.generate_tiles:
        # ── Step 5: Generate streaming tiles ──
        print("\n-- Step 5: Generating streaming tiles --")
        tile_dir = Path(args.tile_dir) if args.tile_dir else Path("provpilot/terrain_tiles")
        manifest = generate_tiles(merged_path, tile_dir, args.tile_size)

        print(f"\n{'=' * 60}")
        print("DONE! Terrain tiles generated for streaming.")
        print(f"  Directory: {tile_dir}")
        print(f"  Tiles: {manifest['tile_count']}")
        print(f"  Each: {args.tile_size}m x {args.tile_size}m "
              f"({args.tile_size * args.tile_size * 4 / 1048576:.1f} MB)")
        n_e = manifest['grid']['ei_max'] - manifest['grid']['ei_min'] + 1
        n_n = manifest['grid']['ni_max'] - manifest['grid']['ni_min'] + 1
        print(f"  Grid: {n_e} x {n_n} = "
              f"{n_e * args.tile_size / 1000:.1f}km x "
              f"{n_n * args.tile_size / 1000:.1f}km")
        print(f"\nNext steps:")
        print(f"  1. Copy {tile_dir}/ into Godot project (or use user:// path)")
        print(f"  2. Implement GDScript TerrainStreamer")
        print(f"  3. Load tiles at runtime via Terrain3DData.add_region()")
        print(f"{'=' * 60}")
        return 0

    # ── Step 5: Convert to R16 (monolithic pipeline) ──
    print("\n-- Step 5: Converting to R16 --")
    output_path = Path(args.output)
    settings = geotiff_to_r16(
        merged_path, output_path,
        args.center_e, args.center_n,
    )

    # Step 6: Self-verify
    print("\n-- Step 6: Self-verification --")
    verify_r16(
        output_path,
        args.center_e, args.center_n,
        int(args.size / args.resolution),
        settings["r16_max"],
    )

    settings_path = output_path.with_suffix(".json")
    with open(settings_path, "w") as f:
        json.dump(settings, f, indent=2)
    print(f"\n  Settings saved to {settings_path}")

    print(f"\n{'=' * 60}")
    print("DONE! Next steps:")
    print(f"  1. Copy {output_path} into your Godot project folder")
    print(f"  2. Open Terrain3D Importer in Godot")
    print(f"  3. Set import parameters:")
    print(f'     height_file_name = "res://{output_path.name}"')
    print(f"     r16_size = {settings['r16_size']}")
    print(f"     r16_range = {settings['r16_range']}")
    print(f"     import_position = {settings['import_position']}")
    print(f"     height_offset = 0")
    print(f"  4. Click 'Run Import'")
    print(f"  5. Set destination_directory = \"res://Terra3DData\"")
    print(f"  6. Click 'Save to Disk'")
    print(f"{'=' * 60}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
