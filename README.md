# ProVPilot — Flight Simulator

A flight simulator built with [JSBSim](https://github.com/JSBSim-Team/jsbsim) flight dynamics, [Godot 4.6+](https://godotengine.org/), and real-world Swiss terrain data from [SwissTopo](https://www.swisstopo.admin.ch/).

## Overview

ProVPilot is a flight simulation framework that combines JSBSim's accurate 6-DOF flight physics with Godot's 3D rendering engine. Fly a Cessna 172 Skyhawk over real Swiss terrain with proper elevations, 3D buildings, and full cockpit controls.

Originally based on [chunky/jsbgodot](https://github.com/chunky/jsbgodot/tree/master/demo) for Godot 3, this version has been completely rewritten for Godot 4.6 with many new features.

## Features

- **Realistic Flight Dynamics**: Full 6-DOF simulation powered by JSBSim (Cessna 172 Skyhawk)
- **Real-World Terrain**: SwissTopo swissALTI3D elevation data at 1m resolution
- **Terrain Streaming**: Dynamic tile-based loading — fly over unlimited terrain without loading screens
- **3D Buildings**: SwissTopo swissBUILDINGS3D models loaded dynamically by proximity
- **Multiple Camera Views**:
  - Cockpit camera with pilot head look (hat switch / D-pad / arrow keys)
  - External orbit camera with full rotation and zoom
- **Gamepad & Keyboard**: Full joystick/gamepad control with trigger-based throttle
- **Animated Control Surfaces**: Ailerons, elevators, rudder, flaps, propeller with blur disc, nose gear steering
- **Elevator Trim**: Fine-tune pitch with Y/X buttons
- **Slew Mode**: Reposition aircraft freely, then resume flight (Shift+Y)
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
| Slew Mode | — | Shift+Y |

### Camera Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Switch Camera | Select / Back | C |
| Head Look (Cockpit) | D-pad | Arrow Keys |
| Orbit Camera (External) | D-pad | Arrow Keys |
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
   # macOS ARM64
   scons arch=arm64

   # macOS x86_64
   scons arch=x86_64

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
│   ├── terrain_streamer.gd     # Dynamic terrain tile loader
│   ├── building_loader.gd      # Dynamic building GLB loader
│   ├── node_3d.tscn            # Main scene
│   ├── C172p.tscn              # Aircraft scene
│   ├── environment.tscn        # Sky/environment
│   ├── bin/                    # GDExtension binaries
│   ├── Assets/                 # 3D models (Cessna, buildings)
│   ├── addons/terrain_3d/      # Terrain3D plugin
│   ├── shaders/                # Custom shaders
│   └── project.godot
├── scripts/                     # Tooling
│   ├── process_swisstopo_terrain.py  # Terrain pipeline (download → tiles)
│   └── convert_buildings_to_gltf.py  # Building GDB → GLB converter
├── docs/                        # Additional documentation
│   └── TERRAIN_SETUP.md        # Terrain & collision architecture
├── jsbsim/                      # JSBSim flight dynamics (submodule)
├── godot-cpp/                   # Godot C++ bindings (submodule)
└── SConstruct                   # Build configuration
```

## Terrain System

Terrain is sourced from SwissTopo swissALTI3D (0.5m resolution, resampled to 1m). The terrain streamer dynamically loads 1024×1024m tiles as the aircraft moves, supporting effectively unlimited flight range.

- **Tile grid**: Aligned to Swiss LV95 coordinates at 1024m intervals
- **Load radius**: Configurable (default 8 tiles = 17×17km loaded area)
- **Hysteresis**: Unload margin prevents rapid load/unload at boundaries
- **Coordinate system**: Godot X = West, Godot Z = North (custom mapping from Swiss LV95)

The terrain center is aligned to the 1024m grid: **E 2,682,880 / N 1,283,072**.

See [docs/TERRAIN_SETUP.md](docs/TERRAIN_SETUP.md) for full details on the terrain pipeline, coordinate systems, and collision architecture.

## Building System

3D buildings from SwissTopo swissBUILDINGS3D are converted to GLB format and loaded dynamically based on aircraft proximity. Multiple regions are supported:

- **Schaffhausen/Schmerlat**: Hallau, Schaffhausen area
- **Zurich Airport**: Kloten, surrounding area

Buildings are colored procedurally with configurable roof (terracotta) and wall (cream) colors.

## Control Surface Animation

The aircraft model supports animated control surfaces driven by JSBSim physics:

| Surface | Range | Notes |
|---------|-------|-------|
| Propeller | 0–2700 RPM | Blur disc above 900 RPM, blades hidden above 1200 RPM |
| Ailerons | ±20° | Opposite deflection for roll |
| Elevators | ±25° | Both move together |
| Rudder | ±16° | With linked nose gear steering (±30°) |
| Flaps | 0°–30° | Standard C172 positions |

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

## Known Issues

- First-run terrain tile generation requires internet access (SwissTopo STAC API)
- Building alignment depends on matching terrain center coordinates
- Debug output is verbose (can be reduced by commenting out print statements)

## License

This project combines multiple open-source components:
- JSBSim: LGPL 2.1
- Godot Engine: MIT
- godot-cpp: MIT
- Terrain3D: MIT
- Aircraft models: Various (see individual model licenses)
- Terrain data: SwissTopo Open Data (free for commercial & non-commercial use)

## Acknowledgments

- [JSBSim Team](https://github.com/JSBSim-Team/jsbsim) for the flight dynamics engine
- [chunky/jsbgodot](https://github.com/chunky/jsbgodot) for the original Godot 3 integration
- [Terrain3D](https://github.com/TokisanGames/Terrain3D) for the terrain rendering plugin
- [SwissTopo](https://www.swisstopo.admin.ch/) for high-resolution terrain and building data
  - [swissALTI3D](https://www.swisstopo.admin.ch/de/hoehenmodell-swissalti3d) — elevation model
  - [swissBUILDINGS3D](https://www.swisstopo.admin.ch/de/landschaftsmodell-swissbuildings3d) — 3D buildings
- Sketchfab artists for aircraft models

### Model Credits

- **Cessna 172**: [KOG_THORNS](https://sketchfab.com/ioai25312) — [Cessna172 on Sketchfab](https://sketchfab.com/3d-models/cessna172-d1b15841c29c43d0862667300bad55a4)
