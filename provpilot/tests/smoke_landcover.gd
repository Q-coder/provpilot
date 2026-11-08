extends Node
## Land-cover ground texture (docs/WORLD_SCENERY_DESIGN.md §14.6): a tile without
## an orthophoto gets its forest polygons (forest_E_N.bin, FOR1) painted into a
## small texture by the tile manager, so woods stay recognisable to the horizon.
## Rasterises the forest smoke's synthetic square at 128 px (regular tile) and
## 16 px (far chunk sub-tile) and checks colours, holes, the corridor gap and the
## orthophoto orientation (column 0 = east edge, row 0 = south edge). Then a
## Scenery3D node with land-cover paths over a synthetic terrain tile must give
## that tile its own material.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_landcover.tscn
const TMP := "/tmp/provpilot_landcover_smoke"
const TILE_E := 2700288
const TILE_N := 1245184
const BASE := Color8(115, 140, 89)
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

## A 400 x 400 m conifer square (100..500) with a 100 x 100 m hole (200..300), a
## 200 x 200 m broadleaf square (600..800), a road corridor along x = 150, half
## width 6 m — the forest smoke's square.
func _write_forest(path: String, e: int, n: int) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer("FOR1".to_ascii_buffer())
	f.store_32(1)
	f.store_32(3)
	f.store_32(0)
	f.store_double(float(e))
	f.store_double(float(n))
	_poly_header(f, 1, 1.0 / 36.0, 24.0, 2)
	_ring(f, [Vector2(100, 100), Vector2(500, 100), Vector2(500, 500), Vector2(100, 500), Vector2(100, 100)])
	_ring(f, [Vector2(200, 200), Vector2(300, 200), Vector2(300, 300), Vector2(200, 300), Vector2(200, 200)])
	_poly_header(f, 0, 1.0 / 36.0, 20.0, 1)
	_ring(f, [Vector2(600, 600), Vector2(800, 600), Vector2(800, 800), Vector2(600, 800), Vector2(600, 600)])
	_poly_header(f, 3, 6.0, 0.0, 1)
	_ring(f, [Vector2(150, 0), Vector2(150, 1024)])
	f.close()

## A flat 34 px terrain tile at 600 m (the base tier's resolution).
func _write_terrain(dir: String, e: int, n: int) -> void:
	var f := FileAccess.open("%s/tile_%d_%d.raw" % [dir, e, n], FileAccess.WRITE)
	for i in range(34 * 34):
		f.store_float(600.0)
	f.close()
	var m := FileAccess.open(dir + "/manifest.json", FileAccess.WRITE)
	m.store_string(JSON.stringify({"pixels_per_tile": 34, "tile_size": 1024}))
	m.close()

## Pixel of an east offset x and a north offset z (metres from the SW corner).
func _px(img: Image, x: float, z: float) -> Color:
	var px := img.get_width()
	var col := px - 1 - int(floor(x / 1024.0 * px))
	var row := int(floor(z / 1024.0 * px))
	return img.get_pixel(col, row)

func _is_base(c: Color) -> bool:
	return c.is_equal_approx(BASE)

