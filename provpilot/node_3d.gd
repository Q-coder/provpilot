extends Node3D

const AirportsLib = preload("res://airports_lib.gd")
const AircraftConfig = preload("res://aircraft_config.gd")

# Default airframe to load when nothing else specifies one.
const DEFAULT_AIRCRAFT_ID := "c172p"

# Per-airframe definition (aircraft.cfg), loaded at the start of _ready() so
# model paths, control-surface node paths and tunables come from data rather
# than being hardcoded here. See aircraft_config.gd.
var _aircraft_cfg: AircraftConfig
var _aircraft_id: String = DEFAULT_AIRCRAFT_ID
# Aircraft id whose visual model (normals flip + surface/lever nodes) is
# currently applied. Guards against re-flipping the same already-correct model,
# which would double-flip normals and light the airframe from below.
var _model_visuals_id: String = DEFAULT_AIRCRAFT_ID
# Scene-tree path (from this node) to the loaded model's root node. Populated
# from aircraft.cfg [model] root_node; falls back to the historical C172P path.
var _model_root_path: String = "AC/Node3D2/C172P_1"

# Terrain coordinate system (Swiss LV95)
# These define the relationship between Godot world coords and Swiss coordinates.
# MUST match Scenery3D's origin_east / origin_north (read from
# ProjectSettings in _ready), otherwise the HUD, teleport, and JSBSim will
# disagree with the tiles that Scenery3D actually places. Scenery3D's origin
# is fixed at startup, so there is no runtime floating-origin rebase.
# Home point in LV95 (Schaffhausen LSPF area): where the aircraft is parked
# before the first flight and the initial sky observer. NOT a frame origin —
# the Godot world is ECEF (design §9.5); this mirrors Scenery3D's origin_east/
# north only so its flat-mode data grid and the home point agree.
var TERRAIN_CENTER_E: float = 2682880.0
var TERRAIN_CENTER_N: float = 1283072.0

# User edit. Get the new organized labels.
@onready var airspeed_label: Label = $Control/BottomRow/Label
@onready var altitude_label: Label = $Control/BottomRow/Label2
@onready var vspeed_label: Label = $Control/BottomRow/Label3
@onready var throttle_label: Label = $Control/BottomRow/Label4
@onready var heading_label: Label = $Control/BottomRow/Label5
@onready var time_label: Label = $Control/TimeLabel
@onready var _vfr_map: VfrMap = $Control/VfrMap

# Assuming your JSBGodot node is a child of the current node
var jsb_node: Node
var active_camera: Camera3D
var camera1: Camera3D
var camera2: Camera3D
var _camera1_scene_transform: Transform3D  # Pristine Camera1 transform from the scene
var _camera1_scene_fov: float = 0.0        # Pristine Camera1 fov from the scene

# Camera mode enum (values mirror the C++ CameraRig::Mode). Kept here because
# lever raycasts and the intro handoff branch on it; kept in sync with the rig
# by flip_camera() and when IntroZoom reports its orbit handoff.
enum CameraMode { COCKPIT, ORBIT, CHASE }
var camera_mode: int = CameraMode.COCKPIT

# Cinematic space→spot intro zoom (world3d globe). When entering fly mode we
# start high above the Earth (whole globe visible) and smoothly zoom down to the
# external "spot" view of the aircraft, then hand off to the normal orbit camera.
# The animation state + math live in the C++ IntroZoom module
# (src/intro_zoom.cpp); _intro_playing is the GDScript-side mirror used by
# _process and the smoke test.
var _intro_playing: bool = false
# Dedicated camera for the intro, parented to the ROOT (not the aircraft) so its
# space position is not dragged around by JSBSim's per-frame AC transform writes.
var _intro_camera: Camera3D = null
# Intro-zoom HUD (replaces the flight HUD while the cinematic plays):
# distance to the aircraft, a tile-loading bar and the skip hint.
var _intro_hud: CanvasLayer = null
var _intro_dist_label: Label = null
var _intro_bar_bg: ColorRect = null
var _intro_status_label: Label = null   # loading status while the destination loads, else "Loading scenery"
var _intro_bar: ColorRect = null
var _intro_pending_prev := -1   # pending count last frame (-1 = none yet)
var _intro_done := 0            # completed tile/chunk loads since the intro started
var _intro_shown := 0.0         # displayed bar fraction (monotonic)

# Spot/orbit and chase camera state (angles, distances, smoothing) lives in the
# C++ CameraRig module (src/camera_rig.cpp); the C++ IntroZoom handoff reads
# the orbit offset directly from it via orbit_offset().

# Control surfaces, propeller and landing-gear animation live in the C++
# ControlSurfaces module (src/control_surfaces.cpp). Per-aircraft tunables are
# pushed from aircraft.cfg in _load_aircraft_config; model nodes are registered
# (with their base transforms captured) in _resolve_aircraft_nodes /
# _resolve_gear_legs. The smoothed visual RPM is read back via get_visual_rpm().
var _control_surfaces := ControlSurfaces.new()

# Cockpit/orbit/chase camera behaviour lives in the C++ CameraRig module
# (src/camera_rig.cpp). GDScript registers the scene cameras + AC root, re-
# captures the cockpit base transform whenever aircraft.cfg [camera] overrides
# are applied, and drives per-frame updates from _process. The "flip_camera"
# action press is detected here (remappable project action) and forwarded to
# the rig; camera_mode/active_camera stay in sync for lever raycasts.
var _camera_rig := CameraRig.new()

# Interactive cockpit levers/switches (mixture, throttle, magneto, flaps, gear)
# live in the C++ CockpitControls module (src/cockpit_controls.cpp). GDScript
# resolves the lever nodes + collision bodies from aircraft.cfg [levers],
# registers them (base transforms captured at registration), pushes the cfg
# flap detents / gear flag, and drives per-frame updates from _process. Mouse
# dragging stays here in _unhandled_input and calls into the module.
var _cockpit_controls := CockpitControls.new()

# Free-movement slew mode (Shift+Y) lives in the C++ SlewMode module
# (src/slew_mode.cpp). GDScript registers the session-stable AC root + HUD
# labels in _ready, pushes the Swiss origin once TERRAIN_CENTER is locked,
# and drives per-frame updates from _process with the calibrated joystick
# axes (JoystickConfig) and the terrain height under the aircraft.
var _slew_mode := SlewMode.new()
# Scenery design / developer mode (F8, scenery_editor.gd, docs/SCENERY_EDITOR.md):
# free camera + terrain cursor + road / rail tools on a saved SceneryDesign.
var _scenery_editor := SceneryEditor.new()
var _edit_session := false   # Edit Mode from the welcome screen (no flight)

# Cinematic space→spot intro zoom (world3d globe) lives in the C++ IntroZoom
# module (src/intro_zoom.cpp). GDScript keeps the scene-side gates (globe
# mounted, AC + dedicated intro camera valid), registers the session-stable
# nodes in _setup_world3d_globe, and drives per-frame updates from _process.
# The handoff to the orbit camera is done by the module through CameraRig; the
# camera_mode/active_camera mirrors are refreshed here when update() reports
# STATUS_FINISHED.
var _intro_zoom := IntroZoom.new()

# Day/night sun + atmospheric sky (sky_atmosphere.gd). Installs the
# atmospheric-scattering sky shader so the horizon follows the world3d globe's
# Earth curvature at every altitude, swings the scene sun/shadows through the
# day from the simulated time of day, and applies the altitude fog/ambient
# ramps. (The former C++ Atmosphere module was removed in step C3.)
var _sky := preload("res://sky_atmosphere.gd").new()

# Engine-sound pitch/volume mapping (RPM -> AudioStreamPlayer pitch_scale +
# volume_db) lives in the C++ EngineSound module (src/engine_sound.cpp).
# GDScript owns the looping AudioStreamPlayer node, pushes the per-aircraft
# tunables from aircraft.cfg ([sound] ranges + [propeller] max_rpm), registers
# the node in _setup_engine_sound, and calls update() from _process with the
# ControlSurfaces smoothed RPM + the user engine-volume setting.
var _engine_sound := EngineSound.new()

# Post-teleport terrain handling (rebase cooldown + per-frame JSBSim ground
# elevation) lives in the C++ TerrainElevation module (src/terrain_elevation.cpp).
# GDScript keeps the scenery-stack terrain query, the terrain_initialized gate,
# node registration, and pushes the config it still owns locally.
var _terrain_elevation := TerrainElevation.new()

# Loading-screen progress blending + completion state machine live in the C++
# LoadingProgress module (src/loading_progress.cpp). GDScript keeps the scenery
# ring sampling (it reports loaded/total) and drives the loading-screen UI.
var _loading_progress := LoadingProgress.new()

# Rudder indicator pivot tweak (debug drag on screen, not part of the animator)
var rudder_pivot_offset: Vector3 = Vector3(0, 0, 0)

# Propeller blur-disc creation parameters (the disc mesh itself is built here
# in GDScript; spin/blur/cone animation is done by ControlSurfaces). The
# max_rpm used to normalise engine sound lives in the C++ EngineSound module.
var PROPELLER_DISC_RADIUS: float = 0.95  # Radius of blur disc in meters (cfg propeller.disc_radius_m)
var PROPELLER_SPIN_AXIS: Vector3 = Vector3.BACK  # Local axis the prop spins about (cfg propeller.spin_axis: x/y/z)
var PROPELLER_DISC_OFFSET: Vector3 = Vector3.ZERO  # Disc translation tweak in parent space (cfg propeller.disc_offset)

# Retractable landing gear (cfg [gear]). The legs themselves are registered
# with the C++ ControlSurfaces module in _resolve_gear_legs(); it rotates each
# leg's wheel + strut RIGIDLY about a shared local pivot (in the model wrapper
# frame) so the whole leg sweeps up as one unit, or drives an imported
# retract/extend clip by seeking instead.
var _gear_retractable: bool = false
# The commanded gear state + G-key debounce live in the C++ CockpitControls
# module (its reset() mirrors JSBSim's gear-cmd back to fully down).
# Optional imported gear retract/extend clip name (cfg [gear] animation).
var _gear_anim_name: String = ""

# Scenery3D reference for ground height queries
var scenery3d_node: Node
# Vegetation node reference
var vegetation_node: Node

# ── World3D global globe (replaces Scenery3D local patch) ──────────────────
# When true, the whole-planet world3d globe provides terrain (elevation +
# visuals) instead of the flat scenery3d LV95 tile patch. The globe renders in
# ECEF; we mount it into ProvPilot's existing flat frame via an inverse tangent
# transform on GlobeMount, so the aircraft/JSBSim/cameras keep working in the
# flat local frame unchanged while the curved Earth appears around them.
const USE_WORLD3D_GLOBE := true
var globe_node: Node = null            # W3DGlobe (under GlobeMount)
var _globe_mount: Node3D = null        # carries inverse-ECEF transform
var _w3d_coords = null                 # W3DCoords instance for conversions
# The world is WGS84 ECEF (design §9.5, step C): the globe sits at the
# origin, Scenery3D places tiles on the ellipsoid (frame_mode 1), the
# aircraft node carries an ECEF position + body-to-ECEF basis. LV95 E/N is
# only a data/grid coordinate now (spawn targets, HUD, VFR map, coverage
# mask); see _ecef_of_lv95 / _geodetic_of.
# Coarse-globe suppression (design §8.1 iteration 4 / §9 D8): the globe is
# hidden PER FRAGMENT wherever Scenery3D geometry is resident. The tile
# manager reports a per-tile coverage mask (one byte per 1 km tile around
# the camera); it is uploaded verbatim as a mask texture to W3DGlobe,
# whose chunk shader discards fragments in covered cells. No region extents,
# no margin, no rectangle merging: the globe ends exactly at the resident
# far-chunk boundary and stays visible as the fallback in unloaded / no-data
# cells (sea, gaps), so a hole to space cannot exist.
var _suppress_last_center: Vector2 = Vector2.ZERO
var _suppress_dirty := true
# Last tile-manager coverage generation the pushed mask was built from —
# refresh only when it changes (a tile/chunk appeared or disappeared).
var _suppress_cov_gen := -1
# Minimum interval between mask rebuilds (each is a ~180 KB grid + texture
# upload; during heavy streaming the coverage generation bumps every frame).
const SUPPRESS_REFRESH_MS := 100
var _suppress_last_ms := 0

# Tree density mapping (tree_density index -> density fraction / load radius)
# lives in the C++ SettingsStore (apply_tree_density).

# Teleport rebase cooldown (pin the AC until JSBSim stabilises) + per-frame
# JSBSim ground-elevation push live in the C++ TerrainElevation module
# (src/terrain_elevation.cpp). GDScript still gates them on terrain_initialized,
# supplies the scenery-stack ground-height query, and reads the cooldown for the
# loading-progress bar.

# Position dialog (Shift+P)
var _position_dialog: Window = null
var _position_key_held: bool = false

# Loading / Welcome / Settings screens
var _loading_screen: CanvasLayer = null
var _loading_done: bool = false
var _fly_loading: bool = false   # True while loading after user clicks Fly!
# Fly-loading timeout safety-net state + tunables live in the C++ LoadingProgress
# module (src/loading_progress.cpp); begin_fly_loading() arms the safety net.
var _welcome_screen: CanvasLayer = null
# Settings UI (C++ SettingsScreen, src/settings_screen.cpp) and its value
# store (C++ SettingsStore, src/settings_store.cpp) — the store also owns the
# scenery-library logic and pushes paths/density into Scenery3D + Vegetation.
var _settings_screen: SettingsScreen = null
var _settings: SettingsStore = null
const LiveWeatherScript := preload("res://live_weather.gd")
const CloudLayersScript := preload("res://clouds.gd")
var _clouds: Node3D = null  # CloudLayers (clouds.gd)
const RunwaysScript := preload("res://runways.gd")
const RunwaysLibScript := preload("res://runways_lib.gd")
var _runways: Node3D = null  # Runways (runways.gd): per-pack runway meshes, design §12
var _aircraft_lights: AircraftLights = null  # exterior lights of the flown aircraft (aircraft_lights.gd)
var _fg_animator: FGAnimator = null          # FlightGear model animations (docs/A10_FLIGHTGEAR_PORT.md)
var _fg_bridge = preload("res://fg_bridge.gd").new()
var _fg_controls = preload("res://fg_controls.gd").new()   # clickable FlightGear cockpit controls
const WelcomeScreenScript = preload("res://welcome_screen.gd")
const AircraftSettingsPanelScript = preload("res://aircraft_settings_panel.gd")
var _fg_seeded: bool = false
var _fg_sounds: FGSounds = null     # FlightGear sound set (aircraft.cfg [sound] fg_sounds)
var _parking_brake: bool = false
var _landing_key_held: bool = false
var _speedbrake_key_held: bool = false
var _hotspot_key_held: bool = false
var _hud_key_held: bool = false
var _hud_mode: int = 0   # H cycles: 0 none, 1 bottom readouts, 2 + upper info (coordinates, time)
var _speedbrake_out: bool = false
var _runways_merged: Dictionary = {}  # icao -> merged pack airport entry
var _live_weather: Node = null  # LiveWeather (live_weather.gd)

# Deferred scenery loading: skip all heavy loading at startup, load on first Fly!
var _first_flight: bool = true
var _first_flight_target: Dictionary = {}  # {swiss_e, swiss_n, heading_deg}

# ── Interactive cockpit mixture lever ──────────────────────────────────────
# Value, base transform, key input + JSBSim writes live in CockpitControls.
var mixture_lever_node: Node3D           # The lever mesh node
var mixture_lever_body: StaticBody3D     # Runtime physics body for raycasting
var _mixture_dragging: bool = false      # True while mouse is dragging the lever
var _mixture_drag_start_y: float = 0.0   # Screen Y at drag start
var _mixture_drag_start_value: float = 0.0  # Mixture value at drag start
const MIXTURE_LEVER_COLLISION_LAYER: int = 16  # Physics layer for lever picking

# ── Interactive cockpit throttle lever ─────────────────────────────────────
# Throttle value lives in JSBSim; base transform + key input live in
# CockpitControls (it syncs the lever visual from the FDM every frame).
var throttle_lever_node: Node3D
var throttle_lever_body: StaticBody3D
var _throttle_dragging: bool = false
var _throttle_drag_start_y: float = 0.0
var _throttle_drag_start_value: float = 0.0
const THROTTLE_LEVER_COLLISION_LAYER: int = 32  # Separate physics layer

# ── Interactive cockpit magneto switch ─────────────────────────────────────
# Position, base transform + key input live in CockpitControls.
var magneto_switch_node: Node3D            # The switch mesh node
var magneto_switch_body: StaticBody3D      # Runtime physics body for raycasting
var _magneto_dragging: bool = false
var _magneto_drag_start_x: float = 0.0    # Screen X at drag start
var _magneto_drag_start_pos: int = 3       # Position at drag start
const MAGNETO_SWITCH_COLLISION_LAYER: int = 64  # Separate physics layer

# ── Interactive cockpit flap lever ─────────────────────────────────────────
# Position, base transform, detent table + key input live in CockpitControls.
var flap_lever_node: Node3D              # The lever mesh node
var flap_lever_body: StaticBody3D        # Runtime physics body for raycasting
var _flap_dragging: bool = false
var _flap_drag_start_y: float = 0.0      # Screen Y at drag start
var _flap_drag_start_pos: int = 0        # Position at drag start
const FLAP_LEVER_COLLISION_LAYER: int = 128  # Separate physics layer

# ── Engine sound ───────────────────────────────────────────────────────────
var _engine_audio: AudioStreamPlayer = null
var ENGINE_SOUND_PATH: String = "res://sounds/c172_engine_inflight.ogg"  # cfg sound.engine
# The RPM->pitch/volume mapping + low-RPM fade threshold live in the C++
# EngineSound module (tunables pushed from aircraft.cfg [sound] / [propeller]).

# FPS counter
var _fps_label: Label = null
var _fps_visible: bool = false
var _fps_key_held: bool = false
# Ctrl+I: weather overlay (top centre) — METAR station + wind / QNH / clouds,
# or the static layers when live weather is off.
var _wx_label: Label = null
var _wx_visible: bool = false
var _wx_key_held: bool = false
const WEATHER_CLOUD_NAMES := ["", "Cumulus", "Stratus", "Stratocumulus", "Cumulonimbus", "Altostratus", "Altocumulus", "Cirrus"]
const WEATHER_TURB_NAMES := ["", "light turb", "moderate turb", "severe turb"]

