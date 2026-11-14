#!/usr/bin/env python3
"""bundle_world_pack.py — ZIP the worldwide *base tier* packs for the in-app
downloader (Settings → Downloads, `src/scenery_downloader_panel.cpp`).

The base tier (docs/WORLD_SCENERY_DESIGN.md §11, §13.1) is one 34 px float32
terrain tile per square kilometre plus the airport catalog of the region
(`airports/runways.json`) and, where baked, the base roads and railways
(`roads/`). It is what `scripts/world_regions.json` registers and
`build_world_strip.sh` bakes to `<build_root>/<id>/<Subdir>/`. This script
packs those folders, one ZIP per region, and writes the manifest the
downloader fetches:

    python3 scripts/bundle_world_pack.py --all --manifest
    python3 scripts/bundle_world_pack.py switzerland-base austria --version 2

ZIP layout: `<Subdir>/terrain/...`, `<Subdir>/airports/...`, `<Subdir>/roads/...`
— the downloader extracts into its install root and finds the region
directory by walking the single-child chain, so the extracted tree is a
Scenery Library area like any other (`<root>/Switzerland`). The bake's
`terrain_base/` layer (the pristine terrain the editor bakes from, see
docs/TERRAIN_SETUP.md) is left out: the sim never reads it and it doubles
the pack.

Compression: deflate level 1 — the float32 tiles only shrink by about 17 %,
but that is 3.5 GB over the nine regions, and ZIPReader inflates faster than
the files download. `--stored` packs without compression.

Manifest (`--manifest`, schema 1, the shape `generate_scenery_manifest.py`
documents): `{schema_version, base_url, regions [{id, name, scenery_subdir,
base {pack_id, version, filename, url, size, sha256}, detail_packs []}]}`.
It describes every `<id>-base-v<ver>.zip` found in the output directory, so
rebuilding one region and re-running `--manifest-only` refreshes the file.

Naming note (design §11.3): the base packs own the plain country directories
(`Switzerland`, `Germany`, …). A 1 m add-on installed under the same name
(the Switzerland HD pack of the first releases uses the same `tile_E_N.raw`
names) must be moved to `Switzerland-HD` before a base pack is installed
beside it, or the base tiles overwrite the add-on's.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = ROOT / "scripts" / "world_regions.json"
DEFAULT_BASE_URL = "https://www.md80.ch/provpilot/scenery/"
DEFAULT_SUBDIRS = ("terrain", "airports", "roads", "map_overview")
SKIP_NAMES = {".DS_Store", "Thumbs.db"}


def fmt_size(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} TB"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def load_regions(config: Path) -> tuple[Path, dict[str, dict]]:
    data = json.loads(config.read_text())
    regions = {r["id"]: r for r in data["regions"]}
    return Path(data["build_root"]), regions


def iter_files(src_dir: Path, subdirs: tuple[str, ...]):
    """Yield (absolute path, archive-relative path below the region dir)."""
    for sub in subdirs:
        d = src_dir / sub
        if not d.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(d):
            dirnames.sort()
            rel_dir = Path(dirpath).relative_to(src_dir)
            for name in sorted(filenames):
                if name in SKIP_NAMES:
                    continue
                yield Path(dirpath) / name, rel_dir / name


def bundle(region: dict, build_root: Path, out_dir: Path, version: str,
           subdirs: tuple[str, ...], stored: bool, dry_run: bool) -> Path | None:
    rid, subdir = region["id"], region["subdir"]
    src_dir = build_root / rid / subdir
    if not src_dir.is_dir():
        print(f"  {rid}: no bake at {src_dir}", file=sys.stderr)
        return None
    out = out_dir / f"{rid}-base-v{version}.zip"
    present = [s for s in subdirs if (src_dir / s).is_dir()]
    print(f"== {rid} ({subdir}): {', '.join(present)} -> {out.name}")
    if dry_run:
        n = total = 0
        for abs_path, _ in iter_files(src_dir, subdirs):
            n += 1
            total += abs_path.stat().st_size
        print(f"   {n} files, {fmt_size(total)} (dry run)")
        return None
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".zip.part")
    method = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    t0 = time.time()
    n = total = 0
    with zipfile.ZipFile(tmp, "w", method, allowZip64=True, compresslevel=1) as zf:
        for abs_path, rel in iter_files(src_dir, subdirs):
            zf.write(abs_path, f"{subdir}/{rel.as_posix()}")
            n += 1
            total += abs_path.stat().st_size
            if n % 25000 == 0:
                print(f"   {n} files, {fmt_size(total)} raw, {time.time() - t0:.0f} s", file=sys.stderr)
    tmp.replace(out)
    size = out.stat().st_size
    print(f"   {n} files, {fmt_size(total)} raw -> {fmt_size(size)} zip in {time.time() - t0:.0f} s")
    return out


def write_manifest(regions: dict[str, dict], out_dir: Path, base_url: str, manifest_path: Path) -> None:
    entries = []
    for rid, region in regions.items():
        zips = sorted(out_dir.glob(f"{rid}-base-v*.zip"))
        if not zips:
            continue
        # highest version tag wins (numeric where possible)
        def ver_of(p: Path) -> tuple:
            tag = p.stem[len(rid) + len("-base-v"):]
            try:
                return (0, float(tag), tag)
            except ValueError:
                return (1, 0.0, tag)
        z = max(zips, key=ver_of)
        version = ver_of(z)[2]
        print(f"  {z.name}: hashing…")
        entries.append({
            "id": rid,
            "name": region["name"],
            "scenery_subdir": region["subdir"],
            "base": {
                "pack_id": f"{rid}-base-v{version}",
                "version": version,
                "filename": z.name,
                "url": base_url + z.name,
                "size": z.stat().st_size,
                "sha256": sha256_of(z),
            },
            "detail_packs": [],
        })
    manifest = {"schema_version": 1, "base_url": base_url, "regions": entries}
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    total = sum(e["base"]["size"] for e in entries)
    print(f"Manifest: {manifest_path} ({len(entries)} regions, {fmt_size(total)} hosted)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("regions", nargs="*", help="Region ids from world_regions.json (e.g. switzerland-base austria)")
    ap.add_argument("--all", action="store_true", help="Every region of the registry that has a bake")
    ap.add_argument("--config", default=str(DEFAULT_CONFIG))
    ap.add_argument("--build-root", default=None, help="Bake root (default: world_regions.json build_root)")
    ap.add_argument("--out-dir", default=None, help="Where the ZIPs go (default: <build_root>/world-packs)")
    ap.add_argument("--version", default="1", help="Pack version tag in the filename and the manifest")
    ap.add_argument("--subdirs", default=",".join(DEFAULT_SUBDIRS),
                    help="Comma-separated asset folders to pack (default terrain,airports,roads,map_overview)")
    ap.add_argument("--stored", action="store_true", help="No compression (ZIP_STORED)")
    ap.add_argument("--dry-run", action="store_true", help="Count files and bytes only")
    ap.add_argument("--manifest", action="store_true", help="Also write scenery_manifest.json in the output dir")
    ap.add_argument("--manifest-only", action="store_true", help="Only (re)write the manifest from the ZIPs present")
    ap.add_argument("--manifest-path", default=None, help="Manifest file (default <out_dir>/scenery_manifest.json)")
    ap.add_argument("--base-url", default=DEFAULT_BASE_URL)
    args = ap.parse_args()

    build_root, regions = load_regions(Path(args.config))
    if args.build_root:
        build_root = Path(args.build_root)
    out_dir = Path(args.out_dir) if args.out_dir else build_root / "world-packs"
    manifest_path = Path(args.manifest_path) if args.manifest_path else out_dir / "scenery_manifest.json"
    subdirs = tuple(s.strip() for s in args.subdirs.split(",") if s.strip())

    if not args.manifest_only:
        ids = list(regions) if args.all else args.regions
        if not ids:
            ap.error("give region ids or --all")
        unknown = [i for i in ids if i not in regions]
        if unknown:
            ap.error(f"unknown region(s): {', '.join(unknown)} (known: {', '.join(regions)})")
        for rid in ids:
            bundle(regions[rid], build_root, out_dir, args.version, subdirs, args.stored, args.dry_run)
    if (args.manifest or args.manifest_only) and not args.dry_run:
        write_manifest(regions, out_dir, args.base_url, manifest_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
