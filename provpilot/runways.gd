extends Node3D
class_name Runways
## Runways — renders the merged per-pack airport surfaces (design §12).
##
## For every airport of RunwaysLib.load_merged(): ONE mesh ("Surfaces") that
## holds the procedural runway strips, taxiway ribbons and apron polygons
## (unless "procedural": false), plus an optional pack-relative "model"
## (glTF) at the reference point. Every vertex is projected exactly onto the
## WGS84 ellipsoid and expressed in the airport's West-Up-North frame (the
## frame is only an origin, so a 4 km runway follows the curvature).
##
## One ShaderMaterial is shared by every surface in the scene: the
## per-runway parameters (width, length, displaced thresholds, designator
## glyphs, stripe count, surface kind) travel in the CUSTOM0/CUSTOM1 vertex
## attributes. A material per runway (3 700 + 6 700 for taxiways/aprons)
## crashed the Metal driver in uniform_set_create (SIGBUS, 2026-09-20).
##
## Heights come from the plane / profile the terrain was flattened to
## (D18: `plane_elev_m`, taxiway and apron vertex `elev_m`), plus a lift so
## the strip wins the depth test: runway 5 cm > taxiway 3 cm > apron 2 cm.

# Depth order at junctions: apron < runway < taxiway. The taxiway ribbon
# is cut at runway pavement edges (on_runway flag → only the yellow line
# is drawn there), so it may sit above the runway.
const SURFACE_LIFT_M := 0.08
const TAXI_LIFT_M := 0.10
const APRON_LIFT_M := 0.03
const LINE_LIFT_M := 0.13     # taxiway guidance lines above every pavement
const LINE_HALF_W := 0.25
const SEGMENT_M := 50.0
const VISIBLE_RANGE_M := 60000.0

const KIND_ASPHALT := 0
const KIND_CONCRETE := 1
const KIND_GRASS := 2
const KIND_GRAVEL := 3
const KIND_TAXIWAY := 4
const KIND_APRON := 5
const KIND_TAXI_LINE := 6

# Distance-driven build: airports within BUILD_RADIUS_M of the camera are
# built (a few per update), those beyond FREE_RADIUS_M are freed. All of
# Europe at once took ~6 s (1 M triangles of pavement); the local set is
# a few hundred milliseconds.
const BUILD_RADIUS_M := 150000.0
const FREE_RADIUS_M := 250000.0
const BUILDS_PER_UPDATE := 4
const UPDATE_INTERVAL_S := 0.5

var _shader: Shader = preload("res://shaders/runway.gdshader")
var _material: ShaderMaterial = null
var _light_shader: Shader = preload("res://shaders/airport_lights.gdshader")
var _light_material: ShaderMaterial = null
var light_count := 0

# Lighting layout (metres). Colours are linear RGB.
const LIGHT_LIFT_M := 0.35
const EDGE_SPACING_M := 60.0
const EDGE_CAUTION_M := 600.0          # yellow edge lights in the last 600 m
const CENTRE_SPACING_M := 30.0
const CENTRE_MIN_LENGTH_M := 1800.0    # runway centreline lights and approach lights from this length
const THRESHOLD_SPACING_M := 6.0
const APPROACH_SPACING_M := 30.0
const APPROACH_LONG_M := 900.0         # international airports (Calvert-style bars)
const APPROACH_SHORT_M := 420.0        # other airports
const TAXI_CENTRE_SPACING_M := 15.0
const TAXI_EDGE_SPACING_M := 30.0
const PAPI_DISTANCE_M := 300.0
const PAPI_OFFSET_M := 15.0
const PAPI_UNIT_SPACING_M := 9.0
const PAPI_SIZE := 3.0                 # sprite scale: PAPI units are far brighter than edge lights
const OSM_CLASS_MIN := 8               # mapped fixtures per class needed to replace the generated ones
const COL_WHITE := Color(1.0, 0.97, 0.88)
const COL_YELLOW := Color(1.0, 0.75, 0.15)
const COL_GREEN := Color(0.15, 1.0, 0.35)
const COL_RED := Color(1.0, 0.12, 0.05)
const COL_BLUE := Color(0.25, 0.45, 1.0)
const COL_OFF := Color(0.0, 0.0, 0.0)
var _merged: Dictionary = {}
var _airport_nodes: Dictionary = {}   # icao -> Node3D holding that airport's mesh/model
var _airport_ecef: Dictionary = {}    # icao -> Vector3 reference point (for the distance test)
# icao -> {inv: Transform3D, runways: [[p0: Vector2, u: Vector2, length, half_w]],
#          aprons: [PackedVector2Array], verts: PackedVector3Array, idx: PackedInt32Array,
#          grid: {Vector2i: Array of triangle first-index}} — the 2D surface
# lookup behind surface_lift_at() (JSBSim's ground plane must be the drawn
# surface, not the terrain 8-10 cm below it).
var _lookup: Dictionary = {}
const LOOKUP_CELL_M := 50.0
const LOOKUP_RANGE_M := 8000.0
var _terrain_cb: Callable = Callable()
var _since_update := 0.0
var runway_count := 0
var model_count := 0
var taxiway_count := 0
var apron_count := 0


## Vertex arrays under construction for one airport mesh.
class _Batch:
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var custom0 := PackedFloat32Array()   # half_width, length, displaced0, displaced1
	var custom1 := PackedFloat32Array()   # glyphs0 packed, glyphs1 packed, kind*64+bars*2+aiming, 0
	var idx := PackedInt32Array()
	var runways: Array = []               # [{name, first, count, h0, h1, length}]
	var rwy_rects: Array = []             # [[p0: Vector2, u: Vector2, length, half_w]] local x/z
	var apron_polys: Array = []           # [PackedVector2Array] local x/z
	var taxi_tri_first: Array = []        # first index of every pavement triangle in idx

	func add_vertex(p: Vector3, uv: Vector2, c0: Array, c1: Array) -> int:
		verts.append(p)
		normals.append(Vector3.UP)
		uvs.append(uv)
		custom0.append_array(PackedFloat32Array(c0))
		custom1.append_array(PackedFloat32Array(c1))
		return verts.size() - 1


