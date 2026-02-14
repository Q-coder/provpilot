#!/usr/bin/env python3
"""Fetch all Swiss runway ways from OSM Overpass API and compute LV95 thresholds."""

import json
import math
import urllib.request

try:
    from pyproj import Transformer
    transformer = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
except ImportError:
    print("pyproj not available, using approximate conversion")
    transformer = None


def wgs84_to_lv95(lat, lon):
    """Convert WGS84 to LV95 (EPSG:2056)."""
    if transformer:
        e, n = transformer.transform(lon, lat)
        return round(e), round(n)
    # Rough fallback
    e = 2600000 + (lon - 7.4388) * 76000
    n = 1200000 + (lat - 46.9511) * 111000
    return round(e), round(n)


def haversine_m(lat1, lon1, lat2, lon2):
    """Approx distance in meters between two WGS84 points."""
    R = 6371000
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = math.sin(dlat/2)**2 + math.cos(math.radians(lat1)) * math.cos(math.radians(lat2)) * math.sin(dlon/2)**2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def main():
    query = """
[out:json][timeout:30];
area["ISO3166-1"="CH"]->.ch;
(
  way["aeroway"="runway"](area.ch);
);
out body;
>;
out skel qt;
"""
    url = "https://overpass-api.de/api/interpreter"
    data = ("data=" + query).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    print("Fetching Swiss runways from OSM Overpass...")
    with urllib.request.urlopen(req, timeout=40) as resp:
        result = json.loads(resp.read().decode("utf-8"))

    nodes = {}
    ways = []
    for el in result["elements"]:
        if el["type"] == "node":
            nodes[el["id"]] = (el["lat"], el["lon"])
        elif el["type"] == "way":
            ways.append(el)

    print(f"Found {len(ways)} runway ways, {len(nodes)} nodes\n")

    # Process each way: compute endpoints and actual length
    runways = []
    for w in ways:
        tags = w.get("tags", {})
        ref = tags.get("ref", "?")
        surface = tags.get("surface", "unknown")
        width = tags.get("width", "")
        osm_length = tags.get("length", "")
        
        # Get first and last node coordinates
        node_ids = w.get("nodes", [])
        if len(node_ids) < 2:
            continue
        
        lat1, lon1 = nodes.get(node_ids[0], (0, 0))
        lat2, lon2 = nodes.get(node_ids[-1], (0, 0))
        
        # Compute actual distance between endpoints
        dist = haversine_m(lat1, lon1, lat2, lon2)
        
        # Convert to LV95
        e1, n1 = wgs84_to_lv95(lat1, lon1)
        e2, n2 = wgs84_to_lv95(lat2, lon2)
        
        # Compute heading (from endpoint 1 to endpoint 2)
        de = e2 - e1
        dn = n2 - n1
        heading = math.degrees(math.atan2(de, dn)) % 360
        if heading >= 180:
            heading -= 180
        
        # Center
        ce = (e1 + e2) // 2
        cn = (n1 + n2) // 2
        
        runways.append({
            "osm_way": w["id"],
            "ref": ref,
            "surface": surface,
            "width": int(float(width)) if width else 0,
            "osm_length": osm_length,
            "computed_length": round(dist),
            "heading": round(heading),
            "e1": e1, "n1": n1,
            "e2": e2, "n2": n2,
            "center_e": ce, "center_n": cn,
        })

    # Sort by location (east to west roughly)
    runways.sort(key=lambda r: r["center_e"])

    # Print results
    print(f"{'Ref':8s}  {'OSM len':>7s}  {'Actual':>6s}  {'Hdg':>3s}  {'Surface':12s}  {'W':>3s}  {'E1':>9s} {'N1':>9s}  {'E2':>9s} {'N2':>9s}  WAY")
    print("-" * 120)
    for r in runways:
        print(f"{r['ref']:8s}  {r['osm_length']:>7s}  {r['computed_length']:5d}m  {r['heading']:3d}  {r['surface']:12s}  {r['width']:3d}  {r['e1']:9d} {r['n1']:9d}  {r['e2']:9d} {r['n2']:9d}  {r['osm_way']}")

    # Save JSON for reference
    with open("/tmp/osm_swiss_runways.json", "w") as f:
        json.dump(runways, f, indent=2)
    print(f"\nSaved to /tmp/osm_swiss_runways.json")


if __name__ == "__main__":
    main()