# Per-second resource/leak diagnostic (helps track down post-multi-jump stalls)
var _diag_last_msec: int = 0
var _diag_jump_count: int = 0

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	print("Node3D _ready() starting...")

	# Create FPS counter label (top-right corner)
	var fps_layer := CanvasLayer.new()
	fps_layer.layer = 100
	add_child(fps_layer)
	_apply_hud_mode()
	_fps_label = Label.new()
	_fps_label.text = "FPS: 0"
	_fps_label.add_theme_font_size_override("font_size", 18)
	_fps_label.add_theme_color_override("font_color", Color.WHITE)
	_fps_label.add_theme_color_override("font_shadow_color", Color.BLACK)
	_fps_label.add_theme_constant_override("shadow_offset_x", 1)
	_fps_label.add_theme_constant_override("shadow_offset_y", 1)
	_fps_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_fps_label.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_fps_label.offset_left = -120
	_fps_label.offset_right = -10
	_fps_label.offset_top = 10
	_fps_label.visible = false
	fps_layer.add_child(_fps_label)
	_wx_label = Label.new()
	_wx_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_wx_label.add_theme_font_size_override("font_size", 20)
	_wx_label.add_theme_color_override("font_color", Color(0.95, 0.97, 1.0))
	_outline_label(_wx_label, 4)
	_wx_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_wx_label.offset_left = -500
	_wx_label.offset_right = 500
	_wx_label.offset_top = 12
	_wx_label.visible = false
	fps_layer.add_child(_wx_label)

	# Show a lightweight loading screen immediately, before any heavy work.
	_create_loading_screen()

	# Load the per-airframe definition (aircraft.cfg) before resolving any
	# model nodes, so paths/tunables come from data instead of hardcoded here.
	_load_aircraft_config()

	# Get JSBGodot node after we're in the scene tree
	jsb_node = $AC/JSBGodot
	# JSBSim's ground model queries the scenery under EACH gear contact point
	# (with the slope normal) through this callable — see JSBGodot::set_ground_query.
	if jsb_node and jsb_node.has_method("set_ground_query"):
		jsb_node.set_ground_query(Callable(self, "_ground_height_geodetic"))
	print("JSBGodot node: ", jsb_node)
	
	# Access the AC node and its cameras
	var ac_node = $AC
	print("AC node: ", ac_node)
	
	# DEBUG: Check transforms of intermediate nodes for Y offsets
	print("=== AIRCRAFT HIERARCHY TRANSFORMS ===")
	print("  AC.position: ", ac_node.position)
	var node3d2 = ac_node.get_node_or_null("Node3D2")
	if node3d2:
		print("  Node3D2.position: ", node3d2.position)
		print("  Node3D2.global_position: ", node3d2.global_position)
	var c172p = ac_node.get_node_or_null("Node3D2/C172P_1")
	if c172p:
		print("  C172P_1.position: ", c172p.position)
		print("  C172P_1.global_position: ", c172p.global_position)
	
	camera1 = ac_node.get_node("Camera1")
	camera2 = ac_node.get_node("Camera2")
	active_camera = camera1
	_camera1_scene_transform = camera1.transform  # Pristine scene value for swaps
	_camera1_scene_fov = camera1.fov
	print("Camera1: ", camera1)
	print("Camera2: ", camera2)

	# The C172P .blend import bakes normals with the opposite handedness from
	# Godot's runtime convention, so without this the sun lights the underside
	# of the wings instead of the top. Flip them in-place once at startup.
	# Controlled by aircraft.cfg [model] flip_normals / flip_normals_prefix.
	_apply_model_normals(ac_node)

	# Register the scene cameras with the C++ camera rig (it captures the
	# cockpit base transform; _apply_camera_config re-captures it after
	# applying any aircraft.cfg [camera] overrides).
	_camera_rig.register_cameras(camera1, camera2, ac_node)
	add_child(_scenery_editor)
	_scenery_editor.setup(self)

	# Position the cockpit camera per the selected aircraft (aircraft.cfg [camera]).
	_apply_camera_config()
	
	# Set Camera1 as the active one initially
	active_camera.current = true
	camera2.current = false
	
	# Resolve all model-dependent nodes (control surfaces + cockpit levers).
	# Extracted into a method so a runtime aircraft swap (welcome-screen
	# selection) can re-resolve them against the freshly-loaded GLB.
	_resolve_aircraft_nodes()

	# Get Scenery3D node for ground height queries
	scenery3d_node = get_node_or_null("Scenery3D")
	if scenery3d_node:
		print("Scenery3D node found: ", scenery3d_node)
	else:
		print("WARNING: Scenery3D node not found - ground collision may not work correctly!")

	# Get Vegetation node — density is applied later, after the settings
	# screen has been constructed (it loads saved settings from disk in its
	# own _ready, so we can read the user's choice from it).
	vegetation_node = get_node_or_null("Vegetation")
	if vegetation_node:
		print("Vegetation node found: ", vegetation_node)

	# Hold all scenery streaming OFF until the user clicks Fly! and the
	# aircraft teleports to the chosen airport. Without this, the tile/
	# building/water/road/vegetation managers would start streaming around
	# the origin (0,0) while the welcome screen is up, then thrash the
	# worker queue with thousands of now-irrelevant tiles when the AC
	# teleports far away — causing a long stall before terrain is ready
	# at the destination. Re-enabled in _do_first_flight().
	_set_scenery_streaming(false)

	# Lock TERRAIN_CENTER to Scenery3D's own origin (from ProjectSettings).
	# Swiss <-> Godot conversions and tile placement must share the same origin.
	if ProjectSettings.has_setting("scenery3d/origin_east"):
		TERRAIN_CENTER_E = float(ProjectSettings.get_setting("scenery3d/origin_east"))
	if ProjectSettings.has_setting("scenery3d/origin_north"):
		TERRAIN_CENTER_N = float(ProjectSettings.get_setting("scenery3d/origin_north"))
	print("TERRAIN_CENTER locked to Scenery3D origin: E=%d N=%d" % [int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N)])

	# Slew mode (C++ module): register the session-stable AC root + HUD labels.
	_slew_mode.register_nodes($AC, airspeed_label, altitude_label, vspeed_label, throttle_label, heading_label, $Control/Label6, $Control/Label7, $Control/Label8)
	_slew_mode.set_pm_label($Control/Label9)
	_outline_hud_labels($Control)

	# VFR map frame (C++ module): one-time wiring with the portable config
	# path. Tile paths arrive via _apply_scenery_sources().
	_vfr_map.setup(InstallRoot.get_config_path())

	# Terrain elevation (C++ module): register the session-stable AC root and push
	# the fallback timeout GDScript still owns locally (it also gates the deferred
	# init handshake). The other tunables use the module's built-in defaults.
	_terrain_elevation.register_nodes($AC)
	_terrain_elevation.set_terrain_init_fallback_msec(TERRAIN_INIT_FALLBACK_MSEC)

	# Mount the world3d global globe at the LV95 origin. Must run after
	# TERRAIN_CENTER_E/N are final so the tangent frame matches the flat frame.
	_setup_world3d_globe()
	# Park the aircraft (and its cameras) on the ellipsoid at the LV95 origin
	# until the first flight: the scene file's transform is a flat-frame
	# position, i.e. a point near the Earth's centre in the ECEF world, and
	# every geodetic conversion of it would be degenerate.
	var ac0 := $AC as Node3D
	if ac0:
		ac0.global_transform = _ac_transform_at(TERRAIN_CENTER_E, TERRAIN_CENTER_N, 600.0, 0.0, ac0.scale)

	# Print the aircraft model hierarchy to find control surfaces
	print("=== Aircraft Node Hierarchy ===")
	print_node_tree($AC, 0)
	print("=== End Hierarchy ===")

	# Skip the loading screen — show welcome screen directly.
	_loading_done = true
	if _loading_screen:
		_loading_screen.hide_loading()
	_create_settings_screen()
	_create_welcome_screen()
	# Day/night sun + atmospheric sky (needs the globe mounted above and the
	# settings screen loaded for the time-of-day config).
	_setup_sky_atmosphere()
	# Apply saved tree density now that _settings_screen has loaded its config.
	_apply_tree_density()
	# Apply scenery sources (orthophoto and terrain paths) from settings
	_apply_scenery_sources()
	# Hide the coarse globe under resident detailed scenery (coverage mask);
	# the per-frame push in _process follows the streamer.
	_refresh_globe_suppress_rects()

	# ── Engine sound ──────────────────────────────────────────────────
	_setup_engine_sound()

	print("Node3D _ready() complete — welcome screen shown (scenery deferred)")



# ── Engine Sound Setup ─────────────────────────────────────────────────────
func _setup_engine_sound() -> void:
	var stream = load(ENGINE_SOUND_PATH)
	if stream == null:
		print("WARNING: Engine sound not found at %s" % ENGINE_SOUND_PATH)
		return
	# Enable looping for whichever format we loaded
	if stream is AudioStreamOggVorbis:
		stream.loop = true
	elif stream is AudioStreamMP3:
		stream.loop = true
	_engine_audio = AudioStreamPlayer.new()
	_engine_audio.stream = stream
	_engine_audio.bus = "Master"
	_engine_audio.volume_db = -80.0  # Start silent
	add_child(_engine_audio)
	_engine_audio.play()
	# Hand the player to the C++ module so update() can drive pitch/volume.
	_engine_sound.register_nodes(_engine_audio)
	print("Engine sound loaded and playing (looped)")


func _update_engine_sound() -> void:
	if _engine_audio == null:
		return

	# The RPM->pitch/volume math lives in the C++ EngineSound module. We keep
	# only the scene/UI concerns here: the null gate, reading the ControlSurfaces
	# smoothed RPM (handles engine-off decay), and the user engine-volume setting.
	var user_vol_pct: float = 70.0  # default
	if _settings:
		user_vol_pct = float(_settings.get_setting("engine_volume"))
	# an aircraft with a FlightGear sound set plays its own engines
	if _fg_sounds != null and _fg_sounds.count > 0:
		user_vol_pct = 0.0
	_engine_sound.update(_control_surfaces.get_visual_rpm(), user_vol_pct)


# Create the propeller blur disc mesh (as a sibling of the propeller) and hand
# it to the C++ animator, which fades it in/out by RPM.
func _create_propeller_blur_disc(prop_node: Node3D) -> void:
	# Use StandardMaterial3D - shaders have transparency issues
	var blur_mat = StandardMaterial3D.new()
	blur_mat.albedo_color = Color(0.15, 0.15, 0.18, 0.1)  # Dark gray, more transparent
	blur_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	blur_mat.cull_mode = BaseMaterial3D.CULL_DISABLED  # Visible from both sides
	blur_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED  # No lighting
	
	# Get the propeller mesh's bounding box to determine disc size
	if prop_node is MeshInstance3D:
		var mesh_inst = prop_node as MeshInstance3D
		var aabb = mesh_inst.get_aabb()
		var mesh_center = aabb.position + aabb.size / 2.0
		
		# Disc radius: prefer the configured value; otherwise derive it from the
		# two mesh extents PERPENDICULAR to the spin axis (the swept disc plane).
		var prop_radius := PROPELLER_DISC_RADIUS
		if prop_radius <= 0.0:
			var extents := Vector3(absf(aabb.size.x), absf(aabb.size.y), absf(aabb.size.z))
			var spin_abs := Vector3(absf(PROPELLER_SPIN_AXIS.x), absf(PROPELLER_SPIN_AXIS.y), absf(PROPELLER_SPIN_AXIS.z))
			# Zero out the spin-axis extent, take the largest remaining half-extent.
			var plane := extents - extents * spin_abs
			prop_radius = maxf(plane.x, maxf(plane.y, plane.z)) / 2.0
		print("Propeller disc radius: ", prop_radius, " m")
		
		# Create a circular disc using CylinderMesh (very thin cylinder = disc)
		var disc_mesh = CylinderMesh.new()
		disc_mesh.top_radius = prop_radius
		disc_mesh.bottom_radius = prop_radius
		disc_mesh.height = 0.02  # Very thin
		disc_mesh.radial_segments = 32  # Smooth circle
		disc_mesh.rings = 1
		
		# Create MeshInstance3D for the blur disc
		var blur_disc := MeshInstance3D.new()
		blur_disc.mesh = disc_mesh
		blur_disc.material_override = blur_mat
		blur_disc.visible = false  # Start hidden
		
		# Add as sibling to propeller's parent
		var prop_parent = prop_node.get_parent()
		if prop_parent:
			prop_parent.add_child(blur_disc)
			# The propeller pivot may sit at a blade tip (PC-7), so the mesh
			# center is offset from the node origin. Map the local mesh center
			# into the parent frame, then apply the cfg tuning offset.
			blur_disc.transform = Transform3D(Basis(), prop_node.transform * mesh_center + PROPELLER_DISC_OFFSET)
			# Orient the cylinder (long axis = local Y) so its flat faces are
			# perpendicular to the spin axis (the disc lies in the swept plane).
			var spin_abs := Vector3(absf(PROPELLER_SPIN_AXIS.x), absf(PROPELLER_SPIN_AXIS.y), absf(PROPELLER_SPIN_AXIS.z))
			if spin_abs.x > 0.5:
				blur_disc.rotation_degrees = Vector3(0, 0, 90)  # Y -> X
			elif spin_abs.z > 0.5:
				blur_disc.rotation_degrees = Vector3(90, 0, 0)  # Y -> Z
			# spin axis Y => no rotation needed
			print("Blur disc created with radius: ", prop_radius)
			# The C++ animator owns the disc's visibility + shader rpm parameter.
			_control_surfaces.register_propeller_blur_disc(blur_disc, blur_mat)
	else:
		print("WARNING: Propeller is not a MeshInstance3D!")



# Deferred terrain initialization - waits for first physics frame
var _pending_terrain_init: bool = false
var _terrain_init_print_timer: int = 0
var _pending_terrain_init_start_msec: int = 0
# If terrain elevation never streams in within this window, give up waiting
# and initialise JSBSim at a fallback elevation so the user isn't stranded
# on the loading screen.
const TERRAIN_INIT_FALLBACK_MSEC: int = 12000   # idle-streamer window before the globe fallback
const TERRAIN_INIT_HARD_CAP_MSEC: int = 90000   # absolute cap on waiting for the precise tile
var _terrain_init_last_busy_msec: int = 0

# Initialize JSBSim with correct terrain elevation at startup
# This must be called BEFORE JSBSim runs its physics, setting terrain during IC
func _initialize_jsbsim_on_terrain() -> void:
	# Store for deferred initialization in first physics frame
	_pending_terrain_init = true
	_pending_terrain_init_start_msec = Time.get_ticks_msec()
	_terrain_init_last_busy_msec = 0
	print("=== Terrain initialization queued for first physics frame ===")


## Recursively traverses node and re-bakes any MeshInstance3D meshes with
## inverted vertex normals. The C172P .blend import uses the opposite
## handedness from Godot's runtime lighting, so its top surfaces are dark
## under a downward-pointing sun without this fix.
##
## We only flip exterior surfaces (Cessna_Exterior_*). The interior cockpit
## meshes — panel, gauges, levers, switches — already ship with correct
## normals; flipping them backface-culls them when viewed from inside the
## cabin, making the dashboard appear shifted/transparent. Likewise we skip
## the Dynamic_Switch_* subtree (re-rigged interactive cockpit controls).
# Flips mesh normals on the currently-loaded model so the sun lights the top
# of the airframe instead of the underside. Driven by aircraft.cfg [model]:
#   flip_normals        (bool)  — whether to flip at all
#   flip_normals_prefix (str)   — only flip MeshInstance3D whose name starts
#                                 with this prefix; empty "" = flip every mesh.
# Re-runnable so a runtime model swap re-applies it to the fresh GLB.
func _apply_model_normals(root: Node) -> void:
	if _aircraft_cfg != null and not _aircraft_cfg.get_bool("model", "flip_normals", true):
		return
	var prefix := "Cessna_Exterior_"
	if _aircraft_cfg != null:
		prefix = _aircraft_cfg.get_string("model", "flip_normals_prefix", "Cessna_Exterior_")
	_flip_normals_recursive(root, prefix)


func _flip_normals_recursive(node: Node, prefix: String) -> void:
	if node.name.begins_with("Dynamic_Switch_"):
		return
	if node is MeshInstance3D and (prefix.is_empty() or node.name.begins_with(prefix)):
		var mi: MeshInstance3D = node
		var mesh: Mesh = mi.mesh
		if mesh:
			var new_mesh := ArrayMesh.new()
			for s in range(mesh.get_surface_count()):
				var arrays: Array = mesh.surface_get_arrays(s)
				var normals = arrays[Mesh.ARRAY_NORMAL]
				if normals != null:
					var flipped := PackedVector3Array()
					flipped.resize(normals.size())
					for i in range(normals.size()):
						flipped[i] = -normals[i]
					arrays[Mesh.ARRAY_NORMAL] = flipped
				new_mesh.add_surface_from_arrays(mesh.surface_get_primitive_type(s), arrays)
				var mat = mesh.surface_get_material(s)
				if mat:
					new_mesh.surface_set_material(s, mat)
			mi.mesh = new_mesh
	for child in node.get_children():
		_flip_normals_recursive(child, prefix)


## Query terrain elevation at a world XZ position via Scenery3D.
## Returns NAN when Scenery3D has no data at the location (tile not loaded).
## Callers that need a numeric fallback should check is_nan() explicitly —
## feeding 0.0 to JSBSim as ground elevation causes catastrophic ground impact
## spikes when the real tile (hundreds of metres ASL) finally loads.
## Query terrain elevation at a WGS84 point (degrees).
## Prefers the precise scenery3d patch (Swiss/BW/...); falls back to the
## coarse whole-planet world3d globe so the aircraft always has ground beneath
## it beyond the detailed patches. Returns NAN only if neither has data.
## Ground height for JSBSim's contact points: scenery terrain plus the lift
## of a drawn runway / taxiway / apron surface there. NAN when no terrain
## data (the bridge then keeps its flat fallback).
func _ground_height_geodetic(lat: float, lon: float) -> float:
	var h: float = _scenery_elevation_geodetic(lat, lon)
	if is_nan(h):
		return NAN
	if _runways:
		h += _runways.surface_lift_at(W3DCoords.geodetic_to_ecef(lat, lon, h))
	return h


func _terrain_elevation_geodetic(lat: float, lon: float) -> float:
	var h: float = _scenery_elevation_geodetic(lat, lon)
	if not is_nan(h):
		return h
	return _globe_elevation_geodetic(lat, lon)


## Same, for a world (ECEF) position.
func _terrain_elevation_below(world_pos: Vector3) -> float:
	var g := _geodetic_of(world_pos)
	return _terrain_elevation_geodetic(g.x, g.y)


## Precise scenery3d elevation (NAN if no node / method / data / tile not loaded).
func _scenery_elevation_geodetic(lat: float, lon: float) -> float:
	if not is_instance_valid(scenery3d_node):
		return NAN
	if not scenery3d_node.has_method("get_elevation_geodetic"):
		return NAN
	var h: float = scenery3d_node.get_elevation_geodetic(lat, lon)
	# Swiss/BW terrain is never at exactly 0 m ASL; treat 0.0 as "no data".
	if h == 0.0 or is_nan(h):
		return NAN
	return h


## Coarse whole-planet globe elevation (NAN if the globe is unavailable).
func _globe_elevation_geodetic(lat: float, lon: float) -> float:
	if not (USE_WORLD3D_GLOBE and is_instance_valid(globe_node)):
		return NAN
	var gh: float = globe_node.get_elevation(lat, lon)
	if is_nan(gh):
		return NAN
	return gh


## ── ECEF helpers (design §9.5) ──────────────────────────────────────────
## (lat, lon, h) of a world position.
func _geodetic_of(world_pos: Vector3) -> Vector3:
	return W3DCoords.ecef_to_geodetic(world_pos)


## World (ECEF) position of an LV95 point at height h (metres, ellipsoidal = ASL).
func _ecef_of_lv95(e: float, n: float, h: float) -> Vector3:
	var ll: Vector2 = Geo.lv95_to_wgs84(e, n)
	return W3DCoords.geodetic_to_ecef(ll.x, ll.y, h)


