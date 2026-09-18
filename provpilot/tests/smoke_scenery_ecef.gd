extends Node
## Headless smoke for Scenery3D frame_mode = 1 (ECEF, design §9.5 step C1):
## a standalone Scenery3D streams the Swiss terrain around LSZH with the
## camera positioned in ECEF; tile and chunk nodes must land on the WGS84
## ellipsoid at their LV95 tile centres, mesh corners must map back to the
## LV95 grid within centimetres, and elevation queries must work through
## geodetic / world entry points.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_scenery_ecef.tscn

const TERRAIN := "/Users/gery/provpilot/scenery/Switzerland/terrain"
const TERRAIN_NO := "/Volumes/Data1/provpilot/build/norway/Norway/terrain"
const LAT := 47.4647   # LSZH
const LON := 8.5492
const LAT_NO := 60.1939  # ENGM — 1 400 km from the LV95 origin, convergence -2.7°
const LON_NO := 11.1004
const SETTLE := 500

var s3d: Node = null
var tm: Node = null
var cam: Camera3D = null
var frame := 0
var fails: Array[String] = []

func _ready() -> void:
	s3d = ClassDB.instantiate("Scenery3D")
	s3d.frame_mode = 1
	s3d.data_paths = PackedStringArray([TERRAIN, TERRAIN_NO])
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
		_seam_check("LSZH", 0.005)
		# Second phase far from the CRS origin where grid convergence and
		# scale vary fastest between neighbouring tiles.
		cam.global_position = W3DCoords.geodetic_to_ecef(LAT_NO, LON_NO, 1500.0)
	if frame == 2 * SETTLE:
		_seam_check("ENGM", 0.005)
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
				# Mesh vertices are WUN-local (centred on the tile, exact corners,
				# bilinear interior): corner vertex (0,0) is the grid anchor
				# ((ei+1)*1024, ni*1024) and corner (n-1,n-1) is (ei*1024, (ni+1)*1024).
				var anchor_e: float = float(c.get_tile_x() + 1) * 1024.0
				var anchor_n: float = float(c.get_tile_z()) * 1024.0
				for corner in [[0, 0, anchor_e, anchor_n], [1, 1, anchor_e - 1024.0, anchor_n + 1024.0]]:
					var v: Vector3 = _corner_vertex(c, corner[0], corner[1])
					var w: Vector3 = xf * v
					var ll: Vector3 = W3DCoords.ecef_to_geodetic(w)
					var lv: Vector2 = Geo.wgs84_to_lv95(ll.x, ll.y)
					worst_corner = maxf(worst_corner, Vector2(lv.x - corner[2], lv.y - corner[3]).length())
					# corner vertex height (sag-corrected local y) must come back at the DTM height
					var h_db: float = s3d.get_elevation_grid(corner[2] + (-0.01 if corner[0] == 0 else 0.01), corner[3] + (0.01 if corner[1] == 0 else -0.01))
					if not is_nan(h_db):
						worst_vertex = maxf(worst_vertex, absf(ll.z - h_db))
				# node origin = tile centre on the ellipsoid
				var llc: Vector3 = W3DCoords.ecef_to_geodetic(xf.origin)
				var lvc: Vector2 = Geo.wgs84_to_lv95(llc.x, llc.y)
				worst_center = maxf(worst_center, Vector2(lvc.x - (anchor_e - 512.0), lvc.y - (anchor_n + 512.0)).length())
				if absf(llc.z) > 0.01:
					_fail("tile (%d,%d) origin height off ellipsoid: %.3f m" % [c.get_tile_x(), c.get_tile_z(), llc.z])
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

## Shared-edge consistency: for every loaded 1 km tile with a loaded east
## neighbour, the world positions of the two meshes' shared corner vertices
## (this tile's local x=0 edge vs the neighbour's x=1024 edge) must agree.
func _seam_check(tag: String, tol_m: float) -> void:
	var by_key := {}
	for c in tm.get_children():
		if c is MeshInstance3D and c.has_method("get_tile_x") and c.get_tile_size() == 1024 and c.mesh != null:
			by_key["%d,%d" % [c.get_tile_x(), c.get_tile_z()]] = c
	var worst := 0.0
	var worst_info := ""
	var worst_up := 0.0
	var pairs := 0
	for k in by_key.keys():
		var a: MeshInstance3D = by_key[k]
		var nb_key := "%d,%d" % [a.get_tile_x() + 1, a.get_tile_z()]  # east neighbour (E + 1024)
		if not by_key.has(nb_key):
			continue
		var b: MeshInstance3D = by_key[nb_key]
		# Tile a: local x = anchor_e - E with anchor_e = (ei+1)*1024 -> its x=0 edge is E=(ei+1)*1024.
		# Tile b (ei+1): its x=1024 edge is E=(ei+1)*1024. Compare the two corner rows z=0 and z=1024.
		for z in [0.0, 1024.0]:
			var iz: int = 0 if z == 0.0 else 1
			var va: Vector3 = _corner_vertex(a, 0, iz)   # a's x=0 edge (east edge, E = (ei+1)*1024)
			var vb: Vector3 = _corner_vertex(b, 1, iz)   # b's x=1024 edge (its west edge, same E)
			var pa: Vector3 = a.global_transform * va
			var pb: Vector3 = b.global_transform * vb
			var ga: Vector3 = W3DCoords.ecef_to_geodetic(pa)
			var upv: Vector3 = W3DCoords.geodetic_normal(ga.x, ga.y)
			var dv: Vector3 = pb - pa
			var d_up: float = absf(dv.dot(upv))
			var d_h: float = (dv - upv * dv.dot(upv)).length()
			worst_up = maxf(worst_up, d_up)
			if d_h > worst:
				worst = d_h
				worst_info = "pair %s|%s lod %d|%d va=%s vb=%s" % [k, nb_key, a.get_lod_level(), b.get_lod_level(), str(va), str(vb)]
		pairs += 1
	# Horizontal mismatch is the ECEF placement's responsibility; a vertical
	# step between different LODs is Scenery3D's pre-existing edge-sampling
	# behaviour (reported, not asserted).
	print("[ecef] %s seam check: %d neighbour pairs, worst HORIZONTAL shared-corner mismatch %.4f m (%s); worst vertical LOD step %.2f m" % [tag, pairs, worst, worst_info, worst_up])
	if pairs == 0:
		_fail("%s seam check: no neighbouring tile pairs loaded" % tag)
	elif worst > tol_m:
		_fail("%s seam check: shared corners mismatch %.3f m > %.3f m horizontally" % [tag, worst, tol_m])

## Corner vertex of a square tile mesh by grid index (ix, iz in {0, 1}):
## vertex order is gz * n + gx with n = sqrt(vertex count).
func _corner_vertex(t: MeshInstance3D, ix: int, iz: int) -> Vector3:
	var verts: PackedVector3Array = t.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
	var n: int = int(round(sqrt(float(verts.size()))))
	var gx: int = (n - 1) if ix == 1 else 0
	var gz: int = (n - 1) if iz == 1 else 0
	return verts[gz * n + gx]

func _finish() -> void:
	if fails.is_empty():
		print("smoke_scenery_ecef: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_scenery_ecef: %d failure(s)" % fails.size())
		get_tree().quit(1)
