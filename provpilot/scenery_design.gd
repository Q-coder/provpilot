class_name SceneryDesign
extends RefCounted
## Editable scenery design document (docs/SCENERY_EDITOR.md): a graph of
## nodes (LV95 metres) joined by edges that are road or railway segments.
## Everything the scenery editor changes lives here; the mesher draws it
## live and the bake (scripts/build_world_roads.py --design) folds it into
## the base tier. Plain JSON on disk so packs can ship and diff it.

const SCHEMA := 1
const KIND_ROAD := "road"
const KIND_RAIL := "rail"
const ROAD_CLASSES := ["motorway", "trunk", "primary", "secondary", "tertiary",
	"motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link"]
const RAIL_CLASSES := ["rail", "narrow_gauge", "light_rail", "tram"]
const EDGE_ATTRS := ["kind", "cls", "lanes", "oneway", "gauge", "tracks", "width"]   # width m: 0 = from the lanes

const ARC_STEP_M := 10.0
const ARC_MIN_SEGMENTS := 4

var replace: Array = []        # polygons [[Vector2, ...], ...]: OSM ways inside are cut by the bake
var replace_ids: Array = []    # OSM way ids the design replaces outright (picked into the design)
var flatten: Array = []        # [{"poly": [Vector2, ...], "h": metres ASL}]: plateaus the bake writes into the terrain
var areas: Array = []          # [{"poly": [Vector2, ...], "type": AREA_TYPES, "h": building height m}]
var patch_cache: Dictionary = {}   # terrain patches computed by the editor (needs terrain): used at flight start
const AREA_TYPES := ["grass", "forest", "field", "water", "building"]
var nodes: Dictionary = {}     # id -> {"e": float, "n": float, "h": height offset m (optional)}
var edges: Dictionary = {}     # id -> {"a": int, "b": int, kind, cls, lanes, oneway, gauge, tracks, "via": [e, n] for an arc}
var next_id: int = 1
var path: String = ""
var dirty: bool = false
var _undo: Array = []


static func road_attrs(cls: String, lanes: int, oneway: bool, width: float = 0.0) -> Dictionary:
	return {"kind": KIND_ROAD, "cls": cls, "lanes": lanes, "oneway": oneway, "gauge": 0, "tracks": 0, "width": width}


static func rail_attrs(cls: String, gauge_mm: int, tracks: int = 1) -> Dictionary:
	return {"kind": KIND_RAIL, "cls": cls, "lanes": 0, "oneway": false, "gauge": gauge_mm, "tracks": tracks, "width": 0.0}


func node_count() -> int:
	return nodes.size()


func edge_count() -> int:
	return edges.size()


func node_pos(id: int) -> Vector2:
	var nd: Dictionary = nodes.get(id, {})
	return Vector2(float(nd.get("e", 0.0)), float(nd.get("n", 0.0)))


func add_node(e: float, n: float) -> int:
	var id := next_id
	next_id += 1
	nodes[id] = {"e": e, "n": n}
	dirty = true
	return id


func move_node(id: int, e: float, n: float) -> void:
	if nodes.has(id):
		nodes[id]["e"] = e
		nodes[id]["n"] = n
		dirty = true


## Height offset of a node above (+) or below (-) the ground: a deck, an
## embankment or a cut. Zero (the default) follows the terrain.
func node_offset(id: int) -> float:
	return float(nodes.get(id, {}).get("h", 0.0))


func set_node_offset(id: int, h: float) -> void:
	if nodes.has(id):
		if absf(h) < 0.005:
			nodes[id].erase("h")
		else:
			nodes[id]["h"] = h
		dirty = true


## Douglas–Peucker simplification of a polyline (LV95), tolerance in metres.
static func simplify(pts: Array, tol: float) -> Array:
	if pts.size() <= 2:
		return pts.duplicate()
	var a: Vector2 = pts[0]
	var b: Vector2 = pts[-1]
	var best_d := -1.0
	var best_i := -1
	for i in range(1, pts.size() - 1):
		var d: float = (pts[i] as Vector2).distance_to(_nearest_on_segment(a, b, pts[i]))
		if d > best_d:
			best_d = d
			best_i = i
	if best_d <= tol:
		return [a, b]
	var left := simplify(pts.slice(0, best_i + 1), tol)
	var right := simplify(pts.slice(best_i), tol)
	left.pop_back()
	return left + right


