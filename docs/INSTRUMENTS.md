# Cockpit instruments

How the working A-10 instruments are built, what the altimeter (the first
generic instrument added from outside the package) taught us, and what it
takes to give the Cessna 172 and the PC-7 working instruments the same way.

## 1. How an A-10 instrument works

Every A-10 gauge is data, not code. Four layers, none of them A-10 specific:

| Layer | What it holds | Where |
|---|---|---|
| Mesh | one object per moving part (`needle`, `drum-1`, `face`, `knob`...) | AC3D file → `a10_fg.glb` |
| Animation | FlightGear `<animation>` blocks: rotate / textranslate / select / material / pick, keyed on a *property path* | `Models/Cockpit/*.xml` → `animations.json` |
| Runtime | `FGAnimator` (C++) evaluates the animations every frame from a property dictionary; `FGControls` (GDScript) does the clickable parts | `src/fg_animator.cpp`, `provpilot/src/fg_controls.cpp` |
| Bridge | `FGBridge` fills the property dictionary from JSBSim, the host and the switch state each frame | `provpilot/src/fg_bridge.cpp` |

Example, the vertical speed indicator (`A-10-vsi-6kfpm.xml`): a `rotate` of
object `needle` by `instrumentation/vertical-speed-indicator/indicated-speed-fpm`
through an interpolation table. The bridge writes that property from
JSBSim's `velocities/h-dot-fps` × 60. Nothing in the animator knows what a VSI
is.

The instrument is a *sub-model*: the A-10's `A-10-model.xml` lists
`<model><path>Models/Cockpit/A-10-vsi-6kfpm.xml</path><offsets>x y z
pitch</offsets></model>`, and the build script imports the sub-model's mesh
under an empty `Sub_<tag>` at those offsets and prefixes its objects
(`A-10-vsi-6kfpm__needle`). Animations are conjugated by that placement,
so the instrument's own XML never needs to know where it sits.

Property paths follow FlightGear's names (`instrumentation/altimeter/...`).
This is the contract between an instrument and the simulator, and it is
what makes instruments portable between aircraft.

## 2. The altimeter: an instrument from outside the package

The A-10 package does not ship an altimeter mesh; its `altimeter.xml`
points at FlightGear's generic `/Aircraft/Instruments-3d/altimeter/altimeter.ac`
from the base package (fgdata). What was needed:

1. **The generic model**: `altimeter.ac`, its two textures and (for
   reference) its own `altimeter.xml`, vendored under
   `third_party/fgdata/Aircraft/Instruments-3d/altimeter/` (GPL-2, see
   `third_party/fgdata/README.md`). The build script resolves absolute
   `Aircraft/...` paths there first. Rebuild: 0 missing models, 23 new
   objects, no other change.
2. **Nothing in the animator**: rotate, textranslate (the drum counters),
   select (the flags) and pick (the knobs) already existed.
3. **Two bridge lines**: `instrumentation/altimeter/indicated-altitude-ft`
   is the ISA inversion of JSBSim's static pressure (`atmosphere/P-psf`)
   against the Kollsman setting, `145366.45 × (1 − (P / setting)^0.190284)`,
   like FlightGear's altimeter instrument; `instrumentation/altimeter/setting-inhg`
   lives in the control state (the knobs turn it, `property-adjust` with
   min / max) and is set to the weather's QNH (`atmosphere/P-sl-psf`) at
   flight start, so the instrument reads the field elevation on the ground.

Smoke `tests/smoke_a10_cockpit.tscn`: needle at 0.36°/ft, Kollsman window
at the QNH, +1 inHg reads ≈ +900 ft.

Two runtime details the altimeter exposed (they affect every drum counter):
the glTF export flips the texture V axis, so a FlightGear v-offset is negated
in Godot; and FlightGear's `textranslate` computes `(value + offset) × factor`
(offset first), unlike `rotate` / `translate`. Drums are authored with the
rest digit "1" under the window and an offset of one digit back.

Lesson: **an instrument costs one mesh with named parts, one animation
file, and the bridge lines that compute its properties.** The mesh and
animation file can come from fgdata's `Instruments-3d` library (altimeter,
airspeed, attitude, heading, VSI, turn coordinator, tachometer, clock, fuel,
oil, EGT, magnetic compass, ...), which is what most FlightGear aircraft use.

## 3. What the Cessna 172 and the PC-7 have today

- `C172P_1.glb` (Sketchfab): the panel is one static mesh with a painted
  instrument texture (`Meter_01` material). No needle objects, so nothing
  can move in place.
- `pc7.glb` (Sketchfab): the cockpit is unnamed geometry (`Object_10`...),
  no panel detail.

Neither model can be animated in place; instruments have to be *added* as
sub-models placed over (or replacing) the painted panel — exactly what the
A-10 does with its own gauges.

## 4. The Cessna 172 panel (done 2026-09-22)

gery removed the painted instrument placeholders from the Sketchfab model in
Blender (`provpilot/aircraft/c172p/model/C172P_1.blend`, the source of
truth). The rest is a build step:

1. **Panel manifest** `aircraft/c172p/model/panel.xml`: a FlightGear
   PropertyList of `<model>` entries (path into `third_party/fgdata`,
   `<offsets>`, optional ProvPilot `<scale>`). Offsets are FlightGear
   model-frame metres (x aft, y right, z up) relative to the aircraft
   origin; the builder puts a `Panel_Root` empty rotated +90° about Z
   between the FlightGear frame and the Sketchfab model's Blender frame
   (+X left, +Y aft, +Z up). The hole centres came from the panel mesh's
   boundary loops (four 8.6 cm holes per row on the pilot side, face at
   Blender y = −0.785, leaning 3.5°). Layout: clock (scaled 0.78), ASI, AI,
   altimeter on top; turn coordinator, heading indicator, VSI in the middle;
   tachometer below.
