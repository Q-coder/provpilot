class_name SceneryDesignMesher
extends Node3D
## Draws a SceneryDesign live: one mesh per chain, extruded in a local ENU
## frame at the chain's first node and drawn with the base-tier roads
## shader (metres in the UVs, class colour + surface kind in the vertex
## colour), exactly like the baked tiles of scripts/build_world_roads.py so
## a designed road looks like an OSM one. Heights come from the host's
## terrain query (`elevation` Callable: LV95 e, n -> metres or NAN).

const DENSIFY_M := 10.0
const ROAD_LIFT_M := 0.20
const BALLAST_LIFT_M := 0.15
const BALLAST_WIDTH_M := 4.2
const TRACK_SPACING_M := 3.8
const RAIL_TOP_M := 0.172
const RAIL_HEAD_M := 0.072
const RAIL_FOOT_M := 0.15
const MITER_LIMIT := 2.5
const KIND_ONEWAY := 1
const KIND_TWOWAY_LINED := 2
const KIND_TWOWAY := 3
const KIND_LINK := 4
const KIND_DUAL := 5
const KIND_JUNCTION := 6
const KIND_CONCRETE := 7
const AREA_KIND := {"grass": 10, "forest": 11, "field": 12, "water": 13}
const KIND_ROOF := 14
const KIND_WALL := 15
const ELEVATED_M := 0.3           # a station this far above the ground gets a structure
const PIER_SPACING_M := 25.0
const PIER_W_M := 1.2
const PARAPET_H_M := 0.45
const PARAPET_W_M := 0.3
const CONCRETE := Color(0.62, 0.61, 0.58)
const KIND_BALLAST := 8
const TAPER_PER_LANE_M := 30.0   # a lane appears / disappears over this length
const JUNCTION_MIN_CUT_M := 1.0
const KIND_RAIL := 9
const LANE_M := {"motorway": 3.75, "motorway_link": 3.75, "trunk": 3.5, "trunk_link": 3.5,
	"primary": 3.25, "primary_link": 3.25, "secondary": 3.0, "secondary_link": 3.0,
	"tertiary": 2.9, "tertiary_link": 2.9}
const SHOULDER_M := 2.5
const VERGE_M := 0.5
const CLASS_COLOUR := {"motorway": Color(0.30, 0.30, 0.31), "motorway_link": Color(0.30, 0.30, 0.31),
	"trunk": Color(0.32, 0.32, 0.33), "trunk_link": Color(0.32, 0.32, 0.33),
	"primary": Color(0.34, 0.34, 0.35), "primary_link": Color(0.34, 0.34, 0.35),
	"secondary": Color(0.36, 0.36, 0.37), "secondary_link": Color(0.36, 0.36, 0.37),
	"tertiary": Color(0.38, 0.38, 0.39), "tertiary_link": Color(0.38, 0.38, 0.39)}
const CLASS_DROP_M := {"motorway": 0.0, "trunk": 0.01, "primary": 0.02, "secondary": 0.03, "tertiary": 0.04,
	"motorway_link": 0.05, "trunk_link": 0.06, "primary_link": 0.07, "secondary_link": 0.08, "tertiary_link": 0.09}
const BALLAST_COLOUR := Color(0.5, 0.48, 0.45)
const RAIL_COLOUR := Color(120.0 / 255.0, 118.0 / 255.0, 110.0 / 255.0)

var elevation: Callable = Callable()   # (e, n) -> float
var tint: Color = Color(1, 1, 1)        # editor: design roads stand out from the baked ones
var _material: ShaderMaterial = null
var chain_count: int = 0
var junction_count: int = 0
var area_count: int = 0
var pier_count: int = 0
var unknown_height_count: int = 0      # stations without terrain at the last rebuild
var _cuts: Dictionary = {}             # edge id -> {node id: metres the leg is cut back}


class _Buf:
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var colors := PackedColorArray()
	var uvs := PackedVector2Array()
	var uv2s := PackedVector2Array()
	var idx := PackedInt32Array()