func _ensure_material() -> void:
	if _material == null:
		_material = ShaderMaterial.new()
		_material.shader = _shader
	if _light_material == null:
		_light_material = ShaderMaterial.new()
		_light_material.shader = _light_shader
		_light_material.set_shader_parameter("intensity", 0.0)


## 0 = day (lights off) … 1 = night / low visibility. Set by the host.
func set_light_intensity(v: float) -> void:
	_ensure_material()
	_light_material.set_shader_parameter("intensity", clampf(v, 0.0, 1.0))


func get_light_material() -> ShaderMaterial:
	_ensure_material()
	return _light_material


## Take a merged airport dictionary (icao -> entry, RunwaysLib.load_merged)
## and let update() build the airports near the camera. Existing nodes are
## dropped (the pack set changed).
func set_merged(merged: Dictionary, terrain_height_geodetic: Callable = Callable()) -> void:
	_ensure_material()
	for n in _airport_nodes.values():
		n.queue_free()
	_airport_nodes.clear()
	_airport_ecef.clear()
	_lookup.clear()
	_merged = merged
	_terrain_cb = terrain_height_geodetic
	runway_count = 0
	model_count = 0
	taxiway_count = 0
	apron_count = 0
	light_count = 0
	for icao in merged.keys():
		var ap: Dictionary = merged[icao]
		_airport_ecef[icao] = W3DCoords.geodetic_to_ecef(float(ap.get("lat", 0.0)), float(ap.get("lon", 0.0)), float(ap.get("elevation_m", 0.0)))
	_since_update = UPDATE_INTERVAL_S


## Per frame from the host: build airports entering BUILD_RADIUS_M of
## `cam_pos` (ECEF), free those beyond FREE_RADIUS_M.
func update(cam_pos: Vector3, delta: float) -> void:
	_since_update += delta
	if _since_update < UPDATE_INTERVAL_S or _merged.is_empty():
		return
	_since_update = 0.0
	var built := 0
	for icao in _merged.keys():
		var d: float = cam_pos.distance_to(_airport_ecef[icao])
		if _airport_nodes.has(icao):
			if d > FREE_RADIUS_M:
				_airport_nodes[icao].queue_free()
				_airport_nodes.erase(icao)
				_lookup.erase(icao)
		elif d < BUILD_RADIUS_M and built < BUILDS_PER_UPDATE:
			_build_airport(icao, _merged[icao], _terrain_cb)
			built += 1


## Build every airport of `merged` now (tests, probes).
func build(merged: Dictionary, terrain_height_geodetic: Callable = Callable()) -> void:
	set_merged(merged, terrain_height_geodetic)
	for icao in merged.keys():
		_build_airport(icao, merged[icao], terrain_height_geodetic)
	print("Runways: %d procedural runways, %d taxiways, %d aprons, %d models from %d airports (1 material)" % [runway_count, taxiway_count, apron_count, model_count, merged.size()])


func _build_airport(icao: String, ap: Dictionary, terrain_height_geodetic: Callable) -> void:
	if _airport_nodes.has(icao):
		return
	if true:
		var root := Node3D.new()
		root.name = str(icao)
		add_child(root)
		_airport_nodes[icao] = root
		var lat := float(ap.get("lat", 0.0))
		var lon := float(ap.get("lon", 0.0))
		var frame: Transform3D = W3DCoords.enu_to_ecef_transform(lat, lon, float(ap.get("elevation_m", 0.0)), true)
		if bool(ap.get("procedural", true)):
			var batch := _Batch.new()
			var inv: Transform3D = frame.affine_inverse()
			for rw in ap.get("runways", []):
				if rw is Dictionary and _add_runway(batch, inv, rw, terrain_height_geodetic):
					runway_count += 1
			_add_aprons(batch, inv, ap.get("aprons", []))
			_add_taxiway_mesh(batch, inv, ap.get("taxiway_mesh", {}))
			_add_taxiways(batch, inv, ap.get("taxiways", []))
			var mi := _mesh_instance(batch, frame)
			if mi != null:
				root.add_child(mi)
				_register_lookup(icao, batch, frame)
			var lm := _build_lights(ap, frame)
			if lm != null:
				root.add_child(lm)
		var model := str(ap.get("model", ""))
		if model != "":
			var node := _load_model(str(ap.get("pack_path", "")).path_join(model))
			if node != null:
				var h: float = float(ap.get("elevation_m", 0.0))
				if terrain_height_geodetic.is_valid():
					var th: float = terrain_height_geodetic.call(lat, lon)
					if is_finite(th) and th > -1000.0:
						h = th
				node.transform = W3DCoords.enu_to_ecef_transform(lat, lon, h, true)
				root.add_child(node)
				model_count += 1


func built_count() -> int:
	return _airport_nodes.size()


func get_airport_node(icao: String) -> Node3D:
	return _airport_nodes.get(icao, null)


## The shared material (for tests and for packs that want to tint it).
func get_material() -> ShaderMaterial:
	_ensure_material()
	return _material


static func _surface_kind(surface: String) -> int:
	match surface:
		"asphalt":
			return KIND_ASPHALT
		"concrete":
			return KIND_CONCRETE
		"grass":
			return KIND_GRASS
		_:
			return KIND_GRAVEL


