extends Node3D

# Terrain coordinate system (Swiss LV95)
# These define the relationship between Godot world coords and Swiss coordinates
# Terrain origin — aligned to 1024m tile grid for streaming.
# Updated at runtime by floating-origin rebase.
var TERRAIN_CENTER_E: float = 2682880.0  # Swiss Easting at Godot X=0
var TERRAIN_CENTER_N: float = 1283072.0  # Swiss Northing at Godot Z=0

# Floating origin: rebase when aircraft moves more than this distance from (0,0)
const ORIGIN_REBASE_THRESHOLD: float = 5000.0  # meters

# Assuming your JSBGodot node is a child of the current node
var jsb_node: Node
var active_camera: Camera3D
var camera1: Camera3D
var camera2: Camera3D
var camera1_base_transform: Transform3D  # Store original camera transform

# Pilot head look angles (in degrees) - for cockpit camera
var head_yaw: float = 0.0    # Left/Right
var head_pitch: float = 0.0  # Up/Down
const HEAD_YAW_MAX: float = 120.0   # Max look left/right
const HEAD_PITCH_MAX: float = 60.0  # Max look up/down
const HEAD_LOOK_SPEED: float = 90.0 # Degrees per second

# Camera mode enum
enum CameraMode { COCKPIT, ORBIT, CHASE }
var camera_mode: int = CameraMode.COCKPIT

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
var chase_smooth: float = 5.0     # Smoothing factor (higher = faster follow)

# Control surface nodes
var rudder_node: Node3D
var rudder_base_transform: Transform3D  # Store original transform
const RUDDER_MAX_ANGLE: float = 16.0  # Realistic rudder deflection
# Disable pivot offset - not needed now that we use the MeshInstance3D
var rudder_pivot_offset: Vector3 = Vector3(0, 0, 0)

# Propeller
var propeller_node: Node3D
var propeller_blur_disc: MeshInstance3D  # Blur disc mesh
var propeller_blur_material: Material  # Blur material (can be Standard or Shader)
var propeller_rotation: float = 0.0  # Current rotation angle
var visual_rpm: float = 0.0  # Smoothed RPM for display (decays when engine off)
const PROPELLER_DECAY_RATE: float = 1.5  # How fast visual RPM decays to 0 when engine off (per second)
const PROPELLER_MAX_RPM: float = 2700.0  # Max RPM for reference
const PROPELLER_BLUR_START_RPM: float = 900.0  # RPM where blur disc appears (above idle)
const PROPELLER_HIDE_BLADES_RPM: float = 1200.0  # RPM where real blades hidden
const PROPELLER_DISC_RADIUS: float = 0.95  # Radius of blur disc in meters

# Ailerons
var left_aileron_node: Node3D
var right_aileron_node: Node3D
var left_aileron_base_transform: Transform3D
var right_aileron_base_transform: Transform3D
const AILERON_MAX_ANGLE: float = 20.0  # Max aileron deflection in degrees

# Elevators
var left_elevator_node: Node3D
var right_elevator_node: Node3D
var left_elevator_base_transform: Transform3D
var right_elevator_base_transform: Transform3D
const ELEVATOR_MAX_ANGLE: float = 25.0  # Max elevator deflection in degrees

# Flaps
var left_flap_node: Node3D
var right_flap_node: Node3D
var left_flap_base_transform: Transform3D
var right_flap_base_transform: Transform3D
const FLAP_MAX_ANGLE: float = 30.0  # C172p max flap deflection (0, 10, 20, 30 degrees)

# Front wheel (nose gear steering)
var front_wheel_node: Node3D
var front_wheel_base_transform: Transform3D
const FRONT_WHEEL_MAX_ANGLE: float = 30.0  # Max steering angle

# Terrain3D reference for ground height queries
var terrain3d_node: Node

# Slew mode - free movement without physics
var slew_mode: bool = false
var slew_speed: float = 400.0  # Base speed in m/s
var slew_fast_multiplier: float = 5.0  # Speed multiplier when holding boost
var slew_vertical_speed: float = 60.0  # Vertical movement speed
var _slew_key_held: bool = false  # Prevent key repeat

# Teleport cooldown: pin the aircraft to the teleport target and skip
# origin rebase until JSBSim has stabilized.  C++ _physics_process sets
# the AC position directly from JSBSim output, which can be garbage on
# the first frames after reinitialize; we override it in _process.
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
const MAGNETO_BASE_POSITION: float = 6.5   # Model default orientation (between R and L)
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
const FLAP_POSITIONS: Array = [0.0, 0.333, 0.667, 1.0]  # JSBSim flap-cmd-norm values
const FLAP_NAMES: Array = ["UP", "10°", "20°", "30°"]

# ── Engine sound ───────────────────────────────────────────────────────────
var _engine_audio: AudioStreamPlayer = null
const ENGINE_SOUND_PATH := "res://sounds/c172_engine_inflight.ogg"
# RPM-to-pitch mapping: idle (~700 RPM) → 0.7, max (2700 RPM) → 1.6
const ENGINE_PITCH_MIN: float = 0.7
const ENGINE_PITCH_MAX: float = 1.6
# RPM-to-volume mapping (before user volume slider): idle → -12 dB, max → +3 dB
const ENGINE_VOL_DB_MIN: float = -12.0
const ENGINE_VOL_DB_MAX: float = 3.0
# Below this RPM the engine sound fades out completely
const ENGINE_SOUND_FADE_RPM: float = 50.0

