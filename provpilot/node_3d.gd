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

# Assuming your JSBGodot node is a child of the current node
var jsb_node: Node
var active_camera: Camera3D
var camera1: Camera3D
var camera2: Camera3D
var camera1_base_transform: Transform3D  # Store original camera transform
var _camera1_scene_transform: Transform3D  # Pristine Camera1 transform from the scene
var _camera1_scene_fov: float = 0.0        # Pristine Camera1 fov from the scene

# Pilot head look angles (in degrees) - for cockpit camera
var head_yaw: float = 0.0    # Left/Right
var head_pitch: float = 0.0  # Up/Down
const HEAD_YAW_MAX: float = 120.0   # Max look left/right
const HEAD_PITCH_MAX: float = 60.0  # Max look up/down
const HEAD_LOOK_SPEED: float = 90.0 # Degrees per second

# Camera mode enum
enum CameraMode { COCKPIT, ORBIT, CHASE }
var camera_mode: int = CameraMode.COCKPIT

# Cinematic space→spot intro zoom (world3d globe). When entering fly mode we
# start high above the Earth (whole globe visible) and smoothly zoom down to the
# external "spot" view of the aircraft, then hand off to the normal orbit camera.
var _intro_playing: bool = false
var _intro_elapsed: float = 0.0
const INTRO_DURATION: float = 6.0
const INTRO_START_ALT_M: float = 1.6e7   # metres above the aircraft (space view)
var _intro_up_dir: Vector3 = Vector3.UP
var _intro_start_pos: Vector3 = Vector3.ZERO
# Dedicated camera for the intro, parented to the ROOT (not the aircraft) so its
# space position is not dragged around by JSBSim's per-frame AC transform writes.
var _intro_camera: Camera3D = null

# Spot camera orbit angles (in degrees) - for external camera
var orbit_yaw: float = 135.0    # Horizontal angle around aircraft (start behind-left)
var orbit_pitch: float = 15.0   # Vertical angle (elevation)
var orbit_distance: float = 6.0   # Distance from aircraft (default view)
const ORBIT_PITCH_MIN: float = -10.0   # Min elevation (slightly below)
const ORBIT_PITCH_MAX: float = 80.0    # Max elevation (almost top-down)
const ORBIT_SPEED: float = 90.0        # Degrees per second
const ORBIT_ZOOM_SPEED: float = 10.0   # Units per second
const ORBIT_DISTANCE_MIN: float = 1.5  # Closest zoom (scaled for 0.1x model)
const ORBIT_DISTANCE_MAX: float = 50.0 # Furthest zoom

# Chase camera parameters
var chase_distance: float = 8.0    # Distance behind aircraft
var chase_height: float = 3.0     # Height above aircraft
var chase_yaw: float = 0.0        # Yaw offset relative to aircraft heading (degrees)
var chase_smooth: float = 5.0     # Smoothing factor (higher = faster follow)

# Control surfaces, propeller and landing-gear animation live in the C++
# ControlSurfaces module (src/control_surfaces.cpp). Per-aircraft tunables are
# pushed from aircraft.cfg in _load_aircraft_config; model nodes are registered
# (with their base transforms captured) in _resolve_aircraft_nodes /
# _resolve_gear_legs. The smoothed visual RPM is read back via get_visual_rpm().
var _control_surfaces := ControlSurfaces.new()

# Rudder indicator pivot tweak (debug drag on screen, not part of the animator)
var rudder_pivot_offset: Vector3 = Vector3(0, 0, 0)

# Propeller blur-disc creation parameters (the disc mesh itself is built here
# in GDScript; spin/blur/cone animation is done by ControlSurfaces).
var PROPELLER_MAX_RPM: float = 2700.0  # Max RPM for reference (cfg propeller.max_rpm)
var PROPELLER_DISC_RADIUS: float = 0.95  # Radius of blur disc in meters (cfg propeller.disc_radius_m)
var PROPELLER_SPIN_AXIS: Vector3 = Vector3.BACK  # Local axis the prop spins about (cfg propeller.spin_axis: x/y/z)
var PROPELLER_DISC_OFFSET: Vector3 = Vector3.ZERO  # Disc translation tweak in parent space (cfg propeller.disc_offset)

# Retractable landing gear (cfg [gear]). The legs themselves are registered
# with the C++ ControlSurfaces module in _resolve_gear_legs(); it rotates each
# leg's wheel + strut RIGIDLY about a shared local pivot (in the model wrapper
# frame) so the whole leg sweeps up as one unit, or drives an imported
# retract/extend clip by seeking instead.
var _gear_retractable: bool = false
var _gear_down: bool = true             # Commanded state (true = extended)
var _gear_key_held: bool = false        # Debounce for the G key
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
# Atmospheric fog is exponential (density-based), so it saturates to the fog
# colour at very large distances — which turns the whole Earth into a white disc
# when viewed from space. Fade the fog density out with camera altitude so we
# keep near-ground haze but see oceans/continents from orbit.
var _world_env: Environment = null
var _fog_base_density: float = 0.0
const FOG_FULL_ALT_M := 4000.0    # full fog at/below this camera altitude (m ASL)
const FOG_ZERO_ALT_M := 60000.0   # no fog at/above this camera altitude (m ASL)
# Fade the sky background to black as we leave the atmosphere (space = black).
# The sun still lights the globe (directional light is unaffected).
var _bg_base_energy: float = 1.0
var _ambient_base_energy: float = 1.0
var _glow_base_intensity: float = 0.0
const SKY_FULL_ALT_M := 12000.0    # full sky at/below this camera altitude (m ASL)
const SKY_BLACK_ALT_M := 100000.0  # black background at/above this altitude (m ASL)
const AMBIENT_SPACE_ENERGY := 0.35 # ambient light energy in space (vs bright ground value)
# The coarse global globe (~2 km ETOPO) would poke through the fine Swiss/BW
# scenery3d patch, so hide it below this camera altitude (the detailed patch is
# the ground there) and show it above (high-altitude / space backdrop).
const GLOBE_SHOW_ALT_M := 25000.0

# Tree density mapping (index = tree_density setting value).
# Off / Low / Medium / High / Very High
const TREE_DENSITY_FRACTION := [0.0, 0.25, 0.50, 0.75, 1.0]
# Fixed render radius (m) when vegetation is enabled. Density controls
# the number of trees within this radius, not the radius itself.
const TREE_LOAD_RADIUS_M := 1500

# Slew mode - free movement without physics
var slew_mode: bool = false
var slew_speed: float = 400.0  # Base speed in m/s
var slew_fast_multiplier: float = 5.0  # Speed multiplier when holding boost
var slew_vertical_speed: float = 60.0  # Vertical movement speed
var _slew_key_held: bool = false  # Prevent key repeat

# Teleport cooldown: pin the aircraft to the teleport target until JSBSim
# has stabilised.  C++ _physics_process sets the AC position directly from
# JSBSim output, which can be garbage on the first frames after reinitialize;
# we override it in _process.
var _rebase_cooldown: int = 0
const REBASE_COOLDOWN_FRAMES: int = 120  # ~2 seconds at 60 fps
var _teleport_pin_pos: Vector3 = Vector3.ZERO  # Position to hold AC at during cooldown

# Position dialog (Shift+P)
var _position_dialog: Window = null
var _position_key_held: bool = false
var _pre_slew_airspeed_kts: float = 0.0  # saved airspeed for slew mode restore

# Loading / Welcome / Settings screens
var _loading_screen: CanvasLayer = null
var _loading_done: bool = false
var _fly_loading: bool = false   # True while loading after user clicks Fly!
var _fly_loading_start_msec: int = 0  # Timestamp when fly loading started (ms)
const FLY_LOADING_MAX_MSEC: int = 45000  # Hard cap on fly loading screen (45s)
const FLY_LOADING_NODATA_MSEC: int = 15000  # Give up faster when zero tiles load (15s — must exceed TERRAIN_INIT_FALLBACK_MSEC)
var _welcome_screen: CanvasLayer = null
var _settings_screen: CanvasLayer = null

# Deferred scenery loading: skip all heavy loading at startup, load on first Fly!
var _first_flight: bool = true
var _first_flight_target: Dictionary = {}  # {swiss_e, swiss_n, heading_deg}

# ── Interactive cockpit mixture lever ──────────────────────────────────────
var mixture_lever_node: Node3D           # The lever mesh node
var mixture_lever_body: StaticBody3D     # Runtime physics body for raycasting
var mixture_lever_base_transform: Transform3D  # Original transform at mixture=0 (cutoff/idle)
var mixture_value: float = 1.0           # 0.0 = cutoff, 1.0 = full rich
var _mixture_dragging: bool = false      # True while mouse is dragging the lever
var _mixture_drag_start_y: float = 0.0   # Screen Y at drag start
var _mixture_drag_start_value: float = 0.0  # Mixture value at drag start
var _mixture_key_held: bool = false          # Debounce for M key
const MIXTURE_LEVER_TRAVEL: float = 0.05  # Lever travel in local units (≈2 cm)
const MIXTURE_LEVER_COLLISION_LAYER: int = 16  # Physics layer for lever picking
const MIXTURE_KEY_STEP: float = 0.05  # Mixture change per key press (5%)
var _mixture_debug_visible: bool = false  # Show collision box for debugging

# ── Interactive cockpit throttle lever ─────────────────────────────────────
var throttle_lever_node: Node3D
var throttle_lever_body: StaticBody3D
var throttle_lever_base_transform: Transform3D  # Original transform at throttle=0 (idle)
var _throttle_dragging: bool = false
var _throttle_drag_start_y: float = 0.0
var _throttle_drag_start_value: float = 0.0
var _throttle_key_held: bool = false
const THROTTLE_LEVER_TRAVEL: float = 0.05  # Same travel as mixture lever
const THROTTLE_LEVER_COLLISION_LAYER: int = 32  # Separate physics layer
const THROTTLE_KEY_STEP: float = 0.05  # 5% per key press
var _throttle_debug_visible: bool = false

