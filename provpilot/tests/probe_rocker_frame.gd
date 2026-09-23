extends Node
## Where is a rocker in the model-root frame, and what does the manifest say?
const E := 2682767.0
const N := 1258797.0
func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(2.0).timeout
	var mroot: Node3D = root.get_node(NodePath(root._model_root_path))
	print("  model root: ", mroot.get_path(), " children: ", mroot.get_children().map(func(c): return c.name))
	for c in mroot.get_children():
		if c is Node3D:
			print("  child ", c.name, " transform ", (c as Node3D).transform)
	var inv: Transform3D = mroot.global_transform.affine_inverse()
	for nm in ["Switch_TaxiLt", "Yoke_L", "altimeter__needle"]:
		var n: Node3D = mroot.find_child(nm, true, false)
		if n:
			var origin: Vector3 = inv * n.global_position
			var centre: Vector3 = inv * (n.global_transform * (n as MeshInstance3D).get_aabb().get_center()) if n is MeshInstance3D else origin
			print("  %s: origin in model root frame %s, mesh centre %s, local transform %s" % [nm, str(origin), str(centre), str(n.transform)])
	var m: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(ProjectSettings.globalize_path("res://aircraft/c172p/model/animations.json")))
	for a in m["animations"]:
		if a["model"] == "panel" and a["type"] == "rotate" and a["objects"][0] == "Switch_TaxiLt":
			print("  manifest centre for Switch_TaxiLt: ", a["raw"]["center"], " axis ", a["raw"]["axis"])
	var sw: Node3D = mroot.find_child("Switch_TaxiLt", true, false)
	var fga: FGAnimator = root._fg_animator
	# rest pose: clear the animator (restores base transforms), measure, reload
	fga.clear()
	await get_tree().process_frame
	var base: Transform3D = sw.transform
	var P := Transform3D()
	var q: Node = sw.get_parent()
	while q != null and q != mroot:
		if q is Node3D:
			P = (q as Node3D).transform * P
		q = q.get_parent()
	print("  rest: origin(model) %s | P origin %s scale %s | base origin %s" % [str(inv * sw.global_position), str(P.origin), str(P.basis.get_scale()), str(base.origin)])
	print("  rest check: (P*base).origin %s vs inv*global %s; mroot*P*local vs global equal: %s" % [str((P * base).origin), str(inv * sw.global_position), str((mroot.global_transform * P * base).is_equal_approx(sw.global_transform))])
	q = sw.get_parent()
	while q != null and q != mroot:
		if q is Node3D:
			print("    parent %s: local origin %s scale %s top_level %s" % [q.name, str((q as Node3D).transform.origin), str((q as Node3D).transform.basis.get_scale()), str((q as Node3D).top_level)])
		else:
			print("    parent %s: not a Node3D (%s)" % [q.name, q.get_class()])
		q = q.get_parent()
	root._setup_fg_animations()
	await get_tree().create_timer(0.3).timeout
	var m0: Transform3D = fga.animated_transform("Switch_TaxiLt")
	var expected: Transform3D = P.affine_inverse() * m0 * P * base
	print("  formula: expected origin(model) %s vs actual %s" % [str((P * expected).origin), str(inv * sw.global_position)])
	print("  actual local %s\n  expected local %s" % [str(sw.transform), str(expected)])
	for v in [0.0, 1.0]:
		root._fg_controls.state["controls/lighting/taxi-light"] = v
		await get_tree().create_timer(0.3).timeout
		var o: Vector3 = inv * sw.global_position
		var bz: Vector3 = (inv * sw.global_transform).basis.y
		print("  taxi=%d: origin(model) %s, up axis %s, applied m: %s" % [int(v), str(o), str(bz), str(fga.animated_transform("Switch_TaxiLt"))])
	get_tree().quit(0)
