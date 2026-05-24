#!/usr/bin/env python3
"""bundle_scenery_pack.py — Pack a region's "base" assets or a quadrant's
"detail" assets into a ZIP suitable for distribution.

Regions and quadrants are defined in `scripts/scenery_regions.json`. The same
asset directories used by Scenery3D at runtime are walked, tile filenames are
parsed for LV95 coordinates, and only the tiles intersecting the requested
bbox are added to the archive. Buildings, which use LK25/UTM naming rather
than raw LV95, are placed via per-tile `center_e/center_n` entries in their
`manifest.json`.

Examples:
    # All of Switzerland's base layer (terrain, roads, water, …)
    scripts/bundle_scenery_pack.py switzerland --base

    # Buildings + orthophoto for the SE quadrant of Switzerland
    scripts/bundle_scenery_pack.py switzerland --quadrant se

    # Inspect what would be packed without writing the ZIP
    scripts/bundle_scenery_pack.py baden-wuerttemberg --base --dry-run
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

TILE_M = 1024
TILE_RX = re.compile(r"_(\d{7})_(\d{7})\.[A-Za-z0-9]+$")
MIP_SUBDIRS = ["mip2", "mip3", "mip4"]
ALWAYS_INCLUDE = {
    "terrain": ["distant_terrain*", "*.json"],
}

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = ROOT / "scripts" / "scenery_regions.json"
DEFAULT_SCENERY = Path("/Users/gery/provpilot/scenery")
DEFAULT_OUTDIR = ROOT / "build" / "scenery"


# ── Config -----------------------------------------------------------------

@dataclass(frozen=True)
class Quadrant:
    id: str
    name: str
    bbox: tuple[int, int, int, int]   # (e_min, n_min, e_max, n_max)


@dataclass(frozen=True)
class Region:
    id: str
    name: str
    subdir: str
    bbox: tuple[int, int, int, int]
    quadrants: dict[str, Quadrant]


def load_config(path: Path) -> tuple[dict[str, Region], dict[str, list[str]]]:
    data = json.loads(path.read_text())
    regions: dict[str, Region] = {}
    for r in data["regions"]:
        qs = {q["id"]: Quadrant(q["id"], q["name"], tuple(q["bbox_lv95"])) for q in r["quadrants"]}
        regions[r["id"]] = Region(r["id"], r["name"], r["scenery_subdir"], tuple(r["bbox_lv95"]), qs)
    return regions, data["asset_classes"]


# ── Helpers ----------------------------------------------------------------

def fmt_size(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}"
        n /= 1024


def bbox_intersects_tile(bbox: tuple[int, int, int, int], e: int, n: int, size: int = TILE_M) -> bool:
    """True if the 1024 m tile at LV95 SE corner (e, n) overlaps `bbox`."""
    e_min, n_min, e_max, n_max = bbox
    return not (e + size <= e_min or e >= e_max or n + size <= n_min or n >= n_max)


def bbox_intersects_point(bbox: tuple[int, int, int, int], ce: float, cn: float,
                          half_e: float, half_n: float) -> bool:
    e_min, n_min, e_max, n_max = bbox
    return not (ce + half_e <= e_min or ce - half_e >= e_max or
                cn + half_n <= n_min or cn - half_n >= n_max)


def sha256_of_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ── Collection ------------------------------------------------------------

def collect_lv95_tiles(asset_dir: Path, bbox: tuple[int, int, int, int],
                       arc_prefix: str, include_mips: bool) -> tuple[list[tuple[Path, str]], int, int]:
    """Walk a directory of `<asset>_<E>_<N>.<ext>` tiles and return (matches, kept, skipped)."""
    out: list[tuple[Path, str]] = []
    kept = skipped = 0
    if not asset_dir.is_dir():
        return out, 0, 0

    # Always-included files (manifests + distant terrain mesh).
    pats = ALWAYS_INCLUDE.get(asset_dir.name, ["manifest.json"])
    for pat in pats:
        for g in asset_dir.glob(pat):
            if g.is_file():
                out.append((g, f"{arc_prefix}/{g.name}"))

    for f in asset_dir.iterdir():
        if not f.is_file():
            continue
        m = TILE_RX.search(f.name)
        if not m:
            continue
        e, n = int(m.group(1)), int(m.group(2))
        if bbox_intersects_tile(bbox, e, n):
            out.append((f, f"{arc_prefix}/{f.name}"))
            kept += 1
        else:
            skipped += 1

    if include_mips:
        for mip in MIP_SUBDIRS:
            mdir = asset_dir / mip
            if not mdir.is_dir():
                continue
            mm = mdir / "manifest.json"
            if mm.is_file():
                out.append((mm, f"{arc_prefix}/{mip}/manifest.json"))
            # Mip tiles cover a larger footprint; the filename's E/N still
            # marks the SE corner so the intersection test works unchanged.
            for f in mdir.iterdir():
                if not f.is_file():
                    continue
                m = TILE_RX.search(f.name)
                if not m:
                    continue
                e, n = int(m.group(1)), int(m.group(2))
                if bbox_intersects_tile(bbox, e, n, size=TILE_M * (1 << (MIP_SUBDIRS.index(mip) + 1))):
                    out.append((f, f"{arc_prefix}/{mip}/{f.name}"))
                    kept += 1

    return out, kept, skipped


def collect_buildings(bdir: Path, bbox: tuple[int, int, int, int],
                      arc_prefix: str) -> tuple[list[tuple[Path, str]], int, int, int]:
    """Bundle building GLBs whose centre lies within `bbox` (per manifest.json)."""
    out: list[tuple[Path, str]] = []
    if not bdir.is_dir():
        return out, 0, 0, 0
    bmanifest = bdir / "manifest.json"
    if not bmanifest.is_file():
        print(f"  buildings           ⚠ no manifest.json in {bdir} — skipping")
        return out, 0, 0, 0
    out.append((bmanifest, f"{arc_prefix}/manifest.json"))

    # LK25 sub-sheet ≈ 4500 × 3000 m. UTM-derived BW sheets are ~1 km. Pick
    # generous bounds so a single tile straddling the boundary is included.
    half_e, half_n = 2500.0, 1750.0

    try:
        bm = json.loads(bmanifest.read_text())
    except Exception as exc:
        print(f"  buildings manifest.json could not be parsed: {exc}")
        return out, 0, 0, 0

    tiles = bm.get("tiles", {})
    bbox_by_key: dict[str, tuple[float, float]] = {}
    iterable = tiles.items() if isinstance(tiles, dict) else (
        (e.get("tile_id") or Path(e.get("file", "")).stem, e) for e in tiles
    )
    for tid, entry in iterable:
        if not isinstance(entry, dict):
            continue
        ce, cn = entry.get("center_e"), entry.get("center_n")
        if ce is None or cn is None:
            continue
        key = Path(entry.get("file", "")).stem.replace("buildings_", "") or str(tid)
        bbox_by_key[key] = (ce, cn)

    kept = skipped = unknown = 0
    for f in bdir.iterdir():
        if not f.is_file() or f.suffix != ".glb":
            continue
        key = f.stem.replace("buildings_", "")
        coords = bbox_by_key.get(key)
        if coords is None:
            unknown += 1
            continue
        ce, cn = coords
        if bbox_intersects_point(bbox, ce, cn, half_e, half_n):
            out.append((f, f"{arc_prefix}/{f.name}"))
            kept += 1
        else:
            skipped += 1
    return out, kept, skipped, unknown


# ── Main ------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("region", help="Region id (e.g. switzerland, baden-wuerttemberg)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--base", action="store_true", help="Pack base layer (terrain, roads, water, …)")
    g.add_argument("--quadrant", metavar="ID", help="Pack a detail quadrant (buildings + orthophoto)")
    ap.add_argument("--config", default=str(DEFAULT_CONFIG))
    ap.add_argument("--scenery", default=str(DEFAULT_SCENERY))
    ap.add_argument("--out", default=None, help="Override output ZIP path")
    ap.add_argument("--no-mips", dest="include_mips", action="store_false")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--version", default="1", help="Pack version tag baked into the filename")
    args = ap.parse_args()

    regions, asset_classes = load_config(Path(args.config))
    region = regions.get(args.region)
    if region is None:
        sys.exit(f"Unknown region '{args.region}'. Known: {', '.join(regions)}")

    scenery = Path(args.scenery).resolve()
    region_root = scenery / region.subdir
    if not region_root.is_dir():
        sys.exit(f"Region scenery not found: {region_root}")

    if args.base:
        bbox = region.bbox
        assets = asset_classes["base"]
        pack_id = f"{region.id}-base-v{args.version}"
        label = f"{region.name} base layer"
    else:
        q = region.quadrants.get(args.quadrant)
        if q is None:
            sys.exit(f"Unknown quadrant '{args.quadrant}' for {region.id}. "
                     f"Known: {', '.join(region.quadrants)}")
        bbox = q.bbox
        assets = asset_classes["detail"]
        pack_id = f"{region.id}-detail-{q.id}-v{args.version}"
        label = f"{region.name} {q.name} detail"

    print(f"=== {label} ===")
    print(f"  bbox E[{bbox[0]:,}…{bbox[2]:,}]  N[{bbox[1]:,}…{bbox[3]:,}]")
    print(f"  assets: {', '.join(assets)}")

    files: list[tuple[Path, str]] = []
    total = 0
    seen: set[str] = set()

    def add_files(items: list[tuple[Path, str]]) -> None:
        nonlocal total
        for src, arc in items:
            if arc in seen:
                continue
            seen.add(arc)
            files.append((src, arc))
            total += src.stat().st_size

    arc_root = region.subdir  # keep the same hierarchy users see in their scenery library

    for asset in assets:
        adir = region_root / asset
        if not adir.is_dir():
            print(f"  {asset:<18} (missing, skipped)")
            continue
        if asset == "buildings":
            items, kept, skipped, unknown = collect_buildings(adir, bbox, f"{arc_root}/{asset}")
            note = f" ⚠ {unknown} unmapped" if unknown else ""
            print(f"  {asset:<18} kept {kept:>5}  skipped {skipped:>6}{note}")
        elif asset in ("trees", "airports"):
            # Small fixed assets — bundle wholesale.
            items = [(p, f"{arc_root}/{asset}/{p.relative_to(adir)}")
                     for p in adir.rglob("*") if p.is_file()]
            print(f"  {asset:<18} bundled {len(items):>5} (full directory)")
        else:
            items, kept, skipped = collect_lv95_tiles(
                adir, bbox, f"{arc_root}/{asset}", args.include_mips
            )
            print(f"  {asset:<18} kept {kept:>5}  skipped {skipped:>6}")
        add_files(items)

    print(f"\n  {len(files):,} files, {fmt_size(total)} raw")
    if not files:
        sys.exit("No files matched. Aborting.")

    if args.dry_run:
        print("  --dry-run: not writing ZIP")
        return 0

    out_path = Path(args.out) if args.out else DEFAULT_OUTDIR / f"{pack_id}.zip"
    out_path = out_path.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"\nWriting {out_path} (stored, no compression)…")

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        for i, (src, arc) in enumerate(files):
            zf.write(src, arc)
            if i and i % 500 == 0:
                pct = 100 * i / len(files)
                print(f"  {i:,}/{len(files):,} ({pct:.0f}%)", end="\r", flush=True)
    print(f"  {len(files):,}/{len(files):,} (100%)        ")

    size = out_path.stat().st_size
    digest = sha256_of_file(out_path)
    print(f"\n  pack_id: {pack_id}")
    print(f"  size:    {fmt_size(size)} ({size} bytes)")
    print(f"  sha256:  {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
