#!/usr/bin/env python3
"""Delete baked tiles of a region that are not in its current plan.json
(used after changing a region's selection, e.g. bbox → country)."""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_world_terrain as b
region = b.get_region(sys.argv[1])
plan = b.load_plan(region)
keep = {f"tile_{e}_{n}.raw" for e, n in zip(plan["cells_e"], plan["cells_n"])}
removed = 0
for f in region.terrain_out.iterdir():
    if f.name.startswith("tile_") and f.name not in keep:
        f.unlink(); removed += 1
print(f"  {region.id}: pruned {removed} tiles outside the plan, {len(keep)} kept")
