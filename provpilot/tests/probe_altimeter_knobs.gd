extends Node
## Hover and click the altimeter knobs from the pilot's eye: prints every
## control the pick ray crosses under each knob part. Run:
##   godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_altimeter_knobs.tscn
const E := 2682767.0
const N := 1258797.0

func _centre(n: Node3D) -> Vector3:
	if n is MeshInstance3D and (n as MeshInstance3D).mesh != null:
		return n.global_transform * (n as MeshInstance3D).get_aabb().get_center()
	return n.global_position

func _ray_hits(ctl, cam: Camera3D, pos: Vector2) -> Array:
	var from: Vector3 = cam.project_ray_origin(pos)
	var dir: Vector3 = cam.project_ray_normal(pos)
	var out: Array = []
	for it in ctl._items:
		var n: MeshInstance3D = it.node
		if not is_instance_valid(n) or not n.is_visible_in_tree():
			continue
		var inv: Transform3D = n.global_transform.affine_inverse()
		var p = (it.aabb as AABB).intersects_ray(inv * from, (inv.basis * dir).normalized())
		if p != null:
			out.append("%s@%.3f" % [n.name, (n.global_transform * p - from).length()])
	return out

func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
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
	await get_tree().create_timer(1.0).timeout   # select animations follow the view
	var ctl = root._fg_controls
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	for nm in ["altimeter__ingh-knob-left", "altimeter__ingh-knob-right", "altimeter__knob", "altimeter__face"]:
		var n = mroot.find_child(nm, true, false)
		if n == null or not (n is Node3D):
			print("  %s: not found" % nm)
			continue
		var pos: Vector2 = cam.unproject_position(_centre(n))
		print("  %s: visible %s, screen %s, %.2f m from the eye, hover -> '%s', ray hits %s" % [nm, str((n as Node3D).is_visible_in_tree()), str(pos), (_centre(n) - cam.global_position).length(), ctl.hover_name(cam, pos), str(_ray_hits(ctl, cam, pos))])
	for nm in ["altimeter__ingh-knob-left", "altimeter__ingh-knob-right"]:
		var n = mroot.find_child(nm, true, false)
		if n == null:
			continue
		var pos: Vector2 = cam.unproject_position(_centre(n))
		var before: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
		var after_l: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, false, false)
		var after_r: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_UP, true, false); ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_UP, false, false)
		var after_w: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_DOWN, true, true); ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_DOWN, false, true)
		var after_ws: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		print("  %s: setting %.3f -> left %.3f -> right %.3f -> wheel up %.3f -> shift wheel down %.3f" % [nm, before, after_l, after_r, after_w, after_ws])
	get_tree().quit(0)
