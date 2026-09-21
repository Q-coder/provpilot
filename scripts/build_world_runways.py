#!/usr/bin/env python3
"""
build_world_runways.py — per-pack runway geometry (design §12).

For every large/medium OurAirports aerodrome of a region, merge:

  * OpenStreetMap `aeroway=runway` centreline ways (Overpass, one query per
    region bbox, cached): pavement ends and `width` — imagery-traced, the
    most precise free geometry;
  * OurAirports runways.csv: designators, threshold coordinates and
    elevations, displaced thresholds, width, surface, lighting — identity,
    metadata and the geometric fallback / cross-check.

Output: <build_root>/<id>/<Name>/airports/runways.json — a self-contained,
generic description the runtime renders procedurally. A higher-priority pack
(a bespoke airport, the CH/BW detail packs) overrides an airport wholesale by
listing the same ICAO in its own airports/runways.json (§12.2).

A report of every disagreement between the two sources is written to
<build_root>/<id>/runways_report.txt so the data can be judged before it is
trusted.

Usage:
    python scripts/build_world_runways.py --region switzerland-base
    python scripts/build_world_runways.py --all [--offline]
"""

import argparse
import csv
import json
import math
import sys
import time
import urllib.parse
import urllib.request
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_world_airports import (load_config, ensure_csv, map_surface,  # noqa: E402
                                  initial_bearing_deg, refine_type, WANTED_TYPES)

OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]
FT_TO_M = 0.3048

# Matching tolerances (metres / degrees).
ARP_SEARCH_M = 4000.0        # OSM ways considered around the airport reference point
BEARING_TOL_DEG = 6.0        # runway axis vs OSM way bearing (mod 180)
LATERAL_TOL_M = 80.0         # OSM way midpoint distance from the runway axis
LENGTH_FLAG_FRAC = 0.10      # |pavement − declared length| beyond this fraction …
LENGTH_FLAG_MIN_M = 60.0     # … and beyond this absolute value is reported
THRESHOLD_FLAG_M = 30.0      # OurAirports threshold vs OSM pavement end (+displaced)
MIN_WAY_LEN_M = 30.0         # ignore OSM stubs shorter than this


# ── geometry ────────────────────────────────────────────────────────────────

class Local:
    """Equirectangular metres around a reference point (fine for < 20 km)."""

    def __init__(self, lat0, lon0):
        self.lat0 = lat0
        self.lon0 = lon0
        self.kx = 111320.0 * math.cos(math.radians(lat0))
        self.ky = 111320.0

    def xy(self, lat, lon):
        return ((lon - self.lon0) * self.kx, (lat - self.lat0) * self.ky)

    def latlon(self, x, y):
        return (self.lat0 + y / self.ky, self.lon0 + x / self.kx)


def bearing_deg(x0, y0, x1, y1):
    return (math.degrees(math.atan2(x1 - x0, y1 - y0)) + 360.0) % 360.0


def bearing_diff_180(a, b):
    d = abs((a - b) % 180.0)
    return min(d, 180.0 - d)


def parse_width_m(text):
    """OSM width tag: '60', '45 m', '150 ft', '45.0'."""
    if not text:
        return None
    t = text.strip().lower().replace(",", ".")
    mult = 1.0
    for unit, m in (("ft", FT_TO_M), ("feet", FT_TO_M), ("m", 1.0)):
        if t.endswith(unit):
            t = t[:-len(unit)].strip()
            mult = m
            break
    try:
        v = float(t) * mult
    except ValueError:
        return None
    return v if 5.0 <= v <= 150.0 else None


def norm_designators(text):
    """'14/32', '32/14', '16L/34R' → frozenset of designators, or None."""
    if not text:
        return None
    parts = [p.strip().upper() for p in text.replace(";", "/").split("/")]
    parts = [_canon_designator(p) for p in parts if p]
    return frozenset(parts) if parts else None


def _canon_designator(p):
    """'H01' (OSM heli strips) → '01H', '7' → '07', 'L34' → '34L'."""
    letters = "".join(ch for ch in p if ch.isalpha())
    digits = "".join(ch for ch in p if ch.isdigit())
    if digits and len(digits) == 1:
        digits = "0" + digits
    return digits + letters if digits else p


# ── Overpass ────────────────────────────────────────────────────────────────