# ── Interactive cockpit magneto switch ─────────────────────────────────────
var magneto_switch_node: Node3D            # The switch mesh node
var magneto_switch_body: StaticBody3D      # Runtime physics body for raycasting
var magneto_switch_base_transform: Transform3D  # Original transform (BOTH position)
var magneto_position: int = 3              # 0=OFF, 1=R, 2=L, 3=BOTH, 4=START
var _magneto_dragging: bool = false
var _magneto_drag_start_x: float = 0.0    # Screen X at drag start
var _magneto_drag_start_pos: int = 3       # Position at drag start
var _magneto_key_held: bool = false
var _magneto_last_printed_pos: int = -1  # Dedup prints
const MAGNETO_SWITCH_COLLISION_LAYER: int = 64  # Separate physics layer
const MAGNETO_STEP_ANGLE: float = 30.0     # Degrees per switch position
const MAGNETO_DEFAULT_POSITION: int = 3    # Switch position the Blender rest pose represents (BOTH)
const MAGNETO_BASE_POSITION: float = 6.5   # Legacy: model default orientation (between R and L)
# Angle offsets relative to base: OFF=-45, R=-15, L=+15, BOTH=+45, START=+75
const MAGNETO_ANGLE_OFFSET: float = -95.0  # Angle of pos 0 (OFF) relative to base

# ── Interactive cockpit flap lever ─────────────────────────────────────────
var flap_lever_node: Node3D              # The lever mesh node
var flap_lever_body: StaticBody3D        # Runtime physics body for raycasting
var flap_lever_base_transform: Transform3D  # Original transform at flaps=0 (UP)
var flap_position: int = 0               # 0=UP, 1=10°, 2=20°, 3=30°
var _flap_dragging: bool = false
var _flap_drag_start_y: float = 0.0      # Screen Y at drag start
var _flap_drag_start_pos: int = 0        # Position at drag start
var _flap_key_held: bool = false
var _flap_last_printed_pos: int = -1     # Dedup prints
const FLAP_LEVER_COLLISION_LAYER: int = 128  # Separate physics layer
const FLAP_LEVER_TRAVEL: float = -0.01    # Total lever travel along Y per notch
var FLAP_POSITIONS: Array = [0.0, 0.333, 0.667, 1.0]  # JSBSim flap-cmd-norm values (cfg flaps.positions)
var FLAP_NAMES: Array = ["UP", "10°", "20°", "30°"]  # HUD labels (cfg flaps.names)

# ── Engine sound ───────────────────────────────────────────────────────────
var _engine_audio: AudioStreamPlayer = null
var ENGINE_SOUND_PATH: String = "res://sounds/c172_engine_inflight.ogg"  # cfg sound.engine
# RPM-to-pitch mapping: idle (~700 RPM) → 0.7, max (2700 RPM) → 1.6 (cfg sound.pitch_*)
var ENGINE_PITCH_MIN: float = 0.7
var ENGINE_PITCH_MAX: float = 1.6
# RPM-to-volume mapping (before user volume slider): idle → -12 dB, max → +3 dB (cfg sound.vol_db_*)
var ENGINE_VOL_DB_MIN: float = -12.0
var ENGINE_VOL_DB_MAX: float = 3.0
# Below this RPM the engine sound fades out completely
const ENGINE_SOUND_FADE_RPM: float = 50.0

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
	camera1_base_transform = camera1.transform  # Store base transform
	_camera1_scene_transform = camera1.transform  # Pristine scene value for swaps
	_camera1_scene_fov = camera1.fov
	print("Camera1: ", camera1)
	print("Camera2: ", camera2)

	# The C172P .blend import bakes normals with the opposite handedness from
	# Godot's runtime convention, so without this the sun lights the underside
	# of the wings instead of the top. Flip them in-place once at startup.
	# Controlled by aircraft.cfg [model] flip_normals / flip_normals_prefix.
	_apply_model_normals(ac_node)

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
	# Apply saved tree density now that _settings_screen has loaded its config.
	_apply_tree_density()
	# Apply scenery sources (orthophoto and terrain paths) from settings
	_apply_scenery_sources()

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
	print("Engine sound loaded and playing (looped)")


func _update_engine_sound() -> void:
	if _engine_audio == null:
		return

	# Use the C++ animator's smoothed RPM (handles engine-off decay)
	var rpm := _control_surfaces.get_visual_rpm()

	# Fade to silence below threshold
	if rpm < ENGINE_SOUND_FADE_RPM:
		_engine_audio.volume_db = -80.0
		return

	# Normalise RPM to 0-1 range (idle ~700 = 0.26, max 2700 = 1.0)
	var rpm_norm = clampf(rpm / PROPELLER_MAX_RPM, 0.0, 1.0)

	# Pitch: linear interpolation
	_engine_audio.pitch_scale = lerpf(ENGINE_PITCH_MIN, ENGINE_PITCH_MAX, rpm_norm)

	# Volume: linear interpolation in dB, then apply user engine_volume setting
	var base_db = lerpf(ENGINE_VOL_DB_MIN, ENGINE_VOL_DB_MAX, rpm_norm)
	var user_vol_pct: float = 70.0  # default
	if _settings_screen and "settings" in _settings_screen:
		user_vol_pct = float(_settings_screen.settings.get("engine_volume", 70))
	# Convert 0-100% to dB offset: 0% → -80 dB (silent), 100% → 0 dB (no attenuation)
	var user_db_offset = lerpf(-80.0, 0.0, user_vol_pct / 100.0)
	_engine_audio.volume_db = base_db + user_db_offset


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
## the flat frame origin. The globe reads the camera via to_local(), so it still
## computes the correct ECEF camera position for LOD/streaming internally.
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
	# Cache the scene Environment so we can fade fog with altitude (the globe
	# would otherwise wash out to the fog colour when seen from space).
	var we := get_node_or_null("Environment/WorldEnvironment")
	if we and we.environment:
		_world_env = we.environment
		_fog_base_density = _world_env.fog_density
		_bg_base_energy = _world_env.background_energy_multiplier
		_ambient_base_energy = _world_env.ambient_light_energy
		_glow_base_intensity = _world_env.glow_intensity
	print("World3D globe mounted: LV95 %d/%d -> lat=%.5f lon=%.5f" % [
		int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N), origin_lat, origin_lon])


## Begin the cinematic space→spot zoom (called when fly-loading completes).
## Animates the external spot camera (camera2) from high above the Earth down to
## the normal orbit "spot" view of the aircraft, then hands off to orbit mode.
func _start_intro_zoom() -> void:
	if not USE_WORLD3D_GLOBE or not is_instance_valid(globe_node) or not _globe_mount:
		return
	var ac_node := get_node_or_null("AC")
	if not ac_node or not is_instance_valid(_intro_camera):
		return
	# Start the camera straight up (local +Y) above the aircraft. Using the true
	# radial direction tilts it a few km horizontally (the aircraft sits off the
	# tangent origin), which makes the scenery managers stream tiles around the
	# wrong spot during the descent. Straight up keeps streaming centred on the
	# aircraft so detailed buildings load directly instead of far-first.
	_intro_up_dir = Vector3.UP
	_intro_start_pos = ac_node.global_position + _intro_up_dir * INTRO_START_ALT_M
	_intro_elapsed = 0.0
	_intro_playing = true
	# Drive the dedicated root-level intro camera.
	var ac_pos: Vector3 = ac_node.global_position
	_intro_camera.global_position = _intro_start_pos
	var fwd0: Vector3 = (ac_pos - _intro_start_pos).normalized()
	var up0: Vector3 = Vector3(0, 0, -1) if absf(fwd0.dot(Vector3.UP)) > 0.98 else Vector3.UP
	_intro_camera.look_at(ac_pos, up0)
	active_camera = _intro_camera
	if camera1:
		camera1.current = false
	if camera2:
		camera2.current = false
	_intro_camera.current = true
	print("World3D intro zoom started (space → spot)")


## Per-frame update of the intro zoom. Log-interpolates distance (many orders of
## magnitude) and slerps the view direction from straight-down to the spot angle.
func _update_intro_zoom(delta: float) -> void:
	var ac_node := get_node_or_null("AC")
	if not ac_node or not is_instance_valid(_intro_camera):
		_end_intro_zoom()
		return
	# Allow the user to skip the cinematic.
	if Input.is_key_pressed(KEY_ESCAPE) or Input.is_key_pressed(KEY_SPACE):
		_end_intro_zoom()
		return

	_intro_elapsed += delta
	var p: float = clampf(_intro_elapsed / INTRO_DURATION, 0.0, 1.0)
	var pe: float = smoothstep(0.0, 1.0, p)  # ease in/out

	var target: Vector3 = ac_node.global_position

	# Spot (orbit) end offset — matches handle_orbit_camera exactly for a
	# seamless handoff.
	var yaw_rad := deg_to_rad(orbit_yaw)
	var pitch_rad := deg_to_rad(orbit_pitch)
	var spot_offset := Vector3(
		orbit_distance * cos(pitch_rad) * sin(yaw_rad),
		orbit_distance * sin(pitch_rad),
		orbit_distance * cos(pitch_rad) * cos(yaw_rad))

	var start_dir := _intro_up_dir
	var end_dir := spot_offset.normalized()
	var start_dist := INTRO_START_ALT_M
	var end_dist := maxf(spot_offset.length(), 0.5)

	# Log distance interpolation feels linear in "orders of magnitude".
	var dist: float = exp(lerpf(log(start_dist), log(end_dist), pe))
	# Keep the view top-down while high, swing to the spot angle near the end.
	var dir := start_dir.slerp(end_dir, pe * pe)
	var cam_pos := target + dir * dist
	_intro_camera.global_position = cam_pos

	# Robust up vector: avoid look_at degeneracy when peering straight down.
	var fwd := (target - cam_pos).normalized()
	var ref_up := Vector3.UP
	if absf(fwd.dot(ref_up)) > 0.98:
		ref_up = Vector3(0, 0, -1)
	_intro_camera.look_at(target, ref_up)

	if p >= 1.0:
		_end_intro_zoom()


## Finish the intro and hand control to the normal orbit spot camera.
func _end_intro_zoom() -> void:
	if not _intro_playing:
		return
	_intro_playing = false
	camera_mode = CameraMode.ORBIT
	active_camera = camera2
	# Position camera2 at the spot immediately so the handoff has no 1-frame gap.
	var ac_node := get_node_or_null("AC")
	if ac_node and is_instance_valid(camera2):
		var yaw_rad := deg_to_rad(orbit_yaw)
		var pitch_rad := deg_to_rad(orbit_pitch)
		var spot_offset := Vector3(
			orbit_distance * cos(pitch_rad) * sin(yaw_rad),
			orbit_distance * sin(pitch_rad),
			orbit_distance * cos(pitch_rad) * cos(yaw_rad))
		camera2.global_position = ac_node.global_position + spot_offset
		if spot_offset.length_squared() > 0.01:
			camera2.look_at(ac_node.global_position, Vector3.UP)
	if is_instance_valid(_intro_camera):
		_intro_camera.current = false
	if is_instance_valid(camera2):
		camera2.current = true
	print("World3D intro zoom complete — spot view active")


