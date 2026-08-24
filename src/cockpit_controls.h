#ifndef COCKPIT_CONTROLS_H
#define COCKPIT_CONTROLS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <vector>

using namespace godot;

namespace jsbgodot
{

    class JSBGodot;
    class ControlSurfaces;

    // Per-frame behaviour of the interactive cockpit levers/switches (mixture,
    // throttle, magneto, flaps, landing gear), moved out of node_3d.gd so the
    // input handling + lever transform math runs in C++ (double precision).
    //
    // GDScript (node_3d.gd) resolves the lever nodes from aircraft.cfg
    // [levers], registers them here (base transforms are captured at
    // registration time), pushes the cfg flap detents / gear flag, and calls
    // update() once per frame with the JSBGodot node and the ControlSurfaces
    // module (for gear-command sync). Keyboard input is read directly through
    // Godot's Input singleton, exactly as the historical GDScript did:
    //   M / Shift+M        mixture richer / leaner
    //   T / Shift+T        throttle up / down (value lives in JSBSim)
    //   I / Shift+I        magneto step; I past BOTH = momentary START that
    //                      springs back to BOTH on release (unless dragging)
    //   F / Shift+F        flaps down / up
    //   G                  toggle retractable gear (no-op when fixed gear)
    //   Shift+Ctrl+M/T     toggle the collision-box debug mesh
    // Mouse dragging stays in GDScript (_unhandled_input raycasts) and drives
    // this module through set_mixture_value() / set_magneto_position() /
    // set_flap_position() plus the set_dragging() guards.
    //
    // Kept as a plain RefCounted so GDScript can instantiate it with
    // `CockpitControls.new()` without node-tree wiring. Registered nodes are
    // raw pointers into the loaded GLB subtree, which the model loader frees
    // synchronously on an airframe swap — call reset() around the swap (from
    // _resolve_aircraft_nodes) so no update() touches a freed node.
    class CockpitControls : public RefCounted
    {
        GDCLASS(CockpitControls, RefCounted);

    public:
        // Which lever a set_dragging() guard applies to.
        enum Lever
        {
            LEVER_MIXTURE = 0,
            LEVER_THROTTLE = 1,
            LEVER_MAGNETO = 2,
            LEVER_FLAP = 3,
        };

        static void _bind_methods();

        // ── Tunables (defaults match the historical GDScript constants) ──
        void set_mixture_key_step(double step);
        void set_throttle_key_step(double step);
        void set_mixture_lever_travel(double travel);
        void set_throttle_lever_travel(double travel);
        void set_flap_lever_travel(double travel);
        void set_magneto_step_angle(double deg);
        void set_magneto_default_position(int position);
        void set_gear_retractable(bool retractable);
        // JSBSim flap-cmd-norm values per lever notch (cfg flaps.positions).
        void set_flap_positions(const Array &positions);
        // HUD labels per lever notch (cfg flaps.names).
        void set_flap_names(const PackedStringArray &names);

        // ── Node registration (base transforms are captured here) ──
        // Nulls every registered pointer and resets the gear command to fully
        // down. Call before/after a model swap so no update() touches a freed
        // node; re-register afterwards from _resolve_aircraft_nodes.
        void reset();
        // Any argument may be null (missing lever in the model); only found
        // nodes have their base transform captured, as before.
        void register_levers(Node3D *mixture, Node3D *throttle, Node3D *magneto, Node3D *flap);
        // Runtime physics bodies the Shift+Ctrl+M / Shift+Ctrl+T debug meshes
        // attach to (may be null).
        void register_debug_bodies(Node *mixture_body, Node *throttle_body);

        // ── Per-frame update (keyboard input + visual sync) ──
        // jsb may be null (controls still move visually); surfaces may be
        // null (the gear key then only updates the commanded state).
        void update(Object *jsb, ControlSurfaces *surfaces);

