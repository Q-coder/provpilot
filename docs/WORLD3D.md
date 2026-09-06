# World3D — Global Globe Integration

ProVPilot renders the whole Earth using the **world3d** GDExtension
(`gitlab.md80.ch:gery/world3d`, `namespace world3d`) as the far/low-detail base,
with the detailed Swiss (LV95) and Baden-Württemberg scenery3d patch mounted on
top near the origin. This document describes how world3d is wired into ProVPilot.

## Overview

- **world3d** renders a cube-sphere Earth from 60″ ETOPO in **ECEF**
  (Earth-Centred Earth-Fixed) double-precision metres (~6.4 M m at the surface).
- ProVPilot's aircraft, JSBSim bridge (`jsbgodot`) and `node_3d.gd` all live in a
  **flat local LV95 frame** (`+X = West`, `+Y = Up`, `+Z = North`; origin
  `E 2682880 / N 1283072`, ≈ Schaffhausen).
- Rather than move the whole flight model into ECEF, the globe is **mapped into
  the existing flat frame** so the aircraft/JSBSim/cameras keep working unchanged.

## How the globe is mounted (`GlobeMount`)

`W3DGlobe` reads the camera for LOD via `to_local(camera.global_position)` and
stores its chunks in its own local (ECEF) space. So mounting the globe under a
node whose transform is the **inverse** of the tangent frame at the LV95 origin
maps the entire ECEF globe into ProVPilot's flat frame, while the globe still
computes the correct ECEF camera position internally.

Scene (`node_3d.tscn`):

```
node_3d
├── GlobeMount (Node3D)      # transform = enu_to_ecef(origin).affine_inverse()
│   └── Globe (W3DGlobe)     # faces_path = res://faces
├── Scenery3D                # flat LV95 patch (terrain/buildings/roads/water)
├── Vegetation
└── AC …                     # aircraft, cameras (flat frame, unchanged)
```

