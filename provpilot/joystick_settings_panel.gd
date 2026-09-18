extends VBoxContainer
## JoystickSettingsPanel — UI for binding & calibrating gamepad axes/buttons.
##
## Mounted as the Joystick tab of the C++ SettingsScreen. All read/write of bindings
## goes through the `JoystickConfig` autoload.

const FONT_SM := 18
const FONT_MD := 22
const FONT_LG := 26
const LABEL_COLOR := Color(0.75, 0.78, 0.82)
const SECTION_COLOR := Color(0.25, 0.55, 0.85, 1.0)
const ACCENT := Color(0.4, 0.85, 0.4)

# Per-row controls keyed by action id.
var _axis_rows: Dictionary = {}     # action -> { spin, detect_btn, raw_bar, cal_bar, invert, dz_slider, curve_opt }
var _button_rows: Dictionary = {}   # action -> { spin, detect_btn, pressed_lbl }

# Detection state: when set, the next significant axis/button input is captured
# for that action.
var _detecting_axis: String = ""
var _detecting_button: String = ""
var _detect_axis_baseline: PackedFloat32Array = PackedFloat32Array()


func _ready() -> void:
	add_theme_constant_override("separation", 6)
	_build_ui()
	_refresh_from_config()
	set_process(true)


# ── UI construction ────────────────────────────────────────────────────

func _build_ui() -> void:
	var header := Label.new()
	header.text = "Joystick / Gamepad"
	header.add_theme_font_size_override("font_size", FONT_LG)
	header.add_theme_color_override("font_color", SECTION_COLOR)
	add_child(header)

	# Device selector
	var dev_hbox := HBoxContainer.new()
	dev_hbox.add_theme_constant_override("separation", 12)
	add_child(dev_hbox)

	var dev_label := Label.new()
	dev_label.text = "Device:"
	dev_label.custom_minimum_size.x = 200
	dev_label.add_theme_font_size_override("font_size", FONT_MD)
	dev_label.add_theme_color_override("font_color", LABEL_COLOR)
	dev_hbox.add_child(dev_label)

	var dev_status := Label.new()
	dev_status.name = "DeviceStatus"
	dev_status.add_theme_font_size_override("font_size", FONT_SM)
	dev_status.add_theme_color_override("font_color", LABEL_COLOR)
	dev_hbox.add_child(dev_status)

	# Help line
	var help := Label.new()
	help.text = "  • Click Detect, then move the desired axis or press the desired button.\n  • Min/Center/Max capture the current raw value. Invert flips the direction."
	help.add_theme_font_size_override("font_size", FONT_SM)
	help.add_theme_color_override("font_color", Color(0.55, 0.6, 0.65))
	add_child(help)

	# Axis subsection
	var axis_header := Label.new()
	axis_header.text = "Axes"
	axis_header.add_theme_font_size_override("font_size", FONT_MD)
	axis_header.add_theme_color_override("font_color", SECTION_COLOR)
	add_child(axis_header)

	for entry in JoystickConfig.AXIS_ACTIONS:
		_build_axis_row(entry["id"], entry["label"])

	# Button subsection
	add_child(HSeparator.new())
	var btn_header := Label.new()
	btn_header.text = "Buttons"
	btn_header.add_theme_font_size_override("font_size", FONT_MD)
	btn_header.add_theme_color_override("font_color", SECTION_COLOR)
	add_child(btn_header)

	for entry in JoystickConfig.BUTTON_ACTIONS:
		_build_button_row(entry["id"], entry["label"])


