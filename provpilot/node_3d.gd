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
var TERRAIN_CENTER_E: float = 2682880.0  # Swiss Easting at Godot X=0
var TERRAIN_CENTER_N: float = 1283072.0  # Swiss Northing at Godot Z=0

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

# Cinematic space→spot intro zoom (world3d globe) lives in the C++ IntroZoom
# module (src/intro_zoom.cpp). GDScript keeps the scene-side gates (globe
# mounted, AC + dedicated intro camera valid), registers the session-stable
# nodes in _setup_world3d_globe, and drives per-frame updates from _process.
# The handoff to the orbit camera is done by the module through CameraRig; the
# camera_mode/active_camera mirrors are refreshed here when update() reports
# STATUS_FINISHED.
var _intro_zoom := IntroZoom.new()

# Altitude-driven atmosphere (fog fade-out toward space, sky-to-black
# background, ambient/glow taper) + coarse-globe visibility gate live in the
# C++ Atmosphere module (src/atmosphere.cpp). GDScript registers the scene
# Environment + globe node once the world3d globe is mounted and calls
# update(get_viewport()) from _process.
var _atmosphere := Atmosphere.new()

# Day/night sun + atmospheric sky (sky_atmosphere.gd). Installs the
# atmospheric-scattering sky shader so the horizon follows the world3d globe's
# Earth curvature at every altitude, swings the scene sun/shadows through the
# day from the simulated time of day, and applies the altitude fog/ambient
# ramps + coarse-globe visibility gate. This replaces the C++ Atmosphere
# module's per-frame update (which forced a flat procedural sky + a solid
# black background above 100 km, hiding the atmospheric limb in orbit).
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
# flat local (X=West, Y=Up, Z=North) at the LV95 origin -> ECEF metres.
var _ecef_from_flat: Transform3D = Transform3D.IDENTITY
# Continuous rebase tracking (design §8.1 Phase 2, decision D2): re-anchor
# the globe tangent point once the AC has moved this far from it, so the
# d²/2R sag never grows and the mount only ever moves at (quantized)
# aircraft speed — no discrete transform events in flight. A parked AC costs
# nothing (the distance check skips the re-anchor).
const GLOBE_REBASE_TRACK_M := 500.0
var _globe_anchor_flat: Vector2 = Vector2.ZERO
var _globe_has_anchor := false
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
# The altitude-driven atmosphere ramps (exponential fog fade-out toward space,
# sky-to-black background above the atmosphere, ambient/glow taper) and the
# coarse-globe visibility gate live in the C++ Atmosphere module
# (src/atmosphere.cpp); it captures the scene Environment's base values when
# registered (see _setup_world3d_globe).

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

	# Show a lightweight loading screen immediately, before any heavy work.
	_create_loading_screen()

	# Load the per-airframe definition (aircraft.cfg) before resolving any
	# model nodes, so paths/tunables come from data instead of hardcoded here.
	_load_aircraft_config()

	# Get JSBGodot node after we're in the scene tree
	jsb_node = $AC/JSBGodot
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

	# Slew mode (C++ module): register the session-stable AC root + HUD labels
	# and the Swiss origin (must run after TERRAIN_CENTER is locked above).
	_slew_mode.register_nodes($AC, airspeed_label, altitude_label, vspeed_label, throttle_label, heading_label, $Control/Label6, $Control/Label7, $Control/Label8)
	_slew_mode.set_swiss_center(TERRAIN_CENTER_E, TERRAIN_CENTER_N)

	# VFR map frame (C++ module): one-time wiring with the Swiss origin and
	# the portable config path. Tile paths arrive via _apply_scenery_sources().
	_vfr_map.setup(TERRAIN_CENTER_E, TERRAIN_CENTER_N, InstallRoot.get_config_path())

	# Terrain elevation (C++ module): register the session-stable AC root and push
	# the fallback timeout GDScript still owns locally (it also gates the deferred
	# init handshake). The other tunables use the module's built-in defaults.
	_terrain_elevation.register_nodes($AC)
	_terrain_elevation.set_terrain_init_fallback_msec(TERRAIN_INIT_FALLBACK_MSEC)

	# Mount the world3d global globe at the LV95 origin. Must run after
	# TERRAIN_CENTER_E/N are final so the tangent frame matches the flat frame.
	_setup_world3d_globe()

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
const TERRAIN_INIT_FALLBACK_MSEC: int = 12000