## Level aircraft transform at an LV95 point: nose (+Z local) along a TRUE
## heading, up = ellipsoid normal, the node's scale preserved.
func _ac_transform_at(e: float, n: float, h: float, true_heading_deg: float, scale: Vector3) -> Transform3D:
	var ll: Vector2 = Geo.lv95_to_wgs84(e, n)
	var wun: Transform3D = W3DCoords.enu_to_ecef_transform(ll.x, ll.y, h, true)
	var yaw := Basis(Vector3.UP, -deg_to_rad(true_heading_deg))
	var b: Basis = wun.basis * yaw
	b = Basis(b.x * scale.x, b.y * scale.y, b.z * scale.z)
	return Transform3D(b, wun.origin)


## TRUE heading (deg) of the aircraft node from its basis (nose = local +Z).
func _ac_true_heading(ac: Node3D) -> float:
	var g := _geodetic_of(ac.global_position)
	var wun: Transform3D = W3DCoords.enu_to_ecef_transform(g.x, g.y, 0.0, true)
	var f: Vector3 = wun.basis.transposed() * ac.global_transform.basis.z.normalized()
	return fposmod(rad_to_deg(atan2(-f.x, f.z)), 360.0)


## Mount the world3d global globe: the world IS ECEF, so the mount is the
## identity and the coverage mask is looked up in LV95 grid metres by the
## globe shader (mask frame 1: fragment ECEF -> LV95).
func _setup_world3d_globe() -> void:
	if not USE_WORLD3D_GLOBE:
		return
	_globe_mount = get_node_or_null("GlobeMount")
	globe_node = get_node_or_null("GlobeMount/Globe")
	if not _globe_mount or not is_instance_valid(globe_node):
		push_warning("World3D: GlobeMount/Globe nodes missing — keeping Scenery3D terrain")
		return
	if not ClassDB.class_exists("W3DCoords"):
		push_warning("World3D: W3DCoords not available — extension not loaded?")
		globe_node = null
		return
	_w3d_coords = ClassDB.instantiate("W3DCoords")
	_globe_mount.transform = Transform3D.IDENTITY
	if globe_node.has_method("set_coverage_mask_frame"):
		globe_node.set_coverage_mask_frame(1)
	# Cameras must reach the global horizon (default far was 200 km).
	if camera1:
		camera1.far = 12000000.0
	if camera2:
		camera2.far = 12000000.0
	# Dedicated root-level camera for the cinematic intro (independent of AC).
	if _intro_camera == null:
		_intro_camera = Camera3D.new()
		_intro_camera.name = "IntroCamera"
		_intro_camera.far = 4.0e7
		_intro_camera.fov = 70.0
		add_child(_intro_camera)
	# Register the cinematic intro zoom nodes (session-stable; the dedicated
	# intro camera was created just above).
	_intro_zoom.register_nodes(get_node_or_null("AC"), _intro_camera, camera1, camera2, _camera_rig)
	print("World3D globe mounted at the ECEF origin (world frame = ECEF)")


## Keep the sky's observer (sun position, local time) in sync with where
## the aircraft is. Cheap; the sky script also derives its own from the camera.
func _update_observer_from_ac() -> void:
	var ac := $AC as Node3D
	if not ac:
		return
	var g := _geodetic_of(ac.global_position)
	_sky.observer_lat = g.x
	_sky.observer_lon = g.y


## Upload Scenery3D's live coverage mask to the globe as its suppression mask
## (design §8.1 iteration 4). The tile manager reports one byte per tile
## around the camera: covered when a loaded far-zone chunk spans it or the
## tile itself has a mesh. The globe's chunk shader
## discards fragments in covered cells, so suppression is exact per fragment
## regardless of globe chunk size (a max-level chunk is 130-170 km wide;
## containment culling could never fit one inside a 220 km region). The
## mask is TILE resolution (1 km): at a region's data edge a resident tile
## next to a no-data tile is still masked, while the no-data tile shows the
## globe (an 8 km chunk-cell mask left such edge cells unmasked). Runs
## every frame from _process; skips work while nothing moved and no tile or
## chunk appeared or disappeared (coverage generation unchanged).
func _refresh_globe_suppress_rects() -> void:
	if not (USE_WORLD3D_GLOBE and is_instance_valid(globe_node) and globe_node.has_method("set_coverage_mask")):
		return
	# Same centre Scenery3D streams around (its update_tiles reads the
	# viewport's current camera); fall back to the AC when none is active.
	var c := Vector2.ZERO
	var cam := get_viewport().get_camera_3d() if is_inside_tree() else null
	var p3 := Vector3.ZERO
	if cam:
		p3 = cam.global_position
	else:
		var ac := $AC as Node3D
		if not ac:
			return
		p3 = ac.global_position
	# 2D proxy of the 3D position for the "moved ≥ 32 m" check.
	c = Vector2(p3.x + p3.y, p3.z - p3.y)

	var tm = _get_tile_manager()
	var gen := -1
	if tm != null:
		gen = int(tm.get_coverage_generation())
	if not _suppress_dirty and gen == _suppress_cov_gen \
			and _suppress_last_center.distance_squared_to(c) < 1024.0:
		return  # nothing moved, no tile/chunk appeared or disappeared
	var now_ms := Time.get_ticks_msec()
	if not _suppress_dirty and now_ms - _suppress_last_ms < SUPPRESS_REFRESH_MS:
		return  # throttle: the globe stays visible as fallback in the meantime
	_suppress_last_ms = now_ms

	if tm != null:
		_push_suppress_from_coverage(tm.get_coverage_mask(), int(tm.get_coverage_mask_radius()),
				int(tm.get_cam_tile_ei()), int(tm.get_cam_tile_ni()), float(tm.get_tile_size()))
	else:
		globe_node.clear_coverage_mask()
	_suppress_cov_gen = gen
	_suppress_last_center = c
	_suppress_dirty = false


## Hand a tile-manager coverage mask to the globe (see
## _refresh_globe_suppress_rects). Kept separate so the smoke test can feed a
## synthetic mask without data files. cells is (2*r_tiles+1)² bytes,
## row-major (row = north tile, column = east tile), centred on tile
## (cam_ei, cam_ni); each cell spans tile_m metres. The mask plane is LV95
## grid metres (mask frame 1): origin = the SW corner of the grid, step
## +tile_m along E and N; the globe shader projects each fragment to LV95.
func _push_suppress_from_coverage(cells: PackedByteArray, r_tiles: int,
		cam_ei: int, cam_ni: int, tile_m: float) -> void:
	var n := 2 * r_tiles + 1
	if cells.size() != n * n or tile_m <= 0.0:
		globe_node.clear_coverage_mask()
		return
	var e0 := float(cam_ei - r_tiles) * tile_m
	var n0 := float(cam_ni - r_tiles) * tile_m
	globe_node.set_coverage_mask(cells, n, e0, n0, tile_m, tile_m)


## VFR map drive: ECEF position -> LV95, true heading -> LV95 grid heading
## (the map is drawn in the grid frame).
func _feed_vfr_map(world_pos: Vector3, true_heading_deg: float, ui_up: bool) -> void:
	# JSBSim's cached position is (0,0,0) until the first initialisation —
	# not a place on the planet; leave the map where it was.
	if not world_pos.is_finite() or world_pos.length() < 1.0e6:
		return
	var g := _geodetic_of(world_pos)
	var lv: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
	var grid_hdg := fposmod(true_heading_deg + Geo.lv95_grid_convergence_deg(g.x, g.y), 360.0)
	_vfr_map.update_lv95(lv.x, lv.y, grid_hdg, ui_up)


## The Scenery3D tile manager (source of the live coverage grid), or null when
## Scenery3D is absent or its extension is not built.
func _get_tile_manager():
	if is_instance_valid(scenery3d_node) and scenery3d_node.has_method("get_tile_manager"):
		return scenery3d_node.get_tile_manager()
	return null


## Set up the day/night sun + atmospheric sky. Registers the scene Environment,
## the Sun/Fill DirectionalLight3Ds and the coarse globe with sky_atmosphere.gd
## (which installs the atmospheric-scattering sky shader), and pushes the
## current time-of-day settings. Uses the LV95 origin as the observer location.
func _setup_sky_atmosphere() -> void:
	var we := get_node_or_null("Environment/WorldEnvironment")
	var env: Environment = we.environment if we else null
	var sun := get_node_or_null("Environment/Sun") as DirectionalLight3D
	var fill := get_node_or_null("Environment/Fill") as DirectionalLight3D
	var ll: Vector2 = Geo.lv95_to_wgs84(TERRAIN_CENTER_E, TERRAIN_CENTER_N)
	_sky.setup(env, sun, fill, globe_node, ll.x, ll.y)
	_push_time_of_day_settings()
	_push_weather_settings()


## Push the time-of-day settings (system clock vs manual local time) into the
## sky controller. Called at setup and whenever the settings screen closes.
## Static weather (Settings > Weather): the layer stack + QNH go to the JSBSim
## bridge (sampled at the aircraft's altitude each tick), visibility to the fog.
## With Live Weather on, the static values apply until the first report
## arrives (LiveWeather then calls _on_live_weather_updated).
func _push_weather_settings() -> void:
	if not _settings:
		return
	var live := bool(_settings.get_setting("live_weather"))
	if _live_weather:
		_live_weather.enabled = live
		if not live:
			_settings_screen.clear_live_weather()
	if live and _live_weather and not _live_weather.last_weather.is_empty():
		_apply_weather(_live_weather.last_weather["layers"], float(_live_weather.last_weather["qnh_hpa"]), float(_live_weather.last_weather["visibility_km"]))
	else:
		_apply_weather(_settings.get_setting("weather_layers"), float(_settings.get_setting("qnh_hpa")), float(_settings.get_setting("visibility_km")))
	if live and _live_weather:
		_live_weather.refresh()
	_refresh_weather_overlay()


var _visibility_km := 40.0   # last applied visibility (airfield lights come on below 1 km)


func _apply_weather(layers: Array, qnh_hpa: float, visibility_km: float) -> void:
	_visibility_km = visibility_km
	if jsb_node and jsb_node.has_method("set_weather"):
		jsb_node.set_weather(layers, qnh_hpa)
	_sky.set_visibility_km(visibility_km)
	if _clouds:
		if _settings:
			_clouds.quality = int(_settings.get_setting("cloud_quality"))
		_clouds.set_layers(layers)


var _cam_sanity_reported := false
## One-shot diagnostic: a camera whose basis collapses (det 0 / NaN) renders
## a black screen and floods the log with "det == 0" from the light culler;
## name the camera and its parent chain once so the cause can be found.
func _check_camera_sanity() -> void:
	if _cam_sanity_reported:
		return
	var cam := get_viewport().get_camera_3d()
	if cam == null:
		return
	var b := cam.global_transform.basis
	if not b.is_finite() or absf(b.determinant()) < 1e-9:
		_cam_sanity_reported = true
		var chain := ""
		var n: Node = cam
		while n != null and n is Node3D:
			chain += "%s(det %.3g, scale %s) <- " % [n.name, (n as Node3D).global_transform.basis.determinant(), str((n as Node3D).scale)]
			n = n.get_parent()
		push_error("Camera '%s' has a degenerate basis: %s" % [cam.name, chain])


func _set_weather_overlay(show: bool) -> void:
	_wx_visible = show
	if _wx_label:
		_wx_label.visible = show
		_refresh_weather_overlay()


func _refresh_weather_overlay() -> void:
	if _wx_label == null or not _wx_visible:
		return
	_wx_label.text = _weather_overlay_text()


## Text for the Ctrl+I overlay: the METAR (station, wind, QNH, visibility,
## cloud layers, raw report) when live weather has one, else the static
## layer stack from the settings.
func _weather_overlay_text() -> String:
	var live := _settings != null and bool(_settings.get_setting("live_weather"))
	var lines: PackedStringArray = []
	if live and _live_weather and not _live_weather.last_weather.is_empty():
		var wx: Dictionary = _live_weather.last_weather
		lines.append("METAR %s %s   %s" % [wx.get("station", "?"), wx.get("when", ""), wx.get("name", "")])
		lines.append("Wind %s   QNH %.0f hPa   Visibility %.0f km" % [wx.get("wind_txt", ""), float(wx["qnh_hpa"]), float(wx["visibility_km"])])
		var clouds: PackedStringArray = []
		for l in wx["layers"]:
			if int(l["coverage_oktas"]) > 0:
				clouds.append("%d/8 %s at %d m" % [int(l["coverage_oktas"]), WEATHER_CLOUD_NAMES[int(l["cloud_type"])], int(l["base_m"])])
		lines.append("Clouds: " + (", ".join(clouds) if not clouds.is_empty() else "sky clear"))
		if str(wx.get("raw", "")) != "":
			lines.append(str(wx["raw"]))
		return "\n".join(lines)
	if live:
		lines.append("Live weather: waiting for a METAR…")
	else:
		lines.append("Static weather (Settings > Weather)")
	if _settings:
		lines.append("QNH %.0f hPa   Visibility %.0f km" % [float(_settings.get_setting("qnh_hpa")), float(_settings.get_setting("visibility_km"))])
		for l in _settings.get_setting("weather_layers"):
			var parts: PackedStringArray = ["%d–%d m: wind %03d°/%d kt" % [int(l["base_m"]), int(l["top_m"]), int(l["wind_dir_deg"]), int(l["wind_speed_kts"])]]
			if int(l["turbulence"]) > 0:
				parts.append(WEATHER_TURB_NAMES[int(l["turbulence"])])
			if int(l["coverage_oktas"]) > 0:
				parts.append("%d/8 %s" % [int(l["coverage_oktas"]), WEATHER_CLOUD_NAMES[int(l["cloud_type"])]])
			lines.append(", ".join(parts))
	return "\n".join(lines)


func _aircraft_latlon() -> Vector2:
	var ac := get_node_or_null("AC") as Node3D
	if ac == null:
		return Vector2.ZERO
	var g := _geodetic_of(ac.global_position)
	return Vector2(g.x, g.y)


func _on_live_weather_updated(wx: Dictionary) -> void:
	_apply_weather(wx["layers"], float(wx["qnh_hpa"]), float(wx["visibility_km"]))
	if _settings_screen:
		_settings_screen.set_live_weather(wx["layers"], str(wx["status"]))
	_refresh_weather_overlay()


func _on_live_weather_failed(reason: String) -> void:
	push_warning("Live weather: %s (keeping the current weather)" % reason)
	if _settings_screen and _live_weather and _live_weather.last_weather.is_empty():
		_settings_screen.set_live_weather([], "no report (%s)" % reason)


func _push_time_of_day_settings() -> void:
	var use_system := true
	var minutes := 720
	if _settings:
		use_system = bool(_settings.get_setting("use_system_time"))
		minutes = int(_settings.get_setting("time_of_day_min"))
	_sky.set_time(use_system, minutes)


## Begin the cinematic space→spot zoom (called when fly-loading completes).
## Animates the dedicated intro camera from high above the Earth down to the
## normal orbit "spot" view of the aircraft, then hands off to orbit mode. The
## animation state + math live in the C++ IntroZoom module; this keeps the
## scene-side gates (globe mounted, AC + dedicated intro camera valid) and the
## active_camera mirror.
## Start the cinematic space → spot zoom. It plays while the destination is
## still loading (_fly_loading): the intro HUD shows the loading status and
## the zoom waits on the spot view until loading is complete. Returns false
## when it cannot start (no globe); the caller then falls back to the loading
## screen.
func _start_intro_zoom() -> bool:
	if not USE_WORLD3D_GLOBE or not is_instance_valid(globe_node) or not _globe_mount:
		return false
	var ac_node := get_node_or_null("AC")
	if not ac_node or not is_instance_valid(_intro_camera):
		return false
	if not _intro_zoom.start():
		return false
	_intro_zoom.set_hold_at_end(_fly_loading)
	_intro_playing = true
	active_camera = _intro_camera
	_intro_pending_prev = -1
	_intro_done = 0
	_intro_shown = 0.0
	_show_intro_hud(true)
	if _intro_status_label:
		_intro_status_label.text = _loading_progress.get_status_text() if _fly_loading else "Loading scenery"
	_set_intro_bar(0.0)
	return true


## Per-frame step of a playing intro; mirrors camera_mode / active_camera when
## the C++ module reports the handoff to the orbit camera.
func _advance_intro(delta: float) -> void:
	_update_intro_hud()
	if _intro_zoom.update(delta) == IntroZoom.STATUS_FINISHED:
		_intro_playing = false
		camera_mode = CameraMode.ORBIT
		active_camera = camera2
		_show_intro_hud(false)


## Build (once) and show/hide the intro HUD; the flight HUD is hidden while
## the intro plays because its readouts mean nothing from space.
func _show_intro_hud(show: bool) -> void:
	if _intro_hud == null:
		_intro_hud = CanvasLayer.new()
		_intro_hud.name = "IntroHud"
		_intro_hud.layer = 5
		add_child(_intro_hud)
		var box := VBoxContainer.new()
		box.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM)
		box.offset_top = -150.0
		box.offset_bottom = -40.0
		box.offset_left = -320.0
		box.offset_right = 320.0
		box.alignment = BoxContainer.ALIGNMENT_END
		box.add_theme_constant_override("separation", 12)
		_intro_hud.add_child(box)
		_intro_dist_label = Label.new()
		_intro_dist_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		_intro_dist_label.add_theme_font_size_override("font_size", 24)
		_intro_dist_label.add_theme_color_override("font_color", Color(0.35, 0.9, 1.0))
		_outline_label(_intro_dist_label)
		box.add_child(_intro_dist_label)
		_intro_status_label = Label.new()
		_intro_status_label.text = "Loading scenery"
		_intro_status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		_intro_status_label.add_theme_font_size_override("font_size", 18)
		_intro_status_label.add_theme_color_override("font_color", Color(0.85, 0.9, 0.95))
		_outline_label(_intro_status_label)
		box.add_child(_intro_status_label)
		_intro_bar_bg = ColorRect.new()
		_intro_bar_bg.color = Color(0.12, 0.14, 0.18, 0.85)
		_intro_bar_bg.custom_minimum_size = Vector2(600, 12)
		box.add_child(_intro_bar_bg)
		_intro_bar = ColorRect.new()
		_intro_bar.color = Color(0.35, 0.9, 1.0)
		_intro_bar.custom_minimum_size = Vector2(0, 12)
		_intro_bar_bg.add_child(_intro_bar)
		var hint := Label.new()
		hint.text = "Press Space to skip animation"
		hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		hint.add_theme_font_size_override("font_size", 28)
		hint.add_theme_color_override("font_color", Color(0.95, 0.95, 0.9))
		_outline_label(hint)
		box.add_child(hint)
	_intro_hud.visible = show
	var hud := get_node_or_null("Control")
	if hud:
		hud.visible = not show


## Per-frame intro HUD: camera distance to the aircraft and how much of the
## tile manager's work since the intro started is done (monotonic display).


## Black outline so HUD text stays readable over bright or similar-coloured
## terrain and sky.
func _outline_label(l: Label, px: int = 4) -> void:
	l.add_theme_constant_override("outline_size", px)
	l.add_theme_color_override("font_outline_color", Color(0.0, 0.0, 0.0, 0.85))


## Apply the outline to every Label under the flight HUD root.
func _outline_hud_labels(node: Node) -> void:
	if node is Label:
		_outline_label(node as Label, 3)
	for c in node.get_children():
		_outline_hud_labels(c)
