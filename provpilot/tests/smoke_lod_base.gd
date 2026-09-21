extends Node
## Headless smoke: LOD behaviour of the 30 m base packs (design §11.5).
## Around the Zugspitze with the Germany base terrain at load_radius 16:
##   - every tile out to ring 12 keeps its native 31 m mesh (same vertex
##     count as the tile under the camera),
##   - the 12–16 ring is exactly one stride coarser,
##   - the nearest far chunks are built at 129 px, far ones at 33 px,
##   - a chunk's mesh is finer than the old 33 px composite by the expected
##     factor, so the tile/chunk handoff keeps a 62 m spacing.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_lod_base.tscn

const TERRAIN := "/Volumes/Data1/provpilot/build/germany/Germany/terrain"
const LAT := 47.42107   # Zugspitze
const LON := 10.98546
const SETTLE := 900

var s3d: Node = null
var tm: Node = null
var cam: Camera3D = null
var frame := 0
var fails: Array[String] = []

func _fail(m: String) -> void:
	fails.append(m)
	printerr("FAIL: " + m)

func _ready() -> void:
	if not DirAccess.dir_exists_absolute(TERRAIN):
		print("smoke_lod_base: Germany base pack not present, skipping")
		get_tree().quit(0)
		return
	s3d = ClassDB.instantiate("Scenery3D")
	s3d.frame_mode = 1
	s3d.data_paths = PackedStringArray([TERRAIN])
	s3d.load_radius = 16
	s3d.far_radius = 80
	add_child(s3d)
	cam = Camera3D.new()
	add_child(cam)
	cam.global_position = W3DCoords.geodetic_to_ecef(LAT, LON, 4500.0)
	cam.make_current()

func _vert_count(c: MeshInstance3D) -> int:
	if c.mesh == null or c.mesh.get_surface_count() == 0:
		return 0
	return c.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size()

func _process(_d: float) -> void:
	frame += 1
	if frame == 2:
		for c in s3d.get_children():
			if c.has_method("update_tiles"):
				tm = c
		if tm == null:
			_fail("no tile manager"); _finish(); return
	if frame < SETTLE:
		return
	var lv: Vector2 = Geo.wgs84_to_lv95(LAT, LON)
	var cam_ei := int(floor(lv.x / 1024.0))
	var cam_ni := int(floor(lv.y / 1024.0))
	var verts_by_dist: Dictionary = {}     # tile ring distance -> {verts: count}
	var chunk_verts_by_dist: Dictionary = {}
	for c in tm.get_children():
		if not (c is MeshInstance3D) or not c.has_method("get_tile_x"):
			continue
		var span: int = int(round(float(c.get_tile_size()) / 1024.0))
		var n := _vert_count(c)
		if n == 0:
			continue
		if span == 1:
			var d: int = maxi(absi(c.get_tile_x() - cam_ei), absi(c.get_tile_z() - cam_ni))
			var s: Dictionary = verts_by_dist.get(d, {})
			s[n] = s.get(n, 0) + 1
			verts_by_dist[d] = s
		else:
			# Chunk node: tile_x/tile_z are the chunk GRID indices (8-tile units).
			var bx: int = c.get_tile_x() * span
			var bz: int = c.get_tile_z() * span
			var cx: int = clampi(cam_ei, bx, bx + span - 1)
			var cz: int = clampi(cam_ni, bz, bz + span - 1)
			var d: int = maxi(absi(cx - cam_ei), absi(cz - cam_ni))
			var s: Dictionary = chunk_verts_by_dist.get(d, {})
			s[n] = s.get(n, 0) + 1
			chunk_verts_by_dist[d] = s
	var dists: Array = verts_by_dist.keys()
	dists.sort()
	for d in dists:
		print("[lod] ring %2d tiles: %s" % [d, str(verts_by_dist[d])])
	var cdists: Array = chunk_verts_by_dist.keys()
	cdists.sort()
	for d in cdists:
		print("[lod] chunk dist %2d: %s" % [d, str(chunk_verts_by_dist[d])])

	if not verts_by_dist.has(0) or not verts_by_dist.has(12) or not verts_by_dist.has(14):
		_fail("tiles missing at ring 0/12/14 — not settled")
		_finish(); return
	var v0: int = verts_by_dist[0].keys()[0]
	for d in range(0, 13):
		if not verts_by_dist.has(d):
			continue
		for n in verts_by_dist[d].keys():
			if n != v0:
				_fail("ring %d tile has %d vertices, expected native %d" % [d, n, v0])
	var v14: int = verts_by_dist[14].keys()[0]
	if v14 >= v0:
		_fail("ring 14 tile (%d verts) should be coarser than native (%d)" % [v14, v0])
	# Native 34 px: 1156 grid verts; stride 2: 289 (skirts are not part of
	# the vertex array, so the counts are exact).
	if v0 != 1156:
		_fail("native 34 px tile should have 1156 vertices, got %d" % v0)
	if v14 != 289:
		_fail("ring 14 tile should be stride 2 (289 vertices), got %d" % v14)

	if cdists.is_empty():
		_fail("no far chunks loaded")
	else:
		var near_d: int = cdists[0]
		var far_d: int = cdists[cdists.size() - 1]
		var near_n: int = chunk_verts_by_dist[near_d].keys().max()
		var far_n: int = chunk_verts_by_dist[far_d].keys().min()
		print("[lod] nearest chunk (dist %d) %d verts, farthest chunk (dist %d) %d verts" % [near_d, near_n, far_d, far_n])
		# 129² = 16641 vs 33² = 1089 grid verts.
		if near_n != 16641:
			_fail("nearest chunk should be a 129 px composite (16641 verts), got %d" % near_n)
		if far_d > 68 and far_n != 1089:
			_fail("farthest chunk (dist %d) should be a 33 px composite (1089 verts), got %d" % [far_d, far_n])
	_finish()

func _finish() -> void:
	if fails.is_empty():
		print("smoke_lod_base: all checks passed")
		get_tree().quit(0)
	else:
		printerr("smoke_lod_base: %d failure(s)" % fails.size())
		get_tree().quit(1)