# FPS counter
var _fps_label: Label = null
var _fps_visible: bool = false
var _fps_key_held: bool = false
var _rebase_label: Label = null
var _rebase_label_frames: int = 0

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

	_rebase_label = Label.new()
	_rebase_label.text = "REBASE"
	_rebase_label.add_theme_font_size_override("font_size", 18)
	_rebase_label.add_theme_color_override("font_color", Color.YELLOW)
	_rebase_label.add_theme_color_override("font_shadow_color", Color.BLACK)
	_rebase_label.add_theme_constant_override("shadow_offset_x", 1)
	_rebase_label.add_theme_constant_override("shadow_offset_y", 1)
	_rebase_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_rebase_label.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_rebase_label.offset_left = -180
	_rebase_label.offset_right = -10
	_rebase_label.offset_top = 30
	_rebase_label.visible = false
	fps_layer.add_child(_rebase_label)

	# Show a lightweight loading screen immediately, before any heavy work.
	_create_loading_screen()

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
	print("Camera1: ", camera1)
	print("Camera2: ", camera2)
	
	# Set Camera1 as the active one initially
	active_camera.current = true
	camera2.current = false
	
	# Get control surface nodes for animation - using the MeshInstance3D nodes
	rudder_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_008/Cessna_Exterior_Body_MAT_0_008_Rudder")
	if rudder_node:
		print("Rudder node found: ", rudder_node)
		rudder_base_transform = rudder_node.transform  # Store original transform
	else:
		print("WARNING: Rudder node not found!")
	
	# Get propeller node - using the MeshInstance3D node
	propeller_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_001/Cessna_Exterior_Body_MAT_0_001_Body_MAT_0")
	if propeller_node:
		print("Propeller node found: ", propeller_node)
		# Create the propeller blur disc
		_create_propeller_blur_disc()
	else:
		print("WARNING: Propeller node not found!")
	
	# Get aileron nodes - using the MeshInstance3D for animation
	left_aileron_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_014/Cessna_Exterior_Body_MAT_0_014_Aileron_Left")
	if left_aileron_node:
		print("Left aileron node found: ", left_aileron_node)
		left_aileron_base_transform = left_aileron_node.transform
	else:
		print("WARNING: Left aileron node not found!")
	
	right_aileron_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_015/Cessna_Exterior_Body_MAT_0_015_Aileron_Right")
	if right_aileron_node:
		print("Right aileron node found: ", right_aileron_node)
		right_aileron_base_transform = right_aileron_node.transform
	else:
		print("WARNING: Right aileron node not found!")
	
	# Get elevator nodes
	left_elevator_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_013/Cessna_Exterior_Body_MAT_0_013_Elevator_Left")
	if left_elevator_node:
		print("Left elevator node found: ", left_elevator_node)
		left_elevator_base_transform = left_elevator_node.transform
	else:
		print("WARNING: Left elevator node not found!")
	
	right_elevator_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_012/Cessna_Exterior_Body_MAT_0_012_Elevator_Right")
	if right_elevator_node:
		print("Right elevator node found: ", right_elevator_node)
		right_elevator_base_transform = right_elevator_node.transform
	else:
		print("WARNING: Right elevator node not found!")
	
	# Get flap nodes
	left_flap_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_016/Cessna_Exterior_Body_MAT_0_016_Flaps_Left")
	if left_flap_node:
		print("Left flap node found: ", left_flap_node)
		left_flap_base_transform = left_flap_node.transform
	else:
		print("WARNING: Left flap node not found!")
	
	right_flap_node = get_node("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_017/Cessna_Exterior_Body_MAT_0_017_Flaps_Right")
	if right_flap_node:
		print("Right flap node found: ", right_flap_node)
		right_flap_base_transform = right_flap_node.transform
	else:
		print("WARNING: Right flap node not found!")
	
	# Get front wheel node for nose gear steering
	# The VehicleWheel3D node is: Cessna_Exterior_Body_MAT_0_021_Body_Front
	front_wheel_node = get_node_or_null("AC/Node3D2/C172P_1/Sketchfab_model/Cessna172_fbx/RootNode/75f3edaeef374a9f89e7b5ef606a0759_fbx/RootNode_001/Cessna-172/Cessna_Exterior/Cessna_Exterior_Body_MAT_0_021/Cessna_Exterior_Body_MAT_0_021_Body_Front")
	if front_wheel_node:
		print("Front wheel node found: ", front_wheel_node)
		front_wheel_base_transform = front_wheel_node.transform
	else:
		print("WARNING: Front wheel node not found!")
	
	# Get mixture lever node — user renamed it in Blender to Dynamic_Switch_006_Mixture
	# Use find_child() to locate it regardless of exact hierarchy path
	var c172_root = get_node_or_null("AC/Node3D2/C172P_1")
	if c172_root:
		mixture_lever_node = c172_root.find_child("Dynamic_Switch_006_Mixture", true, false)
		if not mixture_lever_node:
			# Fallback: try original name from the Sketchfab model
			mixture_lever_node = c172_root.find_child("Dynamic_Switch_006", true, false)
		if mixture_lever_node:
			print("Mixture lever node found: ", mixture_lever_node.name)
			mixture_lever_base_transform = mixture_lever_node.transform
			_setup_mixture_lever_collision()
		else:
			print("WARNING: Mixture lever node not found (tried Dynamic_Switch_006_Mixture and Dynamic_Switch_006)")
	
		# Throttle lever — user renamed it to Dynamic_Switch_005_Throttle
		throttle_lever_node = c172_root.find_child("Dynamic_Switch_005_Throttle", true, false)
		if not throttle_lever_node:
			throttle_lever_node = c172_root.find_child("Dynamic_Switch_005", true, false)
		if throttle_lever_node:
			print("Throttle lever node found: ", throttle_lever_node.name)
			throttle_lever_base_transform = throttle_lever_node.transform
			_setup_throttle_lever_collision()
		else:
			print("WARNING: Throttle lever node not found (tried Dynamic_Switch_005_Throttle and Dynamic_Switch_005)")
	
		# Magneto/ignition switch
		magneto_switch_node = c172_root.find_child("Dynamic_Switch_003_Magnetos_Switch", true, false)
		if not magneto_switch_node:
			magneto_switch_node = c172_root.find_child("Dynamic_Switch_003", true, false)
		if magneto_switch_node:
			print("Magneto switch node found: ", magneto_switch_node.name)
			magneto_switch_base_transform = magneto_switch_node.transform
			_setup_magneto_switch_collision()
			# Default to BOTH (position 3) — already set from init
			_apply_magneto_position()
		else:
			print("WARNING: Magneto switch node not found (tried Dynamic_Switch_003_Magnetos_Switch and Dynamic_Switch_003)")
	
		# Flap lever
		flap_lever_node = c172_root.find_child("Dynamic_Switch_007_Flaps_Leveler", true, false)
		if not flap_lever_node:
			flap_lever_node = c172_root.find_child("Dynamic_Switch_007", true, false)
		if flap_lever_node:
			print("Flap lever node found: ", flap_lever_node.name)
			flap_lever_base_transform = flap_lever_node.transform
			_setup_flap_lever_collision()
			# Sync lever position to current JSBSim flap state
			if jsb_node:
				var cur_flaps := float(jsb_node.get_flaps())
				flap_position = _flaps_norm_to_position(cur_flaps)
			_apply_flap_position()
		else:
			print("WARNING: Flap lever node not found (tried Dynamic_Switch_007_Flaps_Leveler and Dynamic_Switch_007)")
	
	# Get Terrain3D node for ground height queries
	terrain3d_node = get_node_or_null("Terrain3D")
	if terrain3d_node:
		print("Terrain3D node found: ", terrain3d_node)
	else:
		print("WARNING: Terrain3D node not found - ground collision may not work correctly!")

	# ── Deferred scenery loading: skip all heavy work at startup ──────
	# Terrain, buildings, roads, water, runways, and clipmap terrain are
	# loaded only after the user clicks Fly! in the welcome screen.
	# This makes the welcome screen appear almost instantly.

	# Disable scenery loaders so their _process() does not auto-load tiles
	for loader_name in ["BuildingLoader", "RoadLoader", "WaterLoader", "RunwayLoader", "ObjectLoader"]:
		var loader = get_node_or_null(loader_name)
		if loader and "enabled" in loader:
			loader.enabled = false

	# Disable TerrainClipmap processing until flight starts
	var clipmap = get_node_or_null("TerrainClipmap")
	if clipmap:
		clipmap.set_process(false)

	# Print the aircraft model hierarchy to find control surfaces
	print("=== Aircraft Node Hierarchy ===")
	print_node_tree($AC, 0)
	print("=== End Hierarchy ===")

	# Skip the loading screen — show welcome screen directly.
	_loading_done = true
	if _loading_screen:
		_loading_screen.hide_loading()
	_create_welcome_screen()
	_create_settings_screen()

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

	# Use visual_rpm which already handles engine-off decay
	var rpm = visual_rpm

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


