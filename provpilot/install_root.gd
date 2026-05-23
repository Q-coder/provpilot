extends Node
## InstallRoot — Discovers where ProvPilot is installed and where its scenery
## library lives. Registered as a project autoload so any script can call
## `InstallRoot.get_install_root()` etc.
##
## ProvPilot follows the X-Plane "portable install" model: the user copies the
## .app into a folder of their choice (anywhere on their disk), then on first
## launch a wizard asks for the scenery location. The result is persisted to
## `<install_root>/provpilot.cfg` so the install is self-contained and can be
## moved to another machine simply by copying the folder.
##
## ── Discovery order (first hit wins) ───────────────────────────────────
##   1. Directory containing the .app bundle:
##        /…/ProVPilot.app/Contents/MacOS/ProVPilot
##                          ↑                  exec path
##        → install_root = parent of .app
##   2. $PWD — the working directory the user launched from (handy when
##      running the binary directly from a terminal).
##   3. ~/ProvPilot — common user-chosen location.
##   4. res:// — editor / dev fallback.
##
## The first candidate that contains a writable provpilot.cfg (or, if none
## exists, the first writable candidate) becomes the install root.

const CONFIG_FILENAME := "provpilot.cfg"

# Section / key constants kept in one place
const SEC_SCENERY := "scenery"
const KEY_SCENERY_ROOT := "scenery_root"
const KEY_REGIONS := "regions"           # PackedStringArray
const KEY_FIRST_RUN_DONE := "first_run_done"

var _install_root: String = ""
var _config_path: String = ""
var _config: ConfigFile = ConfigFile.new()
var _loaded: bool = false


func _ready() -> void:
	_discover_and_load()


# ── Discovery ──────────────────────────────────────────────────────────

func _discover_and_load() -> void:
	var candidates: Array[String] = _candidate_roots()
	# Pick the first candidate that already has a config file …
	for c in candidates:
		var p := c.path_join(CONFIG_FILENAME)
		if FileAccess.file_exists(p):
			_install_root = c
			_config_path = p
			break
	# … otherwise pick the first writable candidate for future writes.
	if _install_root.is_empty():
		for c in candidates:
			if _is_writable(c):
				_install_root = c
				_config_path = c.path_join(CONFIG_FILENAME)
				break
	# Last resort: user data dir (always writable).
	if _install_root.is_empty():
		_install_root = OS.get_user_data_dir()
		_config_path = _install_root.path_join(CONFIG_FILENAME)

	if FileAccess.file_exists(_config_path):
		var err := _config.load(_config_path)
		if err == OK:
			_loaded = true
			print("InstallRoot: loaded config %s" % _config_path)
		else:
			push_warning("InstallRoot: failed to load %s (err=%d)" % [_config_path, err])
	else:
		print("InstallRoot: no config at %s (first run)" % _config_path)
	print("InstallRoot: install_root=%s" % _install_root)


func _candidate_roots() -> Array[String]:
	var out: Array[String] = []
	# 1) bundle parent
	var exec := OS.get_executable_path()
	if exec.find(".app/Contents/MacOS/") != -1:
		# strip /<exe>, /MacOS, /Contents, then /<bundle>.app
		var bundle_dir := exec.get_base_dir().get_base_dir().get_base_dir()
		var parent := bundle_dir.get_base_dir()
		if not parent.is_empty():
			out.append(parent)
	# 2) $PWD (terminal launch dir)
	var pwd := OS.get_environment("PWD")
	if not pwd.is_empty() and DirAccess.dir_exists_absolute(pwd):
		if not out.has(pwd):
			out.append(pwd)
	# 3) ~/ProvPilot — friendly default
	var home := OS.get_environment("HOME")
	if not home.is_empty():
		var p := home.path_join("ProvPilot")
		if not out.has(p):
			out.append(p)
	# 4) res:// (editor / dev). globalize_path turns it into an OS path.
	if OS.has_feature("editor"):
		var res_dir := ProjectSettings.globalize_path("res://")
		if not out.has(res_dir):
			out.append(res_dir)
	return out


