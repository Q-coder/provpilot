#!/usr/bin/env python3
"""
sync_catalog_runways.py — the runways of the baked regions into the bundled
airport catalog.

provpilot/airports.json is what the welcome screen lists before any scenery
pack is installed. Its runways came from the first catalog builds (a
hand-curated Swiss list with one runway for Zurich, 2400 aerodromes without
any); the full set lived only in each region's airports/runways.json and
reached the list once that pack was installed (RunwaysLib.merge_into_catalog).
A clean install of 1.1.0 therefore offered Zurich without runways 10/28 and
14/32. This script copies every pack runway into the catalog in the catalog's
own form (designator "A/B", true heading of A, length, surface, plus the two
thresholds as WGS84 [lat, lon], which AirportsLib prefers over a runway
centred on the airport's reference point), so the list
is complete without a pack; with the pack installed the runtime merge still
replaces it by the pack data with its real thresholds.

Run after build_world_runways.py / before a release:
    python scripts/sync_catalog_runways.py            # every region of world_regions.json
    python scripts/sync_catalog_runways.py --dry-run
"""

import argparse
import json
import math
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REGIONS_JSON = REPO / "scripts" / "world_regions.json"
CATALOG = REPO / "provpilot" / "airports.json"


def bearing(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return math.degrees(math.atan2(y, x)) % 360.0


def distance_m(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    return 2 * 6371008.8 * math.asin(math.sqrt(a))


def catalog_runways(ap):
    """Same selection as RunwaysLib::catalog_runways (two ends with pavement)."""
    out = []
    for rw in ap.get("runways", []):
        ends, desigs = rw.get("ends", []), rw.get("designators", [])
        if len(ends) != 2 or len(desigs) != 2:
            continue
        if not all("pavement" in e for e in ends):
            continue
        (la0, lo0), (la1, lo1) = ends[0]["pavement"][:2], ends[1]["pavement"][:2]
        length = distance_m(la0, lo0, la1, lo1)
        if length < 1.0:
            continue
        hdg = ends[0].get("heading_deg")
        if hdg is None:
            hdg = bearing(la0, lo0, la1, lo1)
        # thresholds: the pavement ends moved inward by the displacement
        f0 = float(ends[0].get("displaced_m", 0.0)) / length
        f1 = float(ends[1].get("displaced_m", 0.0)) / length
        out.append({
            "designator": "%s/%s" % (desigs[0], desigs[1]),
            "heading_deg": round(float(hdg), 1),
            "length_m": int(round(float(rw.get("length_m", length)))),
            "surface": rw.get("surface", "asphalt"),
            "thresholds": [
                [round(la0 + (la1 - la0) * f0, 6), round(lo0 + (lo1 - lo0) * f0, 6)],
                [round(la1 + (la0 - la1) * f1, 6), round(lo1 + (lo0 - lo1) * f1, 6)],
            ],
        })
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    regions = json.loads(REGIONS_JSON.read_text(encoding="utf-8"))
    build_root = Path(regions["build_root"])
    doc = json.loads(CATALOG.read_text(encoding="utf-8"))
    by_icao = {a["icao"]: a for a in doc["airports"]}

    changed = unknown = 0
    for r in regions["regions"]:
        f = build_root / r["id"] / r["subdir"] / "airports" / "runways.json"
        if not f.is_file():
            print("  %-18s no %s" % (r["id"], f))
            continue
        aps = json.loads(f.read_text(encoding="utf-8"))["airports"]
        items = aps.items() if isinstance(aps, dict) else [(a["icao"], a) for a in aps]
        n = 0
        for icao, pack_ap in items:
            rws = catalog_runways(pack_ap)
            if not rws:
                continue
            entry = by_icao.get(icao)
            if entry is None:
                unknown += 1
                continue
            if entry.get("runways") != rws:
                entry["runways"] = rws
                n += 1
        changed += n
        print("  %-18s %4d airports updated" % (r["id"], n))
    without = sum(1 for a in doc["airports"] if not a.get("runways"))
    print("catalog: %d airports, %d updated, %d still without a runway, %d pack airports not in the catalog"
          % (len(doc["airports"]), changed, without, unknown))
    if not args.dry_run and changed:
        CATALOG.write_text(json.dumps(doc, indent=2), encoding="utf-8")   # the form build_world_airports.py writes
        print("written:", CATALOG)


if __name__ == "__main__":
    main()
