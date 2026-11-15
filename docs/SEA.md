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
- **Land pixels**: `h = max(h, +0.05 x distance to the shore)`, and at least
  0.3 m beyond the first pixel. The waterline, interpolated between a land and
  a sea pixel, falls on the coastline; land below sea level (polders, tidal
  flats) is lifted to 0.3 m so the sea cannot flood it.
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

Test: `DUSK_LAT=59.76 DUSK_LON=10.56 DUSK_HDG=15 DUSK_AGL_FT=2500 probe_dusk -- <out_dir> 12.5`.

## Open

- Lakes: still painted land cover only; the same surface at each lake's own
  level is the natural next step (per-lake level, a small polygon set).
- Stage 2: depth colour from real bathymetry (GEBCO where SRTM is flat),
  sea floor painted sand instead of the land-cover base green.
- Stage 3: waves and swell from the wind, foam at the shore, whitecaps; the
  sky's actual colours in the reflection (sunset), cloud reflections.
- VFR map and map_overview tiles do not know the cut yet.
