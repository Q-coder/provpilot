extends Node
## Where the visual model's wheels sit along the fuselage compared with the
## FDM's gear contact points (both measured from the FDM's CG, the point the
## aircraft node is placed at and pitches about). A model drawn too far
## forward or aft sinks its wheels or tail into the ground when the nose
## comes up. Feeds aircraft.cfg [model] offset (3rd value, along the nose).
## Run: godot --headless --path provpilot res://tests/probe_gear_align.tscn [-- ids...]
const E := 2683732.0
const N := 1257695.0

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(4.0).timeout
	var args := OS.get_cmdline_user_args()
	var ids: Array = args if args.size() > 0 else ["c172p", "pc7", "a10", "md80", "ask21"]
	for id in ids:
		if root._settings:
			root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
			root._settings.set_setting("aircraft_%s_start_speed_kt" % id, 0.0)
		root._on_welcome_flight_requested(id, E, N, 160.0)
		var waited := 0.0
		while not root.terrain_initialized and waited < 60.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		await get_tree().create_timer(10.0).timeout
		var ac: Node3D = root.get_node("AC")
		var xf: Transform3D = ac.global_transform
		var fwd: Vector3 = xf.basis.z.normalized()
		var up: Vector3 = xf.basis.y.normalized()
		var left: Vector3 = xf.basis.x.normalized()
		print("probe_align: == %s (node %.2f m ahead of the CG)" % [id, root.jsb_node.get_origin_forward_m()])
		for c in root.jsb_node.get_gear_contacts():
			print("probe_align:   FDM %-5s %-14s forward %+7.2f m  right %+6.2f  down %+6.2f  wow %s" % [
				"BOGEY" if c["bogey"] else "STRUC", c["name"], c["forward_m"], c["right_m"], c["down_m"], str(c["wow"])])
		# visual meshes: wheels, and the extremes of the whole airframe
		var lo_f := INF
		var hi_f := -INF
		var stack: Array = [ac]
		while not stack.is_empty():
			var n: Node = stack.pop_back()
			for ch in n.get_children():
				stack.append(ch)
			if n is MeshInstance3D and (n as MeshInstance3D).mesh != null and n.is_visible_in_tree():
				var mi := n as MeshInstance3D
				var aabb: AABB = mi.get_aabb()
				var c: Vector3 = mi.global_transform * aabb.get_center() - xf.origin
				for i in range(8):
					var w: Vector3 = mi.global_transform * aabb.get_endpoint(i) - xf.origin
					lo_f = minf(lo_f, w.dot(fwd))
					hi_f = maxf(hi_f, w.dot(fwd))
				var nm := n.name.to_lower()
				if "tyre" in nm or "tire" in nm or "wheel" in nm or "whell" in nm:
					var size: Vector3 = aabb.size * mi.global_transform.basis.get_scale()
					if size.length() < 3.0:
						print("probe_align:   MODEL %-34s forward %+7.2f m  right %+6.2f  down %+6.2f" % [n.name, c.dot(fwd), -c.dot(left), -c.dot(up)])
		# tail strike: the airframe vertex aft of the main wheels that meets the ground at the
		# smallest nose-up angle about the main wheel contact (wheels and gear legs left out)
		var main_fwd := 0.0
		var ground_down := -INF
		# heights from the scene, not from the FDM: the node is not at the CG's height
		var ng: Vector3 = W3DCoords.ecef_to_geodetic(xf.origin)
		var node_agl: float = ng.z - root._world.ground_height_geodetic(ng.x, ng.y)
		var gup: Vector3 = W3DCoords.geodetic_normal(ng.x, ng.y)
		var h_agl: float = root.jsb_node.get_property("position/h-agl-ft") * 0.3048
		for c2 in root.jsb_node.get_gear_contacts():
			if c2["bogey"] and c2["wow"]:
				ground_down = maxf(ground_down, float(c2["down_m"]) - float(c2["compression_m"]))
			if c2["bogey"] and ("MAIN" in str(c2["name"]).to_upper() or "MLG" in str(c2["name"]).to_upper()):
				main_fwd = float(c2["forward_m"])
		main_fwd -= root.jsb_node.get_origin_forward_m()   # contacts are from the CG, vertices from the node
		var best_ang := INF
		var best := Vector3.ZERO
		var best_name := ""
		stack = [ac]
		while not stack.is_empty():
			var n2: Node = stack.pop_back()
			for ch in n2.get_children():
				stack.append(ch)
			if not (n2 is MeshInstance3D) or (n2 as MeshInstance3D).mesh == null or not n2.is_visible_in_tree():
				continue
			var nm2 := n2.name.to_lower()
			if "tyre" in nm2 or "tire" in nm2 or "wheel" in nm2 or "whell" in nm2 or "gear" in nm2 or nm2.begins_with("uc-") or nm2.begins_with("lg") or "door" in nm2 or "shadow" in nm2:
				continue
			var mi2 := n2 as MeshInstance3D
			for si in range(mi2.mesh.get_surface_count()):
				var verts: PackedVector3Array = mi2.mesh.surface_get_arrays(si)[Mesh.ARRAY_VERTEX]
				for v in verts:
					var w2: Vector3 = mi2.global_transform * v - xf.origin
					var aft: float = main_fwd - w2.dot(fwd)
					if aft < 1.0:
						continue
					var height: float = node_agl + w2.dot(gup)
					var ang: float = rad_to_deg(atan2(height, aft))
					if ang < best_ang:
						best_ang = ang
						best = Vector3(w2.dot(fwd), -w2.dot(left), height)
						best_name = n2.name
		var cg: Dictionary = root.jsb_node.get_cg_info()
		var fwd_cg: float = best.x + root.jsb_node.get_origin_forward_m()
		var down_cg: float = h_agl - best.z
		print("probe_align:   TAIL STRIKE at %.1f deg nose up: %s, %.2f m aft of the main wheels, %.2f m above the ground" % [best_ang, best_name, main_fwd - best.x, best.z])
		print("probe_align:   -> FDM contact: %.2f m forward, %.2f right, %.2f down of the CG = struct x %.1f in, y %.1f, z %.1f (CG %.1f / %.1f, h-agl %.2f m, node %.2f m above ground)" % [
			fwd_cg, best.y, down_cg, float(cg.get("cg_x_in", 0.0)) - fwd_cg / 0.0254, best.y / 0.0254, float(cg.get("cg_z_in", 0.0)) - down_cg / 0.0254,
			float(cg.get("cg_x_in", 0.0)), float(cg.get("cg_z_in", 0.0)), h_agl, node_agl])
		print("probe_align:   MODEL airframe from %+.2f m (tail) to %+.2f m (nose) of the CG" % [lo_f, hi_f])
	get_tree().quit(0)
