extends RefCounted
## SkyAtmosphere — day/night sun + atmospheric sky for the world3d globe.
##
## Replaces the old flat procedural sky (whose horizon stayed flat above
## ~90 km) and the C++ Atmosphere module's background handling. It:
##   * installs the atmospheric-scattering sky shader so the horizon follows
##     the Earth's curvature at every altitude (curved limb from orbit, blue
##     sky at ground);
##   * computes the sun position from the simulated time of day + the observer
##     latitude/longitude and swings the scene DirectionalLight3D so shadows
##     track the sun through the day (and switch off at night);
##   * keeps the altitude ramps the C++ module used (fog fades toward space,
##     ambient/glow taper) but leaves the background on the sky shader so the
##     atmospheric limb stays visible in orbit;
##   * leaves the coarse globe visible at every altitude as the fallback
##     surface where Scenery3D has no resident geometry yet (node_3d.gd's
##     coverage-driven suppression culls it under loaded detailed tiles).
##
## Owned by node_3d.gd: setup() once the world3d globe is mounted, set_time()
## when the time-of-day settings change, update(get_viewport()) every frame.

const PLANET_RADIUS := 6371000.0      # mean Earth radius (m)
const ATMOSPHERE_HEIGHT := 100000.0   # scattering shell thickness (m)

# Altitude ramps (m ASL, mirror the old C++ Atmosphere defaults).
# Haze lives in the boundary layer: from a camera above it the light path to
# the ground crosses only HAZE_LAYER_M of haze, so the uniform fog density
# is scaled by HAZE_LAYER_M / altitude (a 40 km camera sees 1/16 of the
# ground density along a 40 km path = the same optical depth as at the
# ground). The old linear 4..60 km ramp kept ~1/3 density at 40 km and
# washed the ground out from 50 down to 30 km.
const HAZE_LAYER_M := 2500.0          # boundary-layer thickness
const FOG_ZERO_ALT_M := 120000.0      # fog off above this camera altitude
const SKY_FULL_ALT_M := 12000.0       # full ambient/glow at/below this altitude
const SKY_TAPER_ALT_M := 100000.0     # ambient/glow fully tapered at/above this

# Constant-colour ambient floor (day/night scaled), normal-independent. Until
# 2026-09-18 this floor was the ONLY light on the terrain: the tiles were
# wound counter-clockwise (Godot front faces are clockwise), so the
# double-sided material flipped their normals and the sun never lit them —
# hence the "normals face away from the sun" impression, and the black patch
# from space where this floor is dimmed. Fixed in S3DTile (winding).
const AMBIENT_COLOR := Color(0.52, 0.58, 0.68)
const AMBIENT_DAY_ENERGY := 0.7
const AMBIENT_NIGHT_ENERGY := 0.04
# Camera altitudes between which the local dusk fade of the sun light gives
# way to full-strength global sunlight (day/night terminator on the globe).
const SUN_LOCAL_ALT_M := 30000.0
const SUN_SPACE_ALT_M := 400000.0

# Registered nodes (session-stable).
var environment: Environment = null
var sun_light: DirectionalLight3D = null
var fill_light: DirectionalLight3D = null
var globe_node: Node = null
var sky_material: ShaderMaterial = null

# Observer location (used for the solar position; the sun barely moves across
# the ~200 km patch so the origin is accurate enough).
var observer_lat := 47.7
var observer_lon := 8.6
var _observer_up := Vector3.UP  # ellipsoid normal at the camera (ECEF world)
# Per-frame light state for other renderers (clouds): set in update().
var sun_dir_now := Vector3.UP
var sun_color_now := Color(1.0, 0.97, 0.92)
var sun_energy_now := 1.0
var ambient_energy_now := 0.7

# Time source: real system clock, or a manual local time (minutes past midnight).
var use_system_time := true
var manual_minutes := 720
# Current local time-of-day in seconds (drives the sun + HUD). In manual mode it
# advances in real time; in system mode it mirrors the real clock.
var _time_seconds := 43200.0
# Base scene values captured at setup (the authored values).
var _base_fog_density := 0.00005
var _base_ambient_energy := 2.2
var _base_glow_intensity := 0.5
var _base_sun_energy := 1.0
var _base_fill_energy := 0.6


