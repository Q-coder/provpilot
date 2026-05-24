#!/usr/bin/env python3
"""
Streaming SwissTopo swissALTI3D → Scenery3D heightmap tile generator.

Unlike the legacy `process_swisstopo_terrain.py`, this script processes one
output tile at a time and never builds a country-sized in-memory mosaic. That
makes it possible to regenerate the whole Switzerland heightmap at lower
pixel resolution (e.g. 2 m/px) in a single pass without running out of RAM.

Pipeline per output tile:
  1.  Find every cached source GeoTIFF whose 1 km × 1 km LV95 footprint
      intersects the output tile bounds.
  2.  Allocate a `pixels_per_tile × pixels_per_tile` float32 destination
      buffer and `rasterio.warp.reproject` each intersecting source into it.
  3.  Flip the buffer to match Scenery3D's SE-corner convention
      (pixel (0,0) = SE corner; cols E→W, rows S→N).
  4.  Write `tile_<E>_<N>.raw` and remember min/max elevation for the
      manifest.

The output manifest mirrors the schema produced by
`process_swisstopo_terrain.py::generate_tiles`, but with `pixels_per_tile`
and `pixel_resolution_m` set from the CLI so downstream consumers (the
Scenery3D GDExtension) can detect non-1 m/px tiles.

Coordinate convention (must not change without C++ updates):
  - Tile filename: `tile_<lv95_east_m>_<lv95_north_m>.raw`
    where the meters are the SW corner of the tile (multiples of tile_size_m).
  - Pixel (0,0) of the raw buffer = SE corner of the tile.
  - cols increase westward (+X in Godot); rows increase northward (+Z).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
import rasterio
from rasterio.transform import from_origin
from rasterio.warp import Resampling, reproject

# Filename pattern: swissalti3d_<year>_<E_km>-<N_km>_<res>_2056_5728.tif
SOURCE_RE = re.compile(
    r"^swissalti3d_(\d{4})_(\d{4})-(\d{4})_([0-9.]+)_2056_5728\.tif$"
)


def index_sources(cache_dir: Path) -> dict[tuple[int, int], Path]:
    """Map (e_km, n_km) → newest available .tif path."""
    by_cell: dict[tuple[int, int], tuple[int, Path]] = {}
    for entry in os.scandir(cache_dir):
        if not entry.is_file():
            continue
        m = SOURCE_RE.match(entry.name)
        if not m:
            continue
        year = int(m.group(1))
        e_km = int(m.group(2))
        n_km = int(m.group(3))
        key = (e_km, n_km)
        prev = by_cell.get(key)
        if prev is None or year > prev[0]:
            by_cell[key] = (year, Path(entry.path))
    return {k: v[1] for k, v in by_cell.items()}


def output_tile_for_source_grid(
    sources: dict[tuple[int, int], Path], tile_size_m: int
) -> tuple[int, int, int, int]:
    """Return (ei_min, ei_max, ni_min, ni_max) covering all source cells."""
    if not sources:
        raise SystemExit("No source TIFs found in cache dir.")
    e_kms = [e for (e, n) in sources]
    n_kms = [n for (e, n) in sources]
    # Source km cell (e_km, n_km) covers LV95 E ∈ [e_km*1000, (e_km+1)*1000).
    # Output tile ei covers LV95 E ∈ [ei*ts_m, (ei+1)*ts_m).
    ei_min = (min(e_kms) * 1000) // tile_size_m
    ei_max = ((max(e_kms) + 1) * 1000 - 1) // tile_size_m
    ni_min = (min(n_kms) * 1000) // tile_size_m
    ni_max = ((max(n_kms) + 1) * 1000 - 1) // tile_size_m
    return int(ei_min), int(ei_max), int(ni_min), int(ni_max)


def intersecting_sources(
    ei: int, ni: int, tile_size_m: int,
    sources: dict[tuple[int, int], Path],
) -> list[Path]:
    """Return paths of source TIFs that intersect output tile (ei, ni)."""
    e_lo = ei * tile_size_m
    e_hi = (ei + 1) * tile_size_m  # exclusive
    n_lo = ni * tile_size_m
    n_hi = (ni + 1) * tile_size_m
    # Source cell (e_km, n_km) covers [e_km*1000, (e_km+1)*1000).
    e_km_start = e_lo // 1000
    e_km_end = (e_hi - 1) // 1000
    n_km_start = n_lo // 1000
    n_km_end = (n_hi - 1) // 1000
    found: list[Path] = []
    for e_km in range(e_km_start, e_km_end + 1):
        for n_km in range(n_km_start, n_km_end + 1):
            p = sources.get((e_km, n_km))
            if p is not None:
                found.append(p)
    return found


def process_tile(
    ei: int, ni: int, tile_size_m: int, tile_px: int,
    src_paths: list[Path], out_path: Path,
) -> tuple[int, int, float, float, bool]:
    """Build one output tile. Returns (ei, ni, min_elev, max_elev, ok)."""
    pixel_res = tile_size_m / tile_px
    # Rasterio-native orientation: dst_array[0, 0] = NW corner of tile.
    # west = ei*ts_m, north = (ni+1)*ts_m. xres=yres=pixel_res.
    west = ei * tile_size_m
    north = (ni + 1) * tile_size_m
    dst_transform = from_origin(west, north, pixel_res, pixel_res)
    dst = np.zeros((tile_px, tile_px), dtype=np.float32)
    have_data = False

    for src_path in src_paths:
        try:
            with rasterio.open(src_path) as src:
                src_nodata = src.nodata if src.nodata is not None else -9999.0
                tmp = np.full((tile_px, tile_px), 0.0, dtype=np.float32)
                reproject(
                    source=rasterio.band(src, 1),
                    destination=tmp,
                    src_transform=src.transform,
                    src_crs=src.crs,
                    src_nodata=src_nodata,
                    dst_transform=dst_transform,
                    dst_crs="EPSG:2056",
                    dst_nodata=0.0,
                    resampling=Resampling.average,
                )
        except Exception as e:
            print(f"  WARN: {src_path.name}: {e}", file=sys.stderr)
            continue
        # Merge: fill zeros in dst from tmp where tmp has data.
        # Treat any negative / extreme sentinel as nodata as well.
        mask = (tmp != 0.0) & np.isfinite(tmp) & (tmp > -1000.0) & (tmp < 10000.0)
        if mask.any():
            empty = dst == 0.0
            apply = mask & empty
            dst[apply] = tmp[apply]
            have_data = True

    if not have_data:
        return ei, ni, 0.0, 0.0, False

    # Apply SE-corner orientation: rasterio is row 0 = north, col 0 = west.
    # We need row 0 = south, col 0 = east. Flip both axes.
    oriented = np.ascontiguousarray(np.flipud(np.fliplr(dst)))

    valid = oriented[oriented != 0.0]
    if valid.size == 0:
        return ei, ni, 0.0, 0.0, False
    mn = float(np.round(valid.min(), 1))
    mx = float(np.round(valid.max(), 1))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(oriented.tobytes(order="C"))
    return ei, ni, mn, mx, True


def _worker(args):
    return process_tile(*args)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cache-dir", type=Path, required=True,
                    help="Directory containing swissalti3d_*.tif source tiles.")
    ap.add_argument("--output-dir", type=Path, required=True,
                    help="Destination for tile_<E>_<N>.raw + manifest.json.")
    ap.add_argument("--tile-size-m", type=int, default=1024,
                    help="LV95 meters covered by one output tile axis.")
    ap.add_argument("--tile-px", type=int, default=512,
                    help="Pixels per output tile axis.")
    ap.add_argument("--ei-min", type=int, default=None)
    ap.add_argument("--ei-max", type=int, default=None)
    ap.add_argument("--ni-min", type=int, default=None)
    ap.add_argument("--ni-max", type=int, default=None)
    ap.add_argument("--workers", type=int,
                    default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument("--limit", type=int, default=None,
                    help="Stop after this many output tiles (testing).")
    ap.add_argument("--overwrite", action="store_true",
                    help="Re-process tiles whose .raw already exists.")
    args = ap.parse_args()

    if args.tile_size_m % args.tile_px != 0 and (args.tile_size_m / args.tile_px) % 1 != 0:
        # Non-integer pixel resolution is fine, just warn.
        print(f"NOTE: pixel_resolution_m = {args.tile_size_m / args.tile_px}")

    print(f"Indexing source tiles in {args.cache_dir} ...")
    sources = index_sources(args.cache_dir)
    print(f"  found {len(sources)} unique km cells")

    ei_min0, ei_max0, ni_min0, ni_max0 = output_tile_for_source_grid(
        sources, args.tile_size_m)
    ei_min = args.ei_min if args.ei_min is not None else ei_min0
    ei_max = args.ei_max if args.ei_max is not None else ei_max0
    ni_min = args.ni_min if args.ni_min is not None else ni_min0
    ni_max = args.ni_max if args.ni_max is not None else ni_max0
    print(f"Output grid: ei [{ei_min}..{ei_max}], ni [{ni_min}..{ni_max}]")

    # Build job list.
    jobs = []
    skipped = 0
    for ei in range(ei_min, ei_max + 1):
        for ni in range(ni_min, ni_max + 1):
            srcs = intersecting_sources(ei, ni, args.tile_size_m, sources)
            if not srcs:
                continue
            lv95_e = ei * args.tile_size_m
            lv95_n = ni * args.tile_size_m
            out_name = f"tile_{lv95_e}_{lv95_n}.raw"
            out_path = args.output_dir / out_name
            if out_path.exists() and not args.overwrite:
                skipped += 1
                continue
            jobs.append((ei, ni, args.tile_size_m, args.tile_px, srcs, out_path))
            if args.limit and len(jobs) >= args.limit:
                break
        if args.limit and len(jobs) >= args.limit:
            break
    print(f"Tiles to build: {len(jobs)} (skipped existing: {skipped})")

    args.output_dir.mkdir(parents=True, exist_ok=True)

    # Load or create manifest header. Re-process keeps existing entries.
    manifest_path = args.output_dir / "manifest.json"
    if manifest_path.exists() and not args.overwrite:
        try:
            manifest = json.load(open(manifest_path))
        except Exception:
            manifest = None
    else:
        manifest = None
    if manifest is None:
        manifest = {
            "tile_size_m": args.tile_size_m,
            "pixels_per_tile": args.tile_px,
            "pixel_resolution_m": args.tile_size_m / args.tile_px,
            "format": "float32_raw_le",
            "bytes_per_tile": args.tile_px * args.tile_px * 4,
            "orientation": {
                "pixel_0_0": "SE corner of Swiss tile",
                "col_direction": "East to West (Godot +X)",
                "row_direction": "South to North (Godot +Z)",
            },
            "grid": {
                "ei_min": ei_min, "ei_max": ei_max,
                "ni_min": ni_min, "ni_max": ni_max,
            },
            "region_mapping": {
                "description": (
                    "Given floating origin (CENTER_E, CENTER_N) aligned to "
                    "tile_size boundaries: region_x = CENTER_E/tile_size_m - "
                    "tile_ei - 1, region_z = tile_ni - CENTER_N/tile_size_m"
                ),
            },
            "tiles": {},
        }
    tiles_dict = manifest["tiles"]

    done = 0
    failed = 0
    total = len(jobs)
    save_every = max(200, total // 50) if total else 0
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futures = [ex.submit(_worker, j) for j in jobs]
        for fut in as_completed(futures):
            ei, ni, mn, mx, ok = fut.result()
            done += 1
            if not ok:
                failed += 1
                continue
            lv95_e = ei * args.tile_size_m
            lv95_n = ni * args.tile_size_m
            key = f"{lv95_e}_{lv95_n}"
            tiles_dict[key] = {
                "swiss_e": lv95_e,
                "swiss_n": lv95_n,
                "tile_ei": ei,
                "tile_ni": ni,
                "file": f"tile_{lv95_e}_{lv95_n}.raw",
                "min_elev": mn,
                "max_elev": mx,
            }
            if done % 100 == 0 or done == total:
                print(f"  {done}/{total} ({failed} no-data)", flush=True)
            if save_every and done % save_every == 0:
                manifest["tile_count"] = len(tiles_dict)
                with open(manifest_path, "w") as f:
                    json.dump(manifest, f, indent=2)

    manifest["tile_count"] = len(tiles_dict)
    # Recompute grid bounds from final tile set.
    if tiles_dict:
        eis = [t["tile_ei"] for t in tiles_dict.values()]
        nis = [t["tile_ni"] for t in tiles_dict.values()]
        manifest["grid"] = {
            "ei_min": min(eis), "ei_max": max(eis),
            "ni_min": min(nis), "ni_max": max(nis),
        }
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"Done. wrote {len(tiles_dict)} tiles to {args.output_dir}")
    print(f"  manifest: {manifest_path}")
    print(f"  no-data tiles: {failed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
