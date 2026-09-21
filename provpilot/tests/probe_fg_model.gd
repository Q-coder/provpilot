extends Node
## Subtask 1 check (docs/A10_FLIGHTGEAR_PORT.md): load the FlightGear-built
## a10_fg.glb the way the aircraft loader does, and verify that every node
## the animation manifest targets exists, plus the span. Headless.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_fg_model.tscn
func _ready() -> void:
	var dir: String = ProjectSettings.globalize_path("res://aircraft/a10/model")
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err: int = doc.append_from_file(dir.path_join("a10_fg.glb"), state)
	if err != OK:
		printerr("probe_fg_model: glb load failed %d" % err); get_tree().quit(1); return
	var scene: Node = doc.generate_scene(state)
	add_child(scene)
	var names: Dictionary = {}
	var meshes := 0
	var stack: Array = [scene]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		names[n.name] = n
		if n is MeshInstance3D:
			meshes += 1
		for c in n.get_children():
			stack.append(c)
	var f := FileAccess.open(dir.path_join("animations.json"), FileAccess.READ)
	var m: Dictionary = JSON.parse_string(f.get_as_text())
	var wanted: Dictionary = {}
	var unresolved: Dictionary = {}
	for u in m.get("unresolved_targets", []):
		unresolved[u] = true
	for a in (m["animations"] as Array) + (m["controls"] as Array):
		for o in a["objects"]:
			if not unresolved.has(o):
				wanted[o] = true
	print("probe_fg_model: %d targets the build could not resolve (absent from the AC3D sources): %s" % [unresolved.size(), unresolved.keys().slice(0, 6)])
	var missing: Array = []
	for o in wanted.keys():
		if not names.has(o):
			# Blender duplicate suffix -> "_001" after Godot sanitising
			var found := false
			for k in names.keys():
				if str(k).begins_with(str(o) + "_") and str(k).length() == str(o).length() + 4:
					found = true; break
			if not found:
				missing.append(o)
	missing.sort()
	var l: Node3D = names.get("L-NavLight")
	var r: Node3D = names.get("R-NavLight")
	var span := -1.0
	if l and r and l is MeshInstance3D and r is MeshInstance3D:
		var lc: Vector3 = l.global_transform * (l as MeshInstance3D).get_aabb().get_center()
		var rc: Vector3 = r.global_transform * (r as MeshInstance3D).get_aabb().get_center()
		span = lc.distance_to(rc)
		print("probe_fg_model: L-NavLight at %s (glTF frame: x aft, y up, z left)" % str(lc))
	print("probe_fg_model: %d nodes, %d meshes, %d animated targets, %d missing, span %.2f m" % [names.size(), meshes, wanted.size(), missing.size(), span])
	if not missing.is_empty():
		print("probe_fg_model: missing targets: ", missing.slice(0, 40))
	for k in ["A10_Root", "Sub_cockpit", "Sub_A-10-asi", "A-10-asi__needle", "UC-NoseStrutTop", "LRudder-L", "Sub_stick", "stick__stick"]:
		print("  %-22s %s" % [k, "ok" if names.has(k) else "MISSING"])
	get_tree().quit(0 if missing.is_empty() and span > 17.0 and span < 18.5 else 1)