# Create the propeller blur disc mesh
func _create_propeller_blur_disc() -> void:
	# Use StandardMaterial3D - shaders have transparency issues
	var blur_mat = StandardMaterial3D.new()
	blur_mat.albedo_color = Color(0.15, 0.15, 0.18, 0.1)  # Dark gray, more transparent
	blur_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	blur_mat.cull_mode = BaseMaterial3D.CULL_DISABLED  # Visible from both sides
	blur_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED  # No lighting
	propeller_blur_material = blur_mat
	
	# Get the propeller mesh's bounding box to determine disc size
	if propeller_node is MeshInstance3D:
		var mesh_inst = propeller_node as MeshInstance3D
		var aabb = mesh_inst.get_aabb()
		var mesh_center = aabb.position + aabb.size / 2.0
		
		# Propeller diameter is the Y size (blades extend along Y axis)
		var prop_diameter = aabb.size.y
		var prop_radius = prop_diameter / 2.0
		print("Propeller diameter: ", prop_diameter, " cm")
		
		# Create a circular disc using CylinderMesh (very thin cylinder = disc)
		var disc_mesh = CylinderMesh.new()
		disc_mesh.top_radius = prop_radius
		disc_mesh.bottom_radius = prop_radius
		disc_mesh.height = 0.5  # Very thin
		disc_mesh.radial_segments = 32  # Smooth circle
		disc_mesh.rings = 1
		
		# Create MeshInstance3D for the blur disc
		propeller_blur_disc = MeshInstance3D.new()
		propeller_blur_disc.mesh = disc_mesh
		propeller_blur_disc.material_override = propeller_blur_material
		propeller_blur_disc.visible = false  # Start hidden
		
		# Add as sibling to propeller's parent
		var prop_parent = propeller_node.get_parent()
		if prop_parent:
			prop_parent.add_child(propeller_blur_disc)
			# Position at the center of the propeller mesh geometry
			propeller_blur_disc.position = mesh_center
			# CylinderMesh stands upright (Y axis), rotate to face forward (Z axis)
			propeller_blur_disc.rotation_degrees = Vector3(90, 0, 0)
			print("Blur disc created with diameter: ", prop_diameter)
	else:
		print("WARNING: Propeller is not a MeshInstance3D!")



# Deferred terrain initialization - waits for first physics frame for raycast to work
var _pending_terrain_init: bool = false
var _terrain_data_for_init = null

# Initialize JSBSim with correct terrain elevation at startup
# This must be called BEFORE JSBSim runs its physics, setting terrain during IC
func _initialize_jsbsim_on_terrain(terrain_data) -> void:
	# Store for deferred initialization in first physics frame
	_pending_terrain_init = true
	_terrain_data_for_init = terrain_data
	print("=== Terrain initialization queued for first physics frame ===")


# Called from _physics_process on first frame to do actual terrain positioning
func _do_deferred_terrain_init() -> void:
	if not _pending_terrain_init or _terrain_data_for_init == null:
		return
	_pending_terrain_init = false
	
	var terrain_data = _terrain_data_for_init
	_terrain_data_for_init = null
	
	var ac_node = get_node_or_null("AC")
	if not ac_node or not jsb_node:
		print("Cannot initialize JSBSim on terrain - missing AC or JSBGodot node")
		return
	
	# Use stored target position (set by _do_first_flight)
	var target_x: float = _first_flight_target.get("godot_x", 980.8)
	var target_z: float = _first_flight_target.get("godot_z", -227.1)
	var target_heading: float = _first_flight_target.get("heading_deg", 250.0)
	
	# Move aircraft to target position
	ac_node.global_position.x = target_x
	ac_node.global_position.z = target_z
	ac_node.rotation.y = -deg_to_rad(target_heading)
	
	# DEBUG: Check terrain heights at multiple points
	var start_pos = Vector3(ac_node.global_position.x, 0, ac_node.global_position.z)
	print("=== TERRAIN HEIGHT DEBUG ===")
	for offset in [Vector3(0, 0, 0), Vector3(100, 0, 100), Vector3(-100, 0, -100), Vector3(500, 0, 500)]:
		var test_pos = start_pos + offset
		var h = terrain_data.get_height(test_pos)
		print("  get_height(", test_pos.x, ", ", test_pos.z, ") = ", h, "m = ", h * 3.28084, " ft")
	
	# Get terrain height from API
	var api_height = terrain_data.get_height(start_pos)
	print("=== Terrain3D API get_height(", start_pos.x, ", ", start_pos.z, ") = ", api_height, "m ===")
	
	# Try Terrain3D's get_intersection() method (raymarching, no physics needed)
	var visual_terrain_height: float = api_height
	var terrain3d = terrain3d_node  # The actual Terrain3D node
	
	if terrain3d and terrain3d.has_method("get_intersection"):
		var ray_origin = Vector3(start_pos.x, 2000, start_pos.z)
		var ray_dir = Vector3(0, -1, 0)  # Straight down
		var intersection = terrain3d.get_intersection(ray_origin, ray_dir)
		if intersection != Vector3.INF and not is_nan(intersection.y):
			visual_terrain_height = intersection.y
			print("=== Terrain3D get_intersection hit at Y: ", visual_terrain_height, "m ===")
		else:
			print("=== Terrain3D get_intersection returned no hit ===")
	else:
		print("=== Terrain3D get_intersection not available ===")
	
	# Fallback to physics raycast
	if visual_terrain_height == api_height:
		var space_state = get_world_3d().direct_space_state
		var ray_origin = Vector3(start_pos.x, 2000, start_pos.z)  # Start high above
		var ray_end = Vector3(start_pos.x, -500, start_pos.z)     # Go below expected terrain
		var query = PhysicsRayQueryParameters3D.create(ray_origin, ray_end)
		query.collide_with_areas = false
		query.collide_with_bodies = true
		
		var result = space_state.intersect_ray(query)
		
		if result:
			visual_terrain_height = result.position.y
			print("=== Physics raycast hit at Y: ", visual_terrain_height, "m ===")
		else:
			print("=== Physics raycast FAILED - using API height: ", visual_terrain_height, "m ===")
	
	# WORKAROUND: Previously we had issues with terrain visual mesh offset
	# Now handled by godot_terrain_y_offset in JSBGodot C++ code
	# The collision/API height is the correct reference point
	
	# Position aircraft on the visual terrain (Godot Y coordinate)
	# Note: JSBSim will take over position control after initialization
	var gear_height = 2.0
	var new_y = visual_terrain_height + gear_height
	
	print("=== Initial aircraft position at Godot Y: ", new_y, "m (terrain: ", visual_terrain_height, "m) ===")
	
	ac_node.global_position.y = new_y
	
	# For JSBSim, we need the REAL terrain elevation (for altimeter, physics, etc.)
	# The offset converts Godot Y to JSBSim ASL elevation
	# For terrain imported with offset 0: Godot Y already equals real ASL elevation
	jsbsim_elevation_offset = 0.0  # No offset needed when terrain imported with offset 0
	var jsbsim_terrain_elevation = api_height + jsbsim_elevation_offset
	print("=== JSBSim terrain elevation: ", jsbsim_terrain_elevation, "m (", jsbsim_terrain_elevation * 3.28084, " ft) ===")
	print("=== JSBSim elevation offset: ", jsbsim_elevation_offset, "m (Godot Y + offset = JSBSim ASL) ===")
	
	# Small vertical offset to lift aircraft visual model so wheels don't clip into terrain
	# This is added to altitude_m in C++ to get final Godot Y position
	const WHEEL_GROUND_CLEARANCE: float = 0.1  # Meters - adjust if wheels clip or float
	print("=== Setting wheel clearance offset: ", WHEEL_GROUND_CLEARANCE, "m ===")
	jsb_node.set_godot_terrain_y_offset(WHEEL_GROUND_CLEARANCE)
	
	# Initialize JSBSim with terrain height, start position, and heading
	var start_godot_pos = ac_node.global_position
	jsb_node.initialize_at_terrain(jsbsim_terrain_elevation, start_godot_pos, target_heading)
	print("=== JSBSim initialized at Godot pos: ", start_godot_pos, " heading: ", target_heading, "° ===")
	
	terrain_initialized = true
	print("=== JSBSim terrain initialization complete ===")


