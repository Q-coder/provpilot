## TerrainStreamer — Dynamic terrain region loading for ProvPilot
##
## Loads/unloads Terrain3D regions at runtime based on aircraft position.
## Pre-generated float32 heightmap tiles (from process_swisstopo_terrain.py
## --generate-tiles) are loaded into Terrain3DRegion height maps on demand.
##
## Tile grid: each tile is tile_size × tile_size meters (default 1024m),
## aligned to a global grid in Swiss LV95 coordinates.
## Tile (ei, ni) covers E: [ei*1024, (ei+1)*1024), N: [ni*1024, (ni+1)*1024).
##
## IMPORTANT: terrain_center_e and terrain_center_n MUST be multiples of
## tile_size (1024) for correct tile-to-region alignment.

extends Node

## Emitted after initial terrain load is complete.
signal terrain_ready

## Emitted whenever tiles are loaded or unloaded.
signal tiles_changed(loaded: int, unloaded: int, total_loaded: int)


# ── Configuration ───────────────────────────────────────────────────────

## Path to the directory containing manifest.json and tile_*.raw files.
@export var tile_directory: String = "user://terrain_tiles"

## NodePath to the Terrain3D node.
@export var terrain_node_path: NodePath = NodePath("../Terrain3D")

## How many tiles in each direction to keep loaded around the aircraft.
## Loaded area = (2 * load_radius + 1)² tiles.
## E.g. load_radius=8 → 17×17 = 289 tiles ≈ 17km × 17km.
@export_range(1, 15) var load_radius: int = 8

## Extra tiles beyond load_radius before unloading (hysteresis to avoid
## rapid load/unload at boundaries).
@export_range(0, 4) var unload_margin: int = 2

## How often to check for needed tile changes (every N physics frames).
@export_range(1, 120) var update_interval: int = 30

## Maximum tiles to load per update cycle (budget to avoid frame hitches).
## Set to 0 for unlimited (useful for initial load).
@export_range(0, 50) var load_budget_per_update: int = 4

## Terrain center in Swiss LV95. MUST be multiples of tile_size (1024).
## These define the mapping: Godot X = center_e - Swiss_E, Godot Z = Swiss_N - center_n.
@export var terrain_center_e: int = 2682880  # 2620 * 1024
@export var terrain_center_n: int = 1283072  # 1253 * 1024


# ── Internal State ──────────────────────────────────────────────────────

var _terrain: Node = null
var _terrain_data: Object = null
var _manifest: Dictionary = {}
var _tile_size: int = 1024
var _pixels_per_tile: int = 1024
var _center_ei: int = 0
var _center_ni: int = 0
var _loaded_tiles: Dictionary = {}   # "E_N" -> Vector2i(region_x, region_z)
var _frame_counter: int = 0
var _initialized: bool = false
var _pending_loads: Array[String] = []


func _ready() -> void:
	if Engine.is_editor_hint():
		return

	_center_ei = terrain_center_e / _tile_size
	_center_ni = terrain_center_n / _tile_size

	# Validate alignment
	if terrain_center_e % _tile_size != 0 or terrain_center_n % _tile_size != 0:
		push_error("TerrainStreamer: terrain_center_e (%d) and terrain_center_n (%d) "
				   % [terrain_center_e, terrain_center_n]
				   + "must be multiples of tile_size (%d)" % _tile_size)
		return

	# Load manifest
	if not _load_manifest():
		return

	# Get Terrain3D references
	_terrain = get_node_or_null(terrain_node_path)
	if not _terrain:
		push_error("TerrainStreamer: Terrain3D node not found at '%s'" % str(terrain_node_path))
		return

	_terrain_data = _terrain.get("data")
	if not _terrain_data:
		push_error("TerrainStreamer: Cannot access Terrain3DData from Terrain3D node")
		return

	_initialized = true
	print("TerrainStreamer: Ready — %d tiles available, center E=%d N=%d (ei=%d ni=%d)"
		  % [_manifest["tiles"].size(), terrain_center_e, terrain_center_n,
			 _center_ei, _center_ni])


func _physics_process(_delta: float) -> void:
	if not _initialized or Engine.is_editor_hint():
		return

	_frame_counter += 1
	if _frame_counter % update_interval != 0:
		return

	# Process any pending loads from previous cycles
	if not _pending_loads.is_empty():
		_process_pending_loads()
		return

	# Check if we need to load/unload based on current position
	_update_regions()


# ── Public API ──────────────────────────────────────────────────────────

