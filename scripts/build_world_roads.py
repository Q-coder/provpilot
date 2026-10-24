#!/usr/bin/env python3
"""build_world_roads.py — roads and railways for the base tier (design §13.9).

Bakes OpenStreetMap roads (motorway … tertiary) and railways (rail, narrow
gauge, light rail, tram) of one base region into Scenery3D road tiles:

    <region>/roads/roads_<E>_<N>.glb        one per 1024 m LV95 tile
    <region>/roads/manifest.json            conversion origin, tile centres

Quality target (gery, 2026-09-22): the same as the runways and taxiways —
baked, not draped. Every centreline is densified to 10 m, its height is the
base terrain (34 px / 1024 m SRTM tiles, bilinear) smoothed along the way
(roads ±120 m, rails ±250 m; bridges span straight between their ends,
tunnels are dropped), and with --corridor the terrain tiles are re-written
so the ground under each road / track follows that profile (the road cut
and embankment on a 30 m posting). Terrain writes are derived, never
cumulative: every tile of the box starts again from <region>/terrain_base/
(the tiles as build_world_terrain.py baked them, airport plateaus included;
a tile without a pristine copy yet gets one from terrain/ on first use), then
the designs' plateaus and the corridors go in. So a plateau or way deleted
from a design and baked again leaves no trace in the ground. Road widths come from the lane count
(OSM `lanes`, class default) per carriageway: motorways are mapped one
way per carriageway, so each gets its own lanes plus a hard shoulder.
Rails are real geometry: a ballast bed per track plus two extruded rails
(UIC 60 section, 172 mm high) at the track gauge (OSM `gauge` in mm,
default 1435; narrow_gauge default 1000), so a train could run on them.

Vertex data: POSITION, NORMAL, COLOR_0 (class colour, alpha = surface kind
for shaders/roads.gdshader), TEXCOORD_0 (u = across in metres, signed;
v = along in metres), TEXCOORD_1 (half width in metres, lane count of a
road / track count of a ballast bed). Vertex Y = terrain + ROAD_LIFT_M; the
runtime drops baked tiles by S3DRoadManager.baked_drop_m (0.32 m).

Usage (Zürich starter box, LV95):
  scripts/build_world_roads.py --region switzerland-base \\
      --bbox 2660000 1230000 2710000 1275000 --corridor
  scripts/build_world_roads.py --region switzerland-base        # whole region

Inputs: Geofabrik PBF in /Volumes/Data1/scenery_in_world/osm/ (downloaded
by hand or --fetch), the region's baked terrain from build_world_terrain.py.
OSM data © OpenStreetMap contributors, ODbL 1.0.
Terrain: the region's base tiles (30 m posting), read from the pristine
terrain_base and written back with the corridors by --corridor and the
design's plateaus. Without --corridor the profiles sit on the pristine
ground, not on the corridor-cut terrain/ that is drawn: a test bake into
another --out must use --corridor with a --terrain-dir copy to compare.
--terrain-overlay DIR adds a finer terrain drawn over the base tiles (an
enabled 2 m Swiss scenery area, same tile format): where it has data the
ribbons follow it, with a short smoothing window, since a ribbon baked on
the 30 m tier floats or sinks by metres on it. The scenery editor's Bake
area passes the overlays of the enabled areas itself.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import struct
import sys
import time
import urllib.request
from collections import defaultdict
from pathlib import Path

import numpy as np

try:
    from pyproj import Transformer
except ImportError as e:  # pragma: no cover
    print("ERROR: pyproj is required (pip install pyproj):", e, file=sys.stderr)
    sys.exit(1)
try:
    import osmium                      # only the OSM source needs it
except ImportError:                    # pragma: no cover
    osmium = None

ROOT = Path(__file__).resolve().parent.parent
BUILD_ROOT = Path("/Volumes/Data1/provpilot/build")
OSM_CACHE = Path("/Volumes/Data1/scenery_in_world/osm")
GEOFABRIK = {"switzerland-base": "europe/switzerland", "austria": "europe/austria", "germany": "europe/germany",
             "denmark": "europe/denmark", "italy": "europe/italy", "france": "europe/france",
             "norway": "europe/norway", "sweden": "europe/sweden", "finland": "europe/finland"}

TILE_M = 1024
CONV_E = 2700000.0     # manifest conversion origin (vertex.x = CONV_E - east, vertex.z = north - CONV_N)
CONV_N = 1310000.0
DENSIFY_M = 10.0
RUNTIME_DROP_M = 0.32  # S3DRoadManager.baked_drop_m: subtracted from every baked Y at load
# Bridges and tunnels: OSM has no heights, so these are the guesses (design
# §13.9). A bridge deck is the straight chord between its end grounds, lifted
# until it clears every way it crosses; the approach ways ramp up to it.
# A tunnel dives below the surface behind its portals and is drawn there
# (hidden by the terrain, so the line stays continuous from the air).
BRIDGE_CLEARANCE_M = 5.5   # deck above the surface of a road / track it crosses
BRIDGE_GROUND_M = 0.3      # deck above the ground under it, at least
RAMP_GRADE = 0.04          # approach embankment grade towards a lifted deck
RAMP_MIN_M = 30.0
TUNNEL_DEPTH_M = 8.0       # ribbon below the surface inside a tunnel
SHORT_TUNNEL_M = 200.0     # a tunnel chain shorter than this is an underpass: drawn at grade
PORTAL_M = 40.0            # dive length behind a portal
# Ribbons of a lower class sit a little lower so a link merging into a
# motorway or a tertiary crossing a primary is drawn cleanly under it.
CLASS_DROP_M = {"motorway": 0.0, "trunk": 0.01, "primary": 0.02, "secondary": 0.03, "tertiary": 0.04,
                "motorway_link": 0.05, "trunk_link": 0.06, "primary_link": 0.07,
                "secondary_link": 0.08, "tertiary_link": 0.09}
ROAD_LIFT_M = 0.52     # baked into Y; after the runtime drop the road is 0.2 m above the ground
MITER_LIMIT = 2.5

# highway class -> (default lanes, colour, smoothing half-window m). Widths are
# built from the lane count: lanes x LANE_M plus the margins of the kind.
#   kind 1 one-way carriageway (motorway / trunk / any oneway way with 2+
#          lanes: edge lines, lane dashes, hard shoulder on the right)
#   kind 2 two-way trunk / primary (edge lines + dashed centre line)
#   kind 3 two-way secondary / tertiary (dashed centre line)
#   kind 4 link / ramp / one-lane one-way street (edge lines only)
#   kind 5 two-way motorway mapped as a single way (median + two carriageways)
ROAD_CLASSES = {
    "motorway":       (2, (0.30, 0.30, 0.31), 120.0),
    "motorway_link":  (1, (0.30, 0.30, 0.31), 80.0),
    "trunk":          (2, (0.32, 0.32, 0.33), 100.0),
    "trunk_link":     (1, (0.32, 0.32, 0.33), 80.0),
    "primary":        (2, (0.34, 0.34, 0.35), 100.0),
    "primary_link":   (1, (0.34, 0.34, 0.35), 60.0),
    "secondary":      (2, (0.36, 0.36, 0.37), 80.0),
    "secondary_link": (1, (0.36, 0.36, 0.37), 60.0),
    "tertiary":       (2, (0.38, 0.38, 0.39), 60.0),
    "tertiary_link":  (1, (0.38, 0.38, 0.39), 60.0),
}
LANE_M = {"motorway": 3.75, "motorway_link": 3.75, "trunk": 3.5, "trunk_link": 3.5,
          "primary": 3.25, "primary_link": 3.25, "secondary": 3.0, "secondary_link": 3.0,
          "tertiary": 2.9, "tertiary_link": 2.9}
SHOULDER_M = 2.5          # hard shoulder on a one-way motorway / trunk carriageway
VERGE_M = 0.5             # paved margin outside the edge line (both sides)
KIND_ONEWAY, KIND_TWOWAY_LINED, KIND_TWOWAY, KIND_LINK, KIND_DUAL = 1, 2, 3, 4, 5


def road_geometry(cls: str, lanes_tag, width_tag, oneway: bool):
    """(width m, kind, lanes) of an OSM highway way. Motorways in the OSM data
    are one way per carriageway, so a one-way motorway is a single carriageway
    of `lanes` lanes with a hard shoulder; the rare two-way motorway way gets
    both carriageways and the median (kind 5)."""
    default_lanes, _colour, _hw = ROAD_CLASSES[cls]
    lane_m = LANE_M[cls]
    lanes = int(parse_num(lanes_tag, 0)) or default_lanes
    lanes = max(1, min(6, lanes))
    link = cls.endswith("_link")
    if cls in ("motorway", "trunk") and oneway:
        width = lanes * lane_m + SHOULDER_M + 2 * VERGE_M
        kind = KIND_ONEWAY
    elif cls == "motorway":                       # two-way motorway on one way: dual carriageway
        per = max(2, lanes // 2)
        width = 2 * (per * lane_m + SHOULDER_M + VERGE_M) + 3.0
        kind, lanes = KIND_DUAL, per
    elif link or (oneway and lanes == 1):
        width = lanes * lane_m + 2 * VERGE_M
        kind = KIND_LINK
    elif oneway:
        width = lanes * lane_m + 2 * VERGE_M
        kind = KIND_ONEWAY
    else:
        width = lanes * lane_m + 2 * VERGE_M
        kind = KIND_TWOWAY_LINED if cls in ("trunk", "primary") else KIND_TWOWAY
    tag_w = parse_num(width_tag, 0)
    if 3.0 <= tag_w <= 60.0:
        width = tag_w
        if width < 5.0 and kind != KIND_ONEWAY:
            kind = KIND_LINK   # a narrow road has no centre line
    return width, kind, lanes


# railway class -> (default gauge mm, ballast, smoothing half-window m)
RAIL_CLASSES = {
    "rail":         (1435, True, 250.0),
    "narrow_gauge": (1000, True, 200.0),
    "light_rail":   (1435, True, 150.0),
    "tram":         (1000, False, 60.0),
}
KIND_BALLAST = 8
KIND_RAIL = 9
BALLAST_WIDTH_M = 4.2      # single track bed; +3.8 m per extra track
TRACK_SPACING_M = 3.8
SLEEPER_M = 2.6            # sleeper length (drawn by the shader per track)
RAIL_HEAD_M = 0.072        # rail head width
RAIL_FOOT_M = 0.15         # rail foot width on the sleepers
RAIL_TOP_M = 0.172         # rail height above the sleeper top (UIC 60)
# rail cross-section across the rail centre, (u offset m, height m): foot, head, head, foot
RAIL_PROFILE = ((-RAIL_FOOT_M / 2, 0.0), (-RAIL_HEAD_M / 2, RAIL_TOP_M),
                (RAIL_HEAD_M / 2, RAIL_TOP_M), (RAIL_FOOT_M / 2, 0.0))
BALLAST_LIFT_M = 0.15      # ballast surface above the ground after the runtime drop
# Terrain pixels within half width + margin follow the profile. The margin
# must reach the far corner of every 30 m grid cell a ribbon passes through
# (pixel spacing x sqrt 2), or the ground interpolated between a corridor
# pixel and an untouched one rises through the ribbon on a hillside; the
# shelf that gives is the price of the base tier's posting.
CORRIDOR_MARGIN_MIN_M = 14.0
CORRIDOR_BLEND_M = 3.0     # overlap weight 1 / (edge distance + this)^2

lv95 = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
wgs = Transformer.from_crs("EPSG:2056", "EPSG:4326", always_xy=True)


# ── terrain ─────────────────────────────────────────────────────────────────
def _sample_tile(layer, e: float, n: float, tri: bool, nodata_zero: bool = False) -> float:
    """Height at (e, n) from a tile layer (`load(te, tn)`, `px`, `tile_m`):
    bilinear, or with tri=True the runtime mesh's own surface (s3d_tile.cpp
    splits every pixel quad along the (col + 1, row) - (col, row + 1)
    diagonal) - what a ribbon must clear. NaN without a tile, and with
    nodata_zero where one of the four pixels is 0.0 (an overlay's "no data")."""
    te = int(math.floor(e / layer.tile_m)) * layer.tile_m
    tn = int(math.floor(n / layer.tile_m)) * layer.tile_m
    a = layer.load(te, tn)
    if a is None:
        return float("nan")
    px = layer.px
    pcol = (1.0 - (e - te) / layer.tile_m) * (px - 1)
    prow = (n - tn) / layer.tile_m * (px - 1)
    c0 = max(0, min(px - 2, int(pcol)))
    r0 = max(0, min(px - 2, int(prow)))
    fc, fr = pcol - c0, prow - r0
    h00, h10, h01, h11 = a[r0, c0], a[r0, c0 + 1], a[r0 + 1, c0], a[r0 + 1, c0 + 1]
    if nodata_zero and (h00 == 0.0 or h10 == 0.0 or h01 == 0.0 or h11 == 0.0):
        return float("nan")
    if tri:
        if fc + fr <= 1.0:
            return float(h00 + (h10 - h00) * fc + (h01 - h00) * fr)
        return float(h11 + (h01 - h11) * (1.0 - fc) + (h10 - h11) * (1.0 - fr))
    return float(h00 * (1 - fc) * (1 - fr) + h10 * fc * (1 - fr) + h01 * (1 - fc) * fr + h11 * fc * fr)


class Overlay:
    """A finer terrain drawn over the region's base tiles (the installed
    Swiss 2 m scenery's terrain/: the same raw float32 tiles and manifest),
    read only. A pixel of 0.0 is "no data" (Scenery3D's rule) and the base
    tier shows through there, so the bake samples the overlays first and the
    ribbons sit on the terrain one sees; the terrain writes (corridors,
    plateaus) stay in the base tier."""

    def __init__(self, terrain_dir: Path):
        self.dir = terrain_dir
        m = json.load(open(terrain_dir / "manifest.json"))
        self.px = int(m["pixels_per_tile"])
        self.tile_m = int(m.get("tile_size_m", TILE_M))
        self.res_m = self.tile_m / (self.px - 1)
        self.tiles: dict = {}
        self.missing: set = set()

    def load(self, te: int, tn: int):
        k = (te, tn)
        if k in self.tiles:
            return self.tiles[k]
        if k in self.missing:
            return None
        p = self.dir / f"tile_{te}_{tn}.raw"
        if not p.exists():
            self.missing.add(k)
            return None
        a = np.fromfile(p, dtype="<f4").reshape(self.px, self.px)
        if not a.any():            # an empty tile: no data at all
            self.missing.add(k)
            return None
        self.tiles[k] = a
        return a

    def sample(self, e: float, n: float, tri: bool = False) -> float:
        return _sample_tile(self, e, n, tri, nodata_zero=True)


class Terrain:
    """The region's baked base tiles: pixel (0,0) = SE corner, columns run
    East→West, rows South→North (build_world_terrain.py / s3d_tile.cpp).
    `overlays` (finer terrains drawn over them) answer a sample first."""

    def __init__(self, terrain_dir: Path):
        self.dir = terrain_dir
        self.base = terrain_dir.parent / "terrain_base"   # the pristine tiles every bake starts from
        m = json.load(open(terrain_dir / "manifest.json"))
        self.px = int(m["pixels_per_tile"])
        self.tile_m = int(m.get("tile_size_m", TILE_M))
        self.res_m = self.tile_m / (self.px - 1)
        self.overlays: list = []
        self.tiles: dict = {}
        self.dirty: set = set()
        self.missing: set = set()
        self.acc: dict = {}       # (te, tn) -> (sum of weights, sum of weight x height) of the corridors over each pixel

    def path(self, te: int, tn: int) -> Path:
        return self.dir / f"tile_{te}_{tn}.raw"

    def load(self, te: int, tn: int):
        k = (te, tn)
        if k in self.tiles:
            return self.tiles[k]
        if k in self.missing:
            return None
        p = self.path(te, tn)
        b = self.base / p.name
        if b.exists():
            src = b
        elif p.exists():
            # no pristine copy yet: the tile as built is the base from now on
            self.base.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, b)
            src = p
        else:
            self.missing.add(k)
            return None
        a = np.fromfile(src, dtype="<f4").reshape(self.px, self.px).copy()
        self.tiles[k] = a
        return a

    def sample(self, e: float, n: float, tri: bool = False) -> float:
        """Terrain height at (e, n) (see _sample_tile): the first overlay
        with data there, else the base tile."""
        for ov in self.overlays:
            h = ov.sample(e, n, tri)
            if not math.isnan(h):
                return h
        return _sample_tile(self, e, n, tri)

    def res_at(self, e: float, n: float) -> float:
        """Posting (m) of the layer that answers at (e, n)."""
        for ov in self.overlays:
            if not math.isnan(ov.sample(e, n)):
                return ov.res_m
        return self.res_m

    def corridor_acc(self, te: int, tn: int):
        k = (te, tn)
        if k not in self.acc:
            self.acc[k] = (np.zeros((self.px, self.px)), np.zeros((self.px, self.px)))
        return self.acc[k]

    def resolve_corridors(self):
        """After every corridor: each pixel takes the weighted mean of the
        corridor heights over it (weights fall off with the distance from
        each ribbon edge, so the ground under a ribbon is its own and the
        step between two ways is spread instead of jagged)."""
        n = 0
        for k, (sw, swh) in self.acc.items():
            a = self.tiles.get(k)
            if a is None:
                continue
            m = sw > 0.0
            if m.any():
                a[m] = (swh[m] / sw[m]).astype(a.dtype)
                self.dirty.add(k)
                n += int(m.sum())
        self.acc.clear()
        return n

    def reset_box(self, e0, n0, e1, n1):
        """Every tile touching the box is loaded from terrain_base and marked
        for writing, so the bake's terrain is base + this design, whatever
        an earlier bake wrote there."""
        n = 0
        for te in range(int(math.floor(e0 / self.tile_m)) * self.tile_m, int(math.ceil(e1 / self.tile_m)) * self.tile_m, self.tile_m):
            for tn in range(int(math.floor(n0 / self.tile_m)) * self.tile_m, int(math.ceil(n1 / self.tile_m)) * self.tile_m, self.tile_m):
                if self.load(te, tn) is not None:
                    self.dirty.add((te, tn))
                    n += 1
        return n

    def reset_all(self):
        """reset_box for the whole region (a bake without --bbox)."""
        n = 0
        for f in self.dir.glob("tile_*.raw"):
            te, tn = (int(v) for v in f.stem.split("_")[1:3])
            if self.load(te, tn) is not None:
                self.dirty.add((te, tn))
                n += 1
        return n

    def pixel_en(self, te: int, tn: int, row: int, col: int):
        step = self.tile_m / (self.px - 1)
        return te + self.tile_m - col * step, tn + row * step

    def flush(self):
        n = 0
        for k in self.dirty:
            self.tiles[k].astype("<f4").tofile(self.path(*k))
            n += 1
        self.dirty.clear()
        return n


