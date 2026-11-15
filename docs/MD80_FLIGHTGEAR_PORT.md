# MD-80 port from the FlightGear package — plan and progress (2026-10-31)

Goal (gery): fly the FlightGear McDonnell Douglas MD-80 by Josh Davidson
(Octal450, GPL-2.0, `aircraft/MD-80`) in ProVPilot, with its Nasal handled
as an optional module that other aircraft can ignore. Variant: MD-82
(JT8D-217A). Source of truth: the package's JSBSim FDE, model XML, AC3D
meshes and sound set, like the A-10 port (docs/A10_FLIGHTGEAR_PORT.md).

## 1. What the package is

| Part | Size | Where it runs in FlightGear |
|---|---|---|
| JSBSim FDE + 20 `<system>` files (electrical, fuel, hydraulics, pneumatics, APU, pressurization, HVAC, fire, FCS, spoilers, ignition, thrust limits, DFGS servos, IRS/AHRS, instrumentation, cockpit, performance) | 1 768 properties defined | JSBSim |
| Model XML: 184 files, 302 sub-models, 978 animations (rotate 323, select 236, translate 160, textranslate 136, material 108), 324 clickable controls | | SimGear scene graph |
| Nasal: 10 400 lines (DFGS autoflight logic 1 800, HT9100 FMS/MCDU 2 000, canvas PFD / ND / EDP-SDP / FMA 1 800, systems glue, radios, cockpit panel, acconfig, autopush) | | FlightGear Nasal VM |
| FlightGear property rules (`Systems/dfgs-*.xml`, `core-rules.xml`, `instrumentation.xml`: PID / filter / logic) | 181 rules | FlightGear autopilot subsystem |
| Canvas displays (PFD, ND, EDP/SDP, FMA, MCDU) | | FlightGear Canvas |
| Sound set `Sounds/sound.xml` | 77 sounds | FlightGear sound manager |

The decisive fact: the **aircraft systems live in JSBSim**, reading
FlightGear-side properties (`/controls/...`, `/instrumentation/...`,
`/engines/engine[n]/n1`, `/position/altitude-ft`, 365 of them) and writing
their results under `/systems/...`, `/engines/engine[n]/*-actual`,
`/instrumentation/ai[n]/...`. Nasal is glue around that core, plus the
displays and the autoflight *mode* logic.

## 2. Architecture

**D1 — FDM as delivered.** `aircraft/md80/jsbsim/` holds `FDE/MD-82.xml`
(as `aircraft/md80/md80.xml` with its `FDE/Config`), `Systems/` and
`Engines/` copied unchanged; the APU is JSBSim engine[2]. In standalone
JSBSim a property path with a leading slash is absolute (the FlightGear
tree), one without lives under `fdm/jsbsim`: the systems' `/controls/...`
references land at the root, JSBSim's own `fcs/...` under the FDM node.
Every FlightGear-side property a system references must exist before the
first run (a JSBSim function throws otherwise), so
`jsbsim/init_properties.json` lists all 365 with defaults, overlaid with the
"after start" panel state of the package's acconfig (battery, generators,
pumps, bleeds, IRS, gear lever, dial-a-flap), and a few FDM-root properties
newer JSBSim versions tie and ours lacks (`sim-time-sec`,
`inertia/empty-weight-x-in`, `propulsion/engine[n]/IdleThrust`,
`propulsion/tank[n]/x-position`). `[jsbsim] init_properties` is applied
after the model loads and on every Fly!.

