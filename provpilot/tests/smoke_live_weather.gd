extends SceneTree
## Smoke: LiveWeather parsing + METAR -> layer translation on canned AWC JSON
## (no network). Run: godot --headless --path provpilot -s tests/smoke_live_weather.gd
## Set PROVPILOT_NET=1 to also do one real fetch around Zürich.
var _fails: Array[String] = []
func check(ok: bool, msg: String) -> void:
	if ok: print("  PASS: " + msg)
	else: _fails.append(msg); printerr("  FAIL: " + msg)

const BODY := """[
{"icaoId":"LSMD","obsTime":1789748400,"wdir":340,"wspd":7,"wgst":null,"visib":null,"altim":1018,"clouds":[{"cover":"FEW","base":7700}],"elev":436,"lat":47.399,"lon":8.648,"rawOb":"METAR LSMD 181620Z AUTO 34007KT 9999NDV FEW077 20/09 Q1018","name":"Dübendorf"},
{"icaoId":"LSME","obsTime":1789748400,"wdir":"VRB","wspd":1,"wgst":null,"visib":null,"altim":1018,"clouds":[{"cover":"BKN","base":6900}],"elev":427,"lat":47.092,"lon":8.305,"rawOb":"METAR LSME 181620Z AUTO VRB01KT 9999NDV BKN069 20/10 Q1018","name":"Emmen"},
{"icaoId":"LSZH","obsTime":1789748400,"wdir":290,"wspd":18,"wgst":31,"visib":"2.5","altim":1004.3,"clouds":[{"cover":"SCT","base":1500},{"cover":"BKN","base":4000},{"cover":"OVC","base":25000}],"elev":424,"lat":47.48,"lon":8.536,"rawOb":"METAR LSZH 181620Z 29018G31KT 4000 SCT015 BKN040CB OVC250 20/08 Q1004","name":"Zürich"}
]"""

const LiveWeather := preload("res://live_weather.gd")

