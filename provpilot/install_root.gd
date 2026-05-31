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

# [aircraft] section — tracks first-run extraction of bundled aircraft data.
const SEC_AIRCRAFT := "aircraft"
const KEY_AIRCRAFT_INITIALIZED := "initialized"
const KEY_AIRCRAFT_VERSION := "version"
const AIRCRAFT_SUBDIR := "aircraft"

var _install_root: String = ""
var _config_path: String = ""
var _config: ConfigFile = ConfigFile.new()
var _loaded: bool = false


func _ready() -> void:
	_discover_and_load()
	_migrate_legacy_user_configs()


# ── Discovery ──────────────────────────────────────────────────────────

func _discover_and_load() -> void:
	var candidates: Array[String] = _candidate_roots()
	# Portable-install rule: in shipped (non-editor) builds the directory
	# next to the executable IS the install root, even if no provpilot.cfg
	# has been written there yet. This way the user can move the folder
	# (e.g. C:\Users\me\ProvPilot → I:\ProvPilot) and the app immediately
	# follows, instead of latching onto a stale provpilot.cfg in the legacy
	# ~/ProvPilot location.
	if not OS.has_feature("editor") and not candidates.is_empty() and _is_writable(candidates[0]):
		_install_root = candidates[0]
		_config_path = _install_root.path_join(CONFIG_FILENAME)
	else:
		# Editor / dev fallback: pick the first candidate that already has
		# a config file, otherwise the first writable candidate.
		for c in candidates:
			var p := c.path_join(CONFIG_FILENAME)
			if FileAccess.file_exists(p):
				_install_root = c
				_config_path = p
				break
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

	# One-time migration: if the chosen install root has no provpilot.cfg
	# yet but the legacy ~/ProvPilot location does, copy that cfg in so
	# the user keeps their scenery root, joystick bindings, etc.
	_migrate_legacy_install_cfg(candidates)

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
	var exec := OS.get_executable_path()
	# 1a) macOS bundle parent: strip /<exe>, /MacOS, /Contents, /<bundle>.app
	if exec.find(".app/Contents/MacOS/") != -1:
		var bundle_dir := exec.get_base_dir().get_base_dir().get_base_dir()
		var parent := bundle_dir.get_base_dir()
		if not parent.is_empty():
			out.append(parent)
	# 1b) Windows / Linux: the executable sits directly in the install root,
	#     so its parent directory is the install root. Skip when inside a
	#     macOS .app bundle (already handled above) or when running in-editor.
	elif not exec.is_empty() and not OS.has_feature("editor"):
		var exec_dir := exec.get_base_dir()
		if not exec_dir.is_empty() and not out.has(exec_dir):
			out.append(exec_dir)
	# 2) $PWD (terminal launch dir — set on Linux/macOS but not on Windows CMD)
	var pwd := OS.get_environment("PWD")
	if not pwd.is_empty() and DirAccess.dir_exists_absolute(pwd):
		if not out.has(pwd):
			out.append(pwd)
	# 3) ~/ProvPilot — friendly default.
	#    Windows uses USERPROFILE instead of HOME; check both.
	var home := OS.get_environment("HOME")
	if home.is_empty():
		home = OS.get_environment("USERPROFILE")
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


# ── Aircraft ────────────────────────────────────────────────────────────
## Path to the writable per-airframe data tree (X-Plane style):
##   <install_root>/aircraft/<id>/jsbsim/{aircraft,engine,systems}/...
## Created on first run by `aircraft_installer.gd`.
func get_aircraft_root() -> String:
	return _install_root.path_join(AIRCRAFT_SUBDIR)


func get_aircraft_dir(aircraft_id: String) -> String:
	return get_aircraft_root().path_join(aircraft_id)


## Returns true when the bundled aircraft tree has already been extracted to
## the writable install location for the given `version`. Bumping the version
## string forces a re-extraction on the next launch.
func is_aircraft_initialized(version: String) -> bool:
	if not bool(_config.get_value(SEC_AIRCRAFT, KEY_AIRCRAFT_INITIALIZED, false)):
		return false
	var stored := str(_config.get_value(SEC_AIRCRAFT, KEY_AIRCRAFT_VERSION, ""))
	return stored == version


func mark_aircraft_initialized(version: String) -> Error:
	_config.set_value(SEC_AIRCRAFT, KEY_AIRCRAFT_INITIALIZED, true)
	_config.set_value(SEC_AIRCRAFT, KEY_AIRCRAFT_VERSION, version)
	var err := _config.save(_config_path)
	if err == OK:
		_loaded = true
		print("InstallRoot: marked aircraft initialized (version=%s)" % version)
	else:
		push_error("InstallRoot: failed to save %s (err=%d)" % [_config_path, err])
	return err


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


