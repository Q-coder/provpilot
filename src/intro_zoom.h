#ifndef INTRO_ZOOM_H
#define INTRO_ZOOM_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace jsbgodot
{

    class CameraRig;

    // Cinematic space→spot intro zoom over the world3d globe, moved out of
    // node_3d.gd so the per-frame animation math runs in C++ (double
    // precision).
    //
    // When fly-loading completes, a dedicated root-level camera starts high
    // above the Earth (whole globe visible) and smoothly descends to the
    // normal orbit "spot" view of the aircraft, then hands off to the orbit
    // camera through CameraRig. Distance is log-interpolated (many orders of
    // magnitude) and the view direction slerps from straight-down to the spot
    // angle; Escape/Space skip the cinematic.
    //
    // GDScript (node_3d.gd) keeps the scene-side gates (globe mounted, AC +
    // intro camera valid), registers the session-stable nodes once, and calls
    // update() from _process while the zoom plays. The handoff to the orbit
    // camera is done here through CameraRig; GDScript refreshes its own
    // camera_mode/active_camera mirrors when update() reports STATUS_FINISHED.
    class IntroZoom : public RefCounted
    {
        GDCLASS(IntroZoom, RefCounted);

    public:
        // Return values of update().
        enum Status
        {
            STATUS_RUNNING = 0,   // zoom still in progress
            STATUS_FINISHED = 1,  // handoff to the orbit camera done this call
        };

        static void _bind_methods();
        Vector3 view_up_hint(const Vector3 &cam_pos, const Vector3 &target, double blend) const;

        // ── Node registration (session-stable; no reset needed) ──
        // ac_node = aircraft root ($AC), intro_camera = dedicated root-level
        // cinematic camera, camera1/camera2 = the scene cockpit/external
        // cameras (current flags toggled on start/end), camera_rig = the
        // CameraRig module used for the spot offset + orbit handoff.
        void register_nodes(Node3D *ac_node, Camera3D *intro_camera,
                            Camera3D *camera1, Camera3D *camera2,
                            CameraRig *camera_rig);

        // ── Lifecycle ──
        // Begin the zoom (called when fly-loading completes). Returns false
        // when ac_node or intro_camera is missing (nothing started).
        bool start();

        // Per-frame update while playing. Space (after release) skips;
        // Input singleton for the skip. Returns STATUS_RUNNING or
        // STATUS_FINISHED (the handoff happened on this call).
        int update(double delta);
        bool is_playing() const;
        // Abort a running zoom (teleport / new flight requested mid-animation):
        // same handoff as a finished zoom, camera2 on the spot view.
        void cancel();
        // While set, a zoom that reaches its end keeps the camera on the spot
        // view and stays playing (STATUS_RUNNING) instead of handing over;
        // Space then only jumps to the end. Used while the destination is
        // still loading behind the zoom. Clearing it lets the next update()
        // finish a zoom that is already at its end.
        void set_hold_at_end(bool hold);
        bool get_hold_at_end() const;
        // True while the zoom sits at its end waiting for the hold to clear.
        bool is_holding() const;

        // ── Tunables (exposed as properties so they can be set from
        // GDScript at runtime — e.g. _intro_zoom.intro_duration = 90.0 —
        // without a C++ rebuild) ──
        void set_intro_duration(double seconds);
        double get_intro_duration() const;
        void set_intro_start_alt_m(double metres);
        double get_intro_start_alt_m() const;

    protected:
        IntroZoom();

        // Tunables (defaults = historical GDScript constants)
        double intro_duration = 60.0;      // seconds
        double intro_start_alt_m = 1.6e7; // metres above the aircraft (space view)

        bool playing = false;
        bool skip_armed = false;  // Space skip enabled once Space was released after start
        bool hold_at_end = false;
        double elapsed = 0.0;
        Vector3 up_dir{0.0, 1.0, 0.0};    // start direction (straight up)
        Vector3 start_pos{0.0, 0.0, 0.0};

        Node3D *ac_node = nullptr;
        Camera3D *intro_camera = nullptr;
        Camera3D *camera1 = nullptr;
        Camera3D *camera2 = nullptr;
        CameraRig *camera_rig = nullptr;

        // World-aligned orbit offset from the rig (fallback: 6 m behind the
        // aircraft) — used for the spot end position.
        Vector3 spot_offset() const;
        void finish();
    };

} // namespace jsbgodot

#endif
