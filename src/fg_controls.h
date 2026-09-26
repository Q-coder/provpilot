#ifndef FG_CONTROLS_H
#define FG_CONTROLS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

namespace jsbgodot
{

    class FGAnimator;

    // Clickable cockpit controls of a FlightGear model (docs/A10_FLIGHTGEAR_PORT.md,
    // subtask 5, D4; formerly fg_controls.gd): the manifest's `pick`, `knob`
    // and `slider` records.
    //
    // Picking: a ray from the cockpit camera through the mouse is tested
    // against the bounding box of every control mesh (no physics bodies for
    // ~100 small parts), refined to the nearest triangle; the nearest hit gets
    // the click. FlightGear input mapping:
    //   pick   - left button runs the action's bindings, release runs `mod-up`
    //   knob   - left click / wheel up = increase, right click / wheel down =
    //            decrease, Shift for the coarse variants; old-style knobs with
    //            an `action` run it on click and `release` on release
    //   slider - click runs the action (FlightGear drags it; a click toggles)
    // Bindings: property-toggle, property-assign, property-adjust (step / min
    // / max / wrap), property-cycle, property-multiply, and the handful of
    // Nasal scripts the A-10 uses, mapped by name.
    //
    // Values changed by the controls live in `state` (property -> value),
    // which FGBridge merges over its computed values every frame; the host
    // is told of every change through `on_change` (property, value).
    class FGControls : public RefCounted
    {
        GDCLASS(FGControls, RefCounted);

    public:
        static void _bind_methods();
        FGControls();

        // Reads the manifest's controls and resolves their meshes under
        // model_root; returns the number of clickable meshes.
        int setup(const String &manifest_path, Node *model_root, FGAnimator *animator);
        // Shift+Ctrl+M: translucent boxes on every clickable part's sense area.
        void set_debug(bool on);
        bool get_debug() const;
        // The control under the screen position ({node, aabb, recs}) or {}.
        Dictionary hit(Camera3D *camera, const Vector2 &screen_pos);
        String hover_name(Camera3D *camera, const Vector2 &screen_pos);
        // Mouse button on the cockpit; true when a control took it.
        bool click(Camera3D *camera, const Vector2 &screen_pos, int button, bool pressed, bool shift);
        // Current value of a property: set by a control before, else the animator's.
        double value_of(const String &prop) const;
        // The clickable meshes as {node, aabb, recs} (tests / probes).
        Array get_items() const;
        int debug_box_count() const;

        Dictionary get_state() const;
        void set_state(const Dictionary &s);
        Callable get_on_change() const;
        void set_on_change(const Callable &c);
        int get_count() const;

    protected:
        struct Item
        {
            uint64_t node_id = 0;
            AABB aabb;
            Array recs;
            PackedVector3Array faces;
            bool faces_loaded = false;
        };
        Dictionary state;
        Callable on_change;
        Vector<Item> items;
        FGAnimator *animator = nullptr;
        Dictionary pressed; // button -> record whose release bindings are pending
        int count = 0;
        bool debug = false;
        Vector<uint64_t> debug_boxes;

        static MeshInstance3D *item_node(const Item &it);
        int hit_index(Camera3D *camera, const Vector2 &screen_pos);
        bool nearest_face_hit(Item &it, const Vector3 &lf, const Vector3 &ld, Vector3 &out) const;
        bool press(const Dictionary &rec, int button, bool shift);
        void release(const Dictionary &rec, int button);
        static Array as_list(const Variant &v);
        static Array bindings(const Variant &container);
        static String norm(const String &p);
        void assign(const String &prop, const Variant &v);
        void run(const Array &bindings, const Dictionary &rec, const Dictionary &ctx);
        void nasal(const String &script, const Dictionary &ctx);
        void nasal_action(const String &kind, const Array &args, const Dictionary &ctx);
        static double num(const Variant &v, double def = 0.0);
    };

} // namespace jsbgodot

#endif
