extends SceneTree
## Real-renderer look at a vehicle model with its aircraft.cfg [model] fit
## applied (rotation, scale, offset, hidden nodes), next to a reference: a
## 1 m grid on the ground, a red bar along the AC frame's nose (+Z, 5 m) and
## a green bar along +X (left, 2 m). Shows whether the nose points the right
## way and the wheels sit on y = 0.
## Env: PROBE_ID car|re44 (default car), PROBE_PNG output path, PROBE_VIEW
## ext|cpt (the cfg's cockpit_position, AC-node units of 0.1 m, looking along +Z).
## Run: godot/bin/godot.macos.editor.double.arm64 --path provpilot --resolution 1400x900 -s tests/probe_vehicle_render.gd
var _frames := 0
var _png := ""


func _box(size: Vector3, pos: Vector3, color: Color) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	var bm := BoxMesh.new()
	bm.size = size
	mi.mesh = bm
	var mat := StandardMaterial3D.new()
	mat.albedo_color = color
	mi.material_override = mat
	mi.position = pos
	return mi


func _initialize() -> void:
	var id: String = OS.get_environment("PROBE_ID")
	if id == "":
		id = "car"
	_png = OS.get_environment("PROBE_PNG")
	if _png == "":
		_png = "/tmp/vehicle_%s.png" % id
	var cfg := AircraftConfig.load_for_aircraft(id)
	var dir: String = ProjectSettings.globalize_path("res://aircraft/%s" % id)
	var file: String = cfg.get_string("model", "file", "model/%s.glb" % id)
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var path := dir.path_join(file)
	if not FileAccess.file_exists(path):
		path = ProjectSettings.globalize_path("res://../aircraft/%s/%s" % [id, file])
	if doc.append_from_file(path, state) != OK:
		printerr("probe: glb load failed: ", path)
		quit(1)
		return
	var model: Node3D = doc.generate_scene(state)
	# the [model] fit, as AircraftModelLoader::model_transform applies it, on
	# a holder scaled 0.2 (the AC frame draws models at 0.2 m per unit)
	var holder := Node3D.new()
	holder.scale = Vector3(0.2, 0.2, 0.2)
	var rot: Array = cfg.get_float_array("model", "rotation_deg", [])
	var basis := Basis()
	if rot.size() == 3:
		basis = Basis.from_euler(Vector3(deg_to_rad(rot[0]), deg_to_rad(rot[1]), deg_to_rad(rot[2])))
	var s: float = cfg.get_float("model", "scale", 1.0)
	basis = basis.scaled(Vector3(s, s, s))
	var fit := Node3D.new()
	fit.transform = Transform3D(basis, cfg.get_vector3("model", "offset", Vector3()))
	root.add_child(holder)
	holder.add_child(fit)
	fit.add_child(model)
	for name in cfg.get_string("model", "hide_nodes", "").split(",", false):
		var n: Node = model.find_child(name.strip_edges(), true, false)
		if n is Node3D:
			n.visible = false
	_print_bounds(model, holder)
	# reference
	root.add_child(_box(Vector3(0.1, 0.1, 5.0), Vector3(0, 0.05, 2.5), Color(1, 0.2, 0.2)))   # nose +Z
	root.add_child(_box(Vector3(2.0, 0.1, 0.1), Vector3(1.0, 0.05, 0), Color(0.2, 1, 0.2)))   # left +X
	root.add_child(_box(Vector3(0.1, 2.0, 0.1), Vector3(0, 1.0, 0), Color(0.3, 0.5, 1)))      # up
	for i in range(-6, 7):
		root.add_child(_box(Vector3(12.0, 0.02, 0.03), Vector3(0, 0, i), Color(0.6, 0.6, 0.6)))
		root.add_child(_box(Vector3(0.03, 0.02, 12.0), Vector3(i, 0, 0), Color(0.6, 0.6, 0.6)))
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.45, 0.55, 0.7)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.7, 0.75, 0.8)
	env.environment.ambient_light_energy = 0.6
	root.add_child(env)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50, 30, 0)
	sun.light_energy = 1.2
	root.add_child(sun)
	var cam := Camera3D.new()
	var d: float = 9.0 if id == "car" else 28.0
	cam.fov = 45
	root.add_child(cam)
	if OS.get_environment("PROBE_VIEW") == "cpt":
		var cp: Array = cfg.get_float_array("camera", "cockpit_position", [0, 10, 0])
		if OS.get_environment("PROBE_EYE") != "":   # "x, y, z" override in the same units, to try eye points
			cp = Array(OS.get_environment("PROBE_EYE").split(",")).map(func(v): return float(v))
		var eye := Vector3(cp[0], cp[1], cp[2]) * 0.1
		cam.fov = cfg.get_float("camera", "cockpit_fov", 70.0)
		cam.look_at_from_position(eye, eye + Vector3(0, -0.05, 1), Vector3.UP)
		cam.near = 0.05
		print("cockpit eye (m, AC frame): ", eye)
	else:
		cam.look_at_from_position(Vector3(-d * 0.8, d * 0.45, d * 0.7), Vector3(0, 0.8 if id == "car" else 2.0, 0), Vector3.UP)
	cam.make_current()


## World AABB of the visible meshes after the fit: length / width / height
## in metres and where the wheels (min y) end up.
func _print_bounds(model: Node, holder: Node3D) -> void:
	var first := true
	var aabb := AABB()
	var stack: Array = [model]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.append(c)
		if n is MeshInstance3D and n.mesh and n.visible:
			# transforms up to and including the holder (not in the tree yet in _initialize)
			var xf := Transform3D()
			var q: Node = n
			while q != null and q != holder.get_parent():
				if q is Node3D:
					xf = (q as Node3D).transform * xf
				q = q.get_parent()
			var b: AABB = xf * n.mesh.get_aabb()
			aabb = b if first else aabb.merge(b)
			first = false
	print("bounds (m, AC frame x left / y up / z nose): min ", aabb.position, " size ", aabb.size)


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 12:
		var img := root.get_viewport().get_texture().get_image()
		img.save_png(_png)
		print("saved ", _png, " ", img.get_size())
		quit(0)
	return false
