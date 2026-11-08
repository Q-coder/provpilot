#include "joystick_bindings.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_joypad_button.hpp>
#include <godot_cpp/classes/input_event_joypad_motion.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

#include "install_config.h"

using namespace godot;

namespace jsbgodot
{

// Legacy stand-alone path, read-only fallback when provpilot.cfg has no
// joystick sections yet.
static const char *LEGACY_CONFIG_PATH = "user://joystick.cfg";

static const struct { const char *id; const char *label; } AXIS_ACTIONS[] = {
	{"slew_strafe", "Slew: Strafe (X)"},
	{"slew_forward", "Slew: Forward/Back"},
	{"slew_yaw", "Slew: Yaw"},
	{"slew_vertical", "Slew: Up/Down"},
	{"flight_roll", "Flight: Roll (aileron)"},
	{"flight_pitch", "Flight: Pitch (elevator)"},
	{"flight_yaw", "Flight: Yaw (rudder)"},
	{"flight_throttle", "Flight: Throttle"},
	{"flight_brakes", "Flight: Wheel brakes (pull side of a centred lever)"},
	{"look_x", "Camera: Look X"},
	{"look_y", "Camera: Look Y"},
	{"drive_steer", "Vehicle: Steer"},
	{"drive_accelerate", "Vehicle: Accelerate (trigger)"},
	{"drive_brake", "Vehicle: Brake (trigger)"},
};

static const struct { const char *id; const char *label; } BUTTON_ACTIONS[] = {
	{"flip_camera", "Cycle camera"},
	{"slew_toggle", "Toggle slew mode"},
	{"flaps_up", "Flaps up a notch"},
	{"flaps_down", "Flaps down a notch"},
	{"trim_up", "Pitch trim up"},
	{"trim_down", "Pitch trim down"},
	{"brakes", "Hold brakes"},
	{"reverse_toggle", "Reverse thrust on / off"},
	{"look_left", "Look left"},
	{"look_right", "Look right"},
	{"look_up", "Look up"},
	{"look_down", "Look down"},
	{"view_closer", "Camera: Zoom in / closer"},
	{"view_farther", "Camera: Zoom out / farther"},
	{"drive_handbrake", "Vehicle: Handbrake"},
};

void JoystickBindings::_bind_methods()
{
	ClassDB::bind_static_method("JoystickBindings", D_METHOD("get_axis_actions"), &JoystickBindings::get_axis_actions);
	ClassDB::bind_static_method("JoystickBindings", D_METHOD("get_button_actions"), &JoystickBindings::get_button_actions);
	ClassDB::bind_static_method("JoystickBindings", D_METHOD("axis_default", "axis", "invert"), &JoystickBindings::axis_default);
	ClassDB::bind_static_method("JoystickBindings", D_METHOD("axis_unbound"), &JoystickBindings::axis_unbound);
	ClassDB::bind_static_method("JoystickBindings", D_METHOD("apply_calibration", "raw", "binding"), &JoystickBindings::apply_calibration);
	ClassDB::bind_method(D_METHOD("get_active_device"), &JoystickBindings::get_active_device);
	ClassDB::bind_method(D_METHOD("get_raw_axis", "action"), &JoystickBindings::get_raw_axis);
	ClassDB::bind_method(D_METHOD("get_axis", "action"), &JoystickBindings::get_axis);
	ClassDB::bind_method(D_METHOD("get_bound_axis", "action"), &JoystickBindings::get_bound_axis);
	ClassDB::bind_method(D_METHOD("get_button", "action"), &JoystickBindings::get_button);
	ClassDB::bind_method(D_METHOD("is_button_just_pressed", "action"), &JoystickBindings::is_button_just_pressed);
	ClassDB::bind_static_method("JoystickBindings", D_METHOD("disable_ui_joypad_navigation"), &JoystickBindings::disable_ui_joypad_navigation);
	ClassDB::bind_method(D_METHOD("detect_active_axis", "threshold"), &JoystickBindings::detect_active_axis, DEFVAL(0.5));
	ClassDB::bind_method(D_METHOD("detect_active_button"), &JoystickBindings::detect_active_button);
	ClassDB::bind_method(D_METHOD("save"), &JoystickBindings::save);
	ClassDB::bind_method(D_METHOD("reset_to_defaults"), &JoystickBindings::reset_to_defaults);
	ClassDB::bind_method(D_METHOD("get_axis_bindings"), &JoystickBindings::get_axis_bindings);
	ClassDB::bind_method(D_METHOD("set_axis_bindings", "bindings"), &JoystickBindings::set_axis_bindings);
	ClassDB::bind_method(D_METHOD("get_button_bindings"), &JoystickBindings::get_button_bindings);
	ClassDB::bind_method(D_METHOD("set_button_bindings", "bindings"), &JoystickBindings::set_button_bindings);
	ClassDB::bind_method(D_METHOD("get_device_id"), &JoystickBindings::get_device_id);
	ClassDB::bind_method(D_METHOD("set_device_id", "id"), &JoystickBindings::set_device_id);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "axis_bindings"), "set_axis_bindings", "get_axis_bindings");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "button_bindings"), "set_button_bindings", "get_button_bindings");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "device_id"), "set_device_id", "get_device_id");
	BIND_ENUM_CONSTANT(CURVE_LINEAR);
	BIND_ENUM_CONSTANT(CURVE_SQUARED);
	BIND_ENUM_CONSTANT(CURVE_CUBED);
}