2. **Build** `scripts/fg_panel_build.py` (headless Blender): opens the
   blend, imports each instrument as a `Sub_<tag>` sub-model under
   `Panel_Root` with the A-10 builder's `import_model` (objects named
   `<tag>__<object>`), adds the nose-gear steering pivot and the tail light
   lens, exports the whole scene as `C172P_1.glb` and writes
   `animations.json` next to it (42 animations, 8 knob picks); it also adds
   `Spinner_Pivot` on the propeller axis carrying the spinner cone, so the
   cone spins and stays while the blades give way to the blur disc. Both copies
   (`provpilot/aircraft/c172p/model`, `aircraft/c172p/model`) must be kept.
   Two more manifest features: `<blank>` entries (ProvPilot extension) put
   black discs behind every hole so the cowling never shows through gaps
   or empty holes, and textured materials whose image has transparent
   pixels get their alpha linked (the heading indicator's front glass and
   the turn coordinator's tube were exported opaque otherwise).
   The builder learned one FlightGear feature on the way: an
   `<object-name>` may name another animation (`<name>`) to nest its
   transform group — the attitude indicator rolls its `PitchTranslation`.
   The record is expanded to that animation's objects and kept outermost.
3. **Bridge** (`src/fg_bridge.cpp`, generic for every aircraft): heading
   indicator (`indicated-heading-deg` = heading + the knob's `offset-deg`),
   attitude indicator pitch / roll, turn coordinator (`indicated-turn-rate`
   = yaw rate / 3°/s, 1.0 = standard rate), gyro `spin` flags, clock
   `day-seconds`, piston `engines/engine[0]/rpm` from JSBSim `engine-rpm`,
   `sim/model/material/instruments/factor` for the instrument lighting
   (also with fgdata's double-slash spelling), and the electrical model
   treats a piston alternator as online above 400 rpm.
4. **aircraft.cfg**: `[model] fg_animations "model/animations.json"` (the
   normal flip stays on the `Cessna_Exterior_` prefix, so the panel is not
   flipped); `[animation] front_wheel` points at the `NoseGear_Steer` empty
   (origin on the strut axis, carries strut + wheel); `[lights]` uses the
   Sketchfab wingtip lenses (`_025` left, `_026` right), the fin-top lens
   (`_009`) as the beacon and the built `TailLight_lens`.
5. **Smoke** `tests/smoke_c172_panel.tscn`: animations and knob picks
   loaded, every gauge part present, heading card at the heading, altimeter
   at the altitude, tachometer on its table, airspeed at rest, clock hand
   moving, pitch card animated, nose wheel turning right with right rudder,
   six light fixtures.

The panel manifest may also carry its own `<animation>` records for parts
of the model that are not FlightGear sub-models, named by their Godot
names; the runtime conjugates them by the part's own parent chain, so their
axes and centres are the model's Blender coordinates (a rotate without
`<center>` gets the part's own origin, i.e. the pivot set in Blender; a
rotate's offset is FlightGear's `<offset-deg>`). Shift+Ctrl+M draws every
clickable part's sense area. The Cessna uses this
for the yokes (split from their shaft cones by the builder; they turn with
the aileron, slide with the elevator, and a click on a cone hides both) and
for the switches: the rocker grid under the glareshield (TAXI, LDG, PULSE,
NAV, BCN, STROBE, PITOT HT — gery split them by hand in the blend with the
pivot at each switch centre; the builder only renames them for the
manifest, and falls back to cutting them out of the panel base by face
boxes when the blend has none; the round bodies next to them are the
circuit breakers) and the red master rocker split into ALT and BAT. A click toggles `controls/lighting/taxi-light`,
`landing-lights`, `nav-lights`, `beacon`, `strobe`, or
`controls/electric/engine/generator` / `battery-switch`; the host maps the
lights onto the exterior fixtures, the electrical model powers everything
from ALT / BAT, and the rocker tilts about its pivot, top in while on.

Wheels roll too: the Sketchfab wheel objects hold the tyre, its sidewalls
and hub caps as loose parts centred on the axle, next to the fairing halves
and brackets. The builder separates every loose part centred on the axle
(within 3 cm, taller than 10 cm) into `NoseWheel_Tyre` / `MainWheel_L_Tyre`
/ `MainWheel_R_Tyre` with the axle as origin (the nose tyre under the
steering pivot), and `[gear] legs` with `roll` spin them about their local
X with the ground speed — the gear-leg system now also serves fixed gear
(`retractable false`, `retract_deg 0`).

What is not done: the fourth hole of the top and middle rows and the small
holes stay empty (VOR / ADF / fuel and oil gauges from fgdata would fit the
same way), and the PC-7 has no panel yet — its cockpit is unnamed geometry,
so it needs the hole positions measured the same way and its own
`panel.xml`.

## 5. Open points

- Magnetic variation: headings are true today (`attitude/psi-deg`); a
  compass should read magnetic. One bridge line once a WMM table is in.
- Temperature error: the ISA inversion ignores the temperature offset the
  weather model sets, as FlightGear's does.
- The A-10 package's remaining gaps (HUD combiner, MFD pages) are canvas
  displays, not gauges: a different mechanism (SubViewport textures).
