## ClipmapStreamer — Toroidal texture streaming for clipmap terrain rings.
##
## Phase 5 of the Scenery Development Plan.
##
## Instead of rebuilding entire ring textures on every floating-origin rebase,
## this manager uses toroidal (wrapping) textures.  When the camera moves,
## only the newly-exposed strip of tiles is loaded and blitted — the rest of
## the texture stays valid.
##
## Each ring has a heightmap Image and (optionally) an orthophoto Image that
## are treated as toroidal atlases.  A pixel offset tracks the current wrap
## position.  The shader reads via `fract(uv + offset)` to undo the wrap.
##
## Budget-limited I/O: at most MAX_TILES_PER_FRAME tiles are loaded per
## frame, prioritising inner rings (higher visual impact).
##
## Velocity-based pre-fetch: detect aircraft movement direction and
## pre-load tiles ahead of the camera.
##
## Floating-origin rebase: mesh positions shift as before; the toroidal
## texture offset adjusts to compensate — no data reload needed unless the
## shift exceeds one full texture width (catastrophic teleport).

extends RefCounted

# ── Configuration ──────────────────────────────────────────────────────

## Maximum tile loads across all rings per frame (budget-limited I/O).
const MAX_TILES_PER_FRAME: int = 8

## Pre-fetch distance in seconds of flight time.
const PREFETCH_SECONDS: float = 5.0

## If a rebase shift exceeds this fraction of a ring's half-extent,
## do a full rebuild instead of incremental update.
const FULL_REBUILD_THRESHOLD: float = 0.9

# ── Types ──────────────────────────────────────────────────────────────

## Per-ring streaming state.
class RingState:
	var mip_level: int
	var pixel_res: float          # metres per texture pixel
	var half_extent: float        # half-extent in metres (TEX_SIZE/2 * pixel_res)
	var tex_size: int             # texture side length (pixels)
	var tile_px: int              # pixels per source tile at this mip level

	# The Swiss LV95 coordinates that the texture is currently centered on.
	# This is the "texture center" — may differ from the terrain center
	# by up to one grid cell before a strip update is needed.
	var tex_center_e: float = 0.0
	var tex_center_n: float = 0.0

	# Toroidal pixel offset: the texture row/column that corresponds to
	# the current geographic center.  Updated when strips are blitted.
	var wrap_col: int = 0         # column offset (East axis)
	var wrap_row: int = 0         # row offset (North axis)

	# Live Image backing the GPU texture (persistent across frames).
	var height_image: Image
	var ortho_image: Image        # null if no orthophoto

	# GPU textures (updated in-place via update()).
	var height_texture: ImageTexture
	var ortho_texture: ImageTexture  # null if no orthophoto

	# Pending strip updates queued for budget-limited processing.
	var pending_strips: Array = []  # Array of {axis, direction, count}

	# Whether this ring needs a full rebuild (e.g. first load or teleport).
	var needs_full_rebuild: bool = true

# ── State ──────────────────────────────────────────────────────────────

var _rings: Array = []            # Array[RingState], indexed 0..2
var _manifests: Dictionary = {}   # mip_level → parsed manifest dict
var _scenery_path: String = ""
var _terrain_center_e: float = 0.0
var _terrain_center_n: float = 0.0
var _tex_size: int = 2048

# Velocity tracking for pre-fetch.
var _last_cam_e: float = 0.0
var _last_cam_n: float = 0.0
var _velocity_e: float = 0.0     # metres / second (Swiss E)
var _velocity_n: float = 0.0     # metres / second (Swiss N)

# Seam (Phase 4) — 1 m manifest + tile directory for Ring 0.
var _manifest_1m: Dictionary = {}
var _tile_directory_1m: String = ""
var _seam_height_image: Image
var _seam_height_texture: ImageTexture

# ── Initialisation ─────────────────────────────────────────────────────

