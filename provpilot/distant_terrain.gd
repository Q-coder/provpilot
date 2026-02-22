## DistantTerrain — Low-detail terrain mesh beyond the Terrain3D region grid
##
## Renders a large terrain mesh covering all of Switzerland (or the area covered
## by the terrain tiles) at 256m per vertex resolution. This provides a visible
## horizon extending 100-200km beyond the detailed Terrain3D terrain, preventing
## the abrupt terrain cutoff visible when flying at altitude.
##
## Setup:
##   1. Run: python scripts/generate_distant_terrain.py
##      This reads the 42,400 high-res tiles and produces:
##        - scenery/terrain/distant_terrain.raw (float32 heightmap)
##        - scenery/terrain/distant_terrain_color.png (elevation color map)
##        - scenery/terrain/distant_terrain_meta.json (metadata)
##   2. Add a DistantTerrain node to the scene (child of root, top_level=true)
##   3. Set scenery_path to "scenery/terrain"
##
## The mesh is generated at startup and moves with the floating origin.
## It sits 2m below the Terrain3D surface where they overlap to avoid z-fighting.

extends Node3D

## Path to the scenery directory containing distant_terrain_meta.json etc.
@export var scenery_path: String = "scenery/terrain"

## Vertical offset to avoid z-fighting with Terrain3D (negative = below)
@export var height_offset: float = -2.0

## Maximum mesh extent from the center in meters (limits vertex count)
## Set to 0 for no limit (render entire available area)
@export var max_extent: float = 0.0

## Chunk size in vertices for mesh generation (splits into smaller meshes
## to avoid the 65536 vertex limit per Godot mesh surface)
const CHUNK_VERTS: int = 250  # 250×250 = 62,500 vertices per chunk

# Internal state
var _meta: Dictionary = {}
var _heightmap: PackedFloat32Array
var _cols: int = 0
var _rows: int = 0
var _resolution: float = 256.0
var _swiss_e_min: float = 0.0
var _swiss_n_min: float = 0.0
var _swiss_e_max: float = 0.0
var _swiss_n_max: float = 0.0
var _mesh_nodes: Array[MeshInstance3D] = []
var _material: ShaderMaterial
var _color_texture: ImageTexture
var _terrain_center_e: float = 0.0
var _terrain_center_n: float = 0.0
var _initialized: bool = false
var _pending_chunks: Array = []    # Deferred mesh generation queue
const CHUNKS_PER_FRAME: int = 2    # Mesh chunks generated per frame


func _ready() -> void:
	if Engine.is_editor_hint():
		return

	# Resolve scenery path (same logic as TerrainStreamer)
	scenery_path = _resolve_scenery_path(scenery_path)

	# Get terrain center from TerrainStreamer or parent
	_terrain_center_e = 2682880.0
	_terrain_center_n = 1283072.0
	var streamer = get_parent().get_node_or_null("TerrainStreamer")
	if streamer:
		_terrain_center_e = float(streamer.terrain_center_e)
		_terrain_center_n = float(streamer.terrain_center_n)

	# Load metadata
	if not _load_metadata():
		push_error("DistantTerrain: Failed to load metadata")
		return

	# Load heightmap
	if not _load_heightmap():
		push_error("DistantTerrain: Failed to load heightmap")
		return

	# Load color texture
	_load_color_texture()

	# Create shader material
	_create_material()

	# Prepare deferred mesh generation (spreads work across frames)
	_prepare_mesh_chunks()

	_initialized = true
	print("DistantTerrain: Ready — %d×%d pixels, %d chunks queued" %
		  [_cols, _rows, _pending_chunks.size()])


## Update terrain center when floating origin rebases.
func rebase_center(new_center_e: float, new_center_n: float) -> void:
	if not _initialized:
		return

	var delta_e = new_center_e - _terrain_center_e
	var delta_n = new_center_n - _terrain_center_n

	_terrain_center_e = new_center_e
	_terrain_center_n = new_center_n

	# Shift all mesh nodes by the delta (in Godot coords)
	# Godot X = center_e - swiss_e  →  shift_x = +delta_e
	# Godot Z = swiss_n - center_n  →  shift_z = -delta_n
	for mesh_node in _mesh_nodes:
		mesh_node.position.x += delta_e
		mesh_node.position.z -= delta_n


