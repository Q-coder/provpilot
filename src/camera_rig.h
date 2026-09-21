#ifndef CAMERA_RIG_H
#define CAMERA_RIG_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace jsbgodot
{

    // Per-frame cockpit / orbit / chase camera behaviour, moved out of
    // node_3d.gd so the per-frame math runs in C++ (double precision).
    //
    // GDScript (node_3d.gd) registers the two scene cameras plus the AC root,
    // re-captures the cockpit base transform whenever aircraft.cfg [camera]
    // overrides are applied, and calls update() once per frame with the frame
    // delta and the active joystick index (from JoystickConfig; -1 = none).
    // Input is read directly through Godot's Input singleton: keyboard arrows /
    // Page Up-Down plus the hat switch (joy axes 6/7) and D-pad buttons, exactly
    // as the historical GDScript did. The "flip_camera" action press itself is
    // still detected in GDScript (it is a remappable project action) which then
    // calls flip().
    //
    // Kept as a plain RefCounted so GDScript can instantiate it with
    // `CameraRig.new()` without node-tree wiring. Registered nodes are raw
    // pointers into the scene tree; the cameras and AC root live for the whole
    // session, so no reset() is needed (unlike the swappable model subtree).
    class CameraRig : public RefCounted
    {
        GDCLASS(CameraRig, RefCounted);

    public:
        // Camera modes — values mirror node_3d.gd's CameraMode enum.
        enum Mode
        {
            MODE_COCKPIT = 0,
            MODE_ORBIT = 1,
            MODE_CHASE = 2,
        };

        static void _bind_methods();

        // ── Tunables (defaults match the historical GDScript constants) ──
        void set_head_yaw_max(double deg);
        void set_head_pitch_max(double deg);
        void set_head_look_speed(double deg_per_sec);
        void set_orbit_speed(double deg_per_sec);
        void set_orbit_zoom_speed(double units_per_sec);
        void set_orbit_distance_min(double distance);
        void set_orbit_distance_max(double distance);
        // Jump the orbit/chase camera to a distance (clamped next frame): per-aircraft default from aircraft.cfg [camera].
        void set_orbit_distance(double distance);
        void set_orbit_pitch_min(double deg);
        void set_orbit_pitch_max(double deg);
        void set_chase_smooth(double smooth);

        // ── Node registration ──
        // camera1 = cockpit camera, camera2 = external (orbit/chase) camera,
        // ac_node = aircraft root. Captures the cockpit base transform; call
        // capture_cockpit_base() again after repositioning the cockpit camera.
        void register_cameras(Camera3D *camera1, Camera3D *camera2, Node3D *ac_node);
        void capture_cockpit_base();

        // ── Per-frame update (mode dispatch; reads Input singleton) ──
        void update(double delta, int joy_device);

        // Cycle COCKPIT → ORBIT → CHASE → COCKPIT. Resets head look when
        // returning to the cockpit and toggles the cameras' current flags
        // (mirrors the historical flip_camera()).
        void flip();
        // Direct mode set — used by the world3d intro-zoom handoff, which must
        // jump straight to ORBIT without a head reset. Toggles current flags.
        void set_mode(int mode);
        // Mouse motion (InputEventMouseMotion.relative) from GDScript; used
        // while Space is held in the cockpit.
        void mouse_look(const Vector2 &relative);
        // Mouse wheel while Space is held: notches > 0 zoom in (narrower
        // field of view), < 0 zoom out.
        void mouse_zoom(int notches);
        bool is_mouse_look_active() const;
        // Head straight ahead, cockpit zoom back to the default.
        void recenter_head();
        int get_mode() const;

        // Suppress the cockpit branch while a dedicated camera (the intro zoom)
        // is active — mirrors the historical `active_camera != camera1` guard.
        void set_cockpit_enabled(bool enabled);

        // Current world-aligned orbit offset from the aircraft centre, i.e. the
        // spherical→cartesian conversion of the orbit state. Used by GDScript's
        // intro-zoom handoff so both paths share one implementation.
        Vector3 orbit_offset() const;
        // orbit_offset() rotated into the local West-Up-North frame at the aircraft.
        Vector3 orbit_offset_world() const;

        double get_orbit_yaw() const;     // degrees
        double get_orbit_pitch() const;   // degrees
        double get_orbit_distance() const;

    protected:
        CameraRig();
        ~CameraRig();

        // Tunables (defaults = historical GDScript constants)
        double head_yaw_max = 120.0;
        double head_pitch_max = 60.0;
        double head_look_speed = 90.0;
        double orbit_speed = 90.0;
        double orbit_zoom_speed = 10.0;
        double orbit_distance_min = 1.5;
        double orbit_distance_max = 50.0;
        double orbit_pitch_min = -10.0;
        double orbit_pitch_max = 80.0;
        double chase_smooth = 5.0;

        // Camera state (angles in degrees, distances in metres; defaults are
        // the historical initial values)
        int mode = MODE_COCKPIT;
        bool cockpit_enabled = true;
        double head_yaw = 0.0;
        double head_pitch = 0.0;
        double orbit_yaw = 135.0;   // Start behind-left
        double orbit_pitch = 15.0;  // Elevation
        double orbit_distance = 6.0;
        double chase_distance = 8.0;
        double chase_height = 3.0;
        double chase_yaw = 0.0;

        Camera3D *camera1 = nullptr;
        Camera3D *camera2 = nullptr;
        Node3D *ac_node = nullptr;
        Transform3D cockpit_base;
        // Cockpit zoom: the orbit/chase zoom inputs (L1/R1, Page Up/Down) change
        // the cockpit camera's field of view instead of a distance.
        double cockpit_zoom_speed = 40.0; // degrees of FOV per second
        double cockpit_fov_min = 15.0;
        double cockpit_fov_max = 100.0;
        double cockpit_fov_default = 65.0; // captured with the cockpit base
        // Space in the cockpit (MSFS style): held, the mouse looks around
        // (mouse captured meanwhile); a short tap without mouse movement
        // recentres the head and the zoom.
        double mouse_look_deg_per_px = 0.15;
        double mouse_zoom_deg_per_notch = 5.0;
        double tap_max_s = 0.3;
        double tap_max_px = 5.0;
        bool space_held = false;
        double space_time = 0.0;
        double space_motion_px = 0.0;
        Vector2 pending_mouse;

        void update_head_look(double delta, int joy_device);
        void update_orbit(double delta, int joy_device);
        void update_chase(double delta, int joy_device);

        // camera1 is current in cockpit mode, camera2 otherwise.
        void apply_current_flags();
    };

} // namespace jsbgodot

#endif