# Initialize JSBSim with correct terrain elevation at startup
# This must be called BEFORE JSBSim runs its physics, setting terrain during IC
func _initialize_jsbsim_on_terrain() -> void:
	# Store for deferred initialization in first physics frame
	_pending_terrain_init = true
	_pending_terrain_init_start_msec = Time.get_ticks_msec()
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
## Query terrain elevation at a world XZ position.
## Prefers the precise scenery3d patch (Swiss/BW); falls back to the coarse
## whole-planet world3d globe so the aircraft always has ground beneath it when
## flying beyond the detailed patch. Returns NAN only if neither has data.
func _terrain_elevation_at(x: float, z: float) -> float:
	var h: float = _scenery_elevation_at(x, z)
	if not is_nan(h):
		return h
	return _globe_elevation_at(x, z)


## Precise scenery3d elevation (NAN if no node / method / data / tile not loaded).
func _scenery_elevation_at(x: float, z: float) -> float:
	if not is_instance_valid(scenery3d_node):
		return NAN
	if not scenery3d_node.has_method("get_elevation"):
		return NAN
	var h: float = scenery3d_node.get_elevation(x, z)
	# Older Scenery3D builds returned 0.0 (not NaN) for unloaded tiles; Swiss/BW
	# terrain is never at 0 m ASL, so treat 0.0 as "no data" defensively.
	if h == 0.0 or is_nan(h):
		return NAN
	return h


## Coarse whole-planet globe elevation (NAN if the globe is unavailable).
func _globe_elevation_at(x: float, z: float) -> float:
	if not (USE_WORLD3D_GLOBE and is_instance_valid(globe_node) and _w3d_coords != null):
		return NAN
	var ecef: Vector3 = _ecef_from_flat * Vector3(x, 0.0, z)
	var geo: Vector3 = _w3d_coords.ecef_to_geodetic(ecef)  # (lat, lon, height)
	var gh: float = globe_node.get_elevation(geo.x, geo.y)
	if is_nan(gh):
		return NAN
	return gh


## Mount the world3d global globe into the flat ProvPilot frame.
## The globe renders in ECEF (~6.4 M m); we give GlobeMount the inverse of the
## tangent frame at the LV95 origin so the globe's origin surface point lands at
## the flat frame origin. _rebase_globe_to() later moves this tangent point onto
## each destination airport (the flat frame itself never changes). The globe
## reads the camera via to_local(), so it still computes the correct ECEF
## camera position for LOD/streaming internally.
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
	var ll: Vector2 = Geo.lv95_to_wgs84(TERRAIN_CENTER_E, TERRAIN_CENTER_N)
	var origin_lat: float = ll.x
	var origin_lon: float = ll.y
	var origin_h: float = 0.0
	# forward transform: flat local (X=West, Y=Up, Z=North) -> ECEF metres.
	_ecef_from_flat = _w3d_coords.enu_to_ecef_transform(origin_lat, origin_lon, origin_h, true)
	# Map the ECEF globe back into the flat frame.
	_globe_mount.transform = _ecef_from_flat.affine_inverse()
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
	# Register the altitude-driven atmosphere ramps + coarse-globe visibility
	# gate with the C++ module (it captures the Environment's base values now,
	# so the globe doesn't wash out to the fog colour when seen from space).
	var we := get_node_or_null("Environment/WorldEnvironment")
	_atmosphere.register_nodes(we.environment if we else null, globe_node)
	# Register the cinematic intro zoom nodes (session-stable; the dedicated
	# intro camera was created just above).
	_intro_zoom.register_nodes(get_node_or_null("AC"), _intro_camera, camera1, camera2, _camera_rig)
	print("World3D globe mounted: LV95 %d/%d -> lat=%.5f lon=%.5f" % [
		int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N), origin_lat, origin_lon])


