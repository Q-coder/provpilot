extends Node
## Probe (not a smoke test): spawn an aircraft at LSZH, apply a livery of its
## catalog (docs/AIRCRAFT_CONFIG.md [liveries]) and save the orbit camera's
## view and the cockpit view to <out_dir>/<id>_<livery>_exterior|cockpit.png.
## Run NON-headless: godot --path provpilot res://tests/probe_livery_shot.tscn -- <aircraft_id> [livery_id|-] [out_dir]
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "md81"
	var livery: String = args[1] if args.size() > 1 and args[1] != "-" else ""
	var out: String = args[2] if args.size() > 2 else "/tmp"
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	if root._settings:
		root._settings.get_settings().erase("aircraft_%s_livery" % id)
	root._on_welcome_flight_requested(id, 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root._aircraft.set_livery(livery)
	var names: Array = []
	for l in root._aircraft.get_liveries():
		names.append(l.get("id", ""))
	print("probe_livery_shot: %s liveries: %s; applied '%s'" % [id, ", ".join(names), livery])
	var tag := livery if livery != "" else "asbuilt"
	root.camera2.current = true
	await get_tree().create_timer(2.0).timeout
	get_viewport().get_texture().get_image().save_png("%s/%s_%s_exterior.png" % [out, id, tag])
	root.camera1.current = true
	await get_tree().create_timer(1.0).timeout
	get_viewport().get_texture().get_image().save_png("%s/%s_%s_cockpit.png" % [out, id, tag])
	print("probe_livery_shot: saved %s/%s_%s_exterior.png and _cockpit.png" % [out, id, tag])
	get_tree().quit(0)
