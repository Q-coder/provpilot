# VFR Map Frame

Heading-up VFR map inset (MFD-style) in the in-game HUD, rendered by the C++
`VfrMap` GDExtension module (`src/vfr_map.cpp`). The frame shows cartographic
hillshade terrain, range rings with a km scale bar, airport symbols + ICAO
labels, and canton borders — all rotating with the aircraft heading.

## Controls

| Action | Input |
|--------|-------|
| Show / hide map | `N` (anywhere, works while hidden) |
| Zoom in / out | Mouse wheel over the frame, or `−` / `+` (`=` also zooms in; on Swiss layouts `+` is Shift+1) (5 → 10 → 20 → 40 → 80 km view width) |

Settings → **VFR Map**: enable/disable and frame size (Small 240 px / Medium
320 px / Large 400 px). The zoom level is persisted automatically.

## Tile Data

Imagery is a cartographic hillshade (NW light, azimuth 315°, altitude 45°)
multiplied by a VFR-sectional hypsometric tint, generated offline from the
swissALTI3D elevation tiles:

```bash
python scripts/generate_vfr_map_tiles.py            # whole installed region
python scripts/generate_vfr_map_tiles.py --e-min 2619 --e-max 2630 \
    --n-min 1251 --n-max 1262                        # a tile index range
python scripts/generate_vfr_map_tiles.py --mips-only # rebuild only mip3/mip5
```

Output (per scenery area, next to `terrain/` and `orthophoto/`):

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
Swiss border, where swissALTI3D stops) are zero-filled instead of being
dropped: the no-data part renders as a flat lowland tint, so wide views keep
full coverage to the map extent instead of showing gaps in the east.

## Runtime Behavior (`src/vfr_map.cpp`)

- **Projection** — aircraft Godot position → LV95 via the shared origin
  (`swiss_e = center_e − x`, `swiss_n = center_n + z`), then a heading-up
  rotation: screen-right = east·cosH − north·sinH, screen-forward =
  east·sinH + north·cosH. All math in double precision.
- **Streaming** — visible grid cells are resolved against an LRU texture
  cache (256 tiles) with a budget of 2 JPEG decodes per frame; missing tiles
  show the neutral background fill. Tile paths follow scenery-area priority
  order (first source with data wins, like orthophotos).
- **Redraw** — `_draw()` is throttled to ~10 Hz and only re-queued when
  state changes; the map freezes while welcome/settings overlays are up.
- **Static data** — airports from `res://airports.json` (international =
  rectangle, regional/GA = circle) and canton borders from
  `res://web/data/cantons.geojson` (WGS84 → LV95 via the swisstopo
  polynomial), parsed once at startup.

## Wiring (`provpilot/node_3d.gd`)

The GDScript side is intentionally tiny: a one-time `setup()` with the Swiss
origin + config path, `set_tile_paths()` from `_apply_scenery_sources()`
(the `map/` subdir of each enabled scenery area), `apply_config()` after a
settings save, and one `update(pos, heading, ui_up)` call per frame.

## Testing

Headless smoke test (hermetic — no JSBSim/scenery data needed):

```bash
godot --headless --path provpilot --res tests/smoke_vfr_map.tscn
```

It generates a synthetic tile pyramid in `/tmp`, drives the module around
LSZH, and asserts tile streaming, airport detection, heading propagation,
zoom steps, and the N-toggle visibility flag.

## Future Work

- Water bodies composited into map tiles (elevation-band based).
- Airspace layer (CTR/TMA) once `airspace.json` lands (see
  SCENERY_ARCHITECTURE.md).
