extends Node
## Smoke: static weather layers reach JSBSim. Part 1 checks the layer sampler
## (wind vector, gap blending, turbulence pick) on a bare JSBGodot node; part 2
## flies briefly and reads the FDM's wind / turbulence / QNH properties back.
## Run: godot --headless --path provpilot res://tests/smoke_weather.tscn
const CANNED := '[{"icaoId":"LSZH","obsTime":1789748400,"wdir":290,"wspd":8,"wgst":null,"visib":"6+","altim":1018,"clouds":[{"cover":"FEW","base":7400}],"elev":424,"lat":47.48,"lon":8.536,"rawOb":"METAR LSZH 181620Z 29008KT 9999 FEW074 20/08 Q1018 NOSIG","name":"Zürich Intl Arpt"}]'
var _root: Node = null
var _fails: Array[String] = []
func _fail(m: String) -> void:
	_fails.append(m); printerr("FAIL: " + m)
func _near(a: float, b: float, tol: float) -> bool:
	return absf(a - b) <= tol
func _layer(base: int, top: int, dir: int, spd: int, turb: int) -> Dictionary:
	return {"base_m": base, "top_m": top, "wind_dir_deg": dir, "wind_speed_kts": spd, "turbulence": turb, "cloud_type": 0, "coverage_oktas": 0}
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	var jsb = _root.jsb_node
	if jsb == null:
		_fail("no JSBGodot node"); _finish(); return
	# The live METAR overrides the synthetic weather this test sets (the
	# checks only held while the fetch failed offline) — switch it off.
	if _root._live_weather:
		_root._live_weather.enabled = false

	# ── Part 1: sampler ────────────────────────────────────────────────────
	jsb.set_weather([_layer(0, 1000, 270, 20, 1), _layer(2000, 4000, 180, 40, 3)], 990.0)
	var w: Vector3 = jsb.get_weather_wind_ned_kts(500.0)
	if not (_near(w.x, 0.0, 0.01) and _near(w.y, 20.0, 0.01)):
		_fail("wind from 270 at 20 kt should blow toward +east, got %s" % w)
	w = jsb.get_weather_wind_ned_kts(3000.0)
	if not (_near(w.x, 40.0, 0.01) and _near(w.y, 0.0, 0.01)):
		_fail("wind from 180 at 40 kt should blow toward +north, got %s" % w)
	w = jsb.get_weather_wind_ned_kts(1500.0)  # halfway across the 1000..2000 gap
	if not (_near(w.x, 20.0, 0.01) and _near(w.y, 10.0, 0.01)):
		_fail("gap blends the wind vectors, expected (20, 10), got %s" % w)
	w = jsb.get_weather_wind_ned_kts(9000.0)
	if not _near(w.x, 40.0, 0.01):
		_fail("above the top layer the top layer applies, got %s" % w)
	w = jsb.get_weather_wind_ned_kts(-50.0)
	if not _near(w.y, 20.0, 0.01):
		_fail("below the lowest base the ground layer applies, got %s" % w)
	if jsb.get_weather_turbulence(1400.0) != 1 or jsb.get_weather_turbulence(1600.0) != 3:
		_fail("gap turbulence should pick the nearer layer (got %d / %d)" % [jsb.get_weather_turbulence(1400.0), jsb.get_weather_turbulence(1600.0)])
	if not _near(jsb.get_weather_qnh_hpa(), 990.0, 0.001):
		_fail("QNH not stored")
	# Layers given out of order are sorted by base.
	jsb.set_weather([_layer(2000, 4000, 180, 40, 0), _layer(0, 1000, 90, 10, 0)], 1013.0)
	w = jsb.get_weather_wind_ned_kts(500.0)
	if not _near(w.y, -10.0, 0.01):
		_fail("layers should be sorted by base; got %s at 500 m" % w)
	print("[weather] sampler OK")

	# ── Part 2: through the FDM ────────────────────────────────────────────
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257695.0, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	jsb.set_weather([_layer(0, 3000, 270, 20, 2)], 990.0)
	await get_tree().create_timer(1.0).timeout
	var wn: float = jsb.get_property("atmosphere/wind-north-fps")
	var we: float = jsb.get_property("atmosphere/wind-east-fps")
	var psl: float = jsb.get_property("atmosphere/P-sl-psf")
	var tt: float = jsb.get_property("atmosphere/turb-type")
	var sev: float = jsb.get_property("atmosphere/turbulence/milspec/severity")
	var w20: float = jsb.get_property("atmosphere/turbulence/milspec/windspeed_at_20ft_AGL-fps")
	print("[weather] FDM: wind N %.2f E %.2f fps, P-sl %.1f psf, turb-type %d severity %d w20 %.1f fps" % [wn, we, psl, int(tt), int(sev), w20])
	if not (_near(wn, 0.0, 0.01) and _near(we, 20.0 * 1.6878098571, 0.01)):
		_fail("FDM wind should be 20 kt toward east (%.2f, %.2f fps)" % [wn, we])
	if not _near(psl, 990.0 * 2.08854342, 0.5):
		_fail("FDM sea-level pressure should be 990 hPa = %.1f psf, got %.1f" % [990.0 * 2.08854342, psl])
	if int(tt) != 3 or int(sev) != 4:
		_fail("moderate turbulence should select milspec (3) severity 4, got type %d severity %d" % [int(tt), int(sev)])
	if not _near(w20, 30.0 * 1.6878098571, 0.01):
		_fail("20 ft wind should be the 30 kt moderate floor, got %.2f fps" % w20)
	# Ctrl+I overlay: static text lists the store's layers.
	_root._settings.set_setting("live_weather", false)
	_root._settings.set_setting("weather_layers", [_layer(0, 3000, 270, 20, 2), _layer(4000, 5000, 200, 35, 0)])
	_root._settings.set_setting("qnh_hpa", 990)
	_root._set_weather_overlay(true)
	var txt: String = _root._hud.get_weather_label().text
	print("[weather] overlay:\n" + txt)
	if not _root._hud.get_weather_label().visible or not txt.contains("270°/20 kt") or not txt.contains("moderate turb") or not txt.contains("QNH 990") or not txt.contains("4000–5000 m"):
		_fail("static overlay text incomplete: " + txt)
	_root._set_weather_overlay(false)
	if _root._hud.get_weather_label().visible:
		_fail("overlay did not hide")
	# Live text from a canned report.
	_root._settings.set_setting("live_weather", true)
	var rep := LiveWeather.parse_response("awc", CANNED, 47.5, 8.5)
	_root._live_weather.last_weather = LiveWeather.report_to_weather(rep)
	_root._set_weather_overlay(true)
	txt = _root._hud.get_weather_label().text
	print("[weather] live overlay:\n" + txt)
	if not txt.begins_with("METAR LSZH 181620Z") or not txt.contains("Wind 290°/8 kt") or not txt.contains("QNH 1018") or not txt.contains("2/8 Altocumulus at 2680 m") or not txt.contains("29008KT"):
		_fail("live overlay text incomplete: " + txt)
	_root._settings.set_setting("live_weather", false)
	_root._set_weather_overlay(false)

	# Switching turbulence off again clears the model.
	jsb.set_weather([_layer(0, 3000, 0, 0, 0)], 1013.25)
	await get_tree().create_timer(0.5).timeout
	if int(jsb.get_property("atmosphere/turb-type")) != 0 or absf(jsb.get_property("atmosphere/wind-east-fps")) > 0.01:
		_fail("calm weather should clear wind and turbulence")
	if not _near(jsb.get_property("atmosphere/P-sl-psf"), 1013.25 * 2.08854342, 0.5):
		_fail("standard QNH not restored")
	_finish()

func _finish() -> void:
	if _fails.is_empty():
		print("smoke_weather: OK"); get_tree().quit(0)
	else:
		printerr("smoke_weather: %d failure(s)" % _fails.size()); get_tree().quit(1)