func _is_writable(path: String) -> bool:
	if not DirAccess.dir_exists_absolute(path):
		# Try to create the directory — that counts as writable.
		var err := DirAccess.make_dir_recursive_absolute(path)
		return err == OK
	# Probe with a small temp file
	var probe := path.path_join(".provpilot_write_probe")
	var f := FileAccess.open(probe, FileAccess.WRITE)
	if f == null:
		return false
	f.close()
	DirAccess.remove_absolute(probe)
	return true


# ── Public API ─────────────────────────────────────────────────────────

func get_install_root() -> String:
	return _install_root


func get_config_path() -> String:
	return _config_path


func has_config() -> bool:
	return _loaded


func is_first_run() -> bool:
	# True when there's no config OR config exists but first_run_done is false
	# AND no scenery root is configured (so a fresh user is still treated as
	# first-run even if config got created accidentally).
	if not _loaded:
		return true
	var done: bool = bool(_config.get_value(SEC_SCENERY, KEY_FIRST_RUN_DONE, false))
	if done:
		return false
	return get_scenery_root().is_empty()


## Returns the configured scenery root (path containing region subdirs like
## "Switzerland/", "Germany/"), or "" if not configured.
func get_scenery_root() -> String:
	return str(_config.get_value(SEC_SCENERY, KEY_SCENERY_ROOT, ""))


## Returns the list of enabled region subdirectory names, in priority order.
## Empty means "use everything the scan finds".
func get_regions() -> PackedStringArray:
	var raw = _config.get_value(SEC_SCENERY, KEY_REGIONS, PackedStringArray())
	if raw is PackedStringArray:
		return raw
	if raw is Array:
		var arr := PackedStringArray()
		for x in raw:
			arr.append(str(x))
		return arr
	return PackedStringArray()


## Default roots for the "Scan for Scenery" feature. Includes a few
## plausible locations so a fresh install doesn't show empty placeholders.
func get_default_scan_roots() -> PackedStringArray:
	var roots := PackedStringArray()
	var configured := get_scenery_root()
	if not configured.is_empty():
		roots.append(configured)
	# Next to the install
	roots.append(_install_root.path_join("scenery"))
	# Home dir candidates
	var home := OS.get_environment("HOME")
	if not home.is_empty():
		roots.append(home.path_join("ProvPilot/scenery"))
		roots.append(home.path_join("Documents/ProvPilot/scenery"))
	# External volumes — best-effort, just list /Volumes/*/scenery candidates
	var volumes := DirAccess.open("/Volumes")
	if volumes != null:
		volumes.list_dir_begin()
		var name := volumes.get_next()
		while name != "":
			if not name.begins_with(".") and volumes.current_is_dir():
				roots.append("/Volumes/%s/scenery" % name)
			name = volumes.get_next()
		volumes.list_dir_end()
	# De-duplicate while preserving order. macOS' default APFS volume is
	# case-insensitive, so `~/provpilot/scenery` and `~/ProvPilot/scenery`
	# refer to the same directory — collapse them to one entry to avoid the
	# scanner adding the same scenery area twice.
	var seen := {}
	var unique := PackedStringArray()
	for r in roots:
		var key := r.to_lower() if OS.get_name() == "macOS" or OS.get_name() == "Windows" else r
		if not seen.has(key):
			seen[key] = true
			unique.append(r)
	return unique


## Persist the user's first-run choice. Writes provpilot.cfg next to the .app
## (or wherever the discovered install root lives).
func save_scenery_choice(scenery_root: String, regions: PackedStringArray = PackedStringArray()) -> Error:
	_config.set_value(SEC_SCENERY, KEY_SCENERY_ROOT, scenery_root)
	if regions.size() > 0:
		_config.set_value(SEC_SCENERY, KEY_REGIONS, regions)
	_config.set_value(SEC_SCENERY, KEY_FIRST_RUN_DONE, true)
	var err := _config.save(_config_path)
	if err == OK:
		_loaded = true
		print("InstallRoot: saved %s" % _config_path)
	else:
		push_error("InstallRoot: failed to save %s (err=%d)" % [_config_path, err])
	return err