# ── OSM ─────────────────────────────────────────────────────────────────────
class Collector(osmium.SimpleHandler if osmium else object):
    def __init__(self, bbox_wgs):
        super().__init__()
        self.bbox = bbox_wgs   # (lon0, lat0, lon1, lat1) or None
        self.ways = []         # dicts: kind 'road'|'rail', cls, pts [(lon,lat)], tags subset
        self.n = 0

    def _in(self, lon, lat):
        b = self.bbox
        return b is None or (b[0] <= lon <= b[2] and b[1] <= lat <= b[3])

    def way(self, w):
        t = w.tags
        hwy = t.get("highway")
        rwy = t.get("railway")
        if hwy in ROAD_CLASSES:
            kind, cls = "road", hwy
        elif rwy in RAIL_CLASSES:
            kind, cls = "rail", rwy
            if t.get("service") in ("yard", "spur", "siding", "crossover"):
                return
            if t.get("railway:preserved") == "yes" or t.get("usage") in ("military", "test"):
                return
        else:
            return
        if hwy and t.get("area") == "yes":
            return
        try:
            nodes = [(nd.ref, nd.lon, nd.lat) for nd in w.nodes if nd.location.valid()]
        except osmium.InvalidLocationError:
            return
        if len(nodes) < 2:
            return
        pts = [(lo, la) for _r, lo, la in nodes]
        if not any(self._in(lo, la) for lo, la in pts[:: max(1, len(pts) // 8)]):
            return
        tunnel = t.get("tunnel") in ("yes", "true", "1", "building_passage", "covered") or t.get("covered") == "yes"
        self.n += 1
        self.ways.append({
            "id": w.id, "kind": kind, "cls": cls, "pts": pts, "refs": [r for r, _lo, _la in nodes],
            "bridge": t.get("bridge") not in (None, "no") and not tunnel, "tunnel": tunnel,
            "layer": t.get("layer", "0"), "lanes": t.get("lanes"), "width": t.get("width"),
            "gauge": t.get("gauge"), "tracks": t.get("tracks"), "oneway": t.get("oneway") == "yes",
        })


# ── swissTLM3D (swisstopo landscape model, Switzerland) ─────────────────────
# Layers tlm_strassen_strasse / tlm_oev_eisenbahn of the LV95 GeoPackage:
# 3D lines (a height on every vertex), structures as attributes, road
# classes with nominal widths. Ways get "z": the true profile, so the
# terrain smoothing, the bridge deck guessing and the tunnel dive of the OSM
# path are not used: at-grade ways get a corridor to their true height,
# bridges are elevated (no corridor, piers), tunnels run at their true axis.
TLM_GPKG_DEFAULT = "/Volumes/Data1/scenery_in_world/swisstlm3d/SWISSTLM3D_2026_LV95_LN02.gpkg"
TLM_REF_BASE = 10 ** 14
# objektart -> (class, lanes, nominal width m or None, oneway)
TLM_ROAD_CLASSES = {
    "Autobahn":       ("motorway", 2, None, True),
    "Autostrasse":    ("trunk", 1, None, None),        # oneway from richtungsgetrennt
    "10m Strasse":    ("primary", 2, 10.0, None),
    "8m Strasse":     ("secondary", 2, 8.0, None),
    "6m Strasse":     ("tertiary", 2, 6.0, None),
    "4m Strasse":     ("tertiary", 1, 4.0, None),
    "3m Strasse":     ("tertiary", 1, 3.0, None),
    "Ausfahrt":       ("motorway_link", 1, None, True),
    "Einfahrt":       ("motorway_link", 1, None, True),
    "Zufahrt":        ("tertiary_link", 1, 4.0, None),
    "Verbindung":     ("tertiary_link", 1, 5.0, None),
}
TLM_BRIDGE = {"Bruecke", "Gedeckte Bruecke", "Bruecke mit Treppe"}
TLM_TUNNEL = {"Tunnel"}
TLM_UNDERPASS = {"Unterfuehrung", "Unterfuehrung mit Treppe"}


def _tlm_bool(v):
    return str(v) == "Wahr"


def collect_tlm(gpkg: Path, bbox_lv95):
    """Ways (the OSM collector's record layout + "z") from swissTLM3D inside an LV95 box."""
    import pyogrio
    import hashlib
    e0, n0, e1, n1 = bbox_lv95
    ways = []
    counter = [0]

    def uid(uuid_str):
        return int(hashlib.md5(str(uuid_str).encode()).hexdigest()[:7], 16) + 1   # fits uint32, never 0

    def refs(n):
        out = []
        for _ in range(n):
            counter[0] += 1
            out.append(TLM_REF_BASE + counter[0])
        return out

    roads = pyogrio.read_dataframe(str(gpkg), layer="tlm_strassen_strasse", bbox=(e0, n0, e1, n1),
                                   columns=["uuid", "objektart", "kunstbaute", "befahrbarkeit", "richtungsgetrennt", "kreisel", "stufe"])
    for row in roads.itertuples(index=False):
        spec = TLM_ROAD_CLASSES.get(str(row.objektart))
        if spec is None or str(row.befahrbarkeit) == "Falsch":
            continue
        cls, lanes, width, oneway = spec
        if oneway is None:
            oneway = _tlm_bool(row.richtungsgetrennt) or _tlm_bool(row.kreisel)
        kb = str(row.kunstbaute)
        if kb in ("Treppe",):
            continue
        for geom in getattr(row.geometry, "geoms", [row.geometry]):
            coords = list(geom.coords)
            if len(coords) < 2:
                continue
            pts = [wgs.transform(c[0], c[1]) for c in coords]
            ways.append({
                "id": uid(row.uuid), "kind": "road", "cls": cls,
                "pts": pts, "refs": refs(len(pts)), "z": [float(c[2]) for c in coords], "has_z": True,
                "bridge": False, "elevated": kb in TLM_BRIDGE, "tunnel": kb in TLM_TUNNEL or kb in TLM_UNDERPASS, "underpass": kb in TLM_UNDERPASS,
                "layer": str(row.stufe), "lanes": str(lanes), "width": str(width) if width else None,
                "gauge": None, "tracks": None, "oneway": bool(oneway), "source": "tlm", "tlm": str(row.objektart),
            })
    n_roads = len(ways)
    rails = pyogrio.read_dataframe(str(gpkg), layer="tlm_oev_eisenbahn", bbox=(e0, n0, e1, n1),
                                   columns=["uuid", "objektart", "kunstbaute", "anzahl_spuren", "verkehrsmittel", "ausser_betrieb", "standseilbahn", "zahnradbahn", "auf_strasse"])
    for row in rails.itertuples(index=False):
        if _tlm_bool(row.ausser_betrieb) or _tlm_bool(row.standseilbahn):
            continue
        vm = str(row.verkehrsmittel)
        cls = "tram" if vm == "Tram" else ("light_rail" if vm == "Metro" else ("narrow_gauge" if str(row.objektart) == "Schmalspur" else "rail"))
        if _tlm_bool(row.auf_strasse):
            cls = "tram"   # rails in a street: no ballast
        gauge = 1000 if str(row.objektart) == "Schmalspur" else 1435
        tracks = int(row.anzahl_spuren) if str(row.anzahl_spuren) not in ("None", "nan", "") else 1
        kb = str(row.kunstbaute)
        for geom in getattr(row.geometry, "geoms", [row.geometry]):
            coords = list(geom.coords)
            if len(coords) < 2:
                continue
            pts = [wgs.transform(c[0], c[1]) for c in coords]
            ways.append({
                "id": uid(row.uuid), "kind": "rail", "cls": cls,
                "pts": pts, "refs": refs(len(pts)), "z": [float(c[2]) for c in coords], "has_z": True,
                "bridge": False, "elevated": kb in TLM_BRIDGE, "tunnel": kb in TLM_TUNNEL or kb == "in/auf Gebaeude", "underpass": False,
                "layer": "0", "lanes": None, "width": None, "gauge": str(gauge), "tracks": str(tracks),
                "oneway": False, "source": "tlm",
            })
    return ways, n_roads, len(ways) - n_roads


def densify_z(pts, zs, step=DENSIFY_M):
    """densify() for (e, n) points with a parallel height list."""
    out_p, out_z = [], []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        d = math.hypot(x1 - x0, y1 - y0)
        n = max(1, int(math.ceil(d / step)))
        for k in range(n):
            t = k / n
            out_p.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
            out_z.append(zs[i] + (zs[i + 1] - zs[i]) * t)
    out_p.append(pts[-1])
    out_z.append(zs[-1])
    return out_p, out_z


def parse_num(s, default):
    if not s:
        return default
    try:
        return float(str(s).split(";")[0].replace(",", ".").replace("m", "").strip())
    except ValueError:
        return default


# ── geometry ────────────────────────────────────────────────────────────────
def densify(pts, step=DENSIFY_M):
    out = [pts[0]]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        d = math.hypot(x1 - x0, y1 - y0)
        k = max(1, int(math.ceil(d / step)))
        for i in range(1, k + 1):
            f = i / k
            out.append((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f))
    return out


def along(pts):
    s = [0.0]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        s.append(s[-1] + math.hypot(x1 - x0, y1 - y0))
    return s


def smooth_profile(h, s, half_window):
    """Gaussian along-track smoothing of heights h at chainages s; NaNs are
    filled from neighbours first."""
    h = np.array(h, dtype=float)
    if np.isnan(h).any():
        ok = ~np.isnan(h)
        if ok.sum() == 0:
            return None
        h = np.interp(s, np.array(s)[ok], h[ok])
    if half_window <= 0 or len(h) < 3:
        return h
    s = np.asarray(s)
    sig = half_window / 2.0
    out = np.empty_like(h)
    for i in range(len(h)):
        w = np.exp(-0.5 * ((s - s[i]) / sig) ** 2)
        out[i] = float((w * h).sum() / w.sum())
    return out


def terrain_profile(pts, terrain, half_window):
    """(ground, profile): the raw terrain under the stations (NaNs filled from
    neighbours) and its Gaussian-smoothed version; None without terrain."""
    s = along(pts)
    h = np.array([terrain.sample(e, n) for e, n in pts], dtype=float)
    if np.isnan(h).all():
        return None
    if np.isnan(h).any():
        ok = ~np.isnan(h)
        h = np.interp(s, np.asarray(s)[ok], h[ok])
    return h, smooth_profile(h, s, half_window)


def _seg_cells(p, q, cell):
    c0, c1 = int(math.floor(min(p[0], q[0]) / cell)), int(math.floor(max(p[0], q[0]) / cell))
    r0, r1 = int(math.floor(min(p[1], q[1]) / cell)), int(math.floor(max(p[1], q[1]) / cell))
    return [(c, r) for c in range(c0, c1 + 1) for r in range(r0, r1 + 1)]


def _seg_intersect(p, p2, q, q2):
    """(t, u) of the crossing of segments p->p2 and q->q2, or None."""
    rx, ry = p2[0] - p[0], p2[1] - p[1]
    sx, sy = q2[0] - q[0], q2[1] - q[1]
    den = rx * sy - ry * sx
    if abs(den) < 1e-9:
        return None
    qx, qy = q[0] - p[0], q[1] - p[1]
    t = (qx * sy - qy * sx) / den
    u = (qx * ry - qy * rx) / den
    if 0.0 <= t <= 1.0 and 0.0 <= u <= 1.0:
        return t, u
    return None


def _chains(bridges):
    """Bridge ways joined end to end (a viaduct split into several OSM ways)
    grouped into components; each is returned as an ordered list of
    (way, flipped) when it is a simple chain, else as unordered members."""
    by_ref = defaultdict(list)
    for wy in bridges:
        for r in set(wy["end_refs"]):
            by_ref[r].append(wy)
    seen = set()
    out = []
    for wy in bridges:
        if id(wy) in seen:
            continue
        comp, stack = [], [wy]
        seen.add(id(wy))
        while stack:
            m = stack.pop()
            comp.append(m)
            for r in m["end_refs"]:
                for o in by_ref[r]:
                    if id(o) not in seen:
                        seen.add(id(o))
                        stack.append(o)
        deg = defaultdict(int)
        for m in comp:
            for r in set(m["end_refs"]):
                deg[r] += 1
        terms = [r for r, d in deg.items() if d == 1]
        ordered = None
        if len(terms) == 2 and all(d <= 2 for d in deg.values()) and all(m["end_refs"][0] != m["end_refs"][1] for m in comp):
            cur, used, ordered = terms[0], set(), []
            while True:
                nxt = [m for m in by_ref[cur] if id(m) not in used and id(m) in {id(c) for c in comp}]
                if not nxt:
                    break
                m = nxt[0]
                used.add(id(m))
                flip = m["end_refs"][1] == cur
                ordered.append((m, flip))
                cur = m["end_refs"][0] if flip else m["end_refs"][1]
            if len(ordered) != len(comp):
                ordered = None
        out.append(ordered if ordered else [(m, False) for m in comp])
    return out


def bridge_decks(ways, stats):
    """Deck profile of every bridge: chord between the chain's end grounds,
    lifted by one `rise` so it clears the ways it crosses (BRIDGE_CLEARANCE_M
    over their profiles) and the ground under it. Returns {end ref: rise}
    for the approach ramps."""
    bridges = [wy for wy in ways if wy["w"]["bridge"]]
    others = [wy for wy in ways if not wy["w"]["bridge"] and not wy["w"]["tunnel"]]
    cell = 50.0
    grid = defaultdict(list)
    for oi, wy in enumerate(others):
        P = wy["pts"]
        for i in range(len(P) - 1):
            for c in _seg_cells(P[i], P[i + 1], cell):
                grid[c].append((oi, i))
    ramps = {}
    for chain in _chains(bridges):
        # chain chord: chainage over the ordered members, ground at the two ends
        total = sum(m["s"][-1] for m, _f in chain)
        first, ff = chain[0]
        last, lf = chain[-1]
        h_a = first["ground"][-1] if ff else first["ground"][0]
        h_b = last["ground"][0] if lf else last["ground"][-1]
        chords = []
        off = 0.0
        for m, flip in chain:
            cs = np.array([(m["s"][-1] - si if flip else si) + off for si in m["s"]])
            chords.append(h_a + (h_b - h_a) * cs / max(total, 1.0))
            off += m["s"][-1]
        rise = 0.0
        refs_of_chain = set(r for m, _f in chain for r in m["end_refs"])
        for (m, _flip), chord in zip(chain, chords):
            rise = max(rise, float(np.max(m["ground"] + BRIDGE_GROUND_M - chord)))
            P = m["pts"]
            for i in range(len(P) - 1):
                cand = set()
                for c in _seg_cells(P[i], P[i + 1], cell):
                    cand.update(grid.get(c, ()))
                for oi, j in cand:
                    o = others[oi]
                    if refs_of_chain & set(o["end_refs"]):
                        continue   # an approach way touching the bridge end, not a crossing
                    hit = _seg_intersect(P[i], P[i + 1], o["pts"][j], o["pts"][j + 1])
                    if hit is None:
                        continue
                    t, u = hit
                    here = chord[i] + (chord[i + 1] - chord[i]) * t
                    under = o["prof"][j] + (o["prof"][j + 1] - o["prof"][j]) * u
                    rise = max(rise, under + BRIDGE_CLEARANCE_M - here)
                    stats["bridge_crossings"] += 1
        rise = max(0.0, rise)
        for (m, _flip), chord in zip(chain, chords):
            m["prof"] = chord + rise
            m["rise"] = rise
        if rise > BRIDGE_GROUND_M:
            stats["bridges_lifted"] += len(chain)
            for r in [r for r, d in ((r, sum(1 for m, _f in chain for rr in set(m["end_refs"]) if rr == r)) for r in refs_of_chain) if d == 1]:
                ramps[r] = max(ramps.get(r, 0.0), rise)
    return ramps


def ramp_approaches(ways, ramps, stats):
    """Raise the profile of every way that ends at a lifted deck so it climbs
    to the deck at RAMP_GRADE (the approach embankment)."""
    for wy in ways:
        if wy["w"]["bridge"] or wy["w"]["tunnel"]:
            continue
        for end, r in enumerate(wy["end_refs"]):
            rise = ramps.get(r, 0.0)
            if rise <= 0.0:
                continue
            L = max(RAMP_MIN_M, rise / RAMP_GRADE)
            s = wy["s"]
            d = np.array([si if end == 0 else s[-1] - si for si in s])
            wy["prof"] = wy["prof"] + rise * np.clip(1.0 - d / L, 0.0, 1.0)
            wy["ramp"] = True
            stats["ramps"] += 1


def tunnel_profile(wy, portal):
    """Below the surface: TUNNEL_DEPTH_M under the ground, diving over
    PORTAL_M behind each end that is a portal (joins a surface way); an end
    shared with another tunnel way stays deep, so a long tunnel split into
    several OSM ways never surfaces in between."""
    g, s = wy["ground"], np.asarray(wy["s"])
    L = max(s[-1], 1.0)
    f = np.ones_like(s)
    if portal[0]:
        f = np.minimum(f, s / PORTAL_M)
    if portal[1]:
        f = np.minimum(f, (L - s) / PORTAL_M)
    return g - TUNNEL_DEPTH_M * np.clip(f, 0.0, 1.0)


def ground_under(pts, prof, half_width, terrain):
    """Final ground under a ribbon: the highest terrain (after every corridor
    bake) across the ribbon at each station and halfway to the next one
    (a terrain vertex between two stations bulges above the straight
    ribbon), sampled on the runtime mesh's own triangles, never below the
    way's own profile. No smoothing: where a crossing way's corridor
    stepped the ground, the ribbon steps with it rather than sinking in."""
    n = len(pts)
    if n < 2:
        return prof
    frames = MeshBuf.frames([(e, n_) for e, n_ in pts])
    g = np.array(prof, dtype=float)
    F = (-1.0, -0.5, 0.0, 0.5, 1.0)

    def highest(e, n_, px, pz):
        hi = -math.inf
        for f in F:
            h = terrain.sample(e + px * half_width * f, n_ + pz * half_width * f, tri=True)
            if not math.isnan(h) and h > hi:
                hi = h
        return hi

    for i, (e, n_) in enumerate(pts):
        px, pz = frames[i]
        g[i] = max(g[i], highest(e, n_, px, pz))
        if i + 1 < n:
            e2, n2 = pts[i + 1]
            qx, qz = frames[i + 1]
            hm = highest((e + e2) * 0.5, (n_ + n2) * 0.5, (px + qx) * 0.5, (pz + qz) * 0.5)
            g[i] = max(g[i], hm)
            g[i + 1] = max(g[i + 1], hm)
    return g


# ── scenery editor designs (docs/SCENERY_EDITOR.md, subtask 7) ─────────────
# A design file (provpilot/scenery_design.gd) is a graph of nodes (LV95) and
# road / rail edges, some of them arcs. Its chains become ways like the OSM
# ones (same corridor, class and lift rules), its junction nodes get the
# same corner-fan pavement the live mesher draws, its lane changes taper,
# and OSM ways inside its `replace` polygons are cut away.
DESIGN_REF_BASE = 10 ** 15
KIND_JUNCTION = 6
KIND_CONCRETE = 7
AREA_KIND = {"grass": 10, "forest": 11, "field": 12, "water": 13}
KIND_ROOF, KIND_WALL = 14, 15
ELEVATED_M = 0.3
KIND_EMBANKMENT = 16
KIND_MARK = 17            # painted line (white), its own thin strip over the pavement
MARK_W_M = 0.15
MARK_LIFT_M = 0.03        # over the ribbon (a patch is PATCH_LIFT_M up, the lines stay above it)
MARK_U8 = (228, 228, 218)
DASH_M, DASH_PERIOD_M = 6.0, 12.0
MERGE_MAX_M = 300.0       # a ramp merging at a shallow angle: pavement union up to this far from the node
MERGE_TAPER_M = 80.0      # the gore fill between a merging ramp's inner edge and the carriageway edge
GORE_GAP_M = 5.0          # ... only while the two edges are this close (the painted nose, not a plaza)
LANE_TAPER_M = 40.0       # a merging lane's outer edge tapers into the carriageway edge over this length
# Junction blocks (design §13.9 "Autobahn first"): an entry or exit of a
# one-way carriageway is a standard shape stamped along the carriageway,
# not the union of the raw ribbons. Swiss norm defaults.
BLOCK_ACC_M = 200.0       # acceleration lane beyond the nose of an entry
BLOCK_DEC_M = 150.0       # deceleration lane before the nose of an exit
BLOCK_TAPER_M = 75.0      # 1:20 taper of the extra lane into the carriageway edge
BLOCK_JOIN_M = 60.0       # the ramp's axis is re-joined to the lane centre over its last / first metres
BLOCK_STEP_M = 5.0
ATTACH_EXTRA_M = 4.0      # a way end this close to another way's pavement edge joins it (swissTLM3D ramps end beside the Autobahn line)
ATTACH_DH_M = 1.5         # ... if at the same level
PATCH_LIFT_M = 0.01       # a junction patch above the ribbons it joins
SKIRT_MIN_M = 0.3         # a ribbon this far above the ground gets an embankment strip
SKIRT_RUN = 1.5           # embankment run per metre of height (1:1.5)
SKIRT_MAX_M = 15.0        # embankment run: a 10 m bank; anything taller stays a floating ribbon
EMBANKMENT_U8 = (115, 140, 89)
PIER_MIN_H_M = 1.0        # no pier under a deck closer than this to the ground
PIER_SPACING_M = 25.0
PIER_W_M = 1.2
PARAPET_H_M = 0.45
PARAPET_W_M = 0.3
CONCRETE_U8 = (158, 156, 148)
JUNCTION_MIN_CUT_M = 1.0
TAPER_PER_LANE_M = 30.0
ARC_STEP_M = 10.0
DESIGN_EDGE_ATTRS = ("kind", "cls", "lanes", "oneway", "gauge", "tracks", "width")
_cdt = None          # shapely.constrained_delaunay_triangles when available (set in main)
_ShPoint = None      # shapely Point (set in main)


def circle_through(a, v, b):
    d = 2.0 * (a[0] * (v[1] - b[1]) + v[0] * (b[1] - a[1]) + b[0] * (a[1] - v[1]))
    if abs(d) < 1e-6:
        return None
    a2, v2, b2 = a[0] ** 2 + a[1] ** 2, v[0] ** 2 + v[1] ** 2, b[0] ** 2 + b[1] ** 2
    ux = (a2 * (v[1] - b[1]) + v2 * (b[1] - a[1]) + b2 * (a[1] - v[1])) / d
    uy = (a2 * (b[0] - v[0]) + v2 * (a[0] - b[0]) + b2 * (v[0] - a[0])) / d
    return (ux, uy), math.hypot(a[0] - ux, a[1] - uy)


def arc_points(a, v, b):
    """Arc a -> v -> b sampled every ARC_STEP_M (at least 4 segments), as scenery_design.gd."""
    circ = circle_through(a, v, b)
    if circ is None or circ[1] > 50000.0:
        return [a, b]
    (cx, cy), r = circ
    ang_a, ang_v, ang_b = (math.atan2(a[1] - cy, a[0] - cx), math.atan2(v[1] - cy, v[0] - cx), math.atan2(b[1] - cy, b[0] - cx))
    sweep_ccw = (ang_b - ang_a) % (2 * math.pi)
    via_ccw = (ang_v - ang_a) % (2 * math.pi)
    sweep = sweep_ccw if via_ccw <= sweep_ccw else sweep_ccw - 2 * math.pi
    n = max(4, int(math.ceil(abs(sweep) * r / ARC_STEP_M)))
    out = [a]
    for i in range(1, n):
        ang = ang_a + sweep * i / n
        out.append((cx + math.cos(ang) * r, cy + math.sin(ang) * r))
    out.append(b)
    return out


def point_in_polygon(p, poly):
    x, y = p
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-12) + xi:
            inside = not inside
        j = i
    return inside


def trim_start_vals(pts, vals, metres):
    """trim_start on a polyline and a parallel value array."""
    out, ov, left = list(pts), list(vals), metres
    while len(out) >= 2 and left > 0.0:
        seg = math.dist(out[0], out[1])
        if seg <= left:
            left -= seg
            out.pop(0)
            ov.pop(0)
        else:
            t = left / seg
            out[0] = (out[0][0] + (out[1][0] - out[0][0]) * t, out[0][1] + (out[1][1] - out[0][1]) * t)
            ov[0] = ov[0] + (ov[1] - ov[0]) * t
            left = 0.0
    return out, ov


def bake_flatten(terrain, poly, h):
    """Every terrain pixel inside the polygon takes height h (a plateau)."""
    step = terrain.tile_m / (terrain.px - 1)
    es = [p[0] for p in poly]
    ns = [p[1] for p in poly]
    touched = 0
    for te in range(int(math.floor(min(es) / terrain.tile_m)) * terrain.tile_m, int(math.floor(max(es) / terrain.tile_m)) * terrain.tile_m + 1, terrain.tile_m):
        for tn in range(int(math.floor(min(ns) / terrain.tile_m)) * terrain.tile_m, int(math.floor(max(ns) / terrain.tile_m)) * terrain.tile_m + 1, terrain.tile_m):
            a = terrain.load(te, tn)
            if a is None:
                continue
            for row in range(terrain.px):
                for col in range(terrain.px):
                    e, n = terrain.pixel_en(te, tn, row, col)
                    if min(es) <= e <= max(es) and min(ns) <= n <= max(ns) and point_in_polygon((e, n), poly):
                        a[row, col] = h
                        touched += 1
            terrain.dirty.add((te, tn))
    return touched


def taper_profile(half, taper, total):
    """Half width by chainage of a way whose ends narrow to a neighbour:
    `taper` = (start, end) target half widths (None = the full width). A
    lane taper of TAPER_PER_LANE_M per 1.6 m of half width, ending at the
    node. Returns half_at(s), or None when nothing narrows."""
    t0 = taper[0] if taper[0] is not None and taper[0] < half - 0.01 else None
    t1 = taper[1] if taper[1] is not None and taper[1] < half - 0.01 else None
    if t0 is None and t1 is None:
        return None
    L0 = TAPER_PER_LANE_M * max(1.0, (half - t0) / 1.6) if t0 is not None else 1.0
    L1 = TAPER_PER_LANE_M * max(1.0, (half - t1) / 1.6) if t1 is not None else 1.0

    def half_at(sv):
        hw = half
        if t0 is not None:
            hw = min(hw, t0 + (half - t0) * min(1.0, sv / L0))
        if t1 is not None:
            hw = min(hw, t1 + (half - t1) * min(1.0, (total - sv) / L1))
        return hw
    return half_at


def trim_start(pts, metres):
    out = list(pts)
    left = metres
    while len(out) >= 2 and left > 0.0:
        seg = math.dist(out[0], out[1])
        if seg <= left:
            left -= seg
            out.pop(0)
        else:
            t = left / seg
            out[0] = (out[0][0] + (out[1][0] - out[0][0]) * t, out[0][1] + (out[1][1] - out[0][1]) * t)
            left = 0.0
    return out


class Design:
    """A scenery_design.json: ways (chains), junction pavements, replace polygons."""

    def __init__(self, path: Path):
        self.path = path
        raw = path.read_bytes()
        self.sha256 = hashlib.sha256(raw).hexdigest()
        d = json.loads(raw)
        self.nodes = {int(r[0]): (float(r[1]), float(r[2])) for r in d.get("nodes", [])}
        self.offsets = {int(r[0]): float(r[3]) for r in d.get("nodes", []) if len(r) > 3}   # height offset per node
        self.flatten = [([(float(p[0]), float(p[1])) for p in f.get("poly", [])], float(f.get("h", 0.0)))
                        for f in d.get("flatten", []) if len(f.get("poly", [])) >= 3]
        self.areas = [([(float(p[0]), float(p[1])) for p in a.get("poly", [])], str(a.get("type", "grass")), float(a.get("h", 0.0)))
                      for a in d.get("areas", []) if len(a.get("poly", [])) >= 3 and a.get("type") in list(AREA_KIND) + ["building"]]
        self.edges = {}
        for rec in d.get("edges", []):
            ed = {k: rec.get(k) for k in DESIGN_EDGE_ATTRS}
            ed["a"], ed["b"] = int(rec["a"]), int(rec["b"])
            ed["lanes"], ed["gauge"], ed["tracks"] = int(ed["lanes"] or 0), int(ed["gauge"] or 0), int(ed["tracks"] or 0)
            ed["width"] = float(ed["width"] or 0.0)
            ed["oneway"] = bool(ed["oneway"])
            if rec.get("via"):
                ed["via"] = (float(rec["via"][0]), float(rec["via"][1]))
            self.edges[int(rec["id"])] = ed
        self.replace = [[(float(p[0]), float(p[1])) for p in poly] for poly in d.get("replace", []) if len(poly) >= 3]
        self.replace_ids = set(int(i) for i in d.get("replace_ids", []))   # OSM ways the design replaces outright
        self.cuts = defaultdict(dict)      # edge id -> {node id: cut m}
        self.junctions = []                # {"node", "centre", "poly", "cls"}
        self._junctions()
        self.ways = self._ways()

    # -- graph helpers (mirrors scenery_design.gd)
    def edges_of(self, nid):
        return [eid for eid, ed in self.edges.items() if ed["a"] == nid or ed["b"] == nid]

    def edge_points(self, eid):
        ed = self.edges[eid]
        a, b = self.nodes[ed["a"]], self.nodes[ed["b"]]
        return arc_points(a, ed["via"], b) if "via" in ed else [a, b]

    def edge_width(self, eid):
        ed = self.edges[eid]
        if ed["kind"] != "road":
            return 0.0
        return road_geometry(ed["cls"], str(ed["lanes"]) if ed["lanes"] else None, str(ed["width"]) if ed["width"] else None, ed["oneway"])[0]

    def chains(self):
        used, out = set(), []
        for eid in sorted(self.edges):
            if eid in used:
                continue
            used.add(eid)
            ed = self.edges[eid]
            node_list, edge_list = [ed["a"], ed["b"]], [eid]
            for direction in (1, -1):
                while True:
                    end_node = node_list[-1] if direction == 1 else node_list[0]
                    adj = self.edges_of(end_node)
                    if len(adj) != 2:
                        break
                    nxt = next((c for c in adj if c not in used and all(self.edges[c][k] == ed[k] for k in DESIGN_EDGE_ATTRS)), None)
                    if nxt is None:
                        break
                    used.add(nxt)
                    ned = self.edges[nxt]
                    other = ned["b"] if ned["a"] == end_node else ned["a"]
                    closes = other in node_list
                    if direction == 1:
                        node_list.append(other)
                        edge_list.append(nxt)
                    else:
                        node_list.insert(0, other)
                        edge_list.insert(0, nxt)
                    if closes:
                        break
            out.append({"attrs": {k: ed[k] for k in DESIGN_EDGE_ATTRS}, "nodes": node_list, "edges": edge_list})
        return out

    def chain_polyline(self, chain):
        return self.chain_polyline_offsets(chain)[0]

    def chain_polyline_offsets(self, chain):
        """(points, height offsets): the node offsets interpolated along each edge by arc length."""
        out, offs = [], []
        for i, eid in enumerate(chain["edges"]):
            pts = self.edge_points(eid)
            if self.edges[eid]["a"] != chain["nodes"][i]:
                pts = pts[::-1]
            h0 = self.offsets.get(chain["nodes"][i], 0.0)
            h1 = self.offsets.get(chain["nodes"][i + 1], 0.0)
            total = sum(math.dist(pts[k - 1], pts[k]) for k in range(1, len(pts))) or 1.0
            run = 0.0
            for k, p in enumerate(pts):
                if k > 0:
                    run += math.dist(pts[k - 1], p)
                if i > 0 and k == 0:
                    continue
                out.append(p)
                offs.append(h0 + (h1 - h0) * run / total)
        return out, offs

    def _junctions(self):
        for nid, centre in self.nodes.items():
            legs = []
            for eid in self.edges_of(nid):
                ed = self.edges[eid]
                if ed["kind"] != "road":
                    continue
                pts = self.edge_points(eid)
                if ed["b"] == nid:
                    pts = pts[::-1]
                dx, dz = pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]
                l = math.hypot(dx, dz) or 1.0
                d = (dx / l, dz / l)
                legs.append({"edge": eid, "dir": d, "ang": math.atan2(d[1], d[0]), "half": self.edge_width(eid) * 0.5, "cls": ed["cls"], "cut": 0.0})
            if len(legs) < 3:
                continue
            poly, best_cls = junction_fan(centre, legs)
            for leg in legs:
                self.cuts[leg["edge"]][nid] = leg["cut"]
            self.junctions.append({"node": nid, "centre": centre, "poly": poly, "cls": best_cls})

    def _neighbour_half(self, chain, at_start):
        node_id = chain["nodes"][0] if at_start else chain["nodes"][-1]
        own = chain["edges"][0] if at_start else chain["edges"][-1]
        adj = self.edges_of(node_id)
        if len(adj) != 2:
            return None
        other = adj[0] if adj[1] == own else adj[1]
        if self.edges[other]["kind"] != "road" or self.edges[own]["kind"] != "road":
            return None
        wo, ww = self.edge_width(other), self.edge_width(own)
        if abs(wo - ww) < 0.05 or wo >= ww:
            return None
        return wo * 0.5

    def _ways(self):
        ways = []
        counter = [0]

        def ref(nid=None):
            if nid is not None:
                return DESIGN_REF_BASE + nid
            counter[0] += 1
            return DESIGN_REF_BASE + 10 ** 9 + counter[0]

        for ci, chain in enumerate(self.chains()):
            at = chain["attrs"]
            pts, offs = self.chain_polyline_offsets(chain)
            cut0 = self.cuts.get(chain["edges"][0], {}).get(chain["nodes"][0], 0.0)
            cut1 = self.cuts.get(chain["edges"][-1], {}).get(chain["nodes"][-1], 0.0)
            if cut0 > 0.0:
                pts, offs = trim_start_vals(pts, offs, cut0)
            if cut1 > 0.0:
                pts, offs = trim_start_vals(pts[::-1], offs[::-1], cut1)
                pts, offs = pts[::-1], offs[::-1]
            if len(pts) < 2 or math.dist(pts[0], pts[-1]) < 0.5:
                continue
            refs = [ref(chain["nodes"][0])] + [ref() for _ in pts[1:-1]] + [ref(chain["nodes"][-1])]
            if chain["nodes"][0] == chain["nodes"][-1]:
                refs[-1] = ref()   # a ring: distinct end refs, or the bridge chain walk loops
            # split into runs by height class: a station more than 0.3 m up is
            # *elevated* (a deck / embankment the corridor must not follow),
            # the rest sits on the ground (a negative offset is a cut it digs)
            runs, start = [], 0
            for i in range(1, len(pts)):
                if (offs[i] > 0.3) != (offs[start] > 0.3):
                    runs.append((start, i))
                    start = i
            runs.append((start, len(pts) - 1))
            for pi, (i0, i1) in enumerate(runs):
                if i1 - i0 < 1:
                    continue
                p_run, o_run, r_run = pts[i0:i1 + 1], offs[i0:i1 + 1], refs[i0:i1 + 1]
                taper = (self._neighbour_half(chain, True) if i0 == 0 else None,
                         self._neighbour_half(chain, False) if i1 == len(pts) - 1 else None)
                ways.append({
                    "id": DESIGN_REF_BASE + ci * 64 + pi, "kind": at["kind"], "cls": at["cls"],
                    "pts": [wgs.transform(e, n) for e, n in p_run], "refs": r_run,
                    "bridge": False, "tunnel": False, "layer": "0",
                    "lanes": str(at["lanes"]) if at["lanes"] else None, "width": str(at["width"]) if at.get("width") else None,
                    "gauge": str(at["gauge"]) if at["gauge"] else None, "tracks": str(at["tracks"]) if at["tracks"] else None,
                    "oneway": bool(at["oneway"]), "design": True, "taper": taper,
                    "offsets": (along(p_run), o_run) if any(abs(o) > 0.005 for o in o_run) else None,
                    "elevated": max(o_run) > 0.3,
                })
        return ways


def junction_fan(centre, legs):
    """The pavement of a node where three or more road legs meet: legs are
    dicts with "dir" (unit vector away from the node, (e, n)), "half" and
    "cls"; each gets its "cut" (metres the ribbon is trimmed back) and the
    fan polygon (corner, edge, edge, corner, ... around the node) and the
    class of the widest-lifted leg are returned."""
    for L in legs:
        L["ang"] = math.atan2(L["dir"][1], L["dir"][0])
        L["cut"] = 0.0
    legs.sort(key=lambda L: L["ang"])
    k = len(legs)
    corners = []
    for i in range(k):
        a, b = legs[i], legs[(i + 1) % k]
        da, db = a["dir"], b["dir"]
        na, nb = (-da[1], da[0]), (-db[1], db[0])
        pa = (centre[0] + na[0] * a["half"], centre[1] + na[1] * a["half"])
        pb = (centre[0] - nb[0] * b["half"], centre[1] - nb[1] * b["half"])
        den = da[0] * db[1] - da[1] * db[0]
        if abs(den) < 0.05:
            # parallel legs: push the corner out by the half width; opposite
            # legs (a through road at a T): the corner is the edge midpoint
            sx, sy = da[0] + db[0], da[1] + db[1]
            m = max(a["half"], b["half"]) * 0.5
            corner = ((pa[0] + pb[0]) / 2 + sx * m, (pa[1] + pb[1]) / 2 + sy * m)
        else:
            qx, qy = pb[0] - pa[0], pb[1] - pa[1]
            t = (qx * db[1] - qy * db[0]) / den
            corner = (pa[0] + da[0] * t, pa[1] + da[1] * t)
            if (corner[0] - centre[0]) * da[0] + (corner[1] - centre[1]) * da[1] < 0 and (corner[0] - centre[0]) * db[0] + (corner[1] - centre[1]) * db[1] < 0:
                corner = ((pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2)
        corners.append(corner)
        a["cut"] = max(a["cut"], (corner[0] - centre[0]) * da[0] + (corner[1] - centre[1]) * da[1])
        b["cut"] = max(b["cut"], (corner[0] - centre[0]) * db[0] + (corner[1] - centre[1]) * db[1])
    poly = []
    best_cls, best_drop = legs[0]["cls"], 1.0
    for i, leg in enumerate(legs):
        leg["cut"] = max(leg["cut"], JUNCTION_MIN_CUT_M)
        d = leg["dir"]
        n = (-d[1], d[0])
        base = (centre[0] + d[0] * leg["cut"], centre[1] + d[1] * leg["cut"])
        poly.append((base[0] - n[0] * leg["half"], base[1] - n[1] * leg["half"]))
        poly.append((base[0] + n[0] * leg["half"], base[1] + n[1] * leg["half"]))
        poly.append(corners[i])
        drop = CLASS_DROP_M.get(leg["cls"], 0.0)
        if drop < best_drop:
            best_drop, best_cls = drop, leg["cls"]
    return poly, best_cls


def clip_way(pts, refs, polys):
    """Pieces of a way outside every replace polygon: [(pts, refs), ...]."""
    if not polys:
        return [(pts, refs)]
    keep = [not any(point_in_polygon(p, poly) for poly in polys) for p in pts]
    pieces, cur_p, cur_r = [], [], []
    for p, r, k in zip(pts, refs, keep):
        if k:
            cur_p.append(p)
            cur_r.append(r)
        else:
            if len(cur_p) >= 2:
                pieces.append((cur_p, cur_r))
            cur_p, cur_r = [], []
    if len(cur_p) >= 2:
        pieces.append((cur_p, cur_r))
    return pieces


def triangulate(poly):
    """Ear clipping of a simple polygon (any winding): triangle index triples."""
    n = len(poly)
    if n < 3:
        return []
    area2 = sum(poly[i][0] * poly[(i + 1) % n][1] - poly[(i + 1) % n][0] * poly[i][1] for i in range(n))
    idx = list(range(n)) if area2 > 0 else list(range(n))[::-1]

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    def inside(p, a, b, c):
        return cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0

    tris = []
    guard = 0
    while len(idx) > 3 and guard < 10 * n:
        guard += 1
        found = False
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if cross(a, b, c) <= 0:
                continue
            if any(inside(poly[j], a, b, c) for j in idx if j not in (i0, i1, i2)):
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            found = True
            break
        if not found:
            break
    if len(idx) == 3:
        tris.append(tuple(idx))
    return tris


class MeshBuf:
    def __init__(self):
        self.v, self.n, self.c, self.uv, self.uv2, self.idx = [], [], [], [], [], []
        self.w = []              # per vertex: the OSM way id (0 for design geometry): _WAYID
        self.current_way = 0     # set by the emitter before adding a way's vertices

    def _tag(self, count):
        self.w.extend([self.current_way] * count)

    def _tri(self, i0, i1, i2):
        """Append a triangle wound like the ribbons (clockwise in x, z seen
        from +y): the material is double-sided and lights a back face as if
        from below, which made the patches look like another material."""
        (x0, _y0, z0), (x1, _y1, z1), (x2, _y2, z2) = self.v[i0], self.v[i1], self.v[i2]
        if (x1 - x0) * (z2 - z0) - (z1 - z0) * (x2 - x0) > 0.0:
            i1, i2 = i2, i1
        self.idx.extend([i0, i1, i2])

    @staticmethod
    def frames(pts_xz):
        """Per-station unit normal (nx, nz, left of travel) with the mitre scale."""
        n = len(pts_xz)
        out = []
        for i in range(n):
            if i == 0:
                dx, dz = pts_xz[1][0] - pts_xz[0][0], pts_xz[1][1] - pts_xz[0][1]
                dx1, dz1 = dx, dz
            elif i == n - 1:
                dx, dz = pts_xz[-1][0] - pts_xz[-2][0], pts_xz[-1][1] - pts_xz[-2][1]
                dx1, dz1 = dx, dz
            else:
                dx, dz = pts_xz[i][0] - pts_xz[i - 1][0], pts_xz[i][1] - pts_xz[i - 1][1]
                dx1, dz1 = pts_xz[i + 1][0] - pts_xz[i][0], pts_xz[i + 1][1] - pts_xz[i][1]
            l0 = math.hypot(dx, dz) or 1.0
            l1 = math.hypot(dx1, dz1) or 1.0
            tx0, tz0, tx1, tz1 = dx / l0, dz / l0, dx1 / l1, dz1 / l1
            nx0, nz0, nx1, nz1 = -tz0, tx0, -tz1, tx1
            den = 1.0 + tx0 * tx1 + tz0 * tz1
            if den < 1e-4:
                px, pz = nx0, nz0
            else:
                px, pz = (nx0 + nx1) / den, (nz0 + nz1) / den
                l = math.hypot(px, pz)
                if l > MITER_LIMIT:
                    px, pz = px * MITER_LIMIT / l, pz * MITER_LIMIT / l
            out.append((px, pz))
        return out

    def ribbon(self, pts_xz, ys, width, colour_u8, kind, u0=0.0, along_s=None, count=0.0, halves=None, end_frames=None):
        """Flat ribbon of `width` centred on pts_xz shifted sideways by u0 (m);
        UV u = across (m, signed, + = right of travel); v = along (m); UV2 =
        (half width, count: lanes of a road, tracks of a ballast bed).
        `halves`: per-station half widths (a lane taper); `end_frames`: the
        (start, end) frame normals of a mitred join, or None."""
        half = width * 0.5
        self.profile(pts_xz, ys, ((-half, 0.0), (half, 0.0)), colour_u8, kind, u0, along_s, half, count, halves, end_frames)

    def quad3(self, a, b, width, colour_u8, kind, s0=0.0):
        """A flat quad of `width` from a to b ((x, y, z) each): a painted line piece."""
        dx, dz = b[0] - a[0], b[2] - a[2]
        l = math.hypot(dx, dz) or 1.0
        nx, nz = -dz / l * width * 0.5, dx / l * width * 0.5
        base = len(self.v)
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        for (x, y, z, u, sv) in ((a[0] + nx, a[1], a[2] + nz, -width / 2, s0), (a[0] - nx, a[1], a[2] - nz, width / 2, s0),
                                 (b[0] + nx, b[1], b[2] + nz, -width / 2, s0 + l), (b[0] - nx, b[1], b[2] - nz, width / 2, s0 + l)):
            self.v.append((x, y, z))
            self.n.append((0.0, 1.0, 0.0))
            self.c.append(cu)
            self.uv.append((u, sv))
            self.uv2.append((width / 2, 0.0))
        self._tag(4)
        self._tri(base, base + 2, base + 1)
        self._tri(base + 1, base + 2, base + 3)

    def tri_mesh(self, tris, colour_u8, kind):
        """Flat triangles ((x, y, z) x 3), normals up, UV = metres from the
        first vertex (small numbers: the shader's noise hash loses precision
        at tens of kilometres)."""
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        ox, oz = (tris[0][0][0], tris[0][0][2]) if tris else (0.0, 0.0)
        for t in tris:
            base = len(self.v)
            for (x, y, z) in t:
                self.v.append((x, y, z))
                self.n.append((0.0, 1.0, 0.0))
                self.c.append(cu)
                self.uv.append((x - ox, z - oz))
                self.uv2.append((0.0, 0.0))
            self._tag(3)
            self._tri(base, base + 1, base + 2)

    def strip(self, left, right, colour_u8, kind, along_s):
        """Two vertex rows (x, z, y) left and right of travel, one per
        station: an embankment from a ribbon edge down to the ground.
        UV = (metres from the left row, along)."""
        n = len(left)
        if n < 2:
            return
        base = len(self.v)
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        self._tag(2 * n)
        for i in range(n):
            lx, lz, ly = left[i]
            rx, rz, ry = right[i]
            # normals straight up like the (mostly flat) terrain's, so the
            # strip takes the terrain's shade and only its outline shows
            wdt = math.hypot(rx - lx, rz - lz)
            for (x, z, y, u) in ((lx, lz, ly, 0.0), (rx, rz, ry, wdt)):
                self.v.append((x, y, z))
                self.n.append((0.0, 1.0, 0.0))
                self.c.append(cu)
                self.uv.append((u, along_s[i]))
                self.uv2.append((0.0, 0.0))
        for i in range(n - 1):
            a = base + i * 2
            self._tri(a, a + 2, a + 1)
            self._tri(a + 1, a + 2, a + 3)

    def polygon(self, poly_xz, y, colour_u8, kind):
        """A flat triangulated polygon (land cover, water, roof); UV = metres."""
        tris = triangulate(poly_xz)
        base = len(self.v)
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        ox, oz = poly_xz[0]
        for (x, z) in poly_xz:
            self.v.append((x, y, z))
            self.n.append((0.0, 1.0, 0.0))
            self.c.append(cu)
            self.uv.append((x - ox, z - oz))
            self.uv2.append((0.0, 0.0))
        self._tag(len(poly_xz))
        for a, b, c in tris:
            self._tri(base + a, base + b, base + c)

    def wall(self, a_xz, b_xz, y0, y1, colour_u8, kind):
        """A vertical quad from a to b between heights y0 and y1; UV = (along, height)."""
        base = len(self.v)
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        dx, dz = b_xz[0] - a_xz[0], b_xz[1] - a_xz[1]
        l = math.hypot(dx, dz) or 1.0
        nrm = (dz / l, 0.0, -dx / l)
        for (p, y, u) in ((a_xz, y0, 0.0), (b_xz, y0, l), (b_xz, y1, l), (a_xz, y1, 0.0)):
            self.v.append((p[0], y, p[1]))
            self.n.append(nrm)
            self.c.append(cu)
            self.uv.append((u, y - y0))
            self.uv2.append((0.0, 0.0))
        self._tag(4)
        self.idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    def fan(self, centre_xz, poly_xz, y, colour_u8, kind):
        """A flat polygon fanned from its centre (a junction pavement); UV = metres from the centre."""
        base = len(self.v)
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        for (x, z) in [centre_xz] + list(poly_xz):
            self.v.append((x, y, z))
            self.n.append((0.0, 1.0, 0.0))
            self.c.append(cu)
            self.uv.append((x - centre_xz[0], z - centre_xz[1]))
            self.uv2.append((0.0, 0.0))
        self._tag(1 + len(poly_xz))
        n = len(poly_xz)
        for i in range(n):
            self._tri(base, base + 1 + i, base + 1 + (i + 1) % n)

    def profile(self, pts_xz, ys, section, colour_u8, kind, u0=0.0, along_s=None, half_w=0.0, count=0.0, halves=None, end_frames=None):
        """Extrude a cross-section (list of (u m, height m), left to right)
        along pts_xz: one vertex strip per section point, normals from the
        section slope. Used for the rails; a flat two-point section is a ribbon.
        `end_frames` replaces the first / last frame normal (a mitred join
        with the neighbouring way)."""
        n = len(pts_xz)
        if n < 2:
            return
        base = len(self.v)
        cu = (colour_u8[0], colour_u8[1], colour_u8[2], kind)
        s = along_s or along(pts_xz)
        frames = self.frames(pts_xz)
        if end_frames:
            if end_frames[0]:
                frames[0] = end_frames[0]
            if end_frames[1]:
                frames[-1] = end_frames[1]
        m = len(section)
        self._tag(n * m)
        # section normals: perpendicular to the segment to the next point, averaged
        sn = []
        for j in range(m):
            j0, j1 = max(0, j - 1), min(m - 1, j + 1)
            du, dh = section[j1][0] - section[j0][0], section[j1][1] - section[j0][1]
            l = math.hypot(du, dh) or 1.0
            sn.append((-dh / l, du / l))          # (across, up) unit normal
        scaled = halves is not None and len(halves) == n and half_w > 0.0
        for i in range(n):
            px, pz = frames[i]                     # unit-ish normal, + = left of travel
            x, z = pts_xz[i]
            y = float(ys[i])
            hw = halves[i] if scaled else half_w
            k = hw / half_w if scaled else 1.0
            for j, (u, h) in enumerate(section):
                u = u * k
                off = -(u0 + u)                    # + u = right of travel = -normal
                self.v.append((x + px * off, y + h, z + pz * off))
                nu, nh = sn[j]
                self.n.append((-px * nu, nh, -pz * nu))
                self.c.append(cu)
                self.uv.append((u, s[i]))
                self.uv2.append((hw, count))
        for i in range(n - 1):
            a = base + i * m
            for j in range(m - 1):
                self.idx.extend([a + j, a + m + j, a + j + 1, a + j + 1, a + m + j, a + m + j + 1])


def write_glb(path: Path, buf: MeshBuf):
    nv = len(buf.v)
    pos = np.array(buf.v, dtype="<f4")
    nrm = np.array(buf.n, dtype="<f4")
    col = np.array(buf.c, dtype="u1")
    uv = np.array(buf.uv, dtype="<f4")
    uv2 = np.array(buf.uv2, dtype="<f4")
    use32 = nv > 65535
    idx = np.array(buf.idx, dtype="<u4" if use32 else "<u2")
    way = np.array(buf.w if len(buf.w) == nv else [0] * nv, dtype="<u4")   # _WAYID per vertex
    blobs = [pos.tobytes(), nrm.tobytes(), col.tobytes(), uv.tobytes(), uv2.tobytes(), idx.tobytes(), way.tobytes()]
    views, accs, off = [], [], 0
    for i, b in enumerate(blobs):
        pad = (4 - len(b) % 4) % 4
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(b)})
        off += len(b) + pad
        blobs[i] = b + b"\0" * pad
    accs = [
        {"bufferView": 0, "componentType": 5126, "count": nv, "type": "VEC3",
         "min": pos.min(axis=0).tolist(), "max": pos.max(axis=0).tolist()},
        {"bufferView": 1, "componentType": 5126, "count": nv, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5121, "count": nv, "type": "VEC4", "normalized": True},
        {"bufferView": 3, "componentType": 5126, "count": nv, "type": "VEC2"},
        {"bufferView": 4, "componentType": 5126, "count": nv, "type": "VEC2"},
        {"bufferView": 5, "componentType": 5125 if use32 else 5123, "count": len(idx), "type": "SCALAR"},
        {"bufferView": 6, "componentType": 5125, "count": nv, "type": "SCALAR"},
    ]
    gltf = {"asset": {"version": "2.0", "generator": "build_world_roads.py"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2, "TEXCOORD_0": 3, "TEXCOORD_1": 4, "_WAYID": 6},
                                         "indices": 5, "mode": 4}]}],
            "buffers": [{"byteLength": off}], "bufferViews": views, "accessors": accs}
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    binb = b"".join(blobs)
    total = 12 + 8 + len(js) + 8 + len(binb)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(binb), 0x004E4942)); f.write(binb)


