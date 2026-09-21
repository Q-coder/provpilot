extends Node
## Headless smoke: per-pack runway data and rendering (design §12).
##  - RunwaysLibScript.load_merged over the base packs gives Zurich its four
##    runways with thresholds at the OSM pavement ends;
##  - a higher-priority override pack listing LSZH with one runway wins
##    wholesale (D16), a lower-priority one is ignored;
##  - Runways.build creates one mesh per runway on the runway plane; a mesh
##    vertex at mid-length sits on the ellipsoid at the plane height.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_runways.tscn

const BUILD := "/Volumes/Data1/provpilot/build"
const RunwaysLibScript := preload("res://runways_lib.gd")
const RunwaysScript := preload("res://runways.gd")
var fails: Array[String] = []

func _fail(m: String) -> void:
	fails.append(m)
	printerr("FAIL: " + m)

func _pack(id: String, name: String) -> Dictionary:
	return {"path": BUILD.path_join(id).path_join(name), "enabled": true}

func _write_override(dir: String, runways: Array) -> void:
	DirAccess.make_dir_recursive_absolute(dir.path_join("airports"))
	var f := FileAccess.open(dir.path_join("airports/runways.json"), FileAccess.WRITE)
	f.store_string(JSON.stringify({"schema_version": 1, "region": "test-override", "airports": [
		{"icao": "LSZH", "name": "Override", "lat": 47.4647, "lon": 8.5492, "elevation_m": 432,
		 "type": "international_airport", "runways": runways}]}))
	f.close()

