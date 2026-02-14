extends Window
## Position Dialog — Teleport aircraft to Swiss airports or manual coordinates.
##
## Opened with Shift+P. Provides:
##  - Searchable airport list with type filters
##  - Runway selector for multi-runway airports
##  - Distance from current position
##  - Manual Easting/Northing/Heading entry
##  - Current position display
##
## Emits `teleport_requested(swiss_e, swiss_n, heading_deg)` when confirmed.

signal teleport_requested(swiss_e: float, swiss_n: float, heading_deg: float)

# UI nodes (created in _ready)
var _easting_input: LineEdit
var _northing_input: LineEdit
var _heading_input: LineEdit
var _search_input: LineEdit
var _airport_list: ItemList
var _runway_picker: OptionButton
var _current_pos_label: Label
var _status_label: Label
var _airport_info_label: RichTextLabel

# Filter checkboxes
var _filter_international: CheckBox
var _filter_regional: CheckBox
var _filter_grass: CheckBox
var _filter_heliport: CheckBox

# Airport data from POILoader
var _airports: Array[Dictionary] = []
var _filtered_airports: Array[Dictionary] = []
var _selected_airport: Dictionary = {}
var _current_swiss_e: float = 0.0
var _current_swiss_n: float = 0.0

# Font size constants
const FONT_SM := 20
const FONT_MD := 24
const FONT_LG := 28