## Road width (m), shader kind and lane count from the attributes, the same
## rules as road_geometry() in scripts/build_world_roads.py.
static func road_geometry(cls: String, lanes: int, oneway: bool, width_m: float = 0.0) -> Array:
	var lane_m: float = LANE_M.get(cls, 3.0)
	lanes = clampi(lanes if lanes > 0 else (1 if cls.ends_with("_link") else 2), 1, 6)
	var link := cls.ends_with("_link")
	var width: float
	var kind: int
	if cls in ["motorway", "trunk"] and oneway:
		width = lanes * lane_m + SHOULDER_M + 2 * VERGE_M
		kind = KIND_ONEWAY
	elif cls == "motorway":
		var per: int = maxi(2, lanes / 2)
		width = 2 * (per * lane_m + SHOULDER_M + VERGE_M) + 3.0
		kind = KIND_DUAL
		lanes = per
	elif link or (oneway and lanes == 1):
		width = lanes * lane_m + 2 * VERGE_M
		kind = KIND_LINK
	elif oneway:
		width = lanes * lane_m + 2 * VERGE_M
		kind = KIND_ONEWAY
	else:
		width = lanes * lane_m + 2 * VERGE_M
		kind = KIND_TWOWAY_LINED if cls in ["trunk", "primary"] else KIND_TWOWAY
	if width_m >= 3.0 and width_m <= 60.0:
		width = width_m   # an explicit width (OSM width tag) wins, as in the bake
		if width < 5.0 and kind != KIND_ONEWAY:
			kind = KIND_LINK   # a narrow road has no centre line
	return [width, kind, lanes]


func _ensure_material() -> void:
	if _material == null:
		_material = ShaderMaterial.new()
		_material.shader = load("res://addons/scenery3d/shaders/roads.gdshader")


func clear() -> void:
	for c in get_children():
		remove_child(c)   # out of the tree now, freed at the frame end
		c.queue_free()
	chain_count = 0


## Rebuild every chain of the design, and a pavement at every junction.
func rebuild(design: SceneryDesign) -> void:
	_ensure_material()
	clear()
	unknown_height_count = 0
	junction_count = 0
	area_count = 0
	pier_count = 0
	_cuts = {}
	for nid in design.nodes:
		var j := _junction(design, nid)
		if j.is_empty():
			continue
		for leg in j["legs"]:
			if not _cuts.has(leg["edge"]):
				_cuts[leg["edge"]] = {}
			_cuts[leg["edge"]][nid] = leg["cut"]
		var mi := _build_junction(design, nid, j)
		if mi:
			add_child(mi)
			junction_count += 1
	for chain in design.chains():
		var mi := _build_chain(design, chain)
		if mi:
			add_child(mi)
			chain_count += 1
	for i in range(design.areas.size()):
		var mi := _build_area(design.areas[i], i)
		if mi:
			add_child(mi)
			area_count += 1


## Width (m) of a road edge; 0 for rails.
static func edge_width(design: SceneryDesign, eid: int) -> float:
	var ed: Dictionary = design.edges[eid]
	if ed["kind"] != SceneryDesign.KIND_ROAD:
		return 0.0
	return float(road_geometry(str(ed["cls"]), int(ed["lanes"]), bool(ed["oneway"]), float(ed.get("width", 0.0)))[0])


