## TerrainClipmap — Multi-ring clipmap terrain beyond Terrain3D
##
## Renders 3 concentric ring meshes at decreasing resolution, replacing
## DistantTerrain.  Each ring samples from the pre-generated mipmap
## tile pyramids (mip2/mip3/mip4) produced by generate_terrain_mipmap.py.
##
## Ring layout (centered on camera, moves with floating origin):
##   Ring 0: 7.0 km – 16.4 km   mip2 (16 m/px)   2048×2048 heightmap
##          (inner 7–8.5 km overlaps T3D with seam blending + depth bias)
##   Ring 1: 16.4 km – 65.5 km  mip3 (64 m/px)   2048×2048 heightmap
##   Ring 2: 65.5 km – 262 km   mip4 (256 m/px)  2048×2048 heightmap
##
## The mesh is a flat grid (Y=0) for each ring; the shader displaces
## vertices vertically using the heightmap texture.
##
## Setup:
##   1. Run: python scripts/generate_terrain_mipmap.py
##   2. Add a TerrainClipmap node to the scene (child of root, top_level=true)
##   3. node_3d.gd calls rebase_center() during floating-origin rebase

extends Node3D

## Path to scenery directory (contains terrain/ and orthophoto/ subdirs)
@export var scenery_path: String = "scenery"

# ── Ring configuration ──────────────────────────────────────────────────

const TEX_SIZE: int = 2048          # Texture size per ring (pixels per side)

# Phase 4 — Terrain3D ↔ Ring 0 seam constants
const SEAM_OVERLAP_INNER: float = 7000.0   # Ring 0 mesh starts here (overlap with T3D)
const SEAM_BLEND_START: float = 8500.0     # Height blend: seam→mip2 begins
const SEAM_BLEND_END: float = 10500.0      # Height blend: fully mip2 by here
const SEAM_DEPTH_BIAS: float = 5.0         # Metres to push Ring 0 down at inner edge
const SEAM_FADE_END: float = 9000.0        # Inner color fade fully opaque by here
const SEAM_TILE_MAX_DIST: float = 12000.0  # Only load 1 m tiles within this radius

# Per-ring config: mip_level, pixel_res (m), inner_radius (m), mesh_divs
# Outer = TEX_SIZE / 2 * pixel_res:  Ring0=16384, Ring1=65536, Ring2=262144
var _ring_config: Array = [
	{"mip_level": 2, "pixel_res": 16.0,  "inner_radius": SEAM_OVERLAP_INNER, "mesh_divs": 512},
	{"mip_level": 3, "pixel_res": 64.0,  "inner_radius": 16384.0, "mesh_divs": 512},
	{"mip_level": 4, "pixel_res": 256.0, "inner_radius": 65536.0, "mesh_divs": 512},
]

# Shader params per ring: boundary radii and geomorph transition start.
# morph_start = where geomorphing begins; at outer_radius it's 100% coarse.
var _ring_shader_params: Array = [
	{"inner_radius": SEAM_OVERLAP_INNER, "outer_radius": 16384.0, "morph_start": 13000.0},
	{"inner_radius": 16384.0, "outer_radius": 65536.0,  "morph_start": 55000.0},
	{"inner_radius": 65536.0, "outer_radius": 300000.0, "morph_start": 300000.0},
]

# ── Internal state ──────────────────────────────────────────────────────

var _terrain_center_e: float = 0.0
var _terrain_center_n: float = 0.0
var _initialized: bool = false
var _shader: Shader

# Per-ring arrays (indexed 0..2)
var _ring_mesh_nodes: Array[MeshInstance3D] = []
var _ring_materials: Array[ShaderMaterial] = []
var _ring_half_extents: Array[float] = []    # half-extent in meters
var _ring_height_textures: Array = []        # ImageTexture per ring
var _ring_ortho_textures: Array = []         # ImageTexture per ring (or null)

# Manifest data (per mip-level)
var _manifests: Dictionary = {}   # mip_level → parsed JSON dict

# Phase 4 — seam state (Ring 0 only)
var _manifest_1m: Dictionary = {}            # Full-resolution 1 m manifest
var _tile_directory_1m: String = ""           # Path to 1 m tile directory
var _seam_height_texture: ImageTexture       # Ring 0 seam heightmap from 1 m data

# ── Lifecycle ───────────────────────────────────────────────────────────

