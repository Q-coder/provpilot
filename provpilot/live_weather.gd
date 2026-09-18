class_name LiveWeather
extends Node
## Live weather: fetches the nearest METAR and turns it into the same layer
## model the Weather settings tab uses (design doc §10). Sources are pluggable
## through `source`; only Aviation Weather Center (aviationweather.gov) exists
## so far. Static functions do the parsing / translation so tests can feed
## canned responses without a network.

signal weather_updated(wx: Dictionary)   # {layers, visibility_km, qnh_hpa, status, station, obs_time}
signal weather_failed(reason: String)

const SOURCE_AWC := "awc"
const REFRESH_SEC := 600.0          # METARs are half-hourly; 10 min keeps us fresh
const SEARCH_DLAT := 1.0            # bbox half-size, degrees (~110 km N-S)
const SEARCH_DLON := 1.5            # (~110 km E-W at 47°)
const UNLIMITED_VIS_KM := 40.0      # "9999" / "6+" / "10+" -> a clear day
const FT_TO_M := 0.3048
const SM_TO_KM := 1.609344

# Cloud type indices match SettingsScreen's WEATHER_CLOUD_TYPES.
const CT_NONE := 0
const CT_CUMULUS := 1
const CT_STRATUS := 2
const CT_STRATOCUMULUS := 3
const CT_CUMULONIMBUS := 4
const CT_ALTOSTRATUS := 5
const CT_ALTOCUMULUS := 6
const CT_CIRRUS := 7
const CLOUD_THICKNESS_M := {CT_CUMULUS: 900, CT_STRATUS: 400, CT_STRATOCUMULUS: 600, CT_CUMULONIMBUS: 6000, CT_ALTOSTRATUS: 1200, CT_ALTOCUMULUS: 800, CT_CIRRUS: 1000}
const SURFACE_LAYER_TOP_M := 1500

var source := SOURCE_AWC
var enabled := false:
	set(v):
		enabled = v
		if not v and _timer:
			_timer.stop()
## Returns Vector2(lat_deg, lon_deg) of the aircraft; set by the scene.
var latlon_provider: Callable
var last_weather: Dictionary = {}

var _http: HTTPRequest
var _timer: Timer
var _busy := false


func _init() -> void:
	_http = HTTPRequest.new()
	_http.use_threads = true
	_http.timeout = 20.0
	_http.request_completed.connect(_on_request_completed)
	add_child(_http)
	_timer = Timer.new()
	_timer.wait_time = REFRESH_SEC
	_timer.one_shot = false
	_timer.timeout.connect(refresh)
	add_child(_timer)


## Fetch now (if enabled) and (re)start the periodic refresh.
func refresh() -> void:
	if not enabled:
		return
	if not is_inside_tree():
		call_deferred("refresh")  # HTTPRequest / Timer need the tree
		return
	if _timer.is_stopped():
		_timer.start()
	if _busy or not latlon_provider.is_valid():
		return
	var ll: Vector2 = latlon_provider.call()
	var url := build_url(source, ll.x, ll.y)
	if url.is_empty():
		weather_failed.emit("unknown weather source '%s'" % source)
		return
	var err := _http.request(url)
	if err != OK:
		weather_failed.emit("request error %d" % err)
		return
	_busy = true
	print("LiveWeather: requesting %s" % url)


func _on_request_completed(result: int, code: int, _headers: PackedStringArray, body: PackedByteArray) -> void:
	_busy = false
	if result != HTTPRequest.RESULT_SUCCESS or code < 200 or code >= 300:
		weather_failed.emit("HTTP result %d code %d" % [result, code])
		return
	if not latlon_provider.is_valid():
		return
	var ll: Vector2 = latlon_provider.call()
	var report := parse_response(source, body.get_string_from_utf8(), ll.x, ll.y)
	if report.is_empty():
		weather_failed.emit("no METAR station within the search box")
		return
	last_weather = report_to_weather(report)
	print("LiveWeather: %s" % last_weather.get("status", ""))
	weather_updated.emit(last_weather)


# ── Source: Aviation Weather Center ──────────────────────────────────────────

static func build_url(src: String, lat: float, lon: float) -> String:
	match src:
		SOURCE_AWC:
			return "https://aviationweather.gov/api/data/metar?format=json&bbox=%.3f,%.3f,%.3f,%.3f" % [lat - SEARCH_DLAT, lon - SEARCH_DLON, lat + SEARCH_DLAT, lon + SEARCH_DLON]
	return ""


