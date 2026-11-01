extends Node
## Probe: positions of a few MD-80 nodes relative to the model root (root units),
## to place lights given in FlightGear's main-model frame.
func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	var mroot: Node3D = root.get_node(NodePath(root._model_root_path))
	var inv: Transform3D = mroot.global_transform.affine_inverse()
	for nm in ["MD-82_Root", "Sub_MD-82_own", "Sub_MD-80-cockpit", "MD-82__LGNStrutUp", "fgcp__Digits_Freq1", "Sub_panel_lighting_l", "Sub_pedestal_lighting", "MD-82__LGNTires"]:
		var n: Node3D = mroot.find_child(nm, true, false) as Node3D
		if n:
			var p: Vector3 = inv * n.global_position
			print("  [frame] %-24s root-local %s  (scale %s)" % [nm, str(p.snapped(Vector3(0.01, 0.01, 0.01))), str(n.global_transform.basis.get_scale().snapped(Vector3(0.01,0.01,0.01)))])
		else:
			print("  [frame] %-24s not found" % nm)
	print("  [frame] model root scale %s" % str(mroot.global_transform.basis.get_scale()))
	for sub in ["Sub_MD-82_own", "Sub_MD-80-cockpit"]:
		var s: Node3D = mroot.find_child(sub, true, false) as Node3D
		if s == null:
			continue
		var lo := Vector3(1e9, 1e9, 1e9)
		var hi := Vector3(-1e9, -1e9, -1e9)
		for m in s.find_children("*", "MeshInstance3D", true, false):
			if sub == "Sub_MD-82_own" and (m as Node).is_ancestor_of(s) == false and mroot.find_child("Sub_MD-80-cockpit", true, false).is_ancestor_of(m):
				continue
			var aabb: AABB = (m as MeshInstance3D).get_aabb()
			var xf: Transform3D = inv * (m as Node3D).global_transform
			for i in range(8):
				var c: Vector3 = xf * aabb.get_endpoint(i)
				lo = lo.min(c)
				hi = hi.max(c)
		print("  [frame] %-20s mesh extent root-local min %s max %s" % [sub, str(lo.snapped(Vector3(0.1,0.1,0.1))), str(hi.snapped(Vector3(0.1,0.1,0.1)))])
	get_tree().quit(0)