func initialize(
	scenery_path: String,
	manifests: Dictionary,       # mip_level → manifest dict
	manifest_1m: Dictionary,     # 1 m terrain manifest (for Phase 4 seam)
	tile_directory_1m: String,
	tex_size: int,
	ring_configs: Array,         # [{mip_level, pixel_res, inner_radius, mesh_divs}, …]
	ring_half_extents: Array,    # [float, float, float]
	center_e: float,
	center_n: float
) -> void:
	_scenery_path = scenery_path
	_manifests = manifests
	_manifest_1m = manifest_1m
	_tile_directory_1m = tile_directory_1m
	_tex_size = tex_size
	_terrain_center_e = center_e
	_terrain_center_n = center_n
	_last_cam_e = center_e
	_last_cam_n = center_n

	_rings.clear()
	for i in range(ring_configs.size()):
		var cfg: Dictionary = ring_configs[i]
		var rs := RingState.new()
		rs.mip_level = cfg["mip_level"]
		rs.pixel_res = cfg["pixel_res"]
		rs.half_extent = ring_half_extents[i]
		rs.tex_size = tex_size
		rs.tile_px = int(_manifests[rs.mip_level].get("pixels_per_tile", 64))
		rs.tex_center_e = center_e
		rs.tex_center_n = center_n
		rs.wrap_col = 0
		rs.wrap_row = 0
		rs.needs_full_rebuild = true
		_rings.append(rs)


## Perform the first full build for all rings.
## Returns arrays of [height_textures, ortho_textures, seam_height_texture].
func build_all() -> Array:
	var height_textures: Array = []
	var ortho_textures: Array = []

	for i in range(_rings.size()):
		var rs: RingState = _rings[i]
		_full_rebuild_ring(rs)
		height_textures.append(rs.height_texture)
		ortho_textures.append(rs.ortho_texture)

	# Phase 4: Build seam heightmap for Ring 0.
	var seam_tex: ImageTexture = null
	if not _manifest_1m.is_empty() and _rings.size() > 0:
		seam_tex = _build_seam_heightmap(_rings[0])

	return [height_textures, ortho_textures, seam_tex]


# ── Per-frame update ───────────────────────────────────────────────────

## Call once per frame.  Processes pending strip updates (budget-limited).
## Returns true if any texture was modified this frame.
func process_frame(delta: float, cam_swiss_e: float, cam_swiss_n: float) -> bool:
	# Update velocity estimate for pre-fetch.
	if delta > 0.001:
		_velocity_e = (cam_swiss_e - _last_cam_e) / delta
		_velocity_n = (cam_swiss_n - _last_cam_n) / delta
	_last_cam_e = cam_swiss_e
	_last_cam_n = cam_swiss_n

	# Check each ring for needed strip updates (camera moved enough to
	# expose new columns/rows).
	for i in range(_rings.size()):
		_check_ring_shift(_rings[i])

	# NOTE: Pre-fetch is disabled.  Each ring's texture covers ±half_extent
	# (Ring 0: ±16 km, Ring 2: ±262 km) while rebase fires at 5 km.
	# The texture is always much larger than the rebase zone, so no
	# per-frame scrolling is needed.  _check_ring_shift handles the rare
	# case where rebase drift exceeds one pixel (small rebases).
	# Pre-fetch will be reconsidered if/when textures are shrunk to save
	# VRAM or if continuous camera tracking replaces rebase-driven drift.

	# Process pending strips with budget.
	var tiles_loaded := 0
	var any_updated := false
	# Prioritise inner rings (lower index = higher priority).
	for i in range(_rings.size()):
		if tiles_loaded >= MAX_TILES_PER_FRAME:
			break
		var rs: RingState = _rings[i]
		while rs.pending_strips.size() > 0 and tiles_loaded < MAX_TILES_PER_FRAME:
			var strip: Dictionary = rs.pending_strips.pop_front()
			var loaded := _apply_strip(rs, strip)
			tiles_loaded += loaded
			any_updated = true

	# Upload modified images to GPU.
	if any_updated:
		for i in range(_rings.size()):
			_upload_ring(_rings[i])

	return any_updated


# ── Rebase handling ────────────────────────────────────────────────────

## Called when floating-origin rebases.
## Rebuilds all ring textures centered on the new position and resets
## toroidal offsets.  Returns true (always triggers a full rebuild).
##
## Rebases happen every ~5 km (~5 s of flight) and a full texture rebuild
## takes <1 ms per ring, so there's no benefit to incremental scrolling.
## The toroidal strip infrastructure is retained for future use if textures
## are shrunk to save VRAM.
func rebase(new_center_e: float, new_center_n: float) -> bool:
	_terrain_center_e = new_center_e
	_terrain_center_n = new_center_n

	for i in range(_rings.size()):
		_full_rebuild_ring(_rings[i])
		_upload_ring(_rings[i])

	# Rebuild seam for Ring 0.
	if not _manifest_1m.is_empty() and _rings.size() > 0:
		_build_seam_heightmap(_rings[0])

	return true


