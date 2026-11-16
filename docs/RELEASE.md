# Releasing ProvPilot

The checklist for a numbered release on both platforms, as run for 1.0.20
(2026-11-01). Platform specifics live in [MACOS_SETUP.md](MACOS_SETUP.md)
and [WINDOWS_SETUP.md](WINDOWS_SETUP.md); the base scenery packs in
[SCENERY_DISTRIBUTION.md](SCENERY_DISTRIBUTION.md) and
[WORLD_SCENERY_DESIGN.md](WORLD_SCENERY_DESIGN.md) §11 / §13.

Hosting: `https://www.md80.ch/provpilot/` (ISP web space, nginx). The app
reads `releases/latest.json` for the update notice on the welcome screen and
`scenery_manifest.json` for Settings → Downloads.

## 1. Branch and version

1. Everything merges to `main`; `main` is what the Windows machine pulls.
2. The version lives in three places, all bumped together:
   - `provpilot/project.godot` `config/version` — the build scripts derive the
     DMG / ZIP names from it and the app reports it;
   - `provpilot/export_presets.cfg` `application/short_version` +
     `application/version` (macOS) and `application/file_version` +
     `application/product_version` (Windows, four components);
   - `src/aircraft_installer.h` `BUNDLE_VERSION` only when the bundled
     aircraft data changed (it forces a re-extraction on existing installs;
     1.0.20 ships bundle 1.0.12, 1.1.0 bundle 1.1.0, 1.1.1 bundle 1.1.1).
