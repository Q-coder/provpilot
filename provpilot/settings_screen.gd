extends CanvasLayer
## SettingsScreen — Configuration for paths, graphics, sound, and realism.
##
## Opened from the welcome screen or in-flight via menu.
## Settings are persisted to user://settings.cfg.

signal closed()

# ── Constants ──────────────────────────────────────────────────────────
const FONT_SM := 20
const FONT_MD := 24
const FONT_LG := 28
const FONT_XL := 36

const BG_COLOR := Color(0.08, 0.10, 0.14, 1.0)
const PANEL_COLOR := Color(0.12, 0.14, 0.18, 1.0)
const SECTION_COLOR := Color(0.25, 0.55, 0.85, 1.0)
const LABEL_COLOR := Color(0.75, 0.78, 0.82)

const CONFIG_PATH := "user://settings.cfg"

# ── Settings state ─────────────────────────────────────────────────────
var settings := {
	# Paths
	"scenery_path": "scenery",
	"jsbsim_path": "jsbsim",
	# Scenery library (FSX-style)
	# scenery_areas is an Array of Dictionaries:
	#   {name: String, path: String, enabled: bool, priority: int}
	# Lower priority number = checked first (1 = highest, like FSX).
	# Each path is a *region* directory; asset subdirs (terrain/, orthophoto/,
	# buildings/, water/, roads/, vegetation/) inside it are auto-detected.
	"scenery_areas": [],
	# Roots scanned by "Scan for Scenery" button.
	# Populated at runtime from InstallRoot (next to the .app, $HOME, etc.).
	"scenery_scan_roots": PackedStringArray(),
	# Per-pack install state for the in-app Scenery Downloader (see
	# scenery_downloader.gd). Keyed by pack_id; value = {version, sha256, size,
	# path, installed_at}.
	"installed_scenery_packs": {},
	"skip_white_pixels": true,
	# Graphics
	"render_distance": 8,      # terrain load radius in tiles
	"building_distance": 3,    # building load radius in tiles
	"shadow_quality": 2,       # 0=off, 1=low, 2=medium, 3=high
	"tree_density": 2,         # 0=off, 1=low(500 m), 2=medium(1500 m), 3=high(3000 m)
	"vsync": true,
	"fullscreen": true,
	# Sound
	"master_volume": 80,
	"engine_volume": 70,
	"wind_volume": 50,
	"ui_volume": 60,
	# Realism
	"auto_mixture": true,
	"auto_carb_heat": true,
	"unlimited_fuel": true,
	"crash_detection": false,
	"turbulence": 1,           # 0=none, 1=light, 2=moderate, 3=severe
	"wind_speed_kts": 0,
	"wind_direction_deg": 0,
}

# UI references for reading back values
var _controls := {}
var _bg: ColorRect
var _main_panel: PanelContainer


func _ready() -> void:
	layer = 101  # Above welcome screen
	visible = false
	_build_ui()
	_load_settings()
	_apply_to_ui()


# ── Public API ─────────────────────────────────────────────────────────

func show_settings() -> void:
	_apply_to_ui()
	visible = true


func hide_settings() -> void:
	visible = false


func get_setting(key: String):
	return settings.get(key)


# ── UI Construction ────────────────────────────────────────────────────