## Load initial terrain around a Godot world position. Call this once
## at startup before the aircraft is placed. Blocks until complete.
func initial_load(godot_position: Vector3) -> int:
	if not _initialized:
		push_error("TerrainStreamer: Not initialized, cannot perform initial load")
		return 0

	# CRITICAL: Set the region size on the data before any region operations.
	# Without data_directory, Terrain3DData._region_size defaults to 0,
	# which causes all region creation to fail silently.
	print("TerrainStreamer: Setting region_size = %d" % _tile_size)
	_terrain_data.change_region_size(_tile_size)

	# Clear any existing regions
	_clear_all_regions()

	var swiss = godot_to_swiss(godot_position)
	var center_tile = _swiss_to_tile_index(swiss.x, swiss.y)

	var count := 0
	var to_load: Array[String] = []

	for dei in range(-load_radius, load_radius + 1):
		for dni in range(-load_radius, load_radius + 1):
			var ei = center_tile.x + dei
			var ni = center_tile.y + dni
			var key = "%d_%d" % [ei * _tile_size, ni * _tile_size]

			if _manifest["tiles"].has(key):
				var rloc = _tile_to_region(ei, ni)
				if _is_valid_region(rloc):
					to_load.append(key)

	print("TerrainStreamer: Initial load — %d tiles to load" % to_load.size())

	for key in to_load:
		if _load_tile(key):
			count += 1

	if count > 0:
		_terrain_data.update_maps()

	print("TerrainStreamer: Initial load complete — %d regions active" % count)
	terrain_ready.emit()
	return count


## Convert a Godot world position to Swiss LV95 coordinates.
func godot_to_swiss(pos: Vector3) -> Vector2:
	return Vector2(
		terrain_center_e - pos.x,
		terrain_center_n + pos.z
	)


## Convert Swiss LV95 coordinates to a Godot world position (Y=0).
func swiss_to_godot(swiss_e: float, swiss_n: float) -> Vector3:
	return Vector3(
		terrain_center_e - swiss_e,
		0.0,
		swiss_n - terrain_center_n
	)


## Get the number of currently loaded tiles.
func get_loaded_count() -> int:
	return _loaded_tiles.size()


## Get the maximum number of tiles that can be loaded (grid limit).
func get_max_loadable() -> int:
	# 32×32 grid, but load_radius limits it further
	var side = 2 * load_radius + 1
	return mini(side * side, 1024)


# ── Internal: Manifest ──────────────────────────────────────────────────

func _load_manifest() -> bool:
	var manifest_path = tile_directory + "/manifest.json"
	var file = FileAccess.open(manifest_path, FileAccess.READ)
	if not file:
		push_error("TerrainStreamer: Cannot open manifest at '%s'" % manifest_path)
		return false

	var json = JSON.new()
	var err = json.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_error("TerrainStreamer: Bad manifest JSON: %s" % json.get_error_message())
		return false

	_manifest = json.data
	_tile_size = int(_manifest.get("tile_size_m", 1024))
	_pixels_per_tile = int(_manifest.get("pixels_per_tile", 1024))

	if not _manifest.has("tiles") or _manifest["tiles"].is_empty():
		push_error("TerrainStreamer: Manifest has no tiles")
		return false

	return true


# ── Internal: Coordinate Mapping ────────────────────────────────────────

func _swiss_to_tile_index(swiss_e: float, swiss_n: float) -> Vector2i:
	return Vector2i(
		int(floor(swiss_e / _tile_size)),
		int(floor(swiss_n / _tile_size))
	)


func _tile_to_region(ei: int, ni: int) -> Vector2i:
	## Map a Swiss tile grid index to a Terrain3D region location.
	##
	## Derivation:
	##   Godot X of tile's east edge = center_e - (ei+1)*tile_size
	##   Region rx covers [rx*tile_size, (rx+1)*tile_size)
	##   So rx*tile_size = center_e - (ei+1)*tile_size
	##   rx = center_ei - ei - 1
	##
	##   Godot Z of tile's south edge = ni*tile_size - center_n
	##   rz*tile_size = ni*tile_size - center_n
	##   rz = ni - center_ni
	var rx = _center_ei - ei - 1
	var rz = ni - _center_ni
	return Vector2i(rx, rz)


func _is_valid_region(rloc: Vector2i) -> bool:
	return rloc.x >= -16 and rloc.x <= 15 and rloc.y >= -16 and rloc.y <= 15


# ── Internal: Region Management ────────────────────────────────────────

func _clear_all_regions() -> void:
	for region in _terrain_data.get_regions_active():
		_terrain_data.remove_region(region, false)
	_terrain_data.update_maps()
	_loaded_tiles.clear()


func _get_aircraft_position() -> Vector3:
	# Try parent's method first (node_3d.gd provides aircraft position)
	var parent = get_parent()
	if parent and parent.has_node("C172p"):
		var ac = parent.get_node("C172p")
		if ac:
			return ac.global_position

	# Fallback: camera or origin
	var vp = get_viewport()
	if vp and vp.get_camera_3d():
		return vp.get_camera_3d().global_position

	return Vector3.ZERO


