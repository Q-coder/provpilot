#include "keyboard_settings_panel.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "key_bindings.h"

using namespace godot;

namespace jsbgodot
{

static const int FONT_SM = 18, FONT_MD = 22, FONT_LG = 26;
static const int LABEL_W = 430, KEY_W = 170;
static const Color LABEL_COLOR(0.75, 0.78, 0.82);
static const Color SECTION_COLOR(0.25, 0.55, 0.85, 1.0);
static const Color HINT_COLOR(0.55, 0.6, 0.65);
static const Color CONFLICT_COLOR(0.95, 0.45, 0.35);
static const Color CAPTURE_COLOR(1.0, 0.85, 0.3);

void KeyboardSettingsPanel::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("refresh"), &KeyboardSettingsPanel::refresh);
	ClassDB::bind_method(D_METHOD("handle_key_event", "event"), &KeyboardSettingsPanel::handle_key_event);
	ClassDB::bind_method(D_METHOD("get_capturing_action"), &KeyboardSettingsPanel::get_capturing_action);
	ClassDB::bind_method(D_METHOD("get_capturing_slot"), &KeyboardSettingsPanel::get_capturing_slot);
	ClassDB::bind_method(D_METHOD("_on_key_button", "action", "slot"), &KeyboardSettingsPanel::_on_key_button);
	ClassDB::bind_method(D_METHOD("_on_reset_button", "action"), &KeyboardSettingsPanel::_on_reset_button);
}

KeyboardSettingsPanel::KeyboardSettingsPanel()
{
}

KeyBindings *KeyboardSettingsPanel::config() const
{
	if (!is_inside_tree())
		return nullptr;
	KeyBindings *kb = Object::cast_to<KeyBindings>(get_node_or_null(NodePath("/root/KeyConfig")));
	if (kb != nullptr)
		return kb;
	// no autoload (a test scene): any KeyBindings under the root
	Node *root = get_tree()->get_root();
	for (int i = 0; i < root->get_child_count(); i++)
		if ((kb = Object::cast_to<KeyBindings>(root->get_child(i))) != nullptr)
			return kb;
	return nullptr;
}

static Label *styled_label(const String &text, int size, const Color &color, int min_w = 0)
{
	Label *l = memnew(Label);
	l->set_text(text);
	l->add_theme_font_size_override("font_size", size);
	l->add_theme_color_override("font_color", color);
	if (min_w > 0)
		l->set_custom_minimum_size(Vector2(min_w, 0));
	return l;
}

void KeyboardSettingsPanel::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
		return;
	add_theme_constant_override("separation", 6);
	build_ui();
	refresh();
	set_process_input(true);
}

void KeyboardSettingsPanel::build_ui()
{
	add_child(styled_label("Keyboard", FONT_LG, SECTION_COLOR));
	Label *help = styled_label(String::utf8("  \xE2\x80\xA2 Click a key, then press the new key or combination. Escape keeps the old one, Backspace clears it.\n  \xE2\x80\xA2 Shift, Ctrl or Alt alone: press and release it without another key. Ctrl also stands for Cmd.\n  \xE2\x80\xA2 Two keys per action; a key used twice in the same section is marked. Save writes keyboard.cfg next to provpilot.cfg."), FONT_SM, HINT_COLOR);
	help->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
	add_child(help);
	status = styled_label("", FONT_SM, CAPTURE_COLOR);
	add_child(status);
	KeyBindings *kb = config();
	const Array sections = KeyBindings::get_sections();
	for (int s = 0; s < sections.size(); s++)
	{
		const Dictionary sec = sections[s];
		add_child(memnew(HSeparator));
		add_child(styled_label(sec["label"], FONT_MD, SECTION_COLOR));
		if (kb == nullptr)
			continue;
		const Array actions = kb->get_actions(sec["id"]);
		for (int i = 0; i < actions.size(); i++)
		{
			const Dictionary a = actions[i];
			build_row(this, a["id"], a["label"]);
		}
	}
	if (kb == nullptr)
		add_child(styled_label("KeyConfig autoload missing: nothing to edit", FONT_SM, CONFLICT_COLOR));
}

