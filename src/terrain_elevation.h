#ifndef TERRAIN_ELEVATION_H
#define TERRAIN_ELEVATION_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace jsbgodot
{

    class JSBGodot;

    // Post-teleport terrain handling, moved out of node_3d.gd so the per-frame
    // state machine + math runs in C++ (double precision). Two responsibilities:
    //
    //   1. Rebase cooldown — after a teleport / first flight, pin the AC at the
    //      destination while terrain tiles stream in. When the countdown expires,
    //      re-query the ground height (retrying up to a timeout if the tile is
    //      still missing), lift the pin to gear clearance, re-initialise JSBSim
    //      at the final position and release slew mode so physics resumes.
    //
    //   2. Per-frame terrain push — feed JSBSim's ground-collision model with the
    //      real terrain elevation under the aircraft (converted to ASL), skipping
    //      the first N frames so JSBSim can stabilise and never pushing a stale
    //      0.0 m when the tile hasn't streamed in yet.
    //
    // GDScript (node_3d.gd) keeps the scenery-stack concern: it owns the AC node
    // registration, the terrain-height query (_terrain_elevation_at, which prefers
    // the precise Scenery3D patch and falls back to the world3d globe), and the
    // terrain_initialized gate set during the deferred init handshake. It calls
    // process_rebase() / update_terrain_elevation() once per frame with the JSBSim
    // object and the terrain height at the relevant position (NAN when no data).
    class TerrainElevation : public RefCounted
    {
        GDCLASS(TerrainElevation, RefCounted);

    public:
        static void _bind_methods();

        // ── Node registration (session-stable; no reset needed) ──
        // ac_node = aircraft root ($AC). The module pins it during the rebase
        // cooldown and reads its position/heading when releasing JSBSim.
        void register_nodes(Node3D *ac_node);

        // ── Rebase cooldown ──
        // Arm the post-teleport cooldown: pin the AC at pin_pos for
        // rebase_cooldown_frames frames while terrain settles.
        void start_rebase(const Vector3 &pin_pos);
        // An airborne start (FlightSpawn::set_start): when the cooldown expires
        // the aircraft is re-initialised agl_m over the terrain at airspeed_kts
        // and heading_deg instead of on the ground. agl_m 0 = ground start.
        void set_airborne_release(double agl_m, double airspeed_kts, double heading_deg);

        // Per-frame: pin the AC while the countdown runs; on expiry, finalise the
        // ground height (with NAN retry up to terrain_init_fallback_msec), lift
        // the pin to gear clearance, reinitialise JSBSim and release slew mode.
        // terrain_height_at_ac is the scenery-stack query at the pinned position
        // (NAN when no data there); scenery_valid is whether the precise Scenery3D
        // node is alive (gates the post-reinit elevation push). p_jsb may be null
        // (skips the JSBSim calls).
        // precise_h is the PRECISE scenery elevation at the pin (NAN while the
        // tile has not streamed in yet); fallback_h the coarse globe elevation
        // (NAN if unavailable), used only once the retry window has expired —
        // releasing JSBSim on the coarse globe and then loading a tile tens of
        // metres higher or lower dropped/buried the aircraft (Emmen 2026-09-18).
        // streamer_busy: the tile streamer still has pending work — the wait
        // for the precise tile then continues (up to REBASE_HARD_CAP_MSEC);
        // the fallback timeout only counts from the last busy moment.
        void process_rebase(Object *p_jsb, double precise_h, double fallback_h, bool scenery_valid, bool streamer_busy);

        // Per-frame: push the terrain elevation under the aircraft to JSBSim's
        // ground-collision model (Godot Y + elevation_offset -> ASL). Skips until
        // terrain_init_delay_frames have elapsed, while p_jsb is null, while the
        // precise scenery node is invalid, and never pushes a NAN height.
        void update_terrain_elevation(Object *p_jsb, double terrain_height_at_ac, bool scenery_valid);

        // ── Getters (loading-progress bar + config) ──
        int get_rebase_cooldown() const;
        int get_rebase_cooldown_frames() const;
        Vector3 get_teleport_pin_pos() const;

        // Config setters (defaults = historical GDScript constants). GDScript pushes
        // the values it still owns locally (e.g. TERRAIN_INIT_FALLBACK_MSEC, which
        // also gates the deferred init handshake) so there is one source of truth.
        void set_rebase_cooldown_frames(int frames);
        void set_terrain_init_fallback_msec(double msec);
        void set_terrain_init_delay_frames(int frames);
        void set_elevation_offset(double offset);
        double get_elevation_offset() const;

    protected:
        TerrainElevation();

        // Tunables (defaults = historical GDScript constants)
        int rebase_cooldown_frames = 120;      // ~2 s at 60 fps
        double terrain_init_fallback_msec = 12000.0;
        int terrain_init_delay_frames = 60;    // ~1 s before per-frame updates

        // Godot Y -> JSBSim ASL offset (set during init; 0.0 for Scenery3D, which
        // already reports ASL).
        double elevation_offset = 0.0;

        // Rebase cooldown state
        int _rebase_cooldown = 0;
        Vector3 _teleport_pin_pos;
        double _airborne_agl_m = 0.0, _airborne_kts = 0.0, _airborne_heading = 0.0;
        double _rebase_retry_start_msec = 0.0;  // 0 = not currently retrying
        double _rebase_last_busy_msec = 0.0;
        static constexpr double REBASE_HARD_CAP_MSEC = 90000.0;

        // Per-frame terrain-push stabilisation counter (monotonic from first call)
        int terrain_init_frame_count = 0;
        // Last elevation pushed to JSBSim (NAN = none since the last release);
        // a jump of > 1 m in one frame means a tile streamed in under the AC.
        double _last_pushed_h = NAN;
        int64_t _last_lift_msec = 0;

        Node3D *ac_node = nullptr;
    };

} // namespace jsbgodot

#endif
