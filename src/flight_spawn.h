#ifndef FLIGHT_SPAWN_H
#define FLIGHT_SPAWN_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "cockpit_controls.h"
#include "control_surfaces.h"
#include "slew_mode.h"
#include "terrain_elevation.h"
#include "world_scenery.h"

using namespace godot;

namespace jsbgodot
{

    // Putting the aircraft on the ground: the first flight (place the AC over
    // the target, stream the scenery there, wait for the precise tile, then
    // initialise JSBSim on it), later teleports (re-initialise JSBSim from a
    // fresh pose on the sampled terrain) and the per-frame post-spawn pushes
    // (rebase cooldown, ground elevation) that node_3d.gd used to gate by
    // hand. Formerly _do_first_flight, _on_teleport_requested,
    // _do_deferred_terrain_init, _process_rebase_cooldown and
    // _update_terrain_elevation.
    //
    // GDScript keeps what touches its own objects: it connects `spawned`
    // to apply the fuel setting, set the parking brake and refresh the live
    // weather after every (re)initialisation.
    class FlightSpawn : public RefCounted
    {
        GDCLASS(FlightSpawn, RefCounted);

    public:
        static void _bind_methods();
        FlightSpawn();

        void register_nodes(Node3D *ac_node, Object *jsb, WorldScenery *world, TerrainElevation *terrain,
                ControlSurfaces *surfaces, CockpitControls *cockpit, SlewMode *slew);

        // Level, static pose at an LV95 point (welcome parking, Edit Mode).
        void park(double e, double n, double h, double true_heading_deg);

        // Fly!: the first flight starts the deferred terrain init, every later
        // one teleports the running simulation. ride_camera is made current
        // when the viewport camera does not ride with the aircraft (the tile
        // streamer follows the viewport camera); returns true when that
        // happened so the caller can mirror its active camera.
        bool request_flight(double e, double n, double true_heading_deg, Camera3D *ride_camera);
        bool is_first_flight() const;

        // Per frame while the deferred init is pending: waits for the precise
        // tile (while the streamer is busy, up to a hard cap), else falls back
        // to the globe, then places the AC and initialises JSBSim. Returns
        // true on the frame the initialisation completed.
        bool process_deferred_init();
        bool is_init_pending() const;
        // True once JSBSim was initialised on terrain (the flight is live).
        bool is_initialized() const;
        // {swiss_e, swiss_n, heading_deg} of the pending / last first flight.
        Dictionary get_target() const;
        bool has_target() const;

        // Per frame once initialised (no-ops before): the post-teleport pin +
        // JSBSim release, and the ground elevation under the aircraft.
        void process_rebase();
        void update_terrain_elevation();

    protected:
        Node3D *ac_node = nullptr;
        Object *jsb = nullptr;
        WorldScenery *world = nullptr;
        TerrainElevation *terrain = nullptr;
        ControlSurfaces *surfaces = nullptr;
        CockpitControls *cockpit = nullptr;
        SlewMode *slew = nullptr;

        bool first_flight = true;
        bool initialized = false;
        bool init_pending = false;
        bool has_target_v = false;
        double target_e = 0.0, target_n = 0.0, target_heading = 0.0;
        int64_t init_start_msec = 0;
        int64_t init_last_busy_msec = 0;
        int64_t init_last_print_msec = 0;

        // Idle-streamer window before the globe fallback, and the absolute cap
        // on waiting for the precise tile.
        static constexpr int64_t TERRAIN_INIT_FALLBACK_MSEC = 12000;
        static constexpr int64_t TERRAIN_INIT_HARD_CAP_MSEC = 90000;
        // JSBSim reports CG altitude; the C172P CG sits ~1.3 m above the wheel
        // contact points, so the visual origin is lifted by this much.
        static constexpr double WHEEL_GROUND_CLEARANCE = -1.1;
        static constexpr double SPAWN_GEAR_HEIGHT = 0.5;       // a hair above the ground
        static constexpr double RUNWAY_SURFACE_OFFSET = 0.1;   // drawn surface above the terrain

        void begin_first_flight(double e, double n, double true_heading_deg);
        bool teleport(double e, double n, double true_heading_deg, Camera3D *ride_camera);
        bool scenery_valid() const;
    };

} // namespace jsbgodot

#endif
