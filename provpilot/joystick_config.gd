extends Node
## JoystickConfig — Autoloaded singleton that owns all joystick/gamepad bindings
## and calibration data, plus the runtime accessors used by gameplay code.
##
## Why this exists: every controller maps stick directions, half-axes and
## button indices differently. The Logitech Dual Action's right-Y, for
## example, is a half-axis with no "down" travel. Hard-coding axis indices
## and signs in gameplay code is fragile, so the read sites call
## `JoystickConfig.get_axis("slew_vertical")` and the user picks an axis +
## calibrates it from the Settings screen.
##
## Bindings persist via the shared InstallRoot config (provpilot.cfg next to
## the .app). Sections used: "device", "axis.<action>", "button.<action>".

# Legacy stand-alone path; kept as a fallback for read-only migration if
# InstallRoot is not yet present at autoload-init time. Writes always go
# through InstallRoot.
const LEGACY_CONFIG_PATH := "user://joystick.cfg"

# ── Action catalog ─────────────────────────────────────────────────────
# Every joystick-readable action gameplay code can request. Keep this list
# the single source of truth — the settings UI and read-site call sites
# both reference it.

const AXIS_ACTIONS := [
	# Slew (free-camera) controls
	{"id": "slew_strafe",       "label": "Slew: Strafe (X)"},
	{"id": "slew_forward",      "label": "Slew: Forward/Back"},
	{"id": "slew_yaw",          "label": "Slew: Yaw"},
	{"id": "slew_vertical",     "label": "Slew: Up/Down"},
	# Flight controls (reserved for future use; read sites can opt in)
	{"id": "flight_roll",       "label": "Flight: Roll (aileron)"},
	{"id": "flight_pitch",      "label": "Flight: Pitch (elevator)"},
	{"id": "flight_yaw",        "label": "Flight: Yaw (rudder)"},
	{"id": "flight_throttle",   "label": "Flight: Throttle"},
	# Camera look
	{"id": "look_x",            "label": "Camera: Look X"},
	{"id": "look_y",            "label": "Camera: Look Y"},
]

const BUTTON_ACTIONS := [
	{"id": "flip_camera",   "label": "Cycle camera"},
	{"id": "slew_toggle",   "label": "Toggle slew mode"},
	{"id": "flaps_up",      "label": "Flaps up a notch"},
	{"id": "flaps_down",    "label": "Flaps down a notch"},
	{"id": "trim_up",       "label": "Pitch trim up"},
	{"id": "trim_down",     "label": "Pitch trim down"},
	{"id": "brakes",        "label": "Hold brakes"},
	{"id": "look_left",     "label": "Look left"},
	{"id": "look_right",    "label": "Look right"},
	{"id": "look_up",       "label": "Look up"},
	{"id": "look_down",     "label": "Look down"},
]

# ── Curve types ────────────────────────────────────────────────────────
enum AxisCurve { LINEAR = 0, SQUARED = 1, CUBED = 2 }

# ── Binding state ──────────────────────────────────────────────────────
# axis_bindings[action_id] = {
#     axis: int  (>=0 to bind, -1 = unbound)
#     invert: bool
#     min: float    raw value at "fully negative" stop (e.g. -1.0)
#     center: float raw value at rest (typically 0.0)
#     max: float    raw value at "fully positive" stop (e.g. +1.0)
#     deadzone: float (0..1, applied after centering)
#     curve: int (Curve enum)
# }
var axis_bindings: Dictionary = {}

# button_bindings[action_id] = { button: int (>=0 = bound, -1 = unbound) }
var button_bindings: Dictionary = {}

# Pad device id we read from. -1 = first connected pad (auto).
var device_id: int = -1


func _ready() -> void:
	_apply_defaults()
	_load()


# ── Defaults (sensible mapping for a standard SDL gamepad) ─────────────