func _ready() -> void:
	if Engine.is_editor_hint():
		return

	scenery_path = _resolve_scenery_path(scenery_path)

	# Get terrain center from TerrainStreamer
	_terrain_center_e = 2682880.0
	_terrain_center_n = 1283072.0
	var streamer = get_parent().get_node_or_null("TerrainStreamer")
	if streamer:
		_terrain_center_e = float(streamer.terrain_center_e)
		_terrain_center_n = float(streamer.terrain_center_n)

	# Load shader
	_shader = load("res://shaders/terrain_clipmap.gdshader")
	if not _shader:
		push_error("TerrainClipmap: Failed to load shader")
		return

	# Load manifests for mip levels 2, 3, 4
	for mip_level in [2, 3, 4]:
		var mf = _load_manifest("terrain", mip_level)
		if mf.is_empty():
			push_error("TerrainClipmap: Missing terrain mip%d manifest" % mip_level)
			return
		_manifests[mip_level] = mf

	# Phase 4: Load 1 m terrain manifest for seam heightmap
	_tile_directory_1m = scenery_path + "/terrain"
	_manifest_1m = _load_manifest_1m()
	if _manifest_1m.is_empty():
		push_warning("TerrainClipmap: No 1 m manifest — seam blending disabled")

	# Compute ring half-extents
	for i in range(_ring_config.size()):
		var cfg = _ring_config[i]
		var half_ext: float = float(TEX_SIZE) / 2.0 * cfg["pixel_res"]
		_ring_half_extents.append(half_ext)

	# Pass 1: Build all ring textures, meshes, and base materials
	for i in range(_ring_config.size()):
		_build_ring(i)

	# Pass 2: Link coarse textures for geomorphing.
	# Ring N's coarse data = Ring N+1's actual textures with a UV transform.
	# This guarantees heights match exactly at the seam.
	for i in range(_ring_config.size() - 1):
		var mat = _ring_materials[i]
		var next_htex = _ring_height_textures[i + 1]
		var next_otex = _ring_ortho_textures[i + 1]

		mat.set_shader_parameter("coarse_heightmap", next_htex)
		if next_otex:
			mat.set_shader_parameter("coarse_color_texture", next_otex)

		# UV transform from this ring's UV space to next ring's UV space.
		# Ring i UV [0,1] maps to world [-half_i, +half_i].
		# Ring i+1 UV [0,1] maps to world [-half_j, +half_j].
		# coarse_uv = uv * (half_i / half_j) + 0.5 * (1 - half_i / half_j)
		var scale: float = _ring_half_extents[i] / _ring_half_extents[i + 1]
		var offset: float = 0.5 * (1.0 - scale)
		mat.set_shader_parameter("coarse_uv_scale", scale)
		mat.set_shader_parameter("coarse_uv_offset", offset)
		mat.set_shader_parameter("has_coarse", true)

		print("  Ring %d→%d geomorph: uv_scale=%.4f, uv_offset=%.4f" % [
			i, i + 1, scale, offset])

	_initialized = true
	print("TerrainClipmap: Ready — %d rings" % _ring_mesh_nodes.size())
	for i in range(_ring_mesh_nodes.size()):
		var he = _ring_half_extents[i]
		var ir = _ring_config[i]["inner_radius"]
		print("  Ring %d: inner=%.0f m, outer=%.0f m, mip%d" % [
			i, ir, he, _ring_config[i]["mip_level"]])


func _process(_delta: float) -> void:
	pass


## Update terrain center when floating origin rebases.
func rebase_center(new_center_e: float, new_center_n: float) -> void:
	if not _initialized:
		print("TerrainClipmap: rebase_center SKIPPED — not initialized!")
		return

	var delta_e = new_center_e - _terrain_center_e
	var delta_n = new_center_n - _terrain_center_n

	_terrain_center_e = new_center_e
	_terrain_center_n = new_center_n

	# Shift mesh positions (Godot X = +delta_e, Godot Z = -delta_n)
	for mesh_node in _ring_mesh_nodes:
		mesh_node.position.x += delta_e
		mesh_node.position.z -= delta_n

	var shift := sqrt(delta_e * delta_e + delta_n * delta_n)
	if shift > 1024.0:
		print("TerrainClipmap: rebase (%.0f m) — rebuilding textures" % shift)
		_rebuild_all_textures()
		for mesh_node in _ring_mesh_nodes:
			mesh_node.position = Vector3.ZERO


