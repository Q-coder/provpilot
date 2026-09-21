#!/usr/bin/env python3
"""
Worldwide base-tier terrain build (WORLD_SCENERY_DESIGN.md §5.3, §11).

Fetches AWS `skadi/` SRTM 1° tiles (big-endian int16, 3601×3601, metres,
nodata −32768, real bathymetry in the ocean) and bakes them into ProvPilot
LV95 heightmap tiles for one *region* from `scripts/world_regions.json`:

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

Region selection (world_regions.json):
    select = "country"  Natural Earth 1:10m admin-0 polygons for `iso_a3`
                        (optionally clipped to `bbox_wgs84`) + land margin.
                        Border cells are baked by both neighbours (same
                        source, identical values) so packs overlap by the
                        margin instead of leaving a seam.
    select = "bbox"     ne_10m_land inside `bbox_wgs84`, iterated over the
                        fixed `bbox_lv95` window (the Fennoscandia packs).

Storage:
    <cache_root>/srtm/            shared raw SRTM cache (rebuildable); the
                                  legacy Norway cache is read as a fallback
    <build_root>/<id>/plan.json   selected cells + SRTM tile list
    <build_root>/<id>/<subdir>/terrain/   final region terrain directory

Usage:
    python scripts/build_world_terrain.py list
    python scripts/build_world_terrain.py plan   --region germany
    python scripts/build_world_terrain.py fetch  --region germany
    python scripts/build_world_terrain.py bake   --region germany [--limit N] [--cell E_N]
    python scripts/build_world_terrain.py flatten --region germany  # runway plateaus (§12)
    python scripts/build_world_terrain.py verify --region germany
    python scripts/build_world_terrain.py build  --region germany   # all of the above

Requirements (repo .venv): numpy shapely pyproj tqdm fiona
"""

import argparse
import gzip
import json
import os
import re
import sys
import struct
import time
import urllib.error
import urllib.request
import zipfile
from collections import OrderedDict
from datetime import date
from pathlib import Path

try:
    import numpy as np
    from shapely import contains_xy
    from pyproj import Transformer
    from tqdm import tqdm
except ImportError as e:
    print(f"Missing required package: {e}")
    print("Install with: pip install numpy shapely pyproj tqdm fiona")
    sys.exit(1)


# ── Configuration ────────────────────────────────────────────────────────────

SKADI_BASE = "https://elevation-tiles-prod.s3.amazonaws.com/skadi"
NE_LAND_URLS = [
    "https://naturalearth.s3.amazonaws.com/10m_physical/ne_10m_land.zip",
    "https://www.naturalearthdata.com/http//www.naturalearthdata.com/"
    "download/10m/physical/ne_10m_land.zip",
]
NE_ADMIN0_URLS = [
    "https://naturalearth.s3.amazonaws.com/10m_cultural/"
    "ne_10m_admin_0_countries.zip",
    "https://www.naturalearthdata.com/http//www.naturalearthdata.com/"
    "download/10m/cultural/ne_10m_admin_0_countries.zip",
]

REGIONS_JSON = Path(__file__).resolve().parent / "world_regions.json"

NODATA_SRTM = -32768          # SRTM int16 nodata
NODATA_BAKE = -9999.0         # internal float nodata during reproject


class Region:
    """One entry of world_regions.json plus the shared build settings."""

    def __init__(self, cfg, entry):
        self.cfg = cfg
        self.entry = entry
        self.id = entry["id"]
        self.name = entry["name"]
        self.subdir = entry.get("subdir", entry["name"])
        self.select = entry.get("select", "country")
        self.iso_a3 = entry.get("iso_a3", [])
        self.bbox_wgs84 = entry.get("bbox_wgs84")
        self.bbox_lv95 = entry.get("bbox_lv95")
        self.landmarks = entry.get("landmarks", [])

        self.tile_m = int(entry.get("tile_size_m", cfg.get("tile_size_m", 1024)))
        self.px = int(entry.get("pixels_per_tile", cfg.get("pixels_per_tile", 34)))
        self.px_spacing = self.tile_m / (self.px - 1)   # exact mesh lattice
        # 5 km margin + half-cell diagonal (the cell test is centre-based).
        self.land_margin_m = float(entry.get("land_margin_m",
                                             cfg.get("land_margin_m", 5000)))
        self.land_margin_m += 0.5 * self.tile_m * 2 ** 0.5

        build_root = Path(cfg["build_root"])
        self.build_dir = build_root / self.id
        self.plan_path = self.build_dir / "plan.json"
        self.terrain_out = self.build_dir / self.subdir / "terrain"

        cache_root = Path(cfg["cache_root"])
        self.srtm_cache = cache_root / "srtm"
        self.legacy_srtm = [Path(p) for p in cfg.get("legacy_srtm_caches", [])]
        self.ne_dir = cache_root

    def utm_zones(self, west_lon, east_lon):
        z0 = int(np.floor((west_lon + 180.0) / 6.0)) + 1
        z1 = int(np.floor((east_lon + 180.0) / 6.0)) + 1
        return list(range(max(1, z0), min(60, z1) + 1))