func _apply_defaults() -> void:
	axis_bindings = {
		"slew_strafe":     _axis_default(JOY_AXIS_LEFT_X,  false),
		"slew_forward":    _axis_default(JOY_AXIS_LEFT_Y,  false),
		"slew_yaw":        _axis_default(JOY_AXIS_RIGHT_X, false),
		"slew_vertical":   _axis_default(JOY_AXIS_RIGHT_Y, true),  # up = climb
		"flight_roll":     _axis_default(JOY_AXIS_LEFT_X,  false),
		"flight_pitch":    _axis_default(JOY_AXIS_LEFT_Y,  true),  # pull-back = pitch up
		"flight_yaw":      _axis_default(JOY_AXIS_RIGHT_X, false),
		"flight_throttle": _axis_default(JOY_AXIS_TRIGGER_RIGHT, false),
		"look_x":          _axis_unbound(),
		"look_y":          _axis_unbound(),
	}
	button_bindings = {
		"flip_camera": {"button": JOY_BUTTON_Y},
		"slew_toggle": {"button": -1},
		"flaps_up":    {"button": JOY_BUTTON_RIGHT_SHOULDER},
		"flaps_down":  {"button": JOY_BUTTON_LEFT_SHOULDER},
		"trim_up":     {"button": JOY_BUTTON_DPAD_UP},
		"trim_down":   {"button": JOY_BUTTON_DPAD_DOWN},
		"brakes":      {"button": JOY_BUTTON_A},
		"look_left":   {"button": JOY_BUTTON_DPAD_LEFT},
		"look_right":  {"button": JOY_BUTTON_DPAD_RIGHT},
		"look_up":     {"button": JOY_BUTTON_DPAD_UP},
		"look_down":   {"button": JOY_BUTTON_DPAD_DOWN},
	}


static func _axis_default(axis: int, invert: bool) -> Dictionary:
	return {
		"axis": axis,
		"invert": invert,
		"min": -1.0,
		"center": 0.0,
		"max": 1.0,
		"deadzone": 0.15,
		"curve": AxisCurve.LINEAR,
	}


static func _axis_unbound() -> Dictionary:
	return {"axis": -1, "invert": false, "min": -1.0, "center": 0.0, "max": 1.0, "deadzone": 0.15, "curve": AxisCurve.LINEAR}


# ── Public API ─────────────────────────────────────────────────────────

## Return the active pad device id, or -1 if no pad is connected.
func get_active_device() -> int:
	if device_id >= 0:
		return device_id
	var pads := Input.get_connected_joypads()
	if pads.is_empty():
		return -1
	return pads[0]


## Read the raw axis value for a binding without any calibration applied.
## Used by the settings UI for live preview.
func get_raw_axis(action: String) -> float:
	var b: Dictionary = axis_bindings.get(action, {})
	if b.is_empty() or int(b.get("axis", -1)) < 0:
		return 0.0
	var dev := get_active_device()
	if dev < 0:
		return 0.0
	return Input.get_joy_axis(dev, int(b["axis"]))


## Read a calibrated axis in [-1, 1]. Applies center/min/max remap, deadzone,
## invert and response curve. Returns 0.0 if unbound or pad disconnected.
func get_axis(action: String) -> float:
	var b: Dictionary = axis_bindings.get(action, {})
	if b.is_empty() or int(b.get("axis", -1)) < 0:
		return 0.0
	var dev := get_active_device()
	if dev < 0:
		return 0.0
	var raw := Input.get_joy_axis(dev, int(b["axis"]))
	return _apply_calibration(raw, b)


static func _apply_calibration(raw: float, b: Dictionary) -> float:
	var center := float(b.get("center", 0.0))
	var lo := float(b.get("min", -1.0))
	var hi := float(b.get("max", 1.0))
	var dz := float(b.get("deadzone", 0.15))
	var curve := int(b.get("curve", AxisCurve.LINEAR))
	var invert := bool(b.get("invert", false))

	# Map to symmetric [-1, 1] using the calibration extremes.
	var v: float
	if raw >= center:
		var span := maxf(hi - center, 0.000001)
		v = clampf((raw - center) / span, 0.0, 1.0)
	else:
		var span2 := maxf(center - lo, 0.000001)
		v = -clampf((center - raw) / span2, 0.0, 1.0)

	# Deadzone with linear rescale outside it.
	if absf(v) < dz:
		v = 0.0
	elif dz < 1.0:
		v = signf(v) * (absf(v) - dz) / (1.0 - dz)

	# Response curve (preserve sign).
	if curve == AxisCurve.SQUARED:
		v = signf(v) * v * v
	elif curve == AxisCurve.CUBED:
		v = v * v * v

	if invert:
		v = -v
	return v


## Read a button binding. Returns false if unbound or pad disconnected.
func get_button(action: String) -> bool:
	var b: Dictionary = button_bindings.get(action, {})
	if b.is_empty() or int(b.get("button", -1)) < 0:
		return false
	var dev := get_active_device()
	if dev < 0:
		return false
	return Input.is_joy_button_pressed(dev, int(b["button"]))


