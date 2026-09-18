extends Node
## Manual visual check for the tabbed C++ SettingsScreen (run NON-headless so
## the canvas is actually rasterized):
##   godot/bin/godot.macos.editor.double.arm64 --path provpilot --res tests/visual_settings.tscn
## Mounts only the settings screen (no node_3d scene, no scenery), cycles
## through every tab, saves one viewport screenshot per tab to /tmp, then
## quits. Never writes provpilot.cfg.

var screen: SettingsScreen = null
var tabs: TabContainer = null
var frame := 0
var tab := 0


func _ready() -> void:
	screen = SettingsScreen.new()
	add_child(screen)
	screen.show_settings()
	tabs = _find_first(screen, "TabContainer")
	assert(tabs != null)


func _process(_delta: float) -> void:
	frame += 1
	# Give each tab a few frames to lay out before capturing it.
	if frame % 6 == 3:
		tabs.current_tab = tab
	elif frame % 6 == 0:
		_capture(tabs.get_tab_title(tab).to_lower())
		tab += 1
		if tab >= tabs.get_tab_count():
			get_tree().quit(0)


func _capture(tag: String) -> void:
	var img := get_viewport().get_texture().get_image()
	img.save_png("/tmp/settings_%s.png" % tag)
	print("saved /tmp/settings_%s.png" % tag)


func _find_first(root: Node, cls: String) -> Node:
	if root.get_class() == cls:
		return root
	for c in root.get_children():
		var r := _find_first(c, cls)
		if r != null:
			return r
	return null