## Imports a baked OSM way (a record of a roads_E_N.ways.json sidecar):
## its polyline becomes a chain of nodes and edges with the way's
## attributes (ends snap to existing nodes within 1 m), and its id goes to
## replace_ids so the bake drops the OSM original. Returns the node ids.
func import_way(rec: Dictionary) -> Array:
	var pts: Array = []
	for p in rec.get("pts", []):
		pts.append(Vector2(float(p[0]), float(p[1])))
	pts = simplify(pts, 0.4)
	if pts.size() < 2:
		return []
	var attrs := {}
	if str(rec.get("kind", "road")) == KIND_RAIL:
		attrs = rail_attrs(str(rec.get("cls", "rail")), int(rec.get("gauge", 1435)), maxi(1, int(rec.get("tracks", 1))))
	else:
		attrs = road_attrs(str(rec.get("cls", "secondary")), int(rec.get("lanes", 0)), bool(rec.get("oneway", false)), float(rec.get("width", 0.0)))
	var ids: Array = []
	for i in range(pts.size()):
		var p: Vector2 = pts[i]
		var nid := node_near(p.x, p.y, 1.0) if (i == 0 or i == pts.size() - 1) else -1
		if nid < 0:
			nid = add_node(p.x, p.y)
		ids.append(nid)
	for i in range(ids.size() - 1):
		add_edge(ids[i], ids[i + 1], attrs)
	var wid := int(rec.get("id", 0))
	if wid > 0 and not (wid in replace_ids):
		replace_ids.append(wid)
	dirty = true
	return ids


## Index of the area polygon with a corner within `radius` m of (e, n), or -1.
func area_near(e: float, n: float, radius: float) -> int:
	var q := Vector2(e, n)
	for i in range(areas.size()):
		for p in areas[i]["poly"]:
			if (p as Vector2).distance_to(q) <= radius:
				return i
	return -1


## Index of the flatten polygon with a corner within `radius` m of (e, n), or -1.
func flatten_near(e: float, n: float, radius: float) -> int:
	var q := Vector2(e, n)
	for i in range(flatten.size()):
		for p in flatten[i]["poly"]:
			if (p as Vector2).distance_to(q) <= radius:
				return i
	return -1


## Adds a segment between two existing nodes; a duplicate of an existing
## a-b edge is refused (returns -1), as is a degenerate a == b edge.
func add_edge(a: int, b: int, attrs: Dictionary) -> int:
	if a == b or not nodes.has(a) or not nodes.has(b):
		return -1
	for eid in edges:
		var ed: Dictionary = edges[eid]
		if (ed["a"] == a and ed["b"] == b) or (ed["a"] == b and ed["b"] == a):
			return -1
	var id := next_id
	next_id += 1
	var ed := {"a": a, "b": b}
	for k in EDGE_ATTRS:
		ed[k] = attrs.get(k, 0.0 if k == "width" else (0 if k in ["lanes", "gauge", "tracks"] else ("" if k in ["kind", "cls"] else false)))
	edges[id] = ed
	dirty = true
	return id


## Makes an edge a circular arc through `via` (LV95); a collinear via
## makes it straight again.
func set_edge_via(id: int, via: Vector2) -> void:
	if not edges.has(id):
		return
	edges[id]["via"] = [via.x, via.y]
	dirty = true


func clear_edge_via(id: int) -> void:
	if edges.has(id) and edges[id].has("via"):
		edges[id].erase("via")
		dirty = true


func is_arc(id: int) -> bool:
	return edges.has(id) and edges[id].has("via")


## Polyline of an edge from a to b (LV95): the two ends of a straight, or
## an arc through its via sampled every ARC_STEP_M.
func edge_points(id: int) -> Array:
	var ed: Dictionary = edges[id]
	var a := node_pos(ed["a"])
	var b := node_pos(ed["b"])
	if not ed.has("via"):
		return [a, b]
	return arc_points(a, Vector2(float(ed["via"][0]), float(ed["via"][1])), b)


