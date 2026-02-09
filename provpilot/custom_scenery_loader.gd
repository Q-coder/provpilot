extends Node
## CustomSceneryLoader — Scans scenery/custom/ for user add-on packages
##
## Each sub-directory under scenery/custom/ is a scenery package if it
## contains a package.json with at least { "name": "...", "type": "..." }.
## Supported types: "buildings", "terrain_overlay", "objects".
##
## This loader discovers packages at startup and exposes them for other
## systems to query. Actual loading is deferred to the relevant loaders
## (e.g. BuildingLoader picks up custom building packages).

class_name CustomSceneryLoader

@export var custom_directory: String = "scenery/custom"

var packages: Array[Dictionary] = []  # List of discovered packages
var _resolved_dir: String = ""


func _ready() -> void:
	_resolved_dir = _resolve_scenery_path(custom_directory)
	_scan_packages()
	if packages.size() > 0:
		print("CustomSceneryLoader: Discovered %d packages" % packages.size())
	else:
		print("CustomSceneryLoader: No custom scenery packages found")


func _scan_packages() -> void:
	var dir = DirAccess.open(_resolved_dir)
	if dir == null:
		# Directory might not exist yet — that's fine
		return

	dir.list_dir_begin()
	var entry = dir.get_next()
	while entry != "":
		if dir.current_is_dir() and not entry.begins_with("."):
			var pkg_path = _resolved_dir.path_join(entry).path_join("package.json")
			var pkg = _load_package_json(pkg_path)
			if pkg.size() > 0:
				pkg["_dir"] = _resolved_dir.path_join(entry)
				pkg["_id"] = entry
				packages.append(pkg)
		entry = dir.get_next()
	dir.list_dir_end()


func _load_package_json(path: String) -> Dictionary:
	var file = FileAccess.open(path, FileAccess.READ)
	if not file:
		return {}

	var json = JSON.new()
	if json.parse(file.get_as_text()) != OK:
		push_warning("CustomSceneryLoader: Bad package.json at '%s'" % path)
		return {}
	file.close()

	var data = json.data
	if not data.has("name") or not data.has("type"):
		push_warning("CustomSceneryLoader: Missing name/type in '%s'" % path)
		return {}

	return data


# ── Queries ────────────────────────────────────────────────────────────

## Get all packages of a given type (e.g. "buildings", "objects")
func get_packages_by_type(type: String) -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	for pkg in packages:
		if pkg.get("type", "") == type:
			result.append(pkg)
	return result


## Get package by id (directory name)
func get_package(id: String) -> Dictionary:
	for pkg in packages:
		if pkg.get("_id", "") == id:
			return pkg
	return {}


# ── Path Resolution ────────────────────────────────────────────────────

static func _resolve_scenery_path(path: String) -> String:
	if path.begins_with("/") or path.begins_with("res://") or path.begins_with("user://"):
		return path
	var project_dir = ProjectSettings.globalize_path("res://")
	if project_dir.ends_with("/"):
		project_dir = project_dir.left(project_dir.length() - 1)
	var workspace_root = project_dir.get_base_dir()
	return workspace_root.path_join(path)