## Move the globe tangent point under a flat-frame position so the coarse
## globe sits beneath the aircraft instead of beneath the LV95 origin. The
## flat frame itself stays fixed for the whole session (Scenery3D tiles,
## JSBSim and all C++ modules use it); only the ECEF<->flat mapping moves:
## _ecef_from_flat becomes F_a * T(-P_a) where F_a is the tangent frame at
## (lat, lon) and P_a is that point's flat position. The mount takes the
## inverse, so W3DGlobe's to_local(camera) LOD/streaming still sees a correct
## ECEF camera, and _globe_elevation_at() keeps its formula unchanged.
## lat/lon come from Geo.lv95_to_wgs84 (swisstopo polynomial — ~1 m near CH,
## degrading with distance; at 70°N the anchor can be tens of km off, which
## shifts globe terrain features but leaves the sag correction intact).
func _rebase_globe_to(flat_x: float, flat_z: float, lat_deg: float, lon_deg: float, quiet := false) -> void:
	# Keep the sun/observer in sync so solar elevation and local mean-solar
	# time match the new region (sky_atmosphere.gd reads these every frame).
	_sky.observer_lat = lat_deg
	_sky.observer_lon = lon_deg
	if not (USE_WORLD3D_GLOBE and is_instance_valid(globe_node) and _w3d_coords != null):
		return
	var f_local: Transform3D = _w3d_coords.enu_to_ecef_transform(lat_deg, lon_deg, 0.0, true)
	_ecef_from_flat = f_local * Transform3D(Basis.IDENTITY, Vector3(-flat_x, 0.0, -flat_z))
	_globe_mount.transform = _ecef_from_flat.affine_inverse()
	# Bookkeeping for the continuous tracker: this is now the anchor.
	_globe_anchor_flat = Vector2(flat_x, flat_z)
	_globe_has_anchor = true
	if not quiet:
		print("World3D globe rebased to airport: flat(%.0f, %.0f) -> lat=%.5f lon=%.5f" % [
			flat_x, flat_z, lat_deg, lon_deg])


## Continuous globe tracking (design §8.1 Phase 2 / decision D2): keep the
## tangent point under the aircraft so the coarse globe stays glued to the
## flat patch everywhere instead of sagging d²/2R away from it as you fly.
## Re-anchors at most once per GLOBE_REBASE_TRACK_M of travel, so a parked AC
## costs nothing and in flight the mount moves at (quantized) aircraft speed —
## no discrete transform events, and the one-shot destination rebase in
## _on_welcome_flight_requested() remains the initial anchor under the loading
## screen. The Geo.lv95_to_wgs84 polynomial is the only approximation in the
## chain; its error grows with distance from CH (tens of km at 70°N), which
## shifts globe terrain features but not the sag correction itself.
func _track_globe_rebase() -> void:
	if not (USE_WORLD3D_GLOBE and is_instance_valid(globe_node) and _w3d_coords != null):
		return
	var ac := $AC as Node3D
	if not ac:
		return
	var p: Vector3 = ac.global_position
	var q := Vector2(p.x, p.z)
	if _globe_has_anchor and _globe_anchor_flat.distance_squared_to(q) < GLOBE_REBASE_TRACK_M * GLOBE_REBASE_TRACK_M:
		return
	# Flat X is West-positive around TERRAIN_CENTER_E (see the _ecef_from_flat
	# comment), so LV95 easting = CENTER_E - x, northing = CENTER_N + z.
	var ll: Vector2 = Geo.lv95_to_wgs84(TERRAIN_CENTER_E - p.x, TERRAIN_CENTER_N + p.z)
	_rebase_globe_to(p.x, p.z, ll.x, ll.y, true)


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
	if cam:
		c = Vector2(cam.global_position.x, cam.global_position.z)
	else:
		var ac := $AC as Node3D
		if not ac:
			return
		c = Vector2(ac.global_position.x, ac.global_position.z)

	var tm = _get_tile_manager()
	var gen := -1
	if tm != null:
		gen = int(tm.get_coverage_generation())
	if not _suppress_dirty and gen == _suppress_cov_gen \
			and _suppress_last_center.distance_squared_to(c) < 1024.0:
		return  # nothing moved, no tile/chunk appeared or disappeared

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
## (cam_ei, cam_ni); each cell spans tile_m metres. The mask maps flat-frame
## XZ to cells: X = TERRAIN_CENTER_E - E runs opposite to the east index
## (negative step), Z = N - TERRAIN_CENTER_N runs with it.
func _push_suppress_from_coverage(cells: PackedByteArray, r_tiles: int,
		cam_ei: int, cam_ni: int, tile_m: float) -> void:
	var n := 2 * r_tiles + 1
	if cells.size() != n * n or tile_m <= 0.0:
		globe_node.clear_coverage_mask()
		return
	var e0 := float(cam_ei - r_tiles) * tile_m
	var n0 := float(cam_ni - r_tiles) * tile_m
	globe_node.set_coverage_mask(cells, n,
			TERRAIN_CENTER_E - e0, n0 - TERRAIN_CENTER_N, -tile_m, tile_m)


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


