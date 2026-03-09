# macOS Development Setup (Apple Silicon — M4 Max Optimized)

Guide for building and running ProvPilot on macOS with a Metal-only Godot engine
optimized for Apple M4 Max. Based on the `4.6-macdev` branch of the Godot fork.

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

4. **Build Godot (Metal-only, M4 Max optimized):**
   ```bash
   source .venv/bin/activate
   cd godot
   scons platform=macos arch=arm64 target=editor \
     vulkan=no opengl3=no metal=yes \
     debug_symbols=yes \
     -j$(sysctl -n hw.ncpu)
   ```

5. **Build the ProvPilot GDExtension:**
   ```bash
   cd ..
   scons platform=macos arch=arm64 target=template_debug -j$(sysctl -n hw.ncpu)
   ```

6. **Run the editor:**
   ```bash
   godot/bin/godot.macos.editor.arm64 --editor --path provpilot
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

### Terrain Streaming (`provpilot/terrain_streamer.gd`)

| Change | Detail |
|--------|--------|
| Zero-copy region recycling | On floating-origin rebase, existing Terrain3DRegion objects are reused in-place via `set_location()` + `add_region()` — no `Image.duplicate()`, no `Terrain3DRegion.new()`, no `sanitize_maps()`. Eliminates ~2.2 GB of memory copies per rebase (289 tiles × 8 MB height+color). |
| Ghost-safe tile loading | Removed `has_region()` / `remove_regionl()` guard from `_load_tile()` to prevent ghost entries in Terrain3D's internal `_regions` dict from accidentally removing active regions after a rebase. `add_region()` safely overwrites. |

### What Was Tested and Reverted

- **Untracked hazard mode for GPU-only textures** — Godot's frame graph does not fully manage all texture read-after-write dependencies, causing flickering artifacts.
- **Incremental rebase (batched `update_maps()`)** — Each `update_maps()` call rebuilds ALL texture arrays regardless of batch size, so batching multiplied cost instead of amortizing it, and left large untextured areas during fill-in.

## Performance Impact

Compared to stock Godot 4.6 on M4 Max:

- **Initialization:** Buildings and scenery load visibly faster (no pop-in)
- **Terrain re-base:** No perceptible pause — zero-copy region recycling eliminates the ~800ms stall from Image.duplicate() × 289 + sanitize_maps() × 289. Only new border tiles (~25 per 1024m shift) require disk I/O.
- **Shader compilation:** Faster due to direct Metal path (no SPIRV→MoltenVK→Metal translation)
- **Steady-state rendering:** Improved frame pacing from ThinLTO and FMA
- **Known limitation:** Minor micro-stutters remain during `update_maps()` — this rebuilds GPU texture arrays for all 289 regions on the main thread. Godot's single-threaded rendering submission prevents async texture uploads.

## Terrain3D Compatibility

Terrain3D is a GDExtension that uses Godot's RenderingDevice abstraction — it does not depend on Vulkan or any specific backend. All terrain features work unchanged:

- Heightmap tile streaming
- Texture splatting and LOD
- Region load/unload with zero-copy rebase recycling
- Distant terrain mesh

## Profiling

Use **Instruments** (from Xcode) with the **Game Performance** or **Metal System Trace** template to profile GPU performance. Key metrics:

- GPU command buffer max time (target: < 16.6ms for 60fps)
- Metal Resource Events (texture/buffer allocation bursts during re-base)
- Encoder time distribution across render passes

## Export Build

```bash
source .venv/bin/activate
cd godot
scons platform=macos arch=arm64 target=template_release \
  vulkan=no opengl3=no metal=yes \
  -j$(sysctl -n hw.ncpu)
```

The export preset in `provpilot/export_presets.cfg` is configured for:
- Code signing: Developer ID Application (DUWB54DV3A)
- Bundle ID: `ch.md80.provpilot`
- Minimum macOS: 12.0 (update to 14.0 to match the engine)
