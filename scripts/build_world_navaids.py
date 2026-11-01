#!/usr/bin/env python3
"""
build_world_navaids.py — OurAirports → bundled radio navaid catalog, per region.

Downloads the daily OurAirports navaids dump (navaids.csv, public domain,
https://ourairports.com/data/) and merges the stations of a region's
countries (`iso_a2` in scripts/world_regions.json, clipped to the region's
bbox like the airports) into provpilot/navaids.json — the catalog NavaidsLib
loads at runtime for the VOR / DME / NDB receivers (docs/RADIO_NAVIGATION.md).

Station types kept: VOR, VOR-DME, VORTAC, DME, TACAN, NDB, NDB-DME. Per
station: ident, name, type, frequency (kHz for everything: 114850 = 114.85
MHz, 375 = 375 kHz), WGS84 position, elevation, the paired DME frequency,
the station's declination (`slaved_variation_deg`: the magnetic north the
VOR's radials are referenced to; OurAirports' `magnetic_variation_deg` as
the fallback) and the power class that sets the nominal reception range.

Re-runs are idempotent: existing entries with the same region id are replaced.

Usage:
    python scripts/build_world_navaids.py --region switzerland-base
    python scripts/build_world_navaids.py --all [--refresh]   # every region with iso_a2
    python scripts/build_world_navaids.py --region norway --offline

CSV cache: <cache_root>/ourairports/navaids.csv (shared with the airports).
"""

import argparse
import csv
import json
import sys
import urllib.request
from datetime import date
from pathlib import Path

OURAIRPORTS_BASE = "https://davidmegginson.github.io/ourairports-data"
REPO = Path(__file__).resolve().parent.parent
REGIONS_JSON = REPO / "scripts" / "world_regions.json"
CATALOG = REPO / "provpilot" / "navaids.json"

FT_TO_M = 0.3048

# OurAirports type → catalog type. Localizers / glideslopes are not in the
# navaids dump (ILS is out of scope for the VOR / NDB receivers anyway).
WANTED_TYPES = {"VOR", "VOR-DME", "VORTAC", "DME", "TACAN", "NDB", "NDB-DME"}

COMMENT = ("Radio navaids (VOR, VOR-DME, VORTAC, DME, TACAN, NDB, NDB-DME) from "
           "OurAirports navaids.csv (public domain), built by "
           "scripts/build_world_navaids.py. freq_khz is the frequency in kHz "
           "for every type (114850 = 114.85 MHz, 375 = 375 kHz); dme_khz the "
           "paired DME channel's frequency; station_var_deg the magnetic "
           "declination the station's radials are slaved to (east positive, "
           "null when unknown); power LOW / MEDIUM / HIGH sets the nominal "
           "range. Region IDs match scripts/world_regions.json.")


def load_config():
    with open(REGIONS_JSON) as f:
        return json.load(f)


