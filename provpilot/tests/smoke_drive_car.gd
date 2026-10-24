extends Node
## Headless smoke test for driving the car in the real main scene
## (src/drive_mode.cpp through node_3d.gd): Fly! with the "car" vehicle at
## the first catalog runway, then the drive keys through the Input singleton.
## Checks: the drive engages after the spawn, JSBSim stays paused, W moves
## the car forward on the ground, A turns it, B stops it, the HUD shows the
## driving readouts. Exits 0 on success, 1 on any failure.

const MAIN_SCENE := "res://node_3d.tscn"
const TIMEOUT_FRAMES := 40000  # headless frames run far faster than 60 fps; the drive phases are timed in simulated seconds

var app: Node = null
var frame := 0
var phase := "boot"
var phase_frame := 0
var phase_time := 0.0   # simulated seconds in the phase (headless frames are not 1/60 s)
var fails: Array[String] = []
var pos0: Vector3 = Vector3.ZERO
var hdg0 := 0.0
var held: Array = []


func _ready() -> void:
	Input.set_use_accumulated_input(false)
	app = load(MAIN_SCENE).instantiate()
	add_child(app)


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


func press(keys: Array) -> void:
	release()
	for k in keys:
		var ev := InputEventKey.new()
		ev.keycode = k
		ev.physical_keycode = k
		ev.pressed = true
		Input.parse_input_event(ev)
	held = keys


func release() -> void:
	for k in held:
		var ev := InputEventKey.new()
		ev.keycode = k
		ev.physical_keycode = k
		ev.pressed = false
		Input.parse_input_event(ev)
	held = []


func _first_departure(airport: Dictionary) -> Dictionary:
	for rwy in airport.get("runways", []):
		var parts: PackedStringArray = String(rwy.get("designator", "")).split("/")
		if parts.size() > 0 and rwy.has("threshold_" + parts[0]):
			var thresh: Dictionary = rwy["threshold_" + parts[0]]
			return {"swiss_e": thresh["e"], "swiss_n": thresh["n"], "heading_deg": rwy.get("heading_deg", 0.0)}
	return {}


func _enter(p: String) -> void:
	phase = p
	phase_frame = frame
	phase_time = 0.0


func _ground_gap() -> float:
	var ac: Node3D = app.get_node("AC")
	var g: float = app._world.ground_height_below(ac.global_position)
	var h: float = W3DCoords.ecef_to_geodetic(ac.global_position).z
	return h - g


func _process(_delta: float) -> void:
	frame += 1
	phase_time += _delta
	if app == null or not is_instance_valid(app):
		fail("main scene died in phase '%s'" % phase)
		return
	if frame > TIMEOUT_FRAMES:
		fail("timeout in phase '%s' (fly_loading=%s pending_init=%s active=%s armed=%s cooldown=%s)" % [
			phase, str(app.get("_fly_loading")), str(app.get("_pending_terrain_init")), str(app._drive_mode.is_active()), str(app._drive_mode.is_armed()), str(app._terrain_elevation.get_rebase_cooldown())])
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
				print("SMOKE: car to %s (E=%.0f N=%.0f hdg=%.0f)" % [icao, dep["swiss_e"], dep["swiss_n"], dep["heading_deg"]])
				app._on_welcome_flight_requested("car", dep["swiss_e"], dep["swiss_n"], dep["heading_deg"])
				app._welcome_screen.hide_welcome()
				check(app._aircraft.is_vehicle() and app._aircraft.get_vehicle_kind() == "car", "the rig loaded the car as a vehicle")
				check(app._drive_mode.is_armed(), "Fly! with the car arms the drive")
				_enter("wait")
		"wait":
			if not app._fly_loading and not app._pending_terrain_init and not app._intro_playing and app._drive_mode.is_active():
				_enter("settle")
		"settle":
			if frame - phase_frame >= 30:
				check(app.jsb_node.get_slew_mode(), "JSBSim is paused while driving")
				check(app.get_node("Control/BottomRow/Label").text.begins_with("Speed:"), "HUD shows the driving readout (got '%s')" % app.get_node("Control/BottomRow/Label").text)
				var gap := _ground_gap()
				check(absf(gap) < 1.5, "the car stands on the ground (origin %.2f m above the drawn ground)" % gap)
				pos0 = app.get_node("AC").global_position
				hdg0 = app._drive_mode.get_heading_deg()
				press([KEY_W])
				_enter("accel")
		"accel":
			if phase_time >= 2.0:
				var moved: float = (app.get_node("AC").global_position - pos0).length()
				var v: float = app._drive_mode.get_speed_kmh()
				check(moved > 5.0 and v > 10.0, "2 s of W: %.1f m forward at %.0f km/h" % [moved, v])
				check(absf(app._drive_mode.get_heading_deg() - hdg0) < 1.0, "... straight (heading %.1f vs %.1f)" % [app._drive_mode.get_heading_deg(), hdg0])
				var gap := _ground_gap()
				check(absf(gap) < 1.5, "... still on the ground (%.2f m)" % gap)
				press([KEY_W, KEY_A])
				_enter("turn")
		"turn":
			if phase_time >= 1.5:
				var d: float = fposmod(app._drive_mode.get_heading_deg() - hdg0 + 180.0, 360.0) - 180.0
				check(d < -3.0, "1.5 s of A turns left (heading change %.1f deg)" % d)
				press([KEY_B])
				_enter("stop")
		"stop":
			if phase_time >= 3.0:
				check(app._drive_mode.get_speed_kmh() == 0.0, "B (handbrake) stops the car (%.1f km/h)" % app._drive_mode.get_speed_kmh())
				release()
				# back to an aircraft: the drive disengages, JSBSim gets its model again
				var dep := {}
				for a in AirportsLib.load_default():
					dep = _first_departure(a)
					if not dep.is_empty():
						break
				app._on_welcome_flight_requested("c172p", dep["swiss_e"], dep["swiss_n"], dep["heading_deg"])
				check(not app._drive_mode.is_armed() and not app._drive_mode.is_active(), "Fly! with an aircraft disengages the drive")
				_enter("done")
		"done":
			if fails.is_empty():
				print("SMOKE RESULT: PASS (car drive checks ok)")
			else:
				printerr("SMOKE RESULT: FAIL (%d failure%s)" % [fails.size(), "s" if fails.size() != 1 else ""])
			get_tree().quit(0 if fails.is_empty() else 1)


func fail(msg: String) -> void:
	fails.append(msg)
	printerr("SMOKE FAIL: ", msg)
	get_tree().quit(1)