func _update_regions() -> void:
	var godot_pos = _get_aircraft_position()
	var swiss = godot_to_swiss(godot_pos)
	var center_tile = _swiss_to_tile_index(swiss.x, swiss.y)

	var unload_radius = load_radius + unload_margin
	var needed_keys: Dictionary = {}
	var new_loads: Array[String] = []

	# Determine tiles that should be loaded
	for dei in range(-load_radius, load_radius + 1):
		for dni in range(-load_radius, load_radius + 1):
			var ei = center_tile.x + dei
			var ni = center_tile.y + dni
			var key = "%d_%d" % [ei * _tile_size, ni * _tile_size]
			needed_keys[key] = true

			if not _loaded_tiles.has(key) and _manifest["tiles"].has(key):
				var rloc = _tile_to_region(ei, ni)
				if _is_valid_region(rloc):
					new_loads.append(key)

	# Determine tiles to unload (beyond unload_radius)
	var to_unload: Array[String] = []
	for key in _loaded_tiles:
		if needed_keys.has(key):
			continue
		var tile_info = _manifest["tiles"].get(key)
		if tile_info:
			var dei = int(tile_info["tile_ei"]) - center_tile.x
			var dni = int(tile_info["tile_ni"]) - center_tile.y
			if absi(dei) > unload_radius or absi(dni) > unload_radius:
				to_unload.append(key)
		else:
			to_unload.append(key)

	if new_loads.is_empty() and to_unload.is_empty():
		return

	# Unload distant tiles
	for key in to_unload:
		_unload_tile(key)

	# Load new tiles (with budget)
	if load_budget_per_update > 0 and new_loads.size() > load_budget_per_update:
		# Queue excess for next cycles
		_pending_loads = new_loads.slice(load_budget_per_update)
		new_loads = new_loads.slice(0, load_budget_per_update)

	var loaded_count := 0
	for key in new_loads:
		if _load_tile(key):
			loaded_count += 1

	# Rebuild texture arrays
	if loaded_count > 0 or not to_unload.is_empty():
		_terrain_data.update_maps()
		tiles_changed.emit(loaded_count, to_unload.size(), _loaded_tiles.size())


func _process_pending_loads() -> void:
	if _pending_loads.is_empty():
		return

	var batch_size = load_budget_per_update if load_budget_per_update > 0 else _pending_loads.size()
	var batch = _pending_loads.slice(0, batch_size)
	_pending_loads = _pending_loads.slice(batch_size)

	var loaded := 0
	for key in batch:
		if not _loaded_tiles.has(key):
			if _load_tile(key):
				loaded += 1

	if loaded > 0:
		_terrain_data.update_maps()
		tiles_changed.emit(loaded, 0, _loaded_tiles.size())


func _load_tile(tile_key: String) -> bool:
	var tile_info = _manifest["tiles"].get(tile_key)
	if not tile_info:
		return false

	# Read raw float32 heightmap data
	var tile_path = tile_directory + "/" + str(tile_info["file"])
	var file = FileAccess.open(tile_path, FileAccess.READ)
	if not file:
		push_warning("TerrainStreamer: Cannot open tile '%s'" % tile_path)
		return false

	var expected_bytes = _pixels_per_tile * _pixels_per_tile * 4
	var bytes = file.get_buffer(expected_bytes)
	file.close()

	if bytes.size() != expected_bytes:
		push_warning("TerrainStreamer: Tile '%s' wrong size %d (expected %d)"
					 % [tile_key, bytes.size(), expected_bytes])
		return false

	# Create height map image (FORMAT_RF = single-channel float32)
	var img = Image.create_from_data(
		_pixels_per_tile, _pixels_per_tile,
		false, Image.FORMAT_RF, bytes
	)

	# Calculate Terrain3D region location
	var ei = int(tile_info["tile_ei"])
	var ni = int(tile_info["tile_ni"])
	var rloc = _tile_to_region(ei, ni)

	if not _is_valid_region(rloc):
		return false

	# Remove existing region at this location (if any)
	if _terrain_data.has_region(rloc):
		_terrain_data.remove_regionl(rloc, false)

	# Create a Terrain3DRegion manually with proper setup.
	# This mirrors what import_images() does internally (terrain_3d_data.cpp).
	# We can't use add_region_blank because it depends on _region_size being
	# already set on the data, and we need explicit control over the maps.
	var region = Terrain3DRegion.new()
	region.set_location(rloc)
	region.set_region_size(_tile_size)
	region.set_vertex_spacing(1.0)
	region.set_height_map(img)  # Also calls calc_height_range() internally
	region.sanitize_maps()      # Creates blank control + color maps

	var err = _terrain_data.add_region(region, false)
	if err != OK:
		push_warning("TerrainStreamer: Failed to add region at (%d, %d): error %d"
					 % [rloc.x, rloc.y, err])
		return false

	_loaded_tiles[tile_key] = rloc
	return true


func _unload_tile(tile_key: String) -> void:
	if not _loaded_tiles.has(tile_key):
		return

	var rloc: Vector2i = _loaded_tiles[tile_key]
	if _terrain_data.has_region(rloc):
		_terrain_data.remove_regionl(rloc, false)

	_loaded_tiles.erase(tile_key)
