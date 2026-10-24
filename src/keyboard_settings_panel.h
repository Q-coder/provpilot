#ifndef KEYBOARD_SETTINGS_PANEL_H
#define KEYBOARD_SETTINGS_PANEL_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <vector>

using namespace godot;

namespace jsbgodot
{

    class KeyBindings;

    // Settings > Keyboard: the key mapping by section (Flight, Slew, Scenery
    // editor). Every action has two key buttons; a click arms the button and
    // the next key press becomes the chord (Escape cancels, Backspace clears a
    // slot, a modifier on its own binds when it is released alone). Conflicts
    // inside a section are marked next to the row. Reads and writes go through
    // the KeyConfig autoload (KeyBindings); Save / Cancel / Restore Defaults of
    // the settings screen persist or discard them.
    class KeyboardSettingsPanel : public VBoxContainer
    {
        GDCLASS(KeyboardSettingsPanel, VBoxContainer);

    public:
        static void _bind_methods();
        KeyboardSettingsPanel();

        void _ready() override;
        void _input(const Ref<InputEvent> &event) override;
        void handle_key_event(const Ref<InputEvent> &event);
        // Re-read every row from KeyConfig (Restore Defaults / Cancel).
        void refresh();
        // The action and slot being captured ("" when none).
        String get_capturing_action() const { return capturing_action; }
        int get_capturing_slot() const { return capturing_slot; }

        void _on_key_button(const String &action, int slot);
        void _on_reset_button(const String &action);

    protected:
        struct ActionRow
        {
            String action;
            Button *keys[2] = {nullptr, nullptr};
            Button *reset = nullptr;
            Label *conflict = nullptr;
        };
        std::vector<ActionRow> rows;
        Label *status = nullptr;
        String capturing_action;
        int capturing_slot = -1;
        bool capture_saw_modifier = false;

        KeyBindings *config() const;
        void build_ui();
        void build_row(VBoxContainer *parent, const String &action, const String &label);
        void refresh_row(ActionRow &row);
        void begin_capture(const String &action, int slot);
        void end_capture();
        void assign(const String &action, int slot, const String &chord);
    };

} // namespace jsbgodot

#endif
