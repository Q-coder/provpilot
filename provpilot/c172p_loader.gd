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

# Fallback to the resource embedded in the PCK (Godot imports the .glb into a
# PackedScene at editor time; the .import sidecar remaps res://…/C172P_1.glb
# to res://.godot/imported/C172P_1.glb-<hash>.scn). The source .glb itself is
# *not* present in exported builds — the include_filter only keeps it for
# direct FileAccess in the editor — so we must rely on the imported PackedScene
# at runtime unless the user has dropped a real .glb on disk.
const MODEL_RES_PATH := "res://aircraft/c172p/model/C172P_1.glb"


func _ready() -> void:
	# Child _ready() runs before parent _ready() in Godot, so node_3d.gd can
	# rely on the GLB subtree being attached by the time it resolves paths.
	var scene: Node = _load_from_disk()
	if scene == null:
		scene = _load_from_resource()
	if scene == null:
		push_error("C172PLoader: failed to load model (no on-disk .glb and no PCK fallback)")
		return

	# Reparent the GLB's top-level child(ren) onto this node, then drop the
	# now-empty wrapper. This makes our node behave exactly like the previous
	# `instance=ExtResource(.blend)` PackedScene-instance did: our name
	# `C172P_1` becomes the parent of `Sketchfab_model`.
	for c in scene.get_children():
		scene.remove_child(c)
		add_child(c)
	scene.queue_free()


# Preferred path: load a real .glb from <install_root>/aircraft/... so users
# can swap the model file without rebuilding the PCK. Returns null if the file
# isn't there or fails to parse.
func _load_from_disk() -> Node:
	var ir := get_node_or_null("/root/InstallRoot")
	if ir == null:
		return null
	var path: String = ir.get_aircraft_root().path_join(MODEL_REL_PATH)
	if not FileAccess.file_exists(path):
		return null

	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err: int = doc.append_from_file(path, state)
	if err != OK:
		push_warning("C172PLoader: append_from_file failed (err=%d) for %s" % [err, path])
		return null
	var scene := doc.generate_scene(state)
	if scene == null:
		push_warning("C172PLoader: generate_scene returned null for %s" % path)
		return null
	print("C172PLoader: loaded model from disk: %s" % path)
	return scene


# Fallback: instantiate the PackedScene Godot built when importing the .glb.
# Works in both the editor and exported builds.
func _load_from_resource() -> Node:
	if not ResourceLoader.exists(MODEL_RES_PATH):
		push_error("C172PLoader: resource missing: %s" % MODEL_RES_PATH)
		return null
	var ps: PackedScene = load(MODEL_RES_PATH) as PackedScene
	if ps == null:
		push_error("C172PLoader: load(%s) returned non-PackedScene" % MODEL_RES_PATH)
		return null
	var inst := ps.instantiate()
	if inst == null:
		push_error("C172PLoader: instantiate() returned null for %s" % MODEL_RES_PATH)
		return null
	print("C172PLoader: loaded model from PCK resource: %s" % MODEL_RES_PATH)
	return inst