JoystickBindings::JoystickBindings()
{
}

void JoystickBindings::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	apply_defaults();
	load();
	disable_ui_joypad_navigation();
	set_process(true);
}

void JoystickBindings::disable_ui_joypad_navigation()
{
	InputMap *im = InputMap::get_singleton();
	if (im == nullptr)
	{
		return;
	}
	const TypedArray<StringName> actions = im->get_actions();
	int removed = 0;
	for (int i = 0; i < actions.size(); i++)
	{
		const StringName action = actions[i];
		if (!String(action).begins_with("ui_"))
		{
			continue;
		}
		const TypedArray<InputEvent> events = im->action_get_events(action);
		for (int k = 0; k < events.size(); k++)
		{
			const Ref<InputEvent> ev = events[k];
			if (ev.is_null())
			{
				continue;
			}
			if (Object::cast_to<InputEventJoypadButton>(ev.ptr()) != nullptr || Object::cast_to<InputEventJoypadMotion>(ev.ptr()) != nullptr)
			{
				im->action_erase_event(action, ev);
				removed++;
			}
		}
	}
	if (removed > 0)
	{
		UtilityFunctions::print("JoystickConfig: ", removed, " joypad events removed from the ui_* actions (menus are mouse / keyboard only)");
	}
}

void JoystickBindings::_process(double)
{
	// Autoloads process before the main scene, so the edge computed here
	// is what node_3d.gd and the C++ modules read during this frame.
	button_prev = button_now.duplicate();
	const Array actions = button_bindings.keys();
	for (int i = 0; i < actions.size(); i++)
	{
		button_now[actions[i]] = get_button(actions[i]);
	}
}

bool JoystickBindings::is_button_just_pressed(const String &action) const
{
	return bool(button_now.get(action, false)) && !bool(button_prev.get(action, false));
}

Dictionary JoystickBindings::get_axis_bindings() const { return axis_bindings; }
void JoystickBindings::set_axis_bindings(const Dictionary &d) { axis_bindings = d; }
Dictionary JoystickBindings::get_button_bindings() const { return button_bindings; }
void JoystickBindings::set_button_bindings(const Dictionary &d) { button_bindings = d; }
int JoystickBindings::get_device_id() const { return device_id; }
void JoystickBindings::set_device_id(int id) { device_id = id; }

Array JoystickBindings::get_axis_actions()
{
	Array out;
	for (const auto &a : AXIS_ACTIONS)
	{
		Dictionary d;
		d["id"] = a.id;
		d["label"] = a.label;
		out.push_back(d);
	}
	return out;
}

