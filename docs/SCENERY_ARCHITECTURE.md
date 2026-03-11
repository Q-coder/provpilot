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
│   ├── airports.json                 # 47 Swiss airports with runway geometry
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

The `conversion_origin_e/n` fields record the Swiss LV95 origin used when converting buildings to GLB. The runtime building loader uses these to compute correct Godot-space positions without hardcoded offsets. The loader reads this manifest at startup — no hardcoded tile arrays or region selection.

Generate the manifest from existing GLB files:

```bash
python scripts/generate_building_manifest.py
```

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

### Custom Scenery Package (`scenery/custom/*/package.json`)

```json
{
  "name": "Schmerlat Airfield",
  "version": "1.0.0",
  "author": "ProvPilot",
  "origin_e": 2681968,
  "origin_n": 1282869,
  "objects": [
    {
      "model": "hangar.glb",
      "position_e": 2681950,
      "position_n": 1282880,
      "elevation_m": 464.0,
      "heading_deg": 70.0,
      "scale": 1.0
    },
    {
      "model": "windsock.glb",
      "position_e": 2682010,
      "position_n": 1282850,
      "elevation_m": 464.5,
      "heading_deg": 0.0,
      "scale": 1.0
    }
  ]
}
```

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

### Loader Changes

| Component | Status | Description |
|-----------|--------|-------------|
| `terrain_streamer.gd` | ✅ Done | Reads `scenery/terrain/manifest.json`, streams 1024m tiles + orthophoto |
| `building_loader.gd` | ✅ Done | Reads `scenery/buildings/manifest.json`, manifest-driven proximity loading |
| `road_loader.gd` | ✅ Done | Reads `scenery/roads/manifest.json`, proximity loading |
| `water_loader.gd` | ✅ Done | Reads `scenery/water/manifest.json`, proximity loading |
| `poi_loader.gd` | ✅ Done | Loads `scenery/poi/airports.json` at startup |
| `runway_loader.gd` | ✅ Done | Procedural terrain-following runway meshes from airport data |
| `position_dialog.gd` | ✅ Done | In-flight airport search/filter/teleport UI |
| `welcome_screen.gd` | ✅ Done | Startup aircraft & airport selection UI |
| `settings_screen.gd` | ✅ Done | Settings UI (graphics, sound, realism, paths) |
| `loading_screen.gd` | ✅ Done | Progress overlay during loading |
| `distant_terrain.gd` | ⚠️ Deprecated | Low-res mesh horizon (256m/vertex) — replaced by clipmap |
| `terrain_clipmap.gd` | ✅ Done | 3-ring geomorphing clipmap (7 km – 300 km), seam-blended with Terrain3D |
| `custom_scenery_loader.gd` | ✅ Done | Scans `custom/*/package.json`, discovers packages by type |
| `object_loader.gd` | ✅ Done | Loads custom scenery objects by proximity (15 km / 20 km) |

### Performance: Progressive Loading & Floating-Origin Rebase

All scenery loading is designed to avoid blocking the main thread:

**Terrain streaming** (`terrain_streamer.gd`):
- **Initial load**: Loads 25 tiles immediately (5×5 ring under the aircraft for ground collision), then queues remaining ~600 tiles for progressive loading at 8 tiles/frame.
- **Floating-origin rebase**: Uses **in-memory region recycling** — extracts heightmap Images from existing Terrain3D regions before clearing, then re-adds them at new grid positions without disk I/O. Only genuinely new border tiles (~25 per 1024m shift) require file reads, loaded progressively.
- Normal streaming loads/unloads tiles at the boundary every 30 physics frames with a budget of 4 tiles per cycle.

**Building / Road / Water loaders**:
- Use `ResourceLoader.load_threaded_request()` for async GLB loading (1–2 tiles per frame budget).
- Proximity checks run every `_process()` frame but are lightweight (distance calculations only).
- Floating-origin rebase shifts all loaded instances by the offset vector — no reload needed.

**Distant terrain** (`distant_terrain.gd`) — **DEPRECATED**:
- Replaced by the clipmap terrain system (see below).
- Pre-generated 256m-resolution heightmap covering all of Switzerland.
- Kept as fallback; processing disabled at startup.

**Clipmap terrain** (`terrain_clipmap.gd`):
- 3 concentric ring meshes beyond Terrain3D's ~8.5 km edge:
  - Ring 0: 7–16.4 km, mip2 (16 m/px), 2048×2048 heightmap
  - Ring 1: 16.4–65.5 km, mip3 (64 m/px), 2048×2048 heightmap
  - Ring 2: 65.5–262 km, mip4 (256 m/px), 2048×2048 heightmap
