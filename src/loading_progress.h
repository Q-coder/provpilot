#ifndef LOADING_PROGRESS_H
#define LOADING_PROGRESS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    // Loading-screen progress math and completion state machine, moved out of
    // node_3d.gd so the blending logic + timeout safety net runs in C++.
    //
    // GDScript (node_3d.gd) keeps the scenery ring sampling (it reports loaded/total)
    // and drives the loading-screen UI. It calls update() once per frame with the
    // sample counts, flags, and rebase cooldown state; this module blends the
    // scenery fraction with the post-teleport rebase-cooldown fraction into one bar
    // value, applies a hard timeout so nodata territory cannot trap the user on the
    // loading screen forever, decides when the screen may be dismissed, and produces
    // the status text shown on the bar.
    class LoadingProgress : public RefCounted
    {
        GDCLASS(LoadingProgress, RefCounted);

    public:
        static void _bind_methods();

        // Record Time::get_singleton()->get_ticks_msec() as the safety-net start time.
        void begin_fly_loading();

        // Per-frame: blend scenery fraction with rebase-cooldown fraction into a
        // progress value in [0,1]. Caches the status text and completion flag.
        // loaded/total are the ring sample counts from GDScript; is_fly_loading gates
        // the cooldown blend + timeout safety net; terrain_init_pending blocks
        // completion while the deferred init handshake is outstanding; rebase_cooldown
        // / rebase_cooldown_frames drive the p_cooldown term.
        double update(int loaded, int total, bool is_fly_loading, bool terrain_init_pending, int rebase_cooldown, int rebase_cooldown_frames);

        // Cached outputs from the last update() call.
        String get_status_text() const;
        bool is_complete() const;
        // Progress of the current phase alone, 0..1, restarting at 0 with each
        // phase (scenery tiles → flight-model init → stabilizing): the bar the
        // intro HUD shows while fly-loading. The init phase has no measurable
        // progress; it creeps with time (12 s ≈ the precise-tile wait window)
        // and caps at 0.95.
        double get_phase_progress() const;
        int get_phase() const;   // PHASE_*
        enum Phase { PHASE_SCENERY = 0, PHASE_FDM_INIT = 1, PHASE_STABILIZE = 2, PHASE_READY = 3 };

        // Config setters (defaults = historical GDScript constants). GDScript pushes
        // the values it still owns locally so there is one source of truth.
        void set_scenery_weight(double weight);
        void set_cooldown_weight(double weight);
        void set_fly_loading_max_msec(double msec);
        void set_fly_loading_nodata_msec(double msec);

    protected:
        LoadingProgress();

        // Tunables (defaults = historical GDScript constants)
        double scenery_weight = 0.7;
        double cooldown_weight = 0.3;
        double fly_loading_max_msec = 45000.0;   // Hard cap on fly loading screen (45s)
        double fly_loading_nodata_msec = 15000.0; // Give up faster when zero tiles load (15s - must exceed TERRAIN_INIT_FALLBACK_MSEC)

        // Safety-net start timestamp (ms); 0 means not armed
        double _fly_loading_start_msec = 0.0;

        // Cached per-frame outputs
        double _progress = 0.0;
        double _phase_progress = 0.0;
        int _phase = PHASE_SCENERY;
        double _phase_start_msec = 0.0;
        String _status_text;
        bool _complete = false;
    };

} // namespace jsbgodot

#endif
