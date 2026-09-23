extends Node
## Propeller blur disc of the C172: is it created, where, and does it show at high rpm?
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
	await get_tree().create_timer(3.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var mroot: Node3D = root.get_node(NodePath(root._model_root_path))
	var prop: Node3D = mroot.find_child("Cessna_Exterior_Body_MAT_0_001_Body_MAT_0", true, false)
	print("  prop node ", prop, " parent ", prop.get_parent() if prop else null)
	var jsb = root.jsb_node
	jsb.set_input_throttle(1.0)
	jsb.set_input_brake(1.0)
	await get_tree().create_timer(3.0).timeout
	print("  rpm %.0f, prop visible %s, prop mesh %s, prop mat %s" % [jsb.get_property("propulsion/engine[0]/engine-rpm"), str(prop.visible), str((prop as MeshInstance3D).mesh), str((prop as MeshInstance3D).get_active_material(0))])
	for c in prop.get_parent().get_children():
		print("  sibling ", c.name, " ", c.get_class(), " visible ", (c as Node3D).visible if c is Node3D else "-")
	if prop:
		for c in prop.get_parent().get_children():
			if c is MeshInstance3D and c.mesh is CylinderMesh:
				var inv: Transform3D = mroot.global_transform.affine_inverse()
				var m = c.material_override
				print("  disc: visible %s, pos(model) %s, radius %.2f, mat alpha %.2f transparency %d shading %d, prop pos(model) %s" % [str(c.visible), str(inv * c.global_position), (c.mesh as CylinderMesh).top_radius, m.albedo_color.a if m else -1.0, m.transparency if m else -1, m.shading_mode if m else -1, str(inv * (prop.global_transform * (prop as MeshInstance3D).get_aabb().get_center()))])
	print("  RESULT %s" % ("OK: blades hidden, disc shown" if (not prop.visible and jsb.get_property("propulsion/engine[0]/engine-rpm") > 1200.0) else "blades still visible"))
	get_tree().quit(0)
