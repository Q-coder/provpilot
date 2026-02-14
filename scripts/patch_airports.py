#!/usr/bin/env python3
"""
Patch airports.json with corrected runway data from MANUAL_RUNWAYS.

This script reads the existing airports.json and updates runway entries
for airports that have MANUAL_RUNWAYS definitions in generate_airports.py.
Use this when the GeoPackage is not available for full regeneration.
"""

import json
import math
import sys
import importlib.util

# Load MANUAL_RUNWAYS from generate_airports.py
spec = importlib.util.spec_from_file_location("gen", "scripts/generate_airports.py")
gen = importlib.util.module_from_spec(spec)

# We can't fully import it (missing fiona/shapely), so just exec the dict
manual_runways = {}
with open("scripts/generate_airports.py") as f:
    src = f.read()
    # Extract MANUAL_RUNWAYS dict
    start = src.index("MANUAL_RUNWAYS = {")
    depth = 0
    end = start
    for i, ch in enumerate(src[start:]):
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                end = start + i + 1
                break
    exec(src[start:end])
    manual_runways = MANUAL_RUNWAYS  # noqa: F821


# Load existing airports.json
with open("scenery/poi/airports.json") as f:
    data = json.load(f)

airports = data["airports"]
updated = 0
unchanged = 0

for ap in airports:
    icao = ap.get("icao", "")
    if icao and icao in manual_runways:
        old_runways = ap.get("runways", [])
        new_runways = manual_runways[icao]
        
        # Check if data actually changed
        old_str = json.dumps(old_runways, sort_keys=True)
        new_str = json.dumps(new_runways, sort_keys=True)
        
        if old_str != new_str:
            if ap.get("type") != "heliport":
                ap["runways"] = new_runways
                updated += 1
                
                # Show what changed
                for old_r in old_runways:
                    old_d = old_r.get("designator", "?")
                    old_l = old_r.get("length_m", 0)
                    found = False
                    for new_r in new_runways:
                        if new_r["designator"] == old_d:
                            new_l = new_r["length_m"]
                            if old_l != new_l:
                                print(f"  {icao} {old_d}: {old_l}m → {new_l}m")
                            found = True
                    if not found:
                        print(f"  {icao} {old_d}: REMOVED (was {old_l}m)")
                for new_r in new_runways:
                    new_d = new_r["designator"]
                    if not any(r["designator"] == new_d for r in old_runways):
                        print(f"  {icao} {new_d}: ADDED ({new_r['length_m']}m)")
        else:
            unchanged += 1

# Also apply length caps to auto-estimated airports without MANUAL_RUNWAYS
capped = 0
for ap in airports:
    icao = ap.get("icao", "")
    atype = ap.get("type", "")
    
    # Skip manual or heliport
    if (icao and icao in manual_runways) or atype == "heliport":
        continue
    
    for rwy in ap.get("runways", []):
        old_len = rwy["length_m"]
        
        # Simple maximum length cap per airport type (idempotent)
        if atype == "grass_strip":
            max_len = 900
        elif atype == "regional_airport":
            max_len = 2000
        else:
            max_len = 3000
        
        new_len = min(old_len, max_len)
        new_len = max(new_len, 300)
        
        if new_len != old_len:
            rwy["length_m"] = new_len
            
            # Recompute thresholds from center + heading
            heading_rad = math.radians(rwy["heading_deg"])
            center_e = ap["center_e"]
            center_n = ap["center_n"]
            de = math.sin(heading_rad) * new_len / 2
            dn = math.cos(heading_rad) * new_len / 2
            
            parts = rwy["designator"].split("/")
            if len(parts) == 2:
                rwy[f"threshold_{parts[0]}"] = {"e": round(center_e - de), "n": round(center_n - dn)}
                rwy[f"threshold_{parts[1]}"] = {"e": round(center_e + de), "n": round(center_n + dn)}
            
            capped += 1
            print(f"  {icao or '----'} {ap['name']:30s} {rwy['designator']}: {old_len}m → {new_len}m (capped)")


# Write back
with open("scenery/poi/airports.json", "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, ensure_ascii=False)

print(f"\nDone: {updated} airports updated from MANUAL_RUNWAYS, {capped} auto-estimated capped, {unchanged} unchanged")
print(f"Total airports: {len(airports)}")
