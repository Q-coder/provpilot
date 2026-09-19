class_name CloudLayers
extends Node3D
## Cloud layers from the weather model (design doc §10.2). One child mesh per
## layer with clouds: thick types (cumulus, stratus, stratocumulus,
## cumulonimbus) are raymarched inside a camera-centred box between base and
## top; thin types (altostratus, altocumulus, cirrus) are a curved textured
## sheet at the base altitude. This node's transform is the West/Up/North
## frame at the camera's ground point, re-set every frame (ECEF world).

const CT_CUMULUS := 1
const CT_STRATUS := 2
const CT_STRATOCUMULUS := 3
const CT_CUMULONIMBUS := 4
const CT_ALTOSTRATUS := 5
const CT_ALTOCUMULUS := 6
const CT_CIRRUS := 7
const BOX_HALF_M := 60000.0
const SHEET_HALF_M := 90000.0
const PLANET_RADIUS := 6371000.0
const STEPS_BY_QUALITY := [0, 20, 32, 56]   # 0 = off

var quality := 2:
	set(v):
		quality = clampi(v, 0, 3)
		_apply_quality()

var _shape_noise: NoiseTexture3D
var _detail_noise: NoiseTexture3D
var _sheet_noise: NoiseTexture2D
var _volume_shader: Shader
var _sheet_shader: Shader
var _layers: Array = []
var _meshes: Array = []          # MeshInstance3D per layer with cloud_type > 0
var _wind: Array = []            # Vector2 drift (m/s, local x=west... converted) per mesh
var _offsets: Array = []         # accumulated Vector2 per mesh


func _ready() -> void:
	_volume_shader = load("res://shaders/cloud_layer.gdshader")
	_sheet_shader = load("res://shaders/cloud_sheet.gdshader")
	_shape_noise = _make_noise3d(64, FastNoiseLite.TYPE_PERLIN, 3, 1)
	_detail_noise = _make_noise3d(32, FastNoiseLite.TYPE_CELLULAR, 2, 7)
	_sheet_noise = NoiseTexture2D.new()
	_sheet_noise.width = 256
	_sheet_noise.height = 256
	_sheet_noise.seamless = true
	var n := FastNoiseLite.new()
	n.noise_type = FastNoiseLite.TYPE_PERLIN
	n.fractal_octaves = 4
	n.frequency = 0.02
	n.seed = 3
	_sheet_noise.noise = n


static func _make_noise3d(size: int, type: int, octaves: int, seed_v: int) -> NoiseTexture3D:
	var t := NoiseTexture3D.new()
	t.width = size
	t.height = size
	t.depth = size
	t.seamless = true
	var n := FastNoiseLite.new()
	n.noise_type = type
	n.fractal_octaves = octaves
	n.frequency = 0.05
	n.seed = seed_v
	if type == FastNoiseLite.TYPE_CELLULAR:
		n.cellular_return_type = FastNoiseLite.RETURN_DISTANCE
		t.invert = true   # worley "puffs": bright cells, dark edges
	t.noise = n
	return t


