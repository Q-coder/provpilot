# ProVPilot — Flight Simulator

A flight simulator built with [JSBSim](https://github.com/JSBSim-Team/jsbsim) flight dynamics, [Godot 4.6+](https://godotengine.org/), and real-world Swiss terrain data from [SwissTopo](https://www.swisstopo.admin.ch/).

## Overview

ProVPilot is a flight simulation framework that combines JSBSim's accurate 6-DOF flight physics with Godot's 3D rendering engine. Fly a Cessna 172 Skyhawk over real Swiss terrain with proper elevations, 3D buildings, and full cockpit controls.

The default start position is Schmerlat Airfield (Godot X=980.8, Z=−227.1), facing heading 250° (WSW).

Originally based on [chunky/jsbgodot](https://github.com/chunky/jsbgodot/tree/master/demo) for Godot 3, this version has been completely rewritten for Godot 4.6 with many new features.

## Features

- **Realistic Flight Dynamics**: Full 6-DOF simulation powered by JSBSim (Cessna 172 Skyhawk)
- **Real-World Terrain**: SwissTopo swissALTI3D elevation data at 1m resolution
- **Orthophoto Aerial Imagery**: Real SWISSIMAGE photography overlaid on terrain at 1m/pixel
- **Terrain Streaming**: Dynamic tile-based loading — fly over unlimited terrain without loading screens
- **3D Buildings**: SwissTopo swissBUILDINGS3D models loaded dynamically by proximity (manifest-driven)
- **Roads & Railroads**: swissTLM3D road and railway network rendered as coloured ribbons
- **Water Bodies**: Rivers, lakes, and streams from swissTLM3D
- **Custom Scenery**: Hand-crafted scenery packages with GLB models, loaded by proximity from `scenery/custom/`
- **MSFS Scenery Import**: Convert Microsoft Flight Simulator 2020 scenery packages (BGL→GLB)
- **47 Swiss Airports**: OSM-verified runway geometry with procedural terrain-following runway rendering
- **Airport Teleport**: Search, filter by type, select runway — teleport anywhere in Switzerland (Shift+P)
- **Welcome Screen**: Startup UI with aircraft selection, airport picker with search/filter, and runway selector
- **Settings Screen**: Configurable graphics, sound, realism, and data path settings (persisted to disk)
- **Loading Screen**: Progress overlay with weighted loading tracking during startup and teleport
- **Multiple Camera Views**:
  - Cockpit camera with pilot head look (hat switch / D-pad / arrow keys)
  - External orbit camera with full rotation and zoom
  - Chase camera with smooth follow behind aircraft
- **Gamepad & Keyboard**: Full joystick/gamepad control with trigger-based throttle
- **Interactive Cockpit Controls**: Mouse-drag mixture lever, throttle lever, flap lever, and magneto/ignition switch with visual feedback and keyboard shortcuts
- **Engine Sound**: RPM-mapped engine audio with pitch and volume tied to propeller speed
- **Animated Control Surfaces**: Ailerons, elevators, rudder, flaps, propeller with blur disc, nose gear steering
- **Propeller Visual Decay**: Propeller visually spins down when engine is off (ignores JSBSim windmilling)
- **Elevator Trim**: Fine-tune pitch with Y/X buttons
- **Slew Mode**: Reposition aircraft freely with airspeed preservation, then resume flight (Shift+Y)
- **Floating Origin**: Automatic coordinate rebasing for precision across all of Switzerland
- **Deferred Scenery Loading**: Terrain and scenery load only after the user clicks Fly — instant startup
- **HUD**: Airspeed, altitude, vertical speed, heading, throttle, Swiss/WGS84 coordinates

## Controls

### Flight Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Pitch (Elevator) | Left Stick Y | W / S |
| Roll (Aileron) | Left Stick X | A / D |
| Rudder | Right Stick X | Q / E |
| Throttle Up | Right Trigger (RT) | Numpad + |
| Throttle Down | Left Trigger (LT) | Numpad - |
| Flaps Up | D-pad Up | F |
| Flaps Down | D-pad Down | V |
| Brakes | A Button | B |
| Trim Up | Y Button | — |
| Trim Down | X Button | — |
| Mixture Richer | — | M |
| Mixture Leaner | — | Shift+M |
| Throttle Increase | — | T |
| Throttle Decrease | — | Shift+T |
| Magneto Clockwise | — | I |
| Magneto Counter-CW | — | Shift+I |
| Slew Mode | — | Shift+Y |
| Flap Lever Drag | — | Mouse drag (cockpit) |
| Position Dialog | — | Shift+P |
| Return to Welcome | — | Escape |

### Camera Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Switch Camera (Cockpit/Orbit/Chase) | Select / Back | C |
| Head Look (Cockpit) | D-pad | Arrow Keys |
| Orbit/Chase Camera Adjust | D-pad | Arrow Keys |
| Zoom In | L1 (Left Shoulder) | Page Up |
| Zoom Out | R1 (Right Shoulder) | Page Down |

### Slew Mode Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Move Horizontal | Left Stick | W/A/S/D |
| Move Vertical | Right Stick Y | Q / E |
| Rotate (Yaw) | Right Stick X | — |
| Fast Movement | — | Hold Shift |
| Exit Slew | — | Shift+Y |

## Building

### Prerequisites

- Godot 4.6+ (dev builds work)
- Python 3.x with SCons
- C++ compiler (Clang on macOS, GCC on Linux, MSVC on Windows)
- JSBSim source (included as submodule)
- godot-cpp (included as submodule)

### Build Steps

1. Clone the repository with submodules:
   ```bash
   git clone --recursive git@gitlab.md80.ch:gery/provpilot.git
   cd provpilot
   ```

2. Build JSBSim:
   ```bash
   cd jsbsim
   mkdir build && cd build
   cmake ..
   make -j$(nproc)
   cd ../..
   ```

3. Build the GDExtension:
   ```bash
   # macOS ARM64 (debug)
   scons arch=arm64

   # macOS ARM64 (release, for distribution)
   scons target=template_release arch=arm64

   # Linux
   scons
   ```

4. Generate terrain tiles (requires Python + rasterio):
   ```bash
   pip install rasterio numpy requests tqdm
   python scripts/process_swisstopo_terrain.py \
       --center-e 2682880 --center-n 1283072 \
       --size 17408 --generate-tiles \
       --tile-output provpilot/terrain_tiles
   ```

5. Open the project in Godot:
   ```bash
   cd provpilot
   godot --editor
   ```

## Project Structure

```
provpilot/
├── src/                         # GDExtension C++ source
│   ├── jsbgodot.cpp/.h         # JSBSim ↔ Godot integration
│   └── register_types.cpp/.h   # GDExtension registration
├── provpilot/                   # Godot project
│   ├── node_3d.gd              # Main scene script (controls, HUD, cameras)
│   ├── terrain_streamer.gd     # Dynamic terrain tile loader (+ orthophoto)
│   ├── building_loader.gd      # Manifest-driven building GLB loader
│   ├── road_loader.gd          # Road & railroad network loader
│   ├── water_loader.gd         # River & lake loader
│   ├── runway_loader.gd        # Procedural runway renderer
│   ├── poi_loader.gd           # Airport/POI database loader
│   ├── distant_terrain.gd      # Low-res distant terrain renderer
│   ├── custom_scenery_loader.gd # Custom scenery package discovery
│   ├── object_loader.gd        # Custom scenery object placement loader
│   ├── welcome_screen.gd       # Startup aircraft & airport selection UI
│   ├── settings_screen.gd      # Settings UI (graphics, sound, realism, paths)
│   ├── loading_screen.gd       # Progress overlay during loading
│   ├── position_dialog.gd      # In-flight airport search & teleport UI
│   ├── export_presets.cfg      # macOS export preset
│   ├── node_3d.tscn            # Main scene
│   ├── C172p.tscn              # Aircraft scene
│   ├── environment.tscn        # Sky/environment
│   ├── bin/                    # GDExtension binaries
│   ├── Assets/                 # 3D models (Cessna, symlinks to scenery)
│   ├── sounds/                 # Audio files (engine sound)
│   ├── addons/terrain_3d/      # Terrain3D plugin
│   ├── shaders/                # Custom shaders
│   └── project.godot
├── scenery/                     # Scenery data (large files gitignored)
│   ├── terrain/                # Elevation heightmaps (.raw)
│   ├── orthophoto/             # Aerial imagery JPEG tiles (1m/pixel)
│   ├── orthophoto_hires/       # High-res orthophoto tiles (optional)
│   ├── buildings/              # 3D building models (.glb)
│   ├── roads/                  # Road & railroad meshes (.glb)
│   ├── water/                  # River & lake meshes (.glb)
│   ├── poi/                    # Airport database (airports.json)
│   └── custom/                 # Hand-crafted scenery packages
├── scripts/                     # Tooling
│   ├── scenery_pipeline.py          # Unified terrain + buildings pipeline
│   ├── process_swisstopo_terrain.py # Terrain-only pipeline (download → tiles)
│   ├── convert_buildings_to_gltf.py # Building GDB → GLB converter
│   ├── generate_building_manifest.py # Generate buildings manifest.json
│   ├── generate_roads.py            # Road/railroad GeoPackage → GLB
│   ├── generate_water.py            # Water body GeoPackage → GLB
│   ├── generate_airports.py         # Airport extraction from swissTLM3D
│   ├── build_airports_json.py       # Build airports.json from verified data
│   ├── build_manual_runways.py      # Generate manual runway overrides from OSM
│   ├── patch_airports.py            # Patch airports.json with corrected runways
│   ├── fetch_osm_runways.py         # Fetch runway data from OSM Overpass
│   ├── generate_orthophoto_tiles.py # Orthophoto tile pipeline (WMS)
│   ├── process_swissimage_orthophoto.py # STAC-based orthophoto (small areas)
│   ├── generate_distant_terrain.py  # Distant terrain low-res mesh
│   ├── generate_distant_orthophoto.py # Distant terrain WMS color map
│   ├── convert_msfs_scenery.py      # MSFS 2020 scenery → GLB converter
│   ├── build_macos.sh               # macOS build, sign & assemble script
│   └── setup_windows.ps1            # Windows directory junction setup
├── docs/                        # Additional documentation
│   ├── SCENERY_ARCHITECTURE.md  # Scenery data architecture & formats
│   ├── TERRAIN_SETUP.md        # Terrain & collision architecture
│   └── WINDOWS_SETUP.md        # Windows build setup guide
├── jsbsim/                      # JSBSim flight dynamics (submodule)
├── godot-cpp/                   # Godot C++ bindings (submodule)
└── SConstruct                   # Build configuration
```

## macOS Distribution

ProvPilot can be exported as a standalone macOS application. The distribution uses a folder-based layout (like X-Plane) where the `.app` bundle, scenery data, and JSBSim data live as siblings:

```
ProvPilot/
  ProvPilot.app/     # Signed Godot export (~6GB)
  scenery/            # Terrain, orthophoto, buildings, roads, water, POI
  jsbsim/             # Aircraft, engine, and systems data
```

### Quick Build

```bash
./scripts/build_macos.sh
```

This script builds the GDExtension, exports the Godot project, signs with Developer ID, and assembles the distribution folder.

### Manual Steps

1. Build the release GDExtension: `scons target=template_release arch=arm64`
2. Export from Godot: uses the `macOS` preset in `provpilot/export_presets.cfg`
3. Code-sign with Developer ID (inside-out: frameworks → dylibs → app)
4. Place `scenery/` and `jsbsim/` alongside the `.app` in a folder

The app auto-detects whether it's running in the editor or as an exported bundle and resolves data paths accordingly.

### Requirements

- Godot 4.6+ export templates installed
- Apple Developer ID Application certificate for code signing
- ~190 GB disk space for full scenery (terrain: 166 GB, orthophoto: 13 GB, buildings: 3.8 GB)
- Default viewport resolution: 3840×2160 (4K)

## Terrain System

Terrain is sourced from SwissTopo swissALTI3D (0.5m resolution, resampled to 1m). The terrain streamer dynamically loads 1024×1024m tiles as the aircraft moves, supporting effectively unlimited flight range.

- **Tile grid**: Aligned to Swiss LV95 coordinates at 1024m intervals
- **Load radius**: Configurable (default 8 tiles = 17×17km loaded area)
- **Hysteresis**: Unload margin prevents rapid load/unload at boundaries
- **Coordinate system**: Godot X = West, Godot Z = North (custom mapping from Swiss LV95)

The terrain center is aligned to the 1024m grid: **E 2,682,880 / N 1,283,072**.

See [docs/TERRAIN_SETUP.md](docs/TERRAIN_SETUP.md) for full details on the terrain pipeline, coordinate systems, and collision architecture.

## Startup Flow

On launch, the simulator follows a deferred loading sequence:

1. **Loading screen** appears immediately (lightweight splash)
2. Core nodes initialize (Terrain3D, JSBSim binding)
3. **Welcome screen** appears with aircraft selection, airport picker (search/filter by ICAO or name, type filters), and runway selector
4. User selects aircraft + departure airport/runway and clicks **Fly!**
5. **Loading screen** reappears with progress bar — terrain tiles, distant terrain, buildings, roads, water, and orthophotos load progressively
6. After loading completes, the simulation begins at the selected runway

Press **Escape** during flight to return to the welcome screen.

## Settings

Open the settings screen from the welcome screen's gear icon. Settings persist to `user://settings.cfg`.

| Section | Settings |
|---------|----------|
| **Paths** | Scenery directory, JSBSim data directory |
| **Graphics** | Render distance (2–16 tiles), building distance (1–8 tiles), shadow quality (Off/Low/Medium/High), VSync, fullscreen |
| **Sound** | Master volume, engine volume, wind volume, UI volume |
| **Realism** | Auto mixture, auto carb heat, unlimited fuel, crash detection, turbulence (None/Light/Moderate/Severe), wind speed & direction |

## Building System

3D buildings from SwissTopo swissBUILDINGS3D are converted to GLB format and loaded dynamically based on aircraft proximity. The building loader reads `scenery/buildings/manifest.json` at startup — no hardcoded tile arrays. Buildings use a wall/roof normal-based shader with configurable roof (terracotta) and wall (cream) colors.

## Road & Railroad System

Roads and railroads from SwissTopo swissTLM3D are rendered as coloured ribbons on the terrain surface, tiled into 4096m grid cells and loaded by proximity. Road types include motorways, primary/secondary roads, residential streets, tracks, paths, and railway lines — each with a distinct colour.

## Water System

Rivers, lakes, and streams from SwissTopo swissTLM3D are rendered as blue surfaces. Lakes are filled polygons; rivers and streams are width-proportional ribbons. Also tiled into 4096m cells with proximity loading.

## Airport & Runway System

47 Swiss airports are included with OSM-verified runway geometry. Runways are rendered procedurally as terrain-following segmented meshes with centerline and threshold markings. Airport types include international airports, regional airports, grass strips, and heliports.

The welcome screen provides departure airport selection. During flight, the position dialog (Shift+P) provides:
- Full-text search by ICAO code or name
- Type filters (international, regional, grass, heliport)
- Runway selector with heading, length, and surface info
- One-click teleport to any runway threshold

## Custom Scenery

Hand-crafted scenery packages live in `scenery/custom/`. Each package contains a `package.json` with metadata and object placements, plus GLB model files. The `custom_scenery_loader.gd` discovers packages at startup; `object_loader.gd` instantiates objects by proximity (15 km load / 20 km unload) with automatic terrain elevation sampling and floating-origin support.

See [docs/SCENERY_ARCHITECTURE.md](docs/SCENERY_ARCHITECTURE.md) for the package.json format.

## MSFS Scenery Import

Microsoft Flight Simulator 2020 scenery packages can be converted to Godot-compatible GLB:

```bash
python scripts/convert_msfs_scenery.py --input /path/to/msfs/package --output scenery/custom/my_scenery/
```

The converter extracts glTF from BGL files, converts DDS textures to PNG, and strips MSFS-specific extensions.

## Control Surface Animation

The aircraft model supports animated control surfaces driven by JSBSim physics:

| Surface | Range | Notes |
|---------|-------|-------|
| Propeller | 0–2700 RPM | Blur disc above 900 RPM, blades hidden above 1200 RPM; visual spin-down on engine off |
| Ailerons | ±20° | Opposite deflection for roll |
| Elevators | ±25° | Both move together |
| Rudder | ±16° | With linked nose gear steering (±30°) |
| Flaps | 0°–30° | Standard C172 positions |
| Mixture Lever | 0–100% | Mouse drag (up=rich) or M/Shift+M keys; cutoff kills engine |
| Throttle Lever | 0–100% | Mouse drag (up=more) or T/Shift+T keys |
| Flap Lever | 0°/10°/20°/30° | Mouse drag (up=retract) or F/Shift+F keys; 4 discrete notches |
| Magneto Switch | OFF/R/L/BOTH/START | Mouse drag (right=CW) or I/Shift+I keys; 5-position rotary |

## Engine Management

The Cessna 172 engine simulation includes realistic magneto/ignition, mixture, and starter controls.

### Magneto Switch Positions

| Position | Effect |
|----------|--------|
| **OFF** | No ignition — engine will not run |
| **R** | Right magneto only |
| **L** | Left magneto only |
| **BOTH** | Both magnetos — normal operation |
| **START** | Engages starter motor (spring-loaded, returns to BOTH on release) |

### Engine Startup Procedure

1. Set **Mixture** to full rich (M key or drag lever up)
2. Set **Magneto** switch to **BOTH** (I key)
3. Advance to **START** (I key again) — hold until engine catches
4. Release I key — switch springs back to BOTH

### Engine Shutdown

- Pull **Mixture** to cutoff (Shift+M repeatedly, or drag lever down)
- The propeller will visually spin down and stop

### Mouse Interaction

All cockpit levers and the magneto switch are clickable in cockpit camera mode:
- **Mixture/Throttle levers**: Click and drag vertically (up = increase)
- **Magneto switch**: Click and drag horizontally (right = clockwise toward START)

## Aircraft Models

### Included
- **Cessna 172 Skyhawk**: Full JSBSim flight model with animated control surfaces

### Adding New Aircraft
1. Add JSBSim aircraft XML files to `jsbsim/aircraft/`
2. Import 3D model into `provpilot/Assets/`
3. Separate control surfaces as individual mesh objects in Blender
4. Set origin/pivot points at hinge lines
5. Update node paths in `node_3d.gd`
6. Modify `jsbgodot.cpp` if needed for aircraft-specific features

## Engine Sound

The engine audio system uses an `AudioStreamPlayer` with RPM-mapped pitch (0.7–1.6) and volume (−12 to +3 dB). Sound fades out below 50 RPM and respects the user's engine volume setting from the settings screen.

## Known Issues

- First-run terrain tile generation requires internet access (SwissTopo STAC API)
- Debug output is verbose (can be reduced by commenting out print statements)

## License

This project combines multiple open-source components:
- JSBSim: LGPL 2.1
- Godot Engine: MIT
- godot-cpp: MIT
- Terrain3D: MIT
- Aircraft models: Various (see individual model licenses)
- Terrain data: SwissTopo Open Data (free for commercial & non-commercial use)
- Road, water, airport data: SwissTopo swissTLM3D (Open Data)

## Acknowledgments

- [JSBSim Team](https://github.com/JSBSim-Team/jsbsim) for the flight dynamics engine
- [chunky/jsbgodot](https://github.com/chunky/jsbgodot) for the original Godot 3 integration
- [Terrain3D](https://github.com/TokisanGames/Terrain3D) for the terrain rendering plugin
- [SwissTopo](https://www.swisstopo.admin.ch/) for high-resolution terrain, building, and geographic data
  - [swissALTI3D](https://www.swisstopo.admin.ch/de/hoehenmodell-swissalti3d) — elevation model
  - [swissBUILDINGS3D](https://www.swisstopo.admin.ch/de/landschaftsmodell-swissbuildings3d) — 3D buildings
  - [swissTLM3D](https://www.swisstopo.admin.ch/de/landschaftsmodell-swisstlm3d) — roads, railroads, water, airports
- [OpenStreetMap](https://www.openstreetmap.org/) for runway verification data
- Sketchfab artists for aircraft models

### Model Credits

- **Cessna 172**: [KOG_THORNS](https://sketchfab.com/ioai25312) — [Cessna172 on Sketchfab](https://sketchfab.com/3d-models/cessna172-d1b15841c29c43d0862667300bad55a4)
