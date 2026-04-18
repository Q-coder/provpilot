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

   The Scenery3D GDExtension dylib is already checked in under
   `provpilot/bin/libscenery3d.macos.template_debug.double.arm64.dylib`;
   rebuild it from the submodule at `provpilot/addons/scenery3d/` only if
   you change the extension source.

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

The export preset in `provpilot/export_presets.cfg` is configured for:
- Code signing: Developer ID Application (DUWB54DV3A)
- Bundle ID: `ch.md80.provpilot`
- Minimum macOS: 12.0 (update to 14.0 to match the engine)