3. The two export presets must carry the same `include_filter`: the aircraft
   folders hold non-resource files (JSBSim XML, `sounds/*.xml`, the livery
   PNGs behind a `.gdignore`, `model/*.json`) that Godot only packs when the
   filter names them. Verify with a pack export and list it:
   ```bash
   godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot \
       --export-pack "Windows Desktop" /tmp/pck.zip && unzip -l /tmp/pck.zip | grep aircraft/md80/sounds
   ```
   Nothing below `provpilot/addons/scenery3d/` reaches the pack (the addon
   folder has a `.gdignore`; an `include_filter` does not help). Its shaders
   are compiled into the library instead (`S3DShaders`, generated header from
   `shaders/*.gdshader` by the addon's SConstruct; `smoke_embedded_shaders`).
   The first 1.1.0 build drew untextured trees for this reason.
   Sound sets: `FGSounds` reads the raw `.wav` from the installed aircraft
   folder, so every `aircraft/*/sounds/*.wav` needs a keep-mode `.import`
   (tracked, see `.gitignore`) and the filter entry `aircraft/*/sounds/*.wav`;
   with Godot's default wav import only the `.sample` is packed and a fresh
   install has no sound (1.1.0). Check: the clean-install smoke below must
   leave `.wav` files in `<install root>/aircraft/a10/sounds/`.
4. Airport catalog: after a runway rebuild of any region run
   `.venv/bin/python scripts/sync_catalog_runways.py`, so the bundled
   `provpilot/airports.json` lists every runway (with thresholds) before a
   scenery pack is installed.
5. Submodules: a release builds from the recorded gitlinks. Commit and push
   `provpilot/addons/scenery3d` first, then the parent's pointer; the engine
   (`godot`, `godot-cpp`, `jsbsim`) rarely moves — if it did, both machines
   rebuild it (MACOS_SETUP.md "Engine upgrade", WINDOWS_SETUP.md step 4).
6. `README.txt` (shipped in the Windows ZIP) names the aircraft and the
   controls: keep it current.

## 2. macOS

```bash
./scripts/build_macos.sh
```
Builds the three release dylibs (libprovpilot, libscenery3d, libworld3d from
the sibling `../world3d` checkout), exports, signs with the Developer ID by
hash, builds and signs `build/ProvPilot-<version>-arm64.dmg`, prints the
`latest.json` block, and notarizes + staples through the keychain profile
`provpilot-notary`. Needs `godot/bin/godot.macos.template_release.double.arm64`
(the custom double-precision template) next to the editor binary.

Smoke the export, not the editor run (the editor cannot load the release
dylibs): copy the app out of `build/` so the first-run extraction does not
land inside the signed bundle, run the binary from a terminal, and check the
log for the extension load lines and the aircraft extraction, then fly once.
Do it once as a clean install too: `HOME=<empty dir>` and no `provpilot.cfg`
next to the app (the first 1.1.0 build showed the default cockpit before the
welcome screen only then, during the switch to fullscreen).

## 3. Windows

On the Windows machine (PowerShell, from the repo root):
```powershell
git checkout main; git pull; git submodule update --init --recursive
cd ..\world3d; git pull; cd ..\provpilot
.\scripts\build_windows.ps1 -SkipJSBSim -SkipGodot -SkipGodotCpp
```
Skip the engine steps only when the `godot`, `godot-cpp` and `jsbsim`
gitlinks did not move since the last Windows build (`git diff <last-tag> HEAD
-- godot godot-cpp jsbsim`); the script always rebuilds the ProvPilot,
Scenery3D and world3d DLLs and packs `build\ProvPilot-Windows-<version>.zip`.
Run `build\ProvPilot.exe` once from a console window before shipping: the
console build prints the extension load lines and the aircraft extraction.

Code written on the Mac must build with MSVC: no VLAs, no POSIX headers
outside `#ifndef _WIN32`, `_USE_MATH_DEFINES` comes from the SConstruct.

## 4. Base scenery packs

The nine base-tier regions (`scripts/world_regions.json`) are distributed as
one ZIP each through Settings → Downloads:
```bash
python3 scripts/bundle_world_pack.py --all --version 2 --manifest   # 1.1.0: v2 with vegetation/ and map_overview/
python3 scripts/bundle_world_pack.py --manifest-only           # after re-packing one region
```
Output `/Volumes/Data1/provpilot/build/world-packs/<id>-base-v<ver>.zip` plus
`scenery_manifest.json`. Upload the ZIPs to `provpilot/scenery/` and the
manifest to `provpilot/scenery_manifest.json`; the downloader keys installs
by sha256, so a re-packed region with a new `--version` shows as "Update
available". Test a pack through the real pipeline before uploading: serve
the output directory locally (`python3 -m http.server 8765` there, a copy of
the manifest with `http://127.0.0.1:8765/` urls) and run
`PROBE_MANIFEST=… PROBE_REGION=denmark PROBE_ROOT=/tmp/dl godot --headless
--path provpilot -s tests/probe_scenery_download.gd`. The packs own the plain country directories (`Switzerland`,
`Germany`, …); a 1 m add-on installed under the same name must move to
`<Name>-HD` first (design §11.3) because the tile names collide. Not
distributed: the detail add-ons (CH 1 m, BW), the custom object packs
(LSZH, Schmerlat, Kjeller, Gardermoen, Bern), `scenery/Design`, and the
bake's `terrain_base/` layer.

## 5. Publish

1. Upload `ProvPilot-<version>-arm64.dmg` and `ProvPilot-Windows-<version>.zip`
   to `provpilot/releases/`.
2. Write `provpilot/releases/latest.json`:
   ```json
   {
     "version": "1.0.20",
     "macos_arm64":   {"url": "ProvPilot-1.0.20-arm64.dmg",   "sha256": "…", "size": 0},
     "windows_x86_64": {"url": "ProvPilot-Windows-1.0.20.zip", "sha256": "…", "size": 0}
   }
   ```
   (`build_macos.sh` prints the macOS block; for Windows use
   `Get-FileHash -Algorithm SHA256` and the file length.)
3. Upload the scenery packs and the manifest (section 4).
4. Tag: `git tag -a v<version> -m "ProvPilot <version>" && git push --tags`.