## Get the toroidal UV offset for a ring (used by shader).
## Returns Vector2(col_offset / tex_size, row_offset / tex_size).
func get_uv_offset(ring_idx: int) -> Vector2:
	if ring_idx < 0 or ring_idx >= _rings.size():
		return Vector2.ZERO
	var rs: RingState = _rings[ring_idx]
	return Vector2(
		float(rs.wrap_col) / float(rs.tex_size),
		float(rs.wrap_row) / float(rs.tex_size)
	)


func get_height_texture(ring_idx: int) -> ImageTexture:
	if ring_idx < 0 or ring_idx >= _rings.size():
		return null
	return _rings[ring_idx].height_texture


func get_ortho_texture(ring_idx: int) -> ImageTexture:
	if ring_idx < 0 or ring_idx >= _rings.size():
		return null
	return _rings[ring_idx].ortho_texture


func get_seam_height_texture() -> ImageTexture:
	return _seam_height_texture


# ── Internal: ring shift detection ─────────────────────────────────────

func _check_ring_shift(rs: RingState) -> void:
	## Check if the terrain center has drifted enough from the texture center
	## to require scrolling a strip of tiles.

	if rs.needs_full_rebuild:
		return  # Will be rebuilt in full.

	# How many pixels has the center drifted in each axis?
	var drift_e := _terrain_center_e - rs.tex_center_e
	var drift_n := _terrain_center_n - rs.tex_center_n

	var drift_cols := int(drift_e / rs.pixel_res)
	var drift_rows := int(drift_n / rs.pixel_res)

	# Queue column strips (East/West movement).
	if drift_cols != 0:
		# Positive drift_cols = center moved East → expose new East columns.
		var direction := 1 if drift_cols > 0 else -1
		var count := absi(drift_cols)
		rs.pending_strips.append({
			"axis": "col",
			"direction": direction,
			"count": count
		})
		# Advance texture center by the consumed drift.
		rs.tex_center_e += float(drift_cols) * rs.pixel_res

	# Queue row strips (North/South movement).
	if drift_rows != 0:
		var direction := 1 if drift_rows > 0 else -1
		var count := absi(drift_rows)
		rs.pending_strips.append({
			"axis": "row",
			"direction": direction,
			"count": count
		})
		rs.tex_center_n += float(drift_rows) * rs.pixel_res


# ── Internal: pre-fetch ────────────────────────────────────────────────

func _queue_prefetch() -> void:
	## If the aircraft is moving fast, queue additional strips ahead
	## so the texture is ready before the camera arrives.

	var speed := sqrt(_velocity_e * _velocity_e + _velocity_n * _velocity_n)
	if speed < 10.0:
		return  # Slow/stopped — no pre-fetch needed.

	for i in range(_rings.size()):
		var rs: RingState = _rings[i]
		if rs.needs_full_rebuild:
			continue

		# How many pixels will be traversed in PREFETCH_SECONDS?
		var prefetch_cols := int(absf(_velocity_e) * PREFETCH_SECONDS / rs.pixel_res)
		var prefetch_rows := int(absf(_velocity_n) * PREFETCH_SECONDS / rs.pixel_res)

		# Only queue if significant (more than 1 pixel).
		if prefetch_cols > 1:
			var dir := 1 if _velocity_e > 0 else -1
			# Don't duplicate if already pending.
			var already_pending := false
			for strip in rs.pending_strips:
				if strip["axis"] == "col" and strip["direction"] == dir:
					already_pending = true
					break
			if not already_pending:
				rs.pending_strips.append({
					"axis": "col",
					"direction": dir,
					"count": mini(prefetch_cols, rs.tex_size / 4)
				})

		if prefetch_rows > 1:
			var dir := 1 if _velocity_n > 0 else -1
			var already_pending := false
			for strip in rs.pending_strips:
				if strip["axis"] == "row" and strip["direction"] == dir:
					already_pending = true
					break
			if not already_pending:
				rs.pending_strips.append({
					"axis": "row",
					"direction": dir,
					"count": mini(prefetch_rows, rs.tex_size / 4)
				})


# ── Internal: apply strip ──────────────────────────────────────────────