Array JoystickBindings::get_button_actions()
{
	Array out;
	for (const auto &a : BUTTON_ACTIONS)
	{
		Dictionary d;
		d["id"] = a.id;
		d["label"] = a.label;
		out.push_back(d);
	}
	return out;
}

Dictionary JoystickBindings::axis_default(int axis, bool invert)
{
	Dictionary d;
	d["axis"] = axis;
	d["invert"] = invert;
	d["min"] = -1.0;
	d["center"] = 0.0;
	d["max"] = 1.0;
	d["deadzone"] = 0.15;
	d["curve"] = (int)CURVE_LINEAR;
	return d;
}

Dictionary JoystickBindings::axis_unbound()
{
	return axis_default(-1, false);
}

// Sensible mapping for a standard SDL gamepad.
void JoystickBindings::apply_defaults()
{
	axis_bindings = Dictionary();
	axis_bindings["slew_strafe"] = axis_default(JOY_AXIS_LEFT_X, false);
	axis_bindings["slew_forward"] = axis_default(JOY_AXIS_LEFT_Y, false);
	axis_bindings["slew_yaw"] = axis_default(JOY_AXIS_RIGHT_X, false);
	axis_bindings["slew_vertical"] = axis_default(JOY_AXIS_RIGHT_Y, true); // up = climb
	axis_bindings["flight_roll"] = axis_default(JOY_AXIS_LEFT_X, false);
	axis_bindings["flight_pitch"] = axis_default(JOY_AXIS_LEFT_Y, true); // pull-back = pitch up
	axis_bindings["flight_yaw"] = axis_default(JOY_AXIS_RIGHT_X, false);
	axis_bindings["flight_throttle"] = axis_default(JOY_AXIS_TRIGGER_RIGHT, false);
	axis_bindings["flight_brakes"] = axis_unbound(); // a centred lever: pulled = negative half = brakes
	axis_bindings["look_x"] = axis_unbound();
	axis_bindings["look_y"] = axis_unbound();
	axis_bindings["drive_steer"] = axis_default(JOY_AXIS_LEFT_X, false);
	axis_bindings["drive_accelerate"] = axis_default(JOY_AXIS_TRIGGER_RIGHT, false); // 0..1 at rest 0
	axis_bindings["drive_brake"] = axis_default(JOY_AXIS_TRIGGER_LEFT, false);
	button_bindings = Dictionary();
	auto btn = [&](const char *action, int button) {
		Dictionary d;
		d["button"] = button;
		button_bindings[String(action)] = d;
	};
	// Conflict-free on an Xbox-layout pad (F310 / F710 in X mode, Xbox,
	// PlayStation): face buttons A brakes, B camera, X / Y trim; shoulders
	// flaps; D-pad looks. The zoom buttons are unbound (Page Up / Down).
	btn("flip_camera", JOY_BUTTON_B);
	btn("slew_toggle", -1);
	btn("flaps_up", JOY_BUTTON_RIGHT_SHOULDER);
	btn("flaps_down", JOY_BUTTON_LEFT_SHOULDER);
	btn("trim_up", JOY_BUTTON_Y);
	btn("trim_down", JOY_BUTTON_X);
	btn("brakes", JOY_BUTTON_A);
	btn("reverse_toggle", -1);
	btn("look_left", JOY_BUTTON_DPAD_LEFT);
	btn("look_right", JOY_BUTTON_DPAD_RIGHT);
	btn("look_up", JOY_BUTTON_DPAD_UP);
	btn("look_down", JOY_BUTTON_DPAD_DOWN);
	btn("view_closer", -1);
	btn("view_farther", -1);
	btn("drive_handbrake", JOY_BUTTON_A);
}

void JoystickBindings::reset_to_defaults()
{
	apply_defaults();
}

// ── Runtime accessors ───────────────────────────────────────────────────────

int JoystickBindings::get_active_device() const
{
	if (device_id >= 0)
	{
		return device_id;
	}
	const Array pads = Input::get_singleton()->get_connected_joypads();
	if (pads.is_empty())
	{
		return -1;
	}
	return pads[0];
}

