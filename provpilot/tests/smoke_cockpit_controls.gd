extends Node
## Headless smoke test for the C++ CockpitControls extraction (src/cockpit_controls.cpp).
## Boots the real main scene, starts a first flight in the c172p, then drives the
## lever/switch keyboard controls through Godot's Input singleton and asserts on
## the module state + JSBSim writes. Exits 0 on success, 1 on any failure.

const MAIN_SCENE := "res://node_3d.tscn"
const TIMEOUT_FRAMES := 9000  # ~150 s at 60 fps (headless loading can be slow)

var app: Node = null
var frame := 0
var phase := "boot"
var phase_next := ""
var phase_frame := 0
var fails: Array[String] = []

# Key-press step machine. Each step: {"press": [Key, ...], "frames": int} —
# the keys are pressed for `frames` frames (the C++ debounce fires once per
# press) and released before the next step starts.
var steps: Array = []
var steps_next := ""
var steps_done_frame := 0
var step_idx := 0
var step_frame := 0
var hold_last_key := false
var pending_release: Array = []

var throttle_before := 0.0
# Local-space origin of the mixture lever (stable while the aircraft moves).
var mixture_origin_before: Vector3 = Vector3.ZERO


func _ready() -> void:
	# Parsed key events must update the raw key state immediately (the C++
	# module reads Input.is_key_pressed), so disable input accumulation.
	Input.set_use_accumulated_input(false)
	app = load(MAIN_SCENE).instantiate()
	add_child(app)


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