# ── Ring building ───────────────────────────────────────────────────────

func _build_ring(ring_idx: int) -> void:
	var cfg = _ring_config[ring_idx]
	var half_ext: float = _ring_half_extents[ring_idx]
	var inner_r: float = cfg["inner_radius"]
	var pixel_res: float = cfg["pixel_res"]
	var mip_level: int = cfg["mip_level"]
	var mesh_divs: int = cfg["mesh_divs"]

	# 1. Create the flat annulus mesh
	var mesh = _generate_ring_mesh(half_ext, inner_r, mesh_divs)

	# 2. Assemble heightmap texture from mip tiles
	var height_tex = _assemble_heightmap_texture(mip_level, pixel_res, half_ext)
	_ring_height_textures.append(height_tex)

	# 3. Assemble orthophoto texture from mip tiles
	var ortho_tex = _assemble_ortho_texture(mip_level, pixel_res, half_ext)
	_ring_ortho_textures.append(ortho_tex)

	# 4. Create material (coarse textures linked in Pass 2)
	var mat = ShaderMaterial.new()
	mat.shader = _shader
	mat.set_shader_parameter("heightmap", height_tex)
	if ortho_tex:
		mat.set_shader_parameter("color_texture", ortho_tex)

	# Set ring-specific shader params
	var sp = _ring_shader_params[ring_idx]
	mat.set_shader_parameter("inner_radius", sp["inner_radius"])
	mat.set_shader_parameter("outer_radius", sp["outer_radius"])
	mat.set_shader_parameter("morph_start", sp["morph_start"])
	mat.set_shader_parameter("has_coarse", false)  # set true in Pass 2

	# Atmosphere (shared)
	mat.set_shader_parameter("fog_start", 80000.0)
	mat.set_shader_parameter("fog_end", 250000.0)
	mat.set_shader_parameter("fog_color", Color(0.75, 0.8, 0.88))
	mat.set_shader_parameter("haze_color", Color(0.7, 0.75, 0.85))
	mat.set_shader_parameter("haze_density", 0.000004)
	mat.set_shader_parameter("brightness", 1.1)

	# Phase 4: Seam blending for Ring 0
	if ring_idx == 0 and not _manifest_1m.is_empty():
		_seam_height_texture = _assemble_seam_heightmap(half_ext, pixel_res)
		if _seam_height_texture:
			mat.set_shader_parameter("seam_heightmap", _seam_height_texture)
			mat.set_shader_parameter("has_seam", true)
			mat.set_shader_parameter("seam_blend_start", SEAM_BLEND_START)
			mat.set_shader_parameter("seam_blend_end", SEAM_BLEND_END)
			mat.set_shader_parameter("seam_depth_bias", SEAM_DEPTH_BIAS)
			mat.set_shader_parameter("inner_fade_end", SEAM_FADE_END)
			print("  Ring 0: seam blending enabled (bias=%.1f m, fade to %.0f m)" % [
				SEAM_DEPTH_BIAS, SEAM_FADE_END])

	# 5. Create MeshInstance3D
	var mesh_inst = MeshInstance3D.new()
	mesh_inst.mesh = mesh
	mesh_inst.material_override = mat
	mesh_inst.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(mesh_inst)

	_ring_mesh_nodes.append(mesh_inst)
	_ring_materials.append(mat)


func _update_ring_textures(ring_idx: int) -> void:
	var cfg = _ring_config[ring_idx]
	var half_ext: float = _ring_half_extents[ring_idx]
	var pixel_res: float = cfg["pixel_res"]
	var mip_level: int = cfg["mip_level"]
	var mat = _ring_materials[ring_idx]

	var height_tex = _assemble_heightmap_texture(mip_level, pixel_res, half_ext)
	mat.set_shader_parameter("heightmap", height_tex)
	_ring_height_textures[ring_idx] = height_tex

	var ortho_tex = _assemble_ortho_texture(mip_level, pixel_res, half_ext)
	if ortho_tex:
		mat.set_shader_parameter("color_texture", ortho_tex)
	_ring_ortho_textures[ring_idx] = ortho_tex

	# Phase 4: Rebuild seam heightmap for Ring 0
	if ring_idx == 0 and not _manifest_1m.is_empty():
		_seam_height_texture = _assemble_seam_heightmap(half_ext, pixel_res)
		if _seam_height_texture:
			mat.set_shader_parameter("seam_heightmap", _seam_height_texture)


