#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

#include "settings_store.h"

using namespace godot;

namespace jsbgodot
{

    // Settings screen: a full-screen CanvasLayer with one tab per subject
    // (Scenery, Downloads, Graphics, World, Map, Sound, Realism, Joystick)
    // and Restore Defaults / Cancel / Save at the bottom. Opened from the
    // welcome screen; emits `closed` after Save or Cancel.
    //
    // All values live in a SettingsStore (get_store()); this class only maps
    // them to controls. Two panels stay GDScript and are mounted as tabs:
    // scenery_downloader.gd (Downloads) and joystick_settings_panel.gd
    // (Joystick — bindings persist through the JoystickConfig autoload).
    class SettingsScreen : public CanvasLayer
    {
        GDCLASS(SettingsScreen, CanvasLayer);

    public:
        static void _bind_methods();
        SettingsScreen();

        void _ready() override;
        void _input(const Ref<InputEvent> &event) override;

        void show_settings();
        void hide_settings();
        // Live weather display: while the Live Weather box is ticked the
        // Weather tab shows these derived layers read-only (plus a status
        // line) instead of the editable static ones. Empty layers + status
        // = no report yet. The store's static layers are never touched.
        void set_live_weather(const Array &layers, const String &status);
        void clear_live_weather();
        Ref<SettingsStore> get_store() const;
        Variant get_setting(const String &key) const;
        // Re-push store values into every control (after external changes).
        void refresh();
        // Read every control back into the store (no disk write).
        void commit();

        // Bottom-bar actions (bound so the buttons can call them).
        void _on_save();
        void _on_cancel();
        void _on_restore_defaults();

        // Scenery-library table actions.
        void _on_scan_scenery();
        void _on_add_scenery_area();
        void _on_area_dir_selected(const String &path);
        void _on_area_dialog_closed();
        void _on_move_selected_up();
        void _on_move_selected_down();
        void _on_remove_selected();
        void _on_area_toggled(bool enabled, int idx);
        void _on_area_priority_changed(double value, int idx);
        void _on_area_row_gui_input(const Ref<InputEvent> &event, int idx);
        // Weather-layer table actions.
        void _on_weather_add_layer();
        void _on_live_weather_toggled(bool pressed);
        void _on_weather_remove_layer(int idx);
        void _on_weather_spin_changed(double value, int idx);
        void _on_weather_option_changed(int value, int idx);

        // Generic row callbacks (row index bound at connect time).
        void _on_slider_value_changed(double value, int row);
        void _on_time_slider_value_changed(double value, int row);

        // Scenery downloader state bridge (scenery_downloader.gd Callables).
        Dictionary _downloader_get_state() const;
        void _downloader_set_state(const Dictionary &state);
        void _downloader_area_installed(const String &path);

    protected:
        // ── Look ──
        static constexpr int FONT_SM = 20;
        static constexpr int FONT_MD = 24;
        static constexpr int FONT_LG = 28;
        static constexpr int FONT_XL = 36;
        static constexpr int LABEL_MIN_W = 220;
        static const Color BG_COLOR;
        static const Color PANEL_COLOR;
        static const Color SECTION_COLOR;
        static const Color LABEL_COLOR;
        static const Color HINT_COLOR;

        enum RowKind
        {
            ROW_SLIDER,
            ROW_TIME_SLIDER,
            ROW_CHECK,
            ROW_OPTION,
        };
        // One settings key <-> one control. Slider rows also own the readout
        // label and its unit suffix.
        struct Row
        {
            String key;
            RowKind kind;
            Control *control = nullptr;
            Label *value_label = nullptr;
            String suffix;
        };
        std::vector<Row> rows;

        Ref<SettingsStore> store;
        TabContainer *tabs = nullptr;
        VBoxContainer *scenery_table = nullptr;
        VBoxContainer *weather_table = nullptr;
        // Per-layer controls in table order (rebuilt by _refresh_weather_table).
        struct WeatherRow
        {
            SpinBox *base = nullptr;
            SpinBox *top = nullptr;
            SpinBox *wind_dir = nullptr;
            SpinBox *wind_speed = nullptr;
            OptionButton *turbulence = nullptr;
            OptionButton *cloud_type = nullptr;
            OptionButton *coverage = nullptr;
        };
        std::vector<WeatherRow> weather_rows;
        CheckBox *live_weather_check = nullptr;
        Label *weather_status = nullptr;
        Button *weather_add_button = nullptr;
        Array live_weather_layers;
        String live_weather_status;
        bool _live_weather_shown() const;
        Node *joystick_panel = nullptr;
        Node *downloader_panel = nullptr;
        Node *area_dialog = nullptr;    // live "Add Area..." FileDialog, if any
        int selected_area_idx = -1;
        // True while refresh() pushes values into controls, so programmatic
        // slider changes don't trip user-interaction side effects.
        bool applying_ui = false;

        // ── Construction ──
        void _build_ui();
        VBoxContainer *_add_tab(const String &title);
        void _build_scenery_tab();
        void _build_downloads_tab();
        void _build_graphics_tab();
        void _build_world_tab();
        void _build_weather_tab();
        void _build_map_tab();
        void _build_sound_tab();
        void _build_realism_tab();
        void _build_joystick_tab();

        Label *_make_label(const String &text, int font_size, const Color &color, int min_w = 0);
        void _add_section_header(VBoxContainer *parent, const String &text);
        void _add_hint(VBoxContainer *parent, const String &text);
        void _add_slider_row(VBoxContainer *parent, const String &key, const String &label, double min_v, double max_v, double step, const String &suffix);
        void _add_time_slider_row(VBoxContainer *parent, const String &key, const String &label);
        void _add_check_row(VBoxContainer *parent, const String &key, const String &label, const String &description);
        void _add_option_row(VBoxContainer *parent, const String &key, const String &label, const PackedStringArray &options);
        Control *_make_button(const String &text, int min_w, int font_size, const Callable &on_pressed);
        // Instantiate a GDScript panel (res:// path) as a Node, or nullptr.
        Node *_instantiate_script(const String &res_path);

        // ── State <-> UI ──
        void _apply_to_ui();
        void _read_from_ui();
        void _refresh_scenery_table();
        void _refresh_weather_table();
        // Read the layer controls back into the store's weather_layers array.
        void _read_weather_table();
        void _select_area(int idx);
        Row *_find_row(const String &key);
        Node *_joystick_config() const;
        void _refresh_joystick_panel();
    };

} // namespace jsbgodot

#endif
