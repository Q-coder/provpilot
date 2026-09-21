# Aircraft Configuration (`aircraft.cfg`)

ProvPilot keeps its engine code **aircraft-agnostic**: every airframe ships a
plain‑text `aircraft.cfg` that wires a 3D model and a JSBSim flight model
together, plus the per‑aircraft visual/animation tuning. Adding or tweaking an
aircraft is mostly a matter of editing this file — no engine code changes.

- Source of truth: `provpilot/aircraft/<id>/aircraft.cfg`
- Parser: `provpilot/aircraft_config.gd` (`AircraftConfig.load_from_file()`)
- Consumers: `provpilot/node_3d.gd` (visuals/animation) and the C++ GDExtension
  `src/jsbgodot.cpp` (flight model).

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

### `[jsbsim]`
`model` — JSBSim model name. Data lives under
`<aircraft_dir>/jsbsim/{aircraft,engine,systems}`; the file
`<aircraft_dir>/jsbsim/aircraft/<model>/<model>.xml` must exist.

### `[model]`
- `file` — model path relative to the aircraft dir; `res_file` — `res://` fallback.
- `root_node` — scene‑tree path the model is parented under. (The PC‑7 reuses the
  Cessna loader node and is swapped in at runtime.)
- `rotation_deg "x,y,z"`, `offset "x,y,z"`, `scale` / `scale_xyz` — optional
  fitting transform applied to the reparented model (orient nose‑forward, lift
  the belly to ground level, rescale).
- `flip_normals` (bool) + `flip_normals_prefix` (str) — flip baked normals so the
  sun lights the top, not the underside. Empty prefix flips **all** meshes
  uniformly; a prefix flips only matching mesh names. Calibrate against the
  Cessna: a mesh whose top faces measure “up” needs `flip_normals true`.

#### FlightGear models (A-10)
`fg_animations "model/animations.json"` — manifest written by
`scripts/fg_a10_build.py`; the `FGAnimator` extension runs every FlightGear
animation of the model with values from `fg_bridge.gd` (see
`docs/A10_FLIGHTGEAR_PORT.md`). `hide_nodes "a, b"` hides model nodes by name
(all matches), used for placeholder display textures. `scale` fits a model
authored in metres: the AC frame draws models at 0.2 m per unit, so `5.0`
gives real size. Generic FlightGear models the package references
(`/Aircraft/Instruments-3d/...`) come from `third_party/fgdata`; see
`docs/INSTRUMENTS.md` for how instruments are built and added.

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

### `[gear]`
Retractable landing gear (toggled with **G**; JSBSim drives drag + weight‑on‑
wheels from `gear/gear-pos-norm`).
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
scale, so one offset unit is not one metre — adjust, re-run, converge (the
A‑10 needed −0.85 for a 0.2 m/unit response; PC‑7 +4 cm, within tolerance).

### `[lights]`

Exterior lights, drawn by `aircraft_lights.gd` (node `AircraftLights` under
the scene root). Each key lists model mesh nodes (comma separated, found by
name anywhere in the model); the light sprite sits at the mesh's bounding-box
centre and follows the node, so lights on animated gear retract with it.

| key | fixture | state |
|---|---|---|
| `nav_left`, `nav_right`, `nav_tail` | red / green / white steady | on while an aircraft is loaded |
| `strobe` | white double flash (1.2 s) | airborne (no weight on wheels) |
| `beacon` | red flash (1 Hz) | always |
| `landing`, `taxi` | white sprite + `SpotLight3D` on the ground | **L** toggles both; out while the gear is retracted |
| `<kind>_pos` | `"x, y, z"` in model-root units | alternative for models without a lens mesh |
| `<kind>_anchor` | animated part the lens rides on | for lens meshes without animation tracks (the A-10 nose-gear lights): the lens is reparented under the part with the offset from the clip pose in which they coincide |
| `landing_pitch_deg` | spot aim below the aircraft's forward axis (default 5) | |
| `landing_range_m`, `taxi_range_m` | spot ranges (300 / 90) | |
| `spot_energy` | landing spot energy (12; taxi half) | |
| `pair_meshes` | `false` leaves the `-on`/`-off` lens meshes to another system (the A-10's FlightGear selects, fed by `fg_bridge.gd`) | |

Each sprite is placed a few model-centimetres outside its lens (outboard for
wingtip lights, up for the beacon, forward for the nose lights) and drawn
with the depth test on, so the airframe hides lights on its far side. Note
that the airframes are drawn at a fraction of world scale (the `AC` node is
0.1, the A-10 ends up at 0.2 m per model metre, wingtip to wingtip 3.6 m in
world metres), so anything metre-based on the model must use model units.

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
`engine` (ogg path) and RPM‑mapped `pitch_*` / volume tuning.

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
   `model/<id>.glb`.
2. Write `aircraft.cfg` (copy an existing one; the Cessna file documents the
   format in‑line).
3. Add the aircraft to the welcome‑screen list in
   `provpilot/welcome_screen.gd`.
4. Bump `BUNDLE_VERSION` in `provpilot/aircraft_installer.gd` (and add any
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
clickable (left / right click, wheel, Shift for coarse steps; see
`docs/A10_FLIGHTGEAR_PORT.md` subtask 5). **H** cycles the ProVPilot HUD
overlay for every aircraft: off, bottom readouts, readouts + upper info.

Ported from the FlightGear A-10 (its standalone JSBSim FDM: twin TF34 turbofan,
retractable tricycle gear, flaps, speedbrake). The visual model is a
self-contained `model/a10.glb` exported from Blender with embedded textures,
double-sided materials, outward normals (`[model] flip_normals false`), and a
merged `gear_cycle` retract animation (`[gear] animation "gear_cycle"`,
`retractable true`). Registered in `welcome_screen.gd` and
`aircraft_installer.gd` (`REQUIRED_FILES += a10/model/a10.glb`). See the
project notes for the FlightGear → ProvPilot porting pipeline.

