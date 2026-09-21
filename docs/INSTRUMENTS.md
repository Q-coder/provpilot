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
| Runtime | `FGAnimator` (C++) evaluates the animations every frame from a property dictionary; `FGControls` (GDScript) does the clickable parts | `src/fg_animator.cpp`, `provpilot/fg_controls.gd` |
| Bridge | `FGBridge` fills the property dictionary from JSBSim, the host and the switch state each frame | `provpilot/fg_bridge.gd` |

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

## 4. Plan: a generic instrument panel for any aircraft

1. **Panel manifest per aircraft** — a small FlightGear-style XML in the
   aircraft folder (`aircraft/c172p/panel/panel.xml`) that lists sub-models
   and offsets, nothing else:
   ```xml
   <PropertyList>
     <model><path>Aircraft/Instruments-3d/altimeter/altimeter.xml</path>
       <offsets><x-m>..</x-m><y-m>..</y-m><z-m>..</z-m></offsets></model>
     <model><path>Aircraft/Instruments-3d/airspeed/airspeed.xml</path> ... </model>
   </PropertyList>
   ```
   Offsets in the model's frame (FlightGear x aft, y right, z up), measured
   once in Blender against the painted panel.
2. **Build** with the existing script (generalised from `fg_a10_build.py`:
   `--src` any package, the panel XML as the root) → `panel.glb` +
   `animations.json`. The Cessna's own glb stays as it is; the panel is a
   second model loaded under the same root (`[model] fg_animations` already
   loads a manifest for the aircraft's model; a `panel_file` key adds a
   second glb + manifest under `root_node`).
3. **Bridge**: already generic. The airspeed, altimeter, VSI, attitude,
   heading, slip, RPM, fuel and oil properties are filled for every JSBSim
   aircraft today (the A-10 needed no special cases for these). Engine
   properties for a piston aircraft (`engines/engine/rpm`, `oil-pressure-psi`,
   `egt-degf`) need one bridge block reading JSBSim's piston engine, next
   to the turbine block.
4. **Controls**: knobs (Kollsman, heading bug, OBS) come free with the
   instrument's XML through `FGControls` picks.
5. **Lighting**: fgdata instruments carry `material` animations on
   `controls/lighting/instruments-norm`, already bridged (panel lighting ×
   night).
6. **Cover the painted gauges**: either hide the `Meter_01` mesh
   (`[model] hide_nodes`) and add a black panel plate in the manifest, or
   place the 3D instruments 2–3 mm in front of the paint.

Effort: the build-script generalisation and the second-model loader are the
only new code (a day); each instrument afterwards is a manifest line plus
Blender placement. The Cessna's six-pack plus RPM/fuel/oil is ~10 lines of
XML and ~10 vendored fgdata folders.

## 5. Open points

- Magnetic variation: headings are true today (`attitude/psi-deg`); a
  compass should read magnetic. One bridge line once a WMM table is in.
- Temperature error: the ISA inversion ignores the temperature offset the
  weather model sets, as FlightGear's does.
- The A-10 package's remaining gaps (HUD combiner, MFD pages) are canvas
  displays, not gauges: a different mechanism (SubViewport textures).
