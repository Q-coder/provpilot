#!/usr/bin/env python3
"""Explore swissTLMRegio road classifications."""

import fiona
from collections import Counter

GPKG = "/Users/gerhardgubler/Desktop/usb-ssd/Mac/terrain_cache/swisstlmregio_2025/swissTLMRegio_Product_LV95.gpkg"

print("=== Road 'objval' distribution ===")
with fiona.open(GPKG, layer="tlmregio_transportation_road") as src:
    objval_dist = Counter()
    rst_dist = Counter()
    ltn_dist = Counter()
    construct_dist = Counter()
    for rec in src:
        p = rec["properties"]
        objval_dist[p.get("objval", "?")] += 1
        rst_dist[p.get("rst", "?")] += 1
        ltn_dist[p.get("ltn", "?")] += 1
        construct_dist[p.get("construct", "?")] += 1
    
    print("  objval (road type):")
    for v, c in objval_dist.most_common():
        print(f"    {v}: {c}")
    print()
    print("  rst (road surface):")
    for v, c in rst_dist.most_common():
        print(f"    {v}: {c}")
    print()
    print("  ltn (lanes):")
    for v, c in ltn_dist.most_common():
        print(f"    {v}: {c}")
    print()
    print("  construct (bridge/tunnel):")
    for v, c in construct_dist.most_common():
        print(f"    {v}: {c}")

print()
print("=== Railway 'objval' distribution ===")
with fiona.open(GPKG, layer="tlmregio_transportation_railway") as src:
    objval_dist = Counter()
    construct_dist = Counter()
    fco_dist = Counter()
    for rec in src:
        p = rec["properties"]
        objval_dist[p.get("objval", "?")] += 1
        construct_dist[p.get("construct", "?")] += 1
        fco_dist[p.get("fco", "?")] += 1
    
    print("  objval (railway type):")
    for v, c in objval_dist.most_common():
        print(f"    {v}: {c}")
    print()
    print("  construct:")
    for v, c in construct_dist.most_common():
        print(f"    {v}: {c}")
    print()
    print("  fco (track count):")
    for v, c in fco_dist.most_common():
        print(f"    {v}: {c}")

# Sample a road near Schaffhausen area (E~2688, N~1283)
print()
print("=== Roads near Schaffhausen (2680k-2700k E, 1275k-1295k N) ===")
with fiona.open(GPKG, layer="tlmregio_transportation_road") as src:
    count = 0
    for rec in src:
        geom = rec["geometry"]
        coords = geom["coordinates"]
        if geom["type"] == "LineString":
            x0, y0 = coords[0][0], coords[0][1]
        else:
            x0, y0 = coords[0][0][0], coords[0][0][1]
        if 2680000 <= x0 <= 2700000 and 1275000 <= y0 <= 1295000:
            count += 1
            if count <= 5:
                p = rec["properties"]
                print(f"  objval={p['objval']}, rst={p['rst']}, ltn={p['ltn']}, construct={p['construct']}")
                print(f"    first_pt=({x0:.0f}, {y0:.0f}), pts={len(coords)}")
    print(f"  Total roads in area: {count}")
