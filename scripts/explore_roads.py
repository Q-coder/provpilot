#!/usr/bin/env python3
"""Explore swissTLMRegio road and railway layers."""

import fiona

GPKG = "/Users/gerhardgubler/Desktop/usb-ssd/Mac/terrain_cache/swisstlmregio_2025/swissTLMRegio_Product_LV95.gpkg"

for layer_name in ["tlmregio_transportation_road", "tlmregio_transportation_railway"]:
    print(f"=== {layer_name} ===")
    with fiona.open(GPKG, layer=layer_name) as src:
        print(f"  CRS: {src.crs}")
        print(f"  Geometry: {src.schema['geometry']}")
        print(f"  Record count: {len(src)}")
        print(f"  Fields:")
        for name, ftype in src.schema["properties"].items():
            print(f"    {name}: {ftype}")
        print(f"  --- Sample records ---")
        for i, rec in enumerate(src):
            if i >= 3:
                break
            props = dict(rec["properties"])
            geom = rec["geometry"]
            if geom["type"] == "LineString":
                n_pts = len(geom["coordinates"])
            elif geom["type"] == "MultiLineString":
                n_pts = sum(len(p) for p in geom["coordinates"])
            else:
                n_pts = "?"
            print(f"  [{i}] type={geom['type']}, pts={n_pts}, props={props}")
    print()

# Get unique road objectval values  
print("=== Road objectval value distribution ===")
with fiona.open(GPKG, layer="tlmregio_transportation_road") as src:
    vals = {}
    for rec in src:
        v = rec["properties"].get("objectval", "UNKNOWN")
        vals[v] = vals.get(v, 0) + 1
    for v, c in sorted(vals.items(), key=lambda x: -x[1]):
        print(f"  {v}: {c}")

print()
print("=== Railway objectval value distribution ===")
with fiona.open(GPKG, layer="tlmregio_transportation_railway") as src:
    vals = {}
    for rec in src:
        v = rec["properties"].get("objectval", "UNKNOWN")
        vals[v] = vals.get(v, 0) + 1
    for v, c in sorted(vals.items(), key=lambda x: -x[1]):
        print(f"  {v}: {c}")
