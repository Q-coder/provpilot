extends Node3D
## ProVPilot main scene. Wires the C++ modules (flight model, aircraft rig,
## scenery stack, cameras, HUD, spawn) to the scene nodes and the GDScript
## helpers (sky, clouds, runways, weather, FlightGear bridge, scenery editor)
## and sequences the welcome -> Fly! -> intro zoom -> flight flow.
##
## The world is WGS84 ECEF (design §9.5): the globe sits at the origin,
## Scenery3D places tiles on the ellipsoid, the aircraft node carries an ECEF
## position + body-to-ECEF basis. LV95 E/N is only a data/grid coordinate
## (spawn targets, HUD, VFR map, coverage mask); see WorldScenery.


const DEFAULT_AIRCRAFT_ID := "c172p"
# Home point in LV95 (Schaffhausen LSPF area) where the aircraft is parked
# before the first flight. Mirrors Scenery3D's origin_east/north (read from
# ProjectSettings in _ready) so its flat-mode data grid and the home point agree.
var TERRAIN_CENTER_E: float = 2682880.0
var TERRAIN_CENTER_N: float = 1283072.0

# Camera modes mirror CameraRig::Mode; kept in sync by flip_camera() and the
# intro handoff because lever picking and the FlightGear hover branch on them.
enum CameraMode { COCKPIT, ORBIT, CHASE }

# ── Scene nodes ────────────────────────────────────────────────────────────
var jsb_node: Node                   # JSBGodot flight model
var camera1: Camera3D                # cockpit
var camera2: Camera3D                # external orbit / chase
var active_camera: Camera3D
var camera_mode: int = CameraMode.COCKPIT
var scenery3d_node: Node
var vegetation_node: Node
var globe_node: Node = null          # W3DGlobe under GlobeMount (world3d)
var _intro_camera: Camera3D = null   # root-level camera for the intro zoom
@onready var _ac: Node3D = $AC
@onready var _vfr_map: VfrMap = $Control/VfrMap

# ── C++ modules ────────────────────────────────────────────────────────────
var _aircraft := AircraftRig.new()          # aircraft.cfg, model nodes, levers, engine loop
var _world := WorldScenery.new()            # elevation stack, geodesy, streaming, globe mask
var _camera_rig := CameraRig.new()          # cockpit head look, orbit, chase
var _slew_mode := SlewMode.new()            # Shift+Y free movement
var _drive_mode := DriveMode.new()          # ground vehicles (welcome "Vehicles"): kinematic car
var _intro_zoom := IntroZoom.new()          # space -> spot cinematic
var _terrain_elevation := TerrainElevation.new()   # post-teleport pin, ground push
var _spawn := FlightSpawn.new()             # first flight, teleport, deferred init
var _loading_progress := LoadingProgress.new()
var _hud := FlightHud.new()                 # readouts, FPS, weather overlay, intro HUD
var _loading_screen: LoadingScreen = null
var _settings_screen: SettingsScreen = null
var _settings: SettingsStore = null

# ── GDScript helpers ───────────────────────────────────────────────────────
var _sky := SkyAtmosphere.new()       # sun, atmosphere, fog
var _clouds: CloudLayers = null
var _runways: Runways = null          # airport surfaces + lights (design §12)
var _runways_merged: Dictionary = {}  # icao -> merged pack airport entry
var _live_weather: Node = null        # C++ LiveWeather (METAR)
var _welcome_screen: WelcomeScreen = null
var _scenery_editor := SceneryEditor.new()   # F8 / welcome Edit Mode
var _scenery_objects := SceneryObjects.new()  # placed models of the object packs (priority order)

# ── Session state ──────────────────────────────────────────────────────────
var _fly_loading := false        # destination streaming in after Fly!
var _intro_playing := false      # GDScript mirror of IntroZoom
var _edit_session := false       # Edit Mode from the welcome screen (no flight)
var _escape_held := false        # Escape seen and acted on; cleared on release
var _settings_from_welcome := false
var _visibility_km := 40.0       # last applied visibility (airfield lights on below 1 km)
var _cam_sanity_reported := false

# Read-only views of module state kept for the smoke tests and the editor.
var terrain_initialized: bool:
	get: return _spawn.is_initialized()
var _pending_terrain_init: bool:
	get: return _spawn.is_init_pending()
var _aircraft_id: String:
	get: return _aircraft.get_aircraft_id()
var _aircraft_cfg: AircraftConfig:
	get: return _aircraft.get_config()
var _model_root_path: String:
	get: return _aircraft.get_model_root_path()
var _control_surfaces: ControlSurfaces:
	get: return _aircraft.get_control_surfaces()
var _cockpit_controls: CockpitControls:
	get: return _aircraft.get_cockpit_controls()
var _fg_bridge: FGBridge:
	get: return _aircraft.get_fg_bridge()
var _fg_controls: FGControls:
	get: return _aircraft.get_fg_controls()
var _fg_sounds: FGSounds:
	get: return _aircraft.get_fg_sounds()
var _fg_animator: FGAnimator:
	get: return _aircraft.get_fg_animator()
var _aircraft_lights: AircraftLights:
	get: return _aircraft.get_aircraft_lights()