# ── Detection helpers (for settings UI) ────────────────────────────────

## Scan all axes on the active pad and return the index of the one whose
## absolute value moved farthest from 0; -1 if nothing exceeds threshold.
func detect_active_axis(threshold: float = 0.5) -> int:
	var dev := get_active_device()
	if dev < 0:
		return -1
	var best_axis := -1
	var best_val := threshold
	for a in range(JOY_AXIS_MAX):
		var v := absf(Input.get_joy_axis(dev, a))
		if v > best_val:
			best_val = v
			best_axis = a
	return best_axis


## Return index of the first pressed button on the active pad, or -1.
func detect_active_button() -> int:
	var dev := get_active_device()
	if dev < 0:
		return -1
	for b in range(JOY_BUTTON_MAX):
		if Input.is_joy_button_pressed(dev, b):
			return b
	return -1


# ── Persistence ────────────────────────────────────────────────────────

func save() -> void:
	var ir := _install_root()
	if ir == null:
		push_warning("JoystickConfig: InstallRoot not available, save skipped")
		return
	# Wipe stale axis.*/button.* sections so removed bindings don't linger.
	for s in ir.get_sections():
		var sn := String(s)
		if sn.begins_with("axis.") or sn.begins_with("button."):
			ir.erase_section(sn)
	ir.set_value("device", "id", device_id)
	for action in axis_bindings.keys():
		var b: Dictionary = axis_bindings[action]
		for k in b.keys():
			ir.set_value("axis." + action, k, b[k])
	for action in button_bindings.keys():
		var b: Dictionary = button_bindings[action]
		ir.set_value("button." + action, "button", int(b.get("button", -1)))
	var err: int = ir.save_config()
	if err == OK:
		print("JoystickConfig: saved to provpilot.cfg")
	else:
		push_warning("JoystickConfig: save failed (err %d)" % err)


func _load() -> void:
	var ir := _install_root()
	var sections: Array = []
	var source := ""
	if ir != null:
		sections = Array(ir.get_sections())
		if not sections.is_empty():
			source = "provpilot.cfg"
	# Fallback: read straight from legacy user://joystick.cfg if InstallRoot
	# doesn't have any joystick sections yet (e.g. fresh install that has
	# never run with the migration code).
	var legacy_cfg: ConfigFile = null
	var have_joy_in_ir := false
	if ir != null:
		if ir.has_section("device") \
				or _any_section_with_prefix(sections, "axis.") \
				or _any_section_with_prefix(sections, "button."):
			have_joy_in_ir = true
	if not have_joy_in_ir:
		if FileAccess.file_exists(LEGACY_CONFIG_PATH):
			legacy_cfg = ConfigFile.new()
			if legacy_cfg.load(LEGACY_CONFIG_PATH) == OK:
				sections = Array(legacy_cfg.get_sections())
				source = LEGACY_CONFIG_PATH
			else:
				legacy_cfg = null
	if sections.is_empty():
		print("JoystickConfig: no saved config, using defaults")
		return
	var read_value := func(section: String, key: String, default: Variant) -> Variant:
		if legacy_cfg != null:
			return legacy_cfg.get_value(section, key, default)
		return ir.get_value(section, key, default)
	if "device" in sections:
		device_id = int(read_value.call("device", "id", -1))
	for section in sections:
		var sn := String(section)
		if sn.begins_with("axis."):
			var action := sn.substr(5)
			if not axis_bindings.has(action):
				continue
			var b: Dictionary = axis_bindings[action]
			var keys: PackedStringArray
			if legacy_cfg != null:
				keys = legacy_cfg.get_section_keys(sn)
			else:
				keys = ir.get_section_keys(sn)
			for k in keys:
				b[k] = read_value.call(sn, k, b.get(k))
			axis_bindings[action] = b
		elif sn.begins_with("button."):
			var action2 := sn.substr(7)
			if not button_bindings.has(action2):
				continue
			button_bindings[action2] = {"button": int(read_value.call(sn, "button", -1))}
	print("JoystickConfig: loaded from %s" % source)


func _install_root() -> Node:
	var ml := Engine.get_main_loop()
	if ml is SceneTree:
		var st: SceneTree = ml
		if st.root:
			return st.root.get_node_or_null(^"InstallRoot")
	return null


static func _any_section_with_prefix(sections: Array, prefix: String) -> bool:
	for s in sections:
		if String(s).begins_with(prefix):
			return true
	return false


func reset_to_defaults() -> void:
	_apply_defaults()
