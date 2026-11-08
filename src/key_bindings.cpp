#include "key_bindings.h"

#include <vector>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_with_modifiers.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cwctype>

using namespace godot;

namespace jsbgodot
{

// ── chords ──────────────────────────────────────────────────────────────────

namespace
{
	struct KeyName
	{
		Key key;
		const char *name;
	};
	// Keys without a printable character, and the printable ones whose
	// character would collide with the chord syntax. Everything else between
	// '!' and '~' is written as its own character ("[", "=", ",").
	const KeyName KEY_NAMES[] = {
		{KEY_SPACE, "Space"}, {KEY_ESCAPE, "Escape"}, {KEY_TAB, "Tab"}, {KEY_BACKSPACE, "Backspace"},
		{KEY_ENTER, "Enter"}, {KEY_KP_ENTER, "KpEnter"}, {KEY_INSERT, "Insert"}, {KEY_DELETE, "Delete"},
		{KEY_HOME, "Home"}, {KEY_END, "End"}, {KEY_PAGEUP, "PageUp"}, {KEY_PAGEDOWN, "PageDown"},
		{KEY_LEFT, "Left"}, {KEY_UP, "Up"}, {KEY_RIGHT, "Right"}, {KEY_DOWN, "Down"},
		{KEY_SHIFT, "Shift"}, {KEY_CTRL, "Ctrl"}, {KEY_ALT, "Alt"}, {KEY_META, "Cmd"}, {KEY_CAPSLOCK, "CapsLock"},
		{KEY_PLUS, "Plus"}, {KEY_KP_ADD, "KpPlus"}, {KEY_KP_SUBTRACT, "KpMinus"}, {KEY_KP_MULTIPLY, "KpStar"},
		{KEY_KP_DIVIDE, "KpSlash"}, {KEY_KP_PERIOD, "KpDot"},
		{KEY_KP_0, "Kp0"}, {KEY_KP_1, "Kp1"}, {KEY_KP_2, "Kp2"}, {KEY_KP_3, "Kp3"}, {KEY_KP_4, "Kp4"},
		{KEY_KP_5, "Kp5"}, {KEY_KP_6, "Kp6"}, {KEY_KP_7, "Kp7"}, {KEY_KP_8, "Kp8"}, {KEY_KP_9, "Kp9"},
		{KEY_F1, "F1"}, {KEY_F2, "F2"}, {KEY_F3, "F3"}, {KEY_F4, "F4"}, {KEY_F5, "F5"}, {KEY_F6, "F6"},
		{KEY_F7, "F7"}, {KEY_F8, "F8"}, {KEY_F9, "F9"}, {KEY_F10, "F10"}, {KEY_F11, "F11"}, {KEY_F12, "F12"},
	};

	bool is_printable(Key k)
	{
		return int(k) > 0x20 && int(k) < 0x7f && k != KEY_PLUS;
	}

	bool is_modifier_key(Key k)
	{
		return k == KEY_SHIFT || k == KEY_CTRL || k == KEY_ALT || k == KEY_META;
	}

	// The character a printable key types with Shift on a US layout: what a
	// chord like "Shift+[" means, whatever keys produce it on this keyboard.
	char32_t shifted_char(Key k)
	{
		const int c = int(k);
		if (c >= 'A' && c <= 'Z')
			return char32_t(c);
		switch (c)
		{
			case '1': return '!'; case '2': return '@'; case '3': return '#'; case '4': return '$'; case '5': return '%';
			case '6': return '^'; case '7': return '&'; case '8': return '*'; case '9': return '('; case '0': return ')';
			case '-': return '_'; case '=': return '+'; case '[': return '{'; case ']': return '}'; case ';': return ':';
			case '\'': return '"'; case ',': return '<'; case '.': return '>'; case '/': return '?'; case '\\': return '|';
			case '`': return '~';
			default: return char32_t(c);
		}
	}

