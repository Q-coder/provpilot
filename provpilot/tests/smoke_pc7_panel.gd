extends Node
## Pilatus PC-7: FlightGear generic instruments in the front panel and the
## radios on the right console (model/panel.xml, fg_panel_build.py --glb).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_pc7_panel.tscn
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
	var d: float = fposmod(a - expected + 180.0, 360.0) - 180.0
	return absf(d) <= tol

func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("pc7", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var fga: FGAnimator = root._fg_animator
	var jsb = root.jsb_node
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	_check(fga != null and fga.animation_count() >= 60, "panel animations loaded (%d)" % (fga.animation_count() if fga else 0))
	_check(root._fg_controls.count >= 20, "instrument and radio knobs clickable (%d parts)" % root._fg_controls.count)
	for nm in ["asi300-3d__ASIneedle", "ai__Face", "altimeter__needle", "tc__Airplane", "hi__Face", "vsi__Needle", "adf__Needle",
			"vor__LocalizerNeedle", "vor__GlideslopeNeedle", "kx165-1__NavUse1M", "kr87__freq_001"]:
		_check(mroot.find_child(nm, true, false) != null, "instrument part present: " + nm)
	# the painted front gauges the instruments replace are gone, the rear cockpit keeps its own
	_check(mroot.find_child("Object_18", true, false) == null and mroot.find_child("Object_19", true, false) != null, "painted front airspeed removed, rear one kept")
	# panel instruments sit on the front panel face (glb x ~ -1.68, scaled x5 in the AC frame)
	var asi: Node3D = mroot.find_child("asi300-3d__ASIface", true, false)
	if asi:
		var p: Vector3 = (mroot as Node3D).global_transform.affine_inverse() * asi.global_position
		print("  [diag] ASI face in model space: %s" % str(p))
		_check(absf(p.x + 1.68) < 0.03 and absf(p.y - 0.286) < 0.02 and absf(p.z - 0.137) < 0.02, "airspeed indicator at the painted gauge's place")
	# readings: altimeter, magnetic heading, airspeed at rest, NAV 1 on KLO
	var alt: float = float(fga.get_property("instrumentation/altimeter/indicated-altitude-ft"))
	var alt_true: float = jsb.get_property("position/h-sl-ft")
	_check(absf(alt - alt_true) < 60.0, "altimeter reads the altitude (%.0f vs %.0f ft)" % [alt, alt_true])
	var hdg: float = float(fga.get_property("instrumentation/heading-indicator/indicated-heading-deg"))
	var face: float = _deg(fga, "hi__Face")
	_check(_near_mod360(face, hdg, 3.0) or _near_mod360(face, -hdg, 3.0), "heading indicator card at the heading (%.0f deg for %.0f)" % [face, hdg])
	var magvar: float = float(fga.get_property("environment/magnetic-variation-deg"))
	_check(magvar > 2.5 and magvar < 4.5, "magnetic variation at Zurich %.2f E" % magvar)
	var ias: float = float(fga.get_property("velocities/airspeed-kt"))
	_check(ias < 30.0, "airspeed near zero on the ground (%.0f kt)" % ias)
	_check(fga.get_property("instrumentation/nav/in-range") == 1.0 and str(fga.get_property("instrumentation/nav/nav-id")) == "KLO", "NAV 1 receives KLO at LSZH")
	_check(float(fga.get_property("systems/electrical/outputs/nav")) > 24.0, "avionics bus powered (%s V)" % str(fga.get_property("systems/electrical/outputs/nav")))
	# the Sketchfab gear clip survived the panel build (it drives the retraction)
	var ap: AnimationPlayer = mroot.find_child("AnimationPlayer", true, false)
	_check(ap != null and ap.has_animation("gear_cycle") and absf(ap.get_animation("gear_cycle").length - 1.5) < 0.1, "gear_cycle clip present (%.2f s)" % (ap.get_animation("gear_cycle").length if ap and ap.has_animation("gear_cycle") else -1.0))
	if ap and ap.has_animation("gear_cycle"):
		_check(ap.get_animation("gear_cycle").get_track_count() >= 9, "gear clip keeps its %d tracks" % ap.get_animation("gear_cycle").get_track_count())
	# the radios tune: wheel over the KR 87 kHz knob from the cockpit camera
	var cam: Camera3D = root.camera1
	cam.current = true
	await get_tree().create_timer(0.5).timeout
	var ctl = root._fg_controls
	var kr_knob: Node3D = mroot.find_child("kr87__KR87_knob1", true, false)
	if kr_knob:
		var stby0: float = float(ctl.value_of("instrumentation/kr-87/outputs/standby-khz"))
		var kpos: Vector2 = cam.unproject_position(kr_knob.global_transform * (kr_knob as MeshInstance3D).get_aabb().get_center())
		print("  [diag] under the KR 87 knob: '%s' at %s (viewport %s)" % [ctl.hover_name(cam, kpos), str(kpos), str(get_viewport().get_visible_rect().size)])
		var took: bool = ctl.click(cam, kpos, MOUSE_BUTTON_WHEEL_UP, true, false)
		ctl.click(cam, kpos, MOUSE_BUTTON_WHEEL_UP, false, false)
		var stby1: float = float(ctl.value_of("instrumentation/kr-87/outputs/standby-khz"))
		_check(took and absf(stby1 - stby0 - 1.0) < 0.01, "KR 87 kHz knob on the console is clickable from the cockpit camera (%.0f -> %.0f)" % [stby0, stby1])
	if _fails.is_empty():
		print("smoke_pc7_panel: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_pc7_panel: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
