#ifndef SLEW_MODE_H
#define SLEW_MODE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace jsbgodot
{

    class JSBGodot;

    // Free-movement "slew mode" (Shift+Y), moved out of node_3d.gd so the
    // per-frame movement math runs in C++ (double precision).
    //
    // While active, JSBSim physics / position updates are paused (via
    // JSBGodot::set_slew_mode) and the aircraft is repositioned directly:
    //   WASD / gamepad "slew_strafe" + "slew_forward"   horizontal move
    //   Q/E/PageUp/PageDown / "slew_vertical"           vertical move
    //   gamepad "slew_yaw"                              yaw (2 rad/s)
    //   hold Shift                                      5x speed
    // On exit, JSBSim reinitialises at the new position with the airspeed
    // saved on entry.
    //
    // GDScript (node_3d.gd) registers the AC root + HUD labels once in _ready
    // (both are session-stable, like CameraRig's cameras), pushes the Swiss
    // LV95 origin after TERRAIN_CENTER is locked, and calls update() once per
    // frame with the calibrated joystick axes (from JoystickConfig) and the
    // terrain height under the aircraft (a scenery-stack query). Keyboard
    // input is read directly through Godot's Input singleton, exactly as the
    // historical GDScript did.
    class SlewMode : public RefCounted
    {
        GDCLASS(SlewMode, RefCounted);

    public:
        static void _bind_methods();

        // ── Node registration (session-stable; no reset needed) ──
        // ac_node = aircraft root ($AC). The eight labels are the normal HUD
        // readouts (BottomRow Label..Label5) plus Control/Label6 (Godot
        // position), Label7 (Swiss coordinates) and Label8 (slew heading).
        // Null labels are skipped.
        void register_nodes(Node3D *ac_node,
                            Label *airspeed_label, Label *altitude_label,
                            Label *vspeed_label, Label *throttle_label,
                            Label *heading_label, Label *godot_pos_label,
                            Label *swiss_label, Label *heading_deg_label);
        // Swiss LV95 origin (TERRAIN_CENTER_E/N) for the coordinate readout.
        void set_swiss_center(double easting, double northing);

        // ── Per-frame update ──
        // Handles the Shift+Y toggle and, while active, the free movement +
        // slew HUD. Returns true while slew mode is active (GDScript then
        // skips flight controls and the normal HUD). axis_* are the calibrated
        // [-1, 1] JoystickConfig values; terrain_height_at_ac is NAN when the
        // scenery has no data there; joy_device is the active joystick index
        // (-1 = none), used for the entry debug dump.
        bool update(double delta, Object *p_jsb,
                    double axis_strafe, double axis_forward,
                    double axis_yaw, double axis_vertical,
                    double terrain_height_at_ac, int joy_device);

        bool is_enabled() const;
        // Airspeed (kts) saved when entering slew mode.
        double get_preserved_airspeed_kts() const;

        // Teleport flow (welcome screen / Shift+P): JSBSim's own slew flag is
        // managed by the caller; this only clears the module state + HUD so
        // the slew movement UI is not shown.
        void note_external_exit();

    protected:
        SlewMode();

        // Tunables (defaults = historical GDScript constants)
        double slew_speed = 400.0;          // m/s
        double slew_fast_multiplier = 5.0;  // hold Shift
        double slew_vertical_speed = 60.0;  // m/s
        double yaw_rate = 2.0;              // rad/s
        double yaw_deadzone = 0.1;

        bool enabled = false;
        bool key_held = false;
        double pre_slew_airspeed_kts = 0.0;

        Node3D *ac_node = nullptr;
        Label *airspeed_label = nullptr;
        Label *altitude_label = nullptr;
        Label *vspeed_label = nullptr;
        Label *throttle_label = nullptr;
        Label *heading_label = nullptr;
        Label *godot_pos_label = nullptr;
        Label *swiss_label = nullptr;
        Label *heading_deg_label = nullptr;
        double swiss_center_e = 0.0;
        double swiss_center_n = 0.0;

        // Terrain height under the aircraft from the last update() call —
        // used for the gear-clearance lift when exiting slew mode (same
        // position, so equivalent to a fresh query).
        double last_terrain_height = 0.0;

        void toggle(JSBGodot *fdm, int joy_device);
        void handle_movement(double delta, double ax_strafe, double ax_forward,
                             double ax_yaw, double ax_vertical, double terrain_h);
        static String format_swiss_coord(double value);
    };

} // namespace jsbgodot

#endif