func _is_forest(c: Color) -> bool:
	# clearly darker than the base, still green
	return c.g < 0.42 and c.g > c.r and c.g > c.b and not _is_base(c)

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(TMP + "/vegetation")
	DirAccess.make_dir_recursive_absolute(TMP + "/terrain")
	for d in ["/vegetation", "/terrain"]:
		for fn in DirAccess.get_files_at(TMP + d):
			DirAccess.remove_absolute(TMP + d + "/" + fn)
	var fpath := "%s/vegetation/forest_%d_%d.bin" % [TMP, TILE_E, TILE_N]
	_write_forest(fpath, TILE_E, TILE_N)
	_write_terrain(TMP + "/terrain", TILE_E, TILE_N)
	_write_terrain(TMP + "/terrain", TILE_E + 1024, TILE_N)

	var tm := S3DTileManager.new()
	tm.tile_size = 1024
	var img: Image = tm.landcover_image(fpath, 128)
	_check(img != null and img.get_width() == 128 and img.get_height() == 128, "128 px raster of the forest file")
	if img != null:
		var conifer := _px(img, 300.0, 400.0)
		var broad := _px(img, 700.0, 700.0)
		_check(_is_forest(conifer), "conifer square painted forest green (%s)" % conifer)
		_check(_is_forest(broad), "broadleaf square painted forest green (%s)" % broad)
		_check(broad.g > conifer.g, "broadleaf is the lighter, conifer the darker green")
		_check(_is_base(_px(img, 250.0, 250.0)), "the polygon's hole stays base colour")
		_check(_is_base(_px(img, 1000.0, 1000.0)) and _is_base(_px(img, 50.0, 50.0)), "ground outside the polygons is base colour")
		_check(_is_base(_px(img, 150.0, 400.0)), "the road corridor through the wood is cleared")
		_check(_is_forest(_px(img, 180.0, 400.0)), "the wood stands 30 m beside the road")
		# orientation: the broadleaf square (NE quarter) lies at low columns (east) and high rows (north)
		var ne := img.get_pixel(127 - int(floor(700.0 / 8.0)), int(floor(700.0 / 8.0)))
		var sw := img.get_pixel(127 - int(floor(50.0 / 8.0)), int(floor(50.0 / 8.0)))
		_check(_is_forest(ne) and _is_base(sw), "orthophoto orientation: column 0 is the east edge, row 0 the south edge")
		var forest_px := 0
		for row in range(128):
			for col in range(128):
				if _is_forest(img.get_pixel(col, row)):
					forest_px += 1
		# (160 000 - 10 000 - 400 x 14) + 40 000 m2 / 64 m2 per px = 2 881
		_check(forest_px > 2700 and forest_px < 3050, "forest area in pixels matches the polygons (%d px, 2 881 expected)" % forest_px)
	var small: Image = tm.landcover_image(fpath, 16)
	_check(small != null and small.get_width() == 16, "16 px raster for a far chunk sub-tile")
	if small != null:
		_check(_is_forest(_px(small, 300.0, 400.0)) and _is_base(_px(small, 900.0, 100.0)), "16 px raster paints the wood and leaves the field")
	_check(tm.landcover_image(TMP + "/vegetation/missing.bin", 128) == null, "a missing forest file gives no raster")
	tm.free()

	# the tile manager inside a Scenery3D node: the forest square gets its own
	# material, the plain neighbour keeps the shared one
	var sc := Scenery3D.new()
	sc.frame_mode = 0
	sc.origin_east = TILE_E + 1024.0
	sc.origin_north = TILE_N + 512.0
	sc.data_paths = PackedStringArray([TMP + "/terrain"])
	sc.landcover_paths = PackedStringArray([TMP + "/vegetation"])
	sc.load_radius = 1
	add_child(sc)
	var cam := Camera3D.new()
	add_child(cam)
	cam.global_position = Vector3(0.0, 1500.0, 0.0)
	cam.current = true
	var tile_a: Node = null
	var tile_b: Node = null
	var waited := 0.0
	while waited < 20.0 and (tile_a == null or tile_b == null or tile_a.mesh == null or tile_b.mesh == null):
		await get_tree().process_frame
		waited += get_process_delta_time()
		tile_a = null
		tile_b = null
		for child in sc.get_children():
			if child.get_class() != "S3DTileManager":
				continue
			for t in child.get_children():
				if not (t is S3DTile):
					continue
				if t.tile_x == TILE_E / 1024 and t.tile_z == TILE_N / 1024:
					tile_a = t
				elif t.tile_x == TILE_E / 1024 + 1 and t.tile_z == TILE_N / 1024:
					tile_b = t
	_check(tile_a != null and tile_b != null, "both synthetic terrain tiles loaded")
	if tile_a != null and tile_b != null:
		var mat_a: Material = tile_a.mesh.surface_get_material(0) if tile_a.mesh != null else null
		var mat_b: Material = tile_b.mesh.surface_get_material(0) if tile_b.mesh != null else null
		var tex_a: Texture2D = mat_a.get_texture(BaseMaterial3D.TEXTURE_ALBEDO) if mat_a is StandardMaterial3D else null
		var tex_b: Texture2D = mat_b.get_texture(BaseMaterial3D.TEXTURE_ALBEDO) if mat_b is StandardMaterial3D else null
		_check(tex_a != null and tex_a.get_width() == 128, "the forest square's tile carries a 128 px land-cover texture")
		_check(tex_b == null, "the square without a forest file keeps the shared ground material")
	if _fails.is_empty():
		print("smoke_landcover: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_landcover: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
