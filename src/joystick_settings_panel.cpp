#include "joystick_settings_panel.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cmath>

#include "joystick_bindings.h"

using namespace godot;

namespace jsbgodot
{

static const int FONT_SM = 18, FONT_MD = 22, FONT_LG = 26;
static const Color LABEL_COLOR(0.75, 0.78, 0.82);
static const Color SECTION_COLOR(0.25, 0.55, 0.85, 1.0);
static const Color ACCENT(0.4, 0.85, 0.4);

void JoystickSettingsPanel::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("refresh"), &JoystickSettingsPanel::refresh);
	ClassDB::bind_method(D_METHOD("_on_detect_axis", "action"), &JoystickSettingsPanel::_on_detect_axis);
	ClassDB::bind_method(D_METHOD("_on_detect_button", "action"), &JoystickSettingsPanel::_on_detect_button);
	ClassDB::bind_method(D_METHOD("_on_axis_index_changed", "value", "action"), &JoystickSettingsPanel::_on_axis_index_changed);
	ClassDB::bind_method(D_METHOD("_on_button_index_changed", "value", "action"), &JoystickSettingsPanel::_on_button_index_changed);
	ClassDB::bind_method(D_METHOD("_on_invert_toggled", "pressed", "action"), &JoystickSettingsPanel::_on_invert_toggled);
	ClassDB::bind_method(D_METHOD("_on_deadzone_changed", "value", "action"), &JoystickSettingsPanel::_on_deadzone_changed);
	ClassDB::bind_method(D_METHOD("_on_curve_selected", "idx", "action"), &JoystickSettingsPanel::_on_curve_selected);
	ClassDB::bind_method(D_METHOD("_on_capture_endpoint", "action", "which"), &JoystickSettingsPanel::_on_capture_endpoint);
}

JoystickSettingsPanel::JoystickSettingsPanel()
{
}

JoystickBindings *JoystickSettingsPanel::config() const
{
	if (!is_inside_tree())
	{
		return nullptr;
	}
	return Object::cast_to<JoystickBindings>(get_node_or_null(NodePath("/root/JoystickConfig")));
}

static Label *styled_label(const String &text, int size, const Color &color)
{
	Label *l = memnew(Label);
	l->set_text(text);
	l->add_theme_font_size_override("font_size", size);
	l->add_theme_color_override("font_color", color);
	return l;
}

void JoystickSettingsPanel::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	add_theme_constant_override("separation", 6);
	build_ui();
	refresh_from_config();
	set_process(true);
}

void JoystickSettingsPanel::build_ui()
{
	add_child(styled_label("Joystick / Gamepad", FONT_LG, SECTION_COLOR));
	HBoxContainer *dev_hbox = memnew(HBoxContainer);
	dev_hbox->add_theme_constant_override("separation", 12);
	add_child(dev_hbox);
	Label *dev_label = styled_label("Device:", FONT_MD, LABEL_COLOR);
	dev_label->set_custom_minimum_size(Vector2(200, 0));
	dev_hbox->add_child(dev_label);
	device_status = styled_label("", FONT_SM, LABEL_COLOR);
	device_status->set_name("DeviceStatus");
	dev_hbox->add_child(device_status);
	Label *help = styled_label(String::utf8("  \xE2\x80\xA2 Click Detect, then move the desired axis or press the desired button.\n  \xE2\x80\xA2 Min/Center/Max capture the current raw value. Invert flips the direction."), FONT_SM, Color(0.55, 0.6, 0.65));
	add_child(help);
	add_child(styled_label("Axes", FONT_MD, SECTION_COLOR));
	const Array axes = JoystickBindings::get_axis_actions();
	for (int i = 0; i < axes.size(); i++)
	{
		const Dictionary e = axes[i];
		build_axis_row(e["id"], e["label"]);
	}
	add_child(memnew(HSeparator));
	add_child(styled_label("Buttons", FONT_MD, SECTION_COLOR));
	const Array buttons = JoystickBindings::get_button_actions();
	for (int i = 0; i < buttons.size(); i++)
	{
		const Dictionary e = buttons[i];
		build_button_row(e["id"], e["label"]);
	}
}

