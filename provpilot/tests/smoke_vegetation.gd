extends Node
## Forest card mode of S3DVegetationManager (docs/WORLD_SCENERY_DESIGN.md §14):
## a synthetic vegetation tile (the swissTLM3D .bin format) in a scratch dir,
## the manager in the flat frame, a camera on the tile. Checks: the card mesh
## is 8 triangles, four species from trees/tree_atlas.json, a near set with
## every kept tree and a far set with a quarter of them, enlarged, the
## visibility ranges, the species / variation in the custom data, and the
## tree density setting still thinning the sets.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_vegetation.tscn
const TMP := "/tmp/provpilot_vegetation_smoke"
const TILE_E := 2700288
const TILE_N := 1245184
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _write_tile(path: String, count: int) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer("VEG1".to_ascii_buffer())
	f.store_32(1)
	f.store_32(count)
	f.store_32(0)
	f.store_double(float(TILE_E))
	f.store_double(float(TILE_N))
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for i in range(count):
		f.store_float(rng.randf_range(0.0, 1024.0))   # dx
		f.store_float(rng.randf_range(0.0, 1024.0))   # dz
		f.store_float(500.0 + 1700.0 * float(i) / float(count))   # ground_z: 500 .. 2200 m (beech country to the tree line)
		f.store_float(rng.randf_range(8.0, 30.0))     # height
		f.store_float(rng.randf_range(0.0, TAU))      # yaw
		f.store_float(rng.randf_range(2.0, 6.0))      # crown radius
	f.close()

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(TMP + "/vegetation")
	_write_tile("%s/vegetation/vegetation_%d_%d.bin" % [TMP, TILE_E, TILE_N], 4000)
	var veg := S3DVegetationManager.new()
	veg.frame_mode = 0
	veg.origin_east = TILE_E + 512.0
	veg.origin_north = TILE_N + 512.0
	veg.card_mode = true
	veg.atlas_path = "res://trees/tree_atlas.png"
	veg.atlas_meta_path = "res://trees/tree_atlas.json"
	veg.near_range_m = 2500.0
	veg.far_range_m = 8000.0
	veg.far_fraction = 0.25
	veg.far_scale = 1.6
	veg.density_fraction = 1.0
	veg.vegetation_paths = PackedStringArray([TMP + "/vegetation"])
	add_child(veg)
	var cam := Camera3D.new()
	add_child(cam)
	cam.global_position = Vector3(0.0, 1200.0, 0.0)
	cam.current = true
	var mesh: Mesh = veg.get_card_mesh()
	_check(mesh != null and mesh.get_surface_count() == 1 and mesh.surface_get_arrays(0)[Mesh.ARRAY_INDEX].size() == 24, "card mesh: one surface of 8 triangles (three crossed quads + canopy)")
	if mesh != null:
		var cols: PackedColorArray = mesh.surface_get_arrays(0)[Mesh.ARRAY_COLOR]
		var tops := 0
		for c in cols:
			if c.r > 0.5:
				tops += 1
		_check(tops == 4, "the canopy quad is marked in COLOR.r (%d of %d vertices)" % [tops, cols.size()])
	_check(veg.get_species_count() == 4, "four species from trees/tree_atlas.json (%d)" % veg.get_species_count())
	var waited := 0.0
	var counts: PackedInt32Array = []
	while waited < 20.0:
		await get_tree().process_frame
		waited += get_process_delta_time()
		counts = veg.tile_instance_counts(TILE_E, TILE_N)
		if counts.size() == 2 and counts[0] > 0:
			break
	_check(counts.size() == 2 and counts[0] == 4000, "near set holds every kept tree (%s)" % str(counts))
	_check(counts.size() == 2 and counts[1] == 1000, "far set holds every fourth tree (%s)" % str(counts))
	var near: MultiMeshInstance3D = null
	var far: MultiMeshInstance3D = null
	for c in veg.get_children():
		if c is MultiMeshInstance3D:
			if String(c.name).ends_with("_near"):
				near = c
			elif String(c.name).ends_with("_far"):
				far = c
	_check(near != null and far != null, "two MultiMeshInstance3D per tile (near / far)")
	if near != null and far != null:
		# the engine ranges only cull outside the hand-over band (+- 400 m) plus the tile half-diagonal; the shader crossfades per tree
		_check(is_equal_approx(near.visibility_range_end, 3700.0) and is_equal_approx(far.visibility_range_begin, 1300.0) and far.visibility_range_end > 8000.0, "visibility ranges: near culled past 3.7 km, far from 1.3 km past 8 km, the 2.5 km hand-over crossfaded in the shader (%.0f / %.0f-%.0f)" % [near.visibility_range_end, far.visibility_range_begin, far.visibility_range_end])
		var mat: ShaderMaterial = near.material_override
		_check(is_equal_approx(float(mat.get_shader_parameter("lod_split_m")), 2500.0) and is_equal_approx(float(mat.get_shader_parameter("lod_band_m")), 400.0), "the shader knows the hand-over distance and band")
		_check(near.material_override is ShaderMaterial and near.material_override == far.material_override, "one shared tree ShaderMaterial on both sets")
		var mm: MultiMesh = near.multimesh
		_check(mm.use_custom_data and mm.mesh == mesh, "near instances use the card mesh with custom data")
		# the headless renderer keeps no instance buffers: the manager's CPU-side statistics
		var st: Dictionary = veg.tile_stats(TILE_E, TILE_N)
		var sc: PackedInt32Array = st.get("species", PackedInt32Array())
		var all_four := sc.size() == 4 and sc[0] > 0 and sc[1] > 0 and sc[2] > 0 and sc[3] > 0
		_check(all_four, "all four species appear (%s)" % str(sc))
		var lc: int = st.get("low_conifers", 0)
		var lt: int = st.get("low_total", 1)
		var hc: int = st.get("high_conifers", 0)
		var ht: int = st.get("high_total", 1)
		_check(lt > 100 and ht > 100 and float(lc) / lt < 0.4 and float(hc) / ht > 0.8, "conifers take over with altitude (below 700 m: %d of %d, above 1 300 m: %d of %d)" % [lc, lt, hc, ht])
		_check(absf(float(st.get("far_height_ratio", 0.0)) - 1.6) < 0.01, "far trees are 1.6 x the near height (%.2f)" % float(st.get("far_height_ratio", 0.0)))
		_check(float(st.get("max_height", 0.0)) <= 30.0 * 1.21 and float(st.get("low_mean_height", 0.0)) > 16.0 and float(st.get("low_mean_height", 0.0)) < 22.0, "card heights below 700 m are the records' tree heights (mean %.1f m, max %.1f m)" % [float(st.get("low_mean_height", 0.0)), float(st.get("max_height", 0.0))])
		_check(int(st.get("alpine_total", 0)) > 200 and float(st.get("alpine_mean_height", 0.0)) < 0.65 * float(st.get("low_mean_height", 0.0)), "trees above 1 900 m shrink towards the tree line (mean %.1f m vs %.1f m below 700 m, %d trees)" % [float(st.get("alpine_mean_height", 0.0)), float(st.get("low_mean_height", 0.0)), int(st.get("alpine_total", 0))])
	# terrain snapping: with the host's elevation database the trees stand on
	# the drawn terrain (a flat 777 m tile here), not on the records' ground height
	var db := S3DElevationDB.new()
	db.set_origin_east(TILE_E + 512.0)
	db.set_origin_north(TILE_N + 512.0)
	var img := Image.create(33, 33, false, Image.FORMAT_RF)
	img.fill(Color(777.0, 0, 0, 1))
	db.load_tile(int(floor((TILE_E + 512.0) / 1024.0)), int(floor((TILE_N + 512.0) / 1024.0)), img)
	veg.set_elevation_db(db)
	veg.snap_to_terrain = true   # drops the tiles; they rebuild on the terrain
	waited = 0.0
	var st2: Dictionary = {}
	while waited < 20.0:
		await get_tree().process_frame
		waited += get_process_delta_time()
		st2 = veg.tile_stats(TILE_E, TILE_N)
		if st2.get("near", 0) > 0 and int(st2.get("snapped", 0)) > 0:
			break
	_check(int(st2.get("snapped", 0)) == 4000 and absf(float(st2.get("ground_min", 0.0)) - 777.0) < 0.01 and absf(float(st2.get("ground_max", 0.0)) - 777.0) < 0.01, "every tree snaps to the terrain height (%d snapped, ground %.1f .. %.1f m)" % [int(st2.get("snapped", 0)), float(st2.get("ground_min", 0.0)), float(st2.get("ground_max", 0.0))])
	# density: Medium = 0.5 keeps about half
	veg.density_fraction = 0.5
	waited = 0.0
	counts = []
	while waited < 20.0:
		await get_tree().process_frame
		waited += get_process_delta_time()
		counts = veg.tile_instance_counts(TILE_E, TILE_N)
		if counts.size() == 2 and counts[0] > 0 and counts[0] < 4000:
			break
	_check(counts.size() == 2 and counts[0] > 1700 and counts[0] < 2300 and counts[1] > 400 and counts[1] < 600, "density 0.5 thins both sets (%s)" % str(counts))
	if _fails.is_empty():
		print("smoke_vegetation: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_vegetation: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