func _update_intro_hud() -> void:
	if _intro_hud == null or not _intro_hud.visible:
		return
	var ac := get_node_or_null("AC") as Node3D
	if ac and is_instance_valid(_intro_camera):
		var d: float = _intro_camera.global_position.distance_to(ac.global_position)
		if d >= 10000.0:
			_intro_dist_label.text = "Distance to aircraft: %.0f km" % (d / 1000.0)
		else:
			_intro_dist_label.text = "Distance to aircraft: %.0f m" % d
	if _fly_loading:
		return   # the bar shows the loading phases (_update_loading_progress)
	var tm = _get_tile_manager()
	if tm != null and tm.has_method("get_pending_count"):
		# Completed work = decreases of the pending count; new requests (the
		# descending camera keeps asking for finer rings) only add to the
		# denominator. The shown fraction never goes back, it just pauses.
		var pending: int = tm.get_pending_count()
		if _intro_pending_prev >= 0 and pending < _intro_pending_prev:
			_intro_done += _intro_pending_prev - pending
		_intro_pending_prev = pending
		var total: int = _intro_done + pending
		# nothing queued and nothing done yet: the streamer has not started, not "all done"
		var frac: float = (1.0 if _intro_done > 0 else 0.0) if total == 0 else float(_intro_done) / float(total)
		_intro_shown = maxf(_intro_shown, clampf(frac, 0.0, 1.0))
		_set_intro_bar(_intro_shown)


func _set_intro_bar(frac: float) -> void:
	if _intro_bar == null or _intro_bar_bg == null:
		return
	_intro_bar.custom_minimum_size.x = _intro_bar_bg.size.x * clampf(frac, 0.0, 1.0)
	_intro_bar.size.x = _intro_bar.custom_minimum_size.x




# Called from _process on first frame to do actual terrain positioning
func _do_deferred_terrain_init() -> void:
	if not _pending_terrain_init:
		return
	_pending_terrain_init = false

	var ac_node = get_node_or_null("AC")
	if not ac_node or not jsb_node:
		print("Cannot initialize JSBSim on terrain - missing AC or JSBGodot node")
		return

	# Use the position/heading already set by _do_first_flight on the AC node.
	# JSBSim's _physics_process may try to overwrite ac_node.global_position
	# from its own (default) IC before we get here, so re-apply the target.
	var target_e: float
	var target_n: float
	var target_heading: float
	if _first_flight_target.has("swiss_e"):
		target_e = _first_flight_target["swiss_e"]
		target_n = _first_flight_target["swiss_n"]
		target_heading = _first_flight_target["heading_deg"]
	else:
		push_error("_do_deferred_terrain_init: _first_flight_target empty — using current AC position as fallback")
		var g0 := _geodetic_of(ac_node.global_position)
		var lv0: Vector2 = Geo.wgs84_to_lv95(g0.x, g0.y)
		target_e = lv0.x
		target_n = lv0.y
		target_heading = _ac_true_heading(ac_node)
	var target_ll: Vector2 = Geo.lv95_to_wgs84(target_e, target_n)

	# Print the target only once per second while we wait for terrain to
	# stream in — otherwise this fires 60×/sec and floods the log.
	_terrain_init_print_timer -= 1
	if _terrain_init_print_timer <= 0:
		_terrain_init_print_timer = 60
		print("=== _do_deferred_terrain_init target: E=%.0f N=%.0f hdg=%.1f° (dict_keys=%s) ===" % [
			target_e, target_n, target_heading, str(_first_flight_target.keys())])
		# Diagnostic: which Camera3D is the viewport actually streaming around?
		var vp := get_viewport()
		var active_cam: Camera3D = vp.get_camera_3d() if vp else null
		var ac_pos = ac_node.global_position
		var cam1_cur := (camera1 != null and camera1.current)
		var cam2_cur := (camera2 != null and camera2.current)
		if active_cam != null:
			var cp = active_cam.global_position
			print("    [cam diag] active=%s path=%s pos=(%.1f, %.1f, %.1f) cam1.current=%s cam2.current=%s AC.pos=(%.1f, %.1f, %.1f)" % [
				active_cam.name, str(active_cam.get_path()),
				cp.x, cp.y, cp.z, str(cam1_cur), str(cam2_cur),
				ac_pos.x, ac_pos.y, ac_pos.z])
		else:
			print("    [cam diag] active=<null> cam1.current=%s cam2.current=%s AC.pos=(%.1f, %.1f, %.1f)" % [
				str(cam1_cur), str(cam2_cur), ac_pos.x, ac_pos.y, ac_pos.z])

	# Spawn on the PRECISE scenery3d patch when available: wait for the Swiss/BW
	# tile to stream in rather than initialising on the coarse globe and then
	# jumping ~100+ m when the fine tile loads.
	var api_height: float = _scenery_elevation_geodetic(target_ll.x, target_ll.y)
	if is_nan(api_height):
		var now := Time.get_ticks_msec()
		if _scenery_streamer_busy() or _terrain_init_last_busy_msec == 0:
			_terrain_init_last_busy_msec = now
		var waited := now - _pending_terrain_init_start_msec
		# Wait while tiles are still streaming (the fallback window counts from
		# the last busy moment), never longer than the hard cap.
		if now - _terrain_init_last_busy_msec < TERRAIN_INIT_FALLBACK_MSEC and waited < TERRAIN_INIT_HARD_CAP_MSEC:
			_pending_terrain_init = true
			return
		# No detailed patch here (outside CH/BW, or partial install): fall back to
		# the coarse global globe so we can still spawn instead of hanging.
		api_height = _globe_elevation_geodetic(target_ll.x, target_ll.y)
		if is_nan(api_height):
			api_height = 0.0
		push_warning("Scenery patch elevation unavailable at E=%.0f N=%.0f after %.0fs — spawning on globe/fallback elevation %.0f m ASL" % [target_e, target_n, waited / 1000.0, api_height])
	else:
		print("=== Scenery3D elevation at E=%.0f N=%.0f = %.1f m ===" % [target_e, target_n, api_height])

	# Place the aircraft on the terrain (ECEF pose, level, true heading).
	# JSBSim will take over position control after initialization.
	# Spawn just a hair above terrain so JSBSim's gear springs barely have to settle.
	var gear_height = 0.5
	var new_y = api_height + gear_height
	print("=== Initial aircraft height: %.1f m ASL (terrain: %.1f m) ===" % [new_y, api_height])
	ac_node.global_transform = _ac_transform_at(target_e, target_n, new_y, target_heading, ac_node.scale)

	# For JSBSim we need real ASL elevation — Scenery3D Y is already ASL. The Godot
	# Y -> JSBSim ASL offset is owned by the C++ TerrainElevation module (used for
	# both this initial setup and the per-frame ground-elevation push).
	_terrain_elevation.set_elevation_offset(0.0)
	var jsbsim_terrain_elevation = api_height + _terrain_elevation.get_elevation_offset()

	# JSBSim reports CG altitude. The C172P CG sits ~1.32 m above the wheel
	# contact points (CG z=36.5" vs main gear z=-15.5" in c172p.xml). The
	# visual model's origin is also near CG, so without this compensation the
	# wheels render ~1.3 m above terrain when JSBSim has them on the ground.
	const WHEEL_GROUND_CLEARANCE: float = -1.1
	jsb_node.set_godot_terrain_y_offset(WHEEL_GROUND_CLEARANCE)

	var start_godot_pos = ac_node.global_position
	jsb_node.initialize_at_terrain(jsbsim_terrain_elevation, start_godot_pos, target_heading)
	_apply_fuel_setting()
	_set_parking_brake_at_start()
	print("=== JSBSim initialized at Godot pos: ", start_godot_pos, " heading: ", target_heading, "° ===")

	terrain_initialized = true
	print("=== JSBSim terrain initialization complete ===")
	if _live_weather and _live_weather.enabled:
		_live_weather.refresh(true)  # new flight, maybe a new region


# Update terrain elevation for JSBSim ground collision (per-frame, after init)
# This updates the terrain elevation as the aircraft moves over varying terrain
var terrain_debug_printed: bool = false
var terrain_initialized: bool = false  # Set during the deferred init handshake
# (terrain_init_frame_count, TERRAIN_INIT_DELAY_FRAMES, jsbsim_elevation_offset
# and _rebase_retry_start_msec moved to the C++ TerrainElevation module.)

func _process_rebase_cooldown() -> void:
	## Handle post-teleport position pinning and JSBSim release. The state machine
	## + math live in the C++ TerrainElevation module; GDScript gates on
	## terrain_initialized and supplies the scenery-stack ground-height query at
	## the pinned position (NAN when no data there).
	if not terrain_initialized:
		return
	var pin := _terrain_elevation.get_teleport_pin_pos()
	var g := _geodetic_of(pin)
	_terrain_elevation.process_rebase(jsb_node, _scenery_elevation_geodetic(g.x, g.y), _globe_elevation_geodetic(g.x, g.y), is_instance_valid(scenery3d_node), _scenery_streamer_busy())


## True while the Scenery3D tile streamer still has pending tile work.
func _scenery_streamer_busy() -> bool:
	var tm = _get_tile_manager()
	if tm != null and tm.has_method("get_pending_count"):
		return int(tm.get_pending_count()) > 0
	return false

func _update_terrain_elevation() -> void:
	## Push the terrain elevation under the aircraft to JSBSim's ground-collision
	## model. The per-frame counter + math live in the C++ TerrainElevation module;
	## GDScript gates on terrain_initialized and supplies the scenery-stack query
	## (NAN when no data there) + whether the precise Scenery3D node is alive.
	if not terrain_initialized:
		return
	var ac = get_node_or_null("AC")
	if ac == null:
		return
	# JSBSim's ground is the DRAWN surface: the runway / taxiway / apron
	# meshes sit a few centimetres above the flattened terrain (design
	# §12.5), so add that lift wherever the aircraft is on a pavement.
	var ground_h: float = _terrain_elevation_below(ac.global_position)
	if _runways and not is_nan(ground_h):
		ground_h += _runways.surface_lift_at(ac.global_position)
	_terrain_elevation.update_terrain_elevation(jsb_node, ground_h, is_instance_valid(scenery3d_node))


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	# Do deferred terrain initialization if pending (needs physics world ready)
	if _pending_terrain_init:
		_do_deferred_terrain_init()

	# Day/night sun + atmospheric sky (sky_atmosphere.gd): swings the sun and
	# shadows through the simulated day, drives the atmospheric-scattering sky
	# shader so the horizon matches the globe's curvature at altitude, fades fog
	# toward space, and gates the coarse globe under the detailed patch.
	_sky.update(delta, get_viewport())
	_check_camera_sanity()
	if _runways:
		var rcam := get_viewport().get_camera_3d()
		if rcam:
			_runways.update(rcam.global_position, delta)
			_runways.set_light_intensity(_airfield_light_intensity(rcam.global_position))
	if jsb_node:
		var gear_down: bool = (not _gear_retractable) or jsb_node.get_gear_pos() > 0.05
		var airborne: bool = not jsb_node.get_weight_on_wheels()
		var lcam := get_viewport().get_camera_3d()
		var night: float = _airfield_light_intensity(lcam.global_position) if lcam else 0.0
		if _aircraft_lights and _aircraft_lights.fixture_count() > 0:
			var acb: Basis = $AC.global_transform.basis
			_aircraft_lights.update(delta, airborne, gear_down, night,
				acb.z.normalized(), -acb.y.normalized())   # AC frame: nose along +Z
		if _fg_animator and _fg_animator.animation_count() > 0:
			var landing_on: bool = _aircraft_lights != null and _aircraft_lights.landing_lights_on() and gear_down
			_fg_bridge.landing_light = landing_on
			_fg_bridge.taxi_light = landing_on
			# "internal" follows the camera that is rendering (the intro zoom and the
			# external cameras are outside even while camera_mode says cockpit)
			var rendering_cam: Camera3D = get_viewport().get_camera_3d()
			_fg_bridge.cockpit_view = rendering_cam == camera1
			_fg_bridge.sim_time_seconds = _sky._time_seconds
			_sync_parking_brake()
			_fg_bridge.control_state = _fg_controls.state
			_fg_bridge.advance_canopy(delta)
			var fg_props: Dictionary = _fg_bridge.collect(jsb_node, airborne, night)
			_fg_animator.set_properties(fg_props)
			if _aircraft_lights:
				_aircraft_lights.powered = _fg_bridge.dc_volts > 0.0
			if _fg_sounds and _fg_sounds.count > 0 and terrain_initialized and not (_fly_loading and not _intro_playing):
				_fg_sounds.user_volume = (float(_settings.get_setting("engine_volume")) if _settings else 70.0) / 100.0
				_fg_sounds.camera_distance_m = 0.0 if _fg_bridge.cockpit_view or rendering_cam == null else (rendering_cam.global_position - $AC.global_position).length()
				_fg_sounds.update(fg_props, delta)
			# properties only the cockpit switches write: seed their defaults once
			if not _fg_seeded:
				_fg_seeded = true
				for prop in _fg_animator.unknown_properties():
					_fg_controls.state[prop] = _fg_bridge.switch_default(prop)
				for prop in FGBridge.SWITCH_DEFAULTS.keys():
					if not _fg_controls.state.has(prop):
						_fg_controls.state[prop] = FGBridge.SWITCH_DEFAULTS[prop]
				_fg_animator.set_properties(_fg_controls.state)
			_fg_animator.update(delta)
			# pointing hand over a clickable cockpit part
			if camera_mode == CameraMode.COCKPIT and active_camera and _fg_controls.count > 0:
				var over: String = _fg_controls.hover_name(active_camera, get_viewport().get_mouse_position())
				Input.set_default_cursor_shape(Input.CURSOR_POINTING_HAND if over != "" else Input.CURSOR_ARROW)
	if _clouds:
		var env: Environment = _sky.environment
		_clouds.update(get_viewport().get_camera_3d(), _sky.sun_dir_now, _sky.sun_color_now, _sky.sun_energy_now,
			env.ambient_light_color if env else Color(0.55, 0.65, 0.85), _sky.ambient_energy_now,
			env.fog_density if env and env.fog_enabled else 0.0, env.fog_light_color if env else Color(0.7, 0.75, 0.85), delta)
	if time_label:
		time_label.text = "Time: " + _sky.get_time_string()

	# While loading screen is active, update progress and wait for completion
	if not _loading_done:
		_update_loading_progress(false)  # initial startup loading
		return

	# Post-Fly loading: terrain/buildings/roads/water settling at destination
	if _fly_loading:
		_process_rebase_cooldown()
		_update_loading_progress(true)  # fly-to-destination loading
		# The intro zoom plays (and then waits on the spot view) while the
		# destination loads; the flight logic below stays paused until then.
		if _fly_loading and _intro_playing and not (_welcome_screen and _welcome_screen.visible) and not (_settings_screen and _settings_screen.visible):
			_advance_intro(delta)
		return

	# Sun/local-time observer follows the aircraft (ECEF world: no rebase).
	_update_observer_from_ac()
	# Keep the coarse-globe suppression clipped to Scenery3D's live coverage
	# square around the camera (no holes to space beyond the streaming radius).
	_refresh_globe_suppress_rects()

	# VFR map frame (C++ module): position/heading drive + overlay gating.
	# The map freezes under full-screen UI; input pass-through is handled in C++.
	var _vfr_ui_up := (_welcome_screen and _welcome_screen.visible) or (_settings_screen and _settings_screen.visible)
	if _edit_session and _scenery_editor.is_active():
		# Edit Mode: no flight; the map follows the editor camera
		_feed_vfr_map(_scenery_editor._cam.global_position, fmod(rad_to_deg(_scenery_editor._yaw) + 360.0, 360.0), _vfr_ui_up)
	elif jsb_node:
		if _slew_mode.is_enabled():
			# JSBSim physics is paused while slewing, so get_godot_position()/
			# get_heading() stay frozen at the pre-slew state; the SlewMode
			# module moves $AC directly. Feed the map from the AC node instead —
			# same local space (JSBGodot writes its position to this node every
			# physics frame) and continuous across both slew transitions. The
			# visual heading convention is 360 - yaw, matching get_heading()
			# (rotation.y = -visual heading, see teleport + SlewMode reinit).
			var ac := $AC as Node3D
			if ac:
				_feed_vfr_map(ac.global_position, _ac_true_heading(ac), _vfr_ui_up)
		else:
			_feed_vfr_map(jsb_node.get_godot_position(), jsb_node.get_heading(), _vfr_ui_up)

	# Skip all flight logic while welcome or settings overlay is visible
	if _welcome_screen and _welcome_screen.visible:
		return
	if _settings_screen and _settings_screen.visible:
		return

	# Cinematic space→spot zoom on entering fly mode (world3d globe). The C++
	# IntroZoom module owns the animation + the CameraRig orbit handoff; sync
	# the camera_mode/active_camera mirrors when it reports the handoff.
	if _intro_playing:
		_advance_intro(delta)
		return

	# Scenery editor (F8): free camera + tools; while active the flight
	# controls, HUD keys and the aircraft cameras below are skipped, the
	# flight model is frozen like in slew mode.
	_scenery_editor.poll(delta)
	if _scenery_editor.is_active():
		return

	# Escape key: return to welcome screen
	if Input.is_key_pressed(KEY_ESCAPE):
		if _welcome_screen:
			# Once at least one flight has started, offer "Continue" so the
			# user can return to the cockpit without re-teleporting.
			_welcome_screen.set_continue_enabled(not _first_flight)
			_welcome_screen.show_welcome()
			return
	
	# Update FPS label
	if _fps_label and _fps_visible:
		_fps_label.text = "FPS: %d" % Engine.get_frames_per_second()

	# HUD overlay with H: none -> bottom readouts -> readouts + upper info
	if Input.is_key_pressed(KEY_H) and not Input.is_key_pressed(KEY_CTRL):
		if not _hud_key_held:
			_hud_key_held = true
			_hud_mode = (_hud_mode + 1) % 3
			_apply_hud_mode()
	else:
		_hud_key_held = false
	# Shift+Ctrl+M: show every cockpit mouse-sense area (FlightGear controls)
	if Input.is_key_pressed(KEY_M) and Input.is_key_pressed(KEY_CTRL) and Input.is_key_pressed(KEY_SHIFT):
		if not _hotspot_key_held:
			_hotspot_key_held = true
			if _fg_controls.count > 0:
				_fg_controls.set_debug(not _fg_controls.debug)
				print("Cockpit hotspots: ", "SHOWN" if _fg_controls.debug else "HIDDEN")
	else:
		_hotspot_key_held = false
	# Speed brakes with B (fcs/speedbrake-cmd-norm 0/1; the A-10 splits its ailerons)
	if Input.is_key_pressed(KEY_B) and not Input.is_key_pressed(KEY_CTRL):
		if not _speedbrake_key_held:
			_speedbrake_key_held = true
			if jsb_node:
				_speedbrake_out = not _speedbrake_out
				jsb_node.set_property("fcs/speedbrake-cmd-norm", 1.0 if _speedbrake_out else 0.0)
				print("Speed brakes: ", "OUT" if _speedbrake_out else "IN")
	else:
		_speedbrake_key_held = false
	# Landing / taxi lights with L
	if Input.is_key_pressed(KEY_L) and not Input.is_key_pressed(KEY_CTRL):
		if not _landing_key_held:
			_landing_key_held = true
			if _aircraft_lights:
				var on: bool = _aircraft_lights.toggle_landing_lights()
				print("Landing lights: ", "ON" if on else "OFF")
				if _fg_controls.count > 0:
					_fg_controls.state["sim/model/A-10/controls/lighting/land-lights-switch"] = 2.0 if on else 1.0
					if _fg_controls.state.has("controls/lighting/landing-lights"):
						_fg_controls.state["controls/lighting/landing-lights"] = 1.0 if on else 0.0
	else:
		_landing_key_held = false
	# Toggle FPS display with Ctrl+Shift+F
	if Input.is_key_pressed(KEY_CTRL) and Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_F):
		if not _fps_key_held:
			_fps_key_held = true
			_fps_visible = !_fps_visible
			if _fps_label:
				_fps_label.visible = _fps_visible
	else:
		_fps_key_held = false

	# Toggle the weather overlay with Ctrl+I
	if Input.is_key_pressed(KEY_CTRL) and Input.is_key_pressed(KEY_I):
		if not _wx_key_held:
			_wx_key_held = true
			_set_weather_overlay(not _wx_visible)
	else:
		_wx_key_held = false

	# --- Post-teleport position guard ---
	_process_rebase_cooldown()

	# --- Per-second resource diagnostic ---
	_emit_diag_snapshot()

	# Position dialog with Shift+P
	if Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_P):
		if not _position_key_held:
			_position_key_held = true
			_show_position_dialog()
	else:
		_position_key_held = false
	
	# Slew mode (C++ module): Shift+Y toggle + free movement while active,
	# including the slew HUD. Joystick axes come from JoystickConfig
	# (user-calibrated); terrain height under the aircraft is queried here
	# because it belongs to the scenery stack. While slewing we still allow
	# camera controls but skip flight controls and normal HUD updates.
	var was_slewing := _slew_mode.is_enabled()
	var slewing := _slew_mode.update(
		delta, jsb_node,
		JoystickConfig.get_axis("slew_strafe"),
		JoystickConfig.get_axis("slew_forward"),
		JoystickConfig.get_axis("slew_yaw"),
		JoystickConfig.get_axis("slew_vertical"),
		_terrain_elevation_below($AC.global_position),
		JoystickConfig.get_active_device())
	if slewing:
		if Input.is_action_just_pressed("flip_camera"):
			flip_camera()
		_camera_rig.update(delta, JoystickConfig.get_active_device())
		return  # Skip flight controls and HUD updates in slew mode
	if was_slewing:
		# Just exited slew mode — refresh JSBSim's terrain elevation at the
		# new position (the C++ toggle already reinitialized JSBSim there).
		_update_terrain_elevation()
	
	if Input.is_action_just_pressed("flip_camera"):
		flip_camera()
	
	# Handle hat switch / D-pad for pilot head look, orbit, or chase camera
	# (C++ CameraRig; the active joystick index comes from JoystickConfig).
	_camera_rig.update(delta, JoystickConfig.get_active_device())
	
	# Cockpit levers/switches (C++ module): keyboard control, lever visual sync
	# and JSBSim writes for mixture / throttle / magneto / flaps / gear.
	_cockpit_controls.update(jsb_node, _control_surfaces)

	# Animate control surfaces, propeller and landing gear (C++ module).
	# Skipped while jsb_node is null, exactly as before; engine sound follows.
	if jsb_node:
		_control_surfaces.update(jsb_node, delta)
		_update_engine_sound()

	# Update terrain elevation for JSBSim ground collision
	_update_terrain_elevation()
	
	if jsb_node:
		var airspeed = jsb_node.get_airspeed_knots()
		var altitude_ft = jsb_node.get_altitude_ft()
		var vertical_spd = jsb_node.get_vertical_speed_fpm()
		var throttle = jsb_node.get_input_throttle()
		var throttle_percent = throttle * 100.0
		var heading = jsb_node.get_heading()

		airspeed_label.text = "Airspeed: " + str(round(airspeed)) + " knots"
		altitude_label.text = "Altitude: " + str(round(altitude_ft)) + " feet"
		vspeed_label.text = "Verical Speed: " + str(round(vertical_spd)) + " ft/m"
		throttle_label.text = "Throttle: " +str(round(throttle_percent)) + " %"
		
		# Coordinate display
		var godot_pos = jsb_node.get_godot_position()  # ECEF metres
		var g := _geodetic_of(godot_pos)
		var lv: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
		$Control/Label6.text = "ECEF: X=%.0f Y=%.0f Z=%.0f" % [godot_pos.x, godot_pos.y, godot_pos.z]
		$Control/Label7.text = "Swiss E: %s, N: %s" % [format_swiss_coord(lv.x), format_swiss_coord(lv.y)]
		# Prime Meridian (WGS84) display — world-scenery design §3.4
		$Control/Label9.text = "PM: " + format_pm_coord(g.x, true) + "  " + format_pm_coord(g.y, false)
		# JSBSim runs at the true location: psi IS the true heading.
		heading_label.text = "Heading: " + str(round(heading))
	else:
		$Label.text = "JSBGodot node not found."