static Button *small_button(const String &text, int w, const String &tooltip = String())
{
	Button *b = memnew(Button);
	b->set_text(text);
	b->set_custom_minimum_size(Vector2(w, 32));
	if (!tooltip.is_empty())
	{
		b->set_tooltip_text(tooltip);
	}
	return b;
}

static ProgressBar *axis_bar()
{
	ProgressBar *bar = memnew(ProgressBar);
	bar->set_min(-1.0);
	bar->set_max(1.0);
	bar->set_step(0.001);
	bar->set_value(0.0);
	bar->set_show_percentage(false);
	bar->set_custom_minimum_size(Vector2(120, 24));
	return bar;
}

void JoystickSettingsPanel::build_axis_row(const String &action_id, const String &label_text)
{
	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override("separation", 6);
	add_child(row);
	Label *lbl = styled_label(label_text, FONT_SM, LABEL_COLOR);
	lbl->set_custom_minimum_size(Vector2(220, 0));
	row->add_child(lbl);
	AxisRow r;
	r.spin = memnew(SpinBox);
	r.spin->set_min(-1);
	r.spin->set_max((int)JOY_AXIS_MAX - 1);
	r.spin->set_step(1);
	r.spin->set_custom_minimum_size(Vector2(70, 32));
	row->add_child(r.spin);
	r.detect_btn = small_button("Detect", 80);
	r.detect_btn->connect("pressed", Callable(this, "_on_detect_axis").bind(action_id));
	row->add_child(r.detect_btn);
	r.raw_bar = axis_bar();
	row->add_child(r.raw_bar);
	r.cal_bar = axis_bar();
	r.cal_bar->set_modulate(ACCENT);
	row->add_child(r.cal_bar);
	r.invert = memnew(CheckBox);
	r.invert->set_text("Inv");
	r.invert->connect("toggled", Callable(this, "_on_invert_toggled").bind(action_id));
	row->add_child(r.invert);
	Button *min_btn = small_button("Min", 56, "Capture current axis value as the negative endpoint");
	min_btn->connect("pressed", Callable(this, "_on_capture_endpoint").bind(action_id, "min"));
	row->add_child(min_btn);
	Button *center_btn = small_button("Ctr", 56, "Capture current axis value as the rest position");
	center_btn->connect("pressed", Callable(this, "_on_capture_endpoint").bind(action_id, "center"));
	row->add_child(center_btn);
	Button *max_btn = small_button("Max", 56, "Capture current axis value as the positive endpoint");
	max_btn->connect("pressed", Callable(this, "_on_capture_endpoint").bind(action_id, "max"));
	row->add_child(max_btn);
	r.dz_slider = memnew(HSlider);
	r.dz_slider->set_min(0.0);
	r.dz_slider->set_max(0.5);
	r.dz_slider->set_step(0.01);
	r.dz_slider->set_custom_minimum_size(Vector2(120, 24));
	r.dz_slider->set_tooltip_text("Deadzone");
	r.dz_slider->connect("value_changed", Callable(this, "_on_deadzone_changed").bind(action_id));
	row->add_child(r.dz_slider);
	r.curve_opt = memnew(OptionButton);
	r.curve_opt->add_item("Linear", JoystickBindings::CURVE_LINEAR);
	r.curve_opt->add_item("Squared", JoystickBindings::CURVE_SQUARED);
	r.curve_opt->add_item("Cubed", JoystickBindings::CURVE_CUBED);
	r.curve_opt->set_custom_minimum_size(Vector2(110, 32));
	r.curve_opt->connect("item_selected", Callable(this, "_on_curve_selected").bind(action_id));
	row->add_child(r.curve_opt);
	r.spin->connect("value_changed", Callable(this, "_on_axis_index_changed").bind(action_id));
	axis_rows[action_id] = r;
}

