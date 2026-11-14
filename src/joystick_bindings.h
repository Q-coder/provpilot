#ifndef JOYSTICK_BINDINGS_H
#define JOYSTICK_BINDINGS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    // Joystick / gamepad bindings and calibration, plus the runtime accessors
    // gameplay code uses (formerly joystick_config.gd; mounted as the
    // `JoystickConfig` autoload from joystick_config.tscn).
    //
    // Every controller maps stick directions, half-axes and button indices
    // differently, so the read sites call get_axis("slew_vertical") and the
    // user picks an axis + calibrates it from Settings > Joystick. Bindings
    // persist in the shared provpilot.cfg (InstallRoot) under "device",
    // "axis.<action>" and "button.<action>".
    //
    // axis_bindings[action] = {axis (-1 unbound), invert, min, center, max,
    //                          deadzone, curve}; button_bindings[action] = {button}.
    class JoystickBindings : public Node
    {
        GDCLASS(JoystickBindings, Node);

    public:
        enum AxisCurve { CURVE_LINEAR = 0, CURVE_SQUARED = 1, CURVE_CUBED = 2 };

        static void _bind_methods();
        JoystickBindings();

        void _ready() override;

        // The action catalog: [{id, label}] for the settings UI.
        static Array get_axis_actions();
        static Array get_button_actions();
        static Dictionary axis_default(int axis, bool invert);
        static Dictionary axis_unbound();

        // The active pad device id, or -1 if no pad is connected.
        int get_active_device() const;
        // Raw axis value (settings UI live preview).
        double get_raw_axis(const String &action) const;
        // Calibrated axis in [-1, 1]: centre/min/max remap, deadzone, invert, curve.
        double get_axis(const String &action) const;
        // The pad axis index an action is bound to, -1 when unbound.
        int get_bound_axis(const String &action) const;
        static double apply_calibration(double raw, const Dictionary &b);
        bool get_button(const String &action) const;
        // True on the first frame a button action is held (edge-detected in
        // _process, so every consumer in the same frame sees it).
        bool is_button_just_pressed(const String &action) const;
        void _process(double delta) override;
        // The axis that moved farthest from 0 (>= threshold), or -1.
        int detect_active_axis(double threshold = 0.5) const;
        // The first pressed button, or -1.
        int detect_active_button() const;

        void save();
        void reset_to_defaults();
        // Discard unsaved changes: the bindings as provpilot.cfg holds them.
        void reload();

        Dictionary get_axis_bindings() const; void set_axis_bindings(const Dictionary &d);
        Dictionary get_button_bindings() const; void set_button_bindings(const Dictionary &d);
        int get_device_id() const; void set_device_id(int id);

    protected:
        Dictionary axis_bindings;
        Dictionary button_bindings;
        int device_id = -1; // -1 = first connected pad
        Dictionary button_now;   // action -> pressed this frame
        Dictionary button_prev;  // ... and the frame before

        void apply_defaults();
        // Godot's built-in ui_* actions carry D-pad / left-stick / A-button
        // events: a pad moved the focus in the settings screen while Detect
        // waited for exactly that movement. The menus are mouse and keyboard
        // driven, so those joypad events are removed from the input map.
        static void disable_ui_joypad_navigation();
        void load();
        Node *install_root() const;
    };

} // namespace jsbgodot

VARIANT_ENUM_CAST(jsbgodot::JoystickBindings::AxisCurve);

#endif
