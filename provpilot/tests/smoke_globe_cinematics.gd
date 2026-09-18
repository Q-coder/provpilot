## Smoke test for the C++ globe cinematics modules (IntroZoom + Atmosphere).
##
## Boots the real main scene, starts a first flight to an airport with a runway
## threshold, then verifies:
##   1. the intro zoom is playing from space (intro camera current, fog off,
##      sky-shader background, coarse globe visible) and
##   2. pressing SPACE skips it and hands off seamlessly to orbit mode
##      (camera_mode/active_camera mirrors synced, CameraRig in MODE_ORBIT,
##      fog back on, coarse globe masked under the detailed patch).
##
## Run: godot --headless res://tests/smoke_globe_cinematics.tscn
extends Node

const MAIN_SCENE := "res://node_3d.tscn"
const TIMEOUT_FRAMES := 9000


func _ready() -> void:
	print("SMOKE-GLOBE: starting (timeout %d frames)" % TIMEOUT_FRAMES)
	# Parsed key events must update the raw key state immediately (the C++
	# module reads Input.is_key_pressed), so disable input accumulation.
	Input.set_use_accumulated_input(false)
	var app_scene: PackedScene = load(MAIN_SCENE) as PackedScene
	if app_scene == null:
		fail("could not load main scene at " + MAIN_SCENE)
		return
	var app: Node = app_scene.instantiate()
	add_child(app)
	app_ref = app


var app_ref: Node = null
var frame := 0
var phase := "boot"
var phase_frame := 0

# Key-press step machine (mirrors smoke_slew_mode.gd).
var steps: Array = []
var steps_next := ""
var step_idx := 0
var step_frame := 0
var steps_done_frame := 0
var pending_release: Array = []


var fails: Array = []


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


# Fatal: the test cannot continue meaningfully.
func fail(msg: String) -> void:
	printerr("SMOKE-GLOBE FAIL [%s]: %s" % [phase, msg])
	get_tree().quit(1)


# ── Key helpers (same pattern as smoke_slew_mode.gd) ────────────────────────

