# Windows Development Setup

Guide for building and running ProvPilot on Windows (tested on Windows 11, MSVC 2019).

## Prerequisites

- **Visual Studio 2019** (or newer) with C++ Desktop workload
- **Godot 4.7** (standard build, not .NET)
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

   > ⚠️ **Rebuild JSBSim after any `jsbsim` submodule bump** (e.g. during a
   > Godot major-version upgrade). The prebuilt `JSBSim.lib` is linked into
   > `libprovpilot`; if the compiled lib is older than the `jsbsim/src`
   > headers, the `FGFDMExec` layout mismatch produces a garbage `GetIC()`
   > pointer → crash at startup, and a corrupted root dir →
   > `Could not open file: aircraft/c172p/c172p.xml`. Delete the build dir
   > (or at least the `*.obj` and `JSBSim.lib`) and rebuild from the current
   > submodule before rebuilding the GDExtension.

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
- Defines `_USE_MATH_DEFINES` (MSVC's `<cmath>` only provides `M_PI` & co. with
  this set before any system header; without it `vfr_map.cpp` fails with
  `error C2065: 'M_PI': undeclared identifier`). Do not rely on a per-file
  `#define` for this — it silently stops working if an include lands above it.
- Links `ws2_32` (Winsock, required by JSBSim)
- Adds `#ifndef _WIN32` guard for `unistd.h`

### Build environment gotchas

- `scons` and `python` must be on `PATH`. If they were installed via Miniconda
  (`C:\ProgramData\miniconda3`), a plain shell does not see them — prepend
  `C:\ProgramData\miniconda3` and `C:\ProgramData\miniconda3\Scripts` to `PATH`
  (or run from an Anaconda Prompt).
- `scripts/build_windows.ps1` runs with `$ErrorActionPreference = 'Stop'`, so any
  stderr output from `vcvars64.bat` (e.g. a harmless
  `'vswhere.exe' is not recognized ...` line) aborts the script at its vcvars step.
  Workaround: run `vcvars64.bat` in `cmd` first and launch the script from that
  shell — with `cl.exe` already on `PATH` the script skips its own vcvars step.

### Performance Tuning

Scenery3D handles terrain, building, road, and water streaming on its own
background thread pool (4 workers by default) with budgeted mesh generation
on the main thread. Tune visible radius via the `[scenery3d]` section of
`project.godot` (`load_radius`, `far_radius`, `detail_radius`).

JSBSim debug output is suppressed with `SetDebugLevel(0)`; per-frame C++
printfs were removed from `copy_outputs_from_JSBSim()`.

## Distribution / Portable Install

ProvPilot follows an X-Plane-style portable install model: the user copies the
folder containing `ProvPilot.exe` anywhere on disk and the app keeps all of
its state in that folder.

**Ship-time layout** (what `build_windows.ps1` packs into the ZIP):

```
ProvPilot\
  ProvPilot.exe
  ProvPilot.pck      — contains res://aircraft/<id>/ subtree (JSBSim XMLs +
                       imported .glb scenes for the 3D model)
  README.txt
  bin\               — GDExtension DLLs (libprovpilot, libscenery3d)
```

There is **no** top-level `jsbsim\` folder anymore. Aircraft data is bundled
inside the PCK and unpacked on first run.

**First-run layout** (created by the app):

```
ProvPilot\
  provpilot.cfg                                — portable settings
  aircraft\c172p\
    jsbsim\{aircraft,engine,systems}\...      — extracted XMLs
    model\license.txt                          — only files DirAccess can
                                                 enumerate land on disk
```

The `aircraft\` extraction is driven by
[`AircraftInstaller`](../provpilot/aircraft_installer.gd) (autoload) and keyed
by `BUNDLE_VERSION`. Bump that constant whenever the bundled data changes to
force a re-extraction on existing installs.

### Loading the 3D model

[`c172p_loader.gd`](../provpilot/c172p_loader.gd) prefers an on-disk
`<install>\aircraft\c172p\model\C172P_1.glb` so users can swap the model.
In exported builds Godot replaces source `.glb` files with the compiled
`.scn` and the source is not reachable via `FileAccess`, so the loader
falls back to `load("res://aircraft/c172p/model/C172P_1.glb")` which
goes through the `.import` remap to the embedded PackedScene.

### Discovery

The `InstallRoot` autoload (C++ `InstallConfig`, [`src/install_config.cpp`](../src/install_config.cpp),
mounted via `provpilot/install_root.tscn`) tries (in order): the
directory containing the executable, `$PWD`, `%USERPROFILE%\ProvPilot`, and
finally the user data directory. The first writable candidate becomes the
install root and `provpilot.cfg` is written there.

### Native JSBSim path

[`src/jsbgodot.cpp`](../src/jsbgodot.cpp) (`JSBGodot::_ready`) reads the
resolved aircraft directory from the `InstallRoot` autoload and points
`FGFDMExec` at `<install>\aircraft\<id>\jsbsim\{aircraft,engine,systems}`.
No environment variables or absolute paths are baked into the binary.
