extends Node
## Headless smoke for Scenery3D frame_mode = 1 (ECEF, design §9.5 step C1):
## a standalone Scenery3D streams the Swiss terrain around LSZH with the
## camera positioned in ECEF; tile and chunk nodes must land on the WGS84
## ellipsoid at their LV95 tile centres, mesh corners must map back to the
## LV95 grid within centimetres, and elevation queries must work through
## geodetic / world entry points.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_scenery_ecef.tscn

const TERRAIN := "/Users/gery/provpilot/scenery/Switzerland/terrain"
const LAT := 47.4647   # LSZH
const LON := 8.5492
const SETTLE := 500

var s3d: Node = null
var tm: Node = null
var cam: Camera3D = null
var frame := 0
var fails: Array[String] = []

func _ready() -> void:
	s3d = ClassDB.instantiate("Scenery3D")
	s3d.frame_mode = 1
	s3d.data_paths = PackedStringArray([TERRAIN])
	s3d.load_radius = 6
	s3d.far_radius = 40
	add_child(s3d)
	cam = Camera3D.new()
	add_child(cam)
	cam.global_position = W3DCoords.geodetic_to_ecef(LAT, LON, 1500.0)
	cam.make_current()

func _fail(m: String) -> void:
	fails.append(m)
	printerr("FAIL: " + m)

func _process(_d: float) -> void:
	frame += 1
	if frame == 2:
		for c in s3d.get_children():
			if c.has_method("update_tiles"):
				tm = c
		if tm == null:
			_fail("no tile manager"); _finish(); return
		print("[ecef] tm frame_mode=%d cam |p|=%.0f" % [tm.get_frame_mode(), cam.global_position.length()])
	if frame == SETTLE:
		_check()
		_finish()

