extends Node
## Probe (not a smoke test): the MD-80 cockpit at night with the panel lighting
## knobs at given levels; saves the main panel, the glareshield (FGCP) and the
## overhead, and prints how much of each shot is blown out (a channel at 250+)
## so the brightness can be judged in numbers (docs/MD80_FLIGHTGEAR_PORT.md step 13).
## Run NON-headless:
##   godot --path provpilot res://tests/probe_md80_night_shot.tscn -- <out_dir> [flood 0..1] [digital 0..1] [panel 0..1]
func _stats(img: Image, name: String) -> void:
	var w := img.get_width()
	var h := img.get_height()
	var n := 0
	var hot := 0
	var lum := 0.0
	var y := 0
	while y < h:
		var x := 0
		while x < w:
			var c := img.get_pixel(x, y)
			n += 1
			if c.r >= 0.98 or c.g >= 0.98 or c.b >= 0.98:
				hot += 1
			lum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			x += 4
		y += 4
	print("  [shot] %-10s blown-out %.2f %%  mean luminance %.3f" % [name, 100.0 * hot / maxf(n, 1), lum / maxf(n, 1)])

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var flood := float(args[1]) if args.size() > 1 else 0.0
	var digital := float(args[2]) if args.size() > 2 else 1.0
	var panel := float(args[3]) if args.size() > 3 else 1.0
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root._sky.set_time(false, 23 * 60)
	var fc = root._aircraft.get_fg_controls()
	var st: Dictionary = fc.get_state()
	for p in ["controls/lighting/capt-flood", "controls/lighting/fo-flood", "controls/lighting/pedestal-flood", "controls/lighting/overhead-flood"]:
		st[p] = flood
	for p in ["controls/lighting/capt-digital", "controls/lighting/fo-digital", "controls/lighting/pedestal-digital", "controls/lighting/fgcp-digital"]:
		st[p] = digital
	for p in ["controls/lighting/capt-panel", "controls/lighting/fo-panel", "controls/lighting/pedestal-panel", "controls/lighting/center-panel", "controls/lighting/fgcp-panel", "controls/lighting/overhead-panel"]:
		st[p] = panel
	fc.set_state(st)
	var rig = root._camera_rig
	rig.set_mode(0)
	rig.set_cockpit_enabled(true)
	root.camera1.current = true
	await get_tree().create_timer(4.0).timeout
	# every emissive mesh of the model with a non-white emission: what the panel lighting lit
	var counts: Dictionary = {}
	var stack: Array = [root._aircraft.get_model_root()]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.append(c)
		if n is MeshInstance3D and n.mesh != null:
			for si in range(n.mesh.get_surface_count()):
				var m = n.get_active_material(si)
				if m is BaseMaterial3D and m.emission_enabled:
					var e: Color = m.emission
					var key := "%s|%.2f,%.2f,%.2f|tex %s|tr %d|sh %d" % [n.name.get_slice("__", 0), e.r, e.g, e.b, str(m.emission_texture != null), m.transparency, m.shading_mode]
					counts[key] = counts.get(key, 0) + 1
	var keys := counts.keys()
	keys.sort()
	for k in keys:
		print("  [emis] %3d x %s" % [counts[k], k])
	# main panel: look down a little, one zoom step
	rig.set_head_look(0.0, 12.0)
	rig.mouse_zoom(1)
	await get_tree().create_timer(1.5).timeout
	var img := get_viewport().get_texture().get_image()
	img.save_png(out.path_join("md80_night_main.png"))
	_stats(img, "main")
	# glareshield digits
	rig.recenter_head()
	rig.set_head_look(0.0, -4.0)
	for i in range(6):
		rig.mouse_zoom(1)
	await get_tree().create_timer(1.5).timeout
	img = get_viewport().get_texture().get_image()
	img.save_png(out.path_join("md80_night_fgcp.png"))
	_stats(img, "fgcp")
	# overhead
	rig.recenter_head()
	rig.set_head_look(0.0, -60.0)
	await get_tree().create_timer(1.5).timeout
	img = get_viewport().get_texture().get_image()
	img.save_png(out.path_join("md80_night_overhead.png"))
	_stats(img, "overhead")
	print("saved night shots to %s (flood %.2f digital %.2f panel %.2f)" % [out, flood, digital, panel])
	get_tree().quit(0)