void JoystickSettingsPanel::build_button_row(const String &action_id, const String &label_text)
{
	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override("separation", 6);
	add_child(row);
	Label *lbl = styled_label(label_text, FONT_SM, LABEL_COLOR);
	lbl->set_custom_minimum_size(Vector2(220, 0));
	row->add_child(lbl);
	ButtonRow r;
	r.spin = memnew(SpinBox);
	r.spin->set_min(-1);
	r.spin->set_max((int)JOY_BUTTON_MAX - 1);
	r.spin->set_step(1);
	r.spin->set_custom_minimum_size(Vector2(70, 32));
	r.spin->connect("value_changed", Callable(this, "_on_button_index_changed").bind(action_id));
	row->add_child(r.spin);
	r.detect_btn = small_button("Detect", 80);
	r.detect_btn->connect("pressed", Callable(this, "_on_detect_button").bind(action_id));
	row->add_child(r.detect_btn);
	r.pressed_lbl = memnew(Label);
	r.pressed_lbl->set_text(String::utf8("\xC2\xB7"));
	r.pressed_lbl->set_custom_minimum_size(Vector2(40, 24));
	r.pressed_lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	r.pressed_lbl->add_theme_font_size_override("font_size", FONT_MD);
	row->add_child(r.pressed_lbl);
	button_rows[action_id] = r;
}

void JoystickSettingsPanel::refresh_from_config()
{
	JoystickBindings *cfg = config();
	if (cfg == nullptr)
	{
		return;
	}
	const Dictionary axes = cfg->get_axis_bindings();
	for (const KeyValue<String, AxisRow> &kv : axis_rows)
	{
		const Dictionary b = axes.get(kv.key, Dictionary());
		kv.value.spin->set_value_no_signal((int)b.get("axis", -1));
		kv.value.invert->set_pressed_no_signal((bool)b.get("invert", false));
		kv.value.dz_slider->set_value_no_signal((double)b.get("deadzone", 0.15));
		kv.value.curve_opt->select((int)b.get("curve", 0));
	}
	const Dictionary buttons = cfg->get_button_bindings();
	for (const KeyValue<String, ButtonRow> &kv : button_rows)
	{
		const Dictionary b = buttons.get(kv.key, Dictionary());
		kv.value.spin->set_value_no_signal((int)b.get("button", -1));
	}
}

void JoystickSettingsPanel::refresh()
{
	refresh_from_config();
}

void JoystickSettingsPanel::_process(double)
{
	JoystickBindings *cfg = config();
	if (cfg == nullptr)
	{
		return;
	}
	const int dev = cfg->get_active_device();
	if (device_status != nullptr)
	{
		if (dev < 0)
		{
			device_status->set_text("(no controller connected)");
		}
		else
		{
			device_status->set_text(Input::get_singleton()->get_joy_name(dev) + String::utf8(" \xE2\x80\x94 pad ") + String::num_int64(dev));
		}
	}
	for (const KeyValue<String, AxisRow> &kv : axis_rows)
	{
		kv.value.raw_bar->set_value(cfg->get_raw_axis(kv.key));
		kv.value.cal_bar->set_value(cfg->get_axis(kv.key));
	}
	if (!detecting_axis.is_empty())
	{
		poll_axis_detection();
	}
	if (!detecting_button.is_empty())
	{
		poll_button_detection();
	}
	for (const KeyValue<String, ButtonRow> &kv : button_rows)
	{
		const bool pressed = cfg->get_button(kv.key);
		kv.value.pressed_lbl->set_text(pressed ? String::utf8("\xE2\x97\x8F") : String::utf8("\xC2\xB7"));
		kv.value.pressed_lbl->add_theme_color_override("font_color", pressed ? ACCENT : LABEL_COLOR);
	}
}

void JoystickSettingsPanel::poll_axis_detection()
{
	JoystickBindings *cfg = config();
	const int dev = cfg->get_active_device();
	if (dev < 0)
	{
		return;
	}
	Input *input = Input::get_singleton();
	if (detect_axis_baseline.size() < (int)JOY_AXIS_MAX)
	{
		detect_axis_baseline.resize(JOY_AXIS_MAX);
		for (int a = 0; a < (int)JOY_AXIS_MAX; a++)
		{
			detect_axis_baseline[a] = input->get_joy_axis(dev, (JoyAxis)a);
		}
		return;
	}
	for (int a = 0; a < (int)JOY_AXIS_MAX; a++)
	{
		const double v = input->get_joy_axis(dev, (JoyAxis)a);
		if (std::abs(v - detect_axis_baseline[a]) > 0.5)
		{
			assign_axis(detecting_axis, a);
			axis_rows[detecting_axis].detect_btn->set_text("Detect");
			detecting_axis = String();
			detect_axis_baseline.clear();
			return;
		}
	}
}

