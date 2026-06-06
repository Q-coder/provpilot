# macOS Development Setup (Apple Silicon — M4 Max Optimized)

Guide for building and running ProvPilot on macOS with a Metal-only
double-precision Godot engine optimized for Apple M4 Max. Based on the
`4.6-macdev` branch of the Godot fork.

## Prerequisites

- **macOS 14.0+** (Sonoma or newer)
- **Apple Silicon Mac** (M1 or newer; optimized for M4 Max)
- **Xcode Command Line Tools** (`xcode-select --install`)
- **Python 3** with venv (`python3 -m venv .venv`)
- **SCons** (`pip install scons` inside the venv)
- **CMake** (`brew install cmake`)

## Quick Start

1. **Clone with submodules:**
   ```bash
   git clone --recurse-submodules <repo-url>
   cd provpilot
   ```

2. **Set up Python environment:**
   ```bash
   python3 -m venv .venv
   source .venv/bin/activate
   pip install scons
   ```

3. **Build JSBSim:**
   ```bash
   cd jsbsim
   mkdir -p build && cd build
   cmake .. -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
   make -j$(sysctl -n hw.ncpu)
   cd ../..
   ```

4. **Build Godot (Metal-only, double precision, M4 Max optimized):**
   ```bash
   source .venv/bin/activate
   cd godot
   scons platform=macos arch=arm64 target=editor precision=double \
     vulkan=no opengl3=no metal=yes \
     debug_symbols=yes \
     -j$(sysctl -n hw.ncpu)
   ```

5. **Regenerate godot-cpp bindings for double precision** (first build only):
   ```bash
   cd godot-cpp/gdextension
   ../../godot/bin/godot.macos.editor.double.arm64 --headless \
     --dump-extension-api extension_api.json
   cd ../..
   ```

6. **Build the ProvPilot GDExtension** (must match engine precision):
   ```bash
   scons platform=macos arch=arm64 precision=double -j$(sysctl -n hw.ncpu)
   ```

   The Scenery3D GDExtension dylib is **not** committed (it lives in the
   gitignored `provpilot/bin/`). Build it from the submodule at
   `provpilot/addons/scenery3d/` after the first checkout and whenever the
   submodule pointer is bumped or you change the extension source:

   ```bash
   cd provpilot/addons/scenery3d
   git submodule update --init --recursive            # first time only
   # The double-precision extension_api.json must match the engine; copy
   # it from the top-level godot-cpp checkout the first time and after a
   # Godot upgrade:
   cp ../../../godot-cpp/gdextension/extension_api.json \
      godot-cpp/gdextension/extension_api.json
   scons platform=macos arch=arm64 precision=double target=template_debug \
     -j$(sysctl -n hw.ncpu)
   cp demo/bin/libscenery3d.macos.template_debug.double.arm64.dylib \
      ../../bin/
   ```

   > This builds the **`template_debug`** dylib used by the editor. The
   > **`template_release`** dylib used by the shipped app is built
   > automatically by `scripts/build_macos.sh` (see *Export Build* below),
   > so you do **not** need to build it by hand before packaging.

7. **Run the editor:**
   ```bash
   godot/bin/godot.macos.editor.double.arm64 --editor --path provpilot
   ```

## What's Different: 4.6-macdev Branch

The `4.6-macdev` branch contains macOS-specific optimizations to the Godot 4.6 engine:

### Build System (`platform/macos/detect.py`)

| Change | Detail |
|--------|--------|
| Metal-only rendering | Vulkan/MoltenVK and D3D12 backends removed (`vulkan=no opengl3=no`) |
| `-mcpu=apple-m4` | Compiler targets M4 Max instruction scheduling and branch prediction |
| ThinLTO default | Link-time optimization enabled by default (was `none`) for cross-module inlining |
| `-ffp-contract=fast` | Fused multiply-add enabled (was `off`), leverages M4 FMA units |
| macOS 14.0 minimum | Raises deployment target from 11.0 to 14.0, unlocking MSL 3.1 and native image atomics |

### Metal Driver (`drivers/metal/rendering_device_driver_metal.mm`)

| Change | Detail |
|--------|--------|
| Residency sets (macOS 15+) | GPU resources are pre-committed to a `MTLResidencySet` on creation, reducing page faults during terrain tile streaming. Gracefully skipped on macOS 14. |

### Double Precision

| Change | Detail |
|--------|--------|
| `precision=double` builds | Real_t is 64-bit everywhere (Transform3D origin, Node3D position, physics). Eliminates floating-origin jitter across Switzerland-scale worlds. |
| Mandatory for all GDExtensions | Scenery3D, ProvPilot, and any other extension loaded by the editor must also be built `precision=double` — mixing single and double precision dylibs crashes the editor at GDExtension verification. |

### Scenery3D Integration