	char32_t plain_char(Key k)
	{
		const int c = int(k);
		if (c >= 'A' && c <= 'Z')
			return char32_t(c - 'A' + 'a');
		return char32_t(c);
	}
} // namespace

String KeyChord::key_name(Key key)
{
	for (const KeyName &kn : KEY_NAMES)
		if (kn.key == key)
			return kn.name;
	if (is_printable(key))
		return String::chr(int(key));
	if (key == KEY_NONE)
		return "";
	return OS::get_singleton()->get_keycode_string(key);
}

Key KeyChord::key_from_name(const String &name)
{
	const String n = name.strip_edges();
	if (n.is_empty())
		return KEY_NONE;
	for (const KeyName &kn : KEY_NAMES)
		if (n.nocasecmp_to(kn.name) == 0)
			return kn.key;
	if (n.length() == 1)
	{
		const char32_t c = n[0];
		if (c >= 'a' && c <= 'z')
			return Key(int(c) - 'a' + 'A');
		if (c > 0x20 && c < 0x7f)
			return Key(int(c));
		return KEY_NONE;
	}
	// a few spellings people write by hand
	if (n.nocasecmp_to("Esc") == 0) return KEY_ESCAPE;
	if (n.nocasecmp_to("Return") == 0) return KEY_ENTER;
	if (n.nocasecmp_to("PgUp") == 0) return KEY_PAGEUP;
	if (n.nocasecmp_to("PgDn") == 0 || n.nocasecmp_to("PgDown") == 0) return KEY_PAGEDOWN;
	if (n.nocasecmp_to("Del") == 0) return KEY_DELETE;
	if (n.nocasecmp_to("Meta") == 0 || n.nocasecmp_to("Command") == 0) return KEY_META;
	if (n.nocasecmp_to("Control") == 0) return KEY_CTRL;
	if (n.nocasecmp_to("Option") == 0) return KEY_ALT;
	const Key k = Key(int64_t(OS::get_singleton()->find_keycode_from_string(n)) & int64_t(KEY_CODE_MASK));
	return k;
}

String KeyChord::to_string() const
{
	if (!is_bound())
		return "";
	String s;
	if (!is_modifier_key(key))
	{
		if (ctrl) s += "Ctrl+";
		if (shift) s += "Shift+";
		if (alt) s += "Alt+";
	}
	return s + key_name(key);
}

KeyChord KeyChord::parse(const String &text)
{
	KeyChord c;
	const PackedStringArray parts = text.strip_edges().split("+", false);
	if (parts.is_empty())
		return c;
	for (int i = 0; i < parts.size() - 1; i++)
	{
		const String m = parts[i].strip_edges();
		if (m.nocasecmp_to("Ctrl") == 0 || m.nocasecmp_to("Cmd") == 0 || m.nocasecmp_to("Control") == 0 || m.nocasecmp_to("Meta") == 0 || m.nocasecmp_to("Command") == 0)
			c.ctrl = true;
		else if (m.nocasecmp_to("Shift") == 0)
			c.shift = true;
		else if (m.nocasecmp_to("Alt") == 0 || m.nocasecmp_to("Option") == 0)
			c.alt = true;
		else
			return KeyChord(); // not a chord
	}
	c.key = key_from_name(parts[parts.size() - 1]);
	if (!c.is_bound())
		return KeyChord();
	if (is_modifier_key(c.key))
		c.shift = c.ctrl = c.alt = false;
	return c;
}

// ── the catalog ─────────────────────────────────────────────────────────────

namespace
{
	struct Spec
	{
		const char *id;
		const char *section;
		const char *label;
		const char *key1;
		const char *key2;
		bool shared; // true: runs in its own phase (the intro), so sharing a key is no conflict
	};
	// Display order. The defaults are the keys the code used before the
	// mapping existed (README "Controls", docs/SCENERY_EDITOR.md).
	const Spec SPECS[] = {
		// ── flight ──
		{"elevator_up", "flight", "Elevator up (nose down)", "W", ""},
		{"elevator_down", "flight", "Elevator down (nose up)", "S", ""},
		{"aileron_left", "flight", "Aileron left", "A", ""},
		{"aileron_right", "flight", "Aileron right", "D", ""},
		{"rudder_left", "flight", "Rudder left", "Y", ""},
		{"rudder_right", "flight", "Rudder right", "X", ""},
		{"throttle_up", "flight", "Throttle up (fine)", "P", ""},
		{"throttle_down", "flight", "Throttle down (fine)", "O", ""},
		{"throttle_step_up", "flight", "Throttle up (step)", "T", ""},
		{"throttle_step_down", "flight", "Throttle down (step)", "Shift+T", ""},
		{"mixture_rich", "flight", "Mixture richer", "M", ""},
		{"mixture_lean", "flight", "Mixture leaner", "Shift+M", ""},
		{"magneto_cw", "flight", "Magneto clockwise (hold for START)", "I", ""},
		{"magneto_ccw", "flight", "Magneto counter-clockwise", "Shift+I", ""},
		{"flaps_down", "flight", "Flaps down a notch", "F", ""},
		{"flaps_up", "flight", "Flaps up a notch", "Shift+F", ""},
		{"gear_toggle", "flight", "Landing gear", "G", ""},
		{"speedbrake_toggle", "flight", "Speed brakes", "B", ""},
		{"reverse_toggle", "flight", "Reverse thrust (on the ground, levers at idle)", "R", ""},
		{"brakes_on", "flight", "Wheel brakes on", ".", ""},
		{"brakes_off", "flight", "Wheel brakes off", ",", ""},
		{"nose_steer_toggle", "flight", "Nose-wheel steering range", "Ctrl+W", ""},
		{"landing_lights", "flight", "Landing / taxi lights", "L", ""},
		{"fdm_start", "flight", "Start the flight model", "Shift+R", ""},
		{"fdm_stop", "flight", "Stop the flight model", "E", ""},
		{"camera_flip", "flight", "Next camera (cockpit / orbit / chase)", "C", ""},
		{"camera_flip_back", "flight", "Previous camera (cockpit / chase / orbit)", "Shift+C", ""},
		{"view_left", "flight", "Look / orbit left", "Left", ""},
		{"view_right", "flight", "Look / orbit right", "Right", ""},
		{"view_up", "flight", "Look / orbit up", "Up", ""},
		{"view_down", "flight", "Look / orbit down", "Down", ""},
		{"view_mouse_look", "flight", "Mouse look (hold), recentre (tap)", "Space", ""},
		{"view_closer", "flight", "Zoom in / closer", "PageUp", ""},
		{"view_farther", "flight", "Zoom out / farther", "PageDown", ""},
		{"view_move_left", "flight", "Eye point left", "Ctrl+Left", ""},
		{"view_move_right", "flight", "Eye point right", "Ctrl+Right", ""},
		{"view_move_up", "flight", "Eye point up", "Ctrl+Up", ""},
		{"view_move_down", "flight", "Eye point down", "Ctrl+Down", ""},
		{"view_move_forward", "flight", "Eye point forward", "Ctrl+Shift+Up", ""},
		{"view_move_back", "flight", "Eye point back", "Ctrl+Shift+Down", ""},
		{"view_move_reset", "flight", "Eye point back to the default", "Ctrl+Space", ""},
		{"hud_cycle", "flight", "Cycle the HUD", "H", ""},
		{"weather_overlay", "flight", "Weather overlay", "Ctrl+I", ""},
		{"fps_toggle", "flight", "FPS counter", "Ctrl+Shift+F", ""},
		{"fullscreen_toggle", "flight", "Fullscreen / windowed", "Alt+Enter", ""},
		{"map_toggle", "flight", "VFR map", "N", ""},
		{"map_zoom_in", "flight", "VFR map zoom in", "=", "Shift+="},
		{"map_zoom_out", "flight", "VFR map zoom out", "-", ""},
		{"intro_skip", "flight", "Skip the intro flight", "Space", "", true},
		{"throttle_debug", "flight", "Debug: throttle lever hit boxes", "Ctrl+Shift+T", ""},
		{"mixture_debug", "flight", "Debug: mixture lever hit boxes", "Ctrl+Shift+M", ""},
		{"hotspots_debug", "flight", "Debug: cockpit mouse hotspots", "Ctrl+Shift+H", ""},
		// ── vehicle (DriveMode) ──
		{"drive_accelerate", "vehicle", "Accelerate", "W", ""},
		{"drive_brake", "vehicle", "Brake / reverse (held at standstill)", "S", ""},
		{"drive_left", "vehicle", "Steer left", "A", ""},
		{"drive_right", "vehicle", "Steer right", "D", ""},
		{"drive_handbrake", "vehicle", "Handbrake", "B", ""},
		// ── slew ──
		{"slew_toggle", "slew", "Slew mode on / off", "Shift+Y", ""},
		{"slew_fast", "slew", "Fast (hold)", "Shift", ""},
		{"slew_forward", "slew", "Forward", "W", ""},
		{"slew_back", "slew", "Back", "S", ""},
		{"slew_left", "slew", "Left", "A", ""},
		{"slew_right", "slew", "Right", "D", ""},
		{"slew_up", "slew", "Up", "Q", "PageUp"},
		{"slew_down", "slew", "Down", "E", "PageDown"},
		// ── editor ──
		{"editor_toggle", "editor", "Scenery editor on / off", "F8", ""},
		{"editor_forward", "editor", "Move forward", "W", ""},
		{"editor_back", "editor", "Move back", "S", ""},
		{"editor_left", "editor", "Move left", "A", ""},
		{"editor_right", "editor", "Move right", "D", ""},
		{"editor_up", "editor", "Climb", "E", "PageUp"},
		{"editor_down", "editor", "Descend", "Q", "PageDown"},
		{"editor_fast", "editor", "Fast (hold)", "Shift", ""},
		{"tool_select", "editor", "Tool: Select", "1", ""},
		{"tool_road", "editor", "Tool: Road", "2", ""},
		{"tool_rail", "editor", "Tool: Rail", "3", ""},
		{"tool_delete", "editor", "Tool: Delete", "4", ""},
		{"tool_circle", "editor", "Tool: Circle", "5", ""},
		{"tool_switch", "editor", "Tool: Switch", "6", ""},
		{"tool_replace", "editor", "Tool: Replace", "7", ""},
		{"tool_flatten", "editor", "Tool: Flatten", "8", ""},
		{"tool_area", "editor", "Tool: Area", "9", ""},
		{"tool_pick", "editor", "Tool: Pick", "0", ""},
		{"tool_object", "editor", "Tool: Object", "O", ""},
		{"view_top", "editor", "View straight down", "Space", ""},
		{"view_top_north", "editor", "View straight down, north up", "Shift+Space", ""},
		{"reverse_or_rotate", "editor", "Reverse the road / rotate gizmo", "R", ""},
		{"height_down", "editor", "Height −0.5 m", "[", ""},
		{"height_up", "editor", "Height +0.5 m", "]", ""},
		{"height_down_big", "editor", "Height −5 m", "Shift+[", ""},
		{"height_up_big", "editor", "Height +5 m", "Shift+]", ""},
		{"curved_toggle", "editor", "Curved on / off", "C", ""},
		{"finish", "editor", "Finish the line / polygon", "Enter", "KpEnter"},
		{"cancel", "editor", "Cancel / deselect / leave", "Escape", ""},
		{"delete", "editor", "Delete at the cursor", "Delete", "Backspace"},
		{"undo", "editor", "Undo", "Ctrl+Z", ""},
		{"save", "editor", "Save the design", "Ctrl+S", ""},
		{"bake", "editor", "Bake the area", "Ctrl+B", ""},
		{"gizmo_move", "editor", "Objects: move gizmo", "G", ""},
		{"gizmo_scale", "editor", "Objects: scale gizmo", "Y", ""},
		{"frame_selection", "editor", "Objects: frame the selection", "F", ""},
		{"drop_to_ground", "editor", "Objects: drop to the ground", "Ctrl+G", ""},
		{"heading_left", "editor", "Objects: heading −1°", ",", ""},
		{"heading_right", "editor", "Objects: heading +1°", ".", ""},
		{"heading_left_big", "editor", "Objects: heading −10°", "Shift+,", ""},
		{"heading_right_big", "editor", "Objects: heading +10°", "Shift+.", ""},
		{"scale_down", "editor", "Objects: scale −5 %", "-", "KpMinus"},
		{"scale_up", "editor", "Objects: scale +5 %", "=", "KpPlus"},
		{"scale_down_big", "editor", "Objects: scale −25 %", "Shift+-", ""},
		{"scale_up_big", "editor", "Objects: scale +25 %", "Shift+=", ""},
	};