## Circle through three points: {"c": centre, "r": radius} or empty when collinear.
static func circle_through(a: Vector2, v: Vector2, b: Vector2) -> Dictionary:
	var d: float = 2.0 * (a.x * (v.y - b.y) + v.x * (b.y - a.y) + b.x * (a.y - v.y))
	if absf(d) < 1e-6:
		return {}
	var a2 := a.length_squared()
	var v2 := v.length_squared()
	var b2 := b.length_squared()
	var ux: float = (a2 * (v.y - b.y) + v2 * (b.y - a.y) + b2 * (a.y - v.y)) / d
	var uy: float = (a2 * (b.x - v.x) + v2 * (a.x - b.x) + b2 * (v.x - a.x)) / d
	var c := Vector2(ux, uy)
	return {"c": c, "r": c.distance_to(a)}


## The arc a -> v -> b as {"c", "r", "ang_a", "sweep"} (sweep signed, the
## direction that passes v); empty when the points are collinear.
static func arc_sweep(a: Vector2, v: Vector2, b: Vector2) -> Dictionary:
	var circ := circle_through(a, v, b)
	if circ.is_empty() or float(circ["r"]) > 50000.0:
		return {}
	var c: Vector2 = circ["c"]
	var ang_a: float = (a - c).angle()
	var ang_v: float = (v - c).angle()
	var ang_b: float = (b - c).angle()
	var sweep_ccw: float = fposmod(ang_b - ang_a, TAU)
	var via_ccw: float = fposmod(ang_v - ang_a, TAU)
	var sweep: float = sweep_ccw if via_ccw <= sweep_ccw else sweep_ccw - TAU
	return {"c": c, "r": float(circ["r"]), "ang_a": ang_a, "sweep": sweep}


## Point of an arc at fraction t (0 = a, 1 = b).
static func arc_at(sw: Dictionary, t: float) -> Vector2:
	var ang: float = float(sw["ang_a"]) + float(sw["sweep"]) * t
	return (sw["c"] as Vector2) + Vector2(cos(ang), sin(ang)) * float(sw["r"])


## Arc from a through v to b, sampled every ARC_STEP_M (at least
## ARC_MIN_SEGMENTS), ends exact. Collinear points give the straight.
static func arc_points(a: Vector2, v: Vector2, b: Vector2) -> Array:
	var sw := arc_sweep(a, v, b)
	if sw.is_empty():
		return [a, b]
	var c: Vector2 = sw["c"]
	var r: float = sw["r"]
	var ang_a: float = sw["ang_a"]
	var sweep: float = sw["sweep"]
	var n: int = maxi(ARC_MIN_SEGMENTS, int(ceil(absf(sweep) * r / ARC_STEP_M)))
	var out: Array = []
	for i in range(n + 1):
		var t: float = float(i) / n
		if i == 0:
			out.append(a)
		elif i == n:
			out.append(b)
		else:
			var ang: float = ang_a + sweep * t
			out.append(c + Vector2(cos(ang), sin(ang)) * r)
	return out


## Via point of the arc that leaves p along the unit tangent t and ends at
## q (tangent-continuous curve); empty when q lies on the tangent line.
static func tangent_arc_via(p: Vector2, t: Vector2, q: Vector2) -> Dictionary:
	var c := q - p
	var nrm := Vector2(-t.y, t.x)
	var dn: float = c.dot(nrm)
	if absf(dn) < 0.01 or c.length() < 0.5:
		return {}
	var r: float = c.length_squared() / (2.0 * dn)     # signed: + = centre to the left
	var centre: Vector2 = p + nrm * r
	var ang_p: float = (p - centre).angle()
	var ang_q: float = (q - centre).angle()
	var sweep: float = ang_q - ang_p
	# the arc leaves p along t: for a left-hand centre it turns counter-clockwise
	if r > 0.0:
		sweep = fposmod(sweep, TAU)
	else:
		sweep = fposmod(sweep, TAU) - TAU
	var mid: float = ang_p + sweep * 0.5
	return {"via": centre + Vector2(cos(mid), sin(mid)) * absf(r), "radius": absf(r)}


## Unit direction of travel of an edge at one of its nodes, pointing away
## from the edge (the continuation a next segment should follow).
func edge_tangent_out(id: int, node_id: int) -> Vector2:
	var pts := edge_points(id)
	if edges[id]["b"] == node_id:
		return (pts[-1] - pts[-2]).normalized() if pts.size() >= 2 else Vector2.ZERO
	return (pts[0] - pts[1]).normalized() if pts.size() >= 2 else Vector2.ZERO


## Tangent to continue from a node: only when exactly one edge ends there.
func node_tangent_out(node_id: int) -> Vector2:
	var adj := edges_of(node_id)
	if adj.size() != 1:
		return Vector2.ZERO
	return edge_tangent_out(adj[0], node_id)


