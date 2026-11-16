# ProVPilot — Flight Simulator

A flight simulator built with [JSBSim](https://github.com/JSBSim-Team/jsbsim) flight dynamics, [Godot 4.6+](https://godotengine.org/), and real-world Swiss terrain data from [SwissTopo](https://www.swisstopo.admin.ch/).

## Overview

ProVPilot is a flight simulation framework that combines JSBSim's accurate 6-DOF flight physics with Godot's 3D rendering engine. Fly a Cessna 172 Skyhawk over real Swiss terrain with proper elevations, 3D buildings, and full cockpit controls.

The default start position is Schmerlat Airfield (Godot X=980.8, Z=−227.1), facing heading 250° (WSW).

Originally based on [chunky/jsbgodot](https://github.com/chunky/jsbgodot/tree/master/demo) for Godot 3, this version has been completely rewritten for Godot 4.6 with many new features.

## Features

- **Realistic Flight Dynamics**: Full 6-DOF simulation powered by JSBSim (Cessna 172P Skyhawk and Pilatus PC-7 Turbo Trainer)
- **McDonnell Douglas MD-82** (FlightGear MD-80 by Octal450, GPL-2): the full JSBSim systems model (electrical, fuel, hydraulics, pneumatics, APU, flight controls, thrust limits) runs inside the FDM, fed by a two-way FlightGear property mirror; the complete virtual cockpit with 399 clickable parts and its sound set, and the DFGS autoflight (the package's control laws run as FlightGear property rules, its mode logic ported to C++: HDG SEL / HOLD, VOR LOC, ILS and autoland, ALT HOLD and capture, V/S, IAS / MACH, TURB, TOGA, autothrottle SPD / MACH / EPR LIM, the FMA windows) — see [docs/MD80_FLIGHTGEAR_PORT.md](docs/MD80_FLIGHTGEAR_PORT.md). Operate it from the glareshield panel: FD and A/T switches, AP ON / OFF, HDG knob (push = HOLD, pull = SEL), ALT knob (pull = arm; it also arms itself a second after a change), the pitch wheel for V/S, SPD SEL / MACH SEL / EPR LIM, VOR LOC / ILS / AUTO LAND, VERT SPD / IAS MACH / ALT HOLD / TURB; set the TRI knob (pedestal) to CL or CR after take-off, with T/O selected the autothrottle stays clamped; the ALT knob steps 1 000 ft per wheel click, 100 ft with Shift or while the knob is held pushed; the TOGA buttons on the thrust levers arm T/O on the ground with flaps 5 or more, and either thrust lever moves both engines (one throttle axis); the FMAs show the active modes in green and the armed ones in amber in the 16-segment face (the armed altitude as a flight level, 060 for 6 000 ft), THR only with the autothrottle on; every FGCP window is the same amber as the NAV / CRS digits and follows the digital dimmer knob, the panel knob lights the backplate; the autopilot flies the glideslope with FlightGear's intercept law (a 3.3° beam like LSZH 28 included), AUTO LAND flares and rolls out, and the autothrottle lets go once it has retarded the levers to idle on the ground; the R key (or a pad button) deploys the thrust reversers on the ground with the levers at idle, the throttle axis then commands reverse power and the same key stows them
- **Schleicher ASK 21** (FlightGear ASK21, GPL-2): the JSBSim glider with its full instrument panel and wind sounds; no winch or tow yet, so it starts airborne from the new **Start** section of the Aircraft settings (height, speed, heading — available for every aircraft) — see [docs/ASK21_PORT.md](docs/ASK21_PORT.md)
- **Working cockpits**: the Cessna 172 and the Pilatus PC-7 fly with FlightGear's generic 3D instruments built into their panels (airspeed, attitude, altimeter, heading, turn, VSI, VOR/ILS, ADF, clock) and KX 165 / KR 87 radios; the A-10 uses its FlightGear cockpit — see [docs/INSTRUMENTS.md](docs/INSTRUMENTS.md)
- **Config-Driven Aircraft**: Each airframe is described by a plain-text `aircraft.cfg` (model + JSBSim wiring, animations, gear, propeller, camera) so the engine stays aircraft-agnostic — see [docs/AIRCRAFT_CONFIG.md](docs/AIRCRAFT_CONFIG.md)
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
- **Settings Screen**: Tabbed C++ settings UI (Aircraft, Scenery, Downloads, Graphics, World, Map, Sound, Realism, Joystick), persisted to `provpilot.cfg`
- **In-App Scenery Downloader**: Pulls per-region base + per-quadrant detail packs from `https://www.md80.ch/provpilot/scenery/`, verifies sha256, extracts to a configurable install root, and auto-registers the result in the Scenery Library
- **Loading Screen**: Progress overlay for the startup load; after **Fly!** the space → spot intro zoom is the loading view (status and progress on its HUD)
- **Multiple Camera Views**:
  - Cockpit camera with pilot head look (hat switch / D-pad / arrow keys)
  - External orbit camera with full rotation and zoom
  - Chase camera with smooth follow behind aircraft
- **Gamepad & Keyboard**: Full joystick/gamepad control with trigger-based throttle. Per‑axis bindings, calibration (min/center/max + invert), deadzone and response curve are configured at runtime from Settings → Joystick.
- **Interactive Cockpit Controls**: Mouse-drag mixture lever, throttle lever, flap lever, and magneto/ignition switch with visual feedback and keyboard shortcuts
- **Engine Sound**: RPM-mapped engine audio with pitch and volume tied to propeller speed
- **Animated Control Surfaces**: Ailerons, elevators, rudder, flaps, propeller with blur disc, nose gear steering
- **Retractable Landing Gear**: Per-leg animated retract/extend (G key), with JSBSim drag and weight-on-wheels driven by gear position
- **Propeller Visual Decay**: Propeller visually spins down when engine is off (ignores JSBSim windmilling)
- **Elevator Trim**: Fine-tune pitch with Y/X buttons
- **Slew Mode**: Reposition aircraft freely with airspeed preservation, then resume flight (Shift+Y)
- **Scenery editor** (F8 in flight, or the welcome screen's **Edit Mode** button: no aircraft, no intro zoom, straight over the selected airport): free camera + terrain cursor, place roads and railways that are saved with the scenery — see [docs/SCENERY_EDITOR.md](docs/SCENERY_EDITOR.md)
- **Deferred Scenery Loading**: Scenery3D starts streaming only after the user clicks Fly — instant startup
- **HUD**: Airspeed, altitude, vertical speed, heading, throttle, Swiss/WGS84 coordinates, tuned NAV / ADF stations
- **Forests**: trees drawn as 8-triangle species cards from a Blender-baked atlas (beech, oak, maple, birch, spruce, fir, pine; conifers take over with altitude and trees shrink towards the tree line), near and far sets per km² tile out to 8 km, every tree on the drawn terrain, roads and railways kept clear — about 100 fps over the densest Zürich Oberland forests. Switzerland from the swissTLM3D tree positions, every other region from OpenStreetMap forest polygons filled at load time (`scripts/build_world_forests.py`); beyond the cards the same polygons are painted into the ground texture of every tile without an orthophoto, so woods stay recognisable to the horizon; see [docs/WORLD_SCENERY_DESIGN.md](docs/WORLD_SCENERY_DESIGN.md) §14
- **Radio navigation**: VOR / DME / NDB receivers (C++ `RadioNav`) over the OurAirports navaid catalog (901 stations across the nine regions), ILS localizer / glide slope / DME at 292 airports (437 real approaches from FlightGear's nav.dat with the frequency changes since 2013 corrected from the AIPs in `scripts/ils_updates.json`, aligned with the drawn runways), magnetic headings from the WMM2025 model, Cessna 172 VOR / ADF gauges with KX 165 and KR 87 radios, navaids on the VFR map — see [docs/RADIO_NAVIGATION.md](docs/RADIO_NAVIGATION.md)
- **VFR Map Frame**: Heading-up in-game map inset (C++ `VfrMap` module) with cartographic hillshade terrain, range rings, airport symbols/ICAO labels, and canton borders — toggle with N (remembered: Continue Flight, the settings screen and the next launch keep the frame as you left it), zoom with the wheel; see [docs/VFR_MAP.md](docs/VFR_MAP.md)

## Controls

Every key below is a default. **Settings → Keyboard** shows the mapping in
three sections (Flight, Slew, Scenery editor), lets you rebind each action to
up to two keys (click the key, press the new one; Escape keeps it, Backspace
clears it; Shift / Ctrl / Alt alone bind on release), marks a key used twice in
the same section, and **Save** writes `keyboard.cfg` next to `provpilot.cfg`
(a plain config file: `[flight] flaps_down="F"`, `[slew] slew_up="Q, PageUp"`;
`Ctrl` also means Cmd). The in-game hints follow the mapping.

### Flight Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Pitch (Elevator) | Left Stick Y | W / S |
| Roll (Aileron) | Left Stick X | A / D |
| Rudder | Right Stick X | Y (left) / X (right) |
| Throttle Up / Down (fine) | Right / Left Trigger | P / O |
| Throttle Up / Down (step) | — | T / Shift+T |
| Flaps Down / Up | LB / RB (left / right shoulder) | F / Shift+F |
| Mixture Richer / Leaner | — | M / Shift+M |
| Magneto Clockwise / Counter-CW | — | I / Shift+I |
| Landing gear | — | G |
| Speed brakes | — | B |
| Reverse thrust on / off (MD-80: on the ground with the levers at idle; the throttle then commands reverse power, the same key stows the buckets) | — (bindable: Reverse thrust) | R |
| Wheel brakes on / off (parking brake set at every flight start) | A Button | . / , |
| Nose-wheel steering range (A-10) | — | Ctrl+W |
| Landing / taxi lights | — | L |
| Start / stop the flight model | — | Shift+R / E |
| Trim Up / Down (hold) | Y / X Button | — |
| Slew Mode | — (bindable: Slew toggle) | Shift+Y |
| Scenery editor (design / developer mode, docs/SCENERY_EDITOR.md) | — | F8; inside: WASD, E/Q or PgUp/PgDn, 1–9 tools, 0 pick a baked road, O objects, C curved, [ ] heights, Space top view, axis gizmo, Ctrl+S, Ctrl+B bake the tiles around; no haze while editing — tutorial in the doc |
| VFR Map Toggle | — | N |
| Move the VFR map frame / resize it | — | drag its title bar / drag its bottom-left or bottom-right corner (remembered in provpilot.cfg) |
| VFR Map Zoom | Wheel over map frame | − / + (=) |
| Weather overlay / FPS counter | — | Ctrl+I / Ctrl+Shift+F |
| Fullscreen / windowed (also sets Graphics > Fullscreen) | — | Alt+Enter |
| Flap Lever Drag | — | Mouse drag (cockpit) |
| Skip the intro flight | — | Space |
| Return to Welcome | — | Escape |

### Camera Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Switch Camera (Cockpit/Orbit/Chase) | B Button | C |
| Switch Camera the other way (Cockpit/Chase/Orbit) | — | Shift+C |
| Pan the view (cockpit head, orbit, chase) | — | hold the right mouse button + mouse |
| Head Look (Cockpit) | D-pad (or the Camera: Look X / Y axes) | Arrow Keys |
| Orbit/Chase Camera Adjust | D-pad (or the Camera: Look X / Y axes) | Arrow Keys |
| Zoom In (orbit/chase distance; cockpit field of view) | — (bindable: Camera: Zoom in) | Page Up |
| Zoom Out | — (bindable: Camera: Zoom out) | Page Down |
| Look around (cockpit) | — | hold Space + mouse |
| Zoom cockpit view | — | hold Space + mouse wheel |
| Move the eye point (cockpit): left / right / up / down | — | Ctrl + Arrow Keys |
| Move the eye point forward / back | — | Ctrl+Shift + Up / Down |
| Eye point back to the aircraft's default | — | Ctrl+Space |
| Cockpit knobs (Kollsman, OBS course, radio frequencies, ADF card…) | — | click zones, or mouse wheel over the knob (Shift = coarse); see docs/RADIO_NAVIGATION.md |
| Show every cockpit mouse-sense area | — | Ctrl+Shift+H |
| Recentre cockpit view (head + zoom) | — | Space (tap) |
| HUD overlay: off / readouts / readouts + coordinates | — | H |

### Vehicles (ground)
The welcome screen has a **Vehicles** frame under the aircraft: a low-poly car
and an SBB Re 4/4 locomotive (credits in `aircraft/<id>/CREDITS.txt`; the
locomotive is CC-BY-4.0 by hotdoghans, the car is the CC0 "sedan" from
Kenney's Car Kit). Fly! places the selected vehicle on the chosen runway. The car
is driven by a kinematic model on the drawn ground (`src/drive_mode.cpp`, no
physics engine; the baked road ribbons, embankments and bridge decks are part
of that ground, see [docs/VEHICLES.md](docs/VEHICLES.md)): the vehicle keys
are their own section of the keyboard mapping. The locomotive is only placed
for now.

| Control | Keyboard |
|---------|----------|
| Accelerate / brake (brake held at standstill reverses) | W / S |
| Steer left / right | A / D |
| Handbrake | B |
| Cameras | as in flight (C, arrows, Page Up / Down) |

### Slew Mode Controls
| Control | Gamepad | Keyboard |
|---------|---------|----------|
| Move Horizontal | Left Stick | W/A/S/D |
| Move Vertical | Right Stick Y | Q / E (or PgUp / PgDn) |
| Rotate (Yaw) | Right Stick X | — |
| Fast Movement | — | Hold Shift |
| Exit Slew | — | Shift+Y |

> Gamepad axes are calibrated per‑user in **Settings → Joystick**. If your right‑Y is a half‑axis (Logitech Dual Action and similar), bind a different axis to **Slew: Up/Down**, or use the keyboard `Q` / `E` keys for vertical motion. Bindings persist in `provpilot.cfg`.

### Joystick / Gamepad Configuration

Every pad input goes through **Settings → Joystick**: the flight, slew, vehicle and camera-look axes, and the buttons (flaps, trim, brakes, camera, zoom, slew toggle, handbrake, reverse thrust). Nothing is hard-wired to a button or axis number, so a pad that reports its hat or sticks differently (a Logitech F710 in D mode on Windows, for example) is fixed by re-detecting the affected rows. The gamepad never moves the focus in the menus: Godot's built-in pad navigation of the UI is switched off, so **Detect** only sees the stick or button you move.

The defaults are an Xbox layout: left stick roll / pitch, right stick X rudder, triggers throttle, A brakes, B camera, Y / X trim, shoulders flaps, D-pad look. If your `provpilot.cfg` still carries an older layout (trim on the D-pad, camera on Y), **Restore Defaults** on the Joystick tab brings it in line.

For every joystick action:

1. Click **Detect** and move the desired axis (or press the desired button), or pick it by name from the row's list (A / Cross, LB / L1, D-pad up, Left stick X, …). **None** leaves the action unassigned — a pad has fewer buttons than there are actions, so bind the ones you use.
2. The blue bar shows the **raw** axis value; the green bar shows the **calibrated** result that gameplay code receives.
3. With the stick at rest, click **Ctr**. Push to one extreme, click **Min** (or **Max**). The captured raw values define the symmetric `[-1, 1]` mapping — half‑axes work fine, just leave one endpoint equal to center.
4. Tick **Inv** if the direction is reversed. Adjust the deadzone slider so the calibrated bar reads exactly 0 at rest. Pick a response curve (Linear / Squared / Cubed) for analog sensitivity.
5. **Save** persists everything to `provpilot.cfg` (sections `axis.*` / `button.*`). **Restore Defaults** falls back to a generic SDL gamepad layout.

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
   git clone --recursive https://github.com/Q-coder/provpilot.git
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
├── src/                         # GDExtension C++ source (the game logic)
│   ├── jsbgodot.cpp/.h          # JSBSim ↔ Godot integration
│   ├── aircraft_config.cpp/.h   # aircraft.cfg parser + per-airframe path lookup
│   ├── aircraft_rig.cpp/.h      # airframe: cfg tunables, model nodes, levers, engine loop
│   ├── aircraft_model_loader.cpp/.h # runtime GLB loader (on-disk first, PCK fallback)
│   ├── aircraft_lights.cpp/.h   # exterior lights (nav, strobe, beacon, landing, taxi)
│   ├── fg_bridge.cpp/.h         # JSBSim -> FlightGear property values
│   ├── fg_controls.cpp/.h       # clickable FlightGear cockpit controls
│   ├── fg_sounds.cpp/.h         # FlightGear sound sets
│   ├── airports_lib.cpp/.h      # bundled airport catalog
│   ├── runways_lib.cpp/.h       # per-pack runways.json merge
│   ├── runways.cpp/.h           # airport surfaces + airfield lights
│   ├── live_weather.cpp/.h      # METAR fetch -> weather layers
│   ├── sky_atmosphere.cpp/.h    # sun, atmospheric sky, fog ramps
│   ├── cloud_layers.cpp/.h      # cloud layers from the weather model
│   ├── joystick_bindings.cpp/.h # JoystickConfig autoload: bindings + calibration
│   ├── aircraft_installer.cpp/.h # AircraftInstaller autoload: first-run extraction
│   ├── welcome_screen.cpp/.h    # startup aircraft / airport selection
│   ├── scenery_editor.cpp/.h    # F8 scenery editor: mode, camera, cursor, tools, UI, view gizmo
│   ├── scenery_design.cpp/.h    # scenery design document (JSON model, arcs, switches)
│   ├── scenery_design_mesher.cpp/.h # live meshes of the design
│   ├── aircraft_settings_panel.cpp/.h # Settings > Aircraft tab
│   ├── joystick_settings_panel.cpp/.h # Settings > Joystick tab
│   ├── scenery_downloader_panel.cpp/.h # Settings > Downloads tab
│   ├── control_surfaces.cpp/.h  # surface / propeller / gear animation
│   ├── cockpit_controls.cpp/.h  # lever keys + mouse picking / dragging
│   ├── camera_rig.cpp/.h        # cockpit head look, orbit, chase
│   ├── world_scenery.cpp/.h     # elevation stack, ECEF/LV95 geodesy, streaming, globe mask
│   ├── flight_spawn.cpp/.h      # first flight, teleport, deferred terrain init
│   ├── terrain_elevation.cpp/.h # post-teleport pin + JSBSim ground push
│   ├── slew_mode.cpp/.h         # Shift+Y free movement
│   ├── intro_zoom.cpp/.h        # space → spot cinematic
│   ├── flight_hud.cpp/.h        # readouts, FPS, weather overlay, intro HUD
│   ├── loading_screen.cpp/.h    # progress overlay (fallback loading view)
│   ├── loading_progress.cpp/.h  # fly-loading phases + completion
│   ├── engine_sound.cpp/.h      # RPM → pitch / volume
│   ├── fg_animator.cpp/.h       # FlightGear model animations
│   ├── vfr_map.cpp/.h           # VFR map inset
│   ├── install_config.cpp/.h    # InstallRoot autoload: install dir discovery + provpilot.cfg
│   ├── settings_store.cpp/.h    # Settings model, provpilot.cfg I/O, scenery library logic
│   ├── settings_screen.cpp/.h   # Tabbed settings UI (CanvasLayer)
│   └── register_types.cpp/.h    # GDExtension registration
├── provpilot/                   # Godot project
│   ├── node_3d.gd               # Main scene script: wires the C++ modules, sequences welcome → Fly! → flight
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
├── scripts/                     # Tooling (see scripts/README.md for the full index)
│   ├── build_world_*.py/.sh         # Worldwide base tier: SRTM terrain, airports, runways, roads
│   ├── runway_plateau.py            # Runway plateaus baked into the base terrain
│   ├── scenery_pipeline.py          # Swiss detail tier: terrain + buildings (swisstopo)
│   ├── streaming_terrain_generator.py # Swiss 2 m/px heightmap tiles, one tile at a time
│   ├── swissbathy3d.py              # Lake bathymetry overlay tiles
│   ├── generate_orthophoto_tiles.py # Orthophoto tiles (WMS); process_swissimage_orthophoto.py (STAC)
│   ├── generate_terrain_mipmap.py   # Terrain / orthophoto mip pyramids
│   ├── generate_roads.py, generate_water.py, fix_water_bake.py   # swissTLM3D roads, water
│   ├── generate_vfr_map_tiles.py    # VFR map raster tiles
│   ├── bundle_scenery_pack.py, generate_scenery_manifest.py   # Scenery packs for the in-app downloader
│   ├── convert_msfs_scenery.py      # MSFS 2020 scenery → GLB converter
│   ├── fg_a10_build.py, fg_panel_build.py, fg_a10_sounds.py   # FlightGear aircraft ports (Blender)
│   ├── glb_tool.py, make_loop.py, pck_list.py                  # GLB pack/inspect/validate, sound loops, .pck listing
│   ├── llm_delegate.py              # Hand a subtask to the remote model (docs/AGENTS_TWO_MACHINE.md)
│   ├── build_macos.sh, build_windows.ps1, setup_windows.ps1    # Build, sign and assemble
│   └── world_regions.json, scenery_regions.json               # Region definitions
├── docs/                        # Additional documentation
│   ├── AIRCRAFT_CONFIG.md      # Per-aircraft aircraft.cfg format & systems
│   ├── SCENERY_ARCHITECTURE.md  # Scenery data architecture & formats
│   ├── TERRAIN_SETUP.md        # Terrain & collision architecture
│   ├── WORLD3D.md              # Global (world3d) globe integration
│   └── WINDOWS_SETUP.md        # Windows build setup guide
├── jsbsim/                      # JSBSim flight dynamics (submodule)
├── godot-cpp/                   # Godot C++ bindings (submodule)
└── SConstruct                   # Build configuration
```

## Portable Install Layout

ProvPilot follows an **X-Plane-style portable install**: the user copies the
`.app` (or `ProvPilot.exe` on Windows) into a folder of their choice anywhere
on disk, and everything mutable lives next to it. The whole tree can be moved
between machines by copying the folder.

```
<install_root>/
  ProvPilot.app/            # Signed Godot export (macOS)         — or —
  ProvPilot.exe             # Windows executable
  provpilot.cfg             # All runtime settings (paths, joystick, graphics, …)
  aircraft/                 # Auto-extracted from the PCK on first run
    c172p/
      jsbsim/               # JSBSim aircraft / engine / systems XML
      model/
        C172P_1.glb         # 3D model loaded from disk via GLTFDocument
        license.txt
  scenery/                  # User-managed via in-app downloader
    terrain/ orthophoto/ buildings/ roads/ water/ poi/
```

Three autoloads coordinate this on startup (see `provpilot/project.godot`):

1. **`InstallRoot`** — a C++ `InstallConfig` node (`src/install_config.cpp`,
   mounted as an autoload through `install_root.tscn`) that discovers
   `<install_root>` (the directory containing the `.app` / `.exe`),
   loads/saves `provpilot.cfg`, and exposes `get_aircraft_root()` /
   `get_aircraft_dir(id)` helpers consumed by GDScript and the other C++
   modules (`SettingsStore`, `src/jsbgodot.cpp`).
2. **`AircraftInstaller`** — on first launch (or when `BUNDLE_VERSION` bumps)
   extracts `res://aircraft/` from the PCK to `<install_root>/aircraft/`.
   Skips editor-only sidecars (`.import`, `.uid`, `.blend`, loose textures);
   only the runtime-needed `.glb` + JSBSim XML lands on disk.
3. **`JoystickConfig`** — bindings + calibration, persisted to the shared
   `provpilot.cfg`.

The 3D model is shipped in the PCK with `importer="keep"` (raw bytes
preserved) and loaded at runtime via `GLTFDocument.append_from_file()` from
the on-disk path — see `src/aircraft_model_loader.cpp`. This means users can
replace `aircraft/c172p/model/C172P_1.glb` without rebuilding the app.

### Quick Build (macOS)

```bash
./scripts/build_macos.sh
```

Builds the GDExtension, exports the Godot project, signs with Developer ID,
and packages a notarizable DMG. Aircraft data and the C172P GLB are baked
into the PCK and extracted on first run — no sibling `jsbsim/` folder is
required anymore.

See [docs/MACOS_SETUP.md](docs/MACOS_SETUP.md) and
[docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md) for build prerequisites.

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
5. The **intro zoom** starts at once over the destination (space → spot view) and shows the loading status (terrain, flight model, "Stabilizing aircraft…") on its HUD, with one progress bar per phase, while tiles, buildings, roads, water, and orthophotos stream in; **Space** jumps to the spot view
6. Once ready, the simulation begins at the selected airport and the zoom hands over to the spot camera (it waits on the spot view if loading takes longer than the zoom)

Press **Escape** during flight to return to the welcome screen.

## Settings

Open the settings screen from the welcome screen's Settings button. It is a C++
GDExtension module (`SettingsScreen` + `SettingsStore`, `src/settings_*.cpp`);
values persist to the portable `provpilot.cfg` through the `InstallRoot` autoload.
One tab per subject:

| Tab | Settings |
|---------|----------|
| **Scenery** | FSX-style per-region scenery library (priority, enable, scan/add/remove, skip white pixels) — see [docs/SCENERY_LIBRARY.md](docs/SCENERY_LIBRARY.md) |
| **Downloads** | One-click install of official region packs from https://www.md80.ch/provpilot/scenery/ — see [docs/SCENERY_DISTRIBUTION.md](docs/SCENERY_DISTRIBUTION.md) |
| **Graphics** | Render distance (2–16 tiles), building distance (1–8 tiles), shadow quality (Off/Low/Medium/High), tree density, VSync, fullscreen |
| **World** | System clock vs. manual time of day (drives the sun) |
| **Map** | VFR map frame on/off and frame size |
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

Hand-placed models live in an `objects/` folder of a scenery library area (`scenery/custom/<name>/objects/` with GLB files and a `package.json`). The C++ `SceneryObjects` node places them on the precise terrain by proximity, and the area's exclusion zones remove the buildings and trees of the areas below it in the Scenery Library, so the highest entry wins.

See [docs/SCENERY_ARCHITECTURE.md](docs/SCENERY_ARCHITECTURE.md) for the package.json format.

## MSFS Scenery Import

Microsoft Flight Simulator 2020 scenery packages can be converted to Godot-compatible GLB:

```bash
python3 scripts/convert_msfs_scenery.py /path/to/msfs/package --output scenery/custom/my_scenery/
```

The converter reads the models embedded in the package's `modelLib.BGL`, decodes
Asobo's packed vertex format, converts the DDS textures to PNG, strips the
MSFS-specific extensions and writes one GLB per model plus a `package.json`
with every placement of the package (from the scenery BGL). Check the result
with `scripts/glb_tool.py validate` and look at a model with
`godot --path provpilot -s tests/render_glb.gd -- model.glb out.png`.

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

ProVPilot is released under the GNU General Public License, version 2
(`LICENSE`). Every third-party component, aircraft and data source with its
licence is listed in [NOTICE.md](NOTICE.md).

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
- **Pilatus PC-7**: [helijah](https://sketchfab.com/helijah) — [Pilatus PC-7 Mk I on Sketchfab](https://sketchfab.com/3d-models/pilatus-pc-7-mk-i-b13c98ceef134febac09dacc51c005f7), CC-BY-4.0
