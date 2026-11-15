#!/usr/bin/env python3
"""The sea for the base tier (docs/SEA.md, WORLD_SCENERY_DESIGN.md §11.4): cuts the
OpenStreetMap coastline into the 34 px terrain tiles, so one sea surface at
0 m drawn by the engine meets the land exactly at the shore.

Per pixel, from the OSM sea polygons (osmdata.openstreetmap.de, water-polygons-
split-4326, in <cache_root>/osm_coast/):
  sea:   h = min(h, -SLOPE * distance to the shore), at most -MAX_DEPTH_M by
         distance alone (real bathymetry already in the tile stays when deeper)
  land:  h = max(h, +SLOPE * distance to the shore, LAND_MIN_M beyond the first
         pixels): the waterline, linearly interpolated between two pixels, lies
         on the coastline; land below sea level (polders) is lifted to
         LAND_MIN_M so the sea does not flood it.
No extra file is written: the coast is part of the terrain tiles, which is all
a streamed base scenery needs to carry. The pass is idempotent and edits both
terrain/ and terrain_base/ in place (the same pixels in both; road corridors
and runway plateaus stay), and lifts airport heights below LAND_MIN_M in
airports/runways.json with the land. Re-run it after `build_world_terrain.py bake` or
`flatten`, before the road bake.

  .venv/bin/python scripts/build_world_sea.py --region norway [--bbox E0 N0 E1 N1] [--jobs 8] [--dry-run]

Needs shapely >= 2.1, pyogrio, pyproj, numpy (the project venv).
"""
import argparse
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import pyogrio
import shapely
from shapely.geometry import box

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_world_terrain as bwt  # noqa: E402

SLOPE = 0.05          # metres of height per metre from the shore
MAX_DEPTH_M = 30.0    # by distance alone (600 m offshore)
LAND_MIN_M = 0.3      # land never lies lower (polders, tidal flats)
MARGIN_M = 640.0      # coastline looked at around a tile: MAX_DEPTH_M / SLOPE + a pixel
NODATA_NUDGE = 0.01   # the engine reads exactly 0 as "no data"

_G = {}


def load_sea(region, bbox_lv95):
    """The sea polygons over the box, in LV95 metres, with an STRtree."""
    shp = Path(bwt.load_config()["cache_root"]) / "osm_coast" / "water-polygons-split-4326" / "water_polygons.shp"
    if not shp.exists():
        sys.exit(f"missing {shp} — download https://osmdata.openstreetmap.de/download/water-polygons-split-4326.zip there")
    e0, n0, e1, n1 = bbox_lv95
    es = np.array([e0, e1, e0, e1, (e0 + e1) / 2, (e0 + e1) / 2, e0, e1])
    ns = np.array([n0, n0, n1, n1, n0, n1, (n0 + n1) / 2, (n0 + n1) / 2])
    lat, lon = bwt.lv95_to_wgs84_proj(es, ns)
    wgs_box = (float(lon.min()) - 0.05, float(lat.min()) - 0.05, float(lon.max()) + 0.05, float(lat.max()) + 0.05)
    geoms = pyogrio.read_dataframe(shp, bbox=wgs_box, columns=[]).geometry.values
    geoms = np.asarray([g for g in geoms if g is not None and not g.is_empty], dtype=object)
    if len(geoms) == 0:
        return np.asarray([], dtype=object)

    def to_lv95(coords):
        e, n = bwt.wgs84_to_lv95(coords[:, 1], coords[:, 0])
        return np.column_stack([e, n])

    return shapely.transform(geoms, to_lv95)


def _init(geoms, terrain_dirs, px, tile_m, dry):
    _G["tree"] = shapely.STRtree(geoms) if len(geoms) else None
    _G["geoms"] = geoms
    _G["dirs"] = terrain_dirs
    _G["px"] = px
    _G["tile_m"] = tile_m
    _G["dry"] = dry


