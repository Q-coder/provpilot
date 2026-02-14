#!/usr/bin/env python3
"""Cross-reference OSM runway data with our airports.json to find corrections needed."""

import json
import math

with open("scenery/poi/airports.json") as f:
    airports = json.load(f)["airports"]

with open("/tmp/osm_swiss_runways.json") as f:
    osm_runways = json.load(f)

def dist(e1, n1, e2, n2):
    return math.sqrt((e2-e1)**2 + (n2-n1)**2)

# For each airport, find OSM runways within 3km of center
print(f"{'ICAO':4s}  {'Name':35s}  {'Type':22s}  {'Our Rwy':8s}  {'Our Len':>7s}  {'OSM Match':8s}  {'OSM Len':>7s}  {'OSM Surf':10s}  {'Delta':>6s}")
print("-" * 140)

for ap in airports:
    if ap.get("type") == "heliport":
        continue
    ce = ap.get("center_e", 0)
    cn = ap.get("center_n", 0)
    icao = ap.get("icao", "")
    name = ap.get("name", "")
    atype = ap.get("type", "")
    
    # Find OSM runways near this airport
    nearby = []
    for r in osm_runways:
        d = dist(ce, cn, r["center_e"], r["center_n"])
        if d < 3000 and r["computed_length"] > 200:  # skip tiny fragments
            # Skip displaced threshold segments (no ref, very short)
            nearby.append((d, r))
    
    nearby.sort(key=lambda x: x[0])
    
    # Our current runway data
    our_rwys = ap.get("runways", [])
    
    # Group OSM runways by ref to combine split ways
    osm_by_ref = {}
    for d, r in nearby:
        ref = r["ref"]
        if ref not in osm_by_ref:
            osm_by_ref[ref] = {"runways": [], "total_length": 0}
        osm_by_ref[ref]["runways"].append(r)
        osm_by_ref[ref]["total_length"] += r["computed_length"]
    
    for our_rwy in our_rwys:
        desig = our_rwy["designator"]
        our_len = our_rwy["length_m"]
        
        # Try to find matching OSM runway
        best_ref = None
        best_len = 0
        best_surf = ""
        for ref, data in osm_by_ref.items():
            if ref != "?":
                best_ref = ref
                best_len = data["total_length"]
                rwys = data["runways"]
                # Get surface from the longest segment
                longest = max(rwys, key=lambda r: r["computed_length"])
                best_surf = longest["surface"]
                break
        
        if best_ref is None and nearby:
            # Use the longest unref'd runway
            longest = max(nearby, key=lambda x: x[1]["computed_length"])
            best_ref = longest[1]["ref"]
            best_len = longest[1]["computed_length"]
            best_surf = longest[1]["surface"]
        
        delta = our_len - best_len if best_len > 0 else 0
        flag = " ***" if abs(delta) > 200 else ""
        
        print(f"{icao:4s}  {name:35s}  {atype:22s}  {desig:8s}  {our_len:6d}m  {best_ref or '?':8s}  {best_len:6d}m  {best_surf:10s}  {delta:+5d}m{flag}")


# Now print OSM data formatted as MANUAL_RUNWAYS entries
print("\n\n=== Suggested MANUAL_RUNWAYS entries ===\n")

# For each airport, reconstruct proper runway data from OSM
for ap in airports:
    if ap.get("type") == "heliport":
        continue
    ce = ap.get("center_e", 0)
    cn = ap.get("center_n", 0)
    icao = ap.get("icao", "")
    if not icao:
        continue
    
    nearby = []
    for r in osm_runways:
        d = dist(ce, cn, r["center_e"], r["center_n"])
        if d < 3000 and r["computed_length"] > 200:
            nearby.append((d, r))
    
    if not nearby:
        continue
    
    # Group by ref, keeping only named runways
    osm_by_ref = {}
    for d, r in nearby:
        ref = r["ref"]
        if ref == "?":
            continue
        if ref not in osm_by_ref:
            osm_by_ref[ref] = []
        osm_by_ref[ref].append(r)
    
    if not osm_by_ref:
        continue
    
    # For each OSM runway ref, find the extremes (first node → last node)
    for ref, segments in osm_by_ref.items():
        # Sort segments and find overall endpoints
        all_e = []
        all_n = []
        for seg in segments:
            all_e.extend([seg["e1"], seg["e2"]])
            all_n.extend([seg["n1"], seg["n2"]])
        
        # Find the two most distant points
        max_dist = 0
        p_start = (0, 0)
        p_end = (0, 0)
        points = list(zip(all_e, all_n))
        for i in range(len(points)):
            for j in range(i+1, len(points)):
                d = dist(points[i][0], points[i][1], points[j][0], points[j][1])
                if d > max_dist:
                    max_dist = d
                    p_start = points[i]
                    p_end = points[j]
        
        # Get properties from longest segment
        longest = max(segments, key=lambda r: r["computed_length"])
        surface = longest["surface"]
        width = longest["width"] or 30
        osm_length = longest.get("osm_length", "")
        
        # Use declared length if available, else computed
        if osm_length:
            try:
                length = int(float(osm_length))
            except:
                length = round(max_dist)
        else:
            length = round(max_dist)
        
        # Heading
        de = p_end[0] - p_start[0]
        dn = p_end[1] - p_start[1]
        heading = round(math.degrees(math.atan2(de, dn)) % 360)
        if heading >= 180:
            heading -= 180
        
        # Designator parts
        parts = ref.split("/")
        if len(parts) == 2:
            t1_key = f"threshold_{parts[0]}"
            t2_key = f"threshold_{parts[1]}"
        else:
            t1_key = f"threshold_{ref}"
            t2_key = "threshold_??"
        
        # Ensure p_start is the lower-numbered threshold
        # (heading should point from lower to higher designator)
        print(f'    # {icao} {ref}: OSM declared={osm_length}m, computed={round(max_dist)}m, segments={len(segments)}')
        print(f'    "{icao}": [{{')
        print(f'        "designator": "{ref}", "heading_deg": {heading}, "length_m": {length},')
        print(f'        "width_m": {width}, "surface": "{surface}",')
        print(f'        "{t1_key}": {{"e": {p_start[0]}, "n": {p_start[1]}}},')
        print(f'        "{t2_key}": {{"e": {p_end[0]}, "n": {p_end[1]}}},')
        print(f'    }}],')
