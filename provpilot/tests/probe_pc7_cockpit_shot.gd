extends Node
## Probe (not a smoke test): spawn the PC-7 at LSZH, cancel the intro and save
## the cockpit camera's view to /tmp/pc7_cockpit.png for a visual check.
## Run NON-headless: godot --path provpilot res://tests/probe_pc7_cockpit_shot.tscn
func _ready() -> void:
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("pc7", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root.camera1.current = true
	await get_tree().create_timer(2.0).timeout
	var img: Image = get_viewport().get_texture().get_image()
	img.save_png("/tmp/pc7_cockpit.png")
	print("saved /tmp/pc7_cockpit.png")
	get_tree().quit(0)