# Function to cycle between camera modes: Cockpit → Orbit → Chase → Cockpit.
# The mode cycling, head-look reset and current-camera flags are handled by the
# C++ CameraRig; GDScript keeps its own mirror of the mode + active camera for
# lever raycasts.
func flip_camera():
	_camera_rig.flip()
	camera_mode = _camera_rig.get_mode()
	active_camera = camera1 if camera_mode == CameraMode.COCKPIT else camera2



# Helper to print node tree
func print_node_tree(node: Node, depth: int) -> void:
	var indent = ""
	for i in range(depth):
		indent += "  "
	print(indent + node.name + " [" + node.get_class() + "]")
	for child in node.get_children():
		print_node_tree(child, depth + 1)

# Adjust rudder pivot offset in real-time for experimentation
# Keys: U/J = X axis, I/K = Y axis, O/L = Z axis
func handle_pivot_adjustment() -> void:
	var step = 0.1
	var changed = false
	
	if Input.is_key_pressed(KEY_U):
		rudder_pivot_offset.x += step * get_process_delta_time() * 5
		changed = true
	elif Input.is_key_pressed(KEY_J):
		rudder_pivot_offset.x -= step * get_process_delta_time() * 5
		changed = true
	
	if Input.is_key_pressed(KEY_I):
		rudder_pivot_offset.y += step * get_process_delta_time() * 5
		changed = true
	elif Input.is_key_pressed(KEY_K):
		rudder_pivot_offset.y -= step * get_process_delta_time() * 5
		changed = true
	
	if Input.is_key_pressed(KEY_O):
		rudder_pivot_offset.z += step * get_process_delta_time() * 5
		changed = true
	elif Input.is_key_pressed(KEY_L):
		rudder_pivot_offset.z -= step * get_process_delta_time() * 5
		changed = true
	
	if changed:
		print("Pivot offset: ", rudder_pivot_offset)

# Format Swiss coordinate with apostrophe thousand separators
# e.g., 2681931 -> "2'681'931"
func format_swiss_coord(value: float) -> String:
	var int_val = int(round(value))
	var str_val = str(int_val)
	var result = ""
	var count = 0
	# Process from right to left
	for i in range(str_val.length() - 1, -1, -1):
		if count > 0 and count % 3 == 0:
			result = "'" + result
		result = str_val[i] + result
		count += 1
	return result

# Format a WGS84 coordinate as degrees + decimal minutes with hemisphere letter,
# e.g. 60.1938 -> "N 60°11.63′", 11.1070 -> "E 011°06.42′" (design §3.4).
func format_pm_coord(deg: float, is_lat: bool) -> String:
	var hemi: String
	if is_lat:
		hemi = "N" if deg >= 0.0 else "S"
	else:
		hemi = "E" if deg >= 0.0 else "W"
	var a = absf(deg)
	var d = int(a)
	# Round minutes to 2 decimals *before* formatting so 59.997 doesn't print as "60.00".
	var m = round((a - float(d)) * 60.0 * 100.0) / 100.0
	if m >= 60.0: # carry into the next degree
		d += 1
		m = 0.0
	if is_lat:
		return "%s %d°%05.2f′" % [hemi, d, m]
	return "%s %03d°%05.2f′" % [hemi, d, m]


# ── Aircraft definition (aircraft.cfg) ─────────────────────────────────────

# Loads <install_root>/aircraft/<id>/aircraft.cfg and applies the values that
# can be set before the model nodes are resolved (model root path + tunables).
# Falls back silently to the hardcoded defaults when the file is missing.
func _load_aircraft_config() -> void:
	var cfg_path := ""
	# In the editor, prefer the res:// source tree so live edits to
	# aircraft.cfg take effect on relaunch without re-running the installer
	# (which only re-extracts to <install_root>/aircraft on a version bump).
	if OS.has_feature("editor"):
		var res_path := "res://aircraft/%s/aircraft.cfg" % _aircraft_id
		if FileAccess.file_exists(res_path):
			cfg_path = res_path
	if cfg_path.is_empty():
		var ir := get_node_or_null("/root/InstallRoot")
		if ir:
			cfg_path = String(ir.get_aircraft_dir(_aircraft_id)).path_join("aircraft.cfg")
		if cfg_path.is_empty() or not FileAccess.file_exists(cfg_path):
			# Fallback: read the source tree shipped with the project.
			cfg_path = "res://aircraft/%s/aircraft.cfg" % _aircraft_id
	_aircraft_cfg = AircraftConfig.load_from_file(cfg_path)
	if not _aircraft_cfg.loaded:
		print("Aircraft config not found (%s) — using built-in defaults" % cfg_path)
		return
	print("Aircraft config loaded: %s" % cfg_path)

	var cfg := _aircraft_cfg
	_model_root_path = cfg.get_string("model", "root_node", _model_root_path)

	# Control-surface deflection limits + hinge axes (pushed to the C++ animator).
	_control_surfaces.set_rudder_max_angle(cfg.get_float("control_limits", "rudder_max_deg", 16.0))
	_control_surfaces.set_rudder_axis(_parse_axis(cfg.get_string("control_limits", "rudder_hinge_axis", "y"), Vector3.UP))
	_control_surfaces.set_aileron_max_angle(cfg.get_float("control_limits", "aileron_max_deg", 20.0))
	var aileron_axis_name := cfg.get_string("control_limits", "aileron_hinge_axis", "x").to_lower()
	_control_surfaces.set_aileron_axis({"x": Vector3.RIGHT, "y": Vector3.UP, "z": Vector3.BACK}.get(aileron_axis_name, Vector3.RIGHT))
	_control_surfaces.set_elevator_max_angle(cfg.get_float("control_limits", "elevator_max_deg", 25.0))
	var elevator_axis_name := cfg.get_string("control_limits", "elevator_hinge_axis", "x").to_lower()
	_control_surfaces.set_elevator_axis({"x": Vector3.RIGHT, "y": Vector3.UP, "z": Vector3.BACK}.get(elevator_axis_name, Vector3.RIGHT))
	_control_surfaces.set_flap_max_angle(cfg.get_float("control_limits", "flap_max_deg", 30.0))
	_control_surfaces.set_flap_axis(_parse_axis(cfg.get_string("control_limits", "flap_hinge_axis", "x"), Vector3.RIGHT))
	_control_surfaces.set_front_wheel_max_angle(cfg.get_float("control_limits", "front_wheel_max_deg", 30.0))

	# Retractable landing gear.
	_gear_retractable = cfg.get_bool("gear", "retractable", false)
	_cockpit_controls.set_gear_retractable(_gear_retractable)
	_control_surfaces.set_gear_retract_seconds(cfg.get_float("gear", "retract_seconds", 4.0))
	_gear_anim_name = cfg.get_string("gear", "animation", "")

	# Flap detents (cockpit lever notches; the C++ module owns the state).
	var positions := cfg.get_float_array("flaps", "positions", [0.0, 0.333, 0.667, 1.0])
	if not positions.is_empty():
		_cockpit_controls.set_flap_positions(positions)
	var names := cfg.get_string_array("flaps", "names", ["UP", "10°", "20°", "30°"])
	if not names.is_empty():
		_cockpit_controls.set_flap_names(names)

	# Propeller visuals: blur thresholds + spin axis go to the C++ animator;
	# disc size/offset stay here because the blur-disc mesh is built in GDScript.
	# (max_rpm is read in the Engine-sound section below — it only feeds the
	# C++ EngineSound RPM normalisation.)
	_control_surfaces.set_propeller_blur_start_rpm(cfg.get_float("propeller", "blur_start_rpm", 900.0))
	_control_surfaces.set_propeller_hide_blades_rpm(cfg.get_float("propeller", "hide_blades_rpm", 1200.0))
	# spinner cone: swap-in mesh shown only while the blades hide (PC-7), or a
	# separate part shown at every rpm; spins with the blades when its origin
	# sits on the axis (fg_panel_build.py's Spinner_Pivot)
	_control_surfaces.set_propeller_cone_always_visible(cfg.get_bool("propeller", "cone_always_visible", false))
	_control_surfaces.set_propeller_cone_spins(cfg.get_bool("propeller", "cone_spins", false))
	PROPELLER_DISC_RADIUS = cfg.get_float("propeller", "disc_radius_m", PROPELLER_DISC_RADIUS)
	PROPELLER_SPIN_AXIS = _parse_axis(cfg.get_string("propeller", "spin_axis", "z"), Vector3.BACK)
	_control_surfaces.set_propeller_spin_axis(PROPELLER_SPIN_AXIS)
	var disc_off := cfg.get_float_array("propeller", "disc_offset", [])
	if disc_off.size() == 3:
		PROPELLER_DISC_OFFSET = Vector3(disc_off[0], disc_off[1], disc_off[2])
	else:
		PROPELLER_DISC_OFFSET = Vector3.ZERO

	# Engine sound. The audio stream path stays here (GDScript loads the node);
	# the RPM->pitch/volume tunables go to the C++ EngineSound module, including
	# [propeller] max_rpm which only feeds its RPM normalisation.
	ENGINE_SOUND_PATH = cfg.get_string("sound", "engine", ENGINE_SOUND_PATH)
	_engine_sound.set_pitch_range(cfg.get_float("sound", "pitch_min", 0.7), cfg.get_float("sound", "pitch_max", 1.6))
	_engine_sound.set_vol_db_range(cfg.get_float("sound", "vol_db_min", -12.0), cfg.get_float("sound", "vol_db_max", 3.0))
	_engine_sound.set_max_rpm(cfg.get_float("propeller", "max_rpm", 2700.0))


# Adopt the airframe the user picked on the welcome screen. Re-reads
# aircraft.cfg for the new id (so model paths / tunables follow) and asks the
# JSBGodot extension to (re)load the matching JSBSim flight model, taken from
# the cfg [jsbsim] model key (defaults to the aircraft id itself).
## Nose-wheel steering ranges from aircraft.cfg [gear] steer_limited_deg /
## steer_full_deg (Ctrl+W toggles in the bridge); starts in LIMITED.
func _apply_nose_steer_range() -> void:
	if jsb_node == null or not jsb_node.has_method("set_nose_steer_range"):
		return
	var limited := 0.0
	var full := 0.0
	if _aircraft_cfg != null:
		limited = _aircraft_cfg.get_float("gear", "steer_limited_deg", 0.0)
		full = _aircraft_cfg.get_float("gear", "steer_full_deg", 0.0)
	jsb_node.set_nose_steer_range(limited, full)
	jsb_node.set_nose_steer_full(false)


func _apply_selected_aircraft(aircraft_id: String) -> void:
	if aircraft_id.is_empty():
		aircraft_id = DEFAULT_AIRCRAFT_ID
	if aircraft_id != _aircraft_id:
		_aircraft_id = aircraft_id
		_load_aircraft_config()
	var jsbsim_model := _aircraft_id
	if _aircraft_cfg != null:
		jsbsim_model = _aircraft_cfg.get_string("jsbsim", "model", _aircraft_id)
	if jsb_node and jsb_node.has_method("load_aircraft"):
		print("Loading JSBSim model '%s' for aircraft '%s'" % [jsbsim_model, _aircraft_id])
		jsb_node.load_aircraft(_aircraft_id, jsbsim_model)
	_apply_nose_steer_range()

	# Swap the visual GLB model to the selected aircraft, then re-resolve all
	# model-dependent nodes (control surfaces + cockpit levers) against it. Only
	# do this when the model actually changes — re-running it on the already-loaded
	# model would double-flip the normals (lighting it from below).
	if _aircraft_id != _model_visuals_id:
		var loader := get_node_or_null(NodePath(_model_root_path))
		if loader and loader.has_method("load_aircraft_model"):
			if loader.load_aircraft_model(_aircraft_id):
				_apply_model_normals(loader)
				_resolve_aircraft_nodes()
				_model_visuals_id = _aircraft_id
	# Reposition the cockpit camera for the newly-selected airframe.
	_apply_camera_config()


# Positions the cockpit camera (Camera1) for the selected airframe from
# aircraft.cfg [camera]:
#   cockpit_position     "x, y, z"  — AC-local origin override (meters)
#   cockpit_rotation_deg "p, y, r"  — optional extra rotation vs the scene basis
#   cockpit_fov          float      — optional field of view
# When no [camera] keys are present (e.g. the Cessna) the scene's own Camera1
# transform is kept unchanged.
func _apply_camera_config() -> void:
	if camera1 == null or _aircraft_cfg == null:
		return
	var pos := _aircraft_cfg.get_float_array("camera", "cockpit_position", [])
	var rot := _aircraft_cfg.get_float_array("camera", "cockpit_rotation_deg", [])
	var fov := _aircraft_cfg.get_float("camera", "cockpit_fov", 0.0)
	# Always start from the pristine scene transform so repeated swaps don't
	# compound, and so an airframe with no [camera] keys restores the default.
	camera1.transform = _camera1_scene_transform
	camera1.fov = _camera1_scene_fov
	if pos.size() == 3 or rot.size() == 3 or fov > 0.0:
		var xform := _camera1_scene_transform
		if rot.size() == 3:
			var extra := Basis.from_euler(Vector3(
				deg_to_rad(rot[0]), deg_to_rad(rot[1]), deg_to_rad(rot[2])))
			xform.basis = xform.basis * extra
		if pos.size() == 3:
			xform.origin = Vector3(pos[0], pos[1], pos[2])
		camera1.transform = xform
		if fov > 0.0:
			camera1.fov = fov
		print("Cockpit camera positioned for '%s': %s" % [_aircraft_id, xform.origin])
	# Head look composes on top of this transform; keep the C++ rig's captured
	# cockpit base in sync (it resets Camera1 to it every frame).
	_camera_rig.capture_cockpit_base()
	# External camera distances in metres (rig defaults 1.5 / 50 / 6 when absent).
	_camera_rig.set_orbit_distance_min(_aircraft_cfg.get_float("camera", "orbit_distance_min", 1.5))
	_camera_rig.set_orbit_distance_max(_aircraft_cfg.get_float("camera", "orbit_distance_max", 50.0))
	_camera_rig.set_orbit_distance(_aircraft_cfg.get_float("camera", "orbit_distance", 6.0))



