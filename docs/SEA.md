# The sea

Stage 1 of the water plan (2026-11-15; WORLD_SCENERY_DESIGN.md §11.4 had left
the sea open): a sea surface with an exact coastline for the base tier.

## Principle: the coast lives in the terrain

No sea polygons, no mask file. `scripts/build_world_sea.py` cuts the
OpenStreetMap coastline into the 34 px terrain tiles, and the engine draws one
surface at 0 m everywhere; the depth test makes the shore.

- **Source**: osmdata.openstreetmap.de `water-polygons-split-4326` (the sea as
  polygons, 908 MB zip) in `<cache_root>/osm_coast/`.
- **Sea pixels**: `h = min(h, -0.05 x distance to the shore)`, at most -30 m by
  distance alone; deeper bathymetry already in the tile stays.
- **Land pixels**: at least 0.3 m (reached over the first metres from the
  shore), never more. The waterline, interpolated between a land and a sea
  pixel, falls on the coastline; land below sea level (polders, tidal flats)
  is lifted to 0.3 m so the sea cannot flood it. (Until 2026-11-15 the floor
  rose by 0.05 x shore distance to 10.3 m and buried every low coast under a
  shelf: Aalborg's runways ended at a tile edge.)
- **Islands the height model lacks**: a no-data pixel on OSM land rises
  gently from the shore (to 3 m), and a tile the region does not have but
  that holds land and sea by the coastline is created (12 tiles outward per
  run, never where another region has the tile). A missing tile therefore
  means open sea: the far terrain fills missing tiles from their neighbours'
  edge heights, and an island that ran into one was stretched over
  kilometres of sea (Tisler, 2026-11-15).
- **In place, idempotent**: the same pixels in `terrain/` and `terrain_base/`;
  runway plateaus and road corridors stay. Airport heights fitted to ground
  below 0.3 m (runway planes, taxiway paths, aprons, lights: 233 in Denmark,
  724 in Norway) are lifted with the land in `airports/runways.json`. Order
  after a rebake:
  `bake` -> `flatten` -> **sea** -> road bake.
- **Streaming**: nothing new to ship. A streamed base scenery carries the
  coast in the terrain tiles it downloads anyway; open sea outside any pack is
  the globe below 0 m under the same surface.

`.venv/bin/python scripts/build_world_sea.py --region norway [--bbox E0 N0 E1 N1] [--dry-run]`

`--from-source` takes the ground of every coastal tile from the height model
again before cutting (undoes an earlier cut). It drops the airport plateaus
of those tiles, so the restore is three steps:
`build_world_sea.py --from-source`, `build_world_terrain.py flatten --no-fetch`,
`build_world_sea.py`.

The globe (world3d) runs with `clamp_sea_level = false`: its ETOPO sea floor
lies under the surface. Clamped to 0 m it met the sea surface at every mesh
vertex and showed through as a grid of light stars from altitude.

## Runtime

`SeaSurface` (`src/sea_surface.cpp`, `shaders/sea_surface.gdshader`): a polar
disc on the local sphere of curvature, centred under the camera, reaching past
the horizon (rebuilt when the altitude band changes), placed in ECEF each
frame. Transparent pass, drawn before the clouds (render priority -20). The
shader reads the depth buffer: clear over the first decimetres (soft shore),
shallow to deep colour with depth, sky reflection by Fresnel, a sun glint,
ripples as procedural normals anchored to the world and drifting with the
FDM's wind. Dark at night through the twilight factor.

Ditching: `WorldScenery::ground_height_*` answers the water surface minus
`water_sink_m` over the sea (never below the floor). `node_3d.gd` keeps it at
0.8 m (the water lets the wheels in before it carries) and, once an aircraft
rests on the water below 30 kt, lowers it by 0.4 m/s: the aircraft goes down
to the sea floor. Back over land or 6 m up, it resets.

The heat shimmer (`ExhaustEffects`) reads the screen copy, which is taken
before the sea is drawn: it is left out where the opaque scene behind the
plume lies under the sea plane (it showed the green sea floor).

Test: `probe_view -- <out_dir> <lat> <lon> <alt_m> <pitch_down> <bearing ...>` (free camera, clear sky), or `DUSK_LAT=59.76 DUSK_LON=10.56 DUSK_HDG=15 DUSK_AGL_FT=2500 probe_dusk -- <out_dir> 12.5`.

## Open

- Lakes: still painted land cover only; the same surface at each lake's own
  level is the natural next step (per-lake level, a small polygon set).
- Stage 2: depth colour from real bathymetry (GEBCO where SRTM is flat),
  sea floor painted sand instead of the land-cover base green.
- Stage 3: waves and swell from the wind, foam at the shore, whitecaps; the
  sky's actual colours in the reflection (sunset), cloud reflections.
- The VFR map shows the cut once `build_map_overview.py` has run after the
  sea bake (done for the seven coastal regions 2026-11-15); the open sea is
  the map's fill colour.
