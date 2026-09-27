extends SceneTree
## Render a GLB to a PNG for a visual check (not a smoke test):
##   godot --path provpilot -s tests/render_glb.gd -- <file.glb> <out.png> [yaw_deg] [pitch_deg]
## Frames the model's bounding box from the given direction with a sun and sky.

func _init() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("usage: render_glb.gd -- <file.glb> <out.png> [yaw] [pitch]")
		quit(1)
		return
	var yaw := deg_to_rad(float(args[2])) if args.size() > 2 else deg_to_rad(35.0)
	var pitch := deg_to_rad(float(args[3])) if args.size() > 3 else deg_to_rad(25.0)
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err := doc.append_from_file(args[0], state)
	if err != OK:
		push_error("load failed: %d" % err)
		quit(1)
		return
	var model: Node3D = doc.generate_scene(state)
	var root3d := Node3D.new()
	root.add_child(root3d)
	root3d.add_child(model)
	await process_frame
	var aabb := _aabb(model)
	print("render_glb: aabb pos %s size %s, %d mesh instances" % [aabb.position, aabb.size, _count(model)])
	var cam := Camera3D.new()
	root3d.add_child(cam)
	var centre := aabb.get_center()
	var radius: float = maxf(aabb.size.length() * 0.6, 1.0)
	var dir := Vector3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch))
	cam.global_position = centre + dir * radius * 1.4
	cam.look_at(centre, Vector3.UP)
	cam.far = radius * 20.0
	cam.near = maxf(radius * 0.01, 0.05)
	cam.current = true
	var sun := DirectionalLight3D.new()
	root3d.add_child(sun)
	sun.rotation_degrees = Vector3(-50, 30, 0)
	sun.light_energy = 1.2
	sun.shadow_enabled = true
	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.55, 0.7, 0.9)
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	e.ambient_light_color = Color(0.7, 0.75, 0.8)
	e.ambient_light_energy = 0.6
	env.environment = e
	root3d.add_child(env)
	root.size = Vector2i(1280, 800)
	await process_frame
	await process_frame
	await process_frame
	var img := root.get_texture().get_image()
	img.save_png(args[1])
	print("render_glb: wrote %s" % args[1])
	quit(0)


func _aabb(n: Node) -> AABB:
	var box := AABB()
	var first := true
	for mi in n.find_children("*", "MeshInstance3D", true, false):
		var b: AABB = (mi as MeshInstance3D).global_transform * (mi as MeshInstance3D).get_aabb()
		if first:
			box = b
			first = false
		else:
			box = box.merge(b)
	return box


func _count(n: Node) -> int:
	return n.find_children("*", "MeshInstance3D", true, false).size()