## Push the time-of-day settings (system clock vs manual local time) into the
## sky controller. Called at setup and whenever the settings screen closes.
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
func _start_intro_zoom() -> void:
	if not USE_WORLD3D_GLOBE or not is_instance_valid(globe_node) or not _globe_mount:
		return
	var ac_node := get_node_or_null("AC")
	if not ac_node or not is_instance_valid(_intro_camera):
		return
	if _intro_zoom.start():
		_intro_playing = true
		active_camera = _intro_camera




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
	var target_x: float
	var target_z: float
	var target_heading: float
	if _first_flight_target.has("godot_x"):
		target_x = _first_flight_target["godot_x"]
		target_z = _first_flight_target["godot_z"]
		target_heading = _first_flight_target["heading_deg"]
	else:
		push_error("_do_deferred_terrain_init: _first_flight_target empty — using current AC position as fallback")
		target_x = ac_node.global_position.x
		target_z = ac_node.global_position.z
		target_heading = -rad_to_deg(ac_node.rotation.y)

	# Print the target only once per second while we wait for terrain to
	# stream in — otherwise this fires 60×/sec and floods the log.
	_terrain_init_print_timer -= 1
	if _terrain_init_print_timer <= 0:
		_terrain_init_print_timer = 60
		print("=== _do_deferred_terrain_init target: x=%.1f z=%.1f hdg=%.1f° (dict_keys=%s) ===" % [
			target_x, target_z, target_heading, str(_first_flight_target.keys())])
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

	# Move aircraft to target position
	ac_node.global_position.x = target_x
	ac_node.global_position.z = target_z
	ac_node.rotation.y = -deg_to_rad(target_heading)

	var start_pos = Vector3(ac_node.global_position.x, 0, ac_node.global_position.z)
	# Spawn on the PRECISE scenery3d patch when available: wait for the Swiss/BW
	# tile to stream in rather than initialising on the coarse globe and then
	# jumping ~100+ m when the fine tile loads.
	var api_height: float = _scenery_elevation_at(start_pos.x, start_pos.z)
	if is_nan(api_height):
		var waited := Time.get_ticks_msec() - _pending_terrain_init_start_msec
		if waited < TERRAIN_INIT_FALLBACK_MSEC:
			_pending_terrain_init = true
			return
		# No detailed patch here (outside CH/BW, or partial install): fall back to
		# the coarse global globe so we can still spawn instead of hanging.
		api_height = _globe_elevation_at(start_pos.x, start_pos.z)
		if is_nan(api_height):
			api_height = 0.0
		push_warning("Scenery patch elevation unavailable at (%.0f, %.0f) after %.0fs — spawning on globe/fallback elevation %.0f m ASL" % [start_pos.x, start_pos.z, waited / 1000.0, api_height])
	else:
		print("=== Scenery3D get_elevation(%.1f, %.1f) = %.1f m ===" % [start_pos.x, start_pos.z, api_height])

	# Position aircraft on the terrain (Godot Y coordinate)
	# JSBSim will take over position control after initialization.
	# Spawn just a hair above terrain so JSBSim's gear springs barely have to settle.
	var gear_height = 0.5
	var new_y = api_height + gear_height
	print("=== Initial aircraft position at Godot Y: %.1f m (terrain: %.1f m) ===" % [new_y, api_height])
	ac_node.global_position.y = new_y

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
	print("=== JSBSim initialized at Godot pos: ", start_godot_pos, " heading: ", target_heading, "° ===")

	terrain_initialized = true
	print("=== JSBSim terrain initialization complete ===")


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
	_terrain_elevation.process_rebase(jsb_node, _terrain_elevation_at(pin.x, pin.z), is_instance_valid(scenery3d_node))

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
	_terrain_elevation.update_terrain_elevation(jsb_node, _terrain_elevation_at(ac.global_position.x, ac.global_position.z), is_instance_valid(scenery3d_node))


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
	if time_label:
		time_label.text = "Time: " + _sky.get_time_string()

	# While loading screen is active, update progress and wait for completion
	if not _loading_done:
		_update_loading_progress(false)  # initial startup loading
		return

	# Post-Fly loading: terrain/buildings/roads/water settling at destination
	if _fly_loading:
		# Process rebase cooldown while loading screen is shown
		_process_rebase_cooldown()
		_update_loading_progress(true)  # fly-to-destination loading
		return

	# Continuous globe rebase tracking (design §8.1 Phase 2): keep the tangent
	# point under the aircraft so the coarse globe never sags away from the
	# flat patch. No-op while parked; runs in flight, slew and position dialog.
	_track_globe_rebase()
	# Keep the coarse-globe suppression clipped to Scenery3D's live coverage
	# square around the camera (no holes to space beyond the streaming radius).
	_refresh_globe_suppress_rects()

	# VFR map frame (C++ module): position/heading drive + overlay gating.
	# The map freezes under full-screen UI; input pass-through is handled in C++.
	var _vfr_ui_up := (_welcome_screen and _welcome_screen.visible) or (_settings_screen and _settings_screen.visible)
	if jsb_node:
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
				var slew_heading := fmod(-rad_to_deg(ac.rotation.y) + 360.0, 360.0)
				_vfr_map.update(ac.global_position, slew_heading, _vfr_ui_up)
		else:
			_vfr_map.update(jsb_node.get_godot_position(), jsb_node.get_heading(), _vfr_ui_up)

	# Skip all flight logic while welcome or settings overlay is visible
	if _welcome_screen and _welcome_screen.visible:
		return
	if _settings_screen and _settings_screen.visible:
		return

	# Cinematic space→spot zoom on entering fly mode (world3d globe). The C++
	# IntroZoom module owns the animation + the CameraRig orbit handoff; sync
	# the camera_mode/active_camera mirrors when it reports the handoff.
	if _intro_playing:
		if _intro_zoom.update(delta) == IntroZoom.STATUS_FINISHED:
			_intro_playing = false
			camera_mode = CameraMode.ORBIT
			active_camera = camera2
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

	# Toggle FPS display with Ctrl+Shift+F
	if Input.is_key_pressed(KEY_CTRL) and Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_F):
		if not _fps_key_held:
			_fps_key_held = true
			_fps_visible = !_fps_visible
			if _fps_label:
				_fps_label.visible = _fps_visible
	else:
		_fps_key_held = false

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
		_terrain_elevation_at($AC.global_position.x, $AC.global_position.z),
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
		heading_label.text = "Heading: " +str(round(heading))
		
		# Coordinate display
		var godot_pos = jsb_node.get_godot_position()
		# Convert Godot position to Swiss LV95 coordinates
		# Godot X = TERRAIN_CENTER_E - Swiss_E  =>  Swiss_E = TERRAIN_CENTER_E - Godot_X
		# Godot Z = Swiss_N - TERRAIN_CENTER_N  =>  Swiss_N = TERRAIN_CENTER_N + Godot_Z
		var swiss_e = TERRAIN_CENTER_E - godot_pos.x
		var swiss_n = TERRAIN_CENTER_N + godot_pos.z
		$Control/Label6.text = "Godot: X=%.1f Y=%.1f Z=%.1f" % [godot_pos.x, godot_pos.y, godot_pos.z]
		$Control/Label7.text = "Swiss E: %s, N: %s" % [format_swiss_coord(swiss_e), format_swiss_coord(swiss_n)]
		# Prime Meridian (WGS84) display — world-scenery design §3.4
		var ll: Vector2 = Geo.lv95_to_wgs84(swiss_e, swiss_n)
		$Control/Label9.text = "PM: " + format_pm_coord(ll.x, true) + "  " + format_pm_coord(ll.y, false)
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
		propeller_cone_node.visible = false
		_control_surfaces.register_propeller_cone(propeller_cone_node)

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
		_cockpit_controls.register_debug_bodies(mixture_lever_body, throttle_lever_body)
		if mixture_lever_node:
			_cockpit_controls.apply_mixture_value(jsb_node)
		if magneto_switch_node:
			_cockpit_controls.apply_magneto_position(jsb_node)
		if flap_lever_node:
			_cockpit_controls.sync_flaps_from_jsb(jsb_node)


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
	if _aircraft_cfg == null or not _gear_retractable:
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
	var sample_x := 0.0
	var sample_z := 0.0
	if is_fly_loading and _first_flight_target.has("godot_x"):
		sample_x = float(_first_flight_target["godot_x"])
		sample_z = float(_first_flight_target["godot_z"])
	else:
		var ac_node := get_node_or_null("AC")
		if ac_node:
			sample_x = ac_node.global_position.x
			sample_z = ac_node.global_position.z

	var samples := [
		Vector2(sample_x, sample_z),
		Vector2(sample_x + RING_RADIUS_M, sample_z),
		Vector2(sample_x - RING_RADIUS_M, sample_z),
		Vector2(sample_x, sample_z + RING_RADIUS_M),
		Vector2(sample_x, sample_z - RING_RADIUS_M),
	]
	var total := samples.size()
	var loaded := 0
	if USE_WORLD3D_GLOBE:
		# The globe has whole-planet elevation immediately, so terrain is always
		# "loaded" — this makes the fly-to-airport screen dismiss promptly.
		for s in samples:
			var h: float = _terrain_elevation_at(s.x, s.y)
			if not is_nan(h):
				loaded += 1
	elif is_instance_valid(scenery3d_node) and scenery3d_node.has_method("get_elevation"):
		for s in samples:
			var h: float = scenery3d_node.get_elevation(s.x, s.y)
			if not is_nan(h) and h != 0.0:
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

	if not _loading_progress.is_complete():
		return

	if is_fly_loading:
		# Post-Fly loading complete — dismiss and start flying
		_fly_loading = false
		if _loading_screen:
			_loading_screen.hide_loading()
		print("Fly loading complete — simulation active")
		# Kick off the cinematic space→spot zoom over the globe.
		_start_intro_zoom()
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
	_welcome_screen.populate_airports(AirportsLib.load_default())

	_welcome_screen.show_welcome()


