# Terrain Setup (Scenery3D)

ProvPilot uses the [Scenery3D](https://github.com/Q-coder/scenery3d) GDExtension
for all world streaming — terrain, buildings, roads, water, and orthophotos.
Scenery3D replaced the Terrain3D + per-loader architecture in April 2026.

## Overview

A single `Scenery3D` node in `provpilot/node_3d.tscn` handles everything the
old stack used to do:

- Terrain heightmap streaming with distance-based LOD rings
- Orthophoto tile loading
- Building GLB loading with detail/far-LOD handoff
- Road & railway ribbon loading
- Water (rivers, lakes, streams) loading
- Far terrain chunks (200 km+ visibility)
- O(1) elevation queries for JSBSim ground reactions

No `TerrainStreamer`, `BuildingLoader`, `RoadLoader`, `WaterLoader`,
`ClipmapStreamer`, or floating-origin rebase logic remains in the Godot project.
All of that now lives in the C++ extension and runs on background threads.

## Precision

The project runs on a **double-precision Godot fork** (branch `4.6-macdev`).
All GDExtensions loaded by the editor must also be built with `precision=double`.
Floating-origin rebasing is no longer necessary — world coordinates stay large
and stable across all of Switzerland.

## Configuration

`project.godot` has a `[scenery3d]` section with the data paths and world origin:

```ini
[scenery3d]

data_path="../scenery/terrain"
buildings_path="../scenery/buildings"
water_paths=PackedStringArray("../scenery/water")
road_paths=PackedStringArray("../scenery/roads")
orthophoto_path="../scenery/orthophoto"
origin_east=2682880.0
origin_north=1283072.0
```

The origin must be a multiple of 1024 m in LV95 — the same grid the tile files
are aligned to.

## Coordinate System

The tile **data** grid is LV95 (tile files are named by their LV95 corner).
Since step C (design §9.5) the Godot **world** is WGS84 ECEF and Scenery3D
runs in `frame_mode = 1`: each tile is placed with its local West-Up-North
frame at its centre on the ellipsoid, so tile meshes stay in their flat
tile-local layout:

```
tile-local X = tile_east_edge − LV95_East   (+X = West)
tile-local Z = LV95_North − tile_south_edge  (+Z = North)
tile-local Y = Elevation (metres ASL, treated as ellipsoidal)
```

`origin_east/north` (E=2,682,880 / N=1,283,072, Schaffhausen) only matters in
the legacy flat mode (`frame_mode = 0`, the scenery3d demo) and as ProVPilot's
home point before the first flight.

## Elevation Queries

The C++ `WorldScenery` module (`src/world_scenery.cpp`) asks Scenery3D for
terrain height with `get_elevation_geodetic(lat, lon)`, which returns `NaN`
(or 0.0, treated as no data) outside loaded tiles. `terrain_elevation_geodetic`
falls back to the world3d globe there; JSBSim's ground query
(`ground_height_geodetic`) uses the precise value plus the drawn runway lift.

## Tile Data

Tile formats, naming, and the LOD ring layout are documented in the Scenery3D
repository: [addons/scenery3d/README.md](../provpilot/addons/scenery3d/README.md).

Summary:
- Terrain: `tile_{lv95_east}_{lv95_north}.raw` — 1024×1024 float32 little-endian,
  SE-corner origin. Mip pyramids in `mip2/`, `mip3/`, `mip4/`.
- Orthophoto: `ortho_{lv95_east}_{lv95_north}.jpg` with matching mip subdirs.
- Buildings: `buildings_{tile_id}.glb` with per-building + far-LOD primitives.
- Water / roads: `water_*.glb`, `roads_*.glb` with vertex-colored meshes.

Each layer directory contains its own `manifest.json`.

## Pipeline

The Python scripts under `scripts/` still produce the tile pyramid the same way
they did before Scenery3D — the on-disk format hasn't changed, only what reads
it at runtime.

See [SCENERY_ARCHITECTURE.md](SCENERY_ARCHITECTURE.md) for the full data layout
and the individual script docstrings for usage.
