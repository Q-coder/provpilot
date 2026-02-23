extends Node3D
## WaterLoader — Manifest-driven dynamic water body tile loading
##
## Loads/unloads water GLB meshes (lakes, rivers, streams) based on
## aircraft proximity.  Uses a translucent blue shader with per-vertex
## colour from the GLB.

class_name WaterLoader

# ── Configuration (exports) ────────────────────────────────────────────

## Path to the water directory (contains manifest.json and GLB files).
@export var water_directory: String = "scenery/water"

## Godot res:// prefix for GLB loading
@export var water_res_prefix: String = "res://Assets/water"

## The node to track for position (usually the aircraft)
@export var aircraft_node: Node3D

## Distance (in meters) at which to load water tiles
@export var load_distance: float = 10000.0

## Distance (in meters) at which to unload water tiles
@export var unload_distance: float = 12000.0

## Maximum tiles to load per frame
@export var max_loads_per_frame: int = 2

## Enable debug output
@export var debug_mode: bool = false

## Enable/disable water loading
@export var enabled: bool = true


# ── Internal State ─────────────────────────────────────────────────────

var _manifest: Dictionary = {}
var _tile_list: Array[Dictionary] = []
var _conversion_origin_e: float = 0.0
var _conversion_origin_n: float = 0.0
var _resolved_dir: String = ""

var _loaded_tiles: Dictionary = {}
var _loading_tiles: Dictionary = {}
var _pending_loads: Array[String] = []

var _origin_offset: Vector3 = Vector3.ZERO
var _main_node: Node = null

# Frame counter for throttling tile distance checks
var _frame_counter: int = 0
const UPDATE_TILE_INTERVAL: int = 30

var tiles_loaded: int = 0
var tiles_in_memory: int = 0

var _water_shader: Shader = null


func _ready() -> void:
	_resolved_dir = _resolve_scenery_path(water_directory)

	if aircraft_node == null:
		aircraft_node = get_parent().get_node_or_null("AC")
		if aircraft_node == null:
			push_warning("WaterLoader: No aircraft node set.")

	_main_node = get_parent()

	if not _load_manifest():
		push_warning("WaterLoader: No manifest loaded — water loading disabled.")
		enabled = false
		return

	_water_shader = _create_water_shader()

	print("WaterLoader: Ready — %d tiles available from '%s'" % [_tile_list.size(), _resolved_dir])


func _process(_delta: float) -> void:
	if not enabled or aircraft_node == null:
		return
	var aircraft_pos = aircraft_node.global_position
	_frame_counter += 1
	if _frame_counter >= UPDATE_TILE_INTERVAL:
		_frame_counter = 0
		_update_tile_loading(aircraft_pos)
	_process_pending_loads()
	_check_loading_progress()


# ── Manifest Loading ───────────────────────────────────────────────────

func _load_manifest() -> bool:
	var manifest_path = _resolved_dir.path_join("manifest.json")
	var file = FileAccess.open(manifest_path, FileAccess.READ)
	if not file:
		push_error("WaterLoader: Cannot open manifest at '%s'" % manifest_path)
		return false

	var json_parser = JSON.new()
	var err = json_parser.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_error("WaterLoader: Bad manifest JSON: %s" % json_parser.get_error_message())
		return false

	_manifest = json_parser.data
	_conversion_origin_e = float(_manifest.get("conversion_origin_e", 0))
	_conversion_origin_n = float(_manifest.get("conversion_origin_n", 0))

	_tile_list.clear()
	var tiles_dict = _manifest.get("tiles", {})
	for tile_id in tiles_dict:
		var info = tiles_dict[tile_id]
		_tile_list.append({
			"id": tile_id,
			"file": str(info.get("file", "")),
			"center_e": float(info.get("center_e", 0)),
			"center_n": float(info.get("center_n", 0)),
		})

	return _tile_list.size() > 0


# ── Coordinate Helpers ─────────────────────────────────────────────────

func _get_terrain_center() -> Vector2:
	if _main_node and "TERRAIN_CENTER_E" in _main_node:
		return Vector2(_main_node.TERRAIN_CENTER_E, _main_node.TERRAIN_CENTER_N)
	return Vector2(2682880.0, 1283072.0)


func _swiss_to_godot(swiss_e: float, swiss_n: float) -> Vector3:
	var center = _get_terrain_center()
	return Vector3(
		center.x - swiss_e,
		0.0,
		swiss_n - center.y
	)


# ── Tile Loading / Unloading ───────────────────────────────────────────

func _update_tile_loading(aircraft_pos: Vector3) -> void:
	var load_dist_sq = load_distance * load_distance
	var unload_dist_sq = unload_distance * unload_distance

	for tile_info in _tile_list:
		var tile_id: String = tile_info["id"]
		var tile_center = _swiss_to_godot(tile_info["center_e"], tile_info["center_n"])

		var delta = aircraft_pos - tile_center
		delta.y = 0
		var dist_sq = delta.length_squared()

		var is_loaded = _loaded_tiles.has(tile_id)
		var is_loading = _loading_tiles.has(tile_id) or tile_id in _pending_loads

		if dist_sq < load_dist_sq:
			if not is_loaded and not is_loading:
				_queue_tile_load(tile_id)
		elif dist_sq > unload_dist_sq:
			if is_loaded:
				_unload_tile(tile_id)
			elif is_loading:
				_cancel_tile_load(tile_id)


func _queue_tile_load(tile_id: String) -> void:
	if tile_id in _pending_loads:
		return
	_pending_loads.append(tile_id)


