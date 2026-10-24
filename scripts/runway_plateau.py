#!/usr/bin/env python3
"""
runway_plateau.py — flatten the baked base terrain under runways (design §12,
D18). Used by `build_world_terrain.py flatten --region <id>`.

For every runway of <pack>/airports/runways.json:

  1. the touched 1024 m tiles are re-baked from SRTM (idempotent: a second
     `flatten` starts from the same source, not from an already-blended tile);
  2. a runway plane is fitted: linear between the two end elevations. The
     OurAirports end elevations are used when both exist and each lies within
     ELEV_TRUST_M of the terrain there; otherwise the plane is the least-
     squares line through terrain samples along the centreline;
  3. every pixel inside the strip (width + APRON_M around, OVERRUN_M beyond
     each end) is set to the plane; pixels within BLEND_M outside the strip
     are blended (smoothstep) from the plane to the original terrain.

The pixel lattice matches the mesh (`build_world_terrain._bake_cell`):
E = e_sw + (px-1-c)·sp, N = n_sw + r·sp. Coordinates are LV95 via PROJ.
A per-region report lists cut/fill per runway.
"""

import json
import math
from pathlib import Path

import numpy as np

ELEV_TRUST_M = 15.0     # OurAirports end elevation vs terrain at that end
APRON_M = 12.0          # extra flat width each side of the pavement
OVERRUN_M = 40.0        # extra flat length beyond each pavement end
BLEND_M = 80.0          # smoothstep blend from the plateau to the terrain
SAMPLE_STEP_M = 25.0    # centreline sampling for the terrain fit
APRON_BLEND_M = 40.0    # blend from an apron plane to the terrain
APRON_MAX_SLOPE = 0.03  # aprons are fitted as planes, capped at 3 %
TAXI_BLEND_M = 30.0     # blend from a taxiway ribbon to the terrain
TAXI_SMOOTH_M = 60.0    # taxiway profile: mean of the terrain within ±60 m along the path
TAXI_EDGE_M = 3.0       # flat margin beside the taxiway pavement
TAXI_MESH_MAX_EDGE_M = 24.0   # subdivide pavement triangles longer than this
TAXI_NODE_M = 20.0      # ribbon vertices at most this far apart (the ribbon
                        # must follow the terrain profile between OSM nodes)
# Airport datum: taxiway and apron heights near a runway follow the runway
# planes (surveyed end elevations), not the 30 m SRTM, which at Zurich ran
# 5-7 m under the runway 28 end and produced a ramp up to the runway. Full
# runway weight within DATUM_NEAR_M of a runway, fading to SRTM at
# DATUM_FAR_M; between runways the planes are inverse-distance weighted.
DATUM_NEAR_M = 300.0
DATUM_FAR_M = 1200.0
# Aprons and taxiways are flattened out to one pixel spacing past their
# outline (31 m on 34 px tiles): every pixel the mesh edge is bilinearly
# sampled from is then inside the flat zone, not a blended pixel (with half
# a pixel Zurich apron edges were still 1.5 m off on steep ground).
DILATE_FRAC = 1.0


def _smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


class TileStore:
    """Read/modify/write of the baked 34 px tiles of one region."""

    def __init__(self, region, base_only=False):
        self.region = region
        # base_only: read and write terrain_base alone (a region whose terrain
        # already carries a road bake that must stay as it is)
        self.base_only = base_only
        self.tm = region.tile_m
        self.px = region.px
        self.sp = region.px_spacing
        self.tiles = {}       # (ei, ni) -> np.ndarray (px, px) float32, row 0 = south, col 0 = east

    def path(self, ei, ni):
        root = self.region.terrain_base if self.base_only else self.region.terrain_out
        return root / f"tile_{ei * self.tm}_{ni * self.tm}.raw"

    def get(self, ei, ni):
        key = (ei, ni)
        if key not in self.tiles:
            p = self.path(ei, ni)
            if not p.exists():
                return None
            self.tiles[key] = np.fromfile(p, dtype="<f4").reshape(self.px, self.px).copy()
        return self.tiles[key]

    def sample(self, e, n):
        """Bilinear terrain sample at LV95 (e, n); NaN outside the data."""
        ei, ni = int(math.floor(e / self.tm)), int(math.floor(n / self.tm))
        a = self.get(ei, ni)
        if a is None:
            return float("nan")
        c = (self.tm - (e - ei * self.tm)) / self.sp        # col 0 = east edge
        r = (n - ni * self.tm) / self.sp
        c0, r0 = int(math.floor(c)), int(math.floor(r))
        c0 = min(max(c0, 0), self.px - 2)
        r0 = min(max(r0, 0), self.px - 2)
        fc, fr = min(max(c - c0, 0.0), 1.0), min(max(r - r0, 0.0), 1.0)
        v = (a[r0, c0] * (1 - fc) * (1 - fr) + a[r0, c0 + 1] * fc * (1 - fr)
             + a[r0 + 1, c0] * (1 - fc) * fr + a[r0 + 1, c0 + 1] * fc * fr)
        return float(v)

    def pixel_coords(self, ei, ni):
        cc = np.arange(self.px)
        rr = np.arange(self.px)
        E = ei * self.tm + (self.px - 1 - cc[None, :]) * self.sp
        N = ni * self.tm + rr[:, None] * self.sp
        return np.broadcast_to(E, (self.px, self.px)), np.broadcast_to(N, (self.px, self.px))

    def write_all(self):
        """Write to the region's terrain and, when the region has one, to
        terrain_base: the airport plateaus are part of the pristine ground
        the road bake derives terrain from (build_world_terrain.py)."""
        n = 0
        base = getattr(self.region, "terrain_base", None)
        if base is not None:
            base.mkdir(parents=True, exist_ok=True)
        for (ei, ni), a in self.tiles.items():
            out = a.astype("<f4")
            out.tofile(self.path(ei, ni))
            if base is not None and not self.base_only:
                out.tofile(base / self.path(ei, ni).name)
            n += 1
        return n


def bbox_tiles(region, emin, nmin, emax, nmax, pad):
    tm = region.tile_m
    emin, emax, nmin, nmax = emin - pad, emax + pad, nmin - pad, nmax + pad
    out = []
    for ei in range(int(math.floor(emin / tm)), int(math.floor(emax / tm)) + 1):
        for ni in range(int(math.floor(nmin / tm)), int(math.floor(nmax / tm)) + 1):
            out.append((ei, ni))
    return out