	struct SectionSpec
	{
		const char *id;
		const char *label;
	};
	const SectionSpec SECTIONS[] = {{"flight", "Flight"}, {"vehicle", "Vehicle"}, {"slew", "Slew"}, {"editor", "Scenery editor"}};

	std::string key_of(const StringName &id)
	{
		return std::string(String(id).utf8().get_data());
	}
} // namespace

void KeyBindings::Table::build_defaults()
{
	actions.clear();
	index.clear();
	edges.clear();
	for (const Spec &s : SPECS)
	{
		Action a;
		a.id = StringName(s.id);
		a.section = s.section;
		a.label = String::utf8(s.label);
		a.shared = s.shared;
		a.defaults[0] = KeyChord::parse(s.key1);
		a.defaults[1] = KeyChord::parse(s.key2);
		for (int i = 0; i < SLOTS; i++)
			a.keys[i] = a.defaults[i];
		index[s.id] = int(actions.size());
		actions.push_back(a);
	}
	rebuild_modifiers();
}

void KeyBindings::Table::rebuild_modifiers()
{
	section_modifiers.clear();
	for (const Action &a : actions)
		for (int i = 0; i < SLOTS; i++)
		{
			const Key k = a.keys[i].key;
			int bit = k == KEY_SHIFT ? 1 : (k == KEY_CTRL || k == KEY_META) ? 2 : k == KEY_ALT ? 4 : 0;
			if (bit)
				section_modifiers[a.section.utf8().get_data()] |= bit;
		}
}

const KeyBindings::Action *KeyBindings::Table::find(const StringName &id) const
{
	const auto it = index.find(key_of(id));
	return it == index.end() ? nullptr : &actions[it->second];
}

KeyBindings::Action *KeyBindings::Table::find(const StringName &id)
{
	const auto it = index.find(key_of(id));
	return it == index.end() ? nullptr : &actions[it->second];
}

// ── the node ────────────────────────────────────────────────────────────────

KeyBindings::Table *KeyBindings::installed = nullptr;
bool KeyBindings::mods_known = false;
bool KeyBindings::mod_shift = false, KeyBindings::mod_ctrl = false, KeyBindings::mod_alt = false;

KeyBindings::Table &KeyBindings::defaults()
{
	static Table t;
	if (t.actions.empty())
		t.build_defaults();
	return t;
}

KeyBindings::Table *KeyBindings::active()
{
	return installed != nullptr ? installed : &defaults();
}

void KeyBindings::_bind_methods()
{
	ClassDB::bind_static_method("KeyBindings", D_METHOD("pressed", "action"), &KeyBindings::pressed);
	ClassDB::bind_static_method("KeyBindings", D_METHOD("just_pressed", "action"), &KeyBindings::just_pressed);
	ClassDB::bind_static_method("KeyBindings", D_METHOD("matches", "action", "event"), &KeyBindings::matches);
	ClassDB::bind_static_method("KeyBindings", D_METHOD("label", "action"), &KeyBindings::label);
	ClassDB::bind_static_method("KeyBindings", D_METHOD("labels", "action"), &KeyBindings::labels);
	ClassDB::bind_static_method("KeyBindings", D_METHOD("chord_of", "event"), &KeyBindings::chord_of);
	ClassDB::bind_static_method("KeyBindings", D_METHOD("get_sections"), &KeyBindings::get_sections);
	ClassDB::bind_method(D_METHOD("get_actions", "section"), &KeyBindings::get_actions);
	ClassDB::bind_method(D_METHOD("get_keys", "action"), &KeyBindings::get_keys);
	ClassDB::bind_method(D_METHOD("set_keys", "action", "keys"), &KeyBindings::set_keys);
	ClassDB::bind_method(D_METHOD("reset_action", "action"), &KeyBindings::reset_action);
	ClassDB::bind_method(D_METHOD("reset_to_defaults"), &KeyBindings::reset_to_defaults);
	ClassDB::bind_method(D_METHOD("conflicts", "action"), &KeyBindings::conflicts);
	ClassDB::bind_method(D_METHOD("is_default", "action"), &KeyBindings::is_default);
	ClassDB::bind_method(D_METHOD("save"), &KeyBindings::save);
	ClassDB::bind_method(D_METHOD("load"), &KeyBindings::load);
	ClassDB::bind_method(D_METHOD("get_config_path"), &KeyBindings::get_config_path);
	ClassDB::bind_method(D_METHOD("set_config_path", "path"), &KeyBindings::set_config_path);
}

KeyBindings::KeyBindings()
{
	table.build_defaults();
}

// The process-wide table is the node directly under the root: the KeyConfig
// autoload (or a stand-in a test puts there). A KeyBindings elsewhere in the
// tree is a plain editable copy.
void KeyBindings::_enter_tree()
{
	if (Engine::get_singleton()->is_editor_hint())
		return;
	if (get_tree() != nullptr && get_parent() == get_tree()->get_root())
		installed = &table;
}

void KeyBindings::_exit_tree()
{
	if (installed == &table)
		installed = nullptr;
}

void KeyBindings::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
		return;
	if (get_tree() != nullptr && get_parent() == get_tree()->get_root())
	{
		installed = &table;
		set_process_input(true);
	}
	load();
}

