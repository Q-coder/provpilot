#ifndef FG_ANIMATOR_H
#define FG_ANIMATOR_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

namespace jsbgodot
{
    // FlightGear model-animation runtime (docs/A10_FLIGHTGEAR_PORT.md, D1).
    //
    // Loads the animations.json written by scripts/fg_a10_build.py (every
    // <animation> of the FlightGear model XML tree, raw) and applies them to
    // the glb nodes every frame: rotate, translate, spin, select, material,
    // textranslate. Animations read FlightGear property paths; the host
    // feeds values with set_property()/set_properties() (the property
    // bridge, D2). Unknown properties read 0 / "" and are listed by
    // unknown_properties() so bridge coverage is measurable.
    //
    // Frames: manifest values are in the FlightGear model frame (x aft,
    // y right, z up); the glb is glTF (x aft, y up, z left), so
    // godot = (x, z, -y). Transforms compose as FlightGear nests them: the
    // first animation of a file is outermost, node.transform = A1·A2·…·base.
    class FGAnimator : public Node
    {
        GDCLASS(FGAnimator, Node);

    public:
        static void _bind_methods();

        // Parse the manifest and resolve its targets under model_root. Safe to
        // call again (drops the previous state). Returns true when loaded.
        bool load(const String &manifest_path, Node *model_root);
        void clear();

        // ── property bridge ──
        void set_property(const String &path, const Variant &value);
        void set_properties(const Dictionary &values);
        Variant get_property(const String &path) const;
        // every property the manifest references (normalised paths)
        PackedStringArray property_names() const;
        // texture-prop files (liveries) are resolved below this folder; set before load()
        void set_texture_root(const String &dir);
        String get_texture_root() const;
        // referenced but never set since load()
        PackedStringArray unknown_properties() const;
        // The manifest's property aliases (path -> target), for the bridge.
        Dictionary get_aliases() const;

        // ── per frame ──
        void update(double delta);

        int animation_count() const { return anims.size(); }
        int node_count() const { return node_states.size(); }
        Dictionary stats() const;
        // the animated transform currently applied to a node (identity when
        // the node is not animated); for tests
        Transform3D animated_transform(const String &node_name) const;

        FGAnimator();

    protected:

    private:
        enum CondKind { C_TRUE, C_PROP, C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE, C_AND, C_OR, C_NOT };
        struct Cond
        {
            CondKind kind = C_TRUE;
            int prop = -1;        // left operand property
            int prop2 = -1;       // right operand property (comparison of two properties)
            double value = 0.0;   // right operand constant
            String svalue;        // right operand string constant
            bool is_string = false;
            Vector<int> children; // indices into conds
        };
        enum AnimType { A_ROTATE, A_TRANSLATE, A_SPIN, A_SELECT, A_MATERIAL, A_TEXTRANSLATE, A_TEXROTATE };
        struct Anim
        {
            AnimType type;
            int prop = -1;
            double factor = 1.0, offset = 0.0;
            bool has_min = false, has_max = false;
            double min_v = 0.0, max_v = 0.0;
            Vector3 center, axis;              // Godot frame
            Vector<double> table_in, table_out; // interpolation
            int cond = -1;                     // condition index (-1 = none)
            // spin
            double angle = 0.0;
            // textranslate
            double step = 0.0, scroll = 0.0, bias = 0.0;
            Vector2 tex_axis;
            Vector2 tex_center = Vector2(0.5, 0.5); // texrotate: pivot in texture coordinates
            // material
            bool has_emission = false, emission_is_prop = false;
            Color emission;
            int emission_factor_prop = -1;
            bool has_diffuse = false;
            Color diffuse;
            int diffuse_factor_prop = -1;      // <diffuse><factor-prop>: scales the digits' colour (Label3D)
            int alpha_prop = -1;
            bool has_alpha = false;
            double alpha = 1.0;
            bool transparent = false;          // FlightGear model-transparent effect: alpha-blend the texture
            // <texture-prop>: the property (property-base/texture-prop) names a
            // texture file (liveries, engine-variant gauge faces) relative to
            // the record's own model XML, as FlightGear resolves it
            int tex_prop = -1;
            String tex_dir;     // that XML's directory below Models/ ("" = Models/)
            String tex_default; // <texture>: the file the glb was built with
            String tex_last;    // value applied last (so a file is loaded once)
            Vector<int> targets;               // indices into node_states / mat_states
            Vector<uint64_t> labels;           // material: Label3D ids under the targets (modulate)
            String model;
        };
        struct NodeState
        {
            Node3D *node = nullptr;
            uint64_t node_id = 0;   // ObjectID: the model may be freed (aircraft swap) before clear()
            Transform3D base;
            // static (file-pose) transform of the parent relative to the model
            // root: FlightGear centres/axes are in model-file coordinates, so
            // the animation chain is conjugated by it (node = P⁻¹·A·P·base)
            Transform3D parent_file;
            Transform3D parent_file_inv;
            Vector<int> anims;   // transform animations in file order
            Vector<int> selects; // select animations
            Transform3D applied;
        };
        // FlightGear <text>: a Label3D hung on the text's empty, re-rendered
        // from a property (number-value: printf format; text-value: the string)
        // or fixed (literal)
        struct TextState
        {
            uint64_t label_id = 0;
            int prop = -1;
            int kind = 0;       // 0 literal, 1 number-value, 2 text-value
            String format;
            String last;
        };
        struct MatState
        {
            MeshInstance3D *mesh = nullptr;
            Vector<Ref<Material>> mats; // per-surface unique copies
            Vector3 uv_offset;
            Vector<Ref<Material>> rot_mats; // texrotate: ShaderMaterial per surface (uv rotation)
            Vector<Ref<Texture2D>> base_albedo; // texture-prop: the glb's own albedo per surface (restored for the default)
        };
        Ref<Shader> texrotate_shader;
        void ensure_texrotate_materials(MatState &m);

