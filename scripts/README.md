# scripts/

Python tooling of ProVPilot. Everything runs from the repository root
(`python3 scripts/<name>.py --help`). The runtime never calls these; they
produce the data the game and the scenery packs ship.

## Worldwide base tier (`docs/WORLD_SCENERY_DESIGN.md`)

| Script | Purpose |
|---|---|
| `build_world_terrain.py` | SRTM → LV95 heightmap tiles per region: `list`, `plan`, `fetch`, `bake` (`--bbox`, `--base-only`), `flatten` (`--bbox`, `--base-only`), `verify`, `prune`, `build`. Writes `terrain/` and the pristine `terrain_base/` that `build_world_roads.py --corridor` derives `terrain/` from |
| `runway_plateau.py` | Runway plateaus written into the baked terrain (used by `build_world_terrain.py flatten`) |
| `build_world_airports.py` | OurAirports catalog → `provpilot/airports.json` |
| `build_world_runways.py` | OSM + OurAirports runway / taxiway / apron geometry → `<pack>/airports/runways.json` |
| `build_world_roads.py` | OSM (or swissTLM3D) roads and railways → road tiles; `--design` folds in the scenery editor's design; `--terrain-overlay DIR` samples a finer drawn terrain (the Swiss 2 m scenery) first; width changes between ways taper |
| `build_world_strip.sh`, `build_world_lights.sh` | Batch drivers for several regions |
| `world_regions.json` | Region definitions (countries, bboxes, build paths) |

## Swiss detail tier (`docs/SCENERY_ARCHITECTURE.md`)

| Script | Purpose |
|---|---|
| `scenery_pipeline.py` | swissALTI3D terrain + swissBUILDINGS3D buildings, chunked, incremental |
| `streaming_terrain_generator.py` | swissALTI3D → 2 m/px heightmap tiles, one output tile at a time (no country-sized mosaic) |
| `process_swisstopo_terrain.py` | Older mosaic-based terrain pipeline (1 m/px); the reference for the tile naming and pixel orientation |
| `swissbathy3d.py` | swissBATHY3D lake beds → overlay heightmap tiles |
| `convert_buildings_to_gltf.py` | swissBUILDINGS3D GDB → GLB tiles + manifest |
| `generate_orthophoto_tiles.py` | Orthophoto JPEG tiles through the swisstopo WMS |
| `process_swissimage_orthophoto.py` | Orthophoto tiles from SWISSIMAGE STAC downloads (small areas) |
| `generate_terrain_mipmap.py` | Terrain and orthophoto mip pyramids for the far LOD rings |
| `generate_roads.py` | swissTLM3D / swissTLMRegio roads and railways → detail GLB tiles |
| `generate_water.py`, `fix_water_bake.py` | swissTLM3D lakes and rivers → water GLB tiles, and the height repair of baked tiles |
| `generate_vfr_map_tiles.py` | Hillshade + hypsometric VFR map raster tiles (`docs/VFR_MAP.md`) |

## Distribution (`docs/SCENERY_DISTRIBUTION.md`)

| Script | Purpose |
|---|---|
| `bundle_scenery_pack.py` | Base / detail pack ZIPs per region or quadrant (`scenery_regions.json`) |
| `generate_scenery_manifest.py` | `scenery_manifest.json` for the in-app downloader |
| `convert_msfs_scenery.py` | MSFS 2020 scenery package (modelLib BGL + placements) → one GLB per model and `package.json`; format notes in `docs/SCENERY_ARCHITECTURE.md` |
| `msfs_dds.py` | DDS decoder (BC1/2/3/4/5) used by the converter; also a CLI to turn a DDS into a PNG |
| `../provpilot/tests/render_glb.gd` | Render a GLB to a PNG with Godot for a visual check: `godot --path provpilot -s tests/render_glb.gd -- model.glb out.png` |

## Aircraft

| Script | Purpose |
|---|---|
| `fg_a10_build.py` | FlightGear A-10 package → `aircraft/a10/model` (runs inside Blender) |
| `fg_panel_build.py` | FlightGear instruments, light lenses and nose gear on a Blender model (runs inside Blender) |
| `fg_a10_sounds.py` | FlightGear sound XML → `sounds.json` + wavs |
| `glb_tool.py` | `pack` a multi-file glTF into one GLB, `inspect` and `validate` GLBs |
| `make_loop.py` | Seamless engine sound loop (MP3 → OGG) |

## Build and infrastructure

| Script | Purpose |
|---|---|
| `build_macos.sh`, `build_windows.ps1`, `setup_windows.ps1` | Build, sign and assemble the app |
| `pck_list.py` | List the entries of a Godot `.pck` |
| `llm_delegate.py` | Hand a subtask to the remote model (`docs/AGENTS_TWO_MACHINE.md`); reads `.llm_remote.env` |

`.llm_remote.env` holds the remote model credentials and is gitignored (as is `.copernicus.env`, kept for the Copernicus DEM download planned in `docs/WORLD_SCENERY_DESIGN.md`).
