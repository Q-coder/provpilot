extends Node
## Probe: AC-local positions (the [camera] cockpit_position frame) of named model
## nodes, for placing the cockpit camera of a ported aircraft.
## Run: godot --headless --path provpilot res://tests/probe_ac_nodes.tscn -- <aircraft_id> <node> [<node> ...]
const E := 2682767.0
const N := 1258797.0

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "ask21"
	var names: Array = args.slice(1) if args.size() > 1 else ["Sub_panelF", "Sub_panelR", "Sub_interior"]
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested(id, E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(2.0).timeout
	var ac: Node3D = root.get_node("AC")
	var cam: Camera3D = root.camera1
	if cam:
		print("probe_ac_nodes: Camera1 AC-local %s" % str(ac.to_local(cam.global_position)))
	for nm in names:
		var n: Node = ac.find_child(nm, true, false)
		if n == null:
			print("probe_ac_nodes: %s not found" % nm)
			continue
		var n3 := n as Node3D
		var lo := Vector3(INF, INF, INF)
		var hi := Vector3(-INF, -INF, -INF)
		var stack: Array = [n3]
		var meshes := 0
		while not stack.is_empty():
			var c: Node = stack.pop_back()
			for k in c.get_children():
				stack.append(k)
			if c is MeshInstance3D and (c as MeshInstance3D).mesh != null:
				meshes += 1
				var mi := c as MeshInstance3D
				var aabb: AABB = mi.get_aabb()
				for i in range(8):
					var p: Vector3 = ac.to_local(mi.global_transform * aabb.get_endpoint(i))
					lo = Vector3(minf(lo.x, p.x), minf(lo.y, p.y), minf(lo.z, p.z))
					hi = Vector3(maxf(hi.x, p.x), maxf(hi.y, p.y), maxf(hi.z, p.z))
		print("probe_ac_nodes: %-16s origin %s  meshes %d  bounds %s .. %s  centre %s" % [nm, str(ac.to_local(n3.global_position)), meshes, str(lo), str(hi), str((lo + hi) * 0.5)])
	get_tree().quit(0)
