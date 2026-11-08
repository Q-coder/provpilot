extends Node
## Forest polygon tiles (docs/WORLD_SCENERY_DESIGN.md §14, scripts/build_world_forests.py):
## a synthetic forest_E_N.bin with a square forest (a hole in it) and a road
## corridor, filled with trees by the vegetation manager at load time; a second
## dir with a surveyed vegetation_E_N.bin of a neighbouring square whose trees
## the same corridor file thins. Checks tree counts against the polygon area,
## no tree in the hole or the corridor, leaf type -> species, and that a square
## with a surveyed tile skips the polygon fill.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_forest_tiles.tscn
const TMP := "/tmp/provpilot_forest_smoke"
const TILE_E := 2700288
const TILE_N := 1245184
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _ring(f: FileAccess, pts: Array) -> void:
	f.store_32(pts.size())
	for p in pts:
		f.store_float(p.x)
		f.store_float(p.y)

func _poly_header(f: FileAccess, kind: int, density: float, height: float, rings: int) -> void:
	f.store_8(kind); f.store_8(0); f.store_8(0); f.store_8(0)
	f.store_float(density)
	f.store_float(height)
	f.store_32(rings)

## forest_E_N.bin: a 400 x 400 m conifer square (100..500) with a 100 x 100 m hole
## (200..300), a 200 x 200 m broadleaf square (600..800), a road corridor along
## x = 150 from z = 0 to 1024, half width 6 m.
func _write_forest(path: String, e: int, n: int, with_forest: bool) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer("FOR1".to_ascii_buffer())
	f.store_32(1)
	f.store_32(3 if with_forest else 1)
	f.store_32(0)
	f.store_double(float(e))
	f.store_double(float(n))
	if with_forest:
		_poly_header(f, 1, 1.0 / 36.0, 24.0, 2)
		_ring(f, [Vector2(100, 100), Vector2(500, 100), Vector2(500, 500), Vector2(100, 500), Vector2(100, 100)])
		_ring(f, [Vector2(200, 200), Vector2(300, 200), Vector2(300, 300), Vector2(200, 300), Vector2(200, 200)])
		_poly_header(f, 0, 1.0 / 36.0, 20.0, 1)
		_ring(f, [Vector2(600, 600), Vector2(800, 600), Vector2(800, 800), Vector2(600, 800), Vector2(600, 600)])
	_poly_header(f, 3, 6.0, 0.0, 1)
	_ring(f, [Vector2(150, 0), Vector2(150, 1024)])
	f.close()

## vegetation_E_N.bin: 2 000 surveyed trees on a 1024 m grid line pattern: 1 000 of
## them exactly on the corridor line x = 150 (should go), 1 000 at x = 700 (stay).
func _write_surveyed(path: String, e: int, n: int) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer("VEG1".to_ascii_buffer())
	f.store_32(1); f.store_32(2000); f.store_32(0)
	f.store_double(float(e)); f.store_double(float(n))
	for i in range(2000):
		f.store_float(150.0 if i < 1000 else 700.0)
		f.store_float(fmod(i * 1.024, 1024.0))
		f.store_float(600.0); f.store_float(20.0); f.store_float(0.0); f.store_float(4.0)
	f.close()

func _ready() -> void:
	for d in ["/a/vegetation", "/b/vegetation"]:
		DirAccess.make_dir_recursive_absolute(TMP + d)
		for fn in DirAccess.get_files_at(TMP + d):
			DirAccess.remove_absolute(TMP + d + "/" + fn)
	# square A: forest polygons + corridor (dir a); square B (east neighbour): a
	# surveyed tile in dir b plus a corridor-only forest file in dir a, and a
	# forest file WITH polygons in dir a that must stand down (surveyed wins)
	_write_forest("%s/a/vegetation/forest_%d_%d.bin" % [TMP, TILE_E, TILE_N], TILE_E, TILE_N, true)
	_write_forest("%s/a/vegetation/forest_%d_%d.bin" % [TMP, TILE_E + 1024, TILE_N], TILE_E + 1024, TILE_N, true)
	_write_surveyed("%s/b/vegetation/vegetation_%d_%d.bin" % [TMP, TILE_E + 1024, TILE_N], TILE_E + 1024, TILE_N)
	var veg := S3DVegetationManager.new()
	veg.frame_mode = 0
	veg.origin_east = TILE_E + 1024.0
	veg.origin_north = TILE_N + 512.0
	veg.card_mode = true
	veg.density_fraction = 1.0
	veg.snap_to_terrain = false
	veg.debug_keep_positions = true
	veg.vegetation_paths = PackedStringArray([TMP + "/a/vegetation", TMP + "/b/vegetation"])
	add_child(veg)
	var cam := Camera3D.new()
	add_child(cam)
	cam.global_position = Vector3(0.0, 800.0, 0.0)
	cam.current = true
	var waited := 0.0
	var a: Dictionary = {}
	var b: Dictionary = {}
	while waited < 20.0:
		await get_tree().process_frame
		waited += get_process_delta_time()
		a = veg.tile_stats(TILE_E, TILE_N)
		b = veg.tile_stats(TILE_E + 1024, TILE_N)
		if a.get("near", 0) > 0 and b.get("near", 0) > 0:
			break
	# polygon fill: 160 000 - 10 000 m2 conifer + 40 000 m2 broadleaf = 190 000 m2 / 36 = 5 278 trees,
	# minus the corridor (x 143..157 through the conifer square, 400 m x 14 m / 36 = 156)
	var na: int = a.get("near", 0)
	_check(na > 4600 and na < 5300, "polygon fill at 1 tree / 36 m2 (%d trees for 190 000 m2 minus the corridor)" % na)
	var sc: PackedInt32Array = a.get("species", PackedInt32Array())
	var conifers := 0
	var broadleaf := 0
	if sc.size() == 7:
		broadleaf = sc[0] + sc[1] + sc[2] + sc[3]
		conifers = sc[4] + sc[5] + sc[6]
	_check(conifers > 3600 and broadleaf > 900 and broadleaf < 1300, "leaf type picks the species group (conifers %d, broadleaf %d)" % [conifers, broadleaf])
	var in_hole := 0
	var in_corridor := 0
	var outside := 0
	var pos: PackedVector2Array = veg.tile_tree_positions(TILE_E, TILE_N)
	for p in pos:
		if p.x >= 200.0 and p.x <= 300.0 and p.y >= 200.0 and p.y <= 300.0:
			in_hole += 1
		if absf(p.x - 150.0) < 6.0:
			in_corridor += 1
		var in_conifer := p.x >= 100.0 and p.x <= 500.0 and p.y >= 100.0 and p.y <= 500.0
		var in_broad := p.x >= 600.0 and p.x <= 800.0 and p.y >= 600.0 and p.y <= 800.0
		if not in_conifer and not in_broad:
			outside += 1
	_check(pos.size() == na and in_hole == 0, "no tree in the polygon's hole (%d of %d positions)" % [in_hole, pos.size()])
	_check(in_corridor == 0, "no tree within the road corridor (%d)" % in_corridor)
	_check(outside == 0, "every tree inside a forest polygon (%d outside)" % outside)
	# square B: the surveyed tile keeps 1 000 of 2 000 (the corridor takes the x = 150 line), the forest file stands down
	var nb: int = b.get("near", 0)
	_check(nb == 1000, "surveyed trees on the road are removed by the square's corridor file (%d of 2000 kept)" % nb)
	_check(int(b.get("forest_fill_skipped", -1)) == 1, "a forest file stands down where a surveyed tile covers the square")
	if _fails.is_empty():
		print("smoke_forest_tiles: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_forest_tiles: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
