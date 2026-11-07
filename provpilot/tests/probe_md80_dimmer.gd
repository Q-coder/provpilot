extends Node
## Probe (not a smoke test): the FGCP dimmers. Sets the digital / panel dimmer
## controls and prints the chain: FDM controls -> systems/lighting -> animator
## -> label colours / backplate emission.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_dimmer.tscn
func _ready() -> void:
	get_window().size = Vector2i(1200, 800)
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
	var jsb = root.jsb_node
	var rig = root._aircraft
	var fga = root._fg_animator
	var fgc = rig.get_fg_controls()
	await get_tree().create_timer(3.0).timeout
	for step in [[1.0, 0.0], [0.3, 0.8], [0.0, 0.3]]:
		fgc.assign_external("controls/lighting/fgcp-digital", step[0])
		fgc.assign_external("controls/lighting/fgcp-panel", step[1])
		await get_tree().create_timer(2.5).timeout
		print("---- digital %.1f panel %.1f" % [step[0], step[1]])
		for p in ["/controls/lighting/fgcp-digital", "/systems/lighting/fgcp-digital", "/controls/lighting/fgcp-panel", "/systems/lighting/fgcp-panel", "/systems/electrical/bus/ac-r", "/controls/cockpit/fgcp/digital"]:
			print("  FDM %s = %s" % [p, str(jsb.get_property(p))])
		for p in ["systems/lighting/fgcp-digital", "systems/lighting/fgcp-panel", "controls/lighting/fgcp-digital"]:
			print("  animator %s = %s" % [p, str(fga.get_property(p))])
		var seen := {}
		for lb in root.find_children("Digits*", "Label3D", true, false):
			var key := "%s %s vis %s" % [lb.name, str(lb.modulate), str(lb.is_visible_in_tree())]
			if not seen.has(lb.name):
				seen[lb.name] = true
				print("  label " + key + " text '" + lb.text + "'")
		for mi in root.find_children("*FGCPLabels*", "MeshInstance3D", true, false):
			var m = mi.get_active_material(0)
			if m is BaseMaterial3D:
				print("  mesh %s emission_on %s emission %s" % [mi.name, str(m.emission_enabled), str(m.emission)])
	get_tree().quit(0)