func _process_pending_loads() -> void:
	var loads_this_frame = 0

	while _pending_loads.size() > 0 and loads_this_frame < max_loads_per_frame:
		var tile_id = _pending_loads.pop_front()

		var tile_info = null
		for t in _tile_list:
			if t["id"] == tile_id:
				tile_info = t
				break

		if tile_info == null:
			continue

		var res_path = water_res_prefix.path_join(tile_info["file"])

		if ResourceLoader.exists(res_path):
			ResourceLoader.load_threaded_request(res_path)
			_loading_tiles[tile_id] = res_path
			loads_this_frame += 1
			if debug_mode:
				print("WaterLoader: Loading %s from %s" % [tile_id, res_path])
		else:
			push_warning("WaterLoader: GLB not found: %s" % res_path)


func _check_loading_progress() -> void:
	var completed: Array[String] = []

	for tile_id in _loading_tiles.keys():
		var path = _loading_tiles[tile_id]
		var status = ResourceLoader.load_threaded_get_status(path)

		match status:
			ResourceLoader.THREAD_LOAD_LOADED:
				var resource = ResourceLoader.load_threaded_get(path)
				_instantiate_tile(tile_id, resource)
				completed.append(tile_id)
			ResourceLoader.THREAD_LOAD_FAILED:
				push_error("WaterLoader: Failed to load tile %s" % tile_id)
				completed.append(tile_id)
			ResourceLoader.THREAD_LOAD_INVALID_RESOURCE:
				push_error("WaterLoader: Invalid resource for tile %s" % tile_id)
				completed.append(tile_id)

	for tile_id in completed:
		_loading_tiles.erase(tile_id)


func _instantiate_tile(tile_id: String, resource: Resource) -> void:
	if resource == null or not resource is PackedScene:
		push_error("WaterLoader: Resource is not a PackedScene for tile %s" % tile_id)
		return

	var instance = (resource as PackedScene).instantiate()
	instance.name = "Water_" + tile_id

	var origin_godot = _swiss_to_godot(_conversion_origin_e, _conversion_origin_n)
	instance.position = origin_godot

	_apply_water_materials(instance)

	add_child(instance)
	_loaded_tiles[tile_id] = instance
	tiles_loaded += 1
	tiles_in_memory += 1

	if debug_mode:
		print("WaterLoader: Loaded tile %s (total: %d)" % [tile_id, tiles_in_memory])


func _unload_tile(tile_id: String) -> void:
	if not _loaded_tiles.has(tile_id):
		return

	var instance = _loaded_tiles[tile_id]
	if instance != null and is_instance_valid(instance):
		instance.queue_free()

	_loaded_tiles.erase(tile_id)
	tiles_in_memory -= 1

	if debug_mode:
		print("WaterLoader: Unloaded tile %s (remaining: %d)" % [tile_id, tiles_in_memory])


func _cancel_tile_load(tile_id: String) -> void:
	_pending_loads.erase(tile_id)
	_loading_tiles.erase(tile_id)


# ── Public API ─────────────────────────────────────────────────────────

func shift_all_water(offset: Vector3) -> void:
	_origin_offset += offset
	var count = 0
	for tile_id in _loaded_tiles:
		var instance = _loaded_tiles[tile_id]
		if instance != null and is_instance_valid(instance):
			instance.position += offset
			count += 1
	print("WaterLoader: Shifted %d water tiles by offset (%0.1f, %0.1f)" % [count, offset.x, offset.z])


func unload_all() -> void:
	for tile_id in _loaded_tiles.keys():
		_unload_tile(tile_id)
	_pending_loads.clear()
	_loading_tiles.clear()


func get_stats() -> Dictionary:
	return {
		"tiles_loaded": tiles_loaded,
		"tiles_in_memory": tiles_in_memory,
		"tiles_loading": _loading_tiles.size(),
		"tiles_pending": _pending_loads.size(),
		"total_available": _tile_list.size()
	}


# ── Materials ──────────────────────────────────────────────────────────

func _apply_water_materials(node: Node) -> void:
	var material = _create_water_shader_material()
	_apply_materials_recursive(node, material)


func _apply_materials_recursive(node: Node, mat: ShaderMaterial) -> void:
	if node is MeshInstance3D:
		(node as MeshInstance3D).material_override = mat
	for child in node.get_children():
		_apply_materials_recursive(child, mat)


func _create_water_shader() -> Shader:
	var shader = Shader.new()
	shader.code = """
shader_type spatial;
render_mode cull_disabled;

uniform float height_offset = 2.0;
uniform vec3 water_color = vec3(0.15, 0.32, 0.42);

void vertex() {
	VERTEX.y += height_offset;
}

void fragment() {
	ALBEDO = water_color;
	ROUGHNESS = 0.15;
	METALLIC = 0.1;
	SPECULAR = 0.6;
}
"""
	return shader


func _create_water_shader_material() -> ShaderMaterial:
	if _water_shader == null:
		_water_shader = _create_water_shader()
	var material = ShaderMaterial.new()
	material.shader = _water_shader
	material.render_priority = 1
	return material


# ── Path Resolution ────────────────────────────────────────────────────

static func _resolve_scenery_path(path: String) -> String:
	if path.begins_with("/") or path.begins_with("res://") or path.begins_with("user://"):
		return path
	var base_dir: String
	if OS.has_feature("editor"):
		var project_dir = ProjectSettings.globalize_path("res://")
		if project_dir.ends_with("/"):
			project_dir = project_dir.left(project_dir.length() - 1)
		base_dir = project_dir.get_base_dir()
	else:
		var exe = OS.get_executable_path()
		base_dir = exe.get_base_dir().get_base_dir().get_base_dir().get_base_dir()
	var resolved = base_dir.path_join(path)
	print("WaterLoader: Resolved '%s' → '%s'" % [path, resolved])
	return resolved
