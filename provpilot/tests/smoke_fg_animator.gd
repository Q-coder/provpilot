extends Node
## Subtask 2 (docs/A10_FLIGHTGEAR_PORT.md): the generic FlightGear animator
## on the built A-10 glb + manifest, fed with synthetic property values.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_fg_animator.tscn
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _angle_deg(t: Transform3D) -> float:
	return rad_to_deg(t.basis.get_rotation_quaternion().get_angle())

func _ready() -> void:
	var dir: String = ProjectSettings.globalize_path("res://aircraft/a10/model")
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	_check(doc.append_from_file(dir.path_join("a10_fg.glb"), state) == OK, "glb loads")
	var model: Node = doc.generate_scene(state)
	add_child(model)
	var an := FGAnimator.new()
	add_child(an)
	_check(an.load(dir.path_join("animations.json"), model), "manifest loads")
	var st: Dictionary = an.stats()
	print("  stats: ", st)
	_check(int(st["animations"]) > 350 and int(st["nodes"]) > 200, "animations resolved onto nodes (%d on %d nodes)" % [int(st["animations"]), int(st["nodes"])])
	_check(an.property_names().size() > 80, "%d properties referenced" % an.property_names().size())
	# nothing set yet: everything at rest
	an.update(0.016)
	var rud: Node3D = model.find_child("LRudder-L", true, false)
	var t0: Transform3D = rud.transform
	_check(an.animated_transform("LRudder-L").is_equal_approx(Transform3D.IDENTITY), "rudder at rest with no properties")
	# rudder: factor -20 deg per unit
	an.set_property("surface-positions/rudder-pos-norm", 1.0)
	an.update(0.016)
	var a: float = _angle_deg(an.animated_transform("LRudder-L"))
	_check(absf(a - 20.0) < 0.01, "rudder full deflection rotates 20 deg (got %.3f)" % a)
	_check(not rud.transform.is_equal_approx(t0), "rudder node transform changed")
	# canopy: 35 deg about the FlightGear y axis (right) -> Godot -z
	an.set_property("/canopy/position-norm", 0.5)
	an.update(0.016)
	var ct: Transform3D = an.animated_transform("BCanopyFrame")
	var ax: Vector3 = ct.basis.get_rotation_quaternion().get_axis()
	_check(absf(_angle_deg(ct) - 17.5) < 0.01 and absf(absf(ax.z) - 1.0) < 1e-3, "canopy half open = 17.5 deg about the lateral axis (axis %s)" % str(ax))
	# flaps: translate then rotate on the same nodes (FlightGear order)
	an.set_property("surface-positions/left-flap-pos-norm", 1.0)
	an.update(0.016)
	var ft: Transform3D = an.animated_transform("LIFlap-F")
	_check(absf(_angle_deg(ft) - 30.0) < 0.01, "inner flap rotates %.1f deg (translate + rotate chain)" % _angle_deg(ft))
	# translate alone: nose oleo compression, factor 0.312 m per unit (its rotations stay at 0)
	an.set_property("surface-positions/rudder-pos-norm", 0.0)   # the nose gear steers with the rudder
	an.set_property("gear/gear[0]/compression-norm", 1.0)
	an.update(0.016)
	var ot: Transform3D = an.animated_transform("UC-NoseLow")
	_check(absf(ot.origin.length() - 0.312) < 1e-4 and _angle_deg(ot) < 1e-4, "nose oleo translates 0.312 m at full compression (got %.3f m)" % ot.origin.length())
	an.set_property("gear/gear[0]/compression-norm", 0.0)
	# gear chain: nose gear nodes rest at 0, move at 1
	_check(an.animated_transform("UC-NoseMain").is_equal_approx(Transform3D.IDENTITY), "nose gear at rest with position-norm 0")
	an.set_property("gear/gear[0]/position-norm", 1.0)
	an.update(0.016)
	_check(not an.animated_transform("UC-NoseMain").is_equal_approx(Transform3D.IDENTITY), "nose gear moves with position-norm 1")
	# select with a string property: canopy glass hidden in the cockpit view
	var glass: Node3D = model.find_child("FCanopyGlass", true, false)
	an.set_property("/sim/current-view/name", "Cockpit View")
	an.set_property("/sim/current-view/view-number", 0)
	an.update(0.016)
	_check(not glass.visible, "canopy glass hidden in the cockpit view")
	an.set_property("/sim/current-view/name", "Chase View")
	an.set_property("/sim/current-view/view-number", 1)
	an.update(0.016)
	_check(glass.visible, "canopy glass shown in an external view")
	# spin: fan turns with n1
	an.set_property("sim/model/A-10/engines/engine[0]/n1", 50.0)
	var f0: Transform3D = an.animated_transform("LEFan")
	an.update(0.1)
	var f1: Transform3D = an.animated_transform("LEFan")
	_check(not f0.is_equal_approx(f1), "engine fan spins with N1")
	# instrument needle inside a sub-model
	an.set_property("instrumentation/airspeed-indicator/indicated-speed-kt", 250.0)
	an.update(0.016)
	var nt: Transform3D = an.animated_transform("A-10-asi__needle")
	_check(_angle_deg(nt) > 5.0, "airspeed needle turns at 250 kt (%.1f deg)" % _angle_deg(nt))
	# unknown properties are reported
	var unknown: PackedStringArray = an.unknown_properties()
	_check(unknown.size() > 50 and not ("surface-positions/rudder-pos-norm" in unknown), "%d properties still unbridged, set ones not among them" % unknown.size())
	# cost
	var t_start := Time.get_ticks_usec()
	for i in range(100):
		an.update(0.016)
	var per_ms: float = (Time.get_ticks_usec() - t_start) / 100.0 / 1000.0
	print("  update cost: %.3f ms per frame" % per_ms)
	_check(per_ms < 4.0, "update under 4 ms per frame")
	if _fails.is_empty():
		print("smoke_fg_animator: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_fg_animator: %d failure(s)" % _fails.size())
		get_tree().quit(1)