func _rebuild_all_textures() -> void:
	## Reassemble all ring textures centered on the current _terrain_center.
	## Called after a large rebase (e.g. first-flight jump).
	for i in range(_ring_config.size()):
		_update_ring_textures(i)

	# Re-link coarse textures for geomorphing (Ring N+1 → Ring N)
	for i in range(_ring_config.size() - 1):
		var mat = _ring_materials[i]
		mat.set_shader_parameter("coarse_heightmap", _ring_height_textures[i + 1])
		if _ring_ortho_textures[i + 1]:
			mat.set_shader_parameter("coarse_color_texture", _ring_ortho_textures[i + 1])

	print("TerrainClipmap: textures rebuilt centered on E=%.0f N=%.0f" % [
		_terrain_center_e, _terrain_center_n])


# ── Mesh generation ────────────────────────────────────────────────────

func _generate_ring_mesh(half_extent: float, inner_radius: float,
		mesh_divs: int = 512) -> ArrayMesh:
	## Generate a flat grid mesh (Y=0) covering [-half, +half] in XZ with
	## an inner square hole of radius inner_radius.  UV maps [0,1] across
	## the full grid, matching the heightmap texture layout.

	var divs := mesh_divs
	var verts_per_side := divs + 1
	var step := 2.0 * half_extent / float(divs)

	# Build vertex and UV arrays for the full grid
	var vertices := PackedVector3Array()
	var uvs := PackedVector2Array()
	var vertex_indices := {}   # (i,j) → index in vertices array
	var idx := 0

	for j in range(verts_per_side):
		for i in range(verts_per_side):
			var x := -half_extent + float(i) * step
			var z := -half_extent + float(j) * step

			# Skip vertices deep inside the inner hole (keep a 1-cell border
			# so triangles at the boundary have valid vertices)
			if absf(x) < inner_radius - step and absf(z) < inner_radius - step:
				continue

			vertices.append(Vector3(x, 0.0, z))
			uvs.append(Vector2(float(i) / float(divs), float(j) / float(divs)))
			vertex_indices[i * 10000 + j] = idx
			idx += 1

	# Build index buffer — 2 triangles per cell, skip cells fully inside hole
	var indices := PackedInt32Array()
	for j in range(divs):
		for i in range(divs):
			# Cell corners
			var k00 := i * 10000 + j
			var k10 := (i + 1) * 10000 + j
			var k01 := i * 10000 + (j + 1)
			var k11 := (i + 1) * 10000 + (j + 1)

			# All 4 corners must exist in the vertex map
			if not (k00 in vertex_indices and k10 in vertex_indices and
					k01 in vertex_indices and k11 in vertex_indices):
				continue

			var i00: int = vertex_indices[k00]
			var i10: int = vertex_indices[k10]
			var i01: int = vertex_indices[k01]
			var i11: int = vertex_indices[k11]

			# Two triangles (CCW winding for front face)
			indices.append(i00)
			indices.append(i01)
			indices.append(i10)

			indices.append(i10)
			indices.append(i01)
			indices.append(i11)

	# Normals (up — the shader displaces Y, normals are approximate)
	var normals := PackedVector3Array()
	normals.resize(vertices.size())
	normals.fill(Vector3.UP)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	print("  Ring mesh: %d verts, %d tris (half=%.0f, inner=%.0f)" % [
		vertices.size(), indices.size() / 3.0, half_extent, inner_radius])

	return mesh


# ── Texture assembly ────────────────────────────────────────────────────

