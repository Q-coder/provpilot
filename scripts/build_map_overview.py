#!/usr/bin/env python3
"""Overview cells for the VFR map's wide views (80 - 250 km).

The map draws those views from 32.768 km cells at 256 m per pixel. Built at
run time, one cell means reading up to 1024 terrain tiles (a 250 km view:
about 170 000 files, over a minute from a cold disk). This script writes the
same samples once per region:

    <Region>/map_overview/L2/c_<ei>_<ni>.i16

128 x 128 little-endian int16 metres, x eastward, y southward (row 0 = the
cell's north edge), 4 x 4 samples per terrain tile (every 8th of its 34):
exactly what VfrMap::render_terrain_image() picks from the tile files.
-32768 = no tile, -32767 = sea level (the bake stores 0.01 m there).

Run it after a terrain bake, before bundling the region:

    python3 scripts/build_map_overview.py /Volumes/Data1/provpilot/build/germany/Germany [...]
"""
import argparse
import os
import re
import sys
import time
from multiprocessing import Pool

import numpy as np

TILE_M = 1024
TILE_PX = 34
CELL_M = 32768
K = CELL_M // TILE_M          # 32 tiles per cell side
P = 4                         # samples per tile side
STRIDE = 8
S = K * P                     # 128
NO_DATA = -32768
SEA = -32767
NAME_RE = re.compile(r"^tile_(-?\d+)_(-?\d+)\.raw$")
PICK = np.minimum(TILE_PX - 1, np.arange(P) * STRIDE)


def build_cell(job):
    terrain_dir, out_dir, ei, ni, tiles = job
    cell = np.full((S, S), NO_DATA, dtype="<i2")
    used = 0
    for e, n in tiles:
        path = os.path.join(terrain_dir, "tile_%d_%d.raw" % (e, n))
        try:
            if os.path.getsize(path) != TILE_PX * TILE_PX * 4:
                continue  # a 1 m add-on tile: not base-tier data
            h = np.fromfile(path, dtype="<f4").reshape(TILE_PX, TILE_PX)
        except (OSError, ValueError):
            continue
        tx = (e - ei * CELL_M) // TILE_M
        ty = (n - ni * CELL_M) // TILE_M
        # raw rows run south -> north, columns east -> west
        block = h[np.ix_(PICK, TILE_PX - 1 - PICK)]       # [sy from south, sx from west]
        block = block[::-1, :]                             # row 0 = north
        q = np.clip(np.rint(block), -32000, 32000).astype("<i2")
        q[block < 0.05] = SEA
        y0 = (K - 1 - ty) * P
        x0 = tx * P
        cell[y0:y0 + P, x0:x0 + P] = q
        used += 1
    if used == 0:
        return 0
    out = os.path.join(out_dir, "c_%d_%d.i16" % (ei, ni))
    tmp = out + ".tmp"
    cell.tofile(tmp)
    os.replace(tmp, out)
    return used


def build_region(region_dir, workers):
    terrain_dir = os.path.join(region_dir, "terrain")
    if not os.path.isdir(terrain_dir):
        print("skip %s: no terrain/" % region_dir)
        return False
    t0 = time.time()
    cells = {}
    with os.scandir(terrain_dir) as it:
        for entry in it:
            m = NAME_RE.match(entry.name)
            if not m:
                continue
            e, n = int(m.group(1)), int(m.group(2))
            cells.setdefault((e // CELL_M, n // CELL_M), []).append((e, n))
    out_dir = os.path.join(region_dir, "map_overview", "L2")
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):      # a re-bake may have dropped cells
        if name.endswith(".i16") or name.endswith(".tmp"):
            os.remove(os.path.join(out_dir, name))
    jobs = [(terrain_dir, out_dir, ei, ni, tiles) for (ei, ni), tiles in sorted(cells.items())]
    used = 0
    with Pool(workers) as pool:
        for i, u in enumerate(pool.imap_unordered(build_cell, jobs, chunksize=4)):
            used += u
            if (i + 1) % 200 == 0:
                print("  %s: %d / %d cells" % (os.path.basename(region_dir), i + 1, len(jobs)), flush=True)
    print("%s: %d cells from %d tiles in %.0f s -> %s" % (
        os.path.basename(region_dir), len(jobs), used, time.time() - t0, out_dir), flush=True)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("regions", nargs="+", help="region folders (each holds terrain/)")
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()
    ok = True
    for r in args.regions:
        ok = build_region(os.path.abspath(r), args.workers) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
