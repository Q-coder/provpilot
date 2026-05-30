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

# Default aircraft id whose aircraft.cfg drives model loading at startup.
const AIRCRAFT_ID := "c172p"
const AircraftConfig = preload("res://aircraft_config.gd")

# Default model location for the Cessna (relative to the aircraft dir), used if
# aircraft.cfg is missing or omits [model] file. Overridden by cfg
# model.file / model.res_file.
const DEFAULT_MODEL_REL_PATH := "model/C172P_1.glb"
const DEFAULT_MODEL_RES_PATH := "res://aircraft/c172p/model/C172P_1.glb"

# Currently-loaded aircraft id (so a swap to the same model is a no-op).
var _loaded_aircraft_id := ""


func _ready() -> void:
	# Child _ready() runs before parent _ready() in Godot, so node_3d.gd can
	# rely on the GLB subtree being attached by the time it resolves paths.
	load_aircraft_model(AIRCRAFT_ID)


# Public: (re)load the 3D model for the given aircraft id, replacing whatever
# subtree is currently attached. Returns true on success. Called once at
# startup with the default aircraft and again by node_3d.gd when the welcome
# screen selects a different aircraft.
func load_aircraft_model(aircraft_id: String) -> bool:
	if aircraft_id.is_empty():
		aircraft_id = AIRCRAFT_ID
	if aircraft_id == _loaded_aircraft_id:
		return true

	_aircraft_id = aircraft_id
	# Generic per-aircraft defaults; cfg may override below.
	_model_rel_path = "model/%s.glb" % aircraft_id
	_model_res_path = "res://aircraft/%s/model/%s.glb" % [aircraft_id, aircraft_id]
	if aircraft_id == "c172p":
		_model_rel_path = DEFAULT_MODEL_REL_PATH
		_model_res_path = DEFAULT_MODEL_RES_PATH
	_load_config()

	var scene: Node = _load_from_disk()
	if scene == null:
		scene = _load_from_resource()
	if scene == null:
		push_error("C172PLoader: failed to load model for '%s' (no on-disk .glb and no PCK fallback)" % aircraft_id)
		return false

	# Drop any previously-attached model subtree before installing the new one.
	for old in get_children():
		remove_child(old)
		old.free()

	# Reparent the GLB's top-level child(ren) onto this node, then drop the
	# now-empty wrapper. This makes our node behave exactly like the previous
	# `instance=ExtResource(.blend)` PackedScene-instance did: our name
	# `C172P_1` becomes the parent of `Sketchfab_model`.
	var model_xform := _model_transform()
	for c in scene.get_children():
		scene.remove_child(c)
		add_child(c)
		# Apply optional per-aircraft scale/offset/rotation so a model authored
		# at a different native scale can be fitted without code changes. Defaults
		# to identity (the Cessna is unaffected).
		if c is Node3D and model_xform != Transform3D.IDENTITY:
			c.transform = model_xform * c.transform
	scene.queue_free()
	_loaded_aircraft_id = aircraft_id
	return true


# Builds the optional model fitting transform from aircraft.cfg [model]:
#   scale       uniform float  (default 1.0)
#   scale_xyz   "x, y, z"       (overrides uniform scale if present)
#   offset      "x, y, z"       local translation, applied after scale
#   rotation_deg "x, y, z"      local euler rotation (degrees)
func _model_transform() -> Transform3D:
	if _cfg == null or not _cfg.loaded:
		return Transform3D.IDENTITY
	var basis := Basis.IDENTITY
	var rot: Array = _cfg.get_float_array("model", "rotation_deg", [])
	if rot.size() == 3:
		basis = Basis.from_euler(Vector3(deg_to_rad(rot[0]), deg_to_rad(rot[1]), deg_to_rad(rot[2])))
	var scale_xyz: Array = _cfg.get_float_array("model", "scale_xyz", [])
	if scale_xyz.size() == 3:
		basis = basis.scaled(Vector3(scale_xyz[0], scale_xyz[1], scale_xyz[2]))
	else:
		var s: float = _cfg.get_float("model", "scale", 1.0)
		if s != 1.0:
			basis = basis.scaled(Vector3(s, s, s))
	var origin := Vector3.ZERO
	var off: Array = _cfg.get_float_array("model", "offset", [])
	if off.size() == 3:
		origin = Vector3(off[0], off[1], off[2])
	return Transform3D(basis, origin)


# Resolved model locations (from aircraft.cfg, with hardcoded fallbacks).
var _aircraft_id := AIRCRAFT_ID
var _model_rel_path := DEFAULT_MODEL_REL_PATH
var _model_res_path := DEFAULT_MODEL_RES_PATH
# Parsed aircraft.cfg for the current aircraft (for the model fitting transform).
var _cfg = null


# Reads [model] file / res_file from <aircraft_dir>/aircraft.cfg so the model
# location is data-driven. Missing file/keys keep the defaults above.
func _load_config() -> void:
	var cfg_path := ""
	# In the editor, prefer the res:// source so live aircraft.cfg edits apply
	# on relaunch (install_root only re-extracts on a BUNDLE_VERSION bump).
	if OS.has_feature("editor"):
		var res_path := "res://aircraft/%s/aircraft.cfg" % _aircraft_id
		if FileAccess.file_exists(res_path):
			cfg_path = res_path
	if cfg_path.is_empty():
		var ir := get_node_or_null("/root/InstallRoot")
		if ir:
			cfg_path = String(ir.get_aircraft_dir(_aircraft_id)).path_join("aircraft.cfg")
		if cfg_path.is_empty() or not FileAccess.file_exists(cfg_path):
			cfg_path = "res://aircraft/%s/aircraft.cfg" % _aircraft_id
	var cfg := AircraftConfig.load_from_file(cfg_path)
	if not cfg.loaded:
		return
	_cfg = cfg
	_model_rel_path = cfg.get_string("model", "file", _model_rel_path)
	_model_res_path = cfg.get_string("model", "res_file", _model_res_path)


# Preferred path: load a real .glb from <install_root>/aircraft/... so users
# can swap the model file without rebuilding the PCK. Returns null if the file
# isn't there or fails to parse.
func _load_from_disk() -> Node:
	var ir := get_node_or_null("/root/InstallRoot")
	if ir == null:
		return null
	var path: String = ir.get_aircraft_dir(_aircraft_id).path_join(_model_rel_path)
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
	if not ResourceLoader.exists(_model_res_path):
		push_error("C172PLoader: resource missing: %s" % _model_res_path)
		return null
	var ps: PackedScene = load(_model_res_path) as PackedScene
	if ps == null:
		push_error("C172PLoader: load(%s) returned non-PackedScene" % _model_res_path)
		return null
	var inst := ps.instantiate()
	if inst == null:
		push_error("C172PLoader: instantiate() returned null for %s" % _model_res_path)
		return null
	print("C172PLoader: loaded model from PCK resource: %s" % _model_res_path)
	return inst