# ── airport protection ──────────────────────────────────────────────────────
# Runway plateaus, aprons and taxiways (runway_plateau.py, design §12 D18)
# are never touched by a road corridor: a corridor's 45 m margin next to a
# runway would cut its plateau and the aircraft placed on the runway plane
# drops or bounces (gery, LSMD, 2026-09-23).
RUNWAY_PROTECT_MARGIN_M = 60.0
TAXI_PROTECT_MARGIN_M = 25.0


def airport_protection(region_dir: Path):
    """{"polys": [[(e,n),...]], "corridors": [{"st": [(e,n),...], "reach": m}]} from airports/runways.json."""
    rj = region_dir / "airports" / "runways.json"
    out = {"polys": [], "corridors": []}
    if not rj.exists():
        return out
    data = json.load(open(rj))
    for ap in data.get("airports", []):
        for rw in ap.get("runways", []):
            ends = rw.get("ends", [])
            if len(ends) != 2 or "pavement" not in ends[0] or "pavement" not in ends[1]:
                continue
            e0, n0 = lv95.transform(ends[0]["pavement"][1], ends[0]["pavement"][0])
            e1, n1 = lv95.transform(ends[1]["pavement"][1], ends[1]["pavement"][0])
            half = 0.5 * float(rw.get("width_m", 45.0)) + RUNWAY_PROTECT_MARGIN_M
            out["corridors"].append({"st": [(e0, n0), (e1, n1)], "reach": half})
        for apn in ap.get("aprons", []):
            poly = [lv95.transform(float(p[1]), float(p[0])) for p in apn.get("polygon", []) if p and p[0] is not None]
            if len(poly) >= 3:
                out["polys"].append(poly)
        for tw in ap.get("taxiways", []):
            path = [lv95.transform(float(p[1]), float(p[0])) for p in tw.get("path", [])]
            if len(path) >= 2:
                out["corridors"].append({"st": path, "reach": 0.5 * float(tw.get("width_m", 23.0)) + TAXI_PROTECT_MARGIN_M})
    return out