static func _bars_for_width(w: float) -> int:
	if w < 20.0:
		return 4
	if w < 27.0:
		return 6
	if w < 38.0:
		return 8
	if w < 52.0:
		return 12
	return 16


static func _pack_glyphs(g: PackedInt32Array) -> float:
	# three codes in -1..12 → base-14 digits (code+1), exact in a float
	return float((g[0] + 1) * 196 + (g[1] + 1) * 14 + (g[2] + 1))


## Runway plane heights (h0, h1) from the flatten stage, the OurAirports
## ends, or the terrain sampler.
static func _end_heights(rw: Dictionary, p0: Vector2, p1: Vector2, terrain_height_geodetic: Callable) -> Vector2:
	var ends: Array = rw.get("ends", [])
	var h0 := NAN
	var h1 := NAN
	if ends.size() == 2:
		for key in ["plane_elev_m", "elev_m"]:
			if is_nan(h0) and (ends[0] as Dictionary).has(key):
				h0 = float(ends[0][key])
			if is_nan(h1) and (ends[1] as Dictionary).has(key):
				h1 = float(ends[1][key])
	if (is_nan(h0) or is_nan(h1)) and terrain_height_geodetic.is_valid():
		var ll0: Vector2 = Geo.lv95_to_wgs84(p0.x, p0.y)
		var ll1: Vector2 = Geo.lv95_to_wgs84(p1.x, p1.y)
		var t0: float = terrain_height_geodetic.call(ll0.x, ll0.y)
		var t1: float = terrain_height_geodetic.call(ll1.x, ll1.y)
		if is_nan(h0) and is_finite(t0):
			h0 = t0
		if is_nan(h1) and is_finite(t1):
			h1 = t1
	if is_nan(h0) and is_nan(h1):
		h0 = 0.0
		h1 = 0.0
	elif is_nan(h0):
		h0 = h1
	elif is_nan(h1):
		h1 = h0
	return Vector2(h0, h1)


func _add_runway(batch: _Batch, inv: Transform3D, rw: Dictionary, terrain_height_geodetic: Callable) -> bool:
	var ends: Array = rw.get("ends", [])
	var desigs: Array = rw.get("designators", [])
	if ends.size() != 2 or not (ends[0] as Dictionary).has("pavement") or not (ends[1] as Dictionary).has("pavement"):
		return false
	var ll0: Array = ends[0]["pavement"]
	var ll1: Array = ends[1]["pavement"]
	var p0: Vector2 = Geo.wgs84_to_lv95(float(ll0[0]), float(ll0[1]))
	var p1: Vector2 = Geo.wgs84_to_lv95(float(ll1[0]), float(ll1[1]))
	var axis: Vector2 = p1 - p0
	var length: float = axis.length()
	if length < 30.0:
		return false
	var u: Vector2 = axis / length
	var r: Vector2 = Vector2(u.y, -u.x)          # right of the along direction
	var half_w: float = 0.5 * float(rw.get("width_m", 45.0))
	var h: Vector2 = _end_heights(rw, p0, p1, terrain_height_geodetic)
	var g0: PackedInt32Array = RunwaysLib.glyph_codes(str(desigs[0]) if desigs.size() > 0 else "")
	var g1: PackedInt32Array = RunwaysLib.glyph_codes(str(desigs[1]) if desigs.size() > 1 else "")
	var kind := _surface_kind(str(rw.get("surface", "asphalt")))
	var bars := _bars_for_width(2.0 * half_w)
	var aiming := 1 if length >= 1800.0 else 0
	var c0: Array = [half_w, length, float((ends[0] as Dictionary).get("displaced_m", 0.0)), float((ends[1] as Dictionary).get("displaced_m", 0.0))]
	var c1: Array = [_pack_glyphs(g0), _pack_glyphs(g1), float(kind * 64 + bars * 2 + aiming), 0.0]

	# Vertex rows every SEGMENT_M plus a duplicated row at each crossing
	# boundary (bake: along-track intervals inside a longer runway's
	# pavement) so the marking cut is crisp; rows inside a crossing carry
	# flag 1 in CUSTOM1.w and the shader draws no markings there.
	var crossings: Array = rw.get("crossings", [])
	var rows: Array = []          # [along, flag]
	var segs: int = maxi(1, int(ceil(length / SEGMENT_M)))
	var cuts: Array[float] = []
	for cr in crossings:
		cuts.append(float(cr[0]))
		cuts.append(float(cr[1]))
	cuts.sort()
	var alongs: Array[float] = []
	for k in range(segs + 1):
		alongs.append(float(k) / float(segs) * length)
	for c in cuts:
		if c > 0.0 and c < length:
			alongs.append(c)
	alongs.sort()
	for along in alongs:
		var inside := 0.0
		for cr in crossings:
			if along > float(cr[0]) + 0.01 and along < float(cr[1]) - 0.01:
				inside = 1.0
		var is_cut := false
		for c in cuts:
			if absf(c - along) < 0.005:
				is_cut = true
		if is_cut:
			# before the boundary: state of the previous row; after: new state
			var prev: float = float(rows[rows.size() - 1][1]) if rows.size() > 0 else 0.0
			var nxt := 0.0
			for cr in crossings:
				if along >= float(cr[0]) - 0.01 and along < float(cr[1]) - 0.01:
					nxt = 1.0
			rows.append([along, prev])
			rows.append([along, nxt])
		else:
			rows.append([along, inside])
	var first := batch.verts.size()
	for row in rows:
		var along: float = row[0]
		var t: float = along / length
		var z: float = h.x + (h.y - h.x) * t + SURFACE_LIFT_M
		var c1r: Array = [c1[0], c1[1], c1[2], float(row[1])]
		for side in [-1.0, 1.0]:
			var g: Vector2 = p0 + u * along + r * (side * half_w)
			var ll: Vector2 = Geo.lv95_to_wgs84(g.x, g.y)
			batch.add_vertex(inv * W3DCoords.geodetic_to_ecef(ll.x, ll.y, z), Vector2(side * half_w, along), c0, c1r)
	for k in range(rows.size() - 1):
		var a := first + k * 2
		batch.idx.append_array([a, a + 2, a + 1, a + 1, a + 2, a + 3])
	batch.runways.append({
		"name": "/".join(PackedStringArray(desigs.map(func(d): return str(d)))),
		"first": first, "count": rows.size() * 2, "h0": h.x, "h1": h.y, "length": length,
	})
	var va: Vector3 = batch.verts[first]
	var vb: Vector3 = batch.verts[first + 1]
	var vc: Vector3 = batch.verts[batch.verts.size() - 2]
	var vd: Vector3 = batch.verts[batch.verts.size() - 1]
	var p0l := Vector2((va.x + vb.x) * 0.5, (va.z + vb.z) * 0.5)
	var p1l := Vector2((vc.x + vd.x) * 0.5, (vc.z + vd.z) * 0.5)
	var ul: Vector2 = (p1l - p0l).normalized()
	batch.rwy_rects.append([p0l, ul, p0l.distance_to(p1l), half_w])
	return true