## Scale exponential fog density by the rendering camera's altitude so the globe
## is fully visible from space (fog off) but keeps atmospheric haze near ground.
func _update_atmosphere() -> void:
	if not USE_WORLD3D_GLOBE or _world_env == null:
		return
	var vp := get_viewport()
	var cam: Camera3D = vp.get_camera_3d() if vp else null
	if not is_instance_valid(cam):
		return
	# Flat-frame Y ≈ altitude ASL near the origin (good enough for a fog ramp).
	var alt: float = cam.global_position.y
	var t: float = clampf((FOG_ZERO_ALT_M - alt) / (FOG_ZERO_ALT_M - FOG_FULL_ALT_M), 0.0, 1.0)
	_world_env.fog_density = _fog_base_density * t
	# Disable fog ENTIRELY above the haze band: exponential fog still tints the
	# infinite-depth background to the pale fog colour via fog_sky_affect /
	# aerial-perspective even at density 0, which turns "black space" pale blue.
	_world_env.fog_enabled = t > 0.0
	# Darken the sky background toward black as we climb out of the atmosphere.
	# Above the atmosphere switch to a solid black background (energy fading alone
	# leaves a lit sky that never reaches true black); below, ramp the sky back in.
	var ts: float = clampf((SKY_BLACK_ALT_M - alt) / (SKY_BLACK_ALT_M - SKY_FULL_ALT_M), 0.0, 1.0)
	if ts <= 0.0:
		if _world_env.background_mode != Environment.BG_COLOR:
			_world_env.background_mode = Environment.BG_COLOR
			_world_env.background_color = Color(0, 0, 0)
	else:
		if _world_env.background_mode != Environment.BG_SKY:
			_world_env.background_mode = Environment.BG_SKY
		_world_env.background_energy_multiplier = _bg_base_energy * ts
	# High ambient (tuned for ground) over-brightens the globe and blooms into
	# space; taper it toward a low value at altitude.
	_world_env.ambient_light_energy = lerpf(AMBIENT_SPACE_ENERGY, _ambient_base_energy, ts)
	# Glow blooms the sunlit globe across the (black) background, which reads as a
	# bright grey haze from space. Fade it out with altitude and disable it fully
	# above the atmosphere (the world3d demo uses no glow for its clean black space).
	_world_env.glow_intensity = _glow_base_intensity * ts
	_world_env.glow_enabled = ts > 0.02


## Show the coarse global globe only above the detailed Swiss/BW patch, so its
## ~2 km ETOPO surface doesn't poke through the fine scenery3d terrain at low
## altitude. The globe's terrain queries are unaffected by visibility.
func _update_globe_visibility() -> void:
	if not USE_WORLD3D_GLOBE or not is_instance_valid(globe_node):
		return
	var vp := get_viewport()
	var cam: Camera3D = vp.get_camera_3d() if vp else null
	if not is_instance_valid(cam):
		return
	var want_visible := cam.global_position.y > GLOBE_SHOW_ALT_M
	if globe_node.visible != want_visible:
		globe_node.visible = want_visible


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

	# For JSBSim we need real ASL elevation — Scenery3D Y is already ASL.
	jsbsim_elevation_offset = 0.0
	var jsbsim_terrain_elevation = api_height + jsbsim_elevation_offset

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
var terrain_initialized: bool = false
var terrain_init_frame_count: int = 0
const TERRAIN_INIT_DELAY_FRAMES: int = 60  # Wait 60 frames (~1 sec) before per-frame updates
# Offset to convert Godot Y coordinates to JSBSim ASL elevation
# Set during initialization, used for per-frame terrain updates
var jsbsim_elevation_offset: float = 0.0

var _rebase_retry_start_msec: int = 0

func _process_rebase_cooldown() -> void:
	## Handle post-teleport position pinning and JSBSim release.
	var ac_guard = get_node_or_null("AC")
	if ac_guard and terrain_initialized and _rebase_cooldown > 0:
		ac_guard.global_position = _teleport_pin_pos
		_rebase_cooldown -= 1
		if _rebase_cooldown == 0:
			# Terrain should be loaded by now — update height at pin position
			var h: float = _terrain_elevation_at(_teleport_pin_pos.x, _teleport_pin_pos.z)
			if is_nan(h):
				# Tile still not loaded — keep pinning and retry next frame,
				# but only up to TERRAIN_INIT_FALLBACK_MSEC so we don't loop
				# forever when the destination is over nodata terrain.
				if _rebase_retry_start_msec == 0:
					_rebase_retry_start_msec = Time.get_ticks_msec()
				var waited := Time.get_ticks_msec() - _rebase_retry_start_msec
				if waited < TERRAIN_INIT_FALLBACK_MSEC:
					_rebase_cooldown = 1
					return
				push_warning("Teleport: terrain elevation unavailable at (%.0f, %.0f) after %.0fs — releasing JSBSim at fallback elevation 0 m ASL" % [_teleport_pin_pos.x, _teleport_pin_pos.z, waited / 1000.0])
				h = 0.0
			_rebase_retry_start_msec = 0
			if h > 0.1:
				_teleport_pin_pos.y = h + 2.1  # terrain + gear clearance
				ac_guard.global_position = _teleport_pin_pos
				print("Teleport: updated terrain height to %.1fm" % h)
			# Re-initialize JSBSim at the final pinned position and release
			if jsb_node:
				if jsb_node.has_method("reinitialize_from_godot_position"):
					var h_terr: float = _terrain_elevation_at(ac_guard.global_position.x, ac_guard.global_position.z)
					if is_nan(h_terr):
						h_terr = h
					jsb_node.reinitialize_from_godot_position(ac_guard.global_position, ac_guard.rotation.y, h_terr)
				_update_terrain_elevation()
				if jsb_node.has_method("set_slew_mode"):
					jsb_node.set_slew_mode(false)
			print("Teleport cooldown expired – JSBSim released at ", ac_guard.global_position)

func _update_terrain_elevation() -> void:
	# Skip if not yet initialized
	if not terrain_initialized:
		return

	# Skip first N frames to let JSBSim stabilize
	terrain_init_frame_count += 1
	if terrain_init_frame_count < TERRAIN_INIT_DELAY_FRAMES:
		return

	if not jsb_node:
		return
	if not is_instance_valid(scenery3d_node):
		return

	var ac_node = get_node_or_null("AC")
	if not ac_node:
		return

	var ac_position = ac_node.global_position
	var terrain_height_godot: float = _terrain_elevation_at(ac_position.x, ac_position.z)

	# If the tile under the aircraft hasn't streamed in yet, do NOT push 0.0
	# to JSBSim — that would make it think the ground is at sea level and
	# produce a multi-hundred-metre collision spike when the real tile loads.
	# Leaving the previous elevation in place keeps physics stable.
	if is_nan(terrain_height_godot):
		return

	# Convert Godot Y coordinate to JSBSim ASL elevation
	var terrain_elevation_jsbsim = terrain_height_godot + jsbsim_elevation_offset

	# Set terrain elevation in JSBSim (in meters - the method converts to feet)
	jsb_node.set_terrain_elevation(terrain_elevation_jsbsim)


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	# Do deferred terrain initialization if pending (needs physics world ready)
	if _pending_terrain_init:
		_do_deferred_terrain_init()

	# Fade fog with camera altitude so the globe is clear from space.
	_update_atmosphere()
	# Hide the coarse globe under the detailed Swiss/BW patch.
	_update_globe_visibility()

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

	# Skip all flight logic while welcome or settings overlay is visible
	if _welcome_screen and _welcome_screen.visible:
		return
	if _settings_screen and _settings_screen.visible:
		return

	# Cinematic space→spot zoom on entering fly mode (world3d globe).
	if _intro_playing:
		_update_intro_zoom(delta)
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

	# Toggle slew mode with Shift+Y
	if Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_Y):
		if not _slew_key_held:
			_slew_key_held = true
			toggle_slew_mode()
	else:
		_slew_key_held = false

	# Position dialog with Shift+P
	if Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_P):
		if not _position_key_held:
			_position_key_held = true
			_show_position_dialog()
	else:
		_position_key_held = false
	
	# Handle slew mode movement
	if slew_mode:
		handle_slew_mode(delta)
		# Allow camera controls in slew mode, but skip other processing
		if Input.is_action_just_pressed("flip_camera"):
			flip_camera()
		match camera_mode:
			CameraMode.COCKPIT:
				handle_head_look(delta)
			CameraMode.ORBIT:
				handle_orbit_camera(delta)
			CameraMode.CHASE:
				handle_chase_camera(delta)
		return  # Skip flight controls and HUD updates in slew mode
	
	if Input.is_action_just_pressed("flip_camera"):
		flip_camera()
	
	# Handle hat switch / D-pad for pilot head look, orbit, or chase camera
	match camera_mode:
		CameraMode.COCKPIT:
			handle_head_look(delta)
		CameraMode.ORBIT:
			handle_orbit_camera(delta)
		CameraMode.CHASE:
			handle_chase_camera(delta)
	
	# Mixture lever keyboard control: M = richer, Shift+M = leaner
	_handle_mixture_keys()
	
	# Throttle lever keyboard control: T = increase, Shift+T = decrease
	_handle_throttle_keys()
	
	# Magneto switch keyboard control: I = clockwise, Shift+I = counterclockwise
	_handle_magneto_keys()
	
	# Flap lever keyboard control: F = flaps down, Shift+F = flaps up
	_handle_flap_keys()
	
	# Landing gear toggle: G raises/lowers retractable gear
	_handle_gear_key()
	
	# Animate control surfaces, propeller and landing gear (C++ module).
	# Skipped while jsb_node is null, exactly as before; engine sound follows.
	if jsb_node:
		_control_surfaces.update(jsb_node, delta)
		_update_engine_sound()

	# Animate cockpit lever visuals (kept in GDScript next to their input code)
	_update_lever_visuals()
	
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
	else:
		$Label.text = "JSBGodot node not found."


