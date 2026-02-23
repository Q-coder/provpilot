extends CanvasLayer
## WelcomeScreen — Startup screen with aircraft/airport selection and settings.
##
## Shown at program start. User selects an aircraft and starting airport,
## then clicks "Fly!" to begin the simulation. The settings button opens
## a separate settings page.
##
## Emits `flight_requested(aircraft_id, swiss_e, swiss_n, heading_deg)`.

signal flight_requested(aircraft_id: String, swiss_e: float, swiss_n: float, heading_deg: float)
signal settings_requested()

# ── Constants ──────────────────────────────────────────────────────────
const FONT_SM := 20
const FONT_MD := 24
const FONT_LG := 28
const FONT_XL := 36
const FONT_TITLE := 48

const BG_COLOR := Color(0.08, 0.10, 0.14, 1.0)
const PANEL_COLOR := Color(0.12, 0.14, 0.18, 1.0)
const ACCENT_COLOR := Color(0.25, 0.55, 0.85, 1.0)
const ACCENT_HOVER := Color(0.35, 0.65, 0.95, 1.0)

# ── Available aircraft ─────────────────────────────────────────────────
# id: JSBSim model name, label: display name, description
const AIRCRAFT := [
	{"id": "c172p", "label": "Cessna 172P Skyhawk", "desc": "Single-engine piston, 4-seat trainer. 160 HP."},
]

# ── State ──────────────────────────────────────────────────────────────
var _airports: Array[Dictionary] = []
var _filtered_airports: Array[Dictionary] = []
var _selected_aircraft_idx: int = 0
var _selected_airport: Dictionary = {}

# UI references
var _bg: ColorRect
var _aircraft_list: ItemList
var _aircraft_desc: Label
var _search_input: LineEdit
var _airport_list: ItemList
var _airport_info: RichTextLabel
var _runway_picker: OptionButton
var _fly_button: Button
var _status_label: Label

# Filter checkboxes
var _filter_international: CheckBox
var _filter_regional: CheckBox
var _filter_grass: CheckBox
var _filter_heliport: CheckBox


func _ready() -> void:
	layer = 100  # On top of everything
	_build_ui()
	_populate_aircraft_list()


# ── Public API ─────────────────────────────────────────────────────────

func populate_airports(airports: Array[Dictionary]) -> void:
	_airports = airports
	_apply_filters()
	# Auto-select the first airport
	if _filtered_airports.size() > 0:
		_airport_list.select(0)
		_on_airport_selected(0)


## Deferred helper — populate from POILoader node if airports weren't ready
func populate_airports_from_loader(poi_loader: Node) -> void:
	if poi_loader and poi_loader.airports.size() > 0:
		populate_airports(poi_loader.airports)


func show_welcome() -> void:
	visible = true
	# Input blocking is handled by the main scene guarding _process.
	if _search_input:
		_search_input.grab_focus()


func hide_welcome() -> void:
	visible = false


# ── UI Construction ────────────────────────────────────────────────────

