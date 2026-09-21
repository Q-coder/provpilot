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
## Emitted when the user wants to resume an in-progress flight without
## re-teleporting to the airport. Only available once a flight has started.
signal continue_requested()
## Emitted by the first-run wizard after the user picks a scenery folder.
## node_3d.gd connects to this; it imports the folder into the scenery library
## and re-applies sources to the Scenery3D node.
signal scenery_setup_requested(path: String)

# ── Constants ──────────────────────────────────────────────────────────
const FONT_SM := 20
const FONT_MD := 24
const FONT_LG := 28
const FONT_XL := 36
const FONT_TITLE := 48

# Update-check endpoint. Returns JSON of the form:
#   { "version": "1.2.3", "macos_arm64": { "url": "...", "sha256": "...", "size": 12345 } }
const UPDATE_LATEST_URL := "https://www.md80.ch/provpilot/releases/latest.json"
const UPDATE_BASE_URL := "https://www.md80.ch/provpilot/releases/"

const BG_COLOR := Color(0.08, 0.10, 0.14, 1.0)
const PANEL_COLOR := Color(0.12, 0.14, 0.18, 1.0)
const ACCENT_COLOR := Color(0.25, 0.55, 0.85, 1.0)
const ACCENT_HOVER := Color(0.35, 0.65, 0.95, 1.0)

# ── Available aircraft ─────────────────────────────────────────────────
# id: JSBSim model name, label: display name, description
const AIRCRAFT := [
	{"id": "c172p", "label": "Cessna 172P Skyhawk", "desc": "Single-engine piston, 4-seat trainer. 160 HP."},
	{"id": "pc7", "label": "Pilatus PC-7 Turbo Trainer", "desc": "Single-engine turboprop military trainer. PT6A, ~650 SHP."},
	{"id": "a10", "label": "Fairchild A-10A Thunderbolt II", "desc": "Twin-engine close air support jet. 2x GE TF34 turbofans."},
]

# ── State ──────────────────────────────────────────────────────────────
var _airports: Array[Dictionary] = []
var _filtered_airports: Array[Dictionary] = []
var _selected_aircraft_idx: int = 0
var _selected_airport: Dictionary = {}

# Set of region IDs (e.g. "switzerland") with a base scenery pack installed.
# Used to annotate airport entries with availability markers. Populated by
# node_3d.gd before populate_airports() is called.
var _installed_regions: Dictionary = {}

# UI references
var _bg: ColorRect
var _main_panel: PanelContainer
var _update_label: RichTextLabel
var _update_request: HTTPRequest
var _latest_download_url: String = ""
var _aircraft_list: ItemList
var _aircraft_desc: Label
var _search_input: LineEdit
var _airport_list: ItemList
var _airport_info: RichTextLabel
var _runway_picker: OptionButton
var _fly_button: Button
var _continue_button: Button
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
	_check_for_updates()
	# Defer one frame so node_3d.gd has time to connect its signal handler.
	call_deferred("_maybe_show_first_run_wizard")


# ── Public API ─────────────────────────────────────────────────────────

func populate_airports(airports: Array[Dictionary]) -> void:
	_airports = airports
	_apply_filters()
	# Auto-select the first airport
	if _filtered_airports.size() > 0:
		_airport_list.select(0)
		_on_airport_selected(0)


## Set the set of regions whose base scenery pack is installed. Airports in
## other regions will be shown with a "locked" marker. Pass the result of
## `AirportsLib.installed_regions(settings.installed_scenery_packs)`.
func set_installed_regions(regions: Dictionary) -> void:
	_installed_regions = regions
	if _airport_list != null:
		_rebuild_airport_list()


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


## Show/hide the "Continue" button. The host calls this with `true` once a
## flight is in progress (i.e. the user pressed Escape mid-flight) so that
## returning to the welcome screen offers both "resume current flight" and
## "restart at airport" actions.
func set_continue_enabled(enabled: bool) -> void:
	if _continue_button:
		_continue_button.visible = enabled
		_continue_button.disabled = not enabled


# ── UI Construction ────────────────────────────────────────────────────

