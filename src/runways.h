#ifndef RUNWAYS_H
#define RUNWAYS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

namespace jsbgodot
{

    // Renders the merged per-pack airport surfaces (design §12; formerly
    // runways.gd). For every airport of RunwaysLib.load_merged(): ONE mesh
    // ("Surfaces") holding the procedural runway strips, taxiway pavement,
    // guidance lines and apron polygons (unless "procedural": false), one
    // point mesh ("Lights") with the airfield lighting, plus an optional
    // pack-relative glTF "model" at the reference point. Every vertex is
    // projected onto the WGS84 ellipsoid and expressed in the airport's
    // West-Up-North frame.
    //
    // One ShaderMaterial is shared by every surface: the per-runway
    // parameters travel in the CUSTOM0 / CUSTOM1 vertex attributes (a
    // material per runway crashed the Metal driver). Heights come from the
    // plane / profile the terrain was flattened to plus a lift so the strip
    // wins the depth test (runway 8 cm, taxiway 10 cm, apron 3 cm); the
    // lookup behind surface_lift_at() gives JSBSim that drawn surface.
    //
    // Airports within BUILD_RADIUS_M of the camera are built a few per
    // update, those beyond FREE_RADIUS_M are freed.
    class Runways : public Node3D
    {
        GDCLASS(Runways, Node3D);

    public:
        enum SurfaceKind { KIND_ASPHALT = 0, KIND_CONCRETE = 1, KIND_GRASS = 2, KIND_GRAVEL = 3, KIND_TAXIWAY = 4, KIND_APRON = 5, KIND_TAXI_LINE = 6 };

        static void _bind_methods();
        Runways();

        // The merged airports (icao -> entry); update() builds the ones near
        // the camera. Existing nodes are dropped.
        void set_merged(const Dictionary &merged, const Callable &terrain_height_geodetic = Callable());
        // Per frame: build airports entering BUILD_RADIUS_M of cam_pos
        // (ECEF), free those beyond FREE_RADIUS_M.
        void update(const Vector3 &cam_pos, double delta);
        // Build every airport now (tests, probes).
        void build(const Dictionary &merged, const Callable &terrain_height_geodetic = Callable());
        void build_airport(const String &icao, const Dictionary &ap, const Callable &terrain_height_geodetic);
        // 0 = day (lights off) .. 1 = night / low visibility.
        void set_light_intensity(double v);
        Ref<ShaderMaterial> get_material();
        Ref<ShaderMaterial> get_light_material();
        int built_count() const;
        Node3D *get_airport_node(const String &icao) const;
        // Lift of the drawn surface above the terrain at an ECEF position
        // (runway 8 cm, taxiway 10 cm, apron 3 cm, 0 off any pavement).
        double surface_lift_at(const Vector3 &world_pos) const;

        int get_runway_count() const;
        int get_model_count() const;
        int get_taxiway_count() const;
        int get_apron_count() const;
        int get_light_count() const;
        static double get_surface_lift_m();
        static double get_taxi_lift_m();
        static double get_apron_lift_m();

    protected:
        static constexpr double SURFACE_LIFT_M = 0.08;
        static constexpr double TAXI_LIFT_M = 0.10;
        static constexpr double APRON_LIFT_M = 0.03;
        static constexpr double LINE_LIFT_M = 0.13;
        static constexpr double LINE_HALF_W = 0.25;
        static constexpr double SEGMENT_M = 50.0;
        static constexpr double VISIBLE_RANGE_M = 60000.0;
        static constexpr double BUILD_RADIUS_M = 150000.0;
        static constexpr double FREE_RADIUS_M = 250000.0;
        static constexpr int BUILDS_PER_UPDATE = 4;
        static constexpr double UPDATE_INTERVAL_S = 0.5;
        static constexpr double LOOKUP_CELL_M = 50.0;
        static constexpr double LOOKUP_RANGE_M = 8000.0;
        // Lighting layout (metres)
        static constexpr double LIGHT_LIFT_M = 0.35;
        static constexpr double EDGE_SPACING_M = 60.0;
        static constexpr double EDGE_CAUTION_M = 600.0;
        static constexpr double CENTRE_SPACING_M = 30.0;
        static constexpr double CENTRE_MIN_LENGTH_M = 1800.0;
        static constexpr double THRESHOLD_SPACING_M = 6.0;
        static constexpr double APPROACH_SPACING_M = 30.0;
        static constexpr double APPROACH_LONG_M = 900.0;
        static constexpr double APPROACH_SHORT_M = 420.0;
        static constexpr double TAXI_CENTRE_SPACING_M = 15.0;
        static constexpr double TAXI_EDGE_SPACING_M = 30.0;
        static constexpr double PAPI_DISTANCE_M = 300.0;
        static constexpr double PAPI_OFFSET_M = 15.0;
        static constexpr double PAPI_UNIT_SPACING_M = 9.0;
        static constexpr double PAPI_SIZE = 3.0;
        static constexpr int OSM_CLASS_MIN = 8;