func _apply_strip(rs: RingState, strip: Dictionary) -> int:
	## Blit a strip of columns or rows into the toroidal texture.
	## Returns the number of tiles loaded (for budget tracking).

	var axis: String = strip["axis"]
	var direction: int = strip["direction"]
	var count: int = strip["count"]
	var tiles_loaded := 0

	# Clamp count to budget-reasonable size.
	count = mini(count, rs.tex_size)

	if axis == "col":
		tiles_loaded = _blit_columns(rs, direction, count)
	elif axis == "row":
		tiles_loaded = _blit_rows(rs, direction, count)

	return tiles_loaded


func _blit_columns(rs: RingState, direction: int, count: int) -> int:
	## Blit `count` new columns on the East (direction=1) or West
	## (direction=-1) edge of the toroidal texture.
	## Updates wrap_col accordingly.

	var manifest: Dictionary = _manifests[rs.mip_level]
	var tiles: Dictionary = manifest["tiles"]
	var tiles_loaded := 0

	for c in range(count):
		# Determine which column in the texture to overwrite.
		var col: int
		if direction > 0:
			# Moving East: overwrite the column at wrap_col (leftmost visible).
			col = rs.wrap_col % rs.tex_size
			rs.wrap_col = (rs.wrap_col + 1) % rs.tex_size
		else:
			# Moving West: overwrite the column just before wrap_col.
			rs.wrap_col = (rs.wrap_col - 1 + rs.tex_size) % rs.tex_size
			col = rs.wrap_col % rs.tex_size

		# Compute the Swiss E coordinate this column represents.
		# Texture column 0 at wrap_col=0 is the East edge of the texture area.
		# The geographic East edge = tex_center_e + half_extent.
		# Column `col` relative to wrap_col covers:
		#   swiss_e = tex_east - (unwrapped_col * pixel_res)
		# where unwrapped_col = (col - wrap_col + tex_size) % tex_size
		var unwrapped := (col - rs.wrap_col + rs.tex_size) % rs.tex_size
		var tex_east := rs.tex_center_e + rs.half_extent
		var swiss_e_at_col := tex_east - float(unwrapped) * rs.pixel_res

		# Fill this column from all tiles that intersect it.
		tiles_loaded += _fill_column(rs, col, swiss_e_at_col, tiles)

	return tiles_loaded


func _blit_rows(rs: RingState, direction: int, count: int) -> int:
	## Blit `count` new rows on the North (direction=1) or South
	## (direction=-1) edge of the toroidal texture.

	var manifest: Dictionary = _manifests[rs.mip_level]
	var tiles: Dictionary = manifest["tiles"]
	var tiles_loaded := 0

	for r in range(count):
		var row: int
		if direction > 0:
			# Moving North: overwrite row at wrap_row (southmost visible).
			row = rs.wrap_row % rs.tex_size
			rs.wrap_row = (rs.wrap_row + 1) % rs.tex_size
		else:
			rs.wrap_row = (rs.wrap_row - 1 + rs.tex_size) % rs.tex_size
			row = rs.wrap_row % rs.tex_size

		var unwrapped := (row - rs.wrap_row + rs.tex_size) % rs.tex_size
		var tex_south := rs.tex_center_n - rs.half_extent
		var swiss_n_at_row := tex_south + float(unwrapped) * rs.pixel_res

		tiles_loaded += _fill_row(rs, row, swiss_n_at_row, tiles)

	return tiles_loaded


func _fill_column(rs: RingState, col: int, swiss_e: float,
		tiles: Dictionary) -> int:
	## Fill a single column in the heightmap (and orthophoto) image
	## by reading pixels from all tiles that cover this E coordinate.

	var loaded := 0

	# The column covers a vertical strip of 1-pixel width at swiss_e,
	# spanning the full N range of the texture.
	var tex_south := rs.tex_center_n - rs.half_extent
	var tex_north := rs.tex_center_n + rs.half_extent

	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		# Tile covers [tile_e, tile_e + 1024] in E.
		if swiss_e < tile_e or swiss_e > tile_e + 1024.0:
			continue
		# Tile covers [tile_n, tile_n + 1024] in N.
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		# Find which source pixel column this swiss_e falls in.
		# Source tile pixel (0,0) is at the SE corner.
		# Col direction: East → West, so col 0 = tile_e + 1024,
		# col (tile_px-1) = tile_e.
		var src_col := int(roundf((tile_e + 1024.0 - swiss_e) / rs.pixel_res))
		src_col = clampi(src_col, 0, rs.tile_px - 1)

		# Load tile heightmap.
		var tile_data := _load_terrain_tile(rs.mip_level, tile["file"], rs.tile_px)
		if tile_data.is_empty():
			continue
		loaded += 1

		# Blit the relevant column portion into the toroidal image.
		var tile_bytes := tile_data.to_byte_array()
		var tile_img := Image.create_from_data(rs.tile_px, rs.tile_px, false,
			Image.FORMAT_RF, tile_bytes)

		# Determine row range in the output texture that this tile covers.
		var tile_row_start := int(roundf((tile_n - tex_south) / rs.pixel_res))
		for src_row in range(rs.tile_px):
			var dst_row := tile_row_start + src_row
			if dst_row < 0 or dst_row >= rs.tex_size:
				continue
			# Apply toroidal wrap to destination row.
			var wrapped_row := (dst_row + rs.wrap_row) % rs.tex_size
			var h := tile_img.get_pixel(src_col, src_row).r
			rs.height_image.set_pixel(col, wrapped_row, Color(h, 0.0, 0.0, 1.0))

	# Orthophoto: same logic but with ortho tiles.
	if rs.ortho_image:
		loaded += _fill_column_ortho(rs, col, swiss_e, tex_south, tex_north)

	return loaded


