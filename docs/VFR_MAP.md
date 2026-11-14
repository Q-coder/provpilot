# VFR Map Frame

Heading-up VFR map inset (MFD-style) in the in-game HUD, rendered by the C++
`VfrMap` GDExtension module (`src/vfr_map.cpp`). The frame shows cartographic
hillshade terrain, range rings with a km scale bar, airport symbols + ICAO
labels, and canton borders — all rotating with the aircraft heading.

## Controls

| Action | Input |
|--------|-------|
| Show / hide map | `N` (anywhere, works while hidden; remembered — Continue Flight, closing the settings screen and the next launch keep the frame as you left it) |
| Zoom in / out | Mouse wheel over the frame, or `−` / `+` (`=` also zooms in; on Swiss layouts `+` is Shift+1) (5 → 10 → 20 → 40 → 80 km view width) |

Settings → **VFR Map**: enable/disable and frame size (Small 240 px / Medium
320 px / Large 400 px). The zoom level is persisted automatically. "Show Map
Frame" and the `N` key are the same flag: the toggle writes `[map]
map_enabled` with the layout and the host mirrors it into the SettingsStore
(`enabled_changed` signal), so the checkbox shows the live state and
`apply_config()` after the settings screen does not bring a hidden frame
back.

## Tile Data

Imagery is a cartographic hillshade (NW light, azimuth 315°, altitude 45°)
multiplied by a VFR-sectional hypsometric tint, generated offline from each
scenery area's elevation tiles (swissALTI3D for Switzerland):

```bash
python scripts/generate_vfr_map_tiles.py            # whole installed region
python scripts/generate_vfr_map_tiles.py --e-min 2619 --e-max 2630 \
    --n-min 1251 --n-max 1262                        # a tile index range
python scripts/generate_vfr_map_tiles.py --mips-only # rebuild only mip3/mip5

# One map per scenery area (next to that area's terrain/). Areas that share
# a border are generated with --merge-with, so the L3/L5 cells straddling it
# are rendered from both areas' elevation data:
python scripts/generate_vfr_map_tiles.py \
    --terrain-dir .../Germany/Baden-Wuertemberg/terrain \
    --output .../Germany/Baden-Wuertemberg/map \
    --merge-with .../Switzerland/terrain
```

Output (per scenery area, next to `terrain/` and `orthophoto/`). The runtime
appends every enabled area's `map/` directory in scenery-library priority
order, so tiles are served by the highest-priority area that has them:

| Level | Path | Cell | Pixels | Resolution |
|-------|------|------|--------|------------|
| L0 | `map/map_{e}_{n}.jpg` | 1024 m | 64×64 | 16 m/px |
| L3 | `map/mip3/map_{e}_{n}.jpg` | 8192 m | 64×64 | 128 m/px |
| L5 | `map/mip5/map_{e}_{n}.jpg` | 32768 m | 64×64 | 512 m/px |

File names encode the **SW corner** (west edge easting, south edge northing)
of each cell — the same LV95 grid family and pixel orientation as the
orthophoto layer (pixel(0,0) = SE corner, rows S→N, cols E→W). The runtime
flips both axes before creating textures.

The module auto-selects the finest level whose tiles still render at ≥ 40 px
on screen, so narrow views use L0 and wide views fall back to L3/L5.

L3/L5 cells that straddle the edge of the elevation data (e.g. along the
national borders, where the heightmap coverage stops) are zero-filled
instead of being dropped: the no-data part renders as a flat lowland tint,
so wide views keep full coverage to the map extent instead of showing gaps.
Cells that span two installed areas (the CH/BW border) take each subcell
from whichever area has it (`--merge-with`), so no foreign half is rendered
flat. Because the higher-priority area's copy of a shared cell wins at
runtime, regenerate **both** neighbors' mip levels after adding an area:

```bash
# 1. new area (full run, neighbor merged in)
python scripts/generate_vfr_map_tiles.py --terrain-dir .../BW/terrain \
    --output .../BW/map --merge-with .../CH/terrain