void KeyBindings::_input(const Ref<InputEvent> &event)
{
	if (installed != &table)
		return;
	Ref<InputEventWithModifiers> m = event;
	if (m.is_null())
		return;
	mods_known = true;
	mod_shift = m->is_shift_pressed();
	mod_ctrl = m->is_ctrl_pressed() || m->is_meta_pressed();
	mod_alt = m->is_alt_pressed();
	// a modifier Input still holds while the event says it is up: its key-up
	// never arrived (macOS system shortcut); release it for everyone
	Input *in = Input::get_singleton();
	const Key stale[] = {KEY_SHIFT, KEY_CTRL, KEY_META, KEY_ALT};
	const bool held[] = {m->is_shift_pressed(), m->is_ctrl_pressed(), m->is_meta_pressed(), m->is_alt_pressed()};
	for (int i = 0; i < 4; i++)
	{
		if (!held[i] && in->is_key_pressed(stale[i]))
		{
			Ref<InputEventKey> up;
			up.instantiate();
			up->set_keycode(stale[i]);
			up->set_physical_keycode(stale[i]);
			up->set_pressed(false);
			in->parse_input_event(up);
		}
	}
}

// ── runtime reads ───────────────────────────────────────────────────────────

bool KeyBindings::chord_pressed(const KeyChord &c, int ignore_mods)
{
	if (!c.is_bound())
		return false;
	Input *in = Input::get_singleton();
	// modifiers: the latest event's state when known (see _input), else Input
	const bool shift = mods_known ? mod_shift : in->is_key_pressed(KEY_SHIFT);
	const bool ctrl = mods_known ? mod_ctrl : (in->is_key_pressed(KEY_CTRL) || in->is_key_pressed(KEY_META));
	const bool alt = mods_known ? mod_alt : in->is_key_pressed(KEY_ALT);
	if (c.key == KEY_CTRL || c.key == KEY_META)
		return ctrl;
	if (c.key == KEY_SHIFT)
		return shift;
	if (c.key == KEY_ALT)
		return alt;
	if (is_modifier_key(c.key))
		return in->is_key_pressed(c.key);
	if (!in->is_key_pressed(c.key))
		return false;
	if (!(ignore_mods & 1) && shift != c.shift)
		return false;
	if (!(ignore_mods & 2) && ctrl != c.ctrl)
		return false;
	if (!(ignore_mods & 4) && alt != c.alt)
		return false;
	return true;
}