func _build_ui() -> void:
	_bg = ColorRect.new()
	_bg.color = BG_COLOR
	_bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_bg)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 32)
	margin.add_theme_constant_override("margin_right", 32)
	margin.add_theme_constant_override("margin_top", 32)
	margin.add_theme_constant_override("margin_bottom", 32)
	_bg.add_child(margin)

	var panel = PanelContainer.new()
	var style = StyleBoxFlat.new()
	style.bg_color = PANEL_COLOR
	style.corner_radius_top_left = 16
	style.corner_radius_top_right = 16
	style.corner_radius_bottom_left = 16
	style.corner_radius_bottom_right = 16
	style.content_margin_left = 48
	style.content_margin_right = 48
	style.content_margin_top = 36
	style.content_margin_bottom = 36
	panel.add_theme_stylebox_override("panel", style)
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_main_panel = panel
	margin.add_child(panel)

	var main_vbox = VBoxContainer.new()
	main_vbox.add_theme_constant_override("separation", 16)
	panel.add_child(main_vbox)

	# Title
	var title = Label.new()
	title.text = "Settings"
	title.add_theme_font_size_override("font_size", FONT_XL)
	title.add_theme_color_override("font_color", SECTION_COLOR)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	main_vbox.add_child(title)

	main_vbox.add_child(HSeparator.new())

	# Scrollable content
	var scroll = ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	main_vbox.add_child(scroll)

	var content = VBoxContainer.new()
	content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	content.add_theme_constant_override("separation", 8)
	scroll.add_child(content)

	# ── Sections ───────────────────────────────────────────────────────
	_build_paths_section(content)
	content.add_child(HSeparator.new())
	_build_scenery_sources_section(content)
	content.add_child(HSeparator.new())
	_build_scenery_downloader_section(content)
	content.add_child(HSeparator.new())
	_build_graphics_section(content)
	content.add_child(HSeparator.new())
	_build_sound_section(content)
	content.add_child(HSeparator.new())
	_build_realism_section(content)
	content.add_child(HSeparator.new())
	_build_joystick_section(content)

	# ── Bottom buttons ─────────────────────────────────────────────────
	main_vbox.add_child(HSeparator.new())

	var btn_hbox = HBoxContainer.new()
	btn_hbox.alignment = BoxContainer.ALIGNMENT_END
	btn_hbox.add_theme_constant_override("separation", 16)
	main_vbox.add_child(btn_hbox)

	var defaults_btn = Button.new()
	defaults_btn.text = "Restore Defaults"
	defaults_btn.custom_minimum_size = Vector2(220, 48)
	defaults_btn.add_theme_font_size_override("font_size", FONT_MD)
	defaults_btn.pressed.connect(_on_restore_defaults)
	btn_hbox.add_child(defaults_btn)

	var spacer = Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	btn_hbox.add_child(spacer)

	var cancel_btn = Button.new()
	cancel_btn.text = "Cancel"
	cancel_btn.custom_minimum_size = Vector2(160, 48)
	cancel_btn.add_theme_font_size_override("font_size", FONT_MD)
	cancel_btn.pressed.connect(_on_cancel)
	btn_hbox.add_child(cancel_btn)

	var save_btn = Button.new()
	save_btn.text = "Save"
	save_btn.custom_minimum_size = Vector2(160, 48)
	save_btn.add_theme_font_size_override("font_size", FONT_MD)
	save_btn.pressed.connect(_on_save)

	var save_style = StyleBoxFlat.new()
	save_style.bg_color = SECTION_COLOR
	save_style.corner_radius_top_left = 6
	save_style.corner_radius_top_right = 6
	save_style.corner_radius_bottom_left = 6
	save_style.corner_radius_bottom_right = 6
	save_btn.add_theme_stylebox_override("normal", save_style)
	save_btn.add_theme_color_override("font_color", Color.WHITE)

	btn_hbox.add_child(save_btn)


# ── Section Builders ───────────────────────────────────────────────────

func _add_section_header(parent: VBoxContainer, text: String) -> void:
	var label = Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", FONT_LG)
	label.add_theme_color_override("font_color", SECTION_COLOR)
	parent.add_child(label)


func _add_path_row(parent: VBoxContainer, key: String, label_text: String, placeholder: String) -> void:
	var hbox = HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 12)

	var label = Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 200
	label.add_theme_font_size_override("font_size", FONT_MD)
	label.add_theme_color_override("font_color", LABEL_COLOR)
	hbox.add_child(label)

	var input = LineEdit.new()
	input.placeholder_text = placeholder
	input.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	input.add_theme_font_size_override("font_size", FONT_MD)
	input.custom_minimum_size.y = 40
	hbox.add_child(input)

	_controls[key] = input
	parent.add_child(hbox)


func _add_slider_row(parent: VBoxContainer, key: String, label_text: String, min_val: float, max_val: float, step: float, suffix: String = "") -> void:
	var hbox = HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 12)

	var label = Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 200
	label.add_theme_font_size_override("font_size", FONT_MD)
	label.add_theme_color_override("font_color", LABEL_COLOR)
	hbox.add_child(label)

	var slider = HSlider.new()
	slider.min_value = min_val
	slider.max_value = max_val
	slider.step = step
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size = Vector2(300, 32)
	hbox.add_child(slider)

	var value_label = Label.new()
	value_label.custom_minimum_size.x = 70
	value_label.add_theme_font_size_override("font_size", FONT_SM)
	value_label.add_theme_color_override("font_color", LABEL_COLOR)
	hbox.add_child(value_label)

	slider.value_changed.connect(func(val: float):
		value_label.text = "%d%s" % [int(val), suffix]
	)

	_controls[key] = slider
	_controls[key + "_label"] = value_label
	parent.add_child(hbox)