func _build_ui() -> void:
	# Full-screen background
	_bg = ColorRect.new()
	_bg.color = BG_COLOR
	_bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_bg)

	# Main layout: centered content area
	var center_container = CenterContainer.new()
	center_container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_bg.add_child(center_container)

	var main_panel = PanelContainer.new()
	var panel_style = StyleBoxFlat.new()
	panel_style.bg_color = PANEL_COLOR
	panel_style.corner_radius_top_left = 16
	panel_style.corner_radius_top_right = 16
	panel_style.corner_radius_bottom_left = 16
	panel_style.corner_radius_bottom_right = 16
	panel_style.content_margin_left = 48
	panel_style.content_margin_right = 48
	panel_style.content_margin_top = 40
	panel_style.content_margin_bottom = 40
	main_panel.add_theme_stylebox_override("panel", panel_style)
	main_panel.custom_minimum_size = Vector2(1600, 900)
	center_container.add_child(main_panel)

	var main_vbox = VBoxContainer.new()
	main_vbox.add_theme_constant_override("separation", 20)
	main_panel.add_child(main_vbox)

	# ── Title ──────────────────────────────────────────────────────────
	var title_hbox = HBoxContainer.new()
	title_hbox.alignment = BoxContainer.ALIGNMENT_CENTER
	main_vbox.add_child(title_hbox)

	var title = Label.new()
	title.text = "ProVPilot"
	title.add_theme_font_size_override("font_size", FONT_TITLE)
	title.add_theme_color_override("font_color", ACCENT_COLOR)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title_hbox.add_child(title)

	var subtitle = Label.new()
	subtitle.text = "  —  Swiss Flight Simulator"
	subtitle.add_theme_font_size_override("font_size", FONT_LG)
	subtitle.add_theme_color_override("font_color", Color(0.6, 0.65, 0.7))
	title_hbox.add_child(subtitle)

	# Separator
	main_vbox.add_child(HSeparator.new())

	# ── Content: Three columns ─────────────────────────────────────────
	var content_hbox = HBoxContainer.new()
	content_hbox.add_theme_constant_override("separation", 32)
	content_hbox.size_flags_vertical = Control.SIZE_EXPAND_FILL
	main_vbox.add_child(content_hbox)

	_build_aircraft_panel(content_hbox)
	_build_airport_panel(content_hbox)
	_build_details_panel(content_hbox)

	# ── Bottom bar ─────────────────────────────────────────────────────
	main_vbox.add_child(HSeparator.new())
	_build_bottom_bar(main_vbox)


func _build_aircraft_panel(parent: HBoxContainer) -> void:
	var vbox = VBoxContainer.new()
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.size_flags_stretch_ratio = 0.7
	vbox.add_theme_constant_override("separation", 12)
	parent.add_child(vbox)

	var header = Label.new()
	header.text = "Aircraft"
	header.add_theme_font_size_override("font_size", FONT_LG)
	header.add_theme_color_override("font_color", ACCENT_COLOR)
	vbox.add_child(header)

	_aircraft_list = ItemList.new()
	_aircraft_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_aircraft_list.add_theme_font_size_override("font_size", FONT_MD)
	_aircraft_list.item_selected.connect(_on_aircraft_selected)
	vbox.add_child(_aircraft_list)

	_aircraft_desc = Label.new()
	_aircraft_desc.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_aircraft_desc.add_theme_font_size_override("font_size", FONT_SM)
	_aircraft_desc.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
	_aircraft_desc.custom_minimum_size.y = 60
	vbox.add_child(_aircraft_desc)


func _build_airport_panel(parent: HBoxContainer) -> void:
	var vbox = VBoxContainer.new()
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.size_flags_stretch_ratio = 1.3
	vbox.add_theme_constant_override("separation", 10)
	parent.add_child(vbox)

	var header = Label.new()
	header.text = "Starting Airport"
	header.add_theme_font_size_override("font_size", FONT_LG)
	header.add_theme_color_override("font_color", ACCENT_COLOR)
	vbox.add_child(header)

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
	vbox.add_child(search_hbox)

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
	vbox.add_child(filter_hbox)

	# Airport list
	_airport_list = ItemList.new()
	_airport_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_airport_list.add_theme_font_size_override("font_size", FONT_SM)
	_airport_list.item_selected.connect(_on_airport_selected)
	_airport_list.item_activated.connect(_on_airport_activated)
	vbox.add_child(_airport_list)


