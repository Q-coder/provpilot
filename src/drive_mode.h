#ifndef DRIVE_MODE_H
#define DRIVE_MODE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

namespace jsbgodot
{

    class WorldScenery;

    // Ground vehicles (the welcome screen's "Vehicles" frame): a kinematic
    // car that drives the AC node over the drawn ground, no physics engine.
    // JSBSim stays paused (slew flag) the whole time, exactly as in slew mode,
    // so the aircraft machinery (cameras, HUD labels, map, spawn) keeps working
    // on the same node.
    //
    // Model: a bicycle model on the LV95 plane. State = position (e, n), true
    // heading, speed along the heading, steering angle. Throttle and brake are
    // accelerations, the brake held at standstill reverses, drag and rolling
    // resistance slow a coasting car, the steering lock shrinks with speed and
    // self-centres. Every frame the ground height is sampled under the four
    // wheels (WorldScenery::ground_height_below: terrain plus the drawn
    // runway / taxiway / apron surface), the car sits on the centre height and
    // pitches / rolls with the wheel heights.
    //
    // Flow (node_3d.gd): arm() on Fly! with a vehicle, update() every frame;
    // the drive engages once the spawn's rebase cooldown has released the
    // node, and disengage() on the next Fly! / Edit.
    //
    // Keys (Settings > Keyboard, section Vehicle): drive_accelerate,
    // drive_brake, drive_left, drive_right, drive_handbrake; gamepad (Settings
    // > Joystick): drive_steer, drive_accelerate, drive_brake, drive_handbrake.
    class DriveMode : public RefCounted
    {
        GDCLASS(DriveMode, RefCounted);

    public:
        static void _bind_methods();
        DriveMode();

        // ac_node = the AC root; world = the WorldScenery module (ground
        // heights), may be null (tests: the car then keeps its height). The
        // labels are the HUD readouts the drive overwrites (null = skipped).
        void register_nodes(Node3D *ac_node, Object *world,
                            Label *speed_label, Label *altitude_label, Label *vspeed_label,
                            Label *throttle_label, Label *heading_label, Label *heading_deg_label);
        // Vehicle parameters from the aircraft.cfg [vehicle] section
        // (AircraftRig::get_vehicle_params); missing keys keep the defaults.
        void configure(const Dictionary &params);
        // Fly! with a vehicle: engage at the first update() whose rebase
        // cooldown is 0 (the spawn has placed the node for good).
        void arm(Object *p_jsb);
        void disengage(Object *p_jsb);
        // Per frame. Returns true while driving (the caller then skips the
        // flight controls and the normal HUD, as for slew mode). The joystick
        // values are the calibrated JoystickConfig axes drive_steer [-1, 1],
        // drive_accelerate / drive_brake [0, 1] and the drive_handbrake button;
        // they combine with the keyboard (the larger input wins).
        bool update(double delta, Object *p_jsb, int rebase_cooldown,
                    double joy_steer = 0.0, double joy_accelerate = 0.0, double joy_brake = 0.0, bool joy_handbrake = false);
        bool is_active() const { return active; }
        bool is_armed() const { return armed; }

        // One kinematic step with explicit inputs in [0, 1] / [-1, 1] (also
        // the test hook). Moves the state, not the node.
        void step(double delta, double throttle, double brake, double steer, bool handbrake);
        // The node from the state (position, heading, ground heights).
        void apply_pose();
        // Start the state from the node's pose (position, heading; speed 0).
        void capture_pose();

        double get_speed_kmh() const { return speed * 3.6; }
        double get_heading_deg() const { return heading_deg; }
        double get_steer_deg() const { return steer_deg; }
        Vector2 get_lv95() const { return Vector2(e, n); }
        void set_lv95(double p_e, double p_n) { e = p_e; n = p_n; }
        void set_heading_deg(double h) { heading_deg = h; }

    protected:
        // ── parameters (defaults: a small car) ──
        double wheelbase_m = 2.6;
        double track_m = 1.5;
        double max_speed = 140.0 / 3.6;     // m/s forward
        double max_reverse = 15.0 / 3.6;    // m/s
        double accel = 3.5;                 // m/s^2 full throttle
        double brake_decel = 8.0;           // m/s^2 full brake
        double max_steer_deg = 32.0;        // lock at standstill
        double steer_rate_deg_s = 120.0;    // toward the target
        double clearance_m = 0.0;           // node origin above the ground
        double rolling = 0.4;               // m/s^2
        double aero = 0.0045;               // m/s^2 per (m/s)^2 (set from accel / max_speed in configure)
        double lat_accel_max = 6.0;         // m/s^2: the steering lock at speed

        // ── state ──
        bool armed = false;
        bool active = false;
        double e = 0.0, n = 0.0;            // LV95
        double heading_deg = 0.0;           // true
        double grid_offset_deg = 0.0;       // grid = true + offset (LV95 convergence)
        double h = 0.0;                     // last height ASL of the node origin
        double speed = 0.0;                 // m/s, + forward
        double steer_deg = 0.0;             // + right
        double pitch = 0.0, roll = 0.0;     // rad, smoothed

        Node3D *ac_node = nullptr;
        WorldScenery *world = nullptr;
        Label *speed_label = nullptr;
        Label *altitude_label = nullptr;
        Label *vspeed_label = nullptr;
        Label *throttle_label = nullptr;
        Label *heading_label = nullptr;
        Label *heading_deg_label = nullptr;

        double ground_at(double pe, double pn) const;
        void hud();
    };

} // namespace jsbgodot

#endif