func _add_check_row(parent: VBoxContainer, key: String, label_text: String, description: String = "") -> void:
	var hbox = HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 12)

	var label = Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 200
	label.add_theme_font_size_override("font_size", FONT_MD)
	label.add_theme_color_override("font_color", LABEL_COLOR)
	hbox.add_child(label)

	var check = CheckBox.new()
	check.add_theme_font_size_override("font_size", FONT_MD)
	hbox.add_child(check)

	if description != "":
		var desc = Label.new()
		desc.text = description
		desc.add_theme_font_size_override("font_size", FONT_SM)
		desc.add_theme_color_override("font_color", Color(0.5, 0.5, 0.5))
		hbox.add_child(desc)

	_controls[key] = check
	parent.add_child(hbox)


func _add_option_row(parent: VBoxContainer, key: String, label_text: String, options: Array) -> void:
	var hbox = HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 12)

	var label = Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 200
	label.add_theme_font_size_override("font_size", FONT_MD)
	label.add_theme_color_override("font_color", LABEL_COLOR)
	hbox.add_child(label)

	var option = OptionButton.new()
	option.add_theme_font_size_override("font_size", FONT_MD)
	option.custom_minimum_size = Vector2(200, 40)
	var popup = option.get_popup()
	popup.add_theme_font_size_override("font_size", FONT_MD)
	for opt in options:
		option.add_item(opt)
	hbox.add_child(option)

	_controls[key] = option
	parent.add_child(hbox)


# ── Section: Paths ─────────────────────────────────────────────────────

func _build_paths_section(parent: VBoxContainer) -> void:
	_add_section_header(parent, "Paths")
	_add_path_row(parent, "scenery_path", "Scenery Data:", "scenery")
	_add_path_row(parent, "jsbsim_path", "JSBSim Data:", "jsbsim")


# ── Section: Scenery Library (FSX-style) ──────────────────────────────────

# Asset subdirs auto-detected inside each scenery area's path.
const ASSET_SUBDIRS := ["terrain", "orthophoto", "buildings", "water", "roads", "vegetation", "trees", "airports"]


func _build_scenery_sources_section(parent: VBoxContainer) -> void:
	_add_section_header(parent, "Scenery Library")

	var info = Label.new()
	info.text = "Available scenery areas. Lower priority = checked first (1 = highest). Disabled areas are skipped. Multiple enabled areas with the same asset type act as fallbacks (e.g. BW orthophotos fill SWISSIMAGE white pixels at the CH/DE border)."
	info.add_theme_font_size_override("font_size", FONT_SM)
	info.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	info.autowrap_mode = TextServer.AUTOWRAP_WORD
	parent.add_child(info)

	# Scrollable table of scenery areas
	var table_scroll := ScrollContainer.new()
	table_scroll.custom_minimum_size = Vector2(0, 260)
	table_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	parent.add_child(table_scroll)

	var table := VBoxContainer.new()
	table.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	table.add_theme_constant_override("separation", 2)
	table_scroll.add_child(table)
	_controls["scenery_table"] = table

	# Header row
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 8)
	_make_table_label(header, "On", 50)
	_make_table_label(header, "Pri", 50)
	_make_table_label(header, "Name", 220)
	_make_table_label(header, "Assets", 240)
	_make_table_label(header, "Path", 520)
	table.add_child(header)
	table.add_child(HSeparator.new())

	# Action buttons
	var btn_row := HBoxContainer.new()
	btn_row.add_theme_constant_override("separation", 12)
	parent.add_child(btn_row)

	var scan_btn := Button.new()
	scan_btn.text = "Scan for Scenery"
	scan_btn.custom_minimum_size = Vector2(200, 36)
	scan_btn.add_theme_font_size_override("font_size", FONT_SM)
	scan_btn.pressed.connect(_on_scan_scenery)
	btn_row.add_child(scan_btn)

	var add_btn := Button.new()
	add_btn.text = "Add Area..."
	add_btn.custom_minimum_size = Vector2(140, 36)
	add_btn.add_theme_font_size_override("font_size", FONT_SM)
	add_btn.pressed.connect(_on_add_scenery_area)
	btn_row.add_child(add_btn)

	var up_btn := Button.new()
	up_btn.text = "Move Up"
	up_btn.custom_minimum_size = Vector2(120, 36)
	up_btn.add_theme_font_size_override("font_size", FONT_SM)
	up_btn.pressed.connect(_on_move_selected_up)
	btn_row.add_child(up_btn)

	var down_btn := Button.new()
	down_btn.text = "Move Down"
	down_btn.custom_minimum_size = Vector2(120, 36)
	down_btn.add_theme_font_size_override("font_size", FONT_SM)
	down_btn.pressed.connect(_on_move_selected_down)
	btn_row.add_child(down_btn)

	var del_btn := Button.new()
	del_btn.text = "Remove"
	del_btn.custom_minimum_size = Vector2(120, 36)
	del_btn.add_theme_font_size_override("font_size", FONT_SM)
	del_btn.pressed.connect(_on_remove_selected)
	btn_row.add_child(del_btn)

	parent.add_child(Control.new())
	_add_check_row(parent, "skip_white_pixels", "Skip White Pixels:", "Treat pure white (255,255,255) ortho pixels as transparent so lower-priority sources fill in (CH/DE border fix)")