# Position aircraft on terrain at startup
func _position_aircraft_on_terrain(terrain_data) -> void:
	var ac_node = get_node_or_null("AC")
	if not ac_node:
		return
	
	# Get current aircraft position
	var ac_position = ac_node.global_position
	
	# Query terrain height at aircraft position
	if not terrain_data.has_method("get_height"):
		print("Terrain3DData does not have get_height method - cannot position aircraft")
		return
	
	var terrain_height = terrain_data.get_height(ac_position)
	
	# Check for NAN (position outside defined regions)
	if is_nan(terrain_height):
		print("Aircraft position is outside terrain regions - keeping original position")
		return
	
	# Position aircraft on terrain (add small offset for gear height)
	var gear_height = 1.5  # Approximate landing gear height in meters
	var new_y = terrain_height + gear_height
	
	print("Positioning aircraft on terrain:")
	print("  Terrain height: ", terrain_height, "m")
	print("  Aircraft old Y: ", ac_position.y, "m")
	print("  Aircraft new Y: ", new_y, "m (terrain + ", gear_height, "m gear)")
	
	ac_node.global_position.y = new_y
	# Note: Don't call set_terrain_elevation here - JSBSim may not be ready
	# The per-frame _update_terrain_elevation will handle it once simulation starts


# Update terrain elevation for JSBSim ground collision (per-frame, after init)
# This updates the terrain elevation as the aircraft moves over varying terrain
var terrain_debug_printed: bool = false
var terrain_initialized: bool = false
var terrain_init_frame_count: int = 0
const TERRAIN_INIT_DELAY_FRAMES: int = 60  # Wait 60 frames (~1 sec) before per-frame updates
# Offset to convert Godot Y coordinates to JSBSim ASL elevation
# Set during initialization, used for per-frame terrain updates
var jsbsim_elevation_offset: float = 0.0  # 0 when terrain imported with offset 0

func _process_rebase_cooldown() -> void:
	## Handle post-teleport position pinning and JSBSim release.
	## Extracted so it can run both in normal flight and during fly-loading.
	var ac_guard = get_node_or_null("AC")
	if ac_guard and terrain_initialized and _rebase_cooldown > 0:
		ac_guard.global_position = _teleport_pin_pos
		_rebase_cooldown -= 1
		if _rebase_cooldown == 0:
			# Terrain should be loaded by now — update height at pin position
			if is_instance_valid(terrain3d_node):
				var td = terrain3d_node.get("data")
				if td and td.has_method("get_height"):
					var h = td.get_height(Vector3(_teleport_pin_pos.x, 0, _teleport_pin_pos.z))
					if not is_nan(h) and h > 0.1:
						_teleport_pin_pos.y = h + 2.1  # terrain + gear clearance
						ac_guard.global_position = _teleport_pin_pos
						print("Teleport: updated terrain height to %.1fm" % h)
			# Re-initialize JSBSim at the final pinned position and release
			if jsb_node:
				if jsb_node.has_method("reinitialize_from_godot_position"):
					jsb_node.reinitialize_from_godot_position(ac_guard.global_position, ac_guard.rotation.y)
				_update_terrain_elevation()
				if jsb_node.has_method("set_slew_mode"):
					jsb_node.set_slew_mode(false)
			print("Teleport cooldown expired – JSBSim released at ", ac_guard.global_position)

func _update_terrain_elevation() -> void:
	# Skip if not yet initialized (initialization happens in _ready via _initialize_jsbsim_on_terrain)
	if not terrain_initialized:
		return
	
	# Skip first N frames to let JSBSim stabilize
	terrain_init_frame_count += 1
	if terrain_init_frame_count < TERRAIN_INIT_DELAY_FRAMES:
		return
	
	if not jsb_node:
		return
	if not is_instance_valid(terrain3d_node):
		return
	
	# Get aircraft position in world space
	var ac_node = get_node_or_null("AC")
	if not ac_node:
		return
	
	var ac_position = ac_node.global_position
	
	# Query terrain height at aircraft position
	# Terrain3D API: terrain3d.data.get_height(global_position)
	var terrain_data = terrain3d_node.get("data")
	if not is_instance_valid(terrain_data):
		return
	
	if not terrain_data.has_method("get_height"):
		if not terrain_debug_printed:
			print("Terrain3DData does not have get_height method!")
			terrain_debug_printed = true
		return
	
	var terrain_height_godot = terrain_data.get_height(ac_position)
	
	# Check for NAN (position outside defined regions)
	if is_nan(terrain_height_godot):
		terrain_height_godot = 0.0  # Default to sea level if outside terrain
	
	# Convert Godot Y coordinate to JSBSim ASL elevation
	# JSBSim expects real-world ASL, so we add the offset
	var terrain_elevation_jsbsim = terrain_height_godot + jsbsim_elevation_offset
	
	# Set terrain elevation in JSBSim (in meters - the method converts to feet)
	jsb_node.set_terrain_elevation(terrain_elevation_jsbsim)


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	# Do deferred terrain initialization if pending (needs physics world ready)
	if _pending_terrain_init:
		_do_deferred_terrain_init()

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

	# Escape key: return to welcome screen
	if Input.is_key_pressed(KEY_ESCAPE):
		if _welcome_screen:
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
			if _rebase_label and not _fps_visible:
				_rebase_label.visible = false
				_rebase_label_frames = 0
	else:
		_fps_key_held = false

	# --- Post-teleport position guard ---
	_process_rebase_cooldown()
	
	# Countdown rebase label visibility
	if _rebase_label_frames > 0:
		_rebase_label_frames -= 1
		if _rebase_label_frames == 0 and _rebase_label:
			_rebase_label.visible = false
	
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
		# Floating origin: rebase if aircraft is too far from Godot origin
		_check_origin_rebase()
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
	
	# Animate control surfaces
	animate_control_surfaces()
	
	# Update terrain elevation for JSBSim ground collision
	_update_terrain_elevation()
	
	# Floating origin: rebase if aircraft is too far from Godot origin
	_check_origin_rebase()
		
	if jsb_node:
		var airspeed = jsb_node.get_airspeed_knots()
		var altitude_ft = jsb_node.get_altitude_ft()
		var vertical_spd = jsb_node.get_vertical_speed_fpm()
		var throttle = jsb_node.get_input_throttle()
		var throttle_percent = throttle * 100.0
		var heading = jsb_node.get_heading()

		$Control/Label.text = "Airspeed: " + str(round(airspeed)) + " knots"
		$Control/Label2.text = "Altitude: " + str(round(altitude_ft)) + " feet"
		$Control/Label3.text = "Verical Speed: " + str(round(vertical_spd)) + " ft/m"
		$Control/Label4.text = "Throttle: " +str(round(throttle_percent)) + " %"
		$Control/Label5.text = "Heading: " +str(round(heading))
		
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
			print("  Reinitializing JSBSim at Godot position: ", pos)
			print("  Heading: ", rad_to_deg(heading), " degrees")
			print("  Restoring airspeed: %.1f kts" % _pre_slew_airspeed_kts)
			
			# Reinitialize with preserved airspeed
			if jsb_node.has_method("reinitialize_from_godot_position_with_speed"):
				jsb_node.reinitialize_from_godot_position_with_speed(pos, heading, _pre_slew_airspeed_kts)
			elif jsb_node.has_method("reinitialize_from_godot_position"):
				jsb_node.reinitialize_from_godot_position(pos, heading)
			
			# Update terrain elevation at new position
			_update_terrain_elevation()