## Pavement geometry of a node with three or more road edges: legs sorted
## by bearing with the corner where the edge lines of neighbouring legs
## meet, the polygon (LV95, counter-clockwise, fanned from the node) and
## how far each leg's ribbon is cut back. Empty when not a junction.
func _junction(design: SceneryDesign, nid: int) -> Dictionary:
	var legs: Array = []
	for eid in design.edges_of(nid):
		var ed: Dictionary = design.edges[eid]
		if ed["kind"] != SceneryDesign.KIND_ROAD:
			continue
		var pts: Array = design.edge_points(eid)
		if ed["b"] == nid:
			pts.reverse()
		var d: Vector2 = (pts[1] - pts[0]).normalized()
		legs.append({"edge": eid, "dir": d, "ang": d.angle(), "half": edge_width(design, eid) * 0.5,
			"cls": str(ed["cls"]), "cut": 0.0})
	if legs.size() < 3:
		return {}
	legs.sort_custom(func(x, y): return x["ang"] < y["ang"])
	var centre := design.node_pos(nid)
	var k := legs.size()
	# corner between leg i (its left edge line) and leg i+1 (its right edge line)
	var corners: Array = []
	for i in range(k):
		var a: Dictionary = legs[i]
		var b: Dictionary = legs[(i + 1) % k]
		var da: Vector2 = a["dir"]
		var db: Vector2 = b["dir"]
		var na := Vector2(-da.y, da.x)   # left of leg a (counter-clockwise side)
		var nb := Vector2(-db.y, db.x)
		var pa: Vector2 = centre + na * float(a["half"])        # a point on a's left edge line
		var pb: Vector2 = centre - nb * float(b["half"])        # a point on b's right edge line
		var corner: Vector2
		var den: float = da.cross(db)
		if absf(den) < 0.05:
			# nearly parallel legs (a fork): the corner sits between the edge lines
			corner = (pa + pb) * 0.5 + (da + db).normalized() * maxf(float(a["half"]), float(b["half"]))
		else:
			# intersection of pa + t*da and pb + u*db
			var t: float = (pb - pa).cross(db) / den
			corner = pa + da * t
			# a corner behind the node (very obtuse pair) is pulled forward
			if (corner - centre).dot(da) < 0.0 and (corner - centre).dot(db) < 0.0:
				corner = (pa + pb) * 0.5
		corners.append(corner)
		a["cut"] = maxf(float(a["cut"]), (corner - centre).dot(da))
		b["cut"] = maxf(float(b["cut"]), (corner - centre).dot(db))
	for leg in legs:
		leg["cut"] = maxf(float(leg["cut"]), JUNCTION_MIN_CUT_M)
	# polygon: for each leg its right cut point, left cut point, then the corner to the next leg
	var poly: Array = []
	for i in range(k):
		var leg: Dictionary = legs[i]
		var d: Vector2 = leg["dir"]
		var n := Vector2(-d.y, d.x)
		var base: Vector2 = centre + d * float(leg["cut"])
		poly.append(base - n * float(leg["half"]))
		poly.append(base + n * float(leg["half"]))
		poly.append(corners[i])
	return {"legs": legs, "poly": poly, "centre": centre}


## Fan mesh of the junction polygon, at the highest class lift of its legs.
func _build_junction(design: SceneryDesign, nid: int, j: Dictionary) -> MeshInstance3D:
	var centre: Vector2 = j["centre"]
	var poly: Array = j["poly"]
	var lift := 0.0
	var colour := Color(0.34, 0.34, 0.35)
	var best_drop := 1.0
	for leg in j["legs"]:
		var drop: float = float(CLASS_DROP_M.get(leg["cls"], 0.0))
		if drop < best_drop:
			best_drop = drop
			colour = CLASS_COLOUR.get(leg["cls"], colour)
	lift = ROAD_LIFT_M - best_drop + 0.005
	# one height for the whole patch: the highest ground under it
	var h := _ground(centre.x, centre.y)
	for p in poly:
		var hp := _ground((p as Vector2).x, (p as Vector2).y)
		if not is_nan(hp) and (is_nan(h) or hp > h):
			h = hp
	if is_nan(h):
		unknown_height_count += 1
		return null
	h += design.node_offset(nid)
	var ll0: Vector2 = Geo.lv95_to_wgs84(centre.x, centre.y)
	var frame: Transform3D = W3DCoords.enu_to_ecef_transform(ll0.x, ll0.y, h + lift, true)
	var inv := frame.affine_inverse()
	var buf := _Buf.new()
	var col := Color(colour.r, colour.g, colour.b, float(KIND_JUNCTION) / 255.0)
	var local_of := func(p: Vector2) -> Vector3:
		var ll: Vector2 = Geo.lv95_to_wgs84(p.x, p.y)
		return inv * W3DCoords.geodetic_to_ecef(ll.x, ll.y, h + lift)
	buf.verts.append(local_of.call(centre))
	buf.normals.append(Vector3.UP)
	buf.colors.append(col)
	buf.uvs.append(Vector2.ZERO)
	buf.uv2s.append(Vector2.ZERO)
	for p in poly:
		buf.verts.append(local_of.call(p))
		buf.normals.append(Vector3.UP)
		buf.colors.append(col)
		buf.uvs.append((p as Vector2) - centre)
		buf.uv2s.append(Vector2.ZERO)
	var n := poly.size()
	for i in range(n):
		buf.idx.append_array(PackedInt32Array([0, 1 + i, 1 + (i + 1) % n]))
	var mi := _mesh_instance(buf, frame)
	mi.name = "Junction_%d" % nid
	mi.set_meta("node", nid)
	return mi


func _mesh_instance(buf: _Buf, frame: Transform3D) -> MeshInstance3D:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = buf.verts
	arrays[Mesh.ARRAY_NORMAL] = buf.normals
	arrays[Mesh.ARRAY_COLOR] = buf.colors
	arrays[Mesh.ARRAY_TEX_UV] = buf.uvs
	arrays[Mesh.ARRAY_TEX_UV2] = buf.uv2s
	arrays[Mesh.ARRAY_INDEX] = buf.idx
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = _material
	mi.transform = frame
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return mi