# ── Data Loading ────────────────────────────────────────────────────────

func _load_metadata() -> bool:
	var meta_path = scenery_path + "/distant_terrain_meta.json"
	var file = FileAccess.open(meta_path, FileAccess.READ)
	if not file:
		push_warning("DistantTerrain: No metadata at '%s' — run generate_distant_terrain.py first" % meta_path)
		return false

	var json = JSON.new()
	var err = json.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_error("DistantTerrain: Bad metadata JSON: %s" % json.get_error_message())
		return false

	_meta = json.data
	_cols = int(_meta["cols"])
	_rows = int(_meta["rows"])
	_resolution = float(_meta["resolution_m"])
	_swiss_e_min = float(_meta["swiss_e_min"])
	_swiss_n_min = float(_meta["swiss_n_min"])
	_swiss_e_max = float(_meta["swiss_e_max"])
	_swiss_n_max = float(_meta["swiss_n_max"])

	print("DistantTerrain: Grid %d×%d at %dm/px, Swiss E[%.0f, %.0f] N[%.0f, %.0f]" %
		  [_cols, _rows, _resolution, _swiss_e_min, _swiss_e_max, _swiss_n_min, _swiss_n_max])
	return true


func _load_heightmap() -> bool:
	var heightmap_path = scenery_path + "/" + str(_meta.get("heightmap_file", "distant_terrain.raw"))
	var file = FileAccess.open(heightmap_path, FileAccess.READ)
	if not file:
		push_error("DistantTerrain: Cannot open heightmap '%s'" % heightmap_path)
		return false

	var expected_bytes = _cols * _rows * 4
	var bytes = file.get_buffer(expected_bytes)
	file.close()

	if bytes.size() != expected_bytes:
		push_error("DistantTerrain: Heightmap wrong size %d (expected %d)" %
				   [bytes.size(), expected_bytes])
		return false

	_heightmap = bytes.to_float32_array()
	print("DistantTerrain: Loaded heightmap (%d values, %.1f MB)" %
		  [_heightmap.size(), bytes.size() / 1048576.0])
	return true


func _load_color_texture() -> void:
	var color_path = scenery_path + "/" + str(_meta.get("color_file", "distant_terrain_color.png"))

	# Try loading as PNG
	var img = Image.new()
	var err = img.load(color_path)
	if err != OK:
		push_warning("DistantTerrain: Cannot load color map '%s', using procedural colors" % color_path)
		_create_procedural_color_texture()
		return

	_color_texture = ImageTexture.create_from_image(img)
	print("DistantTerrain: Loaded color texture %d×%d" % [img.get_width(), img.get_height()])


func _create_procedural_color_texture() -> void:
	# Generate a simple elevation gradient texture (fallback)
	var img = Image.create(256, 1, false, Image.FORMAT_RGB8)
	for x in range(256):
		var t = float(x) / 255.0
		var color: Color
		if t < 0.1:
			color = Color(0.16, 0.24, 0.39)  # Water blue
		elif t < 0.2:
			color = Color(0.36, 0.55, 0.24)  # Valley green
		elif t < 0.35:
			color = Color(0.24, 0.48, 0.16)  # Forest green
		elif t < 0.5:
			color = Color(0.48, 0.71, 0.28)  # Alpine meadow
		elif t < 0.65:
			color = Color(0.66, 0.56, 0.38)  # Sub-alpine
		elif t < 0.8:
			color = Color(0.55, 0.55, 0.48)  # Rock
		elif t < 0.9:
			color = Color(0.69, 0.66, 0.60)  # High alpine
		else:
			color = Color(0.91, 0.91, 0.91)  # Snow
		img.set_pixel(x, 0, color)

	_color_texture = ImageTexture.create_from_image(img)


# ── Material ────────────────────────────────────────────────────────────

