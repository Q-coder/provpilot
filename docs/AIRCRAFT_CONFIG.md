# Aircraft Configuration (`aircraft.cfg`)

ProvPilot keeps its engine code **aircraft-agnostic**: every airframe ships a
plain‑text `aircraft.cfg` that wires a 3D model and a JSBSim flight model
together, plus the per‑aircraft visual/animation tuning. Adding or tweaking an
aircraft is mostly a matter of editing this file — no engine code changes.

- Source of truth: `provpilot/aircraft/<id>/aircraft.cfg`
- Parser: C++ `AircraftConfig` (`src/aircraft_config.cpp`,
  `AircraftConfig.load_from_file()` / `load_for_aircraft(id)`), which also
  owns the path lookup (`resolve_path`, `resolve_aircraft_file`).
- Consumers: C++ `AircraftRig` (`src/aircraft_rig.cpp`: control surfaces,
  gear, levers, propeller, camera, engine sound), `AircraftModelLoader`
  (`src/aircraft_model_loader.cpp`: `[model]` file + fitting transform),
  `src/jsbgodot.cpp` (flight model), and `provpilot/node_3d.gd` for the
  GDScript parts (`[lights]`, FlightGear animations / sounds).

## File format

A small INI‑like format (**not** Godot `ConfigFile`):

```ini
[section]            # section header
key "value"          # string (quoted; may contain spaces/slashes)
key value            # bare value (number / true / false)
key "a, b, c"        # comma-separated list (read via get_*_array)
# ...                # comment — whole line or trailing
```

Typed getters each take a default, so omitted keys fall back gracefully:
`get_string`, `get_float`, `get_int`, `get_bool`, `get_float_array`,
`get_string_array`.

## Where the file is read from (dev vs. exported)

| Context | Reads `aircraft.cfg` from |
| --- | --- |
| **Editor / headless** (`OS.has_feature("editor")`) | `res://aircraft/<id>/aircraft.cfg` **first** — so edits to `provpilot/aircraft/<id>/aircraft.cfg` take effect on the next relaunch, no copy needed. |
| **Exported app** | `<install_root>/aircraft/<id>/aircraft.cfg`, auto‑extracted from the PCK by `AircraftInstaller`. |

> ⚠️ The **model `.glb`** and **JSBSim XML** are *not* read res‑first: the model
> loader and the C++ FDM read the `<install_root>` copy first even in the editor.
> After regenerating a model or editing JSBSim XML you must sync the install‑root
> copy, e.g.
> `cp provpilot/aircraft/<id>/model/<id>.glb aircraft/<id>/model/<id>.glb` or
> `cp provpilot/aircraft/<id>/jsbsim/aircraft/<id>/<id>.xml aircraft/<id>/jsbsim/aircraft/<id>/<id>.xml`.

## Sections

### `[aircraft]`
`id`, `name`, `description` — identity shown on the welcome screen.

`base "<id>"` — a **variant aircraft** (2026-11-01, the MD-80 family). The
config of the base aircraft is loaded first and this file's sections are
overlaid on it key by key (`AircraftConfig::load_for_aircraft`), so a variant
file holds only what differs. Every file resolved through
`AircraftConfig::resolve_aircraft_file` (sounds, manifests, Nasal shims, init
lists, the liveries tree...) is looked for in the variant's folder first and
then in the base's, and the FDM loader does the same per JSBSim folder: a
variant ships `jsbsim/aircraft/<model>/` (its FDM, `FDE/Config`, maybe a
`Systems/` file of its own) and takes `engine/` and `systems/` from the base.
`res_file` paths are explicit, so a variant that shares the base's glb
inherits that key unchanged. Examples: `aircraft/md81`, `md83` (same model
as `md80`), `md87` (own model). The welcome screen lists variants like any
aircraft (`src/welcome_screen.cpp`) and their settings are keyed by their
own id.

