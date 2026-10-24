extends Node
## The drawn road surface in the ground model (docs/VEHICLES.md): once the
## base-tier road tiles have streamed in around LSZH, a point on a baked
## two-way road reports a road height (S3DRoadManager.surface_height_at)
## close to the terrain, the ground queries of WorldScenery return the higher
## of terrain and road there, a hint far above finds nothing, and the runway
## (no road) reports none.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_road_surface.tscn
const E := 2682767.0
const N := 1258797.0
const TILE_M := 1024
var _fails: Array[String] = []


func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)


## A two-way road at grade from the sidecar of the tile under (e, n): its
## middle station, inside that tile (a way may run on into a neighbour).
func _road_point(dir: String, e: float, n: float) -> Dictionary:
	var te := int(floor(e / TILE_M)) * TILE_M
	var tn := int(floor(n / TILE_M)) * TILE_M
	var path := "%s/roads_%d_%d.ways.json" % [dir, te, tn]
	if not FileAccess.file_exists(path):
		printerr("  no sidecar ", path)
		return {}
	var recs = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not (recs is Array):
		return {}
	for r in recs:
		if r.get("kind") != "road" or r.get("oneway") or r.get("bridge") or r.get("tunnel"):
			continue
		var pts: Array = r.get("pts", [])
		if pts.size() < 5:
			continue
		var m: Array = pts[pts.size() / 2]
		if m[0] < te or m[0] >= te + TILE_M or m[1] < tn or m[1] >= tn + TILE_M:
			continue
		return {"e": float(m[0]), "n": float(m[1]), "id": int(r.get("id")), "cls": str(r.get("cls"))}
	return {}


func _finish() -> void:
	if _fails.is_empty():
		print("smoke_road_surface: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_road_surface: %d failure(s)" % _fails.size())
		get_tree().quit(1)


func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(10.0).timeout
	var rms: Array = get_tree().root.find_children("*", "S3DRoadManager", true, false)
	_check(rms.size() >= 1, "road manager present (%d)" % rms.size())
	if rms.is_empty():
		_finish()
		return
	var rm = rms[0]
	var base_path := ""
	for p in rm.get_road_paths():
		if "switzerland-base" in str(p):
			base_path = str(p)
	_check(base_path != "", "Swiss base region roads/ dir among the road paths (%s)" % base_path)
	var active: int = rm.get_active_tile_count()
	_check(active >= 20, "road tiles streamed around LSZH (%d)" % active)
	var world = root._world
	var p := _road_point(base_path, E, N)
	_check(not p.is_empty(), "a two-way road at grade in the sidecar of the LSZH tile (%s)" % str(p))
	if p.is_empty():
		_finish()
		return
	var ll: Vector2 = world.latlon_of_lv95(p["e"], p["n"])
	var terrain: float = world.terrain_elevation_geodetic(ll.x, ll.y)
	_check(not is_nan(terrain), "terrain under the road point (%.2f m)" % terrain)
	var road: float = rm.surface_height_at(p["e"], p["n"], terrain, 2.0)
	print("  [diag] way %d (%s) at E %.1f N %.1f: terrain %.2f, road surface %.2f" % [p["id"], p["cls"], p["e"], p["n"], terrain, road])
	_check(not is_nan(road), "the road manager reports a surface under the way")
	_check(not is_nan(road) and absf(road - terrain) < 2.0, "... within 2 m of the terrain (%.2f vs %.2f)" % [road, terrain])
	var ground: float = world.ground_height_geodetic(ll.x, ll.y)
	_check(not is_nan(ground) and ground >= terrain - 1e-6 and ground >= road - 1e-6, "ground_height_geodetic is the higher of terrain and road (%.2f)" % ground)
	var below: float = world.ground_height_below(world.ecef_of_lv95(p["e"], p["n"], terrain))
	_check(absf(below - ground) < 0.01, "ground_height_below agrees (%.2f vs %.2f)" % [below, ground])
	var via_world: float = world.road_surface_at(ll.x, ll.y, terrain)
	_check(not is_nan(via_world) and absf(via_world - road) < 0.01, "WorldScenery.road_surface_at reaches the road manager (%.2f; the lat/lon round trip moves the point by millimetres)" % via_world)
	_check(is_nan(rm.surface_height_at(p["e"], p["n"], terrain + 50.0, 2.0)), "a hint 50 m above finds no surface within 2 m")
	var ll0: Vector2 = world.latlon_of_lv95(E, N)
	var t0: float = world.terrain_elevation_geodetic(ll0.x, ll0.y)
	var on_runway: float = rm.surface_height_at(E, N, t0, 2.0)
	_check(is_nan(on_runway), "no road surface on the runway at the LSZH spawn (%s)" % str(on_runway))
	_finish()
