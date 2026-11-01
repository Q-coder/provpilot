extends Node
## Probe (not a smoke test): spawn an aircraft at LSZH, cancel the intro and
## save the cockpit camera's view and the orbit camera's view to
## /tmp/<id>_cockpit.png and /tmp/<id>_exterior.png.
## Run NON-headless: godot --path provpilot res://tests/probe_cockpit_shot.tscn -- <aircraft_id> [pitch_deg]
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "md80"
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested(id, 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root.camera1.current = true
	await get_tree().create_timer(2.0).timeout
	get_viewport().get_texture().get_image().save_png("/tmp/%s_cockpit.png" % id)
	root.camera2.current = true
	await get_tree().create_timer(1.0).timeout
	get_viewport().get_texture().get_image().save_png("/tmp/%s_exterior.png" % id)
	print("saved /tmp/%s_cockpit.png and /tmp/%s_exterior.png" % [id, id])
	var fga: FGAnimator = root._fg_animator
	if fga:
		var unknown: PackedStringArray = fga.unknown_properties()
		print("unbridged properties (%d): %s" % [unknown.size(), ", ".join(unknown.slice(0, 80))])
	get_tree().quit(0)