func _build_axis_row(action_id: String, label_text: String) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	add_child(row)

	var lbl := Label.new()
	lbl.text = label_text
	lbl.custom_minimum_size.x = 220
	lbl.add_theme_font_size_override("font_size", FONT_SM)
	lbl.add_theme_color_override("font_color", LABEL_COLOR)
	row.add_child(lbl)

	var spin := SpinBox.new()
	spin.min_value = -1
	spin.max_value = JOY_AXIS_MAX - 1
	spin.step = 1
	spin.custom_minimum_size = Vector2(70, 32)
	row.add_child(spin)

	var detect := Button.new()
	detect.text = "Detect"
	detect.custom_minimum_size = Vector2(80, 32)
	detect.pressed.connect(_on_detect_axis.bind(action_id))
	row.add_child(detect)

	var raw_bar := ProgressBar.new()
	raw_bar.min_value = -1.0
	raw_bar.max_value = 1.0
	raw_bar.step = 0.001
	raw_bar.value = 0.0
	raw_bar.show_percentage = false
	raw_bar.custom_minimum_size = Vector2(120, 24)
	row.add_child(raw_bar)

	var cal_bar := ProgressBar.new()
	cal_bar.min_value = -1.0
	cal_bar.max_value = 1.0
	cal_bar.step = 0.001
	cal_bar.value = 0.0
	cal_bar.show_percentage = false
	cal_bar.custom_minimum_size = Vector2(120, 24)
	cal_bar.modulate = ACCENT
	row.add_child(cal_bar)

	var invert := CheckBox.new()
	invert.text = "Inv"
	invert.toggled.connect(_on_invert_toggled.bind(action_id))
	row.add_child(invert)

	var min_btn := Button.new()
	min_btn.text = "Min"
	min_btn.tooltip_text = "Capture current axis value as the negative endpoint"
	min_btn.custom_minimum_size = Vector2(56, 32)
	min_btn.pressed.connect(_on_capture_endpoint.bind(action_id, "min"))
	row.add_child(min_btn)

	var center_btn := Button.new()
	center_btn.text = "Ctr"
	center_btn.tooltip_text = "Capture current axis value as the rest position"
	center_btn.custom_minimum_size = Vector2(56, 32)
	center_btn.pressed.connect(_on_capture_endpoint.bind(action_id, "center"))
	row.add_child(center_btn)

	var max_btn := Button.new()
	max_btn.text = "Max"
	max_btn.tooltip_text = "Capture current axis value as the positive endpoint"
	max_btn.custom_minimum_size = Vector2(56, 32)
	max_btn.pressed.connect(_on_capture_endpoint.bind(action_id, "max"))
	row.add_child(max_btn)

	var dz := HSlider.new()
	dz.min_value = 0.0
	dz.max_value = 0.5
	dz.step = 0.01
	dz.custom_minimum_size = Vector2(120, 24)
	dz.tooltip_text = "Deadzone"
	dz.value_changed.connect(_on_deadzone_changed.bind(action_id))
	row.add_child(dz)

	var curve := OptionButton.new()
	curve.add_item("Linear", JoystickConfig.AxisCurve.LINEAR)
	curve.add_item("Squared", JoystickConfig.AxisCurve.SQUARED)
	curve.add_item("Cubed", JoystickConfig.AxisCurve.CUBED)
	curve.custom_minimum_size = Vector2(110, 32)
	curve.item_selected.connect(_on_curve_selected.bind(action_id))
	row.add_child(curve)

	spin.value_changed.connect(_on_axis_index_changed.bind(action_id))

	_axis_rows[action_id] = {
		"spin": spin, "detect_btn": detect,
		"raw_bar": raw_bar, "cal_bar": cal_bar,
		"invert": invert, "dz_slider": dz, "curve_opt": curve,
	}


func _build_button_row(action_id: String, label_text: String) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	add_child(row)

	var lbl := Label.new()
	lbl.text = label_text
	lbl.custom_minimum_size.x = 220
	lbl.add_theme_font_size_override("font_size", FONT_SM)
	lbl.add_theme_color_override("font_color", LABEL_COLOR)
	row.add_child(lbl)

	var spin := SpinBox.new()
	spin.min_value = -1
	spin.max_value = JOY_BUTTON_MAX - 1
	spin.step = 1
	spin.custom_minimum_size = Vector2(70, 32)
	spin.value_changed.connect(_on_button_index_changed.bind(action_id))
	row.add_child(spin)

	var detect := Button.new()
	detect.text = "Detect"
	detect.custom_minimum_size = Vector2(80, 32)
	detect.pressed.connect(_on_detect_button.bind(action_id))
	row.add_child(detect)

	var pressed_lbl := Label.new()
	pressed_lbl.text = "·"
	pressed_lbl.custom_minimum_size = Vector2(40, 24)
	pressed_lbl.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	pressed_lbl.add_theme_font_size_override("font_size", FONT_MD)
	row.add_child(pressed_lbl)

	_button_rows[action_id] = {
		"spin": spin, "detect_btn": detect, "pressed_lbl": pressed_lbl,
	}


# ── Refresh from config ────────────────────────────────────────────────

func _refresh_from_config() -> void:
	for action in _axis_rows.keys():
		var b: Dictionary = JoystickConfig.axis_bindings.get(action, {})
		var row: Dictionary = _axis_rows[action]
		row["spin"].set_value_no_signal(int(b.get("axis", -1)))
		row["invert"].set_pressed_no_signal(bool(b.get("invert", false)))
		row["dz_slider"].set_value_no_signal(float(b.get("deadzone", 0.15)))
		row["curve_opt"].select(int(b.get("curve", 0)))

	for action in _button_rows.keys():
		var b2: Dictionary = JoystickConfig.button_bindings.get(action, {})
		_button_rows[action]["spin"].set_value_no_signal(int(b2.get("button", -1)))


# ── Live bars + detection ──────────────────────────────────────────────

