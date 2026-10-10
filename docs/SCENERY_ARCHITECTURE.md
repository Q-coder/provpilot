# ProvPilot Scenery Architecture

This document defines the scenery data strategy for ProvPilot — the directory layout, data formats, redistribution plan, and migration path from the current ad-hoc structure.

## Goals

1. **Complete Switzerland coverage** from SwissTopo open data (~42,000 km²)
2. **Redistributable** as downloadable regional packs (not bundled in git)
3. **Extensible** with POI layers, custom scenery, and future data sources
4. **Unified tile grid** — all spatial data aligned to the same 1024m LV95 grid

## Data Sources

| Layer | Source | License | Format |
|-------|--------|---------|--------|
| Terrain elevation | [swissALTI3D](https://www.swisstopo.admin.ch/de/hoehenmodell-swissalti3d) | Open (OGD) | Cloud-Optimized GeoTIFF |
| Lake bathymetry | [swissBATHY3D](https://www.swisstopo.admin.ch/en/height-model-swissbathy3d) | Open (OGD) | ESRI ASCII grid (3 m) |
| Buildings | [swissBUILDINGS3D](https://www.swisstopo.admin.ch/de/gebaudemodell-swissbuildings3d) | Open (OGD) | GDB → GLB |
| Roads & railroads | [swissTLM3D](https://www.swisstopo.admin.ch/de/landschaftsmodell-swisstlm3d) | Open (OGD) | GeoPackage → GLB |
| Water bodies | [swissTLM3D](https://www.swisstopo.admin.ch/de/landschaftsmodell-swisstlm3d) | Open (OGD) | GeoPackage → GLB |
| Airports / POI | swissTLM3D + OSM Overpass | Open | JSON |
| Runways | OSM-verified + Swiss AIP | Various | Procedural (from JSON) |
| Custom scenery | Hand-crafted | Project | GLB + JSON |

### Disk Space Budget

| Data | Per tile | Tile count (CH) | Total |
|------|----------|-----------------|-------|
| Terrain (1m float32) | 4 MB | ~42,000 | **~165 GB** |
| SwissTopo GeoTIFF cache (2m) | ~0.5 MB | 80,485 | ~37 GB (deletable) |
| Buildings (GLB) | varies | ~200 sheets | ~5–10 GB (est.) |
| Roads & railroads (GLB) | ~300 KB avg | ~2,300 tiles | ~670 MB |
| Water bodies (GLB) | ~300 KB avg | ~2,700 tiles | ~840 MB |
| Airports / POI (JSON) | — | — | < 1 MB |
| **Total deployed** | | | **~172–177 GB** |

The 0.5m source GeoTIFFs (~577 GB) are **not** downloaded; the 2m GeoTIFFs (~37 GB) are used as source and can be deleted after processing. Source data remains available at SwissTopo.

## Directory Layout

Scenery data lives **outside** the Godot project directory to keep the git repo small. The default location is configurable; a typical setup:

```
scenery/                              # Root (configurable via project settings)
├── manifest.json                     # Master index — lists available layers
│
├── terrain/                          # Elevation heightmaps
│   ├── manifest.json                 # Tile index (tile key → file + metadata)
│   ├── tile_2682880_1283072.raw      # 1024×1024 float32, little-endian
│   ├── ...                           # ~42,000 files for full CH
│   ├── mip2/                         # 16 m/px downsampled tiles (Ring 0)
│   │   ├── manifest.json
│   │   └── *.raw
│   ├── mip3/                         # 64 m/px downsampled tiles (Ring 1)
│   │   ├── manifest.json
│   │   └── *.raw
│   └── mip4/                         # 256 m/px downsampled tiles (Ring 2)
│       ├── manifest.json
│       └── *.raw
│
├── buildings/                        # 3D building models
│   ├── manifest.json                 # Tile index with centers + bounding boxes
│   ├── 1031-14.glb                   # Per SwissTopo 1:25k sheet quarter
│   ├── 1031-23.glb
│   └── ...
│
├── roads/                            # Road & railroad network
│   ├── manifest.json                 # Tile index with centers
│   ├── roads_2682880_1283072.glb     # Per 4096m tile
│   └── ...
│
├── water/                            # Rivers, lakes, streams
│   ├── manifest.json                 # Tile index with centers
│   ├── water_2682880_1283072.glb     # Per 4096m tile
│   └── ...
│
├── poi/                              # Points of Interest (lightweight JSON)
│   ├── airports.json                 # legacy Swiss catalog (not read at runtime)
│   ├── navaids.json                  # VOR, NDB, waypoints (planned)
│   ├── airspace.json                 # CTR/TMA boundaries (planned)
│   └── landmarks.json                # Peaks, cities, lakes (planned)
│
└── custom/                           # Hand-crafted scenery packages
    ├── schmerlat_airfield/
    │   ├── package.json              # Metadata: name, origin coords, version
    │   ├── hangar.glb
    │   ├── windsock.glb
    │   └── placement.json            # Object positions in Swiss LV95
    └── zurich_kloten/
        ├── package.json
        ├── terminal.glb
        ├── tower.glb
        └── placement.json
```

### Master Manifest (`scenery/manifest.json`)

```json
{
  "version": 2,
  "coordinate_system": "LV95",
  "layers": {
    "terrain": {
      "path": "terrain/",
      "tile_size_m": 1024,
      "resolution_m": 1.0,
      "format": "float32_raw_le",
      "pixels_per_tile": 1024
    },
    "buildings": {
      "path": "buildings/",
      "format": "glb",
      "source": "swissBUILDINGS3D"
    },
    "poi": {
      "path": "poi/",
      "format": "json"
    },
    "orthophoto": {
      "path": "orthophoto/",
      "tile_size_m": 1024,
      "resolution_m": 1.0,
      "format": "jpeg_rgb",
      "pixels_per_tile": 1024,
      "source": "SWISSIMAGE 10cm (ch.swisstopo.swissimage-dop10)"
    },
    "custom": {
      "path": "custom/",
      "format": "package"
    }
  }
}
```

### Terrain Manifest (`scenery/terrain/manifest.json`)

Same format as the current `terrain_tiles/manifest.json` — already well-structured:

```json
{
  "tile_size_m": 1024,
  "pixels_per_tile": 1024,
  "pixel_resolution_m": 1.0,
  "format": "float32_raw_le",
  "bytes_per_tile": 4194304,
  "orientation": {
    "pixel_0_0": "SE corner of Swiss tile",
    "col_direction": "East to West (Godot +X)",
    "row_direction": "South to North (Godot +Z)"
  },
  "tiles": {
    "2682880_1283072": {
      "swiss_e": 2682880,
      "swiss_n": 1283072,
      "tile_ei": 2620,
      "tile_ni": 1253,
      "file": "tile_2682880_1283072.raw",
      "min_elev": 390.2,
      "max_elev": 512.7
    }
  }
}
```

### Lake Bathymetry Overlay (swissBATHY3D)

swissALTI3D models a lake as a flat plate at its regulated water level, so the
terrain layer on its own has no idea how deep a lake is. swissBATHY3D adds the
surveyed lake *bed* (3 m grid, LV95/LN02, same 1 km cell scheme as swissALTI3D)
for 22 Swiss lakes.

`scripts/swissbathy3d.py` downloads a lake from the swisstopo STAC API and
rasterises it into an **overlay region** — ordinary Scenery3D terrain tiles
(`tile_<E>_<N>.raw`, float32 LE, SE-corner origin, 2 m/px) that are *sparse*:
every pixel outside the surveyed bed is 0.0.

No engine change is needed for this. `S3DTileManager` reads every configured
`data_paths` entry for a tile and merges them per pixel, with **later paths
overriding earlier ones wherever they are non-zero**. An overlay region listed
after Switzerland therefore replaces the flat water surface with the lake bed
and falls through to swissALTI3D everywhere else.

```bash
python3 scripts/swissbathy3d.py --list          # 22 lakes swisstopo publishes

python3 scripts/swissbathy3d.py --lakes bodensee \
    --cache-dir /Volumes/Data1/scenery_in_bathy \
    --base-terrain ~/provpilot/scenery/Switzerland/terrain \
                   ~/provpilot/scenery/Germany/Baden-Wuertemberg/terrain \
    --output ~/provpilot/scenery/Switzerland-Bathymetry/terrain
```

Adding more lakes later is the same command with different `--lakes`; the tool
merges into the existing manifest and leaves already-built tiles alone (pass
`--overwrite` to rebuild them).

**Deep lakes need a lower nodata floor.** The default `MIN_VALID_ELEV` of 100 m
is a sane guard against the coarse land buffer, but two lakes have surveyed bed
*below* it — Lac Léman reaches ~62 m and Lago Maggiore ~0 m. With the default
floor their deepest basins are silently dropped (they fall back to the flat
water surface). Rebuild those with `--min-valid-elev 0 --overwrite`:

```bash
python3 scripts/swissbathy3d.py --lakes lacleman lagomaggiore \
    --no-download --overwrite --min-valid-elev 0 \
    --cache-dir /Volumes/Data1/scenery_in_bathy \
    --base-terrain ~/provpilot/scenery/Switzerland/terrain \
    --output ~/provpilot/scenery/Switzerland-Bathymetry/terrain
```

**`--base-terrain` matters.** The swissBATHY3D lake packages ship a land buffer
around the shore taken from a coarser model. Two guards drop it: the overlay may
only carve *downwards*, and it may only touch pixels the base terrain models as
the flat water surface. The water level is auto-detected per lake as the modal
base-terrain height under the surveyed bed (Bodensee → 395.6 m, Rotsee →
419.4 m); override with `--water-level` if a lake ever fools it.

**Activating it in the sim:** Settings → Scenery Library → *Scan for Scenery*.
Newly discovered regions are appended at the end of the priority list, which is
exactly where the overlay has to sit for its pixels to win.

**Repair the water surface afterwards — mandatory.** `generate_water.py` bakes
the large open-water tiles of swissTLM3D at nonsense heights (the middle of the
Bodensee is one 4-vertex quad whose corners sit at 171–330 m instead of
395.6 m). That was invisible for as long as swissALTI3D modelled every lake as a
flat plate: `s3d_water_manager` distrusts a tile whose worst per-triangle Y span
exceeds 50 m and redraped it from the DTM, which reproduced the right surface.
Carve the real bed in and the same redrape drops the water 250 m. Fix the bake
once, offline, against the un-carved terrain:

```bash
python3 scripts/fix_water_bake.py ~/provpilot/scenery/Switzerland/water \
    --terrain ~/provpilot/scenery/Switzerland/terrain \
              ~/provpilot/scenery/Germany/Baden-Wuertemberg/terrain \
    --apply --backup
```

The repair works per connected water body, not per vertex — half-fixing a lake
polygon would leave a cliff between the corrected and the untouched half. For
each mesh island it samples the ground underneath; if that ground is level
(`--min-flat-fraction` of it within `--flat-tol-m`) the island is a lake surface
with one unambiguous height, and the whole island is snapped to the *modal*
ground level plus its freeboard. Using the mode rather than the median keeps
neighbouring tiles on exactly the same level, otherwise a flat lake picks up
faint steps at every tile seam. Anything not sitting on level ground — a stream
down an alpine flank — keeps its bake, because there the DTM is not a better
source. Originals are kept in `<water_dir>/.orig`.

On the Swiss set this repairs 0.5 % of water vertices, drops the tiles carrying
a corrupt triangle from 395 to 303, and leaves the Bodensee as one continuous
surface at 396.6 m with 0.06 % of its area still defective (was: 42 % of tiles
sinking to the lake bed).

**Side effect:** `Scenery3D.get_elevation()` — which feeds JSBSim's
`position/terrain-elevation-asl-ft` — now returns the lake bed over covered
lakes instead of the water surface. There is no water physics, so an aircraft
ditching in the Bodensee falls through the water surface down to the bed.

### Water Rendering

`S3DWaterManager` shades the surface with a depth-aware shader
(`depth_shading`, on by default): it reads the opaque depth buffer, measures the
water column between the surface and the terrain behind it, and ramps colour
from `shallow_color` to `deep_color` and opacity from `shore_alpha` to
`deep_alpha` over `opaque_depth_m` (16 m). With a bathymetry overlay underneath
this turns a flat blue plate into a readable depth map.

It also keeps the orthophoto honest. The aerial image under a lake is a photo of
the *water surface*, so letting it show through a fully transparent lake would
drape a picture of water over a 250 m basin. Because opacity is driven by depth,
the ortho is only visible in genuinely shallow water near the shore — where it
really is a photo of a shallow bottom — and deep water is opaque.

Two further water settings, both defaulting on:

- `dedup_overlapping_tiles` — every water source shares one absolute 4096 m LV95
  grid, so the same tile id in two manifests means the same ground is modelled
  twice (the Bodensee is a complete lake in both swissTLM3D and the BW OSM set).
  The earlier path in the Scenery Library wins.
- `max_triangle_span_m` (50 m) — a baked triangle taller than this is a bake bug
  ("dark wing") and is dropped. Only the offending triangles go; the rest of the
  tile keeps its correct baked elevation.

### Building Manifest (`scenery/buildings/manifest.json`)
```json
{
  "format": "glb",
  "source": "swissBUILDINGS3D",
  "conversion_origin_e": 2682880,
  "conversion_origin_n": 1283072,
  "tiles": {
    "1031-14": {
      "file": "1031-14.glb",
      "center_e": 2679977,
      "center_n": 1285703,
      "bbox_e_min": 2678400,
      "bbox_e_max": 2681600,
      "bbox_n_min": 1284300,
      "bbox_n_max": 1287100
    }
  }
}
```

The `conversion_origin_e/n` fields record the Swiss LV95 origin used when converting buildings to GLB. The runtime building loader uses these to compute correct Godot-space positions without hardcoded offsets. The loader reads this manifest at startup — no hardcoded tile arrays or region selection. `scenery_pipeline.py` and `convert_buildings_to_gltf.py` write the manifest together with the GLBs.

### Road Manifest (`scenery/roads/manifest.json`)

```json
{
  "format": "glb",
  "source": "swissTLM3D",
  "tile_size_m": 4096,
  "feature_types": ["motorway", "trunk", "primary", "secondary", "tertiary",
                     "residential", "track", "path", "railway"],
  "tiles": {
    "2682880_1283072": {
      "file": "roads_2682880_1283072.glb",
      "center_e": 2684928,
      "center_n": 1285120
    }
  }
}
```

Roads are color-coded by type: motorways (dark gray), primary roads (lighter gray), railways (brown), paths (sandy). The loader uses the same proximity-based loading as buildings.

### Water Manifest (`scenery/water/manifest.json`)

```json
{
  "format": "glb",
  "source": "swissTLM3D",
  "tile_size_m": 4096,
  "feature_types": ["lake", "river", "stream", "canal"],
  "tiles": {
    "2682880_1283072": {
      "file": "water_2682880_1283072.glb",
      "center_e": 2684928,
      "center_n": 1285120
    }
  }
}
```

Water bodies are rendered as blue surfaces. Lakes are filled polygons; rivers and streams are width-proportional ribbons.

### Airport Database (`scenery/poi/airports.json`)

**Legacy.** This Swiss-only catalog (swissTLM3D + OSM, LV95) is no longer read
by the runtime and its generator scripts were retired. Airports now come from
`provpilot/airports.json` (OurAirports, `scripts/build_world_airports.py`)
and the per-pack `airports/runways.json` (`scripts/build_world_runways.py`),
see `docs/WORLD_SCENERY_DESIGN.md` §5 and §12. The file format is kept here
for reference:

```json
{
  "type": "airports",
  "coordinate_system": "LV95",
  "source": "OSM Overpass + Swiss AIP, verified manually",
  "airports": [
    {
      "icao": "LSZB",
      "name": "Bern-Belp",
      "type": "regional_airport",
      "elevation_m": 510,
      "center_e": 2604527,
      "center_n": 1195809,
      "area_ha": 140.0,
      "runways": [
        {
          "designator": "14/32",
          "heading_deg": 140,
          "length_m": 1780,
          "width_m": 30,
          "surface": "asphalt",
          "threshold_14": { "e": 2603956, "n": 1196491 },
          "threshold_32": { "e": 2605098, "n": 1195127 }
        }
      ]
    }
  ]
}
```

Airport types: `international_airport`, `regional_airport`, `grass_strip`, `heliport`.
Runway threshold coordinates are in Swiss LV95 (EPSG:2056), verified against OSM Overpass data.
Heliports have a `pad` object instead of `runways`.

### Runway Rendering

Runways are **procedurally generated** by `runway_loader.gd` — no GLB files needed. The loader:
- Reads threshold positions from `airports.json`
- Builds terrain-following segmented meshes (~50m segments) using `SurfaceTool`
- Adds centerline dashes and threshold bar markings
- Applies material by surface type (concrete gray for asphalt, muted green for grass)
- Loads/unloads based on aircraft proximity (25 km load, 30 km unload)
```

### Object packs (`<area>/objects/package.json`)

A scenery library area may carry hand-placed models next to (or instead of)
its terrain, buildings and other assets: an `objects/` folder with GLB files
and a `package.json`. `scripts/convert_msfs_scenery.py` writes this layout
from an MSFS package; a file written by hand uses the same fields:

```json
{
  "schema_version": 2,
  "name": "lspf-schmerlat",
  "objects": [
    {"model": "schmerlat_hauptgebaeude.glb", "name": "schmerlat_hauptgebaeude",
     "lat": 47.689854, "lon": 8.52757,          "position_e": 2681754.3, "position_n": 1282704.6,
     "agl_m": 2.35, "elevation_m": null, "heading_deg": 338.8, "scale": 0.5,
     "footprint_radius_m": 42.4}
  ],
  "exclusions": [
    {"kinds": "buildings,vegetation,objects", "circle": [2681754.3, 1282704.6, 45.4]},
    {"kinds": "buildings", "polygon": [[2681500, 1282600], [2681900, 1282600], [2681900, 1282800]]}
  ]
}
```

- `lat`/`lon` (WGS84) or `position_e`/`position_n` (LV95) place the model's
  origin; `agl_m` puts it that far above the precise terrain, `elevation_m`
  (when not null) is absolute. `heading_deg` is true, clockwise; the model's
  glTF `+Z` axis faces it (verified against the orthophoto with
  `tests/probe_scenery_objects.tscn`), `+Y` is up. Optional `pitch_deg` and
  `bank_deg` tilt it (rotation = heading, then pitch about the model's X,
  then bank about its Z). `scale` is a number (uniform) or `[x, y, z]`.
  An optional `pivot` `[x, y, z]` (model space) is the point that sits at the
  placement position instead of the model's origin.
- `exclusions` remove content of **lower-priority** areas (the ones further
  down the Scenery Library) inside a circle (`[e, n, r]`, LV95 metres) or a
  polygon: `buildings` (swissBUILDINGS3D tiles), `vegetation` (trees) and
  `objects` (models of lower packs). Areas above the pack are never touched,
  so the highest entry in the library wins. The converter writes one circle
  per placed model (its footprint plus 3 m) unless the footprint radius
  exceeds `--exclusion-max-radius` (120 m): such a model is a sparse detail
  set spread over a whole airport, not a building, and a disc around it
  would wipe out every lower-tier building for kilometres (FSDT Zurich's
  "ED" model, 2.4 km, took the whole town of Kloten with it). Those objects
  get `exclusion_radius_m: 0`, which the sim honours when it rewrites the
  pack (any positive value is used as the disc radius); an airfield
  perimeter is best added by hand as a polygon.

At runtime the C++ `SceneryObjects` node (`src/scenery_objects.cpp`) reads
every enabled area's package in priority order, hands the zones to
`S3DBuildingManager` and `S3DVegetationManager` (`set_exclusion_zones`) and
loads each model within 8 km of the camera on demand (freed beyond 10 km).
The building manager drops an excluded building from both the detail and the
far-LOD merged mesh; the vegetation manager drops the tree instances. The
scenery editor's Object tool (`docs/SCENERY_EDITOR.md`) moves, turns, scales,
drops and removes these models and saves the pack's package.json back.

## Regional Packs for Redistribution

Instead of requiring a 165 GB download, scenery is split into regional packs that can be downloaded independently:

| Pack | Region | Approx Size |
|------|--------|-------------|
| `ch-northeast` | Zurich, Schaffhausen, Thurgau, St. Gallen | ~20 GB |
| `ch-central` | Bern, Luzern, Aargau, Solothurn | ~25 GB |
| `ch-northwest` | Basel, Jura | ~15 GB |
| `ch-southeast` | Graubünden, Glarus | ~30 GB |
| `ch-southwest` | Valais, Vaud | ~30 GB |
| `ch-south` | Ticino | ~15 GB |
| `ch-west` | Genève, Fribourg, Neuchâtel | ~15 GB |
| `ch-full` | Everything | ~165 GB |

Each pack is a tar/zip containing terrain tiles + building GLBs for that region. The processing pipeline generates packs by filtering tiles within the regional bounding box.

## Godot Integration

### Configuration

The scenery root path is set via Godot project settings or an export variable:

```gdscript
# node_3d.gd or a new SceneryManager node
@export var scenery_path: String = "user://scenery"
```

For development, this can point to a local directory. For distribution, `user://scenery` maps to the platform-specific user data directory.

### Runtime Components

All runtime streaming is handled by the [Scenery3D](../provpilot/addons/scenery3d/README.md)
GDExtension. The Godot project contains only glue scripts.

| Component | Role |
|-----------|------|
| `Scenery3D` (GDExtension node) | Single node in `node_3d.tscn`; owns terrain, building, road, water, and orthophoto streaming |
| `node_3d.gd` | Main scene script; wires the C++ modules and sequences welcome → Fly! → flight. Terrain queries go through the C++ `WorldScenery` (`src/world_scenery.cpp`: precise patch first, globe fallback, runway lift) |
| `WelcomeScreen` (GDExtension, `src/welcome_screen.cpp`) | Startup aircraft & airport selection UI (currently seeds one hardcoded LSPF entry) |
| `SettingsScreen` / `SettingsStore` (GDExtension, `src/settings_*.cpp`) | Tabbed settings UI + settings model; owns the scenery library and pushes paths into `Scenery3D` |
| `LoadingScreen` (GDExtension, `src/loading_screen.cpp`) | Progress overlay, the fallback loading view when the intro zoom cannot play; `node_3d.gd::_update_loading_progress()` drives it from `WorldScenery.count_loaded_ring()` (a 5-point ring around the spawn position) so the bar reflects how many near-terrain tiles have actually streamed in |
| `custom_scenery_loader.gd` | Scans `custom/*/package.json`, discovers packages by type |
| `object_loader.gd` | Loads custom scenery objects by proximity, samples elevation via Scenery3D |

### Performance

All heavy I/O and meshing happens inside the Scenery3D extension on a
background thread pool (4 workers). The main thread runs budgeted work only:

- `VERTEX_BUDGET_PER_FRAME = 200000` — mesh build cost.
- `ORTHO_DECODES_PER_FRAME = 3` — tile results carrying a JPEG ortho are
  deferred once the per-frame quota is hit.
- `CHUNKS_PER_FRAME = 1` — each far chunk decodes up to `chunk_size²` mip3
  JPEGs and builds a composite texture, so one per frame is plenty.

These caps spread the JPEG decode + texture upload work across many frames so
the simulation no longer stalls when the camera crosses into a fresh 8×8 far
chunk. The trade-off is that the very first load after `Fly!` takes several
seconds — `node_3d.gd` shows this on the intro zoom's HUD (status + progress)
instead of freezing silently.

A far chunk is dropped only after every individual tile in its footprint that
lies within `load_radius` has finished loading (or is marked `no_data`),
preventing the world-gap that used to appear briefly when the camera moved
into a new chunk.

Because the project runs on `precision=double` Godot, **no floating-origin
rebasing is needed** — world coordinates stay numerically stable across all of
Switzerland, so there is no rebase stall or reload step.

LOD details (close/mid/far/horizon rings plus 8×8 far chunks out to 200 km)
are documented in `addons/scenery3d/README.md`.

## Processing Pipeline

### Unified Pipeline (Recommended)

The `scenery_pipeline.py` script processes both terrain and buildings in a single command with chunk-based processing to limit disk usage:

```bash
# Process terrain + buildings for a region
python scripts/scenery_pipeline.py \
    --center-e 2682880 --center-n 1283072 --radius 20000 \
    --output scenery/

# Terrain only
python scripts/scenery_pipeline.py --terrain-only \
    --center-e 2682880 --center-n 1283072 --radius 20000

# Buildings only
python scripts/scenery_pipeline.py --buildings-only \
    --center-e 2682880 --center-n 1283072 --radius 20000

# Dry run (list what would be downloaded)
python scripts/scenery_pipeline.py --dry-run \
    --center-e 2682880 --center-n 1283072 --radius 20000
```

The unified pipeline is incremental (skips existing tiles) and supports `--clean-cache` to delete intermediate files.

### Individual Pipelines

#### Terrain

```bash
# Generate tiles for all of Switzerland
python scripts/process_swisstopo_terrain.py \
    --full-switzerland \
    --output scenery/terrain/ \
    --source-resolution 2  # Use 2m source (37 GB download vs 577 GB)
```

#### Buildings

```bash
# Convert buildings for all of Switzerland
python scripts/convert_buildings_to_gltf.py \
    --full-switzerland \
    --output scenery/buildings/
```

### Roads & Railroads

```bash
# Generate road/railroad tiles from swissTLM3D GeoPackage
python scripts/generate_roads.py \
    --gpkg /path/to/swissTLM3D.gpkg \
    --output scenery/roads/
```

### Water Bodies

```bash
# Generate river/lake tiles from swissTLM3D GeoPackage
python scripts/generate_water.py \
    --gpkg /path/to/swissTLM3D.gpkg \
    --output scenery/water/
```

### Airports and Runways

```bash
# OurAirports catalog for a region → provpilot/airports.json
python scripts/build_world_airports.py --region switzerland-base

# Runway / taxiway / apron geometry of the region's pack → airports/runways.json
python scripts/build_world_runways.py --region switzerland-base
```

### MSFS Scenery Conversion

Microsoft Flight Simulator 2020 scenery packages can be converted to Godot-compatible GLB:

```bash
python3 scripts/convert_msfs_scenery.py /path/to/msfs/package \
    --output scenery/custom/my_scenery/ [--max-texture 1024]
```

What the package holds and how it is read (verified on the Schmerlat package,
identical to the ModelConverterX export to the millimetre):

- `scenery/global/scenery/modelLib.BGL` is a list of RIFF `GLTF` forms, each
  with an XML `ModelInfo` (GUID, name, LODs) and the glTF binary of the model.
- The glTF uses Asobo's optimized vertex format: one interleaved buffer
  (stride 36: POSITION float3, TANGENT and NORMAL as signed bytes, TEXCOORD_0/1
  and COLOR_0 as half floats) and one index accessor per mesh that every
  primitive slices with `extras.ASOBO_primitive {StartIndex, PrimitiveCount}`
  (plus `BaseVertexIndex` once a mesh passes 65535 vertices: the indices stay
  16-bit and each primitive offsets them). The converter rewrites each mesh
  with float32 attributes compacted to the vertices in use and one index
  accessor per primitive.
- MSFS front faces are clockwise (DirectX, like its normal maps): 59 of the
  60 Schmerlat and Kjeller models have every triangle wound against its
  vertex normals. glTF front faces are counter-clockwise, so the converter
  rewinds each triangle to agree with its vertex normals (2026-10-10). The
  Schmerlat models never showed the problem because their materials are
  double-sided; Kjeller's single-sided McDonald's and Circle K lost their
  roofs and walls and showed their lettering mirrored from inside.
- MSFS has no non-uniform node scale: a node scale `(sx, sy, sz)` is applied
  as the uniform mean of the three. The converter does the same
  (`--keep-nonuniform-scale` keeps the literal values).
- Textures are DDS (BC1, BC3, BC5 signed normal maps, occasionally BC7) next
  to the models in `texture/`; `scripts/msfs_dds.py` decodes them without
  Pillow. Normal maps are flipped from the DirectX to the OpenGL convention.
  Light nodes (`ASOBO_macro_light`) survive as empty nodes with their
  parameters in `extras.msfs_light`.
- `scenery/world/scenery/*_scn.bgl` holds the placements (SceneryObject
  section, LibraryObject records: lat/lon, height above ground (signed: a
  sunk object is negative), heading, scale, model GUID). They go to `package.json`; objects from other libraries
  (Asobo's default vegetation, vehicles) are listed by GUID only.

## Migration History

The runtime was migrated from a Terrain3D + per-loader architecture
(`terrain_streamer.gd`, `building_loader.gd`, `road_loader.gd`, `water_loader.gd`,
`runway_loader.gd`, `poi_loader.gd`, `terrain_clipmap.gd`, `clipmap_streamer.gd`,
`position_dialog.gd`) to a single Scenery3D GDExtension in April 2026. The
on-disk data layout above was preserved; only the consumer changed. The old
GDScript loaders have been removed from the project.

## Future Considerations

- **Texture tiles**: Ortho imagery (swissimage) for ground textures — same 1024m grid, ~4 MB/tile JPEG
- **Vegetation**: Tree placement from swissTLM3D
- **Navaids**: VOR, NDB, waypoints from Swiss AIP for instrument navigation
- **Airspace**: CTR/TMA boundaries for airspace awareness
- **Compression**: Consider zstd-compressed terrain tiles to reduce from 4 MB → ~1–2 MB per tile (~80 GB total)
- **LOD**: Multi-resolution terrain (1m near aircraft, 4m distant) to reduce memory — partially addressed by clipmap mip pyramid
- **Clipmap streaming**: `clipmap_streamer.gd` centralises texture ownership and rebuilds on rebase (<1 ms). Toroidal strip-blit infrastructure retained for future use when textures are shrunk to save VRAM
- **Streaming from network**: Load tiles from a local server or CDN instead of requiring full local copy