**D2 — FlightGear property mirror (FGBridge, generic).** With `[jsbsim]
fg_property_mirror true`, the bridge pushes its FlightGear-side values into
the FDM every frame (controls incl. the cockpit switch state, instruments,
flight state, environment, the engine interface n1 / n2 / fuel flow) and
pulls back every property the model animations or sounds reference that the
FDM defines. `[electrical] power_property` (the MD-80's `/systems/electrical/bus/dc-l`)
replaces the built-in electrical model. Other aircraft are untouched.

**D3 — Model.** `scripts/fg_a10_build.py` generalised: `--prefix
Aircraft/MD-80/ --root Models/MD-82.xml --glb-name md80_fg.glb --skip ...`,
SimGear `include=` / `omit-node` PropertyList includes, bare `.ac`
sub-models, no A-10 overrides. Output `model/md80_fg.glb` (20 MB, 1 882
objects) + `animations.json`. Effects, services (chocks, pushback),
liveries (default textures stay) and the paint kit are skipped; the canvas
screens are hidden via `[model] hide_nodes`.

**D4 — Sounds.** `scripts/fg_a10_sounds.py --base-xml MD-80-main.xml
--prefix Aircraft/MD-80/` exports the 77 sounds; their conditions read the
FDM-mirrored properties and the `sim/sound/*` pulses the cockpit clicks set.

**D5 — Nasal as a module (not in this step).** The 448 cockpit bindings
call 65 distinct Nasal functions; 231 of them are the three sound-click
helpers (`core.Sound.switch1/btn1/knb1`: pulse a `sim/sound/*` property for
0.2 s), the rest adjust radios, EPR bugs, thrust-limit mode, the EFIS and
the autopilot panel. Two ways forward, in order:
1. *Data-driven shims* (`aircraft/<id>/nasal_shims.json`): map a script head
   to a property action (pulse / toggle / adjust / assign). Covers the
   clicks, radios, EFIS, thrust-limit mode and dial-a-flap without an
   interpreter.
2. *A real Nasal runtime* (`src/nasal/`, the SimGear Nasal C library, LGPL)
   with the FlightGear stdlib (`props`, `globals`, `math`, `string`, timers,
   listeners) and our property tree behind `getprop/setprop`, loaded per
   aircraft from `[nasal] modules`. Canvas stays out (static screens) and
   `fgcommand` becomes a small table. This runs the systems glue, radios,
   cockpit panel and the DFGS mode logic unchanged. It is the module gery
   asked for: it must only touch aircraft that list Nasal modules.
Prerequisite for the DFGS: the FlightGear property-rule interpreter (D6).

**D6 — FlightGear property rules (started in step 12 with
`sound-control.xml`: logic + filter components; the controllers are still
open).** `Systems/dfgs-*.xml`,
`core-rules.xml`, `instrumentation.xml` are FlightGear `<autopilot>` files
(pid-controller, pi-simple-controller, filter, predict-simple, logic,
flipflop). A C++ `FGPropertyRules` module (~600 lines) evaluating them each
frame over the same property dictionary gives the autoflight servos, the
EPR constants `/systems/engines/epr/c1,c2`, the engine cutoff hand-off and
several instrument helpers. Without it those properties keep their init
values.

## 3. Progress

### Step 1 — FDM in ProVPilot (2026-10-31)
- `aircraft/md80/` (both copies): jsbsim tree, `aircraft.cfg`,
  `jsbsim/init_properties.json`, sounds.
- C++: `JSBGodot::has_property`, `AircraftRig::apply_fdm_init_properties`,
  `FGBridge` mirror (`set_fdm_mirror`, `power_property`, pull keys from the
  animator and the sound set), FlightGear flight-state paths every aircraft
  now publishes (`position/altitude-ft`, rates, environment,
  `sim/time/elapsed-sec`, `controls/flight/flaps`...), real engine values for
  mirrored aircraft; welcome list and installer entries.
- `tests/smoke_md80_fdm.tscn`: load, engines running, buses, hydraulics,
  bleed, gear, mirror counts, throttle response.

### Step 2 — model, mirror, shims (2026-10-31)
- Model built: `fg_a10_build.py` generalised (prefix / root / glb name / skip
  list, PropertyList `include=` + `omit-node`, bare `.ac` sub-models, a
  model file's own `<offsets>` now move its nested sub-models too — the
  MD-80 airframe file shifts everything 22.6 m nose-ward, which had left the
  cockpit at the model origin). `md80_fg.glb` 20 MB, 1 882 objects, 978
  animations + 324 controls; canvas screens hidden; gear offset 6.49 units
  (`probe_gear_height`); captain's eye from FlightGear view 0.
- Property mirror: 44 inputs pushed, 426 outputs pulled per frame; every one
  of the model's 632 properties is bridged or mirrored (0 unbridged).
  Standalone-JSBSim path rule found the hard way: leading slash = absolute
  (FlightGear side), none = `fdm/jsbsim`; newer-JSBSim ties the systems
  expect are created by the init list (`zero`, `aero/arm-factor`,
  `sim-time-sec` refreshed each frame, `inertia/empty-weight-x-in`,
  `IdleThrust`, tank `x-position`). The cockpit switch state is seeded from
  the init list (and, for switches no animation reads, from the FDM's
  current value), so the panel starts in the "after start" state and a
  click on the battery switch turns it off, not on.
- Nasal shims (D5 step 1): `FGControls` loads `model/nasal_shims.json`
  (34 entries): click sounds pulse `sim/sound/*`, thrust-limit mode, manual
  EPR bugs, dial-a-flap, gear handle, flap lever, speed brake, NAV / ADF /
  COMM tuning (NAV 1 feeds ProVPilot's receiver), EFIS mode and range,
  transponder knob, autopilot panel inputs. Pulses expire in
  `FGControls::tick`.
- Tests: `smoke_md80_fdm` (engines, buses 28 V / 115 V, hydraulics 3 000
  psi, bleed, gear, mirror, thrust), `smoke_md80_cockpit` (model, 399
  clickable parts, shims, hidden screens, sounds, battery switch click ->
  pulse + FDM, NAV knob -> receiver).

### Open after step 2
- Cockpit textures: the glareshield / FGCP / overhead label plates render
  white (FlightGear `<effect>` or `texture-prop` materials the builder does
  not apply); the instruments themselves are textured.
- Canvas displays (PFD, ND, EDP/SDP, FMA, MCDU) are hidden; the EDP/SDP
  "off" plate shows. The analog standby and engine gauges work.
- D6 property rules (DFGS servos, EPR constants, `core-rules` engine
  cutoff hand-off) and the Nasal runtime (D5 step 2) are not started.
- Flap lever: the FDM reads `controls/flight/flaps` (lever 0..0.91) which
  the mirror derives from `fcs/flap-cmd-norm` with the cfg detents; the
  FDM's own `fcs/flap-pos-deg` drives the surfaces. Check in flight.
- Autobrake, pushback, ground services: not wired. Thrust reversers: step 25.

### Step 3 — check flight fixes (2026-10-31, gery's first flight)
Reported: fans and gear doors on wrong pivots and "detached" parts, no
surface motion, mirrored yaw, GPWS altitude callouts on take-off, C and H
keys dead with only the spot view.
- **Pivots**: the manifest's sub-model `node` named the placement empty,
  while the airframe meshes sit under the file's own-offset empty 22.6 m
  away; the animator resolves centres in that node's frame, so every pivot
  was 22.6 m off. `node` now names the frame empty (`placement` keeps the
  other). Tyres, doors and fans rotate in place (`pivot_check` probe).
- **Surfaces**: the model animates `rotors/main/blade[n]/flap-deg`,
  `sim/multiplay/generic/float[n]` etc., which the -set file *aliases* to
  the FDM's `/systems/fcs/.../final-deg`, the gear roll speeds and the
  spoilers (a multiplayer trick). The builder (`--set-xml`) writes those 41
  aliases into the manifest and the bridge resolves them every frame.
  Note for tests: the ailerons and elevators are tab-driven and the FCS
  gives them no authority below 7 psf, so on a stationary aircraft only the
  control tabs and the hydraulic rudder move (`smoke_md80_cockpit` checks
  `AilTabControlL` and `Rudder`).
- **Yaw**: FlightGear's `/controls/flight/rudder` is +1 for right pedal and
  its JSBSim interface negates it into `fcs/rudder-cmd-norm`; the bridge
  published the JSBSim sign, so the MD-80's FCS (tiller, yaw damper) saw the
  pedal mirrored. Fixed for every aircraft (the A-10 pedal animation turns
  the FlightGear way now).
- **Callouts**: `/position/gear-agl-ft-2-sec` is FlightGear's
  predict-simple (height two seconds *ahead*); the "10 .. 500" sounds play
  when the prediction is below the current height. The bridge now publishes
  `position/gear-agl-ft` and the prediction from a two-second history, so
  climbing is silent and descending calls.
- **Keys / spot view**: reproduced the flow headless and in a window: the
  intro zoom holds about 60 s after Fly! (keys are ignored while it plays,
  Space skips it), then C and H act normally at 63 fps. Could not reproduce
  dead keys after the intro; if it recurs, note whether the intro HUD was
  still showing.

### Step 4 — second check flight (2026-10-31, gery's screenshot)
- **"Minimums" repeating on the ground**: the CAWS conditions compare two
  properties (`/position/gear-agl-ft` against `/systems/misc/minimums-minus-50`
  etc.). The sound set's property collector only knew the single-string
  form, so the FDM-side right-hand sides were never pulled and read 0: every
  descent flicker of the 2-second predictor below 200 ft "called minimums".
  Both sides are collected now, and the predictor ignores rates under
  0.5 ft/s (FDM settling noise on the ground).
- **Overlapping altimeter / DH module**: `<model><condition>` (FlightGear
  shows a sub-model only while the condition holds) was ignored by the
  builder, so the EFIS *and* the analog instrument sets were both in the
  panel. The builder now emits a `select` animation on the sub-model's
  placement empty (`exact: true`, so the Blender duplicate stem does not
  catch the other copy; manifest node names use Godot's `_001` suffix). The
  options are set analog in `init_properties.json` (`efis` 0, `edp-sdp` 0,
  `combined-standby` 0, no aux tanks): PVP has no canvas displays, the
  analog ADI / HSI / radio altimeter / engine gauges animate.
- **Eye point**: `[camera] cockpit_position` raised 0.4 m, and the cockpit
  camera can be moved in every aircraft (MSFS style): Ctrl+arrows sideways /
  up-down, Ctrl+Shift+Up/Down forward / back, Ctrl+Space back to the
  default; 0.5 m/s, 1.5 m range, saved per aircraft in provpilot.cfg
  (`aircraft_<id>_eye_offset`).
- **Keys after the scenery editor**: F8 / Escape round trips in flight keep
  C and H working (probe `probe_keys_after_editor -- f8|esc|editmode`). The
  reproducible failure was welcome *Edit Mode* → Escape → **Continue**: Edit
  Mode parks the aircraft hidden at the edited airport, and Continue resumed
  that non-flight (no aircraft, spot view, cockpit camera inside nothing).
  Continue is disabled after an Edit Mode session and never resumes with
  the aircraft hidden.

### Step 5 — third check flight (2026-10-31, EFIS over the engine panel)
- **EDP/SDP displays over the round engine gauges**: FlightGear lets an
  animation name a `<model>` of the same file by its `<name>` (the centre
  panel's `select` switches `edpsdp` / `trp` / `wntart` against the 25 round
  gauges). The builder now maps such names to the sub-model's placement
  empty (exact match); the sub-models are imported before the file's
  animations so the names exist.
- **Keys "changing over time"**: macOS swallows the key-up of Cmd and Shift
  after a system shortcut (Cmd+Shift+4 screenshot), so Godot's `Input` kept
  them pressed: C (needs no modifier) matched nothing and H read as
  Ctrl+Shift+H, the hotspot overlay. `KeyBindings` now takes the modifier
  state from the latest input event and releases a stale modifier with a
  synthetic key-up, so the next mouse move heals it.
- **Hotspot boxes of hidden panels**: the overlay drew boxes for the EFIS
  selector panels switched off by their condition (clicks already ignored
  hidden parts); the overlay skips them too.
- **Nose wheel**: the model rotates the strut by `gear/gear[0]/steering-norm`
  (via a -set alias), FlightGear's steering angle over the gear's
  `max_steer`. The bridge publishes it from `fcs/steer-pos-deg[0]` and the
  new `[jsbsim] nose_steer_max_deg` (82 for the MD-80).
- **Elevator**: not changed. `/controls/flight/elevator` keeps the JSBSim
  sign (FlightGear's JSBSim interface negates only the rudder). The MD-80's
  elevators are tab-driven and free-floating: on the ground they do not
  follow the yoke (zero authority below 7 psf), only the control, geared and
  anti-float tabs move, and in flight the surface follows the tab with the
  FDM's lag and alpha float — ask for a description or video if it still
  looks wrong.

### Step 6 — fourth check flight (2026-10-31, gery: "instruments have no power")
Reported: ADI and nav instruments dead with engines running and generators
on, APU switch only OFF/RUN, white squares on some switches.
- **Dead ADI / HSI / RMI**: not power — every `power-avail` was 1. The
  flags are the *platform*: `att-avail` and `heading-avail` need
  `/systems/platform[0]/aligned`, the AHRS (`acconfig/options/nav` 0) never
  aligned because its `can-align` flip-flop only latches when `aligned` or
  `/systems/acconfig/autoconfig-running` is 1. FlightGear's acconfig holds
  that flag for five seconds after setting the panel state and the systems
  shortcut on it (instant alignment, gauges to their place). The init list
  now has a generic **`_pulse`** entry (`{"/systems/acconfig/autoconfig-running": 5}`):
  `JSBGodot` holds it at 1 for that much *FDM time* after every (re)start
  (wall time would expire while the scenery loads) and then clears it. The
  AHRS aligns within the pulse; ADI and HSI are live from the first frame.
- **APU START**: the knob's upper half is a press-and-hold pick (`mod-up`
  springs 2 back to 1), which `FGControls` already does — hold the mouse
  button two seconds and the starter latches (`starter-run-start-timer`).
  Two things were missing behind it: FlightGear's `core-rules.xml` copies
  `/systems/apu/starter` and `/cutoff` (and the ignition system's for the
  JT8Ds) into `/controls/engines/engine[n]/starter|cutoff`, and its JSBSim
  interface hands those to the engines every frame. Both are generic now:
  the init list's **`_links`** (`{dst: src}`, copied each step) and
  `JSBGodot::apply_property_links_and_pulses`, which calls `SetStarter` /
  `SetCutoff` on every engine whose FlightGear control properties exist.
  The bridge also publishes *every* FDM engine to the FlightGear engine
  interface, not two: the APU is engine[2] and the APU system spools from
  `/engines/engine[2]/n2`.
- **Three copies of the aircraft data**: `provpilot/aircraft/md80` (res://),
  the repo-root `aircraft/md80` (what headless smokes and editor runs load:
  the install root is the first folder with a `provpilot.cfg`) and
  `/Users/gery/provpilot/aircraft/md80` (the packaged app). Sync both after
  editing; the log line `JSBSim root:` names the loaded one.
- **White squares on switches** (L GEN, R GEN, APU master; the cockpit and
  cabin temperature slews, the trim handles and the ASI pointers too): the
  FlightGear model's `<visible>false</visible>` picks — sense surfaces for
  the halves of a three-position switch that FlightGear never draws. The
  importer keeps them as plain meshes, so they rendered as white quads.
  `FGControls::setup` now takes every `visible=false` pick (actionable or
  not) out of the camera's layers (`layer_mask 0`, no shadows); the mesh
  stays in the tree for the ray, so the halves still take clicks. Log line:
  "N invisible sense surfaces taken out of the camera's layers" (14 here,
  4 on the ASK 21's altimeter knob, 5 on the Cessna / PC-7 gyros).
- **Mouse house rule** (gery): on anything that moves both ways the left
  button and wheel down go *down / counter-clockwise*, the right button and
  wheel up go *up / clockwise*. FlightGear aircraft disagree about which of
  their buttons 0 / 1 / 3 / 4 is which way (this model pairs `[0,4]` / `[1,3]`
  on the overhead, `[0,3]` / `[1,4]` on the hydraulic panel, and the APU
  START half has wheel-up going *down*), so `FGControls::press_item` reads
  the direction from the action itself (`action_direction`: the sign of a
  `property-adjust` step, the order of a `property-cycle` list, else the
  wheel button the action is paired with) and the listed buttons only break
  ties; pushes, toggles and one-way halves keep FlightGear's numbering.
  `knob` records: right / wheel up = increase (clockwise). The release runs
  `mod-up` of the action that actually ran, not of every action on the
  record. So the APU is started by holding the *right* button on the upper
  half of the knob; the left button on it moves the knob down to OFF.
- **Switches stop at their ends** (gery: the landing light switch jumped
  from ON back to RETRACT on the next click, same with the wheel):
  FlightGear's `property-cycle` wraps by default and this model never sets
  `wrap`. `FGControls` now wraps only when `wrap` is true or the list is a
  plain toggle — two values that are all the property is ever cycled over
  in this cockpit (`cycle_values`, gathered over every record at setup).
  A two-value list that is a sub-range (the APU master's lower half cycles
  `[1,0]` of `{0,1,2}`, the L GEN upper half `[0,1]` of `{-1,0,1}`) is a
  one-way half and stops; three or more values always stop.
- Tests: `smoke_md80_cockpit` checks the aligned platform, the live ADI /
  HSI, the published APU engine, the hidden L GEN / APU sense surfaces and
  the house rule on EMER LTS (switch), EMER PWR (knob) and APU START (hold),
  the end stops of the landing light switch and the APU master's lower half; `probe_md80_power.tscn` prints the
  power / alignment / APU chain and starts the APU through the control state.

### Step 7 — FlightGear `<text>` elements (2026-11-01, gery: "frequency and course are not displayed in the two NAV instruments")
The FGCP's NAV frequency and CRS windows (and HDG / IAS / ALT / VS, the
pedestal COMM, ADF and transponder heads, the fuel totaliser) are not
textures: FlightGear draws them with `<text>` elements — osgText at
`<offsets>` in the model file's frame, `number-value` with a printf
`<format>` on a property, `text-value` (a string) or `literal`, in a 7-segment
`<font>` from the package's `Fonts/`, `<character-size>` metres tall,
anchored by `<alignment>` (`right-bottom`), lying in `<axis-alignment>`
(`yz-plane`: baseline +y, up +z, facing +x aft). The builder ignored them,
so the windows were empty. Generic now:
- `fg_a10_build.py` (`import_texts`): an empty per text placed like a
  sub-model (`place`), the record in the manifest's `texts` list ({model,
  node, frame, raw}); the referenced fonts copied to `model/fonts/`
  (`fonts` map; LiberationSans is not shipped and falls back to Godot's
  default). A text's `<name>` is an object other animations select; a
  unique name goes into `local_names` (exact), a name several texts share
  (`Digits.Test`, one per window) lists every copy so one exact select
  reaches them all. Octal450's `<prop>-fmt` properties are the Nasal's
  working copies of `<prop>`: the builder points the text at the base
  property and keeps the original as `property_fg`.
- `FGAnimator::load_texts`: a `Label3D` under each empty — the aircraft's
  font (`FontFile::load_dynamic_font`), 64 px glyphs at `character-size /
  64` m per pixel, alignment, the plane as a basis (yz-plane = +90° about
  y in the Godot frame), 0.3 mm in front of the panel, unshaded, alpha
  cut. `update_texts` runs the format through `snprintf` like osgText
  (one conversion; `%003.0f`, `.%3.0f`, `+%04.0f` as written). Select
  animations hide the empty (power, annun-test); a material animation on
  the text tints the label (`diffuse` × `factor-prop`, so the FGCP digits
  are amber at `systems/lighting/fgcp-digital`). FlightGear ships
  `controls/lighting/fgcp-digital` / `capt-digital` / `fo-digital` at 1
  (`MD-80-main.xml`); the init list had 0, which the Digits select treats
  as off. NAV 2 is seeded with FlightGear's 109.55.
- Tests: `smoke_md80_cockpit` checks 61 texts built, the NAV 1 window
  showing the tuned frequency, lit, and the CRS window following the
  course knob; `probe_md80_text_shot.tscn` saves close-ups of the FGCP and
  the pedestal (`CameraRig::set_head_look`, new, aims the head directly).
- Not done: the FGCP's autoflight windows (HDG, IAS, ALT, VS) read
  `it-autoflight/*`, which only the Nasal autoflight writes — they stay
  dark until D6; `text-value` strings (`alt-thousands`) the same.

### Step 8 — CG, pitch trim, IVSI, airborne start (2026-11-01, gery: "does the fuel lever change the CG? I pull a lot to stay level; the VSI does not work")
- **CG is modelled**: the FDM's tanks and point masses carry positions and
  JSBSim recomputes weight and CG every step. Two things hid it. The fuel
  slider filled every tank to the same share, and with this FDM's tank arms
  the fuel's own centre then sits exactly on the aircraft CG, so the bar could
  not move; `JSBGodot::set_fuel_percent` now loads the wing tanks first and the
  centreline tanks after (`apply_fuel` uses it). And the settings screen pauses
  the FDM: `set_tank_contents_lbs` feeds the tank weights into the mass
  balance and runs it at once, so the CG reads follow the slider. `% MAC`
  comes from the cfg's `[performance] mac_le_x_in` / `mac_in` when given
  (the MD-80's own `inertia/cg-percent-mac` constants: 908.06 in, 120.47 in),
  else from the FDM's function, else the 25 % estimate; the MD-80's load
  manager limits (`/systems/load-manager/cg-fwd-limit` / `-aft-limit`,
  weight dependent, from the FCOM) show under the bar.
- **Pitch trim goes through JSBSim's trim command** (`fcs/pitch-trim-cmd-norm`)
  instead of being added to the elevator: the conventional aircraft sum the
  two at unity in their "Pitch Trim Sum", so nothing changes for them; the
  MD-80 drives its stabilizer from it (`controls/flight/elevator-trim`, which
  the mirror derives from the trim command: 12.5 deg per unit, -12.5 .. +2.5).
  Before, the stabilizer sat at 0 deg and the column carried the whole trim,
  hence the pull. `[jsbsim] pitch_trim_default -0.24` starts a session at
  FlightGear's 3 units nose-up. The trim keys / buttons move the trim command
  (the stabilizer follows at its own 1/3 deg/s).
- **IVSI**: the model's `vert-speed-fpm` channel reads
  `it-autoflight/internal/vert-speed-fpm`, which only the Nasal autoflight
  wrote (on the ground it shows -75 x pitch rate, the "moves down on
  rotation"). `_links` values may now be `{src, factor, offset}`; the init
  list links it from `velocities/h-dot-fps` x 60.
- **Airborne start — dynamic trim** (`JSBGodot::trim_longitudinal`, all
  aircraft): JSBSim's FGTrim evaluates every probe with the time step at 0,
  so an FDM whose column, stabilizer or engines answer through filters,
  actuators and spool dynamics shows it no response and it diverges (it did
  for the MD-80: every axis "Failed"). Now the FDM runs at its real rate with
  the vehicle state pinned to the initial conditions after each step
  (`FGPropagate::SetInitialState`), the actuators and systems settle, one
  zero-step read gives the turbines' static thrust, and a secant per axis
  nulls the accelerations: alpha for w-dot, throttles for u-dot (JSBSim's
  command plus the FlightGear levers the thrust limiter reads), the trim
  command for q-dot; a residual the trim cannot take stays on the elevator.
  What it had to work around, all generic: the FDM is *held* before release
  (held models skip) — lifted for the solve; JSBSim's starvation flag was
  latched from ground frames with empty feed pipes and only clears while fuel
  is consumed — a few un-trimmed steps first, the pipes (feed tanks under
  100 lb) topped up, engines re-lit; the systems read the FlightGear engine
  interface the bridge normally mirrors — `mirror_engines_to_fg` (n1, n2,
  running) each pinned step; the autoconfig flag that speeds the stabilizer
  also zeroes the rated thrust — the throttle axis and a final refinement run
  with it off, and the flag is cleared after the solve (it had stayed at 1
  once its pulse had already ended: idle thrust for the whole flight). About
  2 s and 10 000 pinned steps; the log prints every round.
- Tests: `smoke_md80_airborne` (8000 ft, 250 kt: column centred, stabilizer
  carries the trim, FlightGear elevator-trim mirrors it, level within 10 fps,
  autoflight vs linked, IVSI follows a pull, weight and CG change with the fuel
  slider without an FDM step).

### Step 9 — GS flag, gear, fuel quantity (2026-11-01, gery: "GS flag on during the whole approach; gear not animated outside; fuel qty blank")
- **GS flag**: the HSI's flag (and the glide slope needle's select) need
  `instrumentation/nav[0]/signal-quality-norm` above 0.99. The receiver gave
  `1 - distance / range`, i.e. below 0.99 beyond a quarter mile: the flag was
  up everywhere except over the antenna (and on the ground at LSZH, which is
  the "GS shown before take-off" the pilot saw). FlightGear's quality is 1
  within range; `RadioNav` now holds 1 and fades over the last tenth of the
  range, for VOR and ILS alike.
- **Gear**: this FDM has no gear kinematic channel — FlightGear's `gear.nas`
  moved the legs and JSBSim never copies `gear/gear-cmd-norm` into
  `gear/gear-pos-norm` by itself, so the legs stayed down in the FDM (drag,
  WOW) and in the model whatever the lever did. `[gear] fdm_position false`
  makes `JSBGodot` move the position towards the command over
  `retract_seconds` (10 s here); the bridge's `gear/gear[n]/position-norm`
  then animates the exterior.
- **Fuel quantity**: the pedestal indicator's digits are text elements
  selected by `systems/lighting/pedestal-digital`, whose knob the init list
  had at 0; every `controls/lighting/*` default now follows
  `MD-80-main.xml` (pedestal-digital 1).
- Tests: `smoke_md80_airborne` retracts the gear (position after 3 and 12 s,
  the model property), `smoke_md80_cockpit` reads the left main tank
  readout, `smoke_radio_nav` still passes with the new signal quality.

### Step 10 — brakes (2026-11-01, gery: "a wheel brake axis; is the autobrake available?")
- The MD-80's flight controls compute `fcs/left|right-brake-cmd-norm`
  themselves: max of `/controls/gear/brake-left|right`, `brake-parking` and
  `/systems/abs/brake-output`, through a hydraulic actuator, forced to 1 while
  the autoconfig flag or the chocks are on. PVP's push of JSBSim's command was
  overwritten every step, so pedal braking did nothing on this aircraft (the
  start held because of the setup flag). `copy_inputs_to_JSBSim` now also
  writes the FlightGear pedal properties when the FDM has them.
- **Autobrake is available**: it is the JSBSim system "DFGS: Autobrake"
  (`md-80-dfgs.xml`): the pedestal AUTO BRAKE selector (`controls/cockpit/abs-knob`,
  OFF / MIN / MED / MAX / T.O.) and the ARM switch (`controls/abs/armed`) are
  clickable; `can-arm` wants the spoilers armed and the usual conditions,
  the T.O. mode is RTO protection and disarms with the throttles; the
  brake output enters the max above. Not flight-tested yet.
- Joystick: `Flight: Wheel brakes` axis and the per-frame poll of the flight
  axes (docs/AIRCRAFT_CONFIG.md, Joystick).
- Tests: `smoke_md80_fdm` presses the pedal and reads the MD-80's own brake
  command.

### Step 11 — lights, brightness, flaps (2026-11-01, gery: "landing lights don't work; digital lights too bright; no flood lights; flap detents wrong")
- **Landing lights**: the model has no light meshes (FlightGear's lights are
  effect sub-models the build leaves out) and the cfg had no `[lights]`. Now
  `[lights] landing_pos` takes several triples (one fixture each; root units,
  FlightGear's placements with the airframe file's -22.62 m offset: wing
  lights at the wing root, the nose light on the nose gear) and
  `landing_props` names the FDM properties that light them
  (`/systems/exterior-lights/left|nose|right-landing-light`: the MD-80's
  systems extend the wing lights over 7 s and dim the nose light at EXT).
  `landing_switch_props` are what the L key sets (RET 0 / ON 1) and what the
  rig watches when the cockpit switches are clicked.
- **Cockpit floods**: `[lights] flood_pos` / `flood_props`: an omni light per
  flood knob (`/controls/lighting/capt-flood`, `fo-flood`, `pedestal-flood`,
  `overhead-flood`; `flood_range_m`, `flood_energy`). FlightGear has no light
  source for these knobs either (its ALS cockpit lighting is renderer side);
  the positions are a first placement above the glareshield, the pedestal
  and under the overhead — tune by eye.
- **Digital brightness**: FlightGear's colour x dimmer factor is a display
  (sRGB) intent; Godot shades in linear light, so a knob at 0.3 showed about
  60 % brightness. The animator now runs the digits' tint and the material
  emission through `srgb_to_linear`, and the floods' knob through a 2.2 curve.
- **Flaps**: `JSBGodot::set_flaps` snapped every lever value to thirds (a
  Cessna habit), so the 11 detent (0.36) reached the MD-80's lever table as
  0.333 = slats only and the next step was 15. The exact cfg value goes
  through now; the detents are UP/RET, UP/MID, DIAL, 11, 15, 28, 40 (lever
  table: 0.14 / 0.25 / 0.36 / 0.58 / 0.8 / 0.91). The DIAL detent gives the
  thumbwheel's 1-10 deg (`systems.FCS.dialAFlap(+-0.25)` shimmed to
  `controls/flight/dial-a-flap`, -1 .. 24 as FlightGear; the wheel starts at
  -1 = not set, so DIAL is slats only until dialled).
- Tests: `smoke_md80_fdm` sweeps the lever (11 at 0.36, 15 at 0.58, dial 5
  at 0.25), counts 3 landing fixtures and 4 floods, lights the nose light
  from its switch and the captain's flood from its knob.

### Step 12 — flood knobs, nose light DIM, sounds (2026-11-01, gery: "no mouse sense area for the flood dials; no difference between BRT and DIM on the nose light; engine and wind sounds do not work")
- **Flood knobs**: in the FlightGear model they are plain `rotate`
  animations with no `<action>` (the panel / digital knobs next to them are
  `knob` animations with property-adjust bindings), so they were never
  clickable in FlightGear either. New generic `[model] knob_actions
  "property[:step[:min:max]]"`: `FGControls::setup` turns a rotate / knob
  animation on the property that has no binding into a knob with a
  property-adjust action (the same binding shape as the panel knobs, so the
  mouse rule applies: right / wheel up = brighter). The MD-80 lists the four
  flood knobs at 0.05 per click.
- **Nose light DIM**: the systems give `/systems/exterior-lights/nose-landing-light`
  0.6 at DIM and 1 at BRT, and the fixture level read it, but the spot used
  the level only to switch on (> 0.01): its energy was constant. The spot's
  energy now follows the level through the 2.2 curve (DIM = about a third
  of BRT in linear light).
- **Sounds**: the sound set (77 sounds) loaded and 0 files were missing, but
  every engine, wind, pack and fan sound reads `/sim/sound/*` volumes and
  pitches that the package computes in `Sounds/sound-control.xml`, a
  FlightGear property-rule file (`<autopilot n="6">` in MD-80-main.xml:
  51 components, `<logic>` for the view selection and `<filter>` gain /
  noise-spike with `<table>` / `<product>` / `<min>` expressions from
  `n1-actual`, the fake N2, the reversers, airspeed and the pack flows).
  Nothing ran it, so everything stayed at 0 = silent. New
  `src/fg_property_rules.cpp` (`FGPropertyRules`, D6's first half): parses
  the XML at runtime (Godot `XMLParser`), runs `logic` and the filter types
  gain / exponential / double-exponential / moving-average / noise-spike /
  reciprocal / derivative / integrator with FlightGear's conditional input
  lists, `enable`, `u_min` / `u_max`, SimGear's expression and condition
  grammars; outputs go into the bridge dictionary (and into the FDM when it
  has the node) and are remembered so a rule may read a later rule's last
  value. Loaded from `[jsbsim] property_rules` (the file is copied to
  `sounds/sound-control.xml`); its inputs join the mirror's pull keys, and
  the rig runs it between `collect()` and the animator / sounds. Other
  component kinds (`pid-controller`, `pi-simple-controller`,
  `predict-simple`, `flipflop`) are reported once and skipped — the DFGS
  files need them (D6's second half). The cockpit wind sound reads
  `/sim/sound/wind`, which nothing in the package writes (the rules compute
  `wind-v`): `[sound] aliases "/sim/sound/wind = /sim/sound/wind-v"`
  (generic `FGSounds::set_aliases`). Wind starts above 102 kt, as the
  table says.
- Tests: `smoke_md80_fdm` checks the nose light at DIM (0.60), the four
  flood knobs among the binding properties, 51 rule components with none
  unsupported, `internal-custom` 1 in the cockpit, fake N2 54 at idle, and
  the engine whines of both engines playing (-7 dB at idle).

### Step 13 — flap steps, NAV 2, brightness (2026-11-01, gery: "flaps go UP, 0/slats, then straight to 15; digital and overhead lights still overload; the second HSI is slaved to NAV 1")
- **Flap steps**: the joystick buttons (and the keys through
  `CockpitControls`) still stepped by thirds: 0 -> 0.333 -> 0.667 -> 1,
  which the MD-80's lever table reads as UP/MID (slats, the handle near 11),
  15, 40 — exactly the sequence reported. `JSBGodot::increase_flaps` /
  `decrease_flaps` now step a detent list the rig hands over from `[flaps]
  positions` (`set_flap_detents`), the keyboard / lever-drag notch count and
  the resync from the commanded value use the same list (nearest detent),
  so every path walks UP/RET, UP/MID, DIAL, 11, 15, 28, 40. FlightGear's own
  stepping moves the DIAL detent to the slot matching the dialled range
  (0-10 between UP/MID and 11, 12-14 between 11 and 15, 16-24 between 15
  and 28); PVP keeps a single DIAL detent at 0.25 (1-10 deg), the other two
  ranges are not reachable yet.
- **NAV 2**: the bridge had one receiver, and the FO's HSI (`hsi2.xml`,
  `instrumentation/nav[1]/...`) read zeros. `RadioNav` got a
  `receiver_index` (n > 0 writes `instrumentation/nav[n]/...`, no DME / ADF)
  and the bridge runs a second one from the FGCP NAV 2 / CRS 2 knobs
  (`instrumentation/nav[1]/frequencies/selected-mhz`, seeded 109.55 by the
  init list, and `.../radials/selected-deg`). The MD-80's own system gates
  the HSI 2 deviation on `nav[1]/in-range` and `signal-quality-norm`, which
  the mirror now pushes. docs/RADIO_NAVIGATION.md §3.
- **Brightness ("overloaded" panels)**: measured with the new
  `tests/probe_md80_night_shot.tscn` (23:00, knobs as arguments, saves the
  main panel / FGCP / overhead and prints the blown-out fraction). The dials
  were solid orange with white markings: `BaseMaterial3D`'s default
  emission operator is ADD, `EMISSION = colour + texture`, so pairing the
  dial texture as emission texture (step 11's "modulate by the albedo") made
  the black dial `orange + black` and the markings `orange + white`.
  FlightGear's fixed function multiplies. The animator now sets
  `EMISSION_OP_MULTIPLY` whenever an emission texture is in play, also for
  the imported self-lit lamps. Step 11's `srgb_to_linear` on the digits'
  modulate and on the emission colour was a double conversion (Label3D's
  modulate is an sRGB vertex colour, a material colour is a `source_color`
  the renderer linearises) and is gone again: FlightGear's colour x factor
  goes through as it is. The floods were far too strong (energy 1.2 at 2 m
  blew out 27-43 % of the frame at full; 0.3 still washed the walls white): `flood_energy 0.1`, full knob = a dim, even cabin light.

### Step 14 — exterior shading (2026-11-01, gery: "segments of the wings throw some kind of shadow, dark on one side and bright on the other; on the MD-80 mostly the ailerons and elevator")

Vertex normals, not shadows: the AC3D add-on never marked sharp edges (it
applied the crease angle to an empty mesh), so every smooth-shaded face
blended across its neighbours' caps and edges. `fg_a10_build.py` now
re-applies each .ac object's crease once the geometry exists ("Crease
angles" in docs/A10_FLIGHTGEAR_PORT.md). `md80_fg.glb` rebuilt (20.8 MB,
AileronL 196 → 245 vertices from the splits) and copied to the three
folders; `animations.json` and `fonts/` unchanged. Rebuild command as in
D3 plus `--source-note "FlightGear MD-80 by Josh Davidson (Octal450),
GPL-2.0"` to keep the manifest's source note.

### Step 15 — the MD-80 family and liveries (2026-11-01, gery: "get all MD80 models into the sim: MD-81, 82, 83 and 87 with different liveries; the aircraft selectable on the welcome screen, the liveries in the Aircraft settings tab")

**Variants as thin aircraft.** `aircraft/md81`, `md83` and `md87` are
aircraft of their own on the welcome screen and in the settings keys, but
their `aircraft.cfg` names `[aircraft] base "md80"` (new, generic,
docs/AIRCRAFT_CONFIG.md): the MD-82's config is loaded and the variant's
sections overlaid on it, and every file the variant lacks is read from
`md80/` (`AircraftConfig::resolve_aircraft_file`, the FDM loader per JSBSim
folder: `JSBGodot::load_aircraft` now hands JSBSim the variant's
`jsbsim/aircraft` and the base's `engine` / `systems`). What each carries:

- md81: `jsbsim/aircraft/md81/md81.xml` (FDE/MD-81.xml, JT8D-217 engines and
  thrust limits) with `FDE/Config`; the MD-81 livery catalog. 80 KB.
- md83: the same with FDE/MD-83.xml (JT8D-219, forward / aft auxiliary tanks
  5 500 / 8 399 lb). 80 KB.
- md87: FDE/MD-87.xml with the md-87 ground-reaction, mass and external
  files and `Systems/md-87-cockpit-firetank.xml`; `jsbsim/init_md87.json`
  (the -set file's `aero/arm-factor` 0.97; `[jsbsim] init_properties` is a
  list now and merges the base's file first); its own model
  `model/md87_fg.glb` (21 MB: `--root Models/MD-87.xml`, the MD-82 skips
  plus `Effects/Firetank` and `Models/firetank.xml`, `--set-xml
  MD-87-set.xml`; 1 997 objects, 62 texts, the combined standby instrument)
  with `[camera] cockpit_position` z 166.8 (the cockpit sits 2.9 m further
  aft of the model origin in MD-87-main.xml) and the lights moved with it
  (nose light at -17.26 m). Gear height: same `[model] offset` as the MD-82
  (`tests/probe_gear_height.tscn -- md87`).

**Liveries.** FlightGear liveries are PropertyLists setting
`sim/model/livery|fairings|rudders/texture` (the MD-87 also `hstab` and
`wing`), read by `material` animations with `<texture-prop>`; the -set files
seed `sim/model/n1-tach|n2-tach/texture` the same way (the -217 / -217A /
-219 gauge faces). `scripts/fg_liveries.py` exports them: the images (156
files, 18 MB, PNG as shipped) go to `md80/model/liveries/` in FlightGear's
own layout relative to `Models/` (`Liveries/MD-82/AA_fuselage.png`,
`Instruments/n1-217.png`; one tree for the family, keep-mode `.import` files
so the export carries the raw PNGs), one
catalog per variant (`MD-81.json`: 11 liveries, MD-82 24, MD-83 23, MD-87 2;
two skipped for missing files: the MD-81 JAL and the MD-87 White). `FGAnimator` learnt `texture-prop`
(generic: the A-10 / glider manifests have none): the record's property is
`property-base/texture-prop`, the file resolves below `[model] textures_dir`
in the directory of the record's own model XML (`submodels[].xml` below
`Models/`), is loaded once (`Image::load_from_file`, mipmaps) and replaces
the albedo (and a paired emission texture) of the record's objects; "" or
the `<texture>` the glb was built with restores the glb's own.
`AircraftRig::setup_liveries` publishes the catalog defaults and the chosen
livery's files as control state (strings, which the FDM push skips), the
Aircraft tab shows a **Livery** option for aircraft with a `[liveries]
catalog` (all four MD-80 configs have one, the MD-82's in `md80/aircraft.cfg`; stored `aircraft_<id>_livery`, applied at once to the flown
aircraft through `livery_changed` → `AircraftRig::set_livery`). The
installer copies PNGs only below a `liveries` folder (bundle 1.0.12) and the
export preset includes `aircraft/*/model/liveries/**`.

Test: `tests/smoke_md80_variants.tscn` (config inheritance and file
fallback without a flight, then a flight per variant: FDM tanks / arm
factor, shared sounds, livery catalogs, the SAS fuselage swap and restore on
the MD-81, the Aircraft tab's option applying and storing a choice, the
MD-87 model and eye point).

### Step 16 — the DIAL detent while the wheel is stowed (2026-11-02, gery: "from all up one click extends the slats, but to get to 11 I have to click twice; the same going up")
- The lever table reads the DIAL detent (0.25) as UP/MID while the
  thumbwheel is stowed (`/controls/flight/dial-a-flap` -1, `dial-a-flap-pos`
  0), so one notch out of the seven looked like nothing happened in both
  directions. New generic `[flaps] gated_detent <index>`, `gate_property`,
  `gate_min`: `JSBGodot::set_flap_detent_gate` marks a detent as available
  only while the FDM property is at or above the threshold, and
  `step_flap_detent(from, dir)` is now the single stepping function behind
  the keys, the joystick buttons (`increase_flaps` / `decrease_flaps`), the
  cockpit lever click (`provpilot/flaps-step` -> `CockpitControls::step_flaps`)
  and the lever drag (a gated notch in the way is skipped in the drag
  direction). The MD-80 gates DIAL on the wheel >= 1: stowed, the lever walks
  UP/RET, UP/MID, 11, 15, 28, 40; once dialled, DIAL is back in the sequence.
- Tests: `smoke_md80_fdm` sets the wheel back to -1 and checks the gate, the
  key sequence 0.14 / 0.36 / 0.58, two notches up from 15 = UP/MID, one more
  = slats in, and the lever click from UP/MID landing on 11.

### Step 17 — NAV 2 window at start (2026-11-02, gery: "after the airplane is loaded the display shows 0.00; only when rotating one of the frequency knobs does it become accurate")
- Cause: the bridge's `radios()` read the NAV 2 frequency with `ctl(key, 0.0)`,
  a hard default, while the init list carries 109.55. The receiver published
  0.0, the animator then had a value for the property, so the cockpit-state
  seeding (which only fills properties nothing else provides) skipped it; the
  first knob click seeded the state from `initial_value()` and the window came
  alive. `FGBridge::ctl` now falls back to the aircraft's init list before its
  own default, so every bridge-owned control starts from
  `jsbsim/init_properties.json` when the list names it (NAV 1, COMM, ADF keep
  the bridge defaults for aircraft without a list).
- `smoke_md80_cockpit`: the NAV 2 window shows 109.55 and the receiver is tuned
  to it before any knob is touched (+2 checks, 72 PASS).

### Step 18 — the DFGS autoflight (2026-11-02, gery: "look at the Autoflight of the MD80, according to the wiki page it should be close to complete; the question is how to convert it to PVP as well")
Three layers, converted in three parts (D6's second half and a C++ port of
the Nasal mode logic instead of a Nasal runtime):
- **Control laws = the package's property rules**, now complete.
  `FGPropertyRules` runs `pid-controller` (FlightGear's velocity-form PID
  with `<config>` Kp / Ti / Td / alpha / beta / gamma / u_min / u_max as
  numbers, properties or expressions, `<reference>`), `pi-simple-controller`
  (Kp / Ki with anti-windup) and `predict-simple` (`<seconds>`,
  `<filter-gain>`), and follows AnalogComponent's semantics that the DFGS
  files rely on: a component that was disabled runs its next step as a "first
  time" (`<initialize-to>` input (default) / output / none; a PID seeds its
  last output from the output property, bumpless), `<feedback-if-disabled>`
  copies the output into the input property, `<min>`/`<max>` alias
  `u_min`/`u_max`, and **every filter feeds input minus `<reference>`**
  (DigitalFilter::update) — without that the roll-rate / pitch-rate stages
  were zero and the pitch loop commanded a descent. `apply()` remembers which
  outputs an enabled rule wrote this frame (`wrote()`), `set_value()` seeds
  an output. `[jsbsim] property_rules` lists `jsbsim/rules/dfgs-drivers.xml`,
  `dfgs-roll-yaw.xml`, `dfgs-pitch.xml`, `dfgs-thrust.xml` (187 components
  with the sound rules, none skipped). The bridge publishes the inputs the
  rules lacked: `instrumentation/gps/indicated-vertical-speed`,
  `velocities/vertical-speed-fps`, `instrumentation/nav[n]/radials/target-auto-hdg-deg`
  (course + 3 deg per degree of needle, at most 30) and
  `gs-rate-of-climb-fpm` (the descent rate that holds the glideslope at the
  ground speed); `engines/engine[n]/reverser-pos-norm` and the `systems/dfgs/*`
  values come from the FDM through the pull keys. Their outputs reach the FDM
  as before (`/systems/dfgs/afs/{roll,pitch,yaw}-cmd` into the FCS while
  `afs/enabled`, `/systems/engines/ats-cmd-n` into the engines while the A/T
  drives, the FD bars); the rig feeds two of them back into the levers: the
  autotrim's `controls/flight/elevator-trim` into `JSBGodot::set_pitch_trim`
  (new) while an AP is engaged, the backdriven `controls/engines/engine/throttle`
  into the throttle input while the A/T drives (not in CLAMP), so the levers
  follow and nothing jumps at disconnect.
- **Mode logic = `src/it_autoflight.cpp` (`ItAutoflight`)**, a line-by-line
  port of `Nasal/DFGS/DFGS.nas` (ITAF loop at 10 Hz, slow loop, the masters,
  lateral / vertical mode setters, LOC / G/S capture, autoland and flare /
  rollout texts, go-around arming, altitude arm / capture / hold with the
  capture limits, bank limits, FGCP temporary IAS/MACH swap, auto altitude
  re-arm), `DFGS-b.nas` (FMA strings `instrumentation/fma/*-mode-a/-b`, blink
  windows — the texts flash at 2 Hz while a window blinks — the autothrottle
  modes Thrust / Retard / EPR Limit / Clamp with the T/O check) and
  `thrlim.nas` (`systems/engines/limit/active-mode` text from the TRI knob's
  `active-mode-int`, `limitChanged`). The Nasal listeners became direct calls
  (`input_lat` / `input_vert` / ... run the handler like a write-listener) or
  change detection on the properties others write. It is loaded from
  `[autoflight] it_autoflight true` (`auto_arm_alt`, `nav` = the acconfig
  options), owns the `it-autoflight/*` inputs, outputs and texts and publishes
  them through `FGBridge::set_extra_properties` (merged like the cockpit state
  before the mirror push, so the FDM's `afs/enabled` follows `output/ap1|ap2`),
  then `update()` runs between `collect()` and the rules; `absorb()` takes
  back the inputs the driver rules sync while a mode is inactive (vs,
  kts-flch, mach-flch; `wrote()` tells). Cockpit-owned inputs (the FGCP's
  direct bindings: fd1, fd2, athr, bank-limit-sw) are read from the cockpit
  state and written back through `take_control_writes()` (also the TRI
  mode the module sets on TOGA), `take_fdm_writes()` resets the CAWS altitude
  alert, `take_rule_writes()` seeds the rules' V/S altitude-hold reference
  (synced only while inactive — with the wheel at 0 from the first frame it
  would have been 0 ft). LNAV / NAV needs a flight plan: it reverts to HDG
  HLD. The init list starts both flight directors on and `active-ap` 1 like
  ITAF.init.
- **Panel and FMA.** The 34 `cockpit.ApPanel.*` calls go through a new shim
  action `autoflight` (`"kind"`: the handler, `"arg_index"`: the call's
  argument) to `ItAutoflight::panel()`; the yoke / throttle disconnect
  scripts match on their first statement. `[model] screen_texts`
  (`model/screen_texts.json`, `FGAnimator::load_screen_texts`) lays Label3Ds
  in a columns x rows grid over a screen mesh (the thin AABB axis is the
  normal, the first face gives the side, the most upward axis the rows),
  shown while the screen's `power` property reads >= 24 V: the two FMAs'
  THR / ARM / ROLL / PITCH windows, 16 labels. The FGCP's HDG / SPD / ALT /
  V/S windows and the `alt-thousands` / `alt-hundreds` strings now have
  values.
- Flying it: FD and A/T switches, AP ON / OFF switch, HDG knob (push = HOLD,
  pull = SEL), ALT knob (push = cancel arm, pull = arm; auto-arm 1 s after
  setting), pitch wheel = V/S (in IAS: the speed, in TURB: the pitch), SPD
  knob (push = temporary IAS/MACH swap), SPD SEL / MACH SEL / EPR LIM, VOR
  LOC / ILS / AUTO LAND, VERT SPD / IAS MACH / ALT HOLD / TURB, TOGA on the
  throttles. The TRI knob matters: with T/O (or FLX) selected the
  autothrottle stays in CLMP (the T/O check) — select CL / CR after the
  take-off before expecting SPD. An airborne start begins at T/O.
- Tests: `smoke_md80_autoflight` (airborne start: 187 components, none
  skipped, 16 FMA labels, basic modes, AP engage and `afs/enabled`, HDG SEL
  40 deg, altitude auto-arm, V/S 1 200 fpm, A/T SPD 250 kt, ALT CAP after
  92 s and ALT HLD within 3 ft, magnetic heading on the bug, yoke disconnect
  and the AP OFF warning; 23 PASS); `probe_md80_autoflight` prints the roll
  and pitch rule chains once a second after engaging. `smoke_md80_cockpit`
  72, `smoke_md80_fdm` 37, `smoke_md80_airborne` 11 PASS.
- Not done: LNAV (no route manager), the NAV display's heading bug / target
  altitude (canvas), `instrumentation.xml` / `core-rules.xml` rules (the
  starter / cutoff hand-off stays in `_links`), the FMA's canvas-only
  annunciators (AP / FD lights are select animations and work), the EPR
  limit "FLX" branch, the sound set's AP OFF horn (reads
  `it-autoflight/sound/apoff`, which the module writes — to be checked by ear).

### Step 19 — thrust levers, TOGA, ALT knob (2026-11-03, gery: "the right engine keeps going into idle in autothrottle mode; once clicked on the left throttle handle the right one does not move any more; the take-off mode button on the throttle levers do not work; I cannot set the altitude on the fgcp alt")

- Thrust levers. The model's two levers are two knobs on
  `controls/engines/engine[0]/throttle` and `engine[1]/throttle`; PVP has one
  throttle input (keys, joystick, levers) that commands every engine. The left
  lever (normalised `engine/throttle`) was FDM-owned in the bridge and reached
  the FDM through the rig; the right lever's value stayed in the control state
  and overrode the published value of `engine[1]/throttle` every frame, so
  after one drag engine 2 was pinned to that value: it no longer followed the
  left lever or the keys, and whenever the autothrottle was not driving (A/T
  off, CLMP with the TRI at T/O) it fell back to the stale lever — idle if
  the lever had last been dragged there. The A/T itself was symmetric
  (`probe_md80_athr`: both chains identical for 30 s in SPD). Now every
  `controls/engines/engine[n]/throttle` is FDM-owned in the bridge, the rig's
  `_on_fg_control` moves the throttle input for any of them and erases the
  lever's state entry, so the next drag starts from the FDM's live value.
  Both levers move together, as the one axis does.
- TOGA. `cockpit.ApPanel.toga()` reached the module, but `takeoff_go_around`
  reads the flap angle as `fdm/jsbsim/fcs/flap-pos-deg`, FlightGear's name for
  a JSBSim property; the mirror pull looked for `/fdm/jsbsim/...` in the
  standalone FDM, which has no such node, so the flap read 0 and neither the
  T/O (flaps ≥ 5 on the ground) nor the G/A branch (flaps ≥ 26) could fire.
  `fg_path` now maps `fdm/jsbsim/<p>` to the bare JSBSim path `<p>`. The same
  fix feeds the yaw damper's `fdm/jsbsim/aero/beta-deg` / `beta-rad` inputs in
  `dfgs-roll-yaw.xml` (previously 0) and the module's `aero/alpha-deg-damped`.
- ALT knob. The wheel bindings are a Nasal `if (getprop(".../alt-translate")
  == -1) { altAdjust(1); } else { altAdjust(10); }` plus a Shift variant
  conditioned on `devices/status/keyboard/shift`. The shim matcher split the
  script at `;` and prefix-matched each piece, so the call inside the `if`
  block never matched; the Shift property was unknown (0), so the Shift
  binding never ran either. FGControls now evaluates `if (getprop("P") <op>
  V) { A } else { B }` against the current value (`nasal_if`) and runs the
  chosen block, `nasal_actionable` sees calls inside blocks, and
  `devices/status/keyboard/shift` reads the Shift of the click being handled
  (`click_shift`). Wheel = ±1 000 ft (±100 below 1 000), Shift + wheel or the
  wheel while the knob is held pushed = ±100; left click pushes (disarm),
  right click pulls (arm).
- Tests: `smoke_md80_autoflight` +3 (right lever, then left lever: both
  `control-n/throttle-pos` follow; no lever key left in the control state),
  `smoke_md80_cockpit` +5 (ALT knob wheel ±1 000, Shift +100; TOGA click with
  flaps 11 on the ground → lat 5 / vert 7, text T/O, FMA TAK OFF). Probes
  `probe_md80_athr` (A/T chain of both engines, lever assigns) and
  `probe_md80_fgcp` (ground clicks on TogaL and the ALT knob through the
  cockpit camera — `root.camera1`, the exterior camera is beyond the 4 m hit
  range).

### Step 20 — the FMA shows nothing (2026-11-07, gery: "look at the FMA currently it's not showing anything")

- The 16 screen-text labels were there, lit (the DC radio buses read 28 V in
  the air and on the ground) and had their mode strings — and rendered
  nothing: `load_screen_texts` took the label normal from the first face's
  winding, and the FMA screen quads face into the glareshield. A Label3D
  draws its +z side only, so every window faced away from the crew and was
  back-face culled. The normal is now flipped to the model's +x (aft)
  half-space when the face points forward; a screen that faces aft keeps its
  own direction.
- With the labels visible the text ran vertically at four times its size:
  the rows' axis was "the mesh axis that points most upward in the world",
  and the world is ECEF — over Zurich the world's +y is nowhere near up. The
  rows now follow the model root's +y projected into the screen plane, the
  baseline is across, and the screen's width / height are the vertex extents
  along those two directions (the mesh's own axes need not follow the
  screen's edges).
- An empty mode string (`thr-mode-b` with nothing armed) was rendered as
  "0": `update_texts` formatted the numeric value whenever the string was
  empty. The animator now remembers that a property was last set from a
  String (`prop_is_string`), and a screen window shows such a property — or
  one never written — verbatim.
- Checks in `smoke_md80_autoflight` (27 PASS): every label visible in the
  tree, every label's +z within 72° of the cockpit camera, no window shows
  "0". Probes `probe_md80_fma` (headless: power, animator and label state)
  and `probe_md80_fma_shot` (a cockpit screenshot after the airborne start)
  were the diagnosis.

### Step 21 — HDG SEL turn direction, FMA face (2026-11-07, gery: "hdg hld at 360, heading 360, knob to 90: it turns left; amber = loaded, green = active; the FMA should have the segmented LED font; the ALT loading mode only shows ALT, it should show 060 for 6000 ft")

- Heading error. `dfgs-drivers.xml`'s "Heading Error Deg" filter wraps its
  output with `<period><min>-180</min><max>180</max></period>`; the rules
  engine ignored the element, so a bug 90 deg clockwise of a 360 heading was
  an error of -270 — a left turn. `FGPropertyRules::run` now wraps every
  component's output into its `<period>` (eight filters in the drivers file
  use it). Check: bug 200 deg clockwise → error -160 (left).
- FMA face. `fma.svg` draws the four windows in the 16-segment
  `Std16Seg.ttf` at 93.5 px on 90 px rows, THR / ROLL / PITCH in green
  `#2dff2d`, ARM in amber `#ff880a`; `fma.nas` shows the THR window only with
  the autothrottle on, the other three with an autopilot or that side's
  flight director on, blanks a window at 1 Hz while its blink flag is set,
  and (acconfig `armed-alt-as-fl`, the default) writes the armed altitude as
  a flight level into ARM line 2. `screen_texts.json` gained `font` (a TTF
  next to it), `font_scale` (em height in rows, 1.0 here), `colors`,
  `show` and `blink` per column; `ItAutoflight::fma_alt_arm` writes "060"
  for 6 000 ft and `fma_loop` follows the ALT knob while armed.
- Checks in `smoke_md80_autoflight` (32 PASS): heading error +40 / -160,
  the ARM window's flight level, the 16-segment font on all 16 labels, 4
  amber + 12 green, THR blank with the autothrottle off. Screenshot via
  `probe_md80_fma_shot` (AP engaged, 6 000 ft armed): blank THR, amber 060,
  green HDG HLD / ALT HLD.

### Step 22 — FGCP window colour and dimmers, glideslope tracking (2026-11-07, gery: "the Speed, HDG, VS and ALT LEDs should have the same amber as the VHF NAV and CRS LEDs; the dimmers for the fgcp LED and backplates do not work; on my ILS approach to LSZH 28 the autopilot started to strongly deviate from the glide slope")

- Named groups. `fgcp.xml` declares `<animation><name>Digits.Fgcp</name>`
  (the 17 SPD / HDG / V/S / ALT / pitch-mode windows) and nests it in the
  `Digits` group with the NAV / CRS windows; the material animation (amber
  1 / 0.6 / 0 times `systems/lighting/fgcp-digital`) and the power select
  target `Digits`. The builder resolves one level and leaves
  `fgcp__Digits_Fgcp` unresolved, so those windows kept Label3D's white and
  ignored the dimmer. `FGAnimator::load` now expands a group's node name to
  its members recursively (records of type `none` with a `name`).
- Dimmers. The chain itself worked (`probe_md80_dimmer`,
  `probe_md80_knobs`): the knobs are clickable, right click / wheel up adds
  0.05 per step (FlightGear's factor), the controls reach the FDM's
  `systems/lighting/*` switches and the emission factors. The digital knob
  starts at 1.0 (turn it down to see it), the panel knob at 0: the backplate
  labels glow at night (`probe_md80_fgcp_shot ... night`).
- Glideslope. The bridge fed the DFGS a `gs-rate-of-climb-fpm` of a fixed
  3 deg descent at the ground speed; LSZH 28 (IZW) is a 3.3 deg path and
  nothing pulled the aircraft back to the beam. `navradio.cxx`'s rate is
  ported: the receiver publishes `gs-direct-deg` (the aircraft's angle from
  the GS site) and `gs-target-deg` (the site's path), the bridge flies
  `-sin(direct - 10 x (target - direct)) x ground speed`. The DFGS's own
  clamp of -1500 fpm stays (FlightGear's): at 250 kt a 3.3 deg path needs
  more and the needle walks away, so slow down before capture.
- Tests: `smoke_md80_ils` (new: 7 nm out under the beam at 160 kt, gear and
  flaps, AP + A/T + ILS; capture within 30 s, needle within 0.15 norm from
  capture + 40 s to 150 ft, localizer within 0.4, path 3.3 published),
  `smoke_md80_cockpit` + 4 (window colour, digital and panel dimmers).
  Probes `probe_md80_ils`, `probe_md80_dimmer`, `probe_md80_knobs`,
  `probe_md80_fgcp_shot`. `AircraftRig.get_fg_property(key)` exposes the
  frame's FlightGear-side dictionary to tests.

### Step 23 — the ATS after touchdown (2026-11-07, gery: "when the autothrottle has moved the throttles to idle it should turn itself off, the solenoid switch goes down; unsure whether the AP main mode should switch off after the rollout stop")

- FlightGear's DFGS keeps the ATS in RETD after touchdown and drops it
  (silently) when a thrust reverser deploys — `athr-avail` goes false in
  `dfgs-drivers.xml` — or when the pilot presses the throttle disconnect.
  ProvPilot has no reverser control, so the switch stayed up for good. The
  port now lets the ATS go once it is in RETD with both main gears down and
  both levers at the idle limit: `kill_athr_silent()`, no THROTTLE warning,
  the AUTO THROT solenoid drops (`it-autoflight/input/athr` 0).
- The autopilots stay engaged through ROLLOUT until the pilot disconnects,
  as in FlightGear and the manual (ROLLOUT is a mode of the engaged AP);
  nothing switches them off at a standstill.
- `smoke_md80_ils` now continues through AUTO LAND: FLARE, touchdown,
  ROLLOUT, APs still engaged, ATS off within 20 s of touchdown.- Found on the way: the localizer needle sat a steady 0.11 norm off centre on
  every approach, outside the flare's `canAutoland` gate of 0.1 — the bridge
  built `radials/target-auto-hdg-deg` from the magnetic course while the
  DFGS subtracts the true heading (a third of the variation as a needle
  offset), and `/orientation/track-magnetic-deg` did not exist, so the
  drift-angle driver read 87 deg. `navradio.cxx`'s formula is ported: true
  target radial + 3 deg per unit of needle - (track - heading); the bridge
  publishes `orientation/track-deg` / `track-magnetic-deg` from JSBSim's
  `flight-path/psi-gt-rad`. The smoke now sees ALIGN, FLARE, touchdown and
  ROLLOUT with the needle at 0.02 (8 PASS).


### Step 24 — the gear handle springs back (2026-11-08, gery: "when airborne I lower the gear by clicking the handle; the gear comes down only while I hold the button, on release the handle goes up again; the G key works")

FlightGear's pick on `LGLever` binds the press to `controls.gearDownSmart(1)` and the release (`mod-up`) to `controls.gearDownSmart(0)`. The MD-80's `aircraft.nas` treats the argument as a direction: non-zero moves the lever to the other position, zero (the release) does nothing (`controls.gearDown(0)` is a no-op). The shim table mapped both calls onto `gear_cmd` with the argument as the position, so the release raised the gear again.

- `nasal_shims.json`: `controls.gearDownSmart(` → action kind `gear_smart`; `controls.gearDown(` stays `gear_cmd`.
- `FGControls::nasal_action`: `gear_cmd` follows the sign of the argument (positive lowers, negative raises, zero changes nothing); `gear_smart` toggles through `provpilot/gear-toggle` on a non-zero argument and ignores the release.
- `smoke_md80_cockpit`: pressing the handle moves the gear command, releasing the button leaves it, a second click moves it back (86 PASS). `smoke_a10_controls` (its own gear-toggle pick) 26 PASS.

### Step 25 — thrust reversers (2026-11-08, gery: "a button to toggle between normal and reverse thrust; the throttle handles on the gamepad then increase and decrease reverse thrust; ideally t or r")

FlightGear's MD-80 has no reverser lever of its own either: the DEL key calls `systems.toggleRevThrust()`, which flips `/systems/engines/reverse-N/engage` for both engines when a main gear is on the ground and `/systems/engines/throttle-compare-max` (the most forward lever) is at idle, and otherwise stows them and idles the levers. The JSBSim `Reversers` channel (Systems/md-80-*-engine-thrlim.xml) does the rest: the flag swings the buckets (`reverser-pos-norm`, 0.5/s with hydraulic pressure, π rad into `propulsion/engine[n]/reverser-angle-rad` so the direct thruster's thrust goes negative), the throttle input is read through a 0..0.72 table as reverse power, `control-N/throttle-pos` reads 0 (no forward thrust) and the DFGS's `athr-avail` drops at the first centimetre of bucket travel, which the ported ATS already honoured. The port needed only the toggle:

- `AircraftRig::toggle_reverse_thrust(jsb)` (bound; `is_reverse_engaged`) ports `toggleRevThrust`: the flags come from the cfg `[engines] reverse_engage_props` (one FDM property per engine), the idle test from `reverse_lever_max_prop` (the throttle input when absent), the ground test from the FDM's `gear/wow`. Aircraft without the cfg entries return false.
- Keyboard: `reverse_toggle` on **R** ("Reverse thrust (on the ground, levers at idle)"); the flight-model start moved to **Shift+R**. `KeyBindings::load` migrates: an action absent from `keyboard.cfg` keeps its default key, and a stored action of the same section that still sits on that key only because it used to be its own default moves on to its current default (a deliberate rebinding stays). Gamepad: button action `reverse_toggle` ("Reverse thrust on / off"), unbound by default, dispatched in `node_3d.gd` next to the flap buttons.
- `smoke_md80_ils`: the toggle refuses in the air; after the ATS lets go on the rollout the toggle deploys the buckets (1.00 within 8 s), the throttle input at 1 pulls about 7 400 lb per engine backwards at N1 80, the aircraft slows, and the toggle again stows them with the levers idled. `smoke_keyboard`: the migration (fdm_start R → Shift+R when reverse_toggle is new; J stays J).

## HT9100 FMS / MCDU (2026-11-14)

The MCDU works to the extent the FlightGear package implements it. `src/ht9100_mcdu.cpp`
(`Ht9100Mcdu`, cfg `[mcdu] ht9100 true`, `type` / `engines` per variant) is a port of
`Nasal/HT9100/*.nas`: pages MENU, IDENT, POS REF, DATA INDEX, NEAREST INDEX, NEAREST
AIRPORTS, RTE 1 (origin, destination, flight number) and PERF INIT (transition altitude,
cruise level), the scratchpad with its messages (INVALID ENTRY, NOT IN DATA BASE, INVALID
CLEAR / DELETE), CLR, long CLR, DELETE and +/-. Every other page key shows PAGE NOT AVAIL,
as upstream.

- Keys: the shim action `mcdu` (`model/nasal_shims.json`) forwards
  `mcdu_ht9100.unit[0].softKey("l1")` etc. with the text argument to `Ht9100Mcdu::key`.
- Screen: `screen_texts.json` got a second form, `texts` (free labels on a character grid:
  x / y anchor, align, `color_prop`, `small_prop`, `char_w`, `font_small`); the 51 fields of
  `Displays/res/HT9100.svg` are laid out from it with the package's HT9100 fonts. The module
  publishes `instrumentation/ht9100/d|c|s/<field>` and the EXEC / MSG lights.
- Data base: the bundled `airports.json` (origin / destination lookup, nearest airports with
  magnetic course and distance).
- Not there, as upstream has no logic for it either: LEGS, DEP ARR, HOLD, PROG, RTE 2, the
  other NEAREST lists. Company routes answer NOT IN DATA BASE (they are FlightGear flight
  plan files). Origin, destination and cruise altitude are published under
  `autopilot/route-manager/*` for a later route manager / LNAV.
- Test: `tests/probe_md80_mcdu.tscn -- <out_dir>` (24 checks, a real click on R6, close-ups).

## Spoiler lever, flaps on the airframe (2026-11-14)

- The spoilers are the FDM's own system (`md-80-spoilers.xml` reads
  `/controls/flight/speedbrake` in notches of 0.2), so the generic
  `fcs/speedbrake-cmd-norm` did nothing. `[controls] speedbrake_prop /
  speedbrake_key_value / speedbrake_step` in aircraft.cfg: B moves the lever
  to 0.8 and back, a click on the cockpit lever moves one notch. The value
  goes through the cockpit state as well, because the property mirror pushes
  `controls/*` into the FDM every frame.
- The airframe animates `surface-positions/flap-pos-norm` (unsided); the
  bridge only wrote the left / right names.
- `tests/probe_md80_surfaces` checks both and the NAV 2 DME.

## VOR LOC steering and exterior lights (2026-11-15)

- `radials/target-auto-hdg-deg` (the heading the DFGS flies in VOR LOC) is built
  from the receiver's own true target radial: the selected course by the
  *station's* declination for a VOR, the localizer's course for an ILS. It used
  the variation under the aircraft (needle a third of the difference off centre,
  1.4 deg at KPT) and the reciprocal course with the FROM flag (the aircraft
  turned back to the station). Test: `probe_md80_vor -- <to|from> [course]`.
- Position lights, strobes and beacons: `[lights] nav_left_pos / nav_right_pos /
  nav_tail_pos / strobe_pos / beacon_pos` at FlightGear's placements, switched
  by `nav_prop / strobe_prop / beacon_prop` (`/systems/exterior-lights/*`: the
  POS/STROBE and ANTI-COLLISION switches, bus voltage, strobes off with the nose
  wheel on the ground). The flight starts with POS/STROBE at BOTH.

## Landing lamps and nose torque links (2026-11-15)

- `[lights] landing "WingLightL, WingLightR"` puts the wing landing lights on the
  retractable lamp meshes; `landing_ext_props` (one entry per landing fixture,
  `-` = fixed) names the extension property: the lens faces down when stowed,
  the beam swings from straight down to forward with the lamp, and the lamp
  burns while it swings out with its switch at on. Two nose gear lamps
  (`landing_pos`), as in MD-80-main.xml. Test: `probe_md80_lamps -- <out_dir>`.
- FlightGear's MD-80-main.xml has the nose torque link pivots 0.8 m ahead of
  the links (x 1.74994 / 1.68094; the MD-87 file has them on the links): the
  links swung far out of the gear, the lower one 14 cm under the ground with
  the nose strut compressed by braking. Corrected to 2.55 / 2.481 in
  `animations.json` and in the source XML — re-apply after a rebuild from a
  fresh FlightGear package. Test: `probe_brake_dip -- md80 "^LGN"`.