- Shader-based geomorphing: each ring's outer vertices blend toward the next-outer ring's heightmap, eliminating LOD popping.
- **Terrain3D ↔ Ring 0 seam blending**: Ring 0 carries an extra heightmap built from the same 1 m tiles Terrain3D uses (Lanczos downsample → 16 m/px). In a 2 km transition band (8.5–10.5 km) the shader blends seam heights toward mip2 data. A depth bias (up to 5 m at the inner edge) ensures Terrain3D wins the depth test in the overlap zone. Fragment colour fades to atmospheric haze at the inner boundary so the seam is invisible.
- Heightmap textures are rebuilt on floating-origin rebase (>1024 m shift).
- Mip tile pyramids are pre-generated offline by `scripts/generate_terrain_mipmap.py`.

**Runway loader** (`runway_loader.gd`):
- Procedural mesh generation is fast (SurfaceTool); no threaded loading needed.
- Load/unload by proximity (25 km / 30 km).

### Building Loader Simplification

The current `building_loader.gd` has:
- Hardcoded `BUILDING_TILES_ZURICH` and `BUILDING_TILES_SCHAFFHAUSEN` arrays
- A `BUILDING_ORIGIN_OFFSET` constant for coordinate mismatch
- A `TerrainArea` enum to switch regions manually

After migration:
- Reads `buildings/manifest.json` at startup
- Centers are in absolute Swiss LV95 → converted to Godot coords using current `TERRAIN_CENTER_E/N`
- No hardcoded arrays, no offset constants, no area enum
- All buildings are origin-agnostic (manifest stores absolute Swiss coords)

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

### Airport Database

```bash
# Build airports.json from OSM-verified runway data
python scripts/build_airports_json.py

# Or regenerate from swissTLM3D GeoPackage (includes auto-estimation)
python scripts/generate_airports.py \
    --gpkg /path/to/swissTLM3D.gpkg \
    --output scenery/poi/airports.json

# Generate manual runway overrides from OSM data
python scripts/build_manual_runways.py

# Patch airports.json with corrected runways and length caps
python scripts/patch_airports.py
```

### MSFS Scenery Conversion

Microsoft Flight Simulator 2020 scenery packages can be converted to Godot-compatible GLB:

```bash
python scripts/convert_msfs_scenery.py \
    --input /path/to/msfs/package \
    --output scenery/custom/my_scenery/
```

The converter extracts glTF from BGL binary files, converts DDS textures to PNG, and strips 20+ MSFS-specific glTF extensions (ASOBO_*, MSFT_texture_dds). Output is clean GLB with embedded textures, ready for use as custom scenery packages.

## Migration Plan

| Step | Description | Files Changed |
|------|-------------|---------------|
| 1 | Create `scenery/` directory structure with manifests | New files |
| 2 | Move `provpilot/terrain_tiles/` → `scenery/terrain/` | File move |
| 3 | Move `provpilot/Assets/buildings_*/` → `scenery/buildings/` | File move |
| 4 | Add `scenery_path` config to `node_3d.gd` | `node_3d.gd` |
| 5 | Update `terrain_streamer.gd` to use `scenery_path` | `terrain_streamer.gd` |
| 6 | Rewrite `building_loader.gd` to read manifest instead of hardcoded arrays | `building_loader.gd` |
| 7 | Add POI loader (airports, navaids) | New script |
| 8 | Add custom scenery loader | New script |
| 9 | Update processing scripts for full-CH and regional modes | `scripts/*.py` |
| 10 | Build regional packs and test redistribution | Scripts + docs |

## Future Considerations

- **Texture tiles**: Ortho imagery (swissimage) for ground textures — same 1024m grid, ~4 MB/tile JPEG
- **Vegetation**: Tree placement from swissTLM3D
- **Navaids**: VOR, NDB, waypoints from Swiss AIP for instrument navigation
- **Airspace**: CTR/TMA boundaries for airspace awareness
- **Compression**: Consider zstd-compressed terrain tiles to reduce from 4 MB → ~1–2 MB per tile (~80 GB total)
- **LOD**: Multi-resolution terrain (1m near aircraft, 4m distant) to reduce memory — partially addressed by clipmap mip pyramid
- **Clipmap streaming**: `clipmap_streamer.gd` centralises texture ownership and rebuilds on rebase (<1 ms). Toroidal strip-blit infrastructure retained for future use when textures are shrunk to save VRAM
- **Streaming from network**: Load tiles from a local server or CDN instead of requiring full local copy