## Register the scene nodes and install the atmospheric sky shader. Captures
## the Environment's authored fog/ambient/glow values as the ground baselines.
func setup(env: Environment, sun: DirectionalLight3D, fill: DirectionalLight3D, globe: Node, lat_deg: float, lon_deg: float) -> void:
	environment = env
	sun_light = sun
	fill_light = fill
	globe_node = globe
	observer_lat = lat_deg
	observer_lon = lon_deg
	if environment != null:
		_base_fog_density = environment.fog_density
		_base_ambient_energy = environment.ambient_light_energy
		_base_glow_intensity = environment.glow_intensity
		_install_sky_material()
	if is_instance_valid(sun_light):
		_base_sun_energy = sun_light.light_energy
	if is_instance_valid(fill_light):
		_base_fill_energy = fill_light.light_energy


## Meteorological visibility -> exponential fog density (Koschmieder: the 5 %
## contrast threshold sits at 3.912 / beta). Replaces the authored baseline;
## the altitude ramp in _apply_altitude still fades it out toward space.
func set_visibility_km(km: float) -> void:
	_base_fog_density = 3.912 / (maxf(km, 0.1) * 1000.0)


## Choose the time source: real UTC clock, or a manual local time-of-day.
func set_time(use_system: bool, local_minutes: int) -> void:
	use_system_time = use_system
	manual_minutes = clampi(local_minutes, 0, 1439)
	_time_seconds = float(manual_minutes) * 60.0


## Current simulated local time-of-day as "HH:MM:SS".
func get_time_string() -> String:
	var s := int(_time_seconds) % 86400
	return "%02d:%02d:%02d" % [s / 3600, (s / 60) % 60, s % 60]


## Per-frame update: recompute the sun, drive the light + sky shader + shadows,
## apply the altitude ramps, and gate the coarse globe's visibility.
func update(delta: float, viewport: Viewport) -> void:
	if environment == null:
		return
	# Advance the manual clock in real time (system mode is read from the OS).
	if not use_system_time:
		_time_seconds = fposmod(_time_seconds + delta, 86400.0)
	var cam: Camera3D = viewport.get_camera_3d() if viewport != null else null
	# ECEF world (design §9.5): the observer is wherever the camera is —
	# geodetic position and height from its ECEF position.
	var alt: float = 0.0
	var cam_normal := Vector3.UP
	if cam != null and cam.global_position.is_finite() and cam.global_position.length() > 1.0e6:
		var g: Vector3 = W3DCoords.ecef_to_geodetic(cam.global_position)  # (lat, lon, h)
		observer_lat = g.x
		observer_lon = g.y
		alt = g.z
		cam_normal = W3DCoords.geodetic_normal(g.x, g.y)
	_observer_up = cam_normal

	var when := _current_time()
	var dir_to_sun := _solar_direction(observer_lat, observer_lon, when.x, when.y)
	var sin_elev: float = dir_to_sun.dot(cam_normal)
	# Directional light fades to zero AS the sun reaches the horizon and stays
	# off below it — a DirectionalLight3D pointing upward would otherwise light
	# the undersides of the terrain/buildings during dawn/dusk.
	var day := clampf(sin_elev / 0.08, 0.0, 1.0)
	# From space the ECEF geometry already produces the day/night terminator
	# (surfaces facing away from the sun get no light), so the local fade
	# must not dim the whole planet: blend the sun back to full energy between
	# SUN_LOCAL_ALT_M and SUN_SPACE_ALT_M, and pull the ambient floor down to
	# night level so the night side goes dark instead of glowing uniformly.
	var space := clampf((alt - SUN_LOCAL_ALT_M) / (SUN_SPACE_ALT_M - SUN_LOCAL_ALT_M), 0.0, 1.0)
	var sun_factor := lerpf(day, 1.0, space)
	# Sun disk stays visible down to the altitude-adjusted horizon: climbing
	# lowers the geometric horizon (dip angle), so the sun is still up from
	# altitude even when it's below the sea-level horizon. Full until it reaches
	# that horizon, then fades over the last ~1°.
	var dip := acos(clampf(PLANET_RADIUS / (PLANET_RADIUS + maxf(alt, 0.0)), 0.0, 1.0))
	var horizon_sin := -sin(dip) - 0.010  # extra ~0.6° for atmospheric refraction
	var disk := clampf((sin_elev - horizon_sin) / 0.02, 0.0, 1.0)

	sun_dir_now = dir_to_sun
	sun_energy_now = _base_sun_energy * sun_factor
	sun_color_now = _sun_color(sin_elev).lerp(Color(1.0, 0.97, 0.92), space)
	if is_instance_valid(sun_light):
		_orient_sun(dir_to_sun)
		sun_light.light_energy = sun_energy_now
		sun_light.light_color = sun_color_now
		sun_light.visible = sun_factor > 0.001
	if is_instance_valid(fill_light):
		fill_light.light_energy = _base_fill_energy * day * (1.0 - space)

	if sky_material != null:
		sky_material.set_shader_parameter("sun_direction", dir_to_sun)
		sky_material.set_shader_parameter("sun_disk_energy", disk)
		# Keep the scattering sphere tangent to the local ground plane at the
		# camera. The flat world frame is one LV95 tangent plane (Swiss origin),
		# so a fixed centre (0, -R, 0) only matches near that origin: ~1 300 km
		# north in Norway the plane floats ~130 km above the true sphere and
		# the horizon bent like an orbital limb. Tracking the centre under the
		# camera makes the shader's altitude equal to flat-frame Y (= ASL)
		# everywhere, so dip + horizon distance stay correct in every region
		# (WORLD_SCENERY_DESIGN §3.2). The coarse globe itself is re-based onto
		# the active airport by node_3d.gd (_rebase_globe_to), which keeps its
		# surface aligned with this sphere at ground level there.
		# ECEF: the shader's sphere (mean radius) is kept tangent to the
		# ellipsoid under the camera — centre = camera - normal * (R + h) —
		# so dip and horizon distance come out right at every latitude.
		if cam != null:
			sky_material.set_shader_parameter(
				"planet_center", cam.global_position - cam_normal * (PLANET_RADIUS + alt))

	_apply_altitude(alt, day, space)

	# The coarse globe stays visible at ALL altitudes (no more 25 km gate): it
	# is the fallback surface wherever Scenery3D has no resident geometry yet —
	# intro zoom, seconds after a teleport, beyond the streaming square. With
	# the gate on, any tile gap below 25 km showed space and stars straight
	# through the ground. node_3d.gd's coverage-driven suppression rectangles
	# cull the globe cell-by-cell under loaded detailed tiles instead, so no
	# double surface remains where geometry exists (globe_node kept for future
	# altitude gating).


