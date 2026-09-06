#!/usr/bin/env python3
"""
Repair corrupt baked vertex heights in Scenery3D water GLB tiles.

`scripts/generate_water.py` bakes every lake/river vertex at its absolute LV95
elevation, but a sizeable share of the swissTLM3D geometry comes out wrong.
Shoreline tiles are fine; the large open-water tiles are not. The middle of the
Bodensee, for example, is a single 4-vertex quad covering a whole 4096 m tile
whose corners are baked at 171–330 m instead of the lake surface at 395.6 m.

That used to be invisible. `s3d_water_manager.cpp` distrusts a tile whose worst
per-triangle Y span exceeds 50 m and redrapes it from the terrain DTM, and
swissALTI3D models a lake as a flat plate at its water level — so the redrape
silently reproduced a correct surface. Once swissBATHY3D carves the real lake
bed into the terrain, the same redrape drops the water onto the bed: 42 % of the
Bodensee tiles sank by up to 250 m, leaving a mosaic of holes in the lake.

This tool does the redrape once, offline, against the **un-carved** terrain, so
the bake is correct on disk and the runtime never has to second-guess it. A
vertex is only touched when it disagrees with the DTM by more than
`--max-deviation-m`; healthy geometry (water sits about 1 m above bare ground)
is left exactly as it is, including genuinely sloping rivers.

    python3 scripts/fix_water_bake.py ~/provpilot/scenery/Switzerland/water \
        --terrain ~/provpilot/scenery/Switzerland/terrain \
                  ~/provpilot/scenery/Germany/Baden-Wuertemberg/terrain

    ... --apply --backup      # actually rewrite the GLBs

⚠️  Point `--terrain` at the plain swissALTI3D/DGM directories, NEVER at the
    swissBATHY3D overlay — the whole point is to drape water on the water
    surface, not on the lake bed.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np

GLB_MAGIC = b"glTF"
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

COMPONENT_DTYPE = {5120: "i1", 5121: "u1", 5122: "i2", 5123: "u2",
                   5125: "u4", 5126: "f4"}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# --------------------------------------------------------------------------
# GLB IO
# --------------------------------------------------------------------------

def read_glb(path: Path) -> tuple[dict, bytearray]:
    data = path.read_bytes()
    if data[:4] != GLB_MAGIC:
        raise ValueError(f"{path.name}: not a GLB")
    off = 12
    js: dict | None = None
    bin_chunk = bytearray()
    while off < len(data):
        length, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + length]
        off += length
        if ctype == CHUNK_JSON:
            js = json.loads(chunk)
        elif ctype == CHUNK_BIN:
            bin_chunk = bytearray(chunk)
    if js is None:
        raise ValueError(f"{path.name}: no JSON chunk")
    return js, bin_chunk


def write_glb(path: Path, js: dict, bin_chunk: bytes) -> None:
    js_bytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    js_bytes += b" " * (-len(js_bytes) % 4)
    bin_bytes = bytes(bin_chunk) + b"\x00" * (-len(bin_chunk) % 4)
    total = 12 + 8 + len(js_bytes) + (8 + len(bin_bytes) if bin_bytes else 0)
    out = bytearray()
    out += GLB_MAGIC + struct.pack("<II", 2, total)
    out += struct.pack("<II", len(js_bytes), CHUNK_JSON) + js_bytes
    if bin_bytes:
        out += struct.pack("<II", len(bin_bytes), CHUNK_BIN) + bin_bytes
    path.write_bytes(out)


def accessor_slice(js: dict, accessor_index: int) -> tuple[int, np.dtype, int, int]:
    a = js["accessors"][accessor_index]
    bv = js["bufferViews"][a["bufferView"]]
    dtype = np.dtype("<" + COMPONENT_DTYPE[a["componentType"]])
    ncomp = TYPE_COUNT[a["type"]]
    offset = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    return offset, dtype, ncomp, a["count"]


def read_accessor(js: dict, blob: bytearray, accessor_index: int) -> np.ndarray:
    offset, dtype, ncomp, count = accessor_slice(js, accessor_index)
    arr = np.frombuffer(blob, dtype=dtype, count=count * ncomp, offset=offset)
    return arr.reshape(count, ncomp) if ncomp > 1 else arr


# --------------------------------------------------------------------------
# Terrain sampling (matches S3DElevationDB)
# --------------------------------------------------------------------------

class TerrainSampler:
    """Bilinear LV95 elevation lookup over Scenery3D raw heightmap tiles.

    Tile layout: `tile_<E>_<N>.raw`, `tile_px²` float32 LE, pixel (0,0) = SE
    corner, columns run East→West and rows South→North. Several directories are
    merged the way the engine does it: later ones win where non-zero.
    """

    def __init__(self, dirs: list[Path], tile_size_m: int = 1024, tile_px: int = 512):
        self.dirs = dirs
        self.ts = tile_size_m
        self.px = tile_px
        self._cache: dict[tuple[int, int], np.ndarray | None] = {}

    def _tile(self, ei: int, ni: int):
        key = (ei, ni)
        if key in self._cache:
            return self._cache[key]
        merged = None
        name = f"tile_{ei * self.ts}_{ni * self.ts}.raw"
        for d in self.dirs:
            p = d / name
            if not p.is_file():
                continue
            arr = np.fromfile(p, dtype="<f4")
            if arr.size != self.px * self.px:
                continue
            arr = arr.reshape(self.px, self.px)
            if merged is None:
                merged = arr.copy()
            else:
                m = arr != 0.0
                merged[m] = arr[m]
        if len(self._cache) > 256:
            self._cache.clear()
        self._cache[key] = merged
        return merged

    def sample(self, east: np.ndarray, north: np.ndarray) -> np.ndarray:
        out = np.full(east.shape, np.nan, dtype=np.float64)
        ei = np.floor(east / self.ts).astype(np.int64)
        ni = np.floor(north / self.ts).astype(np.int64)
        for key in {(int(a), int(b)) for a, b in zip(ei, ni)}:
            tile = self._tile(*key)
            if tile is None:
                continue
            sel = (ei == key[0]) & (ni == key[1])
            lx = east[sel] - key[0] * self.ts
            lz = north[sel] - key[1] * self.ts
            col = (1.0 - lx / self.ts) * (self.px - 1)
            row = (lz / self.ts) * (self.px - 1)
            c0 = np.clip(np.floor(col).astype(int), 0, self.px - 1)
            r0 = np.clip(np.floor(row).astype(int), 0, self.px - 1)
            c1 = np.clip(c0 + 1, 0, self.px - 1)
            r1 = np.clip(r0 + 1, 0, self.px - 1)
            fc = col - c0
            fr = row - r0
            v = (tile[r0, c0] * (1 - fc) * (1 - fr) + tile[r0, c1] * fc * (1 - fr)
                 + tile[r1, c0] * (1 - fc) * fr + tile[r1, c1] * fc * fr)
            v = np.where((tile[r0, c0] == 0) | (tile[r1, c1] == 0), np.nan, v)
            out[sel] = v
        return out


# --------------------------------------------------------------------------

def worst_triangle_span(y: np.ndarray, tris: np.ndarray) -> float:
    if len(tris) == 0:
        return 0.0
    t = y[tris]
    return float((t.max(axis=1) - t.min(axis=1)).max())


_SAMPLER = None


def _init_worker(dirs, tile_size_m, tile_px):
    global _SAMPLER
    _SAMPLER = TerrainSampler([Path(d) for d in dirs], tile_size_m, tile_px)


def connected_components(n_verts: int, tris: np.ndarray) -> np.ndarray:
    """Label each vertex with the id of the mesh island it belongs to.

    Repairs have to be decided per water body, not per vertex: half-fixing a
    lake polygon leaves a cliff between the corrected and the untouched half,
    which is worse than the original defect.
    """
    parent = np.arange(n_verts, dtype=np.int64)

    def find(x: int) -> int:
        root = x
        while parent[root] != root:
            root = parent[root]
        while parent[x] != root:
            parent[x], x = root, parent[x]
        return root

    for a, b, c in tris:
        ra, rb, rc = find(int(a)), find(int(b)), find(int(c))
        if rb != ra:
            parent[rb] = ra
        if rc != ra:
            parent[rc] = ra
    return np.array([find(i) for i in range(n_verts)], dtype=np.int64)


def repair_tile(path: Path, origin_e: float, origin_n: float,
                max_deviation_m: float, flat_tol_m: float,
                min_flat_fraction: float,
                apply: bool, backup_dir) -> tuple[str, float, float, int, int]:
    """Returns (name, worst_span_before, worst_span_after, fixed, total)."""
    js, blob = read_glb(path)
    before = after = 0.0
    fixed = total = 0
    dirty = False

    for mesh in js.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if prim.get("mode", 4) != 4 or "POSITION" not in prim["attributes"]:
                continue
            if "indices" not in prim:
                continue
            pos_idx = prim["attributes"]["POSITION"]
            offset, dtype, ncomp, count = accessor_slice(js, pos_idx)
            if dtype != np.dtype("<f4") or ncomp != 3:
                continue
            pos = read_accessor(js, blob, pos_idx).astype(np.float64)
            tris = read_accessor(js, blob, prim["indices"]).astype(np.int64).reshape(-1, 3)

            # GLB local frame → LV95 (see manifest conversion_origin_*).
            east = origin_e - pos[:, 0]
            north = origin_n + pos[:, 2]
            ground = _SAMPLER.sample(east, north)

            y = pos[:, 1].copy()
            total += len(y)
            before = max(before, worst_triangle_span(y, tris))

            known = np.isfinite(ground)
            if not known.any():
                after = max(after, worst_triangle_span(y, tris))
                continue

            labels = connected_components(len(y), tris)
            changed = False
            for label in np.unique(labels):
                sel = labels == label
                gsel = sel & known
                if gsel.sum() < 3:
                    continue
                g = ground[gsel]
                # Mode, not median: neighbouring tiles must agree on the water
                # level or the lake gets faint steps at every tile seam. The
                # DTM plateau is quantised, so the most common value is the
                # regulated lake level (Bodensee → 395.6 m everywhere).
                vals, counts = np.unique(np.round(g, 1), return_counts=True)
                level = float(vals[int(np.argmax(counts))])
                # swissALTI3D renders a lake as a perfectly level plate, so a
                # water body whose ground is flat has one unambiguous surface
                # height. Anything else (a stream down a flank) keeps its bake.
                if float(np.mean(np.abs(g - level) <= flat_tol_m)) < min_flat_fraction:
                    continue
                delta = y[gsel] - g
                ok = np.abs(delta) <= max_deviation_m
                lift = float(np.median(delta[ok])) if ok.any() else 1.0
                lift = float(np.clip(lift, 0.0, 2.0))
                target = level + lift
                if np.all(np.abs(y[sel] - target) <= max_deviation_m):
                    continue
                y[sel] = target
                fixed += int(sel.sum())
                changed = True

            after = max(after, worst_triangle_span(y, tris))

            if changed and apply:
                pos[:, 1] = y
                packed = pos.astype("<f4").tobytes()
                blob[offset:offset + len(packed)] = packed
                acc = js["accessors"][pos_idx]
                acc["min"] = [float(pos[:, 0].min()), float(y.min()), float(pos[:, 2].min())]
                acc["max"] = [float(pos[:, 0].max()), float(y.max()), float(pos[:, 2].max())]
            dirty = dirty or changed

    if dirty and apply:
        if backup_dir is not None:
            backup_dir.mkdir(parents=True, exist_ok=True)
            target = backup_dir / path.name
            if not target.exists():
                shutil.copy2(path, target)
        write_glb(path, js, blob)
    return path.name, before, after, fixed, total


def _worker(args):
    try:
        return repair_tile(*args)
    except Exception as e:  # noqa: BLE001 - one bad tile must not kill the run
        print(f"  WARN: {args[0].name}: {e}", file=sys.stderr)
        return (args[0].name, float("nan"), float("nan"), -1, 0)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("water_dir", type=Path,
                    help="Scenery water directory (manifest.json + water_*.glb).")
    ap.add_argument("--terrain", type=Path, nargs="+", required=True,
                    help="Un-carved terrain dir(s) to drape against. NOT the "
                         "swissBATHY3D overlay.")
    ap.add_argument("--max-deviation-m", type=float, default=5.0,
                    help="A water body further than this from the DTM is corrupt.")
    ap.add_argument("--flat-tol-m", type=float, default=1.0,
                    help="Ground varying less than this counts as level.")
    ap.add_argument("--min-flat-fraction", type=float, default=0.6,
                    help="Fraction of a water body that must sit on level "
                         "ground before it is treated as a lake surface.")
    ap.add_argument("--tile-size-m", type=int, default=1024)
    ap.add_argument("--tile-px", type=int, default=512)
    ap.add_argument("--report-threshold-m", type=float, default=50.0,
                    help="Runtime redrape threshold; tiles above it are reported.")
    ap.add_argument("--apply", action="store_true",
                    help="Rewrite the GLBs (default is a dry run).")
    ap.add_argument("--backup", action="store_true",
                    help="Copy each rewritten GLB to <water_dir>/.orig first.")
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    water_dir = args.water_dir.expanduser()
    terrain_dirs = [p.expanduser() for p in args.terrain]
    for d in terrain_dirs:
        if not d.is_dir():
            ap.error(f"--terrain dir does not exist: {d}")
        mpath = d / "manifest.json"
        if mpath.is_file() and json.loads(mpath.read_text()).get("overlay"):
            ap.error(f"{d} is a bathymetry OVERLAY; pass the plain terrain dir")

    manifest = json.loads((water_dir / "manifest.json").read_text())
    origin_e = float(manifest.get("conversion_origin_e", 2600000.0))
    origin_n = float(manifest.get("conversion_origin_n", 1200000.0))

    tiles = sorted(water_dir.glob("water_*.glb"))
    if not tiles:
        print(f"No water_*.glb under {water_dir}", file=sys.stderr)
        return 1
    backup_dir = (water_dir / ".orig") if (args.backup and args.apply) else None

    print(f"{len(tiles)} water tile(s) in {water_dir}"
          + ("" if args.apply else "   [DRY RUN — use --apply to write]"))
    print(f"draping against {', '.join(str(d) for d in terrain_dirs)}")

    jobs = [(t, origin_e, origin_n, args.max_deviation_m, args.flat_tol_m,
             args.min_flat_fraction, args.apply, backup_dir)
            for t in tiles]
    bad_before = bad_after = 0
    total_fixed = total_verts = 0
    worst_after: list[tuple[float, str]] = []
    done = 0
    with ProcessPoolExecutor(max_workers=args.workers, initializer=_init_worker,
                             initargs=([str(d) for d in terrain_dirs],
                                       args.tile_size_m, args.tile_px)) as pool:
        for fut in as_completed([pool.submit(_worker, j) for j in jobs]):
            name, before, after, fixed, total = fut.result()
            done += 1
            if fixed < 0:
                continue
            total_fixed += fixed
            total_verts += total
            if before > args.report_threshold_m:
                bad_before += 1
            if after > args.report_threshold_m:
                bad_after += 1
                worst_after.append((after, name))
            if done % 200 == 0 or done == len(jobs):
                print(f"\r  {done}/{len(jobs)}", end="", flush=True)
    print()

    print(f"tiles over the {args.report_threshold_m:g} m redrape threshold: "
          f"{bad_before} → {bad_after}")
    pct = 100.0 * total_fixed / total_verts if total_verts else 0.0
    print(f"vertices repaired: {total_fixed} of {total_verts} ({pct:.2f} %)"
          + ("" if args.apply else " — dry run, nothing written"))
    for span, name in sorted(worst_after, reverse=True)[:10]:
        print(f"  still {span:8.1f} m  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