double JoystickBindings::get_raw_axis(const String &action) const
{
	const Dictionary b = axis_bindings.get(action, Dictionary());
	if (b.is_empty() || (int)b.get("axis", -1) < 0)
	{
		return 0.0;
	}
	const int dev = get_active_device();
	if (dev < 0)
	{
		return 0.0;
	}
	return Input::get_singleton()->get_joy_axis(dev, (JoyAxis)(int)b["axis"]);
}

int JoystickBindings::get_bound_axis(const String &action) const
{
	const Dictionary b = axis_bindings.get(action, Dictionary());
	return b.is_empty() ? -1 : (int)b.get("axis", -1);
}

double JoystickBindings::get_axis(const String &action) const
{
	const Dictionary b = axis_bindings.get(action, Dictionary());
	if (b.is_empty() || (int)b.get("axis", -1) < 0)
	{
		return 0.0;
	}
	const int dev = get_active_device();
	if (dev < 0)
	{
		return 0.0;
	}
	return apply_calibration(Input::get_singleton()->get_joy_axis(dev, (JoyAxis)(int)b["axis"]), b);
}

double JoystickBindings::apply_calibration(double raw, const Dictionary &b)
{
	const double center = b.get("center", 0.0);
	const double lo = b.get("min", -1.0);
	const double hi = b.get("max", 1.0);
	const double dz = b.get("deadzone", 0.15);
	const int curve = (int)b.get("curve", (int)CURVE_LINEAR);
	const bool invert = b.get("invert", false);
	// Map to symmetric [-1, 1] using the calibration extremes.
	double v;
	if (raw >= center)
	{
		v = Math::clamp((raw - center) / Math::max(hi - center, 0.000001), 0.0, 1.0);
	}
	else
	{
		v = -Math::clamp((center - raw) / Math::max(center - lo, 0.000001), 0.0, 1.0);
	}
	// Deadzone with linear rescale outside it.
	if (std::abs(v) < dz)
	{
		v = 0.0;
	}
	else if (dz < 1.0)
	{
		v = Math::sign(v) * (std::abs(v) - dz) / (1.0 - dz);
	}
	// Response curve (preserve sign).
	if (curve == CURVE_SQUARED)
	{
		v = Math::sign(v) * v * v;
	}
	else if (curve == CURVE_CUBED)
	{
		v = v * v * v;
	}
	return invert ? -v : v;
}

bool JoystickBindings::get_button(const String &action) const
{
	const Dictionary b = button_bindings.get(action, Dictionary());
	if (b.is_empty() || (int)b.get("button", -1) < 0)
	{
		return false;
	}
	const int dev = get_active_device();
	if (dev < 0)
	{
		return false;
	}
	return Input::get_singleton()->is_joy_button_pressed(dev, (JoyButton)(int)b["button"]);
}

int JoystickBindings::detect_active_axis(double threshold) const
{
	const int dev = get_active_device();
	if (dev < 0)
	{
		return -1;
	}
	int best_axis = -1;
	double best_val = threshold;
	for (int a = 0; a < (int)JOY_AXIS_MAX; a++)
	{
		const double v = std::abs(Input::get_singleton()->get_joy_axis(dev, (JoyAxis)a));
		if (v > best_val)
		{
			best_val = v;
			best_axis = a;
		}
	}
	return best_axis;
}

int JoystickBindings::detect_active_button() const
{
	const int dev = get_active_device();
	if (dev < 0)
	{
		return -1;
	}
	for (int b = 0; b < (int)JOY_BUTTON_MAX; b++)
	{
		if (Input::get_singleton()->is_joy_button_pressed(dev, (JoyButton)b))
		{
			return b;
		}
	}
	return -1;
}

// ── Persistence ─────────────────────────────────────────────────────────────

Node *JoystickBindings::install_root() const
{
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (tree == nullptr || tree->get_root() == nullptr)
	{
		return nullptr;
	}
	return tree->get_root()->get_node_or_null(NodePath("InstallRoot"));
}

