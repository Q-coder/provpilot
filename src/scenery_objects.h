#ifndef SCENERY_OBJECTS_H
#define SCENERY_OBJECTS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace godot;

namespace jsbgodot
{

    // Placed models of the scenery library's object packs
    // (docs/SCENERY_ARCHITECTURE.md, "Object packs"): every enabled area with
    // an `objects/package.json` (the output of scripts/convert_msfs_scenery.py
    // or a hand-written file) contributes its objects, in library priority
    // order. A pack's exclusion zones hide the buildings and trees of
    // lower-priority packs (S3DBuildingManager / S3DVegetationManager
    // `set_exclusion_zones`) and the objects of lower-priority packs inside
    // them. Models are GLBs loaded on demand within `load_radius_m` of the
    // camera and freed beyond
    // `unload_radius_m`); the glTF parsing and scene generation run on a
    // worker thread (out of the tree), so a 50 MB model never stalls the
    // frame. Each is placed on the precise terrain (the
    // `elevation` Callable: lat, lon -> metres or NAN) plus its height above
    // ground, facing its true heading with the glTF +Z axis.
    //
    // The scenery editor's Object tool edits the pose of an object (move,
    // height, heading, scale, drop to the ground, remove); `save_packs()`
    // writes the edited packs' package.json back, which is the file a
    // scenery pack ships.
    class SceneryObjects : public Node3D
    {
        GDCLASS(SceneryObjects, Node3D);

    public:
        static void _bind_methods();
        SceneryObjects();
        ~SceneryObjects();

        // Enabled areas sorted by priority (SettingsStore::get_enabled_scenery_areas).
        // Reads every objects/package.json, pushes the exclusion zones to the
        // building and vegetation managers below `scenery3d` / `vegetation`.
        void apply_scenery_areas(const Array &areas, Node *scenery3d, Node *vegetation);
        void _process(double delta) override;
        void reload();

        int get_object_count() const;
        int get_loaded_count() const;
        int get_pending_count() const;
        int get_excluded_object_count() const;
        Array get_exclusion_zones() const;
        // {"name", "model", "lat", "lon", "e", "n", "agl_m", "heading_deg", "scale",
        //  "pack", "radius_m", "loaded", "excluded", "removed", "node"} of object i.
        Dictionary get_object_info(int i) const;
        // Editor selection tint: a translucent orange overlay drawn over the
        // model's visible surfaces (Blender-style highlight without a
        // post-process), kept across reloads of the model.
        void set_object_highlight(int i, bool on);
        void clear_highlights();
        // The instance node of the first loaded object with this name, or null.
        Node3D *find_object_node(const String &name) const;
        String get_object_label(int i) const;

        // ── editing ──
        // The loaded object whose geometry the ray hits first (triangle test on the
        // meshes after a bounding-box check); -1 if none.
        int pick_ray(const Vector3 &origin, const Vector3 &dir);
        // Records the current pose of object i for undo (once per drag). Edits
        // recorded after begin_edit_group() undo together.
        void begin_edit(int i);
        // Edits recorded between begin_edit_group() and end_edit_group() undo together;
        // an edit recorded outside a group is its own undo step.
        void begin_edit_group();
        void end_edit_group();
        // The pivot: the model-space point placed at the object's position. Changing it
        // moves the position so the model stays where it is. Saved as "pivot".
        Vector3 get_object_pivot(int i) const;
        void set_object_pivot(int i, const Vector3 &local, bool record = true);
        // Size of the placed model (bounds times scale) and its inverse.
        Vector3 get_object_dimensions(int i) const;
        void set_object_dimensions(int i, const Vector3 &dims, bool record = true);
        // New pose (LV95 e/n, height above ground, true heading / pitch / bank in
        // degrees, per-axis scale); the model is re-placed at once when it is
        // loaded. record = undo point first.
        void set_object_pose(int i, double e, double n, double agl_m, double heading_deg, double pitch_deg, double bank_deg,
                const Vector3 &scale, bool record = true);
        // The same with the rotation as a Basis in the local frame (+X west, +Y up, +Z north).
        void set_object_transform(int i, double e, double n, double agl_m, const Basis &rot, const Vector3 &scale, bool record = true);
        // heading / pitch / bank (degrees) of a local rotation, and back: rot = Ry(-heading) * Rx(pitch) * Rz(bank).
        static Vector3 euler_of(const Basis &rot);
        static Basis basis_of(double heading_deg, double pitch_deg, double bank_deg);
        // Sets the height above ground so the model's lowest vertex touches the terrain.
        void drop_to_ground(int i);
        void remove_object(int i);
        bool undo_object_edit();
        bool has_unsaved_edits() const;
        // Rewrites the package.json of every edited pack (objects + their exclusion circles). Returns the number written.
        int save_packs();

