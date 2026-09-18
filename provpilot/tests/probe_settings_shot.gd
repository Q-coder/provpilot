extends Node
## Probe: open the Settings screen on a given tab and save a screenshot.
## Run (windowed): godot --path provpilot res://tests/probe_settings_shot.tscn
## Env: PROBE_TAB (index, default 4 = Weather), PROBE_OUT (png path).
func _ready() -> void:
	var screen := SettingsScreen.new()
	add_child(screen)
	await get_tree().process_frame
	screen.show_settings()
	var tabs: TabContainer = _find(screen, "TabContainer")
	if tabs:
		tabs.current_tab = int(OS.get_environment("PROBE_TAB")) if OS.get_environment("PROBE_TAB") != "" else 4
	if OS.get_environment("PROBE_TICK") == "1":
		var cb: CheckBox = _find(tabs.get_current_tab_control(), "CheckBox") if tabs else null
		if cb:
			cb.button_pressed = true
	await get_tree().create_timer(0.8).timeout
	var out := OS.get_environment("PROBE_OUT")
	if out == "":
		out = "user://settings_shot.png"
	var img := get_viewport().get_texture().get_image()
	img.save_png(out)
	print("saved ", out, " ", img.get_size())
	get_tree().quit(0)

func _find(n: Node, cls: String) -> Node:
	if n.get_class() == cls:
		return n
	for c in n.get_children():
		var r := _find(c, cls)
		if r:
			return r
	return null
