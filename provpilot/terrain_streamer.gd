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
## Can be absolute or relative to the project. At startup, a relative path
## is resolved against the project's parent directory (workspace root).
@export var tile_directory: String = "scenery/terrain"

## Path to the directory containing orthophoto manifest.json and ortho_*.jpg
## files (produced by process_swissimage_orthophoto.py). Leave empty to
## disable orthophoto color maps.  Resolved like tile_directory.
@export var orthophoto_directory: String = "scenery/orthophoto"

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
var _ortho_manifest: Dictionary = {}   # orthophoto manifest (may be empty)
var _tile_size: int = 1024
var _pixels_per_tile: int = 1024
var _center_ei: int = 0
var _center_ni: int = 0
var _loaded_tiles: Dictionary = {}   # "E_N" -> Vector2i(region_x, region_z)
var _frame_counter: int = 0
var _initialized: bool = false
var _pending_loads: Array[String] = []
var _rebase_in_progress: bool = false

## Budget: tiles loaded per frame during progressive (rebase / initial) loading.
## Higher = faster terrain fill but bigger per-frame cost.  4 tiles ≈ 16 MB I/O
## ≈ 8 ms on a typical SSD — acceptable for one frame of elevated load.
const REBASE_TILES_PER_FRAME: int = 8

## Tiles loaded synchronously during a floating-origin rebase (ground collision).
const IMMEDIATE_TILES_REBASE: int = 25     # ~5×5 ring

## Tiles loaded synchronously during initial startup load.
const IMMEDIATE_TILES_INITIAL: int = 9     # ~3×3 ring


func _ready() -> void:
	if Engine.is_editor_hint():
		return

	# Resolve tile_directory: if not absolute and not res://, resolve
	# relative to the workspace root (one level above the Godot project).
	tile_directory = _resolve_scenery_path(tile_directory)
	if not orthophoto_directory.is_empty():
		orthophoto_directory = _resolve_scenery_path(orthophoto_directory)

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

	# Load orthophoto manifest (optional — not fatal if missing)
	if not orthophoto_directory.is_empty():
		_load_ortho_manifest()

	# Get Terrain3D references
	_terrain = get_node_or_null(terrain_node_path)
	if not _terrain:
		push_error("TerrainStreamer: Terrain3D node not found at '%s'" % str(terrain_node_path))
		return

	_terrain_data = _terrain.get("data")
	if not _terrain_data:
		push_error("TerrainStreamer: Cannot access Terrain3DData from Terrain3D node")
		return

	# Enable show_colormap on the material when orthophoto data is available.
	# This makes Terrain3D use the color map as the direct terrain albedo
	# (ALBEDO = color_map.rgb) instead of multiplying it with the auto-shader
	# textures (ALBEDO = texture * color_map * macro_variation).
	if not _ortho_manifest.is_empty():
		var mat = _terrain.get("material")
		if mat:
			mat.set("show_colormap", true)
			print("TerrainStreamer: Enabled show_colormap for orthophoto rendering")

	_initialized = true
	print("TerrainStreamer: Ready — %d tiles available, center E=%d N=%d (ei=%d ni=%d)"
		  % [_manifest["tiles"].size(), terrain_center_e, terrain_center_n,
			 _center_ei, _center_ni])