var _parking_brake: bool:
	get: return _aircraft.get_parking_brake()
	set(v): _aircraft.set_parking_brake(v)


# ── Startup ────────────────────────────────────────────────────────────────

func _ready() -> void:
	add_child(_hud)
	_hud.register_flight_hud($Control)
	_hud.weather_overlay_toggled.connect(func(_show: bool) -> void: _refresh_weather_overlay())
	_loading_screen = LoadingScreen.new()
	add_child(_loading_screen)

	jsb_node = $AC/JSBGodot
	camera1 = $AC/Camera1
	camera2 = $AC/Camera2
	active_camera = camera1
	camera1.current = true
	camera2.current = false
	scenery3d_node = get_node_or_null("Scenery3D")
	vegetation_node = get_node_or_null("Vegetation")
	if scenery3d_node == null:
		push_warning("Scenery3D node not found - ground collision may not work correctly")
	# Swiss <-> Godot conversions and tile placement must share one origin.
	if ProjectSettings.has_setting("scenery3d/origin_east"):
		TERRAIN_CENTER_E = float(ProjectSettings.get_setting("scenery3d/origin_east"))
	if ProjectSettings.has_setting("scenery3d/origin_north"):
		TERRAIN_CENTER_N = float(ProjectSettings.get_setting("scenery3d/origin_north"))

	# Aircraft: config, model nodes, cockpit camera, engine loop.
	_camera_rig.register_cameras(camera1, camera2, _ac)
	_aircraft.setup(self, _ac, camera1, _camera_rig)
	_aircraft.load_config(DEFAULT_AIRCRAFT_ID)
	_aircraft.apply_model_normals(_ac)
	_aircraft.apply_camera_config()
	_aircraft.resolve_nodes(jsb_node)
	_aircraft.setup_engine_sound(self)
	add_child(_scenery_editor)
	_scenery_editor.setup(self)
	_scenery_objects.name = "SceneryObjects"
	add_child(_scenery_objects)
	_scenery_objects.elevation = Callable(_world, "scenery_elevation_geodetic")

	# World: globe, scenery stack, ground query for JSBSim's contact points.
	# Streaming stays OFF until Fly! so no tiles load around the origin while
	# the welcome screen is up (FlightSpawn turns it on at the destination).
	_setup_world3d_globe()
	_world.register_nodes(self, _ac, scenery3d_node, globe_node, vegetation_node)
	_world.set_vfr_map(_vfr_map)
	_world.set_streaming(false)
	jsb_node.set_ground_query(Callable(_world, "ground_height_geodetic"))
	_vfr_map.setup(InstallRoot.get_config_path())

	# Flight-side modules.
	_slew_mode.register_nodes(_ac, $Control/BottomRow/Label, $Control/BottomRow/Label2, $Control/BottomRow/Label3,
		$Control/BottomRow/Label4, $Control/BottomRow/Label5, $Control/Label6, $Control/Label7, $Control/Label8)
	_slew_mode.set_pm_label($Control/Label9)
	_drive_mode.register_nodes(_ac, _world, $Control/BottomRow/Label, $Control/BottomRow/Label2, $Control/BottomRow/Label3,
		$Control/BottomRow/Label4, $Control/BottomRow/Label5, $Control/Label8)
	_terrain_elevation.register_nodes(_ac)
	_spawn.register_nodes(_ac, jsb_node, _world, _terrain_elevation, _control_surfaces, _cockpit_controls, _slew_mode)
	_spawn.spawned.connect(_on_spawned)
	# Park the aircraft (and its cameras) on the ellipsoid at the home point:
	# the scene file's transform would be a point near the Earth's centre.
	_spawn.park(TERRAIN_CENTER_E, TERRAIN_CENTER_N, 600.0, 0.0)

	# UI + environment (the settings store must exist before the sky reads
	# the time of day and the tree density is applied).
	_loading_screen.hide_loading()
	_create_settings_screen()
	_create_welcome_screen()
	_setup_sky_atmosphere()
	_apply_tree_density()
	_apply_scenery_sources()
	_world.refresh_globe_suppress()
	print("Node3D ready - welcome screen shown (scenery deferred)")


## Mount the world3d globe: the world IS ECEF, so the mount is the identity
## and the coverage mask is looked up in LV95 grid metres by the globe shader
## (mask frame 1). Also creates the dedicated intro camera.
func _setup_world3d_globe() -> void:
	globe_node = get_node_or_null("GlobeMount/Globe")
	if globe_node == null or not ClassDB.class_exists("W3DCoords"):
		push_warning("World3D: globe missing or extension not loaded - keeping Scenery3D terrain only")
		globe_node = null
		return
	$GlobeMount.transform = Transform3D.IDENTITY
	if globe_node.has_method("set_coverage_mask_frame"):
		globe_node.set_coverage_mask_frame(1)
	# Cameras must reach the global horizon (the default far was 200 km).
	camera1.far = 12000000.0
	camera2.far = 12000000.0
	# Parented to the ROOT (not the aircraft) so its space position is not
	# dragged around by JSBSim's per-frame AC transform writes.
	_intro_camera = Camera3D.new()
	_intro_camera.name = "IntroCamera"
	_intro_camera.far = 4.0e7
	_intro_camera.fov = 70.0
	add_child(_intro_camera)
	_intro_zoom.register_nodes(_ac, _intro_camera, camera1, camera2, _camera_rig)


