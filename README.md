# ProVPilot — Flight Simulator

A flight simulator built with [JSBSim](https://github.com/JSBSim-Team/jsbsim) flight dynamics, [Godot 4.6+](https://godotengine.org/), and real-world Swiss terrain data from [SwissTopo](https://www.swisstopo.admin.ch/).

## Overview

ProVPilot is a flight simulation framework that combines JSBSim's accurate 6-DOF flight physics with Godot's 3D rendering engine. Fly a Cessna 172 Skyhawk over real Swiss terrain with proper elevations, 3D buildings, and full cockpit controls.

The default start position is Schmerlat Airfield (Godot X=980.8, Z=−227.1), facing heading 250° (WSW).

Originally based on [chunky/jsbgodot](https://github.com/chunky/jsbgodot/tree/master/demo) for Godot 3, this version has been completely rewritten for Godot 4.6 with many new features.

## Features

- **Realistic Flight Dynamics**: Full 6-DOF simulation powered by JSBSim (Cessna 172 Skyhawk)
- **Scenery3D World Streaming**: A single C++ GDExtension (`Scenery3D`) streams terrain, buildings, roads, water, and orthophotos on background threads with distance-based LOD
- **Real-World Terrain**: SwissTopo swissALTI3D elevation data at 1m resolution, streamed as 1024×1024 m float32 tiles
- **Orthophoto Aerial Imagery**: Real SWISSIMAGE photography mapped per tile with mip pyramid for far-LOD
- **3D Buildings**: SwissTopo swissBUILDINGS3D models with per-building detail meshes and merged far-LOD handoff
- **Roads & Railroads**: swissTLM3D road and railway network rendered as coloured ribbons with white lane markings
- **Water Bodies**: Rivers, lakes, and streams from swissTLM3D (vertex-coloured ribbon meshes)
- **Far Terrain Chunks**: 200 km+ visibility via 8×8 tile composite meshes for Alpine panoramas
- **Double-Precision Godot**: Custom `4.6-macdev` fork with `precision=double` — no floating-origin rebasing needed
- **Custom Scenery**: Hand-crafted scenery packages with GLB models, loaded by proximity from `scenery/custom/`
- **MSFS Scenery Import**: Convert Microsoft Flight Simulator 2020 scenery packages (BGL→GLB)
- **47 Swiss Airports**: OSM-verified runway geometry (teleport via welcome screen)
- **Welcome Screen**: Startup UI with aircraft selection and airport/runway picker
- **Settings Screen**: Configurable graphics, sound, realism, and data path settings (persisted to disk)
- **In-App Scenery Downloader**: Pulls per-region base + per-quadrant detail packs from `https://www.md80.ch/provpilot/scenery/`, verifies sha256, extracts to a configurable install root, and auto-registers the result in the Scenery Library
- **Loading Screen**: Progress overlay polling `Scenery3D.is_ready()`
- **Multiple Camera Views**:
  - Cockpit camera with pilot head look (hat switch / D-pad / arrow keys)
  - External orbit camera with full rotation and zoom
  - Chase camera with smooth follow behind aircraft
- **Gamepad & Keyboard**: Full joystick/gamepad control with trigger-based throttle. Per‑axis bindings, calibration (min/center/max + invert), deadzone and response curve are configured at runtime from Settings → Joystick.
- **Interactive Cockpit Controls**: Mouse-drag mixture lever, throttle lever, flap lever, and magneto/ignition switch with visual feedback and keyboard shortcuts
- **Engine Sound**: RPM-mapped engine audio with pitch and volume tied to propeller speed
- **Animated Control Surfaces**: Ailerons, elevators, rudder, flaps, propeller with blur disc, nose gear steering
- **Propeller Visual Decay**: Propeller visually spins down when engine is off (ignores JSBSim windmilling)
- **Elevator Trim**: Fine-tune pitch with Y/X buttons
- **Slew Mode**: Reposition aircraft freely with airspeed preservation, then resume flight (Shift+Y)
- **Deferred Scenery Loading**: Scenery3D starts streaming only after the user clicks Fly — instant startup
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

> Gamepad axes are calibrated per‑user in **Settings → Joystick**. If your right‑Y is a half‑axis (Logitech Dual Action and similar), bind a different axis to **Slew: Up/Down**, or use the keyboard `Q` / `E` keys for vertical motion. Bindings persist to `user://joystick.cfg`.

### Joystick / Gamepad Configuration

Open the **Settings** screen from the welcome menu and scroll to the **Joystick** section. For every joystick action (slew strafe / forward / yaw / vertical, plus reserved flight & camera bindings):

1. Click **Detect** and move the desired axis (or press the desired button). The axis index is captured automatically — no need to know controller index numbers.
2. The blue bar shows the **raw** axis value; the green bar shows the **calibrated** result that gameplay code receives.
3. With the stick at rest, click **Ctr**. Push to one extreme, click **Min** (or **Max**). The captured raw values define the symmetric `[-1, 1]` mapping — half‑axes work fine, just leave one endpoint equal to center.
4. Tick **Inv** if the direction is reversed. Adjust the deadzone slider so the calibrated bar reads exactly 0 at rest. Pick a response curve (Linear / Squared / Cubed) for analog sensitivity.
5. **Save** persists everything to `user://joystick.cfg`. **Restore Defaults** falls back to a generic SDL gamepad layout.

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
   # macOS ARM64 (debug, double precision)
   scons arch=arm64 precision=double

   # macOS ARM64 (release)
   scons arch=arm64 target=template_release precision=double

   # Linux
   scons precision=double
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
├── src/                         # GDExtension C++ source (JSBSim binding)
│   ├── jsbgodot.cpp/.h          # JSBSim ↔ Godot integration
│   └── register_types.cpp/.h    # GDExtension registration
├── provpilot/                   # Godot project
│   ├── node_3d.gd               # Main scene script (controls, HUD, cameras, Scenery3D glue)
│   ├── welcome_screen.gd        # Startup aircraft & airport selection UI
│   ├── settings_screen.gd       # Settings UI (graphics, sound, realism, paths)
│   ├── loading_screen.gd        # Progress overlay (polls Scenery3D.is_ready)
│   ├── custom_scenery_loader.gd # Custom scenery package discovery
│   ├── object_loader.gd         # Custom scenery object placement
│   ├── export_presets.cfg       # macOS export preset
│   ├── node_3d.tscn             # Main scene (single Scenery3D node)
│   ├── C172p.tscn               # Aircraft scene
│   ├── environment.tscn         # Sky/environment
│   ├── addons/scenery3d/        # Scenery3D GDExtension (git submodule)
│   ├── bin/                     # GDExtension binaries (provpilot + scenery3d)
│   ├── Assets/                  # 3D models (Cessna, symlinks to scenery)
│   ├── sounds/                  # Audio files (engine sound)
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

Terrain is streamed by the [Scenery3D](provpilot/addons/scenery3d/README.md)
GDExtension. Heightmaps come from SwissTopo swissALTI3D (0.5 m native, resampled
to 1 m). A single `Scenery3D` node loads 1024×1024 m float32 tiles on a
background thread pool with distance-based LOD:

- **Close ring** (≤ 3 tiles): 129×129 vertex meshes with full detail
- **Mid ring** (≤ 8 tiles): 65×65 vertices
- **Far ring** (≤ 25 tiles): 17×17 vertices
- **Horizon ring** (≤ load_radius): 5×5 vertices
- **Chunks** (beyond load_radius): 8×8 tile composite meshes out to 200 km

The terrain origin is aligned to the 1024 m LV95 grid:
**E 2,682,880 / N 1,283,072**.

Because the Godot fork runs at `precision=double`, no floating-origin rebasing
is required — world coordinates stay stable across all of Switzerland.

See [docs/TERRAIN_SETUP.md](docs/TERRAIN_SETUP.md) and the Scenery3D README for
full details on the tile format, elevation API, and LOD pipeline.

## Startup Flow

On launch, the simulator follows a deferred loading sequence:

1. **Loading screen** appears immediately (lightweight splash)
2. Core nodes initialize (Scenery3D, JSBSim binding)
3. **Welcome screen** appears with aircraft selection and a starting airport (currently hardcoded to LSPF Schaffhausen)
4. User selects aircraft and clicks **Fly!**
5. **Loading screen** reappears and polls `Scenery3D.is_ready()` while tiles, buildings, roads, water, and orthophotos stream in
6. Once ready, the simulation begins at the selected airport

Press **Escape** during flight to return to the welcome screen.

## Settings

Open the settings screen from the welcome screen's gear icon. Settings persist to `user://settings.cfg`.

| Section | Settings |
|---------|----------|
| **Paths** | Scenery directory, JSBSim data directory |
| **Scenery Library** | FSX-style per-region scenery sources (priority, enable, scan/add/remove) — see [docs/SCENERY_LIBRARY.md](docs/SCENERY_LIBRARY.md) |
| **Download Scenery** | One-click install of official region packs from https://www.md80.ch/provpilot/scenery/ — see [docs/SCENERY_DISTRIBUTION.md](docs/SCENERY_DISTRIBUTION.md) |
| **Graphics** | Render distance (2–16 tiles), building distance (1–8 tiles), shadow quality (Off/Low/Medium/High), VSync, fullscreen |
| **Sound** | Master volume, engine volume, wind volume, UI volume |
| **Realism** | Auto mixture, auto carb heat, unlimited fuel, crash detection, turbulence (None/Light/Moderate/Severe), wind speed & direction |
| **Joystick** | Per-axis bindings, calibration, deadzone, response curve |

## Building System

3D buildings from SwissTopo swissBUILDINGS3D are converted to GLB format and streamed by Scenery3D's building manager. Each tile GLB contains per-building detail meshes plus a merged far-LOD mesh; Scenery3D swaps between them based on distance without visible pop-in.

## Road & Railroad System

Roads and railroads from SwissTopo swissTLM3D are rendered as coloured ribbons tiled into LV95 grid cells and loaded by Scenery3D's road manager. Road types include motorways, primary/secondary roads, residential streets, tracks, paths, and railway lines — each with a distinct colour and, for higher-class roads, white lane/edge markings.

## Water System

Rivers, lakes, and streams from SwissTopo swissTLM3D are rendered as vertex-coloured meshes (rivers darker, streams lighter) streamed by Scenery3D's water manager, offset above the terrain to avoid z-fighting.

## Airport & Runway System

47 Swiss airports are available with OSM-verified runway geometry in `scenery/poi/airports.json`. The welcome screen currently seeds one default airport (LSPF Schaffhausen); the full picker and in-flight teleport UI from the previous loader-based architecture have not yet been ported to Scenery3D.

## Custom Scenery

Hand-crafted scenery packages live in `scenery/custom/`. Each package contains a `package.json` with metadata and object placements, plus GLB model files. `custom_scenery_loader.gd` discovers packages at startup and `object_loader.gd` instantiates objects by proximity with automatic terrain elevation sampling via `Scenery3D.get_elevation()`.

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
- Godot Engine: MIT (custom `4.6-macdev` fork with `precision=double`)
- godot-cpp: MIT
- Scenery3D: MIT (https://gitlab.md80.ch/gery/scenery3d)
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
