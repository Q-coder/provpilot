# Terrain Setup and JSBSim Collision Detection

This document describes the terrain system, building pipeline, coordinate systems, and collision architecture used in ProvPilot.

## Overview

The flight simulator uses:
- **Terrain3D** for visual terrain rendering and collision queries
- **TerrainStreamer** for dynamic tile-based terrain loading at runtime
- **JSBSim** for flight dynamics and ground reaction physics
- **SwissTopo swissALTI3D** for high-resolution elevation data (0.5m native, 1m resampled)
- **SwissTopo swissBUILDINGS3D** for 3D building models
- **BuildingLoader** for proximity-based dynamic building GLB loading

## Terrain Streaming Architecture

### Tile System

Terrain is divided into 1024×1024m tiles aligned to the Swiss LV95 coordinate grid. Each tile covers one Terrain3D region.

| Parameter | Value |
|-----------|-------|
| **Tile Size** | 1024 × 1024 meters |
| **Pixels per Tile** | 1024 × 1024 (1m resolution) |
| **Terrain Center** | E 2,682,880 / N 1,283,072 |
| **Grid Alignment** | Center is 2620 × 1024 (E), 1253 × 1024 (N) |
| **Load Radius** | 12 tiles (configurable) |
| **Loaded Area** | 25 × 25 tiles = ~25km × 25km |
| **Unload Margin** | 2 tiles (hysteresis) |
| **Format** | Raw float32 heightmaps |

### TerrainStreamer (terrain_streamer.gd)

The `TerrainStreamer` node runs as a child of the main scene and handles:

1. **Manifest loading**: Reads `manifest.json` from the tile directory to discover available tiles
2. **Initial load**: On startup, loads all tiles within `load_radius` of the aircraft
3. **Streaming**: Every `update_interval` physics frames, checks aircraft position and loads/unloads tiles
4. **Budget**: Limits tile loads per update cycle (`load_budget_per_update`) to avoid frame drops

**Critical alignment rule**: `terrain_center_e` and `terrain_center_n` **must** be multiples of 1024 for correct tile-to-region alignment.

### Tile Generation Pipeline

The `scripts/process_swisstopo_terrain.py` script handles the full pipeline:

```bash
# Install dependencies
pip install rasterio numpy requests tqdm

# Generate tiles covering a large area around the terrain center
python scripts/process_swisstopo_terrain.py \
    --center-e 2682880 --center-n 1283072 \
    --size 17408 --generate-tiles \
    --tile-output provpilot/terrain_tiles

# List required SwissTopo tiles (dry run)
python scripts/process_swisstopo_terrain.py \
    --center-e 2682880 --center-n 1283072 \
    --size 17408 --list-tiles

# Skip download (use cached tiles)
python scripts/process_swisstopo_terrain.py \
    --center-e 2682880 --center-n 1283072 \
    --size 17408 --generate-tiles --skip-download \
    --tile-output provpilot/terrain_tiles
```

The pipeline:
1. Discovers required SwissTopo tiles via the STAC API
2. Downloads GeoTIFF elevation tiles to `terrain_cache/`
3. Merges, crops, and resamples to 1m resolution
4. Applies coordinate flips (see Coordinate Systems below)
5. Splits into 1024×1024 float32 tiles with a `manifest.json`

### Manifest Format

```json
{
  "tile_size": 1024,
  "pixels_per_tile": 1024,
  "origin_e": 2672640,
  "origin_n": 1274880,
  "tiles": {
    "2682880_1283072": {
      "file": "tile_2682880_1283072.raw",
      "ei": 2620,
      "ni": 1253,
      "min_h": 422.5,
      "max_h": 650.3
    }
  }
}
```

## Orthophoto Aerial Imagery

Real aerial imagery from Swisstopo SWISSIMAGE is layered onto the terrain as
Terrain3D color maps, replacing the default auto-shader textures.