## Handle slew mode movement using gamepad
func handle_slew_mode(delta: float) -> void:
	var ac_node = $AC
	if ac_node == null:
		return
	
	# Speed multiplier (hold shift for faster movement)
	var speed_mult = slew_fast_multiplier if Input.is_key_pressed(KEY_SHIFT) else 1.0
	
	# Get gamepad input
	# Left stick: horizontal movement (forward/back, strafe)
	var move_x = Input.get_joy_axis(0, JOY_AXIS_LEFT_X)  # Left/Right strafe
	var move_z = Input.get_joy_axis(0, JOY_AXIS_LEFT_Y)  # Forward/Back
	
	# Right stick: rotation and vertical
	var rotate_y = Input.get_joy_axis(0, JOY_AXIS_RIGHT_X)  # Yaw rotation
	var move_y = -Input.get_joy_axis(0, JOY_AXIS_RIGHT_Y)  # Vertical (inverted)
	
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
	
	# Apply deadzone
	if abs(move_x) < 0.1:
		move_x = 0.0
	if abs(move_z) < 0.1:
		move_z = 0.0
	if abs(rotate_y) < 0.1:
		rotate_y = 0.0
	if abs(move_y) < 0.1:
		move_y = 0.0
	
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
	var terrain_height = 0.0
	if terrain3d_node:
		var terrain_data = terrain3d_node.get("data")
		if terrain_data and terrain_data.has_method("get_height"):
			terrain_height = terrain_data.get_height(godot_pos)
			if is_nan(terrain_height):
				terrain_height = 0.0
	
	var heading_deg = fmod(rad_to_deg(-ac_node.rotation.y) + 360.0, 360.0)
	$Control/Label.text = "=== SLEW MODE ==="
	$Control/Label2.text = "Height AGL: %.1f m" % (godot_pos.y - terrain_height)
	$Control/Label3.text = "Terrain: %.1f m ASL" % terrain_height
	$Control/Label4.text = "Aircraft: %.1f m ASL" % godot_pos.y
	$Control/Label5.text = "Shift+Y to exit"
	$Control/Label6.text = "Godot: X=%.1f Y=%.1f Z=%.1f" % [godot_pos.x, godot_pos.y, godot_pos.z]
	$Control/Label7.text = "Swiss E: %s, N: %s" % [format_swiss_coord(swiss_e), format_swiss_coord(swiss_n)]
	$Control/Label8.text = "Heading: %.0f°" % heading_deg


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
	
	# Try hat switch axes first (JOY_AXIS 6 = hat X, JOY_AXIS 7 = hat Y on some controllers)
	look_x = Input.get_joy_axis(0, JOY_AXIS_MAX)  # Some controllers use higher axes
	look_y = Input.get_joy_axis(0, JOY_AXIS_MAX)
	
	# If no hat axis input, check D-pad buttons
	if abs(look_x) < 0.1 and abs(look_y) < 0.1:
		if Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_LEFT):
			look_x = -1.0
		elif Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_RIGHT):
			look_x = 1.0
		if Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_UP):
			look_y = -1.0  # Up on D-pad = look up (negative pitch)
		elif Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_DOWN):
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
	
	# Check D-pad buttons
	if Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_LEFT):
		look_x = -1.0
	elif Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_RIGHT):
		look_x = 1.0
	if Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_UP):
		look_y = 1.0   # Up = increase elevation
	elif Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_DOWN):
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
	
	# Gamepad shoulder buttons (L1 = button 4, R1 = button 5)
	if Input.is_joy_button_pressed(0, JOY_BUTTON_LEFT_SHOULDER):
		zoom_in = 1.0
	if Input.is_joy_button_pressed(0, JOY_BUTTON_RIGHT_SHOULDER):
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
	if Input.is_joy_button_pressed(0, JOY_BUTTON_LEFT_SHOULDER):
		zoom_in = 1.0
	if Input.is_joy_button_pressed(0, JOY_BUTTON_RIGHT_SHOULDER):
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
	if Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_UP) or Input.is_key_pressed(KEY_UP):
		height_delta = 1.0
	elif Input.is_joy_button_pressed(0, JOY_BUTTON_DPAD_DOWN) or Input.is_key_pressed(KEY_DOWN):
		height_delta = -1.0
	chase_height += height_delta * ORBIT_ZOOM_SPEED * delta
	chase_height = clamp(chase_height, 0.5, 20.0)
	
	# Target position: behind and above the aircraft
	var target_pos = aircraft_pos - forward * chase_distance + Vector3(0, chase_height, 0)
	
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

# Animate control surfaces based on JSBSim inputs
func animate_control_surfaces() -> void:
	if not jsb_node:
		return
	
	# Animate rudder - rotate around LOCAL Y axis (hinge line as set in Blender)
	if rudder_node:
		var rudder_input = jsb_node.get_input_rudder()  # -1 to 1
		var rudder_angle = -rudder_input * RUDDER_MAX_ANGLE
		
		# Reset to base transform first, then apply LOCAL rotation around Y axis
		rudder_node.transform = rudder_base_transform
		rudder_node.rotate_object_local(Vector3.UP, deg_to_rad(rudder_angle))
	
	# Animate front wheel (nose gear steering) - opposite to rudder
	if front_wheel_node:
		var rudder_input = jsb_node.get_input_rudder()  # -1 to 1
		# Opposite direction: positive rudder (right) = wheel turns left
		var wheel_angle = rudder_input * FRONT_WHEEL_MAX_ANGLE
		
		# Reset to base transform first, then apply rotation
		front_wheel_node.transform = front_wheel_base_transform
		# Try rotating around local Y axis (vertical when wheel is upright)
		front_wheel_node.rotate_object_local(Vector3.UP, deg_to_rad(wheel_angle))
	
	# Animate propeller based on actual RPM from JSBSim
	if propeller_node:
		# Get actual propeller RPM from JSBSim physics
		var rpm = jsb_node.get_propeller_rpm()
		
		# When engine is not running, visually decay RPM to 0
		# (JSBSim reports windmilling RPM even with engine off)
		if jsb_node.get_engine_running():
			visual_rpm = rpm
		else:
			visual_rpm = move_toward(visual_rpm, 0.0, visual_rpm * PROPELLER_DECAY_RATE * get_process_delta_time() + 5.0)
		
		# Convert RPM to degrees per second (RPM * 360 / 60 = RPM * 6)
		var degrees_per_second = visual_rpm * 6.0
		
		# Update rotation (get_process_delta_time for smooth animation)
		propeller_rotation += degrees_per_second * get_process_delta_time()
		
		# Keep angle in 0-360 range
		propeller_rotation = fmod(propeller_rotation, 360.0)
		
		# Apply rotation around Z axis (forward axis of aircraft)
		propeller_node.rotation_degrees.z = propeller_rotation
		
		# Swap between real propeller blades and blur disc based on RPM
		if propeller_blur_disc:
			# Update shader RPM parameter if using shader material
			if propeller_blur_material and propeller_blur_material is ShaderMaterial:
				propeller_blur_material.set_shader_parameter("rpm", visual_rpm)
			
			if visual_rpm < PROPELLER_BLUR_START_RPM:
				# Low RPM - show real blades, hide blur disc
				propeller_node.visible = true
				propeller_blur_disc.visible = false
			elif visual_rpm < PROPELLER_HIDE_BLADES_RPM:
				# Transition zone - show both with blur disc fading in
				propeller_node.visible = true
				propeller_blur_disc.visible = true
			else:
				# High RPM - hide real blades, show only blur disc
				propeller_node.visible = false
				propeller_blur_disc.visible = true
		else:
			# No blur disc available - always show real propeller
			propeller_node.visible = true
	
	# Update engine sound pitch and volume from RPM
	_update_engine_sound()
	
	# Animate ailerons - they move opposite to each other for roll control
	var aileron_input = jsb_node.get_input_aileron()  # -1 to 1
	# Debug aileron input
	#if abs(aileron_input) > 0.01:
	#	print("Aileron input: ", aileron_input)
	
	if left_aileron_node:
		var left_aileron_angle = aileron_input * AILERON_MAX_ANGLE
		left_aileron_node.transform = left_aileron_base_transform
		# Rotate around X axis (along the wing span)
		left_aileron_node.rotation_degrees.x = left_aileron_base_transform.basis.get_euler().x * (180.0/PI) + left_aileron_angle
	
	if right_aileron_node:
		var right_aileron_angle = -aileron_input * AILERON_MAX_ANGLE  # Opposite direction from left aileron
		right_aileron_node.transform = right_aileron_base_transform
		right_aileron_node.rotation_degrees.x = right_aileron_base_transform.basis.get_euler().x * (180.0/PI) + right_aileron_angle
	
	# Animate elevators - both move in the same direction
	var elevator_input = jsb_node.get_input_elevator()  # -1 to 1
	if left_elevator_node:
		var elevator_angle = -elevator_input * ELEVATOR_MAX_ANGLE  # Inverted
		left_elevator_node.transform = left_elevator_base_transform
		left_elevator_node.rotation_degrees.x = left_elevator_base_transform.basis.get_euler().x * (180.0/PI) + elevator_angle
	
	if right_elevator_node:
		var elevator_angle = -elevator_input * ELEVATOR_MAX_ANGLE  # Inverted
		right_elevator_node.transform = right_elevator_base_transform
		right_elevator_node.rotation_degrees.x = right_elevator_base_transform.basis.get_euler().x * (180.0/PI) + elevator_angle
	
	# Animate flaps - both move in the same direction (down only, 0 to max)
	var flap_input = jsb_node.get_flaps()  # 0 to 1
	if left_flap_node:
		var flap_angle = -flap_input * FLAP_MAX_ANGLE  # Inverted
		left_flap_node.transform = left_flap_base_transform
		left_flap_node.rotation_degrees.x = left_flap_base_transform.basis.get_euler().x * (180.0/PI) + flap_angle
	
	if right_flap_node:
		var flap_angle = -flap_input * FLAP_MAX_ANGLE  # Inverted
		right_flap_node.transform = right_flap_base_transform
		right_flap_node.rotation_degrees.x = right_flap_base_transform.basis.get_euler().x * (180.0/PI) + flap_angle
	
	# Animate mixture lever position (reflects current mixture_value)
	# Base transform = idle/cutoff (Blender default); push in (-Z) for full rich
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
	# Base transform = model default. MAGNETO_BASE_POSITION = which switch position the model represents.
	if magneto_switch_node and not _magneto_dragging:
		magneto_switch_node.transform = magneto_switch_base_transform
		var angle_offset := float(magneto_position - MAGNETO_BASE_POSITION) * MAGNETO_STEP_ANGLE
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


