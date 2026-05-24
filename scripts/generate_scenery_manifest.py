#!/usr/bin/env python3
"""generate_scenery_manifest.py — Scan `build/scenery/*.zip` and produce a
`scenery_manifest.json` describing every pack found, cross-referenced with
the region/quadrant definitions in `scenery_regions.json`.

The output schema is what the ProvPilot in-app downloader expects:

    {
      "schema_version": 1,
      "base_url": "https://www.md80.ch/provpilot/scenery/",
      "regions": [
        {
          "id": "switzerland",
          "name": "Switzerland",
          "base": { "url": "...", "size": …, "sha256": "…", "version": "1" },
          "detail_packs": [
            { "id": "nw", "name": "North-West", "url": "…", "size": …, … },
            …
          ]
        },
        …
      ]
    }

Pack ZIPs are expected to follow the naming produced by
`bundle_scenery_pack.py`:
    <region>-base-v<ver>.zip
    <region>-detail-<quadrant>-v<ver>.zip
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = ROOT / "scripts" / "scenery_regions.json"
DEFAULT_INDIR = ROOT / "build" / "scenery"
DEFAULT_OUT = ROOT / "build" / "scenery_manifest.json"
DEFAULT_BASE_URL = "https://www.md80.ch/provpilot/scenery/"

BASE_RX = re.compile(r"^(?P<region>[a-z0-9-]+)-base-v(?P<ver>[\w.]+)\.zip$")
DETAIL_RX = re.compile(r"^(?P<region>[a-z0-9-]+)-detail-(?P<quad>[a-z0-9]+)-v(?P<ver>[\w.]+)\.zip$")


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=str(DEFAULT_CONFIG))
    ap.add_argument("--indir", default=str(DEFAULT_INDIR),
                    help="Directory containing the pack ZIPs")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--base-url", default=DEFAULT_BASE_URL,
                    help="HTTP base URL prepended to each pack's filename")
    args = ap.parse_args()

    cfg = json.loads(Path(args.config).read_text())
    indir = Path(args.indir)
    if not indir.is_dir():
        sys.exit(f"Pack directory not found: {indir}")

    # Index ZIPs by (region, quad?) so the manifest follows config order.
    by_base: dict[str, Path] = {}
    by_detail: dict[tuple[str, str], Path] = {}
    for z in sorted(indir.glob("*.zip")):
        m = BASE_RX.match(z.name)
        if m:
            by_base[m["region"]] = z
            continue
        m = DETAIL_RX.match(z.name)
        if m:
            by_detail[(m["region"], m["quad"])] = z
            continue

    out_regions = []
    for r in cfg["regions"]:
        entry: dict = {"id": r["id"], "name": r["name"]}
        z = by_base.get(r["id"])
        if z is not None:
            print(f"  {z.name}: hashing…")
            entry["base"] = {
                "url": args.base_url + z.name,
                "size": z.stat().st_size,
                "sha256": sha256_of(z),
                "version": BASE_RX.match(z.name)["ver"],
            }
        else:
            print(f"  {r['id']}: no base pack found")
        packs = []
        for q in r["quadrants"]:
            z = by_detail.get((r["id"], q["id"]))
            if z is None:
                continue
            print(f"  {z.name}: hashing…")
            packs.append({
                "id": q["id"],
                "name": q["name"],
                "url": args.base_url + z.name,
                "size": z.stat().st_size,
                "sha256": sha256_of(z),
                "version": DETAIL_RX.match(z.name)["ver"],
            })
        if packs:
            entry["detail_packs"] = packs
        if "base" in entry or "detail_packs" in entry:
            out_regions.append(entry)

    manifest = {
        "schema_version": 1,
        "base_url": args.base_url,
        "regions": out_regions,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print(f"\nWrote {out_path} ({len(out_regions)} region(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
