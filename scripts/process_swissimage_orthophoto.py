#!/usr/bin/env python3
"""
Process SwissTopo SWISSIMAGE orthophoto tiles into JPEG textures for Terrain3D.

Downloads SWISSIMAGE 10cm (ch.swisstopo.swissimage-dop10) tiles from the
SwissTopo data portal, merges them, resamples to 1 m/pixel, and splits into
1024×1024 JPEG tiles aligned to the same grid as the terrain heightmaps
produced by process_swisstopo_terrain.py.

The pixel orientation matches the ProvPilot / Terrain3D convention:
  pixel(0, 0)  = SE corner of the Swiss tile
  columns      = East → West  (Godot +X)
  rows         = South → North (Godot +Z)

At runtime the terrain streamer loads each JPEG as an RGBA8 Image and calls
Terrain3DRegion.set_color_map() to apply it as the surface texture.

SwissTopo licensing: SWISSIMAGE has been released as Open Government Data
(OGD) since March 2021 — free for commercial and non-commercial use.

Requirements:
    pip install rasterio numpy requests tqdm Pillow
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
    from PIL import Image
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install rasterio numpy requests tqdm Pillow")
    sys.exit(1)


# ── SwissTopo STAC / Data API ──────────────────────────────────────────────

STAC_COLLECTION = "ch.swisstopo.swissimage-dop10"
STAC_BASE = "https://data.geo.admin.ch/api/stac/v0.9"
DATA_BASE = "https://data.geo.admin.ch/ch.swisstopo.swissimage-dop10"

# SWISSIMAGE tiles cover 1 km × 1 km each, named by SW corner in km.
# Available resolutions: 0.1 m (10 cm) and 2.0 m.
# We default to 2.0 m to keep downloads manageable (~100 KB vs ~57 MB per tile).


def get_tile_ids_for_bounds(west, south, east, north):
    """Return list of (easting_km, northing_km) tile coordinates covering bounds."""
    west_km = west // 1000
    south_km = south // 1000
    east_km = (east - 1) // 1000
    north_km = (north - 1) // 1000

    tiles = []
    for e_km in range(west_km, east_km + 1):
        for n_km in range(south_km, north_km + 1):
            tiles.append((e_km, n_km))
    return tiles


def discover_tile_urls(tiles, resolution=2.0):
    """
    Discover download URLs for SWISSIMAGE tiles.

    Tries direct URL construction with common years (2024 → 2017).
    Falls back to STAC API search.

    Returns dict: {(e_km, n_km): url}
    """
    found = {}
    for e_km, n_km in tqdm(tiles, desc="Discovering tiles"):
        url = _construct_tile_url(e_km, n_km, resolution)
        if url:
            found[(e_km, n_km)] = url
        else:
            url = _stac_search_tile(e_km, n_km, resolution)
            if url:
                found[(e_km, n_km)] = url
            else:
                print(f"  WARNING: No tile found for {e_km}-{n_km}")
    return found


def _construct_tile_url(e_km, n_km, resolution):
    """Try to construct a direct download URL, testing common years."""
    res_str = "0.1" if resolution <= 0.1 else str(int(resolution))
    for year in [2024, 2023, 2022, 2021, 2020, 2019, 2018, 2017]:
        tile_id = f"swissimage-dop10_{year}_{e_km}-{n_km}"
        filename = f"{tile_id}_{res_str}_2056.tif"
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
           f"?bbox={lon - delta},{lat - delta},{lon + delta},{lat + delta}"
           f"&limit=10")
    try:
        resp = requests.get(url, timeout=30)
        resp.raise_for_status()
        data = resp.json()
        res_val = resolution

        for feature in data.get("features", []):
            fid = feature["id"]
            parts = fid.split("_")
            if len(parts) >= 3:
                coord_part = parts[-1]
                try:
                    fe, fn = coord_part.split("-")
                    if int(fe) == e_km and int(fn) == n_km:
                        for asset_key, asset in feature.get("assets", {}).items():
                            if (asset.get("eo:gsd") == res_val
                                    and asset_key.endswith(".tif")
                                    and "image/tiff" in asset.get("type", "")):
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
            "bands": src.count,
            "crs": str(src.crs),
            "bounds": (f"W={src.bounds.left:.0f} S={src.bounds.bottom:.0f} "
                       f"E={src.bounds.right:.0f} N={src.bounds.top:.0f}"),
            "resolution": f"{src.res[0]:.2f}m x {src.res[1]:.2f}m",
            "dtype": str(src.dtypes[0]),
            "nodata": src.nodata,
        }
        return info


def merge_and_crop_tiles(tif_paths, west, south, east, north,
                         output_path, target_resolution=1.0):
    """
    Merge multiple GeoTIFF tiles, crop to bounds, and resample to
    target_resolution.  Works with multi-band (RGB/RGBA) imagery.
    """
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
    band_count = datasets[0].count

    # Merge all tiles, cropping to desired bounds
    mosaic, mosaic_transform = merge(datasets, bounds=(west, south, east, north))

    for ds in datasets:
        ds.close()

    print(f"  Merged size: {mosaic.shape[2]}x{mosaic.shape[1]} pixels "
          f"({mosaic.shape[0]} bands)")
    print(f"  Merged bounds: W={west}, S={south}, E={east}, N={north}")

    target_width = int((east - west) / target_resolution)
    target_height = int((north - south) / target_resolution)

    # Resample if needed
    if mosaic.shape[2] != target_width or mosaic.shape[1] != target_height:
        print(f"  Resampling {mosaic.shape[2]}x{mosaic.shape[1]} -> "
              f"{target_width}x{target_height}")
        print(f"  ({abs(mosaic_transform.a):.2f}m -> {target_resolution:.2f}m "
              f"per pixel)")

        dst_transform = transform_from_bounds(west, south, east, north,
                                              target_width, target_height)
        dst_data = np.zeros((band_count, target_height, target_width),
                            dtype=mosaic.dtype)

        for band_idx in range(band_count):
            reproject(
                source=mosaic[band_idx],
                destination=dst_data[band_idx],
                src_transform=mosaic_transform,
                src_crs=crs,
                dst_transform=dst_transform,
                dst_crs=crs,
                resampling=Resampling.bilinear,
            )
        mosaic = dst_data
        mosaic_transform = dst_transform

    out_meta = {
        "driver": "GTiff",
        "height": mosaic.shape[1],
        "width": mosaic.shape[2],
        "count": band_count,
        "dtype": mosaic.dtype,
        "crs": crs,
        "transform": mosaic_transform,
        "nodata": 0,
    }

    with rasterio.open(output_path, "w", **out_meta) as dst:
        dst.write(mosaic)

    print(f"  Written merged GeoTIFF: {output_path}")
    return output_path


# ── Tile Generation ──────────────────────────────────────────────────────────

def generate_tiles(merged_path, tile_dir, tile_size=1024, jpeg_quality=85):
    """
    Split a merged RGB GeoTIFF into individual JPEG tiles for Terrain3D
    color maps.

    Each tile is tile_size × tile_size pixels (1 m/pixel), saved as JPEG.

    Tiles are aligned to the same global grid in Swiss LV95 coordinates
    used by the terrain heightmaps:
      tile (ei, ni) covers E: [ei*1024, (ei+1)*1024), N: [ni*1024, (ni+1)*1024)

    The pixel data is oriented for Terrain3D / ProvPilot:
      pixel(col=0, row=0) = SE corner of the Swiss tile
      columns go East → West  (Godot +X)
      rows go South → North   (Godot +Z)

    Returns: manifest dict
    """
    tile_dir = Path(tile_dir)
    tile_dir.mkdir(parents=True, exist_ok=True)

    with rasterio.open(merged_path) as src:
        # Read all bands (expect RGB: 3 bands or RGBA: 4 bands)
        data = src.read()  # shape: (bands, rows, cols)
        transform = src.transform
        band_count = data.shape[0]

        # If 4 bands (RGBA), keep only RGB
        if band_count >= 4:
            data = data[:3]
            band_count = 3

        # Ensure uint8 (orthophotos should already be uint8)
        if data.dtype != np.uint8:
            # Normalize to 0-255 range
            for b in range(band_count):
                band = data[b].astype(np.float32)
                bmin, bmax = band.min(), band.max()
                if bmax > bmin:
                    data[b] = ((band - bmin) / (bmax - bmin) * 255).astype(np.uint8)
                else:
                    data[b] = np.zeros_like(band, dtype=np.uint8)

        # Mosaic bounds in Swiss coordinates
        w_bound = transform.c
        n_bound = transform.f
        pixel_w = abs(transform.a)
        pixel_h = abs(transform.e)
        e_bound = w_bound + data.shape[2] * pixel_w
        s_bound = n_bound - data.shape[1] * pixel_h

        print(f"\n=== Orthophoto Tile Generation ===")
        print(f"  Mosaic bounds: W={w_bound:.0f} S={s_bound:.0f} "
              f"E={e_bound:.0f} N={n_bound:.0f}")
        print(f"  Mosaic size: {data.shape[2]}x{data.shape[1]} px "
              f"({band_count} bands) at {pixel_w}m/pixel")

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
        print(f"  JPEG quality: {jpeg_quality}")

        manifest = {
            "tile_size_m": tile_size,
            "pixels_per_tile": tile_size,
            "pixel_resolution_m": float(pixel_w),
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

        tile_count = 0
        total_bytes = 0
        for ei in tqdm(range(ei_min, ei_max + 1), desc="Generating tiles"):
            for ni in range(ni_min, ni_max + 1):
                e_sw = ei * tile_size
                n_sw = ni * tile_size

                # Pixel coordinates in merged GeoTIFF array
                # GeoTIFF: col increases East, row increases South (from top)
                col_start = int(round((e_sw - w_bound) / pixel_w))
                col_end = col_start + tile_size
                row_start = int(round((n_bound - n_sw - tile_size) / pixel_h))
                row_end = row_start + tile_size

                # Skip tiles entirely outside the mosaic
                if (col_end <= 0 or col_start >= data.shape[2] or
                        row_end <= 0 or row_start >= data.shape[1]):
                    continue

                # Extract with zero-padding where data is missing
                tile_data = np.zeros(
                    (band_count, tile_size, tile_size), dtype=np.uint8
                )

                sr0 = max(0, row_start)
                sr1 = min(data.shape[1], row_end)
                sc0 = max(0, col_start)
                sc1 = min(data.shape[2], col_end)

                dr0 = sr0 - row_start
                dr1 = dr0 + (sr1 - sr0)
                dc0 = sc0 - col_start
                dc1 = dc0 + (sc1 - sc0)

                tile_data[:, dr0:dr1, dc0:dc1] = data[:, sr0:sr1, sc0:sc1]

                # Skip entirely black (empty) tiles
                if tile_data.max() == 0:
                    continue

                # Apply coordinate transforms for Terrain3D / ProvPilot
                # GeoTIFF: rows N→S, cols W→E
                # Target:  rows S→N, cols E→W  (pixel(0,0) = SE corner)
                tile_data = np.flip(tile_data, axis=1)  # flipud per band
                tile_data = np.flip(tile_data, axis=2)  # fliplr per band

                # Convert (bands, rows, cols) → (rows, cols, bands) for PIL
                tile_rgb = np.transpose(tile_data, (1, 2, 0))

                # Save as JPEG
                tile_name = f"ortho_{e_sw}_{n_sw}.jpg"
                tile_path = tile_dir / tile_name
                img = Image.fromarray(tile_rgb)
                img.save(tile_path, "JPEG", quality=jpeg_quality)

                file_size = tile_path.stat().st_size
                total_bytes += file_size

                manifest["tiles"][f"{e_sw}_{n_sw}"] = {
                    "swiss_e": int(e_sw),
                    "swiss_n": int(n_sw),
                    "tile_ei": int(ei),
                    "tile_ni": int(ni),
                    "file": tile_name,
                    "size_bytes": file_size,
                }
                tile_count += 1

        manifest["tile_count"] = tile_count
        manifest_path = tile_dir / "manifest.json"
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)

        total_mb = total_bytes / (1024 * 1024)
        print(f"\n  Generated {tile_count} tiles "
              f"({total_mb:.1f} MB total)")
        print(f"  Average tile size: {total_bytes / max(tile_count, 1) / 1024:.0f} KB")
        print(f"  Manifest: {manifest_path}")

        return manifest


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Process SwissTopo SWISSIMAGE orthophotos into JPEG tiles "
                    "for Terrain3D color maps",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List tiles for Schaffhausen area (20 km):
  %(prog)s --center-e 2682880 --center-n 1283072 --size 20000 --list-tiles

  # Download and generate orthophoto tiles:
  %(prog)s --center-e 2682880 --center-n 1283072 --size 20000 \\
      --tile-dir scenery/orthophoto

  # Use 10 cm source resolution for higher quality (much larger download):
  %(prog)s --center-e 2682880 --center-n 1283072 --size 5000 \\
      --source-resolution 0.1 --tile-dir scenery/orthophoto
        """,
    )

    parser.add_argument("--center-e", type=int, required=True,
                        help="Center Easting in LV95 (e.g. 2682880)")
    parser.add_argument("--center-n", type=int, required=True,
                        help="Center Northing in LV95 (e.g. 1283072)")
    parser.add_argument("--size", type=int, default=20000,
                        help="Area size in meters (default: 20000)")
    parser.add_argument("--target-resolution", type=float, default=1.0,
                        help="Output resolution m/pixel (default: 1.0)")
    parser.add_argument("--source-resolution", type=float, default=2.0,
                        choices=[0.1, 2.0],
                        help="SWISSIMAGE source resolution: 0.1 (10 cm, "
                             "~57 MB/tile) or 2.0 (2 m, ~100 KB/tile). "
                             "Default: 2.0")
    parser.add_argument("--tile-dir", type=str, default="scenery/orthophoto",
                        help="Output directory for tiles (default: "
                             "scenery/orthophoto)")
    parser.add_argument("--cache-dir", type=str,
                        default="terrain_cache/swissimage",
                        help="Downloaded tile cache directory")
    parser.add_argument("--tile-size", type=int, default=1024,
                        help="Tile size in meters, must match terrain tiles "
                             "(default: 1024)")
    parser.add_argument("--jpeg-quality", type=int, default=85,
                        help="JPEG quality 1-100 (default: 85)")
    parser.add_argument("--list-tiles", action="store_true",
                        help="Only list required source tiles")
    parser.add_argument("--skip-download", action="store_true",
                        help="Use cached tiles only")

    args = parser.parse_args()

    half = args.size // 2
    west = args.center_e - half
    east = args.center_e + half
    south = args.center_n - half
    north = args.center_n + half

    # Snap bounds to tile grid
    ts = args.tile_size
    west = (west // ts) * ts
    south = (south // ts) * ts
    east = ((east + ts - 1) // ts) * ts
    north = ((north + ts - 1) // ts) * ts

    print("=" * 60)
    print("SwissTopo SWISSIMAGE Orthophoto Processor for ProvPilot")
    print("=" * 60)
    print(f"\nCenter: E {args.center_e:,} / N {args.center_n:,}")
    print(f"Size: {args.size}m × {args.size}m ({args.size / 1000:.1f} km)")
    print(f"Bounds (snapped): W={west:,} S={south:,} E={east:,} N={north:,}")
    print(f"Source resolution: {args.source_resolution} m/pixel")
    print(f"Target resolution: {args.target_resolution} m/pixel")

    # Source tiles are 1 km × 1 km
    tiles = get_tile_ids_for_bounds(west, south, east, north)
    print(f"\nRequired source tiles: {len(tiles)}")

    est_mb_per_tile = 0.1 if args.source_resolution >= 2.0 else 57.0
    est_total_mb = len(tiles) * est_mb_per_tile
    print(f"Estimated download: {est_total_mb:.0f} MB "
          f"(~{est_mb_per_tile:.1f} MB/tile at {args.source_resolution}m)")

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

    # ── Download pipeline ──
    cache_dir = Path(args.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    if not args.skip_download:
        print("\n-- Step 1: Discovering tile URLs via SwissTopo API --")
        tile_urls = discover_tile_urls(tiles, args.source_resolution)
        print(f"  Found {len(tile_urls)}/{len(tiles)} tiles")

        if len(tile_urls) < len(tiles):
            missing = set(tiles) - set(tile_urls.keys())
            print(f"  Missing: {[f'{e}-{n}' for e, n in sorted(missing)]}")

        if not tile_urls:
            print("ERROR: No tiles found!")
            return 1

        # Save URL map for reference
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
        tif_paths = sorted(cache_dir.glob("swissimage-dop10_*.tif"))
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
    merged_path = cache_dir / "merged_orthophoto.tif"
    merge_and_crop_tiles(
        tif_paths, west, south, east, north,
        merged_path,
        target_resolution=args.target_resolution,
    )

    # Step 5: Generate tiles
    print("\n-- Step 5: Generating orthophoto tiles --")
    tile_dir = Path(args.tile_dir)
    manifest = generate_tiles(merged_path, tile_dir, args.tile_size,
                              args.jpeg_quality)

    print(f"\n{'=' * 60}")
    print("DONE! Orthophoto tiles generated.")
    print(f"  Directory: {tile_dir}")
    print(f"  Tiles: {manifest['tile_count']}")
    print(f"  Each: {args.tile_size}m × {args.tile_size}m "
          f"({args.tile_size}×{args.tile_size} px)")
    n_e = manifest["grid"]["ei_max"] - manifest["grid"]["ei_min"] + 1
    n_n = manifest["grid"]["ni_max"] - manifest["grid"]["ni_min"] + 1
    print(f"  Grid: {n_e} × {n_n} = "
          f"{n_e * args.tile_size / 1000:.1f} km × "
          f"{n_n * args.tile_size / 1000:.1f} km")
    print(f"\nIntegration:")
    print(f"  The terrain_streamer.gd will automatically load these tiles")
    print(f"  as Terrain3D color maps when orthophoto_directory is set.")
    print(f"{'=' * 60}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