# ── Internals ──────────────────────────────────────────────────────────

func _install_sky_material() -> void:
	var shader: Shader = load("res://shaders/atmosphere_sky.gdshader")
	if shader == null:
		push_warning("SkyAtmosphere: res://shaders/atmosphere_sky.gdshader missing — keeping procedural sky")
		return
	var mat := ShaderMaterial.new()
	mat.shader = shader
	mat.set_shader_parameter("planet_radius", PLANET_RADIUS)
	mat.set_shader_parameter("atmosphere_radius", PLANET_RADIUS + ATMOSPHERE_HEIGHT)
	mat.set_shader_parameter("planet_center", Vector3(0.0, -PLANET_RADIUS, 0.0))
	var sky: Sky = environment.sky
	if sky == null:
		sky = Sky.new()
		environment.sky = sky
	sky.sky_material = mat
	environment.background_mode = Environment.BG_SKY
	sky_material = mat


## Fog fade with altitude + a day/night constant-colour ambient floor. The
## ambient is a plain colour (not sky-derived): the atmosphere sky shader gives
## little usable ambient, and some imported terrain (e.g. the 1 m/px BW tiles)
## has normals facing away from the sun — a normal-independent floor keeps those
## surfaces readable instead of rendering black. The background stays on the sky
## shader (which renders space black + the atmospheric limb by itself).
## True while the scenery editor is on: no fog at all, whatever the weather.
var clear_air := false