func _physics_process(_delta: float) -> void:
	if not _initialized or Engine.is_editor_hint():
		return

	# During rebase or initial progressive loading, load tiles every frame
	# with a higher budget so terrain fills in within a few seconds.
	if _rebase_in_progress:
		if not _pending_loads.is_empty():
			var budget := REBASE_TILES_PER_FRAME
			var batch = _pending_loads.slice(0, budget)
			_pending_loads = _pending_loads.slice(budget)
			var loaded := 0
			for key in batch:
				if not _loaded_tiles.has(key):
					if _load_tile(key):
						loaded += 1
			if loaded > 0:
				_terrain_data.update_maps()
				tiles_changed.emit(loaded, 0, _loaded_tiles.size())
		else:
			_rebase_in_progress = false
			_purge_ghost_regions("progressive_done")
			print("TerrainStreamer: Progressive loading complete — %d regions active"
				  % _loaded_tiles.size())
			verify_tiles("progressive_done")
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

	# Collect all valid tiles with distance from aircraft for priority sorting
	var tile_entries: Array = []

	for dei in range(-load_radius, load_radius + 1):
		for dni in range(-load_radius, load_radius + 1):
			var ei = center_tile.x + dei
			var ni = center_tile.y + dni
			var key = "%d_%d" % [ei * _tile_size, ni * _tile_size]

			if _manifest["tiles"].has(key):
				var rloc = _tile_to_region(ei, ni)
				if _is_valid_region(rloc):
					tile_entries.append({"key": key, "dist": absi(dei) + absi(dni)})

	# Sort nearest-first so ground under the aircraft loads first
	tile_entries.sort_custom(func(a, b): return a["dist"] < b["dist"])

	# Load a small set immediately (essential for ground collision & JSBSim init)
	var count := 0
	var queued: Array[String] = []
	for entry in tile_entries:
		if count < IMMEDIATE_TILES_INITIAL:
			if _load_tile(entry["key"]):
				count += 1
		else:
			queued.append(entry["key"])

	if count > 0:
		_terrain_data.update_maps()

	# Queue remaining tiles for progressive loading across subsequent frames
	_pending_loads = queued
	if not _pending_loads.is_empty():
		_rebase_in_progress = true

	print("TerrainStreamer: Initial load — %d tiles immediate, %d queued for progressive load"
		  % [count, queued.size()])
	verify_tiles("initial_load")
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


## Diagnostic: verify that every loaded tile's region location is consistent
## with its Swiss tile key. Returns the number of mismatched tiles.
func verify_tiles(label: String = "") -> int:
	var mismatches := 0
	var prefix := "VERIFY[%s]" % label if label else "VERIFY"
	print("%s: center_e=%d center_n=%d center_ei=%d center_ni=%d  loaded=%d"
		  % [prefix, terrain_center_e, terrain_center_n, _center_ei, _center_ni,
			 _loaded_tiles.size()])
	for key in _loaded_tiles:
		var rloc: Vector2i = _loaded_tiles[key]
		# Parse tile key to get Swiss coords
		var parts = key.split("_")
		var swiss_e = int(parts[0])
		var swiss_n = int(parts[1])
		var ei = swiss_e / _tile_size
		var ni = swiss_n / _tile_size
		# Expected rloc from _tile_to_region
		var expected = _tile_to_region(ei, ni)
		# Compute where T3D actually places this region in Godot world space
		var world_x_min = rloc.x * _tile_size
		var world_z_min = rloc.y * _tile_size
		# The tile should cover Swiss E from ei*1024 to (ei+1)*1024
		# In Godot: X = center_e - Swiss_E, so:
		#   west edge (min Swiss_E): X = center_e - ei*1024
		#   east edge (max Swiss_E): X = center_e - (ei+1)*1024
		# T3D places region at [rloc.x*1024, (rloc.x+1)*1024) in X
		# So rloc.x*1024 should = center_e - (ei+1)*1024
		var expected_world_x = terrain_center_e - (ei + 1) * _tile_size
		var expected_world_z = (ni * _tile_size) - terrain_center_n
		if rloc != expected or world_x_min != expected_world_x or world_z_min != expected_world_z:
			mismatches += 1
			print("  MISMATCH tile %s: rloc=%s expected=%s  worldX=%d expected=%d  worldZ=%d expected=%d"
				  % [key, str(rloc), str(expected), world_x_min, expected_world_x,
					 world_z_min, expected_world_z])
	if mismatches == 0:
		print("%s: All %d tiles OK" % [prefix, _loaded_tiles.size()])
	else:
		print("%s: *** %d / %d tiles MISMATCHED ***" % [prefix, mismatches, _loaded_tiles.size()])
	# Also check: does T3D actually have regions at these locations?
	var t3d_regions = _terrain_data.get_regions_active()
	var t3d_count = t3d_regions.size() if t3d_regions else 0
	if t3d_count != _loaded_tiles.size():
		print("%s: WARNING — T3D has %d active regions but we track %d tiles"
			  % [prefix, t3d_count, _loaded_tiles.size()])
	return mismatches


