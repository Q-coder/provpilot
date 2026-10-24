extends Node
## Prints, for LV95 points in PROBE_POINTS ("e,n;e,n;..."), the drawn terrain
## height, the ground query and the baked road surface (S3DRoadManager) after
## a flight start at the LSZH spawn: which surface the car / JSBSim get there.
## PROBE_ROADS_DIR limits the road manager to one roads/ directory.
const E := 2682767.0
const N := 1258797.0

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	if OS.get_environment("PROBE_ROADS_DIR") != "":
		for rm in get_tree().root.find_children("*", "S3DRoadManager", true, false):
			rm.set_road_paths(PackedStringArray([OS.get_environment("PROBE_ROADS_DIR")]))
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(10.0).timeout
	var rms: Array = get_tree().root.find_children("*", "S3DRoadManager", true, false)
	var rm = rms[0] if rms.size() > 0 else null
	var world = root._world
	for item in OS.get_environment("PROBE_POINTS").split(";", false):
		var p: PackedStringArray = item.split(",")
		var e := float(p[0])
		var n := float(p[1])
		var ll: Vector2 = world.latlon_of_lv95(e, n)
		var t: float = world.terrain_elevation_geodetic(ll.x, ll.y)
		var patch: float = world.scenery_elevation_geodetic(ll.x, ll.y)
		var globe: float = world.globe_elevation_geodetic(ll.x, ll.y)
		var g: float = world.ground_height_geodetic(ll.x, ll.y)
		var r: float = rm.surface_height_at(e, n, t, 2.0) if rm else NAN
		var r_any: float = rm.surface_height_at(e, n, t, 50.0) if rm else NAN
		print("POINT E %.1f N %.1f: drawn terrain %.2f (patch %s, globe %.2f) ground %.2f road(2 m) %s road(50 m) %s" % [e, n, t, str(patch), globe, g, str(r), str(r_any)])
	get_tree().quit(0)