| Change | Detail |
|--------|--------|
| Single node, no rebase | `provpilot/node_3d.tscn` uses one `Scenery3D` node. All tile lifecycle, LOD, and elevation queries happen on C++ background threads. |
| No floating origin | `ORIGIN_REBASE_THRESHOLD` and the rebase call sites in `node_3d.gd` were removed — double-precision world coordinates stay stable. |
| Loading screen polls progress | `node_3d.gd::_update_loading_progress()` samples `Scenery3D.get_elevation()` on a 5-point ring around the spawn position; the bar advances as the near-terrain tiles stream in. The C++ tile manager throttles JPEG decode and texture upload to `ORTHO_DECODES_PER_FRAME = 3` and `CHUNKS_PER_FRAME = 1`, so the initial fly-to-airport load can take many seconds — visible to the user instead of stalling silently. |
| Chunk → tile hand-off | A far-terrain chunk is dropped only after every individual tile inside its footprint that lies within `load_radius` has finished loading (or is marked `no_data`), preventing the world-gap that used to appear when the camera crossed a chunk boundary. |

## Distribution / Portable Install

ProvPilot follows an X-Plane-style portable install model: the user drops the
`.app` bundle anywhere (or alongside their `scenery/` folder) and the app keeps
all of its state in that location.

**Ship-time layout** (what `build_macos.sh` produces):

```
ProvPilot.app/                       — self-contained, code-signed bundle
  Contents/MacOS/ProvPilot
  Contents/Resources/ProvPilot.pck   — contains res://aircraft/<id>/ subtree
                                       (JSBSim XMLs + imported .glb scenes)
  Contents/Frameworks/*.dylib        — GDExtension dylibs
```

There is **no** top-level `jsbsim/` folder shipped — the macOS build script
explicitly skips bundling it (see step 3 in [`scripts/build_macos.sh`](../scripts/build_macos.sh)).
Aircraft data is bundled inside the PCK and unpacked on first run.

**First-run layout** (created by the app next to the `.app`):

```
<install_root>/
  ProvPilot.app/
  provpilot.cfg                                — portable settings
  aircraft/c172p/
    jsbsim/{aircraft,engine,systems}/...      — extracted XMLs
    model/license.txt                          — only files DirAccess can
                                                 enumerate land on disk
```

The `aircraft/` extraction is driven by
[`AircraftInstaller`](../provpilot/aircraft_installer.gd) (autoload) and keyed
by `BUNDLE_VERSION`. Bump that constant whenever the bundled data changes to
force a re-extraction on existing installs.

### Loading the 3D model

[`c172p_loader.gd`](../provpilot/c172p_loader.gd) prefers an on-disk
`<install>/aircraft/c172p/model/C172P_1.glb` so users can swap the model.
In exported builds Godot replaces source `.glb` files with the compiled
`.scn` and the source is not reachable via `FileAccess`, so the loader
falls back to `load("res://aircraft/c172p/model/C172P_1.glb")` which
goes through the `.import` remap to the embedded PackedScene.

### Discovery

[`install_root.gd`](../provpilot/install_root.gd) walks up from the
`.app/Contents/MacOS/ProvPilot` exec path to the bundle's parent directory.
That folder — wherever the user dropped the `.app` — becomes the install
root. `$PWD`, `~/ProvPilot`, and the user data directory are fallbacks.

### Native JSBSim path

[`src/jsbgodot.cpp`](../src/jsbgodot.cpp) (`JSBGodot::_ready`) reads the
resolved aircraft directory from the `InstallRoot` autoload and points
`FGFDMExec` at `<install>/aircraft/<id>/jsbsim/{aircraft,engine,systems}`.
No environment variables or absolute paths are baked into the dylib.

## Performance Impact

Compared to stock Godot 4.6 on M4 Max:

- **Initialization:** Buildings and scenery load visibly faster (no pop-in)
- **Shader compilation:** Faster due to direct Metal path (no SPIRV→MoltenVK→Metal translation)
- **Steady-state rendering:** Improved frame pacing from ThinLTO and FMA
- **Terrain streaming:** All I/O happens on Scenery3D's 4-thread worker pool; the main thread only does budgeted mesh generation (200K vertex/frame cap).

## Profiling

Use **Instruments** (from Xcode) with the **Game Performance** or **Metal System Trace** template to profile GPU performance. Key metrics:

- GPU command buffer max time (target: < 16.6ms for 60fps)
- Metal Resource Events (texture/buffer allocation bursts during re-base)
- Encoder time distribution across render passes

## Export Build

```bash
source .venv/bin/activate
cd godot
scons platform=macos arch=arm64 target=template_release precision=double \
  vulkan=no opengl3=no metal=yes \
  -j$(sysctl -n hw.ncpu)
```

Once the engine `template_release` binary exists, package the app with:

```bash
./scripts/build_macos.sh
```

This script rebuilds **both** GDExtension `template_release` dylibs from source
— `libprovpilot` and `libscenery3d` (from the `provpilot/addons/scenery3d`
submodule) — copies them into `provpilot/bin/`, exports the Godot project,
code-signs the bundle with the Developer ID, and produces the app-only DMG.
Building the scenery engine inside the script guarantees the shipped app can
never lag behind the source (an earlier release shipped a stale scenery dylib
and rendered only terrain + orthophotos). The script stops at the signed DMG;
**notarization and stapling are a separate manual step** (see below).

The export preset in `provpilot/export_presets.cfg` is configured for:
- Code signing: Developer ID Application (DUWB54DV3A)
- Bundle ID: `ch.md80.provpilot`
- Minimum macOS: 12.0 (update to 14.0 to match the engine)