### Near Terrain Orthophotos

The near terrain (Terrain3D regions) displays 1m/pixel orthophoto tiles
covering all of Switzerland.

```bash
# Generate orthophoto tiles for all terrain tiles (WMS-based, ~4 tiles/s)
python scripts/generate_orthophoto_tiles.py \
    --terrain-manifest scenery/terrain/manifest.json \
    --tile-dir scenery/orthophoto

# Generate tiles for a specific area only
python scripts/generate_orthophoto_tiles.py \
    --center-e 2682880 --center-n 1283072 --size 20000 \
    --tile-dir scenery/orthophoto
```

| Parameter | Value |
|-----------|-------|
| Source | Swisstopo WMS (`ch.swisstopo.swissimage-product`) |
| Resolution | 1m per pixel |
| Tile size | 1024 × 1024 pixels (matching terrain tiles) |
| Format | JPEG (quality 85) |
| Coverage | All of Switzerland (~41,474 tiles, ~13 GB) |
| Resumable | Yes — skips existing tiles on re-run |

The pipeline fetches one WMS request per output tile, orients the image to match
Terrain3D's coordinate convention (pixel 0,0 = SE corner), detects and skips
empty tiles (outside Swiss coverage), and writes a `manifest.json`.

**Runtime**: `terrain_streamer.gd` loads orthophoto tiles alongside height data,
setting Terrain3D's `show_colormap` mode with alpha=128 (roughness-neutral).
Color maps are preserved across floating-origin rebases.

### Alternative: STAC-Based Pipeline

For smaller areas or higher resolution (0.1m source), use the STAC-based script:

```bash
# Download 0.1m source tiles and resample to 1m output
python scripts/process_swissimage_orthophoto.py \
    --center-e 2682880 --center-n 1283072 --size 5000 \
    --source-resolution 0.1 --tile-dir scenery/orthophoto_hires
```

## Distant Terrain (Low-Detail Horizon)

### Problem

Terrain3D has a hard limit of 32×32 regions. At 1024m per region, this allows a maximum of ~32km × 32km. At flight altitudes (4000+ ft), the terrain edge is clearly visible, especially beyond the `load_radius`.

### Solution: Two-Pronged Approach

1. **Increased load_radius** (8 → 12): Terrain3D now loads 25×25 tiles (~25km²), increasing the high-detail coverage.

2. **Distant terrain mesh skirt**: A separate low-resolution MeshInstance3D covers **all of Switzerland** (350km × 229km) at 128m per vertex. This sits 2m below the Terrain3D surface to avoid z-fighting and provides a visible horizon out to 150+ km.

### Data Pipeline

```bash
# Generate the low-res heightmap from all 42,400 tiles (default: 128m/px)
python scripts/generate_distant_terrain.py --resolution 128

# Replace the elevation-band color map with real WMS aerial imagery
python scripts/generate_distant_orthophoto.py
```

This produces three files in `scenery/terrain/`:

| File | Description |
|------|-------------|
| `distant_terrain.raw` | Float32 heightmap, 2736×1792 pixels at 128m/px (18.7 MB) |
| `distant_terrain_color.png` | WMS orthophoto aerial imagery (5.8 MB) |
| `distant_terrain_meta.json` | Grid dimensions, Swiss coordinate bounds, resolution |

The heightmap generator fills NaN holes (areas without source tiles) using
iterative neighbor-averaging, ensuring a gap-free mesh.

### Runtime Components

