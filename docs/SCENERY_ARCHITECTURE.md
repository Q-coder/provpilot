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
| POI / Airports | OpenAIP, custom | Various | JSON |
| Custom scenery | Hand-crafted | Project | GLB + JSON |

### Disk Space Budget

| Data | Per tile | Tile count (CH) | Total |
|------|----------|-----------------|-------|
| Terrain (1m float32) | 4 MB | ~42,000 | **~165 GB** |
| SwissTopo GeoTIFF cache (2m) | ~0.5 MB | 80,485 | ~37 GB (deletable) |
| Buildings (GLB) | varies | ~200 sheets | ~5–10 GB (est.) |
| POI / JSON | — | — | < 1 MB |
| **Total deployed** | | | **~170–175 GB** |

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
├── poi/                              # Points of Interest (lightweight JSON)
│   ├── airports.json                 # Runways, frequencies, ICAO codes
│   ├── navaids.json                  # VOR, NDB, waypoints
│   ├── airspace.json                 # CTR/TMA boundaries (optional)
│   └── landmarks.json                # Peaks, cities, lakes
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

### POI Format (`scenery/poi/airports.json`)

```json
{
  "airports": [
    {
      "icao": "LSZS",
      "name": "Schmerlat",
      "type": "grass_strip",
      "elevation_m": 464,
      "runways": [
        {
          "heading": 70,
          "length_m": 600,
          "width_m": 20,
          "surface": "grass",
          "threshold_e": 2681968,
          "threshold_n": 1282869
        }
      ],
      "frequencies": []
    }
  ]
}
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

| Component | Current | Target |
|-----------|---------|--------|
| `terrain_streamer.gd` | Reads `tile_directory` export | Reads `scenery_path + "/terrain/"` |
| `building_loader.gd` | Hardcoded tile arrays + offsets | Reads `buildings/manifest.json`, computes centers dynamically |
| POI loader | Does not exist | New: loads `poi/*.json` at startup |
| Custom scenery loader | Does not exist | New: scans `custom/*/package.json`, loads GLBs by proximity |

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

# Or generate a regional pack
python scripts/process_swisstopo_terrain.py \
    --region ch-northeast \
    --output scenery/terrain/
```

### Buildings

```bash
# Convert buildings for all of Switzerland
python scripts/convert_buildings_to_gltf.py \
    --full-switzerland \
    --output scenery/buildings/

# Or a specific region
python scripts/convert_buildings_to_gltf.py \
    --region ch-northeast \
    --output scenery/buildings/
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
- **Water bodies**: Lake/river polygons for visual water rendering
- **Compression**: Consider zstd-compressed terrain tiles to reduce from 4 MB → ~1–2 MB per tile (~80 GB total)
- **LOD**: Multi-resolution terrain (1m near aircraft, 4m distant) to reduce memory
- **Streaming from network**: Load tiles from a local server or CDN instead of requiring full local copy