# This custom engine build does not expose the static Input.key_press/release
# helpers, so drive the Input singleton through parsed key events instead.
func press_key(k: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.pressed = true
	# a real keyboard flags Shift on the keys typed while it is held; an event
	# without the flag reads as "Shift is up" (KeyBindings releases a stale one)
	ev.shift_pressed = k == KEY_SHIFT or Input.is_key_pressed(KEY_SHIFT)
	Input.parse_input_event(ev)


func release_key(k: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = k
	ev.physical_keycode = k
	ev.pressed = false
	Input.parse_input_event(ev)


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


func run_steps(step_list: Array, next_phase: String, hold_last := false) -> void:
	steps = step_list
	steps_next = next_phase
	step_idx = 0
	step_frame = frame
	steps_done_frame = 0
	hold_last_key = hold_last
	pending_release = []
	phase = "steps"


func release_held_keys() -> void:
	for k in pending_release:
		release_key(k)
	pending_release = []


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
			# +1 so the next step's release branch runs on the following frame
			# (this frame already passed it when the completion check fired).
			step_frame = frame + 1
	else:
		if steps_done_frame == 0:
			steps_done_frame = frame
		if frame == steps_done_frame and not pending_release.is_empty() and not hold_last_key:
			release_held_keys()
		if frame - steps_done_frame >= 4:
			_enter_phase(steps_next)


func _enter_phase(p: String) -> void:
	phase = p
	phase_frame = frame


func fail(msg: String) -> void:
	fails.append(msg)
	printerr("SMOKE FAIL: ", msg)
	get_tree().quit(1)


func _process(_delta: float) -> void:
	frame += 1
	if app == null or not is_instance_valid(app):
		fail("main scene instance died in phase '%s'" % phase)
		return
	if frame > TIMEOUT_FRAMES:
		fail("timeout in phase '%s' (fly_loading=%s pending_init=%s)" % [
			phase, str(app.get("_fly_loading")), str(app.get("_pending_terrain_init"))])
		return

	match phase:
		"boot":
			if app._welcome_screen != null:
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
				print("SMOKE: first flight to %s (E=%.0f N=%.0f hdg=%.0f)" % [
					icao, dep["swiss_e"], dep["swiss_n"], dep["heading_deg"]])
				app._on_welcome_flight_requested("c172p", dep["swiss_e"], dep["swiss_n"], dep["heading_deg"])
				app._welcome_screen.hide_welcome()
				_enter_phase("wait_jsb")

		"wait_jsb":
			# Wait for JSBSim init AND the post-landing cinematic intro zoom to
			# finish — while it plays, _process returns early and skips all
			# flight/lever logic.
			if not app._fly_loading and not app._pending_terrain_init and not app._intro_playing:
				_enter_phase("settle")

		"settle":
			if frame - phase_frame >= 10:
				var cc = app._cockpit_controls
				print("SMOKE: ready. mixture=%.3f magneto=%d flaps=%d gear_down=%s throttle=%.3f" % [
					cc.get_mixture_value(), cc.get_magneto_position(), cc.get_flap_position(),
					str(cc.is_gear_down()), app.jsb_node.get_input_throttle()])
				check(app._cockpit_controls.get_lever_node(CockpitControls.LEVER_MIXTURE) != null, "mixture lever node resolved")
				check(app._cockpit_controls.get_lever_node(CockpitControls.LEVER_THROTTLE) != null, "throttle lever node resolved")
				check(app._cockpit_controls.get_lever_node(CockpitControls.LEVER_MAGNETO) != null, "magneto switch node resolved")
				check(app._cockpit_controls.get_lever_node(CockpitControls.LEVER_FLAP) != null, "flap lever node resolved")
				check(absf(cc.get_mixture_value() - 1.0) < 1e-9, "initial mixture is full rich (1.0)")
				check(cc.get_magneto_position() == 3, "initial magneto is BOTH (3)")
				check(cc.get_flap_position() == 0, "initial flaps are UP (0)")
				check(cc.is_gear_down(), "initial gear command is down")
				throttle_before = app.jsb_node.get_input_throttle()
				mixture_origin_before = app._cockpit_controls.get_lever_node(CockpitControls.LEVER_MIXTURE).transform.origin if app._cockpit_controls.get_lever_node(CockpitControls.LEVER_MIXTURE) else Vector3.ZERO
				run_steps([{"press": [KEY_SHIFT, KEY_M], "frames": 4}], "check_mixture")

		"check_mixture":
			var cc = app._cockpit_controls
			check(absf(cc.get_mixture_value() - 0.95) < 1e-9, "Shift+M leans mixture to 0.95 (got %.4f)" % cc.get_mixture_value())
			check(absf(app.jsb_node.get_input_mixture() - 0.95) < 1e-6, "JSBSim mixture input follows lever (got %.4f)" % app.jsb_node.get_input_mixture())
			if app._cockpit_controls.get_lever_node(CockpitControls.LEVER_MIXTURE):
				var moved: float = (app._cockpit_controls.get_lever_node(CockpitControls.LEVER_MIXTURE).transform.origin - mixture_origin_before).length()
				check(absf(moved - 0.0025) < 0.001, "mixture lever visual moved ~0.0025 units (got %.5f)" % moved)
			run_steps([{"press": [KEY_T], "frames": 4}], "check_throttle")

		"check_throttle":
			var after: float = app.jsb_node.get_input_throttle()
			check(absf(after - (throttle_before + 0.05)) < 0.01, "T raises JSBSim throttle by 5%% (%.3f -> %.3f)" % [throttle_before, after])
			# Hold I while asserting START — releasing it springs the switch
			# back to BOTH on the very next frame.
			run_steps([{"press": [KEY_I], "frames": 6}], "check_magneto_start", true)

		"check_magneto_start":
			var cc = app._cockpit_controls
			check(cc.get_magneto_position() == 4, "I steps magneto to START (4) (got %d)" % cc.get_magneto_position())
			release_held_keys()
			_enter_phase("wait_springback")

		"wait_springback":
			if frame - phase_frame >= 3:
				_enter_phase("check_magneto_springback")

		"check_magneto_springback":
			var cc = app._cockpit_controls
			check(cc.get_magneto_position() == 3, "magneto springs back to BOTH (3) on I release (got %d)" % cc.get_magneto_position())
			run_steps([{"press": [KEY_F], "frames": 4}, {"press": [KEY_F], "frames": 4}], "check_flaps_down")

		"check_flaps_down":
			var cc = app._cockpit_controls
			check(cc.get_flap_position() == 2, "F F extends flaps to notch 2 (got %d)" % cc.get_flap_position())
			var cmd: float = app.jsb_node.get_flap_cmd()
			check(absf(cmd - 0.667) < 0.05, "JSBSim flap-cmd follows lever (~0.667, got %.3f)" % cmd)
			run_steps([{"press": [KEY_SHIFT, KEY_F], "frames": 4}], "check_flaps_up")

		"check_flaps_up":
			var cc = app._cockpit_controls
			check(cc.get_flap_position() == 1, "Shift+F retracts flaps to notch 1 (got %d)" % cc.get_flap_position())
			run_steps([{"press": [KEY_G], "frames": 4}], "check_gear")

		"check_gear":
			var cc = app._cockpit_controls
			check(cc.is_gear_down(), "G is a no-op on fixed-gear C172P (gear still down)")
			_enter_phase("done")

		"steps":
			_process_steps()

		"done":
			if fails.is_empty():
				print("SMOKE RESULT: PASS (all cockpit controls checks ok)")
			else:
				printerr("SMOKE RESULT: FAIL (%d failure%s)" % [fails.size(), "s" if fails.size() != 1 else ""])
			get_tree().quit(0 if fails.is_empty() else 1)

