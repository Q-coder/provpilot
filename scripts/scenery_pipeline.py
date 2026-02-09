#!/usr/bin/env python3
"""
Unified scenery pipeline for ProvPilot.

Downloads and processes SwissTopo terrain (swissALTI3D) and building
(swissBUILDINGS3D 3.0) data in a chunk-based fashion, keeping disk usage
low by cleaning up intermediate files after each step.

Output:
  scenery/terrain/   — 1024×1024 m float32 .raw heightmap tiles + manifest.json
  scenery/buildings/  — GLB building models + manifest.json
  provpilot/Assets/buildings_*/ — GLB copies for Godot res:// loading

Usage:
  # Process terrain + buildings for 20 km around Schmerlat:
  python scripts/scenery_pipeline.py \\
      --center-e 2682880 --center-n 1283072 --radius 10000

  # Terrain only (incremental — skips existing tiles):
  python scripts/scenery_pipeline.py \\
      --center-e 2682880 --center-n 1283072 --radius 15000 --terrain-only

  # Buildings only:
  python scripts/scenery_pipeline.py \\
      --center-e 2682880 --center-n 1283072 --radius 12000 --buildings-only

  # Dry run (list what would be downloaded):
  python scripts/scenery_pipeline.py \\
      --center-e 2682880 --center-n 1283072 --radius 10000 --dry-run

  # Clean up downloaded source files after processing:
  python scripts/scenery_pipeline.py \\
      --center-e 2682880 --center-n 1283072 --radius 10000 --clean-cache

Requirements:
  pip install rasterio numpy requests tqdm fiona pyproj shapely
"""

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

try:
    import numpy as np
    import rasterio
    from rasterio.merge import merge as rasterio_merge
    from rasterio.transform import from_bounds as transform_from_bounds
    from rasterio.warp import reproject, Resampling
    import requests
    from tqdm import tqdm
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install rasterio numpy requests tqdm")
    sys.exit(1)

# ── Configuration ────────────────────────────────────────────────────────────

WORKSPACE_ROOT = Path(__file__).parent.parent
SCENERY_DIR = WORKSPACE_ROOT / "scenery"
TERRAIN_DIR = SCENERY_DIR / "terrain"
BUILDINGS_DIR = SCENERY_DIR / "buildings"
GODOT_ASSETS_DIR = WORKSPACE_ROOT / "provpilot" / "Assets"
DEFAULT_CACHE_DIR = WORKSPACE_ROOT / "terrain_cache"

TILE_SIZE = 1024  # meters, must match Terrain3D region_size

# SwissTopo API
STAC_BASE = "https://data.geo.admin.ch/api/stac/v0.9"
ALTI3D_DATA = "https://data.geo.admin.ch/ch.swisstopo.swissalti3d"
BUILDINGS_COLLECTION = "ch.swisstopo.swissbuildings3d_3_0"


# ══════════════════════════════════════════════════════════════════════════════
# COORDINATE HELPERS
# ══════════════════════════════════════════════════════════════════════════════

def lv95_to_wgs84(easting, northing):
    """Approximate LV95 → WGS84 conversion."""
    y = (easting - 2600000) / 1000000
    x = (northing - 1200000) / 1000000
    lat = (16.9023892 + 3.238272 * x - 0.270978 * y**2
           - 0.002528 * x**2 - 0.0447 * y**2 * x - 0.0140 * x**3) * 100 / 36
    lon = (2.6779094 + 4.728982 * y + 0.791484 * y * x
           + 0.1306 * y * x**2 - 0.0436 * y**3) * 100 / 36
    return lat, lon


# ══════════════════════════════════════════════════════════════════════════════
# TERRAIN PIPELINE
# ══════════════════════════════════════════════════════════════════════════════

