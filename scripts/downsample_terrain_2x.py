#!/usr/bin/env python3
"""
Downsample existing 1 m/px heightmap tiles (1024×1024 float32 raw LE) to
2 m/px (512×512 float32 raw LE) via 2×2 block averaging, ignoring zero
samples (nodata).

Use this to convert pre-baked terrain directories (e.g. BW) so they match
the global Scenery3D `tile_px = 512` configuration without going back to
the raster source.

Input/output layout (must be preserved):
  - Filename: tile_<lv95_east>_<lv95_north>.raw (SW corner, multiples of 1024).
  - Pixel (0,0) = SE corner, cols E→W, rows S→N.

Usage:
  downsample_terrain_2x.py SRC_DIR DST_DIR [--jobs N] [--overwrite]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np

TILE_SIZE_M = 1024
SRC_PX = 1024
DST_PX = 512
EXPECTED_SRC_BYTES = SRC_PX * SRC_PX * 4
EXPECTED_DST_BYTES = DST_PX * DST_PX * 4

NAME_RE = re.compile(r"^tile_(\d+)_(\d+)\.raw$")


def downsample_file(src_path: Path, dst_path: Path) -> tuple[str, float, float] | None:
    """Read 1024² float32, 2×2 average → 512², write. Returns (name, min, max)."""
    try:
        data = np.fromfile(src_path, dtype="<f4", count=SRC_PX * SRC_PX)
        if data.size != SRC_PX * SRC_PX:
            print(f"WARN: {src_path.name} short read ({data.size})", file=sys.stderr)
            return None
        arr = data.reshape(SRC_PX, SRC_PX)

        # 2×2 block average, ignoring zeros (nodata sentinel).
        a = arr.reshape(DST_PX, 2, DST_PX, 2)
        valid = (a != 0.0).astype(np.float32)
        s = (a * valid).sum(axis=(1, 3))
        n = valid.sum(axis=(1, 3))
        out = np.where(n > 0, s / np.maximum(n, 1.0), 0.0).astype("<f4")

        dst_path.parent.mkdir(parents=True, exist_ok=True)
        # Write atomically.
        tmp = dst_path.with_suffix(".raw.tmp")
        out.tofile(tmp)
        os.replace(tmp, dst_path)

        nz = out[out != 0.0]
        if nz.size == 0:
            return (dst_path.name, 0.0, 0.0)
        return (dst_path.name, float(nz.min()), float(nz.max()))
    except Exception as exc:
        print(f"ERROR processing {src_path}: {exc}", file=sys.stderr)
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("dst", type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--overwrite", action="store_true")
    args = ap.parse_args()

    if not args.src.is_dir():
        print(f"src not a directory: {args.src}", file=sys.stderr)
        return 1
    args.dst.mkdir(parents=True, exist_ok=True)

    tiles = []
    for entry in os.scandir(args.src):
        if not entry.is_file():
            continue
        m = NAME_RE.match(entry.name)
        if not m:
            continue
        size = entry.stat().st_size
        if size != EXPECTED_SRC_BYTES:
            print(f"WARN: {entry.name} unexpected size {size}, skipping", file=sys.stderr)
            continue
        dst = args.dst / entry.name
        if dst.exists() and not args.overwrite and dst.stat().st_size == EXPECTED_DST_BYTES:
            continue
        tiles.append((Path(entry.path), dst, int(m.group(1)), int(m.group(2))))

    print(f"Processing {len(tiles)} tiles with {args.jobs} workers → {args.dst}")
    manifest_tiles: dict[str, dict] = {}
    ei_min = ni_min = 10**9
    ei_max = ni_max = -10**9

    done = 0
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(downsample_file, src, dst): (src, dst, e_m, n_m)
            for src, dst, e_m, n_m in tiles
        }
        for fut in as_completed(futures):
            src, dst, e_m, n_m = futures[fut]
            res = fut.result()
            done += 1
            if res is None:
                continue
            name, mn, mx = res
            ei = e_m // TILE_SIZE_M
            ni = n_m // TILE_SIZE_M
            manifest_tiles[f"{e_m}_{n_m}"] = {
                "swiss_e": e_m,
                "swiss_n": n_m,
                "tile_ei": ei,
                "tile_ni": ni,
                "file": name,
                "min_elev": mn,
                "max_elev": mx,
            }
            ei_min = min(ei_min, ei); ei_max = max(ei_max, ei)
            ni_min = min(ni_min, ni); ni_max = max(ni_max, ni)
            if done % 500 == 0:
                print(f"  {done}/{len(tiles)}")

    # Include any tiles already present in dst (resumed run).
    for entry in os.scandir(args.dst):
        if not entry.is_file():
            continue
        m = NAME_RE.match(entry.name)
        if not m:
            continue
        key = f"{m.group(1)}_{m.group(2)}"
        if key in manifest_tiles:
            continue
        if entry.stat().st_size != EXPECTED_DST_BYTES:
            continue
        e_m = int(m.group(1)); n_m = int(m.group(2))
        ei = e_m // TILE_SIZE_M; ni = n_m // TILE_SIZE_M
        # Cheap min/max read.
        arr = np.fromfile(Path(entry.path), dtype="<f4")
        nz = arr[arr != 0.0]
        mn = float(nz.min()) if nz.size else 0.0
        mx = float(nz.max()) if nz.size else 0.0
        manifest_tiles[key] = {
            "swiss_e": e_m, "swiss_n": n_m, "tile_ei": ei, "tile_ni": ni,
            "file": entry.name, "min_elev": mn, "max_elev": mx,
        }
        ei_min = min(ei_min, ei); ei_max = max(ei_max, ei)
        ni_min = min(ni_min, ni); ni_max = max(ni_max, ni)

    manifest = {
        "tile_size_m": TILE_SIZE_M,
        "pixels_per_tile": DST_PX,
        "pixel_resolution_m": float(TILE_SIZE_M) / DST_PX,
        "format": "float32_raw_le",
        "bytes_per_tile": EXPECTED_DST_BYTES,
        "orientation": {
            "pixel_0_0": "SE corner of Swiss tile",
            "col_direction": "East to West (Godot +X)",
            "row_direction": "South to North (Godot +Z)",
        },
        "grid": {
            "ei_min": ei_min, "ei_max": ei_max,
            "ni_min": ni_min, "ni_max": ni_max,
        },
        "source": "downsample_terrain_2x.py 2x2 block-average from 1m source",
        "tiles": manifest_tiles,
    }
    with open(args.dst / "manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"Wrote {len(manifest_tiles)} tiles + manifest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