## Cut `metres` off the start of a polyline (LV95), keeping the direction.
static func _trim_start(pts: Array, metres: float) -> Array:
	var left := metres
	var out: Array = pts.duplicate()
	while out.size() >= 2 and left > 0.0:
		var seg: float = (out[0] as Vector2).distance_to(out[1])
		if seg <= left:
			left -= seg
			out.remove_at(0)
		else:
			out[0] = (out[0] as Vector2).lerp(out[1], left / seg)
			left = 0.0
	return out


## Half width of the road on the far side of a chain end, when the end
## node joins exactly one other road edge of a different width (a lane
## change); -1 otherwise.
static func _neighbour_half(design: SceneryDesign, chain: Dictionary, at_start: bool) -> float:
	var node_id: int = chain["nodes"][0] if at_start else chain["nodes"][-1]
	var own_edge: int = chain["edges"][0] if at_start else chain["edges"][-1]
	var adj := design.edges_of(node_id)
	if adj.size() != 2:
		return -1.0
	var other: int = adj[0] if adj[1] == own_edge else adj[1]
	if design.edges[other]["kind"] != SceneryDesign.KIND_ROAD or design.edges[own_edge]["kind"] != SceneryDesign.KIND_ROAD:
		return -1.0
	var w_other := edge_width(design, other)
	var w_own := edge_width(design, own_edge)
	if absf(w_other - w_own) < 0.05:
		return -1.0
	return w_other * 0.5


## The chain's polyline in LV95: every edge's points (arcs sampled) in
## chain order, each edge oriented from the previous node to the next.
static func chain_polyline(design: SceneryDesign, chain: Dictionary) -> Array:
	return chain_polyline_offsets(design, chain)["pts"]


## Polyline plus the height offset at every point: the node offsets,
## interpolated along each edge by arc length.
static func chain_polyline_offsets(design: SceneryDesign, chain: Dictionary) -> Dictionary:
	var out: Array = []
	var offs: Array = []
	var node_list: Array = chain["nodes"]
	var edge_list: Array = chain["edges"]
	for i in range(edge_list.size()):
		var eid: int = edge_list[i]
		var pts: Array = design.edge_points(eid)
		if design.edges[eid]["a"] != node_list[i]:
			pts.reverse()
		var h0 := design.node_offset(node_list[i])
		var h1 := design.node_offset(node_list[i + 1])
		var total := 0.0
		for k in range(1, pts.size()):
			total += (pts[k] as Vector2).distance_to(pts[k - 1])
		var run := 0.0
		for k in range(pts.size()):
			if k > 0:
				run += (pts[k] as Vector2).distance_to(pts[k - 1])
			if i > 0 and k == 0:
				continue
			out.append(pts[k])
			offs.append(lerpf(h0, h1, run / total if total > 0.0 else 0.0))
	return {"pts": out, "offs": offs}


static func _densify(pts: Array) -> Array:
	return _densify_with(pts, [])["pts"]


## Densify to DENSIFY_M stations, interpolating a parallel value array.
static func _densify_with(pts: Array, vals: Array) -> Dictionary:
	var out: Array = []
	var ov: Array = []
	var has_vals: bool = vals.size() == pts.size()
	for i in range(pts.size() - 1):
		var a: Vector2 = pts[i]
		var b: Vector2 = pts[i + 1]
		var n: int = maxi(1, int(ceil(a.distance_to(b) / DENSIFY_M)))
		for k in range(n):
			var t := float(k) / n
			out.append(a.lerp(b, t))
			if has_vals:
				ov.append(lerpf(float(vals[i]), float(vals[i + 1]), t))
	out.append(pts[-1])
	if has_vals:
		ov.append(float(vals[-1]))
	return {"pts": out, "vals": ov}


## Cut `metres` off the start of a parallel value array the way _trim_start cuts the polyline.
static func _trim_vals(pts: Array, vals: Array, metres: float) -> Array:
	var left := metres
	var out: Array = vals.duplicate()
	var p: Array = pts.duplicate()
	while p.size() >= 2 and left > 0.0:
		var seg: float = (p[0] as Vector2).distance_to(p[1])
		if seg <= left:
			left -= seg
			p.remove_at(0)
			out.remove_at(0)
		else:
			out[0] = lerpf(float(out[0]), float(out[1]), left / seg)
			left = 0.0
	return out


