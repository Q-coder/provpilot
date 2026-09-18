extends Node
## Manual visual check for the VFR map (run NON-headless so the canvas is
## actually rasterized):
##   godot/bin/godot.macos.editor.double.arm64 --path provpilot --res tests/visual_vfr_map.tscn
## Drives the C++ module with a fixed position near LSZH at two headings /
## positions, saves two viewport screenshots to /tmp, then quits. Inspect the
## PNGs: tiles must show terrain (not flat blocks), stay inside the frame,
## and canton borders must stop at the frame edge.

const CENTER := Vector2(2682880.0, 1283072.0)  # setup() LV95 origin
const AC_E := 2683400.0                        # near LSZH
const AC_N := 1282700.0

var app: Node = null
var vfr_map: VfrMap = null
var frame := 0


func _ready() -> void:
	app = load("res://node_3d.tscn").instantiate()
	add_child(app)


func _process(_delta: float) -> void:
	frame += 1
	if app == null:
		return
	if frame == 2:
		vfr_map = app.get_node_or_null("Control/VfrMap")
		assert(vfr_map != null)
		vfr_map.setup("/tmp/vfr_visual.cfg")
		# Hide full-screen UI layers (welcome/settings) so the HUD is visible.
		for c in app.get_children():
			if c is CanvasLayer:
				c.visible = false
	_drive()


func _drive() -> void:
	# Phase 1 (frames 2..159): heading 250 at AC_E/AC_N.
	# Phase 2 (frames 160+):   heading 90, 3 km further east.
	var e := AC_E if frame < 160 else AC_E + 3000.0
	var heading := 250.0 if frame < 160 else 90.0
	vfr_map.update_lv95(e, AC_N, heading, false)
	if frame == 150:
		_save("frame1")
	elif frame == 260:
		_save("frame2")
		get_tree().quit(0)


func _save(tag: String) -> void:
	var img := get_viewport().get_texture().get_image()
	img.save_png("/tmp/vfr_map_%s.png" % tag)
	print("saved /tmp/vfr_map_%s.png (%dx%d)" % [tag, img.get_width(), img.get_height()])