func _add_aprons(batch: _Batch, inv: Transform3D, aprons: Array) -> void:
	var c0: Array = [0.0, 0.0, 0.0, 0.0]
	var c1: Array = [0.0, 0.0, float(KIND_APRON * 64), 0.0]
	for apn in aprons:
		var poly: Array = apn.get("polygon", [])
		if poly.size() < 4 or (poly[0] as Array).size() < 3:
			continue
		var pts2 := PackedVector2Array()
		var base := batch.verts.size()
		var n := poly.size() - 1 if poly[0] == poly[poly.size() - 1] else poly.size()
		for i in range(n):
			var rec: Array = poly[i]
			var p: Vector3 = inv * W3DCoords.geodetic_to_ecef(float(rec[0]), float(rec[1]), float(rec[2]) + APRON_LIFT_M)
			batch.add_vertex(p, Vector2(p.x, p.z), c0, c1)
			pts2.append(Vector2(p.x, p.z))
		batch.apron_polys.append(pts2)
		var tri: PackedInt32Array = Geometry2D.triangulate_polygon(pts2)
		if tri.is_empty():
			for i in range(1, n - 1):
				tri.append_array([0, i, i + 1])
		for k in range(0, tri.size(), 3):
			var ia := base + tri[k]
			var ib := base + tri[k + 1]
			var ic := base + tri[k + 2]
			if ia >= batch.verts.size() or ib >= batch.verts.size() or ic >= batch.verts.size():
				continue
			var a: Vector3 = batch.verts[ia]
			var b: Vector3 = batch.verts[ib]
			var c: Vector3 = batch.verts[ic]
			if (b - a).cross(c - a).y >= 0.0:
				batch.idx.append_array([ia, ic, ib])
			else:
				batch.idx.append_array([ia, ib, ic])
		apron_count += 1


func _add_taxiways(batch: _Batch, inv: Transform3D, taxiways: Array) -> void:
	# Guidance lines only: the pavement comes from _add_taxiway_mesh (the
	# bake unions all ribbons of an airport, so no junction gaps or
	# overlapping ribbons exist). One 0.5 m strip per path, above every
	# pavement so it reads through crossings and across runways.
	var c0: Array = [0.0, 0.0, 0.0, 0.0]
	var c1l: Array = [0.0, 0.0, float(KIND_TAXI_LINE * 64), 0.0]
	for tw in taxiways:
		var path: Array = tw.get("path", [])
		if path.size() < 2 or (path[0] as Array).size() < 3:
			continue
		var pts: Array[Vector3] = []
		for rec in path:
			pts.append(inv * W3DCoords.geodetic_to_ecef(float(rec[0]), float(rec[1]), float(rec[2]) + LINE_LIFT_M))
		for i in range(pts.size() - 1):
			var d: Vector3 = pts[i + 1] - pts[i]
			var flat := Vector3(d.x, 0.0, d.z)
			var seg_len := flat.length()
			if seg_len < 0.05:
				continue
			var dir: Vector3 = flat / seg_len
			var right := Vector3(-dir.z, 0.0, dir.x) * LINE_HALF_W
			var a := batch.add_vertex(pts[i] - right, Vector2(-LINE_HALF_W, 0.0), c0, c1l)
			batch.add_vertex(pts[i] + right, Vector2(LINE_HALF_W, 0.0), c0, c1l)
			batch.add_vertex(pts[i + 1] - right, Vector2(-LINE_HALF_W, seg_len), c0, c1l)
			batch.add_vertex(pts[i + 1] + right, Vector2(LINE_HALF_W, seg_len), c0, c1l)
			batch.idx.append_array([a, a + 2, a + 1, a + 1, a + 2, a + 3])
			if i + 1 < pts.size() - 1:
				_add_disc(batch, pts[i + 1], LINE_HALF_W, 0.0, c0, c1l)
		taxiway_count += 1


