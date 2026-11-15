#ifndef FLIGHT_LOG_H
#define FLIGHT_LOG_H

// The logbook (docs/LOGBOOK.md): records every flight — when and where it
// began and ended, how long, how far, how high and fast, the landings, a
// crash — into logbook.json next to provpilot.cfg, and the flight's track
// (the "black box": a sample every two seconds) into logbook/<id>.json.
// One pilot for now ("Pilot"); profiles come later.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace jsbgodot
{
using namespace godot;

class FlightLog : public RefCounted
{
    GDCLASS(FlightLog, RefCounted)

protected:
    static void _bind_methods();

public:
    FlightLog();
    ~FlightLog();

    void set_directory(const String &dir);   // where logbook.json lives (the install root)
    String get_directory() const;
    void begin(const String &aircraft_id, const String &aircraft_name, double lat, double lon, const String &place);
    // once per flown frame (not while paused)
    void sample(Object *jsb, double lat, double lon, double delta);
    void note_crash(const String &reason);
    // closes the flight; written to the logbook when it lasted MIN_FLIGHT_S
    Dictionary end(double lat, double lon, const String &place);
    bool is_recording() const;
    Dictionary current_entry() const;   // the running flight's figures so far
    Array current_samples() const;

    // the stored logbook, newest first; a stored flight's samples
    static Array read_entries(const String &dir);
    static Array read_samples(const String &dir, const String &id);
    static FlightLog *get_active();     // the recorder of the running sim (Logbook tab)

    static constexpr double MIN_FLIGHT_S = 30.0;
    static constexpr double SAMPLE_S = 2.0;
    static constexpr int MAX_SAMPLES = 5400;
    static constexpr int MAX_ENTRIES = 2000;

private:
    static FlightLog *active;
    String directory;
    bool recording = false;
    Dictionary entry;
    Array samples;            // [t s, lat, lon, alt ft, ias kt, vs fpm, g]
    double elapsed = 0.0, air_time = 0.0, sample_timer = 0.0, sample_step = SAMPLE_S;
    double distance_m = 0.0;
    double last_lat = 0.0, last_lon = 0.0;
    bool have_last = false;
    bool was_ground = true;
    double recent_sink = 0.0;
    double max_alt = -1e9, max_ias = 0.0, max_gs = 0.0, max_g = 1.0, min_g = 1.0, max_climb = 0.0, max_sink = 0.0, hardest_landing = 0.0;
    int landings = 0, takeoffs = 0;
    String crash_reason;
    double start_unix = 0.0;
};

} // namespace jsbgodot

#endif