# ── Generic config API ─────────────────────────────────────────────────
## Read a value from the shared provpilot.cfg. All app config (scenery,
## aircraft, joystick bindings, user settings) lives in this single file.
func get_value(section: String, key: String, default: Variant = null) -> Variant:
	return _config.get_value(section, key, default)


## Stage a value into the in-memory ConfigFile. Call `save_config()` to
## flush to disk.
func set_value(section: String, key: String, value: Variant) -> void:
	_config.set_value(section, key, value)


func has_section(section: String) -> bool:
	return _config.has_section(section)


func get_sections() -> PackedStringArray:
	return _config.get_sections()


func get_section_keys(section: String) -> PackedStringArray:
	if not _config.has_section(section):
		return PackedStringArray()
	return _config.get_section_keys(section)


func erase_section(section: String) -> void:
	if _config.has_section(section):
		_config.erase_section(section)


## Persist the in-memory config to provpilot.cfg.
func save_config() -> Error:
	var err := _config.save(_config_path)
	if err == OK:
		_loaded = true
	else:
		push_error("InstallRoot: failed to save %s (err=%d)" % [_config_path, err])
	return err


# ── One-time migration from a legacy install-root cfg ─────────────────
## When the user moves a portable install (e.g. C:\Users\me\ProvPilot →
## I:\ProvPilot) the executable's new directory is empty and the old
## ~/ProvPilot folder still has a stale provpilot.cfg. We adopt the new
## exec_dir as the install root (see _discover_and_load) and, if no cfg
## exists there yet, copy the latest legacy cfg in so the user keeps
## their scenery root, joystick bindings, etc.
func _migrate_legacy_install_cfg(candidates: Array[String]) -> void:
	if FileAccess.file_exists(_config_path):
		return  # current install root already has its own cfg
	for c in candidates:
		if c == _install_root:
			continue
		var legacy := c.path_join(CONFIG_FILENAME)
		if not FileAccess.file_exists(legacy):
			continue
		# Copy bytes verbatim — preserves comments, ordering, and any
		# sections we don't know about.
		var src := FileAccess.open(legacy, FileAccess.READ)
		if src == null:
			continue
		var data := src.get_buffer(src.get_length())
		src.close()
		var dst := FileAccess.open(_config_path, FileAccess.WRITE)
		if dst == null:
			push_warning("InstallRoot: cannot write %s — legacy cfg %s NOT migrated" % [_config_path, legacy])
			return
		dst.store_buffer(data)
		dst.close()
		print("InstallRoot: migrated legacy install cfg %s → %s" % [legacy, _config_path])
		return


# ── One-time migration from user://*.cfg ───────────────────────────────
## Earlier builds wrote joystick bindings and user settings to separate
## ConfigFiles under user:// (e.g. ~/Library/Application Support/Godot/…).
## On first launch with the consolidated layout, copy any pre-existing
## values into provpilot.cfg so the user doesn't lose their setup. We never
## delete the legacy files — leaving them in place is harmless and reversible.
func _migrate_legacy_user_configs() -> void:
	var migrated := false
	if not _config.has_section("device") \
			and not _has_any_section_with_prefix("axis.") \
			and not _has_any_section_with_prefix("button."):
		migrated = _import_legacy_cfg("user://joystick.cfg") or migrated
	if not _config.has_section("graphics") \
			and not _config.has_section("sound") \
			and not _config.has_section("realism"):
		migrated = _import_legacy_cfg("user://settings.cfg") or migrated
	if migrated:
		var err := _config.save(_config_path)
		if err == OK:
			_loaded = true
			print("InstallRoot: migrated legacy user://*.cfg → %s" % _config_path)
		else:
			push_warning("InstallRoot: legacy migration save failed (err=%d)" % err)


func _has_any_section_with_prefix(prefix: String) -> bool:
	for s in _config.get_sections():
		if String(s).begins_with(prefix):
			return true
	return false


## Merge keys from a legacy ConfigFile into our shared config. Existing
## keys in `_config` win (no overwrite). Returns true if anything was copied.
func _import_legacy_cfg(legacy_path: String) -> bool:
	if not FileAccess.file_exists(legacy_path):
		return false
	var cfg := ConfigFile.new()
	if cfg.load(legacy_path) != OK:
		return false
	var any := false
	for section in cfg.get_sections():
		for key in cfg.get_section_keys(section):
			if _config.has_section_key(section, key):
				continue
			_config.set_value(section, key, cfg.get_value(section, key))
			any = true
	if any:
		print("InstallRoot: imported %s" % legacy_path)
	return any

