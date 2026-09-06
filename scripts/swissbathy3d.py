#!/usr/bin/env python3
"""
SwissTopo swissBATHY3D (lake bathymetry) → Scenery3D heightmap tile generator.

swissBATHY3D is the depth model of the Swiss lakes: the surveyed lake *bed*,
in the same LV95 / LN02 reference frame and the same 1 km cell grid as
swissALTI3D, at 3 m resolution.  swissALTI3D itself models the flat water
*surface*, so on its own ProvPilot has no idea how deep a lake is.

This tool downloads one or more lakes from the swisstopo STAC API and rasterises
them into an **overlay** set of Scenery3D terrain tiles: byte-identical layout to
the regular terrain tiles (`tile_<E>_<N>.raw`, float32 LE, SE-corner origin), but
*sparse* — every pixel outside the surveyed lake bed is left at 0.0.

That sparsity is what makes the overlay work without any engine change:
`S3DTileManager` reads every configured `data_paths` entry for a tile and merges
them per pixel, with LATER paths overriding earlier ones wherever they are
non-zero.  So an overlay region listed *after* Switzerland replaces the flat
water surface with the lake bed and falls through to swissALTI3D everywhere else.

Typical use
-----------
    # list what swisstopo publishes
    python3 scripts/swissbathy3d.py --list

    # Bodensee into a dedicated overlay region
    python3 scripts/swissbathy3d.py --lakes bodensee \
        --cache-dir /Volumes/Data1/scenery_in_bathy \
        --base-terrain ~/provpilot/scenery/Switzerland/terrain \
                       ~/provpilot/scenery/Germany/Baden-Wuertemberg/terrain \
        --output ~/provpilot/scenery/Switzerland-Bathymetry/terrain

    # add more lakes later — the manifest is merged, existing tiles are kept
    python3 scripts/swissbathy3d.py --lakes zuerichsee walensee thunersee \
        --cache-dir /Volumes/Data1/scenery_in_bathy \
        --base-terrain ~/provpilot/scenery/Switzerland/terrain \
        --output ~/provpilot/scenery/Switzerland-Bathymetry/terrain

Then in ProvPilot: Settings → Scenery Library → Scan for Scenery.  The new
region is appended at the *end* of the priority list, which is exactly where it
must be for its pixels to win over Switzerland.

Coordinate convention (must match streaming_terrain_generator.py):
  - Tile filename `tile_<lv95_east_m>_<lv95_north_m>.raw`, SW corner metres.
  - Pixel (0,0) = SE corner; cols increase westward, rows increase northward.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
import rasterio
from rasterio.transform import from_origin
from rasterio.warp import Resampling, reproject

STAC_COLLECTION = "ch.swisstopo.swissbathy3d"
STAC_ITEMS_URL = (
    f"https://data.geo.admin.ch/api/stac/v0.9/collections/{STAC_COLLECTION}/items"
)
ASSET_SUFFIX = ".esriasciigrid.zip"

# swissBATHY3D_CHLV95_LN02_<E_km>_<N_km>.asc
ASC_RE = re.compile(r"^swissBATHY3D_CHLV95_LN02_(\d{4})_(\d{4})\.asc$", re.I)

# Plausible Swiss lake-bed elevations (m). Anything outside is treated as nodata.
MIN_VALID_ELEV = 100.0
MAX_VALID_ELEV = 3000.0


# --------------------------------------------------------------------------
# STAC / download
# --------------------------------------------------------------------------

def stac_items() -> dict[str, dict]:
    """Return {lake_id: asset_dict} for every published lake."""
    req = urllib.request.Request(
        STAC_ITEMS_URL + "?limit=100",
        headers={"User-Agent": "provpilot-swissbathy3d/1.0"},
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        doc = json.load(resp)
    out: dict[str, dict] = {}
    for feat in doc.get("features", []):
        item_id = feat["id"]
        # "swissbathy3d_bodensee" → "bodensee"
        lake = item_id.split("swissbathy3d_", 1)[-1]
        asset = None
        for name, a in feat.get("assets", {}).items():
            if name.endswith(ASSET_SUFFIX):
                asset = a
                break
        if asset is None:
            continue
        out[lake] = {
            "item_id": item_id,
            "href": asset["href"],
            "bbox_wgs84": feat.get("bbox"),
        }
    return out


def download(url: str, dest: Path) -> Path:
    """Download `url` to `dest`, skipping when the local size already matches."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    remote_size = None
    try:
        head = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(head, timeout=60) as r:
            remote_size = int(r.headers.get("Content-Length", "0")) or None
    except (urllib.error.URLError, OSError, ValueError):
        pass

    if dest.exists() and remote_size and dest.stat().st_size == remote_size:
        print(f"  cached  {dest.name} ({remote_size / 1e6:.0f} MB)")
        return dest

    print(f"  fetching {dest.name}"
          + (f" ({remote_size / 1e6:.0f} MB)" if remote_size else ""))
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        done = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if remote_size:
                pct = 100.0 * done / remote_size
                print(f"\r    {pct:5.1f}%  {done / 1e6:.0f} MB", end="", flush=True)
        print()
    tmp.replace(dest)
    return dest