def protection_mask(protect, E, N):
    """True for the pixels (arrays E, N) inside a protected shape."""
    mask = np.zeros(E.shape, dtype=bool)
    emin, emax, nmin, nmax = E.min(), E.max(), N.min(), N.max()
    for c in protect["corridors"]:
        st = c["st"]
        xs = [p[0] for p in st]; ys = [p[1] for p in st]
        r = c["reach"]
        if max(xs) + r < emin or min(xs) - r > emax or max(ys) + r < nmin or min(ys) - r > nmax:
            continue
        best = np.full(E.shape, np.inf)
        for i in range(len(st) - 1):
            x0, y0 = st[i]; x1, y1 = st[i + 1]
            dx, dy = x1 - x0, y1 - y0
            L2 = dx * dx + dy * dy
            if L2 < 1e-9:
                continue
            t = np.clip(((E - x0) * dx + (N - y0) * dy) / L2, 0.0, 1.0)
            d = np.hypot(E - (x0 + t * dx), N - (y0 + t * dy))
            best = np.minimum(best, d)
        mask |= best <= r
    for poly in protect["polys"]:
        xs = [p[0] for p in poly]; ys = [p[1] for p in poly]
        if max(xs) < emin or min(xs) > emax or max(ys) < nmin or min(ys) > nmax:
            continue
        inside = np.zeros(E.shape, dtype=bool)
        j = len(poly) - 1
        for i in range(len(poly)):
            xi, yi = poly[i]; xj, yj = poly[j]
            cond = ((yi > N) != (yj > N)) & (E < (xj - xi) * (N - yi) / ((yj - yi) or 1e-12) + xi)
            inside ^= cond
            j = i
        mask |= inside
    return mask


# ── corridor bake ───────────────────────────────────────────────────────────
def emit_skirts(buf, xz, tops, s, half, lift, terrain, end_frames=(None, None), halves=None):
    """Embankment strips where a ribbon rides above the ground: from just
    under each ribbon edge down to the terrain at a 1:SKIRT_RUN slope, on
    the runs of stations that are more than SKIRT_MIN_M up (padded by one
    station each side so the strip tapers out). `tops` are the ground-level
    profile heights, `lift` the ribbon's baked lift, `halves` the per-station
    half widths of a tapered ribbon. Returns metres built."""
    n = len(xz)
    if n < 2:
        return 0.0
    frames = MeshBuf.frames(xz)
    if end_frames[0]:
        frames[0] = end_frames[0]
    if end_frames[1]:
        frames[-1] = end_frames[1]
    total = 0.0
    for side in (-1.0, 1.0):          # + = right of travel = -frame normal
        inner, outer, need = [], [], []
        for i in range(n):
            px, pz = frames[i]
            x, z = xz[i]
            hw = halves[i] if halves else half
            ix, iz = x - px * side * hw, z - pz * side * hw
            g = terrain.sample(CONV_E - ix, iz + CONV_N, tri=True)
            top = float(tops[i])
            gap = (top - g) if not math.isnan(g) else 0.0
            l = math.hypot(px, pz) or 1.0
            ux, uz = -px / l * side, -pz / l * side       # outward, unit
            # walk outward down the slope line until the ground meets it (or
            # the ground is above the road: a cut, the strip ends there)
            d = 0.3
            y_out = min(g, top - 0.05) if not math.isnan(g) else top - 0.05
            if gap > SKIRT_MIN_M:
                step_m = 0.5
                k = step_m
                prev_k, prev_gap = 0.0, gap                      # gap between the slope line and the ground
                d_max = min(SKIRT_MAX_M, gap * SKIRT_RUN)
                while k <= d_max + step_m:
                    y_line = top - k / SKIRT_RUN
                    gk = terrain.sample(CONV_E - (ix + ux * k), (iz + uz * k) + CONV_N, tri=True)
                    if math.isnan(gk):
                        d, y_out = k, y_line
                        break
                    cur_gap = y_line - gk
                    if cur_gap <= 0.0:
                        # the line meets the ground between prev_k and k: interpolate
                        f = prev_gap / (prev_gap - cur_gap) if prev_gap > cur_gap else 1.0
                        d = prev_k + (k - prev_k) * f
                        y_out = top - d / SKIRT_RUN
                        break
                    d, y_out = k, y_line
                    prev_k, prev_gap = k, cur_gap
                    k += step_m
            ox, oz = ix + ux * d, iz + uz * d
            inner.append((ix, iz, top + lift - 0.02))
            outer.append((ox, oz, y_out + RUNTIME_DROP_M + 0.08))
            need.append(gap > SKIRT_MIN_M)
        i = 0
        while i < n:
            if not need[i]:
                i += 1
                continue
            j = i
            while j < n and need[j]:
                j += 1
            a, b = max(0, i - 1), min(n - 1, j)
            if b - a >= 1:
                if side < 0:
                    buf.strip(outer[a:b + 1], inner[a:b + 1], EMBANKMENT_U8, KIND_EMBANKMENT, s[a:b + 1])
                else:
                    buf.strip(inner[a:b + 1], outer[a:b + 1], EMBANKMENT_U8, KIND_EMBANKMENT, s[a:b + 1])
                total += s[b] - s[a]
            i = j + 1
    return total


def ribbon_quads(pts, half):
    """The quads (4 (e, n) corners each) of a ribbon of half width `half`
    along pts, with the same mitred frames the mesh uses."""
    if len(pts) < 2:
        return []
    fr = MeshBuf.frames(list(pts))
    out = []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        (a0, b0), (a1, b1) = fr[i], fr[i + 1]
        out.append(((x0 + a0 * half, y0 + b0 * half), (x0 - a0 * half, y0 - b0 * half),
                    (x1 - a1 * half, y1 - b1 * half), (x1 + a1 * half, y1 + b1 * half)))
    return out


def marking_lines(kind, half, lanes):
    """(offset u from the axis, dashed) of every painted line of a pavement
    kind; + u = right of travel."""
    if kind == KIND_ONEWAY:
        left, right = -half + VERGE_M, half - VERGE_M - SHOULDER_M
        lines = [(left, False), (right, False)]
        lane_w = (right - left) / max(1, lanes)
        lines += [(left + lane_w * k, True) for k in range(1, max(1, lanes))]
        return lines
    if kind == KIND_TWOWAY_LINED:
        return [(-half + VERGE_M, False), (half - VERGE_M, False), (0.0, True)]
    if kind == KIND_TWOWAY:
        return [(0.0, True)]
    if kind == KIND_LINK:
        return [(-half + VERGE_M, False), (half - VERGE_M, False)]
    if kind == KIND_DUAL:
        return [(-half + VERGE_M, False), (half - VERGE_M, False)]
    return []


