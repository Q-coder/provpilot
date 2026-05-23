extends S3DVegetationManager
##
## Provpilot vegetation setup.
## Loads the tree mesh from a GLB scene file (set via tree_scene_path) and
## applies foliage-safe material settings (two-sided alpha, no shadow receive).
## Falls back to a unit cone mesh if no path is configured or loading fails.
##

@export_file("*.tscn", "*.glb", "*.gltf") var tree_scene_path: String = ""


func _ready() -> void:
	if tree_mesh == null:
		if not tree_scene_path.is_empty():
			var loaded := _load_mesh_from_scene_path(tree_scene_path)
			if loaded != null:
				tree_mesh = loaded
				_apply_foliage_materials(tree_mesh)
				return
		push_warning("Vegetation: could not load mesh from '%s', using cone fallback." % tree_scene_path)
		tree_mesh = _make_placeholder_cone()
	else:
		_apply_foliage_materials(tree_mesh)


func _load_mesh_from_scene_path(path: String) -> Mesh:
	# Accept both res:// paths and absolute paths.
	# For absolute paths, try res://<filename> first (symlink inside project).
	var load_path := path
	if path.begins_with("/"):
		load_path = "res://" + path.get_file()

	var resource = load(load_path)
	if resource == null:
		push_error("Vegetation: failed to load resource: %s (tried: %s)" % [path, load_path])
		return null
	if resource is PackedScene:
		var root := (resource as PackedScene).instantiate()
		var found := _find_first_mesh_recursive(root)
		root.free()
		return found
	elif resource is Mesh:
		return resource as Mesh
	push_error("Vegetation: resource %s is neither a scene nor a mesh" % path)
	return null


func _find_first_mesh_recursive(node: Node) -> Mesh:
	if node is MeshInstance3D:
		var mi := node as MeshInstance3D
		if mi.mesh != null:
			return mi.mesh
	for child in node.get_children():
		var m := _find_first_mesh_recursive(child)
		if m != null:
			return m
	return null


func _apply_foliage_materials(mesh: Mesh) -> void:
	if mesh == null:
		return
	for i in range(mesh.get_surface_count()):
		var mat = mesh.surface_get_material(i)
		if mat is StandardMaterial3D:
			var sm := (mat as StandardMaterial3D).duplicate()
			# Foliage should not receive terrain shadows (looks black over orthophoto).
			sm.set_flag(BaseMaterial3D.FLAG_DONT_RECEIVE_SHADOWS, true)
			if sm.albedo_texture != null:
				# Textured surface (bark, leaf cards with PNG) — keep colours.
				# Enable two-sided rendering if the surface uses alpha (leaf cards).
				if sm.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED:
					sm.cull_mode = BaseMaterial3D.CULL_DISABLED
			else:
				# Untextured (procedural leaf, etc.) — tint green so it doesn't render white.
				sm.albedo_color = Color(0.22, 0.42, 0.18, 1.0)
				sm.cull_mode = BaseMaterial3D.CULL_DISABLED
				sm.roughness = clamp(sm.roughness, 0.7, 1.0)
				sm.metallic = 0.0
			mesh.surface_set_material(i, sm)
		elif mat == null:
			var fallback := StandardMaterial3D.new()
			fallback.albedo_color = Color(0.22, 0.36, 0.18, 1.0)
			fallback.cull_mode = BaseMaterial3D.CULL_DISABLED
			fallback.roughness = 0.85
			fallback.set_flag(BaseMaterial3D.FLAG_DONT_RECEIVE_SHADOWS, true)
			mesh.surface_set_material(i, fallback)


func _make_placeholder_cone() -> Mesh:
	var arr_mesh := ArrayMesh.new()
	const SEG := 8
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var uvs   := PackedVector2Array()
	var idx   := PackedInt32Array()

	# Cone: base at y=0, tip at y=1.
	var tip := Vector3(0, 1, 0)
	for s in range(SEG):
		var a0 := s * TAU / SEG
		var a1 := (s + 1) * TAU / SEG
		var b0 := Vector3(cos(a0) * 0.3, 0, sin(a0) * 0.3)
		var b1 := Vector3(cos(a1) * 0.3, 0, sin(a1) * 0.3)
		var base := verts.size()
		verts.append_array([b0, b1, tip])
		var n := (b0 + b1).normalized()
		norms.append_array([n, n, Vector3.UP])
		uvs.append_array([Vector2(float(s) / SEG, 0), Vector2(float(s + 1) / SEG, 0), Vector2((s + 0.5) / SEG, 1)])
		idx.append_array([base, base + 1, base + 2])

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX]  = idx
	arr_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.22, 0.36, 0.18, 1.0)
	mat.roughness = 0.85
	mat.set_flag(BaseMaterial3D.FLAG_DONT_RECEIVE_SHADOWS, true)
	arr_mesh.surface_set_material(0, mat)
	return arr_mesh