def extract(zip_path: Path, out_dir: Path) -> int:
    """Extract the .asc members, skipping ones already on disk. Returns total."""
    out_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as zf:
        members = [m for m in zf.namelist() if m.lower().endswith(".asc")]
        for m in members:
            target = out_dir / Path(m).name
            info = zf.getinfo(m)
            if target.exists() and target.stat().st_size == info.file_size:
                continue
            with zf.open(m) as src, open(target, "wb") as dst:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
    return len(members)


# --------------------------------------------------------------------------
# Source indexing
# --------------------------------------------------------------------------

def read_asc_header(path: Path) -> tuple[float, float, float, float]:
    """Return the (e_min, n_min, e_max, n_max) LV95 footprint of an ASCII grid.

    Reading the six-line header is far cheaper than opening the multi-hundred-KB
    grid with GDAL, and swissBATHY3D cells are not exactly 1 km (they carry a
    1 m overlap into their neighbours), so the true bounds matter.
    """
    hdr: dict[str, float] = {}
    with open(path, "r") as f:
        for _ in range(6):
            line = f.readline()
            if not line:
                break
            parts = line.split()
            if len(parts) != 2:
                break
            try:
                hdr[parts[0].lower()] = float(parts[1])
            except ValueError:
                break
    try:
        ncols = hdr["ncols"]
        nrows = hdr["nrows"]
        xll = hdr["xllcorner"]
        yll = hdr["yllcorner"]
        cs = hdr["cellsize"]
    except KeyError as e:
        raise ValueError(f"{path.name}: malformed ASCII grid header ({e})")
    return xll, yll, xll + ncols * cs, yll + nrows * cs


def index_sources(dirs: list[Path]) -> list[tuple[tuple[float, float, float, float], Path]]:
    """Index every swissBATHY3D .asc under `dirs` by its true LV95 footprint."""
    found: dict[str, Path] = {}
    for d in dirs:
        if not d.is_dir():
            continue
        for entry in os.scandir(d):
            if entry.is_file() and ASC_RE.match(entry.name):
                found[entry.name] = Path(entry.path)
    out = []
    for name, path in sorted(found.items()):
        try:
            out.append((read_asc_header(path), path))
        except ValueError as e:
            print(f"  WARN: {e}", file=sys.stderr)
    return out


def intersecting_sources(ei: int, ni: int, tile_size_m: int, index) -> list[Path]:
    e_lo, e_hi = ei * tile_size_m, (ei + 1) * tile_size_m
    n_lo, n_hi = ni * tile_size_m, (ni + 1) * tile_size_m
    hits = []
    for (se_min, sn_min, se_max, sn_max), path in index:
        if se_max <= e_lo or se_min >= e_hi:
            continue
        if sn_max <= n_lo or sn_min >= n_hi:
            continue
        hits.append(path)
    return hits


# --------------------------------------------------------------------------
# Tiling
# --------------------------------------------------------------------------

def load_base_tile(base_dirs: list[Path], tile_name: str,
                   tile_px: int) -> np.ndarray | None:
    """Reconstruct the terrain the engine would show without this overlay.

    Mirrors `S3DTileManager`'s merge rule: read every configured data path in
    order, later sources overriding earlier ones wherever they are non-zero.
    """
    merged: np.ndarray | None = None
    expected = tile_px * tile_px
    for d in base_dirs:
        p = d / tile_name
        if not p.is_file():
            continue
        arr = np.fromfile(p, dtype="<f4")
        if arr.size != expected:
            print(f"  WARN: {p} has {arr.size} samples, expected {expected}",
                  file=sys.stderr)
            continue
        arr = arr.reshape(tile_px, tile_px)
        if merged is None:
            merged = arr.copy()
        else:
            m = arr != 0.0
            merged[m] = arr[m]
    return merged


