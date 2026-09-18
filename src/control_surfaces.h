#ifndef CONTROL_SURFACES_H
#define CONTROL_SURFACES_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <vector>

using namespace godot;

namespace jsbgodot
{

    class JSBGodot;

    // Per-frame animation of the aircraft's movable parts: control surfaces,
    // propeller (spin + blur disc + spinner cone) and retractable landing gear.
    //
    // GDScript (node_3d.gd) pushes the per-aircraft tunables from aircraft.cfg,
    // registers the resolved model nodes (their base transforms are captured at
    // registration time), and calls update() once per frame with the JSBGodot
    // node and the frame delta. Kept as a plain RefCounted so GDScript can
    // instantiate it with `ControlSurfaces.new()` without node-tree wiring.
    //
    // Node lifetime: registered nodes are raw pointers into the loaded GLB
    // subtree. The model loader frees that subtree synchronously when swapping
    // airframes, so the caller must invoke reset() (nulls every pointer and
    // clears legs/flaps) around a model swap and re-register before the next
    // update() runs. Propeller animation state (visual RPM / rotation angle)
    // deliberately persists across swaps, matching the historical behaviour.
    class ControlSurfaces : public RefCounted
    {
        GDCLASS(ControlSurfaces, RefCounted);

    public:
        // One entry of the generic multi-flap list (cfg [animation] flaps).
        struct FlapNode
        {
            Node3D *node = nullptr;
            Transform3D base;
            double sign = 1.0;
        };

        // One retractable-gear leg (cfg [gear]). Wheel + strut rotate rigidly
        // about the shared pivot so the leg moves as a unit; optional nose-wheel
        // steering yaws wheel + strut about steer_pivot, composed UNDER the
        // retract rotation so the steered leg still folds correctly.
        struct GearLeg
        {
            Node3D *wheel = nullptr;
            Node3D *strut = nullptr;
            Transform3D wheel_base;
            Transform3D strut_base;
            Vector3 pivot = Vector3(0, 0, 0);
            Vector3 axis = Vector3(1, 0, 0);
            double retract_deg = 90.0;
            bool steer = false;
            Vector3 steer_axis = Vector3(0, 0, -1);
            double steer_max_deg = 30.0;
            Vector3 steer_pivot = Vector3(0, 0, 0);
            bool roll = false;
            Vector3 roll_axis = Vector3(0, 1, 0);
            double radius = 0.3;
            double spin = 0.0;
        };

        static void _bind_methods();

        // ── Per-aircraft tunables (defaults match the historical values) ──
        void set_rudder_max_angle(double deg);
        void set_rudder_axis(const Vector3 &axis);
        void set_aileron_max_angle(double deg);
        void set_aileron_axis(const Vector3 &axis);
        void set_elevator_max_angle(double deg);
        void set_elevator_axis(const Vector3 &axis);
        void set_flap_max_angle(double deg);
        void set_flap_axis(const Vector3 &axis);
        void set_front_wheel_max_angle(double deg);
        void set_propeller_blur_start_rpm(double rpm);
        void set_propeller_hide_blades_rpm(double rpm);
        void set_propeller_spin_axis(const Vector3 &axis);
        void set_gear_retract_seconds(double seconds);

        // ── Node registration (base transforms are captured here) ──
        // Nulls every registered node, clears the generic flap list and the
        // gear legs (resetting the gear to fully down). Call before/after a
        // model swap so no update() ever touches a freed node.
        void reset();

        void register_rudder(Node3D *node);
        void register_front_wheel(Node3D *node);
        void register_propeller(Node3D *node);
        // Optional spinner cone shown in place of the blades at high RPM;
        // starts hidden, like the historical code.
        void register_propeller_cone(Node3D *node);
        // Blur disc mesh + its material (shader "rpm" parameter is driven per
        // frame when the material is a ShaderMaterial).
        void register_propeller_blur_disc(MeshInstance3D *disc, Material *material);
        void register_aileron_left(Node3D *node);
        void register_aileron_right(Node3D *node);
        // L/R elevator pair rotates about the X euler angle (not a local axis).
        void register_elevator_left(Node3D *node);
        void register_elevator_right(Node3D *node);
        // Single-piece elevator / stabilator rotates about the hinge axis.
        void register_single_elevator(Node3D *node);
        // L/R flap pair rotates about the X euler angle (not a local axis).
        void register_flap_left(Node3D *node);
        void register_flap_right(Node3D *node);
        // Generic split-flap list entry; sign flips the sweep direction.
        void add_flap_node(Node3D *node, double sign);