func _ready() -> void:
	title = "Set Aircraft Position"
	size = Vector2i(1400, 1000)
	exclusive = true
	unresizable = true
	transient = true
	add_theme_font_size_override("title_font_size", FONT_LG)

	# Build UI
	var margin = MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 32)
	margin.add_theme_constant_override("margin_right", 32)
	margin.add_theme_constant_override("margin_top", 28)
	margin.add_theme_constant_override("margin_bottom", 28)
	add_child(margin)

	var main_hbox = HBoxContainer.new()
	main_hbox.add_theme_constant_override("separation", 24)
	margin.add_child(main_hbox)

	# ── Left panel: Airport selector ──────────────────────────────────
	var left_vbox = VBoxContainer.new()
	left_vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	left_vbox.size_flags_stretch_ratio = 1.4
	left_vbox.add_theme_constant_override("separation", 12)
	main_hbox.add_child(left_vbox)

	# Current position
	_current_pos_label = Label.new()
	_current_pos_label.text = "Current: —"
	_current_pos_label.add_theme_font_size_override("font_size", FONT_MD)
	left_vbox.add_child(_current_pos_label)

	# Search bar
	var search_hbox = HBoxContainer.new()
	search_hbox.add_theme_constant_override("separation", 8)
	var search_label = Label.new()
	search_label.text = "Search:"
	search_label.add_theme_font_size_override("font_size", FONT_MD)
	search_hbox.add_child(search_label)
	_search_input = LineEdit.new()
	_search_input.placeholder_text = "ICAO or name..."
	_search_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search_input.add_theme_font_size_override("font_size", FONT_MD)
	_search_input.custom_minimum_size.y = 44
	_search_input.text_changed.connect(_on_search_changed)
	_search_input.clear_button_enabled = true
	search_hbox.add_child(_search_input)
	left_vbox.add_child(search_hbox)

	# Type filters
	var filter_hbox = HBoxContainer.new()
	filter_hbox.add_theme_constant_override("separation", 16)

	_filter_international = _make_filter_checkbox("Intl", true)
	_filter_regional = _make_filter_checkbox("Regional", true)
	_filter_grass = _make_filter_checkbox("Grass", true)
	_filter_heliport = _make_filter_checkbox("Heli", false)

	filter_hbox.add_child(_filter_international)
	filter_hbox.add_child(_filter_regional)
	filter_hbox.add_child(_filter_grass)
	filter_hbox.add_child(_filter_heliport)
	left_vbox.add_child(filter_hbox)

	# Airport list
	_airport_list = ItemList.new()
	_airport_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_airport_list.add_theme_font_size_override("font_size", FONT_SM)
	_airport_list.custom_minimum_size.y = 400
	_airport_list.item_selected.connect(_on_airport_selected)
	_airport_list.item_activated.connect(_on_airport_activated)
	left_vbox.add_child(_airport_list)

	# ── Right panel: Details + Manual entry ───────────────────────────
	var right_vbox = VBoxContainer.new()
	right_vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	right_vbox.size_flags_stretch_ratio = 1.0
	right_vbox.add_theme_constant_override("separation", 16)
	main_hbox.add_child(right_vbox)

	# Airport info
	var info_label = Label.new()
	info_label.text = "Airport Details:"
	info_label.add_theme_font_size_override("font_size", FONT_MD)
	right_vbox.add_child(info_label)

	_airport_info_label = RichTextLabel.new()
	_airport_info_label.bbcode_enabled = true
	_airport_info_label.fit_content = true
	_airport_info_label.custom_minimum_size.y = 180
	_airport_info_label.add_theme_font_size_override("normal_font_size", FONT_SM)
	_airport_info_label.add_theme_font_size_override("bold_font_size", FONT_SM)
	right_vbox.add_child(_airport_info_label)

	# Runway selector
	var rwy_hbox = HBoxContainer.new()
	rwy_hbox.add_theme_constant_override("separation", 8)
	var rwy_label = Label.new()
	rwy_label.text = "Runway:"
	rwy_label.add_theme_font_size_override("font_size", FONT_MD)
	rwy_label.custom_minimum_size.x = 130
	rwy_hbox.add_child(rwy_label)
	_runway_picker = OptionButton.new()
	_runway_picker.add_theme_font_size_override("font_size", FONT_MD)
	_runway_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_runway_picker.custom_minimum_size.y = 44
	_runway_picker.item_selected.connect(_on_runway_selected)
	var rwy_popup = _runway_picker.get_popup()
	rwy_popup.add_theme_font_size_override("font_size", FONT_MD)
	rwy_hbox.add_child(_runway_picker)
	right_vbox.add_child(rwy_hbox)

	# Separator
	right_vbox.add_child(HSeparator.new())

	# Manual coordinates section
	var manual_label = Label.new()
	manual_label.text = "Manual Coordinates (LV95):"
	manual_label.add_theme_font_size_override("font_size", FONT_MD)
	right_vbox.add_child(manual_label)

	# Easting
	var e_hbox = HBoxContainer.new()
	var e_label = Label.new()
	e_label.text = "Easting (E):"
	e_label.custom_minimum_size.x = 130
	e_label.add_theme_font_size_override("font_size", FONT_MD)
	e_hbox.add_child(e_label)
	_easting_input = LineEdit.new()
	_easting_input.placeholder_text = "e.g. 2693200"
	_easting_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_easting_input.add_theme_font_size_override("font_size", FONT_MD)
	_easting_input.custom_minimum_size.y = 44
	e_hbox.add_child(_easting_input)
	right_vbox.add_child(e_hbox)

	# Northing
	var n_hbox = HBoxContainer.new()
	var n_label = Label.new()
	n_label.text = "Northing (N):"
	n_label.custom_minimum_size.x = 130
	n_label.add_theme_font_size_override("font_size", FONT_MD)
	n_hbox.add_child(n_label)
	_northing_input = LineEdit.new()
	_northing_input.placeholder_text = "e.g. 1265300"
	_northing_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_northing_input.add_theme_font_size_override("font_size", FONT_MD)
	_northing_input.custom_minimum_size.y = 44
	n_hbox.add_child(_northing_input)
	right_vbox.add_child(n_hbox)

	# Heading
	var h_hbox = HBoxContainer.new()
	var h_label = Label.new()
	h_label.text = "Heading (°):"
	h_label.custom_minimum_size.x = 130
	h_label.add_theme_font_size_override("font_size", FONT_MD)
	h_hbox.add_child(h_label)
	_heading_input = LineEdit.new()
	_heading_input.placeholder_text = "0-360"
	_heading_input.text = "0"
	_heading_input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_heading_input.add_theme_font_size_override("font_size", FONT_MD)
	_heading_input.custom_minimum_size.y = 44
	h_hbox.add_child(_heading_input)
	right_vbox.add_child(h_hbox)

	# Status label
	_status_label = Label.new()
	_status_label.text = ""
	_status_label.add_theme_color_override("font_color", Color.ORANGE_RED)
	_status_label.add_theme_font_size_override("font_size", FONT_SM)
	right_vbox.add_child(_status_label)

	# Spacer
	var spacer = Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right_vbox.add_child(spacer)

	# Buttons
	var btn_hbox = HBoxContainer.new()
	btn_hbox.alignment = BoxContainer.ALIGNMENT_END
	btn_hbox.add_theme_constant_override("separation", 16)

	var cancel_btn = Button.new()
	cancel_btn.text = "Cancel"
	cancel_btn.custom_minimum_size = Vector2(160, 48)
	cancel_btn.add_theme_font_size_override("font_size", FONT_MD)
	cancel_btn.pressed.connect(_on_cancel)
	btn_hbox.add_child(cancel_btn)

	var teleport_btn = Button.new()
	teleport_btn.text = "Teleport"
	teleport_btn.custom_minimum_size = Vector2(160, 48)
	teleport_btn.add_theme_font_size_override("font_size", FONT_MD)
	teleport_btn.pressed.connect(_on_teleport)
	btn_hbox.add_child(teleport_btn)

	right_vbox.add_child(btn_hbox)

	# Close on X button
	close_requested.connect(_on_cancel)