## Rebuild the meshes from the weather layer stack (Dictionaries with
## base_m, top_m, cloud_type, coverage_oktas, wind_dir_deg, wind_speed_kts).
func set_layers(layers: Array) -> void:
	_layers = layers.duplicate(true)
	for m in _meshes:
		m.queue_free()
	_meshes.clear()
	_wind.clear()
	_offsets.clear()
	for l in _layers:
		var ct := int(l.get("cloud_type", 0))
		var oktas := int(l.get("coverage_oktas", 0))
		if ct <= 0 or oktas <= 0:
			continue
		var base := float(l.get("base_m", 0.0))
		var top := maxf(float(l.get("top_m", base + 100.0)), base + 100.0)
		var mi := MeshInstance3D.new()
		var mat := ShaderMaterial.new()
		if ct <= CT_CUMULONIMBUS:
			mat.shader = _volume_shader
			var box := BoxMesh.new()
			# Contains the curved shell out to BOX_HALF_M (sag ~ d²/2R).
			var sag := BOX_HALF_M * BOX_HALF_M / (2.0 * PLANET_RADIUS)
			box.size = Vector3(2.0 * BOX_HALF_M, (top - base) + sag + 200.0, 2.0 * BOX_HALF_M)
			mi.mesh = box
			mi.position = Vector3(0.0, (base + top) * 0.5 - sag * 0.5 + 50.0, 0.0)
			mat.set_shader_parameter("mesh_offset", mi.position)
			mat.set_shader_parameter("shape_noise", _shape_noise)
			mat.set_shader_parameter("detail_noise", _detail_noise)
			mat.set_shader_parameter("top_m", top)
			mat.set_shader_parameter("steps", STEPS_BY_QUALITY[quality])
			if ct == CT_CUMULONIMBUS:
				mat.set_shader_parameter("density_scale", 1.6)
			elif ct == CT_STRATUS:
				mat.set_shader_parameter("density_scale", 0.8)
		else:
			mat.shader = _sheet_shader
			var plane := PlaneMesh.new()
			plane.size = Vector2(2.0 * SHEET_HALF_M, 2.0 * SHEET_HALF_M)
			plane.subdivide_width = 32
			plane.subdivide_depth = 32
			mi.mesh = plane
			mat.set_shader_parameter("sheet_noise", _sheet_noise)
		mat.set_shader_parameter("base_m", base)
		mat.set_shader_parameter("coverage", float(oktas) / 8.0)
		mat.set_shader_parameter("cloud_type", ct)
		mat.set_shader_parameter("planet_radius", PLANET_RADIUS)
		mi.material_override = mat
		mi.extra_cull_margin = 16384.0
		mi.custom_aabb = AABB(Vector3(-SHEET_HALF_M, -1000.0, -SHEET_HALF_M), Vector3(2.0 * SHEET_HALF_M, 25000.0, 2.0 * SHEET_HALF_M))
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(mi)
		_meshes.append(mi)
		# Wind: meteorological "from" direction -> drift vector in the local
		# frame (x = west, z = north): blowing toward (dir+180).
		var dir := deg_to_rad(float(l.get("wind_dir_deg", 0.0)))
		var spd := float(l.get("wind_speed_kts", 0.0)) * 0.514444
		_wind.append(Vector2(spd * sin(dir), -spd * cos(dir)))  # (west, north) components
		_offsets.append(Vector2.ZERO)
	_apply_quality()


func _apply_quality() -> void:
	visible = quality > 0
	for m in _meshes:
		var mat: ShaderMaterial = m.material_override
		if mat and mat.shader == _volume_shader:
			mat.set_shader_parameter("steps", STEPS_BY_QUALITY[maxi(quality, 1)])


## Per frame: re-anchor the local frame under the camera and push the light
## state. sun_dir_world is the ECEF unit vector toward the sun.
func update(cam: Camera3D, sun_dir_world: Vector3, sun_color: Color, sun_energy: float,
		ambient_color: Color, ambient_energy: float, fog_density: float, fog_color: Color, delta: float) -> void:
	if quality <= 0 or _meshes.is_empty() or cam == null:
		return
	var p := cam.global_position
	if not p.is_finite() or p.length() < 1.0e6:
		return
	var g: Vector3 = W3DCoords.ecef_to_geodetic(p)
	global_transform = W3DCoords.enu_to_ecef_transform(g.x, g.y, 0.0, true)
	var sun_local: Vector3 = global_transform.basis.inverse() * sun_dir_world
	var cam_local: Vector3 = global_transform.affine_inverse() * p
	for i in range(_meshes.size()):
		_offsets[i] += _wind[i] * delta
		var mat: ShaderMaterial = _meshes[i].material_override
		mat.set_shader_parameter("cam_local", cam_local)
		mat.set_shader_parameter("sun_dir", sun_local.normalized())
		mat.set_shader_parameter("sun_color", sun_color)
		mat.set_shader_parameter("sun_energy", sun_energy)
		mat.set_shader_parameter("ambient_color", ambient_color)
		mat.set_shader_parameter("ambient_energy", ambient_energy)
		mat.set_shader_parameter("fog_density", fog_density)
		mat.set_shader_parameter("fog_color", fog_color)
		mat.set_shader_parameter("wind_offset", _offsets[i])
