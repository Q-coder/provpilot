#!/usr/bin/env python3
"""Generate multi-resolution heightmap and orthophoto pyramids.

Reads the 42,400 source tiles (1 m/px, 1024×1024) and generates lower-
resolution tile pyramids for the clipmap rings beyond Terrain3D.

Mip levels
----------
  Level 0: 1 m/px,   1024×1024 — Terrain3D (existing, not generated)
  Level 2: 16 m/px,  64×64     — Ring 0
  Level 3: 64 m/px,  16×16     — Ring 1
  Level 4: 256 m/px, 4×4       — Ring 2

Level 1 (4 m/px) is skipped — Terrain3D covers that range internally.

Downsampling uses **median** for heightmaps (preserves flat surfaces like
lakes and valley floors) and Lanczos for orthophotos (preserves color quality).

Levels are generated in cascade:
  source (1024²) → mip2 (64²) → mip3 (16²) → mip4 (4²)

Output layout
-------------
  scenery/terrain/mip{2,3,4}/   — heightmap tiles + manifest.json
  scenery/orthophoto/mip{2,3,4}/ — orthophoto tiles + manifest.json

Usage
-----
    python scripts/generate_terrain_mipmap.py [--workers N] [--terrain-only] [--ortho-only]
    python scripts/generate_terrain_mipmap.py --level 2   # generate only mip2
    python scripts/generate_terrain_mipmap.py --dry-run    # show what would be generated

Requirements: numpy, Pillow
"""

import argparse
import json
import os
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

TILE_PX = 1024          # source tile pixels per side
TILE_BYTES = TILE_PX * TILE_PX * 4  # float32

MIP_LEVELS = {
    2: {"factor_from_source": 16, "px": 64,  "res_m": 16,  "parent": None},
    3: {"factor_from_parent": 4,  "px": 16,  "res_m": 64,  "parent": 2},
    4: {"factor_from_parent": 4,  "px": 4,   "res_m": 256, "parent": 3},
}

SCENERY_ROOT = Path(__file__).resolve().parent.parent / "scenery"
TERRAIN_DIR = SCENERY_ROOT / "terrain"
ORTHO_DIR = SCENERY_ROOT / "orthophoto"

# ---------------------------------------------------------------------------
# Heightmap helpers
# ---------------------------------------------------------------------------

def read_heightmap(path: Path) -> np.ndarray:
    """Read a float32 raw LE heightmap tile."""
    data = path.read_bytes()
    return np.frombuffer(data, dtype="<f4").reshape(TILE_PX, TILE_PX)


def median_downsample(arr: np.ndarray, factor: int) -> np.ndarray:
    """Downsample a 2-D array by taking the median of (factor×factor) blocks."""
    h, w = arr.shape
    new_h, new_w = h // factor, w // factor
    # Reshape into blocks and compute median
    blocks = arr[:new_h * factor, :new_w * factor].reshape(
        new_h, factor, new_w, factor
    )
    return np.median(blocks, axis=(1, 3)).astype(arr.dtype)