## The airport's unioned taxiway pavement: pre-triangulated by the bake
## (vertices [lat, lon, elev], triangle index list).
func _add_taxiway_mesh(batch: _Batch, inv: Transform3D, mesh: Dictionary) -> void:
	var vs: Array = mesh.get("vertices", [])
	var tris: Array = mesh.get("triangles", [])
	if vs.is_empty() or tris.size() < 3:
		return
	var c0: Array = [0.0, 0.0, 0.0, 0.0]
	var c1: Array = [0.0, 0.0, float(KIND_TAXIWAY * 64), 0.0]
	var base := batch.verts.size()
	for rec in vs:
		var p: Vector3 = inv * W3DCoords.geodetic_to_ecef(float(rec[0]), float(rec[1]), float(rec[2]) + TAXI_LIFT_M)
		batch.add_vertex(p, Vector2(p.x, p.z), c0, c1)
	var n := vs.size()
	for k in range(0, tris.size() - 2, 3):
		var ia := base + int(tris[k])
		var ib := base + int(tris[k + 1])
		var ic := base + int(tris[k + 2])
		if int(tris[k]) >= n or int(tris[k + 1]) >= n or int(tris[k + 2]) >= n:
			continue
		var a: Vector3 = batch.verts[ia]
		var b: Vector3 = batch.verts[ib]
		var c: Vector3 = batch.verts[ic]
		batch.taxi_tri_first.append(batch.idx.size())
		if (b - a).cross(c - a).y >= 0.0:
			batch.idx.append_array([ia, ic, ib])
		else:
			batch.idx.append_array([ia, ib, ic])


## Lift of the drawn surface above the terrain at an ECEF position: runway
## 8 cm, taxiway 10 cm, apron 3 cm, 0 off any pavement. Fed into JSBSim's
## ground plane so the wheels rest on what is drawn.
func surface_lift_at(world_pos: Vector3) -> float:
	var best := 0.0
	for icao in _lookup.keys():
		if world_pos.distance_to(_airport_ecef[icao]) > LOOKUP_RANGE_M:
			continue
		var lk: Dictionary = _lookup[icao]
		var local: Vector3 = (lk["inv"] as Transform3D) * world_pos
		var p := Vector2(local.x, local.z)
		for rr in lk["runways"]:
			var d: Vector2 = p - (rr[0] as Vector2)
			var along: float = d.dot(rr[1])
			var lateral: float = absf(d.cross(rr[1]))
			if along >= 0.0 and along <= float(rr[2]) and lateral <= float(rr[3]):
				best = maxf(best, SURFACE_LIFT_M)
		var cell := Vector2i(int(floor(p.x / LOOKUP_CELL_M)), int(floor(p.y / LOOKUP_CELL_M)))
		var grid: Dictionary = lk["grid"]
		if grid.has(cell):
			var verts: PackedVector3Array = lk["verts"]
			var idx: PackedInt32Array = lk["idx"]
			for k in grid[cell]:
				var a: Vector3 = verts[idx[k]]
				var b: Vector3 = verts[idx[k + 1]]
				var c: Vector3 = verts[idx[k + 2]]
				if Geometry2D.point_is_inside_triangle(p, Vector2(a.x, a.z), Vector2(b.x, b.z), Vector2(c.x, c.z)):
					best = maxf(best, TAXI_LIFT_M)
					break
		for poly in lk["aprons"]:
			if Geometry2D.is_point_in_polygon(p, poly):
				best = maxf(best, APRON_LIFT_M)
				break
	return best


func _register_lookup(icao: String, batch: _Batch, frame: Transform3D) -> void:
	var grid: Dictionary = {}
	for k in batch.taxi_tri_first:
		var a: Vector3 = batch.verts[batch.idx[k]]
		var b: Vector3 = batch.verts[batch.idx[k + 1]]
		var c: Vector3 = batch.verts[batch.idx[k + 2]]
		var minx: float = minf(a.x, minf(b.x, c.x))
		var maxx: float = maxf(a.x, maxf(b.x, c.x))
		var minz: float = minf(a.z, minf(b.z, c.z))
		var maxz: float = maxf(a.z, maxf(b.z, c.z))
		for cx in range(int(floor(minx / LOOKUP_CELL_M)), int(floor(maxx / LOOKUP_CELL_M)) + 1):
			for cz in range(int(floor(minz / LOOKUP_CELL_M)), int(floor(maxz / LOOKUP_CELL_M)) + 1):
				var key := Vector2i(cx, cz)
				if not grid.has(key):
					grid[key] = []
				(grid[key] as Array).append(k)
	_lookup[icao] = {"inv": frame.affine_inverse(), "runways": batch.rwy_rects,
		"aprons": batch.apron_polys, "verts": batch.verts, "idx": batch.idx, "grid": grid}


func _add_disc(batch: _Batch, centre: Vector3, radius: float, along: float, c0: Array, c1: Array) -> void:
	# UV.x is kept off the ±0.15 m centreline band (the disc carries no
	# yellow line; the segment quads draw it through the node).
	var n := 12
	var c := batch.add_vertex(centre, Vector2(0.5 * radius, along), c0, c1)
	for k in range(n):
		var ang: float = TAU * float(k) / float(n)
		batch.add_vertex(centre + Vector3(cos(ang) * radius, 0.0, sin(ang) * radius), Vector2(radius, along + sin(ang) * radius), c0, c1)
	for k in range(n):
		var a := c + 1 + k
		var b := c + 1 + (k + 1) % n
		# Godot's front face is clockwise seen from +Y: (b - c) x (a - c) has
		# +Y for centre -> b -> a, so the front-facing order is c, a, b.
		batch.idx.append_array([c, a, b])


