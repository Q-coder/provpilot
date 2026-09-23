extends Node
## Base-tier roads and railways (scripts/build_world_roads.py, design §13.9):
## the Swiss base region's roads/ tiles stream in around Zürich airport and
## are drawn with the procedural roads shader (metres in the UVs).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_base_roads.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(10.0).timeout
	var rms: Array = get_tree().root.find_children("*", "S3DRoadManager", true, false)
	_check(rms.size() >= 1, "road manager present (%d)" % rms.size())
	var rm = rms[0] if rms.size() > 0 else null
	var base_path := ""
	if rm:
		for p in rm.get_road_paths():
			if "switzerland-base" in str(p):
				base_path = str(p)
	_check(base_path != "", "Swiss base region roads/ dir among the road paths (%s)" % base_path)
	var active: int = rm.get_active_tile_count() if rm else 0
	print("  [diag] active road tiles %d" % active)
	_check(active >= 20, "road tiles streamed around LSZH (%d)" % active)
	var shader_tiles := 0
	var colour_tiles := 0
	var uv_tiles := 0
	if rm:
		for c in rm.get_children():
			if c is MeshInstance3D and (c as MeshInstance3D).mesh != null:
				var mesh: ArrayMesh = (c as MeshInstance3D).mesh
				if mesh.surface_get_material(0) is ShaderMaterial:
					shader_tiles += 1
					var arrays: Array = mesh.surface_get_arrays(0)
					if arrays[Mesh.ARRAY_TEX_UV] != null and arrays[Mesh.ARRAY_TEX_UV2] != null:
						uv_tiles += 1
				else:
					colour_tiles += 1
	print("  [diag] shader tiles %d (with metre UVs %d), vertex-colour tiles %d" % [shader_tiles, uv_tiles, colour_tiles])
	_check(shader_tiles >= 10 and uv_tiles == shader_tiles, "base-tier tiles use the roads shader with metre UVs")
	if _fails.is_empty():
		print("smoke_base_roads: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_base_roads: %d failure(s)" % _fails.size())
		get_tree().quit(1)