func _create_material() -> void:
	var shader = load("res://shaders/distant_terrain.gdshader")
	if not shader:
		push_warning("DistantTerrain: Shader not found, using fallback material")
		return

	_material = ShaderMaterial.new()
	_material.shader = shader
	if _color_texture:
		_material.set_shader_parameter("color_texture", _color_texture)
	# Near clip: hide the mesh within the Terrain3D loaded area to prevent overlap.
	# load_radius=8 → 8×1024 = 8,192m along axes, but aircraft can be up to
	# ~512m from tile center, so worst-case extent is ~7,680m. Use 6km with
	# 2km fade (fully opaque at 8km) to ensure solid overlap and no visible gap.
	_material.set_shader_parameter("near_clip", 6000.0)
	_material.set_shader_parameter("near_fade_width", 2000.0)
	# Fog: starts at 80km, full fog at 250km — clear weather visibility
	_material.set_shader_parameter("fog_start", 80000.0)
	_material.set_shader_parameter("fog_end", 250000.0)
	_material.set_shader_parameter("fog_color", Color(0.75, 0.8, 0.88))
	_material.set_shader_parameter("haze_color", Color(0.7, 0.75, 0.85))
	# Very light haze: at 100km → 33% tint, at 150km → 45% tint
	_material.set_shader_parameter("haze_density", 0.000004)
	_material.set_shader_parameter("brightness", 1.1)


# ── Mesh Generation (deferred) ─────────────────────────────────────────

func _process(_delta: float) -> void:
	if _pending_chunks.is_empty():
		return

	# Generate a few chunks per frame to spread the work
	var chunks_this_frame := 0
	while not _pending_chunks.is_empty() and chunks_this_frame < CHUNKS_PER_FRAME:
		var chunk_info = _pending_chunks.pop_front()
		var mesh_inst = _generate_chunk(
			chunk_info["col_start"], chunk_info["row_start"],
			chunk_info["col_end"], chunk_info["row_end"]
		)
		if mesh_inst:
			add_child(mesh_inst)
			_mesh_nodes.append(mesh_inst)
		chunks_this_frame += 1

	if _pending_chunks.is_empty():
		print("DistantTerrain: All %d mesh chunks generated" % _mesh_nodes.size())


func _prepare_mesh_chunks() -> void:
	var chunks_x = ceili(float(_cols) / CHUNK_VERTS)
	var chunks_z = ceili(float(_rows) / CHUNK_VERTS)
	print("DistantTerrain: Preparing %d×%d chunks (%d×%d verts each)" %
		  [chunks_x, chunks_z, CHUNK_VERTS, CHUNK_VERTS])

	_pending_chunks.clear()
	for cx in range(chunks_x):
		for cz in range(chunks_z):
			var x_start = cx * CHUNK_VERTS
			var z_start = cz * CHUNK_VERTS
			var x_end = mini(x_start + CHUNK_VERTS, _cols)
			var z_end = mini(z_start + CHUNK_VERTS, _rows)
			_pending_chunks.append({
				"col_start": x_start,
				"row_start": z_start,
				"col_end": x_end,
				"row_end": z_end
			})