## Floating origin: update center coordinates and reload terrain regions.
## Clears ALL Terrain3D regions and reloads tiles from disk at updated grid
## positions.  Previous approach tried to recycle Terrain3DRegion objects
## in-place after remove_region → set_location → add_region, but Terrain3D's
## C++ side invalidates region internals once removed, producing ghost
## geometry at stale grid positions.  Clean-slate reload is simpler, correct,
## and fast on SSD (~0.5 s for the immediate batch, rest loads progressively).
## new_center_e and new_center_n MUST be multiples of tile_size (1024).
func rebase_center(new_center_e: int, new_center_n: int) -> void:
	if new_center_e % _tile_size != 0 or new_center_n % _tile_size != 0:
		push_error("TerrainStreamer.rebase_center: new center (%d, %d) must be multiples of %d"
				   % [new_center_e, new_center_n, _tile_size])
		return

	print("TerrainStreamer: Rebasing center E=%d -> %d, N=%d -> %d"
		  % [terrain_center_e, new_center_e, terrain_center_n, new_center_n])

	# ── 1. Clear ALL regions from Terrain3D ───────────────────────────
	_clear_all_regions()

	# ── 2. Update center coordinates ──────────────────────────────────
	terrain_center_e = new_center_e
	terrain_center_n = new_center_n
	_center_ei = terrain_center_e / _tile_size
	_center_ni = terrain_center_n / _tile_size

	# ── 3. Ensure region_size is set (may be cleared after full removal)
	_terrain_data.change_region_size(_tile_size)

	# ── 4. Collect all valid tiles within load_radius, sorted by distance
	var pos = _get_aircraft_position()
	var swiss = godot_to_swiss(pos)
	var center_tile = _swiss_to_tile_index(swiss.x, swiss.y)
	var tile_entries: Array = []

	for dei in range(-load_radius, load_radius + 1):
		for dni in range(-load_radius, load_radius + 1):
			var ei = center_tile.x + dei
			var ni = center_tile.y + dni
			var key = "%d_%d" % [ei * _tile_size, ni * _tile_size]
			if _manifest["tiles"].has(key):
				var rloc = _tile_to_region(ei, ni)
				if _is_valid_region(rloc):
					tile_entries.append({"key": key, "dist": absi(dei) + absi(dni)})

	tile_entries.sort_custom(func(a, b): return a["dist"] < b["dist"])

	# ── 5. Load a small immediate batch (ground under aircraft) ───────
	var count := 0
	var queued: Array[String] = []
	for entry in tile_entries:
		if count < IMMEDIATE_TILES_REBASE:
			if _load_tile(entry["key"]):
				count += 1
		else:
			queued.append(entry["key"])

	if count > 0:
		_terrain_data.update_maps()

	# ── 6. Queue remainder for progressive loading ────────────────────
	_pending_loads = queued
	_rebase_in_progress = not _pending_loads.is_empty()

	print("TerrainStreamer: Rebase — %d immediate, %d queued (center_ei=%d center_ni=%d)"
		  % [count, queued.size(), _center_ei, _center_ni])
	verify_tiles("post_rebase")


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


func _load_ortho_manifest() -> void:
	var manifest_path = orthophoto_directory + "/manifest.json"
	var file = FileAccess.open(manifest_path, FileAccess.READ)
	if not file:
		print("TerrainStreamer: No orthophoto manifest at '%s' — color maps disabled" % manifest_path)
		return

	var json_parser = JSON.new()
	var err = json_parser.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_warning("TerrainStreamer: Bad orthophoto manifest JSON: %s" % json_parser.get_error_message())
		return

	_ortho_manifest = json_parser.data
	var tile_count = _ortho_manifest.get("tile_count", 0)
	print("TerrainStreamer: Orthophoto manifest loaded — %d tiles available" % tile_count)