func _build_ui() -> void:
	# Full-screen background
	_bg = ColorRect.new()
	_bg.color = BG_COLOR
	_bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_bg)

	# Main layout: panel anchored to fill the whole viewport with a small
	# uniform margin so it scales on any display resolution.
	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 32)
	margin.add_theme_constant_override("margin_right", 32)
	margin.add_theme_constant_override("margin_top", 32)
	margin.add_theme_constant_override("margin_bottom", 32)
	_bg.add_child(margin)

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
	main_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	main_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_main_panel = main_panel
	margin.add_child(main_panel)

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

	# Update-available banner (hidden until the version check resolves)
	_update_label = RichTextLabel.new()
	_update_label.bbcode_enabled = true
	_update_label.fit_content = true
	_update_label.scroll_active = false
	_update_label.add_theme_font_size_override("normal_font_size", FONT_SM)
	_update_label.add_theme_color_override("default_color", Color(0.95, 0.8, 0.3))
	_update_label.visible = false
	_update_label.meta_clicked.connect(_on_update_meta_clicked)
	main_vbox.add_child(_update_label)

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

	# About button
	var about_btn = Button.new()
	about_btn.text = "About"
	about_btn.custom_minimum_size = Vector2(140, 52)
	about_btn.add_theme_font_size_override("font_size", FONT_MD)
	about_btn.pressed.connect(_on_about_pressed)
	hbox.add_child(about_btn)

	# Quit button
	var quit_btn = Button.new()
	quit_btn.text = "Quit"
	quit_btn.custom_minimum_size = Vector2(140, 52)
	quit_btn.add_theme_font_size_override("font_size", FONT_MD)
	quit_btn.pressed.connect(_on_quit_pressed)
	hbox.add_child(quit_btn)

	# Version label
	var version_label = Label.new()
	version_label.text = "v" + String(ProjectSettings.get_setting("application/config/version", "0.0.0"))
	version_label.add_theme_font_size_override("font_size", FONT_SM)
	version_label.add_theme_color_override("font_color", Color(0.4, 0.4, 0.4))
	version_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	version_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	hbox.add_child(version_label)

	# Continue button (right, shown only when a flight is in progress).
	# Lets the user return to the cockpit without re-teleporting to the
	# airport — preserves position, attitude and JSBSim state.
	_continue_button = Button.new()
	_continue_button.text = "Continue Flight"
	_continue_button.custom_minimum_size = Vector2(220, 56)
	_continue_button.add_theme_font_size_override("font_size", FONT_LG)
	_continue_button.pressed.connect(_on_continue_pressed)
	_continue_button.visible = false
	_continue_button.disabled = true
	var cont_style_normal = StyleBoxFlat.new()
	cont_style_normal.bg_color = Color(0.20, 0.45, 0.30)
	cont_style_normal.corner_radius_top_left = 8
	cont_style_normal.corner_radius_top_right = 8
	cont_style_normal.corner_radius_bottom_left = 8
	cont_style_normal.corner_radius_bottom_right = 8
	_continue_button.add_theme_stylebox_override("normal", cont_style_normal)
	var cont_style_hover = StyleBoxFlat.new()
	cont_style_hover.bg_color = Color(0.28, 0.58, 0.40)
	cont_style_hover.corner_radius_top_left = 8
	cont_style_hover.corner_radius_top_right = 8
	cont_style_hover.corner_radius_bottom_left = 8
	cont_style_hover.corner_radius_bottom_right = 8
	_continue_button.add_theme_stylebox_override("hover", cont_style_hover)
	_continue_button.add_theme_color_override("font_color", Color.WHITE)
	hbox.add_child(_continue_button)

	# Fly button (right, prominent) — restarts the flight at the chosen airport.
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


## The aircraft currently highlighted in the list (Settings → Aircraft tab).
func get_selected_aircraft_id() -> String:
	return AIRCRAFT[_selected_aircraft_idx]["id"] if _selected_aircraft_idx >= 0 and _selected_aircraft_idx < AIRCRAFT.size() else "c172p"


static func aircraft_label(id: String) -> String:
	for a in AIRCRAFT:
		if a["id"] == id:
			return a["label"]
	return id


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
		var marker := _availability_marker(ap)
		var text = "%s  %s  %s  %s  %dm" % [marker, icao, ap_name, type_str, elev]
		var idx := _airport_list.add_item(text)
		if marker == "\u2717":
			_airport_list.set_item_custom_fg_color(idx, Color(0.55, 0.55, 0.58))


