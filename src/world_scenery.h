#ifndef WORLD_SCENERY_H
#define WORLD_SCENERY_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "vfr_map.h"

using namespace godot;

namespace jsbgodot
{

    // The scenery stack seen from the flight side: the precise Scenery3D
    // patch, the coarse world3d globe under it, the drawn runway surfaces on
    // top, and the conversions between the ECEF world, WGS84 and the LV95
    // data grid (design §9.5). Formerly the elevation / geodetic / streaming
    // / globe-suppression helpers of node_3d.gd.
    //
    // Scenery3D, the globe and the runways are other extensions or scripts,
    // so they are driven through their bound methods (call / has_method)
    // exactly as the GDScript did; every query degrades to NAN when a node
    // or method is missing.
    class WorldScenery : public RefCounted
    {
        GDCLASS(WorldScenery, RefCounted);

    public:
        static void _bind_methods();
        WorldScenery();

        // host = the main scene node (viewport camera lookup), ac_node = $AC,
        // scenery3d / globe / vegetation may be null.
        void register_nodes(Node *host, Node3D *ac_node, Node *scenery3d, Node *globe, Node *vegetation);
        // The Runways node (runways.gd, surface_lift_at) once it exists.
        void set_runways(Node *runways);
        void set_vfr_map(VfrMap *vfr_map);
        bool has_globe() const;
        Node *get_globe() const;
        Node *get_scenery3d() const;
        Node *get_vegetation() const;

        // ── Geodetic helpers (ECEF world) ──
        // (lat, lon, h) of a world position.
        static Vector3 geodetic_of(const Vector3 &world_pos);
        // WGS84 (lat, lon) of an LV95 point.
        static Vector2 latlon_of_lv95(double e, double n);
        // World position of an LV95 point at height h (metres, ellipsoidal = ASL).
        static Vector3 ecef_of_lv95(double e, double n, double h);
        // Level aircraft transform at an LV95 point: nose (+Z local) along a
        // TRUE heading, up = ellipsoid normal, the node's scale preserved.
        static Transform3D ac_transform_at(double e, double n, double h, double true_heading_deg, const Vector3 &scale);
        // TRUE heading (deg) of an aircraft node from its basis (nose = +Z).
        static double ac_true_heading(Node3D *ac);
        // Ellipsoid normal at a world position.
        static Vector3 up_at(const Vector3 &world_pos);

        // ── Elevation (metres ASL; NAN = no data) ──
        // Precise Scenery3D patch only (0.0 means "no data" there).
        double scenery_elevation_geodetic(double lat, double lon) const;
        // Coarse whole-planet globe only.
        double globe_elevation_geodetic(double lat, double lon) const;
        // Patch, else globe.
        double terrain_elevation_geodetic(double lat, double lon) const;
        double terrain_elevation_below(const Vector3 &world_pos) const;
        // JSBSim's ground: the patch elevation plus the lift of a drawn
        // runway / taxiway / apron surface there (design §12.5), or the
        // drawn road surface there when it is higher (within
        // ROAD_SURFACE_TOL_M of the terrain). NAN without patch data (the
        // bridge keeps its flat fallback).
        double ground_height_geodetic(double lat, double lon) const;
        // How far the aircraft has sunk into the sea (m): the ground query over
        // water answers the surface minus this, down to the sea floor.
        void set_water_sink_m(double m);
        double get_water_sink_m() const;
        // Terrain (patch or globe) plus the runway lift under a world
        // position, or the drawn road surface there when it is higher. The
        // position's own height is the hint for the road lookup: a car keeps
        // the deck it is on and the ground under a bridge (docs/VEHICLES.md).
        double ground_height_below(const Vector3 &world_pos) const;
        // The drawn road / rail surface under a geodetic point (metres ASL):
        // the highest baked road triangle within ROAD_SURFACE_TOL_M of h_hint
        // (S3DRoadManager::surface_height_at), NAN when none is there.
        double road_surface_at(double lat, double lon, double h_hint) const;
        static constexpr double ROAD_SURFACE_TOL_M = 2.0;
        // Highest terrain among the threshold and four points along / across
        // the runway heading (±25 m / ±15 m in the LV95 grid), matching the
        // runway mesh approach; NAN when no sample has data.
        double max_terrain_around(double e, double n, double true_heading_deg) const;
        // Loading progress: how many of the centre + four ring points
        // (radius_m along E/N) have terrain data. Returns (loaded, total).
        Vector2i count_loaded_ring(double e, double n, double radius_m) const;

        // ── Streaming ──
        // The Scenery3D tile manager (live coverage grid), or null.
        Object *get_tile_manager() const;
        // Pending tile work of the streamer (-1 when unknown).
        int pending_count() const;
        bool streamer_busy() const;
        // Toggle _process on every Scenery3D child manager and on the
        // Vegetation manager: streaming stays off while the welcome screen is
        // up so no tiles load around the origin before a destination is known.
        void set_streaming(bool enabled);

        // ── Coarse-globe suppression (design §8.1 iteration 4) ──
        // Force the next refresh to upload the mask even if nothing moved.
        void mark_suppress_dirty();
        // Per frame: upload Scenery3D's live coverage mask to the globe when
        // the camera moved ≥ 32 m or a tile/chunk appeared or disappeared
        // (coverage generation), throttled to one upload per 100 ms.
        void refresh_globe_suppress();
        // Hand a coverage mask to the globe: cells is (2*r_tiles+1)² bytes,
        // row-major (row = north tile, column = east tile), centred on tile
        // (cam_ei, cam_ni); each cell spans tile_m metres of the LV95 grid.
        void push_suppress_from_coverage(const PackedByteArray &cells, int r_tiles, int cam_ei, int cam_ni, double tile_m);

        // ── VFR map ──
        // ECEF position + true heading -> LV95 + grid heading for the map.
        // Ignores the FDM's pre-initialisation (0,0,0) position.
        void feed_vfr_map(const Vector3 &world_pos, double true_heading_deg, bool ui_up);

        // ── Airfield lighting ──
        // On from dusk (sun 4° above the horizon -> full at 4° below) and
        // whenever the visibility is below 1 km.
        static double airfield_light_intensity(const Vector3 &cam_pos, const Vector3 &sun_dir, double visibility_km);

    protected:
        Node *host = nullptr;
        Node3D *ac_node = nullptr;
        VfrMap *vfr_map = nullptr;
        // Nodes owned by other extensions / scripts are held by instance id
        // and re-resolved on every use (a freed node yields null).
        uint64_t scenery3d_id = 0;
        uint64_t globe_id = 0;
        uint64_t vegetation_id = 0;
        uint64_t runways_id = 0;
        double water_sink_m = 0.0;
        mutable uint64_t road_manager_id = 0;   // Scenery3D's S3DRoadManager child, found on first use
        static Node *live_node(uint64_t id);
        Node *road_manager_node() const;

        // Suppression-mask refresh state
        Vector2 suppress_last_center;
        bool suppress_dirty = true;
        int64_t suppress_cov_gen = -1;
        int64_t suppress_last_ms = 0;
        static constexpr int64_t SUPPRESS_REFRESH_MS = 100;

        Node *scenery_node() const { return live_node(scenery3d_id); }
        Node *globe_node() const { return live_node(globe_id); }
        double runway_lift_at(const Vector3 &world_pos) const;
        Vector3 stream_center() const; // viewport camera, else the AC
    };

} // namespace jsbgodot

#endif