func _assemble_heightmap_texture(mip_level: int, pixel_res: float,
		half_extent: float) -> ImageTexture:
	## Build a TEX_SIZE × TEX_SIZE float32 heightmap from mip tiles.
	## Texture is centered on (_terrain_center_e, _terrain_center_n).

	var img := Image.create(TEX_SIZE, TEX_SIZE, false, Image.FORMAT_RF)
	var manifest: Dictionary = _manifests[mip_level]
	var tiles: Dictionary = manifest["tiles"]
	var tile_px: int = manifest["pixels_per_tile"]

	# Texture geographic bounds in Swiss coords
	var tex_east: float = _terrain_center_e + half_extent
	var tex_south: float = _terrain_center_n - half_extent

	var tiles_blitted := 0
	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		var tex_west: float = tex_east - float(TEX_SIZE) * pixel_res
		var tex_north: float = tex_south + float(TEX_SIZE) * pixel_res

		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		var col_offset := int(roundf((tex_east - tile_e - 1024.0) / pixel_res))
		var row_offset := int(roundf((tile_n - tex_south) / pixel_res))

		var tile_data := _load_terrain_tile(mip_level, tile["file"], tile_px)
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
		if dst_pos.x + src_rect.size.x > TEX_SIZE:
			src_rect.size.x = TEX_SIZE - dst_pos.x
		if dst_pos.y + src_rect.size.y > TEX_SIZE:
			src_rect.size.y = TEX_SIZE - dst_pos.y

		if src_rect.size.x > 0 and src_rect.size.y > 0:
			img.blit_rect(tile_img, src_rect, dst_pos)

		tiles_blitted += 1

	var tex := ImageTexture.create_from_image(img)
	print("  Heightmap mip%d: blitted %d tiles (center E=%.0f N=%.0f, half=%.0f)" % [
		mip_level, tiles_blitted, _terrain_center_e, _terrain_center_n, half_extent])
	return tex


func _assemble_ortho_texture(mip_level: int, pixel_res: float,
		half_extent: float) -> ImageTexture:
	## Build a TEX_SIZE × TEX_SIZE RGB8 orthophoto from mip tiles.

	var ortho_manifest := _load_manifest("orthophoto", mip_level)
	if ortho_manifest.is_empty():
		return null

	var img := Image.create(TEX_SIZE, TEX_SIZE, false, Image.FORMAT_RGB8)
	img.fill(Color(0.4, 0.45, 0.35))

	var tiles: Dictionary = ortho_manifest["tiles"]
	var tile_px: int = ortho_manifest["pixels_per_tile"]

	var tex_east: float = _terrain_center_e + half_extent
	var tex_south: float = _terrain_center_n - half_extent

	var tiles_blitted := 0
	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		var tex_west: float = tex_east - float(TEX_SIZE) * pixel_res
		var tex_north: float = tex_south + float(TEX_SIZE) * pixel_res

		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		var col_offset := int(roundf((tex_east - tile_e - 1024.0) / pixel_res))
		var row_offset := int(roundf((tile_n - tex_south) / pixel_res))

		var tile_img := _load_ortho_tile(mip_level, tile["file"], tile_px)
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
		if dst_pos.x + src_rect.size.x > TEX_SIZE:
			src_rect.size.x = TEX_SIZE - dst_pos.x
		if dst_pos.y + src_rect.size.y > TEX_SIZE:
			src_rect.size.y = TEX_SIZE - dst_pos.y

		if src_rect.size.x > 0 and src_rect.size.y > 0:
			img.blit_rect(tile_img, src_rect, dst_pos)
			tiles_blitted += 1

	var tex := ImageTexture.create_from_image(img)
	print("  Ortho mip%d: blitted %d tiles into %dx%d texture" % [
		mip_level, tiles_blitted, TEX_SIZE, TEX_SIZE])

	return tex


# ── Tile loading helpers ────────────────────────────────────────────────

func _load_terrain_tile(mip_level: int, filename: String,
		tile_px: int) -> PackedFloat32Array:
	var dir_path := scenery_path + "/terrain/mip%d" % mip_level
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
	var dir_path := scenery_path + "/orthophoto/mip%d" % mip_level
	var path := dir_path + "/" + filename
	var img := Image.new()
	var err := img.load(path)
	if err != OK:
		return null

	# Ensure correct size and format
	if img.get_width() != tile_px or img.get_height() != tile_px:
		img.resize(tile_px, tile_px, Image.INTERPOLATE_LANCZOS)
	if img.get_format() != Image.FORMAT_RGB8:
		img.convert(Image.FORMAT_RGB8)

	return img


# ── Manifest loading ────────────────────────────────────────────────────

func _load_manifest(data_type: String, mip_level: int) -> Dictionary:
	## Load manifest.json for a given data type (terrain/orthophoto) and mip level.
	var path := scenery_path + "/%s/mip%d/manifest.json" % [data_type, mip_level]
	var file := FileAccess.open(path, FileAccess.READ)
	if not file:
		push_warning("TerrainClipmap: No manifest at '%s'" % path)
		return {}

	var json := JSON.new()
	var err := json.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_error("TerrainClipmap: Bad manifest JSON: %s" % json.get_error_message())
		return {}

	return json.data


