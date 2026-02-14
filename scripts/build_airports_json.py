#!/usr/bin/env python3
"""
build_airports_json.py — Build airports.json from MANUAL_RUNWAYS + metadata.

Replaces the GeoPackage-dependent generate_airports.py for creating
scenery/poi/airports.json.  Uses the OSM-verified MANUAL_RUNWAYS data
from generate_airports.py plus hardcoded metadata (name, type, elevation).

Usage:
    python scripts/build_airports_json.py
"""

import json
import math
import sys
from pathlib import Path

# ── Import MANUAL_RUNWAYS from generate_airports.py ────────────────────
# We exec() the file and extract the dict rather than importing,
# because generate_airports.py has dependencies (fiona, shapely) we
# may not want to require.
_gen_path = Path(__file__).parent / "generate_airports.py"
_gen_ns = {}
with open(_gen_path) as f:
    source = f.read()
# Extract just the MANUAL_RUNWAYS dict by finding it in the source
start = source.index("\nMANUAL_RUNWAYS = {")
# Find matching closing brace
depth = 0
end = start
for i, ch in enumerate(source[start:], start):
    if ch == '{':
        depth += 1
    elif ch == '}':
        depth -= 1
        if depth == 0:
            end = i + 1
            break
manual_runways_source = source[start:end]
exec(manual_runways_source, _gen_ns)
MANUAL_RUNWAYS = _gen_ns["MANUAL_RUNWAYS"]


# ── Airport metadata ──────────────────────────────────────────────────
# (name, type, elevation_m, area_ha)
# Types: international_airport, regional_airport, grass_strip, heliport
# Elevations from Swiss AIP / OurAirports.  area_ha estimated.

AIRPORT_META = {
    # International
    "LSZH": ("Zürich",                       "international_airport", 432, 880.0),
    "LSGG": ("Genève",                       "international_airport", 430, 340.0),
    # Military / large regional
    "LSMD": ("Dübendorf",                    "regional_airport",      440, 230.0),
    "LSME": ("Emmen",                        "regional_airport",      430, 200.0),
    "LSMA": ("Alpnach",                      "regional_airport",      460, 100.0),
    "LSMF": ("Mollis",                       "regional_airport",      435, 120.0),
    "LSMM": ("Meiringen",                    "regional_airport",      590, 150.0),
    "LSMP": ("Payerne",                      "regional_airport",      465, 350.0),
    # Regional airports
    "LSZB": ("Bern-Belp",                    "regional_airport",      510, 140.0),
    "LSZC": ("Buochs",                       "regional_airport",      450, 120.0),
    "LSZF": ("Birrfeld",                     "regional_airport",      400, 40.0),
    "LSZG": ("Grenchen",                     "regional_airport",      430, 60.0),
    "LSZL": ("Locarno",                      "regional_airport",      198, 40.0),
    "LSZR": ("St. Gallen-Altenrhein",        "regional_airport",      398, 80.0),
    "LSZS": ("Samedan",                      "regional_airport",     1707, 100.0),
    "LSGS": ("Sion",                         "regional_airport",      482, 130.0),
    "LSZA": ("Lugano-Agno",                  "regional_airport",      279, 45.0),
    "LSGL": ("Lausanne-La Blécherette",      "regional_airport",      617, 50.0),
    "LSGC": ("La Chaux-de-Fonds-Les Eplatures", "regional_airport",  1017, 60.0),
    "LSGE": ("Ecuvillens",                   "regional_airport",      700, 40.0),
    # Grass strips / small airfields
    "LSZN": ("Schaffhausen",                 "grass_strip",           415, 15.0),
    "LSZV": ("Hausen am Albis",              "grass_strip",           600, 25.0),
    "LSZW": ("Thun",                         "grass_strip",           560, 25.0),
    "LSTS": ("St.Stephan",                   "grass_strip",           999, 50.0),
    "LSTA": ("Raron",                        "grass_strip",           636, 30.0),
    "LSTB": ("Reichenbach",                  "grass_strip",           700, 30.0),
    "LSTZ": ("Zweisimmen",                   "grass_strip",           940, 15.0),
    "LSGK": ("Saanen",                       "grass_strip",          1005, 40.0),
    "LSGN": ("Neuchâtel",                    "grass_strip",           434, 30.0),
    "LSGX": ("Bex",                          "grass_strip",           411, 15.0),
    "LSGY": ("Yverdon",                      "grass_strip",           432, 30.0),
    "LSPG": ("Kägiswil",                     "grass_strip",           475, 35.0),
    "LSPM": ("Ambrì-Piotta",                "grass_strip",          1000, 40.0),
    "LSPU": ("Münster",                      "grass_strip",          1352, 35.0),
    "LSML": ("Lodrino",                      "grass_strip",           297, 30.0),
    "LSPN": ("Triengen",                     "grass_strip",           440, 12.0),
    "LSPL": ("Langenthal",                   "grass_strip",           460, 12.0),
    "LSZQ": ("Bressaucourt",                 "grass_strip",           510, 30.0),
    "LSPD": ("Dittingen",                    "grass_strip",           370, 25.0),
    "LSZI": ("Fricktal-Schupfart",           "grass_strip",           410, 20.0),
    "LSZK": ("Speck-Fehraltorf",             "grass_strip",           525, 20.0),
    "LSZE": ("Bad Ragaz",                    "grass_strip",           496, 20.0),
    "LSZX": ("Schänis",                      "grass_strip",           416, 15.0),
    "LSPS": ("Sitterdorf",                   "grass_strip",           540, 15.0),
    "LSGT": ("Gruyères",                     "grass_strip",           677, 30.0),
    "LSZO": ("Luzern-Beromünster",           "grass_strip",           650, 20.0),
    "LSZP": ("Biel-Kappelen",               "grass_strip",           438, 25.0),
}