void JoystickBindings::save()
{
	InstallConfig *ir = Object::cast_to<InstallConfig>(install_root());
	if (ir == nullptr)
	{
		UtilityFunctions::push_warning("JoystickConfig: InstallRoot not available, save skipped");
		return;
	}
	// Wipe stale axis.*/button.* sections so removed bindings don't linger.
	const PackedStringArray sections = ir->get_sections();
	for (int i = 0; i < sections.size(); i++)
	{
		if (sections[i].begins_with("axis.") || sections[i].begins_with("button."))
		{
			ir->erase_section(sections[i]);
		}
	}
	ir->set_value("device", "id", device_id);
	Array actions = axis_bindings.keys();
	for (int i = 0; i < actions.size(); i++)
	{
		const Dictionary b = axis_bindings[actions[i]];
		const Array keys = b.keys();
		for (int k = 0; k < keys.size(); k++)
		{
			ir->set_value(String("axis.") + String(actions[i]), keys[k], b[keys[k]]);
		}
	}
	actions = button_bindings.keys();
	for (int i = 0; i < actions.size(); i++)
	{
		const Dictionary b = button_bindings[actions[i]];
		ir->set_value(String("button.") + String(actions[i]), "button", (int)b.get("button", -1));
	}
	const Error err = ir->save_config();
	if (err == OK)
	{
		UtilityFunctions::print("JoystickConfig: saved to provpilot.cfg");
	}
	else
	{
		UtilityFunctions::push_warning("JoystickConfig: save failed (err ", (int)err, ")");
	}
}

static bool any_section_with_prefix(const PackedStringArray &sections, const char *prefix)
{
	for (int i = 0; i < sections.size(); i++)
	{
		if (sections[i].begins_with(prefix))
		{
			return true;
		}
	}
	return false;
}

void JoystickBindings::load()
{
	InstallConfig *ir = Object::cast_to<InstallConfig>(install_root());
	PackedStringArray sections;
	String source;
	if (ir != nullptr)
	{
		sections = ir->get_sections();
		if (!sections.is_empty())
		{
			source = "provpilot.cfg";
		}
	}
	// Fallback: the legacy user://joystick.cfg when provpilot.cfg has no
	// joystick sections yet.
	Ref<ConfigFile> legacy;
	const bool have_joy_in_ir = ir != nullptr &&
			(ir->has_section("device") || any_section_with_prefix(sections, "axis.") || any_section_with_prefix(sections, "button."));
	if (!have_joy_in_ir && FileAccess::file_exists(LEGACY_CONFIG_PATH))
	{
		legacy.instantiate();
		if (legacy->load(LEGACY_CONFIG_PATH) == OK)
		{
			sections = legacy->get_sections();
			source = LEGACY_CONFIG_PATH;
		}
		else
		{
			legacy.unref();
		}
	}
	if (sections.is_empty())
	{
		UtilityFunctions::print("JoystickConfig: no saved config, using defaults");
		return;
	}
	auto read_value = [&](const String &section, const String &key, const Variant &def) -> Variant {
		if (legacy.is_valid())
		{
			return legacy->get_value(section, key, def);
		}
		return ir->get_value(section, key, def);
	};
	if (sections.has("device"))
	{
		device_id = (int)read_value("device", "id", -1);
	}
	for (int i = 0; i < sections.size(); i++)
	{
		const String sn = sections[i];
		if (sn.begins_with("axis."))
		{
			const String action = sn.substr(5);
			if (!axis_bindings.has(action))
			{
				continue;
			}
			Dictionary b = axis_bindings[action];
			const PackedStringArray keys = legacy.is_valid() ? legacy->get_section_keys(sn) : ir->get_section_keys(sn);
			for (int k = 0; k < keys.size(); k++)
			{
				b[keys[k]] = read_value(sn, keys[k], b.get(keys[k], Variant()));
			}
			axis_bindings[action] = b;
		}
		else if (sn.begins_with("button."))
		{
			const String action = sn.substr(7);
			if (!button_bindings.has(action))
			{
				continue;
			}
			Dictionary d;
			d["button"] = (int)read_value(sn, "button", -1);
			button_bindings[action] = d;
		}
	}
	UtilityFunctions::print("JoystickConfig: loaded from ", source);
}

} // namespace jsbgodot