func _fill_column_ortho(rs: RingState, col: int, swiss_e: float,
		tex_south: float, tex_north: float) -> int:
	## Fill a single column of the orthophoto image.
	var ortho_manifest := _load_ortho_manifest(rs.mip_level)
	if ortho_manifest.is_empty():
		return 0

	var tiles: Dictionary = ortho_manifest.get("tiles", {})
	var tile_px: int = ortho_manifest.get("pixels_per_tile", rs.tile_px)
	var loaded := 0

	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		if swiss_e < tile_e or swiss_e > tile_e + 1024.0:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		var src_col := int(roundf((tile_e + 1024.0 - swiss_e) / rs.pixel_res))
		src_col = clampi(src_col, 0, tile_px - 1)

		var tile_img := _load_ortho_tile(rs.mip_level, tile["file"], tile_px)
		if tile_img == null:
			continue
		loaded += 1

		var tile_row_start := int(roundf((tile_n - tex_south) / rs.pixel_res))
		for src_row in range(tile_px):
			var dst_row := tile_row_start + src_row
			if dst_row < 0 or dst_row >= rs.tex_size:
				continue
			var wrapped_row := (dst_row + rs.wrap_row) % rs.tex_size
			var pixel := tile_img.get_pixel(src_col, src_row)
			rs.ortho_image.set_pixel(col, wrapped_row, pixel)

	return loaded


func _fill_row(rs: RingState, row: int, swiss_n: float,
		tiles: Dictionary) -> int:
	## Fill a single row in the heightmap (and orthophoto) image.
	var loaded := 0

	var tex_east := rs.tex_center_e + rs.half_extent
	var tex_west := rs.tex_center_e - rs.half_extent

	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		# Tile covers [tile_n, tile_n + 1024] in N.
		if swiss_n < tile_n or swiss_n > tile_n + 1024.0:
			continue
		# Tile covers [tile_e, tile_e + 1024] in E.
		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue

		# Source row in tile: row 0 = south edge (tile_n).
		var src_row := int(roundf((swiss_n - tile_n) / rs.pixel_res))
		src_row = clampi(src_row, 0, rs.tile_px - 1)

		var tile_data := _load_terrain_tile(rs.mip_level, tile["file"], rs.tile_px)
		if tile_data.is_empty():
			continue
		loaded += 1

		var tile_bytes := tile_data.to_byte_array()
		var tile_img := Image.create_from_data(rs.tile_px, rs.tile_px, false,
			Image.FORMAT_RF, tile_bytes)

		# Determine col range in output texture this tile covers.
		var tile_col_start := int(roundf((tex_east - tile_e - 1024.0) / rs.pixel_res))
		for src_col in range(rs.tile_px):
			var dst_col := tile_col_start + src_col
			if dst_col < 0 or dst_col >= rs.tex_size:
				continue
			var wrapped_col := (dst_col + rs.wrap_col) % rs.tex_size
			var h := tile_img.get_pixel(src_col, src_row).r
			rs.height_image.set_pixel(wrapped_col, row, Color(h, 0.0, 0.0, 1.0))

	# Orthophoto row.
	if rs.ortho_image:
		loaded += _fill_row_ortho(rs, row, swiss_n, tex_east, tex_west)

	return loaded


