extends Node
## Parks each aircraft at LSZH with the FDM running and compares the visual
## model's lowest wheel point (AC-local Y, model offset applied) with
## JSBSim's CG height above ground: mismatch = wheel_min_y + h_agl.
## Positive = the model floats, negative = it sinks. Feeds aircraft.cfg
## [model] offset (subtract the mismatch from its Y).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_gear_height.tscn
const E := 2683732.0
const N := 1257695.0
var _root: Node = null

## Ground clearance of the visual model in WORLD terms: for every visible
## mesh, the AABB corner with the lowest geodetic altitude minus the ground
## height (scenery + drawn-surface lift) under it. Returns
## [min over all meshes, min over wheel/tyre meshes].
func _clearance(ac: Node3D) -> Array:
	var min_all := INF
	var min_wheel := INF
	var stack: Array = [ac]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.append(c)
		if n is MeshInstance3D and (n as MeshInstance3D).mesh != null and n.is_visible_in_tree():
			var mi := n as MeshInstance3D
			var aabb: AABB = mi.get_aabb()
			var lo := INF
			for i in range(8):
				var w: Vector3 = mi.global_transform * aabb.get_endpoint(i)
				var g: Vector3 = W3DCoords.ecef_to_geodetic(w)
				var ground: float = _root._ground_height_geodetic(g.x, g.y)
				if is_nan(ground):
					continue
				lo = minf(lo, g.z - ground)
			min_all = minf(min_all, lo)
			var nm := n.name.to_lower()
			if "tyre" in nm or "tire" in nm or "wheel" in nm or "whell" in nm:
				min_wheel = minf(min_wheel, lo)
	return [min_all, min_wheel]

func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	for id in ["c172p", "pc7", "a10"]:
		_root._on_welcome_flight_requested(id, E, N, 160.0)
		var waited := 0.0
		while not _root.terrain_initialized and waited < 60.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		await get_tree().create_timer(10.0).timeout
		var jsb = _root.jsb_node
		var ac: Node3D = _root.get_node("AC")
		var agl_m: float = jsb.get_property("position/h-agl-ft") * 0.3048
		var wow: float = jsb.get_property("gear/wow")
		var r := _clearance(ac)
		# the lowest meshes by name (finds parts posed wrongly by an animation)
		var lows: Array = []
		var stack: Array = [ac]
		while not stack.is_empty():
			var n: Node = stack.pop_back()
			for c in n.get_children():
				stack.append(c)
			if n is MeshInstance3D and (n as MeshInstance3D).mesh != null and n.is_visible_in_tree():
				var mi := n as MeshInstance3D
				var aabb: AABB = mi.get_aabb()
				var lo := INF
				for i in range(8):
					var w: Vector3 = mi.global_transform * aabb.get_endpoint(i)
					var g: Vector3 = W3DCoords.ecef_to_geodetic(w)
					var ground: float = _root._ground_height_geodetic(g.x, g.y)
					if not is_nan(ground):
						lo = minf(lo, g.z - ground)
				lows.append([lo, n.name])
		lows.sort()
		print("probe_gear: %s lowest meshes: %s" % [id, str(lows.slice(0, 8))])
		print("probe_gear: %-6s h-agl %.3f m  wow %.0f  clearance: lowest mesh %+.3f m, lowest wheel/tyre mesh %+.3f m (positive = floats)" % [
			id, agl_m, wow, r[0], r[1]])
	get_tree().quit(0)
