#!/usr/bin/env python3
"""
build_world_airports.py — OurAirports → bundled airport catalog, per region.

Downloads the daily OurAirports dump (airports.csv + runways.csv), selects
the aerodromes of a region's countries (`iso_a2` in scripts/world_regions.json)
with type ∈ {large_airport, medium_airport} (design §5.1: all international /
primary airports) and merges them into provpilot/airports.json — the catalog
AirportsLib loads at runtime. Coordinates stay WGS84 decimal degrees; the
grid conversion happens at runtime. Runway heading_deg is the TRUE bearing
of the lower-numbered designator, matching the catalog contract.

Re-runs are idempotent: existing entries with the same region id are replaced.

Usage:
    python scripts/build_world_airports.py --region germany [--region italy]
    python scripts/build_world_airports.py --all            # every region with iso_a2
    python scripts/build_world_airports.py --region norway --offline

CSV cache: <cache_root>/ourairports/ (the legacy Norway cache is read as a
fallback so the Fennoscandia rebuilds stay offline-capable).
"""

import argparse
import csv
import json
import math
import sys
import urllib.request
from datetime import date
from pathlib import Path

OURAIRPORTS_BASE = "https://davidmegginson.github.io/ourairports-data"
REPO = Path(__file__).resolve().parent.parent
REGIONS_JSON = REPO / "scripts" / "world_regions.json"
CATALOG = REPO / "provpilot" / "airports.json"

FT_TO_M = 0.3048

# OurAirports type → catalog type (welcome_screen display classes).
WANTED_TYPES = {
    "large_airport": "international_airport",
    "medium_airport": "regional_airport",
    # Small airfields (design §12, 2026-09-20: Schmerlat LSPF, Thun LSZW were
    # invisible): "grass_strip" unless a runway is paved — see refine_type().
    "small_airport": "grass_strip",
}


def refine_type(cat_type: str, runways: list) -> str:
    """A small airfield with a paved runway is listed as a regional airport;
    unpaved-only stays a grass strip (the welcome-screen filter classes)."""
    if cat_type != "grass_strip":
        return cat_type
    for rw in runways:
        if rw.get("surface") in ("asphalt", "concrete"):
            return "regional_airport"
    return cat_type


def load_config():
    with open(REGIONS_JSON) as f:
        return json.load(f)