func _check() -> void:
	var tiles := 0
	var chunks := 0
	var worst_corner := 0.0
	var worst_center := 0.0
	var worst_vertex := 0.0
	var checked := 0
	for c in tm.get_children():
		if not (c is MeshInstance3D) or not c.has_method("get_tile_x"):
			continue
		var span: int = int(round(float(c.get_tile_size()) / 1024.0))
		var xf: Transform3D = c.global_transform
		var det: float = xf.basis.determinant()
		if span == 1:
			tiles += 1
			if checked < 40 and c.mesh != null:
				checked += 1
				# Local origin corner = grid anchor ((ei+1)*1024, ni*1024);
				# far corner (1024,0,1024) = (ei*1024, (ni+1)*1024).
				var anchor_e: float = float(c.get_tile_x() + 1) * 1024.0
				var anchor_n: float = float(c.get_tile_z()) * 1024.0
				for corner in [[Vector3(0, 0, 0), anchor_e, anchor_n], [Vector3(1024, 0, 1024), anchor_e - 1024.0, anchor_n + 1024.0]]:
					var w: Vector3 = xf * corner[0]
					var ll: Vector3 = W3DCoords.ecef_to_geodetic(w)
					var lv: Vector2 = Geo.wgs84_to_lv95(ll.x, ll.y)
					var err: float = Vector2(lv.x - corner[1], lv.y - corner[2]).length()
					worst_corner = maxf(worst_corner, err)
					# corner at local y=0 must sit on the ellipsoid (h≈0) up to the sag (2 cm)
					if absf(ll.z) > 0.1:
						_fail("tile (%d,%d) corner height off ellipsoid: %.3f m" % [c.get_tile_x(), c.get_tile_z(), ll.z])
				# centre of the tile: geodetic of origin+basis*(512,0,512) → LV95 centre
				var wc: Vector3 = xf * Vector3(512, 0, 512)
				var llc: Vector3 = W3DCoords.ecef_to_geodetic(wc)
				var lvc: Vector2 = Geo.wgs84_to_lv95(llc.x, llc.y)
				worst_center = maxf(worst_center, Vector2(lvc.x - (anchor_e - 512.0), lvc.y - (anchor_n + 512.0)).length())
				# Vertex heights: the far-corner mesh vertex (sag-corrected in
				# tile-local y) must come back at the DTM height above the
				# ellipsoid — i.e. the curvature term cancels exactly there.
				var arrays: Array = c.mesh.surface_get_arrays(0)
				var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
				if verts.size() > 0:
					var v: Vector3 = verts[verts.size() - 1]  # last vertex = (1024, h, 1024) corner
					var wv: Vector3 = xf * v
					var llv: Vector3 = W3DCoords.ecef_to_geodetic(wv)
					var h_db: float = s3d.get_elevation_grid(anchor_e - 1024.0 + 0.01, anchor_n + 1024.0 - 0.01)
					if not is_nan(h_db):
						worst_vertex = maxf(worst_vertex, absf(llv.z - h_db))
				# the node's up axis must be the local ellipsoid normal
				var up_expected: Vector3 = W3DCoords.geodetic_normal(llc.x, llc.y)
				if xf.basis.y.normalized().dot(up_expected) < 0.99999:
					_fail("tile (%d,%d) up axis is not the ellipsoid normal" % [c.get_tile_x(), c.get_tile_z()])
		else:
			chunks += 1
			# Exact placement: pure rotation (det 1, orthonormal), origin on the ellipsoid − 30 m bias
			if absf(det - 1.0) > 1e-6:
				_fail("chunk basis det %.6f (expected 1)" % det)
			var llo: Vector3 = W3DCoords.ecef_to_geodetic(xf.origin)
			if absf(llo.z + 30.0) > 0.05:
				_fail("chunk origin height %.3f (expected -30 m bias)" % llo.z)
	print("[ecef] tiles=%d chunks=%d checked=%d worst corner err %.4f m, worst centre err %.4f m, worst vertex height err %.3f m" % [tiles, chunks, checked, worst_corner, worst_center, worst_vertex])
	if worst_vertex > 1.0:
		_fail("vertex height error %.3f m > 1 m (sag/placement)" % worst_vertex)
	if tiles == 0:
		_fail("no tiles streamed")
	if worst_corner > 0.10:
		_fail("tile corner grid error %.3f m > 0.10 m" % worst_corner)
	if worst_center > 0.05:
		_fail("tile centre grid error %.3f m > 0.05 m" % worst_center)
	# Elevation through the geodetic and world entry points (LSZH ~ 432 m ASL).
	var h_geo: float = s3d.get_elevation_geodetic(LAT, LON)
	var h_world: float = s3d.get_elevation_world(W3DCoords.geodetic_to_ecef(LAT, LON, 900.0))
	print("[ecef] elevation LSZH geodetic=%.1f world=%.1f" % [h_geo, h_world])
	if is_nan(h_geo) or h_geo < 400.0 or h_geo > 470.0:
		_fail("geodetic elevation at LSZH implausible: %s" % str(h_geo))
	if is_nan(h_world) or absf(h_world - h_geo) > 0.01:
		_fail("world elevation differs from geodetic: %s vs %s" % [str(h_world), str(h_geo)])
	# Coords helper round trip in ECEF mode.
	var coords = s3d.get_coords()
	var w: Vector3 = coords.wgs84_to_world_h(LAT, LON, 500.0)
	var back: Vector3 = coords.world_to_lv95(w)
	var lv_ref: Vector2 = Geo.wgs84_to_lv95(LAT, LON)
	if Vector2(back.x - lv_ref.x, back.y - lv_ref.y).length() > 0.01 or absf(back.z - 500.0) > 0.01:
		_fail("S3DCoords ECEF round trip: %s vs %s h=%.3f" % [str(back), str(lv_ref), back.z])

func _finish() -> void:
	if fails.is_empty():
		print("smoke_scenery_ecef: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_scenery_ecef: %d failure(s)" % fails.size())
		get_tree().quit(1)