void JoystickSettingsPanel::poll_button_detection()
{
	JoystickBindings *cfg = config();
	const int dev = cfg->get_active_device();
	if (dev < 0)
	{
		return;
	}
	for (int b = 0; b < (int)JOY_BUTTON_MAX; b++)
	{
		if (Input::get_singleton()->is_joy_button_pressed(dev, (JoyButton)b))
		{
			assign_button(detecting_button, b);
			button_rows[detecting_button].detect_btn->set_text("Detect");
			detecting_button = String();
			return;
		}
	}
}

void JoystickSettingsPanel::_on_detect_axis(const String &action)
{
	if (!detecting_axis.is_empty() && detecting_axis != action)
	{
		axis_rows[detecting_axis].detect_btn->set_text("Detect");
	}
	if (!detecting_button.is_empty())
	{
		button_rows[detecting_button].detect_btn->set_text("Detect");
		detecting_button = String();
	}
	detecting_axis = action;
	detect_axis_baseline.clear();
	axis_rows[action].detect_btn->set_text(String::utf8("\xE2\x80\xA6move"));
}

void JoystickSettingsPanel::_on_detect_button(const String &action)
{
	if (!detecting_button.is_empty() && detecting_button != action)
	{
		button_rows[detecting_button].detect_btn->set_text("Detect");
	}
	if (!detecting_axis.is_empty())
	{
		axis_rows[detecting_axis].detect_btn->set_text("Detect");
		detecting_axis = String();
	}
	detecting_button = action;
	button_rows[action].detect_btn->set_text(String::utf8("\xE2\x80\xA6press"));
}

void JoystickSettingsPanel::_on_axis_index_changed(double value, const String &action)
{
	assign_axis(action, (int)value);
}

void JoystickSettingsPanel::_on_button_index_changed(double value, const String &action)
{
	assign_button(action, (int)value);
}

static void update_axis_binding(JoystickBindings *cfg, const String &action, const char *key, const Variant &value)
{
	Dictionary axes = cfg->get_axis_bindings();
	Dictionary b = axes.get(action, Dictionary());
	b[key] = value;
	axes[action] = b;
}

void JoystickSettingsPanel::_on_invert_toggled(bool pressed, const String &action)
{
	if (config() != nullptr) update_axis_binding(config(), action, "invert", pressed);
}

void JoystickSettingsPanel::_on_deadzone_changed(double value, const String &action)
{
	if (config() != nullptr) update_axis_binding(config(), action, "deadzone", value);
}

void JoystickSettingsPanel::_on_curve_selected(int idx, const String &action)
{
	if (config() != nullptr) update_axis_binding(config(), action, "curve", idx);
}

void JoystickSettingsPanel::_on_capture_endpoint(const String &action, const String &which)
{
	JoystickBindings *cfg = config();
	if (cfg == nullptr)
	{
		return;
	}
	const int dev = cfg->get_active_device();
	if (dev < 0)
	{
		return;
	}
	Dictionary axes = cfg->get_axis_bindings();
	Dictionary b = axes.get(action, Dictionary());
	const int axis_idx = (int)b.get("axis", -1);
	if (axis_idx < 0)
	{
		return;
	}
	b[which] = (double)Input::get_singleton()->get_joy_axis(dev, (JoyAxis)axis_idx);
	axes[action] = b;
}

void JoystickSettingsPanel::assign_axis(const String &action, int axis_idx)
{
	JoystickBindings *cfg = config();
	if (cfg == nullptr)
	{
		return;
	}
	Dictionary axes = cfg->get_axis_bindings();
	Dictionary b = axes.get(action, JoystickBindings::axis_default(axis_idx, false));
	b["axis"] = axis_idx;
	axes[action] = b;
	axis_rows[action].spin->set_value_no_signal(axis_idx);
}

void JoystickSettingsPanel::assign_button(const String &action, int button_idx)
{
	JoystickBindings *cfg = config();
	if (cfg == nullptr)
	{
		return;
	}
	Dictionary buttons = cfg->get_button_bindings();
	Dictionary b;
	b["button"] = button_idx;
	buttons[action] = b;
	button_rows[action].spin->set_value_no_signal(button_idx);
}

} // namespace jsbgodot
