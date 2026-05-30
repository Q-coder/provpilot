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

### `[camera]`
Cockpit camera (Camera1) placement, in AC‑node‑local meters. Omit the whole
section to keep the scene default.
- `cockpit_position "x,y,z"` (x=right, y=up, z=back)
- `cockpit_rotation_deg "p,y,r"` *(optional)*, `cockpit_fov` *(optional)*

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