func _ground(e: float, n: float) -> float:
	if not elevation.is_valid():
		return NAN
	return float(elevation.call(e, n))


func _build_chain(design: SceneryDesign, chain: Dictionary) -> MeshInstance3D:
	var attrs: Dictionary = chain["attrs"]
	var po := chain_polyline_offsets(design, chain)
	var pts: Array = po["pts"]
	var offs: Array = po["offs"]
	if pts.size() < 2:
		return null
	# legs of a junction start at the pavement edge
	var first_edge: int = chain["edges"][0]
	var last_edge: int = chain["edges"][-1]
	var cut0: float = float(_cuts.get(first_edge, {}).get(chain["nodes"][0], 0.0))
	var cut1: float = float(_cuts.get(last_edge, {}).get(chain["nodes"][-1], 0.0))
	if cut0 > 0.0:
		offs = _trim_vals(pts, offs, cut0)
		pts = _trim_start(pts, cut0)
	if cut1 > 0.0:
		pts.reverse()
		offs.reverse()
		offs = _trim_vals(pts, offs, cut1)
		pts = _trim_start(pts, cut1)
		pts.reverse()
		offs.reverse()
	if pts.size() < 2 or (pts[0] as Vector2).distance_to(pts[-1]) < 0.5:
		return null
	var dens := _densify_with(pts, offs)
	pts = dens["pts"]
	offs = dens["vals"]
	var is_road: bool = attrs["kind"] == SceneryDesign.KIND_ROAD
	var width: float
	var kind: int
	var count: float
	var gauge_m := 1.435
	if is_road:
		var g := road_geometry(str(attrs["cls"]), int(attrs["lanes"]), bool(attrs["oneway"]), float(attrs.get("width", 0.0)))
		width = g[0]
		kind = g[1]
		count = float(g[2])
	else:
		var tracks: int = clampi(int(attrs.get("tracks", 1)), 1, 4)
		width = BALLAST_WIDTH_M + TRACK_SPACING_M * (tracks - 1)
		kind = KIND_BALLAST
		count = float(tracks)
		gauge_m = (float(attrs["gauge"]) if int(attrs["gauge"]) > 0 else 1435.0) / 1000.0
	# half width per station: constant, except a lane change at either
	# end, where the wider road tapers to the narrower over 30 m per lane
	var half := width * 0.5
	var halves: Array = []
	var along: Array = [0.0]
	for i in range(1, pts.size()):
		along.append(float(along[i - 1]) + (pts[i] as Vector2).distance_to(pts[i - 1]))
	var total: float = along[-1]
	var nb0 := _neighbour_half(design, chain, true) if is_road else -1.0
	var nb1 := _neighbour_half(design, chain, false) if is_road else -1.0
	for i in range(pts.size()):
		var hw := half
		if nb0 >= 0.0 and nb0 < half:
			var L: float = TAPER_PER_LANE_M * maxf(1.0, (half - nb0) / 1.6)
			hw = minf(hw, lerpf(nb0, half, clampf(float(along[i]) / L, 0.0, 1.0)))
		if nb1 >= 0.0 and nb1 < half:
			var L1: float = TAPER_PER_LANE_M * maxf(1.0, (half - nb1) / 1.6)
			hw = minf(hw, lerpf(nb1, half, clampf((total - float(along[i])) / L1, 0.0, 1.0)))
		halves.append(hw)
	# ground: highest of centre and both edges at every station, no smoothing
	var heights: Array = []
	var last_h := NAN
	for i in range(pts.size()):
		var p: Vector2 = pts[i]
		var d: Vector2 = (pts[mini(i + 1, pts.size() - 1)] - pts[maxi(i - 1, 0)]).normalized()
		var nrm := Vector2(-d.y, d.x)
		var h := NAN
		for f in [-1.0, 0.0, 1.0]:
			var q: Vector2 = p + nrm * half * f
			var hh := _ground(q.x, q.y)
			if not is_nan(hh) and (is_nan(h) or hh > h):
				h = hh
		if is_nan(h):
			unknown_height_count += 1
			h = last_h
		heights.append(h)
		last_h = h
	# fill leading NaNs from the first known height
	var first_known := NAN
	for h in heights:
		if not is_nan(h):
			first_known = h
			break
	if is_nan(first_known):
		return null   # no terrain under this chain yet
	for i in range(heights.size()):
		if is_nan(heights[i]):
			heights[i] = first_known
		heights[i] = float(heights[i]) + float(offs[i])   # node height offsets (decks, cuts)
	# +3 cm over the baked lift: a picked road draws over its OSM original until the re-bake
	var lift: float = (ROAD_LIFT_M + 0.03 - float(CLASS_DROP_M.get(attrs["cls"], 0.0))) if is_road else BALLAST_LIFT_M + 0.03
	if not is_road:
		# the diverging track of a switch shares the bed with the through
		# track for a few metres: a few millimetres per chain avoid z-fighting
		lift += 0.004 * float(int(chain["edges"][0]) % 4)
	# local ENU frame at the first station
	var ll0: Vector2 = Geo.lv95_to_wgs84(pts[0].x, pts[0].y)
	var frame: Transform3D = W3DCoords.enu_to_ecef_transform(ll0.x, ll0.y, float(heights[0]) + lift, true)
	var inv := frame.affine_inverse()
	var local: Array = []
	for i in range(pts.size()):
		var ll: Vector2 = Geo.lv95_to_wgs84(pts[i].x, pts[i].y)
		local.append(inv * W3DCoords.geodetic_to_ecef(ll.x, ll.y, float(heights[i]) + lift))
	var buf := _Buf.new()
	var colour: Color = CLASS_COLOUR.get(attrs["cls"], Color(0.35, 0.35, 0.36)) if is_road else BALLAST_COLOUR
	colour = Color(colour.r * tint.r, colour.g * tint.g, colour.b * tint.b)
	_extrude(buf, local, [[-half, 0.0], [half, 0.0]], colour, kind, 0.0, half, count, halves)
	_structures(buf, local, offs, halves if halves.size() == local.size() else [], half)
	if not is_road:
		var tracks := int(count)
		var section := [[-RAIL_FOOT_M / 2, 0.0], [-RAIL_HEAD_M / 2, RAIL_TOP_M], [RAIL_HEAD_M / 2, RAIL_TOP_M], [RAIL_FOOT_M / 2, 0.0]]
		for tr in range(tracks):
			var centre: float = (tr - (tracks - 1) / 2.0) * TRACK_SPACING_M
			for side in [-1.0, 1.0]:
				_extrude(buf, local, section, RAIL_COLOUR, KIND_RAIL, centre + side * gauge_m / 2.0, RAIL_FOOT_M / 2, 1.0)
	var mi := _mesh_instance(buf, frame)
	mi.name = "Chain_%d" % int(chain["edges"][0])
	mi.set_meta("edges", chain["edges"])
	return mi