def load_config():
    with open(REGIONS_JSON) as f:
        return json.load(f)


def get_region(region_id):
    cfg = load_config()
    for e in cfg["regions"]:
        if e["id"] == region_id:
            return Region(cfg, e)
    ids = ", ".join(e["id"] for e in cfg["regions"])
    sys.exit(f"unknown region '{region_id}' — known: {ids}")


# ── Coordinate helpers ───────────────────────────────────────────────────────

def lv95_to_wgs84(easting, northing):
    """LV95 → WGS84 (lat, lon) via the swisstopo approximation polynomial.

    Kept for reference only (matches the historical C++ `Geo` before the
    rigorous projection library); the build itself uses PROJ throughout.
    """
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
    """WGS84 → LV95 (easting, northing) via PROJ. Scalars or arrays."""
    e, n = _WGS84_TO_LV95.transform(lon, lat)
    if np.ndim(e) == 0:
        return float(e), float(n)
    return e, n


def lv95_to_wgs84_proj(easting, northing):
    """LV95 → WGS84 (lat, lon) via PROJ (full accuracy). Vectorised.

    The swisstopo polynomial drifts up to ~86 km at 71°N — far too much for
    picking SRTM windows and land-mask tests, hence PROJ everywhere.
    """
    e = np.atleast_1d(np.asarray(easting, dtype=np.float64))
    n = np.atleast_1d(np.asarray(northing, dtype=np.float64))
    # always_xy: output order is (lon, lat) — swap to (lat, lon).
    lon, lat = _LV95_TO_4326.transform(e, n)
    if e.size == 1:
        return float(lat[0]), float(lon[0])
    return lat, lon


# ── SRTM tile addressing ─────────────────────────────────────────────────────

_KEY_RE = re.compile(r"^([NS])(\d{2})/\1\2([EW])(\d{3})$")


def srtm_key(lat0, lon0):
    """Key of the 1° tile whose SW corner is (lat0, lon0), e.g. N60/N60E005."""
    ns = "N" if lat0 >= 0 else "S"
    ew = "E" if lon0 >= 0 else "W"
    return f"{ns}{abs(int(lat0)):02d}/{ns}{abs(int(lat0)):02d}{ew}{abs(int(lon0)):03d}"


def srtm_key_origin(key):
    """(lat0, lon0) SW corner of a tile key."""
    m = _KEY_RE.match(key)
    if not m:
        raise ValueError(f"bad SRTM key {key}")
    lat0 = int(m.group(2)) * (1 if m.group(1) == "N" else -1)
    lon0 = int(m.group(4)) * (1 if m.group(3) == "E" else -1)
    return lat0, lon0


def srtm_tiles_for_bbox(west, south, east, north):
    """All SRTM 1° tiles intersecting a WGS84 bbox (sorted list of keys)."""
    lats = range(int(np.floor(south)), int(np.ceil(north)) + 1)
    lons = range(int(np.floor(west)), int(np.ceil(east)) + 1)
    keys = set()
    for la in lats:
        if not (-90 <= la < 90):
            continue
        for lo in lons:
            if not (-180 <= lo < 180):
                continue
            keys.add(srtm_key(la, lo))
    return sorted(keys)


def srtm_tiles_for_points(lat, lon, pad_m=100.0):
    """Tiles touched by a set of WGS84 points, each padded by ±pad_m.

    Vectorised: the four (lat±pad, lon±pad) combinations of every point are
    floored to degree corners and made unique. A 1 km cell never spans a
    whole degree, so this equals the per-cell bbox rule of the Norway build.
    """
    lat = np.asarray(lat, dtype=np.float64).ravel()
    lon = np.asarray(lon, dtype=np.float64).ravel()
    pad_lat = pad_m / 111320.0
    pad_lon = pad_m / (111320.0 * np.cos(np.deg2rad(np.clip(lat, -85.0, 85.0))))
    las = []
    los = []
    for sla in (-1.0, 1.0):
        for slo in (-1.0, 1.0):
            las.append(np.floor(lat + sla * pad_lat))
            los.append(np.floor(lon + slo * pad_lon))
    pairs = np.unique(np.stack([np.concatenate(las), np.concatenate(los)],
                               axis=1), axis=0)
    return sorted(srtm_key(int(la), int(lo)) for la, lo in pairs
                  if -90 <= la < 90 and -180 <= lo < 180)