bool KeyBindings::pressed(const StringName &action)
{
	const Table *t = active();
	const Action *a = t->find(action);
	if (a == nullptr)
	{
		UtilityFunctions::push_warning("KeyBindings: unknown action ", action);
		return false;
	}
	const auto it = t->section_modifiers.find(a->section.utf8().get_data());
	const int ignore = it == t->section_modifiers.end() ? 0 : it->second;
	for (int i = 0; i < SLOTS; i++)
		if (chord_pressed(a->keys[i], ignore))
			return true;
	return false;
}

bool KeyBindings::just_pressed(const StringName &action)
{
	Table *t = active();
	const bool now = pressed(action);
	Edge &e = t->edges[key_of(action)];
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (e.frame == frame)
		return e.just; // asked twice in one frame: the same answer
	e.just = now && !e.was;
	e.was = now;
	e.frame = frame;
	return e.just;
}

bool KeyBindings::chord_matches(const KeyChord &c, Key keycode, char32_t unicode, bool shift, bool ctrl, bool alt)
{
	if (!c.is_bound())
		return false;
	if (c.key == KEY_CTRL || c.key == KEY_META)
		return keycode == KEY_CTRL || keycode == KEY_META;
	if (is_modifier_key(c.key))
		return keycode == c.key;
	if (unicode != 0 && is_printable(c.key))
	{
		// the typed character: "[" is Option+5 on a Swiss Mac, "Shift+=" is
		// the + of any layout
		const int k = int(c.key);
		if (k >= 'A' && k <= 'Z')
		{
			if (std::towlower(unicode) == std::towlower(k) && shift == c.shift && ctrl == c.ctrl && alt == c.alt)
				return true;
		}
		else if (unicode == (c.shift ? shifted_char(c.key) : plain_char(c.key)) && ctrl == c.ctrl)
			return true;
	}
	return keycode == c.key && shift == c.shift && ctrl == c.ctrl && alt == c.alt;
}

