#!/usr/bin/env python3
"""
Generate MANUAL_RUNWAYS dictionary from OSM data, matched to our airports.json.
Groups multi-segment ways, excludes tiny displaced-threshold stubs, and
uses OSM declared lengths when available.
"""

import json
import math

with open("scenery/poi/airports.json") as f:
    airports_data = json.load(f)
    airports = airports_data["airports"]

with open("/tmp/osm_swiss_runways.json") as f:
    osm_runways = json.load(f)


def dist(e1, n1, e2, n2):
    return math.sqrt((e2 - e1) ** 2 + (n2 - n1) ** 2)


# Build a mapping: ICAO -> airport center
ap_centers = {}
for ap in airports:
    icao = ap.get("icao", "")
    if icao and ap.get("type") != "heliport":
        ap_centers[icao] = (ap["center_e"], ap["center_n"], ap["name"], ap["type"])

# For each ICAO airport, find OSM runways within 2500m  
# and build proper runway entries
results = {}

for icao, (ce, cn, name, atype) in sorted(ap_centers.items()):
    nearby = []
    for r in osm_runways:
        d = dist(ce, cn, r["center_e"], r["center_n"])
        if d < 2500:
            nearby.append(r)

    if not nearby:
        continue

    # Group by ref (designator), keeping only ref'd runways
    # Also include "?" refs if they're the only ones and are substantial
    by_ref = {}
    for r in nearby:
        ref = r["ref"]
        if ref not in by_ref:
            by_ref[ref] = []
        by_ref[ref].append(r)

    # Build runway entries from named refs
    rwy_entries = []
    for ref, segments in by_ref.items():
        if ref == "?":
            continue  # Handle below if needed
        
        # Filter out tiny stubs (< 100m) that are displaced threshold markers
        real_segments = [s for s in segments if s["computed_length"] > 100]
        if not real_segments:
            continue

        # Find the two most distant endpoints among all segments
        all_points = []
        for seg in real_segments:
            all_points.append((seg["e1"], seg["n1"]))
            all_points.append((seg["e2"], seg["n2"]))

        max_d = 0
        p_start = all_points[0]
        p_end = all_points[-1]
        for i in range(len(all_points)):
            for j in range(i + 1, len(all_points)):
                d = dist(all_points[i][0], all_points[i][1],
                         all_points[j][0], all_points[j][1])
                if d > max_d:
                    max_d = d
                    p_start = all_points[i]
                    p_end = all_points[j]

        # Get properties from the longest real segment
        longest = max(real_segments, key=lambda r: r["computed_length"])
        surface = longest["surface"]
        if surface in ("unknown", ""):
            surface = "asphalt"
        width = longest["width"] if longest["width"] > 0 else 30

        # Use OSM declared length if available
        osm_len_str = longest.get("osm_length", "")
        if osm_len_str:
            try:
                length = int(float(osm_len_str))
            except ValueError:
                length = round(max_d)
        else:
            length = round(max_d)

        # Compute heading from p_start → p_end
        de = p_end[0] - p_start[0]
        dn = p_end[1] - p_start[1]
        heading = round(math.degrees(math.atan2(de, dn)) % 360)

        # Normalize heading to [0, 180) and ensure p_start is the "lower" threshold
        if heading >= 180:
            heading -= 180
            p_start, p_end = p_end, p_start

        # Parse designator parts
        parts = ref.split("/")
        # Only handle standard "XX/YY" or "XXL/YYR" designators
        if len(parts) != 2:
            continue

        # Skip parallel runways (L/R/C) — we only want the main runway
        # unless there's no main runway
        has_lr = any(c in ref for c in "LRC")

        entry = {
            "designator": ref,
            "heading_deg": heading,
            "length_m": length,
            "width_m": width,
            "surface": surface,
            f"threshold_{parts[0]}": {"e": p_start[0], "n": p_start[1]},
            f"threshold_{parts[1]}": {"e": p_end[0], "n": p_end[1]},
            "_has_lr": has_lr,
            "_computed_dist": round(max_d),
        }
        rwy_entries.append(entry)

    # If no named runways found, try using "?" runways that are substantial
    if not rwy_entries:
        unnamed = by_ref.get("?", [])
        substantial = [s for s in unnamed if s["computed_length"] > 300]
        if substantial:
            longest = max(substantial, key=lambda r: r["computed_length"])
            seg = longest
            surface = seg["surface"] if seg["surface"] not in ("unknown", "") else "asphalt"
            width = seg["width"] if seg["width"] > 0 else 20
            length = round(seg["computed_length"])

            e1, n1 = seg["e1"], seg["n1"]
            e2, n2 = seg["e2"], seg["n2"]
            de = e2 - e1
            dn = n2 - n1
            heading = round(math.degrees(math.atan2(de, dn)) % 360)
            if heading >= 180:
                heading -= 180
                e1, n1, e2, n2 = e2, n2, e1, n1

            rwy_num = round(heading / 10) % 36
            if rwy_num == 0:
                rwy_num = 36
            recip = (rwy_num + 18) % 36
            if recip == 0:
                recip = 36
            desig = f"{rwy_num:02d}/{recip:02d}"
            parts = desig.split("/")

            entry = {
                "designator": desig,
                "heading_deg": heading,
                "length_m": length,
                "width_m": width,
                "surface": surface,
                f"threshold_{parts[0]}": {"e": e1, "n": n1},
                f"threshold_{parts[1]}": {"e": e2, "n": n2},
                "_has_lr": False,
                "_computed_dist": length,
            }
            rwy_entries.append(entry)

    if not rwy_entries:
        continue

    # For airports with parallel runways, prefer the main (non-L/R/C) version
    # or the longest one
    main_entries = [e for e in rwy_entries if not e["_has_lr"]]
    if not main_entries:
        main_entries = rwy_entries

    # Remove internal keys
    final = []
    for e in main_entries:
        e2 = {k: v for k, v in e.items() if not k.startswith("_")}
        final.append(e2)

    results[icao] = {"name": name, "type": atype, "runways": final}

# Print as Python dict
print("MANUAL_RUNWAYS = {")
for icao in sorted(results.keys()):
    info = results[icao]
    runways = info["runways"]
    print(f'    # {info["name"]} ({info["type"]})')
    print(f'    "{icao}": [')
    for rwy in runways:
        desig = rwy["designator"]
        parts = desig.split("/")
        t1_key = f"threshold_{parts[0]}"
        t2_key = f"threshold_{parts[1]}"
        t1 = rwy[t1_key]
        t2 = rwy[t2_key]
        print(f'        {{')
        print(f'            "designator": "{desig}", "heading_deg": {rwy["heading_deg"]}, '
              f'"length_m": {rwy["length_m"]},')
        print(f'            "width_m": {rwy["width_m"]}, "surface": "{rwy["surface"]}",')
        print(f'            "{t1_key}": {{"e": {t1["e"]}, "n": {t1["n"]}}},')
        print(f'            "{t2_key}": {{"e": {t2["e"]}, "n": {t2["n"]}}},')
        print(f'        }},')
    print(f'    ],')
print("}")