func _load_ortho_tile(tile_key: String) -> Image:
	## Load an orthophoto JPEG tile and return it as an RGBA8 Image suitable
	## for Terrain3DRegion.set_color_map().  Returns null if not available.
	var ortho_tiles: Dictionary = _ortho_manifest.get("tiles", {})
	var ortho_info = ortho_tiles.get(tile_key)
	if not ortho_info:
		return null

	var ortho_path: String = orthophoto_directory + "/" + str(ortho_info["file"])
	var ortho_img := Image.new()
	var err := ortho_img.load(ortho_path)
	if err != OK:
		push_warning("TerrainStreamer: Cannot load orthophoto '%s': error %d" % [ortho_path, err])
		return null

	# Terrain3D color maps must be the same size as height maps
	if ortho_img.get_width() != _pixels_per_tile or ortho_img.get_height() != _pixels_per_tile:
		ortho_img.resize(_pixels_per_tile, _pixels_per_tile, Image.INTERPOLATE_BILINEAR)

	# Ensure RGBA8 format (Terrain3D expects this for color maps)
	if ortho_img.get_format() != Image.FORMAT_RGBA8:
		ortho_img.convert(Image.FORMAT_RGBA8)

	# Set alpha to 128 (0.5) for all pixels.
	# Terrain3D's shader uses the color map alpha to modify roughness:
	#   roughness = clamp((color_map.a - 0.5) * 2.0 + base_roughness, 0, 1)
	# Alpha 0.5 (128) is neutral — no roughness modification.
	# JPEG images get alpha=255 (1.0) after RGBA8 conversion, which would
	# add +1.0 to roughness making the terrain unrealistically matte.
	# Operate on raw bytes for speed (GDScript per-pixel loops are too slow).
	var raw := ortho_img.get_data()
	for i in range(3, raw.size(), 4):   # every 4th byte is alpha (RGBA8)
		raw[i] = 128
	ortho_img = Image.create_from_data(
		ortho_img.get_width(), ortho_img.get_height(),
		false, Image.FORMAT_RGBA8, raw
	)

	return ortho_img


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
	_pending_loads.clear()


func _purge_ghost_regions(label: String = "") -> void:
	## Remove Terrain3D active regions that we don't track in _loaded_tiles.
	## These "ghost" regions accumulate because add_region(region, false)
	## doesn't always cleanly overwrite Terrain3D's internal _regions dict.
	var tracked_locs := {}
	for key in _loaded_tiles:
		tracked_locs[_loaded_tiles[key]] = true

	var ghosts_removed := 0
	for region in _terrain_data.get_regions_active():
		var loc: Vector2i = region.get_location()
		if not tracked_locs.has(loc):
			_terrain_data.remove_region(region, false)
			ghosts_removed += 1

	if ghosts_removed > 0:
		_terrain_data.update_maps()
		print("TerrainStreamer[%s]: Purged %d ghost regions" % [label, ghosts_removed])


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

	# NOTE: We don't guard with has_region/remove_regionl before adding.
	# Ghost regions are purged separately via _purge_ghost_regions() after
	# progressive loading completes and after rebase.

	# Create a Terrain3DRegion manually with proper setup.
	# This mirrors what import_images() does internally (terrain_3d_data.cpp).
	# We can't use add_region_blank because it depends on _region_size being
	# already set on the data, and we need explicit control over the maps.
	var region = Terrain3DRegion.new()
	region.set_location(rloc)
	region.set_region_size(_tile_size)
	region.set_vertex_spacing(1.0)
	region.set_height_map(img)  # Also calls calc_height_range() internally

	# Load orthophoto color map if available
	var ortho_loaded := false
	if not _ortho_manifest.is_empty():
		var ortho_img := _load_ortho_tile(tile_key)
		if ortho_img:
			region.set_color_map(ortho_img)
			ortho_loaded = true

	region.sanitize_maps()      # Creates blank control + color maps (fills
								# any map not already set)

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


# ── Internal: Path Resolution ──────────────────────────────────────────

## Resolve a scenery path.  If the path is already absolute or starts with
## res:// / user://, return it unchanged.  Otherwise treat it as relative
## to the workspace root (the parent of the Godot project directory).
static func _resolve_scenery_path(path: String) -> String:
	if path.begins_with("/") or path.begins_with("res://") or path.begins_with("user://"):
		return path
	var base_dir: String
	if OS.has_feature("editor"):
		# In editor: workspace root is one level above the Godot project directory.
		var project_dir = ProjectSettings.globalize_path("res://")
		if project_dir.ends_with("/"):
			project_dir = project_dir.left(project_dir.length() - 1)
		base_dir = project_dir.get_base_dir()
	else:
		# Exported app: data lives alongside the .app bundle.
		# macOS: .../App.app/Contents/MacOS/App → 4× get_base_dir() → parent of .app
		var exe = OS.get_executable_path()
		base_dir = exe.get_base_dir().get_base_dir().get_base_dir().get_base_dir()
	var resolved = base_dir.path_join(path)
	print("TerrainStreamer: Resolved '%s' → '%s'" % [path, resolved])
	return resolved