        // Vertex arrays under construction for one airport mesh.
        struct Batch
        {
            PackedVector3Array verts;
            PackedVector3Array normals;
            PackedVector2Array uvs;
            PackedFloat32Array custom0; // half_width, length, displaced0, displaced1
            PackedFloat32Array custom1; // glyphs0, glyphs1, kind*64+bars*2+aiming, crossing flag
            PackedInt32Array idx;
            Array runways;              // [{name, first, count, h0, h1, length}]
            struct Rect { Vector2 p0, u; double length, half_w; };
            Vector<Rect> rwy_rects;     // local x/z
            Vector<PackedVector2Array> apron_polys;
            Vector<int> taxi_tri_first; // first index of every pavement triangle
            int add_vertex(const Vector3 &p, const Vector2 &uv, const float *c0, const float *c1);
        };
        struct Lights
        {
            PackedVector3Array verts;
            PackedFloat32Array c0, c1, c2;
            void add(const Vector3 &p, const Color &a, const Color &b, const Vector2 &axis, int mode, double size = 1.0, double papi_deg = 0.0);
            int size() const { return verts.size(); }
        };
        // Geometry of a runway in the airport frame for the fixture generators.
        struct RwyGeo
        {
            Vector2 p0, u, r;           // LV95 pavement end 0, unit along, unit right
            double length = 0, half_w = 0, h0 = 0, h1 = 0, disp0 = 0, disp1 = 0;
            bool lit = false;
            double glide_deg[2] = {3.0, 3.0}; // PAPI approach slope per end (the ILS glide path when the end has one)
            Vector2 axis_local;         // along direction in the airport frame (x, z)
            double z_at(double t) const { return h0 + (h1 - h0) * t / length; }
        };
        struct Lookup
        {
            Transform3D inv;
            Vector<Batch::Rect> runways;
            Vector<PackedVector2Array> aprons;
            PackedVector3Array verts;
            PackedInt32Array idx;
            HashMap<int64_t, Vector<int>> grid; // cell key -> triangle first indices
        };

        Ref<Shader> shader;
        Ref<ShaderMaterial> material;
        Ref<Shader> light_shader;
        Ref<ShaderMaterial> light_material;
        Dictionary merged;
        HashMap<String, Node3D *> airport_nodes;
        HashMap<String, Vector3> airport_ecef;
        HashMap<String, Lookup> lookup;
        Callable terrain_cb;
        double since_update = 0.0;
        int runway_count = 0, model_count = 0, taxiway_count = 0, apron_count = 0, light_count = 0;

        void ensure_material();
        static int surface_kind(const String &surface);
        static int bars_for_width(double w);
        static double pack_glyphs(const PackedInt32Array &g);
        static Vector2 end_heights(const Dictionary &rw, const Vector2 &p0, const Vector2 &p1, const Callable &terrain);
        bool add_runway(Batch &batch, const Transform3D &inv, const Dictionary &rw, const Callable &terrain);
        void add_aprons(Batch &batch, const Transform3D &inv, const Array &aprons);
        void add_taxiways(Batch &batch, const Transform3D &inv, const Array &taxiways);
        void add_taxiway_mesh(Batch &batch, const Transform3D &inv, const Dictionary &mesh);
        void add_disc(Batch &batch, const Vector3 &centre, double radius, double along, const float *c0, const float *c1);
        MeshInstance3D *mesh_instance(Batch &batch, const Transform3D &frame);
        void register_lookup(const String &icao, const Batch &batch, const Transform3D &frame);
        static int64_t cell_key(int cx, int cz);
        // lights
        static Vector3 lv95_local(const Transform3D &inv, const Vector2 &p, double h);
        static Vector3 ll_local(const Transform3D &inv, double lat, double lon, double h);
        static bool rwy_geo(const Dictionary &rw, const Transform3D &inv, RwyGeo &out);
        static void gen_runway_lights(Lights &L, const RwyGeo &g, bool intl, const Dictionary &classes, const Transform3D &inv);
        static const RwyGeo *nearest_geo(const Vector2 &q, const Vector<RwyGeo> &geos);
        static void gen_taxiway_lights(Lights &L, const Array &taxiways, const Transform3D &inv, const Dictionary &classes);
        static void add_osm_lights(Lights &L, const Dictionary &ap, const Transform3D &inv, const Dictionary &classes, const Vector<RwyGeo> &geos);
        MeshInstance3D *build_lights(const String &icao, const Dictionary &ap, const Transform3D &frame);
        // The approach slope a runway end's PAPI is set to: the ILS glide path of
        // that end (NavaidsLib catalog, LSZH 28 = 3.3 deg) or 3.0 deg.
        static double papi_glide_deg(const String &icao, const String &end_ident);
        static Node3D *load_model(const String &path);
        static Transform3D frame_at(double lat, double lon, double h);
    };

} // namespace jsbgodot

VARIANT_ENUM_CAST(jsbgodot::Runways::SurfaceKind);

#endif