## Toggle slew mode on/off
func toggle_slew_mode() -> void:
	slew_mode = not slew_mode
	
	# Tell JSBGodot to pause position updates in slew mode
	if jsb_node and jsb_node.has_method("set_slew_mode"):
		jsb_node.set_slew_mode(slew_mode)
	
	if slew_mode:
		# Save current airspeed before entering slew
		if jsb_node:
			_pre_slew_airspeed_kts = jsb_node.get_airspeed_knots()
		print("=== SLEW MODE ENABLED ===")
		# Debug joystick state so axis mapping issues are easy to spot in the log
		var pads := Input.get_connected_joypads()
		print("  Connected joypads: %d — indices: %s" % [pads.size(), pads])
		var active_dev := JoystickConfig.get_active_device()
		print("  JoystickConfig active device: %d" % active_dev)
		if active_dev >= 0:
			for ax in range(8):
				var v := Input.get_joy_axis(active_dev, ax)
				if abs(v) > 0.01:
					print("    axis %d = %.3f (non-zero)" % [ax, v])
		print("  Saved airspeed: %.1f kts" % _pre_slew_airspeed_kts)
		print("  Controls:")
		print("    Left stick / WASD: Move horizontally")
		print("    Right stick Y / Q,E: Move up/down")
		print("    Right stick X: Rotate (yaw)")
		print("    Hold Shift: Move faster (5x)")
		print("  Press Shift+Y to exit slew mode")
	else:
		print("=== SLEW MODE DISABLED ===")
		# Clear slew-only HUD elements
		$Control/Label8.text = ""
		# Re-initialize JSBSim at new position with saved airspeed
		var ac_node = $AC
		if ac_node and jsb_node:
			var pos = ac_node.global_position
			var heading = ac_node.rotation.y
			# Query terrain under aircraft and lift it above ground if it sank below
			# the surface during slew. Without this, ic->RunIC() fires gear-collision
			# forces for the first frame and the aircraft bounces.
			var terr_h: float = _terrain_elevation_at(pos.x, pos.z)
			if not is_nan(terr_h):
				var min_y: float = terr_h + 2.0  # 2 m gear clearance
				if pos.y < min_y:
					pos.y = min_y
					ac_node.global_position = pos
			print("  Reinitializing JSBSim at Godot position: ", pos)
			print("  Heading: ", rad_to_deg(heading), " degrees")
			print("  Restoring airspeed: %.1f kts" % _pre_slew_airspeed_kts)
			
			# Reinitialize with preserved airspeed
			if jsb_node.has_method("reinitialize_from_godot_position_with_speed"):
				jsb_node.reinitialize_from_godot_position_with_speed(pos, heading, _pre_slew_airspeed_kts, terr_h)
			elif jsb_node.has_method("reinitialize_from_godot_position"):
				jsb_node.reinitialize_from_godot_position(pos, heading, terr_h)
			
			# Update terrain elevation at new position
			_update_terrain_elevation()


## Handle slew mode movement using gamepad
func handle_slew_mode(delta: float) -> void:
	var ac_node = $AC
	if ac_node == null:
		return
	
	# Speed multiplier (hold shift for faster movement)
	var speed_mult = slew_fast_multiplier if Input.is_key_pressed(KEY_SHIFT) else 1.0
	
	# Gamepad input — calibrated through JoystickConfig (Settings → Joystick).
	# Each axis returns [-1, 1] with deadzone, invert and curve already applied.
	var move_x := JoystickConfig.get_axis("slew_strafe")
	var move_z := JoystickConfig.get_axis("slew_forward")
	var rotate_y := JoystickConfig.get_axis("slew_yaw")
	var move_y := JoystickConfig.get_axis("slew_vertical")

	# Also support keyboard for vertical movement
	if Input.is_key_pressed(KEY_Q) or Input.is_key_pressed(KEY_PAGEUP):
		move_y = 1.0
	elif Input.is_key_pressed(KEY_E) or Input.is_key_pressed(KEY_PAGEDOWN):
		move_y = -1.0
	
	# Keyboard WASD support
	if Input.is_key_pressed(KEY_W):
		move_z = -1.0
	elif Input.is_key_pressed(KEY_S):
		move_z = 1.0
	if Input.is_key_pressed(KEY_A):
		move_x = -1.0
	elif Input.is_key_pressed(KEY_D):
		move_x = 1.0
	
	# Calculate movement in world space based on aircraft heading
	var current_rotation = ac_node.rotation.y
	var forward = Vector3(sin(current_rotation), 0, cos(current_rotation))
	var right = Vector3(cos(current_rotation), 0, -sin(current_rotation))
	
	# Calculate velocity (negate move_x to fix left/right direction)
	var velocity = Vector3.ZERO
	velocity += forward * (-move_z) * slew_speed * speed_mult
	velocity += right * (-move_x) * slew_speed * speed_mult
	velocity.y = move_y * slew_vertical_speed * speed_mult
	
	# Apply movement
	ac_node.global_position += velocity * delta
	
	# Apply rotation (yaw)
	if abs(rotate_y) > 0.1:
		ac_node.rotation.y -= rotate_y * 2.0 * delta  # 2 rad/s rotation speed
	
	# Update HUD with current position
	var godot_pos = ac_node.global_position
	var swiss_e = TERRAIN_CENTER_E - godot_pos.x
	var swiss_n = TERRAIN_CENTER_N + godot_pos.z
	
	# Query terrain height at current position
	var terrain_height := _terrain_elevation_at(godot_pos.x, godot_pos.z)
	
	var heading_deg = fmod(rad_to_deg(-ac_node.rotation.y) + 360.0, 360.0)
	airspeed_label.text = "=== SLEW MODE ==="
	altitude_label.text = "Height AGL: %.1f m" % (godot_pos.y - terrain_height)
	vspeed_label.text = "Terrain: %.1f m ASL" % terrain_height
	throttle_label.text = "Aircraft: %.1f m ASL" % godot_pos.y
	heading_label.text = "Shift+Y to exit"
	$Control/Label6.text = "Godot: X=%.1f Y=%.1f Z=%.1f" % [godot_pos.x, godot_pos.y, godot_pos.z]
	$Control/Label7.text = "Swiss E: %s, N: %s" % [format_swiss_coord(swiss_e), format_swiss_coord(swiss_n)]
	$Control/Label8.text = "Heading: %.0f°" % heading_deg


## Read a joystick axis with a deadzone, returning 0 inside the deadzone and
## a smoothly rescaled value outside so resting drift never produces motion.
##
## Deprecated: gameplay code should call JoystickConfig.get_axis(action) so
## bindings remain user-configurable. Kept as a fallback for spots that have
## not yet been migrated.
func _slew_axis(axis: int, deadzone: float) -> float:
	var v := Input.get_joy_axis(0, axis)
	if absf(v) < deadzone:
		return 0.0
	var sign_v := signf(v)
	return sign_v * (absf(v) - deadzone) / (1.0 - deadzone)


# Function to cycle between camera modes: Cockpit → Orbit → Chase → Cockpit
func flip_camera():
	match camera_mode:
		CameraMode.COCKPIT:
			camera_mode = CameraMode.ORBIT
			active_camera = camera2
		CameraMode.ORBIT:
			camera_mode = CameraMode.CHASE
			active_camera = camera2
		CameraMode.CHASE:
			camera_mode = CameraMode.COCKPIT
			active_camera = camera1
			# Reset head position when switching back to cockpit view
			head_yaw = 0.0
			head_pitch = 0.0

	# Set the current camera
	camera1.current = (active_camera == camera1)
	camera2.current = (active_camera == camera2)

# Handle pilot head look using hat switch or D-pad
func handle_head_look(delta: float) -> void:
	# Only apply head look when using cockpit camera (Camera1)
	if active_camera != camera1:
		return
	
	# Get hat switch input (typically axes 6 and 7 on many controllers)
	# Or use D-pad which maps to JOY_BUTTON_DPAD_*
	var look_x: float = 0.0
	var look_y: float = 0.0
	var dev := JoystickConfig.get_active_device()

	# Try hat switch axes first (DirectInput hat: axis 6 = X, axis 7 = Y on most Windows joysticks)
	if dev >= 0:
		var hx := Input.get_joy_axis(dev, 6)  # hat horizontal
		var hy := Input.get_joy_axis(dev, 7)  # hat vertical
		if abs(hx) >= 0.5:
			look_x = sign(hx)
		if abs(hy) >= 0.5:
			look_y = sign(hy)

	# If no hat axis input, check D-pad buttons
	if abs(look_x) < 0.1 and abs(look_y) < 0.1:
		var d := dev if dev >= 0 else 0
		if Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_LEFT):
			look_x = -1.0
		elif Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_RIGHT):
			look_x = 1.0
		if Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_UP):
			look_y = -1.0  # Up on D-pad = look up (negative pitch)
		elif Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_DOWN):
			look_y = 1.0   # Down on D-pad = look down (positive pitch)
	
	# Also support keyboard arrows for testing
	if Input.is_key_pressed(KEY_LEFT):
		look_x = -1.0
	elif Input.is_key_pressed(KEY_RIGHT):
		look_x = 1.0
	if Input.is_key_pressed(KEY_UP):
		look_y = -1.0
	elif Input.is_key_pressed(KEY_DOWN):
		look_y = 1.0
	
	# Update head angles
	head_yaw += look_x * HEAD_LOOK_SPEED * delta
	head_pitch += look_y * HEAD_LOOK_SPEED * delta
	
	# Clamp to limits
	head_yaw = clamp(head_yaw, -HEAD_YAW_MAX, HEAD_YAW_MAX)
	head_pitch = clamp(head_pitch, -HEAD_PITCH_MAX, HEAD_PITCH_MAX)
	
	# Apply rotation to camera (relative to base transform)
	# Start from base transform, then apply head rotation
	camera1.transform = camera1_base_transform
	camera1.rotate_y(deg_to_rad(-head_yaw))  # Yaw (left/right)
	camera1.rotate_object_local(Vector3.RIGHT, deg_to_rad(-head_pitch))  # Pitch (up/down)

