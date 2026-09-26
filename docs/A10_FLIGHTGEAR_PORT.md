# A-10 port from the FlightGear package — plan (2026-09-21)

Goal (gery): replace the hand-made A-10 glb with a build from the original
FlightGear data (`/Users/gery/Desktop/A-10`, GPL-2.0) and make the aircraft
as functional as it is in FlightGear. Source of truth: the FlightGear model
XML, the AC3D meshes, the JSBSim FDM already in use.

## 1. What the package contains

Inventory of `Models/**/*.xml` (138 files with animations):

| animation type | count | meaning |
|---|---|---|
| select | 459 | show/hide objects on a condition |
| rotate | 273 | rotate objects about centre+axis by property × factor (needles, surfaces, gear) |
| material | 83 | emission / colour by property (panel lighting, lit lamps) |
| pick, knob, slider | 117 | clickable cockpit controls (mouse) |
| textranslate | 44 | scroll a texture by property (drum counters) |
| translate | 35 | move objects (flaps, oleos, levers) |
| range, dist-scale, billboard, scale | 69 | LOD / effect helpers (mostly ignorable) |
| spin | 5 | fans, tyres |
| light, shader, blend, noshadow | 17 | effects (ignore) |

Property roots used: `payload` (139, weapons — out of scope), `sim` (73,
mostly `/sim/rendering/rembrandt` = ignore), `systems/electrical`,
`controls`, `instrumentation`, `sim/model/A-10/*` (Nasal-computed),
`surface-positions`, `gear`, `engines`, `canopy`, `orientation`.
Nasal: 16 000 lines (electrical, fuel, engines, APU, autopilot, HUD (1 900
lines, Canvas), displays, weapons). JSBSim: `A10JSBsim.xml` + `Systems/*`.

The exterior model's own animations (`A-10-model.xml`): 47 rotate, 8
translate, 5 spin, 14 select — control surfaces (rudder, elevators,
ailerons + spoiler mode, flaps rotate+translate, slats by alpha), canopy,
the complete nose and main gear kinematics (doors, jacks, braces, oleo
compression), tyre spin, engine fans, nose-wheel steering, nose-gear lights.

## 2. Architecture decisions

**D1 — Data-driven port.** A converter reads the FlightGear XML tree and
emits (a) one glb with the original node names and hierarchy and (b)
`model/animations.json`, a flat list of animation records
`{type, objects[], property, factor, offset, center, axis, min, max,
table[], condition}` for the types rotate, translate, spin, select,
material, textranslate, range. ProVPilot gets a generic `FGAnimator` that
evaluates these every frame. No per-part code: what FlightGear animates,
ProVPilot animates. The existing `[animation]` keys stay for the C172/PC-7.

**D2 — Property bridge.** Animations read FlightGear property paths. A
`PropertyBridge` resolves them: JSBSim properties directly where the FDM
has them (`fcs/*-pos-norm`, `gear/unit[n]/pos-norm`, `compression-ft`,
`velocities/*`, `propulsion/engine[n]/n1`, `position/*`, `attitude/*`);
computed values for FlightGear-only paths (`surface-positions/*`,
`instrumentation/*`, `systems/electrical/*`, `sim/model/A-10/*`, `canopy`,
`controls/*`) in a small GDScript/C++ table that grows subtask by subtask.
Unknown paths evaluate to 0 and are listed once at load so coverage is
measurable.

**D3 — Blender is the mesh pipeline, not the animation pipeline.** The
Blender scene built 2026-09-21 (`Models/A10-airframe-cockpit-import.blend`:
airframe 227 objects, cockpit 890, pilot 72, light effects 23) is exported
to glb with transforms *not* applied, so every object keeps its own origin
and name; pivots come from the XML `center`/`axis`, applied by the animator
as T(c)·R·T(−c) in the object's parent frame. The baked `gear_cycle` clip is
retired.

**D4 — Cockpit interaction reuses the existing raycast system**
(`cockpit_controls`): FlightGear `pick`/`knob` records become clickable
regions driving bridge properties; only the controls ProVPilot can act on
are wired (gear, flaps, throttle, lights, canopy, trim, brakes).

**D5 — Out of scope for now:** weapons/payload/stores, MFD/RWR/CMSP canvas
displays (static textures), the Nasal HUD (ProVPilot's own HUD overlay
stays), damage, datalink, multiplayer, sounds beyond the engine loop.