def build_airport(icao: str, runways: list) -> dict:
    """Build a single airport dict from MANUAL_RUNWAYS entry + metadata."""
    meta = AIRPORT_META.get(icao)
    if not meta:
        print(f"  WARNING: No metadata for {icao}, skipping", file=sys.stderr)
        return None

    name, ap_type, elevation, area_ha = meta

    # Compute centroid from runway thresholds
    all_e, all_n = [], []
    for rwy in runways:
        desig = rwy["designator"]
        parts = desig.split("/")
        for p in parts:
            key = f"threshold_{p}"
            if key in rwy:
                all_e.append(rwy[key]["e"])
                all_n.append(rwy[key]["n"])

    if not all_e:
        print(f"  WARNING: No thresholds for {icao}, skipping", file=sys.stderr)
        return None

    center_e = round(sum(all_e) / len(all_e))
    center_n = round(sum(all_n) / len(all_n))

    airport = {
        "icao": icao,
        "name": name,
        "type": ap_type,
        "elevation_m": elevation,
        "center_e": center_e,
        "center_n": center_n,
        "area_ha": area_ha,
        "runways": runways,
    }
    return airport


def main():
    airports = []
    for icao, runways in sorted(MANUAL_RUNWAYS.items()):
        ap = build_airport(icao, runways)
        if ap:
            airports.append(ap)

    output = {
        "type": "airports",
        "coordinate_system": "LV95",
        "source": "OSM Overpass + Swiss AIP, verified manually",
        "generated": True,
        "notes": (
            "Built by scripts/build_airports_json.py from OSM-verified "
            "MANUAL_RUNWAYS in generate_airports.py. "
            f"{len(airports)} airports."
        ),
        "airports": airports,
    }

    out_path = Path(__file__).parent.parent / "scenery" / "poi" / "airports.json"
    with open(out_path, "w") as f:
        json.dump(output, f, indent=2, ensure_ascii=False)

    print(f"Wrote {len(airports)} airports to {out_path}")

    # Summary
    by_type = {}
    for ap in airports:
        t = ap["type"]
        by_type[t] = by_type.get(t, 0) + 1
    for t, c in sorted(by_type.items()):
        print(f"  {t}: {c}")


if __name__ == "__main__":
    main()
