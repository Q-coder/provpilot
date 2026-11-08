#!/usr/bin/env python3
"""Forests for the base tier from OpenStreetMap (docs/WORLD_SCENERY_DESIGN.md §14.2
step 4): per 1024 m LV95 tile a `vegetation/forest_E_N.bin` with the forest
polygons (landuse=forest, natural=wood; leaf_type -> broadleaf / conifer /
mixed) and the road and railway corridors (centre line + half width) no tree
may stand in. S3DVegetationManager fills the polygons with trees at load
time (a few KB per tile instead of 24 bytes per tree) and applies the
corridors to the surveyed vegetation_E_N.bin trees of the same square too.

  python3 scripts/build_world_forests.py --region switzerland-base --roads-only --install /Users/gery/provpilot/scenery/Switzerland
  python3 scripts/build_world_forests.py --region austria [--bbox E0 N0 E1 N1] [--install <pack dir>]

System python3 (osmium + pyproj + shapely). The Geofabrik PBF is cached in
<cache_root>/osm/ like the roads bake. Only squares with a terrain tile in
the region's build dir are written (--all-squares to ignore that).

File (little-endian): "FOR1", u32 1, u32 polygon count, u32 0, f64 tile
east, f64 tile north; polygon: u8 kind (0 broadleaf, 1 conifer, 2 mixed,
3 corridor), u8[3], f32 density trees/m2 (corridor: half width m), f32 mean
height m, u32 ring count; ring: u32 n, n x (f32 dx, f32 dz) from the SW corner.
"""
import argparse
import json
import math
import struct
import sys
import time
import urllib.request
from collections import defaultdict
from pathlib import Path

import osmium
import pyproj
from shapely.geometry import LineString, Polygon, box
from shapely.ops import transform as shp_transform

ROOT = Path(__file__).resolve().parent.parent
REGIONS = json.loads((ROOT / "scripts" / "world_regions.json").read_text())
BUILD_ROOT = Path(REGIONS.get("build_root", "/Volumes/Data1/provpilot/build"))
CACHE_ROOT = Path(REGIONS.get("cache_root", "/Volumes/Data1/scenery_in_world"))
OSM_CACHE = CACHE_ROOT / "osm"
GEOFABRIK = {"switzerland-base": "europe/switzerland", "austria": "europe/austria", "germany": "europe/germany",
             "denmark": "europe/denmark", "italy": "europe/italy", "france": "europe/france",
             "norway": "europe/norway", "sweden": "europe/sweden", "finland": "europe/finland"}
TILE = 1024
DENSITY = 1.0 / 36.0          # trees per m2 inside a forest polygon (6 m spacing)
HEIGHT_M = {0: 20.0, 1: 24.0, 2: 21.0}
# half widths of the corridors kept clear, m (pavement + verge)
ROAD_HALF = {"motorway": 16.0, "trunk": 11.0, "primary": 8.0, "secondary": 7.0, "tertiary": 6.0,
             "motorway_link": 7.0, "trunk_link": 6.0, "primary_link": 5.5, "secondary_link": 5.0, "tertiary_link": 5.0,
             "residential": 5.0, "unclassified": 5.0, "living_street": 4.0, "service": 3.5, "track": 3.0}
RAIL_HALF = 5.0
TO_LV95 = pyproj.Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)


def leaf_kind(tags):
    lt = tags.get("leaf_type", "")
    if lt.startswith("needle"):
        return 1
    if lt.startswith("broad"):
        return 0
    wood = tags.get("wood", "")
    if wood in ("coniferous", "conifer"):
        return 1
    if wood in ("deciduous", "broad_leaved"):
        return 0
    return 2