# This custom engine build does not expose the static Input.key_press/release
# helpers, so drive the Input singleton through parsed key events instead.
func press_key(k: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.pressed = true
	Input.parse_input_event(ev)


func release_key(k: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.pressed = false
	Input.parse_input_event(ev)


func release_held_keys() -> void:
	for k in pending_release:
		release_key(k)
	pending_release = []


func _first_departure(airport: Dictionary) -> Dictionary:
	var runways: Array = airport.get("runways", [])
	for rwy in runways:
		var desig: String = str(rwy.get("designator", ""))
		var parts := desig.split("/")
		if parts.size() > 0 and rwy.has("threshold_" + parts[0]):
			var thresh: Dictionary = rwy["threshold_" + parts[0]]
			return {
				"swiss_e": float(thresh.get("e", airport.get("center_e", 0.0))),
				"swiss_n": float(thresh.get("n", airport.get("center_n", 0.0))),
				"heading_deg": float(rwy.get("heading_deg", 0.0)),
			}
	return {}


func _env() -> Environment:
	var we := app_ref.get_node_or_null("Environment/WorldEnvironment") as WorldEnvironment
	if we == null:
		return null
	return we.environment


func _start_steps(p: Array, next_phase: String) -> void:
	steps = p
	step_idx = 0
	step_frame = frame
	steps_done_frame = 0
	steps_next = next_phase
	phase = "steps"


func _process_steps() -> void:
	if step_idx < steps.size():
		var st: Dictionary = steps[step_idx]
		if frame == step_frame:
			# Release the previous step's keys (one full frame without them so
			# the C++ debounce flags reset before the next press).
			for k in pending_release:
				release_key(k)
			pending_release = []
		elif frame == step_frame + 1:
			for k in st["press"]:
				press_key(k)
			pending_release = (st["press"] as Array).duplicate()
		if frame - step_frame >= int(st["frames"]) + 1:
			step_idx += 1
			# +1 so the next step's release branch runs on the following frame.
			step_frame = frame + 1
	else:
		if steps_done_frame == 0:
			steps_done_frame = frame
		if frame == steps_done_frame and not pending_release.is_empty():
			release_held_keys()
		if frame - steps_done_frame >= 4:
			_enter_phase(steps_next)


func _enter_phase(p: String) -> void:
	phase = p
	phase_frame = frame


func _process(_delta: float) -> void:
	frame += 1
	if app_ref == null or not is_instance_valid(app_ref):
		fail("main scene instance died in phase '%s'" % phase)
		return
	if frame > TIMEOUT_FRAMES:
		fail("timeout in phase '%s' (fly_loading=%s pending_init=%s intro_playing=%s)" % [
			phase,
			str(app_ref.get("_fly_loading")),
			str(app_ref.get("_pending_terrain_init")),
			str(app_ref.get("_intro_playing"))])
		return

	match phase:
		"boot":
			if app_ref._loading_done and app_ref._welcome_screen != null:
				var dep := {}
				var icao := "?"
				for a in AirportsLib.load_default():
					dep = _first_departure(a)
					if not dep.is_empty():
						icao = str(a.get("icao", "?"))
						break
				if dep.is_empty():
					fail("no airport with a runway threshold in the catalog")
					return
				print("SMOKE-GLOBE: first flight to %s (E=%.0f N=%.0f hdg=%.0f)" % [
					icao, dep["swiss_e"], dep["swiss_n"], dep["heading_deg"]])
				app_ref._on_welcome_flight_requested("c172p", dep["swiss_e"], dep["swiss_n"], dep["heading_deg"])
				app_ref._welcome_screen.hide_welcome()
				_enter_phase("wait_loading")

		"wait_loading":
			# The intro starts synchronously when fly-loading completes, so by
			# the time this clears the space view is already up.
			if not app_ref._fly_loading and not app_ref._pending_terrain_init:
				_enter_phase("check_space_view")

		"check_space_view":
			var env := _env()
			check(env != null, "WorldEnvironment/environment exists")
			check(app_ref._intro_playing, "_intro_playing is true after first-flight loading")
			check(app_ref._intro_zoom.is_playing(), "IntroZoom.is_playing() matches _intro_playing")
			check(
				app_ref._intro_camera != null and app_ref._intro_camera.current,
				"dedicated intro camera is the current camera in space view")
			if env != null:
				check(not env.fog_enabled, "fog disabled at space altitude (16000 km)")
				check(
					env.background_mode == Environment.BG_SKY,
					"background is the atmosphere sky shader (renders black space + limb) at space altitude")
			check(
				app_ref.globe_node != null and app_ref.globe_node.visible,
				"coarse globe visible from space")
			_start_steps([{"press": [KEY_SPACE], "frames": 4}], "check_handoff")

		"check_handoff":
			var env := _env()
			check(not app_ref._intro_playing, "_intro_playing cleared after SPACE skip")
			check(not app_ref._intro_zoom.is_playing(), "IntroZoom stopped after handoff")
			check(
				app_ref.camera_mode == app_ref.CameraMode.ORBIT,
				"camera_mode mirror is ORBIT after handoff (got %d)" % app_ref.camera_mode)
			check(app_ref.active_camera == app_ref.camera2, "active_camera mirror is camera2 after handoff")
			check(
				app_ref._camera_rig.get_mode() == 1,
				"CameraRig in MODE_ORBIT after handoff (got %d)" % app_ref._camera_rig.get_mode())
			if env != null:
				check(env.fog_enabled, "fog re-enabled at ground altitude after handoff")
			check(
				app_ref.globe_node != null and app_ref.globe_node.visible and app_ref.globe_node.get_coverage_mask_size() > 0,
				"coarse globe stays visible as the fallback and is masked per fragment under the detailed patch (mask uploaded)")
			_enter_phase("done")

		"steps":
			_process_steps()

		"done":
			if fails.is_empty():
				print("SMOKE-GLOBE RESULT: PASS (all globe cinematics checks ok)")
			else:
				printerr("SMOKE-GLOBE RESULT: FAIL (%d failure%s)" % [fails.size(), "s" if fails.size() != 1 else ""])
			get_tree().quit(0 if fails.is_empty() else 1)

