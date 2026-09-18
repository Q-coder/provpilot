extends SceneTree
## Smoke test: rigorous projections exposed by Geo (design §9 D7 / step B).
## Reference values from pyproj 3.7.2 (EPSG:4326 -> EPSG:2056 / EPSG:32633).
## Run: godot --headless --path provpilot -s tests/smoke_geo_proj.gd

const PTS := [
	# name, lat, lon, LV95 E, LV95 N, UTM33 x, UTM33 y
	["Bern", 46.9524055556, 7.4395833333, 2600072.3897, 1200147.0556, -75179.4127, 5227685.8827],
	["Zurich LSZH", 47.4647, 8.5492, 2683732.1007, 1257694.5675, 13931.4427, 5277010.1805],
	["Oslo ENGM", 60.1939, 11.1004, 2808621.7118, 2692156.1849, 283847.9043, 6679392.0764],
	["Kirkenes", 69.7, 30.0, 3535068.9661, 3950218.8985, 1075730.7920, 7803604.9120],
	["Geneva", 46.2381, 6.1090, 2497440.2505, 1121611.0254, -185366.7548, 5159060.0715],
]

func _fail(msg: String) -> void:
	printerr("FAIL: " + msg)
	quit(1)

func _initialize() -> void:
	for p in PTS:
		var lv: Vector2 = Geo.wgs84_to_lv95(p[1], p[2])
		var d_lv: float = Vector2(lv.x - p[3], lv.y - p[4]).length()
		if d_lv > 0.01:
			_fail("%s LV95 off by %.4f m" % [p[0], d_lv]); return
		var back: Vector2 = Geo.lv95_to_wgs84(lv.x, lv.y)
		var d_rt: float = Vector2((back.x - p[1]) * 111000.0, (back.y - p[2]) * 111000.0 * cos(deg_to_rad(p[1]))).length()
		if d_rt > 0.02:
			_fail("%s LV95 round trip off by %.4f m" % [p[0], d_rt]); return
		var u: Vector2 = Geo.wgs84_to_utm(p[1], p[2], 33, true)
		var d_u: float = Vector2(u.x - p[5], u.y - p[6]).length()
		if d_u > 0.01:
			_fail("%s UTM33 off by %.4f m" % [p[0], d_u]); return
		var ub: Vector2 = Geo.utm_to_wgs84(u.x, u.y, 33, true)
		if Vector2((ub.x - p[1]) * 111000.0, (ub.y - p[2]) * 111000.0).length() > 0.01:
			_fail("%s UTM round trip off" % p[0]); return
	# Grid convergence: ~0 at Bern, -2.75 at Oslo, -17.87 at Kirkenes.
	var c_bern: float = Geo.lv95_grid_convergence_deg(46.9524055556, 7.4395833333)
	var c_oslo: float = Geo.lv95_grid_convergence_deg(60.1939, 11.1004)
	var c_kirk: float = Geo.lv95_grid_convergence_deg(69.7, 30.0)
	if absf(c_bern) > 0.01 or absf(c_oslo + 2.748) > 0.02 or absf(c_kirk + 17.867) > 0.02:
		_fail("convergence: bern=%.3f oslo=%.3f kirkenes=%.3f" % [c_bern, c_oslo, c_kirk]); return
	if Geo.utm_zone_for(8.5) != 32 or Geo.utm_zone_for(11.1) != 32 or Geo.utm_zone_for(30.0) != 36:
		_fail("utm_zone_for"); return
	# AirportsLib: runway grid headings = true + convergence at the airport.
	var found_engm := false
	for ap in AirportsLib.load_default():
		if str(ap.get("icao", "")) != "ENGM":
			continue
		found_engm = true
		for rwy in ap.get("runways", []):
			var diff: float = fposmod(float(rwy["grid_heading_deg"]) - float(rwy["heading_deg"]) + 180.0, 360.0) - 180.0
			if absf(diff + 2.75) > 0.05:
				_fail("ENGM %s grid-true heading diff %.3f, expected -2.75" % [rwy.get("designator", "?"), diff]); return
	if not found_engm:
		print("note: ENGM not in catalog — grid-heading check skipped")
	print("smoke_geo_proj: OK")
	quit(0)
