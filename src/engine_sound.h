#ifndef ENGINE_SOUND_H
#define ENGINE_SOUND_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>

using namespace godot;

namespace jsbgodot
{

    // Engine-sound pitch/volume mapping, moved out of node_3d.gd so the
    // per-frame RPM-to-audio math runs in C++ (double precision).
    //
    // The smoothed visual RPM comes from the ControlSurfaces module (it owns
    // the engine-off decay). Each frame we normalise that RPM to 0-1 and
    // linearly interpolate the AudioStreamPlayer's pitch_scale and volume_db.
    // Below a low-RPM threshold the sound is faded to full silence. The user's
    // engine-volume setting (0-100%) is applied on top as a dB offset.
    //
    // GDScript (node_3d.gd) creates + owns the AudioStreamPlayer node, pushes
    // the per-aircraft tunables from aircraft.cfg ([sound] pitch/vol ranges and
    // [propeller] max_rpm), registers the node once, reads the user volume from
    // the settings screen, and calls update() from _process. Kept as a plain
    // RefCounted so GDScript can instantiate it with `EngineSound.new()`.
    class EngineSound : public RefCounted
    {
        GDCLASS(EngineSound, RefCounted);

    public:
        static void _bind_methods();

        // ── Node registration (session-stable; no reset needed) ──
        // audio = the looping engine AudioStreamPlayer (nullptr until created).
        void register_nodes(AudioStreamPlayer *p_audio);

        // ── Per-aircraft tunables (pushed from aircraft.cfg) ──
        // [sound] pitch_min/pitch_max and vol_db_min/vol_db_max, plus the
        // [propeller] max_rpm used to normalise RPM to 0-1.
        void set_pitch_range(double p_min, double p_max);
        void set_vol_db_range(double p_min, double p_max);
        void set_max_rpm(double p_max_rpm);

        // ── Per-frame update ──
        // Given the smoothed visual RPM and the user engine volume (0-100%),
        // compute + apply pitch_scale and volume_db to the registered player.
        // No-op without a registered audio node.
        void update(double rpm, double user_volume_pct);

    protected:
        EngineSound();

        // Tunables (defaults = historical GDScript constants / c172p cfg).
        double max_rpm = 2700.0;   // RPM normalisation reference (cfg propeller.max_rpm)
        double pitch_min = 0.7;    // pitch_scale at idle (cfg sound.pitch_min)
        double pitch_max = 1.6;    // pitch_scale at max RPM (cfg sound.pitch_max)
        double vol_db_min = -12.0; // volume_db at idle, before user offset (cfg sound.vol_db_min)
        double vol_db_max = 3.0;   // volume_db at max RPM, before user offset (cfg sound.vol_db_max)
        double fade_rpm = 50.0;    // below this RPM the sound is fully silent

        AudioStreamPlayer *audio = nullptr;
    };

} // namespace jsbgodot

#endif