# Resolves all model-dependent nodes (animated control surfaces + interactive
# cockpit levers) against the currently-loaded GLB under _model_root_path.
# Called once at startup and again after a runtime aircraft model swap. Nodes
# absent from the aircraft.cfg [animation]/[levers] sections (or missing in the
# model) resolve to null and are skipped gracefully by the animation code.
func _resolve_aircraft_nodes() -> void:
	# The model loader frees the previous GLB subtree synchronously on an
	# airframe swap, so drop every registered pointer before re-resolving.
	_control_surfaces.reset()
	_cockpit_controls.reset()
	# Drop stale lever physics bodies. They live inside the model subtree, which
	# is freed on an airframe swap; a lingering reference would otherwise be
	# passed to register_debug_bodies() below and dereference freed memory.
	mixture_lever_body = null
	throttle_lever_body = null
	magneto_switch_body = null
	flap_lever_body = null

	var rudder_node := _resolve_surface("rudder")
	if rudder_node:
		print("Rudder node found: ", rudder_node)
		_control_surfaces.register_rudder(rudder_node)
	else:
		print("WARNING: Rudder node not found!")

	var propeller_node := _resolve_surface("propeller")
	if propeller_node:
		print("Propeller node found: ", propeller_node)
		_control_surfaces.register_propeller(propeller_node)
		_create_propeller_blur_disc(propeller_node)
	else:
		print("WARNING: Propeller node not found!")

	# Optional dedicated spinner cone shown in place of the blades at high RPM.
	var propeller_cone_node := _resolve_surface("propeller_cone")
	if propeller_cone_node:
		print("Propeller cone node found: ", propeller_cone_node)
		_control_surfaces.register_propeller_cone(propeller_cone_node)   # sets its start visibility

	var left_aileron_node := _resolve_surface("left_aileron")
	if left_aileron_node:
		print("Left aileron node found: ", left_aileron_node)
		_control_surfaces.register_aileron_left(left_aileron_node)
	else:
		print("WARNING: Left aileron node not found!")

	var right_aileron_node := _resolve_surface("right_aileron")
	if right_aileron_node:
		print("Right aileron node found: ", right_aileron_node)
		_control_surfaces.register_aileron_right(right_aileron_node)
	else:
		print("WARNING: Right aileron node not found!")

	var left_elevator_node := _resolve_surface("left_elevator")
	if left_elevator_node:
		print("Left elevator node found: ", left_elevator_node)
		_control_surfaces.register_elevator_left(left_elevator_node)
	else:
		print("WARNING: Left elevator node not found!")

	var right_elevator_node := _resolve_surface("right_elevator")
	if right_elevator_node:
		print("Right elevator node found: ", right_elevator_node)
		_control_surfaces.register_elevator_right(right_elevator_node)
	else:
		print("WARNING: Right elevator node not found!")

	var single_elevator_node := _resolve_surface("single_elevator")
	if single_elevator_node:
		print("Single elevator node found: ", single_elevator_node)
		_control_surfaces.register_single_elevator(single_elevator_node)

	var left_flap_node := _resolve_surface("left_flap")
	if left_flap_node:
		print("Left flap node found: ", left_flap_node)
		_control_surfaces.register_flap_left(left_flap_node)
	else:
		print("WARNING: Left flap node not found!")

	var right_flap_node := _resolve_surface("right_flap")
	if right_flap_node:
		print("Right flap node found: ", right_flap_node)
		_control_surfaces.register_flap_right(right_flap_node)
	else:
		print("WARNING: Right flap node not found!")

	# Generic multi-flap list (cfg [animation] flaps) — e.g. the PC-7's four split
	# flaps. Each rotates about the configured flap axis by its per-flap sign
	# (cfg flap_signs).
	if _aircraft_cfg:
		var flap_paths := _aircraft_cfg.get_string_array("animation", "flaps", [])
		var flap_signs := _aircraft_cfg.get_float_array("control_limits", "flap_signs", [])
		for i in flap_paths.size():
			var fnode := _resolve_model_node(flap_paths[i])
			if fnode:
				var fsign := 1.0
				if i < flap_signs.size():
					fsign = flap_signs[i]
				_control_surfaces.add_flap_node(fnode, fsign)
				print("Flap node found: ", fnode.name)
			else:
				print("WARNING: Flap node not found: ", flap_paths[i])

	var front_wheel_node := _resolve_surface("front_wheel")
	if front_wheel_node:
		print("Front wheel node found: ", front_wheel_node)
		_control_surfaces.register_front_wheel(front_wheel_node)
	else:
		print("WARNING: Front wheel node not found!")

	_resolve_gear_legs()

	# Interactive cockpit levers — located by name (find_child) under the model
	# root. Names + fallbacks come from aircraft.cfg [levers]. Base transforms,
	# key input and JSBSim writes live in the C++ CockpitControls module;
	# GDScript keeps node resolution, collision bodies and mouse dragging.
	var c172_root = get_node_or_null(NodePath(_model_root_path))
	if c172_root:
		mixture_lever_node = _resolve_lever(c172_root, "mixture")
		if mixture_lever_node:
			print("Mixture lever node found: ", mixture_lever_node.name)
			_setup_mixture_lever_collision()
		else:
			print("WARNING: Mixture lever node not found")

		throttle_lever_node = _resolve_lever(c172_root, "throttle")
		if throttle_lever_node:
			print("Throttle lever node found: ", throttle_lever_node.name)
			_setup_throttle_lever_collision()
		else:
			print("WARNING: Throttle lever node not found")

		magneto_switch_node = _resolve_lever(c172_root, "magneto")
		if magneto_switch_node:
			print("Magneto switch node found: ", magneto_switch_node.name)
			_setup_magneto_switch_collision()
		else:
			print("WARNING: Magneto switch node not found")

		flap_lever_node = _resolve_lever(c172_root, "flap_lever")
		if flap_lever_node:
			print("Flap lever node found: ", flap_lever_node.name)
			_setup_flap_lever_collision()
		else:
			print("WARNING: Flap lever node not found")

		# Register with the C++ module (captures base transforms), then apply
		# the initial states exactly as before.
		_cockpit_controls.register_levers(mixture_lever_node, throttle_lever_node, magneto_switch_node, flap_lever_node)
		# degrees per magneto position about the key's local Z (sign = model's key sense)
		_cockpit_controls.set_magneto_step_angle(_aircraft_cfg.get_float("levers", "magneto_step_deg", 30.0) if _aircraft_cfg else 30.0)
		# which position the mesh's rest pose shows (0 OFF .. 3 BOTH, 4 START)
		_cockpit_controls.set_magneto_default_position(int(_aircraft_cfg.get_float("levers", "magneto_rest_position", 3.0)) if _aircraft_cfg else 3)
		_cockpit_controls.register_debug_bodies(mixture_lever_body, throttle_lever_body)
		if mixture_lever_node:
			_cockpit_controls.apply_mixture_value(jsb_node)
		if magneto_switch_node:
			_cockpit_controls.apply_magneto_position(jsb_node)
		if flap_lever_node:
			_cockpit_controls.sync_flaps_from_jsb(jsb_node)
	_setup_aircraft_lights()
	_setup_fg_animations()
	_setup_fg_sounds()
	_apply_hidden_nodes()


# aircraft.cfg [sound] fg_sounds: a FlightGear sound set (scripts/fg_a10_sounds.py)
# played by FGSounds from the bridge's property values; the basic engine loop
# stays silent for such an aircraft.
func _setup_fg_sounds() -> void:
	if _fg_sounds == null:
		_fg_sounds = FGSounds.new()
		_fg_sounds.name = "FGSounds"
		add_child(_fg_sounds)
	_fg_sounds.clear()
	var rel: String = _aircraft_cfg.get_string("sound", "fg_sounds", "") if _aircraft_cfg else ""
	if rel.is_empty():
		return
	var ir := get_node_or_null("/root/InstallRoot")
	var path: String = ir.get_aircraft_dir(_aircraft_id).path_join(rel) if ir != null else ""
	if path.is_empty() or not FileAccess.file_exists(path):
		path = ProjectSettings.globalize_path("res://aircraft/%s/%s" % [_aircraft_id, rel])
	var n: int = _fg_sounds.load(path, path.get_base_dir())
	print("FlightGear sounds for '%s': %d loaded, %d without a file: %s" % [_aircraft_id, n, _fg_sounds.skipped.size(), str(_fg_sounds.skipped.slice(0, 8))])


# aircraft.cfg [model] hide_nodes: model nodes (by name, all matches) never
# shown, e.g. placeholder display textures of a FlightGear model.
func _apply_hidden_nodes() -> void:
	if _aircraft_cfg == null:
		return
	var model_root := get_node_or_null(NodePath(_model_root_path))
	if model_root == null:
		return
	var n := 0
	for name in _aircraft_cfg.get_string_array("model", "hide_nodes", []):
		var nm := str(name).strip_edges()
		if nm.is_empty():
			continue
		for node in model_root.find_children(nm, "", true, false):
			if node is Node3D:
				(node as Node3D).visible = false
				n += 1
	if n > 0:
		print("Model: %d nodes hidden by [model] hide_nodes" % n)


# FlightGear animation manifest (aircraft.cfg [model] fg_animations) run by
# the FGAnimator extension on the loaded model; FGBridge feeds it per frame.
func _setup_fg_animations() -> void:
	if _fg_animator == null:
		_fg_animator = FGAnimator.new()
		_fg_animator.name = "FGAnimator"
		add_child(_fg_animator)
	var rel: String = _aircraft_cfg.get_string("model", "fg_animations", "") if _aircraft_cfg else ""
	if rel.is_empty():
		_fg_animator.clear()
		return
	var path := ""
	var ir := get_node_or_null("/root/InstallRoot")
	if ir != null:
		path = ir.get_aircraft_dir(_aircraft_id).path_join(rel)
	if path.is_empty() or not FileAccess.file_exists(path):
		path = ProjectSettings.globalize_path("res://aircraft/%s/%s" % [_aircraft_id, rel])
	var model_root := get_node_or_null(NodePath(_model_root_path))
	if _fg_animator.load(path, model_root):
		print("FlightGear animations loaded for '%s': %s" % [_aircraft_id, str(_fg_animator.stats())])
		_fg_controls.state.clear()
		_fg_seeded = false
		_fg_controls.on_change = Callable(self, "_on_fg_control")
		print("FlightGear cockpit controls: %d clickable parts" % _fg_controls.setup(path, model_root, _fg_animator))
	else:
		print("WARNING: FlightGear animations not loaded from %s" % path)
		_fg_controls.setup("", null, null)


## A cockpit switch or knob changed a FlightGear property: real effects.
func _on_fg_control(prop: String, value) -> void:
	match prop:
		"controls/gear/brake-parking":
			if jsb_node:
				_parking_brake = float(value) > 0.5
				jsb_node.set_input_brake(1.0 if _parking_brake else 0.0)
				print("Parking brake: ", "SET" if _parking_brake else "RELEASED")
		"sim/model/A-10/controls/lighting/land-lights-switch":
			if _aircraft_lights:
				_aircraft_lights.set_landing_lights(float(value) != 1.0)   # 0 taxi / 1 off / 2 landing
		"sim/model/A-10/controls/lighting/nav-lights-switch":
			if _aircraft_lights:
				_aircraft_lights.nav_enabled = float(value) > 0.5
		"controls/lighting/strobe":
			if _aircraft_lights:
				_aircraft_lights.strobe_enabled = float(value) > 0.5
				# the A-10's one switch covers the beacon too; a model with its own
				# beacon switch (Cessna) keeps them apart
				if not _fg_animator.property_names().has("controls/lighting/beacon"):
					_aircraft_lights.beacon_enabled = float(value) > 0.5
		"controls/lighting/beacon":
			if _aircraft_lights:
				_aircraft_lights.beacon_enabled = float(value) > 0.5
		"controls/lighting/nav-lights":
			if _aircraft_lights:
				_aircraft_lights.nav_enabled = float(value) > 0.5
		"controls/lighting/landing-lights":
			if _aircraft_lights:
				_aircraft_lights.set_landing_lights(float(value) > 0.5)
				print("Landing lights: ", "ON" if float(value) > 0.5 else "OFF")
		"controls/lighting/taxi-light":
			if _aircraft_lights:
				_aircraft_lights.taxi_enabled = float(value) > 0.5
		"sim/model/A-10/controls/canopy/canopy-switch":
			_fg_bridge.canopy_target = float(value)
		"provpilot/gear-toggle":
			# the gear handle (ProvPilot-defined pick): same path as the G key
			_fg_controls.state.erase(prop)
			if jsb_node and _gear_retractable:
				var down: bool = jsb_node.get_gear_cmd() < 0.5
				_cockpit_controls.set_gear_down(down)
				_control_surfaces.set_gear_down(down)
				jsb_node.set_gear_down(down)
				print("Gear (handle): ", "DOWN" if down else "UP")
		"sim/model/A-10/instrumentation/g-meter/g-max", "sim/model/A-10/instrumentation/g-meter/g-min":
			_fg_bridge.g_max = 1.0
			_fg_bridge.g_min = 1.0
			_fg_controls.state.erase(prop)


# Exterior lights (aircraft.cfg [lights]) on the loaded model; rebuilt on
# every airframe swap because the fixtures live inside the model subtree.
func _setup_aircraft_lights() -> void:
	if _aircraft_lights == null:
		_aircraft_lights = AircraftLights.new()
		_aircraft_lights.name = "AircraftLights"
		add_child(_aircraft_lights)
	_aircraft_lights.setup(get_node_or_null(NodePath(_model_root_path)), _aircraft_cfg, $AC)
	if _aircraft_lights.fixture_count() > 0:
		print("Aircraft lights: %d fixtures (nav %d, strobe %d, beacon %d, landing %d, taxi %d)" % [
			_aircraft_lights.fixture_count(), _aircraft_lights.fixture_count("nav_left") + _aircraft_lights.fixture_count("nav_right") + _aircraft_lights.fixture_count("nav_tail"),
			_aircraft_lights.fixture_count("strobe"), _aircraft_lights.fixture_count("beacon"),
			_aircraft_lights.fixture_count("landing"), _aircraft_lights.fixture_count("taxi")])


# Resolves an animated control-surface mesh from aircraft.cfg [animation].
# The stored path is relative to _model_root_path.
func _resolve_surface(key: String) -> Node3D:
	if _aircraft_cfg == null:
		return null
	var rel := _aircraft_cfg.get_string("animation", key, "")
	if rel.is_empty():
		return null
	return get_node_or_null(NodePath(_model_root_path.path_join(rel))) as Node3D


# Resolves a model node from a path relative to _model_root_path (empty -> null).
func _resolve_model_node(rel: String) -> Node3D:
	if rel.is_empty():
		return null
	return get_node_or_null(NodePath(_model_root_path.path_join(rel))) as Node3D


# Parses an axis name (x/y/z, optionally prefixed with - to invert the sweep
# direction) into a unit Vector3. Falls back to [param fallback] on bad input.
func _parse_axis(name: String, fallback: Vector3) -> Vector3:
	var n := name.strip_edges().to_lower()
	var sgn := 1.0
	if n.begins_with("-"):
		sgn = -1.0
		n = n.substr(1)
	elif n.begins_with("+"):
		n = n.substr(1)
	var axis: Vector3 = {"x": Vector3.RIGHT, "y": Vector3.UP, "z": Vector3.BACK}.get(n, Vector3.ZERO)
	if axis == Vector3.ZERO:
		return fallback
	return axis * sgn


# Builds the retractable-gear leg list from aircraft.cfg [gear]. The "legs" key
# is a comma list of leg names; each leg <name> reads:
#   <name>_wheel        path (rel to model root) of the wheel mesh
#   <name>_strut        path of the strut/mechanic mesh (optional)
#   <name>_pivot        "x, y, z" hinge point in the model wrapper frame
#   <name>_axis         x|y|z  rotation axis (wrapper frame)
#   <name>_retract_deg  sweep angle for a full retraction (sign sets direction)
# Wheel + strut rotate rigidly about the shared pivot so the leg moves as a unit.
func _resolve_gear_legs() -> void:
	# Both C++ modules (ControlSurfaces + CockpitControls) reset their gear
	# state to fully down as part of reset(), called from _resolve_aircraft_nodes.
	# Fixed gear may list legs too (rolling tyres): they never retract.
	if _aircraft_cfg == null:
		return
	if not _gear_retractable and _aircraft_cfg.get_string_array("gear", "legs", []).is_empty():
		return
	var axis_map := {"x": Vector3.RIGHT, "y": Vector3.UP, "z": Vector3.BACK}
	var default_steer_max := _aircraft_cfg.get_float("control_limits", "front_wheel_max_deg", 30.0)
	var leg_names := _aircraft_cfg.get_string_array("gear", "legs", [])
	for leg in leg_names:
		var wheel := _resolve_model_node(_aircraft_cfg.get_string("gear", leg + "_wheel", ""))
		var strut := _resolve_model_node(_aircraft_cfg.get_string("gear", leg + "_strut", ""))
		if wheel == null and strut == null:
			print("WARNING: gear leg '%s' has no resolvable nodes" % leg)
			continue
		var pivot_arr := _aircraft_cfg.get_float_array("gear", leg + "_pivot", [])
		var pivot := Vector3.ZERO
		if pivot_arr.size() == 3:
			pivot = Vector3(pivot_arr[0], pivot_arr[1], pivot_arr[2])
		var axis_name := _aircraft_cfg.get_string("gear", leg + "_axis", "x").to_lower()
		var axis: Vector3 = axis_map.get(axis_name, Vector3.RIGHT)
		# Optional nose-wheel steering: the wheel yaws about its own steer axis
		# with rudder input, composed UNDER the retract rotation.
		var steer := _aircraft_cfg.get_bool("gear", leg + "_steer", false)
		var steer_axis := _parse_axis(_aircraft_cfg.get_string("gear", leg + "_steer_axis", "z"), Vector3.BACK)
		var steer_max := _aircraft_cfg.get_float("gear", leg + "_steer_max_deg", default_steer_max)
		# Steer pivot: vertical hinge line the wheel + mechanic yaw about.
		# Defaults to the retract pivot so they share one axis if unspecified.
		var steer_pivot_arr := _aircraft_cfg.get_float_array("gear", leg + "_steer_pivot", [])
		var steer_pivot := pivot
		if steer_pivot_arr.size() == 3:
			steer_pivot = Vector3(steer_pivot_arr[0], steer_pivot_arr[1], steer_pivot_arr[2])
		# Optional rolling wheel: the wheel mesh spins about its axle while the
		# aircraft rolls on the ground, and stops once airborne (weight-off-wheels).
		var roll := _aircraft_cfg.get_bool("gear", leg + "_roll", false)
		var roll_axis := _parse_axis(_aircraft_cfg.get_string("gear", leg + "_roll_axis", "y"), Vector3.UP)
		var radius := _aircraft_cfg.get_float("gear", leg + "_wheel_radius_m",
			_aircraft_cfg.get_float("gear", "wheel_radius_m", 0.3))
		_control_surfaces.add_gear_leg(
			wheel, strut, pivot, axis,
			_aircraft_cfg.get_float("gear", leg + "_retract_deg", 90.0),
			steer, steer_axis, steer_max, steer_pivot,
			roll, roll_axis, maxf(radius, 0.01))
		print("Gear leg '%s' resolved (wheel=%s strut=%s)" % [leg, wheel != null, strut != null])
	if jsb_node and jsb_node.has_method("set_gear_down"):
		jsb_node.set_gear_down(true)

	# Optional imported retract/extend clip: locate the model's AnimationPlayer
	# and the named clip, then hold it at the gear-DOWN (start) pose. When found,
	# the C++ animator drives this clip by seeking instead of rotating leg pivots.
	if not _gear_anim_name.is_empty():
		var model_root := get_node_or_null(NodePath(_model_root_path))
		if model_root:
			var ap := model_root.find_child("AnimationPlayer", true, false) as AnimationPlayer
			if ap and ap.has_animation(_gear_anim_name):
				var clip := ap.get_animation(_gear_anim_name)
				# Scrub-only: the C++ animator drives the pose every frame via
				# seek(), so the player must NOT auto-advance or loop (otherwise
				# it runs forward on its own and snaps back, extending the gear
				# right after it retracts). Disable looping and pause so seek()
				# is the sole driver.
				clip.loop_mode = Animation.LOOP_NONE
				ap.current_animation = _gear_anim_name
				ap.seek(0.0, true)  # 0 = first keyframe = gear down
				ap.pause()
				print("Gear animation '%s' found (length=%.3fs)" % [_gear_anim_name, clip.length])
				_control_surfaces.set_gear_anim(ap, clip.length)
			else:
				print("WARNING: gear animation '%s' not found in model" % _gear_anim_name)
		else:
			print("WARNING: model root not found for gear animation lookup")


