extends Node
## Probe (not a smoke test): MD-82 on the ground at LSZH; click the TOGA
## button on the thrust lever and turn the FGCP ALT knob with the mouse.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_fgcp.tscn
const E := 2682767.0
const N := 1258797.0

func _part_for(ctl, needle: String) -> Node3D:
	for it in ctl.get_items():
		if JSON.stringify(it["recs"]).find(needle) >= 0:
			return it["node"]
	return null

func _click(ctl, cam: Camera3D, n: Node3D, button: int, shift: bool = false) -> bool:
	var px: Vector2 = cam.unproject_position(n.global_transform * (n as MeshInstance3D).get_aabb().get_center())
	var took: bool = ctl.click(cam, px, button, true, shift)
	ctl.click(cam, px, button, false, shift)
	return took

func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(8.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var rig = root._aircraft
	var ctl = root._fg_controls
	var af = rig.get_autoflight()
	var cam: Camera3D = root.camera1
	cam.current = true
	af.set_option("debug", 1.0)
	# TOGA
	var toga: Node3D = _part_for(ctl, "ApPanel.toga")
	print("TOGA part: %s" % (toga.name if toga else "NOT FOUND"))
	var hit = ctl.hit(cam, cam.unproject_position(toga.global_transform * (toga as MeshInstance3D).get_aabb().get_center())) if toga else {}
	print("  hit at its centre: %s" % str(hit))
	print("  before: lat %d text %s flap(af) %s flap(fdm) %.1f wow1 %s fgcp %s" % [af.get_lat_mode(), str(af.get_value("it-autoflight/text/lat")), str(af.get_value("fdm/jsbsim/fcs/flap-pos-deg")), jsb.get_property("fcs/flap-pos-deg"), str(jsb.get_property("/gear/gear[1]/wow")), str(jsb.get_property("/systems/electrical/outputs/fgcp"))])
	# flaps 11 first (T/O mode wants flaps >= 5)
	jsb.set_flaps(0.36)
	await get_tree().create_timer(6.0).timeout
	print("  flaps now: fdm %.1f, af sees %s" % [jsb.get_property("fcs/flap-pos-deg"), str(af.get_value("fdm/jsbsim/fcs/flap-pos-deg"))])
	if toga:
		var took := _click(ctl, cam, toga, MOUSE_BUTTON_LEFT)
		await get_tree().create_timer(1.0).timeout
		print("  TOGA click took %s -> lat %d vert %d text %s FMA roll %s/%s pitch %s/%s" % [str(took), af.get_lat_mode(), af.get_vert_mode(), str(af.get_value("it-autoflight/text/lat")), str(af.get_value("instrumentation/fma/roll-mode-a")), str(af.get_value("instrumentation/fma/roll-mode-b")), str(af.get_value("instrumentation/fma/pitch-mode-a")), str(af.get_value("instrumentation/fma/pitch-mode-b"))])
	af.panel("toga", 0.0)
	await get_tree().create_timer(1.0).timeout
	print("  panel toga -> lat %d vert %d text %s" % [af.get_lat_mode(), af.get_vert_mode(), str(af.get_value("it-autoflight/text/lat"))])
	# ALT knob
	var alt: Node3D = _part_for(ctl, "ApPanel.altAdjust")
	print("ALT knob part: %s" % (alt.name if alt else "NOT FOUND"))
	if alt:
		print("  alt before %s" % str(af.get_value("it-autoflight/input/alt")))
		var t1 := _click(ctl, cam, alt, MOUSE_BUTTON_WHEEL_UP)
		await get_tree().create_timer(0.3).timeout
		print("  wheel up took %s -> alt %s" % [str(t1), str(af.get_value("it-autoflight/input/alt"))])
		var t2 := _click(ctl, cam, alt, MOUSE_BUTTON_WHEEL_DOWN)
		await get_tree().create_timer(0.3).timeout
		print("  wheel down took %s -> alt %s" % [str(t2), str(af.get_value("it-autoflight/input/alt"))])
		var t3 := _click(ctl, cam, alt, MOUSE_BUTTON_WHEEL_UP, true)
		await get_tree().create_timer(0.3).timeout
		print("  shift wheel up took %s -> alt %s" % [str(t3), str(af.get_value("it-autoflight/input/alt"))])
		var t4 := _click(ctl, cam, alt, MOUSE_BUTTON_LEFT)
		await get_tree().create_timer(0.3).timeout
		print("  left click took %s -> alt-armed %s" % [str(t4), str(af.get_value("it-autoflight/input/alt-armed"))])
		var t5 := _click(ctl, cam, alt, MOUSE_BUTTON_RIGHT)
		await get_tree().create_timer(0.3).timeout
		print("  right click took %s -> alt-armed %s" % [str(t5), str(af.get_value("it-autoflight/input/alt-armed"))])
	get_tree().quit(0)
