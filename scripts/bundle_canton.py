#!/usr/bin/env python3
"""bundle_canton.py — Pack all scenery tiles intersecting a Swiss canton into a ZIP.

Walks <scenery>/Switzerland/{terrain,orthophoto,orthophoto_hires,water,roads,
vegetation,buildings}/, parses the LV95 tile coordinates from each filename
(`<asset>_<E>_<N>.<ext>` with 1024 m tile size), tests intersection against the
canton polygon (read from web/data/cantons.geojson, reprojected WGS84→LV95),
and writes a stored (uncompressed) ZIP suitable for upload.  Optionally updates
web/manifest.json with size + sha256 so the website's map turns the canton
green automatically.

Usage:
    scripts/bundle_canton.py SH                         # writes build/scenery/SH.zip
    scripts/bundle_canton.py ZH --update-manifest       # also updates web/manifest.json
    scripts/bundle_canton.py SH --dry-run               # just print stats
    scripts/bundle_canton.py SH --buffer 0              # exact canton boundary (no 2 km margin)

Buildings are bundled only if Switzerland/buildings/manifest.json carries
per-tile bboxes (keys "tiles[].bbox" or "tiles[].lv95_bbox"); otherwise they are
skipped with a warning (the LK25 sheet naming `buildings_<sheet>-<idx>.glb`
does not contain raw LV95 coordinates).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import zipfile
from pathlib import Path

from pyproj import Transformer
from shapely.geometry import box, shape
from shapely.ops import transform as shp_transform

TILE_M = 1024
TILE_RX = re.compile(r"_(\d{7})_(\d{7})\.[A-Za-z0-9]+$")
ASSET_DIRS = ["terrain", "orthophoto", "orthophoto_hires", "water", "roads", "vegetation"]
MIP_SUBDIRS = ["mip2", "mip3", "mip4"]
ALWAYS_INCLUDE_TERRAIN_GLOBS = ["distant_terrain*", "*.json"]

ROOT = Path(__file__).resolve().parent.parent  # repo root
DEFAULT_CANTONS = ROOT / "web" / "data" / "cantons.geojson"
DEFAULT_MANIFEST = ROOT / "web" / "manifest.json"
DEFAULT_SCENERY = Path("/Users/gery/provpilot/scenery")
DEFAULT_OUTDIR = ROOT / "build" / "scenery"


def load_canton(geojson_path: Path, code: str):
    data = json.loads(geojson_path.read_text(encoding="utf-8"))
    for f in data["features"]:
        p = f.get("properties", {})
        if str(p.get("ak", "")).upper() == code.upper():
            return f, shape(f["geometry"])
    raise SystemExit(f"Canton '{code}' not found in {geojson_path}")


def fmt_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}"
        n /= 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("code", help="2-letter canton code (e.g. ZH, SH, VS)")
    ap.add_argument("--scenery", default=str(DEFAULT_SCENERY),
                    help=f"Scenery root [default {DEFAULT_SCENERY}]")
    ap.add_argument("--out", default=None,
                    help="Output ZIP path [default build/scenery/<CODE>.zip]")
    ap.add_argument("--cantons-geojson", default=str(DEFAULT_CANTONS))
    ap.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    ap.add_argument("--buffer", type=float, default=2000.0,
                    help="Metres of buffer around canton boundary [default 2000]")
    ap.add_argument("--no-mips", dest="include_mips", action="store_false",
                    help="Skip mip2/mip3/mip4 subdirs (smaller bundle)")
    ap.add_argument("--update-manifest", action="store_true",
                    help="Update web/manifest.json with the new bundle info")
    ap.add_argument("--dry-run", action="store_true",
                    help="Compute file list + total size but do not write ZIP")
    args = ap.parse_args()

    code = args.code.upper()
    scenery = Path(args.scenery).resolve()
    if not scenery.is_dir():
        sys.exit(f"Scenery root not found: {scenery}")

    feature, poly_wgs = load_canton(Path(args.cantons_geojson), code)
    name = feature["properties"].get("name", code)
    print(f"Canton {code} ({name})")

    # WGS84 -> LV95
    tr = Transformer.from_crs("EPSG:4326", "EPSG:2056", always_xy=True)
    poly_lv95 = shp_transform(lambda x, y, z=None: tr.transform(x, y), poly_wgs)
    poly_buf = poly_lv95.buffer(args.buffer) if args.buffer else poly_lv95
    minx, miny, maxx, maxy = poly_buf.bounds
    print(f"  area {poly_lv95.area / 1e6:.0f} km²  |  bbox E[{minx:.0f}, {maxx:.0f}]  N[{miny:.0f}, {maxy:.0f}]")

    def tile_intersects(e: int, n: int) -> bool:
        return poly_buf.intersects(box(e, n, e + TILE_M, n + TILE_M))

    ch_root = scenery / "Switzerland"
    if not ch_root.is_dir():
        sys.exit(f"Missing: {ch_root}")

    files: list[tuple[Path, str]] = []
    _arcs_seen: set[str] = set()
    total = 0

    def add(p: Path, arc: str):
        nonlocal total
        if arc in _arcs_seen:
            return
        _arcs_seen.add(arc)
        files.append((p, arc))
        total += p.stat().st_size

    # --- Asset dirs with LV95-named tiles ---
    for asset in ASSET_DIRS:
        adir = ch_root / asset
        if not adir.is_dir():
            continue
        kept = 0
        skipped = 0
        # manifest + distant_terrain* (terrain only)
        for pat in (ALWAYS_INCLUDE_TERRAIN_GLOBS if asset == "terrain" else ["manifest.json"]):
            for g in adir.glob(pat):
                if g.is_file():
                    add(g, f"Switzerland/{asset}/{g.name}")
        # tile files
        for f in adir.iterdir():
            if not f.is_file():
                continue
            m = TILE_RX.search(f.name)
            if not m:
                continue
            e, n = int(m.group(1)), int(m.group(2))
            if tile_intersects(e, n):
                add(f, f"Switzerland/{asset}/{f.name}")
                kept += 1
            else:
                skipped += 1
        # mip pyramid
        if args.include_mips:
            for mip in MIP_SUBDIRS:
                mdir = adir / mip
                if not mdir.is_dir():
                    continue
                mm = mdir / "manifest.json"
                if mm.is_file():
                    add(mm, f"Switzerland/{asset}/{mip}/manifest.json")
                for f in mdir.iterdir():
                    if not f.is_file():
                        continue
                    m = TILE_RX.search(f.name)
                    if not m:
                        continue
                    e, n = int(m.group(1)), int(m.group(2))
                    if tile_intersects(e, n):
                        add(f, f"Switzerland/{asset}/{mip}/{f.name}")
                        kept += 1
        print(f"  {asset:<18} kept {kept:>5}  skipped {skipped:>6}")

    # --- Buildings (LK25 sub-sheet naming buildings_<sheet>-<idx>.glb) ---
    # manifest.json: {tiles: {"1172-31": {file, tile_id, center_e, center_n, building_count}}}
    # LK25 sub-sheet is ~4375 m × 3000 m; use center +/- half plus a small margin.
    LK25_HALF_E = 4500 / 2 + 200
    LK25_HALF_N = 3000 / 2 + 200
    bdir = ch_root / "buildings"
    if bdir.is_dir():
        bmanifest = bdir / "manifest.json"
        sheet_bbox: dict[str, tuple[float, float, float, float]] = {}
        if bmanifest.is_file():
            try:
                bm = json.loads(bmanifest.read_text())
                tiles = bm.get("tiles", {})
                if isinstance(tiles, dict):
                    iterable = tiles.items()
                else:
                    iterable = ((e.get("tile_id") or Path(e.get("file", "")).stem, e) for e in tiles)
                for tid, entry in iterable:
                    if not isinstance(entry, dict):
                        continue
                    bb = entry.get("bbox") or entry.get("lv95_bbox")
                    if bb and len(bb) == 4:
                        sheet_bbox[Path(entry.get("file", tid)).stem.replace("buildings_", "")] = tuple(bb)
                        continue
                    ce = entry.get("center_e")
                    cn = entry.get("center_n")
                    if ce is not None and cn is not None:
                        key = Path(entry.get("file", "")).stem.replace("buildings_", "") or str(tid)
                        sheet_bbox[key] = (ce - LK25_HALF_E, cn - LK25_HALF_N,
                                           ce + LK25_HALF_E, cn + LK25_HALF_N)
            except Exception as exc:
                print(f"  buildings/manifest.json: could not parse ({exc})")
            add(bmanifest, "Switzerland/buildings/manifest.json")
        kept = skipped = unknown = 0
        for f in bdir.iterdir():
            if not f.is_file() or f.suffix != ".glb":
                continue
            key = f.stem.replace("buildings_", "")
            bb = sheet_bbox.get(key)
            if bb is None:
                unknown += 1
                continue
            if poly_buf.intersects(box(*bb)):
                add(f, f"Switzerland/buildings/{f.name}")
                kept += 1
            else:
                skipped += 1
        if unknown:
            print(f"  buildings           kept {kept:>5}  skipped {skipped:>6}  "
                  f"(⚠ {unknown} tiles with no center/bbox in manifest — not included)")
        else:
            print(f"  buildings           kept {kept:>5}  skipped {skipped:>6}")

    print(f"\n  {len(files)} files, {fmt_size(total)} raw")
    if not files:
        sys.exit("No files matched. Aborting.")

    if args.dry_run:
        print("  --dry-run: not writing ZIP")
        return

    out_path = Path(args.out) if args.out else DEFAULT_OUTDIR / f"{code}.zip"
    out_path = out_path.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"\nWriting {out_path} (stored, no compression — jpg/png/raw already compact)…")

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        for i, (src, arc) in enumerate(files):
            zf.write(src, arc)
            if i and i % 500 == 0:
                pct = 100 * i / len(files)
                print(f"  {i}/{len(files)} ({pct:.0f}%)", end="\r", flush=True)
    print(f"  {len(files)}/{len(files)} (100%)        ")

    sha = hashlib.sha256()
    with open(out_path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            sha.update(chunk)
    size = out_path.stat().st_size
    digest = sha.hexdigest()
    print(f"\n  size: {fmt_size(size)} ({size} bytes)")
    print(f"  sha256: {digest}")

    if args.update_manifest:
        mpath = Path(args.manifest)
        m = json.loads(mpath.read_text())
        m.setdefault("bundles", {})
        m["bundles"][code] = {
            "name": name,
            "status": "available",
            "size_mb": round(size / (1024 * 1024), 1),
            "url": f"releases/{out_path.name}",
            "sha256": digest,
        }
        mpath.write_text(json.dumps(m, indent=2, ensure_ascii=False) + "\n")
        print(f"  updated {mpath}")
        print(f"\nNext: upload {out_path} to .../provpilot/releases/{out_path.name}")
        print(f"      upload {mpath} to .../provpilot/manifest.json")


if __name__ == "__main__":
    main()