def carve(data, e_sw, n_sw, px, tile_m, water):
    """data: px x px float32 in the stored orientation (pixel(0,0) = SE corner,
    columns east -> west, rows south -> north). water: the sea geometry around
    the tile (None: none). Returns the new array and what changed."""
    sp = tile_m / (px - 1)
    cc = np.arange(px)
    ee = e_sw + (px - 1 - cc) * sp
    nn = n_sw + cc * sp
    E, N = np.meshgrid(ee, nn)   # rows: north index, columns: stored column
    out = data.astype(np.float64).copy()
    valid = out != 0.0
    if water is None or water.is_empty:
        low = valid & (out < LAND_MIN_M)
        out[low] = LAND_MIN_M
        return out.astype("<f4"), 0, int(low.sum())
    window = box(e_sw - MARGIN_M, n_sw - MARGIN_M, e_sw + tile_m + MARGIN_M, n_sw + tile_m + MARGIN_M)
    land = window.difference(water)
    pts_e = E.ravel()
    pts_n = N.ravel()
    in_sea = shapely.contains_xy(water, pts_e, pts_n).reshape(px, px)
    pts = shapely.points(pts_e, pts_n)
    if land.is_empty:
        d_sea = np.full(px * px, MARGIN_M)
    else:
        d_sea = shapely.distance(pts, land)
    d_land = shapely.distance(pts, water)
    d_sea = d_sea.reshape(px, px)
    d_land = d_land.reshape(px, px)
    sea_h = -np.minimum(SLOPE * d_sea, MAX_DEPTH_M)
    sea_h = np.minimum(sea_h, -NODATA_NUDGE)
    land_h = np.maximum(np.minimum(SLOPE * d_land, LAND_MIN_M + 10.0), NODATA_NUDGE)
    # beyond the first pixel from the shore the land keeps a floor (polders)
    land_floor = np.where(d_land > sp, np.maximum(land_h, LAND_MIN_M), land_h)
    new = np.where(in_sea, np.minimum(out, sea_h), np.maximum(out, land_floor))
    new = np.where(valid | in_sea, new, out)   # a no-data pixel stays no data on land
    changed_sea = int((in_sea & (np.abs(new - out) > 1e-4)).sum())
    changed_land = int((~in_sea & (np.abs(new - out) > 1e-4)).sum())
    return new.astype("<f4"), changed_sea, changed_land


def _work(cell):
    ei, ni = cell
    px, tile_m = _G["px"], _G["tile_m"]
    e_sw, n_sw = ei * tile_m, ni * tile_m
    water = None
    if _G["tree"] is not None:
        window = box(e_sw - MARGIN_M, n_sw - MARGIN_M, e_sw + tile_m + MARGIN_M, n_sw + tile_m + MARGIN_M)
        idx = _G["tree"].query(window, predicate="intersects")
        if len(idx):
            parts = shapely.intersection(_G["geoms"][idx], window)
            water = shapely.union_all(shapely.make_valid(parts))
            # the sea polygons are cut on a degree grid: close the hairline seams
            water = water.buffer(0.05)
    name = f"tile_{e_sw}_{n_sw}.raw"
    sea_px = land_px = 0
    wrote = 0
    for d in _G["dirs"]:
        p = Path(d) / name
        if not p.exists():
            continue
        data = np.fromfile(p, dtype="<f4")
        if data.size != px * px:
            continue
        new, cs, cl = carve(data.reshape(px, px), e_sw, n_sw, px, tile_m, water)
        sea_px = max(sea_px, cs)
        land_px = max(land_px, cl)
        if (cs or cl) and not _G["dry"]:
            new.tofile(p)
            wrote += 1
    kind = 0 if water is None else 1
    return kind, sea_px, land_px, wrote


