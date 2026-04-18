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

4. **Build Godot (double precision)** and the ProvPilot GDExtension:
   ```powershell
   cd godot
   scons platform=windows arch=x86_64 target=editor precision=double
   cd ..\godot-cpp\gdextension
   ..\..\godot\bin\godot.windows.editor.double.x86_64.exe --headless ^
     --dump-extension-api extension_api.json
   cd ..\..
   scons arch=x86_64 precision=double
   ```

   All GDExtensions (ProvPilot, Scenery3D) must match the engine's
   `precision=double` or the editor will crash at GDExtension verification.

5. **Scenery3D** — a Windows double-precision build of the Scenery3D
   extension DLL must live at
   `provpilot/bin/libscenery3d.windows.template_debug.double.x86_64.dll`.
   Build it from the submodule at `provpilot/addons/scenery3d/` or copy
   a pre-built DLL from a teammate.

6. **Open Godot** — first launch will import textures, audio, and other
   assets. Scenery (terrain, buildings, roads, water) streams at runtime
   through Scenery3D and does **not** go through Godot's import pipeline.

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

Scenery3D handles terrain, building, road, and water streaming on its own
background thread pool (4 workers by default) with budgeted mesh generation
on the main thread. Tune visible radius via the `[scenery3d]` section of
`project.godot` (`load_radius`, `far_radius`, `detail_radius`).

JSBSim debug output is suppressed with `SetDebugLevel(0)`; per-frame C++
printfs were removed from `copy_outputs_from_JSBSim()`.
