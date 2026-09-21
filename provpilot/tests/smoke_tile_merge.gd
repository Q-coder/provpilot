extends Node
## Headless smoke: two terrain sources of different resolution covering the
## same tile (design §11 D13). Source A is a 1025 px "detail" tile whose
## north-west triangle is no-data (0.0) — the shape of the BW pack's eastern
## edge. Source B is a 34 px "base" tile at a constant elevation. Listed as
## [A, B], the loaded tile must keep A where it has data and take B in the
## wedge instead of a 0 m cliff or a smear of A's own edge samples.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_tile_merge.tscn

const EI := 2600          # tile index (E 2 662 400, N 1 228 800 — near Bern)
const NI := 1200
const PX_A := 1025
const PX_B := 34
const ELEV_A := 800.0
const ELEV_B := 500.0
const SETTLE := 240

var s3d: Node = null
var frame := 0
var fails: Array[String] = []
var root := ""

func _fail(m: String) -> void:
	fails.append(m)
	printerr("FAIL: " + m)

func _write_tile(dir: String, px: int, elev: float, wedge: bool) -> void:
	DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(dir.path_join("manifest.json"), FileAccess.WRITE)
	f.store_string('{"tile_size_m": 1024, "pixels_per_tile": %d, "crs": "EPSG:2056", "format": "float32_raw_le"}\n' % px)
	f.close()
	var a := PackedFloat32Array()
	a.resize(px * px)
	for r in range(px):
		for c in range(px):
			# col 0 = east edge, row 0 = south edge: the NW triangle is
			# c + r > px - 1 (west = high c, north = high r).
			a[r * px + c] = 0.0 if (wedge and c + r > px - 1) else elev
	f = FileAccess.open(dir.path_join("tile_%d_%d.raw" % [EI * 1024, NI * 1024]), FileAccess.WRITE)
	f.store_buffer(a.to_byte_array())
	f.close()

func _ready() -> void:
	root = OS.get_temp_dir().path_join("provpilot_smoke_tile_merge")
	_write_tile(root.path_join("A/terrain"), PX_A, ELEV_A, true)
	_write_tile(root.path_join("B/terrain"), PX_B, ELEV_B, false)
	s3d = ClassDB.instantiate("Scenery3D")
	s3d.frame_mode = 1
	s3d.data_paths = PackedStringArray([root.path_join("A/terrain"), root.path_join("B/terrain")])
	s3d.load_radius = 2
	s3d.far_radius = 4
	add_child(s3d)
	var cam := Camera3D.new()
	add_child(cam)
	var ll: Vector2 = Geo.lv95_to_wgs84(EI * 1024.0 + 512.0, NI * 1024.0 + 512.0)
	cam.global_position = W3DCoords.geodetic_to_ecef(ll.x, ll.y, 1500.0)
	cam.make_current()

func _process(_d: float) -> void:
	frame += 1
	if frame < SETTLE:
		return
	# The wedge is the triangle north-west of the SW-NE diagonal, i.e.
	# local_n > local_e. South-east of it the detail data is present.
	var e_se := EI * 1024.0 + 800.0
	var n_se := NI * 1024.0 + 200.0
	# Inside the no-data wedge: the base pack must fill.
	var e_nw := EI * 1024.0 + 200.0
	var n_nw := NI * 1024.0 + 800.0
	var h_se: float = s3d.get_elevation_grid(e_se, n_se)
	var h_nw: float = s3d.get_elevation_grid(e_nw, n_nw)
	print("[merge] detail area %.1f m, wedge %.1f m" % [h_se, h_nw])
	if is_nan(h_se) or absf(h_se - ELEV_A) > 1.0:
		_fail("detail area should read %.0f m, got %s" % [ELEV_A, str(h_se)])
	if is_nan(h_nw) or absf(h_nw - ELEV_B) > 1.0:
		_fail("no-data wedge should be filled from the base pack (%.0f m), got %s" % [ELEV_B, str(h_nw)])
	if fails.is_empty():
		print("smoke_tile_merge: all checks passed")
		get_tree().quit(0)
	else:
		printerr("smoke_tile_merge: %d failure(s)" % fails.size())
		get_tree().quit(1)