## Parse a source response into a normalised report for the station nearest
## to (lat, lon): {station, name, obs_time (unix), lat, lon, elev_m,
## wdir_deg (-1 = variable/unknown), wspd_kts, wgst_kts (0 = none), vis_km
## (-1 = unknown), qnh_hpa (-1 = unknown), clouds: [{oktas, base_agl_m, type}],
## raw}. Empty if nothing usable.
static func parse_response(src: String, body: String, lat: float, lon: float) -> Dictionary:
	match src:
		SOURCE_AWC:
			return _parse_awc(body, lat, lon)
	return {}


static func _parse_awc(body: String, lat: float, lon: float) -> Dictionary:
	var json := JSON.new()
	if json.parse(body) != OK or not (json.data is Array):
		return {}
	var best: Dictionary = {}
	var best_d := INF
	for item in json.data:
		if not (item is Dictionary):
			continue
		var r: Dictionary = item
		if not r.has("lat") or not r.has("lon") or str(r.get("icaoId", "")).is_empty():
			continue
		var dlat: float = float(r["lat"]) - lat
		var dlon: float = (float(r["lon"]) - lon) * cos(deg_to_rad(lat))
		var d := dlat * dlat + dlon * dlon
		if d < best_d:
			best_d = d
			best = r
	if best.is_empty():
		return {}
	var rep := {
		"station": str(best.get("icaoId", "")),
		"name": str(best.get("name", "")),
		"obs_time": int(best.get("obsTime", 0)) if best.get("obsTime") != null else 0,
		"lat": float(best["lat"]),
		"lon": float(best["lon"]),
		"elev_m": float(best.get("elev", 0.0)) if best.get("elev") != null else 0.0,
		"wdir_deg": -1,
		"wspd_kts": 0,
		"wgst_kts": 0,
		"vis_km": -1.0,
		"qnh_hpa": -1.0,
		"clouds": [],
		"raw": str(best.get("rawOb", "")),
	}
	var wdir = best.get("wdir")
	if wdir != null and not (wdir is String):
		rep["wdir_deg"] = int(wdir) % 360
	if best.get("wspd") != null:
		rep["wspd_kts"] = int(best["wspd"])
	if best.get("wgst") != null:
		rep["wgst_kts"] = int(best["wgst"])
	var visib = best.get("visib")
	if visib == null:
		rep["vis_km"] = -1.0   # AWC drops "9999"; treat as unlimited downstream
	elif visib is String:
		var vs: String = visib
		if vs.ends_with("+"):
			rep["vis_km"] = UNLIMITED_VIS_KM
		elif vs.is_valid_float():
			rep["vis_km"] = float(vs) * SM_TO_KM
	else:
		rep["vis_km"] = float(visib) * SM_TO_KM
	if best.get("altim") != null:
		rep["qnh_hpa"] = float(best["altim"])
	var raw: String = rep["raw"]
	var clouds: Array = []
	if best.get("clouds") is Array:
		for c in best["clouds"]:
			if not (c is Dictionary):
				continue
			var cover := str(c.get("cover", "")).to_upper()
			var oktas := _cover_to_oktas(cover)
			if oktas <= 0:
				continue
			var base_ft := float(c.get("base", 0.0)) if c.get("base") != null else 0.0
			var special := ""
			var tok := "%s%03d" % [cover, int(round(base_ft / 100.0))]
			if raw.contains(tok + "CB"):
				special = "CB"
			elif raw.contains(tok + "TCU"):
				special = "TCU"
			clouds.append({"oktas": oktas, "base_agl_m": base_ft * FT_TO_M, "special": special})
	rep["clouds"] = clouds
	return rep


static func _cover_to_oktas(cover: String) -> int:
	match cover:
		"FEW": return 2
		"SCT": return 4
		"BKN": return 6
		"OVC", "OVX", "VV": return 8
	return 0   # CLR SKC NSC NCD CAVOK


# ── Report -> weather layers ─────────────────────────────────────────────────