func _create_settings_screen() -> void:
	_settings_screen = SettingsScreen.new()
	add_child(_settings_screen)  # loads provpilot.cfg in its _ready
	_settings = _settings_screen.get_store()
	_wire_aircraft_settings_panel()
	_settings_screen.closed.connect(_on_settings_closed)
	_clouds = CloudLayers.new()
	_clouds.name = "CloudLayers"
	add_child(_clouds)
	_runways = Runways.new()
	_runways.name = "Runways"
	add_child(_runways)
	_world.set_runways(_runways)
	_live_weather = LiveWeather.new()
	_live_weather.latlon_provider = _aircraft_latlon
	_live_weather.weather_updated.connect(_on_live_weather_updated)
	_live_weather.weather_failed.connect(_on_live_weather_failed)
	add_child(_live_weather)


func _create_welcome_screen() -> void:
	_welcome_screen = WelcomeScreen.new()
	add_child(_welcome_screen)
	_welcome_screen.flight_requested.connect(_on_welcome_flight_requested)
	_welcome_screen.edit_requested.connect(_on_welcome_edit_requested)
	_scenery_editor.exited.connect(_on_scenery_editor_exited)
	_welcome_screen.settings_requested.connect(_on_welcome_settings_requested)
	_welcome_screen.continue_requested.connect(_on_welcome_continue_requested)
	_welcome_screen.scenery_setup_requested.connect(_on_welcome_scenery_setup_requested)
	# The bundled airport catalog, tagged by the installed scenery packs plus
	# any region present on disk outside the in-app downloader.
	var regions: Dictionary = {}
	if _settings:
		regions = AirportsLib.installed_regions(_settings.get_setting("installed_scenery_packs"))
		regions.merge(AirportsLib.installed_regions_from_areas(_settings.get_scenery_areas()))
	_welcome_screen.set_installed_regions(regions)
	_welcome_screen.populate_airports(_catalog_with_pack_runways())
	_welcome_screen.show_welcome()


## Day/night sun + atmospheric sky: the scene Environment, the Sun/Fill
## lights and the globe go to the SkyAtmosphere module; the home point is the
## initial observer.
func _setup_sky_atmosphere() -> void:
	var we := get_node_or_null("Environment/WorldEnvironment")
	var ll: Vector2 = Geo.lv95_to_wgs84(TERRAIN_CENTER_E, TERRAIN_CENTER_N)
	_sky.setup(we.environment if we else null, get_node_or_null("Environment/Sun"),
		get_node_or_null("Environment/Fill"), globe_node, ll.x, ll.y)
	_push_time_of_day_settings()
	_push_weather_settings()


# ── Per frame ──────────────────────────────────────────────────────────────

func _process(delta: float) -> void:
	_spawn.process_deferred_init()   # first flight: JSBSim onto the streamed terrain
	_sky.update(delta, get_viewport())
	_check_camera_sanity()
	var cam := get_viewport().get_camera_3d()
	var night: float = WorldScenery.airfield_light_intensity(cam.global_position, _sky.sun_dir_now, _visibility_km) if cam else 0.0
	if _runways and cam:
		_runways.update(cam.global_position, delta)
		_runways.set_light_intensity(night)
	# Exterior lights, the FlightGear bridge / animations / sounds, the hover cursor.
	var pick_cam: Camera3D = active_camera if camera_mode == CameraMode.COCKPIT else null
	var sounds_active: bool = terrain_initialized and not (_fly_loading and not _intro_playing)
	_aircraft.update_systems(jsb_node, delta, cam, pick_cam, night, _sky.time_seconds, sounds_active, _engine_volume_pct())
	if _clouds:
		var env: Environment = _sky.environment
		_clouds.update(cam, _sky.sun_dir_now, _sky.sun_color_now, _sky.sun_energy_now,
			env.ambient_light_color if env else Color(0.55, 0.65, 0.85), _sky.ambient_energy_now,
			env.fog_density if env and env.fog_enabled else 0.0, env.fog_light_color if env else Color(0.7, 0.75, 0.85), delta)
	_hud.set_time_text("Time: " + _sky.get_time_string())

	# Post-Fly loading: the destination settles while the intro zoom plays
	# (and then waits on the spot view); the flight logic below stays paused.
	if _fly_loading:
		_spawn.process_rebase()
		_update_loading_progress()
		if _intro_playing and not _ui_up():
			_advance_intro(delta)
		return

	_update_observer_from_ac()
	_world.refresh_globe_suppress()   # coarse globe hidden under resident tiles
	_feed_vfr_map()
	if _ui_up():
		return   # welcome / settings overlay: flight logic paused
	if _intro_playing:
		_advance_intro(delta)
		return
	# Scenery editor (F8): free camera + tools; the flight model is frozen.
	_scenery_editor.poll(delta)
	if _scenery_editor.is_active():
		return
	# Escape: back to the welcome screen (edge-detected; the editor leaves on
	# the same press and sets _escape_held so the still-held key is ignored).
	if Input.is_key_pressed(KEY_ESCAPE):
		if not _escape_held and _welcome_screen:
			_escape_held = true
			_welcome_screen.set_continue_enabled(not _spawn.is_first_flight())
			_welcome_screen.show_welcome()
			return
	else:
		_escape_held = false

	_hud.poll_keys()
	_aircraft.poll_keys(jsb_node)   # L lights, B speed brakes, Shift+Ctrl+M hotspots
	_spawn.process_rebase()

	# Slew mode (Shift+Y): free movement with the calibrated joystick axes;
	# the cameras keep working, flight controls and readouts are skipped.
	var was_slewing := _slew_mode.is_enabled()
	var slewing := _slew_mode.update(delta, jsb_node,
		JoystickConfig.get_axis("slew_strafe"), JoystickConfig.get_axis("slew_forward"),
		JoystickConfig.get_axis("slew_yaw"), JoystickConfig.get_axis("slew_vertical"),
		_world.terrain_elevation_below(_ac.global_position), JoystickConfig.get_active_device())
	if KeyBindings.just_pressed("camera_flip"):   # Settings > Keyboard
		flip_camera()
	elif KeyBindings.just_pressed("camera_flip_back"):   # Shift+C: the other way round
		flip_camera(true)
	_camera_rig.update(delta, JoystickConfig.get_active_device())
	if slewing:
		return
	# Driving a vehicle: the node follows the car model, JSBSim stays paused.
	if _drive_mode.update(delta, jsb_node, _terrain_elevation.get_rebase_cooldown(),
			JoystickConfig.get_axis("drive_steer"), JoystickConfig.get_axis("drive_accelerate"),
			JoystickConfig.get_axis("drive_brake"), JoystickConfig.get_button("drive_handbrake")):
		return
	if was_slewing:
		_spawn.update_terrain_elevation()   # JSBSim was re-initialised at the new spot
	_poll_flight_joystick()

	_aircraft.update(jsb_node, delta, _engine_volume_pct())
	_spawn.update_terrain_elevation()
	_hud.update(jsb_node)