func _build_details_panel(parent: HBoxContainer) -> void:
	var vbox = VBoxContainer.new()
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.size_flags_stretch_ratio = 0.9
	vbox.add_theme_constant_override("separation", 12)
	parent.add_child(vbox)

	var header = Label.new()
	header.text = "Airport Details"
	header.add_theme_font_size_override("font_size", FONT_LG)
	header.add_theme_color_override("font_color", ACCENT_COLOR)
	vbox.add_child(header)

	_airport_info = RichTextLabel.new()
	_airport_info.bbcode_enabled = true
	_airport_info.fit_content = false
	_airport_info.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_airport_info.add_theme_font_size_override("normal_font_size", FONT_SM)
	_airport_info.add_theme_font_size_override("bold_font_size", FONT_SM)
	vbox.add_child(_airport_info)

	# Runway selector
	var rwy_hbox = HBoxContainer.new()
	rwy_hbox.add_theme_constant_override("separation", 8)
	var rwy_label = Label.new()
	rwy_label.text = "Runway:"
	rwy_label.add_theme_font_size_override("font_size", FONT_MD)
	rwy_label.custom_minimum_size.x = 100
	rwy_hbox.add_child(rwy_label)
	_runway_picker = OptionButton.new()
	_runway_picker.add_theme_font_size_override("font_size", FONT_MD)
	_runway_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_runway_picker.custom_minimum_size.y = 44
	var rwy_popup = _runway_picker.get_popup()
	rwy_popup.add_theme_font_size_override("font_size", FONT_MD)
	rwy_hbox.add_child(_runway_picker)
	vbox.add_child(rwy_hbox)

	# Status
	_status_label = Label.new()
	_status_label.text = ""
	_status_label.add_theme_color_override("font_color", Color.ORANGE_RED)
	_status_label.add_theme_font_size_override("font_size", FONT_SM)
	vbox.add_child(_status_label)


func _build_bottom_bar(parent: VBoxContainer) -> void:
	var hbox = HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 16)
	parent.add_child(hbox)

	# Settings button (left)
	var settings_btn = Button.new()
	settings_btn.text = "Settings"
	settings_btn.custom_minimum_size = Vector2(180, 52)
	settings_btn.add_theme_font_size_override("font_size", FONT_MD)
	settings_btn.pressed.connect(_on_settings_pressed)
	hbox.add_child(settings_btn)

	# Version label
	var version_label = Label.new()
	version_label.text = "v1.0.0"
	version_label.add_theme_font_size_override("font_size", FONT_SM)
	version_label.add_theme_color_override("font_color", Color(0.4, 0.4, 0.4))
	version_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	version_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	hbox.add_child(version_label)

	# Fly button (right, prominent)
	_fly_button = Button.new()
	_fly_button.text = "Fly!"
	_fly_button.custom_minimum_size = Vector2(220, 56)
	_fly_button.add_theme_font_size_override("font_size", FONT_XL)
	_fly_button.pressed.connect(_on_fly_pressed)

	# Style the Fly button with accent color
	var fly_style_normal = StyleBoxFlat.new()
	fly_style_normal.bg_color = ACCENT_COLOR
	fly_style_normal.corner_radius_top_left = 8
	fly_style_normal.corner_radius_top_right = 8
	fly_style_normal.corner_radius_bottom_left = 8
	fly_style_normal.corner_radius_bottom_right = 8
	_fly_button.add_theme_stylebox_override("normal", fly_style_normal)

	var fly_style_hover = StyleBoxFlat.new()
	fly_style_hover.bg_color = ACCENT_HOVER
	fly_style_hover.corner_radius_top_left = 8
	fly_style_hover.corner_radius_top_right = 8
	fly_style_hover.corner_radius_bottom_left = 8
	fly_style_hover.corner_radius_bottom_right = 8
	_fly_button.add_theme_stylebox_override("hover", fly_style_hover)
	_fly_button.add_theme_color_override("font_color", Color.WHITE)

	hbox.add_child(_fly_button)


# ── Aircraft ───────────────────────────────────────────────────────────

func _populate_aircraft_list() -> void:
	_aircraft_list.clear()
	for ac in AIRCRAFT:
		_aircraft_list.add_item(ac["label"])
	if AIRCRAFT.size() > 0:
		_aircraft_list.select(0)
		_on_aircraft_selected(0)


func _on_aircraft_selected(index: int) -> void:
	_selected_aircraft_idx = index
	if index >= 0 and index < AIRCRAFT.size():
		_aircraft_desc.text = AIRCRAFT[index]["desc"]


