extends Node
## Probe: are the streamed terrain tiles' normals pointing up (ECEF)? Prints
## the mean dot(normal_world, local_up) per tile mesh for a few tiles.
## Run: godot --headless --path provpilot res://tests/probe_tile_normals.tscn
var _root: Node = null
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(8.0).timeout
	var meshes: Array = []
	_collect(_root.scenery3d_node, meshes)
	print("[normals] %d mesh instances under Scenery3D" % meshes.size())
	var shown := 0
	var sum_dot := 0.0
	var n_dot := 0
	for mi in meshes:
		var m: Mesh = mi.mesh
		if m == null or m.get_surface_count() == 0:
			continue
		var arrays: Array = m.surface_get_arrays(0)
		var normals = arrays[Mesh.ARRAY_NORMAL]
		var verts = arrays[Mesh.ARRAY_VERTEX]
		if normals == null or normals.is_empty():
			if shown < 3:
				print("[normals] %s: NO normals (%d verts)" % [mi.get_path(), verts.size()])
				shown += 1
			continue
		var xf: Transform3D = mi.global_transform
		var up: Vector3 = xf.origin.normalized()
		var s := 0.0
		var cnt := 0
		var step: int = maxi(1, normals.size() / 200)
		for i in range(0, normals.size(), step):
			var nw: Vector3 = (xf.basis * normals[i]).normalized()
			var p: Vector3 = xf * verts[i]
			s += nw.dot(p.normalized())
			cnt += 1
		var mean := s / maxf(cnt, 1.0)
		# Winding check: geometric normal (CCW = front face in Godot) vs the
		# stored vertex normal. Negative = the front face is the underside.
		var idx = arrays[Mesh.ARRAY_INDEX]
		var wind := 0.0
		var wcnt := 0
		if idx != null and idx.size() >= 3:
			var tstep: int = maxi(3, (idx.size() / 3 / 200) * 3)
			for k in range(0, idx.size() - 2, tstep):
				var a: Vector3 = verts[idx[k]]
				var b: Vector3 = verts[idx[k + 1]]
				var c: Vector3 = verts[idx[k + 2]]
				var gn: Vector3 = (b - a).cross(c - a)
				if gn.length_squared() > 0.0:
					wind += signf(gn.normalized().dot(normals[idx[k]]))
					wcnt += 1
		var det: float = xf.basis.determinant()
		if shown < 8:
			print("[normals] %s: winding mean sign = %.2f over %d tris, basis det = %.3f" % [mi.name, wind / maxf(wcnt, 1.0), wcnt, det])
		sum_dot += mean
		n_dot += 1
		if shown < 8:
			print("[normals] %s: mean n.up = %.3f (%d verts, %d surfaces, mat=%s)" % [mi.name, mean, verts.size(), m.get_surface_count(), str(mi.get_active_material(0))])
			shown += 1
	print("[normals] overall mean n.up = %.3f over %d meshes" % [sum_dot / maxf(n_dot, 1.0), n_dot])
	get_tree().quit(0)

func _collect(n: Node, out: Array) -> void:
	if n is MeshInstance3D:
		out.append(n)
	for c in n.get_children():
		_collect(c, out)