func _make_table_label(parent: HBoxContainer, text: String, min_w: int) -> Label:
	var l := Label.new()
	l.text = text
	l.custom_minimum_size.x = min_w
	l.add_theme_font_size_override("font_size", FONT_SM)
	l.add_theme_color_override("font_color", LABEL_COLOR)
	parent.add_child(l)
	return l


# Currently selected row index in the scenery table (-1 = none).
var _selected_area_idx: int = -1


func _refresh_scenery_table() -> void:
	if not _controls.has("scenery_table"):
		return
	var table: VBoxContainer = _controls["scenery_table"]
	# Remove existing data rows (keep header at index 0 + separator at 1)
	var children := table.get_children()
	for i in range(children.size() - 1, 1, -1):
		children[i].queue_free()

	var areas: Array = settings.get("scenery_areas", [])
	for i in range(areas.size()):
		var area: Dictionary = areas[i]
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 8)

		# Selection background (highlight if selected)
		var bg := ColorRect.new()
		bg.color = SECTION_COLOR if i == _selected_area_idx else Color(0, 0, 0, 0)
		bg.color.a = 0.25 if i == _selected_area_idx else 0.0
		bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
		bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		row.add_child(bg)

		# Click-to-select
		var click_btn := Button.new()
		click_btn.flat = true
		click_btn.toggle_mode = false
		click_btn.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		click_btn.mouse_filter = Control.MOUSE_FILTER_PASS
		var idx_capture := i
		click_btn.pressed.connect(func() -> void:
			_selected_area_idx = idx_capture
			_refresh_scenery_table()
		)
		# (we add it last so checkbox/spinbox stay clickable above it)

		# Enabled checkbox
		var cb := CheckBox.new()
		cb.button_pressed = bool(area.get("enabled", true))
		cb.custom_minimum_size = Vector2(50, 28)
		cb.toggled.connect(func(val: bool) -> void:
			var arr: Array = settings.get("scenery_areas", [])
			if idx_capture < arr.size():
				arr[idx_capture]["enabled"] = val
				settings["scenery_areas"] = arr
		)
		row.add_child(cb)

		# Priority spinbox
		var pri := SpinBox.new()
		pri.min_value = 1
		pri.max_value = 999
		pri.step = 1
		pri.value = int(area.get("priority", i + 1))
		pri.custom_minimum_size = Vector2(50, 28)
		pri.value_changed.connect(func(val: float) -> void:
			var arr: Array = settings.get("scenery_areas", [])
			if idx_capture < arr.size():
				arr[idx_capture]["priority"] = int(val)
				settings["scenery_areas"] = arr
		)
		row.add_child(pri)

		# Name
		var name_lbl := Label.new()
		name_lbl.text = str(area.get("name", "?"))
		name_lbl.custom_minimum_size.x = 220
		name_lbl.add_theme_font_size_override("font_size", FONT_SM)
		row.add_child(name_lbl)

		# Detected assets
		var assets_lbl := Label.new()
		assets_lbl.text = ", ".join(_detect_assets(area.get("path", "")))
		assets_lbl.custom_minimum_size.x = 240
		assets_lbl.add_theme_font_size_override("font_size", FONT_SM)
		assets_lbl.add_theme_color_override("font_color", Color(0.55, 0.78, 0.55))
		row.add_child(assets_lbl)

		# Path — shown with $HOME collapsed to ~ and middle-elided so the
		# tail (the distinguishing component) is always visible. Full path
		# remains in the tooltip on hover.
		var path_lbl := Label.new()
		var p: String = str(area.get("path", ""))
		path_lbl.text = _format_path_for_display(p, 60)
		path_lbl.tooltip_text = p
		path_lbl.custom_minimum_size.x = 520
		path_lbl.add_theme_font_size_override("font_size", FONT_SM)
		path_lbl.add_theme_color_override("font_color", Color(0.55, 0.55, 0.55))
		path_lbl.clip_text = true
		path_lbl.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		path_lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(path_lbl)

		# Highlight selected row via stylebox
		if i == _selected_area_idx:
			var sel := Button.new()
			sel.text = "●"
			sel.flat = true
			sel.add_theme_color_override("font_color", SECTION_COLOR)
			sel.custom_minimum_size = Vector2(20, 28)
			row.add_child(sel)

		# Click-anywhere selector (transparent button overlay on path label)
		path_lbl.gui_input.connect(func(ev: InputEvent) -> void:
			if ev is InputEventMouseButton and ev.pressed and ev.button_index == MOUSE_BUTTON_LEFT:
				_selected_area_idx = idx_capture
				_refresh_scenery_table()
		)
		name_lbl.gui_input.connect(func(ev: InputEvent) -> void:
			if ev is InputEventMouseButton and ev.pressed and ev.button_index == MOUSE_BUTTON_LEFT:
				_selected_area_idx = idx_capture
				_refresh_scenery_table()
		)

		table.add_child(row)


