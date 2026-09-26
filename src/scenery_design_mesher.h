#ifndef SCENERY_DESIGN_MESHER_H
#define SCENERY_DESIGN_MESHER_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/templates/vector.hpp>

#include "scenery_design.h"

using namespace godot;

namespace jsbgodot
{

    // Draws a SceneryDesign live (formerly scenery_design_mesher.gd): one mesh
    // per chain, extruded in a local frame at the chain's first node and
    // drawn with the base-tier roads shader (metres in the UVs, class colour
    // + surface kind in the vertex colour), exactly like the baked tiles of
    // scripts/build_world_roads.py; a pavement at every junction, bridge
    // structures under elevated stations and the area patches. Heights come
    // from the host's terrain query (`elevation` Callable: LV95 e, n ->
    // metres or NAN).
    class SceneryDesignMesher : public Node3D
    {
        GDCLASS(SceneryDesignMesher, Node3D);

    public:
        static void _bind_methods();
        SceneryDesignMesher();

        // [width m, shader kind, lanes] from the attributes (road_geometry()
        // of scripts/build_world_roads.py).
        static Array road_geometry(const String &cls, int lanes, bool oneway, double width_m = 0.0);
        static double edge_width(const Ref<SceneryDesign> &design, int eid);
        static Array chain_polyline(const Ref<SceneryDesign> &design, const Dictionary &chain);
        // {"pts", "offs"}: the polyline and the node height offsets along it.
        static Dictionary chain_polyline_offsets(const Ref<SceneryDesign> &design, const Dictionary &chain);
        static Array densify(const Array &pts);
        static Dictionary densify_with(const Array &pts, const Array &vals);
        static double get_ballast_width_m();

        void clear();
        void rebuild(const Ref<SceneryDesign> &design);
        // edge id -> {node id: metres the leg is cut back at a junction}
        Dictionary get_cuts() const;

        Callable get_elevation() const; void set_elevation(const Callable &c);
        Color get_tint() const; void set_tint(const Color &c);
        int get_chain_count() const;
        int get_junction_count() const;
        int get_area_count() const;
        int get_pier_count() const;
        int get_unknown_height_count() const;

    protected:
        static constexpr double DENSIFY_M = 10.0;
        static constexpr double ROAD_LIFT_M = 0.20;
        static constexpr double BALLAST_LIFT_M = 0.15;
        static constexpr double BALLAST_WIDTH_M = 4.2;
        static constexpr double TRACK_SPACING_M = 3.8;
        static constexpr double RAIL_TOP_M = 0.172;
        static constexpr double RAIL_HEAD_M = 0.072;
        static constexpr double RAIL_FOOT_M = 0.15;
        static constexpr double MITER_LIMIT = 2.5;
        enum Kind { KIND_ONEWAY = 1, KIND_TWOWAY_LINED = 2, KIND_TWOWAY = 3, KIND_LINK = 4, KIND_DUAL = 5, KIND_JUNCTION = 6,
            KIND_CONCRETE = 7, KIND_BALLAST = 8, KIND_RAIL = 9, KIND_ROOF = 14, KIND_WALL = 15 };
        static constexpr double ELEVATED_M = 0.3; // a station this far above the ground gets a structure
        static constexpr double PIER_SPACING_M = 25.0;
        static constexpr double PIER_W_M = 1.2;
        static constexpr double PARAPET_H_M = 0.45;
        static constexpr double PARAPET_W_M = 0.3;
        static constexpr double TAPER_PER_LANE_M = 30.0;
        static constexpr double JUNCTION_MIN_CUT_M = 1.0;
        static constexpr double SHOULDER_M = 2.5;
        static constexpr double VERGE_M = 0.5;

        struct Buf
        {
            PackedVector3Array verts;
            PackedVector3Array normals;
            PackedColorArray colors;
            PackedVector2Array uvs;
            PackedVector2Array uv2s;
            PackedInt32Array idx;
            void push(const Vector3 &v, const Vector3 &n, const Color &c, const Vector2 &uv, const Vector2 &uv2);
        };
        struct Section
        {
            Vector<Vector2> pts; // (u, h)
        };

        Callable elevation;
        Color tint = Color(1, 1, 1);
        Ref<ShaderMaterial> material;
        int chain_count = 0, junction_count = 0, area_count = 0, pier_count = 0, unknown_height_count = 0;
        Dictionary cuts;

        static double lane_m(const String &cls);
        static Color class_colour(const String &cls);
        static double class_drop_m(const String &cls);
        static int area_kind(const String &typ);
        void ensure_material();
        double ground(double e, double n) const;
        Dictionary junction(const Ref<SceneryDesign> &design, int nid) const;
        MeshInstance3D *build_junction(const Ref<SceneryDesign> &design, int nid, const Dictionary &j);
        MeshInstance3D *mesh_instance(const Buf &buf, const Transform3D &frame);
        static Array trim_start(const Array &pts, double metres);
        static Array trim_vals(const Array &pts, const Array &vals, double metres);
        static double neighbour_half(const Ref<SceneryDesign> &design, const Dictionary &chain, bool at_start);
        MeshInstance3D *build_chain(const Ref<SceneryDesign> &design, const Dictionary &chain);
        void structures(Buf &buf, const Vector<Vector3> &local, const Vector<double> &offs, const Vector<double> &halves, double half);
        void pier(Buf &buf, const Vector<Vector3> &local, int i, double height, double hw);
        MeshInstance3D *build_area(const Dictionary &area, int index);
        static Vector<Vector2> frames(const Vector<Vector3> &local);
        static void extrude(Buf &buf, const Vector<Vector3> &local, const Section &section, const Color &colour, int kind,
                double u0, double half_w, double count, const Vector<double> &halves = Vector<double>());
        static Transform3D frame_at_lv95(const Vector2 &p, double h);
        static Vector3 local_of(const Transform3D &inv, const Vector2 &p, double h);
    };

} // namespace jsbgodot

#endif