def srtm_cached_path(region, key):
    """Cached .hgt.gz for a key: the shared cache first, then legacy caches."""
    p = region.srtm_cache / f"{key}.hgt.gz"
    if p.exists() and p.stat().st_size > 0:
        return p
    for legacy in region.legacy_srtm:
        q = legacy / f"{key}.hgt.gz"
        if q.exists() and q.stat().st_size > 0:
            return q
    return None


def load_srtm_tile(region, key):
    """Parse a cached .hgt.gz into a float32 (3601, 3601) array.

    BIG-ENDIAN int16 on disk — parsed with an explicit '>i2' dtype so the
    same cache files work on any host architecture. Nodata → NODATA_BAKE.
    Row 0 = north edge, col 0 = west edge; sample (r, c) is at
    lon0 + c/3600, lat0 + 1 - r/3600 (SRTM corner convention).
    """
    path = srtm_cached_path(region, key)
    if path is None:
        raise FileNotFoundError(key)
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


def download_srtm_tile(region, key, retries=3):
    """Download one SRTM tile into the shared cache; returns path (or None).

    Tiles that are entirely ocean do not exist on the server (HTTP 404):
    an empty marker file is written so the bake treats them as nodata
    (0.0 f32) without retrying the download on every run.
    """
    have = srtm_cached_path(region, key)
    if have is not None:
        return have
    dest = region.srtm_cache / f"{key}.hgt.gz"
    if dest.exists():                     # empty marker: known-missing tile
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
        except urllib.error.HTTPError as e:
            part.unlink(missing_ok=True)
            if e.code == 404:
                dest.write_bytes(b"")
                return dest
            if attempt == retries:
                print(f"  FAILED {key}: {e}")
                return None
            time.sleep(2 * attempt)
        except Exception as e:
            part.unlink(missing_ok=True)
            if attempt == retries:
                print(f"  FAILED {key}: {e}")
                return None
            time.sleep(2 * attempt)
    return None


def _download_ne(region, urls, stem):
    """Fetch + extract a Natural Earth shapefile set (cached)."""
    shp = region.ne_dir / f"{stem}.shp"
    if shp.exists() and shp.stat().st_size > 0:
        return shp
    region.ne_dir.mkdir(parents=True, exist_ok=True)
    zpath = region.ne_dir / f"{stem}.zip"
    if not (zpath.exists() and zpath.stat().st_size > 0):
        data = None
        for url in urls:
            try:
                print(f"  downloading {url}")
                data = _http_get(url, timeout=300)
                break
            except Exception as e:
                print(f"  {url} failed: {e}")
        if data is None:
            raise RuntimeError(f"could not download Natural Earth {stem}")
        zpath.write_bytes(data)
    with zipfile.ZipFile(zpath) as z:
        for ext in ("shp", "shx", "dbf", "prj", "cpg"):
            name = f"{stem}.{ext}"
            if name in z.namelist():
                (region.ne_dir / name).write_bytes(z.read(name))
    return shp


