#ifndef SCENERY_DESIGN_H
#define SCENERY_DESIGN_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

namespace jsbgodot
{

    // Editable scenery design document (docs/SCENERY_EDITOR.md; formerly
    // scenery_design.gd): a graph of nodes (LV95 metres) joined by edges that
    // are road or railway segments, plus replace / flatten / area polygons.
    // Everything the scenery editor changes lives here; the mesher draws it
    // live and the bake (scripts/build_world_roads.py --design) folds it into
    // the base tier. Plain JSON on disk so packs can ship and diff it.
    //
    // The state is kept in Dictionaries with the same shape as the JSON so
    // the GDScript editor keeps reading and mutating it in place:
    //   nodes: id -> {"e", "n", ["h" height offset m]}
    //   edges: id -> {"a", "b", kind, cls, lanes, oneway, gauge, tracks, width, ["via": [e, n]]}
    //   replace: [[Vector2, ...]], flatten: [{"poly", "h"}], areas: [{"poly", "type", "h"}]
    class SceneryDesign : public RefCounted
    {
        GDCLASS(SceneryDesign, RefCounted);

    public:
        static void _bind_methods();
        SceneryDesign();

        static Dictionary road_attrs(const String &cls, int lanes, bool oneway, double width = 0.0);
        static Dictionary rail_attrs(const String &cls, int gauge_mm, int tracks = 1);
        static Array get_road_classes();
        static Array get_rail_classes();
        static Array get_area_types();
        static Array get_edge_attrs();
        static double get_switch_min_radius_m();

        int node_count() const;
        int edge_count() const;
        Vector2 node_pos(int id) const;
        int add_node(double e, double n);
        void move_node(int id, double e, double n);
        // Height offset of a node above (+) / below (-) the ground.
        double node_offset(int id) const;
        void set_node_offset(int id, double h);
        // Douglas-Peucker simplification of a polyline (LV95), tolerance in m.
        static Array simplify(const Array &pts, double tol);
        // Imports a baked OSM way (a record of a roads_E_N.ways.json sidecar);
        // its id goes to replace_ids. Returns the node ids.
        Array import_way(const Dictionary &rec);
        int area_near(double e, double n, double radius) const;
        int flatten_near(double e, double n, double radius) const;
        int flatten_inside(double e, double n) const; // the smallest plateau containing the point, -1 if none
        int replace_near(double e, double n, double radius) const;
        // A duplicate a-b edge or a == b is refused (-1).
        int add_edge(int a, int b, const Dictionary &attrs);
        void set_edge_via(int id, const Vector2 &via);
        void clear_edge_via(int id);
        bool is_arc(int id) const;
        // Polyline of an edge a -> b: the two ends, or an arc sampled every ARC_STEP_M.
        Array edge_points(int id) const;
        static Dictionary circle_through(const Vector2 &a, const Vector2 &v, const Vector2 &b);
        static Dictionary arc_sweep(const Vector2 &a, const Vector2 &v, const Vector2 &b);
        static Vector2 arc_at(const Dictionary &sw, double t);
        static Array arc_points(const Vector2 &a, const Vector2 &v, const Vector2 &b);
        // Via of the arc leaving p along the unit tangent t and ending at q; {} when q lies on the tangent line.
        static Dictionary tangent_arc_via(const Vector2 &p, const Vector2 &t, const Vector2 &q);
        Vector2 edge_tangent_out(int id, int node_id) const;
        Vector2 node_tangent_out(int node_id) const;
        Array add_circle(const Vector2 &centre, double radius, const Dictionary &attrs, int segments = 8);
        Vector2 track_tangent_at(int edge_id, double e, double n, const Vector2 &towards) const;
        // A turnout: {"node", "edge", "radius", "end"} or {"error": ...}.
        Dictionary add_switch(int edge_id, double e, double n, const Vector2 &q);
        int reverse_chain(int edge_id);
        void remove_edge(int id);
        void set_edge_attrs(int id, const Dictionary &attrs);
        int degree(int node_id) const;
        Array edges_of(int node_id) const;
        int split_edge(int id, double e, double n);
        // Nearest point of an edge's polyline: {"p", "dist", "i" (segment index)}.
        Dictionary nearest_on_edge(int id, const Vector2 &q) const;
        int node_near(double e, double n, double radius) const;
        Dictionary edge_near(double e, double n, double radius) const;
        static Vector2 nearest_on_segment(const Vector2 &a, const Vector2 &b, const Vector2 &q);
        // Runs of equal-attribute edges through degree-2 nodes: [{"attrs", "nodes", "edges"}].
        Array chains() const;
        void snapshot();
        bool can_undo() const;
        bool undo();
        Dictionary to_dict() const;
        void from_dict(const Dictionary &d);
        static String file_sha256(const String &p);
        bool save(const String &p = String());
        bool load(const String &p);

        Array get_replace() const; void set_replace(const Array &v);
        Array get_replace_ids() const; void set_replace_ids(const Array &v);
        Array get_flatten() const; void set_flatten(const Array &v);
        Array get_areas() const; void set_areas(const Array &v);
        Dictionary get_patch_cache() const; void set_patch_cache(const Dictionary &v);
        Dictionary get_nodes() const; void set_nodes(const Dictionary &v);
        Dictionary get_edges() const; void set_edges(const Dictionary &v);
        int get_next_id() const; void set_next_id(int v);
        String get_path() const; void set_path(const String &v);
        bool get_dirty() const; void set_dirty(bool v);

    protected:
        static constexpr int SCHEMA = 1;
        static constexpr double ARC_STEP_M = 10.0;
        static constexpr int ARC_MIN_SEGMENTS = 4;
        static constexpr double SWITCH_MIN_RADIUS_M = 150.0; // 190 m is the tightest standard turnout

        Array replace;
        Array replace_ids;
        Array flatten;
        Array areas;
        Dictionary patch_cache;
        Dictionary nodes;
        Dictionary edges;
        int next_id = 1;
        String path;
        bool dirty = false;
        Array undo_stack;

        static bool same_attrs(const Dictionary &x, const Dictionary &y);
        static Variant attr_default(const String &k);
        static Dictionary patches_to_json(const Dictionary &pc);
        static Dictionary patches_from_json(const Dictionary &d);
        static Array poly_to_json(const Array &poly);
        static Array poly_from_json(const Array &poly);
    };

} // namespace jsbgodot

#endif