### `[jsbsim]`
`model` — JSBSim model name. Data lives under
`<aircraft_dir>/jsbsim/{aircraft,engine,systems}`; the file
`<aircraft_dir>/jsbsim/aircraft/<model>/<model>.xml` must exist.
`init_properties` — one JSON file, or a comma-separated list merged in order
(a variant adds its few values after the base's list: `md87` sets the
MD-87-set.xml stabilizer arm factor in `jsbsim/init_md87.json`).

`property_rules "a.xml, b.xml"` — FlightGear property-rule files (the
`<autopilot><path>` entries of a -set file: `<logic>` and `<filter>`
components with SimGear expressions and conditions), paths relative to the
aircraft dir. `src/fg_property_rules.cpp` runs them every frame on the
bridge's property dictionary, after the bridge collects and before the
animator and the sounds read it; an output the FDM has a node for is also
written into the FDM. Supported: `gain`, `exponential`,
`double-exponential`, `moving-average`, `noise-spike`, `reciprocal`,
`derivative`, `integrator` filters with conditional `<input>` lists,
`<gain>`, `<filter-time>`, `<samples>`, `<max-rate-of-change>`, `<u_min>`
/ `<u_max>`, `<enable>`; `<logic>` with `<inverted>`. Other components
(`pid-controller`, `flipflop` ...) are listed once in the log and skipped.
The MD-80 runs its `sounds/sound-control.xml` this way (engine, wind, pack
and fan volumes); see `docs/MD80_FLIGHTGEAR_PORT.md` step 12.

### `[model]`
- `file` — model path relative to the aircraft dir; `res_file` — `res://` fallback.
- `root_node` — scene‑tree path the model is parented under. (The PC‑7 reuses the
  Cessna loader node and is swapped in at runtime.)
- `rotation_deg "x,y,z"`, `offset "x,y,z"`, `scale` / `scale_xyz` — optional
  fitting transform (orient nose‑forward, lift the belly to ground level,
  rescale), composed onto the loader node's own scene transform; the model's
  nodes keep their file transforms.
- `flip_normals` (bool) + `flip_normals_prefix` (str) — flip baked normals so the
  sun lights the top, not the underside. Empty prefix flips **all** meshes
  uniformly; a prefix flips only matching mesh names. Calibrate against the
  Cessna: a mesh whose top faces measure “up” needs `flip_normals true`.

#### FlightGear models (A-10)
`fg_animations "model/animations.json"` — manifest written by
`scripts/fg_a10_build.py`; the `FGAnimator` extension runs every FlightGear
animation of the model with values from `src/fg_bridge.cpp` (see
`docs/A10_FLIGHTGEAR_PORT.md`). `hide_nodes "a, b"` hides model nodes by name
(all matches), used for placeholder display textures. `knob_actions
"property[:step[:min:max]]", ...` gives a rotate / knob animation on that
property that has no binding of its own a property-adjust action (default
0.05 per click, 0..1), so it turns with the mouse like the other knobs (the
MD-80's flood knobs, which FlightGear never made clickable). `scale` fits a model
authored in metres: the AC frame draws models at 0.2 m per unit, so `5.0`
gives a model's own size (PC-7 and A-10, whose meshes match the real
dimensions). The Cessna's Sketchfab mesh is 14 % oversize (wingtip lenses
12.55 m apart for an 11.0 m span, 9.44 m long for 8.28 m), so it uses
`4.38`; check a new model's span and length against the real aircraft
before trusting `5.0`. Generic FlightGear models the package references
(`/Aircraft/Instruments-3d/...`) come from `third_party/fgdata`; see
`docs/INSTRUMENTS.md` for how instruments are built and added. A
non-FlightGear model (the Cessna) gets instruments the same way: a
`model/panel.xml` manifest built into the glb by `scripts/fg_panel_build.py`
and the resulting `animations.json` under `fg_animations`.

`textures_dir` — folder of the texture files FlightGear `material` animations
with `<texture-prop>` name (default `model/liveries`); a variant without its
own folder reads the base's.