        // Adds a gear leg. wheel/strut may be null (the missing part is
        // skipped); base transforms are captured from the current pose.
        void add_gear_leg(Node3D *wheel, Node3D *strut, const Vector3 &pivot,
                const Vector3 &axis, double retract_deg, bool steer,
                const Vector3 &steer_axis, double steer_max_deg,
                const Vector3 &steer_pivot, bool roll, const Vector3 &roll_axis,
                double radius_m);

        // Imported retract/extend clip mode: the player is driven by seeking
        // (0 = gear down, length = fully retracted) instead of rotating legs.
        void set_gear_anim(AnimationPlayer *player, double length);

        // ── Landing gear command (mirrors JSBSim fcs/gear-cmd-norm) ──
        void set_gear_down(bool down);
        // Fresh session: command + visual snap to the given state (no retract
        // animation), e.g. Fly! after a flight with the gear up.
        void reset_gear(bool down);
        double get_gear_visual_pos() const; // 0 = up, 1 = down (animated)

        // ── Per-frame animation. `jsb` is the JSBGodot node (may be null). ──
        void update(Object *jsb, double delta);

        // Smoothed RPM used for visuals (decays to 0 when the engine is off).
        double get_visual_rpm() const;

    protected:
        ControlSurfaces();
        ~ControlSurfaces();

        // Tunables (defaults = historical GDScript values)
        double rudder_max_angle = 16.0;
        Vector3 rudder_axis = Vector3(0, 1, 0);
        double aileron_max_angle = 20.0;
        Vector3 aileron_axis = Vector3(1, 0, 0);
        double elevator_max_angle = 25.0;
        Vector3 elevator_axis = Vector3(1, 0, 0);
        double flap_max_angle = 30.0;
        Vector3 flap_axis = Vector3(1, 0, 0);
        double front_wheel_max_angle = 30.0;
        double propeller_blur_start_rpm = 900.0;
        double propeller_hide_blades_rpm = 1200.0;
        Vector3 propeller_spin_axis = Vector3(0, 0, -1);
        double gear_retract_seconds = 4.0;

        // Registered nodes + captured base transforms
        Node3D *rudder_node = nullptr;
        Transform3D rudder_base;
        Node3D *front_wheel_node = nullptr;
        Transform3D front_wheel_base;
        Node3D *propeller_node = nullptr;
        Transform3D propeller_base;
        Node3D *propeller_cone_node = nullptr;
        MeshInstance3D *propeller_blur_disc = nullptr;
        Material *propeller_blur_material = nullptr;
        Node3D *left_aileron_node = nullptr;
        Transform3D left_aileron_base;
        Node3D *right_aileron_node = nullptr;
        Transform3D right_aileron_base;
        Node3D *left_elevator_node = nullptr;
        Transform3D left_elevator_base;
        Node3D *right_elevator_node = nullptr;
        Transform3D right_elevator_base;
        Node3D *single_elevator_node = nullptr;
        Transform3D single_elevator_base;
        Node3D *left_flap_node = nullptr;
        Transform3D left_flap_base;
        Node3D *right_flap_node = nullptr;
        Transform3D right_flap_base;
        std::vector<FlapNode> flap_nodes;

        // Propeller animation state (persists across airframe swaps)
        double visual_rpm = 0.0;
        double propeller_rotation = 0.0;
        static constexpr double PROPELLER_DECAY_RATE = 1.5;

        // Gear state
        bool gear_down = true;
        double gear_visual_pos = 1.0; // Animated position: 0 = up, 1 = down
        std::vector<GearLeg> gear_legs;
        AnimationPlayer *gear_anim_player = nullptr;
        double gear_anim_length = 0.0;

        // Rigidly rotate a base transform about `pivot` by `steer` when
        // `enabled`, returning the steered Transform3D (wrapper frame).
        static Transform3D apply_steer(const Transform3D &base, const Basis &steer,
                const Vector3 &pivot, bool enabled);

        // Linear ground distance (metres) the wheels rolled this frame; zero
        // when airborne (weight-off-wheels).
        double wheel_roll_delta(const JSBGodot *fdm, double delta) const;

        // Gear retraction/extension animation (manual leg rotation or the
        // imported clip mode); also rolls steered wheels while on the ground.
        void update_gear(const JSBGodot *fdm, double delta);
    };

} // namespace jsbgodot

#endif
