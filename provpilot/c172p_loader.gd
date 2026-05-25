extends Node3D
## C172PLoader — Runtime loader for the Cessna 172 3D model.
##
## The model used to be a `res://aircraft/c172p/model/C172P_1.blend` instance
## baked into `C172p.tscn` at editor time. We now load the GLB from disk so
## the bundle stays editable post-install (X-Plane style):
##
##     <install_root>/aircraft/c172p/model/C172P_1.glb
##
## `AircraftInstaller` extracts the .glb from the PCK on first run; this
## script then parses it via `GLTFDocument.append_from_file()` and reparents
## the resulting subtree under this node (named `C172P_1` in `C172p.tscn`),
## preserving the GLTF scene graph so the hardcoded paths in `node_3d.gd`
## (rudder, propeller, ailerons, …) keep resolving.
##
## NOTE: The previous `C172p.tscn` carried two inline transform overrides on
## the body and rudder nodes. Those values were authored against the
## `.blend` importer's local coordinate frame (which differs from the raw
## GLTF frame — e.g. the rudder override has a translation magnitude of
## ~500 units). The GLB already exposes the correct base poses at runtime,
## and `node_3d.gd` snapshots each control-surface base transform on startup
## before animating, so no override is needed here.

const MODEL_REL_PATH := "c172p/model/C172P_1.glb"


func _ready() -> void:
	# Child _ready() runs before parent _ready() in Godot, so node_3d.gd can
	# rely on the GLB subtree being attached by the time it resolves paths.
	var ir := get_node_or_null("/root/InstallRoot")
	if ir == null:
		push_error("C172PLoader: InstallRoot autoload missing")
		return
	var path: String = ir.get_aircraft_root().path_join(MODEL_REL_PATH)
	if not FileAccess.file_exists(path):
		push_error("C172PLoader: model not found at %s — was AircraftInstaller run?" % path)
		return

	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err: int = doc.append_from_file(path, state)
	if err != OK:
		push_error("C172PLoader: append_from_file failed (err=%d) for %s" % [err, path])
		return
	var scene := doc.generate_scene(state)
	if scene == null:
		push_error("C172PLoader: generate_scene returned null for %s" % path)
		return

	# Reparent the GLB's top-level child(ren) onto this node, then drop the
	# now-empty wrapper. This makes our node behave exactly like the previous
	# `instance=ExtResource(.blend)` PackedScene-instance did: our name
	# `C172P_1` becomes the parent of `Sketchfab_model`.
	for c in scene.get_children():
		scene.remove_child(c)
		add_child(c)
	scene.queue_free()

	print("C172PLoader: loaded model from %s" % path)