# ── Public API ─────────────────────────────────────────────────────────

func populate_airports(airports: Array[Dictionary]) -> void:
	_airports = airports
	_apply_filters()


func set_current_position(swiss_e: float, swiss_n: float, heading_deg: float) -> void:
	_current_swiss_e = swiss_e
	_current_swiss_n = swiss_n
	_current_pos_label.text = "Current: E=%d  N=%d  Hdg=%.0f°" % [
		int(swiss_e), int(swiss_n), heading_deg
	]


# ── Filter & Search ───────────────────────────────────────────────────

func _make_filter_checkbox(label_text: String, checked: bool) -> CheckBox:
	var cb = CheckBox.new()
	cb.text = label_text
	cb.button_pressed = checked
	cb.add_theme_font_size_override("font_size", FONT_SM)
	cb.toggled.connect(func(_on: bool): _apply_filters())
	return cb


func _type_matches_filter(ap_type: String) -> bool:
	match ap_type:
		"international_airport":
			return _filter_international.button_pressed
		"regional_airport":
			return _filter_regional.button_pressed
		"grass_strip":
			return _filter_grass.button_pressed
		"heliport":
			return _filter_heliport.button_pressed
		_:
			return true


func _apply_filters() -> void:
	var search_text = _search_input.text.strip_edges().to_lower() if _search_input else ""

	_filtered_airports.clear()
	for ap in _airports:
		# Type filter
		if not _type_matches_filter(ap.get("type", "")):
			continue

		# Search filter
		if search_text.length() > 0:
			var icao = ap.get("icao", "").to_lower()
			var ap_name = ap.get("name", "").to_lower()
			if search_text not in icao and search_text not in ap_name:
				continue

		_filtered_airports.append(ap)

	# Sort by distance from current position
	_filtered_airports.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var da = _distance_to(a)
		var db = _distance_to(b)
		return da < db
	)

	_rebuild_airport_list()


func _distance_to(ap: Dictionary) -> float:
	if _current_swiss_e == 0.0 and _current_swiss_n == 0.0:
		return 0.0
	var de = float(ap.get("center_e", 0)) - _current_swiss_e
	var dn = float(ap.get("center_n", 0)) - _current_swiss_n
	return sqrt(de * de + dn * dn)


func _format_distance(dist_m: float) -> String:
	if dist_m < 1000:
		return "%dm" % int(dist_m)
	elif dist_m < 100000:
		return "%.1fkm" % (dist_m / 1000.0)
	else:
		return "%dkm" % int(dist_m / 1000.0)


func _type_icon(ap_type: String) -> String:
	match ap_type:
		"international_airport": return "[INTL]"
		"regional_airport": return "[REG ]"
		"grass_strip": return "[GRAS]"
		"heliport": return "[HELI]"
		_: return "[    ]"


func _rebuild_airport_list() -> void:
	_airport_list.clear()
	for ap in _filtered_airports:
		var icao = ap.get("icao", "----")
		if icao == "":
			icao = "----"
		var ap_name = ap.get("name", "")
		var elev = ap.get("elevation_m", 0)
		var dist = _distance_to(ap)
		var dist_str = _format_distance(dist) if dist > 0 else ""
		var type_str = _type_icon(ap.get("type", ""))

		var text = "%s %s  %s  %dm" % [icao, ap_name, type_str, elev]
		if dist_str != "":
			text += "  (%s)" % dist_str
		_airport_list.add_item(text)


func _on_search_changed(_new_text: String) -> void:
	_apply_filters()


# ── Airport Selection ──────────────────────────────────────────────────

func _on_airport_selected(index: int) -> void:
	if index < 0 or index >= _filtered_airports.size():
		return

	_selected_airport = _filtered_airports[index]
	_update_airport_info()
	_populate_runways()
	_fill_coordinates_from_runway(0)


