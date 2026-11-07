#ifndef JOYSTICK_SETTINGS_PANEL_H
#define JOYSTICK_SETTINGS_PANEL_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/templates/hash_map.hpp>

using namespace godot;

namespace jsbgodot
{

    class JoystickBindings;

    // Settings > Joystick tab (formerly joystick_settings_panel.gd): binding and
    // calibrating gamepad axes / buttons. All reads and writes of bindings go
    // through the JoystickConfig autoload (JoystickBindings).
    class JoystickSettingsPanel : public VBoxContainer
    {
        GDCLASS(JoystickSettingsPanel, VBoxContainer);

    public:
        static void _bind_methods();
        JoystickSettingsPanel();

        void _ready() override;
        void _process(double delta) override;
        // Re-read the state from JoystickConfig (Restore Defaults / Cancel).
        void refresh();

        void _on_detect_axis(const String &action);
        void _on_detect_button(const String &action);
        // The axis / button pickers: an OptionButton of named pad inputs,
        // "None" (id -1) leaves the action unassigned — there are more actions
        // than a pad has buttons.
        void _on_axis_selected(int idx, const String &action);
        void _on_button_selected(int idx, const String &action);
        void _on_invert_toggled(bool pressed, const String &action);
        void _on_deadzone_changed(double value, const String &action);
        void _on_curve_selected(int idx, const String &action);
        void _on_capture_endpoint(const String &action, const String &which);

    protected:
        struct AxisRow
        {
            OptionButton *opt = nullptr;
            Button *detect_btn = nullptr;
            ProgressBar *raw_bar = nullptr;
            ProgressBar *cal_bar = nullptr;
            CheckBox *invert = nullptr;
            HSlider *dz_slider = nullptr;
            OptionButton *curve_opt = nullptr;
        };
        struct ButtonRow
        {
            OptionButton *opt = nullptr;
            Button *detect_btn = nullptr;
            Label *pressed_lbl = nullptr;
        };
        HashMap<String, AxisRow> axis_rows;
        HashMap<String, ButtonRow> button_rows;
        Label *device_status = nullptr;
        String detecting_axis;
        String detecting_button;
        PackedFloat32Array detect_axis_baseline;

        JoystickBindings *config() const;
        void build_ui();
        void build_axis_row(const String &action_id, const String &label_text);
        static void fill_axis_options(OptionButton *opt);
        static void fill_button_options(OptionButton *opt);
        // Select the item with this id, adding "Axis / Button N" for an index
        // the list does not name (a pad with more inputs than SDL names).
        static void select_id(OptionButton *opt, int id, const char *generic);
        // The pad index an item of a picker stands for (its metadata; -1 = None).
        static int picked_index(OptionButton *opt, int item);
        void build_button_row(const String &action_id, const String &label_text);
        void refresh_from_config();
        void poll_axis_detection();
        void poll_button_detection();
        void assign_axis(const String &action, int axis_idx);
        void assign_button(const String &action, int button_idx);
    };

} // namespace jsbgodot

#endif