## Settings > Joystick flight axes into the FDM (docs/MD80_FLIGHTGEAR_PORT.md
## step 10): roll / pitch / yaw, an absolute throttle when it is a lever (a
## trigger keeps the incremental RT / LT), the wheel brakes from the pulled
## half of a centred lever and the "Hold brakes" button. Unbound axes go as NAN.
func _poll_flight_joystick() -> void:
	if jsb_node == null or JoystickConfig.get_active_device() < 0:
		return
	var axis_or_nan := func(action: String, sign: float) -> float:
		return sign * JoystickConfig.get_axis(action) if JoystickConfig.get_bound_axis(action) >= 0 else NAN
	var thr_axis: int = JoystickConfig.get_bound_axis("flight_throttle")
	var thr_absolute: bool = thr_axis >= 0 and thr_axis != JOY_AXIS_TRIGGER_LEFT and thr_axis != JOY_AXIS_TRIGGER_RIGHT
	var brakes: float = NAN
	if JoystickConfig.get_bound_axis("flight_brakes") >= 0:
		brakes = maxf(0.0, -JoystickConfig.get_axis("flight_brakes"))
	jsb_node.apply_joystick(axis_or_nan.call("flight_roll", -1.0), axis_or_nan.call("flight_pitch", 1.0), axis_or_nan.call("flight_yaw", 1.0),
		axis_or_nan.call("flight_throttle", 1.0), thr_absolute, brakes, JoystickConfig.get_button("brakes"))


func _ui_up() -> bool:
	return (_welcome_screen and _welcome_screen.visible) or (_settings_screen and _settings_screen.visible)


func _engine_volume_pct() -> float:
	return float(_settings.get_setting("engine_volume")) if _settings else 70.0


## VFR map: the editor camera in Edit Mode, the AC node while slewing
## (JSBSim's cached pose is frozen then), else the flight model.
func _feed_vfr_map() -> void:
	var ui_up := _ui_up()
	if _edit_session and _scenery_editor.is_active():
		_world.feed_vfr_map(_scenery_editor.get_camera().global_position, fmod(rad_to_deg(_scenery_editor.yaw) + 360.0, 360.0), ui_up)
	elif jsb_node and (_slew_mode.is_enabled() or _drive_mode.is_active()):
		_world.feed_vfr_map(_ac.global_position, WorldScenery.ac_true_heading(_ac), ui_up)
	elif jsb_node:
		_world.feed_vfr_map(jsb_node.get_godot_position(), jsb_node.get_heading(), ui_up)


## Sun position / local time follow the aircraft (ECEF world: no rebase).
func _update_observer_from_ac() -> void:
	var g: Vector3 = WorldScenery.geodetic_of(_ac.global_position)
	_sky.observer_lat = g.x
	_sky.observer_lon = g.y


func _set_observer_lv95(e: float, n: float) -> void:
	var ll: Vector2 = Geo.lv95_to_wgs84(e, n)
	_sky.observer_lat = ll.x
	_sky.observer_lon = ll.y
	_world.mark_suppress_dirty()
	_world.refresh_globe_suppress()


func _aircraft_latlon() -> Vector2:
	var g: Vector3 = WorldScenery.geodetic_of(_ac.global_position)
	return Vector2(g.x, g.y)


