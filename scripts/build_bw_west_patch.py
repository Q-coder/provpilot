#!/usr/bin/env python3
"""build_bw_west_patch.py — Package newly-converted western BW building tiles
into a distributable patch ZIP.

The western Upper-Rhine strip of Baden-Württemberg shipped without LoD2
buildings (the LGL source download stopped at UTM easting 433 km). After
`tools/download_lgl_lod2.py` fills the gap into a staging directory, this
script bundles the new GLBs plus the complete merged manifest into a ZIP whose
internal layout matches the existing detail packs:

    Germany/Baden-Wuertemberg/buildings/manifest.json      (complete, merged)
    Germany/Baden-Wuertemberg/buildings/buildings_<E>_<N>.glb   (new western)

Extracting it over an existing install (into the scenery root, exactly like the
in-app downloader does) adds the western buildings and refreshes the manifest;
existing tiles are untouched. ZIP_STORED + ZIP64 to match bundle_scenery_pack.

Usage:
    scripts/build_bw_west_patch.py \\
        --staging /Volumes/Data1/provpilot/build/bw-west-buildings \\
        --out     /Volumes/Data1/provpilot/build/scenery/bw-west-building-patch-v1.zip
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

ARC_PREFIX = "Germany/Baden-Wuertemberg/buildings"


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the BW western buildings patch ZIP.")
    ap.add_argument("--staging", required=True,
                    help="Staging buildings dir produced by download_lgl_lod2.py "
                         "(contains the new GLBs + a complete merged manifest.json).")
    ap.add_argument("--out", required=True, help="Output ZIP path.")
    ap.add_argument("--dry-run", action="store_true",
                    help="List what would be packed without writing the ZIP.")
    args = ap.parse_args()

    staging = Path(args.staging).expanduser().resolve()
    manifest_path = staging / "manifest.json"
    if not manifest_path.is_file():
        print(f"ERROR: no manifest.json in {staging}", file=sys.stderr)
        return 1

    manifest = json.loads(manifest_path.read_text())
    total_tiles = len(manifest.get("tiles", {}))

    glbs = sorted(p for p in staging.glob("buildings_*.glb") if p.is_file())
    if not glbs:
        print(f"ERROR: no buildings_*.glb in {staging} — did the convert step run?",
              file=sys.stderr)
        return 1

    glb_bytes = sum(p.stat().st_size for p in glbs)
    print(f"Staging dir : {staging}")
    print(f"Manifest    : {total_tiles} total tiles (complete, merged)")
    print(f"New GLBs     : {len(glbs)} files, {glb_bytes / 1e6:.1f} MB")
    print(f"Output ZIP   : {args.out}")

    if args.dry_run:
        print("\n[dry-run] first 5 GLBs:")
        for p in glbs[:5]:
            print(f"  {ARC_PREFIX}/{p.name}")
        return 0

    out_path = Path(args.out).expanduser().resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        # Complete merged manifest first.
        zf.write(manifest_path, f"{ARC_PREFIX}/manifest.json")
        for i, p in enumerate(glbs, 1):
            zf.write(p, f"{ARC_PREFIX}/{p.name}")
            if i % 100 == 0 or i == len(glbs):
                print(f"  packed {i}/{len(glbs)}")

    size = out_path.stat().st_size
    sha = hashlib.sha256(out_path.read_bytes()).hexdigest()
    print("\nDone.")
    print(f"  ZIP   : {out_path}")
    print(f"  Size  : {size} bytes ({size / 1e6:.1f} MB)")
    print(f"  SHA256: {sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
