extends Window
## Position Dialog — Teleport aircraft to Swiss LV95 coordinates or saved locations.
##
## Opened with Shift+P. Provides:
##  - Manual Easting/Northing/Heading entry
##  - Dropdown of saved locations (POI airports)
##  - Current position display
##
## Emits `teleport_requested(swiss_e, swiss_n, heading_deg)` when confirmed.

signal teleport_requested(swiss_e: float, swiss_n: float, heading_deg: float)

# UI nodes (created in _ready)
var _easting_input: LineEdit
var _northing_input: LineEdit
var _heading_input: LineEdit
var _location_picker: OptionButton
var _current_pos_label: Label
var _status_label: Label

# Airport data from POILoader
var _airports: Array[Dictionary] = []


func _ready() -> void:
	title = "Set Aircraft Position"
	size = Vector2i(1200, 900)
	exclusive = true
	unresizable = true
	transient = true

	# Build UI
	var margin = MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 48)
	margin.add_theme_constant_override("margin_right", 48)
	margin.add_theme_constant_override("margin_top", 40)
	margin.add_theme_constant_override("margin_bottom", 40)
	add_child(margin)

	var vbox = VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 24)
	margin.add_child(vbox)

	# Current position
	_current_pos_label = Label.new()
	_current_pos_label.text = "Current: —"
	_current_pos_label.add_theme_font_size_override("font_size", 32)
	vbox.add_child(_current_pos_label)

	# Separator
	vbox.add_child(HSeparator.new())

	# Saved locations dropdown
	var loc_label = Label.new()
	loc_label.text = "Saved Locations:"
	loc_label.add_theme_font_size_override("font_size", 28)
	vbox.add_child(loc_label)

	_location_picker = OptionButton.new()
	_location_picker.add_item("— Select location —", 0)
	_location_picker.add_theme_font_size_override("font_size", 28)
	_location_picker.custom_minimum_size.y = 56
	_location_picker.item_selected.connect(_on_location_selected)
	# Make dropdown popup items larger too
	var popup = _location_picker.get_popup()
	popup.add_theme_font_size_override("font_size", 28)
	vbox.add_child(_location_picker)

	# Separator
	vbox.add_child(HSeparator.new())

	# Manual coordinates
	var coord_label = Label.new()
	coord_label.text = "Swiss LV95 Coordinates:"
	coord_label.add_theme_font_size_override("font_size", 28)
	vbox.add_child(coord_label)

	# Easting
	var e_hbox = HBoxContainer.new()
	var e_label = Label.new()
	e_label.text = "Easting (E):"
	e_label.custom_minimum_size.x = 220
	e_label.add_theme_font_size_override("font_size", 28)
	e_hbox.add_child(e_label)
	_easting_input = LineEdit.new()
	_easting_input.placeholder_text = "e.g. 2693200"
	_easting_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_easting_input.add_theme_font_size_override("font_size", 28)
	_easting_input.custom_minimum_size.y = 56
	e_hbox.add_child(_easting_input)
	vbox.add_child(e_hbox)

	# Northing
	var n_hbox = HBoxContainer.new()
	var n_label = Label.new()
	n_label.text = "Northing (N):"
	n_label.custom_minimum_size.x = 220
	n_label.add_theme_font_size_override("font_size", 28)
	n_hbox.add_child(n_label)
	_northing_input = LineEdit.new()
	_northing_input.placeholder_text = "e.g. 1265300"
	_northing_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_northing_input.add_theme_font_size_override("font_size", 28)
	_northing_input.custom_minimum_size.y = 56
	n_hbox.add_child(_northing_input)
	vbox.add_child(n_hbox)

	# Heading
	var h_hbox = HBoxContainer.new()
	var h_label = Label.new()
	h_label.text = "Heading (°):"
	h_label.custom_minimum_size.x = 220
	h_label.add_theme_font_size_override("font_size", 28)
	h_hbox.add_child(h_label)
	_heading_input = LineEdit.new()
	_heading_input.placeholder_text = "0-360 (default: 0)"
	_heading_input.text = "0"
	_heading_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_heading_input.add_theme_font_size_override("font_size", 28)
	_heading_input.custom_minimum_size.y = 56
	h_hbox.add_child(_heading_input)
	vbox.add_child(h_hbox)

	# Status label
	_status_label = Label.new()
	_status_label.text = ""
	_status_label.add_theme_color_override("font_color", Color.ORANGE_RED)
	_status_label.add_theme_font_size_override("font_size", 22)
	vbox.add_child(_status_label)

	# Buttons
	var btn_hbox = HBoxContainer.new()
	btn_hbox.alignment = BoxContainer.ALIGNMENT_END
	btn_hbox.add_theme_constant_override("separation", 24)

	var cancel_btn = Button.new()
	cancel_btn.text = "Cancel"
	cancel_btn.custom_minimum_size = Vector2(180, 56)
	cancel_btn.add_theme_font_size_override("font_size", 28)
	cancel_btn.pressed.connect(_on_cancel)
	btn_hbox.add_child(cancel_btn)

	var teleport_btn = Button.new()
	teleport_btn.text = "Teleport"
	teleport_btn.custom_minimum_size = Vector2(180, 56)
	teleport_btn.add_theme_font_size_override("font_size", 28)
	teleport_btn.pressed.connect(_on_teleport)
	btn_hbox.add_child(teleport_btn)

	vbox.add_child(btn_hbox)

	# Close on X button
	close_requested.connect(_on_cancel)


