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
## width 6 m — the forest smoke's square — plus land cover: a lake (600..800 x
## 100..300), farmland (100..500 x 600..1000) and a town (600..1000 x 350..550).
func _write_forest(path: String, e: int, n: int) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer("FOR1".to_ascii_buffer())
	f.store_32(1)
	f.store_32(6)
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
	_poly_header(f, 4, 0.0, 0.0, 1)
	_ring(f, [Vector2(600, 100), Vector2(800, 100), Vector2(800, 300), Vector2(600, 300), Vector2(600, 100)])
	_poly_header(f, 5, 0.0, 0.0, 1)
	_ring(f, [Vector2(100, 600), Vector2(500, 600), Vector2(500, 1000), Vector2(100, 1000), Vector2(100, 600)])
	_poly_header(f, 7, 0.0, 0.0, 1)
	_ring(f, [Vector2(600, 350), Vector2(1000, 350), Vector2(1000, 550), Vector2(600, 550), Vector2(600, 350)])
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
		var road := _px(img, 150.0, 400.0)
		_check(road.r - conifer.r > 0.1 and absf(road.r - road.g) < 0.08, "the road corridor through the wood is a faint grey line blended into the ground (%s)" % road)
		var water := _px(img, 700.0, 200.0)
		_check(water.b > water.g and water.g > water.r and water.b < 0.5, "the lake is a dark blue-grey (%s)" % water)
		var shades := {}
		for i in range(20):
			var c := _px(img, 120.0 + i * 19.0, 620.0 + (i * 7) % 380)
			shades[Color8(int(c.r8 / 12) * 12, int(c.g8 / 12) * 12, int(c.b8 / 12) * 12)] = true
		_check(shades.size() >= 3, "farmland is a mosaic of parcels, not one tint (%d shades in 20 samples)" % shades.size())
		var town_sum := Color(0, 0, 0)
		var town_dark := 0
		for i in range(40):
			var c := _px(img, 610.0 + (i * 37) % 380, 360.0 + (i * 13) % 180)
			town_sum += c
			if c.r < 0.5 and c.g < 0.5:
				town_dark += 1
		town_sum /= 40.0
		_check(absf(town_sum.r - town_sum.g) < 0.12 and town_dark >= 4, "the town is a grey-brown speckle of roofs and gardens (mean %s, %d dark of 40)" % [town_sum, town_dark])
		_check(_is_forest(_px(img, 180.0, 400.0)), "the wood stands 30 m beside the road")
		# orientation: the broadleaf square (NE quarter) lies at low columns (east) and high rows (north)
		var ne := img.get_pixel(127 - int(floor(700.0 / 8.0)), int(floor(700.0 / 8.0)))
		var sw := img.get_pixel(127 - int(floor(50.0 / 8.0)), int(floor(50.0 / 8.0)))
		var ne_w := img.get_pixel(127 - int(floor(700.0 / 8.0)), int(floor(200.0 / 8.0)))
		_check(ne_w.b > ne_w.g, "orientation: the lake lies at low columns (east) and low rows (south)")
		_check(_is_forest(ne) and _is_base(sw), "orthophoto orientation: column 0 is the east edge, row 0 the south edge")
		var forest_px := 0
		for row in range(128):
			for col in range(128):
				if _is_forest(img.get_pixel(col, row)):
					forest_px += 1
		# (160 000 - 10 000 - 400 x 14) + 40 000 m2 / 64 m2 per px = 2 881
		_check(forest_px > 2700 and forest_px < 3050, "forest area in pixels matches the polygons (%d px, 2 881 expected; fields, town and lake not counted)" % forest_px)
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
		_check(tex_a != null and (tex_a.get_width() == 128 or tex_a.get_width() == 256), "the forest square's tile carries a land-cover texture (%d px)" % (tex_a.get_width() if tex_a != null else 0))
		_check(tex_b == null, "the square without a forest file keeps the shared ground material")
	# the far chunk under the two tiles: a masked shader material that discards
	# its fragments where an individual tile is drawn
	var tm_node: Node = null
	for child in sc.get_children():
		if child.get_class() == "S3DTileManager":
			tm_node = child
	var chunk: Node = null
	waited = 0.0
	while waited < 20.0 and chunk == null:
		await get_tree().process_frame
		waited += get_process_delta_time()
		for t in tm_node.get_children():
			if t is S3DTile and t.tile_size > 1024 and t.mesh != null:
				chunk = t
	_check(chunk != null, "a far chunk is resident under the close ring")
	if chunk != null:
		var cmat: Material = chunk.mesh.surface_get_material(0)
		_check(cmat is ShaderMaterial, "the chunk draws with the masked chunk shader")
		if cmat is ShaderMaterial:
			var rect: Vector4 = cmat.get_shader_parameter("cover_rect")
			_check(rect.z > 0.0, "the straddling chunk carries the cover mask (%s)" % rect)
			var base: Vector2 = cmat.get_shader_parameter("chunk_base")
			_check(int(base.x) == (TILE_E / 1024 / 8) * 8 and int(base.y) == (TILE_N / 1024 / 8) * 8, "chunk_base is the chunk's south-west square (%s)" % base)
		_check(tm_node.chunk_cover_covers(TILE_E / 1024, TILE_N / 1024) and tm_node.chunk_cover_covers(TILE_E / 1024 + 1, TILE_N / 1024), "both drawn tiles are marked in the cover mask")
		_check(not tm_node.chunk_cover_covers(TILE_E / 1024 + 2, TILE_N / 1024), "a square without a tile is not marked")
		_check(tm_node.get_masked_chunk_count() >= 1, "at least one chunk is masked (%d)" % tm_node.get_masked_chunk_count())
	if _fails.is_empty():
		print("smoke_landcover: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_landcover: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