func _detect_assets(path: String) -> PackedStringArray:
	var out := PackedStringArray()
	if path.is_empty():
		return out
	var dir := DirAccess.open(path)
	if dir == null:
		return out
	for sub in ASSET_SUBDIRS:
		if dir.dir_exists(sub):
			out.append(sub)
	return out


# Format a filesystem path for compact display:
#   - Collapse $HOME to "~" so /Users/gery/foo → ~/foo
#   - Case-collide aware: keep the path's original casing intact so
#     `/Users/gery/provpilot/...` vs `/Users/gery/ProvPilot/...` remain
#     distinguishable.
#   - If the result still exceeds `max_chars`, elide the middle, keeping
#     the first 2 components and the last 3 (e.g. ~/.../scenery/Germany/BW).
func _format_path_for_display(path: String, max_chars: int) -> String:
	if path.is_empty():
		return ""
	var p := path
	var home := OS.get_environment("HOME")
	if home != "" and p.begins_with(home + "/"):
		p = "~" + p.substr(home.length())
	elif home != "" and p == home:
		p = "~"
	if p.length() <= max_chars:
		return p
	var parts := p.split("/", false)
	if parts.size() <= 5:
		return p
	var head := "/".join([parts[0], parts[1]])
	var tail := "/".join([parts[parts.size() - 3], parts[parts.size() - 2], parts[parts.size() - 1]])
	# Restore leading slash if absolute path.
	if p.begins_with("/"):
		head = "/" + head
	return "%s/…/%s" % [head, tail]


func _is_scenery_area_dir(path: String) -> bool:
	return _detect_assets(path).size() > 0


func _on_scan_scenery() -> void:
	var roots: PackedStringArray = settings.get("scenery_scan_roots", PackedStringArray())
	var found: Array = []
	for root in roots:
		_scan_dir_recursive(root, 0, 3, found)

	# Merge with existing entries (preserve enabled/priority for known paths)
	var existing: Array = settings.get("scenery_areas", []).duplicate()
	var existing_paths := {}
	for a in existing:
		existing_paths[a.get("path", "")] = true

	# Sort newly found paths by asset count DESC so a "complete" region (e.g.
	# Switzerland with terrain+ortho+buildings+water+roads+vegetation+trees+
	# airports) gets priority 1, before a partial region (e.g. BW with only
	# a terrain dir). This is critical because the aircraft's spawn region
	# must be in the priority-1 path or tile loading silently fails.
	var new_paths: Array = []
	for path in found:
		if existing_paths.has(path):
			continue
		new_paths.append({
			"path": path,
			"asset_count": _detect_assets(path).size(),
		})
	new_paths.sort_custom(func(a, b): return int(a["asset_count"]) > int(b["asset_count"]))

	var added := 0
	var next_priority := existing.size() + 1
	for entry in new_paths:
		var path: String = entry["path"]
		var nice_name := _name_from_path(path)
		existing.append({
			"name": nice_name,
			"path": path,
			"enabled": true,
			"priority": next_priority,
		})
		next_priority += 1
		added += 1

	# Sort by priority for display
	existing.sort_custom(func(a, b): return int(a.get("priority", 999)) < int(b.get("priority", 999)))
	settings["scenery_areas"] = existing
	print("Scenery scan: %d new area(s) added (%d total)" % [added, existing.size()])
	_refresh_scenery_table()