def _http_get(url, timeout=120):
    req = urllib.request.Request(
        url, headers={"User-Agent": "provpilot-build/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def ensure_csv(cfg, name: str, offline: bool) -> Path:
    cache_dir = Path(cfg["cache_root"]) / "ourairports"
    path = cache_dir / name
    if path.exists() and path.stat().st_size > 0:
        return path
    for legacy in cfg.get("legacy_ourairports_caches", []):
        q = Path(legacy) / name
        if q.exists() and q.stat().st_size > 0:
            return q
    if offline:
        sys.exit(f"--offline but no cached {name} in {cache_dir}")
    cache_dir.mkdir(parents=True, exist_ok=True)
    url = f"{OURAIRPORTS_BASE}/{name}"
    print(f"  downloading {url}")
    data = _http_get(url)
    path.write_bytes(data)
    return path


def map_surface(code: str) -> str:
    """OurAirports surface code (ASPH-G, ASP, CONC, TURF, GRS, …) → catalog word."""
    token = (code or "").split("-")[0].strip().upper()
    return {
        "ASPH": "asphalt", "ASP": "asphalt",
        "CONC": "concrete", "CON": "concrete",
        "GRASS": "grass", "GRS": "grass", "GRE": "grass", "TURF": "grass",
        "GRAVEL": "gravel", "GVL": "gravel", "CHIP": "gravel",
        "CLAY": "dirt", "DIRT": "dirt", "SAND": "dirt", "EARTH": "dirt",
    }.get(token, "other")


def initial_bearing_deg(lat1, lon1, lat2, lon2) -> float:
    """Great-circle initial bearing (true) from point 1 to point 2."""
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dlam = math.radians(lon2 - lon1)
    y = math.sin(dlam) * math.cos(phi2)
    x = math.cos(phi1) * math.sin(phi2) - \
        math.sin(phi1) * math.cos(phi2) * math.cos(dlam)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def build_entries(cfg, region_id: str, iso_a2: list, offline: bool,
                  bbox_wgs84=None) -> list:
    airports_csv = ensure_csv(cfg, "airports.csv", offline)
    runways_csv = ensure_csv(cfg, "runways.csv", offline)
    countries = set(iso_a2)

    wanted = {}
    with open(airports_csv, newline="") as f:
        for r in csv.DictReader(f):
            if r["iso_country"] not in countries:
                continue
            if bbox_wgs84 is not None:
                # The region's bbox clips the terrain bake (metropolitan
                # France vs the overseas departments that share ISO "FR");
                # the catalog follows the terrain.
                try:
                    lat = float(r["latitude_deg"])
                    lon = float(r["longitude_deg"])
                except (ValueError, TypeError):
                    continue
                w, s_, e, n = bbox_wgs84
                if not (w <= lon <= e and s_ <= lat <= n):
                    continue
            cat_type = WANTED_TYPES.get(r["type"])
            if cat_type is None:
                continue
            wanted[r["id"]] = {
                "icao": r["icao_code"] or r["ident"],
                "name": r["name"],
                "lat": float(r["latitude_deg"]),
                "lon": float(r["longitude_deg"]),
                "elevation_ft": float(r["elevation_ft"] or 0),
                "type": cat_type,
            }

    runways = {}
    with open(runways_csv, newline="") as f:
        for r in csv.DictReader(f):
            if r["airport_ref"] not in wanted:
                continue
            if r.get("closed") == "1":
                continue
            le = (r.get("le_ident") or "").strip()
            he = (r.get("he_ident") or "").strip()
            designator = "/".join(x for x in (le, he) if x)
            if not designator:
                continue
            length_m = round(float(r["length_ft"] or 0) * FT_TO_M)
            # heading_deg contract: TRUE bearing of the lower-numbered end.
            try:
                heading = float(r["le_heading_degT"])
            except (ValueError, TypeError):
                # Rare rows lack a heading — derive it from the threshold
                # coordinates instead (LE → HE initial bearing).
                try:
                    heading = initial_bearing_deg(
                        float(r["le_latitude_deg"]), float(r["le_longitude_deg"]),
                        float(r["he_latitude_deg"]), float(r["he_longitude_deg"]))
                except (ValueError, TypeError):
                    continue
            runways.setdefault(r["airport_ref"], []).append({
                "designator": designator,
                "heading_deg": round(heading),
                "length_m": length_m,
                "surface": map_surface(r.get("surface", "")),
            })

    entries = []
    for aid, a in wanted.items():
        rws = runways.get(aid, [])
        entries.append({
            "icao": a["icao"],
            "name": a["name"],
            "lat": round(a["lat"], 5),
            "lon": round(a["lon"], 5),
            "elevation_m": int(round(a["elevation_ft"] * FT_TO_M)),
            "type": refine_type(a["type"], rws),
            "region": region_id,
            "runways": sorted(runways.get(aid, []),
                              key=lambda rw: (-rw["length_m"], rw["designator"])),
        })
    entries.sort(key=lambda e: (e["icao"], e["name"]))
    return entries


def merge_into_catalog(region_id: str, entries: list) -> None:
    with open(CATALOG) as f:
        catalog = json.load(f)
    kept = [a for a in catalog.get("airports", [])
            if a.get("region") != region_id]
    # Hand-curated entries of other regions (the CH/BW detail packs carry
    # their own airports with surveyed thresholds) win over OurAirports:
    # an ICAO already present under another region is not duplicated.
    taken = {a.get("icao") for a in kept if a.get("icao")}
    fresh = [e for e in entries if e["icao"] not in taken]
    if len(fresh) != len(entries):
        print(f"  {len(entries) - len(fresh)} already in the catalog under "
              f"another region — skipped")
    catalog["airports"] = kept + fresh
    with open(CATALOG, "w") as f:
        json.dump(catalog, f, indent=2)
        f.write("\n")


def main() -> int:
    p = argparse.ArgumentParser(
        description="OurAirports → provpilot/airports.json, per region")
    p.add_argument("--region", action="append", default=[],
                   help="region id from scripts/world_regions.json (repeatable)")
    p.add_argument("--all", action="store_true",
                   help="every region that lists iso_a2 codes")
    p.add_argument("--offline", action="store_true",
                   help="use cached CSVs only (no download)")
    p.add_argument("--verbose", action="store_true",
                   help="list every aerodrome")
    args = p.parse_args()

    cfg = load_config()
    by_id = {e["id"]: e for e in cfg["regions"]}
    if args.all:
        ids = [e["id"] for e in cfg["regions"] if e.get("iso_a2")]
    else:
        ids = args.region
    if not ids:
        p.error("give --region <id> (repeatable) or --all")

    print(f"=== {date.today().isoformat()}: OurAirports → {CATALOG.name} ===")
    for rid in ids:
        entry = by_id.get(rid)
        if entry is None:
            sys.exit(f"unknown region '{rid}' — known: {', '.join(by_id)}")
        iso = entry.get("iso_a2") or []
        if not iso:
            sys.exit(f"region '{rid}' has no iso_a2 codes")
        entries = build_entries(cfg, rid, iso, offline=args.offline,
                                bbox_wgs84=entry.get("bbox_wgs84"))
        n_rwy = sum(len(e["runways"]) for e in entries)
        print(f"  {rid} ({','.join(iso)}): {len(entries)} aerodromes "
              f"(large/medium), {n_rwy} runways")
        if args.verbose:
            for e in entries:
                rwy = ", ".join(r["designator"] for r in e["runways"]) or "-"
                print(f"   {e['icao']}  {e['name'][:40]:<40} "
                      f"{e['elevation_m']:>5} m  {rwy}")
        merge_into_catalog(rid, entries)
    print(f"  catalog updated: {CATALOG}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