## Bridge structure under elevated stations: parapets along both edges
## of every elevated run and a pier every PIER_SPACING_M from the deck's
## underside down to the ground (the station's offset). Concrete kind.
func _structures(buf: _Buf, local: Array, offs: Array, halves: Array, half: float) -> void:
	var n := local.size()
	if n < 2 or offs.size() != n:
		return
	var run_start := -1
	var since_pier := PIER_SPACING_M
	for i in range(n + 1):
		var elevated: bool = i < n and float(offs[i]) > ELEVATED_M
		if elevated and run_start < 0:
			run_start = i
			since_pier = PIER_SPACING_M
		if (not elevated or i == n) and run_start >= 0:
			var seg: Array = local.slice(run_start, i)
			var hw_seg: Array = halves.slice(run_start, i) if halves.size() == n else []
			if seg.size() >= 2:
				for side in [-1.0, 1.0]:
					var u0: float = side * (half - PARAPET_W_M * 0.5)
					_extrude(buf, seg, [[-PARAPET_W_M * 0.5, 0.0], [-PARAPET_W_M * 0.5, PARAPET_H_M], [PARAPET_W_M * 0.5, PARAPET_H_M], [PARAPET_W_M * 0.5, 0.0]], CONCRETE, KIND_CONCRETE, u0, 0.0, 0.0)
			run_start = -1
		if elevated:
			if i > 0:
				since_pier += (local[i] as Vector3).distance_to(local[i - 1])
			if since_pier >= PIER_SPACING_M:
				since_pier = 0.0
				_pier(buf, local, i, float(offs[i]), (float(halves[i]) if halves.size() == n else half))
				pier_count += 1


