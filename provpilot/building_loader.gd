extends Node3D
## BuildingLoader — Manifest-driven dynamic building tile loading
##
## Loads/unloads SwissTopo building GLB files based on aircraft proximity.
## Tile positions are read from scenery/buildings/manifest.json (absolute
## Swiss LV95 coordinates), eliminating hardcoded arrays and origin offsets.
##
## At runtime, Swiss coordinates are converted to Godot world-space using
## the current TERRAIN_CENTER_E/N (which may change during floating-origin
## rebases).

class_name BuildingLoader

# ── Configuration (exports) ────────────────────────────────────────────

## Path to the buildings directory (contains manifest.json and GLB files).
## Relative paths are resolved against the workspace root (parent of Godot project).
@export var buildings_directory: String = "scenery/buildings"

## Godot res:// prefix for GLB loading (ResourceLoader needs res:// paths)
@export var buildings_res_prefix: String = "res://Assets/buildings"

## The node to track for position (usually the aircraft)
@export var aircraft_node: Node3D

## Distance (in meters) at which to load building tiles
@export var load_distance: float = 8000.0

## Distance (in meters) at which to unload building tiles (should be > load_distance)
@export var unload_distance: float = 12000.0

## Maximum number of tiles to load per frame (to prevent stuttering)
@export var max_loads_per_frame: int = 1

## Enable debug visualization
@export var debug_mode: bool = false

## Enable/disable building loading entirely
@export var enabled: bool = true

## Roof color (terracotta/brownish-red)
@export var roof_color: Color = Color(0.72, 0.38, 0.28, 1.0)

## Wall color (warm beige/cream)
@export var wall_color: Color = Color(0.92, 0.87, 0.75, 1.0)


# ── Internal State ─────────────────────────────────────────────────────

var _manifest: Dictionary = {}        # Parsed manifest.json
var _tile_list: Array[Dictionary] = [] # [{id, file, center_e, center_n}, ...]
var _conversion_origin_e: float = 0.0
var _conversion_origin_n: float = 0.0
var _resolved_dir: String = ""        # Absolute filesystem path to buildings dir

var _loaded_tiles: Dictionary = {}    # tile_id -> Node3D instance
var _loading_tiles: Dictionary = {}   # tile_id -> res:// path
var _pending_loads: Array[String] = []

# Cumulative origin offset from floating-origin rebases.
var _origin_offset: Vector3 = Vector3.ZERO

# Reference to parent for TERRAIN_CENTER access
var _main_node: Node = null

# Statistics
var tiles_loaded: int = 0
var tiles_in_memory: int = 0

# Cached shared shader for building materials
var _building_shader: Shader = null


func _ready() -> void:
	# Resolve buildings directory path
	_resolved_dir = _resolve_scenery_path(buildings_directory)

	# Try to find aircraft node if not set
	if aircraft_node == null:
		aircraft_node = get_parent().get_node_or_null("AC")
		if aircraft_node == null:
			push_warning("BuildingLoader: No aircraft node set.")

	# Get reference to main node (for TERRAIN_CENTER_E/N)
	_main_node = get_parent()

	# Load the building manifest
	if not _load_manifest():
		push_warning("BuildingLoader: No manifest loaded — building loading disabled.")
		enabled = false
		return

	# Pre-create shared shader
	_building_shader = _create_building_shader()

	print("BuildingLoader: Ready — %d tiles available from '%s'" % [_tile_list.size(), _resolved_dir])


func _process(_delta: float) -> void:
	if not enabled or aircraft_node == null:
		return

	var aircraft_pos = aircraft_node.global_position

	_update_tile_loading(aircraft_pos)
	_process_pending_loads()
	_check_loading_progress()


# ── Manifest Loading ───────────────────────────────────────────────────