## 3. Subtasks (in order)

1. **Model build pipeline** — `scripts/fg_a10_build.py`: assemble in
   Blender (headless, from the .ac + XML offsets as done interactively),
   export `aircraft/a10/model/a10.glb` (airframe + cockpit, no pilot, no
   light-effect volumes, textures embedded), emit `animations.json`.
   Verify: probe lists node names and confirms every animated object of
   the XML exists in the glb; wingtip-to-wingtip 17.5 m. **Check the
   visual world scale**: the current airframes measure 3.6 m span in world
   metres (AC node 0.1, model 0.2 m/unit); decide whether that is
   intended and set `[model] scale` accordingly.
2. **Generic animator** — `FGAnimator` (C++ in `src/` for per-frame cost;
   ~1 200 records) for rotate, translate, spin, select, textranslate,
   material, with condition evaluation (equals/less-than/and/or/not,
   properties and constants) and interpolation tables. Headless smoke with
   synthetic property values (rudder ±1 → ±20°, gear 0→1 chain).
3. **Property bridge, exterior** — surfaces, gear position and oleo
   compression, tyre spin, engine N1 fans, nose-wheel steering, canopy,
   slats from alpha, nose-gear lights → swap the model in, retire the clip
   and the `[animation]` keys for the A-10. Check flight: taxi, take-off,
   gear cycle, flaps, spoilers.
4. **Cockpit instruments** — bridge `instrumentation/*` from the FDM
   (ASI, altimeter with setting, VSI, ADI, standby ADI, HSI heading, AoA
   gauge and indexer, accelerometer, engine gauges N1/N2/ITT/fuel flow,
   fuel quantity, clock, warning lights) so the needle/drum animations
   run. Verify with a cockpit-camera probe at known states.
5. **Cockpit controls** — pick/knob: gear handle, flap lever, throttle,
   speed-brake, canopy, lights panel (nav/strobe/landing/taxi/formation),
   trim, brakes, altimeter setting; the lever meshes replace the hard-coded
   C172 lever setup for this aircraft. Verify with the raycast smoke.
6. **Systems (light)** — electrical buses as on/off from battery+generators
   (drives panel lighting and lamp `material` animations), landing-gear
   warning, master caution; aircraft lights driven from the switches
   instead of the automatic rules in `src/aircraft_lights.cpp`.
7. **View and camera** — cockpit eye point from the FlightGear view config
   (0, 1.5, 3.2), head limits; external camera default.
8. **Later** — HUD on the combiner glass, MFD pages, sounds, stores.

Each subtask ends with a headless smoke, a real-renderer probe screenshot
and a check flight by gery before the next starts.

## 4. Progress

### Subtask 1 — model build pipeline (done 2026-09-21)

`scripts/fg_a10_build.py` runs inside headless Blender
(`Blender -b --python scripts/fg_a10_build.py -- --src <A-10 package> --out
provpilot/aircraft/a10/model`): factory scene, AC3D add-on enabled, airframe
import, recursive walk of `A-10-model.xml` with every nested `<model>` placed
by its `<offsets>` under an Empty `Sub_<tag>` (FlightGear frame = Blender
frame: X aft, Y right, Z up; heading about −Z, pitch about +Y, roll about −X),
sub-model objects renamed `<tag>__<name>` and made Godot-safe, glb export
(Y-up, transforms kept, textures embedded), manifest written. Skipped by
design: stores, pilot (`--with-pilot`), light-effect volumes, markers, the
landing/taxi light effects. Output (both copies, `provpilot/aircraft/a10/model`
and `aircraft/a10/model`):

- `a10_fg.glb` — 12.5 MB, 1 118 objects (airframe 227, cockpit 890),
  span 17.82 m between the wingtip lenses. glTF frame: x aft, y up, z left.
- `animations.json` — 455 animations (rotate 113, translate 27, spin 5,
  select 156, material 76, textranslate 44, range 31, misc 3) and 115
  controls (pick 57, knob 53, slider 5) as `{type, model, objects[], raw,
  unresolved[]}` with the XML kept verbatim as nested dicts; 36 sub-models
  with offsets; 23 unresolved targets (the generic altimeter from fgdata,
  which the package does not ship, and four names the AC3D files never had).

