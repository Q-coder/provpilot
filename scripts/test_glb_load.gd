#!/usr/bin/env godot
# Headless repro of c172p_loader._load_from_disk() for a given .glb.
# Usage: godot --headless --path provpilot -s scripts/test_glb_load.gd res://aircraft/a10/model/a10.glb
extends SceneTree

func _init():
	var path := OS.get_cmdline_user_args()[0] if OS.get_cmdline_user_args().size() else "res://aircraft/a10/model/a10.glb"
	print("=== testing ", path)
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err: int = doc.append_from_file(path, state)
	print("append_from_file err=", err)
	if err != OK:
		quit(err)
		return
	var scene := doc.generate_scene(state)
	print("scene root=", scene, " class=", scene.get_class() if scene else null)
	if scene == null:
		quit(1)
		return
	var ap := scene.find_child("AnimationPlayer", true, false) as AnimationPlayer
	print("AnimationPlayer found:", ap != null)
	if ap:
		print("  animations:", ap.get_animation_list())
		for an in ap.get_animation_list():
			print("  ", an, " length=", ap.get_animation(an).length)
	var trans := []
	var opaque_named := []
	var mi_count := 0
	for n in _walk(scene):
		if n is MeshInstance3D:
			mi_count += 1
			var mesh := (n as MeshInstance3D).mesh
			if mesh is ArrayMesh:
				for s in (mesh as ArrayMesh).get_surface_count():
					var mat := (mesh as ArrayMesh).surface_get_material(s)
					if mat == null:
						continue
					if mat.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED:
						trans.append("%s :: %s (transparency=%d)" % [n.name, mat.resource_name if mat.resource_name != "" else "(unnamed)", mat.transparency])
	print("MeshInstance3D count:", mi_count)
	print("transparent materials on meshes:")
	for t in trans:
		print("  ", t)
	quit(0)

func _walk(n: Node) -> Array:
	var out := [n]
	for c in n.get_children():
		out.append_array(_walk(c))
	return out