# ── Floating Origin ────────────────────────────────────────────────────────

## Check if origin rebase is needed (called every frame during normal flight).
func _check_origin_rebase() -> void:
	if not terrain_initialized or not jsb_node:
		return
	# Skip rebase during teleport cooldown (position is pinned in _process)
	if _rebase_cooldown > 0:
		return
	var ac_node = get_node_or_null("AC")
	if not ac_node:
		return
	
	var pos = ac_node.global_position
	# Sanity check: reject absurd positions (JSBSim not yet stabilized)
	if absf(pos.x) > 1e7 or absf(pos.y) > 1e7 or absf(pos.z) > 1e7:
		return
	if absf(pos.x) > ORIGIN_REBASE_THRESHOLD or absf(pos.z) > ORIGIN_REBASE_THRESHOLD:
		_perform_origin_rebase()


## Perform a floating-origin rebase: shift everything so the aircraft is near (0,Y,0).
func _perform_origin_rebase() -> void:
	var ac_node = get_node_or_null("AC")
	if not ac_node or not jsb_node:
		return
	
	# 1. Compute the aircraft's invariant Swiss coordinates.
	var swiss_e = TERRAIN_CENTER_E - ac_node.global_position.x
	var swiss_n = TERRAIN_CENTER_N + ac_node.global_position.z
	
	# 2. Snap new terrain center to 1024m grid (required for tile alignment).
	var old_center_e = TERRAIN_CENTER_E
	var old_center_n = TERRAIN_CENTER_N
	TERRAIN_CENTER_E = roundf(swiss_e / 1024.0) * 1024.0
	TERRAIN_CENTER_N = roundf(swiss_n / 1024.0) * 1024.0
	
	# 3. Derive the Godot-space shift from the center change.
	#    Godot X = CENTER_E - Swiss_E  →  delta_x = new_E - old_E
	#    Godot Z = Swiss_N - CENTER_N  →  delta_z = -(new_N - old_N)
	var shift := Vector3(
		TERRAIN_CENTER_E - old_center_e,
		0.0,
		-(TERRAIN_CENTER_N - old_center_n)
	)
	if shift == Vector3.ZERO:
		return
	
	var rebase_start_usec := Time.get_ticks_usec()

	print("=== FLOATING ORIGIN REBASE ===")
	print("  AC before: ", ac_node.global_position)
	print("  Center E: %d -> %d" % [int(old_center_e), int(TERRAIN_CENTER_E)])
	print("  Center N: %d -> %d" % [int(old_center_n), int(TERRAIN_CENTER_N)])
	print("  Godot shift: ", shift)
	
	# 4. Shift C++ lat/lon reference by the SAME amount so both coordinate
	#    systems stay in sync (no misalignment from grid snap).
	jsb_node.adjust_origin(shift)
	
	# 5. Shift the aircraft node immediately.
	#    On the next frame copy_outputs_from_JSBSim will produce the same
	#    shifted value, so there is no visual jump.
	ac_node.global_position += shift
	print("  AC after: ", ac_node.global_position)
	
	# 6. Reload terrain at the new center.
	var streamer = get_node_or_null("TerrainStreamer")
	if streamer and streamer.has_method("rebase_center"):
		streamer.rebase_center(int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N))
	
	# 7. Shift all building instances.
	var building_loader = get_node_or_null("BuildingLoader")
	if building_loader and building_loader.has_method("shift_all_buildings"):
		building_loader.shift_all_buildings(shift)
	
	# 8. Shift all road instances.
	var road_loader = get_node_or_null("RoadLoader")
	if road_loader and road_loader.has_method("shift_all_roads"):
		road_loader.shift_all_roads(shift)
	
	# 9. Shift all water instances.
	var water_loader = get_node_or_null("WaterLoader")
	if water_loader and water_loader.has_method("shift_all_water"):
		water_loader.shift_all_water(shift)
	
	# 10. Shift all runway instances.
	var runway_loader = get_node_or_null("RunwayLoader")
	if runway_loader and runway_loader.has_method("shift_all_runways"):
		runway_loader.shift_all_runways(shift)

	# 10b. Shift all custom scenery object instances.
	var object_loader = get_node_or_null("ObjectLoader")
	if object_loader and object_loader.has_method("shift_all_objects"):
		object_loader.shift_all_objects(shift)

	# 11. Rebase clipmap terrain rings.
	var terrain_clipmap = get_node_or_null("TerrainClipmap")
	if terrain_clipmap and terrain_clipmap.has_method("rebase_center"):
		terrain_clipmap.rebase_center(TERRAIN_CENTER_E, TERRAIN_CENTER_N)
	
	var rebase_ms := (Time.get_ticks_usec() - rebase_start_usec) / 1000.0
	if _rebase_label and _fps_visible:
		_rebase_label.text = "REBASE  %.0f ms" % rebase_ms
		_rebase_label.visible = true
		_rebase_label_frames = 500

	print("=== REBASE COMPLETE (%.1f ms) ===" % rebase_ms)