Checks: `tests/probe_fg_model.tscn` (headless: loads the glb like the
aircraft loader, every resolved manifest target exists, span 17–18.5 m) and
`tests/probe_fg_model_render.gd` (real renderer, `PROBE_VIEW=ext|cpt`):
exterior textured with a transparent canopy, cockpit complete from the eye
point. The old `a10.glb` and the aircraft config are untouched until
subtask 3 swaps the model in.

**Finding — visual world scale.** The airframes are drawn at about a fifth
of real size: the `AC` node is scaled 0.1 and the A-10 lands at 0.2 m per
model metre (wingtip lenses 3.56 m apart in world metres; C172 2.4 m, PC-7
2.6 m). A render of the parked A-10 on Zurich's concrete runway confirms
it: the jet spans less than one 6 m slab and its length equals about four
0.9 m centreline-dash widths. The FlightGear-built glb is in true metres,
so wiring it in (subtask 3) needs a decision: either scale the A-10 up
to real size (`[model] scale`, then re-calibrate the cockpit camera, the
gear offset and the orbit distances for it) or scale it down to match the
other two. Recommendation: real size for the A-10 now and the same fix for
the C172 and PC-7 afterwards, since every metre-based feature (lights,
cameras, gear clearance, cockpit interaction distances) currently works in
a 0.1-m unit frame.

### Subtask 2 — generic animator (done 2026-09-21)

`src/fg_animator.{h,cpp}` — `FGAnimator` (Node, GDExtension). `load(manifest,
model_root)` resolves every record's targets by name (Blender duplicate
suffixes `_001` match their stem, as FlightGear applies an animation to all
objects of that name), compiles conditions (`equals`, `not-equals`,
`less-than[-equals]`, `greater-than[-equals]`, `and`, `or`, `not`, bare
`property`; numeric or string constants; two-property comparisons) and
interpolation tables, converts `center`/`axis` from the FlightGear frame to
glTF (`godot = (x, z, −y)`), duplicates materials per animated mesh surface
for `material`/`textranslate`, and remembers each node's base transform.
`update(delta)` composes the transform animations per node in file order
(first = outermost, `node.transform = A1·A2·…·base`, as SimGear nests
them), evaluates `select` visibility (all selects on a node must pass),
advances `spin` (factor × property = rpm), applies `material` emission /
diffuse / alpha and `textranslate` UV offsets with `step`/`scroll`/`bias`.
Property bridge API: `set_property(path, value)` (numbers, bools or
strings; paths normalised: leading `/` and `[0]` dropped),
`set_properties(dict)`, `get_property`, `property_names()`,
`unknown_properties()` (referenced but never set), `stats()`,
`animated_transform(node)` for tests. Types `range`, `shader`, `noshadow`
and the controls are skipped (34 records).

On the A-10 manifest: 401 animations on 279 nodes and 603 material
surfaces, 209 properties; `update` costs 0.02 ms per frame. Smoke
`tests/smoke_fg_animator.tscn`: rudder ±20°, canopy 17.5° about the
lateral axis, flap translate+rotate chain, nose oleo 0.312 m, gear chain
at rest / moving, canopy glass select on a string property, fan spin from
N1, airspeed needle from its table, unbridged-property report, cost.
Build note: the extension must be built with
`scons precision=double arch=arm64 target=template_debug`; a bare `scons`
builds single-precision universal and fails on `real_t`. Re-sign the dylib
on a fresh inode afterwards.

### Subtask 3 — exterior bridge and model swap at real size (done 2026-09-21, check flight pending)

- `src/fg_bridge.cpp` (C++ `FGBridge`): per-frame FlightGear property values
  from JSBSim (`fcs/*-pos-norm` incl. `speedbrake` → spoilers,
  `gear/unit[n]/pos-norm`, `compression-ft` normalised by an oleo travel
  table, `wheel-speed-fps` or ground speed on WOW → `rollspeed-ms`,
  `propulsion/engine[n]/n1`, `aero/alpha-deg` when airborne) and the host
  (canopy, landing/taxi light → `systems/electrical/outputs/*-light`,
  cockpit view → `sim/current-view/*`, `rembrandt` 0, `A-10/done` 0).
  28 of 210 manifest properties are bridged; the rest are cockpit
  (subtask 4).
