extends Node
## Headless smoke test for the C++ SlewMode extraction (src/slew_mode.cpp).
## Boots the real main scene, starts a first flight in the c172p, then drives
## slew mode through Godot's Input singleton (Shift+Y toggle, WASD/QE
## movement) and asserts on the module state + JSBSim side effects. Exits 0
## on success, 1 on any failure.

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

var pos_before_move: Vector3 = Vector3.ZERO
var y_before_climb: float = 0.0
var pos_at_exit: Vector3 = Vector3.ZERO


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
			if app._loading_done and app._welcome_screen != null:
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
			# flight/slew logic.
			if not app._fly_loading and not app._pending_terrain_init and not app._intro_playing:
				_enter_phase("settle")

		"settle":
			if frame - phase_frame >= 10:
				var sm = app._slew_mode
				check(sm.is_enabled() == false, "slew mode starts disabled")
				check(app.jsb_node.get_slew_mode() == false, "JSBSim starts outside slew mode")
				pos_before_move = app.get_node("AC").global_position
				print("SMOKE: ready. AC at ", pos_before_move)
				run_steps([{"press": [KEY_SHIFT, KEY_Y], "frames": 4}], "check_enter")

		"check_enter":
			var sm = app._slew_mode
			check(sm.is_enabled(), "Shift+Y enables slew mode")
			check(app.jsb_node.get_slew_mode() == true, "JSBSim physics paused in slew mode")
			check("SLEW MODE" in app.airspeed_label.text, "HUD shows the slew banner (got '%s')" % app.airspeed_label.text)
			pos_before_move = app.get_node("AC").global_position
			run_steps([{"press": [KEY_W], "frames": 12}], "check_move_forward")

		"check_move_forward":
			var pos: Vector3 = app.get_node("AC").global_position
			var moved: float = (pos - pos_before_move).length()
			check(moved > 1.0, "W moves the aircraft forward while slewing (got %.1f m)" % moved)
			y_before_climb = pos.y
			run_steps([{"press": [KEY_Q], "frames": 8}], "check_move_up")

		"check_move_up":
			var pos: Vector3 = app.get_node("AC").global_position
			check(pos.y - y_before_climb > 1.0, "Q raises the aircraft while slewing (got %.1f m)" % (pos.y - y_before_climb))
			run_steps([{"press": [KEY_SHIFT, KEY_Y], "frames": 4}], "check_exit")

		"check_exit":
			var sm = app._slew_mode
			check(sm.is_enabled() == false, "Shift+Y again disables slew mode")
			check(app.jsb_node.get_slew_mode() == false, "JSBSim physics resumed after slew")
			check(sm.get_preserved_airspeed_kts() >= 0.0, "preserved airspeed was captured (%.1f kts)" % sm.get_preserved_airspeed_kts())
			pos_at_exit = app.get_node("AC").global_position
			_enter_phase("wait_settle")

		"wait_settle":
			if frame - phase_frame >= 6:
				var pos: Vector3 = app.get_node("AC").global_position
				var drift: float = (pos - pos_at_exit).length()
				check(drift < 25.0, "JSBSim reinit kept the aircraft at the slewed position (drift %.1f m)" % drift)
				check(app.airspeed_label.text.begins_with("Airspeed"), "normal HUD restored after slew (got '%s')" % app.airspeed_label.text)
				_enter_phase("done")

		"steps":
			_process_steps()

		"done":
			if fails.is_empty():
				print("SMOKE RESULT: PASS (all slew mode checks ok)")
			else:
				printerr("SMOKE RESULT: FAIL (%d failure%s)" % [fails.size(), "s" if fails.size() != 1 else ""])
			get_tree().quit(0 if fails.is_empty() else 1)


func fail(msg: String) -> void:
	fails.append(msg)
	printerr("SMOKE FAIL: ", msg)
	get_tree().quit(1)