# Handle external/spot camera orbit around aircraft
func handle_orbit_camera(delta: float) -> void:
	# Get input from hat switch, D-pad, or keyboard
	var look_x: float = 0.0
	var look_y: float = 0.0
	var dev := JoystickConfig.get_active_device()

	# Hat switch axes (DirectInput: axis 6 = X, axis 7 = Y)
	if dev >= 0:
		var hx := Input.get_joy_axis(dev, 6)
		var hy := Input.get_joy_axis(dev, 7)
		if abs(hx) >= 0.5:
			look_x = sign(hx)
		if abs(hy) >= 0.5:
			look_y = -sign(hy)  # hat up = positive elevation

	# Check D-pad buttons (fallback / gamepads)
	if abs(look_x) < 0.1 and abs(look_y) < 0.1:
		var d := dev if dev >= 0 else 0
		if Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_LEFT):
			look_x = -1.0
		elif Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_RIGHT):
			look_x = 1.0
		if Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_UP):
			look_y = 1.0   # Up = increase elevation
		elif Input.is_joy_button_pressed(d, JOY_BUTTON_DPAD_DOWN):
			look_y = -1.0  # Down = decrease elevation
	
	# Also support keyboard arrows for testing
	if Input.is_key_pressed(KEY_LEFT):
		look_x = -1.0
	elif Input.is_key_pressed(KEY_RIGHT):
		look_x = 1.0
	if Input.is_key_pressed(KEY_UP):
		look_y = 1.0
	elif Input.is_key_pressed(KEY_DOWN):
		look_y = -1.0
	
	# Update orbit angles
	orbit_yaw += look_x * ORBIT_SPEED * delta
	orbit_pitch += look_y * ORBIT_SPEED * delta
	
	# Wrap yaw (allow full 360 rotation)
	if orbit_yaw > 360.0:
		orbit_yaw -= 360.0
	elif orbit_yaw < 0.0:
		orbit_yaw += 360.0
	
	# Clamp pitch to limits
	orbit_pitch = clamp(orbit_pitch, ORBIT_PITCH_MIN, ORBIT_PITCH_MAX)
	
	# Handle zoom - L1/R1 (shoulder buttons) or Page Up/Page Down
	var zoom_in = 0.0
	var zoom_out = 0.0
	var dz := dev if dev >= 0 else 0

	# Gamepad shoulder buttons (L1 = button 4, R1 = button 5)
	if Input.is_joy_button_pressed(dz, JOY_BUTTON_LEFT_SHOULDER):
		zoom_in = 1.0
	if Input.is_joy_button_pressed(dz, JOY_BUTTON_RIGHT_SHOULDER):
		zoom_out = 1.0
	
	# Keyboard Page Up/Down
	if Input.is_key_pressed(KEY_PAGEUP):
		zoom_in = 1.0
	if Input.is_key_pressed(KEY_PAGEDOWN):
		zoom_out = 1.0
	
	# Update distance
	orbit_distance -= zoom_in * ORBIT_ZOOM_SPEED * delta
	orbit_distance += zoom_out * ORBIT_ZOOM_SPEED * delta
	orbit_distance = clamp(orbit_distance, ORBIT_DISTANCE_MIN, ORBIT_DISTANCE_MAX)
	
	# Calculate camera offset in world space (not affected by aircraft rotation)
	var yaw_rad = deg_to_rad(orbit_yaw)
	var pitch_rad = deg_to_rad(orbit_pitch)
	
	# Spherical to Cartesian conversion (world-aligned offset)
	var offset_x = orbit_distance * cos(pitch_rad) * sin(yaw_rad)
	var offset_y = orbit_distance * sin(pitch_rad)
	var offset_z = orbit_distance * cos(pitch_rad) * cos(yaw_rad)
	var world_offset = Vector3(offset_x, offset_y, offset_z)
	
	# Get the AC node (parent of camera2) and its global position
	var ac_node = $AC
	var aircraft_global_pos = ac_node.global_position
	
	# Set camera to world position (aircraft position + world-aligned offset)
	camera2.global_position = aircraft_global_pos + world_offset
	
	# Make camera look at the aircraft center in world space
	# Guard against degenerate case (zero offset → same position)
	if world_offset.length_squared() > 0.01:
		camera2.look_at(aircraft_global_pos, Vector3.UP)

# Chase camera: follows behind the aircraft, rotating with its heading
func handle_chase_camera(delta: float) -> void:
	var ac_node = $AC
	var aircraft_pos = ac_node.global_position
	var dev2 := JoystickConfig.get_active_device()
	var d2 := dev2 if dev2 >= 0 else 0

	# Get aircraft's forward direction projected onto horizontal plane
	var forward = ac_node.global_transform.basis.z
	forward.y = 0
	if forward.length_squared() > 0.001:
		forward = forward.normalized()
	else:
		forward = Vector3(0, 0, -1)  # Default to north if degenerate
	
	# Handle zoom - same controls as orbit camera
	var zoom_in = 0.0
	var zoom_out = 0.0
	if Input.is_joy_button_pressed(d2, JOY_BUTTON_LEFT_SHOULDER):
		zoom_in = 1.0
	if Input.is_joy_button_pressed(d2, JOY_BUTTON_RIGHT_SHOULDER):
		zoom_out = 1.0
	if Input.is_key_pressed(KEY_PAGEUP):
		zoom_in = 1.0
	if Input.is_key_pressed(KEY_PAGEDOWN):
		zoom_out = 1.0
	chase_distance -= zoom_in * ORBIT_ZOOM_SPEED * delta
	chase_distance += zoom_out * ORBIT_ZOOM_SPEED * delta
	chase_distance = clamp(chase_distance, ORBIT_DISTANCE_MIN, ORBIT_DISTANCE_MAX)
	
	# Handle height adjust with D-pad up/down or arrow keys
	var height_delta = 0.0

	# Hat switch axes for chase yaw (horizontal) and height (vertical)
	var hat_x: float = 0.0
	var hat_y: float = 0.0
	if dev2 >= 0:
		var hx := Input.get_joy_axis(dev2, 6)
		var hy := Input.get_joy_axis(dev2, 7)
		if abs(hx) >= 0.5: hat_x = sign(hx)
		if abs(hy) >= 0.5: hat_y = -sign(hy)  # hat up = height up

	if hat_y != 0.0:
		height_delta = hat_y
	elif Input.is_joy_button_pressed(d2, JOY_BUTTON_DPAD_UP) or Input.is_key_pressed(KEY_UP):
		height_delta = 1.0
	elif Input.is_joy_button_pressed(d2, JOY_BUTTON_DPAD_DOWN) or Input.is_key_pressed(KEY_DOWN):
		height_delta = -1.0
	chase_height += height_delta * ORBIT_ZOOM_SPEED * delta
	chase_height = clamp(chase_height, 0.5, 20.0)
	
	# Handle yaw offset with D-pad left/right or arrow keys
	var yaw_delta = 0.0
	if hat_x != 0.0:
		yaw_delta = hat_x
	elif Input.is_joy_button_pressed(d2, JOY_BUTTON_DPAD_LEFT) or Input.is_key_pressed(KEY_LEFT):
		yaw_delta = -1.0
	elif Input.is_joy_button_pressed(d2, JOY_BUTTON_DPAD_RIGHT) or Input.is_key_pressed(KEY_RIGHT):
		yaw_delta = 1.0
	chase_yaw += yaw_delta * ORBIT_SPEED * delta
	if chase_yaw > 360.0:
		chase_yaw -= 360.0
	elif chase_yaw < 0.0:
		chase_yaw += 360.0
	
	# Rotate the "behind" direction by chase_yaw around the vertical axis
	var rotated_forward = forward.rotated(Vector3.UP, deg_to_rad(chase_yaw))
	
	# Target position: behind and above the aircraft
	var target_pos = aircraft_pos - rotated_forward * chase_distance + Vector3(0, chase_height, 0)
	
	# Smooth follow using lerp
	var t = clamp(chase_smooth * delta, 0.0, 1.0)
	camera2.global_position = camera2.global_position.lerp(target_pos, t)
	
	# Look at aircraft
	var to_target = aircraft_pos - camera2.global_position
	if to_target.length_squared() > 0.01:
		camera2.look_at(aircraft_pos, Vector3.UP)

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