def _http_get(url, timeout=120):
    req = urllib.request.Request(url, headers={"User-Agent": "provpilot-build/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def ensure_csv(cfg, name: str, offline: bool, refresh: bool) -> Path:
    cache_dir = Path(cfg["cache_root"]) / "ourairports"
    path = cache_dir / name
    if path.exists() and path.stat().st_size > 0 and not refresh:
        return path
    if offline:
        if path.exists():
            return path
        sys.exit(f"--offline but no cached {name} in {cache_dir}")
    cache_dir.mkdir(parents=True, exist_ok=True)
    url = f"{OURAIRPORTS_BASE}/{name}"
    print(f"  downloading {url}")
    path.write_bytes(_http_get(url))
    return path


def _float(s, default=None):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def build_entries(cfg, region_id: str, iso_a2: list, offline: bool, refresh: bool,
                  bbox_wgs84=None) -> list:
    navaids_csv = ensure_csv(cfg, "navaids.csv", offline, refresh)
    countries = set(iso_a2)
    entries = []
    with open(navaids_csv, newline="") as f:
        for r in csv.DictReader(f):
            if r["iso_country"] not in countries or r["type"] not in WANTED_TYPES:
                continue
            lat = _float(r["latitude_deg"])
            lon = _float(r["longitude_deg"])
            freq = _float(r["frequency_khz"])
            if lat is None or lon is None or not freq:
                continue
            if bbox_wgs84 is not None:
                w, s_, e, n = bbox_wgs84
                if not (w <= lon <= e and s_ <= lat <= n):
                    continue
            elev_ft = _float(r["elevation_ft"])
            slaved = _float(r.get("slaved_variation_deg"))
            magvar = _float(r.get("magnetic_variation_deg"))
            dme = _float(r.get("dme_frequency_khz"))
            entry = {
                "ident": r["ident"].strip(),
                "name": r["name"].strip(),
                "type": r["type"],
                "freq_khz": int(round(freq)),
                "lat": round(lat, 5),
                "lon": round(lon, 5),
                "elev_m": int(round(elev_ft * FT_TO_M)) if elev_ft is not None else None,
                "dme_khz": int(round(dme)) if dme else None,
                # VOR radials are referenced to the declination the station
                # was slaved to; OurAirports' per-station magnetic variation
                # is the fallback. NDBs have no slaved value.
                "station_var_deg": slaved if slaved is not None else magvar,
                "power": (r.get("power") or "").strip() or None,
                "usage": (r.get("usageType") or "").strip() or None,
                "airport": (r.get("associated_airport") or "").strip() or None,
                "region": region_id,
            }
            entries.append(entry)
    entries.sort(key=lambda e: (e["ident"], e["type"], e["name"]))
    return entries


def merge_into_catalog(region_id: str, entries: list) -> None:
    catalog = {"_comment": COMMENT, "navaids": []}
    if CATALOG.exists():
        with open(CATALOG) as f:
            catalog = json.load(f)
        catalog["_comment"] = COMMENT
    kept = [a for a in catalog.get("navaids", []) if a.get("region") != region_id]
    catalog["navaids"] = kept + entries
    catalog["source"] = f"OurAirports navaids.csv, {date.today().isoformat()}"
    with open(CATALOG, "w") as f:
        json.dump(catalog, f, indent=1)
        f.write("\n")


def main() -> int:
    p = argparse.ArgumentParser(description="OurAirports → provpilot/navaids.json, per region")
    p.add_argument("--region", action="append", default=[],
                   help="region id from scripts/world_regions.json (repeatable)")
    p.add_argument("--all", action="store_true", help="every region that lists iso_a2 codes")
    p.add_argument("--offline", action="store_true", help="use the cached CSV only (no download)")
    p.add_argument("--refresh", action="store_true", help="re-download the CSV even when cached")
    p.add_argument("--verbose", action="store_true", help="list every station")
    args = p.parse_args()

    cfg = load_config()
    by_id = {e["id"]: e for e in cfg["regions"]}
    ids = [e["id"] for e in cfg["regions"] if e.get("iso_a2")] if args.all else args.region
    if not ids:
        p.error("give --region <id> (repeatable) or --all")

    print(f"=== {date.today().isoformat()}: OurAirports navaids → {CATALOG.name} ===")
    refresh = args.refresh
    for rid in ids:
        entry = by_id.get(rid)
        if entry is None:
            sys.exit(f"unknown region '{rid}' — known: {', '.join(by_id)}")
        iso = entry.get("iso_a2") or []
        if not iso:
            sys.exit(f"region '{rid}' has no iso_a2 codes")
        entries = build_entries(cfg, rid, iso, offline=args.offline, refresh=refresh,
                                bbox_wgs84=entry.get("bbox_wgs84"))
        refresh = False  # one download per run
        by_type = {}
        for e in entries:
            by_type[e["type"]] = by_type.get(e["type"], 0) + 1
        print(f"  {rid} ({','.join(iso)}): {len(entries)} navaids "
              + ", ".join(f"{k} {v}" for k, v in sorted(by_type.items())))
        if args.verbose:
            for e in entries:
                f = e["freq_khz"]
                ftxt = f"{f / 1000:.2f} MHz" if f >= 100000 else f"{f} kHz"
                print(f"   {e['ident']:<4} {e['type']:<8} {ftxt:>11}  {e['name'][:36]:<36} "
                      f"{e['lat']:8.4f} {e['lon']:8.4f}")
        merge_into_catalog(rid, entries)
    print(f"  catalog updated: {CATALOG}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
