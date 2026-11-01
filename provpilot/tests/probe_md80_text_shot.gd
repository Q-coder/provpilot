extends Node
## Probe (not a smoke test): spawn the MD-80 at LSZH and save close-ups of the
## FGCP digit windows (glareshield) and the pedestal radio heads, the FlightGear
## <text> elements FGAnimator renders as Label3Ds (docs/MD80_FLIGHTGEAR_PORT.md step 7).
## Run NON-headless: godot --path provpilot res://tests/probe_md80_text_shot.tscn -- <out_dir>
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
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
	var rig = root._camera_rig
	rig.set_mode(0) # cockpit: the intro cancel leaves the rig in orbit
	rig.set_cockpit_enabled(true)
	root.camera1.current = true
	# glareshield: look slightly up, zoomed in
	rig.set_head_look(0.0, -4.0)
	for i in range(6):
		rig.mouse_zoom(1)
	await get_tree().create_timer(1.5).timeout
	get_viewport().get_texture().get_image().save_png(out.path_join("md80_fgcp.png"))
	# pedestal: look down and right
	rig.recenter_head()
	rig.set_head_look(25.0, 55.0)
	for i in range(4):
		rig.mouse_zoom(1)
	await get_tree().create_timer(1.5).timeout
	get_viewport().get_texture().get_image().save_png(out.path_join("md80_pedestal.png"))
	print("saved %s and %s" % [out.path_join("md80_fgcp.png"), out.path_join("md80_pedestal.png")])
	get_tree().quit(0)