void KeyboardSettingsPanel::build_row(VBoxContainer *parent, const String &action, const String &label)
{
	HBoxContainer *h = memnew(HBoxContainer);
	h->add_theme_constant_override("separation", 10);
	h->set_name(action);
	parent->add_child(h);
	Label *name = styled_label(label, FONT_MD, LABEL_COLOR, LABEL_W);
	name->set_clip_text(true); // the key columns stay aligned whatever the label length
	h->add_child(name);
	ActionRow row;
	row.action = action;
	for (int slot = 0; slot < 2; slot++)
	{
		Button *b = memnew(Button);
		b->set_custom_minimum_size(Vector2(KEY_W, 34));
		b->add_theme_font_size_override("font_size", FONT_SM);
		b->set_name(vformat("Key%d", slot));
		b->set_tooltip_text(slot == 0 ? "The key (click, then press one)" : "A second key for the same action");
		b->connect("pressed", Callable(this, "_on_key_button").bind(action, slot));
		h->add_child(b);
		row.keys[slot] = b;
	}
	row.reset = memnew(Button);
	row.reset->set_text("Reset");
	row.reset->set_name("Reset");
	row.reset->set_custom_minimum_size(Vector2(80, 34));
	row.reset->add_theme_font_size_override("font_size", FONT_SM);
	row.reset->set_tooltip_text("Back to the default key");
	row.reset->connect("pressed", Callable(this, "_on_reset_button").bind(action));
	h->add_child(row.reset);
	row.conflict = styled_label("", FONT_SM, CONFLICT_COLOR);
	row.conflict->set_name("Conflict");
	h->add_child(row.conflict);
	rows.push_back(row);
}

void KeyboardSettingsPanel::refresh_row(ActionRow &row)
{
	KeyBindings *kb = config();
	if (kb == nullptr)
		return;
	const PackedStringArray keys = kb->get_keys(row.action);
	for (int slot = 0; slot < 2; slot++)
	{
		const bool capturing = capturing_action == row.action && capturing_slot == slot;
		const String text = slot < keys.size() && !keys[slot].is_empty() ? keys[slot] : String::utf8("—");
		row.keys[slot]->set_text(capturing ? String::utf8("press a key…") : text);
		if (capturing)
			row.keys[slot]->add_theme_color_override("font_color", CAPTURE_COLOR);
		else
			row.keys[slot]->remove_theme_color_override("font_color");
	}
	row.reset->set_disabled(kb->is_default(row.action));
	const PackedStringArray c = kb->conflicts(row.action);
	row.conflict->set_text(c.is_empty() ? String() : "also: " + String(", ").join(c));
}

void KeyboardSettingsPanel::refresh()
{
	for (ActionRow &row : rows)
		refresh_row(row);
}

void KeyboardSettingsPanel::_on_key_button(const String &action, int slot)
{
	if (capturing_action == action && capturing_slot == slot)
		end_capture();
	else
		begin_capture(action, slot);
}

void KeyboardSettingsPanel::_on_reset_button(const String &action)
{
	KeyBindings *kb = config();
	if (kb == nullptr)
		return;
	end_capture();
	kb->reset_action(action);
	refresh();
}

void KeyboardSettingsPanel::begin_capture(const String &action, int slot)
{
	capturing_action = action;
	capturing_slot = slot;
	capture_saw_modifier = false;
	if (status != nullptr)
		status->set_text("Press the new key (Escape keeps the old one, Backspace clears it)");
	refresh();
}

void KeyboardSettingsPanel::end_capture()
{
	capturing_action = "";
	capturing_slot = -1;
	capture_saw_modifier = false;
	if (status != nullptr)
		status->set_text("");
	refresh();
}

void KeyboardSettingsPanel::assign(const String &action, int slot, const String &chord)
{
	KeyBindings *kb = config();
	if (kb == nullptr)
		return;
	PackedStringArray keys = kb->get_keys(action);
	while (keys.size() < 2)
		keys.push_back("");
	keys[slot] = chord;
	kb->set_keys(action, keys);
}

void KeyboardSettingsPanel::_input(const Ref<InputEvent> &event)
{
	handle_key_event(event);
}

// The capture itself (also the test hook: _input is not callable from a script).
void KeyboardSettingsPanel::handle_key_event(const Ref<InputEvent> &event)
{
	if (capturing_action.is_empty() || !is_visible_in_tree())
		return;
	Ref<InputEventKey> k = event;
	if (k.is_null() || k->is_echo())
		return;
	const Key code = Key(int64_t(k->get_keycode()) & int64_t(KEY_CODE_MASK));
	const bool modifier = code == KEY_SHIFT || code == KEY_CTRL || code == KEY_ALT || code == KEY_META;
	get_viewport()->set_input_as_handled();
	if (!k->is_pressed())
	{
		// a modifier pressed and released on its own is the chord
		if (modifier && capture_saw_modifier)
		{
			assign(capturing_action, capturing_slot, KeyChord::key_name(code == KEY_META ? KEY_CTRL : code));
			end_capture();
		}
		return;
	}
	if (modifier)
	{
		capture_saw_modifier = true;
		return;
	}
	capture_saw_modifier = false;
	if (code == KEY_ESCAPE)
	{
		end_capture();
		return;
	}
	if (code == KEY_BACKSPACE && !k->is_shift_pressed() && !k->is_ctrl_pressed() && !k->is_meta_pressed() && !k->is_alt_pressed())
	{
		assign(capturing_action, capturing_slot, "");
		end_capture();
		return;
	}
	const String chord = KeyBindings::chord_of(event);
	if (chord.is_empty())
		return;
	assign(capturing_action, capturing_slot, chord);
	end_capture();
}

} // namespace jsbgodot