### `[liveries]`
`catalog "model/liveries/<variant>.json"` — the liveries of a FlightGear
package, exported by `scripts/fg_liveries.py` (2026-11-01). A FlightGear
livery is a PropertyList setting `sim/model/<x>/texture` properties that
`material` animations with `<property-base>` + `<texture-prop>` read; the
-set file seeds a few of the same kind (engine-variant N1 / N2 gauge faces).
The catalog is `{defaults {prop: file}, liveries [{id, name, textures {prop:
file}}]}`; file names are relative to the animation's own model XML, as in
FlightGear, and the exporter lays the images out below `textures_dir` in
exactly that structure (`Liveries/MD-82/AA_fuselage.png`,
`Instruments/n1-217.png`). At flight start `AircraftRig::setup_liveries`
publishes the defaults and the chosen livery's files as control state
(strings the FDM push skips); `FGAnimator::apply_texture_prop` loads the
image once (`Image::load_from_file`, mipmaps) and swaps the albedo (and a
paired emission texture) of the record's objects, restoring the glb's own
texture for "" or the `<texture>` the glb was built with. The Aircraft tab
shows a **Livery** option for aircraft with a catalog; the choice is stored
as `aircraft_<id>_livery` (livery id, "" = as built) and applies at once to
the aircraft being flown (`livery_changed` → `AircraftRig::set_livery`). The
folder carries a `.gdignore` (the editor must not import the PNGs); the
installer copies PNGs only below a `liveries` folder and the export preset
includes `aircraft/*/model/liveries/**`.

### `[camera]`
Cockpit camera (Camera1) placement, in AC‑node‑local meters. Omit the whole
section to keep the scene default.
- `cockpit_position "x,y,z"` (x=right, y=up, z=back)
- `cockpit_rotation_deg "p,y,r"` *(optional)*, `cockpit_fov` *(optional)*

`orbit_distance`, `orbit_distance_min`, `orbit_distance_max` — external
camera distance in metres (defaults 6 / 1.5 / 50; a 17.5 m jet wants 32 / 6 /
300).

### `[animation]`
Control‑surface mesh node paths, **relative to `model.root_node`**:
`rudder`, `left_aileron`, `right_aileron`, `left_elevator`, `right_elevator`,
`left_flap`, `right_flap`, `propeller`. For one‑piece tailplanes use
`single_elevator` instead of the left/right elevator pair.

### `[control_limits]`
Max visual deflection (degrees) and the local **hinge axis** per surface. The
axis lets each airframe rotate a surface about whichever local axis its geometry
was modeled on, and a leading `-`/`+` inverts the sweep:
- `rudder_max_deg`, `rudder_hinge_axis` (e.g. `"-z"`)
- `aileron_max_deg`, `aileron_hinge_axis` (`x` default, `y`, `z`)
- `elevator_max_deg`, `elevator_hinge_axis`
- `flap_max_deg`, `front_wheel_max_deg`

### `[flaps]`
- `positions` — JSBSim `flap-cmd-norm` values per detent.
- `names` — HUD labels (e.g. `"UP, T/O, LAND"`).

Every path that steps the flaps walks this list: the F / Shift+F keys and
the cockpit lever drag (`CockpitControls`), the joystick buttons and the
FlightGear lever clicks (`JSBGodot::increase_flaps` / `decrease_flaps`, which
get the list through `set_flap_detents`). Without a section the list is the
Cessna's thirds (0, 0.333, 0.667, 1).

### `[propeller]`
Drives the spinning prop + RPM‑based **blur disc**.
- `max_rpm`, `blur_start_rpm` (disc fades in), `hide_blades_rpm` *(legacy)*.
- `disc_radius_m` — blur disc radius; if `<= 0` it is derived from the mesh.
- `spin_axis "x|y|z"` — local axis the prop spins about (signed; default `z`).
- `disc_offset "x,y,z"` — parent‑space nudge for the disc center. Needed when the
  prop node’s pivot sits at a blade tip (e.g. the PC‑7) so the disc is centered
  on the spinner rather than the pivot.

The real propeller (and its spinner cone) stays visible at all RPM; the disc just
fades in on top above `blur_start_rpm`, so hiding blades never leaves a hole.