def load_ne_land_geometries(shp_path):
    """All ne_10m_land geometries as shapely objects.

    Pure-Python ESRI Shapefile reader — kept from the Norway build (the
    repo .venv's GDAL build lacked the OGR shapefile driver at the time).
    This Natural Earth file stores POLYGON records (type 5) with a
    ring-starts layout: [NumRings][TotalPoints][RingStarts[NumRings]]
    [points]; verified empirically 2026-09-15.
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


def load_ne_country_geometries(shp_path, iso_a3):
    """Admin-0 polygons whose ADM0_A3 is in iso_a3 (needs fiona)."""
    try:
        import fiona
        from shapely.geometry import shape
    except ImportError:
        sys.exit("select='country' needs fiona + shapely in the .venv")
    wanted = set(iso_a3)
    geoms = []
    found = set()
    with fiona.open(str(shp_path)) as src:
        for feat in src:
            p = feat["properties"]
            code = p.get("ADM0_A3")
            if code in wanted:
                g = shape(feat["geometry"])
                if not g.is_valid:
                    g = g.buffer(0)
                geoms.append(g)
                found.add(code)
    missing = wanted - found
    if missing:
        sys.exit(f"admin-0 codes not found in Natural Earth: {sorted(missing)}")
    return geoms


# ── plan: coverage → LV95 cells → SRTM tiles ─────────────────────────────────

def _coverage_geometries(region):
    """WGS84 polygons whose margin-buffered union is the bake coverage."""
    from shapely.geometry import box

    clip = box(*region.bbox_wgs84) if region.bbox_wgs84 else None
    if region.select == "bbox":
        if clip is None:
            sys.exit(f"region {region.id}: select='bbox' needs bbox_wgs84")
        shp = _download_ne(region, NE_LAND_URLS, "ne_10m_land")
        source = load_ne_land_geometries(shp)
        label = "land"
    elif region.select == "country":
        if not region.iso_a3:
            sys.exit(f"region {region.id}: select='country' needs iso_a3")
        shp = _download_ne(region, NE_ADMIN0_URLS, "ne_10m_admin_0_countries")
        source = load_ne_country_geometries(shp, region.iso_a3)
        label = "country"
    else:
        sys.exit(f"region {region.id}: unknown select '{region.select}'")

    geoms = []
    for g in source:
        if clip is not None:
            if not g.intersects(clip):
                continue
            g = g.intersection(clip)
        if not g.is_empty and g.area > 0:
            geoms.append(g)
    print(f"  {len(geoms)} {label} polygons selected")
    return geoms


def _lv95_window(region, geoms):
    """LV95 cell-iteration window (west, south, east, north), 1024-aligned.

    The WGS84 bounds of the coverage (plus margin) are sampled densely along
    their perimeter and projected; the min/max of those samples, padded by
    one cell, bounds every cell whose centre can lie inside the coverage.
    """
    if region.bbox_lv95:
        return tuple(region.bbox_lv95)
    from shapely.ops import unary_union
    minx, miny, maxx, maxy = unary_union(geoms).bounds
    dlat = region.land_margin_m / 111320.0
    dlon = region.land_margin_m / (111320.0 * np.cos(np.deg2rad(
        np.clip(max(abs(miny), abs(maxy)), 0.0, 85.0))))
    w, s, e, n = minx - dlon, miny - dlat, maxx + dlon, maxy + dlat
    t = np.linspace(0.0, 1.0, 200)
    lons = np.concatenate([w + (e - w) * t, np.full_like(t, e),
                           e - (e - w) * t, np.full_like(t, w)])
    lats = np.concatenate([np.full_like(t, s), s + (n - s) * t,
                           np.full_like(t, n), n - (n - s) * t])
    ee, nn = wgs84_to_lv95(lats, lons)
    tm = region.tile_m
    west = int(np.floor(ee.min() / tm) - 1) * tm
    east = int(np.ceil(ee.max() / tm) + 1) * tm
    south = int(np.floor(nn.min() / tm) - 1) * tm
    north = int(np.ceil(nn.max() / tm) + 1) * tm
    return west, south, east, north


def cmd_plan(region, args):
    print(f"=== Plan: {region.name} ({region.id}) terrain build ===")
    from shapely.ops import unary_union
    from shapely.ops import transform as shp_transform

    geoms = _coverage_geometries(region)
    if not geoms:
        sys.exit("  nothing selected — check the region entry")
    west, south, east, north = _lv95_window(region, geoms)
    gb = unary_union(geoms).bounds
    zones = region.utm_zones(gb[0] - 0.5, gb[2] + 0.5)
    print(f"  WGS84 bounds {tuple(round(v, 2) for v in gb)}, UTM zones {zones}")
    print(f"  LV95 window E {west}…{east}, N {south}…{north}")

    # Coverage = source polygons buffered by the margin, per UTM zone (metres).
    zone_covers = {}
    for z in zones:
        to_zone = Transformer.from_crs("EPSG:4326", f"EPSG:{32600 + z}",
                                       always_xy=True)
        parts = []
        for g in geoms:
            try:
                zg = shp_transform(to_zone.transform, g)
            except Exception:
                continue
            if not zg.is_empty:
                parts.append(zg.buffer(region.land_margin_m))
        if parts:
            zone_covers[z] = unary_union(parts)

    # LV95 grid cells whose centre lies inside the coverage.
    tm = region.tile_m
    ei_min, ei_max = west // tm, (east - 1) // tm
    ni_min, ni_max = south // tm, (north - 1) // tm
    eis = np.arange(ei_min, ei_max + 1)
    nis = np.arange(ni_min, ni_max + 1)
    EE, NN = np.meshgrid(eis, nis)               # cell indices
    sw_e = EE * tm                                # cell SW corners (LV95)
    sw_n = NN * tm
    ce = sw_e + tm / 2                            # cell centres — mask test only
    cn = sw_n + tm / 2

    lat_c, lon_c = lv95_to_wgs84_proj(ce.ravel(), cn.ravel())
    mask = np.zeros(ce.size, dtype=bool)
    for z, cover in zone_covers.items():
        to_zone = Transformer.from_crs("EPSG:4326", f"EPSG:{32600 + z}",
                                       always_xy=True)
        xs, ys = to_zone.transform(lon_c, lat_c)
        mask |= np.asarray(contains_xy(cover, xs, ys), dtype=bool)

    # Store SW corners: the runtime names tile files tile_{ei*1024}_{ni*1024}.raw
    # (C++ S3DTileManager builds the path from the tile index) and _bake_cell
    # lays its pixel lattice out from e_sw over [e_sw, e_sw+tile_m].
    cells_e = sw_e.ravel()[mask]
    cells_n = sw_n.ravel()[mask]
    n_cells = int(mask.sum())
    print(f"  LV95 grid: {len(eis)} × {len(nis)} cells; "
          f"coverage selection: {n_cells} tiles")

    # SRTM tiles needed by the selected cells (all four corners + ~100 m pad).
    corners_e = np.concatenate([cells_e, cells_e + tm, cells_e, cells_e + tm])
    corners_n = np.concatenate([cells_n, cells_n, cells_n + tm, cells_n + tm])
    clat, clon = lv95_to_wgs84_proj(corners_e, corners_n)
    keys = srtm_tiles_for_points(clat, clon)
    print(f"  SRTM 1° tiles required: {len(keys)}")

    mb = n_cells * region.px * region.px * 4 / (1024 * 1024)
    print(f"  estimated bake: {mb:.0f} MB of tiles "
          f"(+ file-system block overhead), "
          f"~{n_cells / 180 / 60:.0f} min at the Norway rate")

    plan = {
        "region": region.id,
        "select": region.select,
        "iso_a3": region.iso_a3,
        "bbox_wgs84": [round(float(v), 4) for v in gb],
        "bbox_lv95": [int(west), int(south), int(east), int(north)],
        "tile_size_m": tm,
        "pixels_per_tile": region.px,
        "pixel_spacing_m": round(region.px_spacing, 4),
        "land_margin_m": region.land_margin_m,
        "utm_zones": zones,
        "n_cells": n_cells,
        "cells_e": cells_e.astype(int).tolist(),
        "cells_n": cells_n.astype(int).tolist(),
        "srtm_tiles": keys,
    }
    region.build_dir.mkdir(parents=True, exist_ok=True)
    with open(region.plan_path, "w") as f:
        json.dump(plan, f)
    print(f"  plan written: {region.plan_path}")
    return plan


def load_plan(region):
    if not region.plan_path.exists():
        sys.exit(f"no {region.plan_path} — run `plan --region {region.id}` first")
    with open(region.plan_path) as f:
        return json.load(f)


# ── fetch ────────────────────────────────────────────────────────────────────

def cmd_fetch(region, args):
    keys = load_plan(region)["srtm_tiles"]
    region.srtm_cache.mkdir(parents=True, exist_ok=True)
    missing = [k for k in keys
               if srtm_cached_path(region, k) is None
               and not (region.srtm_cache / f"{k}.hgt.gz").exists()]
    print(f"=== Fetch {region.id}: {len(keys)} tiles, {len(missing)} missing ===")
    ok = fail = absent = 0
    for key in tqdm(missing, desc="SRTM", unit="tile"):
        p = download_srtm_tile(region, key)
        if p is None:
            fail += 1
        elif p.stat().st_size == 0:
            absent += 1
        else:
            ok += 1
    print(f"  fetched {ok}, not on server (ocean) {absent}, failed {fail}")
    return 1 if fail else 0


# ── bake ─────────────────────────────────────────────────────────────────────

class _SrtmCache:
    """Small LRU cache of parsed SRTM degree tiles (52 MB each as f32)."""

    def __init__(self, region, cap=4):
        self.region = region
        self.cap = cap
        self.d = OrderedDict()

    def get(self, key):
        if key in self.d:
            self.d.move_to_end(key)
            return self.d[key]
        arr = load_srtm_tile(self.region, key)
        self.d[key] = arr
        if len(self.d) > self.cap:
            self.d.popitem(last=False)
        return arr


def _bake_cell(region, cache, e_sw, n_sw, fetch_missing=True):
    """Bake one LV95 cell into a px×px tile by bilinearly sampling SRTM.

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
    px = region.px
    sp = region.px_spacing
    tm = region.tile_m
    cc = np.arange(px)
    rr = np.arange(px)
    E = np.broadcast_to(e_sw + (px - 1 - cc[None, :]) * sp, (px, px))
    N = np.broadcast_to(n_sw + rr[:, None] * sp, (px, px))
    lat, lon = lv95_to_wgs84_proj(E, N)

    buf = np.full((px, px), NODATA_BAKE, dtype=np.float32)

    # Source selection: cell corners + ~100 m pad. Only decides which degree
    # tiles to load; the per-pixel sampling below is exact regardless.
    ce = np.array([e_sw, e_sw + tm, e_sw, e_sw + tm])
    cn = np.array([n_sw, n_sw, n_sw + tm, n_sw + tm])
    clat, clon = lv95_to_wgs84_proj(ce, cn)
    keys = srtm_tiles_for_points(clat, clon)

    for key in keys:
        path = srtm_cached_path(region, key)
        if path is None:
            marker = region.srtm_cache / f"{key}.hgt.gz"
            if marker.exists():              # known-missing (ocean) tile
                continue
            if not fetch_missing:
                continue
            r = download_srtm_tile(region, key)
            if r is None:
                raise RuntimeError(f"could not fetch {key}")
            if r.stat().st_size == 0:
                continue
        arr = cache.get(key)
        lat0, lon0 = srtm_key_origin(key)

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
    # 0.0 is the runtime's no-data marker (gap-filled from neighbours and
    # excluded from the coverage mask), but SRTM stores coastal land at
    # exactly 0 m (Nice airport, the Venice lagoon shore, Danish flats).
    # Nudge genuine sea-level samples to 1 cm so they stay data.
    stored[(stored != NODATA_BAKE) & (stored == 0.0)] = 0.01
    stored[stored == NODATA_BAKE] = 0.0          # C++ no-data convention
    return stored.astype("<f4")