bool KeyBindings::matches_key(const StringName &action, Key keycode, char32_t unicode, bool shift, bool ctrl, bool alt)
{
	const Action *a = active()->find(action);
	if (a == nullptr)
	{
		UtilityFunctions::push_warning("KeyBindings: unknown action ", action);
		return false;
	}
	for (int i = 0; i < SLOTS; i++)
		if (chord_matches(a->keys[i], keycode, unicode, shift, ctrl, alt))
			return true;
	return false;
}

bool KeyBindings::matches(const StringName &action, const Ref<InputEvent> &event)
{
	Ref<InputEventKey> k = event;
	if (k.is_null())
		return false;
	return matches_key(action, k->get_keycode(), char32_t(k->get_unicode()), k->is_shift_pressed(), k->is_ctrl_pressed() || k->is_meta_pressed(), k->is_alt_pressed());
}

String KeyBindings::label(const StringName &action)
{
	const Action *a = active()->find(action);
	if (a == nullptr)
		return "?";
	for (int i = 0; i < SLOTS; i++)
		if (a->keys[i].is_bound())
			return a->keys[i].to_string();
	return "unbound";
}

String KeyBindings::labels(const StringName &action)
{
	const Action *a = active()->find(action);
	if (a == nullptr)
		return "?";
	String s;
	for (int i = 0; i < SLOTS; i++)
		if (a->keys[i].is_bound())
			s += (s.is_empty() ? "" : " / ") + a->keys[i].to_string();
	return s.is_empty() ? String("unbound") : s;
}

