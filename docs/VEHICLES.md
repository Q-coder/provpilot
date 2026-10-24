# Ground vehicles (cars, later trains)

Status 2026-10-11: a first cut. The welcome screen lists vehicles next to the
aircraft, Fly! places the selected one on the chosen runway, and a car drives
over the drawn ground with a kinematic model. Trains are placed only.

## What exists

| Piece | Where | Notes |
|---|---|---|
| Vehicle definitions | `aircraft/<id>/aircraft.cfg` with a `[vehicle]` section (`kind "car"` or `"train"`, wheelbase, track, speed and acceleration limits, `clearance_m`), `[model]` as for aircraft plus `hide_nodes` (a comma list of model nodes not to draw, e.g. a Sketchfab collision proxy), `[camera]` as for aircraft. Mirrored in `provpilot/aircraft/<id>/` like the aircraft (the editor reads `res://`, the installed build the install root; `AircraftInstaller` copies the models). | `aircraft/car` (Sketchfab "Low poly car 6", assetfactory, Sketchfab Standard), `aircraft/re44` (Sketchfab "Swiss Lok Re 4/4", hotdoghans, CC-BY-4.0: the credit in `CREDITS.txt` must stay with any distribution). Both packed from the downloaded glTF with `scripts/glb_tool.py pack`. |
| Welcome screen | `src/welcome_screen.cpp`: the `VEHICLES` table and the "Vehicles" frame under the aircraft; one selection shared with the aircraft list; `get_selected_aircraft_id()` returns the vehicle id, `is_vehicle_selected()`. | The spawn point is the runway's threshold stripes (`SPAWN_PAST_THRESHOLD_M`), as for aircraft. |
| Rig | `src/aircraft_rig.cpp`: `is_vehicle()`, `get_vehicle_kind()`, `get_vehicle_params()`; a vehicle loads no JSBSim model (the previous one stays, paused) and `hide_model_nodes()` applies `hide_nodes`. | |
| Drive | `src/drive_mode.cpp` (`DriveMode`, RefCounted): bicycle model on the LV95 plane (speed, heading, steering angle), throttle / brake as accelerations, the brake held at standstill reverses, rolling and air drag (the air drag is derived from the acceleration and the speed limit, so full throttle tops out at the limit), the steering lock shrinks with speed so the lateral acceleration stays under 6 m/s², the wheel centres itself. Every frame the ground is sampled under the four wheels (`WorldScenery::ground_height_below`: terrain plus the drawn runway / taxiway / apron lift, or the baked road surface where it is higher, see below) and the node sits on the centre height, pitched and rolled with the wheel heights (smoothed). JSBSim keeps its slew flag the whole time. | `node_3d.gd`: `arm()` on Fly! with a car, `update()` every frame after the slew update (it engages once the spawn's rebase cooldown released the node), `disengage()` on the next Fly! / Edit. The HUD labels of the slew mode show speed, ground height, steering and heading. |
| Keys | Section "Vehicle" of the keyboard mapping (`src/key_bindings.cpp`): `drive_accelerate` W, `drive_brake` S, `drive_left` A, `drive_right` D, `drive_handbrake` B. The cameras keep their flight keys (C, arrows, Page Up / Down). | Rebind in Settings > Keyboard. |
| Tests | `tests/smoke_drive_mode.tscn` (welcome frame, configs, kinematics without scenery), `tests/smoke_drive_car.tscn` (the real scene: Fly! with the car, W / A / B through the Input singleton, ground contact), `tests/probe_vehicle_render.gd` (a model with its cfg fit next to a metre grid: orientation and wheel height, `PROBE_ID car|re44`). | |

## Why no physics engine (Jolt)

Godot 4.7 ships Jolt, and it would be the right tool for wheel suspension,
collisions with buildings and vehicle-to-vehicle contact. It is not used here
on purpose, for now:

- The world is an ECEF double-precision scene with streamed terrain and road
  tiles that carry no collision shapes. Driving on Jolt needs a collision mesh
  per streamed tile (terrain, roads, aprons) kept in sync with streaming and
  with the floating origin, which is a project of its own.
- The aircraft never used it either: JSBSim gets the ground through a height
  query. The car uses the same query, so it stands on exactly the surface the
  aircraft lands on, roads included once the ground model knows them.
- The kinematic model is deterministic and cheap, which is what the road
  testing needs: drive along a baked road, see where it floats, sinks or
  jumps, and fix the bake.

When the roads are good enough to want suspension and collisions, the switch
is contained: `DriveMode::apply_pose` becomes "set the body pose from Jolt",
and the ground sampling becomes collision shapes built from the same height
data.

## The road surface in the ground model (2026-10-17)

The baked road tiles are part of the ground the car (and JSBSim) stand on:

- `S3DRoadManager::surface_height_at(e, n, h_hint, tol_m)` (scenery3d
  addon): every elevation-baked tile gets, when its GLB is parsed in the
  worker, a 2D grid (8 m cells) of its triangles one can stand on: ribbons,
  junction patches, embankment skirts, ballast and rails; walls, piers,
  parapets, roofs and the painted lines (vertex kinds 7, 14, 15, 17) are
  left out, as are the triangles of the ways the design hides. The query
  takes the triangles under the LV95 point, interpolates the baked Y, adds
  the tile's root drop (`baked_drop_m`) and the curvature sag of the tile's
  tangent plane, so the number is exactly the drawn surface, and returns the
  **highest one within `tol_m` of `h_hint`** (NAN when none): a car under a
  bridge keeps the ground, a car on the deck keeps the deck, a junction
  patch beats the ribbon it covers.
- `WorldScenery::road_surface_at(lat, lon, h_hint)` finds the road manager
  under the Scenery3D node and calls it with `ROAD_SURFACE_TOL_M` (2 m).
  `ground_height_below(world_pos)` uses the position's own height as the
  hint (the car's wheels: the centre height), `ground_height_geodetic` (the
  JSBSim ground callback) the terrain height. Both return
  **max(terrain + pavement lift, road)**: the drawn ground is the top-most
  surface, and a ribbon sunk into a terrain bump stays below it.
- Only elevation-baked tiles (the base tier) take part; the editor's live
  design meshes do not until they are baked.
- Test: `tests/smoke_road_surface.tscn`.

## Next steps (not done)

1. **Trains.** The rails of the base tier are real geometry at true gauge.
   A train needs a track graph (the design's rail chains and the baked
   railway ways carry it, the switches too) and a "follow the track" mode:
   position = distance along a chain, speed from throttle / brake, the
   locomotive's two bogie pivots sample the rail height. `kind "train"` in the
   cfg is the hook.
2. **Vehicle cockpit.** The driver's eye point is in the cfg; the car has no
   instruments. Speed is on the HUD.
