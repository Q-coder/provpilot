extends Node3D

# Terrain coordinate system (Swiss LV95)
# These define the relationship between Godot world coords and Swiss coordinates.
# MUST match Scenery3D's origin_east / origin_north (read from
# ProjectSettings in _ready), otherwise the HUD, teleport, and JSBSim will
# disagree with the tiles that Scenery3D actually places. Scenery3D's origin
# is fixed at startup, so there is no runtime floating-origin rebase.
var TERRAIN_CENTER_E: float = 2682880.0  # Swiss Easting at Godot X=0
var TERRAIN_CENTER_N: float = 1283072.0  # Swiss Northing at Godot Z=0

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
var chase_yaw: float = 0.0        # Yaw offset relative to aircraft heading (degrees)
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

# Scenery3D reference for ground height queries
var scenery3d_node: Node
# Vegetation node reference
var vegetation_node: Node

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
const FLY_LOADING_NODATA_MSEC: int = 10000  # Give up faster when zero tiles load (10s)
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

	# The C172P .blend import bakes normals with the opposite handedness from
	# Godot's runtime convention, so without this the sun lights the underside
	# of the wings instead of the top. Flip them in-place once at startup.
	_flip_normals_recursive(ac_node)
	
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

	# Lock TERRAIN_CENTER to Scenery3D's own origin (from ProjectSettings).
	# Swiss <-> Godot conversions and tile placement must share the same origin.
	if ProjectSettings.has_setting("scenery3d/origin_east"):
		TERRAIN_CENTER_E = float(ProjectSettings.get_setting("scenery3d/origin_east"))
	if ProjectSettings.has_setting("scenery3d/origin_north"):
		TERRAIN_CENTER_N = float(ProjectSettings.get_setting("scenery3d/origin_north"))
	print("TERRAIN_CENTER locked to Scenery3D origin: E=%d N=%d" % [int(TERRAIN_CENTER_E), int(TERRAIN_CENTER_N)])

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



# Deferred terrain initialization - waits for first physics frame
var _pending_terrain_init: bool = false
var _terrain_init_print_timer: int = 0

# Initialize JSBSim with correct terrain elevation at startup
# This must be called BEFORE JSBSim runs its physics, setting terrain during IC
func _initialize_jsbsim_on_terrain() -> void:
	# Store for deferred initialization in first physics frame
	_pending_terrain_init = true
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
func _flip_normals_recursive(node: Node) -> void:
	if node.name.begins_with("Dynamic_Switch_"):
		return
	if node is MeshInstance3D and node.name.begins_with("Cessna_Exterior_"):
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
		_flip_normals_recursive(child)