String KeyBindings::chord_of(const Ref<InputEvent> &event)
{
	Ref<InputEventKey> k = event;
	if (k.is_null())
		return "";
	KeyChord c;
	c.key = Key(int64_t(k->get_keycode()) & int64_t(KEY_CODE_MASK));
	if (c.key == KEY_NONE || is_modifier_key(c.key))
		return "";
	c.shift = k->is_shift_pressed();
	c.ctrl = k->is_ctrl_pressed() || k->is_meta_pressed();
	c.alt = k->is_alt_pressed();
	// a typed character names the key better than the key code (Option+5 on
	// a Swiss Mac is [, Shift+1 is +): keep the character when it is one of
	// ours, and then it already carries the Shift / Option
	const char32_t u = char32_t(k->get_unicode());
	if (u > 0x20 && u < 0x7f && !(u >= 'a' && u <= 'z') && !(u >= 'A' && u <= 'Z') && !(u >= '0' && u <= '9'))
	{
		for (int base = 0x21; base < 0x7f; base++)
		{
			const Key bk = Key(base);
			if (!is_printable(bk) || (base >= 'A' && base <= 'Z') || (base >= 'a' && base <= 'z'))
				continue;
			if (plain_char(bk) == u)
			{
				c.key = bk;
				c.shift = false;
				c.alt = false;
				break;
			}
			if (shifted_char(bk) == u && shifted_char(bk) != plain_char(bk))
			{
				c.key = bk;
				c.shift = true;
				c.alt = false;
				break;
			}
		}
	}
	return c.to_string();
}

// ── catalog / editing ───────────────────────────────────────────────────────

Array KeyBindings::get_sections()
{
	Array out;
	for (const SectionSpec &s : SECTIONS)
	{
		Dictionary d;
		d["id"] = s.id;
		d["label"] = s.label;
		out.push_back(d);
	}
	return out;
}

Array KeyBindings::get_actions(const String &section) const
{
	Array out;
	for (const Action &a : table.actions)
	{
		if (a.section != section)
			continue;
		Dictionary d;
		d["id"] = a.id;
		d["label"] = a.label;
		PackedStringArray keys, defs;
		for (int i = 0; i < SLOTS; i++)
		{
			keys.push_back(a.keys[i].to_string());
			defs.push_back(a.defaults[i].to_string());
		}
		d["keys"] = keys;
		d["defaults"] = defs;
		out.push_back(d);
	}
	return out;
}

PackedStringArray KeyBindings::get_keys(const StringName &action) const
{
	PackedStringArray out;
	const Action *a = table.find(action);
	if (a != nullptr)
		for (int i = 0; i < SLOTS; i++)
			out.push_back(a->keys[i].to_string());
	return out;
}

void KeyBindings::set_keys(const StringName &action, const PackedStringArray &keys)
{
	Action *a = table.find(action);
	if (a == nullptr)
	{
		UtilityFunctions::push_warning("KeyBindings: unknown action ", action);
		return;
	}
	for (int i = 0; i < SLOTS; i++)
		a->keys[i] = i < keys.size() ? KeyChord::parse(keys[i]) : KeyChord();
	table.rebuild_modifiers();
}

void KeyBindings::reset_action(const StringName &action)
{
	Action *a = table.find(action);
	if (a == nullptr)
		return;
	for (int i = 0; i < SLOTS; i++)
		a->keys[i] = a->defaults[i];
	table.rebuild_modifiers();
}

void KeyBindings::reset_to_defaults()
{
	for (Action &a : table.actions)
		for (int i = 0; i < SLOTS; i++)
			a.keys[i] = a.defaults[i];
	table.rebuild_modifiers();
}

PackedStringArray KeyBindings::conflicts(const StringName &action) const
{
	PackedStringArray out;
	const Action *a = table.find(action);
	if (a == nullptr)
		return out;
	for (const Action &b : table.actions)
	{
		if (b.section != a->section || b.id == a->id || a->shared || b.shared)
			continue;
		bool hit = false;
		for (int i = 0; i < SLOTS && !hit; i++)
			for (int j = 0; j < SLOTS && !hit; j++)
				hit = a->keys[i].is_bound() && a->keys[i] == b.keys[j];
		if (hit)
			out.push_back(String(b.id));
	}
	return out;
}