## A box pier under station i: from the deck underside to the ground.
func _pier(buf: _Buf, local: Array, i: int, height: float, hw: float) -> void:
	var p: Vector3 = local[i]
	var q: Vector3 = local[mini(i + 1, local.size() - 1)] if i < local.size() - 1 else local[i - 1]
	var d := Vector2(q.x - p.x, q.z - p.z).normalized()
	var nrm := Vector2(-d.y, d.x)
	var w: float = maxf(hw * 2.0 - 2.0, PIER_W_M)   # across the deck, 1 m in from each edge
	var t: float = PIER_W_M * 0.5
	var top: float = -ROAD_LIFT_M
	var bottom: float = -height - ROAD_LIFT_M - 0.5
	var base := buf.verts.size()
	var corners: Array = []
	for sx in [-1.0, 1.0]:
		for sz in [-1.0, 1.0]:
			corners.append(Vector3(p.x + nrm.x * w * 0.5 * sx + d.x * t * sz, 0.0, p.z + nrm.y * w * 0.5 * sx + d.y * t * sz))
	var col := Color(CONCRETE.r, CONCRETE.g, CONCRETE.b, float(KIND_CONCRETE) / 255.0)
	# four walls, each a quad with metres UVs (u across, v = height)
	var order := [0, 1, 3, 2]
	for k in range(4):
		var a: Vector3 = corners[order[k]]
		var b: Vector3 = corners[order[(k + 1) % 4]]
		var nx := Vector3(b.z - a.z, 0.0, -(b.x - a.x)).normalized()
		for v in [[a, bottom], [b, bottom], [b, top], [a, top]]:
			buf.verts.append(Vector3((v[0] as Vector3).x, p.y + float(v[1]), (v[0] as Vector3).z))
			buf.normals.append(nx)
			buf.colors.append(col)
			buf.uvs.append(Vector2(0.0 if v[0] == a else a.distance_to(b), float(v[1]) - bottom))
			buf.uv2s.append(Vector2.ZERO)
		var s0 := base + k * 4
		buf.idx.append_array(PackedInt32Array([s0, s0 + 1, s0 + 2, s0, s0 + 2, s0 + 3]))


## An area: a flat patch (grass, forest, field, water) on the highest
## ground under it, or a building (roof at ground + h, walls down to
## the ground). Concave outlines are triangulated.
func _build_area(area: Dictionary, index: int) -> MeshInstance3D:
	var poly: Array = area["poly"]
	if poly.size() < 3:
		return null
	var typ := str(area["type"])
	var centre := Vector2.ZERO
	for p in poly:
		centre += p as Vector2
	centre /= poly.size()
	var h := _ground(centre.x, centre.y)
	var lowest := h
	for p in poly:
		var hp := _ground((p as Vector2).x, (p as Vector2).y)
		if is_nan(hp):
			continue
		if is_nan(h) or hp > h:
			h = hp
		if is_nan(lowest) or hp < lowest:
			lowest = hp
	if is_nan(h):
		unknown_height_count += 1
		return null
	var building := typ == "building"
	var lift: float = 0.06 if not building else 0.0
	if typ == "water":
		h = lowest   # water fills the low ground
		lift = 0.02
	var ll0: Vector2 = Geo.lv95_to_wgs84(centre.x, centre.y)
	var frame: Transform3D = W3DCoords.enu_to_ecef_transform(ll0.x, ll0.y, h + lift, true)
	var inv := frame.affine_inverse()
	var local2: PackedVector2Array = PackedVector2Array()
	var local3: Array = []
	for p in poly:
		var ll: Vector2 = Geo.lv95_to_wgs84((p as Vector2).x, (p as Vector2).y)
		var v: Vector3 = inv * W3DCoords.geodetic_to_ecef(ll.x, ll.y, h + lift)
		local3.append(v)
		local2.append(Vector2(v.x, v.z))
	var tris: PackedInt32Array = Geometry2D.triangulate_polygon(local2)
	if tris.is_empty():
		return null
	var buf := _Buf.new()
	var top_h: float = float(area.get("h", 0.0)) if building else 0.0
	var kind: int = KIND_ROOF if building else int(AREA_KIND.get(typ, 10))
	var col := Color(0.5, 0.5, 0.5, float(kind) / 255.0)
	for v in local3:
		buf.verts.append(Vector3((v as Vector3).x, top_h, (v as Vector3).z))
		buf.normals.append(Vector3.UP)
		buf.colors.append(col)
		buf.uvs.append(Vector2((v as Vector3).x, (v as Vector3).z))
		buf.uv2s.append(Vector2.ZERO)
	buf.idx.append_array(tris)
	if building:
		var wcol := Color(0.5, 0.5, 0.5, float(KIND_WALL) / 255.0)
		var bottom: float = lowest - h - 0.5
		var n := local3.size()
		for i in range(n):
			var a: Vector3 = local3[i]
			var b: Vector3 = local3[(i + 1) % n]
			var nx := Vector3(b.z - a.z, 0.0, -(b.x - a.x)).normalized()
			var s0 := buf.verts.size()
			for v in [[a, bottom], [b, bottom], [b, top_h], [a, top_h]]:
				buf.verts.append(Vector3((v[0] as Vector3).x, float(v[1]), (v[0] as Vector3).z))
				buf.normals.append(nx)
				buf.colors.append(wcol)
				buf.uvs.append(Vector2(0.0 if v[0] == a else a.distance_to(b), float(v[1]) - bottom))
				buf.uv2s.append(Vector2.ZERO)
			buf.idx.append_array(PackedInt32Array([s0, s0 + 1, s0 + 2, s0, s0 + 2, s0 + 3]))
	var mi := _mesh_instance(buf, frame)
	mi.name = "Area_%d" % index
	mi.set_meta("area", index)
	return mi