func _scan_dir_recursive(path: String, depth: int, max_depth: int, found: Array) -> void:
	if depth > max_depth:
		return
	var dir := DirAccess.open(path)
	if dir == null:
		return
	# If this dir IS a scenery area, record it and stop descending.
	if _is_scenery_area_dir(path):
		found.append(path)
		return
	# Otherwise recurse into subdirs.
	dir.list_dir_begin()
	var name := dir.get_next()
	while name != "":
		if name.begins_with(".") or not dir.current_is_dir():
			name = dir.get_next()
			continue
		_scan_dir_recursive(path.path_join(name), depth + 1, max_depth, found)
		name = dir.get_next()
	dir.list_dir_end()


func _name_from_path(path: String) -> String:
	# Use parent + leaf for disambiguation (e.g. "Germany/Baden-Wuertemberg")
	var leaf := path.get_file()
	var parent := path.get_base_dir().get_file()
	if parent.is_empty() or parent == "scenery":
		return leaf
	return "%s/%s" % [parent, leaf]


func _on_add_scenery_area() -> void:
	var dialog := FileDialog.new()
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.use_native_dialog = true
	dialog.title = "Select Scenery Area Directory"
	add_child(dialog)
	dialog.dir_selected.connect(func(path: String) -> void:
		_add_scenery_area(path)
		dialog.queue_free()
	)
	dialog.canceled.connect(func() -> void: dialog.queue_free())
	dialog.popup_centered()


func _add_scenery_area(path: String) -> void:
	var areas: Array = settings.get("scenery_areas", []).duplicate()
	for a in areas:
		if a.get("path", "") == path:
			print("Scenery area already in library: %s" % path)
			return
	if not _is_scenery_area_dir(path):
		print("WARNING: %s does not contain any known asset subdirs (%s) — adding anyway" % [path, ", ".join(ASSET_SUBDIRS)])
	areas.append({
		"name": _name_from_path(path),
		"path": path,
		"enabled": true,
		"priority": areas.size() + 1,
	})
	settings["scenery_areas"] = areas
	_refresh_scenery_table()


func _on_remove_selected() -> void:
	var areas: Array = settings.get("scenery_areas", []).duplicate()
	if _selected_area_idx < 0 or _selected_area_idx >= areas.size():
		return
	areas.remove_at(_selected_area_idx)
	settings["scenery_areas"] = areas
	_selected_area_idx = -1
	_refresh_scenery_table()


# ── Section: Download Scenery ─────────────────────────────────────────

const SceneryDownloaderScript = preload("res://scenery_downloader.gd")


func _build_scenery_downloader_section(parent: VBoxContainer) -> void:
	var dl = SceneryDownloaderScript.new()
	dl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	# Wire state callbacks so the downloader reads/writes our settings dict
	# without taking a hard dependency on SettingsScreen.
	dl.get_state = func() -> Dictionary:
		var s = settings.get("installed_scenery_packs", {})
		return s if s is Dictionary else {}
	dl.set_state = func(state: Dictionary) -> void:
		settings["installed_scenery_packs"] = state
	dl.on_area_installed = func(path: String) -> void:
		_add_scenery_area(path)
	parent.add_child(dl)
	_controls["scenery_downloader"] = dl
	# Initial install-root suggestion: configured scenery_root, else <install>/scenery.
	var root := InstallRoot.get_scenery_root()
	if root.is_empty():
		root = InstallRoot.get_install_root().path_join("scenery")
	dl.bootstrap(root)


func _on_move_selected_up() -> void:
	_swap_priority(_selected_area_idx, _selected_area_idx - 1)


func _on_move_selected_down() -> void:
	_swap_priority(_selected_area_idx, _selected_area_idx + 1)


func _swap_priority(a_idx: int, b_idx: int) -> void:
	var areas: Array = settings.get("scenery_areas", []).duplicate()
	if a_idx < 0 or a_idx >= areas.size() or b_idx < 0 or b_idx >= areas.size():
		return
	var tmp = areas[a_idx]
	areas[a_idx] = areas[b_idx]
	areas[b_idx] = tmp
	# Renumber priorities to match list order
	for i in range(areas.size()):
		areas[i]["priority"] = i + 1
	settings["scenery_areas"] = areas
	_selected_area_idx = b_idx
	_refresh_scenery_table()