bool KeyBindings::is_default(const StringName &action) const
{
	const Action *a = table.find(action);
	if (a == nullptr)
		return true;
	for (int i = 0; i < SLOTS; i++)
		if (!(a->keys[i] == a->defaults[i]))
			return false;
	return true;
}

// ── persistence ─────────────────────────────────────────────────────────────

String KeyBindings::resolve_config_path() const
{
	if (!config_path.is_empty())
		return config_path;
	String root;
	if (is_inside_tree())
	{
		Node *ir = get_node_or_null(NodePath("/root/InstallRoot"));
		if (ir != nullptr && ir->has_method("get_install_root"))
			root = ir->call("get_install_root");
	}
	if (root.is_empty())
		root = OS::get_singleton()->get_user_data_dir();
	return root.path_join(CONFIG_FILENAME);
}

String KeyBindings::get_config_path() const
{
	return resolve_config_path();
}

void KeyBindings::set_config_path(const String &path)
{
	config_path = path;
}

void KeyBindings::save()
{
	Ref<ConfigFile> cf;
	cf.instantiate();
	for (const SectionSpec &s : SECTIONS)
		for (const Action &a : table.actions)
		{
			if (a.section != s.id)
				continue;
			String v;
			for (int i = 0; i < SLOTS; i++)
				if (a.keys[i].is_bound())
					v += (v.is_empty() ? "" : ", ") + a.keys[i].to_string();
			cf->set_value(s.id, String(a.id), v);
		}
	const String path = resolve_config_path();
	const Error err = cf->save(path);
	if (err != OK)
		UtilityFunctions::push_error("KeyBindings: cannot write ", path, " (error ", int(err), ")");
	else
		UtilityFunctions::print("KeyBindings: saved ", path);
}

void KeyBindings::load()
{
	reset_to_defaults();
	const String path = resolve_config_path();
	if (!FileAccess::file_exists(path))
		return;
	Ref<ConfigFile> cf;
	cf.instantiate();
	if (cf->load(path) != OK)
	{
		UtilityFunctions::push_warning("KeyBindings: cannot read ", path, "; defaults apply");
		return;
	}
	int n = 0;
	std::vector<bool> stored(table.actions.size(), false);
	for (size_t ai = 0; ai < table.actions.size(); ai++)
	{
		Action &a = table.actions[ai];
		if (!cf->has_section_key(a.section, String(a.id)))
			continue;
		const String v = cf->get_value(a.section, String(a.id), "");
		const PackedStringArray parts = v.split(",", false);
		for (int i = 0; i < SLOTS; i++)
			a.keys[i] = i < parts.size() ? KeyChord::parse(parts[i]) : KeyChord();
		stored[ai] = true;
		n++;
	}
	// An action the file does not know yet (added since the file was written)
	// keeps its default key. When a stored action of the same section still
	// sits on that key because the key used to be ITS default (the file
	// documents every key, so an untouched action is stored too), that action
	// moves on to its current default: the catalog's new layout wins over a
	// stale default, a deliberate rebinding stays.
	for (size_t ai = 0; ai < table.actions.size(); ai++)
	{
		const Action &fresh = table.actions[ai];
		if (stored[ai] || fresh.shared)
			continue;
		for (int fi = 0; fi < SLOTS; fi++)
		{
			if (!fresh.keys[fi].is_bound())
				continue;
			for (size_t bi = 0; bi < table.actions.size(); bi++)
			{
				Action &b = table.actions[bi];
				if (bi == ai || !stored[bi] || b.shared || b.section != fresh.section)
					continue;
				bool on_default = true;
				for (int i = 0; i < SLOTS; i++)
					on_default = on_default && b.keys[i] == b.defaults[i];
				if (on_default)
					continue;
				for (int i = 0; i < SLOTS; i++)
				{
					if (!(b.keys[i] == fresh.keys[fi]))
						continue;
					for (int j = 0; j < SLOTS; j++)
						b.keys[j] = b.defaults[j];
					UtilityFunctions::print("KeyBindings: ", String(fresh.id), " is new on ", fresh.keys[fi].to_string(), "; ", String(b.id), " moves to its default ", b.keys[0].to_string());
					break;
				}
			}
		}
	}
	table.rebuild_modifiers();
	UtilityFunctions::print("KeyBindings: ", n, " actions from ", path);
}

} // namespace jsbgodot
