extends Node
## A free look at the scenery: the camera at a place and height, one
## screenshot per bearing given (true, degrees), looking down by a pitch.
## Run: godot --path provpilot res://tests/probe_view.tscn -- <out_dir> <lat> <lon> <alt_m> <pitch_down_deg> <bearing ...>
var _cam: Camera3D = null
var _xf := Transform3D.IDENTITY

func _process(_d: float) -> void:
	if _cam != null:
		_cam.global_transform = _xf
		_cam.make_current()

func _ready() -> void:
	process_priority = 1000
	var a := OS.get_cmdline_user_args()
	var out_dir: String = a[0]
	var lat := float(a[1])
	var lon := float(a[2])
	var alt := float(a[3])
	var pitch := float(a[4])
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_c172p_start_agl_ft"] = alt / 0.3048
	root._settings.get_settings()["aircraft_c172p_start_speed_kt"] = 100.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	var lv: Vector2 = Geo.wgs84_to_lv95(lat, lon)
	root._on_welcome_flight_requested("c172p", lv.x, lv.y, 0.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(5.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	root._freeze_flight(true)
	root._sky.set_time(false, 12 * 60)
	var p: Vector3 = W3DCoords.geodetic_to_ecef(lat, lon, alt)
	var up: Vector3 = W3DCoords.geodetic_normal(lat, lon)
	var north: Vector3 = (W3DCoords.geodetic_to_ecef(lat + 0.01, lon, alt) - p)
	north = (north - up * north.dot(up)).normalized()
	var east: Vector3 = north.cross(up)
	root.get_node("AC").visible = false
	_cam = Camera3D.new()
	_cam.far = 12000000.0
	_cam.near = 1.0
	_cam.fov = 60.0
	add_child(_cam)
	for i in range(5, a.size()):
		var b := deg_to_rad(float(a[i]))
		var fwd: Vector3 = (north * cos(b) + east * sin(b)) * cos(deg_to_rad(pitch)) - up * sin(deg_to_rad(pitch))
		var right: Vector3 = fwd.cross(up).normalized()
		var cu: Vector3 = right.cross(fwd).normalized()
		_xf = Transform3D(Basis(right, cu, -fwd), p)
		await get_tree().create_timer(6.0 if i == 5 else 2.5).timeout
		var img: Image = get_viewport().get_texture().get_image()
		img.resize(img.get_width() / 2, img.get_height() / 2)
		img.save_png(out_dir.path_join("view_%03d.png" % int(float(a[i]))))
		print("probe_view: bearing %s saved" % a[i])
	get_tree().quit(0)