- **distant_terrain.gd**: Loads the pre-processed data, generates ArrayMesh chunks (max 250×250 vertices each to stay within Godot's 65536 vertex limit), applies shader material. Supports floating origin rebasing.
- **shaders/distant_terrain.gdshader**: Unshaded spatial shader with near-clip discard (6km), fade-in (6–8km), linear distance fog (80–250km), and exponential atmospheric haze.

### Rendering

| Parameter | Value |
|-----------|-------|
| Resolution | 128m per vertex |
| Coverage | 350 km × 229 km (all Switzerland) |
| Height offset | -2m (below Terrain3D to avoid z-fighting) |
| Near clip | 6 km (hidden behind Terrain3D) |
| Near fade | 6–8 km (smooth transition) |
| Fog start | 80 km |
| Fog end | 250 km |
| Shadows | Disabled (performance) |
| Chunk size | 250×250 vertices |

Empty chunks (outside Swiss borders) are skipped automatically. The mesh follows the floating origin via `rebase_center()`, called from `node_3d.gd` during origin shifts.

## Building System

### Building Conversion

Buildings from SwissTopo swissBUILDINGS3D (GDB format) are converted to GLB:

```bash
python scripts/convert_buildings_to_gltf.py \
    --input buildings_data/ \
    --output provpilot/Assets/buildings/ \
    --origin-e 2682880 --origin-n 1283072
```

The `--origin-e` and `--origin-n` **must match** the terrain center.

### BuildingLoader (building_loader.gd)

The `BuildingLoader` script dynamically loads building GLB files based on aircraft proximity:

| Setting | Value |
|---------|-------|
| **Load Distance** | 8000m |
| **Unload Distance** | 12000m |
| **Max Loads/Frame** | 1 |
| **Roof Color** | Terracotta (0.72, 0.38, 0.28) |
| **Wall Color** | Cream (0.92, 0.87, 0.75) |

Multiple tile sets are defined for different regions:
- `BUILDING_TILES_SCHAFFHAUSEN`: Schaffhausen/Schmerlat area (tile 1031-*)
- `BUILDING_TILES_ZURICH`: Zurich Airport area (tiles 1071-*, 1072-*, 1091-*, 1092-*)

Select the active region via the `terrain_area` export variable.

### Building Origin Offset

If buildings were converted with a different origin than the terrain center, a constant offset is applied:

```gdscript
# Schmerlat buildings were converted with origin E 2681968, N 1282869
# Terrain center is now E 2682880, N 1283072
const BUILDING_ORIGIN_OFFSET: Vector3 = Vector3(912, 0, -203)
```

## Coordinate Systems

### Swiss LV95 to Godot World

The terrain is centered at Swiss LV95 coordinates, converted to Godot with a custom axis mapping:

```
Godot X = TERRAIN_CENTER_E - Swiss_E   (+X = West)
Godot Z = Swiss_N - TERRAIN_CENTER_N   (+Z = North)
Godot Y = Elevation ASL (meters)
```

| Constant | Value | Notes |
|----------|-------|-------|
| `TERRAIN_CENTER_E` | 2,682,880.0 | Swiss Easting at Godot X=0 |
| `TERRAIN_CENTER_N` | 1,283,072.0 | Swiss Northing at Godot Z=0 |

### Terrain Data Orientation

SwissTopo GeoTIFF files have rows North→South and columns West→East. The R16/raw conversion requires **both** array flips:

```python
terrain_data = np.flipud(terrain_data)  # Row 0=North → bottom=North (+Z)
terrain_data = np.fliplr(terrain_data)  # Col 0=West → right=West (+X)
```

**Why**:
- Terrain3D maps pixel(0,0) to world position at the most negative X and Z
- In ProvPilot, most negative X = East, most negative Z = South
- GeoTIFF pixel(0,0) = NW corner → needs to become SE corner

### Godot to WGS84 (Lat/Lon)

The HUD displays real-world coordinates by:
1. Converting Godot position back to Swiss LV95
2. Using SwissTopo approximate formulas to convert LV95 → WGS84

```gdscript
func lv95_to_wgs84(easting: float, northing: float) -> Vector2:
    # Returns Vector2(latitude, longitude) in degrees
```

## Collision Detection Architecture

### Per-Frame Flow

```
┌───────────────────────────────────────────────────────────┐
│                      Per Frame                            │
├───────────────────────────────────────────────────────────┤
│                                                           │
│  1. Get aircraft position (AC.global_position)            │
│                         │                                 │
│                         ▼                                 │
│  2. Query Terrain3D height at XZ position                 │
│     terrain_height = terrain_data.get_height(ac_pos)      │
│                         │                                 │
│                         ▼                                 │
│  3. Update JSBSim terrain elevation (meters → feet)       │
│     jsb_node.set_terrain_elevation(terrain_height)        │
│                         │                                 │
│                         ▼                                 │
│  4. JSBSim runs physics with correct ground height        │
│     - Ground reaction forces                              │
│     - Gear compression                                    │
│     - Altimeter reading                                   │
│                         │                                 │
│                         ▼                                 │
│  5. JSBSim outputs altitude → Godot Y position            │
│     Godot Y = altitude_m + WHEEL_GROUND_CLEARANCE         │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

### Initialization Sequence

1. Scene loads, `_ready()` finds Terrain3D and TerrainStreamer nodes
2. TerrainStreamer performs `initial_load()` — loads tiles around aircraft start position
3. Terrain data becomes available; `_initialize_jsbsim_on_terrain()` is called
4. On first `_process()` frame, `_do_deferred_terrain_init()`:
   - Moves aircraft to start position (Schmerlat Airfield: Godot X=980.8, Z=−227.1)
   - Sets visual heading to 250° (WSW)
   - Queries terrain height at aircraft position
   - Tries `get_intersection()` (raymarching) then physics raycast as fallback
   - Positions aircraft on terrain with gear clearance
   - Calls `initialize_at_terrain()` with terrain elevation, Godot position, and heading
   - Sets `WHEEL_GROUND_CLEARANCE` offset (0.1m)
5. After 60-frame stabilization delay, per-frame terrain updates begin

### JSBSim Integration (jsbgodot.cpp)

Key methods:
- `initialize_at_terrain(elevation_m, godot_pos, heading_deg)` — Sets initial ground elevation, position, and heading before first physics step. Converts Godot XZ to lat/lon and visual heading to JSBSim psi.
- `set_terrain_elevation(elevation_m)` — Updates ground height each frame (converts to feet internally)
- `set_godot_terrain_y_offset(offset_m)` — Small Y offset to prevent wheel clipping
- `reinitialize_from_godot_position(pos, heading)` — Re-init after slew mode

### Heading Convention

JSBSim psi and Godot `rotation.y` relate as:

```
visual_heading = (360 - jsbsim_psi) mod 360
jsbsim_psi     = (360 - visual_heading) mod 360
```

- **Visual heading** is what the pilot sees (0° = North, 90° = East, 250° = WSW)
- **JSBSim psi** is set via `SetPsiDegIC()` and read from `attitude/psi-deg`
- The C++ `get_heading()` method returns the visual heading (after 360−psi conversion)
- GDScript sets `rotation.y = -deg_to_rad(visual_heading)` for the model orientation
- The slew mode HUD computes heading as `fmod(rad_to_deg(-rotation.y) + 360, 360)`

### Floating Origin

Godot uses 32-bit floats for positions, causing visible jitter beyond ~10 km from the world origin. Since Switzerland spans ~350 × 220 km, a **floating origin** system keeps the aircraft near Godot (0, Y, 0) at all times.

#### How It Works

1. **Threshold check** (`_check_origin_rebase` in `node_3d.gd`): Every frame, if the aircraft's |X| or |Z| exceeds `ORIGIN_REBASE_THRESHOLD` (5000 m), a rebase triggers.

2. **Center snap** (`_perform_origin_rebase` in `node_3d.gd`):
   - Computes the aircraft's Swiss LV95 position (invariant)
   - Snaps `TERRAIN_CENTER_E` / `TERRAIN_CENTER_N` to the nearest 1024 m grid point around the aircraft
   - Derives the Godot-space shift vector from the center change

3. **C++ reference shift** (`adjust_origin()` in `jsbgodot.cpp`):
   - Adjusts the internal lat/lon → Godot reference by the **same** Godot-space shift
   - This keeps the C++ flat-earth mapping in sync with the Swiss → Godot terrain mapping (no misalignment from grid snap)

4. **Scene shift** (continues in `_perform_origin_rebase`):
   - Shifts the aircraft node by the shift vector
   - Calls `terrain_streamer.rebase_center()` to clear and reload terrain regions at the new center
   - Calls `building_loader.shift_all_buildings()` to shift loaded buildings and accumulate a cumulative offset

#### Key Functions

| Component | Method | Purpose |
|-----------|--------|---------|
| `jsbgodot.cpp` | `adjust_origin(shift)` | Shift lat/lon reference by Godot-space amount |
| `jsbgodot.cpp` | `adjust_lat_lon_reference()` | Low-level ref shift (file-scope) |
| `node_3d.gd` | `_check_origin_rebase()` | Threshold test each frame |
| `node_3d.gd` | `_perform_origin_rebase()` | Orchestrate the full rebase |
| `terrain_streamer.gd` | `rebase_center(e, n)` | Clear regions, reload at new center |
| `building_loader.gd` | `shift_all_buildings(offset)` | Shift instances + accumulate `_origin_offset` |

#### Design Notes

- **JSBSim is unaffected**: it uses double-precision lat/lon internally and never loses precision.
- **Terrain3D `top_level = true`**: Terrain3D positions regions absolutely in world space, so regions are cleared and reloaded rather than shifted.
- **Building `_origin_offset`**: Because building tile centers are hardcoded constants, a cumulative offset vector is maintained in `BuildingLoader` and applied to (a) distance checks for load/unload decisions, and (b) position of newly instantiated tile scenes.
- **Consistent shift**: The Godot-space shift derived from the terrain center change is applied to **both** the C++ lat/lon reference and the scene objects, eliminating any mismatch between the two coordinate systems.
- **1024 m grid snap**: The new terrain center is snapped to multiples of 1024 to maintain tile-to-region alignment. After snap, the aircraft is at most ~512 m from Godot origin — well within the 5000 m threshold.

## Aircraft Model

### Scale

The Cessna 172 model is in **decimeters** (not meters). Apply scale factor `0.1`:

| Dimension | Raw Model | Scaled (0.1x) | Real C172 |
|-----------|-----------|---------------|-----------|
| Wingspan | 1110 units | 11.1m | 10.97m |
| Length | 799 units | 8.0m | 8.28m |
| Height | 318 units | 3.2m | 2.72m |

### Control Surface Nodes

Control surfaces are individual `MeshInstance3D` nodes within the aircraft hierarchy. Each stores a base transform and applies rotation relative to it each frame, driven by JSBSim input values.

## Configuration Reference

### node_3d.gd Constants

| Variable | Value | Purpose |
|----------|-------|---------|
| `TERRAIN_CENTER_E` | 2,682,880.0 | Swiss Easting at Godot X=0 (var, updated on rebase) |
| `TERRAIN_CENTER_N` | 1,283,072.0 | Swiss Northing at Godot Z=0 (var, updated on rebase) |
| `ORIGIN_REBASE_THRESHOLD` | 5000.0 | Distance triggering floating-origin rebase |
| `HEAD_YAW_MAX` | 120° | Max pilot head look left/right |
| `HEAD_PITCH_MAX` | 60° | Max pilot head look up/down |
| `ORBIT_DISTANCE_MIN` | 1.5m | Closest external camera zoom |
| `ORBIT_DISTANCE_MAX` | 50.0m | Furthest external camera zoom |
| `RUDDER_MAX_ANGLE` | 16° | Rudder deflection limit |
| `AILERON_MAX_ANGLE` | 20° | Aileron deflection limit |
| `ELEVATOR_MAX_ANGLE` | 25° | Elevator deflection limit |
| `FLAP_MAX_ANGLE` | 30° | Maximum flap angle |
| `FRONT_WHEEL_MAX_ANGLE` | 30° | Nose gear steering limit |
| `PROPELLER_BLUR_START_RPM` | 900 | RPM where blur disc appears |
| `PROPELLER_HIDE_BLADES_RPM` | 1200 | RPM where real blades hidden |

### TerrainStreamer Exports

| Variable | Default | Purpose |
|----------|---------|---------|
| `tile_directory` | `user://terrain_tiles` | Path to tile directory |
| `terrain_node_path` | `../Terrain3D` | Terrain3D node |
| `load_radius` | 8 | Tiles to load in each direction |
| `unload_margin` | 2 | Extra tiles before unloading |
| `update_interval` | 30 | Check every N physics frames |
| `load_budget_per_update` | 4 | Max tiles loaded per cycle |
| `terrain_center_e` | 2,682,880 | Must be multiple of 1024 |
| `terrain_center_n` | 1,283,072 | Must be multiple of 1024 |

## Troubleshooting

### Aircraft falls through terrain
- Check that `_update_terrain_elevation()` is being called
- Verify Terrain3D collision mode is set to "Full"
- Check terrain data is loaded (`region_count > 0`)
- Ensure TerrainStreamer completed `initial_load()`

### Wheels clip through ground
- Increase `WHEEL_GROUND_CLEARANCE` in the deferred init (default 0.1m)

### Terrain not loading
- Verify `manifest.json` exists in the tile directory
- Check that terrain center is aligned to 1024m grid
- Look for TerrainStreamer error messages in console
- Ensure `_terrain_data.change_region_size(1024)` is called before adding regions

### Lat/Lon display incorrect
- Verify `TERRAIN_CENTER_E` and `TERRAIN_CENTER_N` match in both `node_3d.gd` and `terrain_streamer.gd`

### Buildings floating or underground
- Ensure building origin coordinates match terrain center
- Check `BUILDING_ORIGIN_OFFSET` if buildings were converted with old origin

## File Locations

| File | Purpose |
|------|---------|
| `provpilot/node_3d.gd` | Main scene script |
| `provpilot/terrain_streamer.gd` | Terrain streaming |
| `provpilot/building_loader.gd` | Building loading |
| `provpilot/terrain_tiles/` | Generated terrain tiles + manifest |
| `provpilot/Assets/` | 3D models (aircraft, buildings) |
| `provpilot/Terra3DData/` | Terrain3D region cache |
| `src/jsbgodot.cpp` / `src/jsbgodot.h` | JSBSim ↔ Godot C++ binding |
| `scripts/process_swisstopo_terrain.py` | Terrain processing pipeline |
| `scripts/convert_buildings_to_gltf.py` | Building GDB → GLB converter |
| `provpilot/distant_terrain.gd` | Low-res distant terrain mesh loader |
| `provpilot/shaders/distant_terrain.gdshader` | Distant terrain fog/haze shader |
| `scripts/generate_distant_terrain.py` | Distant terrain heightmap preprocessor |
| `scripts/generate_distant_orthophoto.py` | Distant terrain WMS color map generator |
| `scripts/generate_orthophoto_tiles.py` | Full-Switzerland WMS orthophoto tiler |
| `scripts/process_swissimage_orthophoto.py` | STAC-based orthophoto pipeline (small areas) |
| `scenery/terrain/distant_terrain.raw` | Low-res float32 heightmap (128m/px) |
| `scenery/terrain/distant_terrain_color.png` | Distant terrain aerial imagery (WMS) |
| `scenery/terrain/distant_terrain_meta.json` | Distant terrain metadata |
| `scenery/orthophoto/` | Near-terrain orthophoto JPEG tiles + manifest |
