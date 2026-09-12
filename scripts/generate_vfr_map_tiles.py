#!/usr/bin/env python3
"""Generate cartographic VFR map tiles from swissALTI3D heightmaps.

Reads the elevation tiles (tile_{e}_{n}.raw, float32 LE, pixel(0,0) = SE
corner, cols E->W, rows S->N). Both resolutions of the scenery pipeline are
supported: 1 m/px (1024x1024 px) and the current 2 m/px (512x512 px); the
file name still encodes the SW corner of a 1024 m LV95 cell. Renders a
classic VFR sectional look: hillshade (NW light) x hypsometric tint.

The output reuses the LV95 tile grid family of the orthophoto layer so the
runtime loader (C++ VfrMap module) can index tiles exactly like Scenery3D
does — file names encode the SW corner (west edge E, south edge N) of each
cell:

    scenery/map/map_{e}_{n}.jpg          L0  64x64 px   16 m/px   (1024 m cells)
    scenery/map/mip3/map_{e}_{n}.jpg     L3  64x64 px   128 m/px  (8192 m cells)
    scenery/map/mip5/map_{e}_{n}.jpg     L5  64x64 px   512 m/px  (32768 m cells)

Orientation: output pixels keep the source convention (pixel(0,0) = SE
corner, rows S->N, cols E->W). The runtime flips both axes before turning a
tile into a texture.

Downsampling cascades from L0 (median over 16x16 blocks -> L3, then 8x8
-> L5), matching the cascade approach of generate_terrain_mipmap.py.

Usage:
    python scripts/generate_vfr_map_tiles.py [--workers N] [--dry-run]
    python scripts/generate_vfr_map_tiles.py --e-min 2600 --e-max 2700 \
        --n-min 1250 --n-max 1300          # L0 tile index range

Requirements: numpy, Pillow
"""

import argparse
import json
import math
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
from PIL import Image

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

L0_PX = 64            # output pixels per tile at every level
TILE_M = 1024         # L0 cell size (m) — same grid as orthophoto tiles

LEVELS = {
    # name: (cell_m, subdir) — all levels render at L0_PX x L0_PX pixels.
    "L0": (1024, ""),
    "L3": (8192, "mip3"),
    "L5": (32768, "mip5"),
}

SCENERY_ROOT = Path(__file__).resolve().parent.parent / "scenery"

# Hillshade light: NW direction (azimuth 315° from north, clockwise),
# sun altitude 45°.
LIGHT_AZIMUTH_DEG = 315.0
LIGHT_ALTITUDE_DEG = 45.0

# Vertical exaggeration for the gradient (meters of slope per meter).
SLOPE_SCALE = 1.0


def read_heightmap(path: Path):
    """Read a float32 LE heightmap tile (SE-corner origin).

    Returns (elev, px) where px is the pixel dimension (512 or 1024); the
    ground sampling interval is then TILE_M / px meters per pixel.
    """
    data = path.read_bytes()
    n = len(data) // 4
    for px in (1024, 512):
        if n == px * px:
            return np.frombuffer(data, dtype="<f4").reshape(px, px), px
    raise ValueError(f"unexpected heightmap size {len(data)} bytes in {path}")


def median_downsample(arr: np.ndarray, factor: int) -> np.ndarray:
    """Median of (factor x factor) blocks; keeps flat surfaces clean."""
    h, w = arr.shape
    new_h, new_w = h // factor, w // factor
    blocks = arr[: new_h * factor, : new_w * factor].reshape(
        new_h, factor, new_w, factor
    )
    return np.median(blocks, axis=(1, 3))