func _load_manifest_1m() -> Dictionary:
	## Load the full-resolution 1 m terrain manifest (scenery/terrain/manifest.json).
	var path := _tile_directory_1m + "/manifest.json"
	var file := FileAccess.open(path, FileAccess.READ)
	if not file:
		push_warning("TerrainClipmap: No 1 m manifest at '%s'" % path)
		return {}

	var json := JSON.new()
	var err := json.parse(file.get_as_text())
	file.close()

	if err != OK:
		push_error("TerrainClipmap: Bad 1 m manifest JSON: %s" % json.get_error_message())
		return {}

	return json.data


# ── Phase 4: Seam heightmap from 1 m tiles ─────────────────────────────

func _assemble_seam_heightmap(half_extent: float, pixel_res: float) -> ImageTexture:
	## Build a TEX_SIZE × TEX_SIZE float32 heightmap from the full-resolution
	## 1 m tiles (same data Terrain3D uses), bilinear-downsampled to pixel_res.
	## Only tiles within SEAM_TILE_MAX_DIST from center are loaded, keeping
	## I/O costs reasonable.  The resulting texture shares UV space with
	## Ring 0's mip2 heightmap so both can be sampled at the same UVs.

	var img := Image.create(TEX_SIZE, TEX_SIZE, false, Image.FORMAT_RF)
	var tiles: Dictionary = _manifest_1m.get("tiles", {})
	if tiles.is_empty():
		return ImageTexture.create_from_image(img)

	var tile_src_px: int = int(_manifest_1m.get("pixels_per_tile", 1024))
	var out_px: int = int(1024.0 / pixel_res)  # 1024 m tile → pixels at target res

	var tex_east: float = _terrain_center_e + half_extent
	var tex_south: float = _terrain_center_n - half_extent
	var tex_west: float = tex_east - float(TEX_SIZE) * pixel_res
	var tex_north: float = tex_south + float(TEX_SIZE) * pixel_res

	var tiles_processed := 0
	for key in tiles:
		var tile: Dictionary = tiles[key]
		var tile_e: float = tile["swiss_e"]
		var tile_n: float = tile["swiss_n"]

		# Spatial cull: skip tiles outside texture area
		if tile_e + 1024.0 < tex_west or tile_e > tex_east:
			continue
		if tile_n + 1024.0 < tex_south or tile_n > tex_north:
			continue

		# Seam-band cull: skip tiles far from the inner boundary
		var tile_cx: float = _terrain_center_e - (tile_e + 512.0)
		var tile_cz: float = (tile_n + 512.0) - _terrain_center_n
		var tile_dist: float = sqrt(tile_cx * tile_cx + tile_cz * tile_cz)
		if tile_dist > SEAM_TILE_MAX_DIST:
			continue

		# Load raw 1 m tile and downsample via Lanczos (matches GPU bilinear
		# behaviour better than the median used for mip2 tiles)
		var tile_img := _load_1m_tile(tile["file"], tile_src_px)
		if tile_img == null:
			continue
		tile_img.resize(out_px, out_px, Image.INTERPOLATE_LANCZOS)

		# Blit into output at correct position (same math as _assemble_heightmap_texture)
		var col_offset := int(roundf((tex_east - tile_e - 1024.0) / pixel_res))
		var row_offset := int(roundf((tile_n - tex_south) / pixel_res))

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
		if dst_pos.x + src_rect.size.x > TEX_SIZE:
			src_rect.size.x = TEX_SIZE - dst_pos.x
		if dst_pos.y + src_rect.size.y > TEX_SIZE:
			src_rect.size.y = TEX_SIZE - dst_pos.y

		if src_rect.size.x > 0 and src_rect.size.y > 0:
			img.blit_rect(tile_img, src_rect, dst_pos)
			tiles_processed += 1

	var tex := ImageTexture.create_from_image(img)
	print("  Seam heightmap: %d tiles processed (1 m → %.0f m/px, band < %.0f m)" % [
		tiles_processed, pixel_res, SEAM_TILE_MAX_DIST])
	return tex


func _load_1m_tile(filename: String, tile_px: int) -> Image:
	## Load a single raw float32 1 m tile and return it as an Image.
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


# ── Path helpers ────────────────────────────────────────────────────────

func _resolve_scenery_path(path: String) -> String:
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
	return base_dir.path_join(path)
