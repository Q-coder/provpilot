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
| **Load Radius** | 8 tiles (configurable) |
| **Loaded Area** | 17 × 17 tiles = ~17km × 17km |
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
   - Queries terrain height at aircraft position
   - Tries `get_intersection()` (raymarching) then physics raycast as fallback
   - Positions aircraft on terrain with gear clearance
   - Initializes JSBSim with correct ASL elevation
   - Sets `WHEEL_GROUND_CLEARANCE` offset (0.1m)
5. After 60-frame stabilization delay, per-frame terrain updates begin

### JSBSim Integration (jsbgodot.cpp)

Key methods:
- `initialize_at_terrain(elevation_m)` — Sets initial ground elevation before first physics step
- `set_terrain_elevation(elevation_m)` — Updates ground height each frame (converts to feet internally)
- `set_godot_terrain_y_offset(offset_m)` — Small Y offset to prevent wheel clipping
- `reinitialize_from_godot_position(pos, heading)` — Re-init after slew mode

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
| `TERRAIN_CENTER_E` | 2,682,880.0 | Swiss Easting at Godot X=0 |
| `TERRAIN_CENTER_N` | 1,283,072.0 | Swiss Northing at Godot Z=0 |
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
