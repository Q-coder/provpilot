extends Node
## Smoke: with live_weather on, a flight start triggers a METAR fetch and the
## report reaches JSBSim + the Settings screen (needs network; skips without
## PROVPILOT_NET=1). Run: godot --headless --path provpilot res://tests/smoke_live_weather_scene.tscn
var _root: Node = null
var _fails: Array[String] = []
func _fail(m: String) -> void:
	_fails.append(m); printerr("FAIL: " + m)
func _ready() -> void:
	if OS.get_environment("PROVPILOT_NET") != "1":
		print("smoke_live_weather_scene: SKIPPED (set PROVPILOT_NET=1)"); get_tree().quit(0); return
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	var store = _root._settings
	store.set_setting("live_weather", true)
	store.set_setting("weather_layers", [{"base_m": 0, "top_m": 3000, "wind_dir_deg": 90, "wind_speed_kts": 33, "turbulence": 0, "cloud_type": 0, "coverage_oktas": 0}])
	_root._push_weather_settings()   # what Settings close does
	var jsb = _root.jsb_node
	if absf(jsb.get_weather_wind_ned_kts(500.0).y + 33.0) > 0.01:
		_fail("static layers should apply until the report arrives")
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while _root._live_weather.last_weather.is_empty() and waited < 40.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	var wx: Dictionary = _root._live_weather.last_weather
	if wx.is_empty():
		_fail("no live report within 40 s")
	else:
		print("[live] %s" % wx["status"])
		var l0: Dictionary = wx["layers"][0]
		var w: Vector3 = jsb.get_weather_wind_ned_kts(100.0)
		var spd := sqrt(w.x * w.x + w.y * w.y)
		if absf(spd - float(l0["wind_speed_kts"])) > 0.01:
			_fail("JSBSim wind (%.1f kt) does not match the report's surface wind (%d kt)" % [spd, int(l0["wind_speed_kts"])])
		if absf(jsb.get_weather_qnh_hpa() - float(wx["qnh_hpa"])) > 0.01:
			_fail("QNH not applied")
	# Static layers in the store must be untouched.
	var stored: Array = store.get_setting("weather_layers")
	if stored.size() != 1 or int(stored[0]["wind_speed_kts"]) != 33:
		_fail("live weather must not overwrite the static layers in the store")
	# Turning live off restores the static layers immediately.
	store.set_setting("live_weather", false)
	_root._push_weather_settings()
	if absf(jsb.get_weather_wind_ned_kts(500.0).y + 33.0) > 0.01:
		_fail("disabling live weather should restore the static layers")
	if _fails.is_empty():
		print("smoke_live_weather_scene: OK"); get_tree().quit(0)
	else:
		printerr("smoke_live_weather_scene: %d failure(s)" % _fails.size()); get_tree().quit(1)
