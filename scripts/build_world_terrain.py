#!/usr/bin/env python3
"""
Norway terrain build pipeline (WORLD_SCENERY_DESIGN.md §5.3, P1).

Fetches AWS `skadi/` SRTM 1° tiles (big-endian int16, 3601×3601, metres,
nodata −32768, real bathymetry in the ocean) and bakes them into ProvPilot
LV95 heightmap tiles:

    tile_{E}_{N}.raw   float32 little-endian, 34×34 px per 1024 m cell

Tile naming / pixel orientation are identical to the CH/BW pipeline
(`process_swisstopo_terrain.py`): a tile named by its SW corner (E, N), both
multiples of 1024; pixel(0,0) = SE corner, columns East→West (Godot +X),
rows South→North (Godot +Z).

Pixel lattice: the C++ mesh (`s3d_tile.cpp`) places pixel column c at
local_e = tile_m * (1 - c/(px-1)) and row r at local_n = tile_m * r/(px-1),
i.e. pixel CENTRES span the full 1024 m with spacing 1024/33 ≈ 31.03 m for
the 34 px tiles (≈ the requested 30 m/px posting). The bake reprojects
directly onto that lattice, so baked pixels land exactly where the mesh and
the chunk-composite sampler (`s3d_tile_manager.cpp`) expect them.

No-data convention: 0.0 f32 = "no data" (the C++ loader gap-fills zeros);
negative elevations are real bathymetry and are preserved.

Endianness: the downloaded .hgt.gz files are always big-endian on disk —
they are parsed with an explicit `>i2` dtype at build time, so ONE set of
downloaded data serves both macOS (ARM) and Windows (x86_64). The baked
output is written with an explicit `<f4` dtype (little-endian), matching the
runtime format on both platforms. No per-OS copies anywhere.

Storage (gery decision 2026-09-14):
    /Volumes/Data1/scenery_in_norway/srtm/     raw SRTM cache (rebuildable)
    /Volumes/Data1/provpilot/build/norway/     plan.json + final Norway/ dir

Usage:
    python scripts/build_world_terrain.py plan
    python scripts/build_world_terrain.py fetch
    python scripts/build_world_terrain.py bake [--limit N] [--cell E_N]
    python scripts/build_world_terrain.py verify

Requirements (repo .venv): numpy shapely pyproj tqdm
"""

import argparse
import gzip
import json
import os
import sys
import struct
import time
import urllib.request
import zipfile
from collections import OrderedDict
from pathlib import Path

try:
    import numpy as np
    from shapely import contains_xy
    from pyproj import Transformer
    from tqdm import tqdm
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install numpy shapely pyproj tqdm")
    sys.exit(1)


# ── Configuration ────────────────────────────────────────────────────────────

SKADI_BASE = "https://elevation-tiles-prod.s3.amazonaws.com/skadi"
NE_LAND_URLS = [
    "https://naturalearth.s3.amazonaws.com/10m_physical/ne_10m_land.zip",
    "https://www.naturalearthdata.com/http//www.naturalearthdata.com/"
    "download/10m/physical/ne_10m_land.zip",
]

NORWAY_DIR = Path("/Volumes/Data1/provpilot/build/norway")
SRTM_CACHE = Path("/Volumes/Data1/scenery_in_norway/srtm")
NE_LAND_ZIP = Path("/Volumes/Data1/scenery_in_norway/ne_10m_land.zip")
PLAN_PATH = NORWAY_DIR / "plan.json"
TERRAIN_OUT = NORWAY_DIR / "Norway" / "terrain"

# Norway mainland WGS84 bbox (design §5.2; Svalbard excluded) and the LV95
# bbox from design §4.2 (west, south, east, north).
BBOX_WGS84 = (4.6, 57.9, 31.3, 71.2)
BBOX_LV95 = (2462000, 2453000, 3521000, 4113000)

TILE_M = 1024                 # LV95 grid cell size (must match C++ tile_size)
TARGET_PX = 34                # pixels per tile axis at the ~30 m/px posting
PX_SPACING = TILE_M / (TARGET_PX - 1)   # 1024/33 — exact mesh lattice spacing
NODATA_SRTM = -32768          # SRTM int16 nodata
NODATA_BAKE = -9999.0         # internal float nodata during reproject
LAND_MARGIN_M = 5000 + 800    # 5 km margin + half-cell diagonal (centre test)