# Animate the interactive cockpit levers/switches (kept in GDScript next to
# their input handling; the airframe control surfaces, propeller and landing
# gear are animated by the C++ ControlSurfaces module).
func _update_lever_visuals() -> void:
	# Animate mixture lever position (reflects current mixture_value)
	# Base transform = Blender rest pose for mixture = 0.0 (cutoff, pulled out).
	# Push lever forward along local -Z as mixture increases (same as throttle).
	if mixture_lever_node and not _mixture_dragging:
		mixture_lever_node.transform = mixture_lever_base_transform
		var travel_offset := -mixture_value * MIXTURE_LEVER_TRAVEL
		mixture_lever_node.translate_object_local(Vector3(0, 0, travel_offset))
	
	# Animate throttle lever position (reads current throttle from JSBSim)
	# Base transform = idle (Blender default); push in (-Z) for full throttle
	if throttle_lever_node and not _throttle_dragging and jsb_node:
		var cur_throttle := float(jsb_node.get_input_throttle())
		throttle_lever_node.transform = throttle_lever_base_transform
		var t_offset := -cur_throttle * THROTTLE_LEVER_TRAVEL
		throttle_lever_node.translate_object_local(Vector3(0, 0, t_offset))
	
	# Animate magneto switch rotation (rotates around Z axis)
	# Base transform = Blender rest pose for the BOTH position (MAGNETO_DEFAULT_POSITION).
	if magneto_switch_node and not _magneto_dragging:
		magneto_switch_node.transform = magneto_switch_base_transform
		var angle_offset := float(magneto_position - MAGNETO_DEFAULT_POSITION) * MAGNETO_STEP_ANGLE
		magneto_switch_node.rotate_object_local(Vector3.FORWARD, deg_to_rad(angle_offset))
	
	# Animate flap lever position (slides along Y axis)
	# Base transform = Blender default (flaps UP). Lever moves down (-Y) for more flaps.
	# Sync flap_position from the commanded value so gamepad changes move the lever.
	if flap_lever_node and not _flap_dragging:
		if jsb_node:
			var cmd_pos := _flaps_norm_to_position(jsb_node.get_flap_cmd())
			if cmd_pos != flap_position:
				flap_position = cmd_pos
				if flap_position != _flap_last_printed_pos:
					_flap_last_printed_pos = flap_position
					print("Flaps: %s" % FLAP_NAMES[flap_position])
		flap_lever_node.transform = flap_lever_base_transform
		var y_offset := -float(flap_position) * FLAP_LEVER_TRAVEL
		flap_lever_node.translate_object_local(Vector3(0, y_offset, 0))


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
	_control_surfaces.set_gear_retract_seconds(cfg.get_float("gear", "retract_seconds", 4.0))
	_gear_anim_name = cfg.get_string("gear", "animation", "")

	# Flap detents.
	var positions := cfg.get_float_array("flaps", "positions", FLAP_POSITIONS)
	if not positions.is_empty():
		FLAP_POSITIONS = positions
	var names := cfg.get_string_array("flaps", "names", FLAP_NAMES)
	if not names.is_empty():
		FLAP_NAMES = names

	# Propeller visuals: blur thresholds + spin axis go to the C++ animator;
	# disc size/offset stay here because the blur-disc mesh is built in GDScript.
	PROPELLER_MAX_RPM = cfg.get_float("propeller", "max_rpm", PROPELLER_MAX_RPM)
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

	# Engine sound.
	ENGINE_SOUND_PATH = cfg.get_string("sound", "engine", ENGINE_SOUND_PATH)
	ENGINE_PITCH_MIN = cfg.get_float("sound", "pitch_min", ENGINE_PITCH_MIN)
	ENGINE_PITCH_MAX = cfg.get_float("sound", "pitch_max", ENGINE_PITCH_MAX)
	ENGINE_VOL_DB_MIN = cfg.get_float("sound", "vol_db_min", ENGINE_VOL_DB_MIN)
	ENGINE_VOL_DB_MAX = cfg.get_float("sound", "vol_db_max", ENGINE_VOL_DB_MAX)


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
	camera1_base_transform = _camera1_scene_transform
	camera1.transform = _camera1_scene_transform
	camera1.fov = _camera1_scene_fov
	if pos.size() != 3 and rot.size() != 3 and fov <= 0.0:
		return  # no camera overrides for this aircraft

	var xform := camera1_base_transform
	if rot.size() == 3:
		var extra := Basis.from_euler(Vector3(
			deg_to_rad(rot[0]), deg_to_rad(rot[1]), deg_to_rad(rot[2])))
		xform.basis = xform.basis * extra
	if pos.size() == 3:
		xform.origin = Vector3(pos[0], pos[1], pos[2])
	camera1_base_transform = xform
	camera1.transform = xform
	if fov > 0.0:
		camera1.fov = fov
	print("Cockpit camera positioned for '%s': %s" % [_aircraft_id, xform.origin])



# Resolves all model-dependent nodes (animated control surfaces + interactive
# cockpit levers) against the currently-loaded GLB under _model_root_path.
# Called once at startup and again after a runtime aircraft model swap. Nodes
# absent from the aircraft.cfg [animation]/[levers] sections (or missing in the
# model) resolve to null and are skipped gracefully by the animation code.
func _resolve_aircraft_nodes() -> void:
	# The model loader frees the previous GLB subtree synchronously on an
	# airframe swap, so drop every registered pointer before re-resolving.
	_control_surfaces.reset()

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
	# root. Names + fallbacks come from aircraft.cfg [levers].
	var c172_root = get_node_or_null(NodePath(_model_root_path))
	if c172_root:
		mixture_lever_node = _resolve_lever(c172_root, "mixture")
		if mixture_lever_node:
			print("Mixture lever node found: ", mixture_lever_node.name)
			mixture_lever_base_transform = mixture_lever_node.transform
			_setup_mixture_lever_collision()
		else:
			print("WARNING: Mixture lever node not found")

		throttle_lever_node = _resolve_lever(c172_root, "throttle")
		if throttle_lever_node:
			print("Throttle lever node found: ", throttle_lever_node.name)
			throttle_lever_base_transform = throttle_lever_node.transform
			_setup_throttle_lever_collision()
		else:
			print("WARNING: Throttle lever node not found")

		magneto_switch_node = _resolve_lever(c172_root, "magneto")
		if magneto_switch_node:
			print("Magneto switch node found: ", magneto_switch_node.name)
			magneto_switch_base_transform = magneto_switch_node.transform
			_setup_magneto_switch_collision()
			_apply_magneto_position()
		else:
			print("WARNING: Magneto switch node not found")

		flap_lever_node = _resolve_lever(c172_root, "flap_lever")
		if flap_lever_node:
			print("Flap lever node found: ", flap_lever_node.name)
			flap_lever_base_transform = flap_lever_node.transform
			_setup_flap_lever_collision()
			if jsb_node:
				var cur_flaps := float(jsb_node.get_flaps())
				flap_position = _flaps_norm_to_position(cur_flaps)
			_apply_flap_position()
		else:
			print("WARNING: Flap lever node not found")


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
	# C++ side resets gear state (fully down) and clears previously registered
	# legs/animation player as part of reset(), called from _resolve_aircraft_nodes.
	_gear_down = true
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
	var p_scenery := float(loaded) / float(total)
	var scenery_ready := loaded >= total

	# Hard timeout safety net — if scenery never reports ready (e.g. over
	# nodata territory), don't trap the user on the loading screen forever.
	if is_fly_loading and _fly_loading_start_msec > 0:
		var elapsed := Time.get_ticks_msec() - _fly_loading_start_msec
		var max_msec := FLY_LOADING_NODATA_MSEC if loaded == 0 else FLY_LOADING_MAX_MSEC
		if elapsed > max_msec:
			scenery_ready = true
			p_scenery = 1.0

	var cooldown_pending := _rebase_cooldown if is_fly_loading else 0
	var p_cooldown := 1.0 - (float(cooldown_pending) / float(maxi(1, REBASE_COOLDOWN_FRAMES)))

	# Block loading-screen completion while the deferred terrain-init handshake
	# is still pending. Otherwise the scenery-ready timeout can fire first and
	# we dismiss the loading screen into a void with the aircraft hanging at
	# Y=500 m, only to slam down to ground level seconds later when JSBSim
	# finally initialises.
	var terrain_init_pending := is_fly_loading and _pending_terrain_init

	var progress: float
	if is_fly_loading:
		progress = p_scenery * 0.7 + p_cooldown * 0.3
	else:
		progress = p_scenery

	if _loading_screen:
		_loading_screen.set_progress(progress)
		if not scenery_ready:
			_loading_screen.set_status("Loading scenery... (%d/%d tiles)" % [loaded, total])
		elif terrain_init_pending:
			_loading_screen.set_status("Initialising flight model...")
		elif cooldown_pending > 0:
			_loading_screen.set_status("Stabilizing aircraft...")
		else:
			_loading_screen.set_status("Ready")

	var all_done := scenery_ready and not terrain_init_pending and cooldown_pending == 0
	if not all_done:
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
	if _settings_screen != null:
		installed_packs = _settings_screen.settings.get("installed_scenery_packs", {})
		scenery_areas = _settings_screen.settings.get("scenery_areas", [])
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
	var script = load("res://settings_screen.gd")
	_settings_screen = CanvasLayer.new()
	_settings_screen.set_script(script)
	add_child(_settings_screen)
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
	var missing := _missing_scenery_reason()
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
	_fly_loading_start_msec = Time.get_ticks_msec()
	if _loading_screen:
		_loading_screen.set_progress(0.0)
		_loading_screen.set_status("Loading terrain...")
		_loading_screen.show_loading()

	if _first_flight:
		_do_first_flight(swiss_e, swiss_n, heading_deg)
		_first_flight = false
	else:
		_on_teleport_requested(swiss_e, swiss_n, heading_deg)


## Returns "" when the configured scenery library has at least one enabled
## area with terrain or orthophoto data present on disk. Otherwise returns
## a human-readable explanation suitable for an error dialog.
func _missing_scenery_reason() -> String:
	if not _settings_screen or not _settings_screen.has_method("get_enabled_scenery_areas"):
		return "Settings unavailable — cannot verify scenery configuration."
	var areas: Array = _settings_screen.get_enabled_scenery_areas()
	if areas.is_empty():
		return "No scenery areas are configured.\n\nOpen Settings → Scenery and add the directory that contains your scenery data (e.g. the folder with Switzerland/, Germany/, …)."
	var missing_paths := PackedStringArray()
	var has_any_data := false
	for a in areas:
		var base: String = str(a.get("path", ""))
		if base.is_empty():
			continue
		if not DirAccess.dir_exists_absolute(base):
			missing_paths.append(base)
			continue
		# A region is considered usable if it has terrain or orthophoto tiles.
		if DirAccess.dir_exists_absolute(base.path_join("terrain")) \
				or DirAccess.dir_exists_absolute(base.path_join("orthophoto")):
			has_any_data = true
	if has_any_data:
		return ""
	if missing_paths.size() > 0:
		return "The configured scenery folder(s) do not exist on disk:\n\n  %s\n\nOpen Settings → Scenery to update the paths." % "\n  ".join(missing_paths)
	return "The configured scenery folder(s) contain no terrain or orthophoto tiles.\n\nExtract the scenery ZIPs into the folder you registered in Settings → Scenery, then click Scan."


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

	# 7. Set teleport pin so aircraft stays in place until terrain settles
	_teleport_pin_pos = ac_node.global_position
	_rebase_cooldown = REBASE_COOLDOWN_FRAMES

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
	if not is_instance_valid(_settings_screen):
		push_warning("Welcome scenery setup: settings screen not ready")
		return
	# Inject `path` into the scan roots and run the scanner. This mirrors what
	# the user would do manually in Settings → Scenery → Scan.
	var roots: PackedStringArray = _settings_screen.settings.get("scenery_scan_roots", PackedStringArray())
	if not roots.has(path):
		roots.append(path)
		_settings_screen.settings["scenery_scan_roots"] = roots
	if _settings_screen.has_method("_on_scan_scenery"):
		_settings_screen._on_scan_scenery()
	# Persist immediately so the next launch skips the wizard.
	if _settings_screen.has_method("_save_settings"):
		_settings_screen._save_settings()
	# Apply newly-found areas to the Scenery3D node.
	_apply_scenery_sources()
	print("First-run setup complete: applied scenery library from %s" % path)


func _on_settings_closed() -> void:
	if _settings_screen:
		_settings_screen.hide_settings()
	_apply_tree_density()
	_apply_scenery_sources()
	if _welcome_screen:
		_welcome_screen.set_continue_enabled(not _first_flight)
		_welcome_screen.visible = true
		_welcome_screen.show_welcome()