# Convert Swiss LV95 coordinates to WGS84 latitude/longitude
# Based on the approximate formulas from swisstopo
# Returns Vector2(latitude, longitude) in degrees
func lv95_to_wgs84(easting: float, northing: float) -> Vector2:
	# Convert to auxiliary values (shift origin and scale)
	var y_aux = (easting - 2600000.0) / 1000000.0
	var x_aux = (northing - 1200000.0) / 1000000.0
	
	# Calculate latitude in 10000" units
	var lat_aux = 16.9023892 \
		+ 3.238272 * x_aux \
		- 0.270978 * y_aux * y_aux \
		- 0.002528 * x_aux * x_aux \
		- 0.0447 * y_aux * y_aux * x_aux \
		- 0.0140 * x_aux * x_aux * x_aux
	
	# Calculate longitude in 10000" units
	var lon_aux = 2.6779094 \
		+ 4.728982 * y_aux \
		+ 0.791484 * y_aux * x_aux \
		+ 0.1306 * y_aux * x_aux * x_aux \
		- 0.0436 * y_aux * y_aux * y_aux
	
	# Convert to degrees
	var latitude = lat_aux * 100.0 / 36.0
	var longitude = lon_aux * 100.0 / 36.0
	
	return Vector2(latitude, longitude)


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


# ── Loading Screen ─────────────────────────────────────────────────────────

func _create_loading_screen() -> void:
	var script = load("res://loading_screen.gd")
	_loading_screen = CanvasLayer.new()
	_loading_screen.set_script(script)
	add_child(_loading_screen)
	_loading_screen.show_loading()


func _count_loader_pending(node_name: String) -> int:
	## Return the number of pending loads for a loader node (building/road/water).
	var loader = get_node_or_null(node_name)
	if loader and "_pending_loads" in loader:
		var count: int = loader._pending_loads.size()
		if "_loading_tiles" in loader:
			count += loader._loading_tiles.size()
		return count
	return 0


func _update_loading_progress(is_fly_loading: bool) -> void:
	## Monitor all progressive loaders and update the loading bar.
	var streamer = get_node_or_null("TerrainStreamer")

	# ── Terrain tiles ────────────────────────────────────────────────
	var terrain_pending := 0
	var terrain_total := 1
	if streamer and "_pending_loads" in streamer:
		terrain_pending = streamer._pending_loads.size()
		terrain_total = max(1, streamer._loaded_tiles.size() + terrain_pending)

	# ── Scenery loaders (buildings, roads, water) ────────────────────
	var bld_pending := _count_loader_pending("BuildingLoader")
	var road_pending := _count_loader_pending("RoadLoader")
	var water_pending := _count_loader_pending("WaterLoader")
	var scenery_pending := bld_pending + road_pending + water_pending

	# ── Rebase cooldown (only after fly) ─────────────────────────────
	var cooldown_pending := _rebase_cooldown if is_fly_loading else 0

	# ── Compute weighted progress ────────────────────────────────────
	var progress: float
	if is_fly_loading:
		# After Fly!: terrain 40%, cooldown 20%, scenery 40%
		var p_terrain := 1.0 - (float(terrain_pending) / terrain_total)
		var p_cooldown := 1.0 - (float(cooldown_pending) / float(maxi(1, REBASE_COOLDOWN_FRAMES)))
		var scenery_total := maxi(1, scenery_pending + 1)  # +1 avoids instant 100%
		var p_scenery := 1.0 - (float(scenery_pending) / float(scenery_total))
		progress = p_terrain * 0.4 + p_cooldown * 0.2 + p_scenery * 0.4
	else:
		# Startup: terrain 60%, scenery 40%
		var p_terrain := 1.0 - (float(terrain_pending) / terrain_total)
		var scenery_total := maxi(1, scenery_pending + 1)
		var p_scenery := 1.0 - (float(scenery_pending) / float(scenery_total))
		progress = p_terrain * 0.6 + p_scenery * 0.4

	# ── Update loading screen ────────────────────────────────────────
	if _loading_screen:
		_loading_screen.set_progress(progress)
		if terrain_pending > 0:
			_loading_screen.set_status("Loading terrain... %d tiles remaining" % terrain_pending)
		elif scenery_pending > 0:
			var parts: Array[String] = []
			if bld_pending > 0:
				parts.append("%d buildings" % bld_pending)
			if road_pending > 0:
				parts.append("%d roads" % road_pending)
			if water_pending > 0:
				parts.append("%d water" % water_pending)
			_loading_screen.set_status("Loading scenery... %s remaining" % ", ".join(parts))
		elif cooldown_pending > 0:
			_loading_screen.set_status("Stabilizing aircraft...")
		else:
			_loading_screen.set_status("Ready")

	# ── Check completion ─────────────────────────────────────────────
	var all_done := terrain_pending == 0 and scenery_pending == 0 and cooldown_pending == 0
	if not all_done:
		return

	if is_fly_loading:
		# Post-Fly loading complete — dismiss and start flying
		_fly_loading = false
		if _loading_screen:
			_loading_screen.hide_loading()
		print("Fly loading complete — simulation active")
	else:
		# Startup loading complete — show welcome screen
		_loading_done = true
		if _loading_screen:
			_loading_screen.hide_loading()
		_create_welcome_screen()
		_create_settings_screen()
		print("Loading complete — showing welcome screen")


# ── Welcome / Settings Screens ─────────────────────────────────────────────

func _create_welcome_screen() -> void:
	var script = load("res://welcome_screen.gd")
	_welcome_screen = CanvasLayer.new()
	_welcome_screen.set_script(script)
	add_child(_welcome_screen)
	_welcome_screen.flight_requested.connect(_on_welcome_flight_requested)
	_welcome_screen.settings_requested.connect(_on_welcome_settings_requested)

	# Populate airports from POILoader
	var poi_loader = get_node_or_null("POILoader")
	if poi_loader and poi_loader.airports.size() > 0:
		_welcome_screen.populate_airports(poi_loader.airports)
	else:
		# POILoader may still be loading — try again next frame
		_welcome_screen.call_deferred("populate_airports_from_loader", poi_loader)

	_welcome_screen.show_welcome()


func _create_settings_screen() -> void:
	var script = load("res://settings_screen.gd")
	_settings_screen = CanvasLayer.new()
	_settings_screen.set_script(script)
	add_child(_settings_screen)
	_settings_screen.closed.connect(_on_settings_closed)