func _create_settings_screen() -> void:
	_settings_screen = SettingsScreen.new()
	add_child(_settings_screen)  # loads provpilot.cfg in its _ready
	_settings = _settings_screen.get_store()
	_settings_screen.closed.connect(_on_settings_closed)


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

	# Adopt the airframe the user selected (reloads aircraft.cfg and asks the
	# JSBSim extension to load the matching flight model) before we position
	# and initialise the simulation.
	_apply_selected_aircraft(aircraft_id)

	# Show loading screen while destination loads
	_fly_loading = true
	_loading_progress.begin_fly_loading()
	if _loading_screen:
		_loading_screen.set_progress(0.0)
		_loading_screen.set_status("Loading terrain...")
		_loading_screen.show_loading()

	# Rebase the coarse globe tangent point under the destination airport so
	# the horizon/atmosphere and globe-terrain alignment stay correct in far
	# regions (ENGM is ~1400 km from the LV95 origin, where the fixed Swiss
	# tangent frame sags the globe ~157 km below the flat terrain). The flat
	# frame itself is unchanged — only the ECEF<->flat mapping moves.
	if USE_WORLD3D_GLOBE:
		var gx := TERRAIN_CENTER_E - swiss_e
		var gz := swiss_n - TERRAIN_CENTER_N
		var ll: Vector2 = Geo.lv95_to_wgs84(swiss_e, swiss_n)
		_rebase_globe_to(gx, gz, ll.x, ll.y)
		_suppress_dirty = true
		_refresh_globe_suppress_rects()

	if _first_flight:
		_do_first_flight(swiss_e, swiss_n, heading_deg)
		_first_flight = false
	else:
		_on_teleport_requested(swiss_e, swiss_n, heading_deg)


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

	# TERRAIN_CENTER is fixed (matches Scenery3D's origin_east/north from
	# ProjectSettings). Compute Godot position relative to that origin.
	print("  Terrain center: E=%d N=%d" % [int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N)])
	var godot_x = TERRAIN_CENTER_E - swiss_e
	var godot_z = swiss_n - TERRAIN_CENTER_N

	# 3. Position aircraft at target (Y will be corrected after terrain loads)
	ac_node.global_position = Vector3(godot_x, 500.0, godot_z)
	ac_node.rotation.y = -deg_to_rad(heading_deg)

	# Store target for deferred JSBSim init (uses these in _do_deferred_terrain_init)
	_first_flight_target = {
		"godot_x": godot_x,
		"godot_z": godot_z,
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

	# TERRAIN_CENTER is fixed (matches Scenery3D origin). No rebase — just
	# compute new Godot position relative to the fixed origin.
	var new_godot_x = TERRAIN_CENTER_E - swiss_e
	var new_godot_z = swiss_n - TERRAIN_CENTER_N

	# 8. Get terrain height at new position — sample a grid around the
	#    threshold and use the maximum, matching the runway mesh approach.
	var terrain_height := 0.0
	if is_instance_valid(scenery3d_node):
		var max_h := -1e9
		var heading_rad := deg_to_rad(heading_deg)
		# Sample centre + 4 offsets along/across runway (±25 m each way)
		var offsets: Array = [
			Vector3(0, 0, 0),
			Vector3(-sin(heading_rad) * 25.0, 0, cos(heading_rad) * 25.0),
			Vector3(sin(heading_rad) * 25.0, 0, -cos(heading_rad) * 25.0),
			Vector3(cos(heading_rad) * 15.0, 0, sin(heading_rad) * 15.0),
			Vector3(-cos(heading_rad) * 15.0, 0, -sin(heading_rad) * 15.0),
		]
		for off in offsets:
			var h := _terrain_elevation_at(new_godot_x + off.x, new_godot_z + off.z)
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
	var gear_height := 2.0
	ac_node.global_position = Vector3(new_godot_x, terrain_height + runway_surface_offset + gear_height, new_godot_z)
	ac_node.rotation.y = -deg_to_rad(heading_deg)

	print("  New Godot pos: ", ac_node.global_position)
	print("  New center: E=%d N=%d" % [int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N)])

	# 10. Reinitialize JSBSim at the new position but keep slew mode ON.
	#     JSBSim stays paused during cooldown; we reinit again when terrain
	#     has loaded and the cooldown expires (in _process).
	if jsb_node.has_method("reinitialize_from_godot_position"):
		jsb_node.reinitialize_from_godot_position(ac_node.global_position, ac_node.rotation.y, terrain_height)
	_update_terrain_elevation()

	# 11. Keep JSBSim in slew mode — physics stays paused until cooldown expires.
	#     The C++ module's state + HUD are cleared so the slew UI is not shown.
	_slew_mode.note_external_exit()

	# Arm the rebase cooldown (C++ module): pin the AC here while JSBSim stabilises.
	_terrain_elevation.start_rebase(ac_node.global_position)

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
		if _mixture_dragging:
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