func _apply_tree_density() -> void:
	if not is_instance_valid(vegetation_node):
		return
	var density_idx: int = 2  # default medium
	if _settings_screen and "settings" in _settings_screen:
		density_idx = int(_settings_screen.settings.get("tree_density", 2))
	density_idx = clamp(density_idx, 0, TREE_DENSITY_FRACTION.size() - 1)
	var fraction: float = TREE_DENSITY_FRACTION[density_idx]
	vegetation_node.density_fraction = fraction
	vegetation_node.load_radius_m = TREE_LOAD_RADIUS_M
	vegetation_node.visible = fraction > 0.0
	print("Tree density: index=%d fraction=%.2f radius=%d m" % [density_idx, fraction, TREE_LOAD_RADIUS_M])


## Apply scenery data sources from settings to Scenery3D node.
## Reads the FSX-style scenery library (settings.scenery_areas), filters to
## enabled entries sorted by priority, then for each asset subdir that exists
## inside an area's path, appends to the corresponding Scenery3D paths array.
##
## Example: if "Switzerland" (priority 1) and "Germany/Baden-Wuertemberg"
## (priority 2) are both enabled, and BW only contains an orthophoto subdir,
## the resulting orthophoto_paths is [CH/orthophoto, BW/orthophoto] — Swiss
## tiles are tried first, German tiles serve as fallback (combined with
## skip_white_pixels=true this fixes the CH/DE border).
func _apply_scenery_sources() -> void:
	if not is_instance_valid(scenery3d_node):
		return
	if not _settings_screen or not _settings_screen.has_method("get_enabled_scenery_areas"):
		print("Scenery sources: settings screen unavailable, leaving scene defaults")
		return

	var areas: Array = _settings_screen.get_enabled_scenery_areas()
	if areas.is_empty():
		print("Scenery sources: no enabled scenery areas in library — clearing Scenery3D paths")

	# Build per-asset path arrays from enabled areas (priority order).
	var terrain_paths := PackedStringArray()
	var ortho_paths := PackedStringArray()
	var building_paths := PackedStringArray()
	var water_paths := PackedStringArray()
	var road_paths := PackedStringArray()
	var vegetation_paths := PackedStringArray()
	# macOS APFS is case-insensitive by default — two areas that differ only
	# in path case (e.g. ~/provpilot vs ~/ProvPilot) refer to the same data
	# on disk, so dedup case-insensitively to avoid loading every tile twice.
	var _seen := {}
	var _case_insensitive := OS.get_name() == "macOS" or OS.get_name() == "Windows"
	var _append_unique := func(arr: PackedStringArray, p: String) -> void:
		var k: String = p.to_lower() if _case_insensitive else p
		if not _seen.has(k):
			_seen[k] = true
			arr.append(p)
	for a in areas:
		var base: String = str(a.get("path", ""))
		if base.is_empty():
			continue
		if not DirAccess.dir_exists_absolute(base):
			push_warning("Scenery area path does not exist (skipped): %s" % base)
			continue
		var t := base.path_join("terrain")
		if DirAccess.dir_exists_absolute(t): _append_unique.call(terrain_paths, t)
		var o := base.path_join("orthophoto")
		if DirAccess.dir_exists_absolute(o): _append_unique.call(ortho_paths, o)
		var b := base.path_join("buildings")
		if DirAccess.dir_exists_absolute(b): _append_unique.call(building_paths, b)
		var w := base.path_join("water")
		if DirAccess.dir_exists_absolute(w): _append_unique.call(water_paths, w)
		var r := base.path_join("roads")
		if DirAccess.dir_exists_absolute(r): _append_unique.call(road_paths, r)
		var v := base.path_join("vegetation")
		if DirAccess.dir_exists_absolute(v): _append_unique.call(vegetation_paths, v)

	# Always assign — even empty arrays. Otherwise stale paths baked into the
	# scene file (.tscn) or ProjectSettings (scenery3d/*) would survive when
	# the runtime library has nothing, producing confusing "/Users/gery/..."
	# manifest warnings on a fresh Windows install.
	# IMPORTANT: do NOT also set the single-path properties (data_path,
	# orthophoto_path) — their setters in C++ CLEAR the array first, which
	# would silently drop all but one path. buildings_path stays as a legacy
	# fallback for older Scenery3D builds that do not expose buildings_paths.
	scenery3d_node.data_paths = terrain_paths
	scenery3d_node.orthophoto_paths = ortho_paths
	if scenery3d_node.has_method("set_buildings_paths") or "buildings_paths" in scenery3d_node:
		scenery3d_node.buildings_paths = building_paths
	elif building_paths.size() > 0:
		scenery3d_node.buildings_path = building_paths[0]
	scenery3d_node.water_paths = water_paths
	scenery3d_node.road_paths = road_paths

	# Vegetation node uses its own paths
	if is_instance_valid(vegetation_node):
		vegetation_node.vegetation_paths = vegetation_paths

	print("Scenery library applied: %d enabled area(s)" % areas.size())
	print("  terrain_paths    = %s" % [terrain_paths])
	print("  orthophoto_paths = %s" % [ortho_paths])
	print("  buildings_paths  = %s" % [building_paths])
	print("  water_paths      = %s" % [water_paths])
	print("  road_paths       = %s" % [road_paths])
	print("  vegetation_paths = %s" % [vegetation_paths])

	# White-pixel skip (border fix) — only effective once C++ side ships it.
	var skip_white: bool = _settings_screen.settings.get("skip_white_pixels", true)
	if scenery3d_node.has_method("set_skip_white_pixels"):
		scenery3d_node.set_skip_white_pixels(skip_white)
		print("  skip_white_pixels = %s" % [skip_white])
	elif "skip_white_pixels" in scenery3d_node:
		scenery3d_node.skip_white_pixels = skip_white
		print("  skip_white_pixels = %s" % [skip_white])

	# Terrain Distance (render_distance, in tiles) → Scenery3D.load_radius.
	# Previously this slider was saved but never applied, so it had no effect.
	# Keep the radius modest: the low-res far-terrain CHUNK panorama fills
	# everything beyond it out to far_radius, so a small value (default 8 =
	# ~8 km of full-detail tiles) keeps streaming fast and memory bounded
	# while distant scenery still renders. Large values (e.g. 50) push the
	# chunk panorama past the horizon haze and balloon the tile count.
	var render_distance: int = int(_settings_screen.settings.get("render_distance", 8))
	render_distance = clampi(render_distance, 2, 16)
	if "load_radius" in scenery3d_node:
		scenery3d_node.load_radius = render_distance
		print("  load_radius (terrain distance) = %d tiles" % render_distance)


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
	#     GDScript slew_mode is false so the slew movement UI is not shown.
	slew_mode = false
	$Control/Label8.text = ""

	# Arm cooldown: pin AC at this position while JSBSim stabilizes
	_teleport_pin_pos = ac_node.global_position
	_rebase_cooldown = REBASE_COOLDOWN_FRAMES

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
	
	# Also send initial mixture value to JSBSim
	_apply_mixture_value()


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


## Handle M / Shift+M keyboard input for mixture control.
## Shift+Ctrl+M toggles debug visibility of the collision box.
func _handle_mixture_keys() -> void:
	if Input.is_action_just_pressed("ui_text_scroll_down"):
		# Avoid conflicts — only use raw key checks
		pass
	
	if Input.is_key_pressed(KEY_M):
		# Debounce: only act on the frame the key is first pressed
		pass
	
	# Use is_physical_key_pressed for single-fire via just_pressed workaround
	if not Input.is_key_pressed(KEY_M):
		_mixture_key_held = false
		return
	
	if _mixture_key_held:
		return  # Wait for key release before next step
	_mixture_key_held = true
	
	# Shift+Ctrl+M = toggle collision debug visibility
	if Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_CTRL):
		_toggle_mixture_debug()
		return
	
	var old_value := mixture_value
	if Input.is_key_pressed(KEY_SHIFT):
		# Shift+M = decrease mixture (leaner)
		mixture_value = clampf(mixture_value - MIXTURE_KEY_STEP, 0.0, 1.0)
	else:
		# M = increase mixture (richer)
		mixture_value = clampf(mixture_value + MIXTURE_KEY_STEP, 0.0, 1.0)
	
	if mixture_value != old_value:
		_apply_mixture_value()
		print("Mixture: %.0f%%" % (mixture_value * 100.0))


## Handle T / Shift+T keyboard input for throttle control.
## Shift+Ctrl+T toggles debug visibility of the throttle collision box.
func _handle_throttle_keys() -> void:
	if not Input.is_key_pressed(KEY_T):
		_throttle_key_held = false
		return
	
	if _throttle_key_held:
		return
	_throttle_key_held = true
	
	# Shift+Ctrl+T = toggle collision debug visibility
	if Input.is_key_pressed(KEY_SHIFT) and Input.is_key_pressed(KEY_CTRL):
		_toggle_throttle_debug()
		return
	
	if not jsb_node:
		return
	
	var cur := float(jsb_node.get_input_throttle())
	if Input.is_key_pressed(KEY_SHIFT):
		cur = clampf(cur - THROTTLE_KEY_STEP, 0.0, 1.0)
	else:
		cur = clampf(cur + THROTTLE_KEY_STEP, 0.0, 1.0)
	
	jsb_node.set_input_throttle(cur)
	print("Throttle: %.0f%%" % (cur * 100.0))


## Toggle debug visualization of the mixture lever collision area.
func _toggle_mixture_debug() -> void:
	_mixture_debug_visible = not _mixture_debug_visible
	if not mixture_lever_body:
		print("No mixture lever body to debug")
		return
	
	# Find or create the debug mesh
	var debug_mesh := mixture_lever_body.get_node_or_null("DebugMesh") as MeshInstance3D
	if _mixture_debug_visible:
		if not debug_mesh:
			debug_mesh = MeshInstance3D.new()
			debug_mesh.name = "DebugMesh"
			var box := BoxMesh.new()
			box.size = Vector3(0.02, 0.02, 0.04)  # Same as collision shape
			debug_mesh.mesh = box
			var mat := StandardMaterial3D.new()
			mat.albedo_color = Color(1.0, 0.0, 0.0, 0.3)
			mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			mat.cull_mode = BaseMaterial3D.CULL_DISABLED
			mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
			debug_mesh.material_override = mat
			mixture_lever_body.add_child(debug_mesh)
		debug_mesh.visible = true
		print("Mixture lever collision box: VISIBLE")
		print("  Global position: ", mixture_lever_body.global_position)
		print("  Lever node global pos: ", mixture_lever_node.global_position)
	else:
		if debug_mesh:
			debug_mesh.visible = false
		print("Mixture lever collision box: HIDDEN")