def emit_markings(buf, xz, ys, s, half, kind, lanes, end_frames, cutters, halves=None):
    """Painted lines of a ribbon as thin strips MARK_LIFT_M over it: edge
    lines piecewise between stations, lane / centre lines as 6 m dashes.
    A piece whose midpoint lies inside one of the `cutters` (the other
    legs' pavement at the way's nodes, LV95 polygons, prepared) is left
    out: at a merge the ramp's inner edge line stops where it enters the
    carriageway and the carriageway's edge line opens for the ramp."""
    n = len(xz)
    if n < 2:
        return 0
    frames = MeshBuf.frames(xz)
    if end_frames:
        if end_frames[0]:
            frames[0] = end_frames[0]
        if end_frames[1]:
            frames[-1] = end_frames[1]
    total = s[-1]

    def hidden(x, z, u, dashed):
        """0 = drawn, 1 = dropped, 2 = drawn as dashes. A cutter without a
        side (the carriageway under a ramp's end) drops every line inside
        it; a cutter with a side (the ramp lane beside a through carriageway)
        turns the edge line on its side into a dashed line, the way an
        acceleration lane is separated from the through lanes."""
        if not cutters:
            return 0
        e, n_ = CONV_E - x, z + CONV_N
        pt = _ShPoint(e, n_)
        out = 0
        for c, side, mode in cutters:
            if side is not None and (dashed or (u > 0) != (side > 0)):
                continue
            if c.contains(pt):
                if mode == "drop":
                    return 1
                out = 2
        return out

    count = 0
    for u, dashed in marking_lines(kind, half, lanes):
        pts3 = []
        for i in range(n):
            px, pz = frames[i]
            k = (halves[i] / half) if (halves and half > 0) else 1.0
            off = -u * k
            pts3.append((xz[i][0] + px * off, ys[i] + MARK_LIFT_M, xz[i][1] + pz * off))
        xs = [q[0] for q in pts3]
        ysl = [q[1] for q in pts3]
        zs = [q[2] for q in pts3]

        def at(sv):
            return (float(np.interp(sv, s, xs)), float(np.interp(sv, s, ysl)), float(np.interp(sv, s, zs)))

        def dashes(sa, sb, check):
            """6 m dashes every 12 m between chainages sa and sb (pattern anchored at 0)."""
            c = 0
            k0 = int(sa // DASH_PERIOD_M)
            while k0 * DASH_PERIOD_M < sb:
                s0 = max(sa, k0 * DASH_PERIOD_M)
                s1 = min(sb, k0 * DASH_PERIOD_M + DASH_M)
                k0 += 1
                if s1 - s0 < 1.0:
                    continue
                a, b = at(s0), at(s1)
                if check and hidden((a[0] + b[0]) * 0.5, (a[2] + b[2]) * 0.5, u, True) == 1:
                    continue
                buf.quad3(a, b, MARK_W_M, MARK_U8, KIND_MARK, s0)
                c += 1
            return c

        if not dashed:
            for i in range(n - 1):
                a, b = pts3[i], pts3[i + 1]
                h = hidden((a[0] + b[0]) * 0.5, (a[2] + b[2]) * 0.5, u, False)
                if h == 1:
                    continue
                if h == 2:
                    count += dashes(s[i], s[i + 1], False)
                    continue
                buf.quad3(a, b, MARK_W_M, MARK_U8, KIND_MARK, s[i])
                count += 1
        else:
            count += dashes(0.0, total, True)
    return count


def patch_triangles(poly):
    """Triangles ((e, n) x 3) of a shapely polygon: constrained Delaunay
    when shapely 2.1 is there, ear clipping of the exterior otherwise."""
    if _cdt is not None:
        tris = []
        for t in _cdt(poly).geoms:
            c = list(t.exterior.coords)[:3]
            tris.append(tuple((float(x), float(y)) for x, y in c))
        return tris
    ring = [(float(x), float(y)) for x, y in list(poly.exterior.coords)[:-1]]
    return [(ring[a], ring[b], ring[c]) for a, b, c in triangulate(ring)]


class StationIndex:
    """Stations of the ways near the elevated ones, by 32 m cell: what a
    bridge pier would stand on."""
    CELL = 32.0

    def __init__(self, ways, everything=False):
        hot = set()
        for wy in ways:
            if everything or wy["w"].get("elevated"):
                for e, n in wy["pts"]:
                    ce, cn = int(e // self.CELL), int(n // self.CELL)
                    for de in (-2, -1, 0, 1, 2):
                        for dn in (-2, -1, 0, 1, 2):
                            hot.add((ce + de, cn + dn))
        self.cells = defaultdict(list)
        for wi, wy in enumerate(ways):
            for si, (e, n) in enumerate(wy["pts"]):
                k = (int(e // self.CELL), int(n // self.CELL))
                if k in hot:
                    self.cells[k].append((wi, si))

    def near(self, e, n, radius):
        r = int(math.ceil(radius / self.CELL))
        ce, cn = int(e // self.CELL), int(n // self.CELL)
        for de in range(-r, r + 1):
            for dn in range(-r, r + 1):
                yield from self.cells.get((ce + de, cn + dn), ())


def _seg_dist(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 < 1e-9 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2))
    return math.hypot(p[0] - (a[0] + dx * t), p[1] - (a[1] + dy * t))


def way_under(index, ways, wi, e, n, deck_h, radius):
    """True when another way passes within `radius` of (e, n) more than
    PIER_MIN_H_M below the deck (a pier would stand on it)."""
    seen = set()
    for oi, si in index.near(e, n, radius + DENSIFY_M * 1.5):
        if oi == wi or (oi, si) in seen:
            continue
        seen.add((oi, si))
        o = ways[oi]
        P = o["pts"]
        for j in (si - 1, si):
            if j < 0 or j + 1 >= len(P):
                continue
            if _seg_dist((e, n), P[j], P[j + 1]) < o["width"] * 0.5 + radius:
                if deck_h - max(float(o["prof"][j]), float(o["prof"][j + 1])) > PIER_MIN_H_M:
                    return True
    return False


def bake_corridor(terrain: Terrain, pts, prof, half_width, protect=None):
    """Terrain pixels within half_width + margin of the polyline take the
    profile height of the nearest point (road cut / embankment); where
    corridors overlap the way with the nearest ribbon edge wins."""
    step = terrain.tile_m / (terrain.px - 1)
    reach = half_width + max(CORRIDOR_MARGIN_MIN_M, step * math.sqrt(2.0) + 1.0)
    P = np.array(pts)
    emin, emax = P[:, 0].min() - reach, P[:, 0].max() + reach
    nmin, nmax = P[:, 1].min() - reach, P[:, 1].max() + reach
    te0 = int(math.floor(emin / terrain.tile_m)) * terrain.tile_m
    te1 = int(math.floor(emax / terrain.tile_m)) * terrain.tile_m
    tn0 = int(math.floor(nmin / terrain.tile_m)) * terrain.tile_m
    tn1 = int(math.floor(nmax / terrain.tile_m)) * terrain.tile_m
    touched = 0
    for te in range(te0, te1 + 1, terrain.tile_m):
        for tn in range(tn0, tn1 + 1, terrain.tile_m):
            a = terrain.load(te, tn)
            if a is None:
                continue
            c0 = max(0, int((te + terrain.tile_m - emax) / step) - 1)
            c1 = min(terrain.px - 1, int((te + terrain.tile_m - emin) / step) + 1)
            r0 = max(0, int((nmin - tn) / step) - 1)
            r1 = min(terrain.px - 1, int((nmax - tn) / step) + 1)
            if c0 > c1 or r0 > r1:
                continue
            cols = np.arange(c0, c1 + 1)
            rows = np.arange(r0, r1 + 1)
            pe = te + terrain.tile_m - cols * step
            pn = tn + rows * step
            E, N = np.meshgrid(pe, pn)   # shape (rows, cols)
            # nearest polyline segment per pixel (vectorised over pixels, loop over segments)
            best_d = np.full(E.shape, np.inf)
            best_h = np.zeros(E.shape)
            for i in range(len(P) - 1):
                x0, y0 = P[i]; x1, y1 = P[i + 1]
                dx, dy = x1 - x0, y1 - y0
                L2 = dx * dx + dy * dy
                if L2 < 1e-9:
                    continue
                t = ((E - x0) * dx + (N - y0) * dy) / L2
                t = np.clip(t, 0.0, 1.0)
                d = np.hypot(E - (x0 + t * dx), N - (y0 + t * dy))
                h = prof[i] + (prof[i + 1] - prof[i]) * t
                m = d < best_d
                best_d[m] = d[m]
                best_h[m] = h[m]
            m = best_d <= reach
            if protect is not None and m.any():
                m &= ~protection_mask(protect, E, N)   # never inside an airport plateau
            if m.any():
                # where corridors overlap (a lane beside a motorway, a road
                # along a railway) the pixel blends the ways' heights by
                # 1 / (distance from the ribbon edge + 3 m)^2: under a ribbon
                # the ground is its own, between two ways the step is spread
                # over the overlap instead of jumping at a jagged pixel
                # boundary. A 31 m pixel shared by two ways at different
                # heights still lifts the farther one, and its embankment
                # strip (emit_skirts) covers that (Terrain.resolve_corridors
                # writes the means once every corridor is in)
                sw, swh = terrain.corridor_acc(te, tn)
                edge_d = np.maximum(best_d - half_width, 0.0)
                wgt = 1.0 / (edge_d + CORRIDOR_BLEND_M) ** 2
                sw[r0:r1 + 1, c0:c1 + 1][m] += wgt[m]
                swh[r0:r1 + 1, c0:c1 + 1][m] += (wgt * best_h)[m]
                touched += int(m.sum())
    return touched


# ── driver ──────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--region", required=True)
    ap.add_argument("--pbf", default="")
    ap.add_argument("--fetch", action="store_true", help="download the Geofabrik extract when missing")
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("E0", "N0", "E1", "N1"), help="LV95 box (default: whole region)")
    ap.add_argument("--terrain-dir", default="")
    ap.add_argument("--terrain-overlay", action="append", default=[], metavar="DIR",
                    help="a finer terrain drawn over the region's (the installed Swiss 2 m scenery's terrain/, same tile format): "
                         "the ribbons follow it where it has data; repeatable")
    ap.add_argument("--out", default="")
    ap.add_argument("--corridor", action="store_true", help="re-write the terrain tiles so the ground follows each road / track")
    ap.add_argument("--source", choices=["osm", "tlm"], default="osm",
                    help="osm: Geofabrik PBF (default); tlm: swissTLM3D GeoPackage (Switzerland, true heights)")
    ap.add_argument("--tlm-gpkg", default=TLM_GPKG_DEFAULT)
    ap.add_argument("--design", action="append", default=[], metavar="JSON",
                    help="scenery editor design file (repeatable): its roads / tracks join the bake, OSM ways inside its replace polygons are cut")
    ap.add_argument("--blocks", default=str(ROOT / "scenery" / "blocks"), metavar="DIR",
                    help="junction block library (docs/ROAD_BLOCKS.md): *.json exported from the Blender drawings")
    ap.add_argument("--classes", default="", metavar="LIST",
                    help="comma list of road / rail classes to bake, e.g. motorway,motorway_link (default: all)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    global _cdt, _ShPoint
    try:
        from shapely.geometry import Point as _ShPoint
    except ImportError:
        sys.exit("shapely is required for the pavement patches (pip install shapely)")
    try:
        from shapely import constrained_delaunay_triangles as _cdt
    except ImportError:
        _cdt = None

    regs = json.load(open(ROOT / "scripts" / "world_regions.json"))
    regs = regs["regions"] if isinstance(regs, dict) else regs
    reg = next((r for r in regs if r["id"] == args.region), None)
    if reg is None:
        sys.exit("unknown region " + args.region)
    region_dir = BUILD_ROOT / args.region / reg["subdir"]
    terrain_dir = Path(args.terrain_dir) if args.terrain_dir else region_dir / "terrain"
    out_dir = Path(args.out) if args.out else region_dir / "roads"
    pbf = Path(args.pbf) if args.pbf else OSM_CACHE / (GEOFABRIK.get(args.region, args.region).split("/")[-1] + "-latest.osm.pbf")
    if not pbf.exists() and args.source == "osm":
        if not args.fetch:
            sys.exit(f"PBF not found: {pbf} (use --fetch)")
        url = f"https://download.geofabrik.de/{GEOFABRIK[args.region]}-latest.osm.pbf"
        print("fetching", url)
        pbf.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, pbf)
    terrain = Terrain(terrain_dir)
    print(f"region {args.region}: terrain {terrain.px} px tiles at {terrain_dir}, PBF {pbf} ({pbf.stat().st_size / 1e6:.0f} MB)")
    for d in args.terrain_overlay:
        ov = Overlay(Path(d))
        terrain.overlays.append(ov)
        print(f"terrain overlay: {ov.px} px tiles ({ov.res_m:.1f} m posting) at {d}")

    bbox_wgs = None
    if args.bbox:
        e0, n0, e1, n1 = args.bbox
        lons, lats = zip(*[wgs.transform(e, n) for e, n in ((e0, n0), (e1, n0), (e0, n1), (e1, n1))])
        bbox_wgs = (min(lons), min(lats), max(lons), max(lats))
    t0 = time.time()
    if args.source == "osm" and osmium is None:
        sys.exit("the OSM source needs pyosmium (pip install osmium)")
    if args.source == "tlm":
        if not args.bbox:
            sys.exit("--source tlm needs --bbox (the GeoPackage is read by box)")
        class _Col:
            pass
        col = _Col()
        col.ways, n_r, n_t = collect_tlm(Path(args.tlm_gpkg), args.bbox)
        col.n = len(col.ways)
        print(f"swissTLM3D: {col.n} ways in {time.time() - t0:.0f} s ({n_r} roads, {n_t} railways) from {args.tlm_gpkg}")
    else:
        col = Collector(bbox_wgs)
        col.apply_file(str(pbf), locations=True, idx="flex_mem")
        print(f"OSM: {col.n} ways in {time.time() - t0:.0f} s "
              f"({sum(1 for w in col.ways if w['kind'] == 'road')} roads, {sum(1 for w in col.ways if w['kind'] == 'rail')} railways)")

    # Scenery editor designs: their ways join the OSM set, OSM ways inside
    # their replace polygons are cut away (docs/SCENERY_EDITOR.md §6.7).
    designs = [Design(Path(p)) for p in args.design]
    replace_polys = [poly for d in designs for poly in d.replace]
    replace_ids = set().union(*[d.replace_ids for d in designs]) if designs else set()
    source_ways = list(col.ways)
    if args.classes:
        keep = set(c.strip() for c in args.classes.split(",") if c.strip())
        source_ways = [w for w in source_ways if w["cls"] in keep]
        print(f"classes {sorted(keep)}: {len(source_ways)} ways kept")
    for d in designs:
        source_ways.extend(d.ways)
        print(f"design {d.path}: {len(d.ways)} ways, {len(d.junctions)} junctions, {len(d.replace)} replace polygons (sha256 {d.sha256[:12]})")

    stats = defaultdict(int)
    block_lib = {}
    for bf in sorted(Path(args.blocks).glob("*.json")) if Path(args.blocks).is_dir() else []:
        try:
            b = json.load(open(bf))
            block_lib[b["name"]] = b
        except Exception as e:   # pragma: no cover
            print("WARNING: block", bf, "not readable:", e)
    print(f"junction blocks: {sorted(block_lib)} from {args.blocks}")
    # Stage 0: with --corridor (the flag that allows terrain writes) every
    # tile of the box starts from terrain_base again, then the designs'
    # plateaus go in, before any profile is sampled.
    if args.corridor:
        stats["terrain_reset"] = terrain.reset_box(*args.bbox) if args.bbox else terrain.reset_all()
        print(f"terrain: {stats['terrain_reset']} tiles reset from {terrain.base}")
        for d in designs:
            for poly, h in d.flatten:
                stats["flatten_px"] += bake_flatten(terrain, poly, h)
                stats["plateaus"] += 1

    # per-tile mesh buffers
    tiles: dict = defaultdict(MeshBuf)
    sidecars: dict = defaultdict(list)   # tile -> OSM way records for the editor
    corridor_px = 0
    # Stage 1: geometry and the smoothed terrain profile of every way.
    ways = []
    pieces = []
    for w in source_ways:
        if replace_ids and not w.get("design") and w["id"] in replace_ids:
            stats["osm_ways_replaced"] += 1
            continue
        pts = [lv95.transform(lo, la) for lo, la in w["pts"]]
        refs = w["refs"]
        if args.bbox:
            e0, n0, e1, n1 = args.bbox
            keep = [i for i, p in enumerate(pts) if e0 - 200 <= p[0] <= e1 + 200 and n0 - 200 <= p[1] <= n1 + 200]
            if len(keep) < 2:
                continue
            pts = [pts[i] for i in keep]
            refs = [refs[i] for i in keep]
        if replace_polys and not w.get("design"):
            cut = clip_way(pts, refs, replace_polys)
            if len(cut) != 1 or len(cut[0][0]) != len(pts):
                stats["osm_ways_cut"] += 1
            for pp, rr in cut:
                pieces.append((w, pp, rr))
        else:
            pieces.append((w, pts, refs))
    for w, pts, refs in pieces:
        end_refs = (refs[0], refs[-1])
        zs = None
        if w.get("has_z"):
            # keep the heights aligned with the (bbox-filtered) points, then densify both
            zmap = {}
            all_pts = [lv95.transform(lo, la) for lo, la in w["pts"]]
            for p, z in zip(all_pts, w["z"]):
                zmap[(round(p[0], 3), round(p[1], 3))] = z
            zs = [zmap.get((round(p[0], 3), round(p[1], 3)), float("nan")) for p in pts]
            if any(math.isnan(z) for z in zs):
                zs = None
        if zs is not None:
            pts, zs = densify_z(pts, zs)
        else:
            pts = densify(pts)
        if w["kind"] == "road":
            _lanes, colour, hw = ROAD_CLASSES[w["cls"]]
            width, kind, lanes = road_geometry(w["cls"], w["lanes"], w["width"], w["oneway"])
            gauge = ballast = tracks = None
        else:
            gauge_mm, ballast, hw = RAIL_CLASSES[w["cls"]]
            gauge = parse_num(w["gauge"], gauge_mm) / 1000.0
            tracks = int(max(1, min(4, parse_num(w["tracks"], 1))))
            width = BALLAST_WIDTH_M + TRACK_SPACING_M * (tracks - 1) if ballast else gauge + 1.0
            colour = kind = lanes = None
        # the along-track smoothing: the class's window on the 30 m base tier
        # (its posting makes the raw profile a staircase), a few metres on a
        # 2 m overlay (that DTM already is the road's own grade)
        tp = terrain_profile(pts, terrain, min(hw, max(8.0, 4.0 * terrain.res_at(pts[0][0], pts[0][1]))))
        if tp is None:
            stats["no_terrain"] += 1
            continue
        ground, prof = tp
        if zs is not None:
            prof = np.array(zs, dtype=float)   # the surveyed profile: no smoothing, no guessing
            stats["ways_with_heights"] += 1
        if w.get("offsets"):
            # a design chain's node height offsets, resampled onto the densified stations
            s_old, o_old = w["offsets"]
            prof = prof + np.interp(along(pts), s_old, o_old)
        ways.append({"w": w, "pts": pts, "s": along(pts), "ground": ground, "prof": prof, "width": width,
                     "colour": colour, "kind": kind, "lanes": lanes, "gauge": gauge, "ballast": ballast,
                     "tracks": tracks, "end_refs": end_refs, "ramp": False, "rise": 0.0})

    # Stage 2: bridge decks (lifted over what they cross), approach ramps, tunnels.
    ramps = bridge_decks([wy for wy in ways if not wy["w"].get("has_z")], stats)
    ramp_approaches(ways, ramps, stats)
    # Short tunnels are underpasses through a road or rail embankment that a
    # 30 m posting cannot dig: keep them at grade so the line stays whole
    # from the air (the crossing way is drawn over them by its class lift).
    for chain in _chains([wy for wy in ways if wy["w"]["tunnel"] and not wy["w"].get("has_z")]):
        if sum(m["s"][-1] for m, _f in chain) < SHORT_TUNNEL_M:
            for m, _f in chain:
                m["w"]["tunnel"] = False
                stats["underpasses"] += 1
    surface_ends = set(r for wy in ways if not wy["w"]["tunnel"] for r in wy["end_refs"])
    for wy in ways:
        if wy["w"]["tunnel"] and not wy["w"].get("has_z"):
            wy["prof"] = tunnel_profile(wy, tuple(r in surface_ends for r in wy["end_refs"]))
            stats["tunnels"] += 1
        elif wy["w"]["tunnel"]:
            # surveyed axis: drawn where it is, no corridor. An Unterfuehrung
            # is a short tunnel under the crossing way's ground (the ground
            # around the crossing belongs to the way on top).
            stats["underpasses" if wy["w"].get("underpass") else "tunnels"] += 1

    # Stage 3: the corridor bake. Corridors of neighbouring ways overlap
    # (parallel tracks of a yard, a road beside a railway) and the last one
    # written wins, so the ramped approaches go first: a road passing under
    # the bridge then cuts its own level through the embankment. Bridges
    # and tunnels leave the ground alone.
    if args.corridor:
        protect = airport_protection(region_dir)
        print(f"airport protection: {len(protect['corridors'])} runway / taxiway strips, {len(protect['polys'])} aprons")
        for wy in sorted((wy for wy in ways if not wy["w"]["bridge"] and not wy["w"]["tunnel"] and not wy["w"].get("elevated")), key=lambda wy: not wy["ramp"]):
            corridor_px += bake_corridor(terrain, wy["pts"], wy["prof"], wy["width"] * 0.5, protect)
        corridor_px = terrain.resolve_corridors()

    # Stage 4a: the final profile of every way on the final ground, never
    # below it (a bridge deck is raised too where a corridor lifted the ground
    # under it; a tunnel stays below the surface), then the junctions of the
    # baked network. A junction is (a) a point where way ends meet (OSM
    # shares node ids, swissTLM3D splits its lines) and (b) a way end lying
    # on or beside another way's pavement at the same level (a swissTLM3D
    # Ein-/Ausfahrt ends beside the Autobahn line without sharing a vertex):
    # the other way is split there into two legs. Two legs of one kind and
    # width meet on a mitre; every other junction gets a PATCH: the 2D union
    # of its legs' ribbons back to where they stop overlapping (a ramp at 6
    # degrees overlaps its carriageway for ~100 m) plus the gore taper of a
    # merging ramp, one plain pavement without wedges or seams; the legs'
    # ribbons are cut back to it. Markings are separate strips
    # (emit_markings), so a patch is never a gap in the lane lines.
    from shapely.geometry import Polygon as _ShPoly, box as _ShBox, LineString as _ShLine
    import shapely as _shapely
    from shapely.ops import unary_union as _sh_union
    from shapely.strtree import STRtree as _ShTree
    from shapely import prepared as _sh_prepared

    for wy in ways:
        if not wy["w"]["tunnel"]:
            wy["prof"] = ground_under(wy["pts"], wy["prof"], wy["width"] * 0.5, terrain)
        wy["s"] = along(wy["pts"])
        wy["gaps"] = []
        wy["end_frames"] = [None, None]
        wy["cutters"] = []

    def way_piece(wy, a, b):
        """Stations (e, n) and heights of a way between chainages a < b (a, the
        stations strictly inside, b)."""
        s, pts, prof = wy["s"], wy["pts"], wy["prof"]
        chain = [a] + [v for v in s if a < v < b] + [b]
        E = np.interp(chain, s, [q[0] for q in pts])
        N = np.interp(chain, s, [q[1] for q in pts])
        H = np.interp(chain, s, prof)
        out_p, out_h = [], []
        for e, n, h in zip(E, N, H):
            if out_p and math.hypot(e - out_p[-1][0], n - out_p[-1][1]) < 0.05:
                continue
            out_p.append((float(e), float(n)))
            out_h.append(float(h))
        return out_p, out_h

    def leg_slice(wy, s0, fwd, length):
        """way_piece from the node at chainage s0 along the leg (node first)."""
        if fwd:
            return way_piece(wy, s0, min(wy["s"][-1], s0 + length))
        pp, hh = way_piece(wy, max(0.0, s0 - length), s0)
        return pp[::-1], hh[::-1]

    def tangent_at(wy, s0):
        s, pts = wy["s"], wy["pts"]
        i = min(len(s) - 2, max(0, int(np.searchsorted(s, s0, side="right")) - 1))
        dx, dy = pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]
        l = math.hypot(dx, dy) or 1.0
        return (dx / l, dy / l)

    def project(wy, e, n, near_stations):
        """(distance, chainage) of the nearest point of the way to (e, n) among the segments at near_stations."""
        best = (math.inf, 0.0)
        s, pts = wy["s"], wy["pts"]
        for si in near_stations:
            for j in (si - 1, si):
                if j < 0 or j + 1 >= len(pts):
                    continue
                a, b = pts[j], pts[j + 1]
                dx, dy = b[0] - a[0], b[1] - a[1]
                l2 = dx * dx + dy * dy
                t = 0.0 if l2 < 1e-9 else max(0.0, min(1.0, ((e - a[0]) * dx + (n - a[1]) * dy) / l2))
                d = math.hypot(e - (a[0] + dx * t), n - (a[1] + dy * t))
                if d < best[0]:
                    best = (d, s[j] + (s[j + 1] - s[j]) * t)
        return best

    def key_of(p):
        return (round(p[0], 1), round(p[1], 1))

    def way_point(wy, sv):
        s, pts = wy["s"], wy["pts"]
        return (float(np.interp(sv, s, [q[0] for q in pts])), float(np.interp(sv, s, [q[1] for q in pts])))

    # ── travel direction of one-way ways ────────────────────────────────
    # swissTLM3D has no direction attribute and digitises only ~3/4 of its
    # one-way lines in the driving direction. Right-hand traffic settles a
    # carriageway: the opposite carriageway (the nearest roughly parallel
    # line of the same class, 6–45 m away) must lie on its left. A ramp is
    # settled by its type: an Ausfahrt starts at the Autobahn, an Einfahrt
    # ends there. Chains of connected one-way ways are then made consistent
    # by majority, which also orients the ramp pieces that touch nothing.
    def flip_way(wy):
        wy["pts"] = wy["pts"][::-1]
        wy["prof"] = np.asarray(wy["prof"])[::-1].copy()
        wy["ground"] = np.asarray(wy["ground"])[::-1].copy() if wy.get("ground") is not None else wy.get("ground")
        wy["s"] = along(wy["pts"])
        wy["w"]["flipped"] = not wy["w"].get("flipped", False)

    orient_index = StationIndex(ways, everything=True)
    flips = 0
    votes = {}   # wi -> +1 keep / -1 flip (strength); swissTLM3D only
    if any(w["w"].get("source") == "tlm" for w in ways):
        oneways = [wi for wi, wy in enumerate(ways) if wy["w"]["kind"] == "road" and wy["w"]["oneway"] and wy["w"].get("source") == "tlm"]
        for wi in oneways:
            wy = ways[wi]
            cls = wy["w"]["cls"]
            tlm = wy["w"].get("tlm", "")
            s = wy["s"]
            if tlm in ("Ausfahrt", "Einfahrt"):
                # the end that touches a carriageway
                def near_carriageway(p):
                    best = math.inf
                    for oi, si in orient_index.near(p[0], p[1], 40.0):
                        o = ways[oi]
                        if oi == wi or o["w"]["cls"] not in ("motorway", "trunk"):
                            continue
                        P = o["pts"]
                        for j in (si - 1, si):
                            if 0 <= j < len(P) - 1:
                                best = min(best, _seg_dist(p, P[j], P[j + 1]))
                    return best
                d0, d1 = near_carriageway(wy["pts"][0]), near_carriageway(wy["pts"][-1])
                if min(d0, d1) < 20.0 and abs(d0 - d1) > 5.0:
                    starts_at = d0 < d1
                    want_start = tlm == "Ausfahrt"
                    votes[wi] = 3 if starts_at == want_start else -3
            elif cls in ("motorway", "trunk") and len(wy["pts"]) >= 2:
                score = 0
                for f in (0.25, 0.5, 0.75):
                    sv = s[-1] * f
                    e, n = way_point(wy, sv)
                    t = tangent_at(wy, sv)
                    best = None
                    for oi, si in orient_index.near(e, n, 45.0):
                        o = ways[oi]
                        if oi == wi or o["w"]["cls"] != cls:
                            continue
                        P = o["pts"]
                        for j in (si - 1, si):
                            if not (0 <= j < len(P) - 1):
                                continue
                            d = _seg_dist((e, n), P[j], P[j + 1])
                            if not (6.0 < d < 45.0):
                                continue
                            dx, dy = P[j + 1][0] - P[j][0], P[j + 1][1] - P[j][1]
                            l = math.hypot(dx, dy) or 1.0
                            if abs(dx / l * t[0] + dy / l * t[1]) < 0.85:
                                continue
                            if best is None or d < best[0]:
                                mx, my = (P[j][0] + P[j + 1][0]) * 0.5, (P[j][1] + P[j + 1][1]) * 0.5
                                best = (d, mx, my)
                    if best is not None:
                        left = -(best[1] - e) * t[1] + (best[2] - n) * t[0]
                        score += 1 if left > 0 else -1
                if score != 0:
                    votes[wi] = 2 if score > 0 else -2
        # chains: connected one-way ways of one class agree on a direction
        end_map = defaultdict(list)
        for wi in oneways:
            wy = ways[wi]
            end_map[key_of(wy["pts"][0])].append((wi, True))
            end_map[key_of(wy["pts"][-1])].append((wi, False))
        seen = set()
        for wi in oneways:
            if wi in seen:
                continue
            # walk the chain (degree-2 joins of the same class)
            chain, orient = [], {}
            stack = [(wi, +1)]
            while stack:
                cur, sign = stack.pop()
                if cur in orient:
                    continue
                orient[cur] = sign
                chain.append(cur)
                cw = ways[cur]
                for at_start in (True, False):
                    key = key_of(cw["pts"][0] if at_start else cw["pts"][-1])
                    others = [(oi, os_) for oi, os_ in end_map[key] if oi != cur]
                    if len(others) != 1 or len(end_map[key]) != 2:
                        continue
                    oi, o_start = others[0]
                    if ways[oi]["w"]["cls"] != cw["w"]["cls"]:
                        continue
                    # same travel sense if this way's start meets the other's end (or vice versa)
                    same = (at_start != o_start)
                    stack.append((oi, sign if same else -sign))
            seen.update(chain)
            total = sum(votes.get(c, 0) * orient[c] for c in chain)
            if total == 0:
                continue
            for c in chain:
                want = 1 if total > 0 else -1
                if orient[c] != want:
                    flip_way(ways[c])
                    flips += 1
    stats["oriented_flips"] = flips
    orient_index = StationIndex(ways, everything=True)


    nodes: dict = defaultdict(list)          # key -> legs
    for wi, wy in enumerate(ways):
        if len(wy["pts"]) < 2 or wy["w"]["kind"] != "road":
            continue
        for at_start in (True, False):
            p = wy["pts"][0] if at_start else wy["pts"][-1]
            d = tangent_at(wy, 0.0 if at_start else wy["s"][-1])
            nodes[key_of(p)].append({"wi": wi, "s0": 0.0 if at_start else wy["s"][-1], "fwd": at_start, "end": True,
                                     "dir": d if at_start else (-d[0], -d[1]), "half": wy["width"] * 0.5,
                                     "cls": wy["w"]["cls"], "kind": "road", "cut": 0.0})
    # (b) attachments: a way end on / beside another road way at the same level
    all_index = StationIndex(ways, everything=True)
    attached = 0
    for key, legs in list(nodes.items()):
        for L in [L for L in legs if L["end"]]:
            wy = ways[L["wi"]]
            e, n = wy["pts"][0] if L["fwd"] else wy["pts"][-1]
            h_end = float(wy["prof"][0] if L["fwd"] else wy["prof"][-1])
            cands = defaultdict(list)
            for oi, si in all_index.near(e, n, 40.0):
                if oi != L["wi"] and ways[oi]["w"]["kind"] == "road" and not ways[oi]["w"]["tunnel"]:
                    cands[oi].append(si)
            best = None
            t_end = tangent_at(wy, 0.0 if L["fwd"] else wy["s"][-1])   # the way's travel direction at this end
            for oi, sts in cands.items():
                if any(M["wi"] == oi for M in legs):
                    continue                    # already a leg of this node
                o = ways[oi]
                d, sp = project(o, e, n, sts)
                if d > L["half"] + o["width"] * 0.5 + ATTACH_EXTRA_M:
                    continue
                if abs(float(np.interp(sp, o["s"], o["prof"])) - h_end) > ATTACH_DH_M:
                    continue
                if wy["w"]["oneway"] and o["w"]["oneway"]:
                    to = tangent_at(o, sp)      # the opposite carriageway is not a junction
                    if t_end[0] * to[0] + t_end[1] * to[1] < -0.3:
                        continue
                if best is None or d < best[0]:
                    best = (d, oi, sp)
            if best is None:
                continue
            d, oi, sp = best
            o = ways[oi]
            t = tangent_at(o, sp)
            if sp > 0.5:
                legs.append({"wi": oi, "s0": sp, "fwd": False, "end": False, "dir": (-t[0], -t[1]), "half": o["width"] * 0.5,
                             "cls": o["w"]["cls"], "kind": "road", "cut": 0.0})
            if sp < o["s"][-1] - 0.5:
                legs.append({"wi": oi, "s0": sp, "fwd": True, "end": False, "dir": t, "half": o["width"] * 0.5,
                             "cls": o["w"]["cls"], "kind": "road", "cut": 0.0})
            attached += 1
    stats["attached_ends"] = attached

    # ── Junction blocks (docs/ROAD_BLOCKS.md) ───────────────────────────────
    # A block is a drawing of one situation in the frame of the carriageway
    # it attaches to (X from the carriageway's outer edge, Y along travel, 0
    # at the nose / change), stamped along the real carriageway and its
    # same-class one-way continuation so it bends with the road.
    def chain_walk(wi, s0, back_m, fwd_m):
        """(points (e, n, h, way index), chainage relative to s0) along the way
        and its same-class one-way continuations, from s0 - back_m to s0 + fwd_m."""
        def continuation(wj, forward):
            w = ways[wj]
            key = key_of(w["pts"][-1] if forward else w["pts"][0])
            for L in nodes.get(key, []):
                o = ways[L["wi"]]
                if L["wi"] == wj or not L["end"] or o["w"]["cls"] != w["w"]["cls"] or not o["w"]["oneway"] or o["w"]["tunnel"]:
                    continue                   # a lane never runs on into a tunnel
                if forward and L["fwd"]:
                    return L["wi"]
                if not forward and not L["fwd"]:
                    return L["wi"]
            return None

        w = ways[wi]
        need, cur, sa = back_m, wi, s0
        ups = []
        guard = 0
        while need > 0.0 and cur is not None and guard < 20:
            guard += 1
            cw = ways[cur]
            take = min(need, sa)
            pp, hh = way_piece(cw, sa - take, sa)
            ups.append((cur, pp, hh))
            need -= take
            if need > 0.0:
                cur = continuation(cur, False)
                sa = ways[cur]["s"][-1] if cur is not None else 0.0
        pts_all = []
        for cur, pp, hh in reversed(ups):
            for (e, n), h in zip(pp, hh):
                if pts_all and math.dist(pts_all[-1][:2], (e, n)) < 0.05:
                    continue
                pts_all.append((e, n, h, cur))
        need, cur, sa = fwd_m, wi, s0
        guard = 0
        while need > 0.0 and cur is not None and guard < 20:
            guard += 1
            cw = ways[cur]
            take = min(need, cw["s"][-1] - sa)
            pp, hh = way_piece(cw, sa, sa + take)
            for (e, n), h in zip(pp, hh):
                if pts_all and math.dist(pts_all[-1][:2], (e, n)) < 0.05:
                    continue
                pts_all.append((e, n, h, cur))
            need -= take
            if need > 0.0:
                cur = continuation(cur, True)
                sa = 0.0
        if len(pts_all) < 2:
            return pts_all, [0.0] * len(pts_all)
        rel = [0.0]
        for i in range(1, len(pts_all)):
            rel.append(rel[-1] + math.dist(pts_all[i - 1][:2], pts_all[i][:2]))
        node_pt = way_point(w, s0)
        k0 = min(range(len(pts_all)), key=lambda i: math.dist(pts_all[i][:2], node_pt))
        rel = [r - rel[k0] for r in rel]
        return pts_all, rel

    def frame_at(pts_all, rel, sv):
        """(e, n, h, left normal (e, n), way index) on the chain at relative chainage sv."""
        if sv <= rel[0]:
            i = 0
        elif sv >= rel[-1]:
            i = len(rel) - 2
        else:
            i = int(np.searchsorted(rel, sv, side="right")) - 1
        i = max(0, min(len(rel) - 2, i))
        a, b = pts_all[i], pts_all[i + 1]
        seg = rel[i + 1] - rel[i]
        t = 0.0 if seg < 1e-9 else max(0.0, min(1.0, (sv - rel[i]) / seg))
        e, n = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        h = a[2] + (b[2] - a[2]) * t
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1.0
        return e, n, h, (-dy / l, dx / l), (a[3] if t < 0.5 else b[3])

    def hermite(p0, t0, p1, t1, n=12):
        out = []
        for k in range(n + 1):
            u = k / n
            h00, h10, h01, h11 = 2 * u ** 3 - 3 * u ** 2 + 1, u ** 3 - 2 * u ** 2 + u, -2 * u ** 3 + 3 * u ** 2, u ** 3 - u ** 2
            out.append((h00 * p0[0] + h10 * t0[0] + h01 * p1[0] + h11 * t1[0],
                        h00 * p0[1] + h10 * t0[1] + h01 * p1[1] + h11 * t1[1]))
        return out

    class BlockFrame:
        """The carriageway chain a block is stamped on: block (x, y) -> world."""

        def __init__(self, wi_B, s0, side, half_b, lift_b, extent):
            self.side, self.half_b, self.lift_b = side, half_b, lift_b
            self.chain, self.rel = chain_walk(wi_B, s0, -extent[0] + 10.0, extent[1] + 10.0)
            self.ok = len(self.chain) >= 2
            if self.ok:
                self.y_lo, self.y_hi = self.rel[0] + 0.5, self.rel[-1] - 0.5
                self.ok = self.y_hi - self.y_lo >= 20.0

        def stamp(self, y, x):
            e, n, h, ln, wj = frame_at(self.chain, self.rel, y)
            u = self.half_b + x
            return e + ln[0] * u * self.side, n + ln[1] * u * self.side, h, wj

        def tangent(self, y):
            e, n, h, ln, wj = frame_at(self.chain, self.rel, y)
            return (ln[1], -ln[0])          # travel direction (left normal rotated back)

    def stamp_block(block, frame, wb, colour_u8):
        """Stamp every object of a block on its frame; returns the ways it touched."""
        clip = _ShBox(-500.0, frame.y_lo, 500.0, frame.y_hi)
        mid = frame.stamp((frame.y_lo + frame.y_hi) * 0.5, 0.0)
        k = (int(math.floor(mid[0] / TILE_M)) * TILE_M, int(math.floor(mid[1] / TILE_M)) * TILE_M)
        buf = tiles[k]
        buf.current_way = int(wb["w"]["id"])
        touched = set()
        cut_side = -1 if frame.side > 0 else 1

        def world3(x, y, lift):
            e, n, h, wj = frame.stamp(y, x)
            touched.add(wj)
            return (CONV_E - e, h + frame.lift_b + lift, n - CONV_N), (e, n)

        def line_pieces(axis, width, dashed, lift):
            (x0, y0), (x1, y1) = axis
            L = math.hypot(x1 - x0, y1 - y0)
            if L < 0.5:
                return 0
            count = 0
            if not dashed:
                n = max(1, int(math.ceil(L / BLOCK_STEP_M)))
                prev = None
                for i in range(n + 1):
                    t = i / n
                    x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
                    if not (frame.y_lo <= y <= frame.y_hi):
                        prev = None
                        continue
                    pt, _en = world3(x, y, lift)
                    if prev is not None:
                        buf.quad3(prev, pt, width, MARK_U8, KIND_MARK, t * L)
                        count += 1
                    prev = pt
            else:
                s = 0.0
                while s < L:
                    s1 = min(L, s + DASH_M)
                    ya, yb = y0 + (y1 - y0) * s / L, y0 + (y1 - y0) * s1 / L
                    if frame.y_lo <= ya <= frame.y_hi and frame.y_lo <= yb <= frame.y_hi and s1 - s > 1.0:
                        a, _ = world3(x0 + (x1 - x0) * s / L, ya, lift)
                        b, _ = world3(x0 + (x1 - x0) * s1 / L, yb, lift)
                        buf.quad3(a, b, width, MARK_U8, KIND_MARK, s)
                        count += 1
                    s += DASH_PERIOD_M
            return count

        for ob in block["objects"]:
            role = ob["role"]
            if role in ("ref", "ramp"):
                continue
            if role in ("pave", "gore", "cut"):
                poly = _ShPoly(ob["polygon"]).buffer(0).intersection(clip)
                if poly.is_empty:
                    continue
                geoms = list(poly.geoms) if poly.geom_type == "MultiPolygon" else [poly]
                for g in geoms:
                    g = _shapely.segmentize(g, BLOCK_STEP_M)
                    ring = list(g.exterior.coords)[:-1]
                    heights = {}
                    world_en = []
                    for x, y in ring:
                        pt, en = world3(x, y, PATCH_LIFT_M)
                        key_en = (round(en[0], 2), round(en[1], 2))
                        heights[key_en] = pt[1]
                        world_en.append(en)
                    wpoly = _ShPoly(world_en).buffer(0)
                    if wpoly.is_empty:
                        continue
                    if role == "cut":
                        for wj in touched:
                            ways[wj]["cutters"].append((_sh_prepared.prep(wpoly.buffer(SHOULDER_M * 0.0 + 0.3)), cut_side, ob.get("mode", "dash")))
                        continue
                    if role == "pave":
                        wgeoms = list(wpoly.geoms) if wpoly.geom_type == "MultiPolygon" else [wpoly]
                        for wg in wgeoms:
                            tris = patch_triangles(wg)
                            tris3 = []
                            for t in tris:
                                tri = []
                                for e, n in t:
                                    kk = (round(e, 2), round(n, 2))
                                    if kk not in heights:
                                        hh = frame.stamp(0.0, 0.0)[2]   # fallback: a point CDT invented
                                        heights[kk] = hh + frame.lift_b + PATCH_LIFT_M
                                    tri.append((CONV_E - e, heights[kk], n - CONV_N))
                                tris3.append(tuple(tri))
                            buf.tri_mesh(tris3, colour_u8, KIND_JUNCTION)
                            stats["block_pave_m2"] += wg.area
                    else:   # gore: outline + hatch every 3 m
                        for i in range(len(ring)):
                            a, b = ring[i], ring[(i + 1) % len(ring)]
                            stats["markings"] += line_pieces((a, b), MARK_W_M, False, MARK_LIFT_M)
                        ys = [pt[1] for pt in ring]
                        yy = math.floor(min(ys) / 3.0) * 3.0 + 1.5
                        while yy < max(ys):
                            cutline = _ShLine([(-500.0, yy), (500.0, yy)]).intersection(_ShPoly(ob["polygon"]).buffer(0))
                            segs = list(cutline.geoms) if cutline.geom_type == "MultiLineString" else ([cutline] if cutline.geom_type == "LineString" else [])
                            for sg in segs:
                                c = list(sg.coords)
                                if len(c) >= 2:
                                    stats["markings"] += line_pieces((c[0], c[-1]), MARK_W_M, False, MARK_LIFT_M)
                            yy += 3.0
            elif role in ("line", "dash"):
                stats["markings"] += line_pieces(ob["axis"], ob.get("width", MARK_W_M), role == "dash", MARK_LIFT_M)
        return touched

    def rejoin_ramp(wa, at_end, target, tangent):
        """Replace the ramp's last (at_end) / first BLOCK_JOIN_M metres by a Hermite
        curve into `target` (e, n, h) along `tangent`."""
        sa = wa["s"]
        if at_end:
            j0 = max(0.0, sa[-1] - BLOCK_JOIN_M)
            p0 = way_point(wa, j0)
            t0 = tangent_at(wa, j0)
            L = max(1.0, sa[-1] - j0)
            curve = hermite(p0, (t0[0] * L, t0[1] * L), (target[0], target[1]), (tangent[0] * L, tangent[1] * L))
            keep_p, keep_h = way_piece(wa, 0.0, j0) if j0 > 0.5 else ([], [])
            h0 = float(np.interp(j0, sa, wa["prof"]))
            hs = [h0 + (target[2] - h0) * k / (len(curve) - 1) for k in range(len(curve))]
            new_p = (keep_p[:-1] + curve) if keep_p else curve
            new_h = (keep_h[:-1] + hs) if keep_p else hs
        else:
            j1 = min(sa[-1], BLOCK_JOIN_M)
            p1 = way_point(wa, j1)
            t1 = tangent_at(wa, j1)
            L = max(1.0, j1)
            curve = hermite((target[0], target[1]), (tangent[0] * L, tangent[1] * L), p1, (t1[0] * L, t1[1] * L))
            keep_p, keep_h = way_piece(wa, j1, sa[-1]) if j1 < sa[-1] - 0.5 else ([], [])
            h1 = float(np.interp(j1, sa, wa["prof"]))
            hs = [target[2] + (h1 - target[2]) * k / (len(curve) - 1) for k in range(len(curve))]
            new_p = (curve + keep_p[1:]) if keep_p else curve
            new_h = (hs + keep_h[1:]) if keep_p else hs
        wa["pts"] = [(float(e), float(n)) for e, n in new_p]
        wa["prof"] = np.array(new_h, dtype=float)
        wa["s"] = along(wa["pts"])
        wa["end_frames"] = [None, None]

    # (a) entries and exits: a one-way link on a one-way carriageway. Two
    # forms of the same situation: swissTLM3D's link ends BESIDE an unsplit
    # carriageway line (an attachment: the carriageway is a through leg),
    # OSM splits the carriageway at the junction node, so the node holds the
    # link's end plus the end of the arriving and the start of the departing
    # carriageway way (three way ends). The block is stamped on the
    # carriageway chain either way (chain_walk follows the OSM split).
    for key, legs in list(nodes.items()):
        ends = [L for L in legs if L["end"]]
        thrus = [L for L in legs if not L["end"]]
        cw_legs = []
        if len(ends) == 1 and thrus and len(set(L["wi"] for L in thrus)) == 1:
            A, B_wi, s0 = ends[0], thrus[0]["wi"], thrus[0]["s0"]          # the TLM form
        elif not thrus and len(ends) == 3:
            links = [L for L in ends if ways[L["wi"]]["w"]["cls"] in ("motorway_link", "trunk_link")]
            cws = [L for L in ends if ways[L["wi"]]["w"]["cls"] in ("motorway", "trunk")]
            if len(links) != 1 or len(cws) != 2:
                continue
            arriving = [L for L in cws if not L["fwd"]]
            departing = [L for L in cws if L["fwd"]]
            if len(arriving) != 1 or len(departing) != 1:
                continue
            if ways[arriving[0]["wi"]]["w"]["cls"] != ways[departing[0]["wi"]]["w"]["cls"]:
                continue
            if arriving[0]["dir"][0] * departing[0]["dir"][0] + arriving[0]["dir"][1] * departing[0]["dir"][1] > -0.7:
                continue                                                  # the carriageway bends sharply here: not a continuation
            if not all(ways[L["wi"]]["w"]["oneway"] for L in cws):
                continue
            A, B_wi, s0 = links[0], arriving[0]["wi"], ways[arriving[0]["wi"]]["s"][-1]   # the OSM form
            cw_legs = cws
        else:
            continue
        wa, wb = ways[A["wi"]], ways[B_wi]
        if not (wa["w"]["oneway"] and wb["w"]["oneway"]) or wa["w"]["kind"] != "road" or wb["w"]["kind"] != "road":
            continue
        if wb["w"]["cls"] not in ("motorway", "trunk") or wa["w"]["cls"] not in ("motorway_link", "trunk_link"):
            continue
        tb = tangent_at(wb, s0)
        ta = tangent_at(wa, 0.0 if A["fwd"] else wa["s"][-1])
        if ta[0] * tb[0] + ta[1] * tb[1] < 0.7:
            continue
        tlm_kind = wa["w"].get("tlm", "")
        entry = True if tlm_kind == "Einfahrt" else (False if tlm_kind == "Ausfahrt" else not A["fwd"])
        if entry == A["fwd"]:
            continue                                  # the ramp touches the carriageway with its wrong end for its type
        block = block_lib.get("entry" if entry else "exit")
        if block is None:
            continue
        node_e, node_n = key
        left_n = (-tb[1], tb[0])
        ref = way_point(wa, min(30.0, wa["s"][-1]) if A["fwd"] else max(0.0, wa["s"][-1] - 30.0))
        side = 1.0 if ((ref[0] - node_e) * left_n[0] + (ref[1] - node_n) * left_n[1]) > 0 else -1.0
        half_b = wb["width"] * 0.5
        lift_b = ROAD_LIFT_M - CLASS_DROP_M.get(wb["w"]["cls"], 0.0)
        frame = BlockFrame(B_wi, s0, side, half_b, lift_b, block["extent"])
        if not frame.ok:
            continue
        ramp_ob = next((o for o in block["objects"] if o["role"] == "ramp"), None)
        if ramp_ob is not None:
            px, py = ramp_ob["point"]
            e, n, h, _wj = frame.stamp(py, px)
            rejoin_ramp(wa, entry, (e, n, h), frame.tangent(py))
        colour = ROAD_CLASSES[wb["w"]["cls"]][1]
        stamp_block(block, frame, wb, tuple(int(c * 255) for c in colour))
        # the ramp leaves the node (the block has it); the two carriageway
        # halves of the OSM form stay and meet on a mitre or a width taper
        nodes[key] = cw_legs
        stats["blocks_entry" if entry else "blocks_exit"] += 1
        if os.environ.get("PVP_DEBUG_NODE"):
            de, dn = (float(v) for v in os.environ["PVP_DEBUG_NODE"].split(","))
            if math.hypot(node_e - de, node_n - dn) < 150:
                print(f"DEBUG block at {key}: {'ENTRY' if entry else 'EXIT'} ramp {wa['w']['id']} {tlm_kind} flipped={wa['w'].get('flipped', False)} "
                      f"A.fwd={A['fwd']} vote={votes.get(A['wi'])} | carriageway {wb['w']['id']} flipped={wb['w'].get('flipped', False)} vote={votes.get(B_wi)} side={side:+.0f}")

    # (b) lane add / drop: two one-way ways of one class continuing each other with a lane more / less
    for key, legs in list(nodes.items()):
        if len(legs) != 2 or not all(L["end"] for L in legs):
            continue
        a, b = legs
        wa2, wb2 = ways[a["wi"]], ways[b["wi"]]
        if a["wi"] == b["wi"] or a["fwd"] == b["fwd"]:
            continue
        if not (wa2["w"]["oneway"] and wb2["w"]["oneway"]) or wa2["w"]["cls"] != wb2["w"]["cls"] or wa2["w"]["cls"] not in ("motorway", "trunk"):
            continue
        up, down = (a, b) if not a["fwd"] else (b, a)          # `up` ends here, `down` starts here
        lu, ld = int(ways[up["wi"]]["lanes"] or 0), int(ways[down["wi"]]["lanes"] or 0)
        if abs(lu - ld) != 1:
            continue
        add = ld > lu
        block = block_lib.get("lane_add" if add else "lane_drop")
        if block is None:
            continue
        narrow = up if add else down
        wn = ways[narrow["wi"]]
        s0 = wn["s"][-1] if add else 0.0
        half_b = wn["width"] * 0.5
        lift_b = ROAD_LIFT_M - CLASS_DROP_M.get(wn["w"]["cls"], 0.0)
        frame = BlockFrame(narrow["wi"], s0, -1.0, half_b, lift_b, block["extent"])   # the outer (right) side
        if not frame.ok:
            continue
        colour = ROAD_CLASSES[wn["w"]["cls"]][1]
        stamp_block(block, frame, wn, tuple(int(c * 255) for c in colour))
        nodes[key] = []
        stats["blocks_lane_add" if add else "blocks_lane_drop"] += 1

    # (c) a width change: two road ways continuing each other (one node, both
    # way ends) with different widths met as a step with a notch, the wider
    # ribbon's edges jumping to the narrower one at the node. The wider way
    # narrows to the other's width over TAPER_PER_LANE_M per lane of
    # difference ending at the node (the lane taper of the design chains,
    # taper_profile) and the two then meet on a plain mitre like two ways of
    # one width.
    width_tapers = 0
    for key, legs in nodes.items():
        if len(legs) != 2 or not all(L["end"] for L in legs):
            continue
        a, b = legs
        if a["wi"] == b["wi"] or abs(a["half"] - b["half"]) < 0.05:
            continue
        if ways[a["wi"]]["w"]["tunnel"] or ways[b["wi"]]["w"]["tunnel"]:
            continue
        wide, narrow = (a, b) if a["half"] > b["half"] else (b, a)
        ways[wide["wi"]].setdefault("taper", [None, None])[0 if wide["fwd"] else 1] = narrow["half"]
        wide["half"] = narrow["half"]
        width_tapers += 1
    stats["width_tapers"] = width_tapers

    patches = []
    for key, legs in nodes.items():
        if len(legs) < 2:
            continue
        road_legs = [L for L in legs if not ways[L["wi"]]["w"]["tunnel"]]
        if len(road_legs) < 2:
            continue
        simple = len(road_legs) == 2 and all(L["end"] for L in road_legs) and abs(road_legs[0]["half"] - road_legs[1]["half"]) < 0.05
        if simple:
            a, b = road_legs
            t0 = (-a["dir"][0], -a["dir"][1])   # travelling a -> node -> b, in (e, n)
            t1 = b["dir"]
            den = 1.0 + t0[0] * t1[0] + t0[1] * t1[1]
            if den < 1e-4:
                continue
            # frames live in xz (x = -e, z = n): tangent (-te, tn), left normal (-tn, -te)
            n0, n1 = (-t0[1], -t0[0]), (-t1[1], -t1[0])
            mx, mz = (n0[0] + n1[0]) / den, (n0[1] + n1[1]) / den
            l = math.hypot(mx, mz)
            if l > MITER_LIMIT:
                mx, mz = mx * MITER_LIMIT / l, mz * MITER_LIMIT / l
            ways[a["wi"]]["end_frames"][0 if a["fwd"] else 1] = (-mx, -mz) if a["fwd"] else (mx, mz)
            ways[b["wi"]]["end_frames"][0 if b["fwd"] else 1] = (mx, mz) if b["fwd"] else (-mx, -mz)
            stats["mitres"] += 1
            continue
        centre = (key[0], key[1])
        if len(road_legs) >= 3:
            junction_fan(centre, road_legs)          # corner cuts of the sharp legs
        for L in road_legs:
            L["cut"] = max(L["cut"], JUNCTION_MIN_CUT_M)
        def overlap_len(A, B):
            """How far from the node leg A's ribbon still overlaps leg B's (measured along A)."""
            wa, wb = ways[A["wi"]], ways[B["wi"]]
            sl, _h = leg_slice(wa, A["s0"], A["fwd"], MERGE_MAX_M)
            near = list(range(len(wb["pts"])))
            run, last = 0.0, 0.0
            for i, (e, n) in enumerate(sl):
                if i > 0:
                    run += math.dist(sl[i - 1], (e, n))
                d, _sp = project(wb, e, n, near)
                if d < A["half"] + B["half"]:
                    last = run
                elif run > 30.0:
                    break
            return last

        for i in range(len(road_legs)):
            for j in range(i + 1, len(road_legs)):
                a, b = road_legs[i], road_legs[j]
                cosang = a["dir"][0] * b["dir"][0] + a["dir"][1] * b["dir"][1]
                if cosang > 0.5:                       # legs within 60 degrees: a merge / split
                    ov_a, ov_b = overlap_len(a, b), overlap_len(b, a)
                    a["cut"] = max(a["cut"], min(MERGE_MAX_M, ov_a + 3.0 + (LANE_TAPER_M if (b["end"] and not a["end"]) else 0.0)))
                    b["cut"] = max(b["cut"], min(MERGE_MAX_M, ov_b + 3.0 + (LANE_TAPER_M if (a["end"] and not b["end"]) else 0.0)))
                elif cosang < -0.85 and (a["end"] != b["end"]):
                    # the through leg the merging lane continues into: room for its end taper
                    thru = b if a["end"] else a
                    thru["cut"] = max(thru["cut"], LANE_TAPER_M + 3.0)
        for L in road_legs:
            wy = ways[L["wi"]]
            room = (wy["s"][-1] - L["s0"]) if L["fwd"] else L["s0"]
            L["cut"] = min(L["cut"], room)
            sl, hs = leg_slice(wy, L["s0"], L["fwd"], L["cut"] + 0.3)
            L["slice"], L["slice_h"] = sl, hs
            L["quads"] = [g for g in (_ShPoly(q).buffer(0) for q in ribbon_quads(sl, L["half"])) if not g.is_empty]
            wy["gaps"].append((L["s0"], min(wy["s"][-1], L["s0"] + L["cut"])) if L["fwd"] else (max(0.0, L["s0"] - L["cut"]), L["s0"]))
        # gore taper of a merging / splitting ramp: the wedge between its
        # inner edge and the carriageway edge, from the node back
        fills = []
        for A in road_legs:
            if not A["end"]:
                continue
            for B in road_legs:
                if B is A or B["end"] or B["wi"] == A["wi"]:
                    continue
                cosab = A["dir"][0] * B["dir"][0] + A["dir"][1] * B["dir"][1]
                ob = ways[B["wi"]]
                near = list(range(len(ob["pts"])))
                if cosab < -0.85 and len(A["slice"]) >= 1:
                    # the leg the merging lane continues into: taper its outer edge
                    # into the carriageway edge over LANE_TAPER_M beyond the node
                    e, n = A["slice"][0]
                    d, sp = project(ob, e, n, near)
                    pe = float(np.interp(sp, ob["s"], [q[0] for q in ob["pts"]]))
                    pn = float(np.interp(sp, ob["s"], [q[1] for q in ob["pts"]]))
                    vx, vy = pe - e, pn - n
                    vl = math.hypot(vx, vy)
                    if vl > 1e-6 and vl - A["half"] - B["half"] <= GORE_GAP_M:
                        vx, vy = vx / vl, vy / vl
                        sp2 = min(ob["s"][-1], sp + LANE_TAPER_M) if B["fwd"] else max(0.0, sp - LANE_TAPER_M)
                        qe = float(np.interp(sp2, ob["s"], [q[0] for q in ob["pts"]]))
                        qn = float(np.interp(sp2, ob["s"], [q[1] for q in ob["pts"]]))
                        g = _ShPoly([(e - vx * A["half"], n - vy * A["half"]), (pe - vx * B["half"], pn - vy * B["half"]),
                                     (qe - vx * B["half"], qn - vy * B["half"])]).buffer(0)
                        if not g.is_empty:
                            fills.append(g)
                    continue
                if cosab < 0.85:
                    continue
                inner, edge = [], []
                run = 0.0
                for i, (e, n) in enumerate(A["slice"]):
                    if i > 0:
                        run += math.dist(A["slice"][i - 1], (e, n))
                    if run > MERGE_TAPER_M:
                        break
                    d, sp = project(ob, e, n, near)
                    pe = float(np.interp(sp, ob["s"], [q[0] for q in ob["pts"]]))
                    pn = float(np.interp(sp, ob["s"], [q[1] for q in ob["pts"]]))
                    vx, vy = pe - e, pn - n
                    vl = math.hypot(vx, vy)
                    if vl < 1e-6:
                        continue
                    vx, vy = vx / vl, vy / vl
                    if vl - A["half"] - B["half"] > GORE_GAP_M:
                        break
                    inner.append((e + vx * A["half"], n + vy * A["half"]))
                    edge.append((pe - vx * B["half"], pn - vy * B["half"]))
                for i in range(len(inner) - 1):
                    g = _ShPoly([inner[i], inner[i + 1], edge[i + 1], edge[i]]).buffer(0)
                    if not g.is_empty:
                        fills.append(g)
                # the ramp lane's outer edge tapers into the carriageway edge beyond the ramp's start
                if len(A["slice"]) >= 2:
                    e, n = A["slice"][-1]
                    d, sp = project(ob, e, n, near)
                    if d < A["half"] + B["half"] + GORE_GAP_M:
                        pe = float(np.interp(sp, ob["s"], [q[0] for q in ob["pts"]]))
                        pn = float(np.interp(sp, ob["s"], [q[1] for q in ob["pts"]]))
                        vx, vy = pe - e, pn - n
                        vl = math.hypot(vx, vy) or 1.0
                        vx, vy = vx / vl, vy / vl
                        sp2 = min(ob["s"][-1], sp + LANE_TAPER_M) if B["fwd"] else max(0.0, sp - LANE_TAPER_M)
                        qe = float(np.interp(sp2, ob["s"], [q[0] for q in ob["pts"]]))
                        qn = float(np.interp(sp2, ob["s"], [q[1] for q in ob["pts"]]))
                        g = _ShPoly([(e - vx * A["half"], n - vy * A["half"]), (pe - vx * B["half"], pn - vy * B["half"]),
                                     (qe - vx * B["half"], qn - vy * B["half"])]).buffer(0)
                        if not g.is_empty:
                            fills.append(g)
        shoulder_zone = SHOULDER_M + VERGE_M + 0.5   # an edge line sits this far inside its pavement edge
        for L in road_legs:
            wl = ways[L["wi"]]
            t = tangent_at(wl, L["s0"])
            if L["end"]:
                # a way that ends here: every line inside the other legs' pavement goes,
                # and its edge line on the side of the carriageway it merges into goes
                # within the shoulder zone too (a merging lane has no line of its own there)
                others = [q for M in road_legs if M is not L for q in M["quads"]] + fills
                if others:
                    wl["cutters"].append((_sh_prepared.prep(_sh_union(others).buffer(0)), None, "drop"))
                for M in road_legs:
                    if M is L or M["wi"] == L["wi"] or not M["quads"] or M["end"]:
                        continue
                    ref = M["slice"][min(len(M["slice"]) - 1, 2)]
                    left = -(ref[0] - centre[0]) * t[1] + (ref[1] - centre[1]) * t[0]   # > 0: left of travel
                    wl["cutters"].append((_sh_prepared.prep(_sh_union(M["quads"] + fills).buffer(shoulder_zone)), -1 if left > 0 else 1, "drop"))
            else:
                # a through leg: each other leg's pavement turns the edge line on its
                # side into dashes (the acceleration lane's separation), widened by the
                # hard shoulder because that edge line sits SHOULDER_M inside the edge
                for M in road_legs:
                    if M is L or M["wi"] == L["wi"] or not M["quads"]:
                        continue
                    ref = M["slice"][min(len(M["slice"]) - 1, 2)]
                    left = -(ref[0] - centre[0]) * t[1] + (ref[1] - centre[1]) * t[0]   # > 0: left of travel
                    wl["cutters"].append((_sh_prepared.prep(_sh_union(M["quads"] + fills).buffer(shoulder_zone)), -1 if left > 0 else 1, "dash"))
        poly = _sh_union([q for L in road_legs for q in L["quads"]] + fills).buffer(0)
        if not poly.is_empty:
            patches.append({"poly": poly, "legs": road_legs})
        if os.environ.get("PVP_DEBUG_NODE"):
            de, dn = (float(v) for v in os.environ["PVP_DEBUG_NODE"].split(","))
            if math.hypot(centre[0] - de, centre[1] - dn) < 60:
                print(f"DEBUG node {centre}: " + "; ".join(f"way {ways[L['wi']]['w']['id']} {L['cls']} end={L['end']} fwd={L['fwd']} cut={L['cut']:.1f} half={L['half']}" for L in road_legs)
                      + f" | fills {len(fills)} | cutters " + ", ".join(f"{ways[L['wi']]['w']['id']}:{[(c[1], c[2]) for c in ways[L['wi']]['cutters']]}" for L in road_legs))
    # patches that overlap and share a way are one pavement (TLM puts the
    # split and the merge of a ramp a few metres apart)
    merged = []
    if patches:
        tree = _ShTree([p["poly"] for p in patches])
        parent = list(range(len(patches)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        for i, p in enumerate(patches):
            wis = set(L["wi"] for L in p["legs"])
            for j in tree.query(p["poly"]):
                j = int(j)
                if j <= i or not (wis & set(L["wi"] for L in patches[j]["legs"])):
                    continue
                if patches[j]["poly"].intersects(p["poly"]):
                    parent[find(i)] = find(j)
        groups = defaultdict(list)
        for i in range(len(patches)):
            groups[find(i)].append(i)
        for idxs in groups.values():
            poly = patches[idxs[0]]["poly"] if len(idxs) == 1 else _sh_union([patches[i]["poly"] for i in idxs]).buffer(0)
            merged.append((poly, [L for i in idxs for L in patches[i]["legs"]]))
    station_index = StationIndex(ways)

    # Stage 4b: ribbons (plain pavement) in the pieces between the junction
    # gaps, markings, embankments, structures.
    for wi, wy in enumerate(ways):
        w, width = wy["w"], wy["width"]
        colour, kind, lanes, gauge, ballast, tracks = wy["colour"], wy["kind"], wy["lanes"], wy["gauge"], wy["ballast"], wy["tracks"]
        total = wy["s"][-1]
        gaps = sorted(wy["gaps"])
        pieces, cursor = [], 0.0
        for g0, g1 in gaps:
            if g0 > cursor + 0.5:
                pieces.append((cursor, g0))
            cursor = max(cursor, g1)
        if total > cursor + 0.5:
            pieces.append((cursor, total))
        if not pieces:
            stats["ways_inside_junctions"] += 1
        ef = wy["end_frames"]
        on_ground = not (w["tunnel"] or w["bridge"] or w.get("elevated"))
        if w.get("has_z") and w.get("elevated"):
            stats["surveyed_bridges"] += 1
        road_lift = ROAD_LIFT_M - CLASS_DROP_M.get(w["cls"], 0.0)
        ballast_lift = RUNTIME_DROP_M + BALLAST_LIFT_M + (w["id"] % 5) * 0.003
        if not w.get("design"):
            # the way's source geometry for the scenery editor's Pick tool (a
            # sidecar per tile; the same way may appear in several tiles):
            # the full way to its nodes, not the ribbon trimmed at junctions
            full = wy["pts"]
            tag_w = parse_num(w.get("width"), 0)
            rec = {"id": w["id"], "kind": w["kind"], "cls": w["cls"], "lanes": lanes or 0, "oneway": bool(w["oneway"]),
                   "width": tag_w if 3.0 <= tag_w <= 60.0 else 0,
                   "gauge": int(round(gauge * 1000)) if gauge else 0, "tracks": tracks or 0,
                   "bridge": bool(w["bridge"] or w.get("elevated")), "tunnel": bool(w["tunnel"]),
                   "pts": [[round(e, 1), round(n, 1)] for e, n in full[::2]] + ([[round(full[-1][0], 1), round(full[-1][1], 1)]] if len(full) % 2 == 0 else [])}
            for k in {(int(math.floor(e / TILE_M)) * TILE_M, int(math.floor(n / TILE_M)) * TILE_M) for e, n in full}:
                sidecars[k].append(rec)
        cutters = wy["cutters"]
        # the lane taper of this way: from its design graph (Design._ways) or
        # a width change of the baked network at one of its nodes (stage 4a)
        taper = [t if t is not None else u for t, u in zip(wy.get("taper") or (None, None), w.get("taper") or (None, None))]
        half_at = taper_profile(width * 0.5, taper, total) if w["kind"] == "road" else None
        if w["kind"] == "road" and len(wy["pts"]) >= 2 and not w["tunnel"]:
            # markings over the whole way (through the patches), by tile
            full_p, full_h = wy["pts"], list(wy["prof"])
            by_tile_m: dict = defaultdict(list)
            for i, (e, n) in enumerate(full_p):
                by_tile_m[(int(math.floor(e / TILE_M)) * TILE_M, int(math.floor(n / TILE_M)) * TILE_M)].append(i)
            for k, idxs in by_tile_m.items():
                lo, hi = max(0, idxs[0] - 1), min(len(full_p) - 1, idxs[-1] + 1)
                seg = full_p[lo:hi + 1]
                if len(seg) < 2:
                    continue
                xz = [(CONV_E - e, n - CONV_N) for e, n in seg]
                s = along(xz)
                ys = [h + road_lift for h in full_h[lo:hi + 1]]
                buf = tiles[k]
                buf.current_way = 0 if w.get("design") else int(w["id"])
                stats["markings"] += emit_markings(buf, xz, ys, s, width * 0.5, kind, lanes,
                                                   (ef[0] if lo == 0 else None, ef[1] if hi == len(full_p) - 1 else None), cutters,
                                                   [half_at(sv) for sv in wy["s"][lo:hi + 1]] if half_at else None)
        for (pa, pb) in pieces:
            pts, prof = way_piece(wy, pa, pb)
            if len(pts) < 2:
                continue
            prof = np.asarray(prof, dtype=float)
            piece_ef = (ef[0] if pa <= 0.0 else None, ef[1] if pb >= total else None)
            # the lane taper by chainage along the whole way (pa + the piece's own)
            halves_full = [half_at(pa + sv) for sv in along(pts)] if half_at else None
            # split into 1 km tiles (a segment crossing a boundary is kept in both tiles' ribbons)
            by_tile: dict = defaultdict(list)
            for i, (e, n) in enumerate(pts):
                k = (int(math.floor(e / TILE_M)) * TILE_M, int(math.floor(n / TILE_M)) * TILE_M)
                by_tile[k].append(i)
            for k, idxs in by_tile.items():
                lo, hi = max(0, idxs[0] - 1), min(len(pts) - 1, idxs[-1] + 1)
                seg = pts[lo:hi + 1]
                seg_h = prof[lo:hi + 1]
                if len(seg) < 2:
                    continue
                xz = [(CONV_E - e, n - CONV_N) for e, n in seg]
                s = along(xz)
                frames_end = (piece_ef[0] if lo == 0 else None, piece_ef[1] if hi == len(pts) - 1 else None)
                buf = tiles[k]
                buf.current_way = 0 if w.get("design") else int(w["id"])
                cu = tuple(int(c * 255) for c in (colour if w["kind"] == "road" else (0.5, 0.48, 0.45)))
                if w["kind"] == "road":
                    ys = [h + road_lift for h in seg_h]
                    halves_seg = halves_full[lo:hi + 1] if halves_full else None
                    buf.ribbon(xz, ys, width, cu, KIND_JUNCTION, 0.0, s, float(lanes), halves_seg, frames_end)
                    stats["road_m"] += s[-1]
                    if on_ground:
                        stats["skirt_m"] += emit_skirts(buf, xz, seg_h, s, width * 0.5, road_lift, terrain, frames_end, halves_seg)
                else:
                    ys_b = [h + ballast_lift for h in seg_h]
                    if ballast:
                        buf.ribbon(xz, ys_b, width, cu, KIND_BALLAST, 0.0, s, float(tracks), None, frames_end)
                        if on_ground:
                            stats["skirt_m"] += emit_skirts(buf, xz, seg_h, s, width * 0.5, ballast_lift, terrain, frames_end)
                    rail_c = (120, 118, 110)
                    for tr in range(tracks):
                        centre = (tr - (tracks - 1) / 2.0) * TRACK_SPACING_M
                        for side in (-1, 1):
                            buf.profile(xz, ys_b, RAIL_PROFILE, rail_c, KIND_RAIL, centre + side * gauge / 2.0, s, RAIL_FOOT_M / 2, 1.0, None, frames_end)
                    stats["rail_m"] += s[-1]
                if w.get("elevated") and w["kind"] == "road":
                    # bridge structure: parapets along both edges, a pier every
                    # PIER_SPACING_M where the deck is clear of the ground and
                    # nothing passes underneath
                    ys = [h + road_lift for h in seg_h]
                    half_e = width * 0.5
                    for side in (-1.0, 1.0):
                        buf.profile(xz, ys, ((-PARAPET_W_M / 2, 0.0), (-PARAPET_W_M / 2, PARAPET_H_M), (PARAPET_W_M / 2, PARAPET_H_M), (PARAPET_W_M / 2, 0.0)),
                                    CONCRETE_U8, KIND_CONCRETE, side * (half_e - PARAPET_W_M / 2), s, 0.0, 0.0, None, frames_end)
                    since = PIER_SPACING_M if lo == 0 else 0.0
                    for i in range(len(xz)):
                        if i > 0:
                            since += s[i] - s[i - 1]
                        if since < PIER_SPACING_M:
                            continue
                        since = 0.0
                        x, z = xz[i]
                        q = xz[min(i + 1, len(xz) - 1)] if i < len(xz) - 1 else xz[i - 1]
                        dx, dz = q[0] - x, q[1] - z
                        l = math.hypot(dx, dz) or 1.0
                        dx, dz = dx / l, dz / l
                        nx, nz = -dz, dx
                        wpier = max(width - 2.0, PIER_W_M)
                        t = PIER_W_M / 2
                        ground = terrain.sample(seg[i][0], seg[i][1], tri=True)
                        if math.isnan(ground):
                            continue
                        top, bottom = ys[i] - ROAD_LIFT_M, ground - 0.5 + RUNTIME_DROP_M
                        if top - ground < PIER_MIN_H_M:
                            stats["piers_low"] += 1
                            continue
                        if way_under(station_index, ways, wi, seg[i][0], seg[i][1], float(seg_h[i]), wpier * 0.5 + 0.5):
                            stats["piers_over_ways"] += 1
                            continue
                        c = [(x + nx * wpier / 2 * sx + dx * t * sz, z + nz * wpier / 2 * sx + dz * t * sz) for sx, sz in ((-1, -1), (-1, 1), (1, 1), (1, -1))]
                        for k2 in range(4):
                            buf.wall(c[k2], c[(k2 + 1) % 4], bottom, top, CONCRETE_U8, KIND_CONCRETE)
                        stats["piers"] += 1
    # junction patches: one pavement per node cluster at the height of the
    # nearest leg station, PATCH_LIFT_M over the ribbons
    for poly, legs in merged:
        st = [(e, n, h) for L in legs for (e, n), h in zip(L["slice"], L["slice_h"])]
        if not st:
            continue
        best_cls = min((L["cls"] for L in legs), key=lambda c: CLASS_DROP_M.get(c, 0.0))
        lift = ROAD_LIFT_M - CLASS_DROP_M.get(best_cls, 0.0) + PATCH_LIFT_M
        SE = np.array([[e, n] for e, n, _h in st])
        SH = np.array([h for _e, _n, h in st])
        colour = ROAD_CLASSES[best_cls][1] if best_cls in ROAD_CLASSES else (0.34, 0.34, 0.35)
        cu = tuple(int(c * 255) for c in colour)
        geoms = list(poly.geoms) if poly.geom_type == "MultiPolygon" else [poly]
        widest = max(legs, key=lambda L: L["half"])
        for g in geoms:
            if g.is_empty or g.area < 0.5:
                continue
            tris = patch_triangles(g)
            cache = {}

            def height_at(e, n):
                key = (round(e, 2), round(n, 2))
                if key not in cache:
                    d2 = (SE[:, 0] - e) ** 2 + (SE[:, 1] - n) ** 2
                    cache[key] = float(SH[int(np.argmin(d2))])
                return cache[key]

            tris3 = []
            for t in tris:
                tris3.append(tuple((CONV_E - e, height_at(e, n) + lift, n - CONV_N) for e, n in t))
            ce, cn = g.centroid.x, g.centroid.y
            k = (int(math.floor(ce / TILE_M)) * TILE_M, int(math.floor(cn / TILE_M)) * TILE_M)
            tiles[k].current_way = 0 if ways[widest["wi"]]["w"].get("design") else int(ways[widest["wi"]]["w"]["id"])
            tiles[k].tri_mesh(tris3, cu, KIND_JUNCTION)
            stats["patches"] += 1
            stats["patch_m2"] += g.area
    # junction pavements of the designs: one flat fan per node on the final ground
    for d in designs:
        for j in d.junctions:
            ce, cn = j["centre"]
            hs = [terrain.sample(ce, cn, tri=True)] + [terrain.sample(e, n, tri=True) for e, n in j["poly"]]
            hs = [h for h in hs if not math.isnan(h)]
            if not hs:
                continue
            y = max(hs) + ROAD_LIFT_M - CLASS_DROP_M.get(j["cls"], 0.0) + 0.005
            k = (int(math.floor(ce / TILE_M)) * TILE_M, int(math.floor(cn / TILE_M)) * TILE_M)
            colour = ROAD_CLASSES[j["cls"]][1] if j["cls"] in ROAD_CLASSES else (0.34, 0.34, 0.35)
            cu = tuple(int(c * 255) for c in colour)
            tiles[k].current_way = 0
            tiles[k].fan((CONV_E - ce, cn - CONV_N), [(CONV_E - e, n - CONV_N) for e, n in j["poly"]], y, cu, KIND_JUNCTION)
            stats["junctions"] += 1
        # areas: flat patches on the highest ground (water on the lowest), buildings as roof + walls
        for poly, typ, hb in d.areas:
            hs = [terrain.sample(e, n) for e, n in poly]
            hs = [h for h in hs if not math.isnan(h)]
            if not hs:
                continue
            ce = sum(p[0] for p in poly) / len(poly)
            cn = sum(p[1] for p in poly) / len(poly)
            k = (int(math.floor(ce / TILE_M)) * TILE_M, int(math.floor(cn / TILE_M)) * TILE_M)
            xz = [(CONV_E - e, n - CONV_N) for e, n in poly]
            tiles[k].current_way = 0
            if typ == "building":
                top = max(hs) + hb + RUNTIME_DROP_M
                tiles[k].polygon(xz, top, (128, 128, 128), KIND_ROOF)
                for i in range(len(xz)):
                    tiles[k].wall(xz[i], xz[(i + 1) % len(xz)], min(hs) - 0.5 + RUNTIME_DROP_M, top, (128, 128, 128), KIND_WALL)
            else:
                y = (min(hs) + 0.02 if typ == "water" else max(hs) + 0.06) + RUNTIME_DROP_M
                tiles[k].polygon(xz, y, (128, 128, 128), AREA_KIND[typ])
            stats["areas"] += 1
    if args.dry_run:
        print("dry run:", dict(stats), "tiles", len(tiles), "corridor px", corridor_px)
        return
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"schema_version": 1, "region": args.region, "built": time.strftime("%Y-%m-%d"),
                "source": ("swisstopo swissTLM3D (open data), build_world_roads.py" if args.source == "tlm" else "OpenStreetMap contributors (ODbL 1.0) via Geofabrik, build_world_roads.py"),
                "conversion_origin_e": CONV_E, "conversion_origin_n": CONV_N,
                "elevation_baked": True, "shader": "roads", "tile_size_m": TILE_M, "tiles": {}}
    mpath = out_dir / "manifest.json"
    if mpath.exists():   # merge with an earlier partial bake (other boxes)
        try:
            old = json.load(open(mpath))
            if old.get("conversion_origin_e") == CONV_E:
                manifest["tiles"] = old.get("tiles", {})
        except Exception:
            pass
    for (te, tn), buf in tiles.items():
        if not buf.idx:
            continue
        name = f"roads_{te}_{tn}.glb"
        write_glb(out_dir / name, buf)
        if sidecars.get((te, tn)):
            json.dump(sidecars[(te, tn)], open(out_dir / f"roads_{te}_{tn}.ways.json", "w"), separators=(",", ":"))
        manifest["tiles"][f"{te}_{tn}"] = {"file": name, "center_e": te + TILE_M // 2, "center_n": tn + TILE_M // 2,
                                          "segments": len(buf.idx) // 3}
    manifest["tile_count"] = len(manifest["tiles"])
    manifest["ways_sidecar"] = True   # roads_E_N.ways.json next to each tile: source ways for the editor's Pick tool
    # designs folded into these tiles: the runtime skips their live meshes
    baked = {}
    if mpath.exists():
        try:
            for rec in json.load(open(mpath)).get("designs", []):
                baked[rec["path"]] = rec
        except Exception:
            pass
    for d in designs:
        baked[str(d.path)] = {"path": str(d.path), "sha256": d.sha256, "built": manifest["built"]}
    manifest["designs"] = list(baked.values())
    json.dump(manifest, open(mpath, "w"), indent=1)
    n_terrain = terrain.flush() if args.corridor else 0
    size = sum((out_dir / t["file"]).stat().st_size for t in manifest["tiles"].values())
    print(f"roads: {len(tiles)} tiles, {size / 1e6:.1f} MB, road {stats['road_m'] / 1000:.0f} km, rail {stats['rail_m'] / 1000:.0f} km, "
          f"no-terrain ways {stats['no_terrain']}; bridges lifted {stats['bridges_lifted']} over {stats['bridge_crossings']} crossings, "
          f"ramps {stats['ramps']}, tunnels {stats['tunnels']} (+{stats['underpasses']} underpasses); "
          f"surveyed heights {stats['ways_with_heights']} ways, surveyed bridges {stats['surveyed_bridges']}; "
          f"network: one-way flips {stats['oriented_flips']}, blocks: entry {stats['blocks_entry']}, exit {stats['blocks_exit']}, lane add {stats['blocks_lane_add']}, lane drop {stats['blocks_lane_drop']} ({stats['block_pave_m2'] / 1e4:.1f} ha); pavement patches {stats['patches']} ({stats['patch_m2'] / 1e4:.1f} ha, {stats['attached_ends']} attached ends), mitred joins {stats['mitres']} ({stats['width_tapers']} width changes tapered), ways inside patches {stats['ways_inside_junctions']}, marking pieces {stats['markings']}, embankments {stats['skirt_m'] / 1000:.0f} km, "
          f"piers {stats['piers']} (skipped: {stats['piers_low']} low decks, {stats['piers_over_ways']} over ways); "
          f"designs {len(designs)} (junctions {stats['junctions']}, OSM ways cut {stats['osm_ways_cut']}, replaced by id {stats['osm_ways_replaced']}, plateaus {stats['plateaus']} = {stats['flatten_px']} px, areas {stats['areas']}); corridor: {corridor_px} px, {n_terrain} terrain tiles written ({stats['terrain_reset']} reset from terrain_base) -> {out_dir}")


if __name__ == "__main__":
    main()
