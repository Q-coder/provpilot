#!/usr/bin/env python3
"""
build_world_airports.py — OurAirports → bundled airport catalog (Norway pilot).

Downloads the daily OurAirports dump (airports.csv + runways.csv), selects
the Norwegian aerodromes with type ∈ {large_airport, medium_airport}
(design §5.1: all international/primary airports, expected ~20–30) and
merges them into provpilot/airports.json — the catalog AirportsLib loads at
runtime. Coordinates stay WGS84 decimal degrees; the LV95 conversion happens
in GDScript (airports_lib.gd). Runway heading_deg is the TRUE bearing of the
lower-numbered designator, matching the catalog contract.

Re-runs are idempotent: existing region="norway" entries are replaced.

Usage:
    python scripts/build_world_airports.py            # download + merge
    python scripts/build_world_airports.py --offline  # use cached CSVs only

CSV cache: /Volumes/Data1/provpilot/build/norway/ourairports/
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
NORWAY_DIR = Path("/Volumes/Data1/provpilot/build/norway")
CACHE_DIR = NORWAY_DIR / "ourairports"
CATALOG = Path(__file__).resolve().parent.parent / "provpilot" / "airports.json"

REGION_ID = "norway"
FT_TO_M = 0.3048

# OurAirports type → catalog type (welcome_screen display classes).
WANTED_TYPES = {
    "large_airport": "international_airport",
    "medium_airport": "regional_airport",
}


def _http_get(url, timeout=120):
    req = urllib.request.Request(
        url, headers={"User-Agent": "provpilot-build/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def ensure_csv(name: str, offline: bool) -> Path:
    path = CACHE_DIR / name
    if path.exists() and path.stat().st_size > 0:
        return path
    if offline:
        sys.exit(f"--offline but no cached {name} in {CACHE_DIR}")
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
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


def build_entries(offline: bool = False) -> list:
    ensure_csv("airports.csv", offline)
    ensure_csv("runways.csv", offline)

    wanted = {}
    with open(CACHE_DIR / "airports.csv", newline="") as f:
        for r in csv.DictReader(f):
            if r["iso_country"] != "NO":
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
    with open(CACHE_DIR / "runways.csv", newline="") as f:
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
        entries.append({
            "icao": a["icao"],
            "name": a["name"],
            "lat": round(a["lat"], 5),
            "lon": round(a["lon"], 5),
            "elevation_m": int(round(a["elevation_ft"] * FT_TO_M)),
            "type": a["type"],
            "region": REGION_ID,
            "runways": sorted(runways.get(aid, []),
                              key=lambda rw: (-rw["length_m"], rw["designator"])),
        })
    entries.sort(key=lambda e: (e["icao"], e["name"]))
    return entries


def merge_into_catalog(entries: list) -> None:
    with open(CATALOG) as f:
        catalog = json.load(f)
    kept = [a for a in catalog.get("airports", [])
            if a.get("region") != REGION_ID]
    catalog["airports"] = kept + entries
    with open(CATALOG, "w") as f:
        json.dump(catalog, f, indent=2)
        f.write("\n")


def main() -> int:
    p = argparse.ArgumentParser(
        description="OurAirports → provpilot/airports.json (Norway pilot)")
    p.add_argument("--offline", action="store_true",
                   help="use cached CSVs only (no download)")
    args = p.parse_args()

    print(f"=== {date.today().isoformat()}: OurAirports → {CATALOG.name} "
          f"(region={REGION_ID}) ===")
    entries = build_entries(offline=args.offline)
    n_rwy = sum(len(e["runways"]) for e in entries)
    print(f"  {len(entries)} Norwegian aerodromes (large/medium), "
          f"{n_rwy} runways")
    for e in entries:
        rwy = ", ".join(r["designator"] for r in e["runways"]) or "-"
        print(f"   {e['icao']}  {e['name'][:40]:<40} "
              f"{e['elevation_m']:>5} m  {rwy}")

    merge_into_catalog(entries)
    print(f"  catalog updated: {CATALOG}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