func _apply_altitude(alt: float, day: float, space: float = 0.0) -> void:
	var t := clampf(HAZE_LAYER_M / maxf(alt, HAZE_LAYER_M), 0.0, 1.0)
	if alt >= FOG_ZERO_ALT_M or clear_air:
		t = 0.0
	environment.fog_density = _base_fog_density * t
	environment.fog_enabled = t > 0.0

	var ts := clampf((SKY_TAPER_ALT_M - alt) / (SKY_TAPER_ALT_M - SKY_FULL_ALT_M), 0.0, 1.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_sky_contribution = 0.0
	environment.ambient_light_color = AMBIENT_COLOR
	var ambient_local := lerpf(AMBIENT_NIGHT_ENERGY, AMBIENT_DAY_ENERGY, day) * lerpf(0.5, 1.0, ts)
	environment.ambient_light_energy = lerpf(ambient_local, AMBIENT_NIGHT_ENERGY, space)
	ambient_energy_now = environment.ambient_light_energy
	environment.glow_intensity = _base_glow_intensity * ts
	environment.glow_enabled = ts > 0.02


## Point the DirectionalLight3D so its light travels from the sun toward the
## ground (local -Z = -dir_to_sun), leaving its position untouched.
func _orient_sun(dir_to_sun: Vector3) -> void:
	var z := dir_to_sun.normalized()       # local +Z points at the sun
	var up_ref := _observer_up
	if absf(z.dot(up_ref)) > 0.999:
		up_ref = Vector3.FORWARD
	var x := up_ref.cross(z).normalized()
	var y := z.cross(x).normalized()
	sun_light.global_transform = Transform3D(Basis(x, y, z), sun_light.global_position)


func _sun_color(sin_elev: float) -> Color:
	var warm := Color(1.0, 0.55, 0.30)
	var white := Color(1.0, 0.97, 0.92)
	return warm.lerp(white, clampf(sin_elev / 0.25, 0.0, 1.0))


## Returns Vector2(day_of_year, utc_hours) for the active time source, and keeps
## `_time_seconds` in sync (local mean-solar time) for the HUD.
func _current_time() -> Vector2:
	var sys := Time.get_datetime_dict_from_system(true)  # UTC
	var doy := float(_day_of_year(sys.year, sys.month, sys.day))
	var utc_hours: float
	if use_system_time:
		utc_hours = sys.hour + sys.minute / 60.0 + sys.second / 3600.0
		# Local mean-solar time (what drives the sun) for the HUD readout.
		_time_seconds = fposmod((utc_hours + observer_lon / 15.0) * 3600.0, 86400.0)
	else:
		# Manual value is local mean-solar time; convert to UTC by longitude.
		utc_hours = (_time_seconds / 3600.0) - observer_lon / 15.0
	return Vector2(doy, utc_hours)


## Unit vector toward the sun in ECEF, using the NOAA solar-position
## approximation (equation of time + declination) evaluated in the observer's
## local ENU frame and rotated into ECEF.
func _solar_direction(lat_deg: float, lon_deg: float, doy: float, utc_hours: float) -> Vector3:
	var g := TAU / 365.0 * (doy - 1.0 + (utc_hours - 12.0) / 24.0)
	var eqtime := 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) \
		- 0.014615 * cos(2.0 * g) - 0.040849 * sin(2.0 * g))  # minutes
	var decl := 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) \
		- 0.006758 * cos(2.0 * g) + 0.000907 * sin(2.0 * g) \
		- 0.002697 * cos(3.0 * g) + 0.00148 * sin(3.0 * g)    # radians

	var tst := utc_hours * 60.0 + eqtime + 4.0 * lon_deg      # true solar time (min)
	var ha := deg_to_rad(tst / 4.0 - 180.0)                   # hour angle (0 at solar noon)

	var lat := deg_to_rad(lat_deg)
	var sin_lat := sin(lat)
	var cos_lat := cos(lat)
	var sin_dec := sin(decl)
	var cos_dec := cos(decl)
	var sin_ha := sin(ha)
	var cos_ha := cos(ha)

	# Equatorial → local horizontal (ENU) direct transform.
	var east := -cos_dec * sin_ha
	var up := sin_dec * sin_lat + cos_dec * cos_ha * cos_lat
	var north := sin_dec * cos_lat - cos_dec * cos_ha * sin_lat
	# ENU → ECEF at the observer (W3DCoords WUN frame: +X West, +Y Up, +Z North).
	var wun: Transform3D = W3DCoords.enu_to_ecef_transform(lat_deg, lon_deg, 0.0, true)
	return (wun.basis * Vector3(-east, up, north)).normalized()


static func _day_of_year(y: int, m: int, d: int) -> int:
	var days := [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
	if (y % 4 == 0 and y % 100 != 0) or y % 400 == 0:
		days[1] = 29
	var n := d
	for i in range(m - 1):
		n += days[i]
	return n