## A ring of `segments` arc edges around centre (counter-clockwise = the
## driving direction of a right-hand-traffic roundabout). Returns the node ids.
func add_circle(centre: Vector2, radius: float, attrs: Dictionary, segments: int = 8) -> Array:
	var ids: Array = []
	for k in range(segments):
		var ang: float = TAU * k / segments
		ids.append(add_node(centre.x + cos(ang) * radius, centre.y + sin(ang) * radius))
	for k in range(segments):
		var eid := add_edge(ids[k], ids[(k + 1) % segments], attrs)
		var mid: float = TAU * (k + 0.5) / segments
		if eid > 0:
			set_edge_via(eid, centre + Vector2(cos(mid), sin(mid)) * radius)
	return ids


const SWITCH_MIN_RADIUS_M := 150.0   # below this a turnout is refused (190 m is the tightest standard)

## Direction of travel of an edge at the point of it nearest to (e, n),
## pointing towards `towards`: the tangent a turnout leaves along.
func track_tangent_at(edge_id: int, e: float, n: float, towards: Vector2) -> Vector2:
	var near := _nearest_on_edge(edge_id, Vector2(e, n))
	var pts := edge_points(edge_id)
	var i: int = int(near["i"])
	var t: Vector2 = (pts[i + 1] - pts[i]).normalized()
	if (towards - (near["p"] as Vector2)).dot(t) < 0.0:
		t = -t
	return t


## A turnout: splits the track edge at the point nearest to (e, n) (the
## switch node) and adds the diverging track from there to q as the arc
## tangent to the track. Returns {"node", "edge", "radius", "end"} or
## {"error": ...} when q lies on the track line or the radius is too tight.
func add_switch(edge_id: int, e: float, n: float, q: Vector2) -> Dictionary:
	if not edges.has(edge_id) or edges[edge_id]["kind"] != KIND_RAIL:
		return {"error": "not a track"}
	var t := track_tangent_at(edge_id, e, n, q)
	var near := _nearest_on_edge(edge_id, Vector2(e, n))
	var toe: Vector2 = near["p"]
	var via := tangent_arc_via(toe, t, q)
	if via.is_empty():
		return {"error": "the end point lies on the track"}
	if float(via["radius"]) < SWITCH_MIN_RADIUS_M:
		return {"error": "radius %.0f m is below %.0f m" % [float(via["radius"]), SWITCH_MIN_RADIUS_M]}
	var attrs := {}
	for k in EDGE_ATTRS:
		attrs[k] = edges[edge_id][k]
	var snode := split_edge(edge_id, toe.x, toe.y)
	var end := add_node(q.x, q.y)
	var div := add_edge(snode, end, attrs)
	set_edge_via(div, via["via"])
	return {"node": snode, "edge": div, "radius": float(via["radius"]), "end": end}


## Reverses the direction of the chain an edge belongs to (every edge with
## the same attributes through degree-2 nodes): a one-way road then flows
## the other way. Returns the number of edges flipped.
func reverse_chain(edge_id: int) -> int:
	if not edges.has(edge_id):
		return 0
	for chain in chains():
		if edge_id in chain["edges"]:
			for eid in chain["edges"]:
				var ed: Dictionary = edges[eid]
				var a: int = ed["a"]
				ed["a"] = ed["b"]
				ed["b"] = a
			dirty = true
			return chain["edges"].size()
	return 0


## Removes an edge and any node it leaves without edges.
func remove_edge(id: int) -> void:
	if not edges.has(id):
		return
	var ed: Dictionary = edges[id]
	edges.erase(id)
	for nid in [ed["a"], ed["b"]]:
		if degree(nid) == 0:
			nodes.erase(nid)
	dirty = true


func set_edge_attrs(id: int, attrs: Dictionary) -> void:
	if not edges.has(id):
		return
	for k in EDGE_ATTRS:
		if attrs.has(k):
			edges[id][k] = attrs[k]
	dirty = true


func degree(node_id: int) -> int:
	var d := 0
	for eid in edges:
		var ed: Dictionary = edges[eid]
		if ed["a"] == node_id or ed["b"] == node_id:
			d += 1
	return d


func edges_of(node_id: int) -> Array:
	var out: Array = []
	for eid in edges:
		var ed: Dictionary = edges[eid]
		if ed["a"] == node_id or ed["b"] == node_id:
			out.append(eid)
	return out