def fetch_osm_runways(cfg, region, bbox, offline):
    cache = Path(cfg["cache_root"]) / "osm_runways" / f"{region['id']}.json"
    if cache.exists() and cache.stat().st_size > 0:
        with open(cache) as f:
            return json.load(f)
    if offline:
        sys.exit(f"--offline but no cached OSM runways for {region['id']} ({cache})")
    w, s, e, n = bbox
    query = (f'[out:json][timeout:300];way["aeroway"="runway"]'
             f'({s:.4f},{w:.4f},{n:.4f},{e:.4f});out tags geom;')
    data = None
    for url in OVERPASS_ENDPOINTS:
        for attempt in range(2):
            try:
                print(f"  Overpass {url} ({region['id']} bbox)…", flush=True)
                req = urllib.request.Request(
                    url, data=urllib.parse.urlencode({"data": query}).encode(),
                    headers={"User-Agent": "provpilot-build/1.0"})
                with urllib.request.urlopen(req, timeout=360) as r:
                    data = json.load(r)
                break
            except Exception as ex:  # noqa: BLE001
                print(f"  failed: {ex}")
                time.sleep(10)
        if data is not None:
            break
    if data is None:
        sys.exit("Overpass failed on every endpoint")
    cache.parent.mkdir(parents=True, exist_ok=True)
    with open(cache, "w") as f:
        json.dump(data, f)
    return data


def fetch_osm_airfield(cfg, region, bbox, offline):
    """Taxiway ways and apron areas (closed ways + multipolygon relations)
    for the region bbox, cached like the runways."""
    cache = Path(cfg["cache_root"]) / "osm_airfield" / f"{region['id']}.json"
    if cache.exists() and cache.stat().st_size > 0:
        with open(cache) as f:
            return json.load(f)
    if offline:
        print(f"  --offline and no cached taxiways/aprons for {region['id']} — skipped")
        return {"elements": []}
    w, s, e, n = bbox
    bb = f"({s:.4f},{w:.4f},{n:.4f},{e:.4f})"
    query = (f'[out:json][timeout:600];(way["aeroway"="taxiway"]{bb};'
             f'way["aeroway"="apron"]{bb};relation["aeroway"="apron"]{bb};);out tags geom;')
    data = None
    for url in OVERPASS_ENDPOINTS:
        for attempt in range(2):
            try:
                print(f"  Overpass {url} ({region['id']} taxiways/aprons)…", flush=True)
                req = urllib.request.Request(
                    url, data=urllib.parse.urlencode({"data": query}).encode(),
                    headers={"User-Agent": "provpilot-build/1.0"})
                with urllib.request.urlopen(req, timeout=660) as r:
                    data = json.load(r)
                break
            except Exception as ex:  # noqa: BLE001
                print(f"  failed: {ex}")
                time.sleep(15)
        if data is not None:
            break
    if data is None:
        print("  Overpass failed for taxiways/aprons — continuing without")
        return {"elements": []}
    cache.parent.mkdir(parents=True, exist_ok=True)
    with open(cache, "w") as f:
        json.dump(data, f)
    return data


LIGHT_KINDS = {"als", "txe", "txc", "rwe", "rwt", "rwc", "tdz", "papi", "vasi"}


def fetch_osm_lights(cfg, region, bbox, offline):
    """Individually mapped airfield lights (aeroway=navigationaid nodes:
    approach, runway edge/centre/threshold/TDZ, taxiway edge/centre, PAPI,
    VASI) for the region bbox, cached like the runways."""
    cache = Path(cfg["cache_root"]) / "osm_lights" / f"{region['id']}.json"
    if cache.exists() and cache.stat().st_size > 0:
        with open(cache) as f:
            return json.load(f)
    if offline:
        print(f"  --offline and no cached lights for {region['id']} — skipped")
        return {"elements": []}
    w, s, e, n = bbox
    query = (f'[out:json][timeout:600];node["aeroway"="navigationaid"]'
             f"({s:.4f},{w:.4f},{n:.4f},{e:.4f});out;")
    data = None
    for url in OVERPASS_ENDPOINTS:
        for attempt in range(2):
            try:
                print(f"  Overpass {url} ({region['id']} lights)…", flush=True)
                req = urllib.request.Request(
                    url, data=urllib.parse.urlencode({"data": query}).encode(),
                    headers={"User-Agent": "provpilot-build/1.0"})
                with urllib.request.urlopen(req, timeout=660) as r:
                    data = json.load(r)
                break
            except Exception as ex:  # noqa: BLE001
                print(f"  failed: {ex}")
                time.sleep(15)
        if data is not None:
            break
    if data is None:
        print("  Overpass failed for lights — continuing without")
        return {"elements": []}
    cache.parent.mkdir(parents=True, exist_ok=True)
    with open(cache, "w") as f:
        json.dump(data, f)
    return data