func _initialize() -> void:
	print("== LiveWeather ==")
	var url := LiveWeather.build_url("awc", 47.5, 8.5)
	check(url.begins_with("https://aviationweather.gov/api/data/metar?format=json&bbox=46.500,7.000,48.500,10.000"), "AWC url with bbox: %s" % url)
	check(LiveWeather.build_url("nope", 0, 0) == "", "unknown source gives no url")

	# Nearest station: Schaffhausen (47.68, 8.62) -> LSZH; near Emmen -> LSME.
	var rep := LiveWeather.parse_response("awc", BODY, 47.68, 8.62)
	check(rep.get("station") == "LSZH", "nearest station to Schaffhausen is LSZH (got %s)" % rep.get("station"))
	var rep2 := LiveWeather.parse_response("awc", BODY, 47.1, 8.3)
	check(rep2.get("station") == "LSME", "nearest station to Emmen is LSME")
	check(int(rep2["wdir_deg"]) == -1 and int(rep2["wspd_kts"]) == 1, "VRB wind parses as direction -1")
	check(float(rep2["vis_km"]) < 0.0, "null visib (9999) parses as unknown")
	check(LiveWeather.parse_response("awc", "not json", 47, 8).is_empty(), "garbage body -> empty report")
	check(LiveWeather.parse_response("awc", "[]", 47, 8).is_empty(), "no stations -> empty report")

	# LSZH report fields.
	check(int(rep["wdir_deg"]) == 290 and int(rep["wspd_kts"]) == 18 and int(rep["wgst_kts"]) == 31, "wind 290/18G31 parsed")
	check(absf(float(rep["vis_km"]) - 2.5 * 1.609344) < 0.01, "2.5 SM -> km")
	check(absf(float(rep["qnh_hpa"]) - 1004.3) < 0.01, "altim -> QNH hPa")
	var clouds: Array = rep["clouds"]
	check(clouds.size() == 3, "three cloud groups")
	check(clouds.size() == 3 and clouds[1]["special"] == "CB", "BKN040CB flagged as CB from the raw text")
	check(clouds.size() == 3 and int(clouds[0]["oktas"]) == 4 and absf(float(clouds[0]["base_agl_m"]) - 1500 * 0.3048) < 0.1, "SCT015 -> 4/8 at 457 m AGL")

	# Translation to layers.
	var wx := LiveWeather.report_to_weather(rep)
	var layers: Array = wx["layers"]
	check(layers.size() == 4, "surface + 3 cloud layers (got %d)" % layers.size())
	if layers.size() == 4:
		var s: Dictionary = layers[0]
		check(int(s["base_m"]) == 0 and int(s["top_m"]) == 424 + 457 - 100, "surface layer tops 100 m below the lowest cloud base (top %d)" % int(s["top_m"]))
		check(int(s["wind_dir_deg"]) == 290 and int(s["wind_speed_kts"]) == 18 and int(s["turbulence"]) == 2, "gust factor 13 kt -> moderate turbulence")
		check(int(s["cloud_type"]) == 0 and int(s["coverage_oktas"]) == 0, "surface layer has no cloud")
		var c1: Dictionary = layers[1]
		check(int(c1["base_m"]) == 424 + 457 and int(c1["coverage_oktas"]) == 4 and int(c1["cloud_type"]) == LiveWeather.CT_CUMULUS, "SCT at 881 m -> cumulus 4/8")
		var c2: Dictionary = layers[2]
		check(int(c2["cloud_type"]) == LiveWeather.CT_CUMULONIMBUS and int(c2["turbulence"]) == 3 and int(c2["top_m"]) - int(c2["base_m"]) == 6000, "CB layer: cumulonimbus, severe, 6 km thick")
		var c3: Dictionary = layers[3]
		check(int(c3["cloud_type"]) == LiveWeather.CT_CIRRUS and int(c3["coverage_oktas"]) == 8, "OVC250 -> cirrus 8/8")
		check(int(c3["base_m"]) >= int(c2["top_m"]), "layers never overlap (c3 base %d >= c2 top %d)" % [int(c3["base_m"]), int(c2["top_m"])])
		check(int(c1["wind_dir_deg"]) == 290 and int(c1["wind_speed_kts"]) == 18, "cloud layers carry the surface wind")
	check(absf(float(wx["visibility_km"]) - 2.5 * 1.609344) < 0.01 and absf(float(wx["qnh_hpa"]) - 1004.3) < 0.01, "visibility + QNH passed through")
	check(str(wx["status"]).begins_with("LSZH 181620Z: wind 290°/18 ktG31"), "status line: %s" % wx["status"])

	# Clear-sky / unknown-visibility report.
	var wx2 := LiveWeather.report_to_weather(rep2)
	var l2: Array = wx2["layers"]
	check(l2.size() == 2 and int(l2[0]["top_m"]) == 427 + 1500 and int(l2[0]["turbulence"]) == 0, "calm report: surface layer 1500 m AGL, no turbulence")
	check(absf(float(wx2["visibility_km"]) - LiveWeather.UNLIMITED_VIS_KM) < 0.01, "unknown visibility -> unlimited (%.0f km)" % LiveWeather.UNLIMITED_VIS_KM)
	check(l2.size() == 2 and int(l2[1]["cloud_type"]) == LiveWeather.CT_ALTOSTRATUS and int(l2[1]["base_m"]) == 427 + int(round(6900 * 0.3048)), "BKN069 -> altostratus at 2530 m")
	var empty := LiveWeather.report_to_weather({})
	check((empty["layers"] as Array).size() == 1 and absf(float(empty["qnh_hpa"]) - 1013.25) < 0.01, "empty report -> one calm surface layer, standard QNH")

	# Refetch policy: distance and age.
	check(absf(LiveWeather.distance_km(Vector2(47.48, 8.54), Vector2(60.19, 11.10)) - 1414.0) < 30.0, "LSZH-ENGM distance ~1414 km")
	var lw2 := LiveWeather.new()
	check(lw2.needs_refetch(Vector2(47.5, 8.5), 0), "first fetch is always due")
	lw2._last_fetch_latlon = Vector2(47.5, 8.5)
	lw2._last_fetch_msec = 1000
	check(not lw2.needs_refetch(Vector2(47.6, 8.6), 60000), "no refetch after 1 min / 13 km")
	check(lw2.needs_refetch(Vector2(48.1, 8.5), 60000), "refetch after moving 67 km")
	check(lw2.needs_refetch(Vector2(47.5, 8.5), 1000 + 600000), "refetch after 10 min")
	lw2.free()

	if OS.get_environment("PROVPILOT_NET") == "1":
		_net_check()
		return
	_finish()

func _net_check() -> void:
	var lw := LiveWeather.new()
	root.add_child(lw)
	lw.enabled = true
	lw.latlon_provider = func() -> Vector2: return Vector2(47.68, 8.62)
	var got: Array = []
	lw.weather_updated.connect(func(wx): got.append(wx))
	lw.weather_failed.connect(func(r): _fails.append("net: " + r))
	await process_frame
	lw.refresh()
	var waited := 0.0
	while got.is_empty() and waited < 25.0 and _fails.is_empty():
		await create_timer(0.5).timeout
		waited += 0.5
	check(not got.is_empty(), "live fetch delivered a report: %s" % (got[0]["status"] if not got.is_empty() else "-"))
	_finish()

func _finish() -> void:
	if _fails.is_empty():
		print("SMOKE LIVE WEATHER: ALL PASS"); quit(0)
	else:
		printerr("SMOKE LIVE WEATHER: %d FAIL" % _fails.size()); quit(1)