## Splits an edge at the point of it nearest to (e, n); the new node
## joins both halves (a crossing or a junction). Returns the new node id.
func split_edge(id: int, e: float, n: float) -> int:
	if not edges.has(id):
		return -1
	var ed: Dictionary = edges[id].duplicate(true)
	var near := _nearest_on_edge(id, Vector2(e, n))
	var p: Vector2 = near["p"]
	var nid := add_node(p.x, p.y)
	edges.erase(id)
	var attrs := {}
	for k in EDGE_ATTRS:
		attrs[k] = ed[k]
	var e1 := add_edge(ed["a"], nid, attrs)
	var e2 := add_edge(nid, ed["b"], attrs)
	if ed.has("via"):
		# each half is an arc of the same circle through its own midpoint
		var a := node_pos(ed["a"])
		var b := node_pos(ed["b"])
		var v := Vector2(float(ed["via"][0]), float(ed["via"][1]))
		var sw := arc_sweep(a, v, b)
		if not sw.is_empty():
			var ang_p: float = (p - (sw["c"] as Vector2)).angle()
			var tp: float = fposmod(ang_p - float(sw["ang_a"]), TAU) / TAU
			if float(sw["sweep"]) < 0.0:
				tp = 1.0 - tp
			tp = tp * TAU / absf(float(sw["sweep"]))
			tp = clampf(tp, 0.02, 0.98)
			if e1 > 0:
				set_edge_via(e1, arc_at(sw, tp * 0.5))
			if e2 > 0:
				set_edge_via(e2, arc_at(sw, tp + (1.0 - tp) * 0.5))
	return nid


## Nearest point of an edge's polyline to q: {"p", "dist", "i" (segment index)}.
func _nearest_on_edge(id: int, q: Vector2) -> Dictionary:
	var pts := edge_points(id)
	var best := {"p": pts[0], "dist": pts[0].distance_to(q), "i": 0}
	for i in range(pts.size() - 1):
		var p := _nearest_on_segment(pts[i], pts[i + 1], q)
		var d := p.distance_to(q)
		if d < float(best["dist"]):
			best = {"p": p, "dist": d, "i": i}
	return best


## Nearest node within `radius` metres, or -1.
func node_near(e: float, n: float, radius: float) -> int:
	var best := -1
	var best_d := radius
	var q := Vector2(e, n)
	for nid in nodes:
		var d: float = node_pos(nid).distance_to(q)
		if d <= best_d:
			best_d = d
			best = nid
	return best


## Nearest edge within `radius` metres: {"id", "e", "n", "dist"}; empty when none.
func edge_near(e: float, n: float, radius: float) -> Dictionary:
	var best := {}
	var best_d := radius
	var q := Vector2(e, n)
	for eid in edges:
		var near := _nearest_on_edge(eid, q)
		var d: float = near["dist"]
		if d <= best_d:
			best_d = d
			var p: Vector2 = near["p"]
			best = {"id": eid, "e": p.x, "n": p.y, "dist": d}
	return best


static func _nearest_on_segment(a: Vector2, b: Vector2, q: Vector2) -> Vector2:
	var ab := b - a
	var l2 := ab.length_squared()
	if l2 < 1e-9:
		return a
	var t: float = clampf((q - a).dot(ab) / l2, 0.0, 1.0)
	return a + ab * t


static func _same_attrs(x: Dictionary, y: Dictionary) -> bool:
	for k in EDGE_ATTRS:
		if x.get(k) != y.get(k):
			return false
	return true


## Runs of edges with equal attributes joined through nodes of degree 2:
## [{"attrs": {...}, "nodes": [id, ...], "edges": [id, ...]}]. A chain is
## what the mesher extrudes in one piece (mitred corners, continuous UVs).
func chains() -> Array:
	var used := {}
	var out: Array = []
	var ids: Array = edges.keys()
	ids.sort()
	for eid in ids:
		if used.has(eid):
			continue
		used[eid] = true
		var ed: Dictionary = edges[eid]
		var node_list: Array = [ed["a"], ed["b"]]
		var edge_list: Array = [eid]
		# extend forward from b, then backward from a
		for dir in [1, -1]:
			while true:
				var end_node: int = node_list[-1] if dir == 1 else node_list[0]
				var nxt := -1
				var adj := edges_of(end_node)
				if adj.size() != 2:
					break
				for cand in adj:
					if not used.has(cand) and _same_attrs(edges[cand], ed):
						nxt = cand
				if nxt < 0:
					break
				used[nxt] = true
				var ned: Dictionary = edges[nxt]
				var other: int = ned["b"] if ned["a"] == end_node else ned["a"]
				var closes: bool = other in node_list   # a ring: close it and stop
				if dir == 1:
					node_list.append(other)
					edge_list.append(nxt)
				else:
					node_list.push_front(other)
					edge_list.push_front(nxt)
				if closes:
					break
		var attrs := {}
		for k in EDGE_ATTRS:
			attrs[k] = ed[k]
		out.append({"attrs": attrs, "nodes": node_list, "edges": edge_list})
	return out