## One-shot diagnostic: a camera whose basis collapses (det 0 / NaN) renders
## a black screen and floods the log with "det == 0"; name it and its parent
## chain once so the cause can be found.
func _check_camera_sanity() -> void:
	if _cam_sanity_reported:
		return
	var cam := get_viewport().get_camera_3d()
	if cam == null:
		return
	var b := cam.global_transform.basis
	if b.is_finite() and absf(b.determinant()) >= 1e-9:
		return
	_cam_sanity_reported = true
	var chain := ""
	var n: Node = cam
	while n != null and n is Node3D:
		chain += "%s(det %.3g, scale %s) <- " % [n.name, (n as Node3D).global_transform.basis.determinant(), str((n as Node3D).scale)]
		n = n.get_parent()
	push_error("Camera '%s' has a degenerate basis: %s" % [cam.name, chain])


# Cockpit -> Orbit -> Chase -> Cockpit, or the reverse (the rig handles the
# cycling; the mirrors here serve lever picking).
func flip_camera(reverse: bool = false) -> void:
	if reverse:
		_camera_rig.flip_back()
	else:
		_camera_rig.flip()
	camera_mode = _camera_rig.get_mode()
	active_camera = camera1 if camera_mode == CameraMode.COCKPIT else camera2


# ── Intro zoom ─────────────────────────────────────────────────────────────

## Start the cinematic space -> spot zoom. It plays while the destination is
## still loading (the intro HUD shows the loading status and the zoom waits
## on the spot view until loading is complete). False when it cannot start
## (no globe); the caller then falls back to the loading screen.
func _start_intro_zoom() -> bool:
	if globe_node == null or not is_instance_valid(_intro_camera) or not _intro_zoom.start():
		return false
	_intro_zoom.set_hold_at_end(_fly_loading)
	_intro_playing = true
	active_camera = _intro_camera
	_hud.show_intro_hud(true)
	_hud.reset_intro_progress()
	_hud.set_intro_status(_loading_progress.get_status_text() if _fly_loading else "Loading scenery")
	return true


## Per-frame step of a playing intro; mirrors camera_mode / active_camera
## when the module reports the handoff to the orbit camera.
func _advance_intro(delta: float) -> void:
	var d: float = _intro_camera.global_position.distance_to(_ac.global_position) if is_instance_valid(_intro_camera) else NAN
	_hud.update_intro(d, _world.pending_count(), _fly_loading)
	if _intro_zoom.update(delta) == IntroZoom.STATUS_FINISHED:
		_intro_playing = false
		camera_mode = CameraMode.ORBIT
		active_camera = camera2
		_hud.show_intro_hud(false)


func _cancel_intro_zoom() -> void:
	if not _intro_playing:
		return
	_intro_zoom.cancel()
	_intro_playing = false
	camera_mode = CameraMode.ORBIT
	active_camera = camera2
	if camera2:
		camera2.current = true
	_hud.show_intro_hud(false)


## Post-Fly loading: the terrain ring around the target reports how much has
## streamed in; LoadingProgress blends it with the rebase cooldown into the
## bar + status and decides when the simulation may run.
func _update_loading_progress() -> void:
	var e := 0.0
	var n := 0.0
	var target: Dictionary = _spawn.get_target()
	if target.has("swiss_e"):
		e = float(target["swiss_e"])
		n = float(target["swiss_n"])
	else:
		var g: Vector3 = WorldScenery.geodetic_of(_ac.global_position)
		var lv: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
		e = lv.x
		n = lv.y
	var ring: Vector2i = _world.count_loaded_ring(e, n, 200.0)
	var progress: float = _loading_progress.update(ring.x, ring.y, true, _spawn.is_init_pending(),
		_terrain_elevation.get_rebase_cooldown(), _terrain_elevation.get_rebase_cooldown_frames())
	if _loading_screen:
		_loading_screen.set_progress(progress)
		_loading_screen.set_status(_loading_progress.get_status_text())
	if _intro_playing:
		# one bar per loading phase (scenery tiles, flight model, stabilizing)
		_hud.set_intro_status(_loading_progress.get_status_text())
		_hud.set_intro_bar(_loading_progress.get_phase_progress())
	if not _loading_progress.is_complete():
		return
	# The simulation runs; the intro zoom may now hand over to the spot view.
	_fly_loading = false
	if _loading_screen:
		_loading_screen.hide_loading()
	_intro_zoom.set_hold_at_end(false)
	_hud.set_intro_status("Loading scenery")
	_hud.reset_intro_progress()   # the tile bar starts over for the last phase
	print("Fly loading complete - simulation active")


# ── Welcome screen ─────────────────────────────────────────────────────────

