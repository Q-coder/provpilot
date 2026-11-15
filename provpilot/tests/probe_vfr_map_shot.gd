extends Node
## Probe (not a smoke test): fly the C172 at LSZH, cancel the intro, set the
## VFR map's zoom, optionally move / resize the frame, wait for the tiles and
## save the map frame's pixels (plus the whole viewport) as PNGs.
## Run NON-headless:
##   godot --path provpilot res://tests/probe_vfr_map_shot.tscn -- <zoom_index 0-6> [out_dir] [resize_w,resize_h | -] [lat lon]
## Output: <out_dir>/vfr_map_z<zoom>.png (frame crop) and vfr_map_z<zoom>_full.png.
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var zoom: int = int(args[0]) if args.size() > 0 else 2
	var out_dir: String = args[1] if args.size() > 1 else "/tmp"
	get_window().size = Vector2i(1600, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var at := Vector2(2682767.0, 1258797.0)
	if args.size() > 4:
		at = Geo.wgs84_to_lv95(float(args[3]), float(args[4]))
	root._on_welcome_flight_requested("c172p", at.x, at.y, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var map = root._vfr_map
	if not map.is_map_enabled():
		map.toggle_visible()
	while map.get_zoom_index() < zoom:
		map.zoom_out()
	while map.get_zoom_index() > zoom:
		map.zoom_in()
	if args.size() > 2 and args[2] != "-":
		var wh := args[2].split(",")
		var cur: Vector2 = map.get_frame_size()
		map.resize_frame_by(Vector2(float(wh[0]) - cur.x, float(wh[1]) - cur.y))
		map.move_frame_by(Vector2(-200.0, 120.0))
	waited = 0.0
	while root._fly_loading and waited < 40.0:   # the map is fed once the destination has settled
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(8.0).timeout
	var img: Image = get_viewport().get_texture().get_image()
	img.save_png(out_dir.path_join("vfr_map_z%d_full.png" % zoom))
	var r: Rect2 = map.get_global_rect()
	var crop := img.get_region(Rect2i(int(r.position.x), int(r.position.y), int(r.size.x), int(r.size.y)))
	crop.save_png(out_dir.path_join("vfr_map_z%d.png" % zoom))
	print("probe_vfr_map_shot: zoom %d, frame %s, %d cached tiles, saved to %s" % [zoom, str(r), map.get_cached_tile_count(), out_dir])
	get_tree().quit(0)
