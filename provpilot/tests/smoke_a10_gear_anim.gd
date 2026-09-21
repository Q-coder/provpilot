extends Node
## A-10 gear animation through the property bridge: retract with the FDM
## command and the FlightGear gear chain must fold (docs/A10_FLIGHTGEAR_PORT.md).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_a10_gear_anim.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _sample(root: Node, tag: String) -> Dictionary:
	var jsb = root.jsb_node
	var fga: FGAnimator = root._fg_animator
	var d := {
		"gear-pos-norm": jsb.get_property("gear/gear-pos-norm"),
		"gear-cmd-norm": jsb.get_property("gear/gear-cmd-norm"),
		"unit0": jsb.get_property("gear/unit[0]/pos-norm"),
		"unit1": jsb.get_property("gear/unit[1]/pos-norm"),
		"bridge0": fga.get_property("gear/gear[0]/position-norm") if fga else null,
		"nose_angle": rad_to_deg(fga.animated_transform("UC-NoseMain").basis.get_rotation_quaternion().get_angle()) if fga else -1.0,
		"left_angle": rad_to_deg(fga.animated_transform("UC-LeftTop").basis.get_rotation_quaternion().get_angle()) if fga else -1.0,
		"speedbrake": jsb.get_property("fcs/speedbrake-pos-norm"),
	}
	print("  [%s] %s" % [tag, str(d)])
	return d

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
	var down := _sample(root, "parked")
	_check(float(down["nose_angle"]) > 80.0 and float(down["left_angle"]) > 80.0, "gear chain extended while parked (nose %.0f deg, left %.0f deg)" % [down["nose_angle"], down["left_angle"]])
	root.jsb_node.set_gear_down(false)
	await get_tree().create_timer(6.0).timeout
	var up := _sample(root, "retracted")
	_check(float(up["gear-pos-norm"]) < 0.05, "FDM gear position reaches 0 after the up command (%.2f)" % up["gear-pos-norm"])
	_check(float(up["nose_angle"]) < 5.0 and float(up["left_angle"]) < 5.0, "gear chain folded (nose %.0f deg, left %.0f deg)" % [up["nose_angle"], up["left_angle"]])
	root.jsb_node.set_gear_down(true)
	await get_tree().create_timer(6.0).timeout
	var again := _sample(root, "extended again")
	_check(float(again["nose_angle"]) > 80.0, "gear chain extends again (nose %.0f deg)" % again["nose_angle"])
	if root.jsb_node.has_method("set_property"):
		root.jsb_node.set_property("fcs/speedbrake-cmd-norm", 1.0)
		await get_tree().create_timer(2.0).timeout
		var sb := _sample(root, "speedbrake")
		_check(float(sb["speedbrake"]) > 0.9, "speed brakes deploy from fcs/speedbrake-cmd-norm (%.2f)" % sb["speedbrake"])
		var fga: FGAnimator = root._fg_animator
		var a: float = rad_to_deg(fga.animated_transform("LUAileron").basis.get_rotation_quaternion().get_angle())
		_check(a > 50.0, "upper aileron splits open as a speed brake (%.0f deg)" % a)
	# nose-wheel steering visual follows the FDM steering angle (limited 5 deg, full 60 deg)
	var fga2: FGAnimator = root._fg_animator
	var axis_g := Vector3(2.0882, -0.2016, 0.0).normalized()   # FlightGear (2.0882, 0, -0.2016) -> Godot (x, z, -y)
	root.jsb_node.set_input_rudder(0.0)
	await get_tree().create_timer(1.0).timeout
	var t0: Transform3D = fga2.animated_transform("UC-NoseLow")
	root.jsb_node.set_input_rudder(1.0)
	await get_tree().create_timer(1.5).timeout
	var t1: Transform3D = fga2.animated_transform("UC-NoseLow")
	var rel: Quaternion = (t1.basis * t0.basis.inverse()).get_rotation_quaternion()
	var lim_deg: float = rad_to_deg(rel.get_angle()) * signf(rel.get_axis().dot(axis_g))
	var rud: float = root.jsb_node.get_property("fcs/rudder-pos-norm")
	print("  [steer limited] visual %.1f deg, rudder-pos %.2f, steer-pos %.1f" % [lim_deg, rud, root.jsb_node.get_property("fcs/steer-pos-deg[0]")])
	_check(absf(absf(lim_deg) - 5.0) < 1.0, "limited range: nose wheel visual 5 deg (%.1f)" % lim_deg)
	_check(signf(lim_deg) == signf(20.0 * rud), "visual steering sign matches the rudder-driven original")
	root.jsb_node.set_nose_steer_full(true)
	await get_tree().create_timer(1.5).timeout
	var t2: Transform3D = fga2.animated_transform("UC-NoseLow")
	var rel2: Quaternion = (t2.basis * t0.basis.inverse()).get_rotation_quaternion()
	var full_deg: float = rad_to_deg(rel2.get_angle()) * signf(rel2.get_axis().dot(axis_g))
	print("  [steer full] visual %.1f deg, steer-pos %.1f" % [full_deg, root.jsb_node.get_property("fcs/steer-pos-deg[0]")])
	_check(absf(absf(full_deg) - 60.0) < 3.0, "full range: nose wheel visual 60 deg (%.1f)" % full_deg)
	root.jsb_node.set_input_rudder(0.0)
	root.jsb_node.set_nose_steer_full(false)
	# ailerons move opposite to each other (signed about the common lateral axis, Godot +z = left);
	# the speed brakes act on the same halves, so stow them first
	root.jsb_node.set_property("fcs/speedbrake-cmd-norm", 0.0)
	await get_tree().create_timer(2.0).timeout
	root.jsb_node.set_input_aileron(1.0)
	await get_tree().create_timer(1.5).timeout
	var la: Quaternion = fga2.animated_transform("LUAileron").basis.get_rotation_quaternion()
	var ra: Quaternion = fga2.animated_transform("RUAileron").basis.get_rotation_quaternion()
	var l_deg: float = rad_to_deg(la.get_angle()) * signf(la.get_axis().z)
	var r_deg: float = rad_to_deg(ra.get_angle()) * signf(ra.get_axis().z)
	print("  [ailerons] left %.1f deg, right %.1f deg about the lateral axis (aileron-pos %.2f)" % [l_deg, r_deg, root.jsb_node.get_property("fcs/left-aileron-pos-norm")])
	_check(absf(l_deg) > 10.0 and signf(l_deg) == -signf(r_deg) and absf(absf(l_deg) - absf(r_deg)) < 1.0, "ailerons deflect opposite to each other")
	root.jsb_node.set_input_aileron(0.0)
	if _fails.is_empty():
		print("smoke_a10_gear_anim: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_a10_gear_anim: %d failure(s)" % _fails.size())
		get_tree().quit(1)
