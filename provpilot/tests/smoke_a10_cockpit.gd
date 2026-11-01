extends Node
## Subtask 4 (docs/A10_FLIGHTGEAR_PORT.md): the cockpit instruments of the
## FlightGear A-10 follow the FDM through the property bridge.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_a10_cockpit.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _deg(fga: FGAnimator, node: String) -> float:
	return rad_to_deg(fga.animated_transform(node).basis.get_rotation_quaternion().get_angle())

func _near_mod360(a: float, expected: float, tol: float) -> bool:
	# quaternion angles are 0..180: compare against expected and 360-expected
	var e := fposmod(expected, 360.0)
	return absf(a - e) < tol or absf(a - (360.0 - e)) < tol

## First target object of the manifest's rotate record on [param prop] (sub-model [param model]).
func _rotate_target(model: String, prop: String) -> String:
	var f := FileAccess.open(ProjectSettings.globalize_path("res://aircraft/a10/model/animations.json"), FileAccess.READ)
	var m: Dictionary = JSON.parse_string(f.get_as_text())
	for r in m["animations"]:
		if r["model"] == model and r["type"] == "rotate" and str(r["raw"].get("property", "")).trim_prefix("/") == prop and (r["objects"] as Array).size() > 0:
			return r["objects"][0]
	return ""

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	var fga: FGAnimator = root._fg_animator
	var jsb = root.jsb_node
	_check(fga != null and fga.animation_count() > 300, "animator running")
	var unknown: PackedStringArray = fga.unknown_properties()
	print("  [diag] unbridged (%d): %s" % [unknown.size(), unknown])
	_check(unknown.size() <= 6, "at most 6 manifest properties left unbridged (%d)" % unknown.size())
	# HSI compass card follows the magnetic heading (155 true on runway 16
	# minus the WMM declination, docs/RADIO_NAVIGATION.md)
	var hdg: float = fposmod(jsb.get_property("attitude/psi-deg") - float(fga.get_property("environment/magnetic-variation-deg")), 360.0)
	var card: String = _rotate_target("A-10-HSI", "instrumentation/heading-indicator-fg/indicated-heading-deg")
	print("  [diag] heading %.1f, HSI card node '%s' angle %.1f" % [hdg, card, _deg(fga, card) if card != "" else -1.0])
	_check(card != "" and _near_mod360(_deg(fga, card), hdg, 2.0), "HSI compass card rotated by the heading")
	# VSI at rest: table 0 fpm -> -90 deg
	_check(absf(_deg(fga, "A-10-vsi-6kfpm__needle") - 90.0) < 3.0, "VSI needle at the 0 fpm mark (%.1f deg)" % _deg(fga, "A-10-vsi-6kfpm__needle"))
	# altimeter (generic fgdata instrument vendored in third_party/fgdata): the
	# needle turns 0.36 deg/ft, the reading follows the Kollsman setting
	var alt_ind: float = float(fga.get_property("instrumentation/altimeter/indicated-altitude-ft"))
	var alt_true: float = jsb.get_property("position/h-sl-ft")
	var qnh_inhg: float = jsb.get_property("atmosphere/P-sl-psf") / 70.7262
	var setting_now: float = float(fga.get_property("instrumentation/altimeter/setting-inhg"))
	print("  [diag] altimeter %.0f ft (true %.0f ft), setting %.2f inHg, QNH %.2f inHg" % [alt_ind, alt_true, setting_now, qnh_inhg])
	_check(absf(setting_now - qnh_inhg) < 0.011, "Kollsman window set to the weather's QNH at flight start (%.2f vs %.2f inHg)" % [setting_now, qnh_inhg])
	_check(fga.animated_transform("altimeter__needle") != Transform3D() and absf(alt_ind - alt_true) < 60.0, "altimeter present and reading the altitude with QNH set (%.0f vs %.0f ft)" % [alt_ind, alt_true])
	_check(_near_mod360(_deg(fga, "altimeter__needle"), fposmod(alt_ind * 0.36, 360.0), 2.0) or _near_mod360(_deg(fga, "altimeter__needle"), fposmod(-alt_ind * 0.36, 360.0), 2.0), "altimeter needle at 0.36 deg per ft (%.1f deg)" % _deg(fga, "altimeter__needle"))
	root._fg_controls.state["instrumentation/altimeter/setting-inhg"] = setting_now + 1.0
	await get_tree().create_timer(0.5).timeout
	var alt_set: float = float(fga.get_property("instrumentation/altimeter/indicated-altitude-ft"))
	_check(alt_set - alt_ind > 800.0 and alt_set - alt_ind < 1100.0, "one inch more on the Kollsman window reads about 900 ft higher (%+.0f ft)" % (alt_set - alt_ind))
	root._fg_controls.state["instrumentation/altimeter/setting-inhg"] = setting_now
	# fuel: left display needle from its table (0 -> 189, 3000 -> 270, 6000 -> 351)
	var left_lbs: float = float(fga.get_property("sim/model/A-10/consumables/fuel/fuel-dsp-left"))
	var exp_left: float = 189.0 + 81.0 * (left_lbs / 3000.0) if left_lbs <= 3000.0 else 270.0 + 81.0 * ((left_lbs - 3000.0) / 3000.0)
	var fuel_needle: String = _rotate_target("A-10-fuel-gauges", "sim/model/A-10/consumables/fuel/fuel-dsp-left")
	var fuel_names: Array = [fuel_needle]
	print("  [diag] fuel left %.0f lbs -> expected %.1f deg; needle node '%s' angle %.1f; fuel nodes %s" % [left_lbs, exp_left, fuel_needle, _deg(fga, fuel_needle) if fuel_needle != "" else -1.0, fuel_names.slice(0, 12)])
	_check(left_lbs > 1000.0, "fuel display fed from the FDM tanks (%.0f lbs left)" % left_lbs)
	if fuel_needle != "":
		_check(_near_mod360(_deg(fga, fuel_needle), exp_left, 2.0), "left fuel needle on its table value")
	# engines: N1 gauge property equals the FDM, ITT reads the thrust fraction
	_check(absf(float(fga.get_property("engines/engine/n1")) - jsb.get_property("propulsion/engine[0]/n1")) < 0.01, "engine N1 bridged (%.1f %%)" % jsb.get_property("propulsion/engine[0]/n1"))
	# clock digits fed
	_check(int(fga.get_property("sim/time/utc/hour")) == int(root._sky.time_seconds / 3600.0) % 24, "clock shows the simulated time (%02d:%02d)" % [int(fga.get_property("sim/time/utc/hour")), int(fga.get_property("sim/time/utc/minute"))])
	_check(float(fga.get_property("sim/model/A-10/controls/gear/ld-gear-handle-anim")) < -0.5, "gear handle down (-1) with the gear down")
	# stick follows the aileron command
	root.jsb_node.set_input_aileron(1.0)
	await get_tree().create_timer(1.0).timeout
	var stick_nodes: Array = root.get_node(NodePath(root._model_root_path)).find_children("stick__*", "", true, false)
	var stick_moved := false
	for n in stick_nodes:
		if not fga.animated_transform(n.name).is_equal_approx(Transform3D.IDENTITY):
			stick_moved = true
	_check(stick_moved, "stick moves with the aileron command (%d stick nodes)" % stick_nodes.size())
	root.jsb_node.set_input_aileron(0.0)
	if _fails.is_empty():
		print("smoke_a10_cockpit: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_a10_cockpit: %d failure(s)" % _fails.size())
		get_tree().quit(1)