def runway_tiles(region, e0, n0, e1, n1, half_w):
    """Tile indices whose square intersects the strip's bounding box + blend."""
    pad = half_w + APRON_M + BLEND_M + OVERRUN_M + region.px_spacing
    tm = region.tile_m
    emin, emax = min(e0, e1) - pad, max(e0, e1) + pad
    nmin, nmax = min(n0, n1) - pad, max(n0, n1) + pad
    out = []
    for ei in range(int(math.floor(emin / tm)), int(math.floor(emax / tm)) + 1):
        for ni in range(int(math.floor(nmin / tm)), int(math.floor(nmax / tm)) + 1):
            out.append((ei, ni))
    return out


def runways_json_path(region):
    return region.terrain_out.parent / "airports" / "runways.json"


def write_plane_heights(region, data, strips):
    """Store the plateau plane in runways.json (ends[i].plane_elev_m,
    runway.plane_source) so the runtime draws the strip exactly where the
    terrain was flattened without sampling terrain."""
    by_key = {(s["icao"], s["designators"]): s for s in strips if "plane" in s}
    store = TileStore(region)
    for ap in data.get("airports", []):
        if ap["icao"] in _apron_lists:
            ap["aprons"] = [e for e in _apron_lists[ap["icao"]] if e["polygon"] and e["polygon"][0] is not None]
        # Mapped light fixtures: terrain height under each (after the plateaus).
        for lt in ap.get("lights", []):
            try:
                x, y = _to_lv95(float(lt["lat"]), float(lt["lon"]))
                z = store.sample(x, y)
                if math.isfinite(z) and z != 0.0:
                    lt["elev_m"] = round(float(z), 2)
            except Exception:  # noqa: BLE001
                pass
        for rw in ap.get("runways", []):
            key = (ap["icao"], "/".join(rw.get("designators", [])))
            s = by_key.get(key)
            if s is None or len(rw.get("ends", [])) != 2:
                continue
            z0, z1, src = s["plane"]
            rw["ends"][0]["plane_elev_m"] = z0
            rw["ends"][1]["plane_elev_m"] = z1
            rw["plane_source"] = src
            if s.get("crossings"):
                rw["crossings"] = s["crossings"]
            else:
                rw.pop("crossings", None)
    clean = {k: v for k, v in data.items() if not k.startswith("_")}
    with open(runways_json_path(region), "w") as f:
        json.dump(clean, f, indent=1, ensure_ascii=False)