func _mesh_instance(batch: _Batch, frame: Transform3D) -> MeshInstance3D:
	if batch.idx.is_empty():
		return null
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = batch.verts
	arrays[Mesh.ARRAY_NORMAL] = batch.normals
	arrays[Mesh.ARRAY_TEX_UV] = batch.uvs
	arrays[Mesh.ARRAY_CUSTOM0] = batch.custom0
	arrays[Mesh.ARRAY_CUSTOM1] = batch.custom1
	arrays[Mesh.ARRAY_INDEX] = batch.idx
	var flags: int = (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT) \
		| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM1_SHIFT)
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, flags)
	var mi := MeshInstance3D.new()
	mi.name = "Surfaces"
	mi.mesh = mesh
	mi.material_override = _material
	mi.transform = frame
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.visibility_range_end = VISIBLE_RANGE_M
	mi.set_meta("runways", batch.runways)
	return mi


# ── Lights ────────────────────────────────────────────────────────────────

class _Lights:
	var verts := PackedVector3Array()
	var c0 := PackedFloat32Array()   # colour A rgb, size
	var c1 := PackedFloat32Array()   # colour B rgb, mode
	var c2 := PackedFloat32Array()   # axis x, axis z, papi angle, 0
	func add(p: Vector3, a: Color, b: Color, axis: Vector2, mode: int, size: float = 1.0, papi_deg: float = 0.0) -> void:
		verts.append(p)
		c0.append_array(PackedFloat32Array([a.r, a.g, a.b, size]))
		c1.append_array(PackedFloat32Array([b.r, b.g, b.b, float(mode)]))
		c2.append_array(PackedFloat32Array([axis.x, axis.y, papi_deg, float(mode)]))
	func size() -> int:
		return verts.size()


## Geometry of a runway in the airport frame for the fixture generators.
class _RwyGeo:
	var p0: Vector2      # LV95 pavement end 0
	var u: Vector2       # LV95 unit along end0 -> end1
	var r: Vector2       # LV95 unit right of u
	var length: float
	var half_w: float
	var h0: float
	var h1: float
	var disp0: float
	var disp1: float
	var lit: bool
	var axis_local: Vector2   # along direction in the airport frame (x, z)
	func z_at(t: float) -> float:
		return h0 + (h1 - h0) * t / length


func _rwy_geo(rw: Dictionary, inv: Transform3D) -> _RwyGeo:
	var ends: Array = rw.get("ends", [])
	if ends.size() != 2 or not (ends[0] as Dictionary).has("pavement") or not (ends[1] as Dictionary).has("pavement"):
		return null
	var g := _RwyGeo.new()
	var ll0: Array = ends[0]["pavement"]
	var ll1: Array = ends[1]["pavement"]
	g.p0 = Geo.wgs84_to_lv95(float(ll0[0]), float(ll0[1]))
	var p1: Vector2 = Geo.wgs84_to_lv95(float(ll1[0]), float(ll1[1]))
	g.length = g.p0.distance_to(p1)
	if g.length < 30.0:
		return null
	g.u = (p1 - g.p0) / g.length
	g.r = Vector2(g.u.y, -g.u.x)
	g.half_w = 0.5 * float(rw.get("width_m", 45.0))
	var h: Vector2 = _end_heights(rw, g.p0, p1, Callable())
	g.h0 = h.x
	g.h1 = h.y
	g.disp0 = float((ends[0] as Dictionary).get("displaced_m", 0.0))
	g.disp1 = float((ends[1] as Dictionary).get("displaced_m", 0.0))
	g.lit = bool(rw.get("lighted", false))
	var a: Vector3 = _lv95_local(inv, g.p0, g.h0)
	var b: Vector3 = _lv95_local(inv, p1, g.h1)
	g.axis_local = Vector2(b.x - a.x, b.z - a.z).normalized()
	return g


func _lv95_local(inv: Transform3D, p: Vector2, h: float) -> Vector3:
	var ll: Vector2 = Geo.lv95_to_wgs84(p.x, p.y)
	return inv * W3DCoords.geodetic_to_ecef(ll.x, ll.y, h + LIGHT_LIFT_M)


func _ll_local(inv: Transform3D, lat: float, lon: float, h: float) -> Vector3:
	return inv * W3DCoords.geodetic_to_ecef(lat, lon, h + LIGHT_LIFT_M)