def write_heightmap(path: Path, arr: np.ndarray) -> None:
    """Write a float32 raw LE heightmap tile."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(arr.astype("<f4").tobytes())


# ---------------------------------------------------------------------------
# Orthophoto helpers
# ---------------------------------------------------------------------------

def downsample_ortho(src_path: Path, dst_path: Path, target_px: int,
                     jpeg_quality: int = 85) -> int:
    """Downsample a JPEG orthophoto to target_px × target_px using Lanczos."""
    from PIL import Image
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(src_path) as img:
        resized = img.resize((target_px, target_px), Image.LANCZOS)
        resized.save(dst_path, "JPEG", quality=jpeg_quality)
    return dst_path.stat().st_size


# ---------------------------------------------------------------------------
# Worker functions (run in child processes)
# ---------------------------------------------------------------------------

def _process_terrain_tile_mip2(args):
    """Generate mip2 (64×64) from a source 1024×1024 tile."""
    key, src_file, dst_file = args
    src_path = Path(src_file)
    dst_path = Path(dst_file)
    if not src_path.exists():
        return key, None, None, None
    arr = read_heightmap(src_path)
    down = median_downsample(arr, 16)  # 1024 / 16 = 64
    write_heightmap(dst_path, down)
    return key, float(np.min(down)), float(np.max(down)), dst_path.stat().st_size


def _process_terrain_tile_cascade(args):
    """Generate mip3 or mip4 from a parent mip tile."""
    key, src_file, dst_file, src_px, factor = args
    src_path = Path(src_file)
    dst_path = Path(dst_file)
    if not src_path.exists():
        return key, None, None, None
    data = src_path.read_bytes()
    arr = np.frombuffer(data, dtype="<f4").reshape(src_px, src_px)
    down = median_downsample(arr, factor)
    write_heightmap(dst_path, down)
    return key, float(np.min(down)), float(np.max(down)), dst_path.stat().st_size


def _process_ortho_tile_mip2(args):
    """Generate mip2 (64×64) orthophoto from source 1024×1024."""
    key, src_file, dst_file = args
    src_path = Path(src_file)
    dst_path = Path(dst_file)
    if not src_path.exists():
        return key, None
    sz = downsample_ortho(src_path, dst_path, 64)
    return key, sz


def _process_ortho_tile_cascade(args):
    """Generate mip3 or mip4 orthophoto from parent mip."""
    key, src_file, dst_file, target_px = args
    src_path = Path(src_file)
    dst_path = Path(dst_file)
    if not src_path.exists():
        return key, None
    sz = downsample_ortho(src_path, dst_path, target_px)
    return key, sz


# ---------------------------------------------------------------------------
# Main generation logic
# ---------------------------------------------------------------------------

def generate_terrain_mips(levels: list[int], workers: int, dry_run: bool):
    """Generate heightmap mipmaps for the requested levels."""
    manifest_path = TERRAIN_DIR / "manifest.json"
    if not manifest_path.exists():
        print(f"ERROR: terrain manifest not found: {manifest_path}", file=sys.stderr)
        return False

    manifest = json.loads(manifest_path.read_text())
    tiles = manifest["tiles"]
    print(f"Terrain manifest: {len(tiles)} tiles")

    for level in sorted(levels):
        info = MIP_LEVELS[level]
        target_px = info["px"]
        res_m = info["res_m"]
        out_dir = TERRAIN_DIR / f"mip{level}"

        print(f"\n{'='*60}")
        print(f"Mip level {level}: {res_m} m/px, {target_px}×{target_px} px")
        print(f"Output: {out_dir}")

        if level == 2:
            # Direct from source
            work_items = []
            for key, tile in tiles.items():
                src = TERRAIN_DIR / tile["file"]
                dst = out_dir / f"tile_{key}.raw"
                work_items.append((key, str(src), str(dst)))

            if dry_run:
                print(f"  Would generate {len(work_items)} tiles")
                continue

            out_dir.mkdir(parents=True, exist_ok=True)
            results = {}
            t0 = time.monotonic()

            with ProcessPoolExecutor(max_workers=workers) as pool:
                futures = {pool.submit(_process_terrain_tile_mip2, item): item[0]
                           for item in work_items}
                done = 0
                for future in as_completed(futures):
                    key, min_e, max_e, size = future.result()
                    if min_e is not None:
                        results[key] = {
                            "min_elev": round(min_e, 1),
                            "max_elev": round(max_e, 1),
                            "size_bytes": size,
                        }
                    done += 1
                    if done % 2000 == 0 or done == len(work_items):
                        elapsed = time.monotonic() - t0
                        rate = done / elapsed if elapsed > 0 else 0
                        print(f"  {done}/{len(work_items)} tiles "
                              f"({elapsed:.0f}s, {rate:.0f} tiles/s)")

        else:
            # Cascade from parent
            parent_level = info["parent"]
            parent_px = MIP_LEVELS[parent_level]["px"]
            factor = info["factor_from_parent"]
            parent_dir = TERRAIN_DIR / f"mip{parent_level}"

            work_items = []
            for key in tiles:
                src = parent_dir / f"tile_{key}.raw"
                dst = out_dir / f"tile_{key}.raw"
                work_items.append((key, str(src), str(dst), parent_px, factor))

            if dry_run:
                print(f"  Would generate {len(work_items)} tiles (from mip{parent_level})")
                continue

            out_dir.mkdir(parents=True, exist_ok=True)
            results = {}
            t0 = time.monotonic()

            with ProcessPoolExecutor(max_workers=workers) as pool:
                futures = {pool.submit(_process_terrain_tile_cascade, item): item[0]
                           for item in work_items}
                done = 0
                for future in as_completed(futures):
                    key, min_e, max_e, size = future.result()
                    if min_e is not None:
                        results[key] = {
                            "min_elev": round(min_e, 1),
                            "max_elev": round(max_e, 1),
                            "size_bytes": size,
                        }
                    done += 1
                    if done % 5000 == 0 or done == len(work_items):
                        elapsed = time.monotonic() - t0
                        rate = done / elapsed if elapsed > 0 else 0
                        print(f"  {done}/{len(work_items)} tiles "
                              f"({elapsed:.0f}s, {rate:.0f} tiles/s)")

        # Write manifest for this mip level
        mip_manifest = {
            "tile_size_m": 1024,
            "pixels_per_tile": target_px,
            "pixel_resolution_m": res_m,
            "format": "float32_raw_le",
            "bytes_per_tile": target_px * target_px * 4,
            "orientation": manifest["orientation"],
            "grid": manifest["grid"],
            "source_level": 0 if level == 2 else info["parent"],
            "mip_level": level,
            "tiles": {},
        }

        for key, tile in tiles.items():
            entry = {
                "swiss_e": tile["swiss_e"],
                "swiss_n": tile["swiss_n"],
                "tile_ei": tile["tile_ei"],
                "tile_ni": tile["tile_ni"],
                "file": f"tile_{key}.raw",
            }
            if key in results:
                entry["min_elev"] = results[key]["min_elev"]
                entry["max_elev"] = results[key]["max_elev"]
                entry["size_bytes"] = results[key]["size_bytes"]
            mip_manifest["tiles"][key] = entry

        mf_path = out_dir / "manifest.json"
        mf_path.write_text(json.dumps(mip_manifest, indent=2) + "\n")
        skipped = len(tiles) - len(results)
        print(f"  Manifest written: {mf_path} ({len(results)} tiles"
              f"{f', {skipped} skipped' if skipped else ''})")

    return True


def generate_ortho_mips(levels: list[int], workers: int, dry_run: bool):
    """Generate orthophoto mipmaps for the requested levels."""
    manifest_path = ORTHO_DIR / "manifest.json"
    if not manifest_path.exists():
        print(f"ERROR: orthophoto manifest not found: {manifest_path}",
              file=sys.stderr)
        return False

    manifest = json.loads(manifest_path.read_text())
    tiles = manifest["tiles"]
    print(f"\nOrthophoto manifest: {len(tiles)} tiles")

    for level in sorted(levels):
        info = MIP_LEVELS[level]
        target_px = info["px"]
        res_m = info["res_m"]
        out_dir = ORTHO_DIR / f"mip{level}"

        print(f"\n{'='*60}")
        print(f"Ortho mip level {level}: {res_m} m/px, {target_px}×{target_px} px")
        print(f"Output: {out_dir}")

        if level == 2:
            work_items = []
            for key, tile in tiles.items():
                src = ORTHO_DIR / tile["file"]
                dst = out_dir / f"ortho_{key}.jpg"
                work_items.append((key, str(src), str(dst)))

            if dry_run:
                print(f"  Would generate {len(work_items)} tiles")
                continue

            out_dir.mkdir(parents=True, exist_ok=True)
            results = {}
            t0 = time.monotonic()

            with ProcessPoolExecutor(max_workers=workers) as pool:
                futures = {pool.submit(_process_ortho_tile_mip2, item): item[0]
                           for item in work_items}
                done = 0
                for future in as_completed(futures):
                    key, size = future.result()
                    if size is not None:
                        results[key] = size
                    done += 1
                    if done % 2000 == 0 or done == len(work_items):
                        elapsed = time.monotonic() - t0
                        rate = done / elapsed if elapsed > 0 else 0
                        print(f"  {done}/{len(work_items)} tiles "
                              f"({elapsed:.0f}s, {rate:.0f} tiles/s)")
        else:
            parent_level = info["parent"]
            parent_dir = ORTHO_DIR / f"mip{parent_level}"

            work_items = []
            for key in tiles:
                src = parent_dir / f"ortho_{key}.jpg"
                dst = out_dir / f"ortho_{key}.jpg"
                work_items.append((key, str(src), str(dst), target_px))

            if dry_run:
                print(f"  Would generate {len(work_items)} tiles (from mip{parent_level})")
                continue

            out_dir.mkdir(parents=True, exist_ok=True)
            results = {}
            t0 = time.monotonic()

            with ProcessPoolExecutor(max_workers=workers) as pool:
                futures = {pool.submit(_process_ortho_tile_cascade, item): item[0]
                           for item in work_items}
                done = 0
                for future in as_completed(futures):
                    key, size = future.result()
                    if size is not None:
                        results[key] = size
                    done += 1
                    if done % 5000 == 0 or done == len(work_items):
                        elapsed = time.monotonic() - t0
                        rate = done / elapsed if elapsed > 0 else 0
                        print(f"  {done}/{len(work_items)} tiles "
                              f"({elapsed:.0f}s, {rate:.0f} tiles/s)")

        # Write manifest
        mip_manifest = {
            "tile_size_m": 1024,
            "pixels_per_tile": target_px,
            "pixel_resolution_m": res_m,
            "format": "jpeg_rgb",
            "jpeg_quality": 85,
            "orientation": manifest["orientation"],
            "grid": manifest["grid"],
            "source_level": 0 if level == 2 else info["parent"],
            "mip_level": level,
            "tiles": {},
        }

        for key, tile in tiles.items():
            entry = {
                "swiss_e": tile["swiss_e"],
                "swiss_n": tile["swiss_n"],
                "tile_ei": tile["tile_ei"],
                "tile_ni": tile["tile_ni"],
                "file": f"ortho_{key}.jpg",
            }
            if key in results:
                entry["size_bytes"] = results[key]
            mip_manifest["tiles"][key] = entry

        mf_path = out_dir / "manifest.json"
        mf_path.write_text(json.dumps(mip_manifest, indent=2) + "\n")
        skipped = len(tiles) - len(results)
        print(f"  Manifest written: {mf_path} ({len(results)} tiles"
              f"{f', {skipped} skipped' if skipped else ''})")

    return True


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate multi-resolution heightmap & orthophoto pyramids.")
    parser.add_argument("--workers", type=int, default=os.cpu_count(),
                        help="Number of parallel workers (default: CPU count)")
    parser.add_argument("--level", type=int, nargs="+", choices=[2, 3, 4],
                        help="Generate only specific mip levels (default: all)")
    parser.add_argument("--terrain-only", action="store_true",
                        help="Generate only heightmap mipmaps")
    parser.add_argument("--ortho-only", action="store_true",
                        help="Generate only orthophoto mipmaps")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be generated without writing")
    args = parser.parse_args()

    levels = args.level or [2, 3, 4]

    # Cascade requires parent level to exist first
    if 3 in levels and 2 not in levels:
        mip2_dir = TERRAIN_DIR / "mip2"
        if not mip2_dir.exists():
            print("ERROR: mip level 3 requires mip2 — add --level 2 3 or "
                  "generate mip2 first.", file=sys.stderr)
            sys.exit(1)
    if 4 in levels and 3 not in levels:
        mip3_dir = TERRAIN_DIR / "mip3"
        if not mip3_dir.exists():
            print("ERROR: mip level 4 requires mip3 — add --level 3 4 or "
                  "generate mip3 first.", file=sys.stderr)
            sys.exit(1)

    t_total = time.monotonic()
    ok = True

    if not args.ortho_only:
        ok = generate_terrain_mips(levels, args.workers, args.dry_run) and ok

    if not args.terrain_only:
        ok = generate_ortho_mips(levels, args.workers, args.dry_run) and ok

    elapsed = time.monotonic() - t_total
    print(f"\nTotal time: {elapsed:.0f}s")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