`node_3d.gd::_setup_world3d_globe()`:
- Converts the LV95 origin → WGS84 via `_lv95_to_wgs84()` (CH1903+ approximation).
- `_ecef_from_flat = W3DCoords.enu_to_ecef_transform(lat, lon, 0, x_axis_west=true)`
  (the `x_axis_west` convention exactly matches scenery3d's `+X = West / +Z = North`).
- `GlobeMount.transform = _ecef_from_flat.affine_inverse()`.
- Bumps camera far planes so the globe horizon is reachable.

## Terrain elevation (ground contact)

`node_3d.gd` queries terrain **patch-first, globe-fallback** so the aircraft has
ground everywhere on the planet while keeping Swiss/BW precision where available:

- `_scenery_elevation_at(x, z)` — precise scenery3d value (`NAN` if no data).
- `_globe_elevation_at(x, z)` — coarse whole-planet ETOPO (flat → ECEF → geodetic
  → `globe.get_elevation(lat, lon)`).
- `_terrain_elevation_at(x, z)` — scenery first, else globe.
- Spawn (`_do_deferred_terrain_init`) waits for the **precise** scenery value
  (avoids a ~100 m jump from initialising on the coarse globe), falling back to
  the globe only after a timeout / outside the patch.

## Globe visibility under the patch

The coarse ETOPO surface (~2 km posting, e.g. Schaffhausen reads ~571 m vs the
real ~460 m) would poke through the fine scenery3d terrain. `_update_globe_visibility()`
hides the globe below `GLOBE_SHOW_ALT_M` (25 km) — the detailed patch is the
ground there — and shows it above (high altitude / during the intro). Globe
terrain **queries are unaffected** by visibility.

## Cinematic intro (space → spot)

On entering fly mode (`_update_loading_progress` → `_start_intro_zoom`), a
dedicated **root-parented** `IntroCamera` zooms from ~16 000 km down to the
external spot view over ~6 s:

- The camera is parented to the **root**, not the aircraft — a camera parented to
  `AC` would be dragged around by JSBSim's per-frame transform writes and never
  reach space.
- It starts **straight up** (`+Y`) above the aircraft (not the true radial
  direction, which tilts a few km horizontally and makes the scenery managers
  stream around the wrong spot).
- Distance is interpolated **logarithmically** (feels linear in orders of
  magnitude); the view direction slerps from top-down to the spot angle.
- Ends exactly on the orbit spot offset and hands off to `CameraMode.ORBIT`
  (seamless). Skippable with **ESC** / **SPACE**.

## Day/night sun + atmospheric sky

The sky, sun and altitude ramps live in `provpilot/sky_atmosphere.gd` (a
`RefCounted` owned by `node_3d.gd`), which replaces the old C++ `Atmosphere`
module. `node_3d.gd::_setup_sky_atmosphere()` registers the scene
`Environment`, the `Sun`/`Fill` `DirectionalLight3D`s and the globe, then
`_process` calls `_sky.update(delta, get_viewport())` every frame.

**Atmospheric sky shader** (`provpilot/shaders/atmosphere_sky.gdshader`,
`shader_type sky`): a single-scattering Rayleigh + Mie atmosphere installed at
runtime as the `Environment.sky` material. Because it ray-traces the planet +
atmosphere shell from the camera position, the horizon is the *curved* Earth
limb at altitude (fixes the old flat procedural-sky horizon that diverged from
the globe above ~90 km) and a normal blue sky at ground level. It also draws
the sun disk, faint stars, and a dim sun-lit **ground hemisphere** (so
downward view directions / down-facing normals aren't pitch black), and fades
to black in space by itself — so the background stays `BG_SKY` at all altitudes
(the old module switched to solid black above 100 km, which hid the limb).

**Day/night sun:** the sun direction is computed from the simulated time of day
plus the observer latitude/longitude (NOAA solar position — equation of time +
declination → hour angle), and the `Sun` `DirectionalLight3D` is swung to match
so shadows track the sun through the day; its energy/colour ramp down to off as
it reaches the horizon (a light pointing upward would otherwise illuminate the
undersides of terrain/buildings at dawn/dusk).

The sun *disk* uses an **altitude-adjusted** horizon instead of the sea-level
one: climbing lowers the geometric horizon by the dip angle
`acos(R / (R + alt))`, so the disk stays visible from altitude after it has set
at ground level, and reappears when you climb. Terrain occlusion clips it near
the ground. Lighting still follows the true horizon.

**Ambient:** a constant-colour day/night ambient floor (`AMBIENT_SOURCE_COLOR`,
normal-independent) rather than sky-derived ambient — the atmosphere sky gives
little usable ambient, and some imported terrain (notably the 1 m/px BW tiles)
has normals facing away from the sun, so a normal-independent floor keeps those
surfaces readable instead of black.

**Time of day** (Settings → World): `use_system_time` (real UTC clock) or a
manual `time_of_day_min` HH:MM slider. `set_time()` seeds the clock;
`get_time_string()` feeds the HUD `Time:` label. Manual time can advance in real
time via the `delta` passed to `update()` (currently held static).

| Effect | Ground | Space |
|--------|--------|-------|
| Fog | on (density ramps 4 km→60 km) | **disabled** (`fog_enabled=false`) |
| Background | atmospheric sky shader | same shader → **black by itself** |
| Ambient (constant colour) | full (day) | tapered |
| Glow | on | faded → off |

> **Gotcha:** exponential fog tints the infinite-depth background to the pale
> `fog_light_color` via `fog_sky_affect`/aerial-perspective **even at density 0**.
> Zeroing density is not enough — fog must be fully **disabled** at altitude to get
> true black space.
> **Gotcha:** Godot `shader_type sky` predefines `PI` — do not redeclare it.

**Ray-march midpoint bias (fixed).** `compute_atmosphere` marches 16 primary
samples along the view ray. Attenuating each sample with the *running* optical
depth charges it its own entire step; the zenith chord is short so nothing shows,
but a horizon chord is ~1100 km, one step spans ~70 km, and that is a blue
optical depth near 2 — the first sample lost most of its blue before contributing
anything. The daytime horizon therefore rendered sunset-yellow at every hour.
Attenuating to the sample **midpoint** (`od - d * 0.5`) fixes it: measured
through the Filmic tonemap the noon horizon goes from hue 53° / blue-red 0.37 to
a neutral pale haze at blue-red 0.91, evening stays orange (hue 47°→50°) and the
zenith is unchanged (hue 208°→209°).

The **sun-path** optical depth (`lod_*`) deliberately keeps the running-total
form. That bias only reddens light arriving through a long low-sun path, which is
what gives the sunsets their colour.

> To retune the sky without rendering, port `compute_atmosphere` to numpy, sweep
> sun elevation / altitude / view pitch, apply Godot's Filmic curve and compare
> hue and blue/red ratios. Brightness changes are easy to mistake for colour
> fixes otherwise.

## Scenery3D building LOD fix

Re-enabling scenery3d exposed a latent building-manager issue: tiles first loaded
as far-LOD (e.g. because the intro camera streamed around an offset point) had
their far mesh hidden on approach with no detail to show, so buildings appeared to
vanish / stream in very slowly. Fixed in
`addons/scenery3d/src/s3d_building_manager.cpp`:

- LOD visibility: far mesh stays visible as a placeholder while detail streams in
  (`far = desired_lod≥1 || !has_detail`; `detail = near && has_detail`) — a tile
  never renders as nothing.
- `process_load_results` budget raised from 2 → 6 tiles/frame so detailed
  buildings near the aircraft appear promptly after a teleport / intro.

Combined with the straight-up intro camera (no far-first double-load), buildings
near the aircraft fill in within a few seconds.

## Build / deploy notes

- **Extension manifest:** `provpilot/bin/world3d.gdextension` (tracked). The dylib
  `libworld3d.macos.*.dylib` is a **gitignored build artifact** — copy it from the
  world3d build (`demo/bin/`) into `provpilot/bin/` and `codesign --force --sign -`.
  After adding the manifest, run `godot --headless --path provpilot --import` once
  so `.godot/extension_list.cfg` picks it up.
- **Face data:** `provpilot/faces/{face0..5.f32, meta.json}` (60″ ETOPO,
  ~24 MB, tracked) — generated by world3d's `tools/bake_faces.py`.
- **scenery3d dylib:** after editing `addons/scenery3d/src`, rebuild
  (`scons platform=macos precision=double arch=arm64 target=template_debug`), copy
  `demo/bin/libscenery3d.*.dylib` → `provpilot/bin/`, and re-codesign.
- **TODO (shipping):** build the `template_release` world3d dylib, wire world3d
  into `build_macos.sh` / `build_windows.ps1`, and add `faces/*` to the export
  `include_filter`.

## Known limitations

- The flat scenery3d patch diverges from the ellipsoid by ~`d²/2R` (~1.8 km at
  150 km from the origin) — a faint "shelf" at the far edges; fine where you fly.
- The curved horizon comes from the atmospheric sky shader + the globe (shown
  above 25 km); below that the flat patch defines the near horizon and scenery3d's
  own far panorama fills terrain to ~200 km.
- Global terrain is coarse (60″ ETOPO, ~2 km); peaks are smoothed (Everest reads
  ~5.9 km). Fine detail is what the Swiss/BW patch adds.
- BW terrain is 1 m/px vs CH 2 m/px with the node `tile_px = 512` — a pre-existing
  mixed-resolution caveat (scenery library config), unrelated to world3d. Its
  mis-loaded normals render flat under the directional sun (lit only by the
  ambient floor); fly in Switzerland for correct directional terrain shading.
