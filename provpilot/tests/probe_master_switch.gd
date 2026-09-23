extends Node
## ALT / BAT rockers: what the hit test finds and what the state does per click.
const E := 2682767.0
const N := 1258797.0
static func _centre_px(cam: Camera3D, n: Node3D) -> Vector2:
	return cam.unproject_position(n.global_transform * (n as MeshInstance3D).get_aabb().get_center())
func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	var cam: Camera3D = root.camera1
	cam.current = true
	await get_tree().create_timer(0.5).timeout
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	var ctl = root._fg_controls
	for nm in ["Switch_Alt", "Switch_Bat"]:
		var n: Node3D = mroot.find_child(nm, true, false)
		var aabb: AABB = (n as MeshInstance3D).get_aabb()
		print("  %s: aabb %s, screen %s, hover '%s', state gen=%s bat=%s" % [nm, str(aabb), str(_centre_px(cam, n)), ctl.hover_name(cam, _centre_px(cam, n)), str(ctl.state.get("controls/electric/engine/generator")), str(ctl.state.get("controls/electric/battery-switch"))])
	for nm in ["Switch_Alt", "Switch_Bat", "Switch_Alt", "Switch_Bat"]:
		var n: Node3D = mroot.find_child(nm, true, false)
		var pos: Vector2 = _centre_px(cam, n)
		var took: bool = ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false)
		ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
		await get_tree().create_timer(0.3).timeout
		print("  click %s at %s: took %s, hover now '%s', gen=%s bat=%s, dc %.0f V" % [nm, str(pos), str(took), ctl.hover_name(cam, _centre_px(cam, n)), str(ctl.state.get("controls/electric/engine/generator")), str(ctl.state.get("controls/electric/battery-switch")), root._fg_bridge.dc_volts])
	get_tree().quit(0)