def rasterise_tile(ei: int, ni: int, tile_size_m: int, tile_px: int,
                   src_paths: list[Path], resampling_name: str,
                   min_valid_elev: float = MIN_VALID_ELEV) -> np.ndarray:
    """Resample the intersecting ASCII grids onto one Scenery3D tile buffer.

    Returns a `tile_px²` float32 array in Scenery3D orientation (pixel (0,0) =
    SE corner), 0.0 wherever no lake bed was surveyed.
    """
    pixel_res = tile_size_m / tile_px
    dst_transform = from_origin(ei * tile_size_m, (ni + 1) * tile_size_m,
                                pixel_res, pixel_res)
    dst = np.zeros((tile_px, tile_px), dtype=np.float32)
    resampling = getattr(Resampling, resampling_name)

    for src_path in src_paths:
        try:
            with rasterio.open(src_path) as src:
                src_nodata = src.nodata if src.nodata is not None else -9999.0
                tmp = np.zeros((tile_px, tile_px), dtype=np.float32)
                reproject(
                    source=rasterio.band(src, 1),
                    destination=tmp,
                    src_transform=src.transform,
                    # AAIGrid carries no CRS; swissBATHY3D is LV95 by definition.
                    src_crs=src.crs or "EPSG:2056",
                    src_nodata=src_nodata,
                    dst_transform=dst_transform,
                    dst_crs="EPSG:2056",
                    dst_nodata=0.0,
                    resampling=resampling,
                )
        except Exception as e:  # noqa: BLE001 - one bad grid must not kill the run
            print(f"  WARN: {src_path.name}: {e}", file=sys.stderr)
            continue
        mask = (np.isfinite(tmp) & (tmp > min_valid_elev)
                & (tmp < MAX_VALID_ELEV))
        if mask.any():
            apply = mask & (dst == 0.0)
            dst[apply] = tmp[apply]

    # Orientation: rasterio gives row 0 = north / col 0 = west; Scenery3D wants
    # row 0 = south / col 0 = east.
    return np.ascontiguousarray(np.flipud(np.fliplr(dst)))


def process_tile(ei: int, ni: int, tile_size_m: int, tile_px: int,
                 src_paths: list[Path], out_path: Path, resampling_name: str,
                 base_dirs: list[Path], min_depth_m: float,
                 water_level_m: float | None,
                 water_level_tol_m: float,
                 min_valid_elev: float = MIN_VALID_ELEV) -> tuple[int, int, float, float, int]:
    """Build one sparse overlay tile.

    Returns (ei, ni, min_elev, max_elev, valid_px). valid_px == 0 means the tile
    carries no usable lake bed and must not be written — an all-zero tile would
    still be *found* by the engine and would contribute nothing, but it would
    waste 1 MB and slow the tile scan down.
    """
    oriented = rasterise_tile(ei, ni, tile_size_m, tile_px, src_paths,
                              resampling_name, min_valid_elev)

    # Two guards, both needed because the swissBATHY3D lake packages include a
    # land buffer around the shore taken from a coarser model:
    #   1. the overlay may only carve DOWNWARDS, never raise terrain;
    #   2. it may only touch pixels the base terrain models as the flat water
    #      surface — otherwise the land buffer punches craters into the hills
    #      around the lake wherever it happens to sit below swissALTI3D.
    if base_dirs:
        base = load_base_tile(base_dirs, out_path.name, tile_px)
        if base is None:
            oriented[:] = 0.0
        else:
            keep = (base != 0.0) & (oriented != 0.0)
            keep &= oriented < base - min_depth_m
            if water_level_m is not None:
                keep &= np.abs(base - water_level_m) <= water_level_tol_m
            oriented = np.ascontiguousarray(
                np.where(keep, oriented, np.float32(0.0)), dtype=np.float32)

    valid = oriented[oriented != 0.0]
    if valid.size == 0:
        # A previous run may have written a tile here; leave nothing behind.
        out_path.unlink(missing_ok=True)
        return ei, ni, 0.0, 0.0, 0

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(oriented.tobytes(order="C"))
    return (ei, ni, float(np.round(valid.min(), 1)),
            float(np.round(valid.max(), 1)), int(valid.size))


