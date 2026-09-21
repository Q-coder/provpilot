extends Node
## Real-renderer look at Settings > Aircraft. Env: PROBE_PNG, PROBE_FLY=1 to
## screenshot while flying the A-10 (live CG) instead of on the welcome page.
## Run: godot/bin/godot.macos.editor.double.arm64 --path provpilot --resolution 1400x900 res://tests/probe_settings_aircraft_tab.tscn
const E := 2682767.0
const N := 1258797.0
func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._welcome_screen._on_aircraft_selected(2)
	if OS.get_environment("PROBE_FLY") == "1":
		root._on_welcome_flight_requested("a10", E, N, 155.0)
		var waited := 0.0
		while not root.terrain_initialized and waited < 60.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		await get_tree().create_timer(2.0).timeout
		root._cancel_intro_zoom()
		root._welcome_screen.visible = false
	var screen = root._settings_screen
	screen.show_settings()
	await get_tree().process_frame
	# select the Aircraft tab (first)
	var tabs: TabContainer = screen.find_child("*", true, false) as TabContainer if false else null
	for n in screen.find_children("*", "TabContainer", true, false):
		(n as TabContainer).current_tab = 0
	for i in range(12):
		await get_tree().process_frame
	var png := OS.get_environment("PROBE_PNG")
	if png != "":
		get_viewport().get_texture().get_image().save_png(png)
		print("probe_settings_aircraft_tab: screenshot ", png)
	get_tree().quit(0)