# ── undo ────────────────────────────────────────────────────────────────

func snapshot() -> void:
	_undo.append(to_dict())
	if _undo.size() > 100:
		_undo.pop_front()


func can_undo() -> bool:
	return not _undo.is_empty()


func undo() -> bool:
	if _undo.is_empty():
		return false
	from_dict(_undo.pop_back())
	dirty = true
	return true


# ── persistence ─────────────────────────────────────────────────────────

func to_dict() -> Dictionary:
	var nl: Array = []
	var nids: Array = nodes.keys()
	nids.sort()
	for nid in nids:
		var rec: Array = [nid, nodes[nid]["e"], nodes[nid]["n"]]
		if nodes[nid].has("h"):
			rec.append(nodes[nid]["h"])
		nl.append(rec)
	var el: Array = []
	var eids: Array = edges.keys()
	eids.sort()
	for eid in eids:
		var d: Dictionary = edges[eid].duplicate(true)
		d["id"] = eid
		el.append(d)
	var rl: Array = []
	for poly in replace:
		var pl: Array = []
		for p in poly:
			pl.append([(p as Vector2).x, (p as Vector2).y])
		rl.append(pl)
	var fl: Array = []
	for f in flatten:
		var pl: Array = []
		for p in f["poly"]:
			pl.append([(p as Vector2).x, (p as Vector2).y])
		fl.append({"poly": pl, "h": float(f["h"])})
	var al: Array = []
	for a in areas:
		var pl: Array = []
		for p in a["poly"]:
			pl.append([(p as Vector2).x, (p as Vector2).y])
		al.append({"poly": pl, "type": str(a["type"]), "h": float(a.get("h", 0.0))})
	var out := {"schema": SCHEMA, "crs": "EPSG:2056", "next_id": next_id, "nodes": nl, "edges": el, "replace": rl, "replace_ids": replace_ids.duplicate(), "flatten": fl, "areas": al}
	if not patch_cache.is_empty():
		out["patches"] = _patches_to_json(patch_cache)
	return out


static func _patches_to_json(pc: Dictionary) -> Dictionary:
	var out := {"plateaus": [], "corridors": [], "protect_polys": [], "protect_corridors": []}
	for pl in pc.get("plateaus", []):
		var poly: Array = []
		for p in pl["poly"]:
			poly.append([(p as Vector2).x, (p as Vector2).y])
		out["plateaus"].append({"poly": poly, "h": float(pl["h"])})
	for c in pc.get("corridors", []):
		var st: Array = []
		for v in c["stations"]:
			st.append([(v as Vector3).x, (v as Vector3).y, (v as Vector3).z])
		out["corridors"].append({"stations": st, "reach": float(c["reach"])})
	for poly in pc.get("protect_polys", []):
		var pl: Array = []
		for p in poly:
			pl.append([(p as Vector2).x, (p as Vector2).y])
		out["protect_polys"].append(pl)
	for c in pc.get("protect_corridors", []):
		var st: Array = []
		for v in c["stations"]:
			st.append([(v as Vector2).x, (v as Vector2).y])
		out["protect_corridors"].append({"stations": st, "reach": float(c["reach"])})
	return out