func _fill_row_ortho(rs: RingState, row: int, swiss_n: float,
		tex_east: float, tex_west: float) -> int:
	var ortho_manifest := _load_ortho_manifest(rs.mip_level)
	if ortho_manifest.is_empty():
		return 0

	var tiles: Dictionary = ortho_manifest.get("tiles", {})
	var tile_px: int = ortho_manifest.get("pixels_per_tile", rs.tile_px)
	var loaded := 0

	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		if swiss_n < tile_n or swiss_n > tile_n + 1024.0:
			continue
		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue

		var src_row := int(roundf((swiss_n - tile_n) / rs.pixel_res))
		src_row = clampi(src_row, 0, tile_px - 1)

		var tile_img := _load_ortho_tile(rs.mip_level, tile["file"], tile_px)
		if tile_img == null:
			continue
		loaded += 1

		var tile_col_start := int(roundf((tex_east - tile_e - 1024.0) / rs.pixel_res))
		for src_col in range(tile_px):
			var dst_col := tile_col_start + src_col
			if dst_col < 0 or dst_col >= rs.tex_size:
				continue
			var wrapped_col := (dst_col + rs.wrap_col) % rs.tex_size
			var pixel := tile_img.get_pixel(src_col, src_row)
			rs.ortho_image.set_pixel(wrapped_col, row, pixel)

	return loaded


# ── Internal: full rebuild ─────────────────────────────────────────────

func _full_rebuild_ring(rs: RingState) -> void:
	## Build the entire heightmap + orthophoto from scratch.
	## Used for initial load and catastrophic teleport.

	rs.tex_center_e = _terrain_center_e
	rs.tex_center_n = _terrain_center_n
	rs.wrap_col = 0
	rs.wrap_row = 0
	rs.pending_strips.clear()
	rs.needs_full_rebuild = false

	# Heightmap.
	rs.height_image = _assemble_full_heightmap(rs)
	if rs.height_texture:
		rs.height_texture.update(rs.height_image)
	else:
		rs.height_texture = ImageTexture.create_from_image(rs.height_image)

	# Orthophoto.
	var ortho_img := _assemble_full_ortho(rs)
	if ortho_img:
		rs.ortho_image = ortho_img
		if rs.ortho_texture:
			rs.ortho_texture.update(ortho_img)
		else:
			rs.ortho_texture = ImageTexture.create_from_image(ortho_img)
	else:
		rs.ortho_image = null
		rs.ortho_texture = null

	print("ClipmapStreamer: Ring mip%d fully rebuilt (center E=%.0f N=%.0f)" % [
		rs.mip_level, rs.tex_center_e, rs.tex_center_n])


func _assemble_full_heightmap(rs: RingState) -> Image:
	## Same logic as terrain_clipmap._assemble_heightmap_texture — builds
	## a TEX_SIZE × TEX_SIZE float32 image from mip tiles.

	var img := Image.create(rs.tex_size, rs.tex_size, false, Image.FORMAT_RF)
	var manifest: Dictionary = _manifests[rs.mip_level]
	var tiles: Dictionary = manifest["tiles"]
	var tile_px: int = rs.tile_px

	var tex_east: float = rs.tex_center_e + rs.half_extent
	var tex_south: float = rs.tex_center_n - rs.half_extent
	var tex_west: float = tex_east - float(rs.tex_size) * rs.pixel_res
	var tex_north: float = tex_south + float(rs.tex_size) * rs.pixel_res

	var tiles_blitted := 0
	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		var col_offset := int(roundf((tex_east - tile_e - 1024.0) / rs.pixel_res))
		var row_offset := int(roundf((tile_n - tex_south) / rs.pixel_res))

		var tile_data := _load_terrain_tile(rs.mip_level, tile["file"], tile_px)
		if tile_data.is_empty():
			continue

		var tile_bytes := tile_data.to_byte_array()
		var tile_img := Image.create_from_data(tile_px, tile_px, false,
			Image.FORMAT_RF, tile_bytes)

		var src_rect := Rect2i(0, 0, tile_px, tile_px)
		var dst_pos := Vector2i(col_offset, row_offset)

		if dst_pos.x < 0:
			src_rect.position.x = -dst_pos.x
			src_rect.size.x += dst_pos.x
			dst_pos.x = 0
		if dst_pos.y < 0:
			src_rect.position.y = -dst_pos.y
			src_rect.size.y += dst_pos.y
			dst_pos.y = 0
		if dst_pos.x + src_rect.size.x > rs.tex_size:
			src_rect.size.x = rs.tex_size - dst_pos.x
		if dst_pos.y + src_rect.size.y > rs.tex_size:
			src_rect.size.y = rs.tex_size - dst_pos.y

		if src_rect.size.x > 0 and src_rect.size.y > 0:
			img.blit_rect(tile_img, src_rect, dst_pos)
		tiles_blitted += 1

	print("  Heightmap mip%d: %d tiles (center E=%.0f N=%.0f)" % [
		rs.mip_level, tiles_blitted, rs.tex_center_e, rs.tex_center_n])
	return img