func _on_airport_activated(index: int) -> void:
	# Double-click: select and teleport
	_on_airport_selected(index)
	_on_teleport()


func _update_airport_info() -> void:
	var ap = _selected_airport
	if ap.is_empty():
		_airport_info_label.text = ""
		return

	var icao = ap.get("icao", "----")
	if icao == "":
		icao = "----"
	var ap_name = ap.get("name", "")
	var ap_type = ap.get("type", "").replace("_", " ").capitalize()
	var elev = ap.get("elevation_m", 0)
	var area = ap.get("area_ha", 0.0)
	var dist = _distance_to(ap)

	var info = "[b]%s[/b]  %s\n" % [icao, ap_name]
	info += "Type: %s\n" % ap_type
	info += "Elevation: %dm MSL\n" % elev
	if area > 0:
		info += "Area: %.1f ha\n" % area
	if dist > 0:
		info += "Distance: %s\n" % _format_distance(dist)

	var runways = ap.get("runways", [])
	if runways.size() > 0:
		info += "\nRunways:\n"
		for rwy in runways:
			var desig = rwy.get("designator", "?")
			var length_m = rwy.get("length_m", 0)
			var width_m = rwy.get("width_m", 0)
			var surface = rwy.get("surface", "?")
			info += "  %s  %dm x %dm  %s\n" % [desig, length_m, width_m, surface]
	elif ap.has("pad"):
		info += "\nHelipad at center\n"

	_airport_info_label.text = info


func _populate_runways() -> void:
	_runway_picker.clear()
	var ap = _selected_airport
	if ap.is_empty():
		return

	var runways = ap.get("runways", [])
	if runways.size() == 0:
		# Heliport — add pad center
		_runway_picker.add_item("Pad center")
		return

	for rwy in runways:
		var desig = rwy.get("designator", "?")
		var parts = desig.split("/")
		# Add each direction as a separate entry
		for p in parts:
			var threshold_key = "threshold_" + p
			if rwy.has(threshold_key):
				var hdg := int(rwy.get("heading_deg", 0))
				# Second designator is reciprocal heading
				if parts.size() > 1 and p == parts[1]:
					hdg = (hdg + 180) % 360
				var length_m = rwy.get("length_m", 0)
				var surface = rwy.get("surface", "?")
				_runway_picker.add_item("Rwy %s  (%dm, %s, hdg %d°)" % [p, length_m, surface, hdg])


func _on_runway_selected(index: int) -> void:
	_fill_coordinates_from_runway(index)


func _fill_coordinates_from_runway(rwy_ui_index: int) -> void:
	var ap = _selected_airport
	if ap.is_empty():
		return

	var runways = ap.get("runways", [])
	if runways.size() == 0:
		# Heliport
		var pad = ap.get("pad", {})
		_easting_input.text = str(pad.get("e", ap.get("center_e", 0)))
		_northing_input.text = str(pad.get("n", ap.get("center_n", 0)))
		_heading_input.text = "0"
		return

	# Build flat list of (threshold_e, threshold_n, heading) matching UI order
	var entries: Array = []
	for rwy in runways:
		var desig = rwy.get("designator", "?")
		var parts = desig.split("/")
		for i_part in range(parts.size()):
			var p = parts[i_part]
			var threshold_key = "threshold_" + p
			if rwy.has(threshold_key):
				var thresh = rwy[threshold_key]
				var hdg := int(rwy.get("heading_deg", 0))
				if i_part == 1:
					hdg = (hdg + 180) % 360
				entries.append({
					"e": thresh.get("e", ap.get("center_e", 0)),
					"n": thresh.get("n", ap.get("center_n", 0)),
					"heading": hdg,
				})

	if rwy_ui_index >= 0 and rwy_ui_index < entries.size():
		var entry = entries[rwy_ui_index]
		_easting_input.text = str(entry["e"])
		_northing_input.text = str(entry["n"])
		_heading_input.text = str(entry["heading"])
	elif entries.size() > 0:
		var entry = entries[0]
		_easting_input.text = str(entry["e"])
		_northing_input.text = str(entry["n"])
		_heading_input.text = str(entry["heading"])

	_status_label.text = ""


# ── Teleport / Cancel ──────────────────────────────────────────────────

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
			# Only teleport on Enter if not in the search field
			if not _search_input.has_focus():
				_on_teleport()
				get_viewport().set_input_as_handled()
