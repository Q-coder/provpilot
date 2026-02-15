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
│   ├── tile_2683904_1283072.raw
│   └── ...                           # ~42,000 files for full CH
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
  "origin_e": 2682880,
  "origin_n": 1283072,
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

The building loader reads this manifest at startup instead of using hardcoded tile arrays — centers are computed dynamically relative to the current `TERRAIN_CENTER_E/N`.

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
| `terrain_streamer.gd` | ✅ Done | Reads `scenery/terrain/manifest.json`, streams 1024m tiles |
| `building_loader.gd` | ✅ Done | Reads `scenery/buildings/manifest.json`, proximity loading |
| `road_loader.gd` | ✅ Done | Reads `scenery/roads/manifest.json`, proximity loading |
| `water_loader.gd` | ✅ Done | Reads `scenery/water/manifest.json`, proximity loading |
| `poi_loader.gd` | ✅ Done | Loads `scenery/poi/airports.json` at startup |
| `runway_loader.gd` | ✅ Done | Procedural terrain-following runway meshes from airport data |
| `position_dialog.gd` | ✅ Done | Airport search/filter/teleport UI |
| Custom scenery loader | Planned | Scans `custom/*/package.json`, loads GLBs by proximity |

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

### Terrain

```bash
# Generate tiles for all of Switzerland
python scripts/process_swisstopo_terrain.py \
    --full-switzerland \
    --output scenery/terrain/ \
    --source-resolution 2  # Use 2m source (37 GB download vs 577 GB)
```

### Buildings

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
```

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
- **LOD**: Multi-resolution terrain (1m near aircraft, 4m distant) to reduce memory
- **Streaming from network**: Load tiles from a local server or CDN instead of requiring full local copy
