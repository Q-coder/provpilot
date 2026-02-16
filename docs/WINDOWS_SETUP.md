# Windows Development Setup

Guide for building and running ProvPilot on Windows (tested on Windows 11, MSVC 2019).

## Prerequisites

- **Visual Studio 2019** (or newer) with C++ Desktop workload
- **Godot 4.6** (standard build, not .NET)
- **SCons** (`pip install scons`)
- **CMake** (included with Visual Studio)
- **Python 3** (for scenery pipeline scripts)
- **Windows Developer Mode** (optional, for native Git symlinks)

## Quick Start

1. **Clone with submodules:**
   ```
   git clone --recurse-submodules <repo-url>
   cd provpilot
   ```

2. **Run the Windows setup script** (creates directory junctions for scenery assets):
   ```powershell
   .\scripts\setup_windows.ps1
   ```

3. **Build JSBSim** (from a VS Developer Command Prompt / vcvars64):
   ```powershell
   cd jsbsim
   mkdir build; cd build
   cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ..
   ```

   **Important — patch CRT flags** before building (Godot uses /MT static CRT):
   ```powershell
   Get-ChildItem -Recurse -Filter "flags.make" | ForEach-Object {
       (Get-Content $_.FullName) -replace '-MD', '-MT' | Set-Content $_.FullName
   }
   Get-ChildItem -Recurse -Filter "*.obj" | Remove-Item -Force
   nmake
   ```

4. **Build the GDExtension DLL:**
   ```powershell
   cd <repo-root>
   scons arch=x86_64
   ```

5. **Install Terrain3D plugin** — download Windows DLLs from
   [Terrain3D releases](https://github.com/TxturaDesign/Terrain3D/releases) (v1.0.1)
   and place them in `provpilot/addons/terrain_3d/bin/`:
   - `libterrain.windows.debug.x86_64.dll`
   - `libterrain.windows.release.x86_64.dll`

6. **Open Godot** — first launch will reimport ~7,500 GLB files (buildings, roads, water).
   This is a one-time process and takes several minutes.

## Cross-Platform Notes

### Symlinks / Directory Junctions

On macOS, `provpilot/Assets/roads` and `provpilot/Assets/water` are symlinks pointing
to `scenery/roads` and `scenery/water`. On Windows, Git stores these as plain text
files (unless `core.symlinks = true`).

The `setup_windows.ps1` script replaces them with **NTFS directory junctions**, which
work without admin rights and are transparent to Godot's `res://` path resolution.

| Asset     | Junction source               | Junction target        |
|-----------|-------------------------------|------------------------|
| Buildings | `provpilot/Assets/buildings`  | `scenery/buildings/`   |
| Roads     | `provpilot/Assets/roads`      | `scenery/roads/`       |
| Water     | `provpilot/Assets/water`      | `scenery/water/`       |

### CRT Linking

Both JSBSim and godot-cpp must use the same C Runtime. Godot uses `/MT` (static CRT).
JSBSim's CMake defaults to `/MD` (dynamic CRT), causing linker errors. The build
instructions above patch `flags.make` after CMake configure to fix this.

### Build Outputs

The SConstruct automatically handles Windows-specific settings:
- Defines `JSBSIM_STATIC_LINK` (required for static JSBSim.lib)
- Links `ws2_32` (Winsock, required by JSBSim)
- Adds `#ifndef _WIN32` guard for `unistd.h`

### Performance Tuning

The following parameters were tuned for Windows (also benefit macOS):
- **Terrain streamer**: `load_radius=8`, `REBASE_TILES_PER_FRAME=4`, `IMMEDIATE_TILES_INITIAL=9`
- **Building/Road/Water loaders**: tile distance checks throttled to every 30 frames
- **JSBSim**: debug output suppressed with `SetDebugLevel(0)`
- **C++ printf**: per-frame debug prints removed from `copy_outputs_from_JSBSim()`