func _assemble_full_ortho(rs: RingState) -> Image:
	## Build full orthophoto image. Returns null if no data.

	var ortho_manifest := _load_ortho_manifest(rs.mip_level)
	if ortho_manifest.is_empty():
		return null

	var img := Image.create(rs.tex_size, rs.tex_size, false, Image.FORMAT_RGB8)
	img.fill(Color(0.4, 0.45, 0.35))

	var tiles: Dictionary = ortho_manifest["tiles"]
	var tile_px: int = ortho_manifest.get("pixels_per_tile", rs.tile_px)

	var tex_east: float = rs.tex_center_e + rs.half_extent
	var tex_south: float = rs.tex_center_n - rs.half_extent
	var tex_west: float = tex_east - float(rs.tex_size) * rs.pixel_res
	var tex_north: float = tex_south + float(rs.tex_size) * rs.pixel_res

	var tiles_blitted := 0
	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		var col_offset := int(roundf((tex_east - tile_e - 1024.0) / rs.pixel_res))
		var row_offset := int(roundf((tile_n - tex_south) / rs.pixel_res))

		var tile_img := _load_ortho_tile(rs.mip_level, tile["file"], tile_px)
		if tile_img == null:
			continue

		var src_rect := Rect2i(0, 0, tile_px, tile_px)
		var dst_pos := Vector2i(col_offset, row_offset)

		if dst_pos.x < 0:
			src_rect.position.x = -dst_pos.x
			src_rect.size.x += dst_pos.x
			dst_pos.x = 0
		if dst_pos.y < 0:
			src_rect.position.y = -dst_pos.y
			src_rect.size.y += dst_pos.y
			dst_pos.y = 0
		if dst_pos.x + src_rect.size.x > rs.tex_size:
			src_rect.size.x = rs.tex_size - dst_pos.x
		if dst_pos.y + src_rect.size.y > rs.tex_size:
			src_rect.size.y = rs.tex_size - dst_pos.y

		if src_rect.size.x > 0 and src_rect.size.y > 0:
			img.blit_rect(tile_img, src_rect, dst_pos)
			tiles_blitted += 1

	if tiles_blitted == 0:
		return null

	print("  Ortho mip%d: %d tiles" % [rs.mip_level, tiles_blitted])
	return img


# ── Internal: seam heightmap (Phase 4) ─────────────────────────────────

