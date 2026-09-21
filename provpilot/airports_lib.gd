extends RefCounted
class_name AirportsLib
## AirportsLib — Loads the bundled airport catalog (res://airports.json) and
## converts each entry into the dictionary shape expected by
## `welcome_screen.populate_airports()`:
##
##     {
##       icao, name, type, region, [quadrant],
##       center_e, center_n, elevation_m,
##       runways: [ { designator, heading_deg, grid_heading_deg, length_m, surface,
##                    threshold_<low>: {e, n}, threshold_<high>: {e, n} } ],
##     }
##
## WGS84 latitude / longitude in the JSON are converted to Swiss LV95 with the
## rigorous projection in Geo (matches the pyproj bake pipeline). Entries may
## include explicit `swiss_e` / `swiss_n` overrides when the conversion does
## not line up with the loaded scenery (e.g. LSPF Schmerlat).
##
## `heading_deg` is the runway's TRUE bearing (for display); `grid_heading_deg`
## is that bearing in the LV95 grid frame (true + grid convergence at the
## airport), which is what the flat scenery frame, spawn heading and the
## threshold positions use. The two differ by <1° in Switzerland, -2.7° at
## Oslo and -18° at Kirkenes.
##
## Also exposes a helper to derive the set of installed scenery regions from
## the `installed_scenery_packs` dict the scenery downloader maintains.

const CATALOG_PATH := "res://airports.json"


## Load the bundled catalog and return a fully-resolved airport array.
static func load_default() -> Array[Dictionary]:
	var data: Variant = _read_json(CATALOG_PATH)
	if data == null:
		push_warning("AirportsLib: failed to read %s" % CATALOG_PATH)
		return []
	var entries: Array = []
	if data is Dictionary:
		entries = (data as Dictionary).get("airports", [])
	var out: Array[Dictionary] = []
	for raw in entries:
		if raw is Dictionary:
			out.append(_resolve(raw))
	return out


## Convert one raw JSON entry into the runtime shape expected by the
## welcome screen. Pure function — no I/O.
static func _resolve(raw: Dictionary) -> Dictionary:
	var ap: Dictionary = {
		"icao": raw.get("icao", "----"),
		"name": raw.get("name", ""),
		"type": raw.get("type", "small_airport"),
		"region": raw.get("region", ""),
		"elevation_m": int(raw.get("elevation_m", 0)),
	}
	if raw.has("quadrant"):
		ap["quadrant"] = raw["quadrant"]

	# Resolve center: explicit LV95 override wins, else convert from lat/lon.
	var center_e: float
	var center_n: float
	if raw.has("swiss_e") and raw.has("swiss_n"):
		center_e = float(raw["swiss_e"])
		center_n = float(raw["swiss_n"])
	else:
		var lv := wgs84_to_lv95(float(raw.get("lat", 0.0)), float(raw.get("lon", 0.0)))
		center_e = lv.x
		center_n = lv.y
	ap["center_e"] = center_e
	ap["center_n"] = center_n

	# Grid convergence at the airport: true bearings from the catalog become
	# LV95 grid bearings for everything placed in the flat scenery frame.
	var ll: Vector2 = Geo.lv95_to_wgs84(center_e, center_n)
	var convergence: float = Geo.lv95_grid_convergence_deg(ll.x, ll.y)
	ap["grid_convergence_deg"] = convergence

	# Compute runway thresholds from heading + length (true bearing of the
	# lower-numbered designator, rotated into the grid frame).
	var runways_out: Array = []
	for r in raw.get("runways", []):
		if not (r is Dictionary):
			continue
		var desig := str(r.get("designator", ""))
		var heading: float = float(r.get("heading_deg", 0))
		var grid_heading: float = fposmod(heading + convergence, 360.0)
		var length: float = float(r.get("length_m", 0))
		var rwy: Dictionary = {
			"designator": desig,
			"heading_deg": heading,
			"grid_heading_deg": grid_heading,
			"length_m": length,
			"surface": r.get("surface", "asphalt"),
		}
		var parts := desig.split("/")
		if parts.size() >= 2 and length > 0.0:
			var hdg_rad := deg_to_rad(grid_heading)
			var half := length * 0.5
			var dx_e := sin(hdg_rad) * half
			var dy_n := cos(hdg_rad) * half
			rwy["threshold_" + parts[0]] = {"e": center_e - dx_e, "n": center_n - dy_n}
			rwy["threshold_" + parts[1]] = {"e": center_e + dx_e, "n": center_n + dy_n}
		runways_out.append(rwy)
	ap["runways"] = runways_out
	return ap