# ── OurAirports ─────────────────────────────────────────────────────────────

def load_ourairports(cfg, iso_a2, bbox, offline):
    airports_csv = ensure_csv(cfg, "airports.csv", offline)
    runways_csv = ensure_csv(cfg, "runways.csv", offline)
    countries = set(iso_a2)
    w, s, e, n = bbox
    airports = {}
    with open(airports_csv, newline="") as f:
        for r in csv.DictReader(f):
            if r["iso_country"] not in countries or r["type"] not in WANTED_TYPES:
                continue
            try:
                lat, lon = float(r["latitude_deg"]), float(r["longitude_deg"])
            except (ValueError, TypeError):
                continue
            if not (w <= lon <= e and s <= lat <= n):
                continue
            airports[r["id"]] = {
                "icao": r["icao_code"] or r["ident"],
                "name": r["name"],
                "lat": lat, "lon": lon,
                "elevation_m": int(round(float(r["elevation_ft"] or 0) * FT_TO_M)),
                "type": WANTED_TYPES[r["type"]],
                "_runways": [],
            }
    with open(runways_csv, newline="") as f:
        for r in csv.DictReader(f):
            a = airports.get(r["airport_ref"])
            if a is None or r.get("closed") == "1":
                continue
            a["_runways"].append(r)
    return airports


def _f(v):
    try:
        return float(v)
    except (ValueError, TypeError):
        return None


# ── matching ────────────────────────────────────────────────────────────────

PAVED = {"asphalt", "concrete"}


def surface_class(word):
    """'paved' / 'unpaved' / None (unknown) from a catalog or OSM surface word."""
    if not word:
        return None
    w = word.lower()
    if w in PAVED or w.startswith(("asp", "con", "pav", "bit", "tar")):
        return "paved"
    if w in ("other",):
        return None
    return "unpaved"