func _build_seam_heightmap(rs: RingState) -> ImageTexture:
	## Build seam heightmap from 1 m tiles (same as terrain_clipmap's
	## _assemble_seam_heightmap but updates persistent images).

	var tiles: Dictionary = _manifest_1m.get("tiles", {})
	if tiles.is_empty():
		return null

	var tile_src_px: int = int(_manifest_1m.get("pixels_per_tile", 1024))
	var out_px: int = int(1024.0 / rs.pixel_res)

	var img := Image.create(rs.tex_size, rs.tex_size, false, Image.FORMAT_RF)

	var tex_east: float = rs.tex_center_e + rs.half_extent
	var tex_south: float = rs.tex_center_n - rs.half_extent
	var tex_west: float = tex_east - float(rs.tex_size) * rs.pixel_res
	var tex_north: float = tex_south + float(rs.tex_size) * rs.pixel_res

	# Use the same SEAM_TILE_MAX_DIST as terrain_clipmap.
	const SEAM_TILE_MAX_DIST: float = 12000.0

	var tiles_processed := 0
	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		var tile_cx: float = _terrain_center_e - (tile_e + 512.0)
		var tile_cz: float = (tile_n + 512.0) - _terrain_center_n
		var tile_dist: float = sqrt(tile_cx * tile_cx + tile_cz * tile_cz)
		if tile_dist > SEAM_TILE_MAX_DIST:
			continue

		var tile_img := _load_1m_tile(tile["file"], tile_src_px)
		if tile_img == null:
			continue
		tile_img.resize(out_px, out_px, Image.INTERPOLATE_LANCZOS)

		var col_offset := int(roundf((tex_east - tile_e - 1024.0) / rs.pixel_res))
		var row_offset := int(roundf((tile_n - tex_south) / rs.pixel_res))

		var src_rect := Rect2i(0, 0, out_px, out_px)
		var dst_pos := Vector2i(col_offset, row_offset)

		if dst_pos.x < 0:
			src_rect.position.x = -dst_pos.x
			src_rect.size.x += dst_pos.x
			dst_pos.x = 0
		if dst_pos.y < 0:
			src_rect.position.y = -dst_pos.y
			src_rect.size.y += dst_pos.y
			dst_pos.y = 0
		if dst_pos.x + src_rect.size.x > rs.tex_size:
			src_rect.size.x = rs.tex_size - dst_pos.x
		if dst_pos.y + src_rect.size.y > rs.tex_size:
			src_rect.size.y = rs.tex_size - dst_pos.y

		if src_rect.size.x > 0 and src_rect.size.y > 0:
			img.blit_rect(tile_img, src_rect, dst_pos)
			tiles_processed += 1

	_seam_height_image = img
	if _seam_height_texture:
		_seam_height_texture.update(img)
	else:
		_seam_height_texture = ImageTexture.create_from_image(img)

	print("  Seam heightmap: %d tiles (1 m → %.0f m/px)" % [
		tiles_processed, rs.pixel_res])
	return _seam_height_texture


# ── Internal: GPU upload ───────────────────────────────────────────────

func _upload_ring(rs: RingState) -> void:
	## Upload the modified Image to GPU via ImageTexture.update().
	## This is <1 ms for a 2048×2048 texture.
	if rs.height_texture and rs.height_image:
		rs.height_texture.update(rs.height_image)
	if rs.ortho_texture and rs.ortho_image:
		rs.ortho_texture.update(rs.ortho_image)


# ── Internal: tile I/O ─────────────────────────────────────────────────

func _load_terrain_tile(mip_level: int, filename: String,
		tile_px: int) -> PackedFloat32Array:
	var dir_path := _scenery_path + "/terrain/mip%d" % mip_level
	var path := dir_path + "/" + filename
	var file := FileAccess.open(path, FileAccess.READ)
	if not file:
		return PackedFloat32Array()
	var expected_bytes := tile_px * tile_px * 4
	var raw := file.get_buffer(expected_bytes)
	file.close()
	if raw.size() != expected_bytes:
		return PackedFloat32Array()
	return raw.to_float32_array()


func _load_ortho_tile(mip_level: int, filename: String,
		tile_px: int) -> Image:
	var dir_path := _scenery_path + "/orthophoto/mip%d" % mip_level
	var path := dir_path + "/" + filename
	var img := Image.new()
	var err := img.load(path)
	if err != OK:
		return null
	if img.get_width() != tile_px or img.get_height() != tile_px:
		img.resize(tile_px, tile_px, Image.INTERPOLATE_LANCZOS)
	if img.get_format() != Image.FORMAT_RGB8:
		img.convert(Image.FORMAT_RGB8)
	return img


func _load_1m_tile(filename: String, tile_px: int) -> Image:
	var path := _tile_directory_1m + "/" + filename
	var file := FileAccess.open(path, FileAccess.READ)
	if not file:
		return null
	var expected_bytes := tile_px * tile_px * 4
	var raw := file.get_buffer(expected_bytes)
	file.close()
	if raw.size() != expected_bytes:
		return null
	return Image.create_from_data(tile_px, tile_px, false, Image.FORMAT_RF, raw)


# Cached ortho manifests to avoid repeated disk reads.
var _ortho_manifest_cache: Dictionary = {}

func _load_ortho_manifest(mip_level: int) -> Dictionary:
	if mip_level in _ortho_manifest_cache:
		return _ortho_manifest_cache[mip_level]

	var path := _scenery_path + "/orthophoto/mip%d/manifest.json" % mip_level
	var file := FileAccess.open(path, FileAccess.READ)
	if not file:
		_ortho_manifest_cache[mip_level] = {}
		return {}
	var json := JSON.new()
	var err := json.parse(file.get_as_text())
	file.close()
	if err != OK:
		_ortho_manifest_cache[mip_level] = {}
		return {}
	_ortho_manifest_cache[mip_level] = json.data
	return json.data