## Fixtures of one lit runway: edges, thresholds/ends, centreline (long
## runways), PAPI, approach lights (long runways). Colour A is seen from
## the +axis side (looking back toward end0), B from the -axis side.
func _gen_runway_lights(L: _Lights, g: _RwyGeo, intl: bool, classes: Dictionary) -> void:
	var ax: Vector2 = g.axis_local
	# Edges: white, yellow in the last 600 m toward the end you are moving to.
	if not classes.has("rwe"):
		var t := 0.0
		while t <= g.length + 0.1:
			var a_col: Color = COL_YELLOW if t < EDGE_CAUTION_M else COL_WHITE          # seen from ahead (+axis side): moving toward end0
			var b_col: Color = COL_YELLOW if g.length - t < EDGE_CAUTION_M else COL_WHITE   # moving toward end1
			for side in [-1.0, 1.0]:
				L.add(_lv95_local(inv_cache, g.p0 + g.u * t + g.r * (float(side) * (g.half_w + 1.0)), g.z_at(t)), a_col, b_col, ax, 0, 1.0)
			t += EDGE_SPACING_M
	# Thresholds (green from the approach, red from the runway) and ends (red).
	if not classes.has("rwt"):
		for end_i in [0, 1]:
			var t_thr: float = g.disp0 if end_i == 0 else g.length - g.disp1
			var t_end: float = 0.0 if end_i == 0 else g.length
			var lat := -g.half_w + 3.0
			while lat <= g.half_w - 3.0 + 0.1:
				# end0: approach side is -axis (B) -> B green, A red
				var a_col: Color = COL_RED if end_i == 0 else COL_GREEN
				var b_col: Color = COL_GREEN if end_i == 0 else COL_RED
				L.add(_lv95_local(inv_cache, g.p0 + g.u * t_thr + g.r * lat, g.z_at(t_thr)), a_col, b_col, ax, 0, 1.2)
				if absf(t_thr - t_end) > 1.0:
					var ea: Color = COL_RED if end_i == 0 else COL_OFF
					var eb: Color = COL_OFF if end_i == 0 else COL_RED
					L.add(_lv95_local(inv_cache, g.p0 + g.u * t_end + g.r * lat, g.z_at(t_end)), ea, eb, ax, 0, 1.0)
				lat += THRESHOLD_SPACING_M
	# Centreline: white, red/white in the last 900 m, red in the last 300 m.
	if g.length >= CENTRE_MIN_LENGTH_M and not classes.has("rwc"):
		var t := 15.0
		var k := 0
		while t < g.length - 15.0:
			var rem0 := t                 # distance to end0 (relevant when moving toward end0 = A side)
			var rem1 := g.length - t
			var a_col := COL_WHITE
			var b_col := COL_WHITE
			if rem0 < 300.0: a_col = COL_RED
			elif rem0 < 900.0: a_col = COL_RED if k % 2 == 0 else COL_WHITE
			if rem1 < 300.0: b_col = COL_RED
			elif rem1 < 900.0: b_col = COL_RED if k % 2 == 0 else COL_WHITE
			L.add(_lv95_local(inv_cache, g.p0 + g.u * t + g.r * 0.6, g.z_at(t)), a_col, b_col, ax, 0, 0.8)
			t += CENTRE_SPACING_M
			k += 1
	# PAPI: four units on the pilot's left, 300 m past the threshold, angles
	# 2.5 / 2.83 / 3.17 / 3.5 deg from the outermost unit to the one nearest
	# the runway (on a 3 deg slope: two white outer, two red inner).
	if not classes.has("papi"):
		for end_i in [0, 1]:
			var t_thr: float = g.disp0 if end_i == 0 else g.length - g.disp1
			var t_p: float = t_thr + PAPI_DISTANCE_M if end_i == 0 else t_thr - PAPI_DISTANCE_M
			var left: Vector2 = -g.r if end_i == 0 else g.r
			var axis_i: Vector2 = ax if end_i == 0 else -ax
			for unit in range(4):
				var angle: float = 3.5 - unit * (1.0 / 3.0)
				var lat_off: float = g.half_w + PAPI_OFFSET_M + unit * PAPI_UNIT_SPACING_M
				L.add(_lv95_local(inv_cache, g.p0 + g.u * t_p + left * lat_off, g.z_at(t_p)), COL_WHITE, COL_WHITE, axis_i, 2, PAPI_SIZE, angle)
	# Approach lights on both ends of long lit runways.
	if g.length >= CENTRE_MIN_LENGTH_M and not classes.has("als"):
		var total: float = APPROACH_LONG_M if intl else APPROACH_SHORT_M
		for end_i in [0, 1]:
			var axis_i: Vector2 = ax if end_i == 0 else -ax   # directed toward -axis_i (the approach)
			var d := APPROACH_SPACING_M
			while d <= total + 0.1:
				var t: float = -d if end_i == 0 else g.length + d
				var z: float = g.z_at(0.0) if end_i == 0 else g.z_at(g.length)
				L.add(_lv95_local(inv_cache, g.p0 + g.u * t, z), COL_WHITE, COL_WHITE, axis_i, 1, 1.2)
				# crossbars (Calvert-style: wider farther out) at 300 m, and every 150 m on the long system
				var bar_half := 0.0
				if absf(d - 300.0) < 1.0:
					bar_half = 15.0
				elif intl and int(round(d)) % 150 == 0:
					bar_half = 10.0 + 5.0 * (d / 150.0 - 1.0)
				if bar_half > 0.0:
					var lat := -bar_half
					while lat <= bar_half + 0.1:
						if absf(lat) > 1.0:
							L.add(_lv95_local(inv_cache, g.p0 + g.u * t + g.r * lat, z), COL_WHITE, COL_WHITE, axis_i, 1, 1.0)
						lat += 3.0
				d += APPROACH_SPACING_M


var inv_cache: Transform3D


## The runway whose centreline segment is closest to an LV95 point.
func _nearest_geo(q: Vector2, geos: Array) -> _RwyGeo:
	var best: _RwyGeo = null
	var best_d := INF
	for g in geos:
		var rg := g as _RwyGeo
		var t: float = clampf((q - rg.p0).dot(rg.u), 0.0, rg.length)
		var d: float = (q - (rg.p0 + rg.u * t)).length()
		if d < best_d:
			best_d = d
			best = rg
	return best


## Taxiway centreline (green) and edge (blue) lights from the paths; nodes
## flagged on a runway are skipped.
func _gen_taxiway_lights(L: _Lights, taxiways: Array, inv: Transform3D, classes: Dictionary) -> void:
	var want_centre := not classes.has("txc")
	var want_edge := not classes.has("txe")
	if not want_centre and not want_edge:
		return
	for tw in taxiways:
		var path: Array = tw.get("path", [])
		if path.size() < 2 or (path[0] as Array).size() < 3:
			continue
		var half_w: float = 0.5 * float(tw.get("width_m", 23.0))
		var pts: Array[Vector3] = []
		var flags: Array[float] = []
		for rec in path:
			pts.append(_ll_local(inv, float(rec[0]), float(rec[1]), float(rec[2])))
			flags.append(float(rec[3]) if (rec as Array).size() > 3 else 0.0)
		var carry_c := 0.0
		var carry_e := 0.0
		for i in range(pts.size() - 1):
			if flags[i] > 0.5 and flags[i + 1] > 0.5:
				continue
			var d: Vector3 = pts[i + 1] - pts[i]
			var seg := Vector2(d.x, d.z).length()
			if seg < 0.1:
				continue
			var dir := Vector2(d.x, d.z) / seg
			var right := Vector2(-dir.y, dir.x)
			var s := carry_c
			while want_centre and s < seg:
				var f := s / seg
				L.add(pts[i] + d * f, COL_GREEN, COL_GREEN, dir, 0, 0.7)
				s += TAXI_CENTRE_SPACING_M
			carry_c = s - seg
			s = carry_e
			while want_edge and s < seg:
				var f := s / seg
				var base: Vector3 = pts[i] + d * f
				for side in [-1.0, 1.0]:
					var off: Vector2 = right * (float(side) * (half_w + 1.5))
					L.add(base + Vector3(off.x, 0.0, off.y), COL_BLUE, COL_BLUE, dir, 0, 0.7)
				s += TAXI_EDGE_SPACING_M
			carry_e = s - seg


