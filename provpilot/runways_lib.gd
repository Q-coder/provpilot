extends RefCounted
class_name RunwaysLib
## RunwaysLib — per-pack runway data (design §12, D16).
##
## Every scenery pack may carry `airports/runways.json` (schema v1, written
## by scripts/build_world_runways.py or by hand for a bespoke airport). The
## enabled packs are merged PER AIRPORT: the highest-priority pack that lists
## an ICAO defines that airport completely — its runways, an optional model,
## and whether the procedural runways are drawn. Lower packs are ignored for
## that airport, so a detail pack overrides the base tier by listing the
## airport, and removes a base runway by omitting it.
##
## Also converts merged airports into the catalog shape the welcome screen
## and spawn code use (threshold_<ident> in LV95, true heading, length).

const RUNWAYS_REL := "airports/runways.json"

## Glyph codes for the runway shader: digits 0-9, L=10, R=11, C=12, none=-1.
static func glyph_codes(ident: String) -> PackedInt32Array:
	var out := PackedInt32Array([-1, -1, -1])
	var i := 0
	for ch in ident.to_upper():
		if i >= 3:
			break
		if ch >= "0" and ch <= "9":
			out[i] = int(ch)
		elif ch == "L":
			out[i] = 10
		elif ch == "R":
			out[i] = 11
		elif ch == "C":
			out[i] = 12
		else:
			continue
		i += 1
	return out


## Read one pack's runways.json (absolute pack directory). Returns the
## parsed Dictionary or {} when absent/unreadable.
static func read_pack(pack_path: String) -> Dictionary:
	var p := pack_path.path_join(RUNWAYS_REL)
	var f := FileAccess.open(p, FileAccess.READ)
	if f == null:
		return {}
	var parsed = JSON.parse_string(f.get_as_text())
	f.close()
	if parsed is Dictionary and (parsed as Dictionary).has("airports"):
		return parsed
	push_warning("RunwaysLib: %s is not a runways.json (schema_version/airports missing)" % p)
	return {}


## Merge the enabled scenery areas (priority order, highest first) into
## icao -> airport entry. Each entry gains "pack_path" (its source pack) and
## "pack_region" (the pack's region id).
static func load_merged(scenery_areas: Array) -> Dictionary:
	var merged: Dictionary = {}
	var areas: Array = []
	for a in scenery_areas:
		if a is Dictionary and bool((a as Dictionary).get("enabled", true)):
			areas.append(a)
	areas.sort_custom(func(a, b): return int(a.get("priority", 999)) < int(b.get("priority", 999)))
	for a in areas:
		var path := str(a.get("path", ""))
		if path == "":
			continue
		var data := read_pack(path)
		if data.is_empty():
			continue
		for ap in data.get("airports", []):
			if not (ap is Dictionary):
				continue
			var icao := str(ap.get("icao", ""))
			if icao == "" or merged.has(icao):
				continue          # a higher-priority pack already defines it
			var entry: Dictionary = (ap as Dictionary).duplicate(true)
			entry["pack_path"] = path
			entry["pack_region"] = str(data.get("region", ""))
			merged[icao] = entry
	return merged


## Catalog-shaped runway list for a merged airport entry: one Dictionary per
## runway with designator "16/34", true heading of designators[0], grid
## heading, length, surface and threshold_<ident> = {e, n} in LV95 (the
## pavement end moved inward by the displaced threshold).
static func catalog_runways(ap: Dictionary) -> Array:
	var out: Array = []
	for rw in ap.get("runways", []):
		if not (rw is Dictionary):
			continue
		var ends: Array = rw.get("ends", [])
		var desigs: Array = rw.get("designators", [])
		if ends.size() != 2 or desigs.size() != 2:
			continue
		var e0: Dictionary = ends[0]
		var e1: Dictionary = ends[1]
		if not (e0.has("pavement") and e1.has("pavement")):
			continue
		var p0: Vector2 = Geo.wgs84_to_lv95(float(e0["pavement"][0]), float(e0["pavement"][1]))
		var p1: Vector2 = Geo.wgs84_to_lv95(float(e1["pavement"][0]), float(e1["pavement"][1]))
		var axis: Vector2 = p1 - p0
		var length: float = axis.length()
		if length < 1.0:
			continue
		var u: Vector2 = axis / length
		var d0: float = float(e0.get("displaced_m", 0.0))
		var d1: float = float(e1.get("displaced_m", 0.0))
		var t0: Vector2 = p0 + u * d0
		var t1: Vector2 = p1 - u * d1
		# True heading of designators[0]: OurAirports value, else the
		# initial bearing between the ends.
		var hdg: float
		if e0.has("heading_deg"):
			hdg = float(e0["heading_deg"])
		else:
			hdg = _initial_bearing(float(e0["pavement"][0]), float(e0["pavement"][1]),
				float(e1["pavement"][0]), float(e1["pavement"][1]))
		var ll: Vector2 = Geo.lv95_to_wgs84((p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5)
		var conv: float = Geo.lv95_grid_convergence_deg(ll.x, ll.y)
		var rec: Dictionary = {
			"designator": "%s/%s" % [str(desigs[0]), str(desigs[1])],
			"heading_deg": hdg,
			"grid_heading_deg": fposmod(hdg + conv, 360.0),
			"length_m": float(rw.get("length_m", length)),
			"width_m": float(rw.get("width_m", 45.0)),
			"surface": str(rw.get("surface", "asphalt")),
			"geometry_source": str(rw.get("geometry_source", "")),
		}
		rec["threshold_" + str(desigs[0])] = {"e": t0.x, "n": t0.y}
		rec["threshold_" + str(desigs[1])] = {"e": t1.x, "n": t1.y}
		out.append(rec)
	return out


## Replace the runways of catalog airports that a pack defines, and append
## pack airports the catalog lacks. `catalog` is the resolved array from
## AirportsLib.load_default(); modified in place and returned.
static func merge_into_catalog(catalog: Array[Dictionary], merged: Dictionary) -> Array[Dictionary]:
	var seen: Dictionary = {}
	for ap in catalog:
		var icao := str(ap.get("icao", ""))
		seen[icao] = true
		if merged.has(icao):
			var m: Dictionary = merged[icao]
			ap["runways"] = catalog_runways(m)
			ap["runways_source"] = "pack:" + str(m.get("pack_region", ""))
	for icao in merged.keys():
		if seen.has(icao):
			continue
		var m: Dictionary = merged[icao]
		var lv: Vector2 = Geo.wgs84_to_lv95(float(m.get("lat", 0.0)), float(m.get("lon", 0.0)))
		var ll: Vector2 = Geo.lv95_to_wgs84(lv.x, lv.y)
		catalog.append({
			"icao": icao,
			"name": str(m.get("name", "")),
			"type": str(m.get("type", "regional_airport")),
			"region": str(m.get("pack_region", "")),
			"elevation_m": int(m.get("elevation_m", 0)),
			"center_e": lv.x,
			"center_n": lv.y,
			"grid_convergence_deg": Geo.lv95_grid_convergence_deg(ll.x, ll.y),
			"runways": catalog_runways(m),
			"runways_source": "pack:" + str(m.get("pack_region", "")),
		})
	return catalog


static func _initial_bearing(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
	var p1 := deg_to_rad(lat1)
	var p2 := deg_to_rad(lat2)
	var dl := deg_to_rad(lon2 - lon1)
	var y := sin(dl) * cos(p2)
	var x := cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)
	return fposmod(rad_to_deg(atan2(y, x)), 360.0)