func _process(_delta: float) -> void:
	var dev := JoystickConfig.get_active_device()
	var status: Label = get_node("HBoxContainer/DeviceStatus") if has_node("HBoxContainer/DeviceStatus") else null
	# Use a stable lookup since the HBox name isn't fixed:
	if status == null:
		# Fallback: find_child returns the first Label named DeviceStatus.
		var n := find_child("DeviceStatus", true, false)
		if n is Label:
			status = n
	if status:
		if dev < 0:
			status.text = "(no controller connected)"
		else:
			status.text = "%s — pad %d" % [Input.get_joy_name(dev), dev]

	# Update axis bars + handle detection
	for action in _axis_rows.keys():
		var row: Dictionary = _axis_rows[action]
		var raw := JoystickConfig.get_raw_axis(action)
		var cal := JoystickConfig.get_axis(action)
		row["raw_bar"].value = raw
		row["cal_bar"].value = cal

	if _detecting_axis != "":
		_poll_axis_detection()
	if _detecting_button != "":
		_poll_button_detection()

	# Update button "pressed" indicator
	for action in _button_rows.keys():
		var pressed := JoystickConfig.get_button(action)
		_button_rows[action]["pressed_lbl"].text = "●" if pressed else "·"
		_button_rows[action]["pressed_lbl"].add_theme_color_override(
			"font_color", ACCENT if pressed else LABEL_COLOR)


func _poll_axis_detection() -> void:
	var dev := JoystickConfig.get_active_device()
	if dev < 0:
		return
	if _detect_axis_baseline.size() < JOY_AXIS_MAX:
		_detect_axis_baseline.resize(JOY_AXIS_MAX)
		for a in range(JOY_AXIS_MAX):
			_detect_axis_baseline[a] = Input.get_joy_axis(dev, a)
		return
	for a in range(JOY_AXIS_MAX):
		var v := Input.get_joy_axis(dev, a)
		if absf(v - _detect_axis_baseline[a]) > 0.5:
			_assign_axis(_detecting_axis, a)
			_axis_rows[_detecting_axis]["detect_btn"].text = "Detect"
			_detecting_axis = ""
			_detect_axis_baseline.clear()
			return


func _poll_button_detection() -> void:
	var dev := JoystickConfig.get_active_device()
	if dev < 0:
		return
	for b in range(JOY_BUTTON_MAX):
		if Input.is_joy_button_pressed(dev, b):
			_assign_button(_detecting_button, b)
			_button_rows[_detecting_button]["detect_btn"].text = "Detect"
			_detecting_button = ""
			return


# ── Signal handlers ────────────────────────────────────────────────────

func _on_detect_axis(action: String) -> void:
	# Cancel any other detection in flight
	if _detecting_axis != "" and _detecting_axis != action:
		_axis_rows[_detecting_axis]["detect_btn"].text = "Detect"
	if _detecting_button != "":
		_button_rows[_detecting_button]["detect_btn"].text = "Detect"
		_detecting_button = ""
	_detecting_axis = action
	_detect_axis_baseline.clear()
	_axis_rows[action]["detect_btn"].text = "…move"


func _on_detect_button(action: String) -> void:
	if _detecting_button != "" and _detecting_button != action:
		_button_rows[_detecting_button]["detect_btn"].text = "Detect"
	if _detecting_axis != "":
		_axis_rows[_detecting_axis]["detect_btn"].text = "Detect"
		_detecting_axis = ""
	_detecting_button = action
	_button_rows[action]["detect_btn"].text = "…press"


func _on_axis_index_changed(value: float, action: String) -> void:
	_assign_axis(action, int(value))


func _on_button_index_changed(value: float, action: String) -> void:
	_assign_button(action, int(value))


func _on_invert_toggled(pressed: bool, action: String) -> void:
	var b: Dictionary = JoystickConfig.axis_bindings.get(action, {})
	b["invert"] = pressed
	JoystickConfig.axis_bindings[action] = b


func _on_deadzone_changed(value: float, action: String) -> void:
	var b: Dictionary = JoystickConfig.axis_bindings.get(action, {})
	b["deadzone"] = value
	JoystickConfig.axis_bindings[action] = b


func _on_curve_selected(idx: int, action: String) -> void:
	var b: Dictionary = JoystickConfig.axis_bindings.get(action, {})
	b["curve"] = idx
	JoystickConfig.axis_bindings[action] = b


func _on_capture_endpoint(action: String, which: String) -> void:
	var dev := JoystickConfig.get_active_device()
	if dev < 0:
		return
	var b: Dictionary = JoystickConfig.axis_bindings.get(action, {})
	var axis_idx := int(b.get("axis", -1))
	if axis_idx < 0:
		return
	var raw := Input.get_joy_axis(dev, axis_idx)
	b[which] = raw
	JoystickConfig.axis_bindings[action] = b


func _assign_axis(action: String, axis_idx: int) -> void:
	var b: Dictionary = JoystickConfig.axis_bindings.get(action, JoystickConfig._axis_default(axis_idx, false))
	b["axis"] = axis_idx
	JoystickConfig.axis_bindings[action] = b
	_axis_rows[action]["spin"].set_value_no_signal(axis_idx)


func _assign_button(action: String, button_idx: int) -> void:
	JoystickConfig.button_bindings[action] = {"button": button_idx}
	_button_rows[action]["spin"].set_value_no_signal(button_idx)


# Public: re-read state from JoystickConfig (after Restore Defaults / Cancel).
func refresh() -> void:
	_refresh_from_config()