func _ready() -> void:
	if not DirAccess.dir_exists_absolute(BUILD.path_join("switzerland-base/Switzerland/airports")):
		print("smoke_runways: base packs not present, skipping")
		get_tree().quit(0)
		return
	var areas: Array = [
		_pack("germany", "Germany"), _pack("switzerland-base", "Switzerland"),
		_pack("austria", "Austria"), _pack("france", "France")]
	for i in range(areas.size()):
		areas[i]["priority"] = 10 + i

	# 1. Merge of the base packs.
	var merged: Dictionary = RunwaysLibScript.load_merged(areas)
	print("[runways] merged airports: %d" % merged.size())
	if not merged.has("LSZH"):
		_fail("LSZH missing from the merge"); _finish(); return
	var zrh: Dictionary = merged["LSZH"]
	var desigs: Array = []
	for rw in zrh["runways"]:
		desigs.append("/".join(PackedStringArray(rw["designators"])))
	print("[runways] LSZH: %s from %s" % [str(desigs), zrh.get("pack_region")])
	for want in ["16/34", "14/32", "10/28"]:
		if not desigs.has(want):
			_fail("LSZH lacks runway %s" % want)
	if zrh.get("pack_region") != "switzerland-base":
		_fail("LSZH should come from the switzerland-base pack, got %s" % str(zrh.get("pack_region")))
	# Germany (priority 10) also lists border airports? It must not define LSZH.
	var cat: Array = RunwaysLibScript.catalog_runways(zrh)
	var r1634: Dictionary = {}
	for c in cat:
		if c["designator"] == "16/34":
			r1634 = c
	if r1634.is_empty():
		_fail("catalog_runways lost 16/34")
	else:
		var t16: Dictionary = r1634["threshold_16"]
		var t34: Dictionary = r1634["threshold_34"]
		var exp16: Vector2 = Geo.wgs84_to_lv95(47.475713, 8.535937)
		var d16: float = Vector2(t16["e"] - exp16.x, t16["n"] - exp16.y).length()
		if d16 > 2.0:
			_fail("threshold 16 is %.1f m from the OSM pavement end" % d16)
		# 34 has a 470 m displaced threshold: its threshold lies 470 m inward.
		var exp34: Vector2 = Geo.wgs84_to_lv95(47.445562, 8.556699)
		var d34: float = Vector2(t34["e"] - exp34.x, t34["n"] - exp34.y).length()
		if absf(d34 - 470.0) > 5.0:
			_fail("threshold 34 should be 470 m inward of the pavement end, got %.1f m" % d34)
		if absf(float(r1634["heading_deg"]) - 155.0) > 1.5:
			_fail("16 heading %.1f, expected ~155" % float(r1634["heading_deg"]))
		print("[runways] 16/34 thresholds ok (d16 %.1f m, d34 inward %.1f m, hdg %.1f)" % [d16, d34, float(r1634["heading_deg"])])

	# 2. Override packs (D16).
	var tmp := OS.get_temp_dir().path_join("provpilot_smoke_runways")
	var one_rwy := [{"designators": ["16", "34"], "width_m": 60.0, "surface": "asphalt",
		"ends": [{"ident": "16", "pavement": [47.475713, 8.535937], "elev_m": 423.7},
				 {"ident": "34", "pavement": [47.445562, 8.556699], "elev_m": 423.1}]}]
	_write_override(tmp.path_join("hi"), one_rwy)
	_write_override(tmp.path_join("lo"), [])
	var areas2: Array = areas.duplicate(true)
	areas2.append({"path": tmp.path_join("hi"), "enabled": true, "priority": 1})
	areas2.append({"path": tmp.path_join("lo"), "enabled": true, "priority": 99})
	var merged2: Dictionary = RunwaysLibScript.load_merged(areas2)
	if merged2["LSZH"]["runways"].size() != 1 or merged2["LSZH"].get("pack_region") != "test-override":
		_fail("higher-priority override should replace LSZH wholesale (got %d runways from %s)" % [merged2["LSZH"]["runways"].size(), str(merged2["LSZH"].get("pack_region"))])
	var areas3: Array = areas.duplicate(true)
	areas3.append({"path": tmp.path_join("lo"), "enabled": true, "priority": 99})
	var merged3: Dictionary = RunwaysLibScript.load_merged(areas3)
	if merged3["LSZH"]["runways"].size() < 3:
		_fail("lower-priority pack must not override LSZH")
	var areas4: Array = areas.duplicate(true)
	areas4.append({"path": tmp.path_join("hi"), "enabled": false, "priority": 1})
	if RunwaysLibScript.load_merged(areas4)["LSZH"]["runways"].size() < 3:
		_fail("disabled pack must be ignored")

	# 3. Catalog merge: Zurich's curated entry gains the pack runways.
	var catalog: Array[Dictionary] = AirportsLib.load_default()
	RunwaysLibScript.merge_into_catalog(catalog, merged)
	var n_zrh := 0
	for ap in catalog:
		if ap["icao"] == "LSZH":
			n_zrh = ap["runways"].size()
	if n_zrh < 3:
		_fail("catalog LSZH should list the pack runways, got %d" % n_zrh)

	# 4. Rendering.
	var rws: Node3D = RunwaysScript.new()
	add_child(rws)
	rws.build({"LSZH": zrh})
	if rws.runway_count != zrh["runways"].size():
		_fail("Runways built %d meshes for %d runways" % [rws.runway_count, zrh["runways"].size()])
	var node: Node3D = rws.get_airport_node("LSZH")
	# Taxiways and aprons (§12.5): present in the data with the flatten
	# stage's elevations, and built into the airport's single mesh.
	var n_tw: int = zrh.get("taxiways", []).size()
	var n_ap: int = zrh.get("aprons", []).size()
	print("[runways] LSZH taxiways %d (built %d), aprons %d (built %d)" % [n_tw, rws.taxiway_count, n_ap, rws.apron_count])
	if n_tw < 50 or n_ap < 5:
		_fail("LSZH should have taxiways and aprons in the data")
	if rws.taxiway_count != n_tw:
		_fail("taxiway ribbons built %d of %d" % [rws.taxiway_count, n_tw])
	if rws.apron_count < n_ap - 2:
		_fail("apron polygons built %d of %d" % [rws.apron_count, n_ap])
	var mi: MeshInstance3D = node.get_node_or_null("Surfaces")
	if mi == null:
		_fail("no Surfaces mesh for LSZH")
	else:
		if mi.material_override != rws.get_material():
			_fail("airport mesh must use the shared material")
		var arr: Array = mi.mesh.surface_get_arrays(0)
		var v: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var uv: PackedVector2Array = arr[Mesh.ARRAY_TEX_UV]
		var c0: PackedFloat32Array = arr[Mesh.ARRAY_CUSTOM0]
		var c1: PackedFloat32Array = arr[Mesh.ARRAY_CUSTOM1]
		var recs: Array = mi.get_meta("runways")
		var rec: Dictionary = {}
		for r in recs:
			if r["name"] == "16/34":
				rec = r
		if rec.is_empty():
			_fail("no 16/34 record in the LSZH mesh")
		else:
			var worst := 0.0
			for i in range(rec["first"], rec["first"] + rec["count"]):
				var w: Vector3 = mi.transform * v[i]
				var g: Vector3 = W3DCoords.ecef_to_geodetic(w)
				var t: float = uv[i].y / float(rec["length"])
				var expect: float = lerpf(float(rec["h0"]), float(rec["h1"]), t) + RunwaysScript.SURFACE_LIFT_M
				worst = maxf(worst, absf(g.z - expect))
			print("[runways] 16/34: %d vertices, worst height error vs plane %.4f m; attrs half_w %.1f len %.0f code %d" % [rec["count"], worst, c0[rec["first"] * 4], c0[rec["first"] * 4 + 1], int(c1[rec["first"] * 4 + 2])])
			if worst > 0.05:
				_fail("mesh vertices off the runway plane by %.3f m" % worst)
			if rec["count"] < 2 * 60:
				_fail("mesh too coarse for a 3.7 km runway (%d vertices)" % rec["count"])
			if absf(c0[rec["first"] * 4] - 30.0) > 0.1:
				_fail("half width attribute wrong: %.1f" % c0[rec["first"] * 4])
			# kind concrete(1)*64 + bars 16*2 + aiming 1 = 97
			if int(c1[rec["first"] * 4 + 2]) != 1 * 64 + 16 * 2 + 1:
				_fail("packed kind/bars/aiming attribute wrong: %d" % int(c1[rec["first"] * 4 + 2]))
		# the first apron vertex sits at its stored elevation + lift
		var first_poly: Array = zrh["aprons"][0]["polygon"]
		var ap_first := -1
		for i in range(v.size()):
			if int(c1[i * 4 + 2]) == RunwaysScript.KIND_APRON * 64:
				ap_first = i
				break
		if ap_first < 0:
			_fail("no apron vertices in the mesh")
		else:
			var g0: Vector3 = W3DCoords.ecef_to_geodetic(mi.transform * v[ap_first])
			var d_h: float = absf(g0.z - (float(first_poly[0][2]) + RunwaysScript.APRON_LIFT_M))
			print("[runways] first apron vertex height err %.4f m; mesh %d vertices, %d indices" % [d_h, v.size(), (arr[Mesh.ARRAY_INDEX] as PackedInt32Array).size()])
			if d_h > 0.01:
				_fail("apron vertex off its stored height")
	# 5. Surface lift lookup for JSBSim's ground plane.
	var on_rwy: float = rws.surface_lift_at(W3DCoords.geodetic_to_ecef(47.4605, 8.5463, 424.0))   # 16/34 mid
	var off: float = rws.surface_lift_at(W3DCoords.geodetic_to_ecef(47.50, 8.70, 500.0))          # fields
	var tw0: Array = zrh["taxiways"][0]["path"][0]
	var on_taxi: float = rws.surface_lift_at(W3DCoords.geodetic_to_ecef(float(tw0[0]), float(tw0[1]), float(tw0[2])))
	print("[runways] surface lift: on 16/34 %.2f, on a taxiway node %.2f, off airport %.2f" % [on_rwy, on_taxi, off])
	if absf(on_rwy - RunwaysScript.SURFACE_LIFT_M) > 1e-6:
		_fail("lift on the runway should be %.2f, got %.2f" % [RunwaysScript.SURFACE_LIFT_M, on_rwy])
	if on_taxi < RunwaysScript.APRON_LIFT_M:
		_fail("lift on a taxiway node should be a pavement lift, got %.2f" % on_taxi)
	if off != 0.0:
		_fail("lift off the airport should be 0, got %.2f" % off)
	_finish()

func _finish() -> void:
	if fails.is_empty():
		print("smoke_runways: all checks passed")
		get_tree().quit(0)
	else:
		printerr("smoke_runways: %d failure(s)" % fails.size())
		get_tree().quit(1)