# UTM zones covering the Norway bbox (lon 4.6…31.3 → zones 30…34).
UTM_ZONES = [30, 31, 32, 33, 34]


# ── Coordinate helpers (swisstopo polynomials, vectorised) ──────────────────

def lv95_to_wgs84(easting, northing):
    """LV95 → WGS84 (lat, lon). Vectorised over numpy arrays."""
    y = (np.asarray(easting, dtype=np.float64) - 2600000.0) / 1000000.0
    x = (np.asarray(northing, dtype=np.float64) - 1200000.0) / 1000000.0
    lat = (16.9023892 + 3.238272 * x - 0.270978 * y**2
           - 0.002528 * x**2 - 0.0447 * y**2 * x - 0.0140 * x**3) * 100 / 36
    lon = (2.6779094 + 4.728982 * y + 0.791484 * y * x
           + 0.1306 * y * x**2 - 0.0436 * y**3) * 100 / 36
    if np.ndim(lat) == 0:
        return float(lat), float(lon)
    return lat, lon


_WGS84_TO_LV95 = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
_LV95_TO_4326 = Transformer.from_crs("EPSG:2056", "EPSG:4326", always_xy=True)


def wgs84_to_lv95(lat, lon):
    """WGS84 → LV95 (easting, northing) via PROJ (verification only)."""
    e, n = _WGS84_TO_LV95.transform(lon, lat)
    return float(e), float(n)


def lv95_to_wgs84_proj(easting, northing):
    """LV95 → WGS84 via PROJ (full accuracy). Vectorised.

    Use this for source selection in the build: the swisstopo polynomial
    above matches C++ `Geo::lv95_to_wgs84` (HUD) but drifts up to ~86 km
    at 71°N — too much for picking SRTM windows and land-mask tests.
    """
    e = np.atleast_1d(np.asarray(easting, dtype=np.float64))
    n = np.atleast_1d(np.asarray(northing, dtype=np.float64))
    # always_xy: output order is (lon, lat) — swap to match the polynomial's
    # (lat, lon) contract.
    lon, lat = _LV95_TO_4326.transform(e, n)
    if e.size == 1:
        return float(lat[0]), float(lon[0])
    return lat, lon


# ── SRTM tile addressing ─────────────────────────────────────────────────────

def srtm_tiles_for_bbox(west, south, east, north):
    """All SRTM 1° tiles intersecting a WGS84 bbox (sorted list of keys)."""
    lats = range(int(np.floor(south)), int(np.ceil(north)) + 1)
    lons = range(int(np.floor(west)), int(np.ceil(east)) + 1)
    keys = set()
    for la in lats:
        if not (0 <= la < 90):
            continue
        for lo in lons:
            keys.add(f"N{int(la)}/N{int(la)}E{int(lo):03d}")
    return sorted(keys)


def all_srtm_tiles_for_norway():
    """The full tile set covering the Norway WGS84 bbox."""
    west, south, east, north = BBOX_WGS84
    return srtm_tiles_for_bbox(west, south, east, north)


def load_srtm_tile(key):
    """Parse a cached .hgt.gz into a float32 (3601, 3601) array.

    BIG-ENDIAN int16 on disk — parsed with an explicit '>i2' dtype so the
    same cache files work on any host architecture. Nodata → NODATA_BAKE.
    Row 0 = north edge, col 0 = west edge; sample (r, c) is at
    lon + c/3600, lat + 1 - r/3600 (SRTM corner convention).
    """
    path = SRTM_CACHE / f"{key}.hgt.gz"
    with gzip.open(path, "rb") as f:
        raw = f.read()
    a = np.frombuffer(raw, dtype=">i2").reshape(3601, 3601).astype(np.float32)
    a[a == NODATA_SRTM] = NODATA_BAKE
    return a


# ── Download helpers (stdlib urllib — no `requests` dependency) ─────────────