def detect_water_level(index, tile_size_m: int, tile_px: int,
                       base_dirs: list[Path], resampling_name: str,
                       sample_tiles: int = 16) -> float | None:
    """Estimate a lake's water surface elevation from the base terrain.

    swissALTI3D (and the German DGM) render a lake as a perfectly flat plateau
    at its regulated level, so the modal base elevation underneath the surveyed
    lake bed *is* the water level. Sampling the tiles with the most source
    coverage keeps the estimate in open water, away from the shore.
    """
    if not base_dirs:
        return None
    by_tile: dict[tuple[int, int], list[Path]] = {}
    for (se_min, sn_min, se_max, sn_max), path in index:
        for ei in range(int(se_min // tile_size_m), int(se_max // tile_size_m) + 1):
            for ni in range(int(sn_min // tile_size_m), int(sn_max // tile_size_m) + 1):
                by_tile.setdefault((ei, ni), []).append(path)
    ranked = sorted(by_tile.items(), key=lambda kv: -len(kv[1]))[:sample_tiles]

    hist: dict[float, int] = {}
    for (ei, ni), srcs in ranked:
        name = f"tile_{ei * tile_size_m}_{ni * tile_size_m}.raw"
        base = load_base_tile(base_dirs, name, tile_px)
        if base is None:
            continue
        bed = rasterise_tile(ei, ni, tile_size_m, tile_px, srcs, resampling_name)
        under = base[(bed != 0.0) & (base != 0.0)]
        if under.size == 0:
            continue
        vals, counts = np.unique(np.round(under, 1), return_counts=True)
        for v, c in zip(vals, counts):
            hist[float(v)] = hist.get(float(v), 0) + int(c)
    if not hist:
        return None
    return max(hist.items(), key=lambda kv: kv[1])[0]


def _worker(args):
    return process_tile(*args)


# --------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------

def write_manifest(out_dir: Path, tiles: dict, drop: set[str], tile_size_m: int,
                   tile_px: int, lakes: list[str]) -> None:
    """Write/merge manifest.json using the regular terrain manifest schema."""
    path = out_dir / "manifest.json"
    manifest: dict = {}
    if path.exists():
        try:
            manifest = json.loads(path.read_text())
        except json.JSONDecodeError:
            print(f"  WARN: {path} unreadable, rewriting from scratch",
                  file=sys.stderr)
            manifest = {}

    merged: dict = manifest.get("tiles", {})
    for key in drop:
        merged.pop(key, None)
    merged.update(tiles)
    if not merged:
        path.unlink(missing_ok=True)
        return

    known = set(manifest.get("lakes", [])) | set(lakes)
    eis = [t["tile_ei"] for t in merged.values()]
    nis = [t["tile_ni"] for t in merged.values()]

    manifest.update({
        "tile_size_m": tile_size_m,
        "pixels_per_tile": tile_px,
        "pixel_resolution_m": tile_size_m / tile_px,
        "format": "float32_raw_le",
        "bytes_per_tile": tile_px * tile_px * 4,
        "source": "swissBATHY3D (swisstopo, LV95/LN02)",
        "overlay": True,
        "overlay_note": (
            "Sparse lake-bed overlay: pixels outside the surveyed lake bed are "
            "0.0 so S3DTileManager falls back to the swissALTI3D terrain below. "
            "This region MUST be listed after Switzerland in the Scenery Library."
        ),
        "lakes": sorted(known),
        "orientation": {
            "pixel_0_0": "SE corner of Swiss tile",
            "col_direction": "East to West (Godot +X)",
            "row_direction": "South to North (Godot +Z)",
        },
        "grid": {
            "ei_min": min(eis), "ei_max": max(eis),
            "ni_min": min(nis), "ni_max": max(nis),
        },
        "tile_count": len(merged),
        "tiles": merged,
    })
    path.write_text(json.dumps(manifest, indent=1))


# --------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true",
                    help="List the lakes published by swisstopo and exit.")
    ap.add_argument("--lakes", nargs="+", default=[],
                    help="Lake ids to process (e.g. bodensee zuerichsee).")
    ap.add_argument("--all-lakes", action="store_true",
                    help="Process every lake in the collection.")
    ap.add_argument("--cache-dir", type=Path,
                    default=Path("/Volumes/Data1/scenery_in_bathy"),
                    help="Where the downloaded .zip and extracted .asc live.")
    ap.add_argument("--output", type=Path,
                    help="Destination terrain dir (tile_<E>_<N>.raw + manifest.json).")
    ap.add_argument("--tile-size-m", type=int, default=1024)
    ap.add_argument("--tile-px", type=int, default=512,
                    help="Must match the base terrain (CH is 512 px / 1024 m).")
    ap.add_argument("--base-terrain", type=Path, nargs="+", default=[],
                    help="Terrain dir(s) the overlay sits on top of, in Scenery "
                         "Library order (e.g. .../Switzerland/terrain "
                         ".../Baden-Wuerttemberg/terrain). Overlay pixels that "
                         "are not below this terrain are discarded. Strongly "
                         "recommended: swissBATHY3D ships a land buffer around "
                         "each lake that would otherwise raise the shoreline.")
    ap.add_argument("--min-depth-m", type=float, default=0.25,
                    help="Overlay pixels must be at least this far below the "
                         "base terrain to be kept.")
    ap.add_argument("--min-valid-elev", type=float, default=MIN_VALID_ELEV,
                    help=f"Bed elevations (m) below this are treated as nodata "
                         f"(default {MIN_VALID_ELEV:g}). Lower it for lakes whose "
                         "deepest bed sits below the default floor — e.g. "
                         "lacleman (~62 m) or lagomaggiore (~0 m) need 0.")
    ap.add_argument("--water-level", type=float, default=None,
                    help="Lake surface elevation (m). Default: auto-detected "
                         "per lake as the modal base-terrain height under the "
                         "surveyed bed. Only pixels the base terrain models as "
                         "water surface are carved.")
    ap.add_argument("--water-level-tol-m", type=float, default=2.0,
                    help="How far the base terrain may deviate from the water "
                         "level and still count as lake surface.")
    ap.add_argument("--resampling", default="average",
                    choices=["average", "bilinear", "nearest", "cubic"])
    ap.add_argument("--workers", type=int,
                    default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument("--overwrite", action="store_true",
                    help="Re-process tiles whose .raw already exists.")
    ap.add_argument("--no-download", action="store_true",
                    help="Use only the .asc files already in --cache-dir.")
    ap.add_argument("--no-convert", action="store_true",
                    help="Download + extract only.")
    args = ap.parse_args()

    if args.list:
        items = stac_items()
        print(f"{len(items)} lakes in {STAC_COLLECTION}:")
        for lake in sorted(items):
            bb = items[lake]["bbox_wgs84"]
            print(f"  {lake:24s} lon {bb[0]:.3f}..{bb[2]:.3f}  lat {bb[1]:.3f}..{bb[3]:.3f}")
        return 0

    lakes = list(args.lakes)
    items: dict[str, dict] = {}
    if args.all_lakes or lakes:
        if not args.no_download:
            items = stac_items()
        if args.all_lakes:
            lakes = sorted(items)
    if not lakes:
        ap.error("give --lakes <id> ... or --all-lakes (or --list)")

    if not args.no_download:
        unknown = [l for l in lakes if l not in items]
        if unknown:
            ap.error(f"unknown lake(s): {', '.join(unknown)} "
                     f"(run --list to see valid ids)")

    src_dirs: list[Path] = []
    for lake in lakes:
        lake_dir = args.cache_dir / lake
        if not args.no_download:
            asset = items[lake]
            print(f"[{lake}]")
            zip_path = download(asset["href"],
                                args.cache_dir / Path(asset["href"]).name)
            n = extract(zip_path, lake_dir)
            print(f"  {n} ASCII grid cells in {lake_dir}")
        src_dirs.append(lake_dir)

    if args.no_convert:
        return 0
    if args.output is None:
        ap.error("--output is required unless --no-convert is given")

    base_dirs = [Path(p).expanduser() for p in args.base_terrain]
    for d in base_dirs:
        if not d.is_dir():
            ap.error(f"--base-terrain dir does not exist: {d}")
    if not base_dirs:
        print("WARNING: no --base-terrain given — the swissBATHY3D land buffer "
              "around the shore will be baked into the overlay.", file=sys.stderr)

    all_tiles: dict[str, dict] = {}
    all_dropped: set[str] = set()
    converted: list[str] = []
    for lake, src_dir in zip(lakes, src_dirs):
        tiles, dropped = convert_lake(lake, src_dir, base_dirs, args)
        all_tiles.update(tiles)
        all_dropped |= dropped
        if tiles:
            converted.append(lake)

    # A tile can hold two lakes; a later lake writing it wins over an earlier
    # lake declaring it empty.
    all_dropped -= set(all_tiles)
    if all_tiles or all_dropped:
        write_manifest(args.output, all_tiles, all_dropped, args.tile_size_m,
                       args.tile_px, converted)
        print(f"Wrote {len(all_tiles)} overlay tile(s) + manifest to {args.output}")
    return 0


def convert_lake(lake: str, src_dir: Path, base_dirs: list[Path],
                 args) -> tuple[dict[str, dict], set[str]]:
    """Rasterise one lake into the overlay tile set."""
    print(f"[{lake}] indexing {src_dir} ...")
    index = index_sources([src_dir])
    if not index:
        print(f"  no swissBATHY3D_*.asc found — skipped", file=sys.stderr)
        return {}, set()
    e_min = min(b[0] for b, _ in index)
    e_max = max(b[2] for b, _ in index)
    n_min = min(b[1] for b, _ in index)
    n_max = max(b[3] for b, _ in index)
    print(f"  {len(index)} cells, LV95 E {e_min:.0f}..{e_max:.0f} "
          f"N {n_min:.0f}..{n_max:.0f}")

    ts = args.tile_size_m
    ei_min, ei_max = int(e_min // ts), int((e_max - 1e-6) // ts)
    ni_min, ni_max = int(n_min // ts), int((n_max - 1e-6) // ts)
    print(f"  grid ei [{ei_min}..{ei_max}], ni [{ni_min}..{ni_max}] "
          f"@ {ts / args.tile_px:g} m/px")

    water_level = args.water_level
    if water_level is None and base_dirs:
        water_level = detect_water_level(index, ts, args.tile_px, base_dirs,
                                         args.resampling)
        print(f"  water surface in base terrain: "
              + (f"{water_level:.1f} m (auto)" if water_level is not None
                 else "not detected — level gate disabled"))

    jobs = []
    skipped = 0
    for ei in range(ei_min, ei_max + 1):
        for ni in range(ni_min, ni_max + 1):
            srcs = intersecting_sources(ei, ni, ts, index)
            if not srcs:
                continue
            out_path = args.output / f"tile_{ei * ts}_{ni * ts}.raw"
            if out_path.exists() and not args.overwrite:
                skipped += 1
                continue
            jobs.append((ei, ni, ts, args.tile_px, srcs, out_path,
                         args.resampling, base_dirs, args.min_depth_m,
                         water_level, args.water_level_tol_m,
                         args.min_valid_elev))

    print(f"  {len(jobs)} tile(s) to build ({skipped} already present)")
    if not jobs:
        return {}, set()

    tiles: dict[str, dict] = {}
    dropped: set[str] = set()
    empty = 0
    done = 0
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(_worker, j) for j in jobs]
        for fut in as_completed(futures):
            ei, ni, mn, mx, valid_px = fut.result()
            done += 1
            lv95_e, lv95_n = ei * ts, ni * ts
            key = f"{lv95_e}_{lv95_n}"
            if valid_px == 0:
                empty += 1
                dropped.add(key)
            else:
                tiles[key] = {
                    "swiss_e": lv95_e,
                    "swiss_n": lv95_n,
                    "tile_ei": ei,
                    "tile_ni": ni,
                    "file": f"tile_{lv95_e}_{lv95_n}.raw",
                    "min_elev": mn,
                    "max_elev": mx,
                    "valid_px": valid_px,
                }
            if done % 50 == 0 or done == len(jobs):
                print(f"\r  {done}/{len(jobs)} tiles ({len(tiles)} written, "
                      f"{empty} empty)", end="", flush=True)
    print()
    return tiles, dropped


if __name__ == "__main__":
    raise SystemExit(main())