## Handle Fly! from welcome screen — teleport to the selected airport
func _on_welcome_flight_requested(aircraft_id: String, swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== WELCOME: Flight requested ===")
	print("  Aircraft: %s" % aircraft_id)
	print("  Target: E=%.0f N=%.0f Heading=%.0f°" % [swiss_e, swiss_n, heading_deg])

	# Show loading screen while destination loads
	_fly_loading = true
	if _loading_screen:
		_loading_screen.set_progress(0.0)
		_loading_screen.set_status("Loading terrain...")
		_loading_screen.show_loading()

	if _first_flight:
		_do_first_flight(swiss_e, swiss_n, heading_deg)
		_first_flight = false
	else:
		_on_teleport_requested(swiss_e, swiss_n, heading_deg)


## First flight: load terrain, initialize JSBSim, enable scenery loaders.
## Called instead of _on_teleport_requested on the very first Fly! click.
func _do_first_flight(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	print("=== FIRST FLIGHT: Loading scenery for E=%.0f N=%.0f ===" % [swiss_e, swiss_n])

	var ac_node = get_node_or_null("AC")
	if not ac_node:
		push_error("_do_first_flight: missing AC node")
		return

	# 1. Set terrain center aligned to 1024m grid around target
	TERRAIN_CENTER_E = roundf(swiss_e / 1024.0) * 1024.0
	TERRAIN_CENTER_N = roundf(swiss_n / 1024.0) * 1024.0
	print("  Terrain center: E=%d N=%d" % [int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N)])

	# Update TerrainStreamer center coordinates (including internal tile indices)
	var streamer = get_node_or_null("TerrainStreamer")
	if streamer:
		streamer.terrain_center_e = int(TERRAIN_CENTER_E)
		streamer.terrain_center_n = int(TERRAIN_CENTER_N)
		# Update internal tile indices derived from center
		if "_tile_size" in streamer and streamer._tile_size > 0:
			streamer._center_ei = streamer.terrain_center_e / streamer._tile_size
			streamer._center_ni = streamer.terrain_center_n / streamer._tile_size

	# 2. Compute Godot position for the target
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

	# 4. Load terrain tiles around the target position
	if streamer:
		print("  Loading initial terrain...")
		var loaded = streamer.initial_load(ac_node.global_position)
		print("  TerrainStreamer: %d regions loaded" % loaded)

	# 5. Queue deferred JSBSim initialization (needs terrain data + physics frame)
	if terrain3d_node:
		var tdata = terrain3d_node.get("data")
		if tdata:
			_initialize_jsbsim_on_terrain(tdata)

	# 6. Enable scenery loaders
	for loader_name in ["BuildingLoader", "RoadLoader", "WaterLoader", "RunwayLoader", "ObjectLoader"]:
		var loader = get_node_or_null(loader_name)
		if loader and "enabled" in loader:
			loader.enabled = true
			print("  Enabled %s" % loader_name)

	# 7. Enable TerrainClipmap and rebase
	var clipmap = get_node_or_null("TerrainClipmap")
	if clipmap:
		if clipmap.has_method("rebase_center"):
			clipmap.rebase_center(TERRAIN_CENTER_E, TERRAIN_CENTER_N)
		clipmap.set_process(true)
		print("  Enabled TerrainClipmap")

	# 8. Set teleport pin so aircraft stays in place until terrain settles
	_teleport_pin_pos = ac_node.global_position
	_rebase_cooldown = REBASE_COOLDOWN_FRAMES

	print("=== FIRST FLIGHT: scenery loading initiated ===")


func _on_welcome_settings_requested() -> void:
	if _welcome_screen:
		_welcome_screen.visible = false
	if _settings_screen:
		_settings_screen.show_settings()


func _on_settings_closed() -> void:
	if _settings_screen:
		_settings_screen.hide_settings()
	if _welcome_screen:
		_welcome_screen.visible = true
		_welcome_screen.show_welcome()


# ── Position / Teleport ────────────────────────────────────────────────────

## Show the position dialog (Shift+P)
func _show_position_dialog() -> void:
	# Create dialog on first use
	if _position_dialog == null:
		var dialog_script = load("res://position_dialog.gd")
		_position_dialog = Window.new()
		_position_dialog.set_script(dialog_script)
		add_child(_position_dialog)
		_position_dialog.teleport_requested.connect(_on_teleport_requested)

	# Populate airports from POILoader
	var poi_loader = get_node_or_null("POILoader")
	if poi_loader and poi_loader.airports.size() > 0:
		_position_dialog.populate_airports(poi_loader.airports)

	# Show current position
	var ac_node = get_node_or_null("AC")
	if ac_node:
		var pos = ac_node.global_position
		var swiss_e = TERRAIN_CENTER_E - pos.x
		var swiss_n = TERRAIN_CENTER_N + pos.z
		var heading_deg = rad_to_deg(ac_node.rotation.y)
		# Normalize heading to 0-360
		if heading_deg < 0:
			heading_deg += 360.0
		_position_dialog.set_current_position(swiss_e, swiss_n, heading_deg)

	_position_dialog.popup_centered()


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

	# 2. Snap new terrain center to 1024m grid around target
	var old_center_e = TERRAIN_CENTER_E
	var old_center_n = TERRAIN_CENTER_N
	TERRAIN_CENTER_E = roundf(swiss_e / 1024.0) * 1024.0
	TERRAIN_CENTER_N = roundf(swiss_n / 1024.0) * 1024.0

	# 3. Compute Godot-space shift from center change
	var shift := Vector3(
		TERRAIN_CENTER_E - old_center_e,
		0.0,
		-(TERRAIN_CENTER_N - old_center_n)
	)

	# 4. Update JSBSim's coordinate reference
	if shift != Vector3.ZERO:
		jsb_node.adjust_origin(shift)

	# 5. Reload terrain around new center
	var streamer = get_node_or_null("TerrainStreamer")
	if streamer and streamer.has_method("rebase_center"):
		streamer.rebase_center(int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N))

	# 6. Shift existing building instances
	if shift != Vector3.ZERO:
		var building_loader = get_node_or_null("BuildingLoader")
		if building_loader and building_loader.has_method("shift_all_buildings"):
			building_loader.shift_all_buildings(shift)
		var road_loader = get_node_or_null("RoadLoader")
		if road_loader and road_loader.has_method("shift_all_roads"):
			road_loader.shift_all_roads(shift)
		var water_loader = get_node_or_null("WaterLoader")
		if water_loader and water_loader.has_method("shift_all_water"):
			water_loader.shift_all_water(shift)
		var runway_loader = get_node_or_null("RunwayLoader")
		if runway_loader and runway_loader.has_method("shift_all_runways"):
			runway_loader.shift_all_runways(shift)
		var terrain_clipmap = get_node_or_null("TerrainClipmap")
		if terrain_clipmap and terrain_clipmap.has_method("rebase_center"):
			terrain_clipmap.rebase_center(TERRAIN_CENTER_E, TERRAIN_CENTER_N)

	# 7. Compute new Godot position from Swiss coords
	var new_godot_x = TERRAIN_CENTER_E - swiss_e
	var new_godot_z = swiss_n - TERRAIN_CENTER_N

	# 8. Get terrain height at new position — sample a grid around the
	#    threshold and use the maximum, matching the runway mesh approach.
	var terrain_height := 0.0
	if is_instance_valid(terrain3d_node):
		var terrain_data = terrain3d_node.get("data")
		if terrain_data and terrain_data.has_method("get_height"):
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
				var probe := Vector3(new_godot_x + off.x, 0.0, new_godot_z + off.z)
				var h = terrain_data.get_height(probe)
				if not is_nan(h) and h > max_h:
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
		jsb_node.reinitialize_from_godot_position(ac_node.global_position, ac_node.rotation.y)
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
	# Animate lever: slide along local Z axis
	# Base transform = idle/cutoff (Blender default)
	# mixture_value 1.0 = full rich = pushed in (-Z from base)
	# mixture_value 0.0 = cutoff = at base position
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
	
	# Update visual rotation
	if magneto_switch_node:
		magneto_switch_node.transform = magneto_switch_base_transform
		var angle_offset := float(magneto_position - MAGNETO_BASE_POSITION) * MAGNETO_STEP_ANGLE
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