func _load_manifest() -> bool:
	var manifest_path = _resolved_dir.path_join("manifest.json")
	var file = FileAccess.open(manifest_path, FileAccess.READ)
	if not file:
		push_error("BuildingLoader: Cannot open manifest at '%s'" % manifest_path)
		return false

	var json = JSON.new()
	var err = json.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_error("BuildingLoader: Bad manifest JSON: %s" % json.get_error_message())
		return false

	_manifest = json.data
	_conversion_origin_e = float(_manifest.get("conversion_origin_e", 0))
	_conversion_origin_n = float(_manifest.get("conversion_origin_n", 0))

	# Build tile list from manifest
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
	## Return current TERRAIN_CENTER_E/N from the main node.
	if _main_node and "TERRAIN_CENTER_E" in _main_node:
		return Vector2(_main_node.TERRAIN_CENTER_E, _main_node.TERRAIN_CENTER_N)
	# Fallback to initial values
	return Vector2(2682880.0, 1283072.0)


func _swiss_to_godot(swiss_e: float, swiss_n: float) -> Vector3:
	## Convert absolute Swiss LV95 to Godot world-space using current terrain center.
	var center = _get_terrain_center()
	return Vector3(
		center.x - swiss_e,   # Godot X = CENTER_E - Swiss_E  (+X = West)
		0.0,
		swiss_n - center.y    # Godot Z = Swiss_N - CENTER_N  (+Z = North)
	)


# ── Tile Loading / Unloading ───────────────────────────────────────────

func _update_tile_loading(aircraft_pos: Vector3) -> void:
	var load_dist_sq = load_distance * load_distance
	var unload_dist_sq = unload_distance * unload_distance

	for tile_info in _tile_list:
		var tile_id: String = tile_info["id"]

		# Convert absolute Swiss center to current Godot coords
		var tile_center = _swiss_to_godot(tile_info["center_e"], tile_info["center_n"])

		# Apply cumulative floating-origin offset
		tile_center += _origin_offset

		# Calculate horizontal distance (ignore Y/altitude)
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
	if debug_mode:
		print("BuildingLoader: Queued tile %s" % tile_id)


func _process_pending_loads() -> void:
	var loads_this_frame = 0

	while _pending_loads.size() > 0 and loads_this_frame < max_loads_per_frame:
		var tile_id = _pending_loads.pop_front()

		# Find tile info
		var tile_info = null
		for t in _tile_list:
			if t["id"] == tile_id:
				tile_info = t
				break

		if tile_info == null:
			continue

		# GLBs must be inside res:// for Godot's ResourceLoader.
		# Use the configurable res:// prefix + filename from manifest.
		var res_path = buildings_res_prefix.path_join(tile_info["file"])

		if ResourceLoader.exists(res_path):
			ResourceLoader.load_threaded_request(res_path)
			_loading_tiles[tile_id] = res_path
			loads_this_frame += 1
			if debug_mode:
				print("BuildingLoader: Loading tile %s from %s" % [tile_id, res_path])
		else:
			push_warning("BuildingLoader: GLB not found: %s" % res_path)


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
				push_error("BuildingLoader: Failed to load tile %s" % tile_id)
				completed.append(tile_id)
			ResourceLoader.THREAD_LOAD_INVALID_RESOURCE:
				push_error("BuildingLoader: Invalid resource for tile %s" % tile_id)
				completed.append(tile_id)

	for tile_id in completed:
		_loading_tiles.erase(tile_id)


func _instantiate_tile(tile_id: String, resource: Resource) -> void:
	if resource == null or not resource is PackedScene:
		push_error("BuildingLoader: Resource is not a PackedScene for tile %s" % tile_id)
		return

	var instance = (resource as PackedScene).instantiate()
	instance.name = "Buildings_" + tile_id

	# Position: GLB geometry is relative to conversion_origin (the Swiss coord
	# that was subtracted when converting CityGML → GLB).  Place that origin
	# in current Godot world-space plus floating-origin offset.
	var origin_godot = _swiss_to_godot(_conversion_origin_e, _conversion_origin_n)
	instance.position = origin_godot + _origin_offset

	# Apply building colors via shader
	_apply_building_materials(instance)

	add_child(instance)
	_loaded_tiles[tile_id] = instance
	tiles_loaded += 1
	tiles_in_memory += 1

	if debug_mode:
		print("BuildingLoader: Loaded tile %s (total: %d)" % [tile_id, tiles_in_memory])


func _unload_tile(tile_id: String) -> void:
	if not _loaded_tiles.has(tile_id):
		return

	var instance = _loaded_tiles[tile_id]
	if instance != null and is_instance_valid(instance):
		instance.queue_free()

	_loaded_tiles.erase(tile_id)
	tiles_in_memory -= 1

	if debug_mode:
		print("BuildingLoader: Unloaded tile %s (remaining: %d)" % [tile_id, tiles_in_memory])