# 2. existing area (mips only — fast; fixes its border cells too)
python scripts/generate_vfr_map_tiles.py --terrain-dir .../CH/terrain \
    --output .../CH/map --mips-only --merge-with .../BW/terrain
```

## Runtime Behavior (`src/vfr_map.cpp`)

- **Projection** — aircraft Godot position → LV95 via the shared origin
  (`swiss_e = center_e − x`, `swiss_n = center_n + z`), then a heading-up
  rotation: screen-right = east·cosH − north·sinH, screen-forward =
  east·sinH + north·cosH. All math in double precision.
- **Streaming** — visible grid cells are resolved against an LRU texture
  cache. Missing cells are built by three loader threads, nearest to the
  aircraft first (`TileLoader`): the main thread only turns finished images
  into textures (24 per frame). Under the 5 - 40 km views the overview level
  (L5) is drawn first, so the frame is never empty while the finer cells
  arrive. Tile paths follow scenery-area priority order (first source with
  data wins, like orthophotos).
- **Base tier (no `map/` tiles)** — cells are shaded from the 34 x 34 terrain
  tiles. An overview cell (32.768 km, the 80 - 250 km views) is 1024 tile
  files; a 250 km view is about 170 000 of them (100 s from a cold disk,
  measured at Frankfurt). Two things remove that:
  - `scripts/build_map_overview.py <Region> ...` writes
    `<Region>/map_overview/L2/c_<ei>_<ni>.i16` (128 x 128 int16 metres, the
    very samples the renderer picks). One file per cell and region; the 250 km
    view fills in 0.3 s. Run it after every terrain bake; `bundle_world_pack.py`
    packs the folder. A region without it falls back to the tile files.
  - Composite cells (L3 / L5) are kept as PNG under
    `user://map_cache/<key>/L1|L2/`. The key hashes the terrain dirs and their
    modification times, so a changed scenery set starts a fresh cache; caches
    unused for 30 days are deleted. A 40 km view: 2.5 s the first time, 0.14 s
    after.
- **Terrain checkbox** — the title bar's checkbox (`[map] map_background`)
  turns the background off: symbols, rings and ILS feathers on a plain fill.
- **Redraw** — `_draw()` is throttled to ~10 Hz and only re-queued when
  state changes; the map freezes while welcome/settings overlays are up.
- **Static data** — airports from `res://airports.json` (international =
  rectangle, regional/GA = circle) and canton borders from
  `res://web/data/cantons.geojson` (WGS84 → LV95 via the swisstopo
  polynomial), parsed once at startup.

## Wiring (`provpilot/node_3d.gd`)

The GDScript side is intentionally tiny: a one-time `setup()` with the config
path, `set_tile_paths()` from `_apply_scenery_sources()` (the `map/` subdir of
each enabled scenery area), `apply_config()` after a settings save, the
`enabled_changed` signal into `SettingsStore.set_setting("map_enabled")`, and one
`WorldScenery.feed_vfr_map(ecef_pos, true_heading, ui_up)` call per frame
(ECEF → LV95 + grid heading, then `update_lv95`).

## Testing

Headless smoke test (hermetic — no JSBSim/scenery data needed):

```bash
godot --headless --path provpilot --res tests/smoke_vfr_map.tscn
```

It generates a synthetic tile pyramid in `/tmp`, drives the module around
LSZH, and asserts tile streaming, airport detection, heading propagation,
zoom steps, and the N-toggle visibility flag.

`tests/probe_vfr_map_fill.tscn` (non-headless, real scenery, run from a
scratch directory holding a copy of `provpilot.cfg`) prints the fill time of
the 250 / 160 / 40 / 20 km views and checks the Terrain checkbox.

## Future Work

- Water bodies composited into map tiles (elevation-band based).
- Airspace layer (CTR/TMA) once `airspace.json` lands (see
  SCENERY_ARCHITECTURE.md).
