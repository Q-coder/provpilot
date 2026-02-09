extends Node
## POILoader — Loads Points of Interest from scenery/poi/ manifests
##
## Currently supports airport definitions (scenery/poi/airports.json).
## POI data is available for other systems (e.g. HUD, map, navigation)
## but does NOT render anything itself.

class_name POILoader

@export var poi_directory: String = "scenery/poi"

# Loaded POI data
var airports: Array[Dictionary] = []

var _resolved_dir: String = ""
var _main_node: Node = null


func _ready() -> void:
	_resolved_dir = _resolve_scenery_path(poi_directory)
	_main_node = get_parent()
	_load_airports()
	print("POILoader: %d airports loaded" % airports.size())


func _load_airports() -> void:
	var path = _resolved_dir.path_join("airports.json")
	var file = FileAccess.open(path, FileAccess.READ)
	if not file:
		push_warning("POILoader: No airports.json at '%s'" % path)
		return

	var json = JSON.new()
	if json.parse(file.get_as_text()) != OK:
		push_error("POILoader: Bad airports JSON: %s" % json.get_error_message())
		return
	file.close()

	var data = json.data
	for ap in data.get("airports", []):
		airports.append(ap)


# ── Queries ────────────────────────────────────────────────────────────

## Find nearest airport to a Swiss LV95 position
func find_nearest_airport(swiss_e: float, swiss_n: float) -> Dictionary:
	var best := {}
	var best_dist := INF
	for ap in airports:
		var de = swiss_e - float(ap.get("center_e", 0))
		var dn = swiss_n - float(ap.get("center_n", 0))
		var d = de * de + dn * dn
		if d < best_dist:
			best_dist = d
			best = ap
	return best


## Get airport by ICAO code
func get_airport(icao: String) -> Dictionary:
	for ap in airports:
		if ap.get("icao", "") == icao:
			return ap
	return {}


## Get all airport ICAO codes
func get_airport_codes() -> Array[String]:
	var codes: Array[String] = []
	for ap in airports:
		codes.append(ap.get("icao", ""))
	return codes


# ── Path Resolution ────────────────────────────────────────────────────

static func _resolve_scenery_path(path: String) -> String:
	if path.begins_with("/") or path.begins_with("res://") or path.begins_with("user://"):
		return path
	var project_dir = ProjectSettings.globalize_path("res://")
	var workspace_root = project_dir.get_base_dir()
	return workspace_root.path_join(path)
