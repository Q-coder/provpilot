extends RefCounted
## Parser for ProvPilot `aircraft.cfg` files.
##
## The format is a lightweight INI-like dialect (NOT Godot's ConfigFile):
##
##     [section]
##         key "quoted string value"
##         key bare_value          # numbers, true/false
##         key "a, b, c"           # comma list (read via get_*_array)
##         # full-line or trailing comment
##
## Use [method load_from_file] to obtain an instance, then the typed getters
## ([method get_string], [method get_float], …) to read values. Every getter
## takes a default so callers degrade gracefully when a key is missing.
##
## This keeps the engine code (node_3d.gd, the loaders, JSBSim glue) free of
## hardcoded, per-airframe paths and constants — each aircraft ships its own
## `aircraft.cfg` describing where to find what.

## Parsed data: { section_name: { key: String } }. All values are stored as
## raw strings; the typed getters convert on read.
var sections: Dictionary = {}

## Absolute path the config was loaded from ("" if built in-memory).
var source_path: String = ""

## True when [method load_from_file] (or [method parse_text]) succeeded.
var loaded: bool = false

# Self-reference so the static factory can construct instances without a
# registered global class_name (consumers preload this script as a const).
const _Self = preload("res://aircraft_config.gd")


## Loads and parses the file at [param path] (absolute or res://). Returns an
## `AircraftConfig` whose `loaded` flag reflects success; on failure the
## instance is still usable (every getter just returns its default).
static func load_from_file(path: String) -> RefCounted:
	var cfg := _Self.new()
	cfg.source_path = path
	if not FileAccess.file_exists(path):
		push_warning("AircraftConfig: file not found: %s" % path)
		return cfg
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		push_warning("AircraftConfig: cannot open %s (err=%d)" % [path, FileAccess.get_open_error()])
		return cfg
	cfg.parse_text(f.get_as_text())
	cfg.loaded = true
	return cfg


## Parses raw config [param text] into [member sections]. Exposed mainly for
## testing; [method load_from_file] is the normal entry point.
func parse_text(text: String) -> void:
	sections.clear()
	var current := ""
	for raw in text.split("\n"):
		var line := raw.strip_edges()
		if line.is_empty() or line.begins_with("#"):
			continue
		if line.begins_with("[") and line.ends_with("]"):
			current = line.substr(1, line.length() - 2).strip_edges()
			if not sections.has(current):
				sections[current] = {}
			continue
		if current.is_empty():
			continue  # key before any section header — ignore
		var sep := _first_whitespace(line)
		if sep < 0:
			continue  # bare token with no value — ignore
		var key := line.substr(0, sep).strip_edges()
		var value := _parse_value(line.substr(sep).strip_edges())
		if not key.is_empty():
			sections[current][key] = value


## Returns true when [param section]/[param key] exists.
func has_value(section: String, key: String) -> bool:
	return sections.has(section) and sections[section].has(key)


## Returns the raw string value, or [param default] if absent.
func get_string(section: String, key: String, default: String = "") -> String:
	if has_value(section, key):
		return sections[section][key]
	return default


func get_float(section: String, key: String, default: float = 0.0) -> float:
	if has_value(section, key):
		return sections[section][key].to_float()
	return default


func get_int(section: String, key: String, default: int = 0) -> int:
	if has_value(section, key):
		return sections[section][key].to_int()
	return default


## Reads a boolean. Accepts true/1/yes/on (case-insensitive) as true.
func get_bool(section: String, key: String, default: bool = false) -> bool:
	if not has_value(section, key):
		return default
	var v := String(sections[section][key]).strip_edges().to_lower()
	return v == "true" or v == "1" or v == "yes" or v == "on"


## Reads a comma-separated list as floats.
func get_float_array(section: String, key: String, default: Array = []) -> Array:
	if not has_value(section, key):
		return default
	var out: Array = []
	for part in String(sections[section][key]).split(",", false):
		out.append(part.strip_edges().to_float())
	return out


## Reads a comma-separated list as trimmed strings.
func get_string_array(section: String, key: String, default: Array = []) -> Array:
	if not has_value(section, key):
		return default
	var out: Array = []
	for part in String(sections[section][key]).split(",", false):
		out.append(part.strip_edges())
	return out


## Returns the whole section as a { key: String } dictionary (empty if absent).
func get_section(section: String) -> Dictionary:
	return sections.get(section, {})


# ── internals ──────────────────────────────────────────────────────────────

# Index of the first space or tab in [param s], or -1 if none.
func _first_whitespace(s: String) -> int:
	var sp := s.find(" ")
	var tab := s.find("\t")
	if sp < 0:
		return tab
	if tab < 0:
		return sp
	return mini(sp, tab)


# Extracts the value portion of a line. Quoted values keep their inner text
# verbatim; bare values are taken up to the first '#' comment marker.
func _parse_value(rest: String) -> String:
	if rest.begins_with("\""):
		var end := rest.find("\"", 1)
		if end > 0:
			return rest.substr(1, end - 1)
		return rest.substr(1)  # unterminated quote — take the remainder
	var hash_idx := rest.find("#")
	if hash_idx >= 0:
		rest = rest.substr(0, hash_idx)
	return rest.strip_edges()