## Public API: returns ordered list of enabled scenery area paths (priority asc).
## Used by node_3d.gd to build asset path arrays for Scenery3D.
func get_enabled_scenery_areas() -> Array:
	var out: Array = []
	var areas: Array = settings.get("scenery_areas", [])
	var sorted := areas.duplicate()
	sorted.sort_custom(func(a, b): return int(a.get("priority", 999)) < int(b.get("priority", 999)))
	for a in sorted:
		if bool(a.get("enabled", true)):
			out.append(a)
	return out


# ── Section: Graphics ──────────────────────────────────────────────────

func _build_graphics_section(parent: VBoxContainer) -> void:
	_add_section_header(parent, "Graphics")
	_add_slider_row(parent, "render_distance", "Terrain Distance:", 2, 16, 1, " tiles")
	_add_slider_row(parent, "building_distance", "Building Distance:", 1, 8, 1, " tiles")
	_add_option_row(parent, "shadow_quality", "Shadow Quality:", ["Off", "Low", "Medium", "High"])
	_add_option_row(parent, "tree_density", "Tree Density:", ["Off", "Low", "Medium", "High", "Very High"])
	_add_check_row(parent, "vsync", "V-Sync:", "Synchronize with display refresh")
	_add_check_row(parent, "fullscreen", "Fullscreen:", "Run in fullscreen mode")


# ── Section: Sound ─────────────────────────────────────────────────────

func _build_sound_section(parent: VBoxContainer) -> void:
	_add_section_header(parent, "Sound")
	_add_slider_row(parent, "master_volume", "Master Volume:", 0, 100, 5, "%")
	_add_slider_row(parent, "engine_volume", "Engine Sound:", 0, 100, 5, "%")
	_add_slider_row(parent, "wind_volume", "Wind Sound:", 0, 100, 5, "%")
	_add_slider_row(parent, "ui_volume", "UI Sounds:", 0, 100, 5, "%")


# ── Section: Realism ───────────────────────────────────────────────────

func _build_realism_section(parent: VBoxContainer) -> void:
	_add_section_header(parent, "Realism")
	_add_check_row(parent, "auto_mixture", "Auto Mixture:", "Automatically manage mixture control")
	_add_check_row(parent, "auto_carb_heat", "Auto Carb Heat:", "Automatically manage carburetor heat")
	_add_check_row(parent, "unlimited_fuel", "Unlimited Fuel:", "Fuel never runs out")
	_add_check_row(parent, "crash_detection", "Crash Detection:", "Enable crash on hard impact")
	_add_option_row(parent, "turbulence", "Turbulence:", ["None", "Light", "Moderate", "Severe"])
	_add_slider_row(parent, "wind_speed_kts", "Wind Speed:", 0, 50, 1, " kts")
	_add_slider_row(parent, "wind_direction_deg", "Wind Direction:", 0, 360, 10, "°")


# ── Section: Joystick ──────────────────────────────────────────────────

var _joystick_panel: Node = null

func _build_joystick_section(parent: VBoxContainer) -> void:
	var panel_script: GDScript = load("res://joystick_settings_panel.gd")
	var panel = VBoxContainer.new()
	panel.set_script(panel_script)
	parent.add_child(panel)
	_joystick_panel = panel


# ── Settings I/O ───────────────────────────────────────────────────────

func _apply_to_ui() -> void:
	for key in settings.keys():
		if not _controls.has(key):
			continue
		var ctrl = _controls[key]
		var val = settings[key]
		if ctrl is LineEdit:
			ctrl.text = str(val)
		elif ctrl is HSlider:
			ctrl.value = float(val)
			var suffix = ""
			if _controls.has(key + "_label"):
				# Determine suffix from existing label text or key name
				if "volume" in key:
					suffix = "%"
				elif "distance" in key:
					suffix = " tiles"
				elif "kts" in key:
					suffix = " kts"
				elif "deg" in key:
					suffix = "°"
				_controls[key + "_label"].text = "%d%s" % [int(val), suffix]
		elif ctrl is CheckBox:
			ctrl.button_pressed = bool(val)
		elif ctrl is OptionButton:
			ctrl.selected = int(val)
	
	# Refresh scenery library table
	if _controls.has("scenery_table"):
		_refresh_scenery_table()


func _read_from_ui() -> void:
	for key in settings.keys():
		if not _controls.has(key):
			continue
		var ctrl = _controls[key]
		if ctrl is LineEdit:
			settings[key] = ctrl.text
		elif ctrl is HSlider:
			settings[key] = int(ctrl.value)
		elif ctrl is CheckBox:
			settings[key] = ctrl.button_pressed
		elif ctrl is OptionButton:
			settings[key] = ctrl.selected