        // ── State access for the GDScript mouse-drag code ──
        double get_mixture_value() const;
        int get_magneto_position() const;   // 0=OFF, 1=R, 2=L, 3=BOTH, 4=START
        int get_flap_position() const;      // notch index into flap_positions
        bool is_gear_down() const;
        void set_dragging(int lever, bool dragging);

        // Value setters used by the mouse drag: assign + apply visual + JSBSim.
        void set_mixture_value(double value, Object *jsb);
        void set_magneto_position(int position, Object *jsb);
        void set_flap_position(int position, Object *jsb);
        // Throttle has no C++ state (its value lives in JSBSim); this only
        // drives the lever visual during a drag.
        void apply_throttle_visual(double throttle);

        // Initial syncs after node resolution (historical behaviour of
        // _resolve_aircraft_nodes / _setup_mixture_lever_collision).
        void apply_mixture_value(Object *jsb);
        void apply_magneto_position(Object *jsb);
        // Map the FDM's actual kinematic flap position onto a lever notch and
        // apply it (historical startup sync, uses get_flaps()).
        void sync_flaps_from_jsb(Object *jsb);

    protected:
        CockpitControls();
        ~CockpitControls();

        // Tunables (defaults = historical GDScript constants)
        double mixture_key_step = 0.05;
        double throttle_key_step = 0.05;
        double mixture_lever_travel = 0.05;
        double throttle_lever_travel = 0.05;
        double flap_lever_travel = -0.01;
        double magneto_step_angle = 30.0;
        int magneto_default_position = 3; // Blender rest pose = BOTH
        bool gear_retractable = false;
        std::vector<double> flap_positions = {0.0, 0.333, 0.667, 1.0};
        PackedStringArray flap_names = {"UP", "10°", "20°", "30°"};

        // Lever state (defaults = historical initial values)
        double mixture_value = 1.0; // 0.0 = cutoff, 1.0 = full rich
        int magneto_position = 3;
        int flap_position = 0;
        bool gear_down = true;

        // Key debounce (single-fire per press) + print dedup
        bool mixture_key_held = false;
        bool throttle_key_held = false;
        bool magneto_key_held = false;
        bool flap_key_held = false;
        bool gear_key_held = false;
        int magneto_last_printed_pos = -1;
        int flap_last_printed_pos = -1;

        // Mouse-drag guards (set from GDScript _unhandled_input)
        bool dragging[4] = {false, false, false, false};

        // Debug collision-box visibility (Shift+Ctrl+M / Shift+Ctrl+T)
        bool mixture_debug_visible = false;
        bool throttle_debug_visible = false;

        // Registered nodes + captured base transforms
        Node3D *mixture_lever_node = nullptr;
        Transform3D mixture_lever_base;
        Node3D *throttle_lever_node = nullptr;
        Transform3D throttle_lever_base;
        Node3D *magneto_switch_node = nullptr;
        Transform3D magneto_switch_base;
        Node3D *flap_lever_node = nullptr;
        Transform3D flap_lever_base;
        Node *mixture_debug_body = nullptr;
        Node *throttle_debug_body = nullptr;

        // ── Keyboard handlers (historical _handle_*_keys) ──
        void handle_mixture_keys(JSBGodot *fdm);
        void handle_throttle_keys(JSBGodot *fdm);
        void handle_magneto_keys(JSBGodot *fdm);
        void handle_flap_keys(JSBGodot *fdm);
        void handle_gear_key(ControlSurfaces *surfaces, JSBGodot *fdm);

        // ── Visual apply (historical _apply_* / _update_lever_visuals) ──
        void sync_visuals(JSBGodot *fdm);
        void apply_mixture(JSBGodot *fdm);
        void apply_magneto(JSBGodot *fdm);
        void apply_flap(JSBGodot *fdm);
        void toggle_mixture_debug();
        void toggle_throttle_debug();

        // JSBSim flap-pos-norm (0.0-1.0) -> lever notch index (0-3).
        static int flaps_norm_to_position(double norm);
        String flap_name(int position) const;
    };

} // namespace jsbgodot

#endif