- `node_3d.gd`: `[model] fg_animations` → `FGAnimator` loaded on the model
  root after every airframe swap, fed by the bridge and updated each frame
  (after the aircraft lights, which supply the landing-light state);
  `[model] hide_nodes`; `[camera] orbit_distance[_min|_max]` (metres) through
  a new `CameraRig::set_orbit_distance`.
- `aircraft.cfg` (both copies): `model/a10_fg.glb`, `scale 5.0` (real
  size), `offset "0, 10.5, 0"` (loader units of 0.2 m, measured with
  `tests/probe_gear_height.tscn`: main wheels within ±6 cm of the ground,
  the nose wheel 0.30 m high — the model's nose strut is shorter than the
  FDM's contact geometry, left as is), `cockpit_position "0, 36, -32"` (the
  FlightGear eye point 1.5 m up / 3.2 m aft plus the 2.1 m offset), orbit
  32 / 6 / 300 m, no `gear_cycle` clip, `[lights] pair_meshes false`
  (the FlightGear selects switch the nose-gear lens pairs), placeholder
  display meshes hidden (HUD combiner, MPCD, CMSC, CMSP canvases).
- `FGAnimator` frame fix found here: FlightGear centres/axes are in the
  *model file's* coordinates while AC3D groups carry `loc` offsets, so each
  node's animation chain is conjugated by its parent's static transform up
  to the file root — the airframe's `A10_Root` (below the loader's fitting
  transform) or the `Sub_<tag>` empty for sub-model animations. Without it
  the nose gear pivoted 2.8 m too far aft, the fans orbited 9 m below the
  jet and instrument screws landed on the runway. The lights smoke now
  checks that every mesh stays inside the airframe box.
- Checks: `smoke_fg_animator`, `smoke_aircraft_lights` (A-10 on the new
  model: gear down from JSBSim, lens pair by FlightGear select, sprites
  outboard, no stray meshes), `smoke_ecef_spawn`, `smoke_nose_steer`,
  `smoke_runways` pass; `probe_gear_height`; renders (`probe_aircraft_lights`
  with `PROBE_COCKPIT=1`): the jet spans three 6 m runway slabs, gear down
  on the pavement, pilot's view over the glareshield.
- Still 1/5 scale: the C172 and PC-7 (same treatment later). The old
  `model/a10.glb` stays on disk until the check flight is good.
- Check flight 2026-09-21 (gery): animations and the virtual cockpit good;
  the gear did not retract — JSBSim never updates `gear/unit[n]/pos-norm`
  (stays 1), only the kinematic's `gear/gear-pos-norm`; the bridge now uses
  that for all legs (`tests/smoke_a10_gear_anim.tscn`: chain 88°/90° parked,
  0° six seconds after the up command, back again). Speed brakes had no
  key: **B** toggles `fcs/speedbrake-cmd-norm` through a new
  `JSBGodot.set_property`; the smoke sees the upper aileron split 60°.
- Nose-wheel steering visual (gery, 2026-09-21): FlightGear rotates the
  nose wheel from the rudder with a 20° factor, so the 60° NWS range never
  showed. `scripts/fg_a10_build.py` now carries an `OVERRIDES` table applied
  to manifest records (the first: that rotate reads
  `gear/gear[0]/steering-deg`, factor −1) and the bridge feeds JSBSim's
  `fcs/steer-pos-deg[0]`. Smoke: 5° limited, 60° full, sign as before.