## Query terrain elevation at a world XZ position via Scenery3D.
## Returns NAN when Scenery3D has no data at the location (tile not loaded).
## Callers that need a numeric fallback should check is_nan() explicitly —
## feeding 0.0 to JSBSim as ground elevation causes catastrophic ground impact
## spikes when the real tile (hundreds of metres ASL) finally loads.
func _terrain_elevation_at(x: float, z: float) -> float:
	if not is_instance_valid(scenery3d_node):
		return NAN
	if not scenery3d_node.has_method("get_elevation"):
		return NAN
	var h: float = scenery3d_node.get_elevation(x, z)
	# Older builds of the Scenery3D extension returned 0.0 instead of NaN
	# when a tile was not yet loaded. Swiss LV95 terrain is never below
	# ~193 m ASL, so treat a returned 0.0 as "no data" defensively. This
	# prevents JSBSim from being initialised at sea level while we wait
	# for the real tile to stream in.
	if h == 0.0 or is_nan(h):
		return NAN
	return h


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

	# Move aircraft to target position
	ac_node.global_position.x = target_x
	ac_node.global_position.z = target_z
	ac_node.rotation.y = -deg_to_rad(target_heading)

	var start_pos = Vector3(ac_node.global_position.x, 0, ac_node.global_position.z)
	var api_height: float = _terrain_elevation_at(start_pos.x, start_pos.z)
	# Tile not yet loaded — retry next frame rather than hand 0.0 to JSBSim.
	if is_nan(api_height):
		_pending_terrain_init = true
		return
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
				# Tile still not loaded — keep pinning and retry next frame.
				_rebase_cooldown = 1
				return
			if h > 0.1:
				_teleport_pin_pos.y = h + 2.1  # terrain + gear clearance
				ac_guard.global_position = _teleport_pin_pos
				print("Teleport: updated terrain height to %.1fm" % h)
			# Re-initialize JSBSim at the final pinned position and release
			if jsb_node:
				if jsb_node.has_method("reinitialize_from_godot_position"):
					var h_terr: float = _terrain_elevation_at(ac_guard.global_position.x, ac_guard.global_position.z)
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
	
	# Animate control surfaces
	animate_control_surfaces()
	
	# Update terrain elevation for JSBSim ground collision
	_update_terrain_elevation()
	
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
	$Control/Label.text = "=== SLEW MODE ==="
	$Control/Label2.text = "Height AGL: %.1f m" % (godot_pos.y - terrain_height)
	$Control/Label3.text = "Terrain: %.1f m ASL" % terrain_height
	$Control/Label4.text = "Aircraft: %.1f m ASL" % godot_pos.y
	$Control/Label5.text = "Shift+Y to exit"
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
	if is_instance_valid(scenery3d_node) and scenery3d_node.has_method("get_elevation"):
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

	var progress: float
	if is_fly_loading:
		progress = p_scenery * 0.7 + p_cooldown * 0.3
	else:
		progress = p_scenery

	if _loading_screen:
		_loading_screen.set_progress(progress)
		if not scenery_ready:
			_loading_screen.set_status("Loading scenery... (%d/%d tiles)" % [loaded, total])
		elif cooldown_pending > 0:
			_loading_screen.set_status("Stabilizing aircraft...")
		else:
			_loading_screen.set_status("Ready")

	var all_done := scenery_ready and cooldown_pending == 0
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
	_welcome_screen.continue_requested.connect(_on_welcome_continue_requested)
	_welcome_screen.scenery_setup_requested.connect(_on_welcome_scenery_setup_requested)

	# Provide a default airport list (Scenery3D no longer ships POI data).
	# LSPF Schaffhausen-Schmerlat: small grass airfield, runway 07/25, 600 m.
	# Coordinates verified against visible scenery (≈ 47°41′28″N 8°33′13″E).
	var default_airports: Array[Dictionary] = [
		{
			"icao": "LSPF",
			"name": "Schaffhausen",
			"type": "small_airport",
			"center_e": 2681881.0,
			"center_n": 1282845.0,
			"elevation_m": 464,
			"runways": [
				{
					"designator": "07/25",
					"heading_deg": 70,
					"length_m": 600,
					"surface": "grass",
					# Thresholds derived from centre ±300 m along heading 070°:
					#   dE = 300·sin70° ≈ 282,  dN = 300·cos70° ≈ 103
					"threshold_07": {"e": 2681599.0, "n": 1282742.0},
					"threshold_25": {"e": 2681901.0, "n": 1282855.0},
				},
			],
		},
	]
	_welcome_screen.populate_airports(default_airports)

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

	# 4. Scenery3D streams tiles automatically around load_radius.

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
		print("Scenery sources: no enabled scenery areas in library")
		return

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

	# Apply to Scenery3D node (only override if we have at least one entry,
	# so an empty library falls back to scene-defined defaults).
	# IMPORTANT: do NOT also set the single-path properties (data_path,
	# orthophoto_path) — their setters in C++ CLEAR the array first, which
	# would silently drop all but one path. buildings_path stays as a legacy
	# fallback for older Scenery3D builds that do not expose buildings_paths.
	if terrain_paths.size() > 0:
		scenery3d_node.data_paths = terrain_paths
	if ortho_paths.size() > 0:
		scenery3d_node.orthophoto_paths = ortho_paths
	if building_paths.size() > 0:
		if scenery3d_node.has_method("set_buildings_paths"):
			scenery3d_node.buildings_paths = building_paths
		else:
			scenery3d_node.buildings_path = building_paths[0]
	if water_paths.size() > 0:
		scenery3d_node.water_paths = water_paths
	if road_paths.size() > 0:
		scenery3d_node.road_paths = road_paths

	# Vegetation node uses its own paths
	if is_instance_valid(vegetation_node) and vegetation_paths.size() > 0:
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
