extends CanvasLayer
## LoadingScreen — Lightweight overlay shown during initial asset loading.
##
## Displays a title, progress bar, and status text. Stays on top of
## everything (layer 200) until dismissed.

signal loading_complete()

const BG_COLOR := Color(0.06, 0.08, 0.11, 1.0)
const ACCENT_COLOR := Color(0.25, 0.55, 0.85, 1.0)
const BAR_BG_COLOR := Color(0.15, 0.17, 0.21, 1.0)

var _bg: ColorRect
var _progress_bar: ColorRect
var _progress_bg: ColorRect
var _status_label: Label
var _progress: float = 0.0


func _ready() -> void:
	layer = 200  # Above welcome (100) and settings (101)
	_build_ui()


func _build_ui() -> void:
	_bg = ColorRect.new()
	_bg.color = BG_COLOR
	_bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_bg)

	var center = CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_bg.add_child(center)

	var vbox = VBoxContainer.new()
	vbox.alignment = BoxContainer.ALIGNMENT_CENTER
	vbox.add_theme_constant_override("separation", 32)
	vbox.custom_minimum_size = Vector2(700, 300)
	center.add_child(vbox)

	# Title
	var title = Label.new()
	title.text = "ProVPilot"
	title.add_theme_font_size_override("font_size", 56)
	title.add_theme_color_override("font_color", ACCENT_COLOR)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(title)

	# Subtitle
	var sub = Label.new()
	sub.text = "Swiss Flight Simulator"
	sub.add_theme_font_size_override("font_size", 24)
	sub.add_theme_color_override("font_color", Color(0.5, 0.55, 0.6))
	sub.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(sub)

	# Spacer
	var spacer = Control.new()
	spacer.custom_minimum_size.y = 16
	vbox.add_child(spacer)

	# Progress bar background
	_progress_bg = ColorRect.new()
	_progress_bg.color = BAR_BG_COLOR
	_progress_bg.custom_minimum_size = Vector2(600, 12)
	vbox.add_child(_progress_bg)

	# Progress bar fill
	_progress_bar = ColorRect.new()
	_progress_bar.color = ACCENT_COLOR
	_progress_bar.custom_minimum_size = Vector2(0, 12)
	_progress_bar.size = Vector2(0, 12)
	_progress_bg.add_child(_progress_bar)

	# Status text
	_status_label = Label.new()
	_status_label.text = "Initializing..."
	_status_label.add_theme_font_size_override("font_size", 20)
	_status_label.add_theme_color_override("font_color", Color(0.45, 0.48, 0.52))
	_status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(_status_label)


func set_status(text: String) -> void:
	if _status_label:
		_status_label.text = text


func set_progress(value: float) -> void:
	_progress = clampf(value, 0.0, 1.0)
	if _progress_bar and _progress_bg:
		_progress_bar.custom_minimum_size.x = _progress_bg.custom_minimum_size.x * _progress
		_progress_bar.size.x = _progress_bg.size.x * _progress


func show_loading() -> void:
	visible = true


func hide_loading() -> void:
	visible = false
	loading_complete.emit()


## Block all input while loading
func _unhandled_input(_event: InputEvent) -> void:
	if visible:
		get_viewport().set_input_as_handled()
