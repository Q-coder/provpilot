#ifndef FG_SOUNDS_H
#define FG_SOUNDS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

namespace jsbgodot
{

    // FlightGear sound set runtime (docs/A10_FLIGHTGEAR_PORT.md, subtask 8;
    // formerly fg_sounds.gd). Plays the sounds of a FlightGear <fx>
    // configuration exported by scripts/fg_a10_sounds.py (sounds.json + .wav
    // files) from the same property values the animator gets (FGBridge).
    // Per sound:
    //   mode looped     - plays while its <condition> holds
    //   mode once       - plays once when the condition becomes true
    //   mode in-transit - plays while its top-level <property> keeps changing
    //   <volume>        - product of entries: value = property (or 1), <type>
    //                     linear / log / ln / inverse / square, x factor +
    //                     offset, clamped to min / max; <internal>dt_play =
    //                     seconds playing
    //   <pitch>         - same, default 1
    // Positional sounds are played as plain 2D sounds, attenuated by the
    // camera distance (gain x reference / max(distance, reference), silent
    // beyond the max distance) when heard from outside the cockpit.
    class FGSounds : public Node
    {
        GDCLASS(FGSounds, Node);

    public:
        static void _bind_methods();
        FGSounds();

        int load(const String &json_path, const String &sounds_dir);
        void clear();
        // Per frame with the FlightGear property values of this frame.
        void update(const Dictionary &props, double delta);
        // Names of the sounds playing now (tests).
        Array playing() const;
        // How often a once-sound was triggered (tests).
        int play_count(const String &name) const;
        double volume_db_of(const String &name) const;

        double get_user_volume() const; void set_user_volume(double v);
        double get_camera_distance_m() const; void set_camera_distance_m(double v);
        int get_count() const;
        Array get_skipped() const;

    protected:
        struct Sound
        {
            Dictionary rec;
            Dictionary raw;
            AudioStreamPlayer *player = nullptr;
            String name;
            String mode;
            Variant cond;
            String prop;
            Array vol;
            Array pitch;
            double last = NAN;
            double play_t = 0.0;
            bool was_true = false;
            double moving_t = 0.0;
            int plays = 0;
        };
        static constexpr double REFERENCE_DIST_M = 60.0;
        static constexpr double MAX_DIST_M = 8000.0;

        double user_volume = 0.7;    // settings engine_volume / 100
        double camera_distance_m = 0.0; // camera to aircraft; 0 in the cockpit
        int count = 0;
        Array skipped;
        Vector<Sound> sounds;
        Dictionary props;

        static Array as_list(const Variant &v);
        static String norm(const String &p);
        static double num(const Variant &v, double def = 0.0);
        double val(const String &prop) const;
        bool cond(const Variant &c) const;
        bool compare(const String &op, const Variant &body) const;
        double factor(const Array &entries, const Sound &s, double default_value) const;
    };

} // namespace jsbgodot

#endif