## Fly!: adopt the selected airframe, apply the scenery design, spawn at the
## airport (first flight or teleport) and start the intro zoom as the
## loading view.
func _on_welcome_flight_requested(aircraft_id: String, swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== WELCOME: Fly! %s at E=%.0f N=%.0f heading %.0f ===" % [aircraft_id, swiss_e, swiss_n, heading_deg])
	if not _scenery_available():
		return
	_settings_from_welcome = false
	_edit_session = false
	_ac.visible = true   # hidden during an Edit Mode session
	_aircraft.apply_selected_aircraft(aircraft_id, jsb_node)
	# A ground vehicle: DriveMode takes the node over once the spawn has
	# settled (a train is only placed for now).
	_drive_mode.disengage(jsb_node)
	if _aircraft.is_vehicle() and _aircraft.get_vehicle_kind() == "car":
		_drive_mode.configure(_aircraft.get_vehicle_params())
		_drive_mode.arm(jsb_node)
	# The scenery design (replaced roads, terrain patches) is applied before
	# the first tile of this flight loads: no reload under the aircraft later.
	_scenery_editor.apply_startup_design()
	_fly_loading = true
	_loading_progress.begin_fly_loading()
	_set_observer_lv95(swiss_e, swiss_n)
	_cancel_intro_zoom()   # a Fly! during the previous flight's intro ends that zoom first
	var ride: Camera3D = camera2 if camera_mode == CameraMode.ORBIT else camera1
	# Start conditions from the Aircraft tab (height above the ground, airspeed,
	# heading); 0 / 0 = on the runway, the cfg [start] section gives the defaults
	# (a glider starts airborne until winch and tow exist).
	var cfg = _config_for_aircraft(aircraft_id)
	var agl_ft: float = AircraftSettingsPanel.start_agl_ft(_settings, cfg, aircraft_id)
	var speed_kt: float = AircraftSettingsPanel.start_speed_kt(_settings, cfg, aircraft_id)
	var start_hdg: float = AircraftSettingsPanel.start_heading_deg(_settings, aircraft_id)
	_spawn.set_start(agl_ft * 0.3048, speed_kt, start_hdg)
	if agl_ft > 0.0 and speed_kt > 0.0:
		print("Start: %s airborne %.0f ft above the ground at %.0f kt%s" % [aircraft_id, agl_ft, speed_kt, "" if is_nan(start_hdg) else ", heading %.0f" % start_hdg])
	if _spawn.request_flight(swiss_e, swiss_n, heading_deg, ride):
		active_camera = ride
	# The zoom doubles as the loading view (status + progress on its HUD); the
	# loading screen is only the fallback when it cannot start.
	if not _start_intro_zoom() and _loading_screen:
		_loading_screen.set_progress(0.0)
		_loading_screen.set_status("Loading terrain...")
		_loading_screen.show_loading()


## Welcome "Edit Mode": the scenery editor over the selected airport with no
## aircraft and no intro zoom. The AC node is parked (hidden) at the
## destination so the sun observer and the map have a position, JSBSim is
## kept in slew, and the editor camera drives the tile streamer.
func _on_welcome_edit_requested(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== WELCOME: Edit Mode at E=%.0f N=%.0f ===" % [swiss_e, swiss_n])
	if not _scenery_available():
		if _welcome_screen:
			_welcome_screen.show_welcome()
		return
	_cancel_intro_zoom()
	_drive_mode.disengage(jsb_node)
	_edit_session = true
	_settings_from_welcome = false
	_set_observer_lv95(swiss_e, swiss_n)
	_spawn.park(swiss_e, swiss_n, 500.0, heading_deg)
	_ac.visible = false
	jsb_node.set_slew_mode(true)
	_scenery_editor.apply_startup_design()
	_world.set_streaming(true)
	_scenery_editor.enter_at(swiss_e, swiss_n, heading_deg)


func _on_scenery_editor_exited() -> void:
	if not _edit_session:
		return
	_edit_session = false
	if _welcome_screen:
		# Edit Mode parked the (hidden) aircraft at the edited airport: there is
		# no flight to continue, the next flight starts with Fly!
		_welcome_screen.set_continue_enabled(false)
		_welcome_screen.show_welcome()


## Pre-flight check: without any terrain / orthophoto data on disk the
## loading would stall forever waiting for tiles that never arrive.
func _scenery_available() -> bool:
	var missing: String = _settings.missing_scenery_reason() if _settings else "Settings unavailable - cannot verify scenery configuration."
	if missing == "":
		return true
	var dlg := AcceptDialog.new()
	dlg.title = "Scenery not available"
	dlg.dialog_text = missing
	dlg.dialog_autowrap = true
	dlg.min_size = Vector2(560, 0)
	add_child(dlg)
	dlg.confirmed.connect(dlg.queue_free)
	dlg.canceled.connect(dlg.queue_free)
	dlg.popup_centered()
	push_warning("Fly! aborted: %s" % missing)
	return false


## JSBSim was (re)initialised on the ground: fuel from the Aircraft tab, the
## parking brake set, and the METAR of the (maybe new) region.
func _on_spawned(_first: bool) -> void:
	_apply_fuel_setting()
	_aircraft.set_parking_brake_at_start(jsb_node)
	if _live_weather and _live_weather.enabled:
		_live_weather.refresh(true)


func _on_welcome_settings_requested() -> void:
	_settings_from_welcome = true   # the Aircraft tab follows the page's selection
	if _welcome_screen:
		_welcome_screen.visible = false
	if _settings_screen:
		_settings_screen.show_settings()


## Resume the in-progress flight: flight logic only pauses while the welcome
## screen is visible, so hiding it is enough.
func _on_welcome_continue_requested() -> void:
	_ac.visible = true   # never resume with the aircraft hidden (Edit Mode hides it)
	if _welcome_screen:
		_welcome_screen.hide_welcome()


## First-run wizard: the chosen folder becomes a scan root, the scanner
## builds the library and the result is applied to Scenery3D.
func _on_welcome_scenery_setup_requested(path: String) -> void:
	if not _settings:
		push_warning("Welcome scenery setup: settings screen not ready")
		return
	_settings.add_scan_root(path)
	_settings.scan_scenery()
	_settings.save()   # the next launch skips the wizard
	_apply_scenery_sources()
	print("First-run setup complete: applied scenery library from %s" % path)


func _on_settings_closed() -> void:
	if _settings_screen:
		_settings_screen.hide_settings()
	_apply_tree_density()
	_apply_scenery_sources()
	_vfr_map.apply_config()
	_push_time_of_day_settings()
	_push_weather_settings()
	_world.mark_suppress_dirty()   # scenery areas may have changed
	_world.refresh_globe_suppress()
	if _welcome_screen:
		_welcome_screen.set_continue_enabled(not _spawn.is_first_flight())
		_welcome_screen.visible = true
		_welcome_screen.show_welcome()


func _apply_tree_density() -> void:
	if _settings and is_instance_valid(vegetation_node):
		_settings.apply_tree_density(vegetation_node)


## Scenery data sources from the settings library -> Scenery3D, Vegetation
## and the VFR map (enabled areas in priority order), then the runways.
func _apply_scenery_sources() -> void:
	if not is_instance_valid(scenery3d_node) or not _settings:
		return
	_settings.apply_scenery_sources(scenery3d_node, vegetation_node, _vfr_map)
	_scenery_objects.apply_scenery_areas(_settings.get_enabled_scenery_areas(), scenery3d_node, vegetation_node)
	_rebuild_runways()


## Merge the enabled packs' airports/runways.json (design §12 D16) and
## rebuild the procedural runway meshes.
func _rebuild_runways() -> void:
	if not _settings or not is_instance_valid(_runways):
		return
	_runways_merged = RunwaysLib.load_merged(_settings.get_scenery_areas())
	_runways.set_merged(_runways_merged, Callable(_world, "terrain_elevation_geodetic"))
	print("Runways: %d airports merged from the enabled packs (built by distance)" % _runways_merged.size())


## The bundled catalog with every pack-defined airport's runways replaced by
## the pack data (real thresholds), plus pack airports the catalog lacks.
func _catalog_with_pack_runways() -> Array[Dictionary]:
	var catalog: Array[Dictionary] = AirportsLib.load_default()
	if _settings:
		if _runways_merged.is_empty():
			_runways_merged = RunwaysLib.load_merged(_settings.get_scenery_areas())
		RunwaysLib.merge_into_catalog(catalog, _runways_merged)
	return catalog


# ── Weather / time of day ──────────────────────────────────────────────────

## Static weather (Settings > Weather): the layer stack + QNH go to the
## JSBSim bridge, the visibility to the fog. With Live Weather on, the static
## values apply until the first report arrives.
func _push_weather_settings() -> void:
	if not _settings:
		return
	var live := bool(_settings.get_setting("live_weather"))
	if _live_weather:
		_live_weather.enabled = live
		if not live:
			_settings_screen.clear_live_weather()
	if live and _live_weather and not _live_weather.last_weather.is_empty():
		var wx: Dictionary = _live_weather.last_weather
		_apply_weather(wx["layers"], float(wx["qnh_hpa"]), float(wx["visibility_km"]))
	else:
		_apply_weather(_settings.get_setting("weather_layers"), float(_settings.get_setting("qnh_hpa")), float(_settings.get_setting("visibility_km")))
	if live and _live_weather:
		_live_weather.refresh()


func _apply_weather(layers: Array, qnh_hpa: float, visibility_km: float) -> void:
	_visibility_km = visibility_km
	if jsb_node:
		jsb_node.set_weather(layers, qnh_hpa)
	_sky.set_visibility_km(visibility_km)
	if _clouds:
		if _settings:
			_clouds.quality = int(_settings.get_setting("cloud_quality"))
		_clouds.set_layers(layers)
	_refresh_weather_overlay()


## The Ctrl+I overlay text is built by the HUD from this state.
func _refresh_weather_overlay() -> void:
	var live := _settings != null and bool(_settings.get_setting("live_weather"))
	var wx: Dictionary = _live_weather.last_weather if _live_weather else {}
	_hud.set_weather_state(live, wx,
		_settings.get_setting("weather_layers") if _settings else [],
		float(_settings.get_setting("qnh_hpa")) if _settings else 1013.25,
		float(_settings.get_setting("visibility_km")) if _settings else 40.0)


func _set_weather_overlay(show: bool) -> void:
	_refresh_weather_overlay()
	_hud.set_weather_overlay(show)


func _on_live_weather_updated(wx: Dictionary) -> void:
	_apply_weather(wx["layers"], float(wx["qnh_hpa"]), float(wx["visibility_km"]))
	if _settings_screen:
		_settings_screen.set_live_weather(wx["layers"], str(wx["status"]))


func _on_live_weather_failed(reason: String) -> void:
	push_warning("Live weather: %s (keeping the current weather)" % reason)
	if _settings_screen and _live_weather and _live_weather.last_weather.is_empty():
		_settings_screen.set_live_weather([], "no report (%s)" % reason)


func _push_time_of_day_settings() -> void:
	if _settings:
		_sky.set_time(bool(_settings.get_setting("use_system_time")), int(_settings.get_setting("time_of_day_min")))
	else:
		_sky.set_time(true, 720)


# ── Aircraft ───────────────────────────────────────────────────────────────

## Fuel load from the Aircraft tab (aircraft_<id>_fuel_pct), applied to every
## tank when a flight starts.
func _apply_fuel_setting() -> void:
	if _settings == null or jsb_node == null:
		return
	var key := "aircraft_%s_fuel_pct" % _aircraft_id
	if _settings.has_setting(key):
		var pct: float = float(_settings.get_setting(key))
		AircraftSettingsPanel.apply_fuel(jsb_node, pct)
		print("Fuel: %s tanks set to %.0f %% from the Aircraft settings" % [_aircraft_id, pct])


# ── Cockpit mouse interaction ──────────────────────────────────────────────

## Every camera: the right mouse button held pans the view (cockpit head,
## orbit, chase). Cockpit camera: Space + wheel zooms the view, Space + drag
## looks around, FlightGear switches / knobs take clicks first (a right press
## on a knob turns it and does not start a pan), then the lever picking of
## CockpitControls (mixture, throttle, magneto, flaps).
func _unhandled_input(event: InputEvent) -> void:
	if _fly_loading or _ui_up():
		if _camera_rig.is_mouse_drag_active():
			_camera_rig.set_mouse_drag(false)
		return
	if event is InputEventMouseButton and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_RIGHT and not (event as InputEventMouseButton).pressed:
		if _camera_rig.is_mouse_drag_active():
			_camera_rig.set_mouse_drag(false)
			get_viewport().set_input_as_handled()
			return
	var handled := false
	if camera_mode != CameraMode.COCKPIT or not active_camera:
		if event is InputEventMouseButton:
			var ext_mb := event as InputEventMouseButton
			if ext_mb.button_index == MOUSE_BUTTON_RIGHT and ext_mb.pressed:
				_camera_rig.set_mouse_drag(true)
				handled = true
		elif event is InputEventMouseMotion and _camera_rig.is_mouse_drag_active():
			_camera_rig.mouse_look((event as InputEventMouseMotion).relative)
			handled = true
		if handled:
			get_viewport().set_input_as_handled()
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.pressed and _camera_rig.is_mouse_look_active() and mb.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
			_camera_rig.mouse_zoom(1 if mb.button_index == MOUSE_BUTTON_WHEEL_UP else -1)
			handled = true
		elif _fg_controls.count > 0 and _fg_controls.click(active_camera, mb.position, mb.button_index, mb.pressed, mb.shift_pressed):
			handled = true
		else:
			handled = _cockpit_controls.handle_mouse_button(active_camera, mb.position, mb.button_index, mb.pressed, jsb_node)
		if not handled and mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
			_camera_rig.set_mouse_drag(true)   # right press beside the controls: pan the view
			handled = true
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _camera_rig.is_mouse_look_active() or _camera_rig.is_mouse_drag_active():
			_camera_rig.mouse_look(mm.relative)
			handled = true
		else:
			handled = _cockpit_controls.handle_mouse_motion(mm.position, jsb_node)
	if handled:
		get_viewport().set_input_as_handled()


# ── Settings > Aircraft tab ────────────────────────────────────────────────

## Tell the Aircraft tab (C++ AircraftSettingsPanel) which aircraft is
## selected, which is flown, and how to read a config.
func _wire_aircraft_settings_panel() -> void:
	var panel = _settings_screen.get_aircraft_panel() if _settings_screen else null
	if panel == null:
		return
	panel.aircraft_provider = Callable(self, "_settings_aircraft_id")
	panel.flown_provider = func() -> String: return _aircraft_id if terrain_initialized else ""
	panel.fdm_provider = func(): return jsb_node
	panel.config_provider = Callable(self, "_config_for_aircraft")
	panel.name_provider = func(id: String) -> String: return WelcomeScreen.aircraft_label(id)
	# a livery picked for the aircraft being flown changes it now
	panel.livery_changed.connect(_on_livery_changed)


func _on_livery_changed(id: String, livery_id: String) -> void:
	if terrain_initialized and id == _aircraft_id:
		_aircraft.set_livery(livery_id)


## The aircraft the Aircraft tab shows: the welcome page's selection when the
## settings were opened from that page or while it is up, else the flown one.
func _settings_aircraft_id() -> String:
	if _welcome_screen and (_welcome_screen.visible or _settings_from_welcome) and _welcome_screen.has_method("get_selected_aircraft_id"):
		return _welcome_screen.get_selected_aircraft_id()
	return _aircraft_id


func _config_for_aircraft(id: String):
	if id == _aircraft_id and _aircraft_cfg != null:
		return _aircraft_cfg
	var path: String = AircraftConfig.resolve_path(id)
	return AircraftConfig.load_for_aircraft(id) if FileAccess.file_exists(path) else null
