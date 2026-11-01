# ASK 21 glider — FlightGear port

Source: the FlightGear **ASK21** package (GPL-2; Heiko Schulz, Thomas Polzer,
Viktor Radnai, Benedikt Wolf, Gilberto Agostinho and others — see
`provpilot/aircraft/ask21/jsbsim/aircraft/ask21/AUTHORS.md`), kept as the
untracked source tree `aircraft/flightgear-ask21`. Aircraft id `ask21`.
Started 2026-10-31 as an intermezzo during the MD-80 port, with the same
tool chain (docs/A10_FLIGHTGEAR_PORT.md, docs/MD80_FLIGHTGEAR_PORT.md).

## What is in

- **FDM**: the package's JSBSim variant (`ask21-jsb.xml` → `jsbsim/aircraft/
  ask21/ask21.xml`, `Systems/ask21-aero.xml` alongside, `ask21-fcs.xml` in
  `jsbsim/systems`). It is self-contained apart from four inputs that must
  exist (`jsbsim/init_properties.json`). No engine, no property mirror.
  Two edits in our copy of `ask21-fcs.xml` (marked "ProvPilot"): FlightGear
  flies the airbrakes on the throttle axis (throttle 0 = brakes fully out,
  which made the first airborne start dive at 8 m/s), so the airbrake
  command is now the maximum of that table and `fcs/speedbrake-cmd-norm`
  (B key, lever, joystick) with the throttle parked at 1 in the init list;
  the wheel brake likewise follows the pushed brake command, not only the
  last tenth of the airbrake travel. Winch, aerotow and ground handling forces are in the FDM
  but nothing drives them yet.
- **Model**: `scripts/fg_a10_build.py --src aircraft/flightgear-ask21 --prefix
  Aircraft/ASK21/ --root Models/ask21.xml --glb-name ask21_fg.glb --skip
  Effects/ --skip Immat/ --skip Liveries/ --skip Paintkit/ --skip Smoke/
  --set-xml aircraft/flightgear-ask21/ask21-main.xml` → 10 MB glb, 367
  animations, 95 clickable parts, 17 multiplayer aliases (speedbrake, canopy,
  wheel spin, wing flex). The pilots and the registration canvas are skipped;
  the FLARM display and the AR 6201 radio come from fgdata's
  `Aircraft/Instruments-3d/glider` and `ar-6201` folders, which are not
  vendored yet (holes in the panel).
- **Sounds**: `scripts/fg_a10_sounds.py --base-xml ask21-jsb-set.xml --prefix
  Aircraft/ASK21/`; the wind samples are fgdata's `Sounds/wind.wav`, now
  vendored in `third_party/fgdata/Sounds` (the sound builder resolves generic
  `Sounds/` paths there).
- **Bridge**: new glider instrument properties for every aircraft —
  total-energy variometer (`instrumentation/variometer/te-reading-mps`,
  `ilec-sc7/te-reading-mps`: climb rate plus the kinetic-energy term), yaw
  string from sideslip, wind speed / from-heading for the wind compass,
  elevator trim, airbrake lever, `surface-positions/speedbrake-pos-norm`.
- **cfg**: `[model] rotation_deg "0, 90, 0"`, scale 5, offset from
  `tests/probe_gear_height.tscn -- ask21`; `[camera]` front seat from the FDM
  EYEPOINT (AC units are 10 per model metre, see the cfg comment);
  `[start] agl_ft 3000 speed_kt 55`.

## Airborne start (Settings > Aircraft > Start)

There is no winch or tow yet, so a glider starts in the air. The Aircraft
tab has a **Start** section for every aircraft: height above the ground (ft),
airspeed (kt) and heading (runway heading, or a value). 0 ft / 0 kt is the
usual runway start; the cfg `[start]` section gives the defaults, the stored
values are `aircraft_<id>_start_agl_ft / _speed_kt / _heading_deg`.

`FlightSpawn::set_start` carries the values into the next Fly!; both the
first flight and the teleport call `JSBGodot::initialize_airborne`: level at
the height over the terrain, the calibrated airspeed set, then a JSBSim trim.
A glider (no engines) trims alpha and elevator along a −2° glide (`tCustom`
with `tWdot/tAlpha`, `tQdot/tElevator`; the standard longitudinal trim fails
on the speed axis without a throttle); powered aircraft use `tLongitudinal`.
A trim that does not converge is reported and the flight starts untrimmed.

## Metric readouts and the airbrake lever (2026-10-31, gery)

- The ASK 21 panel is metric (km/h ASI, metre altimeter, m/s variometer),
  so `[instruments] units "metric"` switches the HUD readouts to km/h, m and
  m/s (`FlightHud::set_metric`).
- **Settings > Aircraft > Controls**: "Throttle keys, triggers and axis work
  the …" Throttle or Airbrakes, stored as `aircraft_<id>_throttle_drives`,
  default from `[controls] throttle_drives` (the glider: `airbrakes`). In
  airbrake mode `JSBGodot` pushes `fcs/speedbrake-cmd-norm = 1 − throttle`
  (lever forward = brakes in, idle = fully out), the engines get no command,
  B toggles the lever between the two ends, and the HUD shows "Airbrakes: %".
- The rear-seat whiskey compass card rotates about its vertical axis with
  `instrumentation/magnetic-compass/indicated-heading-deg`; seen from the
  seat the card scrolls sideways as the heading changes — that is the
  indication, nothing else drives it.

## Builder fixes this port needed

- A file used several times imports its parts under the same names
  (`57mm_compass__scala`, `winter-4110__needle-m`, Blender adds `.001`…),
  and the animator's stem match applied every copy's record to every copy:
  the altimeter needle showed four times the altitude ("reads feet"), the
  ASI two needles, and the rear compass card rotated about the front
  compass and floated outside the fuselage. The builder now records the
  import's own object names (`local_names`, `exact: true`).
- `texrotate` (the yaw string's texture rotation) was unsupported; the
  animator now swaps such surfaces to a small UV-rotation shader.
- The model turns elevator and rudder by `surface-positions/*-pos-norm × 6°`
  and YASim scaled those outputs to ±4; the JSBSim variant gives ±1 / ±0.52.
  `[animation] elevator_pos_norm_per_deg 0.2` and `rudder_pos_norm_per_deg
  0.1333` publish degrees × factor instead (`surface-positions/elevator-pos-norm`
  is new in the bridge).
- The instrument variants (`/sim/model/livery/instruments/asi|alt|vsi`) are
  pinned to 0 (km/h, metres) in `init_properties.json`.

- A file imported several times (one altimeter per seat) shares its manifest
  tag, so animation centres resolved in the *last* copy's frame and the
  front-panel needles swung a metre away. Records now carry `frame` (their
  own sub-model instance, exact name) and the animator uses it; the MD-80
  model was rebuilt with the same fix.

- **Wing shading (2026-11-01)**: the wing segments shaded dark on one side
  and bright on the other because the AC3D add-on never marked sharp edges
  (crease applied to an empty mesh); the builder now re-applies each object's
  crease angle, see "Crease angles" in docs/A10_FLIGHTGEAR_PORT.md. glb
  rebuilt (animations.json kept: its source note is hand-written).

## Airbrake lever and transparency (2026-10-31, gery)

- The blue lever is FlightGear's **throttle** lever (the glider maps the
  throttle axis to the airbrakes: 1 = in, 0 = out) and reads
  `/controls/engines/engine/throttle`; the `/controls/flight/spoilers`
  animation belongs to the self-launcher variant. The bridge publishes the
  throttle input under that name, the control state no longer pins such
  FDM-owned lever properties (`FDM_OWNED` in `FGBridge::collect`), and the
  lever's mouse binding (`property-adjust`) sets the throttle input through
  `AircraftRig::_on_fg_control` — so keys, triggers and mouse all move the
  same lever and the airbrakes.
- Two animator fixes it needed: translate records may give their axis as two
  points (`x1-m … x2-m`), and interpolation tables are sorted by input the
  way SimGear does (the lever's table is written 1 → 0, 0 → 0.21).
- The grey squares were the instruments' shadow planes and glass: FlightGear
  blends any texture with alpha. The animator now alpha-blends every surface
  whose albedo texture carries alpha (77 on the glider, depth pre-pass), and
  the builder also emits the `Effects/model-transparent` effect objects as
  transparent material records.

## Variometer lag, inert hotspots (2026-10-31, gery)

- The total-energy variometer took the frame-to-frame true-airspeed
  difference raw, and the needle flickered next to its own position. The
  bridge now runs the reading through a one-second lag (`te_filtered_mps`),
  the response of the real Winter instrument.
- Ctrl+Shift+H painted the whole cockpit one green hotspot, and the airbrake
  lever could not be grabbed: the canopy glass carries click bindings that are
  Nasal only (`doors.canopyF.toggle()` and friends, five records), it sits in
  front of everything, and it took every click. `FGControls::setup` now keeps
  only records with a binding it can run (a property command, a Nasal call a
  shim or the built-in map understands, a `setprop`); the rest draw no hotspot
  and take no clicks. Because shims decide that, the rig loads
  `[model] nasal_shims` before the controls setup (an empty path clears the
  previous aircraft's shims). Control records also honour `exact` like the
  animations, so a file imported several times does not make one copy's part
  clickable through the other copies. The check reads the bindings where the
  click handlers do: `action` for picks and sliders, `increase` / `decrease`
  (and their shift variants, `release`) for knobs — the MD-80's flap lever and
  EFIS knobs are knobs. Left out per model: glider 5, MD-80 7 (canvas screens
  and knobs whose Nasal has no shim), A-10 10.
- **Airborne start rolled off in some runs** (a 33° bank and 5.8 m/s dive after
  15 s). The second trim (the one after the teleport) converged on a random
  sideslip, −0.4…−0.7°, and alpha 1.03–1.05° instead of 1.17°: JSBSim's wind
  model is not part of the initial conditions, it keeps the weather's wind and
  its MIL-spec turbulence draws random gusts on every FDM run, including the
  trim iterations. The longitudinal trim never touches beta, so the glider was
  released sideslipping. `JSBGodot::initialize_airborne` now starts from a
  clean IC (`InitializeIC`) and trims in still air without turbulence; the
  next physics frame applies the weather again. The trim log line prints
  beta, phi, rates, lateral commands, steady and total wind.
- Even so, hands off in the cfg's weather (5 kt from 250°, turbulence 1) the
  glider banks 30–40° and dives within 15 s of the release: the trim is done in
  still air, the weather arrives as a step on the next frame (beta −1.8° at
  t0, a 500 lb·ft yaw moment), and the MIL-spec turbulence keeps nudging a
  spirally unstable airframe. With a hand on the stick this is a normal
  glider; trimming inside the steady wind (the IC wind, which `RunIC` copies
  into FGWinds) is an open point. The smoke therefore clears `weather_layers`
  in the settings and pushes them (`_push_weather_settings`) before the
  flight starts — the JSBGodot node exists from scene start and a flight
  start does not push the weather again — so the FDM never sees wind or
  turbulence: the static layer applies below its base as well, and its
  turbulence once turned the 15 s sink check into a 1.2 m/s climb. Its
  altimeter check folds the measured quaternion angle to 0..180° like the
  expectation (the sign of w flips between runs).

## Tests

`tests/smoke_ask21.tscn`: airborne start from the cfg defaults, still gliding
after 15 s with the speed held and a glider's sink rate, the variometer
showing it, the airbrakes extending on the model, the wind sounds loaded.
Probes: `probe_ac_nodes.tscn -- ask21 <node>…` (AC-local positions for camera
placement), `probe_gear_height.tscn -- ask21`, `probe_cockpit_shot -- ask21`.

## Open

Winch and aerotow (the FDM has the forces, the hitch logic lives in Nasal),
FLARM / radio instruments (vendor from fgdata), the pilots (skipped), the
`mi` self-launcher variant, thermals.

## Airborne trim solver (2026-11-01)
The airborne start no longer uses JSBSim's FGTrim but the dynamic trim in
`JSBGodot::trim_longitudinal` (docs/MD80_FLIGHTGEAR_PORT.md step 8): the FDM
runs pinned to the initial conditions while a secant per axis nulls the
accelerations — for the glider alpha and the pitch trim command along the
-2 deg glide. The glider's "Pitch Trim Sum" adds trim and elevator at
unity, so the result is the same trimmed glide as before; `smoke_ask21`
covers it.