def terrain_get_needed_tiles(west, south, east, north):
    """Return list of (easting_km, northing_km) SwissTopo tile coords."""
    tiles = []
    for e_km in range(west // 1000, (east - 1) // 1000 + 1):
        for n_km in range(south // 1000, (north - 1) // 1000 + 1):
            tiles.append((e_km, n_km))
    return tiles


def terrain_discover_url(e_km, n_km, resolution=0.5):
    """Try direct URL construction for a SwissTopo ALTI3D tile."""
    res_str = "0.5" if resolution == 0.5 else str(int(resolution))
    for year in [2024, 2023, 2022, 2021, 2020, 2019]:
        tile_id = f"swissalti3d_{year}_{e_km}-{n_km}"
        filename = f"{tile_id}_{res_str}_2056_5728.tif"
        url = f"{ALTI3D_DATA}/{tile_id}/{filename}"
        try:
            resp = requests.head(url, timeout=10, allow_redirects=True)
            if resp.status_code == 200:
                return url
        except requests.RequestException:
            continue
    return None


def terrain_download_tile(url, cache_dir):
    """Download a single tile, skip if cached."""
    filename = url.split("/")[-1]
    output_path = cache_dir / filename
    if output_path.exists() and output_path.stat().st_size > 0:
        return output_path
    resp = requests.get(url, stream=True, timeout=120)
    resp.raise_for_status()
    with open(output_path, "wb") as f:
        for chunk in resp.iter_content(chunk_size=65536):
            f.write(chunk)
    return output_path


def terrain_merge_and_generate_tiles(tif_paths, west, south, east, north,
                                     tile_dir, tile_size=TILE_SIZE,
                                     target_resolution=1.0,
                                     skip_existing=None):
    """
    Merge GeoTIFFs, crop, resample to target_resolution, then split into
    tile_size × tile_size float32 raw tiles for Terrain3D streaming.

    skip_existing: set of tile keys ("easting_northing") to skip writing.
    Returns dict of new tiles: {tile_key: tile_info}
    """
    print(f"\n  Merging {len(tif_paths)} GeoTIFF tiles...")
    datasets = []
    for p in tif_paths:
        try:
            datasets.append(rasterio.open(p))
        except Exception as e:
            print(f"    WARNING: Could not open {p}: {e}")

    if not datasets:
        raise RuntimeError("No valid GeoTIFFs to merge")

    crs = datasets[0].crs

    # Snap bounds to tile grid
    grid_west = (west // tile_size) * tile_size
    grid_south = (south // tile_size) * tile_size
    grid_east = ((east + tile_size - 1) // tile_size) * tile_size
    grid_north = ((north + tile_size - 1) // tile_size) * tile_size

    mosaic, mosaic_transform = rasterio_merge(
        datasets, bounds=(grid_west, grid_south, grid_east, grid_north)
    )
    for ds in datasets:
        ds.close()

    data = mosaic[0]
    print(f"  Merged: {data.shape[1]}×{data.shape[0]} px")

    # Resample to target resolution if needed
    src_res = abs(mosaic_transform.a)
    target_w = int((grid_east - grid_west) / target_resolution)
    target_h = int((grid_north - grid_south) / target_resolution)

    if data.shape[1] != target_w or data.shape[0] != target_h:
        print(f"  Resampling {src_res:.2f}m → {target_resolution:.2f}m "
              f"({target_w}×{target_h} px)")
        dst_transform = transform_from_bounds(
            grid_west, grid_south, grid_east, grid_north, target_w, target_h
        )
        dst_data = np.zeros((target_h, target_w), dtype=data.dtype)
        reproject(
            source=data, destination=dst_data,
            src_transform=mosaic_transform, src_crs=crs,
            dst_transform=dst_transform, dst_crs=crs,
            resampling=Resampling.bilinear,
        )
        data = dst_data
    else:
        dst_transform = mosaic_transform

    # Handle nodata
    data = data.astype(np.float32)
    data[data < 0] = 0
    data[np.isnan(data)] = 0

    # Generate individual tiles
    tile_dir.mkdir(parents=True, exist_ok=True)

    ei_min = grid_west // tile_size
    ei_max = grid_east // tile_size - 1
    ni_min = grid_south // tile_size
    ni_max = grid_north // tile_size - 1

    new_tiles = {}
    pix_per_tile = tile_size  # 1 pixel per meter at 1m resolution
    if skip_existing is None:
        skip_existing = set()

    for ei in range(ei_min, ei_max + 1):
        for ni in range(ni_min, ni_max + 1):
            e_sw = ei * tile_size
            n_sw = ni * tile_size
            tile_key = f"{e_sw}_{n_sw}"

            # Skip tiles that already exist on disk
            if tile_key in skip_existing:
                continue

            # Pixel coords in the merged array (GeoTIFF: row 0 = north)
            col_start = int(round((e_sw - grid_west) / target_resolution))
            col_end = col_start + pix_per_tile
            row_start = int(round((grid_north - n_sw - tile_size) / target_resolution))
            row_end = row_start + pix_per_tile

            if (col_end <= 0 or col_start >= data.shape[1] or
                    row_end <= 0 or row_start >= data.shape[0]):
                continue

            tile_data = np.zeros((pix_per_tile, pix_per_tile), dtype=np.float32)
            sr0, sr1 = max(0, row_start), min(data.shape[0], row_end)
            sc0, sc1 = max(0, col_start), min(data.shape[1], col_end)
            dr0, dr1 = sr0 - row_start, sr0 - row_start + (sr1 - sr0)
            dc0, dc1 = sc0 - col_start, sc0 - col_start + (sc1 - sc0)
            tile_data[dr0:dr1, dc0:dc1] = data[sr0:sr1, sc0:sc1]

            # Apply Terrain3D orientation: pixel(0,0) = SE corner
            tile_data = np.flipud(np.fliplr(tile_data))

            tile_name = f"tile_{e_sw}_{n_sw}.raw"
            tile_path = tile_dir / tile_name
            tile_data.astype("<f4").tofile(tile_path)

            valid = tile_data[tile_data > 0]
            new_tiles[f"{e_sw}_{n_sw}"] = {
                "swiss_e": int(e_sw),
                "swiss_n": int(n_sw),
                "tile_ei": int(ei),
                "tile_ni": int(ni),
                "file": tile_name,
                "min_elev": round(float(valid.min()), 1) if len(valid) > 0 else 0.0,
                "max_elev": round(float(tile_data.max()), 1),
            }

    return new_tiles


def terrain_update_manifest(tile_dir, new_tiles):
    """Merge new tiles into existing manifest.json."""
    manifest_path = tile_dir / "manifest.json"
    if manifest_path.exists():
        with open(manifest_path) as f:
            manifest = json.load(f)
    else:
        manifest = {
            "tile_size_m": TILE_SIZE,
            "pixels_per_tile": TILE_SIZE,
            "pixel_resolution_m": 1.0,
            "format": "float32_raw_le",
            "bytes_per_tile": TILE_SIZE * TILE_SIZE * 4,
            "orientation": {
                "pixel_0_0": "SE corner of Swiss tile",
                "col_direction": "East to West (Godot +X)",
                "row_direction": "South to North (Godot +Z)",
            },
            "grid": {},
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

    # Merge tiles
    manifest["tiles"].update(new_tiles)
    manifest["tile_count"] = len(manifest["tiles"])

    # Recompute grid bounds
    all_ei = [t["tile_ei"] for t in manifest["tiles"].values()]
    all_ni = [t["tile_ni"] for t in manifest["tiles"].values()]
    manifest["grid"] = {
        "ei_min": min(all_ei),
        "ei_max": max(all_ei),
        "ni_min": min(all_ni),
        "ni_max": max(all_ni),
    }

    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"  Manifest updated: {manifest['tile_count']} total tiles")
    return manifest


def run_terrain_pipeline(center_e, center_n, radius, cache_dir,
                         tile_resolution=0.5, dry_run=False, clean_cache=False):
    """Full terrain pipeline: discover → download → merge → tile → manifest."""
    half = radius
    west = ((center_e - half) // TILE_SIZE) * TILE_SIZE
    south = ((center_n - half) // TILE_SIZE) * TILE_SIZE
    east = ((center_e + half + TILE_SIZE - 1) // TILE_SIZE) * TILE_SIZE
    north = ((center_n + half + TILE_SIZE - 1) // TILE_SIZE) * TILE_SIZE

    print(f"\n{'='*60}")
    print(f"TERRAIN PIPELINE")
    print(f"{'='*60}")
    print(f"  Bounds: W={west} S={south} E={east} N={north}")
    print(f"  Coverage: {(east-west)/1000:.0f}×{(north-south)/1000:.0f} km")

    # Check which tiles already exist
    existing = set()
    terrain_manifest = TERRAIN_DIR / "manifest.json"
    if terrain_manifest.exists():
        with open(terrain_manifest) as f:
            existing = set(json.load(f).get("tiles", {}).keys())

    # Determine needed 1024m output tiles
    needed_output = set()
    for ei in range(west // TILE_SIZE, east // TILE_SIZE):
        for ni in range(south // TILE_SIZE, north // TILE_SIZE):
            key = f"{ei * TILE_SIZE}_{ni * TILE_SIZE}"
            if key not in existing:
                needed_output.add((ei, ni))

    if not needed_output:
        print(f"  All terrain tiles already exist — skipping.")
        return

    print(f"  New output tiles needed: {len(needed_output)}")
    print(f"  Already existing: {len(existing)}")

    # Determine source SwissTopo 1km tiles needed
    source_tiles = terrain_get_needed_tiles(west, south, east, north)
    print(f"  SwissTopo source tiles: {len(source_tiles)}")

    if dry_run:
        print(f"\n  [DRY RUN] Would download {len(source_tiles)} TIF tiles")
        print(f"  [DRY RUN] Would generate {len(needed_output)} terrain tiles")
        return

    cache_dir.mkdir(parents=True, exist_ok=True)

    # Discover & download
    print(f"\n  Discovering tile URLs...")
    tile_urls = {}
    for e_km, n_km in tqdm(source_tiles, desc="  Discovering"):
        url = terrain_discover_url(e_km, n_km, tile_resolution)
        if url:
            tile_urls[(e_km, n_km)] = url
        else:
            pass  # Tiles near borders may not exist

    print(f"  Found {len(tile_urls)}/{len(source_tiles)} source tiles")

    if not tile_urls:
        print("  ERROR: No source tiles found!")
        return

    print(f"\n  Downloading tiles...")
    tif_paths = []
    for (e_km, n_km), url in tqdm(tile_urls.items(), desc="  Downloading"):
        try:
            path = terrain_download_tile(url, cache_dir)
            tif_paths.append(path)
        except Exception as e:
            print(f"\n    ERROR downloading {e_km}-{n_km}: {e}")

    # Merge & generate tiles (skip tiles that already exist)
    new_tiles = terrain_merge_and_generate_tiles(
        tif_paths, west, south, east, north,
        TERRAIN_DIR, TILE_SIZE, target_resolution=1.0,
        skip_existing=existing,
    )

    if new_tiles:
        terrain_update_manifest(TERRAIN_DIR, new_tiles)
        print(f"  ✓ {len(new_tiles)} new terrain tiles generated")
    else:
        print(f"  No new tiles to add")

    # Clean cache if requested
    if clean_cache:
        for p in tif_paths:
            if p.exists():
                p.unlink()
        merged = cache_dir / "merged_terrain.tif"
        if merged.exists():
            merged.unlink()
        print(f"  Cleaned {len(tif_paths)} cached TIF files")


# ══════════════════════════════════════════════════════════════════════════════
# BUILDINGS PIPELINE
# ══════════════════════════════════════════════════════════════════════════════

def buildings_discover_tiles(west, south, east, north):
    """
    Search STAC API for swissBUILDINGS3D tiles covering the bounding box.
    Returns list of dicts: [{id, year, gdb_url, bbox}, ...]
    """
    lat_s, lon_w = lv95_to_wgs84(west, south)
    lat_n, lon_e = lv95_to_wgs84(east, north)

    tiles = []
    cursor = None

    while True:
        url = (f"{STAC_BASE}/collections/{BUILDINGS_COLLECTION}/items"
               f"?bbox={lon_w},{lat_s},{lon_e},{lat_n}&limit=50")
        if cursor:
            url += f"&cursor={cursor}"

        try:
            resp = requests.get(url, timeout=30)
            resp.raise_for_status()
            data = resp.json()
        except Exception as e:
            print(f"  STAC search error: {e}")
            break

        for feature in data.get("features", []):
            fid = feature["id"]
            # Extract tile ID: swissbuildings3d_3_0_2019_1031-14
            # splits to ["swissbuildings3d","3","0","2019","1031-14"]
            parts = fid.split("_")
            if len(parts) >= 5:
                year = parts[3]
                tile_id = parts[4]
                # Find GDB asset
                gdb_url = None
                for asset_key, asset in feature.get("assets", {}).items():
                    if asset_key.endswith(".gdb.zip"):
                        gdb_url = asset["href"]
                        break

                if gdb_url and tile_id:
                    tiles.append({
                        "stac_id": fid,
                        "tile_id": tile_id,
                        "year": year,
                        "gdb_url": gdb_url,
                        "bbox": feature.get("bbox", []),
                    })

        # Pagination
        next_link = None
        for link in data.get("links", []):
            if link.get("rel") == "next":
                next_link = link.get("href")
                break
        if next_link and data.get("features"):
            # Extract cursor from next link
            import urllib.parse
            parsed = urllib.parse.urlparse(next_link)
            params = urllib.parse.parse_qs(parsed.query)
            cursor = params.get("cursor", [None])[0]
            if not cursor:
                break
        else:
            break

    # Deduplicate by tile_id (keep newest year)
    by_tile = {}
    for t in tiles:
        tid = t["tile_id"]
        if tid not in by_tile or t["year"] > by_tile[tid]["year"]:
            by_tile[tid] = t

    return list(by_tile.values())


def buildings_download_gdb(tile_info, cache_dir):
    """Download and extract a GDB zip file. Returns path to .gdb directory."""
    zip_name = tile_info["gdb_url"].split("/")[-1]
    zip_path = cache_dir / zip_name
    gdb_name = zip_name.replace(".zip", "")

    # Check for extracted GDB
    gdb_path = cache_dir / gdb_name
    if gdb_path.exists() and gdb_path.is_dir():
        return gdb_path

    # Download zip
    if not zip_path.exists():
        resp = requests.get(tile_info["gdb_url"], stream=True, timeout=120)
        resp.raise_for_status()
        with open(zip_path, "wb") as f:
            for chunk in resp.iter_content(chunk_size=65536):
                f.write(chunk)

    # Extract
    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(cache_dir)

    # The GDB is a directory, find it
    if gdb_path.exists():
        return gdb_path

    # Try to find extracted GDB
    for p in cache_dir.iterdir():
        if p.suffix == ".gdb" and p.is_dir() and tile_info["tile_id"] in p.name:
            return p

    raise FileNotFoundError(f"Could not find extracted GDB for {tile_info['tile_id']}")


def buildings_convert_gdb_to_glb(gdb_path, output_dir, origin_e, origin_n,
                                  tile_id, layer_name="Building_solid"):
    """
    Convert a single GDB file to GLB format.
    Adapted from convert_buildings_to_gltf.py.

    Returns dict with center_e, center_n, file, building_count or None on failure.
    """
    try:
        import fiona
        from shapely.geometry import shape as shapely_shape
    except ImportError:
        print("  ERROR: fiona and shapely required for building conversion")
        print("  Install: pip install fiona shapely")
        return None

    layers = fiona.listlayers(str(gdb_path))
    if layer_name not in layers:
        for alt in ["Building_solid", "Roof_solid", "Wall"]:
            if alt in layers:
                layer_name = alt
                break
        else:
            print(f"  WARNING: No building layer in {gdb_path.name}, skipping")
            return None

    all_vertices = []
    all_triangles = []
    vertex_offset = 0
    building_count = 0

    # Track bounds for center computation
    all_e = []
    all_n = []

    with fiona.open(str(gdb_path), layer=layer_name) as src:
        total = len(src)
        for i, feature in enumerate(src):
            geom = feature.get("geometry")
            if geom is None:
                continue

            vertices, triangles = _parse_tin_geometry(geom, shapely_shape)
            if len(vertices) > 0 and len(triangles) > 0:
                # Track Swiss coordinates for center
                all_e.extend(vertices[:, 0].tolist())
                all_n.extend(vertices[:, 1].tolist())

                # Transform to Godot coords
                vertices = _transform_to_godot(vertices, origin_e, origin_n)
                triangles = _flip_winding(triangles)
                triangles = triangles + vertex_offset

                all_vertices.append(vertices)
                all_triangles.append(triangles)
                vertex_offset += len(vertices)
                building_count += 1

            if (i + 1) % 500 == 0:
                print(f"    {i+1}/{total} features...")

    if building_count == 0:
        return None

    vertices = np.vstack(all_vertices)
    triangles = np.vstack(all_triangles)
    normals = _compute_normals(vertices, triangles)

    # Create and write GLB
    glb_name = f"buildings_{tile_id}.glb"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / glb_name

    gltf = _create_gltf(vertices, normals, triangles, f"buildings_{tile_id}")
    if gltf is None:
        return None

    buffer = _create_gltf_buffer(vertices, normals, triangles)
    _write_glb(gltf, buffer, output_path)

    # Compute center in Swiss coords
    center_e = int(round(np.mean(all_e)))
    center_n = int(round(np.mean(all_n)))

    size_mb = output_path.stat().st_size / (1024 * 1024)
    print(f"    → {glb_name} ({size_mb:.1f} MB, {building_count} buildings, "
          f"center: E={center_e} N={center_n})")

    return {
        "file": glb_name,
        "center_e": center_e,
        "center_n": center_n,
        "building_count": building_count,
    }


def buildings_update_manifest(buildings_dir, origin_e, origin_n, new_tiles):
    """Merge new building tiles into existing manifest.json."""
    manifest_path = buildings_dir / "manifest.json"
    if manifest_path.exists():
        with open(manifest_path) as f:
            manifest = json.load(f)
    else:
        manifest = {
            "format": "glb",
            "source": "swissBUILDINGS3D",
            "conversion_origin_e": origin_e,
            "conversion_origin_n": origin_n,
            "tiles": {},
        }

    for tile_id, info in new_tiles.items():
        manifest["tiles"][tile_id] = info

    manifest["conversion_origin_e"] = origin_e
    manifest["conversion_origin_n"] = origin_n

    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"  Manifest updated: {len(manifest['tiles'])} total building tiles")


def run_buildings_pipeline(center_e, center_n, radius, cache_dir,
                           origin_e=None, origin_n=None,
                           dry_run=False, clean_cache=False):
    """Full buildings pipeline: discover → download GDB → convert to GLB → manifest."""
    half = radius
    west = center_e - half
    south = center_n - half
    east = center_e + half
    north = center_n + half

    # Check existing manifest — if one exists, REUSE its conversion origin
    # so new GLBs are compatible with existing ones.
    existing = set()
    bldg_manifest = BUILDINGS_DIR / "manifest.json"
    if bldg_manifest.exists():
        with open(bldg_manifest) as f:
            manifest_data = json.load(f)
        existing = set(manifest_data.get("tiles", {}).keys())
        existing_origin_e = manifest_data.get("conversion_origin_e")
        existing_origin_n = manifest_data.get("conversion_origin_n")
        if existing_origin_e and existing_origin_n:
            if origin_e is not None and origin_e != existing_origin_e:
                print(f"  WARNING: Overriding existing conversion origin "
                      f"E={existing_origin_e} → {origin_e}")
                print(f"  Existing GLBs will need reconversion!")
            else:
                origin_e = int(existing_origin_e)
                origin_n = int(existing_origin_n)
                print(f"  Using existing conversion origin: E={origin_e} N={origin_n}")

    # Fallback: use center as origin (first run)
    if origin_e is None:
        origin_e = center_e
    if origin_n is None:
        origin_n = center_n

    print(f"\n{'='*60}")
    print(f"BUILDINGS PIPELINE")
    print(f"{'='*60}")
    print(f"  Bounds: W={west} S={south} E={east} N={north}")
    print(f"  Conversion origin: E={origin_e} N={origin_n}")

    # Discover building tiles
    print(f"\n  Searching STAC API for building tiles...")
    stac_tiles = buildings_discover_tiles(west, south, east, north)
    print(f"  Found {len(stac_tiles)} building tiles")

    # Filter out existing
    new_tiles_to_process = [t for t in stac_tiles if t["tile_id"] not in existing]
    print(f"  Already have: {len(stac_tiles) - len(new_tiles_to_process)}")
    print(f"  New to process: {len(new_tiles_to_process)}")

    if not new_tiles_to_process:
        print(f"  All building tiles already exist — skipping.")
        return

    if dry_run:
        print(f"\n  [DRY RUN] Would download & convert {len(new_tiles_to_process)} tiles:")
        for t in new_tiles_to_process:
            print(f"    {t['tile_id']} (year {t['year']})")
        return

    buildings_cache = cache_dir / "buildings"
    buildings_cache.mkdir(parents=True, exist_ok=True)

    # All GLBs go in a single Godot Assets directory so the building_loader's
    # single res:// prefix (buildings_res_prefix) can find them all.
    godot_buildings = GODOT_ASSETS_DIR / "buildings"
    godot_buildings.mkdir(parents=True, exist_ok=True)

    new_manifest_tiles = {}

    for i, tile_info in enumerate(new_tiles_to_process):
        tile_id = tile_info["tile_id"]
        print(f"\n  [{i+1}/{len(new_tiles_to_process)}] Processing tile {tile_id}...")

        # Download GDB
        try:
            gdb_path = buildings_download_gdb(tile_info, buildings_cache)
        except Exception as e:
            print(f"    ERROR downloading: {e}")
            continue

        # Convert GDB → GLB
        result = buildings_convert_gdb_to_glb(
            gdb_path, BUILDINGS_DIR, origin_e, origin_n, tile_id
        )

        if result is None:
            print(f"    No buildings converted for {tile_id}")
            continue

        # Copy GLB to Godot Assets for res:// loading
        src_glb = BUILDINGS_DIR / result["file"]
        dst_glb = godot_buildings / result["file"]
        shutil.copy2(src_glb, dst_glb)

        new_manifest_tiles[tile_id] = {
            "file": result["file"],
            "center_e": result["center_e"],
            "center_n": result["center_n"],
        }

        # Clean up GDB after conversion (chunk-based: process one, delete source)
        if clean_cache:
            if gdb_path.exists():
                shutil.rmtree(gdb_path)
            zip_path = buildings_cache / (gdb_path.name + ".zip" if not gdb_path.name.endswith(".zip") else "")
            # Also remove the zip
            for zf in buildings_cache.glob(f"*{tile_id}*.zip"):
                zf.unlink()
            print(f"    Cleaned up source GDB for {tile_id}")

    if new_manifest_tiles:
        buildings_update_manifest(BUILDINGS_DIR, origin_e, origin_n, new_manifest_tiles)
        print(f"\n  ✓ {len(new_manifest_tiles)} new building tiles converted")
        print(f"  GLB copies in: {godot_buildings}")
    else:
        print(f"\n  No new building tiles were converted")


# ══════════════════════════════════════════════════════════════════════════════
# GEOMETRY HELPERS (from convert_buildings_to_gltf.py)
# ══════════════════════════════════════════════════════════════════════════════

def _parse_tin_geometry(geometry, shapely_shape_fn):
    """Parse TIN geometry into (vertices, triangles) numpy arrays."""
    vertices = []
    triangles = []
    vertex_map = {}

    geom = shapely_shape_fn(geometry)
    polys = []
    if geom.geom_type == "MultiPolygon":
        polys = list(geom.geoms)
    elif geom.geom_type == "Polygon":
        polys = [geom]

    for polygon in polys:
        coords = list(polygon.exterior.coords)
        if len(coords) >= 4:
            tri_indices = []
            for coord in coords[:3]:
                x, y = coord[0], coord[1]
                z = coord[2] if len(coord) > 2 else 0.0
                key = (round(x, 4), round(y, 4), round(z, 4))
                if key not in vertex_map:
                    vertex_map[key] = len(vertices)
                    vertices.append([x, y, z])
                tri_indices.append(vertex_map[key])
            if len(tri_indices) == 3:
                triangles.append(tri_indices)

    return (np.array(vertices, dtype=np.float32) if vertices else np.empty((0, 3), dtype=np.float32),
            np.array(triangles, dtype=np.uint32) if triangles else np.empty((0, 3), dtype=np.uint32))


def _transform_to_godot(vertices, origin_e, origin_n):
    """Swiss LV95 (E,N,Z) → Godot (X,Y,Z)."""
    if len(vertices) == 0:
        return vertices
    transformed = np.zeros_like(vertices)
    transformed[:, 0] = origin_e - vertices[:, 0]  # X = origin - E  (+X = West)
    transformed[:, 1] = vertices[:, 2]              # Y = height
    transformed[:, 2] = vertices[:, 1] - origin_n   # Z = N - origin  (+Z = North)
    return transformed


def _flip_winding(triangles):
    """Flip triangle winding order for coordinate axis inversion."""
    if len(triangles) == 0:
        return triangles
    flipped = triangles.copy()
    flipped[:, 1], flipped[:, 2] = triangles[:, 2].copy(), triangles[:, 1].copy()
    return flipped


def _compute_normals(vertices, triangles):
    """Per-vertex normals from face normals."""
    normals = np.zeros_like(vertices)
    for tri in triangles:
        v0, v1, v2 = vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]
        face_n = np.cross(v1 - v0, v2 - v0)
        norm = np.linalg.norm(face_n)
        if norm > 0:
            face_n /= norm
            for idx in tri:
                normals[idx] += face_n
    norms = np.linalg.norm(normals, axis=1, keepdims=True)
    norms[norms == 0] = 1
    normals /= norms
    return normals.astype(np.float32)


def _create_gltf_buffer(vertices, normals, triangles):
    """Binary buffer: vertices + normals + indices."""
    buf = vertices.astype(np.float32).tobytes()
    buf += normals.astype(np.float32).tobytes()
    if len(vertices) < 65536:
        buf += triangles.flatten().astype(np.uint16).tobytes()
    else:
        buf += triangles.flatten().astype(np.uint32).tobytes()
    return buf


def _create_gltf(vertices, normals, triangles, name="Building"):
    """Create GLTF 2.0 JSON dict."""
    if len(vertices) == 0 or len(triangles) == 0:
        return None

    v_min = vertices.min(axis=0).tolist()
    v_max = vertices.max(axis=0).tolist()

    vbl = len(vertices) * 12
    nbl = len(normals) * 12
    use16 = len(vertices) < 65536
    ibl = len(triangles) * 3 * (2 if use16 else 4)
    total = vbl + nbl + ibl

    return {
        "asset": {"version": "2.0", "generator": "ProvPilot scenery_pipeline"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
        ]}],
        "materials": [{"name": "BuildingMaterial", "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.75, 0.7, 1.0],
            "metallicFactor": 0.0, "roughnessFactor": 0.8
        }}],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0, "componentType": 5126,
             "count": len(vertices), "type": "VEC3", "min": v_min, "max": v_max},
            {"bufferView": 1, "byteOffset": 0, "componentType": 5126,
             "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "byteOffset": 0,
             "componentType": 5123 if use16 else 5125,
             "count": len(triangles) * 3, "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": vbl, "target": 34962},
            {"buffer": 0, "byteOffset": vbl, "byteLength": nbl, "target": 34962},
            {"buffer": 0, "byteOffset": vbl + nbl, "byteLength": ibl, "target": 34963},
        ],
        "buffers": [{"byteLength": total}],
    }


def _write_glb(gltf, buffer, output_path):
    """Write binary GLTF (GLB) file."""
    json_str = json.dumps(gltf, separators=(",", ":"))
    json_bytes = json_str.encode("utf-8")
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_pad

    buf_pad = (4 - len(buffer) % 4) % 4
    buffer += b"\x00" * buf_pad

    gltf["buffers"][0]["byteLength"] = len(buffer) - buf_pad
    json_str = json.dumps(gltf, separators=(",", ":"))
    json_bytes = json_str.encode("utf-8")
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_pad

    total = 12 + 8 + len(json_bytes) + 8 + len(buffer)

    with open(output_path, "wb") as f:
        f.write(b"glTF")
        f.write(struct.pack("<I", 2))
        f.write(struct.pack("<I", total))
        f.write(struct.pack("<I", len(json_bytes)))
        f.write(b"JSON")
        f.write(json_bytes)
        f.write(struct.pack("<I", len(buffer)))
        f.write(b"BIN\x00")
        f.write(buffer)


# ══════════════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="ProvPilot scenery pipeline — download & process SwissTopo data",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Full pipeline (terrain + buildings), 20 km coverage:
  %(prog)s --center-e 2682880 --center-n 1283072 --radius 10000

  # Terrain only, larger area:
  %(prog)s --center-e 2682880 --center-n 1283072 --radius 20000 --terrain-only

  # Buildings only, with cache cleanup:
  %(prog)s --center-e 2682880 --center-n 1283072 --radius 12000 --buildings-only --clean-cache

  # Dry run (list what would be downloaded):
  %(prog)s --center-e 2682880 --center-n 1283072 --radius 10000 --dry-run
        """,
    )

    parser.add_argument("--center-e", type=int, required=True,
                        help="Center Easting in LV95 (e.g. 2682880)")
    parser.add_argument("--center-n", type=int, required=True,
                        help="Center Northing in LV95 (e.g. 1283072)")
    parser.add_argument("--radius", type=int, default=10000,
                        help="Radius in meters from center (default: 10000)")
    parser.add_argument("--terrain-only", action="store_true",
                        help="Process terrain only")
    parser.add_argument("--buildings-only", action="store_true",
                        help="Process buildings only")
    parser.add_argument("--tile-resolution", type=float, default=0.5,
                        choices=[0.5, 2.0],
                        help="SwissTopo tile resolution (default: 0.5)")
    parser.add_argument("--cache-dir", type=str,
                        default=str(DEFAULT_CACHE_DIR),
                        help="Download cache directory")
    parser.add_argument("--dry-run", action="store_true",
                        help="List what would be done without downloading")
    parser.add_argument("--clean-cache", action="store_true",
                        help="Delete source files after processing")
    parser.add_argument("--building-origin-e", type=int, default=None,
                        help="Building conversion origin E (default: center-e)")
    parser.add_argument("--building-origin-n", type=int, default=None,
                        help="Building conversion origin N (default: center-n)")

    args = parser.parse_args()
    cache_dir = Path(args.cache_dir)

    print("=" * 60)
    print("ProvPilot Scenery Pipeline")
    print("=" * 60)
    print(f"Center: E={args.center_e} N={args.center_n}")
    print(f"Radius: {args.radius}m ({args.radius/1000:.0f}km)")
    lat, lon = lv95_to_wgs84(args.center_e, args.center_n)
    print(f"WGS84:  {lat:.5f}°N {lon:.5f}°E")
    print(f"Output: {SCENERY_DIR}")
    print(f"Cache:  {cache_dir}")

    do_terrain = not args.buildings_only
    do_buildings = not args.terrain_only

    if do_terrain:
        run_terrain_pipeline(
            args.center_e, args.center_n, args.radius, cache_dir,
            tile_resolution=args.tile_resolution,
            dry_run=args.dry_run, clean_cache=args.clean_cache,
        )

    if do_buildings:
        run_buildings_pipeline(
            args.center_e, args.center_n, args.radius, cache_dir,
            origin_e=args.building_origin_e,  # None = auto-detect from existing manifest
            origin_n=args.building_origin_n,
            dry_run=args.dry_run, clean_cache=args.clean_cache,
        )

    print(f"\n{'='*60}")
    print("Pipeline complete.")
    print(f"{'='*60}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
