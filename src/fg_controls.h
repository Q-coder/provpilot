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
        // Property path normalised like the animator: no leading slash, no [0].
        static String norm(const String &p);

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
        // cfg [model] knob_actions ("property[:step[:min:max]]", ...): rotate /
        // knob animations driving these properties that have no binding of
        // their own (the MD-80's flood knobs) get a property-adjust action,
        // so they turn with the mouse like the other knobs. Set before setup().
        void set_knob_actions(const Array &specs);
        Array get_knob_actions() const;
        // Nasal shim table (JSON) for the aircraft's cockpit bindings; count loaded.
        int load_shims(const String &path);
        int get_shim_count() const;
        // Per frame: expire the pulses the shims started.
        void tick(double delta);
        // Every property path the controls' bindings act on (toggle, adjust, assign ...).
        PackedStringArray binding_properties() const;
        // Current value of a property: set by a control before, else the animator's.
        double value_of(const String &prop) const;
        // The clickable meshes as {node, aabb, recs} (tests / probes).
        Array get_items() const;
        int debug_box_count() const;

        Dictionary get_state() const;
        void set_state(const Dictionary &s);
        Callable get_on_change() const;
        void set_on_change(const Callable &c);
        // Shim action "autoflight": {"action": "autoflight", "kind": "hdgAdjust", "arg_index": 0}
        // calls on_module_action(kind, argument) (the rig forwards it to the autoflight module).
        Callable get_on_module_action() const;
        void set_on_module_action(const Callable &c);
        // Set one property of the cockpit state from outside (the autoflight's
        // writes to the FGCP's own switches), reporting the change like a click.
        void assign_external(const String &prop, const Variant &v);
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
        Callable on_module_action;
        Vector<Item> items;
        FGAnimator *animator = nullptr;
        Dictionary pressed; // button -> [{rec, action}] whose release / mod-up bindings are pending
        bool click_shift = false; // Shift held for the click being handled (devices/status/keyboard/shift)
        // Nasal shims (docs/MD80_FLIGHTGEAR_PORT.md D5): aircraft/<id>/.../nasal_shims.json
        // maps a Nasal call head ("core.Sound.switch1(") to a property action
        // (pulse / assign / set / adjust / toggle / swap / cycle / action).
        Array shims;
        Array knob_actions;
        Array pulses;       // [{prop, t}] properties set back to 0 after t seconds
        Dictionary cycle_values; // norm(prop) -> every value the cockpit's property-cycle lists hold for it
        void collect_cycle_values(const Variant &v);
        bool run_shim(const String &statement, const Dictionary &ctx);
        // Does any binding of this record do something here (a property command,
        // or a Nasal call a shim / the built-in map understands)? Records whose
        // bindings are all unknown Nasal (the ASK 21's canopy glass) take no
        // clicks and draw no hotspot, so they do not shadow the real controls.
        bool record_actionable(const Dictionary &rec) const;
        bool nasal_actionable(const String &script) const;
        static Array shim_args(const String &statement);
        static String shim_prop(const String &tmpl, const Array &args);
        int count = 0;
        bool debug = false;
        Vector<uint64_t> debug_boxes;

        static MeshInstance3D *item_node(const Item &it);
        int hit_index(Camera3D *camera, const Vector2 &screen_pos);
        bool nearest_face_hit(Item &it, const Vector3 &lf, const Vector3 &ld, Vector3 &out) const;
        void press_item(const Array &recs, int button, bool shift, Array &ran);
        bool press_knob(const Dictionary &rec, int button, bool shift);
        void release(const Array &entries);
        static int action_direction(const Dictionary &action);
        static Array as_list(const Variant &v);
        static Array bindings(const Variant &container);

        void assign(const String &prop, const Variant &v);
        void run(const Array &bindings, const Dictionary &rec, const Dictionary &ctx);
        bool cond_ok(const Variant &condition) const;
        void nasal(const String &script, const Dictionary &ctx);
        bool nasal_if(const String &script, const Dictionary &ctx);
        void nasal_action(const String &kind, const Array &args, const Dictionary &ctx);
        static double num(const Variant &v, double def = 0.0);
    };

} // namespace jsbgodot

#endif
