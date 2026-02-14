#!/usr/bin/env python3
"""Check SwissALTI3D coverage: what's cached, what's generated, what's remaining."""

from pathlib import Path
import json
import re

# SwissALTI3D coverage (LV95 km)
E_MIN, E_MAX = 2480, 2840
N_MIN, N_MAX = 1070, 1300

CACHE_DIR = Path("/Users/gerhardgubler/Desktop/usb-ssd/Mac/terrain_cache/")
SCENERY_DIR = Path("/Users/gerhardgubler/code/provpilot/scenery/terrain/")
TILE_SIZE = 1024

# --- Source tiles ---
total_source = (E_MAX - E_MIN) * (N_MAX - N_MIN)
print("=== SwissALTI3D Coverage ===")
print(f"Bounding box: E {E_MIN}-{E_MAX} km, N {N_MIN}-{N_MAX} km")
print(f"Total possible 1km source tiles in bbox: {total_source}")

# Count cached TIFs
print("\nScanning cache directory...")
cached_coords = set()
sizes = []
for p in CACHE_DIR.glob("swissalti3d_*.tif"):
    m = re.search(r'_(\d+)-(\d+)_', p.name)
    if m:
        cached_coords.add((int(m.group(1)), int(m.group(2))))
        if len(sizes) < 200:
            sizes.append(p.stat().st_size)

cached_in_swiss = {c for c in cached_coords if E_MIN <= c[0] <= E_MAX and N_MIN <= c[1] <= N_MAX}
print(f"Cached source TIFs (total): {len(cached_coords)}")
print(f"Cached within Swiss bbox: {len(cached_in_swiss)}")

# Negative cache
neg_path = CACHE_DIR / "_negative_cache.json"
neg_in_swiss = set()
if neg_path.exists():
    with open(neg_path) as f:
        neg = json.load(f)
    neg_in_swiss = {(e, n) for e, n in neg if E_MIN <= e <= E_MAX and N_MIN <= n <= N_MAX}
    print(f"Negative cache (Swiss bbox): {len(neg_in_swiss)} tiles known non-existent")
else:
    print("No negative cache found")

# Remaining
all_swiss = {(e, n) for e in range(E_MIN, E_MAX) for n in range(N_MIN, N_MAX)}
known = cached_in_swiss | neg_in_swiss
remaining = all_swiss - known
# Tiles that actually exist on SwissTopo ≈ cached + remaining that will be real
# Switzerland ≈ 41,285 km², bbox ≈ 82,800 km² → ~50% coverage ratio
actual_ratio = len(cached_in_swiss) / (len(cached_in_swiss) + len(neg_in_swiss)) if neg_in_swiss else 0.5
estimated_real_remaining = int(len(remaining) * actual_ratio)

print(f"\nRemaining undiscovered in Swiss bbox: {len(remaining)}")
print(f"  Of which estimated real tiles: ~{estimated_real_remaining}")
print(f"  Of which estimated non-existent: ~{len(remaining) - estimated_real_remaining}")

# Size estimates
avg_size = sum(sizes) / len(sizes) if sizes else 1_000_000
print(f"\n=== Download Estimates ===")
print(f"Average TIF size: {avg_size / 1024 / 1024:.1f} MB")
print(f"Already downloaded: {len(cached_coords) * avg_size / 1024**3:.1f} GB")
print(f"Estimated remaining download: ~{estimated_real_remaining * avg_size / 1024**3:.1f} GB")
total_download = (len(cached_in_swiss) + estimated_real_remaining) * avg_size / 1024**3
print(f"Estimated total download for all Swiss: ~{total_download:.0f} GB")

# --- Output tiles ---
raw_size = 4 * 1024 * 1024  # 4 MB per float32 1024x1024 .raw
out_w = (E_MIN * 1000 // TILE_SIZE) * TILE_SIZE
out_s = (N_MIN * 1000 // TILE_SIZE) * TILE_SIZE
out_e = ((E_MAX * 1000 + TILE_SIZE - 1) // TILE_SIZE) * TILE_SIZE
out_n = ((N_MAX * 1000 + TILE_SIZE - 1) // TILE_SIZE) * TILE_SIZE
total_output = ((out_e - out_w) // TILE_SIZE) * ((out_n - out_s) // TILE_SIZE)

existing_output = len(list(SCENERY_DIR.glob("tile_*.raw"))) if SCENERY_DIR.exists() else 0
remaining_output = total_output - existing_output

print(f"\n=== Output Tiles (1024m .raw) ===")
print(f"Total for full Swiss bbox: {total_output}")
print(f"Already generated: {existing_output}")
print(f"Remaining: {remaining_output}")
print(f"Size per tile: {raw_size / 1024 / 1024:.0f} MB")
print(f"Current output size: ~{existing_output * raw_size / 1024**3:.1f} GB")
print(f"Remaining output: ~{remaining_output * raw_size / 1024**3:.1f} GB")
print(f"Total output (full Swiss bbox): ~{total_output * raw_size / 1024**3:.0f} GB")

print(f"\n=== Summary ===")
print(f"To complete all of Switzerland:")
print(f"  Source TIFs to download:  ~{estimated_real_remaining * avg_size / 1024**3:.0f} GB")
print(f"  Output .raw tiles to gen: ~{remaining_output * raw_size / 1024**3:.0f} GB")
print(f"  Total additional disk:    ~{(estimated_real_remaining * avg_size + remaining_output * raw_size) / 1024**3:.0f} GB")