## Per-station across unit vectors (x/z of the local frame, left of travel)
## scaled by the mitre at corners.
static func _frames(local: Array) -> Array:
	var n := local.size()
	var out: Array = []
	for i in range(n):
		var p0: Vector3 = local[maxi(i - 1, 0)]
		var p1: Vector3 = local[i]
		var p2: Vector3 = local[mini(i + 1, n - 1)]
		var t0 := Vector2(p1.x - p0.x, p1.z - p0.z).normalized()
		var t1 := Vector2(p2.x - p1.x, p2.z - p1.z).normalized()
		if i == 0:
			t0 = t1
		if i == n - 1:
			t1 = t0
		var n0 := Vector2(-t0.y, t0.x)
		var n1 := Vector2(-t1.y, t1.x)
		var den: float = 1.0 + t0.dot(t1)
		var m: Vector2
		if den < 1e-4:
			m = n0
		else:
			m = (n0 + n1) / den
			if m.length() > MITER_LIMIT:
				m = m.normalized() * MITER_LIMIT
		out.append(m)
	return out


## Extrude a cross-section [[u, h], ...] (left to right, metres) along the
## local stations; u0 shifts it sideways (rails), UV = (u, along), UV2 =
## (half width, count), colour alpha = kind / 255.
static func _extrude(buf: _Buf, local: Array, section: Array, colour: Color, kind: int, u0: float, half_w: float, count: float, halves: Array = []) -> void:
	var n := local.size()
	var m := section.size()
	if n < 2:
		return
	var scaled: bool = halves.size() == n and half_w > 0.0
	var base := buf.verts.size()
	var frames := _frames(local)
	var col := Color(colour.r, colour.g, colour.b, float(kind) / 255.0)
	var sn: Array = []
	for j in range(m):
		var j0: int = maxi(0, j - 1)
		var j1: int = mini(m - 1, j + 1)
		var du: float = section[j1][0] - section[j0][0]
		var dh: float = section[j1][1] - section[j0][1]
		var l: float = maxf(sqrt(du * du + dh * dh), 1e-6)
		sn.append(Vector2(-dh / l, du / l))   # (across, up)
	var s := 0.0
	for i in range(n):
		if i > 0:
			s += (local[i] as Vector3).distance_to(local[i - 1])
		var f: Vector2 = frames[i]
		var p: Vector3 = local[i]
		var hw: float = float(halves[i]) if scaled else half_w
		var k: float = hw / half_w if scaled else 1.0
		for j in range(m):
			var u: float = float(section[j][0]) * k
			var h: float = section[j][1]
			var off: float = -(u0 + u)   # + u = right of travel = -normal
			buf.verts.append(Vector3(p.x + f.x * off, p.y + h, p.z + f.y * off))
			var nu: float = sn[j].x
			var nh: float = sn[j].y
			buf.normals.append(Vector3(-f.x * nu, nh, -f.y * nu).normalized())
			buf.colors.append(col)
			buf.uvs.append(Vector2(u, s))
			buf.uv2s.append(Vector2(hw, count))
	for i in range(n - 1):
		var a := base + i * m
		for j in range(m - 1):
			buf.idx.append_array(PackedInt32Array([a + j, a + m + j, a + j + 1, a + j + 1, a + m + j, a + m + j + 1]))