        Callable get_elevation() const; void set_elevation(const Callable &c);
        double get_load_radius_m() const; void set_load_radius_m(double v);
        double get_unload_radius_m() const; void set_unload_radius_m(double v);
        bool get_enabled() const; void set_enabled(bool v);

    protected:
        struct Pack
        {
            String dir, name;
            Dictionary json;
            bool dirty = false;
        };
        struct Placed
        {
            String name, model_path;
            int pack = -1, pack_obj_index = -1;
            Dictionary orig; // the package.json entry as read
            double lat = 0.0, lon = 0.0, e = 0.0, n = 0.0;
            double agl = 0.0, elevation = 0.0; // elevation: absolute, used when has_elevation
            bool has_elevation = false;
            Basis rot;              // rotation in the local frame (see set_object_transform)
            Vector3 scale3 = Vector3(1, 1, 1);
            double radius_m = 0.0;  // horizontal footprint radius from the package (unscaled)
            AABB bounds;            // of the unscaled model, once its template is in
            bool bounds_known = false;
            Vector3 pivot;          // model-space point that sits at the placement position (0 = the model origin)
            bool excluded = false, removed = false;
            bool highlighted = false; // editor selection tint (material overlay on every mesh)
            Vector3 ground_ecef; // at h = 0, for distance tests
            Vector3 size_m;         // scaled model size from the package (optional, before the template is in)
            double view_m = 0.0;    // "view_distance_m" from the package (0 = from the size)
            Node3D *node = nullptr;
        };
        // Visible range of a placed model: from its size (a lamp 2.5 km, a hangar
        // ~9 km, a terminal pier 30 km), or the package's view_distance_m. The
        // model loads inside it and fades at its edge (GeometryInstance3D
        // visibility range), and fades in over a second when it appears.
        double view_distance_m(const Placed &p) const;
        struct Fade { uint64_t id; uint64_t t0_msec; };
        std::vector<Fade> fading;
        void apply_view_range(Placed &p);
        void step_fades();
        struct Edit
        {
            int index;
            double e, n, agl;
            Basis rot;
            Vector3 scale3;
            Vector3 pivot;
            bool removed;
            int group;
        };
        int undo_group = 0;
        bool group_open = false;
        std::vector<Pack> packs;
        std::vector<Placed> objects;
        std::vector<Edit> undo_stack;
        Array zones;          // for the managers and for get_exclusion_zones()
        Dictionary templates; // model path -> Node (kept out of the tree)
        Dictionary template_bounds; // model path -> AABB
        std::unordered_map<int64_t, PackedVector3Array> mesh_triangles; // Mesh instance id -> triangle vertices (mesh space)
        const PackedVector3Array &triangles_of(const Ref<Mesh> &mesh);
        static bool ray_hits_node(Node *node, const Vector3 &origin, const Vector3 &dir, double &t, SceneryObjects *self);
        // Background glTF loading: paths queued by the main thread, the
        // generated (out-of-tree) scenes handed back to it.
        struct LoadResult
        {
            String path;
            Node *scene = nullptr; // generated on the worker as well: a state parsed
                                   // elsewhere leaves ImporterMeshInstance3Ds behind
        };
        std::thread worker;
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::deque<String> load_queue;
        std::deque<LoadResult> results;
        Dictionary pending; // path -> true while queued or parsing
        bool worker_stop = false;
        void start_worker();
        void stop_worker();
        void worker_main();
        void collect_results();
        Callable elevation;
        double load_radius_m = 2500.0;    // minimum view distance of any object
        double unload_radius_m = 30000.0; // maximum view distance (unload at 1.25 x the view distance)
        bool enabled = true;
        int frame_counter = 0;
        int excluded_objects = 0;

        void clear_instances();
        void clear_templates();
        // The parsed template, or null while it is still loading (queued on demand).
        Node *load_template(const String &path);
        static void accumulate_bounds(Node *node, const Transform3D &xf, AABB &box, bool &first);
        double placement_height(Placed &p) const;
        Transform3D placement_transform(const Placed &p, double h) const;
        bool place(Placed &p);
        void unplace(Placed &p);
        void apply_highlight(Placed &p);
        Ref<Material> highlight_material;
        void refresh(Placed &p);
        // Horizontal radius / lowest point of the placed (rotated, scaled) model.
        double footprint_m(const Placed &p) const;
        double lowest_m(const Placed &p) const;
        Vector3 camera_position() const;
        static Array read_package(const String &path);
        static Node *find_class(Node *root, const char *cls);
    };

} // namespace jsbgodot

#endif