def hillshade(elev: np.ndarray, m_per_px: float) -> np.ndarray:
    """Shaded relief in [0, 1] from an elevation array (m).

    elev is stored with pixel(0,0) at the SE corner: rows go S->N (row index
    increases northward), cols go E->W (col index increases westward). Each
    pixel spans m_per_px meters.
    """
    # Slopes in meters/meter. Rows go S->N, cols go E->W (SE-corner origin),
    # so d/dn = +d(row)/dr and d/de = -d(col)/dc; each pixel spans m_per_px.
    dz_dn = np.gradient(elev, axis=0) / m_per_px
    dz_de = -np.gradient(elev, axis=1) / m_per_px

    az = math.radians(LIGHT_AZIMUTH_DEG)
    alt = math.radians(LIGHT_ALTITUDE_DEG)
    # Light vector pointing FROM the surface TOWARD the sun.
    lx = math.cos(alt) * math.sin(az)   # east component
    ly = math.cos(alt) * math.cos(az)   # north component
    lz = math.sin(alt)

    # Surface normal for z = f(e, n): (-dz/de, -dz/dn, 1).
    nx = -dz_de
    ny = -dz_dn
    nz = np.ones_like(nx)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)

    shade = (nx * lx + ny * ly + nz * lz) / norm
    return np.clip(shade, 0.0, 1.0)


def hypsometric_tint(elev: np.ndarray) -> np.ndarray:
    """VFR-sectional color ramp by elevation (m). Returns float RGB in [0,1]."""
    e = np.asarray(elev, dtype=np.float64)
    # Control points: (elevation m, R, G, B)
    stops = [
        (-50.0, 0.82, 0.87, 0.93),   # water / below datum: pale blue
        (0.0,   0.76, 0.84, 0.62),   # lowland green
        (500.0, 0.72, 0.80, 0.52),   # forest green
        (1000.0, 0.83, 0.79, 0.56),  # alpine meadow tan
        (1800.0, 0.80, 0.72, 0.55),  # brown scree
        (2500.0, 0.88, 0.84, 0.78),  # rocky grey
        (3200.0, 0.97, 0.97, 0.97),  # snow white
    ]
    xs = np.array([s[0] for s in stops])
    rs = np.array([s[1] for s in stops])
    gs = np.array([s[2] for s in stops])
    bs = np.array([s[3] for s in stops])
    r = np.interp(e, xs, rs)
    g = np.interp(e, xs, gs)
    b = np.interp(e, xs, bs)
    return np.stack([r, g, b], axis=-1)


def render_tile(elev: np.ndarray, m_per_px: float) -> Image.Image:
    """Compose hillshade x tint into an RGB image (SE-corner origin kept)."""
    shade = hillshade(elev, m_per_px)
    tint = hypsometric_tint(elev)
    # Shade multiplies the tint; keep a floor so valleys stay readable.
    rgb = tint * (0.35 + 0.65 * shade)[..., None]
    out = np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGB")


def tile_is_empty(elev: np.ndarray) -> bool:
    """No-data tiles are all-zero in the raw pipeline output."""
    return float(elev.max(initial=0.0)) <= 0.0


# ---------------------------------------------------------------------------
# Per-tile workers (module level so ProcessPoolExecutor can pickle them)
# ---------------------------------------------------------------------------