func populate_airports(airports: Array[Dictionary]) -> void:
	_airports = airports
	# Clear existing items (keep first "select" item)
	while _location_picker.item_count > 1:
		_location_picker.remove_item(1)

	for i in range(airports.size()):
		var ap = airports[i]
		var icao = ap.get("icao", "")
		var ap_name = ap.get("name", "")
		var elev = ap.get("elevation_m", 0)
		_location_picker.add_item("%s — %s (%dm)" % [icao, ap_name, elev], i + 1)


func set_current_position(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	_current_pos_label.text = "Current: E=%d  N=%d  Hdg=%.0f°" % [
		int(swiss_e), int(swiss_n), heading_deg
	]


func _on_location_selected(index: int) -> void:
	if index <= 0 or index - 1 >= _airports.size():
		return

	var ap = _airports[index - 1]
	var runways = ap.get("runways", [])

	# Prefer first runway threshold (puts you ON the runway)
	if runways.size() > 0:
		var rwy = runways[0]
		var desig = rwy.get("designator", "")
		var first_rwy = desig.split("/")[0] if "/" in desig else desig
		var threshold_key = "threshold_" + first_rwy
		var threshold = rwy.get(threshold_key, {})
		if threshold.has("e") and threshold.has("n"):
			_easting_input.text = str(threshold["e"])
			_northing_input.text = str(threshold["n"])
		else:
			# Fallback to airport center
			_easting_input.text = str(ap.get("center_e", 0))
			_northing_input.text = str(ap.get("center_n", 0))
		_heading_input.text = str(rwy.get("heading_deg", 0))
	else:
		_easting_input.text = str(ap.get("center_e", 0))
		_northing_input.text = str(ap.get("center_n", 0))

	_status_label.text = ""


func _on_teleport() -> void:
	var e_text = _easting_input.text.strip_edges()
	var n_text = _northing_input.text.strip_edges()
	var h_text = _heading_input.text.strip_edges()

	if e_text.is_empty() or n_text.is_empty():
		_status_label.text = "Please enter Easting and Northing."
		return

	var swiss_e = float(e_text)
	var swiss_n = float(n_text)
	var heading = float(h_text) if not h_text.is_empty() else 0.0

	# Basic validation (Switzerland LV95 bounds)
	if swiss_e < 2480000 or swiss_e > 2840000:
		_status_label.text = "Easting out of range (2'480'000 – 2'840'000)"
		return
	if swiss_n < 1070000 or swiss_n > 1300000:
		_status_label.text = "Northing out of range (1'070'000 – 1'300'000)"
		return

	teleport_requested.emit(swiss_e, swiss_n, heading)
	hide()


func _on_cancel() -> void:
	hide()


## Allow Enter key to confirm, Escape to cancel
func _input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_ESCAPE:
			_on_cancel()
			get_viewport().set_input_as_handled()
		elif event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER:
			_on_teleport()
			get_viewport().set_input_as_handled()