# ── Airport Filter & Search (mirrors position_dialog.gd) ──────────────

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
		if not _type_matches_filter(ap.get("type", "")):
			continue
		if search_text.length() > 0:
			var icao = ap.get("icao", "").to_lower()
			var ap_name = ap.get("name", "").to_lower()
			if search_text not in icao and search_text not in ap_name:
				continue
		_filtered_airports.append(ap)

	# Sort alphabetically by ICAO
	_filtered_airports.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return a.get("icao", "") < b.get("icao", "")
	)

	_rebuild_airport_list()


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
		var type_str = _type_icon(ap.get("type", ""))
		var text = "%s  %s  %s  %dm" % [icao, ap_name, type_str, elev]
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


func _on_airport_activated(index: int) -> void:
	_on_airport_selected(index)
	_on_fly_pressed()


func _update_airport_info() -> void:
	var ap = _selected_airport
	if ap.is_empty():
		_airport_info.text = ""
		return

	var icao = ap.get("icao", "----")
	if icao == "":
		icao = "----"
	var ap_name = ap.get("name", "")
	var ap_type = ap.get("type", "").replace("_", " ").capitalize()
	var elev = ap.get("elevation_m", 0)
	var area = ap.get("area_ha", 0.0)

	var info = "[b]%s[/b]  %s\n" % [icao, ap_name]
	info += "Type: %s\n" % ap_type
	info += "Elevation: %dm MSL\n" % elev
	if area > 0:
		info += "Area: %.1f ha\n" % area

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

	_airport_info.text = info


func _populate_runways() -> void:
	_runway_picker.clear()
	var ap = _selected_airport
	if ap.is_empty():
		return

	var runways = ap.get("runways", [])
	if runways.size() == 0:
		_runway_picker.add_item("Pad center")
		return

	for rwy in runways:
		var desig = rwy.get("designator", "?")
		var parts = desig.split("/")
		for p in parts:
			var threshold_key = "threshold_" + p
			if rwy.has(threshold_key):
				var hdg := int(rwy.get("heading_deg", 0))
				if parts.size() > 1 and p == parts[1]:
					hdg = (hdg + 180) % 360
				var length_m = rwy.get("length_m", 0)
				var surface = rwy.get("surface", "?")
				_runway_picker.add_item("Rwy %s  (%dm, %s, hdg %d°)" % [p, length_m, surface, hdg])


func _get_selected_departure() -> Dictionary:
	## Returns {swiss_e, swiss_n, heading_deg} for the selected airport/runway
	var ap = _selected_airport
	if ap.is_empty():
		return {}

	var runways = ap.get("runways", [])
	var rwy_idx = _runway_picker.selected if _runway_picker.selected >= 0 else 0

	if runways.size() == 0:
		# Heliport
		var pad = ap.get("pad", {})
		return {
			"swiss_e": pad.get("e", ap.get("center_e", 0)),
			"swiss_n": pad.get("n", ap.get("center_n", 0)),
			"heading_deg": 0.0,
		}

	# Build flat list matching UI order
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
					"swiss_e": thresh.get("e", ap.get("center_e", 0)),
					"swiss_n": thresh.get("n", ap.get("center_n", 0)),
					"heading_deg": float(hdg),
				})

	if rwy_idx >= 0 and rwy_idx < entries.size():
		return entries[rwy_idx]
	elif entries.size() > 0:
		return entries[0]
	return {}


# ── Actions ────────────────────────────────────────────────────────────

func _on_fly_pressed() -> void:
	var departure = _get_selected_departure()
	if departure.is_empty():
		_status_label.text = "Please select an airport."
		return

	var aircraft_id = AIRCRAFT[_selected_aircraft_idx]["id"] if _selected_aircraft_idx < AIRCRAFT.size() else "c172p"
	flight_requested.emit(
		aircraft_id,
		departure["swiss_e"],
		departure["swiss_n"],
		departure["heading_deg"]
	)
	hide_welcome()


func _on_settings_pressed() -> void:
	settings_requested.emit()


## Allow Enter to start flight, Escape does nothing on welcome screen
func _input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER:
			_on_fly_pressed()
			get_viewport().set_input_as_handled()


## Block all unhandled input from reaching the sim behind the overlay
func _unhandled_input(_event: InputEvent) -> void:
	if visible:
		get_viewport().set_input_as_handled()
