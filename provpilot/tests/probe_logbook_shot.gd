extends Node
## Screenshots of Settings > Logbook and of the black box of the newest flight.
## Run: godot --path provpilot res://tests/probe_logbook_shot.tscn -- <out_dir>
func _ready() -> void:
	var out_dir: String = OS.get_cmdline_user_args()[0] if OS.get_cmdline_user_args().size() > 0 else ""
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(4.0).timeout
	root._welcome_screen.visible = false
	root._settings_screen.show_settings()
	var tabs: TabContainer = root._settings_screen.find_children("*", "TabContainer", true, false)[0]
	for i in range(tabs.get_tab_count()):
		if tabs.get_tab_title(i) == "Logbook":
			tabs.current_tab = i
	await get_tree().create_timer(1.0).timeout
	get_viewport().get_texture().get_image().save_png(out_dir.path_join("logbook_list.png"))
	var panel = root._settings_screen.find_children("*", "LogbookPanel", true, false)[0]
	panel._on_black_box()
	await get_tree().create_timer(1.0).timeout
	get_viewport().get_texture().get_image().save_png(out_dir.path_join("logbook_box.png"))
	get_tree().quit(0)
