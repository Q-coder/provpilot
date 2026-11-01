extends SceneTree
## Real-renderer look at the FlightGear-built a10_fg.glb on its own (no
## aircraft wiring): exterior 3/4 view and the pilot's eye point.
## Env: PROBE_VIEW ext|cpt|top (default ext), PROBE_PNG output, PROBE_GLB an
## absolute path to another FlightGear-built glb (top frames it from its AABB,
## the view of the shading screenshots: above and behind, sun ahead).
## Run: godot/bin/godot.macos.editor.double.arm64 --path provpilot --resolution 1400x900 -s tests/probe_fg_model_render.gd
var _frames := 0
var _png := ""
func _initialize() -> void:
	var dir: String = ProjectSettings.globalize_path("res://aircraft/a10/model")
	var glb: String = OS.get_environment("PROBE_GLB")
	if glb == "":
		glb = dir.path_join("a10_fg.glb")
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(glb, state) != OK:
		printerr("probe: glb load failed"); quit(1); return
	var model: Node = doc.generate_scene(state)
	root.add_child(model)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.45, 0.55, 0.7)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.7, 0.75, 0.8)
	env.environment.ambient_light_energy = 0.6
	root.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.5
	sun.transform = Transform3D(Basis.from_euler(Vector3(deg_to_rad(-50), deg_to_rad(-40), 0)), Vector3.ZERO)
	root.add_child(sun)
	var cam := Camera3D.new()
	root.add_child(cam)
	var view := OS.get_environment("PROBE_VIEW")
	# glTF frame: x aft (nose at 0, tail at 16), y up, z left
	var pos := Vector3(-14.0, 7.0, 18.0)
	var target := Vector3(8.0, 0.3, 0.0)
	cam.fov = 45.0
	if view == "top":
		var aabb := _model_aabb(model)
		var c := aabb.get_center()
		var r: float = aabb.size.length() * 0.5
		pos = c + Vector3(r * 1.6, r * 1.5, 0.0)
		target = c
		sun.transform = Transform3D(Basis.from_euler(Vector3(deg_to_rad(-55), deg_to_rad(160), 0)), Vector3.ZERO)
	if view == "cpt":
		pos = Vector3(2.95, 1.45, 0.0)      # FlightGear eye point (3.2 aft, 1.5 up), a bit forward
		target = Vector3(2.4, 1.05, 0.0)
		cam.fov = 80.0
		var fill := OmniLight3D.new()
		fill.light_energy = 1.2
		fill.omni_range = 3.0
		fill.position = Vector3(3.0, 1.55, 0.0)
		root.add_child(fill)
	cam.near = 0.05
	cam.far = 500.0
	var fwd: Vector3 = (target - pos).normalized()
	var right: Vector3 = fwd.cross(Vector3.UP).normalized()
	var up: Vector3 = right.cross(fwd).normalized()
	cam.transform = Transform3D(Basis(right, up, -fwd), pos)
	cam.make_current()
	_png = OS.get_environment("PROBE_PNG")
func _model_aabb(n: Node) -> AABB:
	var box := AABB()
	var first := true
	for m in n.find_children("*", "MeshInstance3D", true, false):
		var mi := m as MeshInstance3D
		if mi.mesh == null:
			continue
		# not in the tree yet in _initialize: compose the local transforms
		var xf := Transform3D()
		var node: Node = mi
		while node != null and node != n.get_parent():
			if node is Node3D:
				xf = (node as Node3D).transform * xf
			node = node.get_parent()
		var b: AABB = xf * mi.get_aabb()
		if first:
			box = b; first = false
		else:
			box = box.merge(b)
	return box
func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 12:
		if _png != "":
			root.get_viewport().get_texture().get_image().save_png(_png)
			print("probe_fg_model_render: screenshot ", _png)
		quit(0)
	return false
