extends SceneTree
## Smoke test: radio navigation (docs/RADIO_NAVIGATION.md) — the WMM2025
## magnetic model against NOAA's test values, the navaid catalog, the WGS84
## geodesics and the NAV / DME / ADF receiver geometry.
## Run: godot --headless --path provpilot -s tests/smoke_radio_nav.gd

var _fails := 0

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails += 1
		printerr("  FAIL: " + what)

## Spherical destination point: [lat, lon] at bearing / distance from a point.
static func _dest(lat: float, lon: float, bearing_deg: float, dist_m: float) -> Array:
	var R := 6371008.8
	var d: float = dist_m / R
	var p1: float = deg_to_rad(lat)
	var l1: float = deg_to_rad(lon)
	var b: float = deg_to_rad(bearing_deg)
	var p2: float = asin(sin(p1) * cos(d) + cos(p1) * sin(d) * cos(b))
	var l2: float = l1 + atan2(sin(b) * sin(d) * cos(p1), cos(d) - sin(p1) * sin(p2))
	return [rad_to_deg(p2), rad_to_deg(l2)]

func _initialize() -> void:
	# WMM2025 test values (NOAA WMM2025_TestValues.txt): year, alt km, lat, lon, declination, inclination
	var wmm := [
		[2025.0, 28, 89, -121, -99.77, 88.47],
		[2025.0, 65, 43, 93, 0.50, 64.10],
		[2026.0, 46, -24, -122, 14.01, -34.17],
		[2027.5, 8, 62, 53, 19.39, 76.67],
		[2029.5, 93, -2, 158, 7.09, -17.84],
	]
	for t in wmm:
		var d: float = GeoMagnetic.declination_deg(t[2], t[3], t[1] * 1000.0, t[0])
		var i: float = GeoMagnetic.inclination_deg(t[2], t[3], t[1] * 1000.0, t[0])
		_check(absf(d - t[4]) < 0.02 and absf(i - t[5]) < 0.02, "WMM2025 %s lat %d lon %d: D %.2f (%.2f) I %.2f (%.2f)" % [t[0], t[2], t[3], d, t[4], i, t[5]])
	var yr: float = GeoMagnetic.decimal_year_now()
	_check(yr > 2026.0 and yr < 2031.0, "decimal year now %.3f" % yr)
	var zrh_var: float = GeoMagnetic.declination_deg(47.46, 8.55, 430.0, yr)
	_check(zrh_var > 2.5 and zrh_var < 4.5, "declination at Zurich %.2f deg E" % zrh_var)

	# catalog
	var n: int = NavaidsLib.count()
	_check(n > 800, "navaid catalog loaded (%d stations)" % n)
	var klo: Array = NavaidsLib.find_frequency(114850, NavaidsLib.KIND_VOR)
	var klo_ok := false
	for s in klo:
		klo_ok = klo_ok or s["ident"] == "KLO"
	_check(klo_ok, "114.85 MHz finds KLO Kloten (%d stations share the frequency)" % klo.size())
	var tra: Array = NavaidsLib.find_frequency(114300, NavaidsLib.KIND_VOR | NavaidsLib.KIND_DME)
	var tra_ok := false
	for s in tra:
		if s["ident"] == "TRA" and (int(s["kind"]) & NavaidsLib.KIND_VOR) == 0:
			tra_ok = true
	_check(tra_ok, "114.30 MHz finds the TRA DME without azimuth")
	var ber: Array = NavaidsLib.find_frequency(335, NavaidsLib.KIND_NDB)
	var ber_ok := false
	for s in ber:
		ber_ok = ber_ok or s["ident"] == "BER"
	_check(ber_ok, "335 kHz finds BER Bern NDB (%d stations share the frequency)" % ber.size())
	var near: Array = NavaidsLib.nearby(47.4647, 8.5492, 10.0, NavaidsLib.KIND_VOR)
	_check(near.size() >= 1 and near[0]["ident"] == "KLO" and near[0]["distance_nm"] < 2.0, "nearest VOR to LSZH is KLO, %.2f nm" % (near[0]["distance_nm"] if near.size() else -1.0))

	# geodesy: Bern - Zurich main station, 96 137 m (independent Vincenty), bearings consistent
	var d_m: float = NavaidsLib.distance_m(46.9489, 7.4394, 47.3779, 8.5403)
	_check(absf(d_m - 96137.0) < 5.0, "Vincenty Bern -> Zurich %.0f m" % d_m)
	var b12: float = NavaidsLib.bearing_deg(46.9489, 7.4394, 47.3779, 8.5403)
	_check(b12 > 59.0 and b12 < 61.5, "initial bearing Bern -> Zurich %.1f deg" % b12)
	_check(absf(NavaidsLib.angle_diff_deg(10.0, 350.0) - 20.0) < 1e-9 and absf(NavaidsLib.angle_diff_deg(350.0, 10.0) + 20.0) < 1e-9, "angle_diff wraps")

	# receiver geometry: 10 nm north of KLO, 2000 m, heading south
	var rn := RadioNav.new()
	var lat0: float = 47.4571 + 10.0 / 60.0
	rn.nav_frequency_mhz = 114.85
	rn.obs_deg = 0.0
	rn.adf_frequency_khz = 335.0
	for k in range(60):
		rn.update(0.1, lat0, 8.54558, 2000.0, 180.0)
	_check(rn.nav_in_range() and rn.nav_ident() == "KLO", "NAV receives KLO 10 nm north of the station")
	_check(absf(NavaidsLib.angle_diff_deg(rn.nav_radial_deg(), 360.0 - 0.514)) < 0.3, "radial 360 minus the station declination (%.2f)" % rn.nav_radial_deg())
	_check(rn.nav_from_flag() and not rn.nav_to_flag(), "OBS 360 on the 360 radial shows FROM")
	_check(absf(rn.cdi_deflection()) < 1.0, "needle centred (%.2f)" % rn.cdi_deflection())
	_check(rn.dme_in_range() and absf(rn.dme_distance_nm() - 10.04) < 0.1, "DME slant range %.2f nm" % rn.dme_distance_nm())
	rn.obs_deg = 180.0
	for k in range(60):
		rn.update(0.1, lat0, 8.54558, 2000.0, 180.0)
	_check(rn.nav_to_flag() and not rn.nav_from_flag() and absf(rn.cdi_deflection()) < 1.0, "OBS 180 shows TO, needle centred (%.2f)" % rn.cdi_deflection())
	# 5 deg east of the 360 radial, OBS 360: the course is to the left (negative), FROM
	var east_lon: float = 8.54558 + (10.0 / 60.0) * tan(deg_to_rad(5.0)) / cos(deg_to_rad(47.5))
	rn.obs_deg = 0.0
	for k in range(60):
		rn.update(0.1, lat0, east_lon, 2000.0, 180.0)
	_check(rn.nav_from_flag() and rn.cdi_deflection() < -4.0 and rn.cdi_deflection() > -6.0, "5 deg east of the course: needle left, %.1f" % rn.cdi_deflection())
	# inbound on the 180 course from the north-east side: TO, needle right (course to the right)
	rn.obs_deg = 180.0
	for k in range(60):
		rn.update(0.1, lat0, east_lon, 2000.0, 180.0)
	_check(rn.nav_to_flag() and rn.cdi_deflection() > 4.0, "inbound TO from east of the course: needle right, %.1f" % rn.cdi_deflection())
	# out of range: 200 nm away
	for k in range(30):
		rn.update(0.1, 47.4571 + 200.0 / 60.0, 8.54558, 2000.0, 180.0)
	_check(not rn.nav_in_range() and not rn.dme_in_range(), "no reception 200 nm out")
	# radio horizon: on the ground 25 nm away the VOR is below the horizon
	for k in range(30):
		rn.update(0.1, 47.4571 + 25.0 / 60.0, 8.54558, 450.0, 180.0)
	_check(not rn.nav_in_range(), "25 nm out at 450 m: below the radio horizon")
	for k in range(30):
		rn.update(0.1, 47.4571 + 25.0 / 60.0, 8.54558, 2500.0, 180.0)
	_check(rn.nav_in_range(), "25 nm out at 2500 m: in range")
	# ADF: 10 nm north of BER, heading east -> station on the right wing (relative 180 - 90 = 90)
	for k in range(100):
		rn.update(0.1, 46.8991 + 10.0 / 60.0, 7.51048, 1500.0, 90.0)
	_check(rn.adf_in_range() and rn.adf_ident() == "BER", "ADF receives BER 10 nm north of it")
	_check(absf(NavaidsLib.angle_diff_deg(rn.adf_relative_bearing_deg(), 90.0)) < 1.5, "ADF needle at 90 deg relative (%.1f)" % rn.adf_relative_bearing_deg())
	for k in range(100):
		rn.update(0.1, 46.8991 + 10.0 / 60.0, 7.51048, 1500.0, 180.0)
	_check(absf(NavaidsLib.angle_diff_deg(rn.adf_relative_bearing_deg(), 0.0)) < 1.5, "heading south: needle straight ahead (%.1f)" % rn.adf_relative_bearing_deg())
	# ILS: LSZH 16 (IZH 110.50) from the bundled catalog runway; 5 nm out on the
	# extended centreline at the 3 deg slope -> both needles centred, TO, DME ~5 nm
	var ils_list: Array = NavaidsLib.find_ils(110500)
	var izh: Dictionary = {}
	for s in ils_list:
		if s["icao"] == "LSZH" and s["rwy"] == "16":
			izh = s
	_check(not izh.is_empty() and izh["resolved"], "ILS 110.50 is IZH LSZH 16, geometry resolved (%d ILS in the catalog)" % NavaidsLib.ils_count())
	# gery 2026-11-08: no ILS for LSZH 14 on 111.75 — nav.dat 2013 still had IKL on 108.30;
	# scripts/ils_updates.json carries the AIP correction
	var ikl_new := false
	for s in NavaidsLib.find_ils(111750):
		if s["icao"] == "LSZH" and s["rwy"] == "14" and s["ident"] == "IKL":
			ikl_new = true
	var ikl_old := false
	for s in NavaidsLib.find_ils(108300):
		if s["icao"] == "LSZH":
			ikl_old = true
	_check(ikl_new and not ikl_old, "ILS 111.75 is IKL LSZH 14 (AIP correction over the 2013 nav.dat), nothing of LSZH left on 108.30")
	if not izh.is_empty() and izh["resolved"]:
		var course: float = izh["course_deg"]
		var gs_lat: float = izh["gs_lat"]
		var gs_lon: float = izh["gs_lon"]
		var gs_elev: float = izh["gs_elev_m"]
		var out5: Array = _dest(gs_lat, gs_lon, fposmod(course + 180.0, 360.0), 5.0 * 1852.0)
		var alt5: float = gs_elev + tan(deg_to_rad(izh["gs_deg"])) * 5.0 * 1852.0
		rn.nav_frequency_mhz = 110.50
		rn.obs_deg = course
		for k in range(60):
			rn.update(0.1, out5[0], out5[1], alt5, course)
		_check(rn.nav_is_loc() and rn.nav_in_range() and rn.nav_ident() == "IZH" and rn.ils_runway() == "LSZH 16", "localizer received: %s %s" % [rn.nav_ident(), rn.ils_runway()])
		_check(rn.nav_to_flag() and absf(rn.cdi_deflection()) < 0.5, "on the centreline: TO, needle centred (%.2f)" % rn.cdi_deflection())
		_check(rn.gs_in_range() and absf(rn.gs_deflection()) < 0.3, "on the 3 deg slope: GS centred (%.2f)" % rn.gs_deflection())
		_check(rn.dme_in_range() and absf(rn.dme_distance_nm() - 5.0) < 0.15, "DME %.2f nm" % rn.dme_distance_nm())
		# 1 deg left of the course (looking inbound): the course is to the right -> needle right (+)
		var left: Array = _dest(out5[0], out5[1], fposmod(course - 90.0, 360.0), 5.0 * 1852.0 * tan(deg_to_rad(1.0)))
		for k in range(60):
			rn.update(0.1, left[0], left[1], alt5, course)
		_check(rn.cdi_deflection() > 2.0 and rn.cdi_deflection() < 8.0, "1 deg left of the localizer: needle right, %.1f (half width %.2f deg)" % [rn.cdi_deflection(), izh["half_width_deg"]])
		# 0.3 deg below the slope: fly up (+)
		for k in range(60):
			rn.update(0.1, out5[0], out5[1], alt5 - tan(deg_to_rad(0.3)) * 5.0 * 1852.0, course)
		_check(rn.gs_deflection() > 1.0 and rn.gs_deflection() < 2.0, "0.3 deg below the glide slope: GS needle up, %.2f" % rn.gs_deflection())
		# back course: 4 nm beyond the far end, FROM and no glide slope
		var back: Array = _dest(izh["far_lat"], izh["far_lon"], course, 4.0 * 1852.0)
		for k in range(60):
			rn.update(0.1, back[0], back[1], alt5, course)
		_check(rn.nav_in_range() and rn.nav_from_flag() and not rn.gs_in_range(), "back course: FROM, glide slope flagged")
		# an ILS channel with no ILS anywhere near leaves the receiver silent
		rn.nav_frequency_mhz = 111.95
		for k in range(30):
			rn.update(0.1, out5[0], out5[1], alt5, course)
		_check(not rn.nav_in_range(), "111.95 with no ILS in range: flagged")
	# gery 2026-11-01: LSZH 28 (IZW 109.75, 3.3 deg) from 3 nm on the final —
	# and the same position tuned to 110.75, which is ILS 34 (IZS): a localizer
	# 60 deg off the heading pegs the needle and has no glide slope here
	# Runway 10/28 only exists in the Switzerland base pack: hand the navaids
	# the catalog with the pack runways merged in, as the welcome screen does
	var pack := "/Volumes/Data1/provpilot/build/switzerland-base/Switzerland"
	var izw: Dictionary = {}
	if DirAccess.dir_exists_absolute(pack):
		var catalog: Array[Dictionary] = AirportsLib.load_default()
		RunwaysLib.merge_into_catalog(catalog, RunwaysLib.load_merged([{"path": pack, "enabled": true, "priority": 1}]))
		NavaidsLib.set_airports(catalog)
		for s in NavaidsLib.find_ils(109750):
			if s["icao"] == "LSZH" and s["rwy"] == "28":
				izw = s
		_check(not izw.is_empty() and izw["resolved"], "ILS 109.75 is IZW LSZH 28, resolved against the pack's runway 10/28")
	else:
		print("  skip: Switzerland base pack not mounted, no runway 10/28 for the ILS 28 check")
	if not izw.is_empty() and izw["resolved"]:
		var crs28: float = izw["course_deg"]
		var out3: Array = _dest(izw["gs_lat"], izw["gs_lon"], fposmod(crs28 + 180.0, 360.0), 3.0 * 1852.0)
		var alt3: float = izw["gs_elev_m"] + tan(deg_to_rad(izw["gs_deg"])) * 3.0 * 1852.0
		rn.nav_frequency_mhz = 109.75
		rn.obs_deg = crs28
		for k in range(60):
			rn.update(0.1, out3[0], out3[1], alt3, crs28)
		_check(rn.nav_is_loc() and rn.nav_ident() == "IZW" and rn.ils_runway() == "LSZH 28", "109.75 on the 28 final: %s %s" % [rn.nav_ident(), rn.ils_runway()])
		_check(rn.nav_to_flag() and absf(rn.cdi_deflection()) < 0.5 and rn.gs_in_range() and absf(rn.gs_deflection()) < 0.3, "ILS 28: localizer and glide slope centred (%.2f / %.2f), DME %.1f nm" % [rn.cdi_deflection(), rn.gs_deflection(), rn.dme_distance_nm()])
		rn.nav_frequency_mhz = 110.75
		for k in range(60):
			rn.update(0.1, out3[0], out3[1], alt3, crs28)
		_check(rn.nav_is_loc() and rn.ils_runway() == "LSZH 34", "110.75 at the same spot is ILS 34 (%s %s)" % [rn.nav_ident(), rn.ils_runway()])
		_check(absf(rn.cdi_deflection()) > 9.0 and not rn.gs_in_range(), "ILS 34 seen from the 28 final: needle pegged (%.1f), glide slope flagged" % rn.cdi_deflection())
	var rn2 := RadioNav.new()
	rn2.receiver_index = 1
	rn2.nav_frequency_mhz = rn.nav_frequency_mhz
	rn2.obs_deg = rn.obs_deg
	rn2.update(0.5, 47.4647, 8.5492, 432.0, 0.0)
	var props2: Dictionary = rn2.get_properties()
	_check(props2.has("instrumentation/nav[1]/in-range") and not props2.has("instrumentation/nav/in-range") and not props2.has("instrumentation/dme/in-range"), "receiver 1 writes instrumentation/nav[1]/... only (no DME / ADF)")
	var props: Dictionary = rn.get_properties()
	_check(props.has("instrumentation/nav/heading-needle-deflection") and props.has("instrumentation/adf/indicated-bearing-deg") and props.has("instrumentation/dme/indicated-distance-nm"), "FlightGear property names published")
	print("  status: " + rn.status_text())
	if _fails == 0:
		print("smoke_radio_nav: OK")
		quit(0)
	else:
		printerr("smoke_radio_nav: %d failures" % _fails)
		quit(1)