- Ailerons (gery, 2026-09-21): both moved the same way. The model rotates
  the right aileron about a mirrored axis with a negated factor, i.e. it
  expects `right-aileron-pos-norm = −left` (FlightGear's convention), while
  the A-10 FDM's Roll channel outputs the same sign for both; the bridge now
  derives the right side from the left. Smoke: +30° / −30° about the lateral
  axis (`JSBGodot.set_input_aileron` is now bound for tests). Seen once
  during these runs: JSBSim produced a non-finite state right after the
  first-flight init (terrain 0 m at that moment) and reset itself; not
  reproduced, worth watching.

### Subtask 4 — cockpit instruments (done 2026-09-21, check flight pending)

`src/fg_bridge.cpp` now covers every property the manifest references (0 of 210
unbridged, `tests/smoke_a10_cockpit.tscn`):

- Flight instruments from JSBSim: ASI `velocities/vc-kts`, VSI
  `h-dot-fps × 60`, ADI and standby ADI `attitude/theta-deg` / `phi-deg`,
  HSI and magnetic compass `attitude/psi-deg` (true heading; magnetic
  variation later), slip ball `accelerations/Ny`, accelerometer
  `accelerations/Nz` with max/min memory needles, altimeter values
  (`position/h-sl-ft`, 29.92) although the generic altimeter model is not
  in the package, AoA gauge `aero/alpha-deg` (already fed for the slats,
  hence the needle gery saw moving) and the AoA indexer thresholds from
  `A-10-base.xml` (9.5 / 8.5 / 7.5 / 6.5°).
- Engines: N1 / N2 from `propulsion/engine[n]`, fuel flow from
  `fuel-flow-rate-pps`, the ITT gauge and hot light read a *thrust fraction*
  (`thrust-lbs` / 9 065 lbf: the model's table is keyed 0 – 0.987, not
  degrees), oil pressure follows N2; throttle levers from
  `fcs/throttle-cmd-norm[n]`.
- Fuel: the FDM's three 3 450 lb tanks map onto the left / right displays
  (wing + half the centre tank each) and the total drum; per-tank gallons for
  the pump caution logic.
- Controls: stick and pedals from the `fcs/*-cmd-norm` commands, flap lever
  detent from `fcs/flap-cmd-norm` (UP / MVR / DN), gear handle ±1 from
  `get_gear_cmd`, landing/nav light switches, panel and instrument lighting
  knobs at 0.6 × the dusk level (material emission; full 1.0 floods the panel
  orange).
- Electrical outputs (ADI, HSI, AoA indexer, caution panel, HUD, gear
  warning, nav mode, RWR, GAU-8, fuel gauge) at 28 V, R-AC 115 V; hydraulics
  3 000 psi, reservoirs, feed pressure, boost pumps, oxygen, all caution
  inputs healthy; radios, TACAN, nav flags, weapons and stations idle;
  UTC clock from the system time.

Smoke: HSI card rotated by the heading (155° on runway 16), VSI at the 0
mark, left fuel needle on its table value for 2 585 lb, N1 bridged, clock,
stick follows the aileron command; the needle nodes are looked up from the
manifest. Renders (`PROBE_COCKPIT=2` looks down at the panel): needles and
levers in place by day; at night the panel lighting emission lights the
panel and the runway lights show through the canopy.
- Check flight 2026-09-21 (gery): gear handle reversed — the Nasal animates
  `ld-gear-handle-anim` to 0 for UP and −1 for DOWN (not ±1); fixed in the
  bridge, smoke checks −1 with the gear down. Night panel unreadable: the
  animator applied FlightGear's flat material emission, which floods the
  panel; it now sets the albedo texture as the emission texture so the
  emission is modulated by the markings (the FlightGear-style red flood
  remains, readable). Clock: was system UTC; now the simulated time of day
  (`sky_atmosphere._time_seconds`, the HUD clock) via
  `FGBridge.sim_time_seconds` — FlightGear's chronometer would show Zulu,
  the sim's clock is local mean solar time; the same value as the HUD was
  preferred.

### Subtask 5 — clickable cockpit controls (done 2026-09-21, check flight pending)

`src/fg_controls.cpp` (C++ `FGControls`) runs the manifest's 115 `pick` /
`knob` / `slider` records (126 clickable meshes; group objects contribute
every mesh below them). Picking is a ray from the cockpit camera through
the mouse against each control's bounding box, nearest hit within 4 m —
no physics bodies. FlightGear input mapping: pick = left button runs the
action bindings, release runs `mod-up`; knob = left click / wheel up
increase, right click / wheel down decrease, Shift for the coarse variants,
old-style knobs (`action` / `release`) get the click direction as the
offset; slider = the action on click. Bindings: property-toggle / assign /
adjust (step, min, max, wrap) / cycle / multiply and the A-10's Nasal
scripts by name (landing and nav light switchers, canopy switch and hold,
fuel display selector, fill-disable buttons, APU switch, nav mode ILS /
TACAN, `setprop(...)` calls).

Switch and knob values live in `FGControls.state`, which `FGBridge` merges
last every frame, so positions persist and their own animations follow
(switch levers, knob rotations, lamp selects). Real effects through
`node_3d._on_fg_control`: parking-brake handle → FDM brakes, landing light
switch (0 taxi / 1 off / 2 landing; the L key keeps it in step) →
`AircraftLights`, nav-light and strobe switches gate those fixtures, the
sprung canopy switch opens / closes the canopy (`FGBridge.advance_canopy`,
10 s travel — the FlightGear model then swings the canopy), HSI heading-set
knob offsets the compass card, the accelerometer button resets the memory
needles, panel / instrument / console lighting knobs override the dusk
default. The cursor becomes a pointing hand over a clickable part. Gear,
flaps and throttle remain on keys, as in FlightGear.

Smoke `tests/smoke_a10_controls.tscn`: synthetic clicks at the switches'
screen positions — battery switch toggles and the animator sees it, HSI
course knob +1 / +5 (Shift) / −1 and wrap to 359, landing light switch
turns the lights on, parking brake handle sets the brakes, accelerometer
reset, a click on empty panel is not taken.

**ProVPilot HUD key (2026-09-21, gery):** H cycles the overlay — 0 nothing
(default), 1 the bottom readouts, 2 also the upper info (Swiss and PM
coordinates, ECEF, time, slew heading). The VFR map, weather overlay and
FPS counter keep their own toggles.
- Check flight 2026-09-21 (gery): switches and knobs took clicks but did
  not move. Cause: a FlightGear `knob` / `slider` record *is* the part's
  rotate / translate animation as well as its bindings; the animator now
  runs those records too (446 animations), a knob without `<center>` turns
  about its own bounds centre, and 27 axes given as helper objects
  (two-vertex lines the glb drops) are resolved by the build script into
  the two-point axis form (`axis_from_object`). The 3-position switches
  need their second direction: `shift-action` (sliders) and FlightGear's
  middle-button actions answer to Shift or the right button. The gear
  handle had no binding in the FlightGear model (keys only): the build
  script's `EXTRA_CONTROLS` adds a ProvPilot pick (`provpilot.gear_toggle`)
  that goes through the same path as the G key. Properties only the
  switches write are seeded once with defaults (battery and generators on,
  canopy switch held). The L key toggles landing / off; the taxi position
  is the switch's third position, reached by right-clicking it down.

### Subtask 6 — light systems (done 2026-09-21, check flight pending)

`FGBridge._electrical`: one DC bus from the battery switch and the two engine
generator switches (`FGControls.state`, seeded on); a generator is on line
when its switch is on and the engine's N2 is above 50 %; the inverter makes
AC from DC. Every powered output the model tests (`> 24 V`: ADI, HSI, AoA
indexer, caution panel, HUD, gear warning, nav mode, RWR, GAU-8, fuel
gauge) follows the bus, as do the landing / taxi light outputs and the panel
lighting knobs. Generator caution lights come on for a generator off line,
the master caution is their union (the model compares it with the string
"true"). `AircraftLights.powered` gates every exterior fixture; the strobe
switch also gates the beacon. The gear-panel lamps (safe / in-transit),
canopy warning and the steering-engaged light were already data-driven
from the gear position, canopy and airspeed. Not modelled: APU, inverter
switch, converters, hydraulics, fuel system, caution acknowledge.

Smoke `tests/smoke_a10_systems.tscn`: bus up with the engines running,
battery alone keeps it up with both generator cautions and the master
caution on, battery and generators off kill the outputs, the nav lights and
the panel lighting, power restored clears the caution.

Trigger throttle (gery, 2026-09-21, Logitech F310 in D mode): both triggers
sit on one shared axis; a raw `> 0.1` test kept the throttle running down
on a drifted rest. Dead zone 0.3 with a proportional rate in
`JSBGodot::_physics_process`; `tests/smoke_throttle_trigger.tscn`.
- Check flight 2026-09-21 (gery): old-style knobs (single `action` with
  `property-adjust` `factor`, no `step`: HUD mode selector, AIM-9 mode)
  only turned one way — FlightGear passes the click direction as `offset`
  and multiplies it by `factor`; done now, right click / wheel down turns
  them back. Lit caution lamps were white blobs: their AC3D material has
  `emis 1 1 1` on the lit texture, which Godot rendered as sunlight plus a
  flat white emission; `FGAnimator::fixup_emissive_materials` now draws
  such self-lit textured surfaces unshaded (11 surfaces: caution lamps,
  master caution, nose-gear lens), so "L GEN" / "R GEN" read amber with
  black text. The engine gauges have one needle each in the FlightGear
  model (temp, fan, core, fuel flow, oil, APU); the small inner scales are
  texture only, there is no second needle to drive.

### Settings → Aircraft tab (2026-09-21, gery's request before subtask 8)

`src/aircraft_settings_panel.cpp` (C++ `AircraftSettingsPanel`), mounted first in the C++ settings
screen (`SettingsScreen::_build_aircraft_tab`, `get_aircraft_panel()`), for
the aircraft selected on the welcome page or the one being flown
(`node_3d._settings_aircraft_id`). Per-aircraft settings are
`aircraft_<id>_<name>` keys in a new `[aircraft]` section of provpilot.cfg
(`SettingsStore::section_for_key`), saved with the screen's Save button:
`fuel_pct` fills every tank to that share of its capacity at flight start
(`node_3d._apply_fuel_setting` after both FDM initialisations) and at once
when that aircraft is being flown. Reference data comes from a new
`aircraft.cfg [performance]` section (Vne, Vno, Vs, Vs0, Vr, Vref, ceiling,
weights, notes; public figures, not FDM-tuned, added for all three
aircraft). Live centre of gravity from new `JSBGodot.get_cg_info()`
(inches aft of the datum, % MAC with the aerodynamic centre at 25 % MAC,
weight; the A-10 FDM keeps its CG on the aero reference point, 25 % MAC)
plus `get_tank_count / get_tank_capacity_lbs / get_tank_contents_lbs /
set_tank_contents_lbs`. The FlightGear A-10's external tanks are not in the
FDM (three internal tanks of 3 450 lb). Smoke
`tests/smoke_aircraft_settings.tscn`; render
`tests/probe_settings_aircraft_tab.tscn`.

### Parking brake at flight start (2026-09-21, gery)

Every flight now starts with the parking brake set (`node_3d._set_parking_brake_at_start`
after both FDM initialisations; the aircraft used to roll off while the scenery
was still loading). Release with `,`, set with `.`; the A-10's emergency brake
handle toggles it and follows the keys (`_sync_parking_brake`); the switch
default seeds `controls/gear/brake-parking` = 1.

### Subtask 8a — sound set (done 2026-09-21, check flight pending)

`scripts/fg_a10_sounds.py` exports `Sounds/A-10-sound.xml` (140 records) to
`aircraft/a10/sounds/sounds.json` with the XML kept verbatim, and copies the
28 .wav files the package ships (9.6 MB; both copies). Five generic
FlightGear files ($FG_ROOT/Sounds: rumble, gear, gear-lck, squeal, flaps)
are not in the package: their 19 records are skipped and listed.
`src/fg_sounds.cpp` (C++ `FGSounds`) plays the set from the same property
values the animator gets: looped sounds while their condition holds, once
sounds on the condition's rising edge (switch clicks), in-transit sounds
while the watched property moves (canopy), volume and pitch as the product
of the <volume>/<pitch> entries with FlightGear's types (linear / log / ln /
inverse / square), factor, offset, min, max and the `dt_play` internal;
WAVs are loaded at runtime with `AudioStreamWAV.load_from_file` and looped by
frame count; positional sounds play in 2D. `aircraft.cfg [sound] fg_sounds
"sounds/sounds.json"` enables it; the basic engine loop stays silent for such
an aircraft, and the settings' engine volume scales the set. Bridge additions:
`sim/current-view/internal`, `engines/engine[n]/thrust_lb` and `thrust-lbs`,
`velocities/groundspeed-kt`, `speed-down-fps`, `gear/gear[n]/wow`, warning
audio volume, rain 0, weapons audio 0. Smoke `tests/smoke_a10_sounds.tscn`:
cockpit view plays the internal engine loops (−6 dB from N1) and not the
outside fans, the external view swaps them, a switch click plays once,
the canopy sound plays while it moves and stops when it holds, the basic
engine loop is silent. For the C172 / PC-7 the same runtime can take a
hand-written sounds.json (or a FlightGear <fx> file) later.
- Check flight 2026-09-21 (gery): cockpit sounds during the intro zoom, no
  distance, and the left generator switch silencing the engines. Fixes:
  `sim/current-view/internal` now follows the camera that renders (the intro
  camera and the external cameras are outside even while camera_mode says
  cockpit; `_cancel_intro_zoom` also hands `current` to camera2); outside
  listening attenuates every sound by reference / max(distance, reference)
  (60 m, per-sound `<reference-dist>`) and cuts it beyond `<max-dist>`
  (8 km), so the earth zoom starts silent and the jet fades in; the
  FlightGear engine sounds test the LEFT generator switch as "engine
  running" — `scripts/fg_a10_sounds.py` rewrites those conditions onto the
  engine's own N1 (`override`). Smoke: intro from 16 000 km selects the
  outside set at −80 dB, cockpit / external swap, generator off keeps the
  engines, 40 m −28 dB vs 5 km silent.
