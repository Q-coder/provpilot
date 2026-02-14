#!/usr/bin/env python3
"""Compute accurate LV95 coords for LSZH from OSM Overpass data."""

from pyproj import Transformer
import math

t = Transformer.from_crs('EPSG:4326', 'EPSG:2056', always_xy=True)

def to_lv95(lon, lat):
    e, n = t.transform(lon, lat)
    return round(e), round(n)

def stats(e1, n1, e2, n2):
    de, dn = e2-e1, n2-n1
    length = math.sqrt(de**2 + dn**2)
    heading = math.degrees(math.atan2(de, dn)) % 360
    return round(length), round(heading)

# OSM Overpass data (accessed 2026-02-14)
# Including displaced threshold extensions for full paved surface

# RWY 14/32: way 4386417 (tagged 3300m) + displaced threshold way 1255711169
# Full paved: 47.4830753,8.5347226 to 47.4612909,8.5644609
thr14 = to_lv95(8.5347226, 47.4830753)
thr32 = to_lv95(8.5644609, 47.4612909)
l, h = stats(thr14[0], thr14[1], thr32[0], thr32[1])
print("14/32 full: E=%d,%d -> E=%d,%d  L=%dm hdg=%d" % (
    thr14[0], thr14[1], thr32[0], thr32[1], l, h))

# RWY 10/28: way 4386418 (tagged 2500m), no displaced thresholds
thr10 = to_lv95(8.5374791, 47.4589387)
thr28 = to_lv95(8.5704525, 47.4565986)
l, h = stats(thr10[0], thr10[1], thr28[0], thr28[1])
print("10/28:      E=%d,%d -> E=%d,%d  L=%dm hdg=%d" % (
    thr10[0], thr10[1], thr28[0], thr28[1], l, h))

# RWY 16/34: way 4386419 (tagged 3700m) + displaced threshold way 1104890051
# Full paved: 47.4757133,8.5359369 to 47.4455617,8.5566994
thr16 = to_lv95(8.5359369, 47.4757133)
thr34 = to_lv95(8.5566994, 47.4455617)
l, h = stats(thr16[0], thr16[1], thr34[0], thr34[1])
print("16/34 full: E=%d,%d -> E=%d,%d  L=%dm hdg=%d" % (
    thr16[0], thr16[1], thr34[0], thr34[1], l, h))