        Vector<Cond> conds;
        Vector<Anim> anims;
        Vector<NodeState> node_states;
        Vector<MatState> mat_states;
        Vector<TextState> text_states;
        HashMap<String, Ref<Font>> fonts; // font file name -> loaded font (manifest "fonts")
        HashMap<String, int> node_index;
        HashMap<String, int> mat_index;
        HashMap<String, int> prop_index;
        Vector<String> prop_names;
        Dictionary aliases;
        Vector<double> prop_values;
        Vector<String> prop_strings;
        Vector<bool> prop_set;
        Dictionary type_counts;
        int skipped = 0;
        Node *model_root = nullptr;
        // the manifest's root node ("A10_Root"): its local frame is the model
        // file frame the FlightGear centres/axes refer to (the loader's fitting
        // transform sits on this node and must not enter the conjugation)
        Node *file_root = nullptr;
        // sub-model tag -> its root empty ("Sub_<tag>"): animations from a
        // sub-model XML are in that file's own frame
        HashMap<String, Node *> model_roots;
        // sub-model tag -> directory of its XML below Models/ (texture-prop files)
        HashMap<String, String> model_dirs;
        // texture-prop files are read below this folder (aircraft.cfg [model]
        // textures_dir, the liveries tree scripts/fg_liveries.py writes)
        String texture_root;
        HashMap<String, Ref<Texture2D>> textures; // path -> loaded (null = missing, warned once)
        void apply_texture_prop(Anim &a);
        Ref<Texture2D> load_texture_file(const String &path);

        static String normalise(const String &path);
        int prop_id(const String &path);
        int parse_cond(const Variant &v);
        int parse_cond_children(const Dictionary &d, CondKind combine);
        bool eval(int cond) const;
        double eval_value(const Anim &a) const;
        static Vector3 fg_vec(const Dictionary &d, const char *x, const char *y, const char *z, const Vector3 &def);
        static double num(const Variant &v, double def);
        static bool is_number(const String &s);
        void collect_nodes(Node *n, HashMap<String, Vector<Node *>> &by_name);
        int node_state_for(Node3D *n, Node *frame_root);
        int mat_state_for(MeshInstance3D *m);
        void apply_material(Anim &a, MatState &m, bool on);
        void fixup_emissive_materials(Node *root);
        void load_texts(const Dictionary &m, const String &manifest_dir, HashMap<String, Vector<Node *>> &by_name);
        void update_texts();
        static String format_text(const String &fmt, double v, const String &s, int kind);
    };
}

#endif