### `[levers]`
Interactive cockpit levers/switches, located by `find_child` under
`model.root_node`. Each `<name>` has a `<name>_fallback` (the original,
unrenamed mesh node) — e.g. `throttle` / `throttle_fallback`.
`magneto_step_deg` (default 30): degrees per magneto position about the
key's local Z; negative when the model's key turns the other way (Cessna).
`magneto_rest_position` (default 3 = BOTH): the position the mesh's rest
pose shows (the Cessna's key rests at OFF, 0).

### `[gear]`
Retractable landing gear (toggled with **G**; JSBSim drives drag + weight‑on‑
wheels from `gear/gear-pos-norm`). Fixed gear (`retractable false`) may list
`legs` as well, with `retract_deg 0`, to get rolling (`<leg>_roll`) and
steering wheels (the Cessna's tyres, split out by `scripts/fg_panel_build.py`).
- `retractable` (bool), `retract_seconds`, `legs "nose, left, right"`.
- Per leg `<leg>_`: `wheel`, `strut`, `pivot "x,y,z"`, `axis x|y|z`, `retract_deg`.
  Each leg’s wheel + strut rotate **rigidly about a shared pivot** because the
  mesh origins sit at component centers, not the real hinges — so pivots/angles
  are tuned live. Flip a leg’s `retract_deg` sign if it sweeps the wrong way.
- Optional nose‑wheel **steering**, composed under the retract transform:
  `<leg>_steer true`, `<leg>_steer_axis "z"` (signed), `<leg>_steer_max_deg`,
  `<leg>_steer_pivot "x,y,z"`.
- Nose‑wheel steering **range** (Ctrl+W toggles limited ↔ full, like the
  A‑10's NWS engage): `steer_limited_deg` must equal the FDM's `<max_steer>`
  (the pedal range at speed); `steer_full_deg` is the engaged taxi range
  (A‑10: 5 / 60). The bridge scales `fcs/steer-cmd-norm` by full/limited,
  which JSBSim does not clamp. Omit both (or full ≤ limited) to disable.


**Calibrating `offset` (wheels on the ground):** run
`godot --headless --path provpilot res://tests/probe_gear_height.tscn`. It parks
each aircraft at LSZH with the FDM on its gear and prints the clearance of
the lowest tyre mesh above the ground under it (positive = floats). Note the
offset is applied in the model's parent frame, which carries the AC node's
scale, so one offset unit is 0.2 m — subtract clearance / 0.2 from the Y
offset, re-run, converge (2026-09-27, all three at real size: C172 −0.64,
PC‑7 9.55, A‑10 8.75 put every wheel within 2 mm of the LSZH pavement). The
cockpit eye point moves with the model: shift `cockpit_position` Y by twice
the offset change (AC units of 0.1 m).

### `[lights]`

Exterior lights, drawn by `src/aircraft_lights.cpp` (node `AircraftLights` under
the scene root). Each key lists model mesh nodes (comma separated, found by
name anywhere in the model); the light sprite sits at the mesh's bounding-box
centre and follows the node, so lights on animated gear retract with it.

| key | fixture | state |
|---|---|---|
| `nav_left`, `nav_right`, `nav_tail` | red / green / white steady | on while an aircraft is loaded |
| `strobe` | white double flash (1.2 s) | airborne (no weight on wheels) |
| `beacon` | red flash (1 Hz) | always |
| `landing`, `taxi` | white sprite + `SpotLight3D` on the ground | **L** toggles both; out while the gear is retracted; with `<kind>_props` the spot's energy follows the aircraft's level (DIM 0.6 / BRT 1) through a 2.2 curve |
| `<kind>_pos` | `"x, y, z"` in model-root units | alternative for models without a lens mesh (the Cessna's cowl lights) |
| `<kind>_anchor` | animated part the lens rides on | for lens meshes without animation tracks (the A-10 nose-gear lights): the lens is reparented under the part with the offset from the clip pose in which they coincide |
| `landing_pitch_deg` | spot aim below the aircraft's forward axis (default 5) | |
| `landing_range_m`, `taxi_range_m` | spot ranges (300 / 90) | |
| `spot_energy` | landing spot energy (12; taxi half) | |
| `pair_meshes` | `false` leaves the `-on`/`-off` lens meshes to another system (the A-10's FlightGear selects, fed by `src/fg_bridge.cpp`) | |

Each sprite is placed a few model-centimetres outside its lens (outboard for
wingtip lights, up for the beacon, forward for the nose lights) and drawn
with the depth test on, so the airframe hides lights on its far side. Note
that the AC frame draws models at 0.2 m per model unit (the `AC` node is 0.1,
`Node3D2` 2), so every airframe carries `[model] scale 5.0` to be at real
size (A-10 since 2026-09-21, C172 and PC-7 since 2026-09-27; before that the
two were drawn at 1/5, "model planes" next to real-size scenery objects).
The loader puts the fitting transform (rotation, scale, offset) on its own
node, so `<kind>_pos` values and everything else "in model-root units" are
plain model metres.

A node named `<x>-on` with a sibling `<x>-off` is a lit / unlit mesh pair
(FlightGear convention): the pair's visibility follows the light. Sprites are
dimmer by day and full at night (host rule: sun below 4° or visibility under
1 km, the same as the airfield lights).

### `[performance]`
Reference data shown in Settings → Aircraft (not used by the flight model):
`vne_kts`, `vno_kts`, `vs_kts`, `vs0_kts`, `vr_kts`, `vref_kts`,
`ceiling_ft`, `max_takeoff_lbs`, `empty_lbs`, `notes` (free text), and
optionally `cg_fwd_pct_mac` / `cg_aft_pct_mac` for the CG limits line. The
tab also holds the per-aircraft fuel load (`aircraft_<id>_fuel_pct` in
provpilot.cfg `[aircraft]`, applied to every tank at flight start; such keys
have no default and are loaded as they are, next to InstallConfig's own
`initialized` / `version` keys in that section) and a live
centre-of-gravity readout from JSBSim (inches aft of the datum, % MAC with
the aerodynamic centre taken at 25 % MAC, weight).

### `[sound]`
`engine` (ogg path) and RPM‑mapped `pitch_*` / volume tuning. `fg_sounds`
names a FlightGear sound set (`scripts/fg_a10_sounds.py` output) played by
`src/fg_sounds.cpp`; `aliases "a = b, ..."` lets a property the set reads
that nothing writes take another's value (the MD-80's cockpit wind reads
`/sim/sound/wind` while its rules compute `/sim/sound/wind-v`).

### `[propeller]`
`blur_start_rpm` shows the translucent disc, `hide_blades_rpm` hides the
blade mesh above that rpm (0 keeps the blades). `[animation] propeller_cone`
names a spinner cone: by default a swap-in mesh shown only while the blades
hide (PC-7, whose blade mesh includes its spinner); with
`cone_always_visible true` it is a separate part shown at every rpm, and
`cone_spins true` turns it with the blades about `spin_axis` (its node
origin must sit on the axis — the Cessna's `Spinner_Pivot` empty from
`scripts/fg_panel_build.py`). `disc_radius_m`, `spin_axis`, `disc_offset`
tune the disc.

## Flap pitch “feel”

To make flap changes noticeable (clear nose‑up on extend, nose‑down on retract,
like X‑Plane/MSFS), each aircraft’s JSBSim `PITCH` axis carries an extra
**deliberately exaggerated** flap pitching‑moment term on top of the realistic
aero (positive Cm = nose‑up):
- Cessna: `aero/coefficient/Cmdf_provpilot_fx` in
  `aircraft/c172p/jsbsim/aircraft/c172p/c172p.xml`.
- PC‑7: `aero/moment/Pitch_flaps_provpilot_fx` in
  `aircraft/pc7/jsbsim/aircraft/pc7/pc7.xml`.

These are small lookup tables vs. `fcs/flap-pos-deg`; scale the table values to
make the effect stronger/weaker. The transient scales with airspeed (no effect
near zero knots). Validate with
`./jsbsim/build/src/JSBSim --root=provpilot/aircraft/<id>/jsbsim --aircraft=<id> --catalog`.

## Aircraft lighting

Aircraft shading is set in `provpilot/environment.tscn` (a `res://` scene,
loaded directly — no install‑root sync):
- `Sun` `DirectionalLight3D` — key light; `shadow_opacity` controls cast‑shadow
  darkness (lower = lighter).
- `Fill` `DirectionalLight3D` — opposite‑azimuth, shadow‑less fill that lifts the
  sun‑away side of the airframe and the cockpit interior out of near‑black.
- `Environment.ambient_light_energy` — overall ambient level; SSAO darkens
  crevices/undersides.

## Adding a new aircraft (checklist)

1. Create `provpilot/aircraft/<id>/` with `jsbsim/{aircraft,engine,systems}` and
   `model/<id>.glb` — or, for a variant of an existing aircraft, only
   `aircraft.cfg` with `[aircraft] base` plus the files that differ.
2. Write `aircraft.cfg` (copy an existing one; the Cessna file documents the
   format in‑line).
3. Add the aircraft to the welcome‑screen list in
   `src/welcome_screen.cpp` (the `AIRCRAFT` table).
4. Bump `BUNDLE_VERSION` in `src/aircraft_installer.cpp` (C++ `AircraftInstaller`) (and add any
   required files) so existing installs re‑extract the new data.
5. Validate: `./jsbsim/build/src/JSBSim --root=provpilot/aircraft/<id>/jsbsim --aircraft=<id> --catalog`
   and `./godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot --quit`.

## Jets & multi-engine

Jets omit the `[propeller]` section (the propeller code is silent when unset).
The C++ bridge (`src/jsbgodot.cpp`) drives **all** engines together:
`FCS->SetThrottleCmd(-1, throttle)` and `propulsion/set-running = -1` (`-1` = all
engines), so a twin behaves symmetrically. Single-engine airframes are
unaffected (engine 0 == all engines).

### A-10 Thunderbolt II (`aircraft/a10`)

Keys specific to this airframe: **Ctrl+W** full / limited nose-wheel
steering, **B** speed brakes (split ailerons, `fcs/speedbrake-cmd-norm`),
**L** landing / taxi lights, **G** gear. The cockpit switches and knobs are
clickable (left click / wheel down = down or counter-clockwise, right click
/ wheel up = up or clockwise, Shift for coarse steps; see
`docs/A10_FLIGHTGEAR_PORT.md` subtask 5). **H** cycles the ProVPilot HUD
overlay for every aircraft: off, bottom readouts, readouts + upper info.

**Joystick** (Settings > Joystick, `JoystickBindings`, polled once per frame
by `node_3d.gd` into `JSBGodot::apply_joystick`, 2026-11-01): roll, pitch
and yaw from their bound axes (applied when the axis moved, so the keyboard
still nudges a resting stick's controls); the throttle is absolute when it is
a lever (0..1 across the calibrated range) and keeps the incremental RT / LT
behaviour when bound to a trigger; **Flight: Wheel brakes** takes the pulled
half of a centred lever (gery flies brakes and rudder on one lever, as in
X-Plane: rest = none, pull = 0..1; tick *Inv* if your lever reads positive
when pulled); the *Hold brakes* button and the `.` / `,` keys combine with it,
whichever asks for more. The parking brake is that held brake state
mirrored to the cockpit handle. The brake command also goes to FlightGear's
`/controls/gear/brake-left|right` when the FDM has them, because an FDM that
computes its own brake command from those (the MD-80: pedals, parking brake,
autobrake) overrides JSBSim's.

Ported from the FlightGear A-10 (its standalone JSBSim FDM: twin TF34 turbofan,
retractable tricycle gear, flaps, speedbrake). The visual model is a
self-contained `model/a10.glb` exported from Blender with embedded textures,
double-sided materials, outward normals (`[model] flip_normals false`), and a
merged `gear_cycle` retract animation (`[gear] animation "gear_cycle"`,
`retractable true`). Registered in `src/welcome_screen.cpp` and
`src/aircraft_installer.cpp` (`REQUIRED_FILES += a10/model/a10.glb`). See the
project notes for the FlightGear → ProvPilot porting pipeline.