## Availability marker derived from the installed scenery base packs.
## Mirrors AirportsLib.availability_marker() but kept local so the welcome
## screen does not need to import the lib at parse time.
func _availability_marker(airport: Dictionary) -> String:
	var region := str(airport.get("region", ""))
	if region == "":
		return "  "
	if _installed_regions.has(region):
		return "\u2713"
	return "\u2717"


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
				# Spawn heading is the runway's TRUE bearing: the world is ECEF
				# (design §9.5) and the aircraft is oriented against true north.
				# Thresholds are still computed in the LV95 grid (AirportsLib).
				var hdg: float = float(rwy.get("heading_deg", 0))
				if i_part == 1:
					hdg = fmod(hdg + 180.0, 360.0)
				entries.append({
					"swiss_e": thresh.get("e", ap.get("center_e", 0)),
					"swiss_n": thresh.get("n", ap.get("center_n", 0)),
					"heading_deg": hdg,
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


func _on_about_pressed() -> void:
	_show_about_dialog()


## Build and show the About dialog. Lists the third-party components,
## credits, license, and a use-at-your-own-risk disclaimer.
func _show_about_dialog() -> void:
	var dlg := AcceptDialog.new()
	dlg.title = "About ProVPilot"
	dlg.min_size = Vector2(760, 640)
	dlg.dialog_hide_on_ok = true
	dlg.confirmed.connect(func(): dlg.queue_free())
	dlg.close_requested.connect(func(): dlg.queue_free())

	var rtl := RichTextLabel.new()
	rtl.bbcode_enabled = true
	rtl.fit_content = false
	rtl.scroll_active = true
	rtl.custom_minimum_size = Vector2(720, 560)
	rtl.add_theme_font_size_override("normal_font_size", FONT_SM)
	rtl.add_theme_font_size_override("bold_font_size", FONT_SM)
	rtl.meta_clicked.connect(func(meta): OS.shell_open(String(meta)))

	var ver: String = String(ProjectSettings.get_setting("application/config/version", "0.0.0"))
	rtl.text = """[center][b]ProVPilot v%s[/b]
Swiss Flight Simulator — © 2025 Gerhard Gubler[/center]

[b]Built with[/b]
• [url=https://godotengine.org]Godot Engine[/url] 4.6 (custom [i]4.6-macdev[/i] fork, double precision) — MIT
• [url=https://jsbsim.sourceforge.net]JSBSim[/url] flight dynamics model — LGPL 2.1
• [url=https://gitlab.md80.ch/gery/scenery3d]Scenery3D[/url] GDExtension — MIT

[b]Data sources[/b]
• Switzerland terrain, orthophotos, buildings, roads, water: [url=https://www.swisstopo.admin.ch]swisstopo[/url] (swissALTI3D, SWISSIMAGE, swissTLM3D) — [url=https://www.swisstopo.admin.ch/en/terms-of-use-free-geodata-and-geoservices]free geodata licence[/url]
• Baden-Württemberg terrain & orthophotos: [url=https://www.lgl-bw.de]LGL Baden-Württemberg[/url] — [url=https://www.govdata.de/dl-de/by-2-0]dl-de/by-2-0[/url] (Datenquelle: LGL, www.lgl-bw.de)
• Airport & runway data: [url=https://www.openstreetmap.org]OpenStreetMap[/url] contributors — [url=https://www.openstreetmap.org/copyright]ODbL[/url]

[b]3D models[/b]
• Cessna 172: [url=https://sketchfab.com/3d-models/cessna172-d1b15841c29c43d0862667300bad55a4]"Cessna172"[/url] by [url=https://sketchfab.com/ioai25312]KOG_THORNS[/url] on Sketchfab — [url=http://creativecommons.org/licenses/by/4.0/]CC-BY-4.0[/url]

[b]ProVPilot license[/b]
Released under the [url=https://opensource.org/license/mit]MIT License[/url]. See [code]LICENSE[/code] in the source repository.

[b]Disclaimer[/b]
ProVPilot is provided "AS IS", without warranty of any kind. It is intended for entertainment and educational purposes only. It is [b]not[/b] certified for flight training and must [b]not[/b] be used for real-world navigation, flight planning, or any safety-critical purpose. Use at your own risk.""" % ver

	dlg.add_child(rtl)
	add_child(dlg)
	dlg.popup_centered()


func _on_continue_pressed() -> void:
	continue_requested.emit()
	hide_welcome()


func _on_quit_pressed() -> void:
	get_tree().quit()


## Allow Enter to start flight, Escape does nothing on welcome screen
func _input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER:
			# If Continue is available, prefer resuming the existing flight.
			if _continue_button and _continue_button.visible and not _continue_button.disabled:
				_on_continue_pressed()
			else:
				_on_fly_pressed()
			get_viewport().set_input_as_handled()
		elif event.keycode == KEY_ESCAPE:
			# Escape on the welcome screen resumes the flight when possible.
			if _continue_button and _continue_button.visible and not _continue_button.disabled:
				_on_continue_pressed()
				get_viewport().set_input_as_handled()


## Block all unhandled input from reaching the sim behind the overlay
func _unhandled_input(_event: InputEvent) -> void:
	if visible:
		get_viewport().set_input_as_handled()


# ── Update check ──────────────────────────────────────────────────────
# Fires an HTTP GET against UPDATE_LATEST_URL. On success, compares the
# remote `version` field with ProjectSettings application/config/version.
# If newer, shows a clickable banner above the title. Failures are silent.

func _check_for_updates() -> void:
	if _update_request != null:
		return
	_update_request = HTTPRequest.new()
	add_child(_update_request)
	_update_request.timeout = 5.0
	_update_request.request_completed.connect(_on_update_request_completed)
	var err := _update_request.request(UPDATE_LATEST_URL)
	if err != OK:
		_update_request.queue_free()
		_update_request = null


func _on_update_request_completed(result: int, response_code: int, _headers: PackedStringArray, body: PackedByteArray) -> void:
	if _update_request:
		_update_request.queue_free()
		_update_request = null
	if result != HTTPRequest.RESULT_SUCCESS or response_code != 200:
		return
	var json := JSON.new()
	if json.parse(body.get_string_from_utf8()) != OK:
		return
	var data: Dictionary = json.data if json.data is Dictionary else {}
	var remote_version: String = String(data.get("version", ""))
	if remote_version.is_empty():
		return
	var current_version: String = String(ProjectSettings.get_setting("application/config/version", "0.0.0"))
	if not _is_remote_newer(remote_version, current_version):
		return
	var macos_arm64: Dictionary = data.get("macos_arm64", {})
	var rel_url: String = String(macos_arm64.get("url", ""))
	if rel_url.is_empty():
		return
	_latest_download_url = rel_url if rel_url.begins_with("http") else (UPDATE_BASE_URL + rel_url)
	if _update_label:
		_update_label.text = "[center]Update available: ProVPilot %s — [url=download]Download[/url][/center]" % remote_version
		_update_label.visible = true


func _is_remote_newer(remote: String, current: String) -> bool:
	var r := _parse_version(remote)
	var c := _parse_version(current)
	for i in range(max(r.size(), c.size())):
		var rv: int = r[i] if i < r.size() else 0
		var cv: int = c[i] if i < c.size() else 0
		if rv > cv:
			return true
		if rv < cv:
			return false
	return false


func _parse_version(v: String) -> Array[int]:
	var parts: PackedStringArray = v.split(".")
	var out: Array[int] = []
	for p in parts:
		out.append(int(p))
	return out


func _on_update_meta_clicked(_meta: Variant) -> void:
	if _latest_download_url.is_empty():
		return
	OS.shell_open(_latest_download_url)


# ── First-run wizard ──────────────────────────────────────────────────
# Shown once, when InstallRoot reports the user has never configured a scenery
# location. Emits `scenery_setup_requested` with the chosen folder; node_3d.gd
# imports the folder into the settings scenery library and re-applies
# sources to the Scenery3D node so tile loading actually starts.

var _first_run_dialog: AcceptDialog = null
var _first_run_path_edit: LineEdit = null

func _maybe_show_first_run_wizard() -> void:
	# Skip if InstallRoot already knows where scenery lives, or the user has
	# any scenery_areas configured (the settings library takes precedence).
	# The latter check is best-effort via parent's _settings (SettingsStore).
	if not InstallRoot.is_first_run():
		return
	if _has_scenery_areas_configured():
		return
	_show_first_run_dialog()


func _has_scenery_areas_configured() -> bool:
	var parent := get_parent()
	if parent == null or not parent.has_method("get"):
		return false
	var s = parent.get("_settings") if "_settings" in parent else null
	if s == null or not s.has_method("get_scenery_areas"):
		return false
	var areas: Array = s.get_scenery_areas()
	return areas.size() > 0


func _show_first_run_dialog() -> void:
	if _first_run_dialog != null and is_instance_valid(_first_run_dialog):
		_first_run_dialog.popup_centered()
		return
	_first_run_dialog = AcceptDialog.new()
	_first_run_dialog.title = "Welcome to ProVPilot"
	_first_run_dialog.ok_button_text = "Use This Folder"
	_first_run_dialog.add_cancel_button("Skip for Now")
	_first_run_dialog.unresizable = false
	_first_run_dialog.min_size = Vector2i(700, 280)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 12)
	_first_run_dialog.add_child(vbox)

	var intro := Label.new()
	intro.text = "ProVPilot needs to know where your scenery library is located.\n\nPick a folder that contains region subfolders such as 'Switzerland/' or 'Baden-Wuertemberg/'. You can change or add more folders later in Settings."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.add_theme_font_size_override("font_size", FONT_MD)
	intro.custom_minimum_size = Vector2(660, 0)
	vbox.add_child(intro)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	vbox.add_child(row)

	_first_run_path_edit = LineEdit.new()
	_first_run_path_edit.placeholder_text = "/path/to/scenery"
	_first_run_path_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_first_run_path_edit.add_theme_font_size_override("font_size", FONT_MD)
	_first_run_path_edit.custom_minimum_size.y = 40
	# Pre-fill with a sensible default — first existing candidate.
	_first_run_path_edit.text = _first_run_default_path()
	row.add_child(_first_run_path_edit)

	var browse_btn := Button.new()
	browse_btn.text = "Browse…"
	browse_btn.add_theme_font_size_override("font_size", FONT_MD)
	browse_btn.pressed.connect(_on_first_run_browse_pressed)
	row.add_child(browse_btn)

	var hint := Label.new()
	hint.text = "Tip: Place the 'scenery/' folder next to ProVPilot.app for a portable install — the choice is saved to provpilot.cfg in the same folder."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.add_theme_font_size_override("font_size", FONT_SM)
	hint.add_theme_color_override("font_color", Color(0.6, 0.65, 0.7))
	hint.custom_minimum_size = Vector2(660, 0)
	vbox.add_child(hint)

	_first_run_dialog.confirmed.connect(_on_first_run_confirmed)
	_first_run_dialog.canceled.connect(_on_first_run_canceled)
	add_child(_first_run_dialog)
	_first_run_dialog.popup_centered()


func _first_run_default_path() -> String:
	for candidate in InstallRoot.get_default_scan_roots():
		if DirAccess.dir_exists_absolute(candidate):
			return candidate
	# Fall back to install_root/scenery even if it doesn't exist yet — the user
	# can edit before confirming.
	return InstallRoot.get_install_root().path_join("scenery")


func _on_first_run_browse_pressed() -> void:
	var dlg := FileDialog.new()
	dlg.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	dlg.access = FileDialog.ACCESS_FILESYSTEM
	dlg.use_native_dialog = true
	dlg.title = "Select Scenery Folder"
	add_child(dlg)
	dlg.dir_selected.connect(func(path: String) -> void:
		if _first_run_path_edit:
			_first_run_path_edit.text = path
		dlg.queue_free()
	)
	dlg.canceled.connect(func() -> void: dlg.queue_free())
	dlg.popup_centered_ratio(0.7)


func _on_first_run_confirmed() -> void:
	var path: String = (_first_run_path_edit.text if _first_run_path_edit else "").strip_edges()
	if path.is_empty():
		return
	if not DirAccess.dir_exists_absolute(path):
		# Show a quick error and bring the dialog back.
		var err := AcceptDialog.new()
		err.title = "Folder Not Found"
		err.dialog_text = "The folder does not exist:\n%s" % path
		add_child(err)
		err.confirmed.connect(func() -> void:
			err.queue_free()
			_show_first_run_dialog()
		)
		err.popup_centered()
		return
	# Persist to provpilot.cfg next to the .app
	InstallRoot.save_scenery_choice(path)
	# Hand off to node_3d.gd to populate the settings library + apply sources.
	scenery_setup_requested.emit(path)


func _on_first_run_canceled() -> void:
	# User chose to skip — they'll get the standard "0/5 tiles" stall but at
	# least they can reach Settings to configure manually.
	print("WelcomeScreen: first-run wizard skipped")