## Turn a normalised report into {layers, visibility_km, qnh_hpa, status,
## station, obs_time}. Surface layer 0..1500 m (or up to the lowest cloud
## base) carries the wind; gusts set its turbulence. One layer per cloud
## group above, base = station elevation + reported AGL base, thickness by
## cloud type. METARs have no winds aloft, so every layer gets the surface wind.
static func report_to_weather(rep: Dictionary) -> Dictionary:
	var wdir: int = int(rep.get("wdir_deg", -1))
	var wspd: int = int(rep.get("wspd_kts", 0))
	var wgst: int = int(rep.get("wgst_kts", 0))
	var elev: float = float(rep.get("elev_m", 0.0))
	var turb := 0
	var gust_delta := wgst - wspd if wgst > 0 else 0
	if gust_delta >= 20:
		turb = 3
	elif gust_delta >= 10:
		turb = 2
	elif gust_delta >= 5 or wspd >= 20 or wdir < 0 and wspd >= 5:
		turb = 1
	var dir_deg := wdir if wdir >= 0 else 0

	var cloud_layers: Array = []
	for c in rep.get("clouds", []):
		var oktas: int = int(c.get("oktas", 0))
		var base_agl: float = float(c.get("base_agl_m", 0.0))
		var special: String = str(c.get("special", ""))
		var ctype := CT_CUMULUS
		if special == "CB":
			ctype = CT_CUMULONIMBUS
		elif special == "TCU":
			ctype = CT_CUMULUS
		elif base_agl >= 6000.0:
			ctype = CT_CIRRUS
		elif base_agl >= 2000.0:
			ctype = CT_ALTOSTRATUS if oktas >= 6 else CT_ALTOCUMULUS
		elif oktas >= 6:
			ctype = CT_STRATUS if base_agl < 600.0 else CT_STRATOCUMULUS
		var base_m := int(round(elev + base_agl))
		cloud_layers.append({"base_m": base_m, "top_m": base_m + int(CLOUD_THICKNESS_M[ctype]), "cloud_type": ctype, "coverage_oktas": oktas, "turbulence": 3 if ctype == CT_CUMULONIMBUS else (2 if special == "TCU" else 0)})
	cloud_layers.sort_custom(func(a, b): return int(a["base_m"]) < int(b["base_m"]))

	var layers: Array = []
	var surface_top := int(elev) + SURFACE_LAYER_TOP_M
	if not cloud_layers.is_empty():
		surface_top = mini(surface_top, int(cloud_layers[0]["base_m"]) - 100)
	surface_top = maxi(surface_top, 100)
	layers.append({"base_m": 0, "top_m": surface_top, "wind_dir_deg": dir_deg, "wind_speed_kts": wspd, "turbulence": turb, "cloud_type": CT_NONE, "coverage_oktas": 0})
	var prev_top := surface_top
	for cl in cloud_layers:
		var base_m: int = maxi(int(cl["base_m"]), prev_top)
		var top_m: int = base_m + maxi(int(cl["top_m"]) - int(cl["base_m"]), 100)  # keep the thickness when lifted
		layers.append({"base_m": base_m, "top_m": top_m, "wind_dir_deg": dir_deg, "wind_speed_kts": wspd, "turbulence": maxi(int(cl["turbulence"]), turb if base_m < 3000 else 0), "cloud_type": int(cl["cloud_type"]), "coverage_oktas": int(cl["coverage_oktas"])})
		prev_top = top_m

	var vis_km: float = float(rep.get("vis_km", -1.0))
	if vis_km < 0.0:
		vis_km = UNLIMITED_VIS_KM
	var qnh: float = float(rep.get("qnh_hpa", -1.0))
	if qnh < 0.0:
		qnh = 1013.25

	var obs: int = int(rep.get("obs_time", 0))
	var when := ""
	if obs > 0:
		var t := Time.get_datetime_dict_from_unix_time(obs)
		when = "%02d%02d%02dZ" % [t["day"], t["hour"], t["minute"]]
	var wind_txt := ("VRB" if wdir < 0 else "%03d°" % wdir) + "/%d kt" % wspd + ("G%d" % wgst if wgst > 0 else "")
	var cloud_txt := "sky clear"
	if not cloud_layers.is_empty():
		var parts: PackedStringArray = []
		for cl in cloud_layers:
			parts.append("%d/8 at %d m" % [int(cl["coverage_oktas"]), int(cl["base_m"])])
		cloud_txt = ", ".join(parts)
	var status := "%s %s: wind %s, vis %.0f km, QNH %.0f, %s" % [str(rep.get("station", "?")), when, wind_txt, vis_km, qnh, cloud_txt]
	return {"layers": layers, "visibility_km": vis_km, "qnh_hpa": qnh, "status": status, "station": str(rep.get("station", "")), "name": str(rep.get("name", "")), "obs_time": obs, "when": when, "wind_txt": wind_txt, "raw": str(rep.get("raw", ""))}
