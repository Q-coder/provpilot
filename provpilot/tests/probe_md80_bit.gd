extends Node
## MD-80 DFGC power-up test on the FMAs: after a ground start (and two minutes
## after landing) each FMA flashes "DFGC 973 BOX n / PWR UP TST" while the
## FDM's /systems/dfgs/bit-n/active runs, then returns to the modes.
## Run: godot --path provpilot res://tests/probe_md80_bit.tscn -- [shot_dir] [seconds]
const E := 2682767.0
const N := 1258797.0

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var shot_dir: String = args[0] if args.size() > 0 else ""
	var seconds: float = float(args[1]) if args.size() > 1 else 25.0
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.set_setting("aircraft_md80_start_agl_ft", 0.0)
	root._settings.set_setting("aircraft_md80_start_speed_kt", 0.0)
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var af = root._aircraft.get_autoflight()
	var seen1 := false
	var seen2 := false
	var blank := false
	var after := ""
	var shot := false
	var t := 0.0
	while t < seconds:
		await get_tree().create_timer(0.5).timeout
		t += 0.5
		var pub: Dictionary = af.get_published()
		var b1: float = jsb.get_property("/systems/dfgs/bit-1/active")
		var l1 := "%s %s %s %s / %s %s %s %s" % [pub.get("instrumentation/fma/s1/thr-mode-a", "?"), pub.get("instrumentation/fma/s1/arm-mode-a", "?"), pub.get("instrumentation/fma/s1/roll-mode-a", "?"), pub.get("instrumentation/fma/s1/pitch-mode-a", "?"),
			pub.get("instrumentation/fma/s1/thr-mode-b", "?"), pub.get("instrumentation/fma/s1/arm-mode-b", "?"), pub.get("instrumentation/fma/s1/roll-mode-b", "?"), pub.get("instrumentation/fma/s1/pitch-mode-b", "?")]
		var p2 := str(pub.get("instrumentation/fma/s2/pitch-mode-a", "?"))
		print("probe_bit: t+%4.1f bit-1 %.0f (timer %.2f, cmd %.0f, wow-timer %.0f, power %.0f) bit-2 %.0f | FMA 1: [%s] | FMA 2 pitch: [%s]" % [t, b1, jsb.get_property("/systems/dfgs/bit-1/timer"), jsb.get_property("/systems/dfgs/bit-1/cmd"),
			jsb.get_property("/systems/dfgs/nlg-timer-120/wow-timer"), jsb.get_property("/systems/dfgs/dfgs-1-power"), jsb.get_property("/systems/dfgs/bit-2/active"), l1, p2])
		if b1 > 0.5 and "DFGC 973" in l1 and "BOX1" in l1 and "PWR UP TST" in l1:
			seen1 = true
			if shot_dir != "" and not shot:
				shot = true
				root._camera_rig.set_mode(0)
				root.camera_mode = 0
				root._camera_rig.set_cockpit_enabled(true)
				await get_tree().process_frame
				await get_tree().process_frame
				get_viewport().get_texture().get_image().save_png(shot_dir.path_join("md80_bit.png"))
		if b1 > 0.5 and l1.strip_edges() == "/":
			blank = true
		if p2 == "BOX2":
			seen2 = true
		if seen1 and b1 < 0.5:
			after = l1
	var ok := seen1 and seen2 and blank and after != "" and not ("DFGC" in after)
	print(("  PASS: " if ok else "  FAIL: ") + "both FMAs flash the DFGC power-up test (BOX 1 %s, BOX 2 %s, blank phase %s) and return to the modes [%s]" % [str(seen1), str(seen2), str(blank), after])
	get_tree().quit(0 if ok else 1)
