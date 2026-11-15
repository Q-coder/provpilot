# Aircraft effects: strut compression, heat shimmer, contrails

## Strut compression (`src/gear_struts.h`)

Every drawn wheel is pinned to the FDM's gear contact point. The tyres stay on
the ground while the airframe sinks on its struts (taxi loads, rotation, a
hard landing) and hang at full extension in the air.

Per FDM gear unit, each frame:

1. where the FDM has the contact: the unit's body location less its
   compression (`JSBGodot::get_gear_contacts()`), as a height along the local
   vertical from the aircraft node;
2. where the drawn tyre's lowest point is: the tyre mesh's centre less its
   radius (measured once from the mesh vertices, gear down);
3. the difference, along the aircraft's up axis, is the wheel's shift.

Nothing is calibrated by hand: an FDM gear change or a `[model] offset` change
moves the wheels along. The shift fades out while the gear retracts (the wheel
has to fit its bay) and leads by one frame (the wheels are placed before the
FDM steps).

Two ways to move the wheel, from `aircraft.cfg [gear]`, one entry per FDM gear
unit in the FDM's order:

| key | meaning |
|---|---|
| `strut_tyres` | the tyre meshes (node names, or paths under the model root) |
| `strut_travel_m` | FlightGear models: metres of wheel travel per unit of `gear/gear[n]/compression-norm`, the factor of the model's oleo translate animation. The shift is published as that property, so torque links and dampers follow. |
| `strut_nodes` | other models: the nodes to move (default: the tyres). The Cessna's nose leg moves with its steering pivot; its main wheels and the PC-7's wheels move on their legs. |
| `strut_range_m` | `"down, up"` limits from the drawn pose |
| `strut_units` | FDM unit indices when not 0, 1, 2 |

Test: `tests/probe_gear_compress.tscn -- <id>` presses the aircraft into its
gear and reports, per wheel, compression against the tyre's height above the
ground (all five aircraft within 1.5 cm). `tests/probe_rotation.tscn` covers
the take-off rotation.

## Exhaust effects (`src/exhaust_effects.h`)

`ExhaustEffects` is a node under the scene root, created by `AircraftRig`,
that processes late in the frame (after the FDM has placed the aircraft).

```
[exhaust]
    nozzles   "right, up, forward; ..."   # metres from the aircraft node
    radius_m  0.40
    engines   "0, 1"                      # FDM engine per nozzle (default: in order)
    heat      true
    contrail  true
    heat_length_m 12.0                    # default 30 x radius
```

Nozzle positions: `tests/probe_model_nodes.tscn -- <id> <name regex>` prints
mesh bounds in that frame. Configured for the A-10, the MD-80 family and the
PC-7 (heat at the two exhaust stacks).

**Heat shimmer.** A cone behind each nozzle (plus cross discs for the view
along the jet) drawn with a shader that samples the screen texture through
scrolling noise. Strength follows the engine (N1 for turbines; off with the
engine). The screen texture holds the opaque scene only: clouds and other
transparent things behind the plume are not refracted.

**Contrails.** In air colder than -36 degC a trail starts, full below -42 degC
(`atmosphere/T-R`; about FL260 and up in standard air), scaled by engine
power. Each nozzle drops a puff per metre of flight path at its ECEF position.
A puff's diameter grows with the square root of its age; as puffs grow, every
second one fades out (by the trailing zero bits of its serial number), so the
overlap stays the same along the trail. Life 55 s (about 12 km at cruise),
about 3 000 camera-facing quads per engine, rebuilt into one mesh per frame.
`force_contrail` on the node turns the trail on at any temperature (tests).

Test: `tests/probe_exhaust.tscn -- <out_dir> [id] [ground|air|both]` (real
renderer) saves screenshots with the effect on and off.

## Sky light through twilight (2026-11-15)

`SkyAtmosphere` computes a twilight factor (the sun from 8 deg under the
horizon to 5 deg over it) and scales the fog's light and the clouds' sky light
with it (`fog_color_now`, `cloud_ambient_now`; a warm tint near the horizon
sun). The fog light used to be constant: ground haze and distant clouds kept
their daytime brightness all night. Test: `probe_dusk -- <out_dir> [hours ...]`.