## WGS84 → Swiss LV95 (CH1903+). Rigorous projection (Geo / s3d_proj.h).
static func wgs84_to_lv95(lat_deg: float, lon_deg: float) -> Vector2:
	return Geo.wgs84_to_lv95(lat_deg, lon_deg)


## Return the set of region IDs (e.g. "switzerland", "baden-wuerttemberg")
## for which a base pack is installed.
##
## Pack IDs follow `<region>-base-v<ver>` and `<region>-detail-<quad>-v<ver>`.
## We only consider base packs here, since the base pack ships the country's
## terrain — once it's on disk the whole region is technically flyable. Detail
## quadrants only add buildings / orthophoto / vegetation density.
static func installed_regions(installed_packs: Dictionary) -> Dictionary:
	var regions: Dictionary = {}
	for pack_id in installed_packs.keys():
		var pid := str(pack_id)
		var idx := pid.find("-base-")
		if idx > 0:
			regions[pid.substr(0, idx)] = true
	return regions


## Map region id -> on-disk folder name inside any scenery root.
## Used as a fallback when the user has scenery on disk that wasn't
## installed through the in-app downloader (so it's not tracked in
## settings.installed_scenery_packs).
const REGION_DIRS := {
	"switzerland": "Switzerland",
	"baden-wuerttemberg": "Baden-Wuertemberg",
	"norway": "Norway",
	"sweden": "Sweden",
	"finland": "Finland",
	"denmark": "Denmark",
	"germany": "Germany",
	"switzerland-base": "Switzerland",
	"austria": "Austria",
	"italy": "Italy",
	"france": "France",
}


## Return the set of region IDs that have scenery on disk, derived from
## the configured `scenery_areas` (regardless of enabled flag — the files
## are present even when the area is disabled, the user can re-enable it).
## Each entry is a Dictionary with at least a "path" field.
static func installed_regions_from_areas(scenery_areas: Array) -> Dictionary:
	var regions: Dictionary = {}
	for area in scenery_areas:
		if not (area is Dictionary):
			continue
		var path := str((area as Dictionary).get("path", ""))
		if path == "":
			continue
		# Base-tier packs (build_world_terrain.py, design §11) name their
		# region in terrain/manifest.json — authoritative when present.
		var tagged := manifest_region(path)
		if tagged != "":
			regions[tagged] = true
			continue
		# Untagged packs: the area's own directory name decides. Only the
		# last path component counts, so "Germany/Baden-Wuertemberg" is the
		# BW add-on and not also the Germany base pack.
		var leaf := path.rstrip("/").get_file()
		for region_id in REGION_DIRS.keys():
			if leaf == REGION_DIRS[region_id]:
				regions[region_id] = true
	return regions


## The `"region"` scalar from `<area>/terrain/manifest.json`, or "" when the
## manifest is absent or untagged. Only the first 64 KB are read: the
## Fennoscandia manifests weigh hundreds of MB and the scalars sit at the top.
static func manifest_region(area_path: String) -> String:
	var f := FileAccess.open(area_path.path_join("terrain/manifest.json"), FileAccess.READ)
	if f == null:
		return ""
	var head := f.get_buffer(65536).get_string_from_utf8()
	f.close()
	var re := RegEx.new()
	re.compile("\"region\"\\s*:\\s*\"([^\"]+)\"")
	var m := re.search(head)
	return m.get_string(1) if m != null else ""


## Availability marker for the welcome screen list. Returns one of:
##   "✓"   region base pack installed (airport is flyable)
##   "🔒"  region base pack NOT installed (airport will fail to load scenery)
##   ""    unknown region (e.g. catalog has no region tag) — treat as flyable
static func availability_marker(airport: Dictionary, installed_regions_set: Dictionary) -> String:
	var region := str(airport.get("region", ""))
	if region == "":
		return ""
	if installed_regions_set.has(region):
		return "\u2713"
	return "\u2717"


# ── Internals ──────────────────────────────────────────────────────────

static func _read_json(path: String) -> Variant:
	# Godot 4 imports .json as a JSON resource. Loading via `load()` returns the
	# JSON instance; .data holds the parsed value. Falling back to direct
	# FileAccess parsing keeps this working if the importer ever changes.
	var res := load(path)
	if res is JSON:
		return (res as JSON).data
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return null
	var txt := f.get_as_text()
	f.close()
	var parsed = JSON.parse_string(txt)
	return parsed