- Check flight 2026-09-21 (gery): cockpit sound audible on the fly-loading
  screen, and the A-10 fuel setting lost after Save. The loading screen is
  gone for Fly!: the intro zoom now starts with the request over the
  destination and is the loading view — its HUD shows `LoadingProgress`'s
  status ("Loading scenery… (n/5 tiles)", "Initialising flight model…",
  "Stabilizing aircraft…") with one bar per phase, each starting at 0
  (`LoadingProgress.get_phase_progress`; the tile bar takes over, from 0,
  once loading is complete — gery: it started full); the zoom waits on the
  spot view (`IntroZoom.set_hold_at_end`) until loading completes, Space
  then only jumps to the end. No cockpit camera renders meanwhile, so no
  cockpit sounds; under the fallback loading screen (zoom cannot start) the
  sound set is muted. The fuel setting was saved to `[aircraft]` but
  `SettingsStore::load()` only read keys that have a default —
  `aircraft_*` keys are now loaded as they are (and stale ones erased on
  save after Restore Defaults; `InstallConfig.erase_section_key`). Smokes:
  aircraft settings (load round trip), globe cinematics (intro as loading
  view, hold, Space), A-10 sounds (nothing internal while loading).
- 2026-09-21 (gery): cycling back to the cockpit view no longer recentres
  the head look; `CameraRig::flip` keeps yaw / pitch (and the zoom) like
  the spot and chase views keep theirs (smoke_cockpit_zoom). Space in the
  cockpit (MSFS style, gery's request): held, the mouse looks around
  (`CameraRig::mouse_look`, 0.15°/px, mouse captured meanwhile); a tap
  under 0.3 s without mouse movement recentres head and zoom
  (`recenter_head`); the wheel while Space is held zooms 5° of FOV per
  notch (`mouse_zoom`, not a tap). Space still skips the intro zoom (the
  rig's cockpit branch is off while it plays).
- Altimeter 2026-09-21 (gery: solve it as the POC for instruments in
  general): the package references fgdata's generic
  `Aircraft/Instruments-3d/altimeter`; vendored under `third_party/fgdata`
  (GPL-2), the build script resolves `Aircraft/...` paths there first
  (rebuild: 0 missing, 23 objects). Bridge: reading = ISA inversion of
  `atmosphere/P-psf` against the Kollsman setting; the setting lives in
  the control state (knobs, `property-adjust`), seeded to the weather's
  QNH at flight start. Design write-up and the C172 / PC-7 plan in
  `docs/INSTRUMENTS.md`. Smoke: needle 0.36°/ft, QNH seed, +1 inHg ≈ +900 ft.
- Drum counters 2026-09-21 (gery: altimeter digits inverted, QNH window
  wrong): two `textranslate` errors in FGAnimator that affected all 44 drums
  (altimeter, fuel, HSI course / miles, UHF, ILS, TACAN; the clock scrolls
  along u and was right). (1) The glTF export flips V, so a FlightGear
  v-axis offset must be negated in `uv1_offset` (digits ran backwards).
  (2) FlightGear applies a textranslate's `offset` BEFORE the `factor`,
  `(value + offset) × factor`: the drums are authored with a rest digit of
  1 in the window and `offset -100 / factor 0.001` (one digit back), so the
  old `offset + factor × value` read one digit high. Measured with forced
  values (`PROBE_ALT_FT`, `PROBE_SETTING` in probe_aircraft_lights): 0 ft /
  30.00 and 12345 ft / 29.87 now read exactly. The Kollsman knob zones were
  right all along (left −0.005 / −0.025, right +); the wheel now works on
  any pick that adjusts a property (up +step, down −step, Shift ×5),
  `smoke_a10_controls`.