def _foreign_airports(region, own_icaos):
    """Airports of the OTHER regions' runways.json whose runways touch this
    region's tiles. Packs overlap (bbox packs, the 5 km country margins),
    and at runtime either pack's tile may win, so every pack's tiles must
    carry the same plateaus (ENGM sat 6 m under its runway: Sweden's raw
    tiles won over Norway's flattened ones, 2026-09-20)."""
    out = []
    build_root = Path(region.cfg["build_root"])
    for e in region.cfg.get("regions", []):
        if e["id"] == region.id:
            continue
        rj = build_root / e["id"] / e.get("subdir", e["name"]) / "airports" / "runways.json"
        if not rj.exists():
            continue
        with open(rj) as f:
            other = json.load(f)
        for ap in other.get("airports", []):
            if ap.get("icao") in own_icaos:
                continue
            # cheap test: does the tile under the reference point exist here?
            from pyproj import Transformer
            tr = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
            x, y = tr.transform(float(ap.get("lon", 0.0)), float(ap.get("lat", 0.0)))
            tm = region.tile_m
            near = False
            for dx in (-2, 0, 2):
                for dy in (-2, 0, 2):
                    ei, ni = int(x // tm) + dx, int(y // tm) + dy
                    if (region.terrain_out / f"tile_{ei * tm}_{ni * tm}.raw").exists():
                        near = True
            if near:
                ap = json.loads(json.dumps(ap))      # private copy, never written back
                ap["_foreign"] = True
                out.append(ap)
                own_icaos.add(ap["icao"])
    return out


def plan_flatten(region, wgs84_to_lv95):
    """Runway strips in LV95 for the region's runways.json (or None)."""
    rj = runways_json_path(region)
    if not rj.exists():
        return None, []
    with open(rj) as f:
        data = json.load(f)
    strips = []
    aprons = []
    taxiways = []
    own = {ap.get("icao") for ap in data.get("airports", [])}
    foreign = _foreign_airports(region, own)
    data["_foreign_count"] = len(foreign)
    for ap in list(data.get("airports", [])) + foreign:
        for apn in ap.get("aprons", []):
            poly = apn.get("polygon", [])
            if len(poly) < 4:
                continue
            pts = [wgs84_to_lv95(float(p[0]), float(p[1])) for p in poly]
            aprons.append({"icao": ap["icao"], "pts": pts, "ref": apn})
        for tw in ap.get("taxiways", []):
            path = tw.get("path", [])
            if len(path) < 2:
                continue
            pts = [wgs84_to_lv95(float(p[0]), float(p[1])) for p in path]
            taxiways.append({"icao": ap["icao"], "pts": pts, "half_w": 0.5 * float(tw.get("width_m", 23.0)) + TAXI_EDGE_M, "ref": tw})
        for rw in ap.get("runways", []):
            ends = rw.get("ends", [])
            if len(ends) != 2 or "pavement" not in ends[0] or "pavement" not in ends[1]:
                continue
            (la0, lo0), (la1, lo1) = ends[0]["pavement"], ends[1]["pavement"]
            e0, n0 = wgs84_to_lv95(la0, lo0)
            e1, n1 = wgs84_to_lv95(la1, lo1)
            length = math.hypot(e1 - e0, n1 - n0)
            if length < 50.0:
                continue
            strips.append({
                "icao": ap["icao"],
                "designators": "/".join(rw.get("designators", [])),
                "e0": e0, "n0": n0, "e1": e1, "n1": n1, "length": length,
                "half_w": 0.5 * float(rw.get("width_m", 45.0)),
                "elev0": ends[0].get("elev_m"),
                "elev1": ends[1].get("elev_m"),
            })
    data["_aprons"] = aprons
    data["_taxiways"] = taxiways
    return data, strips


def _seg_closest(a, b):
    """Closest points of two centreline segments (LV95): returns
    (dist, ta, tb, Pa, Pb) with t in [0,1] along each segment."""
    p = np.array([a["e0"], a["n0"]]); r = np.array([a["e1"] - a["e0"], a["n1"] - a["n0"]])
    q = np.array([b["e0"], b["n0"]]); s_ = np.array([b["e1"] - b["e0"], b["n1"] - b["n0"]])
    rxs = r[0] * s_[1] - r[1] * s_[0]
    qp = q - p
    if abs(rxs) > 1e-9:
        ta = (qp[0] * s_[1] - qp[1] * s_[0]) / rxs
        tb = (qp[0] * r[1] - qp[1] * r[0]) / rxs
        if 0.0 <= ta <= 1.0 and 0.0 <= tb <= 1.0:
            P = p + r * ta
            return 0.0, ta, tb, P, P
    # No proper crossing: sample the closest approach.
    best = None
    for ta in np.linspace(0.0, 1.0, 65):
        Pa = p + r * ta
        w = Pa - q
        tb = float(np.clip(np.dot(w, s_) / max(np.dot(s_, s_), 1e-9), 0.0, 1.0))
        Pb = q + s_ * tb
        d = float(np.hypot(*(Pa - Pb)))
        if best is None or d < best[0]:
            best = (d, float(ta), tb, Pa, Pb)
    return best


def _plane_z(s, t):
    return s["z0"] + (s["z1"] - s["z0"]) * t


def fit_planes(strips, store, report_lines):
    """Per-strip plane (z0, z1, src) from the end elevations or the terrain."""
    for s in strips:
        ux, uy = (s["e1"] - s["e0"]) / s["length"], (s["n1"] - s["n0"]) / s["length"]
        s["ux"], s["uy"] = ux, uy
        ts = np.arange(0.0, s["length"] + 1e-6, SAMPLE_STEP_M)
        zs = np.array([store.sample(s["e0"] + ux * t, s["n0"] + uy * t) for t in ts])
        ok = np.isfinite(zs) & (zs != 0.0)
        if ok.sum() < 2:
            s["skip"] = True
            report_lines.append(f"  {s['icao']} {s['designators']}: no terrain data — skipped")
            continue
        slope, icpt = np.polyfit(ts[ok], zs[ok], 1)
        z0, z1 = icpt, icpt + slope * s["length"]
        z_end0 = store.sample(s["e0"], s["n0"])
        z_end1 = store.sample(s["e1"], s["n1"])
        src = "terrain_fit"
        if (s["elev0"] is not None and s["elev1"] is not None
                and math.isfinite(z_end0) and math.isfinite(z_end1)
                and abs(s["elev0"] - z_end0) <= ELEV_TRUST_M
                and abs(s["elev1"] - z_end1) <= ELEV_TRUST_M):
            z0, z1 = float(s["elev0"]), float(s["elev1"])
            src = "ourairports"
        s["z0"], s["z1"], s["src"] = float(z0), float(z1), src
        s["_ts"], s["_zs"] = ts[ok], zs[ok]
        # flat zone one pixel past the margin: the bilinear terrain at the
        # pavement edge then only sees plane pixels (was 11 cm high at LSZH)
        s["dilate"] = DILATE_FRAC * store.sp


def reconcile_crossings(strips, report_lines):
    """Runways of one airport that cross (or meet) must agree in height at
    the crossing: the longer runway keeps its plane, the shorter one is
    shifted so both planes coincide there. Real crossing runways share the
    pavement, so a mismatch is an error of the source elevations."""
    by_icao = {}
    for s in strips:
        if not s.get("skip"):
            by_icao.setdefault(s["icao"], []).append(s)
    for icao, group in by_icao.items():
        group.sort(key=lambda s: -s["length"])
        for i, a in enumerate(group):
            for b in group[i + 1:]:
                d, ta, tb, Pa, Pb = _seg_closest(a, b)
                if d > a["half_w"] + b["half_w"] + 2.0 * APRON_M:
                    continue
                # Marking cut (like the taxiway cut): along-track interval of
                # the shorter runway b that lies inside a's pavement.
                from shapely.geometry import Polygon
                def rect(st):
                    ux, uy, hw = st["ux"], st["uy"], st["half_w"]
                    rx, ry = uy, -ux
                    return Polygon([(st["e0"] + rx * hw, st["n0"] + ry * hw), (st["e1"] + rx * hw, st["n1"] + ry * hw),
                                    (st["e1"] - rx * hw, st["n1"] - ry * hw), (st["e0"] - rx * hw, st["n0"] - ry * hw)])
                inter = rect(a).intersection(rect(b))
                if not inter.is_empty and inter.area > 1.0:
                    ts_ = [((x - b["e0"]) * b["ux"] + (y - b["n0"]) * b["uy"]) for x, y in inter.exterior.coords]
                    b.setdefault("crossings", []).append([round(max(0.0, min(ts_)), 1), round(min(b["length"], max(ts_)), 1)])
                dz = _plane_z(a, ta) - _plane_z(b, tb)
                if abs(dz) < 0.05:
                    continue
                b["z0"] += dz
                b["z1"] += dz
                b["src"] = b["src"] + "+crossing"
                report_lines.append(
                    f"  {icao} {b['designators']}: shifted {dz:+.1f} m to meet "
                    f"{a['designators']} at the crossing")


def apply_plateaus(region, strips, store, report_lines):
    """Flatten every strip into the store's tiles (jointly, so crossing and
    neighbouring runways never overwrite each other); returns the tile set."""
    fit_planes(strips, store, report_lines)
    reconcile_crossings(strips, report_lines)
    live = [s for s in strips if not s.get("skip")]
    for s in live:
        s["plane"] = (round(s["z0"], 1), round(s["z1"], 1), s["src"])
        plane_at = _plane_z(s, s["_ts"] / s["length"])
        cut = float(np.max(s["_zs"] - plane_at))
        fill = float(np.max(plane_at - s["_zs"]))
        report_lines.append(
            f"  {s['icao']} {s['designators']}: {s['length']:.0f} m, plane {s['z0']:.1f} → {s['z1']:.1f} m "
            f"({s['src']}), terrain along centreline {s['_zs'].min():.1f}…{s['_zs'].max():.1f} m, "
            f"max cut {cut:.1f} m, max fill {fill:.1f} m")

    aprons = fit_aprons(region, store, live, data_aprons(region), report_lines)
    # Taxiway profiles are sampled from the terrain AFTER runways and aprons
    # are flattened (phase A), so junction heights agree; the final pass
    # then rasterises all three jointly from the untouched tiles.
    snapshot = {k: v.copy() for k, v in store.tiles.items()}
    rasterise(region, store, live, aprons, [])
    taxiways = fit_taxiways(region, store, live, aprons, data_taxiways(region), report_lines)
    for k, v in snapshot.items():
        store.tiles[k][:] = v
    for k in list(store.tiles.keys()):
        if k not in snapshot:
            store.tiles[k][:] = np.fromfile(store.path(*k), dtype="<f4").reshape(store.px, store.px)
    touched = rasterise(region, store, live, aprons, taxiways)
    # Taxiways overlap each other (parallels, crossings, stubs): where two
    # ribbons cover a pixel the last profile wins, so a node of the other
    # ribbon can sit off the terrain. Resample every node from the final
    # terrain and re-rasterise the ribbons; two rounds converge to
    # centimetres. Nodes on runway strips / aprons keep their exact value.
    for _ in range(2):
        resample_taxiways(store, live, aprons, taxiways)
        rasterise(region, store, live, aprons, taxiways, taxi_inside_only=True)
    resample_taxiways(store, live, aprons, taxiways)
    for tw in taxiways:
        _write_taxiway_path(tw)
    build_taxiway_meshes(region, store, live, taxiways, report_lines)
    return touched


def build_taxiway_meshes(region, store, strips, taxiways, report_lines):
    """Per airport: union of all taxiway pavements (ribbons with round
    joins), minus the runway pavements, constrained-Delaunay triangulated,
    vertex heights from the final terrain. Stored as airport
    ["taxiway_mesh"] = {vertices: [[lat, lon, elev]], triangles: [i...]}.
    Junction gaps and overlapping ribbons cannot occur in a union."""
    import shapely
    from shapely.geometry import LineString, Polygon
    from shapely.ops import unary_union
    by_icao = {}
    for tw in taxiways:
        by_icao.setdefault(tw["icao"], []).append(tw)
    airports = {ap["icao"]: ap for ap in _DATA.get(region.id, {}).get("airports", [])}
    n_tri = 0
    worst = 0.0
    for icao, tws in by_icao.items():
        ribbons = []
        for tw in tws:
            if len(tw["pts"]) < 2:
                continue
            w = tw["half_w"] - TAXI_EDGE_M
            try:
                ribbons.append(LineString(tw["pts"]).buffer(w, join_style="round", cap_style="round", quad_segs=6))
            except Exception:  # noqa: BLE001
                continue
        if not ribbons:
            continue
        surface = unary_union(ribbons)
        for st in strips:
            if st["icao"] != icao or st.get("skip"):
                continue
            ux, uy, hw = st["ux"], st["uy"], st["half_w"]
            rx, ry = uy, -ux
            c = [(st["e0"] + rx * hw, st["n0"] + ry * hw), (st["e1"] + rx * hw, st["n1"] + ry * hw),
                 (st["e1"] - rx * hw, st["n1"] - ry * hw), (st["e0"] - rx * hw, st["n0"] - ry * hw)]
            surface = surface.difference(Polygon(c))
        if surface.is_empty:
            continue
        # Boundary vertices every ≤ TAXI_NODE_M so no triangle spans far
        # along a taxiway (the terrain profile is not planar between OSM
        # nodes; with simplified outlines the mesh dipped 2.7 m under it).
        surface = shapely.segmentize(surface, TAXI_NODE_M)
        tris = shapely.constrained_delaunay_triangles(surface)
        verts = {}
        vlist = []
        index = []

        def vid(x, y):
            key = (round(x, 2), round(y, 2))
            if key not in verts:
                verts[key] = len(vlist)
                vlist.append(key)
            return verts[key]

        # Triangles wider than TAXI_MESH_MAX_EDGE_M (junction fans) are
        # split at their edge midpoints so interior vertices sample the
        # terrain too (mesh-vs-terrain went 0.64 m without this).
        def emit(a, b, c, depth):
            (ax, ay), (bx, by), (cx, cy) = a, b, c
            longest = max(math.hypot(bx - ax, by - ay), math.hypot(cx - bx, cy - by), math.hypot(ax - cx, ay - cy))
            if longest > TAXI_MESH_MAX_EDGE_M and depth < 3:
                ab = ((ax + bx) / 2, (ay + by) / 2); bc = ((bx + cx) / 2, (by + cy) / 2); ca = ((cx + ax) / 2, (cy + ay) / 2)
                emit(a, ab, ca, depth + 1); emit(ab, b, bc, depth + 1)
                emit(ca, bc, c, depth + 1); emit(ab, bc, ca, depth + 1)
                return
            ids = [vid(*a), vid(*b), vid(*c)]
            if len(set(ids)) == 3:
                index.extend(ids)
            mx, my = (ax + bx + cx) / 3, (ay + by + cy) / 3
            zc = store.sample(mx, my)
            zm = (store.sample(ax, ay) + store.sample(bx, by) + store.sample(cx, cy)) / 3.0
            nonlocal_worst[0] = max(nonlocal_worst[0], abs(zc - zm)) if math.isfinite(zc) and math.isfinite(zm) else nonlocal_worst[0]

        nonlocal_worst = [0.0]
        for tri in getattr(tris, "geoms", [tris]):
            coords = list(tri.exterior.coords)[:3]
            if len(coords) < 3:
                continue
            emit(coords[0], coords[1], coords[2], 0)
        worst = max(worst, nonlocal_worst[0])
        ll = _lv95_to_wgs84_list(vlist)
        out_v = []
        heights = []
        for (lat, lon), (x, y) in zip(ll, vlist):
            z = store.sample(x, y)
            z = float(z) if math.isfinite(z) and z != 0.0 else 0.0
            heights.append(z)
            out_v.append([lat, lon, round(z, 2)])
        # The terrain under the pavement may keep creases where two ribbon
        # profiles meet (lowest-surface rule); sink it under the mesh so the
        # pavement is never below the ground it covers.
        sink_terrain_under_mesh(region, store, vlist, heights, index)
        ap = airports.get(icao)
        if ap is not None:
            ap["taxiway_mesh"] = {"vertices": out_v, "triangles": index}
        n_tri += len(index) // 3
    if n_tri:
        report_lines.append(f"  taxiway surfaces: {n_tri} triangles, worst centroid-vs-terrain before sinking {worst:.2f} m")


def sink_terrain_under_mesh(region, store, vlist, heights, index):
    """terrain := min(terrain, mesh) on every pixel inside a pavement triangle."""
    tm = region.tile_m
    for k in range(0, len(index) - 2, 3):
        (ax, ay), (bx, by), (cx, cy) = vlist[index[k]], vlist[index[k + 1]], vlist[index[k + 2]]
        za, zb, zc = heights[index[k]], heights[index[k + 1]], heights[index[k + 2]]
        # Grow the triangle by one pixel spacing about its centroid (plane
        # extrapolated, lowering capped) so the pixels the bilinear terrain
        # blends at the pavement edge are sunk too (LSZB: 13 cm residual).
        gx, gy = (ax + bx + cx) / 3.0, (ay + by + cy) / 3.0
        grow = store.sp
        def push(x, y):
            dx, dy = x - gx, y - gy
            r = math.hypot(dx, dy)
            return (x + dx / r * grow, y + dy / r * grow) if r > 1e-6 else (x, y)
        (ax, ay), (bx, by), (cx, cy) = push(ax, ay), push(bx, by), push(cx, cy)
        det = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if abs(det) < 1e-6:
            continue
        minx, maxx = min(ax, bx, cx), max(ax, bx, cx)
        miny, maxy = min(ay, by, cy), max(ay, by, cy)
        for (ei, ni) in bbox_tiles(region, minx, miny, maxx, maxy, 0.0):
            a = store.get(ei, ni)
            if a is None:
                continue
            E, N = store.pixel_coords(ei, ni)
            # barycentric
            w_a = ((by - cy) * (E - cx) + (cx - bx) * (N - cy)) / det
            w_b = ((cy - ay) * (E - cx) + (ax - cx) * (N - cy)) / det
            w_c = 1.0 - w_a - w_b
            inside = (w_a >= -1e-9) & (w_b >= -1e-9) & (w_c >= -1e-9)
            if not inside.any():
                continue
            h = w_a * za + w_b * zb + w_c * zc
            cur = a.astype(np.float64)
            lower = inside & (cur > h) & (cur != 0.0) & (cur - h <= 0.5)
            if lower.any():
                a[lower] = h[lower].astype(np.float32)
                a[(a == 0.0) & lower] = 0.01


def _write_taxiway_path(tw):
    """runways.json path records: [lat, lon, elev_m, on_runway]."""
    ll = _lv95_to_wgs84_list(tw["pts"])
    tw["ref"]["path"] = [[lat, lon, round(h, 2), int(f)]
                         for (lat, lon), h, f in zip(ll, tw["z"], tw["flags"])]


def resample_taxiways(store, strips, aprons, taxiways):
    """Node heights straight from the current terrain: on a runway strip or
    apron that IS the (lowest) surface there, so junctions agree."""
    for tw in taxiways:
        z = []
        for (x, y), z_old in zip(tw["pts"], tw["z"]):
            zz = store.sample(x, y)
            z.append(float(zz) if math.isfinite(zz) and zz != 0.0 else z_old)
        tw["z"] = z


_DATA = {}   # region id -> parsed runways.json (set by cmd_flatten via set_data)
_apron_lists = {}   # icao -> rebuilt apron entries (merged polygons)


_TO_LV95 = None


def _to_lv95(lat, lon):
    global _TO_LV95
    if _TO_LV95 is None:
        from pyproj import Transformer
        _TO_LV95 = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
    return _TO_LV95.transform(lon, lat)


def _lv95_to_wgs84_list(pts):
    from pyproj import Transformer
    tr = Transformer.from_crs("EPSG:2056", "EPSG:4326", always_xy=True)
    out = []
    for e, n in pts:
        lon, lat = tr.transform(e, n)
        out.append((round(lat, 6), round(lon, 6)))
    return out


def set_data(region, data):
    _DATA[region.id] = data


def data_aprons(region):
    return _DATA.get(region.id, {}).get("_aprons", [])


def data_taxiways(region):
    return _DATA.get(region.id, {}).get("_taxiways", [])


def _strip_field(s, E, N, core_only=False):
    """(inside, d_out, plane) of a runway strip on a pixel grid. The strip
    is the pavement plus APRON_M each side; with core_only the pavement
    alone counts as inside (the margin yields to aprons in rasterise)."""
    ux, uy = s["ux"], s["uy"]
    dx, dy = E - s["e0"], N - s["n0"]
    along = dx * ux + dy * uy
    lateral = np.abs(dx * uy - dy * ux)
    extra = 0.0 if core_only else APRON_M + s.get("dilate", 0.0)
    flat_half_w = s["half_w"] + extra
    over = OVERRUN_M + (0.0 if core_only else s.get("dilate", 0.0))
    d_along = np.maximum(np.maximum(-over - along, along - s["length"] - over), 0.0)
    d_lat = np.maximum(lateral - flat_half_w, 0.0)
    d_out = np.hypot(d_along, d_lat)
    plane = _plane_z(s, np.clip(along, 0.0, s["length"]) / s["length"])
    return d_out <= 0.0, d_out, plane


def _apron_field(apn, E, N):
    from shapely import contains_xy, distance
    from shapely.geometry import Point
    import shapely
    pts = shapely.points(E.ravel(), N.ravel())
    inside = np.asarray(contains_xy(apn["poly"], E.ravel(), N.ravel()), dtype=bool).reshape(E.shape)
    d_out = np.asarray(distance(apn["poly"], pts), dtype=float).reshape(E.shape)
    d_out = np.where(inside, 0.0, d_out)
    d_out = np.maximum(d_out - apn["dilate"], 0.0)
    a, b, c = apn["plane"]
    plane = a * (E - apn["ce"]) + b * (N - apn["cn"]) + c
    return d_out <= 0.0, d_out, plane


def _taxi_field(tw, E, N):
    """Nearest-segment distance and interpolated profile height."""
    pts = tw["pts"]
    zs = tw["z"]
    best_d = np.full(E.shape, np.inf)
    best_z = np.zeros(E.shape)
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        sx, sy = x1 - x0, y1 - y0
        L2 = sx * sx + sy * sy
        if L2 < 1e-6:
            continue
        t = np.clip(((E - x0) * sx + (N - y0) * sy) / L2, 0.0, 1.0)
        px, py = x0 + t * sx, y0 + t * sy
        d = np.hypot(E - px, N - py)
        z = zs[i] + (zs[i + 1] - zs[i]) * t
        better = d < best_d
        best_d = np.where(better, d, best_d)
        best_z = np.where(better, z, best_z)
    d_out = np.maximum(best_d - tw["half_w"] - tw["dilate"], 0.0)
    return d_out <= 0.0, d_out, best_z


def fit_aprons(region, store, strips, aprons, report_lines):
    """Plane per apron polygon, least squares on the terrain inside it
    (after the runway plateaus, sampled on the pixel grid), slope capped;
    lowered where it would rise above a runway plane inside the polygon."""
    from shapely.geometry import Polygon, MultiPolygon
    from shapely.ops import unary_union
    from shapely import contains_xy
    # Merge each airport's apron polygons (OSM maps aprons as adjoining or
    # overlapping pieces; separate planes for each stepped by metres at
    # their shared edges). Each connected piece becomes one apron surface;
    # the airport's list in runways.json is rebuilt from the merge.
    by_icao = {}
    for apn in aprons:
        try:
            poly = Polygon(apn["pts"])
            if not poly.is_valid:
                poly = poly.buffer(0)
        except Exception:  # noqa: BLE001
            continue
        if poly.is_empty or poly.area < 100.0:
            continue
        by_icao.setdefault(apn["icao"], []).append((poly, apn))
    merged = []
    for icao, items in by_icao.items():
        # Pieces closer than two dilations would fight over the pixels
        # between them (the lower plane wins there and the other piece's
        # edge floats), so they are merged into one plane.
        gap = DILATE_FRAC * store.sp
        union = unary_union([pl.buffer(gap) for pl, _ in items]).buffer(-gap)
        parts = list(union.geoms) if isinstance(union, MultiPolygon) else [union]
        surface = items[0][1]["ref"].get("surface", "concrete")
        entries = []
        for part in parts:
            if part.is_empty or part.area < 100.0:
                continue
            ext = [(x, y) for x, y in part.exterior.coords]
            ref = {"surface": surface, "polygon": [None] * len(ext)}
            entries.append(ref)
            merged.append({"icao": icao, "pts": ext, "ref": ref, "_part": Polygon(part.exterior)})
        _apron_lists[icao] = entries
    out = []
    for apn in merged:
        poly = apn["_part"]
        ce, cn = poly.centroid.x, poly.centroid.y
        minx, miny, maxx, maxy = poly.bounds
        xs, ys, zs = [], [], []
        for (ei, ni) in bbox_tiles(region, minx, miny, maxx, maxy, 0.0):
            a = store.get(ei, ni)
            if a is None:
                continue
            E, N = store.pixel_coords(ei, ni)
            m = np.asarray(contains_xy(poly, E.ravel(), N.ravel()), dtype=bool)
            zz = a.ravel()[m]
            ok = zz != 0.0
            xs.append(E.ravel()[m][ok]); ys.append(N.ravel()[m][ok]); zs.append(zz[ok])
        if not xs:
            continue
        x = np.concatenate(xs) - ce; y = np.concatenate(ys) - cn; z = np.concatenate(zs).astype(float)
        hd, wd = runway_datum(strips, apn["icao"], x + ce, y + cn)
        z = wd * hd + (1.0 - wd) * z
        if z.size >= 3:
            A = np.column_stack([x, y, np.ones_like(x)])
            (a_, b_, c_), *_ = np.linalg.lstsq(A, z, rcond=None)
        elif z.size > 0:
            a_, b_, c_ = 0.0, 0.0, float(z.mean())
        else:
            continue
        slope = math.hypot(a_, b_)
        if slope > APRON_MAX_SLOPE:
            k = APRON_MAX_SLOPE / slope
            a_ *= k; b_ *= k
            c_ = float(np.mean(z - a_ * x - b_ * y))
        # Never above a runway plane on the runway strip (pavement + margin):
        # checked on the interior pixels AND the outline vertices, so an
        # apron whose edge runs along a runway cannot float above it.
        vx = np.array([e for e, _ in apn["pts"]]) - ce
        vy = np.array([n for _, n in apn["pts"]]) - cn
        qx, qy = np.concatenate([x, vx]), np.concatenate([y, vy])
        for s in strips:
            if s["icao"] != apn["icao"]:
                continue
            inside_s, _, plane_s = _strip_field(s, qx + ce, qy + cn)
            if inside_s.any():
                viol = float(np.max((a_ * qx + b_ * qy + c_)[inside_s] - plane_s[inside_s]))
                if viol > 0.0:
                    c_ -= viol
        apn["poly"] = poly; apn["ce"] = ce; apn["cn"] = cn; apn["plane"] = (float(a_), float(b_), float(c_))
        apn["dilate"] = DILATE_FRAC * store.sp
        apn["ref"]["polygon"] = [[lat, lon, round(a_ * (e - ce) + b_ * (n - cn) + c_, 2)]
                                 for (lat, lon), (e, n) in zip(_lv95_to_wgs84_list(apn["pts"]), apn["pts"])]
        out.append(apn)
    if out:
        report_lines.append(f"  aprons: {len(out)} planes fitted")
    return out


def runway_datum(strips, icao, xs, ys):
    """(h, w) arrays: inverse-distance blend of the airport's runway planes
    at the nearest points, and the weight of that datum vs the terrain."""
    xs = np.asarray(xs, dtype=float); ys = np.asarray(ys, dtype=float)
    num = np.zeros(xs.shape); den = np.zeros(xs.shape); dmin = np.full(xs.shape, np.inf)
    for s in strips:
        if s["icao"] != icao or s.get("skip"):
            continue
        dx, dy = xs - s["e0"], ys - s["n0"]
        along = np.clip(dx * s["ux"] + dy * s["uy"], 0.0, s["length"])
        px, py = s["e0"] + s["ux"] * along, s["n0"] + s["uy"] * along
        d = np.maximum(np.hypot(xs - px, ys - py) - s["half_w"], 0.0)
        h = _plane_z(s, along / s["length"])
        wgt = 1.0 / (d + 30.0) ** 2
        num += wgt * h; den += wgt; dmin = np.minimum(dmin, d)
    h = np.where(den > 0, num / np.where(den > 0, den, 1.0), 0.0)
    w = np.where(den > 0, 1.0 - _smoothstep((dmin - DATUM_NEAR_M) / (DATUM_FAR_M - DATUM_NEAR_M)), 0.0)
    return h, w


def _pavement_inside(strips, icao, x, y):
    """True when (x, y) lies on a runway pavement (no margin) of the airport."""
    for s in strips:
        if s["icao"] != icao:
            continue
        dx, dy = x - s["e0"], y - s["n0"]
        along = dx * s["ux"] + dy * s["uy"]
        lateral = abs(dx * s["uy"] - dy * s["ux"])
        if 0.0 <= along <= s["length"] and lateral <= s["half_w"]:
            return True
    return False


def densify_taxiway(strips, icao, pts):
    """Insert nodes every TAXI_NODE_M and EXACT nodes where the path enters
    or leaves a runway pavement (duplicated so the on-runway flag switches
    crisply). Returns (points, flags)."""
    out_pts, out_flags = [], []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        L = math.hypot(x1 - x0, y1 - y0)
        if L < 1e-6:
            continue
        # candidate split parameters: pavement edges of every strip
        ts = {0.0, 1.0}
        for st in strips:
            if st["icao"] != icao:
                continue
            ux, uy = st["ux"], st["uy"]
            a0 = (x0 - st["e0"]) * ux + (y0 - st["n0"]) * uy
            a1 = (x1 - st["e0"]) * ux + (y1 - st["n0"]) * uy
            l0 = (x0 - st["e0"]) * uy - (y0 - st["n0"]) * ux
            l1 = (x1 - st["e0"]) * uy - (y1 - st["n0"]) * ux
            for (v0, v1, target) in ((a0, a1, 0.0), (a0, a1, st["length"]),
                                     (l0, l1, st["half_w"]), (l0, l1, -st["half_w"])):
                if abs(v1 - v0) > 1e-9:
                    t = (target - v0) / (v1 - v0)
                    if 0.0 < t < 1.0:
                        ts.add(t)
        ts = sorted(ts)
        # walk the pieces; each piece has constant inside-ness (test its middle)
        pieces = []
        for a, b in zip(ts[:-1], ts[1:]):
            tm = 0.5 * (a + b)
            inside = _pavement_inside(strips, icao, x0 + (x1 - x0) * tm, y0 + (y1 - y0) * tm)
            pieces.append((a, b, inside))
        for k, (a, b, inside) in enumerate(pieces):
            pa = (x0 + (x1 - x0) * a, y0 + (y1 - y0) * a)
            pb = (x0 + (x1 - x0) * b, y0 + (y1 - y0) * b)
            if k == 0 and i == 0:
                out_pts.append(pa); out_flags.append(1 if inside else 0)
            elif k > 0 or True:
                # duplicate the boundary node with the new flag (crisp cut)
                if not out_pts or out_flags[-1] != (1 if inside else 0) or out_pts[-1] != pa:
                    out_pts.append(pa); out_flags.append(1 if inside else 0)
            seg = math.hypot(pb[0] - pa[0], pb[1] - pa[1])
            n = max(1, int(math.ceil(seg / TAXI_NODE_M)))
            for j in range(1, n + 1):
                t = j / n
                out_pts.append((pa[0] + (pb[0] - pa[0]) * t, pa[1] + (pb[1] - pa[1]) * t))
                out_flags.append(1 if inside else 0)
    if not out_pts:
        return pts, [0] * len(pts)
    return out_pts, out_flags


def fit_taxiways(region, store, strips, aprons, taxiways, report_lines):
    """Profile height per taxiway node from the phase-A terrain: nodes on a
    runway strip or apron take the exact surface there, other nodes the
    mean of the terrain within ±TAXI_SMOOTH_M along the path. Paths are
    densified first (see densify_taxiway)."""
    out = []
    for tw in taxiways:
        tw["pts"], tw["flags"] = densify_taxiway(strips, tw["icao"], tw["pts"])
        pts = tw["pts"]
        # densified samples with cumulative distance
        samp_s, samp_z = [], []
        node_s = [0.0]
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            L = math.hypot(x1 - x0, y1 - y0)
            n = max(1, int(math.ceil(L / 10.0)))
            for k in range(n):
                t = k / n
                samp_s.append(node_s[-1] + t * L)
                samp_z.append(store.sample(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
            node_s.append(node_s[-1] + L)
        samp_s.append(node_s[-1]); samp_z.append(store.sample(*pts[-1]))
        samp_s = np.array(samp_s); samp_z = np.array(samp_z, dtype=float)
        okm = np.isfinite(samp_z) & (samp_z != 0.0)
        # samples' positions for the datum blend
        samp_xy = []
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            L = math.hypot(x1 - x0, y1 - y0)
            n = max(1, int(math.ceil(L / 10.0)))
            for k in range(n):
                t = k / n
                samp_xy.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
        samp_xy.append(pts[-1])
        hd, wd = runway_datum(strips, tw["icao"], [q[0] for q in samp_xy], [q[1] for q in samp_xy])
        samp_z = np.where(okm, wd * hd + (1.0 - wd) * samp_z, samp_z)
        if okm.sum() < 1:
            continue
        z = []
        for i, (x, y) in enumerate(pts):
            exact = None
            for s in strips:
                if s["icao"] == tw["icao"]:
                    ins, _, pl = _strip_field(s, np.array([[x]]), np.array([[y]]))
                    if ins[0, 0]:
                        exact = float(pl[0, 0]); break
            if exact is None:
                for apn in aprons:
                    if apn["icao"] == tw["icao"] and apn["poly"].contains(__import__("shapely").geometry.Point(x, y)):
                        a_, b_, c_ = apn["plane"]
                        exact = a_ * (x - apn["ce"]) + b_ * (y - apn["cn"]) + c_; break
            if exact is not None:
                z.append(exact)
                continue
            w = okm & (np.abs(samp_s - node_s[i]) <= TAXI_SMOOTH_M)
            if w.any():
                z.append(float(samp_z[w].mean()))
            else:
                zz = store.sample(x, y)
                z.append(float(zz) if math.isfinite(zz) and zz != 0.0 else float(samp_z[okm].mean()))
        tw["z"] = z
        tw["dilate"] = DILATE_FRAC * store.sp
        _write_taxiway_path(tw)
        out.append(tw)
    if out:
        report_lines.append(f"  taxiways: {len(out)} profiles fitted")
    return out


def rasterise(region, store, strips, aprons, taxiways, taxi_inside_only=False):
    """Joint rasterisation. Inside priority: runway strips (lowest plane of
    the covering strips) > aprons > taxiways; pixels inside no feature blend
    toward the weight-averaged nearby surfaces."""
    per_tile = {}
    for s in strips:
        for t in runway_tiles(region, s["e0"], s["n0"], s["e1"], s["n1"], s["half_w"]):
            per_tile.setdefault(t, []).append(("rwy", s))
    for apn in aprons:
        minx, miny, maxx, maxy = apn["poly"].bounds
        for t in bbox_tiles(region, minx, miny, maxx, maxy, APRON_BLEND_M + apn["dilate"]):
            per_tile.setdefault(t, []).append(("apron", apn))
    for tw in taxiways:
        xs = [p[0] for p in tw["pts"]]; ys = [p[1] for p in tw["pts"]]
        for t in bbox_tiles(region, min(xs), min(ys), max(xs), max(ys), tw["half_w"] + tw["dilate"] + TAXI_BLEND_M):
            per_tile.setdefault(t, []).append(("taxi", tw))

    touched = set()
    for (ei, ni), feats in per_tile.items():
        a = store.get(ei, ni)
        if a is None:
            continue
        E, N = store.pixel_coords(ei, ni)
        inside_val = np.full(a.shape, np.nan)
        inside_pri = np.zeros(a.shape, dtype=int)
        blend_num = np.zeros(a.shape)
        blend_den = np.zeros(a.shape)
        wmax = np.zeros(a.shape)
        passes = []
        for kind, f in feats:
            if kind == "rwy":
                inside_m, d_out, plane = _strip_field(f, E, N)
                inside_c, _, _ = _strip_field(f, E, N, core_only=True)
                # pavement: highest priority; the flat margin beside it
                # yields to aprons (an apron along a runway edge) but not
                # to taxiways
                passes.append((inside_c, np.where(inside_m, 0.0, d_out), plane, 4, BLEND_M))
                passes.append((inside_m & ~inside_c, np.where(inside_c, 1e9, d_out), plane, 2, BLEND_M))
            elif kind == "apron":
                inside, d_out, plane = _apron_field(f, E, N)
                passes.append((inside, d_out, plane, 3, APRON_BLEND_M))
            else:
                inside, d_out, plane = _taxi_field(f, E, N)
                passes.append((inside, d_out, plane, 1, TAXI_BLEND_M))
        for inside, d_out, plane, pri, blend in passes:
            kind = "taxi" if pri == 1 else "other"
            if taxi_inside_only and kind != "taxi":
                # only reserve the pixels (priority) — values stay as they are
                inside_pri = np.where(inside, np.maximum(inside_pri, pri), inside_pri)
                inside_val = np.where(inside & np.isnan(inside_val), 0.0, inside_val)
                continue
            take = inside & (inside_pri < pri)
            same = inside & (inside_pri == pri)
            # Within one priority class the LOWEST surface wins (crossing
            # runways, adjoining aprons, overlapping taxiway ribbons), so no
            # mesh of that class ever ends up below the terrain; the higher
            # one floats by the overlap difference instead.
            inside_val = np.where(take, plane, inside_val)
            inside_val = np.where(same, np.fmin(inside_val, plane), inside_val)
            inside_pri = np.where(inside, np.maximum(inside_pri, pri), inside_pri)
            if pri == 2:
                continue   # the pavement pass already carries this strip's blend
            w = np.where(inside, 0.0, 1.0 - _smoothstep(d_out / blend))
            blend_num += w * plane
            blend_den += w
            wmax = np.maximum(wmax, w)
        orig = a.astype(np.float64)
        orig_ok = orig != 0.0
        out = orig.copy()
        if taxi_inside_only:
            im = (inside_pri == 1) & np.isfinite(inside_val)
            out[im] = inside_val[im]
            if im.any():
                a[im] = out[im].astype(np.float32)
                a[(a == 0.0) & im] = 0.01
                touched.add((ei, ni))
            continue
        im = inside_pri > 0
        bm = (blend_den > 0.0) & ~im
        if bm.any():
            target = np.where(bm, blend_num / np.where(bm, blend_den, 1.0), 0.0)
            mixed = np.where(orig_ok, target * wmax + orig * (1.0 - wmax), target)
            out[bm] = mixed[bm]
        out[im] = inside_val[im]
        # Runway ceiling: nothing inside a runway's flat zone (pavement +
        # margin + dilation) may stand above that runway's plane. At a
        # crossing the longer runway's pavement overlapped the shorter one's
        # margin at a higher level and the bilinear terrain at the shorter
        # runway's edge rose 11 cm through it (LSZH 10/28, 2026-09-20).
        ceiling = np.full(a.shape, np.inf)
        for kind, f in feats:
            if kind != "rwy":
                continue
            inside_m, _, plane = _strip_field(f, E, N)
            ceiling = np.where(inside_m, np.minimum(ceiling, plane), ceiling)
        cm = np.isfinite(ceiling) & (out > ceiling)
        out[cm] = ceiling[cm]
        changed = bm | im | cm
        if not changed.any():
            continue
        a[changed] = out[changed].astype(np.float32)
        a[(a == 0.0) & changed] = 0.01
        touched.add((ei, ni))
    return touched
