extends Node
## Where the meshes of an aircraft model sit, in metres from the aircraft
## node (right, up, forward): for placing lights, exhausts, contact points.
## Run: godot --headless --path provpilot res://tests/probe_model_nodes.tscn -- <id> <name pattern, regex, case-insensitive>
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "a10"
	var re := RegEx.new()
	re.compile("(?i)" + (args[1] if args.size() > 1 else "."))
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(4.0).timeout
	root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
	root._settings.set_setting("aircraft_%s_start_speed_kt" % id, 0.0)
	root._on_welcome_flight_requested(id, 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	var ac: Node3D = root.get_node("AC")
	var xf: Transform3D = ac.global_transform
	var b: Basis = xf.basis.orthonormalized()
	var stack: Array = [ac]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.append(c)
		if n is MeshInstance3D and (n as MeshInstance3D).mesh != null and re.search(n.name) != null:
			var mi := n as MeshInstance3D
			var lo := Vector3(INF, INF, INF)
			var hi := -lo
			for si in range(mi.mesh.get_surface_count()):
				for v in mi.mesh.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]:
					var w: Vector3 = mi.global_transform * v - xf.origin
					var p := Vector3(-w.dot(b.x), w.dot(b.y), w.dot(b.z))
					lo = lo.min(p)
					hi = hi.max(p)
			print("nodes: %-44s right %+6.2f..%+6.2f  up %+6.2f..%+6.2f  forward %+7.2f..%+7.2f  %s" % [n.name, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, "" if n.is_visible_in_tree() else "(hidden)"])
	get_tree().quit(0)
