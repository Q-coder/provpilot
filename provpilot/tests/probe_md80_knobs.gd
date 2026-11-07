extends Node
## Probe (not a smoke test): the FGCP dimmer knobs (KnobDigital / KnobPanel)
## through the cockpit camera: what the controls see at the knob, what a right
## click (increase) does to controls/lighting/fgcp-digital / fgcp-panel.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_knobs.tscn
func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var cam: Camera3D = root.camera1
	cam.current = true
	var jsb = root.jsb_node
	var rig = root._aircraft
	var ctl = rig.get_fg_controls()
	await get_tree().create_timer(3.0).timeout
	for name in ["fgcp__KnobDigital", "fgcp__KnobPanel"]:
		var node: Node3D = null
		var recs = null
		for it in ctl.get_items():
			if (it["node"] as Node).name == name:
				node = it["node"]
				recs = it["recs"]
		print("---- %s: item %s" % [name, str(node != null)])
		if node == null:
			continue
		print("  recs: %s" % JSON.stringify(recs).left(400))
		var centre: Vector3 = node.global_transform * (node as MeshInstance3D).get_aabb().get_center()
		var px: Vector2 = cam.unproject_position(centre)
		print("  centre %s, %.2f m from the camera, px %s, in front %s" % [str(centre), centre.distance_to(cam.global_position), str(px), str(not cam.is_position_behind(centre))])
		var h: Dictionary = ctl.hit(cam, px)
		print("  hit: %s, hover '%s'" % [str((h.get("node") as Node).name) if h.has("node") else "{}", ctl.hover_name(cam, px)])
		var prop := "controls/lighting/fgcp-digital" if name.ends_with("Digital") else "controls/lighting/fgcp-panel"
		var before: float = float(ctl.value_of(prop))
		for i in range(3):
			var took: bool = ctl.click(cam, px, MOUSE_BUTTON_RIGHT, true, false)
			ctl.click(cam, px, MOUSE_BUTTON_RIGHT, false, false)
			print("  right click %d took %s -> %s = %s" % [i, str(took), prop, str(ctl.value_of(prop))])
		for i in range(2):
			ctl.click(cam, px, MOUSE_BUTTON_WHEEL_UP, true, false)
			ctl.click(cam, px, MOUSE_BUTTON_WHEEL_UP, false, false)
		print("  after 2 wheel up: %s (before %.2f); state has key %s" % [str(ctl.value_of(prop)), before, str(ctl.get_state().has(prop))])
		await get_tree().create_timer(1.5).timeout
		print("  FDM /%s = %s" % [prop, str(jsb.get_property("/" + prop))])
	get_tree().quit(0)