# Resolves a cockpit lever/switch by name (find_child) under [param root].
# Reads the primary name from aircraft.cfg [levers] <key> and an optional
# "<key>_fallback" for the original (unrenamed) Sketchfab node name.
func _resolve_lever(root: Node, key: String) -> Node3D:
	if _aircraft_cfg == null or root == null:
		return null
	var primary := _aircraft_cfg.get_string("levers", key, "")
	var node: Node = null
	if not primary.is_empty():
		node = root.find_child(primary, true, false)
	if node == null:
		var fallback := _aircraft_cfg.get_string("levers", key + "_fallback", "")
		if not fallback.is_empty():
			node = root.find_child(fallback, true, false)
	return node as Node3D


# ── Loading Screen ─────────────────────────────────────────────────────────

func _create_loading_screen() -> void:
	var script = load("res://loading_screen.gd")
	_loading_screen = CanvasLayer.new()
	_loading_screen.set_script(script)
	add_child(_loading_screen)
	_loading_screen.show_loading()


func _update_loading_progress(is_fly_loading: bool) -> void:
	## Monitor Scenery3D load state and update the loading bar.
	## Scenery3D doesn't expose a load-progress API, so we sample terrain
	## elevation at the target position and four ring points; the fraction
	## of points that have non-NaN elevations approximates how much of the
	## near terrain ring has actually streamed in. The C++ tile manager now
	## throttles JPEG decode/upload (ORTHO_DECODES_PER_FRAME=3, CHUNKS_PER_FRAME=1)
	## so the initial fly-to-airport load can take many seconds — the bar
	## reflects that progress instead of dismissing immediately.
	##
	## The ring sampling stays here because it needs this node's elevation query
	## (precise Scenery3D patch, falling back to the world3d globe). The progress
	## blending, hard timeout safety net and completion state machine live in the
	## C++ LoadingProgress module; it reports the bar value + status text.
	const RING_RADIUS_M := 200.0  # Sample radius around target (≈ tile size)
	var sample_e := 0.0
	var sample_n := 0.0
	if is_fly_loading and _first_flight_target.has("swiss_e"):
		sample_e = float(_first_flight_target["swiss_e"])
		sample_n = float(_first_flight_target["swiss_n"])
	else:
		var ac_node := get_node_or_null("AC")
		if ac_node:
			var g := _geodetic_of(ac_node.global_position)
			var lv: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
			sample_e = lv.x
			sample_n = lv.y

	var samples := [
		Vector2(sample_e, sample_n),
		Vector2(sample_e + RING_RADIUS_M, sample_n),
		Vector2(sample_e - RING_RADIUS_M, sample_n),
		Vector2(sample_e, sample_n + RING_RADIUS_M),
		Vector2(sample_e, sample_n - RING_RADIUS_M),
	]
	var total := samples.size()
	var loaded := 0
	for s in samples:
		var ll: Vector2 = Geo.lv95_to_wgs84(s.x, s.y)
		# The globe has whole-planet elevation immediately, so terrain is always
		# "loaded" — this makes the fly-to-airport screen dismiss promptly.
		var h: float = _terrain_elevation_geodetic(ll.x, ll.y)
		if not is_nan(h):
			loaded += 1

	# Progress blending + timeout safety net + completion live in the C++ module.
	# It reports the bar value + status text; GDScript still owns the UI and the
	# post-completion actions (intro zoom on fly-load, welcome screen on startup).
	var terrain_init_pending := is_fly_loading and _pending_terrain_init
	var progress := _loading_progress.update(
		loaded, total, is_fly_loading, terrain_init_pending,
		_terrain_elevation.get_rebase_cooldown(),
		_terrain_elevation.get_rebase_cooldown_frames())

	if _loading_screen:
		_loading_screen.set_progress(progress)
		_loading_screen.set_status(_loading_progress.get_status_text())
	if is_fly_loading and _intro_playing and _intro_status_label:
		# one bar per loading phase (scenery tiles, flight model, stabilizing),
		# each restarting at 0; the tile bar takes over after completion
		_intro_status_label.text = _loading_progress.get_status_text()
		_set_intro_bar(_loading_progress.get_phase_progress())

	if not _loading_progress.is_complete():
		return

	if is_fly_loading:
		# Post-Fly loading complete — the simulation runs; the intro zoom
		# (started with the Fly! request) may now hand over to the spot view.
		_fly_loading = false
		if _loading_screen:
			_loading_screen.hide_loading()
		_intro_zoom.set_hold_at_end(false)
		if _intro_status_label:
			_intro_status_label.text = "Loading scenery"
		_intro_pending_prev = -1   # the tile bar starts over for the last phase
		_intro_done = 0
		_intro_shown = 0.0
		_set_intro_bar(0.0)
		print("Fly loading complete — simulation active")
	else:
		# Startup loading complete — show welcome screen
		_loading_done = true
		if _loading_screen:
			_loading_screen.hide_loading()
		_create_settings_screen()
		_create_welcome_screen()
		print("Loading complete — showing welcome screen")


# ── Welcome / Settings Screens ─────────────────────────────────────────────

func _create_welcome_screen() -> void:
	var script = load("res://welcome_screen.gd")
	_welcome_screen = CanvasLayer.new()
	_welcome_screen.set_script(script)
	add_child(_welcome_screen)
	_welcome_screen.flight_requested.connect(_on_welcome_flight_requested)
	_welcome_screen.edit_requested.connect(_on_welcome_edit_requested)
	_scenery_editor.exited.connect(_on_scenery_editor_exited)
	_welcome_screen.settings_requested.connect(_on_welcome_settings_requested)
	_welcome_screen.continue_requested.connect(_on_welcome_continue_requested)
	_welcome_screen.scenery_setup_requested.connect(_on_welcome_scenery_setup_requested)

	# Load the bundled airport catalog (res://airports.json) and tag entries
	# with availability markers based on which scenery base packs are
	# installed. Scenery3D no longer ships POI data so we curate the list
	# in-app — see provpilot/airports_lib.gd.
	var installed_packs: Dictionary = {}
	var scenery_areas: Array = []
	if _settings:
		installed_packs = _settings.get_setting("installed_scenery_packs")
		scenery_areas = _settings.get_scenery_areas()
	var regions := AirportsLib.installed_regions(installed_packs)
	# Also accept scenery placed on disk outside the in-app downloader
	# (e.g. users who copied a Germany/ folder in manually, or who have
	# the area present-but-disabled in settings).
	var disk_regions := AirportsLib.installed_regions_from_areas(scenery_areas)
	for r in disk_regions.keys():
		regions[r] = true
	_welcome_screen.set_installed_regions(regions)
	_welcome_screen.populate_airports(_catalog_with_pack_runways())

	_welcome_screen.show_welcome()


func _create_settings_screen() -> void:
	_settings_screen = SettingsScreen.new()
	add_child(_settings_screen)  # loads provpilot.cfg in its _ready
	_settings = _settings_screen.get_store()
	_wire_aircraft_settings_panel()
	_settings_screen.closed.connect(_on_settings_closed)
	_clouds = CloudLayersScript.new()
	_clouds.name = "CloudLayers"
	add_child(_clouds)
	_runways = RunwaysScript.new()
	_runways.name = "Runways"
	add_child(_runways)
	_live_weather = LiveWeatherScript.new()
	_live_weather.latlon_provider = _aircraft_latlon
	_live_weather.weather_updated.connect(_on_live_weather_updated)
	_live_weather.weather_failed.connect(_on_live_weather_failed)
	add_child(_live_weather)


## Per-second snapshot of scene-graph / renderer counters. Lets us spot
## what's growing across repeated teleports. Logs at most once per second.
func _emit_diag_snapshot() -> void:
	var now: int = Time.get_ticks_msec()
	if now - _diag_last_msec < 1000:
		return
	_diag_last_msec = now
	var mem_static: int = int(Performance.get_monitor(Performance.MEMORY_STATIC))
	var objects: int = int(Performance.get_monitor(Performance.OBJECT_COUNT))
	var nodes: int = int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT))
	var orphans: int = int(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT))
	var resources: int = int(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT))
	var render_objs: int = int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME))
	var render_prims: int = int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))
	var tex_mem: int = int(Performance.get_monitor(Performance.RENDER_TEXTURE_MEM_USED))
	var buf_mem: int = int(Performance.get_monitor(Performance.RENDER_BUFFER_MEM_USED))
	var fps: int = Engine.get_frames_per_second()
	var s3d_children := 0
	if is_instance_valid(scenery3d_node):
		s3d_children = scenery3d_node.get_child_count()
	var veg_children := 0
	if is_instance_valid(vegetation_node):
		veg_children = vegetation_node.get_child_count()
	print("[diag] jump=%d fps=%d mem=%.1fMB obj=%d node=%d orph=%d res=%d render_obj=%d prim=%d tex=%.1fMB buf=%.1fMB s3d_kids=%d veg_kids=%d" % [
		_diag_jump_count, fps,
		mem_static / 1048576.0, objects, nodes, orphans, resources,
		render_objs, render_prims,
		tex_mem / 1048576.0, buf_mem / 1048576.0,
		s3d_children, veg_children,
	])