def write_manifest(region, cells):
    """terrain/manifest.json — scalar fields only.

    The runtime probes tiles by file name and reads just the scalar fields
    from the file head (`S3DTileManager::set_data_paths`), so the per-tile
    list the Norway build used to write (139–309 MB per pack) is gone.
    """
    tm = region.tile_m
    manifest = {
        "tile_size_m": tm,
        "pixels_per_tile": region.px,
        "pixel_resolution_m": round(region.px_spacing, 4),
        "format": "float32_raw_le",
        "bytes_per_tile": region.px * region.px * 4,
        "crs": "EPSG:2056",
        "region": region.id,
        "name": region.name,
        "tier": "base",
        "built": date.today().isoformat(),
        "orientation": {
            "pixel_0_0": "SE corner of Swiss tile",
            "col_direction": "East to West (Godot +X)",
            "row_direction": "South to North (Godot +Z)",
        },
        "grid": {},
        "tile_count": len(cells),
        "source": ("AWS skadi SRTM 1° tiles (elevation-tiles-prod), "
                   "WGS84→LV95 reproject, build_world_terrain.py"),
    }
    if cells:
        manifest["grid"] = {
            "ei_min": min(e for e, _ in cells) // tm,
            "ei_max": max(e for e, _ in cells) // tm,
            "ni_min": min(n for _, n in cells) // tm,
            "ni_max": max(n for _, n in cells) // tm,
        }
    with open(region.terrain_out / "manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
    return manifest


def cmd_bake(region, args):
    plan = load_plan(region)
    region.terrain_out.mkdir(parents=True, exist_ok=True)

    cells = list(zip(plan["cells_e"], plan["cells_n"]))
    if args.cell:
        e, n = (int(v) for v in args.cell.split("_"))
        cells = [(e, n)]
    if args.limit:
        cells = cells[:args.limit]
    # Cache locality: process cells in geographic order so the SRTM LRU cache
    # stays hot on a small set of degree tiles.
    cells.sort(key=lambda c: (c[1] // 102400, c[0]))

    nbytes = region.px * region.px * 4
    todo = cells
    skipped = 0
    if not args.force:
        todo = []
        for e_sw, n_sw in cells:
            p = region.terrain_out / f"tile_{e_sw}_{n_sw}.raw"
            try:
                if p.stat().st_size == nbytes:
                    skipped += 1
                    continue
            except FileNotFoundError:
                pass
            todo.append((e_sw, n_sw))

    print(f"=== Bake {region.id}: {len(todo)} tiles "
          f"({skipped} already baked) → {region.terrain_out} ===")
    cache = _SrtmCache(region)
    t0 = time.time()
    all_zero = 0
    for i, (e_sw, n_sw) in enumerate(tqdm(todo, desc="Bake", unit="tile",
                                          mininterval=5.0)):
        data = _bake_cell(region, cache, e_sw, n_sw,
                          fetch_missing=not args.no_fetch)
        data.tofile(region.terrain_out / f"tile_{e_sw}_{n_sw}.raw")
        if not data.any():
            all_zero += 1
        if (i + 1) % 20000 == 0:
            el = time.time() - t0
            rate = (i + 1) / el
            print(f"  {i+1}/{len(todo)} ({rate:.0f} tiles/s, "
                  f"ETA {(len(todo)-i-1)/rate/60:.0f} min)", flush=True)

    manifest = write_manifest(region, cells)
    mb = len(cells) * nbytes / (1024 * 1024)
    print(f"  baked {len(todo)} tiles now, {len(cells)} total ({mb:.0f} MB), "
          f"{all_zero} all-zero, in {time.time()-t0:.0f}s")
    print(f"  manifest: {region.terrain_out / 'manifest.json'} "
          f"(grid {manifest['grid']})")
    return 0


# ── verify: sample baked tiles at known landmarks ───────────────────────────

def cmd_verify(region, args):
    """Check baked elevations against the region's landmarks."""
    tm = region.tile_m
    px = region.px
    if not region.landmarks:
        print(f"  no landmarks configured for {region.id}")
        return 0
    manifest = region.terrain_out / "manifest.json"
    if not manifest.exists():
        print(f"  MISSING manifest {manifest}")
        return 1
    ok = True
    for name, lat, lon, expect, tol in region.landmarks:
        e, n = wgs84_to_lv95(lat, lon)
        ei, ni = int(np.floor(e / tm)), int(np.floor(n / tm))
        path = region.terrain_out / f"tile_{ei*tm}_{ni*tm}.raw"
        if not path.exists():
            print(f"  MISSING {name}: {path.name} not baked yet")
            ok = False
            continue
        data = np.fromfile(path, dtype="<f4").reshape(px, px)
        # Stored lattice: col c ↔ local_e = tm*(1 - c/(px-1)),
        # row r ↔ local_n = tm*r/(px-1).
        local_e = e - ei * tm
        local_n = n - ni * tm
        c = int(round((tm - local_e) / tm * (px - 1)))
        r = int(round(local_n / tm * (px - 1)))
        c = min(max(c, 0), px - 1)
        r = min(max(r, 0), px - 1)
        got = float(data[r, c])
        status = "OK" if abs(got - expect) <= tol else "CHECK"
        if status != "OK":
            ok = False
        print(f"  [{status}] {name}: baked {got:.1f} m (expected ~{expect} ±{tol})")
    return 0 if ok else 1


# ── flatten: runway plateaus (design §12 D18) ───────────────────────────────

def cmd_flatten(region, args):
    """Re-bake the tiles under the region's runways and flatten them onto
    each runway plane (see runway_plateau.py)."""
    import runway_plateau as rp
    data, strips = rp.plan_flatten(region, wgs84_to_lv95)
    if data is None:
        print(f"  {region.id}: no airports/runways.json — nothing to flatten")
        return 0
    rp.set_data(region, data)
    cells = set()
    for st in strips:
        cells.update(rp.runway_tiles(region, st["e0"], st["n0"], st["e1"], st["n1"], st["half_w"]))
    for apn in data.get("_aprons", []):
        xs = [p[0] for p in apn["pts"]]; ys = [p[1] for p in apn["pts"]]
        cells.update(rp.bbox_tiles(region, min(xs), min(ys), max(xs), max(ys), rp.APRON_BLEND_M + rp.DILATE_FRAC * region.px_spacing))
    for tw in data.get("_taxiways", []):
        xs = [p[0] for p in tw["pts"]]; ys = [p[1] for p in tw["pts"]]
        cells.update(rp.bbox_tiles(region, min(xs), min(ys), max(xs), max(ys), tw["half_w"] + rp.TAXI_BLEND_M + rp.DILATE_FRAC * region.px_spacing))
    existing = sorted(c for c in cells
                      if (region.terrain_out / f"tile_{c[0] * region.tile_m}_{c[1] * region.tile_m}.raw").exists())
    print(f"=== Flatten {region.id}: {len(strips)} runways, {len(data.get('_aprons', []))} aprons, "
          f"{len(data.get('_taxiways', []))} taxiways ({data.get('_foreign_count', 0)} airports from "
          f"neighbouring packs), {len(existing)} tiles ===")
    # 1. Fresh source data for every touched tile (idempotent).
    cache = _SrtmCache(region)
    for (ei, ni) in existing:
        data_px = _bake_cell(region, cache, ei * region.tile_m, ni * region.tile_m,
                             fetch_missing=not getattr(args, "no_fetch", False))
        data_px.tofile(region.terrain_out / f"tile_{ei * region.tile_m}_{ni * region.tile_m}.raw")
    # 2. Plateaus.
    store = rp.TileStore(region)
    report = [f"{region.id}: {len(strips)} runways, {len(existing)} tiles re-baked"]
    touched = rp.apply_plateaus(region, strips, store, report)
    n = store.write_all()
    rp.write_plane_heights(region, data, strips)
    report.insert(1, f"{len(touched)} tiles flattened, {n} written")
    rpath = region.build_dir / "plateau_report.txt"
    with open(rpath, "w") as f:
        f.write("\n".join(report) + "\n")
    print(f"  {len(touched)} tiles flattened → {rpath}")
    return 0


# ── main ─────────────────────────────────────────────────────────────────────

def cmd_list(args):
    cfg = load_config()
    print(f"regions in {REGIONS_JSON.name} (build root {cfg['build_root']}):")
    for e in cfg["regions"]:
        r = Region(cfg, e)
        state = "baked" if (r.terrain_out / "manifest.json").exists() else (
            "planned" if r.plan_path.exists() else "-")
        sel = (f"country {','.join(r.iso_a3)}" if r.select == "country"
               else f"bbox {r.bbox_wgs84}")
        print(f"  {r.id:<18} {r.name:<14} {sel:<28} {state}")
    return 0


def main():
    p = argparse.ArgumentParser(
        description="Worldwide base terrain build (SRTM → LV95 .raw tiles)")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_region(sp):
        sp.add_argument("--region", required=True,
                        help="region id from scripts/world_regions.json")

    sub.add_parser("list", help="show the configured regions and their state")
    add_region(sub.add_parser("plan", help="compute LV95 cells + SRTM tile list"))
    add_region(sub.add_parser("fetch", help="download missing SRTM tiles"))
    pb = sub.add_parser("bake", help="reproject cells into .raw tiles + manifest")
    add_region(pb)
    pb.add_argument("--limit", type=int, default=0,
                    help="only bake the first N cells (testing)")
    pb.add_argument("--cell", type=str, default=None,
                    help="bake a single cell 'E_N' (SW corner, ×1024)")
    pb.add_argument("--no-fetch", action="store_true",
                    help="do not fetch missing SRTM tiles on demand")
    pb.add_argument("--force", action="store_true",
                    help="re-bake tiles that already exist (default: resume)")
    add_region(sub.add_parser("verify", help="check baked tiles at landmarks"))
    add_region(sub.add_parser("flatten", help="runway plateaus from airports/runways.json (re-run after bake --force)"))
    pbuild = sub.add_parser("build", help="plan + fetch + bake + flatten + verify")
    add_region(pbuild)
    pbuild.add_argument("--force", action="store_true")

    args = p.parse_args()
    if args.cmd == "list":
        sys.exit(cmd_list(args))
    region = get_region(args.region)
    if args.cmd == "plan":
        cmd_plan(region, args)
    elif args.cmd == "fetch":
        sys.exit(cmd_fetch(region, args))
    elif args.cmd == "bake":
        sys.exit(cmd_bake(region, args))
    elif args.cmd == "verify":
        sys.exit(cmd_verify(region, args))
    elif args.cmd == "flatten":
        sys.exit(cmd_flatten(region, args))
    elif args.cmd == "build":
        cmd_plan(region, args)
        if cmd_fetch(region, args):
            sys.exit(1)
        args.limit = 0
        args.cell = None
        args.no_fetch = False
        if cmd_bake(region, args):
            sys.exit(1)
        cmd_flatten(region, args)
        sys.exit(cmd_verify(region, args))


if __name__ == "__main__":
    main()
