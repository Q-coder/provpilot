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
	# Graphics
	"render_distance": 8,      # terrain load radius in tiles
	"building_distance": 3,    # building load radius in tiles
	"shadow_quality": 2,       # 0=off, 1=low, 2=medium, 3=high
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

	var center = CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_bg.add_child(center)

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
	panel.custom_minimum_size = Vector2(1200, 860)
	center.add_child(panel)

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
	_build_graphics_section(content)
	content.add_child(HSeparator.new())
	_build_sound_section(content)
	content.add_child(HSeparator.new())
	_build_realism_section(content)

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


# ── Section: Graphics ──────────────────────────────────────────────────

func _build_graphics_section(parent: VBoxContainer) -> void:
	_add_section_header(parent, "Graphics")
	_add_slider_row(parent, "render_distance", "Terrain Distance:", 2, 16, 1, " tiles")
	_add_slider_row(parent, "building_distance", "Building Distance:", 1, 8, 1, " tiles")
	_add_option_row(parent, "shadow_quality", "Shadow Quality:", ["Off", "Low", "Medium", "High"])
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
		elif key in ["render_distance", "building_distance", "shadow_quality", "vsync", "fullscreen"]:
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
		return
	for section in config.get_sections():
		for key in config.get_section_keys(section):
			if settings.has(key):
				settings[key] = config.get_value(section, key)
	print("Settings loaded from %s" % CONFIG_PATH)


func _get_defaults() -> Dictionary:
	return {
		"scenery_path": "scenery",
		"jsbsim_path": "jsbsim",
		"render_distance": 8,
		"building_distance": 3,
		"shadow_quality": 2,
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
	hide_settings()
	closed.emit()


func _on_cancel() -> void:
	# Revert UI to last saved state
	_load_settings()
	_apply_to_ui()
	hide_settings()
	closed.emit()


func _on_restore_defaults() -> void:
	settings = _get_defaults()
	_apply_to_ui()


func _input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_ESCAPE:
			_on_cancel()
			get_viewport().set_input_as_handled()