func _cancel_tile_load(tile_id: String) -> void:
	_pending_loads.erase(tile_id)
	_loading_tiles.erase(tile_id)


# ── Public API ─────────────────────────────────────────────────────────

## Floating origin: shift all loaded building instances by an offset vector.
func shift_all_buildings(offset: Vector3) -> void:
	_origin_offset += offset
	var count = 0
	for tile_id in _loaded_tiles:
		var instance = _loaded_tiles[tile_id]
		if instance != null and is_instance_valid(instance):
			instance.position += offset
			count += 1
	print("BuildingLoader: Shifted %d building tiles by offset (%0.1f, %0.1f)" % [count, offset.x, offset.z])


## Force load all tiles within a given distance
func force_load_nearby(distance: float) -> void:
	if aircraft_node == null:
		return

	var aircraft_pos = aircraft_node.global_position
	var dist_sq = distance * distance

	for tile_info in _tile_list:
		var tile_id = tile_info["id"]
		var tile_center = _swiss_to_godot(tile_info["center_e"], tile_info["center_n"])
		tile_center += _origin_offset

		var delta = aircraft_pos - tile_center
		delta.y = 0

		if delta.length_squared() < dist_sq:
			if not _loaded_tiles.has(tile_id) and not _loading_tiles.has(tile_id):
				_queue_tile_load(tile_id)


## Unload all tiles
func unload_all() -> void:
	for tile_id in _loaded_tiles.keys():
		_unload_tile(tile_id)
	_pending_loads.clear()
	_loading_tiles.clear()


## Get statistics
func get_stats() -> Dictionary:
	return {
		"tiles_loaded": tiles_loaded,
		"tiles_in_memory": tiles_in_memory,
		"tiles_loading": _loading_tiles.size(),
		"tiles_pending": _pending_loads.size(),
		"total_available": _tile_list.size()
	}


# ── Materials ──────────────────────────────────────────────────────────

func _apply_building_materials(node: Node) -> void:
	var material = _create_building_shader_material()
	_apply_materials_recursive(node, material)


func _apply_materials_recursive(node: Node, mat: ShaderMaterial) -> void:
	if node is MeshInstance3D:
		(node as MeshInstance3D).material_override = mat
	for child in node.get_children():
		_apply_materials_recursive(child, mat)


func _create_building_shader() -> Shader:
	var shader = Shader.new()
	shader.code = """
shader_type spatial;

uniform vec4 roof_color : source_color = vec4(0.72, 0.38, 0.28, 1.0);
uniform vec4 wall_color : source_color = vec4(0.92, 0.87, 0.75, 1.0);
uniform float roof_threshold : hint_range(0.0, 1.0) = 0.5;

void fragment() {
	vec3 world_normal = (INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz;
	float up_factor = max(0.0, world_normal.y);
	float roof_blend = smoothstep(roof_threshold - 0.1, roof_threshold + 0.1, up_factor);
	ALBEDO = mix(wall_color.rgb, roof_color.rgb, roof_blend);
	ROUGHNESS = 0.85;
	METALLIC = 0.0;
}
"""
	return shader


func _create_building_shader_material() -> ShaderMaterial:
	if _building_shader == null:
		_building_shader = _create_building_shader()
	var material = ShaderMaterial.new()
	material.shader = _building_shader
	material.set_shader_parameter("roof_color", roof_color)
	material.set_shader_parameter("wall_color", wall_color)
	material.set_shader_parameter("roof_threshold", 0.5)
	return material


# ── Path Resolution ────────────────────────────────────────────────────

static func _resolve_scenery_path(path: String) -> String:
	if path.begins_with("/") or path.begins_with("res://") or path.begins_with("user://"):
		return path
	var project_dir = ProjectSettings.globalize_path("res://")
	if project_dir.ends_with("/"):
		project_dir = project_dir.left(project_dir.length() - 1)
	var workspace_root = project_dir.get_base_dir()
	var resolved = workspace_root.path_join(path)
	print("BuildingLoader: Resolved '%s' → '%s'" % [path, resolved])
	return resolved