def fix_airports(region, dry):
    """Airport heights fitted to the terrain before the cut (runway planes,
    taxiway paths, apron polygons, mapped lights) follow the land floor: a
    strip on ground that lay below LAND_MIN_M would be drawn under it."""
    import json
    import runway_plateau as rp
    path = rp.runways_json_path(region)
    if not path.exists():
        return 0
    data = json.loads(path.read_text())
    n = 0
    for ap in data.get("airports", []):
        for rw in ap.get("runways", []):
            for end in rw.get("ends", []):
                v = end.get("plane_elev_m")
                if isinstance(v, (int, float)) and v < LAND_MIN_M:
                    end["plane_elev_m"] = LAND_MIN_M
                    n += 1
        for key, sub in (("taxiways", "path"), ("aprons", "polygon")):
            for item in ap.get(key, []):
                for pt in item.get(sub) or []:
                    if isinstance(pt, list) and len(pt) >= 3 and isinstance(pt[2], (int, float)) and pt[2] < LAND_MIN_M:
                        pt[2] = LAND_MIN_M
                        n += 1
        for lt in ap.get("lights", []):
            v = lt.get("elev_m")
            if isinstance(v, (int, float)) and v < LAND_MIN_M:
                lt["elev_m"] = LAND_MIN_M
                n += 1
    if n and not dry:
        with open(path, "w") as f:
            json.dump(data, f, indent=1, ensure_ascii=False)
    print(f"  airports: {n} heights below {LAND_MIN_M} m {'would be ' if dry else ''}lifted in {path.name}")
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--region", required=True)
    ap.add_argument("--bbox", type=float, nargs=4, metavar=("E0", "N0", "E1", "N1"), help="LV95 box (default: every tile of the region)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--airports-only", action="store_true", help="only lift the airport heights in runways.json")
    args = ap.parse_args()
    region = bwt.get_region(args.region)
    px, tile_m = region.px, region.tile_m
    t0 = time.time()
    if args.airports_only:
        fix_airports(region, args.dry_run)
        return 0
    cells = []
    for p in region.terrain_out.glob("tile_*.raw"):
        _, e, n = p.stem.split("_")
        e, n = int(e), int(n)
        if args.bbox and not (args.bbox[0] - tile_m < e < args.bbox[2] and args.bbox[1] - tile_m < n < args.bbox[3]):
            continue
        cells.append((e // tile_m, n // tile_m))
    if not cells:
        sys.exit("no tiles in the box")
    es = [c[0] for c in cells]
    ns = [c[1] for c in cells]
    bbox = (min(es) * tile_m - MARGIN_M, min(ns) * tile_m - MARGIN_M, (max(es) + 1) * tile_m + MARGIN_M, (max(ns) + 1) * tile_m + MARGIN_M)
    print(f"=== Sea {region.id}: {len(cells)} tiles, box {bbox[0]:.0f} {bbox[1]:.0f} {bbox[2]:.0f} {bbox[3]:.0f} ===", flush=True)
    geoms = load_sea(region, bbox)
    print(f"  {len(geoms)} sea polygons ({time.time() - t0:.0f} s)", flush=True)
    dirs = [str(region.terrain_out), str(region.terrain_base)]
    cells.sort()
    tot = {"coast": 0, "sea_px": 0, "land_px": 0, "files": 0}
    done = 0
    with ProcessPoolExecutor(max_workers=args.jobs, initializer=_init, initargs=(geoms, dirs, px, tile_m, args.dry_run)) as ex:
        for kind, sp_, lp_, wrote in ex.map(_work, cells, chunksize=64):
            tot["coast"] += kind
            tot["sea_px"] += sp_
            tot["land_px"] += lp_
            tot["files"] += wrote
            done += 1
            if done % 20000 == 0:
                print(f"  {done}/{len(cells)} tiles, {tot['coast']} with sea, {tot['files']} files written ({time.time() - t0:.0f} s)", flush=True)
    print(f"  {len(cells)} tiles: {tot['coast']} with sea in reach, {tot['sea_px']} sea pixels and {tot['land_px']} land pixels changed, "
          f"{tot['files']} files {'would be ' if args.dry_run else ''}written ({time.time() - t0:.0f} s)")
    fix_airports(region, args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