func _generate_chunk(col_start: int, row_start: int, col_end: int, row_end: int) -> MeshInstance3D:
	var w = col_end - col_start
	var h = row_end - row_start

	if w < 2 or h < 2:
		return null

	# Check if this chunk has any valid (non-NaN) height data
	var has_data = false
	for r in range(row_start, row_end, max(1, h / 4)):
		for c in range(col_start, col_end, max(1, w / 4)):
			var idx = r * _cols + c
			if idx < _heightmap.size() and not is_nan(_heightmap[idx]):
				has_data = true
				break
		if has_data:
			break

	if not has_data:
		return null

	var st = SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	# UV span for this chunk within the full heightmap
	var uv_x0 = float(col_start) / float(_cols)
	var uv_x1 = float(col_end - 1) / float(_cols)
	var uv_z0 = float(row_start) / float(_rows)
	var uv_z1 = float(row_end - 1) / float(_rows)

	# Generate vertices with positions in Godot world coordinates
	# The heightmap orientation:
	#   pixel_0_0 = SW corner (n_min, e_min)
	#   col_direction = West to East (+E)
	#   row_direction = South to North (+N)
	#
	# Godot coordinates (from TerrainStreamer):
	#   Godot X = center_e - swiss_e  (X increases westward)
	#   Godot Z = swiss_n - center_n  (Z increases northward)

	# Build vertex array
	var vertices: PackedVector3Array = PackedVector3Array()
	var uvs: PackedVector2Array = PackedVector2Array()
	vertices.resize(w * h)
	uvs.resize(w * h)

	for r in range(h):
		for c in range(w):
			var gr = row_start + r  # global row
			var gc = col_start + c  # global col

			# Swiss coordinates of this pixel
			var swiss_e = _swiss_e_min + float(gc) * _resolution
			var swiss_n = _swiss_n_min + float(gr) * _resolution

			# Godot world position
			var godot_x = _terrain_center_e - swiss_e
			var godot_z = swiss_n - _terrain_center_n

			# Height from heightmap (NaN = no data)
			var idx = gr * _cols + gc
			var height = _heightmap[idx] if idx < _heightmap.size() else NAN
			var godot_y = height + height_offset if not is_nan(height) else NAN

			var vi = r * w + c
			vertices[vi] = Vector3(godot_x, godot_y if not is_nan(godot_y) else 0.0, godot_z)

			# UV: map to the color texture
			uvs[vi] = Vector2(
				float(gc) / float(_cols),
				float(gr) / float(_rows)  # No V flip: PNG row 0 = south = top of image, matching gr=0
			)

	# Generate triangle indices — skip quads where any vertex has no data.
	# NaN holes in the heightmap are pre-filled by generate_distant_terrain.py,
	# so this only skips quads at the very edge of the data coverage.
	var indices: PackedInt32Array = PackedInt32Array()
	# Pre-allocate max size, then resize to actual at the end
	indices.resize((w - 1) * (h - 1) * 6)
	var ii = 0

	for r in range(h - 1):
		for c in range(w - 1):
			var tl = r * w + c
			var tr = r * w + c + 1
			var bl = (r + 1) * w + c
			var br = (r + 1) * w + c + 1

			var gr = row_start + r
			var gc = col_start + c

			# Check all four corners for valid data
			var idx_tl = gr * _cols + gc
			var idx_tr = gr * _cols + gc + 1
			var idx_bl = (gr + 1) * _cols + gc
			var idx_br = (gr + 1) * _cols + gc + 1

			if idx_br >= _heightmap.size():
				continue
			if is_nan(_heightmap[idx_tl]) and is_nan(_heightmap[idx_tr]) \
			   and is_nan(_heightmap[idx_bl]) and is_nan(_heightmap[idx_br]):
				continue

			# Triangle 1: TL - BL - TR
			indices[ii] = tl; ii += 1
			indices[ii] = bl; ii += 1
			indices[ii] = tr; ii += 1
			# Triangle 2: TR - BL - BR
			indices[ii] = tr; ii += 1
			indices[ii] = bl; ii += 1
			indices[ii] = br; ii += 1

	# Trim unused index capacity
	indices.resize(ii)

	# If no valid triangles, skip this chunk entirely
	if ii == 0:
		return null

	# Build the mesh using ArrayMesh (faster than SurfaceTool for large meshes)
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	var mesh_inst = MeshInstance3D.new()
	mesh_inst.mesh = mesh
	mesh_inst.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

	if _material:
		mesh_inst.material_override = _material

	# Name for debugging
	mesh_inst.name = "DistantChunk_%d_%d" % [col_start, row_start]

	return mesh_inst


# ── Path Resolution ─────────────────────────────────────────────────────

static func _resolve_scenery_path(path: String) -> String:
	if path.begins_with("/") or path.begins_with("res://") or path.begins_with("user://"):
		return path
	var project_dir = ProjectSettings.globalize_path("res://")
	if project_dir.ends_with("/"):
		project_dir = project_dir.left(project_dir.length() - 1)
	var workspace_root = project_dir.get_base_dir()
	return workspace_root.path_join(path)