def build_runway(ap, row, osm_ways, loc, flags_out, claimed):
    """One OurAirports runway row + candidate OSM ways → runway dict."""
    le, he = (row.get("le_ident") or "").strip(), (row.get("he_ident") or "").strip()
    designators = [d for d in (le, he) if d]
    if not designators:
        return None
    declared_len = (_f(row.get("length_ft")) or 0.0) * FT_TO_M
    le_ll = (_f(row.get("le_latitude_deg")), _f(row.get("le_longitude_deg")))
    he_ll = (_f(row.get("he_latitude_deg")), _f(row.get("he_longitude_deg")))
    have_le = None not in le_ll
    have_he = None not in he_ll
    le_hdg = _f(row.get("le_heading_degT"))
    flags = []

    # Runway axis in local metres: from the thresholds, else from one
    # threshold + heading, else from the ARP + heading (weakest).
    if have_le and have_he:
        p0 = loc.xy(*le_ll)
        p1 = loc.xy(*he_ll)
        axis_src = "thresholds"
    else:
        hdg = le_hdg
        if hdg is None:
            flags.append("no_geometry")
            return None
        if have_le:
            p0 = loc.xy(*le_ll)
            axis_src = "le_threshold+heading"
        elif have_he:
            p_he = loc.xy(*he_ll)
            p0 = (p_he[0] - math.sin(math.radians(hdg)) * declared_len,
                  p_he[1] - math.cos(math.radians(hdg)) * declared_len)
            axis_src = "he_threshold+heading"
        else:
            c = loc.xy(ap["lat"], ap["lon"])
            p0 = (c[0] - math.sin(math.radians(hdg)) * declared_len / 2,
                  c[1] - math.cos(math.radians(hdg)) * declared_len / 2)
            axis_src = "arp+heading"
            flags.append("axis_from_arp")
        p1 = (p0[0] + math.sin(math.radians(hdg)) * declared_len,
              p0[1] + math.cos(math.radians(hdg)) * declared_len)
    axis_len = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
    if axis_len < 10.0:
        flags.append("degenerate_axis")
        return None
    ux, uy = (p1[0] - p0[0]) / axis_len, (p1[1] - p0[1]) / axis_len
    axis_bearing = bearing_deg(*p0, *p1)
    mid = ((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2)
    want = norm_designators("/".join(designators))

    # OSM candidates: bearing, lateral offset, designator agreement.
    matched = []
    for wy in osm_ways:
        pts = wy["_xy"]
        if len(pts) < 2:
            continue
        wlen = wy["_len"]
        if wlen < MIN_WAY_LEN_M:
            continue
        wb = bearing_deg(*pts[0], *pts[-1])
        if bearing_diff_180(wb, axis_bearing) > BEARING_TOL_DEG:
            continue
        wm = wy["_mid"]
        # lateral distance of the way midpoint from the (infinite) axis line
        dx, dy = wm[0] - mid[0], wm[1] - mid[1]
        lateral = abs(dx * uy - dy * ux)
        along = dx * ux + dy * uy
        if lateral > LATERAL_TOL_M or abs(along) > axis_len / 2 + 1500.0:
            continue
        ref = norm_designators(wy["tags"].get("ref"))
        if ref and want and ref != want:
            # Same axis but another designator (e.g. a parallel mapped with a
            # wrong offset): trust the ref.
            continue
        if wy["id"] in claimed and not (ref and ref == want):
            continue
        sc_row = surface_class(map_surface(row.get("surface", "")))
        sc_way = surface_class(wy["tags"].get("surface"))
        if sc_row and sc_way and sc_row != sc_way:
            continue
        matched.append(wy)
    for wy in matched:
        claimed.add(wy["id"])

    ends_ll = None
    width_m = None
    if matched:
        # Pavement = extreme node projections along the axis over all
        # matched ways (split ways, displaced-threshold stubs included).
        best_lo, best_hi = None, None
        for wy in matched:
            for (x, y), ll in zip(wy["_xy"], wy["_ll"]):
                t = (x - mid[0]) * ux + (y - mid[1]) * uy
                if best_lo is None or t < best_lo[0]:
                    best_lo = (t, ll)
                if best_hi is None or t > best_hi[0]:
                    best_hi = (t, ll)
        ends_ll = [best_lo[1], best_hi[1]]
        widths = [parse_width_m(wy["tags"].get("width")) for wy in matched]
        widths = [w for w in widths if w]
        if widths:
            width_m = max(widths)
        geometry_source = "osm"
        pav_len = best_hi[0] - best_lo[0]
        if declared_len > 0 and abs(pav_len - declared_len) > max(LENGTH_FLAG_MIN_M, LENGTH_FLAG_FRAC * declared_len):
            flags.append(f"length_osm_{pav_len:.0f}_vs_declared_{declared_len:.0f}")
    else:
        geometry_source = "ourairports" if axis_src == "thresholds" else axis_src
        ends_ll = [loc.latlon(*p0), loc.latlon(*p1)]
        flags.append("no_osm_match")
        if declared_len > 0 and abs(axis_len - declared_len) > max(LENGTH_FLAG_MIN_M, LENGTH_FLAG_FRAC * declared_len):
            flags.append(f"length_thresholds_{axis_len:.0f}_vs_declared_{declared_len:.0f}")

    if width_m is None:
        wft = _f(row.get("width_ft"))
        width_m = wft * FT_TO_M if wft else None
        if width_m is None:
            flags.append("no_width")
            width_m = 45.0

    # Orient pavement ends to the designators: end[0] is the LE end (the
    # threshold you land over when using runway `le`). The LE threshold lies
    # at the pavement end whose projection is smaller along the LE→HE axis.
    # With an OSM match the extreme along-axis points already follow the
    # axis direction (lo = LE side).
    def end_record(ident, ll_thr, elev_ft, displaced_ft, hdg, pav_ll):
        rec = {"ident": ident}
        if pav_ll is not None:
            rec["pavement"] = [round(pav_ll[0], 6), round(pav_ll[1], 6)]
        if ll_thr and None not in ll_thr:
            rec["end_ourairports"] = [round(ll_thr[0], 6), round(ll_thr[1], 6)]
        if elev_ft is not None:
            rec["elev_m"] = round(elev_ft * FT_TO_M, 1)
        if displaced_ft:
            rec["displaced_m"] = round(displaced_ft * FT_TO_M, 1)
        if hdg is not None:
            rec["heading_deg"] = round(hdg, 1)
        return rec

    he_hdg = _f(row.get("he_heading_degT"))
    if he_hdg is None and le_hdg is not None:
        he_hdg = (le_hdg + 180.0) % 360.0
    if le_hdg is None:
        le_hdg = axis_bearing
        he_hdg = (axis_bearing + 180.0) % 360.0
    ends = [
        end_record(le, le_ll if have_le else None, _f(row.get("le_elevation_ft")),
                   _f(row.get("le_displaced_threshold_ft")), le_hdg, ends_ll[0]),
        end_record(he, he_ll if have_he else None, _f(row.get("he_elevation_ft")),
                   _f(row.get("he_displaced_threshold_ft")), he_hdg, ends_ll[1]),
    ]

    # Cross-check: OurAirports le/he coordinates are the runway ENDS (the
    # displaced threshold is a separate distance), so they should coincide
    # with the OSM pavement ends.
    if matched:
        for rec in ends:
            if "end_ourairports" not in rec:
                continue
            thr = loc.xy(*rec["end_ourairports"])
            pav = loc.xy(*rec["pavement"])
            d = math.hypot(thr[0] - pav[0], thr[1] - pav[1])
            if d > THRESHOLD_FLAG_M:
                flags.append(f"end_{rec['ident']}_osm_vs_ourairports_{d:.0f}m")

    rw = {
        "designators": designators,
        "ends": ends,
        "width_m": round(width_m, 1),
        "length_m": round(math.hypot(*(a - b for a, b in zip(loc.xy(*ends_ll[0]), loc.xy(*ends_ll[1])))), 0),
        "surface": map_surface(row.get("surface", "")),
        "lighted": row.get("lighted") == "1",
        "geometry_source": geometry_source,
    }
    if flags:
        rw["flags"] = flags
        flags_out.append((ap["icao"], "/".join(designators), flags))
    return rw


OSM_ONLY_MAX_ARP_M = 2500.0   # unclaimed OSM runway ways this close to the ARP
OSM_ONLY_MIN_LEN_M = 150.0    # shortest strip taken from OSM alone


def designators_from_bearing(bearing):
    d1 = int(round(bearing / 10.0)) % 36
    if d1 == 0:
        d1 = 36
    d2 = d1 + 18 if d1 <= 18 else d1 - 18
    return [f"{d1:02d}", f"{d2:02d}"]


def osm_only_runways(ap, near, loc, claimed):
    """Runways built from OSM ways alone (no OurAirports row matched):
    collinear unclaimed ways are merged; designators come from the OSM
    `ref` or the way bearing; surface/width from the tags."""
    cands = [w for w in near if w["id"] not in claimed and w["_len"] >= OSM_ONLY_MIN_LEN_M
             and math.hypot(*w["_mid"]) <= OSM_ONLY_MAX_ARP_M]
    cands.sort(key=lambda w: -w["_len"])
    out = []
    used = set()
    for seed in cands:
        if seed["id"] in used:
            continue
        pts = seed["_xy"]
        sb = bearing_deg(*pts[0], *pts[-1])
        ux, uy = math.sin(math.radians(sb)), math.cos(math.radians(sb))
        mid = seed["_mid"]
        group = [seed]
        for w in cands:
            if w["id"] in used or w is seed:
                continue
            wb = bearing_deg(*w["_xy"][0], *w["_xy"][-1])
            if bearing_diff_180(wb, sb) > BEARING_TOL_DEG:
                continue
            dx, dy = w["_mid"][0] - mid[0], w["_mid"][1] - mid[1]
            if abs(dx * uy - dy * ux) > 40.0 or abs(dx * ux + dy * uy) > seed["_len"] / 2 + 1500.0:
                continue
            sref, wref = norm_designators(seed["tags"].get("ref")), norm_designators(w["tags"].get("ref"))
            if sref and wref and sref != wref:
                continue
            group.append(w)
        lo = hi = None
        for w in group:
            used.add(w["id"])
            claimed.add(w["id"])
            for (x, y), ll in zip(w["_xy"], w["_ll"]):
                t = (x - mid[0]) * ux + (y - mid[1]) * uy
                if lo is None or t < lo[0]:
                    lo = (t, ll)
                if hi is None or t > hi[0]:
                    hi = (t, ll)
        length = hi[0] - lo[0]
        if length < OSM_ONLY_MIN_LEN_M:
            continue
        ref = None
        for w in group:
            r = w["tags"].get("ref")
            if r:
                parts = [x.strip().upper() for x in r.replace(";", "/").split("/") if x.strip()]
                if len(parts) == 2:
                    ref = parts
                    break
        # Orient: end0 = the end you land over on designators[0]. The lo end
        # (smaller projection along the seed bearing) is where a pilot
        # flying the seed bearing enters.
        desig = ref if ref else designators_from_bearing(sb)
        # designators_from_bearing()[0] is the seed bearing's own number, so
        # lo is its threshold. With an OSM ref, match the ref's first
        # designator to the closer bearing.
        if ref:
            try:
                r0 = int("".join(ch for ch in ref[0] if ch.isdigit())) * 10.0
                if bearing_diff_180(r0, sb) < 45.0 and abs(((r0 - sb) + 180.0) % 360.0 - 180.0) > 90.0:
                    lo, hi = hi, lo
            except ValueError:
                pass
        widths = [parse_width_m(w["tags"].get("width")) for w in group]
        widths = [x for x in widths if x]
        surf_tag = next((w["tags"].get("surface") for w in group if w["tags"].get("surface")), "")
        surface = map_surface(surf_tag) if surf_tag else "grass"
        if surface == "other":
            surface = "grass"
        width = max(widths) if widths else (30.0 if surface in ("grass", "dirt", "gravel") else 30.0)
        hdg0 = sb
        if lo is not hi and lo[0] > hi[0]:
            hdg0 = (sb + 180.0) % 360.0
        out.append({
            "designators": desig,
            "ends": [
                {"ident": desig[0], "pavement": [round(lo[1][0], 6), round(lo[1][1], 6)], "heading_deg": round(hdg0, 1)},
                {"ident": desig[1], "pavement": [round(hi[1][0], 6), round(hi[1][1], 6)], "heading_deg": round((hdg0 + 180.0) % 360.0, 1)},
            ],
            "width_m": round(width, 1),
            "length_m": round(length, 0),
            "surface": surface,
            "lighted": False,
            "geometry_source": "osm_only",
            "flags": ["no_ourairports_row"] + ([] if ref else ["designators_from_bearing"]),
        })
    return out


def _duplicates_existing(rw, runways, loc):
    """True when an OSM-only strip lies on an already-built runway of the
    same airport (same bearing, centreline within the widths)."""
    (la0, lo0), (la1, lo1) = rw["ends"][0]["pavement"], rw["ends"][1]["pavement"]
    a0, a1 = loc.xy(la0, lo0), loc.xy(la1, lo1)
    ab = bearing_deg(*a0, *a1)
    am = ((a0[0] + a1[0]) / 2, (a0[1] + a1[1]) / 2)
    for other in runways:
        (lb0, lb1) = other["ends"][0]["pavement"], other["ends"][1]["pavement"]
        b0, b1 = loc.xy(*lb0), loc.xy(*lb1)
        bb = bearing_deg(*b0, *b1)
        if bearing_diff_180(ab, bb) > BEARING_TOL_DEG:
            continue
        L = math.hypot(b1[0] - b0[0], b1[1] - b0[1])
        if L < 1.0:
            continue
        ux, uy = (b1[0] - b0[0]) / L, (b1[1] - b0[1]) / L
        dx, dy = am[0] - b0[0], am[1] - b0[1]
        lateral = abs(dx * uy - dy * ux)
        along = dx * ux + dy * uy
        if lateral <= 0.5 * (rw["width_m"] + other["width_m"]) + 5.0 and -100.0 <= along <= L + 100.0:
            return True
    return False


def build_region(cfg, region, offline):
    rid = region["id"]
    iso = region.get("iso_a2") or []
    if not iso:
        print(f"  {rid}: no iso_a2 — skipped")
        return
    plan = Path(cfg["build_root"]) / rid / "plan.json"
    bbox = None
    if plan.exists():
        with open(plan) as f:
            bbox = json.load(f).get("bbox_wgs84")
    if bbox is None:
        bbox = region.get("bbox_wgs84")
    if bbox is None:
        sys.exit(f"{rid}: no bbox (run the terrain plan first)")

    osm = fetch_osm_runways(cfg, region, bbox, offline)
    ways = []
    areas = 0
    for el in osm.get("elements", []):
        if el.get("type") != "way" or "geometry" not in el:
            continue
        g = el["geometry"]
        tags = el.get("tags", {})
        closed = len(g) > 2 and g[0] == g[-1]
        if closed or tags.get("area") == "yes":
            areas += 1
            continue
        ways.append({"id": el["id"], "tags": tags,
                     "_ll": [(p["lat"], p["lon"]) for p in g]})

    # Taxiways and aprons: assigned to the nearest airport reference point
    # within its reach (large 4 km, medium 3 km, small 2 km).
    airfield = fetch_osm_airfield(cfg, region, bbox, offline)
    taxiways = []
    aprons = []
    for el in airfield.get("elements", []):
        tags = el.get("tags", {})
        kind = tags.get("aeroway")
        if el.get("type") == "way" and "geometry" in el:
            g = [(p["lat"], p["lon"]) for p in el["geometry"]]
            if kind == "taxiway" and len(g) >= 2:
                taxiways.append({"ll": g, "tags": tags})
            elif kind == "apron" and len(g) >= 4:
                if g[0] != g[-1]:
                    g = g + [g[0]]
                aprons.append({"ll": g, "tags": tags})
        elif el.get("type") == "relation" and kind == "apron":
            for m in el.get("members", []):
                if m.get("type") == "way" and m.get("role", "outer") in ("outer", "") and "geometry" in m:
                    g = [(p["lat"], p["lon"]) for p in m["geometry"]]
                    if len(g) >= 4:
                        if g[0] != g[-1]:
                            g = g + [g[0]]
                        aprons.append({"ll": g, "tags": tags})
    n_taxi_in, n_apron_in = len(taxiways), len(aprons)

    lights_osm = fetch_osm_lights(cfg, region, bbox, offline)
    lights = []
    for el in lights_osm.get("elements", []):
        tags = el.get("tags", {})
        kind = tags.get("navigationaid", "")
        if el.get("type") != "node" or kind not in LIGHT_KINDS:
            continue
        rec = {"kind": kind, "lat": round(el["lat"], 6), "lon": round(el["lon"], 6)}
        if tags.get("light:colour"):
            rec["colour"] = tags["light:colour"]
        try:
            rec["dir"] = float(tags["light:direction"])
        except (KeyError, ValueError):
            pass
        lights.append(rec)
    n_lights_in = len(lights)

    airports = load_ourairports(cfg, iso, bbox, offline)
    reach = {"international_airport": 4000.0, "regional_airport": 3000.0, "grass_strip": 2000.0}
    ap_list = list(airports.values())
    ap_lat = [a["lat"] for a in ap_list]
    ap_lon = [a["lon"] for a in ap_list]

    def nearest_airport(lat, lon):
        best, bd = None, None
        for i, a in enumerate(ap_list):
            d = math.hypot((lat - ap_lat[i]) * 111320.0, (lon - ap_lon[i]) * 111320.0 * math.cos(math.radians(lat)))
            if d <= reach.get(a["type"], 2000.0) and (bd is None or d < bd):
                best, bd = a, d
        return best

    for tw in taxiways:
        mid = tw["ll"][len(tw["ll"]) // 2]
        a = nearest_airport(*mid)
        if a is None:
            continue
        w = parse_width_m(tw["tags"].get("width")) or 23.0
        surf = map_surface(tw["tags"].get("surface", "")) if tw["tags"].get("surface") else "asphalt"
        a.setdefault("_taxiways", []).append({
            "path": [[round(la, 6), round(lo, 6)] for la, lo in tw["ll"]],
            "width_m": round(w, 1),
            "surface": "asphalt" if surf == "other" else surf,
            "ref": tw["tags"].get("ref", ""),
        })
    for apn in aprons:
        lat_c = sum(p[0] for p in apn["ll"]) / len(apn["ll"])
        lon_c = sum(p[1] for p in apn["ll"]) / len(apn["ll"])
        a = nearest_airport(lat_c, lon_c)
        if a is None:
            continue
        surf = map_surface(apn["tags"].get("surface", "")) if apn["tags"].get("surface") else "concrete"
        a.setdefault("_aprons", []).append({
            "polygon": [[round(la, 6), round(lo, 6)] for la, lo in apn["ll"]],
            "surface": "concrete" if surf == "other" else surf,
        })
    for lt in lights:
        a = nearest_airport(lt["lat"], lt["lon"])
        if a is None:
            continue
        a.setdefault("_lights", []).append(lt)
    out_airports = []
    n_taxi = n_apron = n_light = 0
    flags_out = []
    n_rw = n_osm = 0
    for aid, ap in sorted(airports.items(), key=lambda kv: kv[1]["icao"]):
        loc = Local(ap["lat"], ap["lon"])
        near = []
        for wy in ways:
            # cheap prefilter on the first node
            d = math.hypot(*loc.xy(*wy["_ll"][0]))
            if d > ARP_SEARCH_M:
                continue
            xy = [loc.xy(*ll) for ll in wy["_ll"]]
            seg = sum(math.hypot(xy[i + 1][0] - xy[i][0], xy[i + 1][1] - xy[i][1]) for i in range(len(xy) - 1))
            near.append({**wy, "_xy": xy, "_len": seg,
                         "_mid": ((xy[0][0] + xy[-1][0]) / 2, (xy[0][1] + xy[-1][1]) / 2)})
        runways = []
        claimed = set()
        refs = {norm_designators(w["tags"].get("ref")) for w in near}
        rows = ap["_runways"]
        # Runways whose designators appear as an OSM ref claim their ways
        # first, so a parallel strip without a ref cannot steal them.
        rows = sorted(rows, key=lambda r: 0 if norm_designators(
            "/".join(d for d in ((r.get("le_ident") or "").strip(), (r.get("he_ident") or "").strip()) if d)) in refs else 1)
        for row in rows:
            rw = build_runway(ap, row, near, loc, flags_out, claimed)
            if rw is None:
                continue
            runways.append(rw)
            n_rw += 1
            if rw["geometry_source"] == "osm":
                n_osm += 1
        # OurAirports has no runway row (or none with geometry) for most
        # small airfields; OSM usually has the strip. Unclaimed runway ways
        # near the reference point become runways of their own.
        for rw in osm_only_runways(ap, near, loc, claimed):
            if _duplicates_existing(rw, runways, loc):
                continue
            runways.append(rw)
            n_rw += 1
            n_osm += 1
            flags_out.append((ap["icao"], "/".join(rw["designators"]), rw["flags"]))
        runways.sort(key=lambda r: (-r["length_m"], r["designators"][0]))
        entry = {k: v for k, v in ap.items() if not k.startswith("_")}
        entry["type"] = refine_type(entry["type"], runways)
        entry["runways"] = runways
        if ap.get("_taxiways"):
            entry["taxiways"] = ap["_taxiways"]
            n_taxi += len(ap["_taxiways"])
        if ap.get("_aprons"):
            entry["aprons"] = ap["_aprons"]
            n_apron += len(ap["_aprons"])
        if ap.get("_lights"):
            entry["lights"] = ap["_lights"]
            n_light += len(ap["_lights"])
        out_airports.append(entry)

    subdir = region.get("subdir", region["name"])
    out_dir = Path(cfg["build_root"]) / rid / subdir / "airports"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = {
        "schema_version": 1,
        "region": rid,
        "built": date.today().isoformat(),
        "sources": [
            "OurAirports (public domain): airports.csv, runways.csv",
            "OpenStreetMap contributors (ODbL): aeroway=runway ways via Overpass",
        ],
        "notes": ("taxiways[].path / aprons[].polygon: WGS84 [lat, lon] (+ elev_m once the "
                  "terrain flatten stage has run); ends[i].pavement = end of the paved/graded strip (OSM centreline "
                  "extreme, or the OurAirports threshold when no OSM match); "
                  "ends[i].end_ourairports = the OurAirports runway-end coordinate (cross-check); "
                  "displaced_m is measured inward from the pavement end. Coordinates WGS84, "
                  "metres; elev_m = threshold elevation (absent: sample terrain)."),
        "airports": out_airports,
    }
    with open(out_dir / "runways.json", "w") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)

    report = Path(cfg["build_root"]) / rid / "runways_report.txt"
    with open(report, "w") as f:
        f.write(f"{rid}: {len(out_airports)} airports, {n_rw} runways, "
                f"{n_osm} with OSM geometry ({100.0 * n_osm / max(1, n_rw):.0f}%), "
                f"{len(ways)} OSM runway ways in bbox ({areas} area-mapped ignored)\n")
        f.write(f"taxiways {n_taxi} of {n_taxi_in} in bbox assigned, aprons {n_apron} of {n_apron_in}, "
                f"mapped lights {n_light} of {n_lights_in}\n")
        f.write(f"{len(flags_out)} runways flagged:\n")
        for icao, desig, fl in flags_out:
            f.write(f"  {icao} {desig}: {', '.join(fl)}\n")
    print(f"  {rid}: {len(out_airports)} airports, {n_rw} runways, {n_osm} from OSM "
          f"({100.0 * n_osm / max(1, n_rw):.0f}%), {n_taxi} taxiways, {n_apron} aprons, {n_light} mapped lights, "
          f"{len(flags_out)} flagged → {report.name}")
    print(f"  written: {out_dir / 'runways.json'}")


def main():
    p = argparse.ArgumentParser(description="per-pack runways.json from OSM + OurAirports")
    p.add_argument("--region", action="append", default=[])
    p.add_argument("--all", action="store_true")
    p.add_argument("--offline", action="store_true")
    args = p.parse_args()
    cfg = load_config()
    by_id = {e["id"]: e for e in cfg["regions"]}
    ids = [e["id"] for e in cfg["regions"] if e.get("iso_a2")] if args.all else args.region
    if not ids:
        p.error("give --region <id> or --all")
    print(f"=== {date.today().isoformat()}: runways ===")
    for rid in ids:
        if rid not in by_id:
            sys.exit(f"unknown region '{rid}'")
        build_region(cfg, by_id[rid], args.offline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
