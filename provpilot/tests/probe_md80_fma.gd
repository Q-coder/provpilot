extends Node
## Probe (not a smoke test): why the MD-80's FMA windows show nothing. Prints
## the FMA screens' power property as the FDM, the bridge and the animator see
## it, and every ScreenText label's visibility / text / transform.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_fma.tscn -- [ground]
const E := 2682767.0
const N := 1258797.0

func _ready() -> void:
	var ground := OS.get_cmdline_user_args().has("ground")
	get_window().size = Vector2i(1200, 800)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	if not ground:
		root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 6000.0
		root._settings.get_settings()["aircraft_md80_start_speed_kt"] = 250.0
		root._settings.get_settings()["aircraft_md80_start_heading_deg"] = 155.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var rig = root._aircraft
	var fga = root._fg_animator
	var br = rig.get_fg_bridge()
	for t in range(3):
		await get_tree().create_timer(3.0).timeout
		print("---- t+%d s (%s start)" % [(t + 1) * 3, "ground" if ground else "airborne"])
		for p in ["/systems/electrical/outputs/fma[0]", "/systems/electrical/outputs/fma[1]", "/systems/electrical/bus/dc-radio-l", "/systems/electrical/bus/dc-radio-r", "/systems/electrical/outputs/comm[1]"]:
			print("  FDM %s = %s (has %s)" % [p, str(jsb.get_property(p)), str(jsb.has_property(p))])
		for p in ["systems/electrical/outputs/fma", "systems/electrical/outputs/fma[1]", "instrumentation/fma/thr-mode-a", "instrumentation/fma/roll-mode-a", "instrumentation/fma/pitch-mode-a"]:
			print("  animator %s = %s" % [p, str(fga.get_property(p))])
		var pulled: Dictionary = br.get_published() if br.has_method("get_published") else {}
		for k in ["systems/electrical/outputs/fma", "systems/electrical/outputs/fma[1]"]:
			print("  bridge published %s = %s" % [k, str(pulled.get(k, "<none>"))])
	var cam: Camera3D = root.camera1
	var n := 0
	for lb in root.find_children("ScreenText_*", "Label3D", true, false):
		n += 1
		var gp: Vector3 = lb.global_position
		var fwd: Vector3 = -lb.global_transform.basis.z
		var to_cam: Vector3 = (cam.global_position - gp).normalized()
		print("  label %s visible %s (tree %s) text '%s' dist %.2f m facing cam %.2f" % [lb.name, str(lb.visible), str(lb.is_visible_in_tree()), lb.text, gp.distance_to(cam.global_position), fwd.dot(to_cam)])
	print("  %d labels" % n)
	get_tree().quit(0)