## Individually mapped OSM fixtures (bake: airport "lights"), replacing the
## generated class when at least OSM_CLASS_MIN of that class exist.
func _add_osm_lights(L: _Lights, ap: Dictionary, inv: Transform3D, classes: Dictionary, geos: Array) -> void:
	for lt in ap.get("lights", []):
		var kind := str(lt.get("kind", ""))
		if not classes.has(kind):
			continue
		var lat := float(lt.get("lat", 0.0))
		var lon := float(lt.get("lon", 0.0))
		var h: float = float(lt.get("elev_m", ap.get("elevation_m", 0.0)))
		var p: Vector3 = _ll_local(inv, lat, lon, h)
		# axis: mapped direction (degrees true, toward the beam) else the
		# nearest runway's axis; an approach light without a direction beams
		# away from the runway end it belongs to.
		var axis := Vector2(0.0, 1.0)
		if lt.has("dir"):
			var dd := deg_to_rad(float(lt["dir"]))
			axis = Vector2(-sin(dd), cos(dd))   # frame x = west: east = -x
		else:
			var q: Vector2 = Geo.wgs84_to_lv95(lat, lon)
			var best: _RwyGeo = _nearest_geo(q, geos)
			if best != null:
				axis = best.axis_local
				if kind == "als":
					var t: float = (q - best.p0).dot(best.u)
					axis = -best.axis_local if t < 0.5 * best.length else best.axis_local
		var colour_tag := str(lt.get("colour", ""))
		match kind:
			"rwe":
				L.add(p, COL_WHITE, COL_WHITE, axis, 0, 1.0)
			"rwc":
				L.add(p, COL_WHITE, COL_WHITE, axis, 0, 0.8)
			"rwt":
				var c: Color = COL_GREEN if colour_tag == "green" else (COL_RED if colour_tag == "red" else COL_GREEN)
				L.add(p, c, c, axis, 0, 1.2)
			"tdz":
				L.add(p, COL_WHITE, COL_WHITE, axis, 0, 0.8)
			"txc":
				L.add(p, COL_GREEN, COL_GREEN, axis, 0, 0.7)
			"txe":
				L.add(p, COL_BLUE, COL_BLUE, axis, 0, 0.7)
			"als":
				# beam toward `dir`: the shader's directed mode lights the -axis side
				L.add(p, COL_WHITE, COL_WHITE, -axis, 1, 1.2)
			"papi", "vasi":
				# OSM maps a PAPI as one node per system (rarely per unit) and
				# never the angles, so mapped PAPIs never replace the
				# generated four-unit ones (see _build_lights); unreachable.
				pass


func _build_lights(ap: Dictionary, frame: Transform3D) -> MeshInstance3D:
	var inv: Transform3D = frame.affine_inverse()
	inv_cache = inv
	var L := _Lights.new()
	# classes with enough mapped fixtures replace the generated ones
	var counts: Dictionary = {}
	for lt in ap.get("lights", []):
		var k := str(lt.get("kind", ""))
		counts[k] = int(counts.get(k, 0)) + 1
	var classes: Dictionary = {}
	for k in counts.keys():
		if int(counts[k]) >= OSM_CLASS_MIN and k != "papi" and k != "vasi":
			classes[k] = true
	var intl: bool = str(ap.get("type", "")) == "international_airport"
	var geos: Array = []
	for rw in ap.get("runways", []):
		if not (rw is Dictionary):
			continue
		var g := _rwy_geo(rw, inv)
		if g == null:
			continue
		geos.append(g)
		if g.lit:
			_gen_runway_lights(L, g, intl, classes)
	_gen_taxiway_lights(L, ap.get("taxiways", []), inv, classes)
	_add_osm_lights(L, ap, inv, classes, geos)
	if L.size() == 0:
		return null
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = L.verts
	arrays[Mesh.ARRAY_CUSTOM0] = L.c0
	arrays[Mesh.ARRAY_CUSTOM1] = L.c1
	arrays[Mesh.ARRAY_CUSTOM2] = L.c2
	var flags: int = (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT) \
		| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM1_SHIFT) \
		| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM2_SHIFT)
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_POINTS, arrays, [], {}, flags)
	var mi := MeshInstance3D.new()
	mi.name = "Lights"
	mi.mesh = mesh
	mi.material_override = _light_material
	mi.transform = frame
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.visibility_range_end = VISIBLE_RANGE_M
	light_count += L.size()
	return mi


func _load_model(path: String) -> Node3D:
	if not FileAccess.file_exists(path):
		push_warning("Runways: model not found: %s" % path)
		return null
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(path, state) != OK:
		push_warning("Runways: failed to parse model %s" % path)
		return null
	var scene := doc.generate_scene(state)
	return scene as Node3D