class Collector(osmium.SimpleHandler):
    """Forest areas and corridor ways, projected to LV95 and clipped per tile."""

    def __init__(self, bbox, roads_only):
        super().__init__()
        self.bbox = bbox            # (e0, n0, e1, n1) or None
        self.roads_only = roads_only
        self.tiles = defaultdict(lambda: {"forest": [], "corridor": []})
        self.n_forest = self.n_ways = 0
        self.wkb = osmium.geom.WKBFactory()

    def _tile_range(self, geom):
        e0, n0, e1, n1 = geom.bounds
        if self.bbox and (e1 < self.bbox[0] or e0 > self.bbox[2] or n1 < self.bbox[1] or n0 > self.bbox[3]):
            return None
        return range(int(e0 // TILE), int(e1 // TILE) + 1), range(int(n0 // TILE), int(n1 // TILE) + 1)

    def area(self, a):
        if self.roads_only:
            return
        tags = a.tags
        if tags.get("landuse") != "forest" and tags.get("natural") != "wood":
            return
        try:
            from shapely import wkb as shp_wkb
            geom = shp_wkb.loads(self.wkb.create_multipolygon(a), hex=True)
        except Exception:
            return
        geom = shp_transform(TO_LV95.transform, geom)
        if geom.is_empty:
            return
        rng = self._tile_range(geom)
        if rng is None:
            return
        kind = leaf_kind(tags)
        self.n_forest += 1
        for te in rng[0]:
            for tn in rng[1]:
                cell = box(te * TILE, tn * TILE, (te + 1) * TILE, (tn + 1) * TILE)
                part = geom.intersection(cell)
                if part.is_empty:
                    continue
                polys = [part] if part.geom_type == "Polygon" else [g for g in getattr(part, "geoms", []) if g.geom_type == "Polygon"]
                for poly in polys:
                    if poly.area < 50.0:
                        continue
                    rings = [list(poly.exterior.coords)] + [list(r.coords) for r in poly.interiors]
                    self.tiles[(te, tn)]["forest"].append((kind, rings))

    def way(self, w):
        tags = w.tags
        half = None
        if "highway" in tags and tags["highway"] in ROAD_HALF:
            half = ROAD_HALF[tags["highway"]]
            if tags.get("tunnel") in ("yes", "building_passage") or tags.get("covered") == "yes":
                return
        elif tags.get("railway") == "rail" and tags.get("tunnel") not in ("yes",):
            half = RAIL_HALF
        if half is None or len(w.nodes) < 2:
            return
        try:
            pts = [(n.lon, n.lat) for n in w.nodes if n.location.valid()]
        except Exception:
            return
        if len(pts) < 2:
            return
        line = shp_transform(TO_LV95.transform, LineString(pts))
        rng = self._tile_range(line)
        if rng is None:
            return
        self.n_ways += 1
        for te in rng[0]:
            for tn in rng[1]:
                cell = box(te * TILE - half - 2, tn * TILE - half - 2, (te + 1) * TILE + half + 2, (tn + 1) * TILE + half + 2)
                part = line.intersection(cell)
                if part.is_empty:
                    continue
                lines = [part] if part.geom_type == "LineString" else [g for g in getattr(part, "geoms", []) if g.geom_type == "LineString"]
                for ln in lines:
                    if len(ln.coords) >= 2:
                        self.tiles[(te, tn)]["corridor"].append((half, list(ln.coords)))


def write_tile(path: Path, te: int, tn: int, data):
    e0, n0 = te * TILE, tn * TILE
    polys = []
    for kind, rings in data["forest"]:
        polys.append((kind, DENSITY, HEIGHT_M[kind], rings))
    for half, coords in data["corridor"]:
        polys.append((3, half, 0.0, [coords]))
    with open(path, "wb") as f:
        f.write(b"FOR1")
        f.write(struct.pack("<III", 1, len(polys), 0))
        f.write(struct.pack("<dd", float(e0), float(n0)))
        for kind, density, height, rings in polys:
            f.write(struct.pack("<BBBBffI", kind, 0, 0, 0, density, height, len(rings)))
            for ring in rings:
                f.write(struct.pack("<I", len(ring)))
                for x, y in ring:
                    f.write(struct.pack("<ff", x - e0, y - n0))


def ensure_pbf(region: str, pbf_arg):
    if pbf_arg:
        return Path(pbf_arg)
    name = GEOFABRIK.get(region, region).split("/")[-1] + "-latest.osm.pbf"
    pbf = OSM_CACHE / name
    if not pbf.exists():
        url = f"https://download.geofabrik.de/{GEOFABRIK[region]}-latest.osm.pbf"
        print(f"  downloading {url} -> {pbf}")
        OSM_CACHE.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, pbf)
    return pbf


def main() -> int:
    p = argparse.ArgumentParser(description="OSM forests + road corridors -> <pack>/vegetation/forest_E_N.bin")
    p.add_argument("--region", required=True, help="world_regions.json id (switzerland-base, austria, germany, ...)")
    p.add_argument("--pbf", help="a local .osm.pbf instead of the cached Geofabrik file")
    p.add_argument("--bbox", nargs=4, type=float, metavar=("E0", "N0", "E1", "N1"), help="LV95 window")
    p.add_argument("--roads-only", action="store_true", help="corridors only (a pack whose trees come from surveyed vegetation_*.bin)")
    p.add_argument("--all-squares", action="store_true", help="write every square, not only those with a terrain tile")
    p.add_argument("--out", help="output pack dir (default <build_root>/<region>/<Subdir>)")
    p.add_argument("--install", help="also copy the vegetation dir into this installed pack dir")
    args = p.parse_args()

    regions = REGIONS["regions"]
    regions = regions if isinstance(regions, list) else list(regions.values())
    reg = next((r for r in regions if r["id"] == args.region), None)
    if reg is None:
        print("unknown region", args.region)
        return 2
    out = Path(args.out) if args.out else BUILD_ROOT / reg["id"] / reg.get("subdir", reg["id"])
    veg_dir = out / "vegetation"
    veg_dir.mkdir(parents=True, exist_ok=True)
    terrain_dir = out / "terrain"
    squares = None
    if not args.all_squares and terrain_dir.exists():
        squares = set()
        for f in terrain_dir.glob("tile_*.raw"):
            parts = f.stem.split("_")
            if len(parts) >= 3:
                squares.add((int(parts[-2]) // TILE, int(parts[-1]) // TILE))
        print(f"  {len(squares)} terrain squares in {terrain_dir}")
    pbf = ensure_pbf(args.region, args.pbf)
    t0 = time.time()
    print(f"=== forests {args.region}: {pbf} ===")
    col = Collector(tuple(args.bbox) if args.bbox else None, args.roads_only)
    col.apply_file(str(pbf), locations=True, idx="flex_mem")
    print(f"  {col.n_forest} forest areas, {col.n_ways} corridor ways in {time.time() - t0:.0f} s; {len(col.tiles)} squares touched")
    written = trees_est = 0
    for (te, tn), data in sorted(col.tiles.items()):
        if squares is not None and (te, tn) not in squares:
            continue
        if not data["forest"] and not data["corridor"]:
            continue
        write_tile(veg_dir / f"forest_{te * TILE}_{tn * TILE}.bin", te, tn, data)
        written += 1
        trees_est += sum(Polygon(r[0], r[1:]).area for _, r in data["forest"]) * DENSITY
    size_mb = sum(f.stat().st_size for f in veg_dir.glob("forest_*.bin")) / 1048576
    print(f"  {written} forest tiles written to {veg_dir} ({size_mb:.1f} MB), about {trees_est / 1e6:.1f} M trees at 1 per {1 / DENSITY:.0f} m2")
    if args.install:
        import shutil
        dst = Path(args.install) / "vegetation"
        dst.mkdir(parents=True, exist_ok=True)
        n = 0
        for f in veg_dir.glob("forest_*.bin"):
            shutil.copy2(f, dst / f.name)
            n += 1
        print(f"  installed {n} tiles into {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