static func _patches_from_json(d: Dictionary) -> Dictionary:
	var out := {"plateaus": [], "corridors": [], "protect_polys": [], "protect_corridors": []}
	for pl in d.get("plateaus", []):
		var poly := PackedVector2Array()
		for p in pl.get("poly", []):
			poly.append(Vector2(float(p[0]), float(p[1])))
		out["plateaus"].append({"poly": poly, "h": float(pl.get("h", 0.0))})
	for c in d.get("corridors", []):
		var st := PackedVector3Array()
		for v in c.get("stations", []):
			st.append(Vector3(float(v[0]), float(v[1]), float(v[2])))
		out["corridors"].append({"stations": st, "reach": float(c.get("reach", 20.0))})
	for poly in d.get("protect_polys", []):
		var pl := PackedVector2Array()
		for p in poly:
			pl.append(Vector2(float(p[0]), float(p[1])))
		out["protect_polys"].append(pl)
	for c in d.get("protect_corridors", []):
		var st := PackedVector2Array()
		for v in c.get("stations", []):
			st.append(Vector2(float(v[0]), float(v[1])))
		out["protect_corridors"].append({"stations": st, "reach": float(c.get("reach", 20.0))})
	return out


func from_dict(d: Dictionary) -> void:
	nodes.clear()
	edges.clear()
	for rec in d.get("nodes", []):
		nodes[int(rec[0])] = {"e": float(rec[1]), "n": float(rec[2])}
		if rec.size() > 3 and absf(float(rec[3])) >= 0.005:
			nodes[int(rec[0])]["h"] = float(rec[3])
	for rec in d.get("edges", []):
		var ed := {"a": int(rec["a"]), "b": int(rec["b"])}
		for k in EDGE_ATTRS:
			var v = rec.get(k, 0.0 if k == "width" else (0 if k in ["lanes", "gauge", "tracks"] else ("" if k in ["kind", "cls"] else false)))
			ed[k] = int(v) if k in ["lanes", "gauge", "tracks"] else (float(v) if k == "width" else v)
		if rec.has("via") and rec["via"] is Array and rec["via"].size() == 2:
			ed["via"] = [float(rec["via"][0]), float(rec["via"][1])]
		edges[int(rec["id"])] = ed
	replace.clear()
	for poly in d.get("replace", []):
		var pl: Array = []
		for p in poly:
			pl.append(Vector2(float(p[0]), float(p[1])))
		if pl.size() >= 3:
			replace.append(pl)
	flatten.clear()
	for f in d.get("flatten", []):
		var pl: Array = []
		for p in f.get("poly", []):
			pl.append(Vector2(float(p[0]), float(p[1])))
		if pl.size() >= 3:
			flatten.append({"poly": pl, "h": float(f.get("h", 0.0))})
	replace_ids.clear()
	for i in d.get("replace_ids", []):
		replace_ids.append(int(i))
	areas.clear()
	for a in d.get("areas", []):
		var pl: Array = []
		for p in a.get("poly", []):
			pl.append(Vector2(float(p[0]), float(p[1])))
		if pl.size() >= 3 and str(a.get("type", "")) in AREA_TYPES:
			areas.append({"poly": pl, "type": str(a["type"]), "h": float(a.get("h", 0.0))})
	patch_cache = _patches_from_json(d["patches"]) if d.get("patches") is Dictionary else {}
	next_id = int(d.get("next_id", 1))
	for nid in nodes:
		next_id = maxi(next_id, nid + 1)
	for eid in edges:
		next_id = maxi(next_id, eid + 1)


## Index of the replace polygon with a vertex within `radius` m of (e, n), or -1.
func replace_near(e: float, n: float, radius: float) -> int:
	var q := Vector2(e, n)
	for i in range(replace.size()):
		for p in replace[i]:
			if (p as Vector2).distance_to(q) <= radius:
				return i
	return -1


## Whole-file signature, as the bake records it in the roads manifest.
static func file_sha256(p: String) -> String:
	if not FileAccess.file_exists(p):
		return ""
	return FileAccess.get_sha256(p)


func save(p: String = "") -> bool:
	if p != "":
		path = p
	if path == "":
		return false
	var dir := path.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_warning("SceneryDesign: cannot write %s" % path)
		return false
	f.store_string(JSON.stringify(to_dict(), "  "))
	f.close()
	dirty = false
	return true


func load(p: String) -> bool:
	path = p
	if not FileAccess.file_exists(p):
		nodes.clear()
		edges.clear()
		next_id = 1
		dirty = false
		return false
	var txt := FileAccess.get_file_as_string(p)
	var parsed = JSON.parse_string(txt)
	if not (parsed is Dictionary):
		push_warning("SceneryDesign: %s is not a design file" % p)
		return false
	from_dict(parsed)
	dirty = false
	return true
