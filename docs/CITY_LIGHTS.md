# City lights

Town and city lights at night (`src/city_lights.cpp`, `CityLights`, shader
`provpilot/shaders/city_lights.gdshader`), since 2026-11-15.

- **Data**: no new bake. The lamps come from the land-cover tiles every base
  region already ships (`vegetation/forest_E_N.bin`, FOR1, written by
  `scripts/build_world_forests.py`): polygons of kind 7 (built-up) and 8
  (industrial), and the road corridors (kind 3) that run through them. The
  node gets the same directories as the tile manager's `landcover_paths`.
- **Lamps**: position-hashed, so a town wears the same lights every session.
  Built-up ground: a jittered 44 m grid, about a fifth of the plots dark,
  sodium / warm white / LED colours; industry: cooler and larger; street lamps
  every 46 m along roads (half width from 2.5 m) inside built-up polygons.
- **Two levels**: near squares (1024 m, every lamp and the streets) where the
  camera is closer than `near_radius_m` (9 km), and far blocks (4 x 4 squares
  in one mesh, an 88 m grid of larger lamps) out to `far_radius_m` (45 km).
  The shader cross-fades the two by distance (UV.x = level), so nothing pops
  and no lamp is drawn twice. From cruise altitude only far blocks load.
- **Heights**: the terrain callable sampled on a grid per mesh (128 m near,
  256 m far), bilinear in between, lamps 6 m / 14 m over it.
- **Placement**: vertices are float offsets from the mesh's SW corner on the
  ellipsoid; the node sits there in double precision (ECEF).
- **Light**: `node_3d.gd` passes `1 - smoothstep(0.2, 0.55, twilight)` (and the
  airfield factor in poor visibility) and the fog density; sprites scale with
  the screen height, dim with the haze and fade out at the far radius. By day
  nothing is read or built.
- **Cost** (Zurich, night, 4K frame): about 185 000 lamps in 820 meshes, 200
  draw calls in view, no measurable frame-rate change. Files are read on one
  worker thread, nearest first; the first full ring takes some seconds.

Test: `probe_dusk -- <out_dir> [hours ...]` (`DUSK_WAIT` seconds per shot,
`DUSK_NO_CITY=1` compares the frame rate without the lights).

Open: no Settings switch; the lamps do not light the ground or the haze
(no glow dome over a city); motorways outside towns stay dark.