## Handle Fly! from welcome screen — teleport to the selected airport
func _on_welcome_flight_requested(aircraft_id: String, swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== WELCOME: Flight requested ===")
	print("  Aircraft: %s" % aircraft_id)
	print("  Target: E=%.0f N=%.0f Heading=%.0f°" % [swiss_e, swiss_n, heading_deg])

	# Pre-flight scenery check: if no enabled scenery area actually has any
	# terrain/orthophoto data on disk, the loading screen would stall forever
	# waiting for tiles that never arrive. Bail out early with a clear message
	# so the user can fix the configuration in Settings → Scenery.
	var missing := _settings.missing_scenery_reason() if _settings else "Settings unavailable — cannot verify scenery configuration."
	if missing != "":
		_show_scenery_error_dialog(missing)
		return

	_diag_jump_count += 1
	_settings_from_welcome = false
	_edit_session = false
	var ac_vis := get_node_or_null("AC")
	if ac_vis:
		ac_vis.visible = true   # hidden during an Edit Mode session

	# Adopt the airframe the user selected (reloads aircraft.cfg and asks the
	# JSBSim extension to load the matching flight model) before we position
	# and initialise the simulation.
	_apply_selected_aircraft(aircraft_id)

	# Loading state while the destination streams in: the intro zoom shows the
	# progress; the flight logic waits for _loading_progress (below).
	_fly_loading = true
	_loading_progress.begin_fly_loading()

	# Sun/observer to the destination now (the loading screen hides the jump);
	# the coverage mask follows the streamer once tiles arrive.
	if USE_WORLD3D_GLOBE:
		var ll: Vector2 = Geo.lv95_to_wgs84(swiss_e, swiss_n)
		_sky.observer_lat = ll.x
		_sky.observer_lon = ll.y
		_suppress_dirty = true
		_refresh_globe_suppress_rects()

	if _first_flight:
		_do_first_flight(swiss_e, swiss_n, heading_deg)
		_first_flight = false
	else:
		# A Fly! during the previous flight's intro zoom: end that zoom first
		# (a new one starts over the new destination below).
		_cancel_intro_zoom()
		_on_teleport_requested(swiss_e, swiss_n, heading_deg)

	# The cinematic space → spot zoom starts now, over the destination, and
	# doubles as the loading view (status + progress on its HUD; it waits on
	# the spot view until loading is complete). No cockpit camera renders
	# meanwhile, so no cockpit sounds. The loading screen is only the fallback
	# when the zoom cannot start.
	if not _start_intro_zoom() and _loading_screen:
		_loading_screen.set_progress(0.0)
		_loading_screen.set_status("Loading terrain...")
		_loading_screen.show_loading()


## Welcome "Edit Mode": the scenery editor over the selected airport with
## no aircraft and no intro zoom. The AC node is parked (hidden) at the
## destination so the sun observer and the map have a position, JSBSim is
## kept in slew, and the editor camera drives the tile streamer. Leaving
## the editor returns to the welcome screen.
func _on_welcome_edit_requested(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== WELCOME: Edit Mode requested at E=%.0f N=%.0f ===" % [swiss_e, swiss_n])
	var missing := _settings.missing_scenery_reason() if _settings else "Settings unavailable — cannot verify scenery configuration."
	if missing != "":
		_show_scenery_error_dialog(missing)
		if _welcome_screen:
			_welcome_screen.show_welcome()
		return
	_cancel_intro_zoom()
	_edit_session = true
	_settings_from_welcome = false
	if USE_WORLD3D_GLOBE:
		var ll: Vector2 = Geo.lv95_to_wgs84(swiss_e, swiss_n)
		_sky.observer_lat = ll.x
		_sky.observer_lon = ll.y
		_suppress_dirty = true
		_refresh_globe_suppress_rects()
	var ac_node := get_node_or_null("AC")
	if ac_node:
		ac_node.global_transform = _ac_transform_at(swiss_e, swiss_n, 500.0, heading_deg, ac_node.scale)
		ac_node.visible = false
	if jsb_node and jsb_node.has_method("set_slew_mode"):
		jsb_node.set_slew_mode(true)
	_set_scenery_streaming(true)
	_scenery_editor.enter_at(swiss_e, swiss_n, heading_deg)


func _on_scenery_editor_exited() -> void:
	if not _edit_session:
		return
	_edit_session = false
	if _welcome_screen:
		_welcome_screen.set_continue_enabled(not _first_flight)
		_welcome_screen.show_welcome()


func _cancel_intro_zoom() -> void:
	if not _intro_playing:
		return
	_intro_zoom.cancel()
	_intro_playing = false
	camera_mode = CameraMode.ORBIT
	active_camera = camera2
	if camera2:
		camera2.current = true
	_show_intro_hud(false)


func _show_scenery_error_dialog(message: String) -> void:
	var dlg := AcceptDialog.new()
	dlg.title = "Scenery not available"
	dlg.dialog_text = message
	dlg.dialog_autowrap = true
	dlg.min_size = Vector2(560, 0)
	add_child(dlg)
	dlg.confirmed.connect(func() -> void: dlg.queue_free())
	dlg.canceled.connect(func() -> void: dlg.queue_free())
	dlg.popup_centered()
	push_warning("Fly! aborted: %s" % message)


## Toggle the per-frame `_process()` callback on every Scenery3D child
## manager (tile / building / water / road) and on the sibling Vegetation
## manager. Used to suppress all scenery streaming while the welcome
## screen is up, so we don't waste worker time loading tiles around the
## origin before the user has picked a destination.
func _set_scenery_streaming(enabled: bool) -> void:
	if scenery3d_node:
		for child in scenery3d_node.get_children():
			if child.has_method("set_process"):
				child.set_process(enabled)
	if vegetation_node and vegetation_node.has_method("set_process"):
		vegetation_node.set_process(enabled)
	print("Scenery streaming ", "ENABLED" if enabled else "DISABLED")


## First flight: load terrain, initialize JSBSim, enable scenery loaders.
## Called instead of _on_teleport_requested on the very first Fly! click.
func _do_first_flight(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== FIRST FLIGHT: Loading scenery for E=%.0f N=%.0f ===" % [swiss_e, swiss_n])

	var ac_node = get_node_or_null("AC")
	if not ac_node:
		push_error("_do_first_flight: missing AC node")
		return

	# 3. Position aircraft at target (height is corrected after terrain loads):
	#    ECEF pose on the ellipsoid at the LV95 target, true heading.
	ac_node.global_transform = _ac_transform_at(swiss_e, swiss_n, 500.0, heading_deg, ac_node.scale)

	# Store target for deferred JSBSim init (uses these in _do_deferred_terrain_init)
	_first_flight_target = {
		"swiss_e": swiss_e,
		"swiss_n": swiss_n,
		"heading_deg": heading_deg
	}

	# 4. Now that the camera-tracking AC is at its real destination, turn
	# scenery streaming back on. The tile manager's first _process tick
	# will see the camera at the airport and queue tiles around it,
	# instead of around the origin.
	_set_scenery_streaming(true)

	# 5. Queue deferred JSBSim initialization (runs in next process frame).
	_initialize_jsbsim_on_terrain()

	# 6. Nothing else to enable — Scenery3D handles buildings/roads/water natively.

	# 7. Arm the rebase cooldown (C++ module) so the aircraft stays in place until
	#    terrain settles and JSBSim stabilises.
	_terrain_elevation.start_rebase(ac_node.global_position)

	print("=== FIRST FLIGHT: scenery loading initiated ===")


func _on_welcome_settings_requested() -> void:
	_settings_from_welcome = true   # the Aircraft tab follows the page's selection
	if _welcome_screen:
		_welcome_screen.visible = false
	if _settings_screen:
		_settings_screen.show_settings()


## Resume the in-progress flight — simply hide the welcome overlay without
## teleporting or reloading. Position, attitude and JSBSim state are
## preserved because flight logic only pauses while the welcome screen is
## visible (see `_process`).
func _on_welcome_continue_requested() -> void:
	if _welcome_screen:
		_welcome_screen.hide_welcome()


## First-run wizard handler. The user picked a folder; treat it as a scan root,
## scan it for regions (Switzerland/, Baden-Wuertemberg/, …) and apply the
## resulting library to Scenery3D so tile loading starts.
func _on_welcome_scenery_setup_requested(path: String) -> void:
	if not _settings:
		push_warning("Welcome scenery setup: settings screen not ready")
		return
	# Inject `path` into the scan roots and run the scanner. This mirrors what
	# the user would do manually in Settings → Scenery → Scan.
	_settings.add_scan_root(path)
	_settings.scan_scenery()
	# Persist immediately so the next launch skips the wizard.
	_settings.save()
	# Apply newly-found areas to the Scenery3D node.
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
	_suppress_dirty = true  # scenery areas may have changed
	_refresh_globe_suppress_rects()
	if _welcome_screen:
		_welcome_screen.set_continue_enabled(not _first_flight)
		_welcome_screen.visible = true
		_welcome_screen.show_welcome()


func _apply_tree_density() -> void:
	if _settings and is_instance_valid(vegetation_node):
		_settings.apply_tree_density(vegetation_node)


## Apply scenery data sources from the settings library to the Scenery3D,
## Vegetation and VfrMap nodes (C++ SettingsStore.apply_scenery_sources:
## enabled areas in priority order -> per-asset path arrays, plus
## skip_white_pixels and the terrain load radius).
func _apply_scenery_sources() -> void:
	if not is_instance_valid(scenery3d_node):
		return
	if not _settings:
		print("Scenery sources: settings screen unavailable, leaving scene defaults")
		return
	_settings.apply_scenery_sources(scenery3d_node, vegetation_node, _vfr_map)
	_rebuild_runways()


## Airfield lights: on from dusk (sun 4 deg above the horizon -> full at
## 4 deg below) and whenever the visibility is below 1 km (low-visibility
## operations), as agreed 2026-09-20 instead of the JAR minima tables.
func _airfield_light_intensity(cam_pos: Vector3) -> float:
	if _visibility_km < 1.0:
		return 1.0
	var g := _geodetic_of(cam_pos)
	var up: Vector3 = W3DCoords.geodetic_normal(g.x, g.y)
	var sun_elev_deg: float = rad_to_deg(asin(clampf(_sky.sun_dir_now.dot(up), -1.0, 1.0)))
	return clampf((4.0 - sun_elev_deg) / 8.0, 0.0, 1.0)


## Merge the enabled packs' airports/runways.json (design §12 D16) and
## rebuild the procedural runway meshes. Called with the scenery sources
## (startup, first flight, settings closed).
func _rebuild_runways() -> void:
	if not _settings or not is_instance_valid(_runways):
		return
	_runways_merged = RunwaysLibScript.load_merged(_settings.get_scenery_areas())
	_runways.set_merged(_runways_merged, Callable(self, "_terrain_elevation_geodetic"))
	print("Runways: %d airports merged from the enabled packs (built by distance)" % _runways_merged.size())


## The bundled catalog with every pack-defined airport's runways replaced
## by the pack data (real thresholds instead of ARP + heading), plus pack
## airports the catalog lacks.
func _catalog_with_pack_runways() -> Array[Dictionary]:
	var catalog: Array[Dictionary] = AirportsLib.load_default()
	if _settings:
		if _runways_merged.is_empty():
			_runways_merged = RunwaysLibScript.load_merged(_settings.get_scenery_areas())
		RunwaysLibScript.merge_into_catalog(catalog, _runways_merged)
	return catalog


# ── Position / Teleport ────────────────────────────────────────────────────

## Show the position dialog (Shift+P) — disabled, position dialog script was
## removed. Welcome screen handles airport selection instead.
func _show_position_dialog() -> void:
	print("Position dialog is not available; use the Welcome screen (restart) to change airport.")


## Handle teleport request from the dialog
func _on_teleport_requested(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== TELEPORT REQUESTED ===")
	print("  Target: E=%d N=%d Heading=%.0f°" % [int(swiss_e), int(swiss_n), heading_deg])

	var ac_node = get_node_or_null("AC")
	if not ac_node or not jsb_node:
		push_error("Cannot teleport: missing AC or JSBGodot node")
		return

	# 1. Force JSBSim into slew (pauses physics position updates)
	if jsb_node.has_method("set_slew_mode"):
		jsb_node.set_slew_mode(true)
	# Fly! is a fresh session: no throttle, stick, trim, flaps or brakes carried
	# over from the previous flight ("Continue Flight" keeps everything).
	if jsb_node.has_method("reset_controls"):
		jsb_node.reset_controls()
	# reset_controls() puts JSBSim's gear down; mirror that in the cockpit
	# command and snap the visual (legs / imported clip) to down as well.
	_cockpit_controls.set_gear_down(true)
	_control_surfaces.reset_gear(true)

	# 8. Get terrain height at new position — sample a grid around the
	#    threshold and use the maximum, matching the runway mesh approach.
	#    Offsets are along/across the runway in the LV95 grid (grid heading).
	var terrain_height := 0.0
	if is_instance_valid(scenery3d_node):
		var max_h := -1e9
		var ll0: Vector2 = Geo.lv95_to_wgs84(swiss_e, swiss_n)
		var heading_rad := deg_to_rad(heading_deg + Geo.lv95_grid_convergence_deg(ll0.x, ll0.y))
		# Sample centre + 4 offsets along/across runway (±25 m each way): (dE, dN)
		var offsets: Array = [
			Vector2(0, 0),
			Vector2(sin(heading_rad) * 25.0, cos(heading_rad) * 25.0),
			Vector2(-sin(heading_rad) * 25.0, -cos(heading_rad) * 25.0),
			Vector2(cos(heading_rad) * 15.0, -sin(heading_rad) * 15.0),
			Vector2(-cos(heading_rad) * 15.0, sin(heading_rad) * 15.0),
		]
		for off in offsets:
			var ll: Vector2 = Geo.lv95_to_wgs84(swiss_e + off.x, swiss_n + off.y)
			var h := _terrain_elevation_geodetic(ll.x, ll.y)
			if h > max_h:
				max_h = h
		if max_h > -1e8:
			terrain_height = max_h
			print("  Terrain height at target (max of 5 samples): %.1fm" % terrain_height)
		else:
			print("  WARNING: No terrain data at target — using 0m")

	# 9. Place aircraft on runway surface with gear clearance
	# Runway surface sits y_offset (0.1 m) above raw terrain;
	# gear_height accounts for model origin being above the wheels.
	var runway_surface_offset := 0.1
	var gear_height := 0.5  # same hair-above-ground spawn as the first flight
	ac_node.global_transform = _ac_transform_at(swiss_e, swiss_n, terrain_height + runway_surface_offset + gear_height, heading_deg, ac_node.scale)

	print("  New ECEF pos: ", ac_node.global_position)

	# 10. Reinitialize JSBSim at the new position but keep slew mode ON.
	#     JSBSim stays paused during cooldown; we reinit again when terrain
	#     has loaded and the cooldown expires (in _process).
	if jsb_node.has_method("reinitialize_from_pose"):
		jsb_node.reinitialize_from_pose(ac_node.global_transform, 0.0, terrain_height)
		_apply_fuel_setting()
		_set_parking_brake_at_start()
	if _live_weather and _live_weather.enabled:
		_live_weather.refresh(true)  # new region: fetch the METAR there now
	_update_terrain_elevation()

	# 11. Keep JSBSim in slew mode — physics stays paused until cooldown expires.
	#     The C++ module's state + HUD are cleared so the slew UI is not shown.
	_slew_mode.note_external_exit()

	# Arm the rebase cooldown (C++ module): pin the AC here while JSBSim stabilises.
	_terrain_elevation.start_rebase(ac_node.global_position)
	# The tile streamer follows the viewport camera: make sure it is one that
	# rides with the pinned aircraft, or the destination tiles never load.
	var cur := get_viewport().get_camera_3d()
	if cur == null or not ac_node.is_ancestor_of(cur):
		var cam: Camera3D = camera2 if camera_mode == CameraMode.ORBIT else camera1
		if is_instance_valid(cam):
			cam.current = true
			active_camera = cam

	print("=== TELEPORT COMPLETE ===")


# ── Interactive Cockpit Mixture Lever ──────────────────────────────────────

## Create a StaticBody3D + CollisionShape3D on the mixture lever so the
## camera raycast can detect mouse clicks on it.
func _setup_mixture_lever_collision() -> void:
	mixture_lever_body = StaticBody3D.new()
	mixture_lever_body.collision_layer = MIXTURE_LEVER_COLLISION_LAYER
	mixture_lever_body.collision_mask = 0  # doesn't need to collide with anything
	
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	# Lever is a small knob — model units are ~meters at this hierarchy level
	box.size = Vector3(0.02, 0.02, 0.04)
	shape.shape = box
	
	mixture_lever_body.add_child(shape)
	mixture_lever_node.add_child(mixture_lever_body)
	# Initial mixture value is applied in _resolve_aircraft_nodes after the
	# lever nodes are registered with the C++ module (base transform needed).


## Create collision body on the throttle lever for mouse picking.
func _setup_throttle_lever_collision() -> void:
	throttle_lever_body = StaticBody3D.new()
	throttle_lever_body.collision_layer = THROTTLE_LEVER_COLLISION_LAYER
	throttle_lever_body.collision_mask = 0
	
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(0.02, 0.02, 0.04)
	shape.shape = box
	
	throttle_lever_body.add_child(shape)
	throttle_lever_node.add_child(throttle_lever_body)


func _unhandled_input(event: InputEvent) -> void:
	# Only interact in cockpit camera mode
	if camera_mode != CameraMode.COCKPIT:
		return
	if not active_camera:
		return
	
	# Don't process cockpit interaction during overlays
	if not _loading_done or _fly_loading:
		return
	if _welcome_screen and _welcome_screen.visible:
		return
	if _settings_screen and _settings_screen.visible:
		return
	
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		# Space held: the wheel zooms the cockpit view
		if mb.pressed and _camera_rig.is_mouse_look_active() and mb.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
			_camera_rig.mouse_zoom(1 if mb.button_index == MOUSE_BUTTON_WHEEL_UP else -1)
			get_viewport().set_input_as_handled()
			return
		# FlightGear cockpit controls (switches, knobs) take clicks and wheel first
		if _fg_controls.count > 0 and _fg_controls.click(active_camera, mb.position, mb.button_index, mb.pressed, mb.shift_pressed):
			get_viewport().set_input_as_handled()
			return
		if mb.button_index == MOUSE_BUTTON_LEFT:
			if mb.pressed:
				# Try mixture lever first, then throttle, then magneto switch
				if mixture_lever_node and _raycast_hits_body(mb.position, mixture_lever_body, MIXTURE_LEVER_COLLISION_LAYER):
					_mixture_dragging = true
					_cockpit_controls.set_dragging(CockpitControls.LEVER_MIXTURE, true)
					_mixture_drag_start_y = mb.position.y
					_mixture_drag_start_value = _cockpit_controls.get_mixture_value()
					get_viewport().set_input_as_handled()
				elif throttle_lever_node and jsb_node and _raycast_hits_body(mb.position, throttle_lever_body, THROTTLE_LEVER_COLLISION_LAYER):
					_throttle_dragging = true
					_cockpit_controls.set_dragging(CockpitControls.LEVER_THROTTLE, true)
					_throttle_drag_start_y = mb.position.y
					_throttle_drag_start_value = float(jsb_node.get_input_throttle())
					get_viewport().set_input_as_handled()
				elif magneto_switch_node and _raycast_hits_body(mb.position, magneto_switch_body, MAGNETO_SWITCH_COLLISION_LAYER):
					_magneto_dragging = true
					_cockpit_controls.set_dragging(CockpitControls.LEVER_MAGNETO, true)
					_magneto_drag_start_x = mb.position.x
					_magneto_drag_start_pos = _cockpit_controls.get_magneto_position()
					get_viewport().set_input_as_handled()
				elif flap_lever_node and _raycast_hits_body(mb.position, flap_lever_body, FLAP_LEVER_COLLISION_LAYER):
					_flap_dragging = true
					_cockpit_controls.set_dragging(CockpitControls.LEVER_FLAP, true)
					_flap_drag_start_y = mb.position.y
					_flap_drag_start_pos = _cockpit_controls.get_flap_position()
					get_viewport().set_input_as_handled()
			else:
				if _mixture_dragging:
					_mixture_dragging = false
					_cockpit_controls.set_dragging(CockpitControls.LEVER_MIXTURE, false)
					get_viewport().set_input_as_handled()
				if _throttle_dragging:
					_throttle_dragging = false
					_cockpit_controls.set_dragging(CockpitControls.LEVER_THROTTLE, false)
					get_viewport().set_input_as_handled()
				if _magneto_dragging:
					_magneto_dragging = false
					_cockpit_controls.set_dragging(CockpitControls.LEVER_MAGNETO, false)
					# Spring back from START on mouse release
					if _cockpit_controls.get_magneto_position() == 4:
						_cockpit_controls.set_magneto_position(3, jsb_node)
						print("Magneto: BOTH")
					get_viewport().set_input_as_handled()
				if _flap_dragging:
					_flap_dragging = false
					_cockpit_controls.set_dragging(CockpitControls.LEVER_FLAP, false)
					get_viewport().set_input_as_handled()
	
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _camera_rig.is_mouse_look_active():
			_camera_rig.mouse_look(mm.relative)   # Space held: look around
			get_viewport().set_input_as_handled()
		elif _mixture_dragging:
			var delta_y := _mixture_drag_start_y - mm.position.y
			var delta_mixture := delta_y / 200.0
			var new_value := clampf(_mixture_drag_start_value + delta_mixture, 0.0, 1.0)
			_cockpit_controls.set_mixture_value(new_value, jsb_node)
			get_viewport().set_input_as_handled()
		elif _throttle_dragging and jsb_node:
			var delta_y := _throttle_drag_start_y - mm.position.y
			var delta_throttle := delta_y / 200.0
			var new_throttle := clampf(_throttle_drag_start_value + delta_throttle, 0.0, 1.0)
			jsb_node.set_input_throttle(new_throttle)
			# Update lever visual immediately during drag (C++ owns the node)
			_cockpit_controls.apply_throttle_visual(new_throttle)
			get_viewport().set_input_as_handled()
		elif _magneto_dragging:
			# Drag right = clockwise (toward START), left = counterclockwise (toward OFF)
			var delta_x := mm.position.x - _magneto_drag_start_x
			var steps := int(delta_x / 40.0)  # 40px per position step
			var new_pos := clampi(_magneto_drag_start_pos + steps, 0, 4)
			if new_pos != _cockpit_controls.get_magneto_position():
				_cockpit_controls.set_magneto_position(new_pos, jsb_node)
			get_viewport().set_input_as_handled()
		elif _flap_dragging:
			# Drag down = more flaps, up = less flaps (vertical, like a pull-down lever)
			var delta_y := mm.position.y - _flap_drag_start_y
			var steps := int(delta_y / 40.0)  # 40px per notch
			var new_pos := clampi(_flap_drag_start_pos + steps, 0, 3)
			if new_pos != _cockpit_controls.get_flap_position():
				_cockpit_controls.set_flap_position(new_pos, jsb_node)
			get_viewport().set_input_as_handled()


## Raycast from the cockpit camera through the mouse position.
## Returns true if the ray hits the given StaticBody3D on the given layer.
func _raycast_hits_body(screen_pos: Vector2, body: StaticBody3D, layer: int) -> bool:
	if not active_camera or not body:
		return false
	
	var from := active_camera.project_ray_origin(screen_pos)
	var dir := active_camera.project_ray_normal(screen_pos)
	var to := from + dir * 5.0
	
	var space := get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(from, to)
	query.collision_mask = layer
	query.collide_with_areas = false
	query.collide_with_bodies = true
	
	var result := space.intersect_ray(query)
	if result.is_empty():
		return false
	
	return result["collider"] == body


func _setup_magneto_switch_collision() -> void:
	magneto_switch_body = StaticBody3D.new()
	magneto_switch_body.collision_layer = MAGNETO_SWITCH_COLLISION_LAYER
	magneto_switch_body.collision_mask = 0
	
	var shape := CollisionShape3D.new()
	var cylinder := CylinderShape3D.new()
	cylinder.radius = 0.015
	cylinder.height = 0.02
	shape.shape = cylinder
	
	magneto_switch_body.add_child(shape)
	magneto_switch_node.add_child(magneto_switch_body)


func _setup_flap_lever_collision() -> void:
	flap_lever_body = StaticBody3D.new()
	flap_lever_body.collision_layer = FLAP_LEVER_COLLISION_LAYER
	flap_lever_body.collision_mask = 0
	
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = Vector3(0.02, 0.04, 0.02)  # Taller along Y for lever travel
	shape.shape = box
	
	flap_lever_body.add_child(shape)
	flap_lever_node.add_child(flap_lever_body)


## ProVPilot HUD overlay (H key): 0 nothing, 1 the bottom readouts (airspeed,
## altitude, vertical speed, throttle, heading), 2 also the upper info block
## (Swiss coordinates, PM, ECEF, time, slew heading). The VFR map, the weather
## overlay and the FPS counter have their own toggles.
func _apply_hud_mode() -> void:
	var bottom := get_node_or_null("Control/BottomRow")
	if bottom:
		bottom.visible = _hud_mode >= 1
	for nm in ["Control/Label6", "Control/Label7", "Control/Label8", "Control/Label9", "Control/TimeLabel"]:
		var n := get_node_or_null(nm)
		if n:
			n.visible = _hud_mode >= 2


## Settings → Aircraft tab (aircraft_settings_panel.gd): tell it which aircraft
## is selected, which is flown, and how to read a config.
func _wire_aircraft_settings_panel() -> void:
	if _settings_screen == null:
		return
	var panel = _settings_screen.get_aircraft_panel()
	if panel == null:
		return
	panel.aircraft_provider = Callable(self, "_settings_aircraft_id")
	panel.flown_provider = func() -> String: return _aircraft_id if terrain_initialized else ""
	panel.fdm_provider = func(): return jsb_node
	panel.config_provider = Callable(self, "_config_for_aircraft")
	panel.name_provider = func(id: String) -> String: return WelcomeScreenScript.aircraft_label(id) if WelcomeScreenScript else id


## The aircraft the Aircraft tab shows: the welcome page's selection when the
## settings were opened from that page (it hides itself first) or while it is
## up, else the one being flown.
var _settings_from_welcome := false

func _settings_aircraft_id() -> String:
	if _welcome_screen and (_welcome_screen.visible or _settings_from_welcome) and _welcome_screen.has_method("get_selected_aircraft_id"):
		return _welcome_screen.get_selected_aircraft_id()
	return _aircraft_id


func _config_for_aircraft(id: String):
	if id == _aircraft_id and _aircraft_cfg != null:
		return _aircraft_cfg
	var ir := get_node_or_null("/root/InstallRoot")
	var path: String = ir.get_aircraft_dir(id).path_join("aircraft.cfg") if ir != null else ""
	if path == "" or not FileAccess.file_exists(path):
		path = ProjectSettings.globalize_path("res://aircraft/%s/aircraft.cfg" % id)
	return AircraftConfig.load_from_file(path) if FileAccess.file_exists(path) else null


## Fuel load from the Aircraft tab (aircraft_<id>_fuel_pct), applied to every
## tank when a flight starts.
func _apply_fuel_setting() -> void:
	if _settings == null or jsb_node == null:
		return
	var key := "aircraft_%s_fuel_pct" % _aircraft_id
	if _settings.has_setting(key):
		var pct: float = float(_settings.get_setting(key))
		AircraftSettingsPanelScript.apply_fuel(jsb_node, pct)
		print("Fuel: %s tanks set to %.0f %% from the Aircraft settings" % [_aircraft_id, pct])


## Every flight starts with the parking brake set (the aircraft used to roll
## off while the scenery was still loading). Release: "," (comma), the A-10's
## emergency brake handle, or set again with ".".
func _set_parking_brake_at_start() -> void:
	if jsb_node == null:
		return
	jsb_node.set_input_brake(1.0)
	_parking_brake = true
	if _fg_controls.count > 0:
		_fg_controls.state["controls/gear/brake-parking"] = 1.0
	print("Parking brake: SET at flight start (release with ,)")


## Keep the cockpit handle (FlightGear control) in step with the brake keys.
func _sync_parking_brake() -> void:
	if jsb_node == null or _fg_controls.count == 0:
		return
	var braked: bool = jsb_node.get_input_brake() > 0.5
	if braked != _parking_brake:
		_parking_brake = braked
		_fg_controls.state["controls/gear/brake-parking"] = 1.0 if braked else 0.0