def _http_get(url, timeout=120):
    req = urllib.request.Request(
        url, headers={"User-Agent": "provpilot-build/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def download_srtm_tile(key, retries=3):
    """Download one SRTM tile into the cache; returns path (or None)."""
    dest = SRTM_CACHE / f"{key}.hgt.gz"
    if dest.exists() and dest.stat().st_size > 0:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    url = f"{SKADI_BASE}/{key}.hgt.gz"
    part = dest.with_name(f"{dest.name}.part.{os.getpid()}")
    for attempt in range(1, retries + 1):
        try:
            data = _http_get(url)
            # Validate before publishing: gzip magic + exact decompressed size.
            if data[:2] != b"\x1f\x8b":
                raise ValueError("not a gzip file")
            if len(gzip.decompress(data)) != 3601 * 3601 * 2:
                raise ValueError("unexpected decompressed size")
            part.write_bytes(data)
            os.replace(part, dest)   # atomic — concurrent fetchers can't race
            return dest
        except Exception as e:
            part.unlink(missing_ok=True)
            if attempt == retries:
                print(f"  FAILED {key}: {e}")
                return None
            time.sleep(2 * attempt)
    return None


def download_ne_land():
    """Fetch + extract the Natural Earth 1:10m land shapefile (cached)."""
    shp = NE_LAND_ZIP.with_name("ne_10m_land.shp")
    if not (shp.exists() and shp.stat().st_size > 0):
        NE_LAND_ZIP.parent.mkdir(parents=True, exist_ok=True)
        data = None
        for url in NE_LAND_URLS:
            try:
                print(f"  downloading {url}")
                data = _http_get(url, timeout=300)
                break
            except Exception as e:
                print(f"  {url} failed: {e}")
        if data is None:
            raise RuntimeError("could not download Natural Earth land data")
        NE_LAND_ZIP.write_bytes(data)
        with zipfile.ZipFile(NE_LAND_ZIP) as z:
            for ext in ("shp", "shx", "dbf"):
                name = f"ne_10m_land.{ext}"
                if name in z.namelist():
                    shp.with_suffix("." + ext).write_bytes(z.read(name))
    return shp


def load_ne_land_geometries(shp_path):
    """All land geometries as shapely objects.

    Pure-Python ESRI Shapefile reader — the repo .venv's GDAL build lacks
    the OGR shapefile driver. This Natural Earth file stores POLYGON records
    (type 5) with a ring-starts layout: [NumRings][TotalPoints]
    [RingStarts[NumRings]][points]; verified empirically 2026-09-15.
    """
    from shapely.geometry import Polygon

    def parse_poly(body):
        nr = int.from_bytes(body[36:40], "little")
        total = int.from_bytes(body[40:44], "little")
        starts = struct.unpack_from(f"<{nr}i", body, 44)
        p0 = 44 + nr * 4
        if (len(body) - p0) // 16 != total or total <= 0:
            return None
        pts = np.frombuffer(body[p0:p0 + total * 16], dtype="<f8")
        pts = pts.reshape(total, 2)
        rings = []
        for i in range(nr):
            s, e = starts[i], (starts[i + 1] if i + 1 < nr else total)
            ring = [tuple(p) for p in pts[s:e]]
            if len(ring) >= 4:
                rings.append(ring)
        return rings

    with open(shp_path, "rb") as f:
        blob = f.read()
    geoms = []
    off = 100
    while off + 8 <= len(blob):
        clen = int.from_bytes(blob[off + 4:off + 8], "big") * 2
        body = blob[off + 8:off + 8 + clen]
        stype = int.from_bytes(body[:4], "little")
        off += 8 + clen
        if stype != 5:                       # POLYGON (this file has no others)
            if stype != 0:
                print(f"  WARNING: unsupported shape type {stype} skipped")
            continue
        rings = parse_poly(body)
        if rings is None:
            print("  WARNING: inconsistent polygon record skipped")
            continue
        # ESRI: outer rings clockwise, holes counter-clockwise (WGS84).
        polys = []
        i = 0
        while i < len(rings):
            ext = Polygon(rings[i])
            if not ext.is_valid or ext.area <= 0:
                i += 1
                continue
            holes = []
            j = i + 1
            while j < len(rings):
                h = Polygon(rings[j])
                if h.exterior.is_ccw and h.within(ext):
                    holes.append(h.exterior)
                    j += 1
                else:
                    break
            polys.append(Polygon(ext, holes))
            i = j
        geoms.extend(polys)
    return geoms



# ── plan: land mask → LV95 cells → SRTM tiles ────────────────────────────────

def cmd_plan(args):
    print("=== Plan: Norway terrain build ===")
    west, south, east, north = BBOX_LV95
    w_lon, s_lat, e_lon, n_lat = BBOX_WGS84

    # 1. Land polygons clipped to the Norway bbox.
    shp_path = download_ne_land()
    from shapely.geometry import box
    clip_box = box(w_lon, s_lat, e_lon, n_lat)
    land = []
    for g in load_ne_land_geometries(shp_path):
        if g.intersects(clip_box):
            c = g.intersection(clip_box)
            if not c.is_empty and c.area > 0:
                land.append(c)
    print(f"  {len(land)} land polygons inside the Norway bbox")

    # 2. Coverage = land buffered by LAND_MARGIN_M, per UTM zone (metres).
    from shapely.ops import unary_union
    from shapely.ops import transform as shp_transform
    zone_covers = {}
    for z in UTM_ZONES:
        to_zone = Transformer.from_crs("EPSG:4326", f"EPSG:{32600 + z}",
                                       always_xy=True)
        parts = []
        for g in land:
            try:
                zg = shp_transform(to_zone.transform, g)
            except Exception:
                continue
            if not zg.is_empty:
                parts.append(zg.buffer(LAND_MARGIN_M))
        if parts:
            zone_covers[z] = unary_union(parts)

    # 3. LV95 grid cells whose centre lies inside the coverage.
    ei_min, ei_max = west // TILE_M, (east - 1) // TILE_M
    ni_min, ni_max = south // TILE_M, (north - 1) // TILE_M
    eis = np.arange(ei_min, ei_max + 1)
    nis = np.arange(ni_min, ni_max + 1)
    EE, NN = np.meshgrid(eis, nis)               # cell SW corners
    ce = EE * TILE_M + TILE_M / 2                 # cell centres (LV95)
    cn = NN * TILE_M + TILE_M / 2

    lat_c, lon_c = lv95_to_wgs84_proj(ce.ravel(), cn.ravel())
    n_pts = ce.size
    mask = np.zeros(n_pts, dtype=bool)
    for z in UTM_ZONES:
        if z not in zone_covers:
            continue
        to_zone = Transformer.from_crs("EPSG:4326", f"EPSG:{32600 + z}",
                                       always_xy=True)
        xs, ys = to_zone.transform(lon_c, lat_c)
        mask |= np.asarray(contains_xy(zone_covers[z], xs, ys),
                           dtype=bool)

    cells_e = ce.ravel()[mask]
    cells_n = cn.ravel()[mask]
    n_cells = int(mask.sum())
    print(f"  LV95 grid: {len(eis)} × {len(nis)} cells; "
          f"land+margin selection: {n_cells} tiles")

    # 4. SRTM tiles needed by the selected cells (corners + ~100 m pad).
    corners_e = np.concatenate([cells_e, cells_e + TILE_M])
    corners_n = np.concatenate([cells_n, cells_n + TILE_M])
    clat, clon = lv95_to_wgs84_proj(corners_e, corners_n)
    pad_lat = 100.0 / 111320.0
    pad_lon = 100.0 / (111320.0 * np.cos(np.deg2rad(
        np.clip(clat, 55.0, 72.0))))
    keys = set()
    for k in range(n_cells):
        i0 = 2 * k
        keys.update(srtm_tiles_for_bbox(
            clon[i0:i0 + 2].min() - pad_lon.max(),
            clat[i0:i0 + 2].min() - pad_lat,
            clon[i0:i0 + 2].max() + pad_lon.max(),
            clat[i0:i0 + 2].max() + pad_lat,
        ))
    print(f"  SRTM 1° tiles required: {len(keys)} "
          f"(full Norway span: {len(all_srtm_tiles_for_norway())})")

    plan = {
        "bbox_wgs84": list(BBOX_WGS84),
        "bbox_lv95": list(BBOX_LV95),
        "tile_size_m": TILE_M,
        "pixels_per_tile": TARGET_PX,
        "pixel_spacing_m": round(PX_SPACING, 4),
        "land_margin_m": LAND_MARGIN_M,
        "n_cells": n_cells,
        "cells_e": cells_e.astype(int).tolist(),
        "cells_n": cells_n.astype(int).tolist(),
        "srtm_tiles": sorted(keys),
    }
    NORWAY_DIR.mkdir(parents=True, exist_ok=True)
    with open(PLAN_PATH, "w") as f:
        json.dump(plan, f)
    print(f"  plan written: {PLAN_PATH}")
    return plan


def load_plan():
    with open(PLAN_PATH) as f:
        return json.load(f)


# ── fetch ────────────────────────────────────────────────────────────────────

def cmd_fetch(args):
    if args.all:
        keys = all_srtm_tiles_for_norway()
    elif PLAN_PATH.exists():
        keys = load_plan()["srtm_tiles"]
    else:
        print("No plan.json — use --all or run `plan` first.")
        return 1
    SRTM_CACHE.mkdir(parents=True, exist_ok=True)
    missing = [k for k in keys if not (SRTM_CACHE / f"{k}.hgt.gz").exists()]
    print(f"=== Fetch: {len(keys)} tiles, {len(missing)} missing ===")
    ok = fail = 0
    for key in tqdm(missing, desc="SRTM", unit="tile"):
        if download_srtm_tile(key):
            ok += 1
        else:
            fail += 1
    print(f"  fetched {ok}, failed {fail}")
    return 1 if fail else 0



# ── bake ─────────────────────────────────────────────────────────────────────

class _SrtmCache:
    """Small LRU cache of parsed SRTM degree tiles (52 MB each as f32)."""

    def __init__(self, cap=4):
        self.cap = cap
        self.d = OrderedDict()

    def get(self, key):
        if key in self.d:
            self.d.move_to_end(key)
            return self.d[key]
        arr = load_srtm_tile(key)
        self.d[key] = arr
        if len(self.d) > self.cap:
            self.d.popitem(last=False)
        return arr


def _bake_cell(cache, e_sw, n_sw, fetch_missing=True):
    """Bake one LV95 cell into a 34×34 tile by bilinearly sampling SRTM.

    The WGS84↔LV95 mapping uses the same pyproj Transformer as `plan` and
    `verify` (and the rest of the game's pipelines). rasterio/GDAL's bundled
    PROJ was deliberately NOT used for this: it picks a different
    CH1903+→WGS84 transformation path that drifts ~5 m at Bern but
    ~320–360 m in latitude at 60–63°N, which left nodata bands along tile
    north edges and misplaced the SRTM data (diagnosed 2026-09-15).

    Stored orientation (matches s3d_tile.cpp): pixel(0,0) = SE corner,
    columns East→West, rows South→North; pixel centres on the exact mesh
    lattice E = e_sw + (PX-1-c)*PX_SPACING, N = n_sw + r*PX_SPACING.
    """
    # Destination pixel centres in WGS84 (one vectorised pyproj call).
    cc = np.arange(TARGET_PX)
    rr = np.arange(TARGET_PX)
    E = np.broadcast_to(e_sw + (TARGET_PX - 1 - cc[None, :]) * PX_SPACING,
                        (TARGET_PX, TARGET_PX))
    N = np.broadcast_to(n_sw + rr[:, None] * PX_SPACING,
                        (TARGET_PX, TARGET_PX))
    lat, lon = lv95_to_wgs84_proj(E, N)

    buf = np.full((TARGET_PX, TARGET_PX), NODATA_BAKE, dtype=np.float32)

    # Source selection: cell corners + ~100 m pad. Only decides which degree
    # tiles to load; the per-pixel sampling below is exact regardless.
    ce = np.array([e_sw, e_sw + TILE_M, e_sw, e_sw + TILE_M])
    cn = np.array([n_sw, n_sw, n_sw + TILE_M, n_sw + TILE_M])
    clat, clon = lv95_to_wgs84_proj(ce, cn)
    pad_lat = 100.0 / 111320.0
    pad_lon = 100.0 / (111320.0 * np.cos(np.deg2rad(float(clat.mean()))))
    keys = srtm_tiles_for_bbox(clon.min() - pad_lon, clat.min() - pad_lat,
                               clon.max() + pad_lon, clat.max() + pad_lat)

    for key in keys:
        if not (SRTM_CACHE / f"{key}.hgt.gz").exists():
            if not fetch_missing:
                continue
            r = download_srtm_tile(key)
            if r is None:
                raise RuntimeError(f"could not fetch {key}")
        arr = cache.get(key)
        lat0 = int(key[1:3])                     # 'N60/…' → 60
        lon0 = int(key.split("E")[1])

        # Pixels whose centre is inside this degree tile — the bilinear
        # support (4 neighbours) then stays within the 3601×3601 grid.
        m = ((lon >= lon0) & (lon <= lon0 + 1) &
             (lat >= lat0) & (lat <= lat0 + 1))
        if not m.any():
            continue

        # SRTM corner convention: sample (r, c) centre at lon0 + c/3600,
        # lat0 + 1 - r/3600 → fractional grid indices:
        u = (lon[m] - lon0) * 3600.0
        v = (lat0 + 1.0 - lat[m]) * 3600.0
        c0 = np.floor(u).astype(np.intp)
        r0 = np.floor(v).astype(np.intp)
        c1 = np.minimum(c0 + 1, 3600)
        r1 = np.minimum(r0 + 1, 3600)
        fc = u - c0
        fr = v - r0

        a00 = arr[r0, c0]
        a01 = arr[r0, c1]
        a10 = arr[r1, c0]
        a11 = arr[r1, c1]
        val = (a00 * (1.0 - fc) + a01 * fc) * (1.0 - fr) \
            + (a10 * (1.0 - fc) + a11 * fc) * fr
        # Any nodata corner poisons the blend (same as GDAL src_nodata).
        bad = ((a00 == NODATA_BAKE) | (a01 == NODATA_BAKE) |
               (a10 == NODATA_BAKE) | (a11 == NODATA_BAKE))
        val = np.where(bad, NODATA_BAKE, val).astype(np.float32)

        # First non-nodata wins; adjacent degree tiles share their boundary
        # sample and agree there, so the merge is seam-free.
        buf[m] = np.where(buf[m] == NODATA_BAKE, val, buf[m])

    stored = buf.copy()
    stored[stored == NODATA_BAKE] = 0.0          # C++ no-data convention
    return stored.astype("<f4")



def cmd_bake(args):
    plan = load_plan()
    TERRAIN_OUT.mkdir(parents=True, exist_ok=True)

    cells = list(zip(plan["cells_e"], plan["cells_n"]))
    if args.cell:
        e, n = (int(v) for v in args.cell.split("_"))
        cells = [(e, n)]
    if args.limit:
        cells = cells[:args.limit]
    # Cache locality: process cells in geographic order so the SRTM LRU cache
    # stays hot on a small set of degree tiles.
    cells.sort(key=lambda c: (c[1] // 102400, c[0]))

    print(f"=== Bake: {len(cells)} tiles → {TERRAIN_OUT} ===")
    cache = _SrtmCache()
    manifest = {
        "tile_size_m": TILE_M,
        "pixels_per_tile": TARGET_PX,
        "pixel_resolution_m": round(PX_SPACING, 4),
        "format": "float32_raw_le",
        "bytes_per_tile": TARGET_PX * TARGET_PX * 4,
        "orientation": {
            "pixel_0_0": "SE corner of Swiss tile",
            "col_direction": "East to West (Godot +X)",
            "row_direction": "South to North (Godot +Z)",
        },
        "grid": {},
        "source": ("AWS skadi SRTM 1° tiles (elevation-tiles-prod), "
                   "WGS84→LV95 reproject, build_world_terrain.py"),
        "tiles": {},
    }

    t0 = time.time()
    all_zero = 0
    for i, (e_sw, n_sw) in enumerate(tqdm(cells, desc="Bake", unit="tile")):
        data = _bake_cell(cache, e_sw, n_sw, fetch_missing=not args.no_fetch)
        name = f"tile_{e_sw}_{n_sw}.raw"
        data.tofile(TERRAIN_OUT / name)

        nz = data[data != 0.0]
        if nz.size == 0:
            all_zero += 1
            mn = mx = 0.0
        else:
            mn, mx = float(nz.min()), float(data.max())
        manifest["tiles"][f"{e_sw}_{n_sw}"] = {
            "swiss_e": e_sw,
            "swiss_n": n_sw,
            "tile_ei": e_sw // TILE_M,
            "tile_ni": n_sw // TILE_M,
            "file": name,
            "min_elev": round(mn, 1),
            "max_elev": round(mx, 1),
        }
        if (i + 1) % 2000 == 0:
            el = time.time() - t0
            rate = (i + 1) / el
            print(f"  {i+1}/{len(cells)} ({rate:.0f} tiles/s, "
                  f"ETA {(len(cells)-i-1)/rate/60:.0f} min)")

    if cells:
        manifest["grid"] = {
            "ei_min": min(e for e, _ in cells) // TILE_M,
            "ei_max": max(e for e, _ in cells) // TILE_M,
            "ni_min": min(n for _, n in cells) // TILE_M,
            "ni_max": max(n for _, n in cells) // TILE_M,
        }
    manifest["tile_count"] = len(manifest["tiles"])
    with open(TERRAIN_OUT / "manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)

    mb = len(cells) * TARGET_PX * TARGET_PX * 4 / (1024 * 1024)
    print(f"  baked {len(cells)} tiles ({mb:.0f} MB), "
          f"{all_zero} all-zero, in {time.time()-t0:.0f}s")
    print(f"  manifest: {TERRAIN_OUT / 'manifest.json'}")
    return 0



# ── verify: sample baked tiles at known points ───────────────────────────────

def cmd_verify(args):
    """Check baked elevations against known landmarks."""
    checks = [
        # (name, lat, lon, expected m, tolerance m)
        ("ENGM Oslo Gardermoen (field ~37 m)", 59.9097, 10.7296, 37, 40),
        ("Oslo city centre (plateau 20–80 m)", 59.9130, 10.7520, 50, 40),
        ("Galdhøpiggen summit (2469 m, SRTM fill ~2380)", 61.63644, 8.31248, 2380, 150),
        ("Tromsø (coastal plain <30 m)", 69.6492, 18.9553, 10, 40),
    ]
    ok = True
    for name, lat, lon, expect, tol in checks:
        e, n = wgs84_to_lv95(lat, lon)
        ei, ni = int(np.floor(e / TILE_M)), int(np.floor(n / TILE_M))
        path = TERRAIN_OUT / f"tile_{ei*TILE_M}_{ni*TILE_M}.raw"
        if not path.exists():
            print(f"  MISSING {name}: {path.name} not baked yet")
            ok = False
            continue
        data = np.fromfile(path, dtype="<f4").reshape(TARGET_PX, TARGET_PX)
        # Stored lattice: col c ↔ local_e = TILE_M*(1 - c/(PX-1)),
        # row r ↔ local_n = TILE_M*r/(PX-1).
        local_e = e - ei * TILE_M
        local_n = n - ni * TILE_M
        c = int(round((TILE_M - local_e) / TILE_M * (TARGET_PX - 1)))
        r = int(round(local_n / TILE_M * (TARGET_PX - 1)))
        c = min(max(c, 0), TARGET_PX - 1)
        r = min(max(r, 0), TARGET_PX - 1)
        got = float(data[r, c])
        status = "OK" if abs(got - expect) <= tol else "CHECK"
        if status != "OK":
            ok = False
        print(f"  [{status}] {name}: baked {got:.1f} m (expected ~{expect} ±{tol})")
    return 0 if ok else 1


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="Norway terrain build (SRTM → LV95 .raw tiles)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("plan", help="compute LV95 cells + SRTM tile list")
    pf = sub.add_parser("fetch", help="download missing SRTM tiles")
    pf.add_argument("--all", action="store_true",
                    help="fetch the full Norway span (ignores plan)")
    pb = sub.add_parser(
        "bake", help="reproject cells into .raw tiles + manifest")
    pb.add_argument("--limit", type=int, default=0,
                    help="only bake the first N cells (testing)")
    pb.add_argument("--cell", type=str, default=None,
                    help="bake a single cell 'E_N' (SW corner, ×1024)")
    pb.add_argument("--no-fetch", action="store_true",
                    help="do not fetch missing SRTM tiles on demand")
    sub.add_parser("verify", help="check baked tiles at known landmarks")

    args = p.parse_args()
    if args.cmd == "plan":
        cmd_plan(args)
    elif args.cmd == "fetch":
        sys.exit(cmd_fetch(args))
    elif args.cmd == "bake":
        sys.exit(cmd_bake(args))
    elif args.cmd == "verify":
        sys.exit(cmd_verify(args))


if __name__ == "__main__":
    main()

