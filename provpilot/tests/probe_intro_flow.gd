extends Node
## Probe: the real Fly! flow — intro zoom plays to the end, then a screenshot
## of the handoff view. Env: PROBE_WEATHER_JSON (applied before Fly!),
## PROBE_OUT (png path), PROBE_AIRCRAFT (default c172p).
var _root: Node = null
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	if OS.get_environment("PROBE_WEATHER_JSON") != "":
		var wx = JSON.parse_string(OS.get_environment("PROBE_WEATHER_JSON"))
		if wx is Dictionary:
			_root._settings.set_setting("live_weather", false)
			if OS.get_environment("PROBE_CLOUD_QUALITY") != "":
				_root._settings.set_setting("cloud_quality", int(OS.get_environment("PROBE_CLOUD_QUALITY")))
			if wx.has("layers"):
				_root._settings.set_setting("weather_layers", wx["layers"])
			_root._push_weather_settings()
	if OS.get_environment("PROBE_TIME_MIN") != "":
		_root._settings.set_setting("use_system_time", false)
		_root._settings.set_setting("time_of_day_min", int(OS.get_environment("PROBE_TIME_MIN")))
		_root._push_time_of_day_settings()
	if _root._welcome_screen:
		_root._welcome_screen.visible = false
	var ac := OS.get_environment("PROBE_AIRCRAFT")
	if ac == "":
		ac = "c172p"
	_root._on_welcome_flight_requested(ac, 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while not _root._intro_playing and waited < 90.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	print("[intro] zoom started after %.1f s" % waited)
	while _root._intro_playing and waited < 180.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	print("[intro] zoom finished after %.1f s; camera=%s" % [waited, str(get_viewport().get_camera_3d().name if get_viewport().get_camera_3d() else "none")])
	await get_tree().create_timer(3.0).timeout
	var cam := get_viewport().get_camera_3d()
	if cam:
		print("[intro] cam basis det=%.6f pos=%s" % [cam.global_transform.basis.determinant(), str(cam.global_position)])
	if _root._clouds:
		print("[intro] clouds det=%.6f meshes=%d" % [_root._clouds.global_transform.basis.determinant(), _root._clouds.get_meshes().size()])
	var out := OS.get_environment("PROBE_OUT")
	if out != "":
		get_viewport().get_texture().get_image().save_png(out)
	get_tree().quit(0)