## Toggle debug visualization of the throttle lever collision area.
func _toggle_throttle_debug() -> void:
	_throttle_debug_visible = not _throttle_debug_visible
	if not throttle_lever_body:
		print("No throttle lever body to debug")
		return
	
	var debug_mesh := throttle_lever_body.get_node_or_null("DebugMesh") as MeshInstance3D
	if _throttle_debug_visible:
		if not debug_mesh:
			debug_mesh = MeshInstance3D.new()
			debug_mesh.name = "DebugMesh"
			var box := BoxMesh.new()
			box.size = Vector3(0.02, 0.02, 0.04)
			debug_mesh.mesh = box
			var mat := StandardMaterial3D.new()
			mat.albedo_color = Color(0.0, 0.0, 1.0, 0.3)
			mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			mat.cull_mode = BaseMaterial3D.CULL_DISABLED
			mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
			debug_mesh.material_override = mat
			throttle_lever_body.add_child(debug_mesh)
		debug_mesh.visible = true
		print("Throttle lever collision box: VISIBLE")
		print("  Global position: ", throttle_lever_body.global_position)
	else:
		if debug_mesh:
			debug_mesh.visible = false
		print("Throttle lever collision box: HIDDEN")


## Handle mouse input for cockpit lever interaction.
## Called from _unhandled_input so UI elements get priority.
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
					_mixture_drag_start_y = mb.position.y
					_mixture_drag_start_value = mixture_value
					get_viewport().set_input_as_handled()
				elif throttle_lever_node and jsb_node and _raycast_hits_body(mb.position, throttle_lever_body, THROTTLE_LEVER_COLLISION_LAYER):
					_throttle_dragging = true
					_throttle_drag_start_y = mb.position.y
					_throttle_drag_start_value = float(jsb_node.get_input_throttle())
					get_viewport().set_input_as_handled()
				elif magneto_switch_node and _raycast_hits_body(mb.position, magneto_switch_body, MAGNETO_SWITCH_COLLISION_LAYER):
					_magneto_dragging = true
					_magneto_drag_start_x = mb.position.x
					_magneto_drag_start_pos = magneto_position
					get_viewport().set_input_as_handled()
				elif flap_lever_node and _raycast_hits_body(mb.position, flap_lever_body, FLAP_LEVER_COLLISION_LAYER):
					_flap_dragging = true
					_flap_drag_start_y = mb.position.y
					_flap_drag_start_pos = flap_position
					get_viewport().set_input_as_handled()
			else:
				if _mixture_dragging:
					_mixture_dragging = false
					get_viewport().set_input_as_handled()
				if _throttle_dragging:
					_throttle_dragging = false
					get_viewport().set_input_as_handled()
				if _magneto_dragging:
					_magneto_dragging = false
					# Spring back from START on mouse release
					if magneto_position == 4:
						magneto_position = 3
						_apply_magneto_position()
						print("Magneto: BOTH")
					get_viewport().set_input_as_handled()
				if _flap_dragging:
					_flap_dragging = false
					get_viewport().set_input_as_handled()
	
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _mixture_dragging:
			var delta_y := _mixture_drag_start_y - mm.position.y
			var delta_mixture := delta_y / 200.0
			mixture_value = clampf(_mixture_drag_start_value + delta_mixture, 0.0, 1.0)
			_apply_mixture_value()
			get_viewport().set_input_as_handled()
		elif _throttle_dragging and jsb_node:
			var delta_y := _throttle_drag_start_y - mm.position.y
			var delta_throttle := delta_y / 200.0
			var new_throttle := clampf(_throttle_drag_start_value + delta_throttle, 0.0, 1.0)
			jsb_node.set_input_throttle(new_throttle)
			# Update lever visual immediately during drag
			throttle_lever_node.transform = throttle_lever_base_transform
			throttle_lever_node.translate_object_local(Vector3(0, 0, -new_throttle * THROTTLE_LEVER_TRAVEL))
			get_viewport().set_input_as_handled()
		elif _magneto_dragging:
			# Drag right = clockwise (toward START), left = counterclockwise (toward OFF)
			var delta_x := mm.position.x - _magneto_drag_start_x
			var steps := int(delta_x / 40.0)  # 40px per position step
			var new_pos := clampi(_magneto_drag_start_pos + steps, 0, 4)
			if new_pos != magneto_position:
				magneto_position = new_pos
				_apply_magneto_position()
			get_viewport().set_input_as_handled()
		elif _flap_dragging:
			# Drag down = more flaps, up = less flaps (vertical, like a pull-down lever)
			var delta_y := mm.position.y - _flap_drag_start_y
			var steps := int(delta_y / 40.0)  # 40px per notch
			var new_pos := clampi(_flap_drag_start_pos + steps, 0, 3)
			if new_pos != flap_position:
				flap_position = new_pos
				_apply_flap_position()
			# Update lever visual immediately during drag
			flap_lever_node.transform = flap_lever_base_transform
			flap_lever_node.translate_object_local(Vector3(0, -float(flap_position) * FLAP_LEVER_TRAVEL, 0))
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


## Apply the current mixture_value to both the lever animation and JSBSim.
func _apply_mixture_value() -> void:
	# The captured base_transform is the Blender rest pose, which represents
	# mixture = 0.0 (cutoff, lever pulled out). Push the lever forward along
	# local -Z as the pilot enriches the mixture (same convention as throttle).
	if mixture_lever_node:
		mixture_lever_node.transform = mixture_lever_base_transform
		var travel_offset := -mixture_value * MIXTURE_LEVER_TRAVEL
		mixture_lever_node.translate_object_local(Vector3(0, 0, travel_offset))
	
	# Send to JSBSim
	if jsb_node and jsb_node.has_method("set_input_mixture"):
		jsb_node.set_input_mixture(mixture_value)


## Create collision body on the magneto switch for mouse picking.
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


## Handle I / Shift+I keyboard input for magneto switch control.
## I past BOTH = momentary START (springs back to BOTH on release).
const MAGNETO_NAMES: Array = ["OFF", "R", "L", "BOTH", "START"]

func _handle_magneto_keys() -> void:
	# I / Shift+I = step through positions (with debounce)
	if not Input.is_key_pressed(KEY_I):
		_magneto_key_held = false
		# If at START and I released, spring back to BOTH (but not during mouse drag)
		if magneto_position == 4 and not _magneto_dragging:
			magneto_position = 3
			_apply_magneto_position()
		return
	
	if _magneto_key_held:
		return
	_magneto_key_held = true
	
	var old_pos := magneto_position
	if Input.is_key_pressed(KEY_SHIFT):
		magneto_position = maxi(magneto_position - 1, 0)
	else:
		magneto_position = mini(magneto_position + 1, 4)  # Allow going to START (4)
	
	if magneto_position != old_pos:
		_apply_magneto_position()


## Apply current magneto position: update visual + send to JSBSim.
func _apply_magneto_position() -> void:
	# Print only on actual change
	if magneto_position != _magneto_last_printed_pos:
		_magneto_last_printed_pos = magneto_position
		print("Magneto: %s" % MAGNETO_NAMES[magneto_position])
	
	# The captured base_transform is the Blender rest pose at the default
	# startup position (BOTH = 3). Offsets are measured relative to that.
	if magneto_switch_node:
		magneto_switch_node.transform = magneto_switch_base_transform
		var angle_offset := float(magneto_position - MAGNETO_DEFAULT_POSITION) * MAGNETO_STEP_ANGLE
		magneto_switch_node.rotate_object_local(Vector3.FORWARD, deg_to_rad(angle_offset))
	
	# Send to JSBSim
	if jsb_node and jsb_node.has_method("set_input_magnetos"):
		jsb_node.set_input_magnetos(magneto_position)


## Create collision body on the flap lever for mouse picking.
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


## Handle F / Shift+F keyboard input for flap lever control.
func _handle_flap_keys() -> void:
	if not Input.is_key_pressed(KEY_F):
		_flap_key_held = false
		return
	
	if _flap_key_held:
		return
	_flap_key_held = true
	
	var old_pos := flap_position
	if Input.is_key_pressed(KEY_SHIFT):
		flap_position = maxi(flap_position - 1, 0)  # Shift+F = flaps up
	else:
		flap_position = mini(flap_position + 1, 3)  # F = flaps down
	
	if flap_position != old_pos:
		_apply_flap_position()


## Handle G keyboard input: toggle retractable landing gear up/down.
func _handle_gear_key() -> void:
	if not _gear_retractable:
		return
	if not Input.is_key_pressed(KEY_G):
		_gear_key_held = false
		return
	if _gear_key_held:
		return
	_gear_key_held = true
	_gear_down = not _gear_down
	_control_surfaces.set_gear_down(_gear_down)
	if jsb_node and jsb_node.has_method("set_gear_down"):
		jsb_node.set_gear_down(_gear_down)
	print("Gear: ", "DOWN" if _gear_down else "UP")


## Convert JSBSim flap-pos-norm (0.0-1.0) to lever position index (0-3).
func _flaps_norm_to_position(norm: float) -> int:
	if norm < 0.167:
		return 0
	elif norm < 0.5:
		return 1
	elif norm < 0.833:
		return 2
	else:
		return 3


## Apply current flap position: update visual + send to JSBSim.
func _apply_flap_position() -> void:
	# Print only on actual change
	if flap_position != _flap_last_printed_pos:
		_flap_last_printed_pos = flap_position
		print("Flaps: %s" % FLAP_NAMES[flap_position])
	
	# Update lever visual
	if flap_lever_node:
		flap_lever_node.transform = flap_lever_base_transform
		var y_offset := -float(flap_position) * FLAP_LEVER_TRAVEL
		flap_lever_node.translate_object_local(Vector3(0, y_offset, 0))
	
	# Send to JSBSim
	if jsb_node and jsb_node.has_method("set_flaps"):
		jsb_node.set_flaps(FLAP_POSITIONS[flap_position])
