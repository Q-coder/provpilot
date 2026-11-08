#ifndef KEY_BINDINGS_H
#define KEY_BINDINGS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <string>
#include <unordered_map>
#include <vector>

using namespace godot;

namespace jsbgodot
{

    // The keyboard mapping (Settings > Keyboard, keyboard.cfg next to
    // provpilot.cfg): every key the simulator reads goes through an action of
    // the catalog below, in three sections — flight, slew and editor. A call
    // site asks KeyBindings::pressed("flaps_down") (polled, every frame),
    // just_pressed (the first frame only) or matches("undo", key_event)
    // (event style) and never names a key itself; label("undo") gives the
    // chord for hints ("Ctrl+Z").
    //
    // A chord is one key plus modifiers, written "Ctrl+Shift+F", "PageUp",
    // "[", "Kp+"; Ctrl stands for Ctrl or Cmd. An action has up to two
    // chords ("Q, PageUp"). Polled matching ignores a modifier that the same
    // section binds on its own (Shift = fast in slew and the editor), so W
    // still moves while Shift speeds it up; event matching compares the
    // modifiers exactly and, for a printable key, also accepts the typed
    // character, so "[" is Option+5 on a Swiss Mac and "Shift+[" is the {
    // of any layout.
    //
    // The table in use is process-wide: the KeyConfig autoload installs its
    // own on ready; without one (tests, tools) the defaults apply.
    struct KeyChord
    {
        Key key = KEY_NONE;
        bool shift = false;
        bool ctrl = false; // Ctrl or Cmd
        bool alt = false;

        bool is_bound() const { return key != KEY_NONE; }
        bool operator==(const KeyChord &o) const { return key == o.key && shift == o.shift && ctrl == o.ctrl && alt == o.alt; }
        // "Ctrl+Shift+F"; "" when unbound.
        String to_string() const;
        // The reverse of to_string; KEY_NONE when the text is not a chord.
        static KeyChord parse(const String &text);
        static String key_name(Key key);
        static Key key_from_name(const String &name);
    };

    class KeyBindings : public Node
    {
        GDCLASS(KeyBindings, Node);

    public:
        static constexpr int SLOTS = 2;
        static constexpr const char *CONFIG_FILENAME = "keyboard.cfg";

        struct Action
        {
            StringName id;
            String section;
            String label;
            KeyChord defaults[SLOTS];
            KeyChord keys[SLOTS];
            bool shared = false; // never reported as a conflict (its own phase, e.g. the intro)
        };

        static void _bind_methods();
        KeyBindings();

        void _enter_tree() override;
        void _exit_tree() override;
        void _ready() override;
        // The installed node watches every event for the real modifier state:
        // macOS swallows the key-up of Cmd / Shift after a system shortcut
        // (Cmd+Shift+4 screenshot) and Input keeps them "pressed" for good —
        // C then matches nothing and H reads as Ctrl+Shift+H. The polled
        // matching uses the modifiers of the latest event instead, and a
        // stale Input state is released with synthetic key-ups.
        void _input(const Ref<InputEvent> &event) override;

        // ── runtime reads (static: the active table) ──
        static bool pressed(const StringName &action);
        static bool just_pressed(const StringName &action);
        static bool matches(const StringName &action, const Ref<InputEvent> &event);
        static bool matches_key(const StringName &action, Key keycode, char32_t unicode, bool shift, bool ctrl, bool alt);
        // The first chord's text ("Ctrl+Z"), or "unbound".
        static String label(const StringName &action);
        // Every chord's text, " / "-joined ("Q / PageUp").
        static String labels(const StringName &action);
        // The chord of a key event, as text ("" for a bare modifier press).
        static String chord_of(const Ref<InputEvent> &event);

        // ── catalog / editing (the settings panel) ──
        // [{id, label}] in display order.
        static Array get_sections();
        // [{id, label, keys: PackedStringArray[SLOTS], defaults: PackedStringArray[SLOTS]}]
        Array get_actions(const String &section) const;
        PackedStringArray get_keys(const StringName &action) const;
        void set_keys(const StringName &action, const PackedStringArray &keys);
        void reset_action(const StringName &action);
        void reset_to_defaults();
        // Other actions of the same section sharing a chord with this one.
        PackedStringArray conflicts(const StringName &action) const;
        bool is_default(const StringName &action) const;

        void save();
        void load();
        String get_config_path() const;
        void set_config_path(const String &path);

    protected:
        struct Edge
        {
            bool was = false;
            bool just = false;
            uint64_t frame = ~uint64_t(0);
        };
        struct Table
        {
            std::vector<Action> actions;
            std::unordered_map<std::string, int> index;
            std::unordered_map<std::string, Edge> edges;
            // per section: the modifiers bound on their own (ignored when polling)
            std::unordered_map<std::string, int> section_modifiers; // bit 1 shift, 2 ctrl, 4 alt
            void build_defaults();
            void rebuild_modifiers();
            const Action *find(const StringName &id) const;
            Action *find(const StringName &id);
        };
        Table table;
        String config_path;

        static Table *active();
        static godot::PackedStringArray split_chords(const godot::String &value);
        static Table *installed;
        static bool mods_known;
        static bool mod_shift, mod_ctrl, mod_alt;
        static Table &defaults();
        static bool chord_pressed(const KeyChord &c, int ignore_mods);
        static bool chord_matches(const KeyChord &c, Key keycode, char32_t unicode, bool shift, bool ctrl, bool alt);
        String resolve_config_path() const;
    };

} // namespace jsbgodot

#endif
