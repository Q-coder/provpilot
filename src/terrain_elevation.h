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

        // Per-frame: pin the AC while the countdown runs; on expiry, finalise the
        // ground height (with NAN retry up to terrain_init_fallback_msec), lift
        // the pin to gear clearance, reinitialise JSBSim and release slew mode.
        // terrain_height_at_ac is the scenery-stack query at the pinned position
        // (NAN when no data there); scenery_valid is whether the precise Scenery3D
        // node is alive (gates the post-reinit elevation push). p_jsb may be null
        // (skips the JSBSim calls).
        void process_rebase(Object *p_jsb, double terrain_height_at_ac, bool scenery_valid);

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
        double _rebase_retry_start_msec = 0.0;  // 0 = not currently retrying

        // Per-frame terrain-push stabilisation counter (monotonic from first call)
        int terrain_init_frame_count = 0;

        Node3D *ac_node = nullptr;
    };

} // namespace jsbgodot

#endif
