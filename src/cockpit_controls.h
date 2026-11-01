#ifndef COCKPIT_CONTROLS_H
#define COCKPIT_CONTROLS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
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
    // Mouse dragging is handled here too: create_pick_bodies() hangs a
    // physics body under each lever, handle_mouse_button() / handle_mouse_motion()
    // raycast from the cockpit camera and drive the levers during a drag
    // (the set_dragging() guards keep sync_visuals() off a dragged lever).
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
        CockpitControls();
        ~CockpitControls();

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
        Array get_flap_positions() const;

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
        // Creates a StaticBody3D + CollisionShape3D under every registered
        // lever (each on its own physics layer) so a camera raycast can pick
        // it; the mixture/throttle bodies double as the debug bodies. The
        // bodies live inside the model subtree and are dropped by reset().
        void create_pick_bodies();
        // The registered lever node for a Lever value (null when missing).
        Node3D *get_lever_node(int lever) const;

        // ── Mouse interaction (historical node_3d.gd _unhandled_input) ──
        // A mouse button from the cockpit camera: a left press over a lever
        // body starts a drag, a release ends every drag (the magneto springs
        // back from START). Returns true when the event was consumed.
        bool handle_mouse_button(Camera3D *camera, const Vector2 &position, int button_index, bool pressed, Object *jsb);
        // Mouse motion while a drag is active: vertical drags move the
        // mixture/throttle levers and step the flap notches, horizontal
        // drags step the magneto. Returns true when the event was consumed.
        bool handle_mouse_motion(const Vector2 &position, Object *jsb);
        bool is_dragging() const;

        // ── Per-frame update (keyboard input + visual sync) ──
        // jsb may be null (controls still move visually); surfaces may be
        // null (the gear key then only updates the commanded state).
        void update(Object *jsb, ControlSurfaces *surfaces);

        // ── State access for the GDScript mouse-drag code ──
        double get_mixture_value() const;
        int get_magneto_position() const;   // 0=OFF, 1=R, 2=L, 3=BOTH, 4=START
        int get_flap_position() const;      // notch index into flap_positions
        bool is_gear_down() const;
        // Set the commanded gear state without touching JSBSim / the visual
        // (used by the Fly! session reset, which resets those itself).
        void set_gear_down(bool down);
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
        // \xC2\xB0 = U+00B0 (°) as UTF-8 bytes; String(const char*) parses
        // Latin-1, so build the non-ASCII names via String::utf8().
        PackedStringArray flap_names = {String("UP"), String::utf8("10\xC2\xB0"), String::utf8("20\xC2\xB0"), String::utf8("30\xC2\xB0")};

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

        // Pick bodies (created by create_pick_bodies, one per lever) + the
        // physics layer each one answers on.
        static constexpr int PICK_LAYER[4] = {16, 32, 64, 128};
        StaticBody3D *pick_body[4] = {nullptr, nullptr, nullptr, nullptr};
        // Mouse-drag anchors (screen position + value at the press).
        double drag_start_px = 0.0;
        double drag_start_value = 0.0;
        int drag_start_pos = 0;
        static constexpr double DRAG_PX_PER_UNIT = 200.0; // mixture / throttle full travel
        static constexpr double DRAG_PX_PER_STEP = 40.0;  // magneto / flap notch

        StaticBody3D *make_pick_body(Node3D *lever, int layer, const Ref<Shape3D> &shape);
        bool ray_hits_body(Camera3D *camera, const Vector2 &screen_pos, StaticBody3D *body, int layer) const;

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
        int flaps_norm_to_position(double norm) const; // nearest detent
        int flap_last_position() const { return int(flap_positions.size()) - 1; }
        String flap_name(int position) const;
    };

} // namespace jsbgodot

#endif