func _save_settings() -> void:
	_read_from_ui()
	var config = ConfigFile.new()
	for key in settings.keys():
		var section = "paths"
		if key in ["scenery_path", "jsbsim_path"]:
			section = "paths"
		elif key in ["scenery_areas", "scenery_scan_roots", "installed_scenery_packs", "skip_white_pixels"]:
			section = "scenery"
		elif key in ["render_distance", "building_distance", "shadow_quality", "tree_density", "vsync", "fullscreen"]:
			section = "graphics"
		elif "volume" in key:
			section = "sound"
		else:
			section = "realism"
		config.set_value(section, key, settings[key])
	config.save(CONFIG_PATH)
	print("Settings saved to %s" % CONFIG_PATH)


func _load_settings() -> void:
	var config = ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		print("SettingsScreen: No saved settings, using defaults")
	else:
		for section in config.get_sections():
			for key in config.get_section_keys(section):
				if settings.has(key):
					settings[key] = config.get_value(section, key)
		print("Settings loaded from %s" % CONFIG_PATH)
	# Ensure scan roots are always seeded from InstallRoot — even if a stale
	# saved config has obsolete dev-machine paths, the user still sees sensible
	# defaults when they click "Scan for Scenery". The user can append their own.
	var default_roots: PackedStringArray = InstallRoot.get_default_scan_roots()
	var saved_roots: PackedStringArray = settings.get("scenery_scan_roots", PackedStringArray())
	var merged := PackedStringArray()
	var seen := {}
	for r in default_roots:
		if not seen.has(r):
			seen[r] = true
			merged.append(r)
	for r in saved_roots:
		if not seen.has(r):
			seen[r] = true
			merged.append(r)
	settings["scenery_scan_roots"] = merged

	# Honor a portable provpilot.cfg living next to the .app: if it specifies a
	# scenery_root and we have no library yet, scan it once and adopt the result.
	var areas: Array = settings.get("scenery_areas", [])
	var portable_root: String = InstallRoot.get_scenery_root()
	if areas.is_empty() and not portable_root.is_empty() and DirAccess.dir_exists_absolute(portable_root):
		print("SettingsScreen: bootstrapping library from provpilot.cfg scenery_root=%s" % portable_root)
		if not merged.has(portable_root):
			merged.append(portable_root)
			settings["scenery_scan_roots"] = merged
		_on_scan_scenery()


func _get_defaults() -> Dictionary:
	return {
		"scenery_path": "scenery",
		"jsbsim_path": "jsbsim",
		"scenery_areas": [],
		"scenery_scan_roots": PackedStringArray(),
		"installed_scenery_packs": {},
		"skip_white_pixels": true,
		"render_distance": 8,
		"building_distance": 3,
		"shadow_quality": 2,
		"tree_density": 2,
		"vsync": true,
		"fullscreen": true,
		"master_volume": 80,
		"engine_volume": 70,
		"wind_volume": 50,
		"ui_volume": 60,
		"auto_mixture": true,
		"auto_carb_heat": true,
		"unlimited_fuel": true,
		"crash_detection": false,
		"turbulence": 1,
		"wind_speed_kts": 0,
		"wind_direction_deg": 0,
	}


# ── Callbacks ──────────────────────────────────────────────────────────

func _on_save() -> void:
	_save_settings()
	if Engine.has_singleton("JoystickConfig") or get_node_or_null("/root/JoystickConfig") != null:
		get_node("/root/JoystickConfig").save()
	hide_settings()
	closed.emit()


func _on_cancel() -> void:
	# Revert UI to last saved state
	_load_settings()
	_apply_to_ui()
	# Reload joystick bindings from disk so unsaved changes are discarded.
	var jc := get_node_or_null("/root/JoystickConfig")
	if jc:
		jc.reset_to_defaults()
		jc._load()
		if _joystick_panel and _joystick_panel.has_method("refresh"):
			_joystick_panel.refresh()
	hide_settings()
	closed.emit()


func _on_restore_defaults() -> void:
	settings = _get_defaults()
	_apply_to_ui()
	var jc := get_node_or_null("/root/JoystickConfig")
	if jc:
		jc.reset_to_defaults()
		if _joystick_panel and _joystick_panel.has_method("refresh"):
			_joystick_panel.refresh()


func _input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_ESCAPE:
			_on_cancel()
			get_viewport().set_input_as_handled()