def process_l0_tile(args):
    ei, ni, terrain_dir = args
    path = Path(terrain_dir) / f"tile_{ei * TILE_M}_{ni * TILE_M}.raw"
    if not path.exists():
        return (ei, ni, "missing", None)
    elev, px = read_heightmap(path)
    if tile_is_empty(elev):
        return (ei, ni, "empty", None)
    elev64 = median_downsample(elev, px // L0_PX)  # 1024 m cell -> 64 px at 16 m/px
    img = render_tile(elev64, TILE_M / L0_PX)
    return (ei, ni, "ok", img)


def _read_cell_grid(span: int, ei0: int, ni0: int, terrain_dir: str):
    """Read a span x span block of L0 tiles; returns (stack, px0) or None.

    Missing tiles (outside the elevation data extent — e.g. along the Swiss
    border where swissALTI3D stops) are zero-filled instead of failing the
    whole cell: no-data renders as a flat lowland tint, so boundary cells
    still cover the area instead of dropping out. Returns None only when no
    tile of the block exists at all.

    Normalizes mixed-resolution directories to the first tile's pixel size
    (2x median down or 2x nearest-neighbor up).
    """
    tdir = Path(terrain_dir)
    paths = [(dy, dx, tdir / f"tile_{(ei0 + dx) * TILE_M}_{(ni0 + dy) * TILE_M}.raw")
             for dy in range(span) for dx in range(span)]
    existing = [p for _, _, p in paths if p.exists()]
    if not existing:
        return None
    _, px0 = read_heightmap(existing[0])

    rows = []
    for dy in range(span):
        row = []
        for dx in range(span):
            p = tdir / f"tile_{(ei0 + dx) * TILE_M}_{(ni0 + dy) * TILE_M}.raw"
            if p.exists():
                elev, px = read_heightmap(p)
                if px != px0:
                    elev = median_downsample(elev, 2) if px > px0 else \
                        np.repeat(np.repeat(elev, 2, axis=0), 2, axis=1)
            else:
                elev = np.zeros((px0, px0), dtype=np.float32)
            # Raw tiles store cols E->W, but cells are concatenated W->E.
            # Flip each tile's columns so the assembled grid runs smoothly
            # west->east; a plain concat leaves a one-cell jump-back at every
            # subcell boundary (visible as vertical seams in L3/L5 imagery).
            row.append(elev[:, ::-1])
        rows.append(row)
    grid = np.concatenate([np.concatenate(r, axis=1) for r in rows], axis=0)
    # Restore the SE-corner convention shared with L0 tiles (pixel(0,0)=SE):
    # flip the smooth W->E grid back to E->W.
    return grid[:, ::-1], px0


def make_l3_cell(args):
    """One L3 cell = 8x8 L0 tiles. Median-downsample to a 64 px elevation."""
    ei3, ni3, terrain_dir = args
    grid = _read_cell_grid(8, ei3 * 8, ni3 * 8, terrain_dir)
    if grid is None:
        return (ei3, ni3, "missing", None)
    stack, px0 = grid
    if tile_is_empty(stack):
        return (ei3, ni3, "empty", None)
    elev64 = median_downsample(stack, 8 * px0 // L0_PX)  # 8192 m -> 64 px at 128 m/px
    img = render_tile(elev64, 8192.0 / L0_PX)
    return (ei3, ni3, "ok", img)


def make_l5_cell(args):
    """One L5 cell = 32x32 L0 tiles -> 64 px elevation at 512 m/px."""
    ei5, ni5, terrain_dir = args
    grid = _read_cell_grid(32, ei5 * 32, ni5 * 32, terrain_dir)
    if grid is None:
        return (ei5, ni5, "missing", None)
    stack, px0 = grid
    if tile_is_empty(stack):
        return (ei5, ni5, "empty", None)
    elev64 = median_downsample(stack, 32 * px0 // L0_PX)  # 32768 m -> 64 px at 512 m/px
    img = render_tile(elev64, 32768.0 / L0_PX)
    return (ei5, ni5, "ok", img)


def save_level(out_dir: Path, subdir: str, ei, ni, cell_m: int, img: Image.Image, quality: int):
    d = out_dir / subdir if subdir else out_dir
    d.mkdir(parents=True, exist_ok=True)
    name = f"map_{ei * cell_m}_{ni * cell_m}.jpg"
    img.save(d / name, "JPEG", quality=quality)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate VFR map tiles (hillshade + hypsometric tint) from swissALTI3D .raw heightmaps."
    )
    parser.add_argument("--terrain-dir", default=str(SCENERY_ROOT / "terrain"),
                        help="Directory with tile_{e}_{n}.raw files")
    parser.add_argument("--output", default=str(SCENERY_ROOT / "map"),
                        help="Output map directory (default: scenery/map)")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--quality", type=int, default=82, help="JPEG quality")
    parser.add_argument("--e-min", type=int, default=None, help="L0 tile index range (east), inclusive")
    parser.add_argument("--e-max", type=int, default=None)
    parser.add_argument("--n-min", type=int, default=None, help="L0 tile index range (north), inclusive")
    parser.add_argument("--n-max", type=int, default=None)
    parser.add_argument("--mips-only", action="store_true",
                        help="Skip L0 regeneration; rebuild only mip3/mip5 from the existing terrain")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    terrain_dir = Path(args.terrain_dir)
    out_dir = Path(args.output)

    # Discover L0 tiles on disk (or use the requested index range).
    if args.e_min is not None:
        assert args.e_max is not None and args.n_min is not None and args.n_max is not None, \
            "--e-min/--e-max/--n-min/--n-max must be given together"
        l0_tiles = [(ei, ni) for ni in range(args.n_min, args.n_max + 1)
                    for ei in range(args.e_min, args.e_max + 1)]
    else:
        l0_tiles = []
        for f in sorted(terrain_dir.glob("tile_*.raw")):
            try:
                e_n = f.stem.split("_", 1)[1].split("_")
                ei, ni = int(e_n[0]) // TILE_M, int(e_n[1]) // TILE_M
                l0_tiles.append((ei, ni))
            except (IndexError, ValueError):
                continue

    if not l0_tiles:
        print(f"No L0 tiles found in {terrain_dir}")
        sys.exit(1)

    e_min = min(e for e, _ in l0_tiles)
    e_max = max(e for e, _ in l0_tiles)
    n_min = min(n for _, n in l0_tiles)
    n_max = max(n for _, n in l0_tiles)

    # L3 / L5 cell index ranges derived from the L0 extent.
    l3_cells = [(ei // 8, ni // 8) for ei, ni in l0_tiles]
    l3_cells = sorted(set(l3_cells))
    l5_cells = sorted(set((ei // 32, ni // 32) for ei, ni in l0_tiles))

    print(f"L0 tiles : {len(l0_tiles)}  (E {e_min*TILE_M}-{(e_max+1)*TILE_M}, N {n_min*TILE_M}-{(n_max+1)*TILE_M})")
    print(f"L3 cells : {len(l3_cells)}   (8192 m)")
    print(f"L5 cells : {len(l5_cells)}   (32768 m)")

    if args.dry_run:
        return

    stats = {"L0": 0, "L3": 0, "L5": 0, "empty": 0, "missing": 0}
    t0 = time.time()

    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        # L0
        if not args.mips_only:
            futures = {pool.submit(process_l0_tile, (ei, ni, str(terrain_dir))): (ei, ni)
                       for ei, ni in l0_tiles}
            for fut in as_completed(futures):
                ei, ni, status, img = fut.result()
                if status == "ok":
                    save_level(out_dir, "", ei, ni, TILE_M, img, args.quality)
                    stats["L0"] += 1
                else:
                    stats[status] += 1
        else:
            print("L0 skipped (--mips-only)")

        # L3 (needs the full 8x8 neighborhood — read raw directly)
        futures = {pool.submit(make_l3_cell, (ei, ni, str(terrain_dir))): (ei, ni)
                   for ei, ni in l3_cells}
        for fut in as_completed(futures):
            ei, ni, status, img = fut.result()
            if status == "ok":
                save_level(out_dir, "mip3", ei, ni, 8192, img, args.quality)
                stats["L3"] += 1
            else:
                stats[status] += 1

        # L5
        futures = {pool.submit(make_l5_cell, (ei, ni, str(terrain_dir))): (ei, ni)
                   for ei, ni in l5_cells}
        for fut in as_completed(futures):
            ei, ni, status, img = fut.result()
            if status == "ok":
                save_level(out_dir, "mip5", ei, ni, 32768, img, args.quality)
                stats["L5"] += 1
            else:
                stats[status] += 1

    # Manifest (same spirit as the orthophoto/terrain manifests).
    manifest = {
        "format": "vfr_map_tiles_v1",
        "generator": "scripts/generate_vfr_map_tiles.py",
        "orientation": "pixel(0,0)=SE corner, rows S->N, cols E->W (runtime flips both axes)",
        "levels": {
            "L0": {"cell_m": 1024, "px": L0_PX, "subdir": "", "count": stats["L0"]},
            "L3": {"cell_m": 8192, "px": L0_PX, "subdir": "mip3", "count": stats["L3"]},
            "L5": {"cell_m": 32768, "px": L0_PX, "subdir": "mip5", "count": stats["L5"]},
        },
        "light": {"azimuth_deg": LIGHT_AZIMUTH_DEG, "altitude_deg": LIGHT_ALTITUDE_DEG},
        "stats": stats,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    print(f"Done in {time.time() - t0:.1f}s — L0={stats['L0']} L3={stats['L3']} L5={stats['L5']} "
          f"(empty={stats['empty']}, missing={stats['missing']})")


if __name__ == "__main__":
    main()
