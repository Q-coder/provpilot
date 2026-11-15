#ifndef CRASH_MONITOR_H
#define CRASH_MONITOR_H

// Crash detection (docs/CRASH_DETECTION.md): watches the FDM for what an
// airframe does not survive — a touchdown far beyond the gear's sink rate,
// the airframe (not the wheels) meeting the ground at speed, ending up
// overturned, coming down on water, load factors beyond the ultimate load,
// speed well beyond the never-exceed speed — and reports the reason once.
// Independently of the report it takes the energy out of a hard bounce: the
// contact springs of a JSBSim model give back what a real airframe absorbs.

#include "aircraft_config.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

namespace jsbgodot
{
using namespace godot;

class JSBGodot;

class CrashMonitor : public RefCounted
{
    GDCLASS(CrashMonitor, RefCounted)

protected:
    static void _bind_methods();

public:
    void configure(const Ref<AircraftConfig> &cfg);
    void reset();   // a new flight: a few seconds of grace while it settles
    // Returns the reason of a crash ("" while all is well). over_water: the
    // ground under the aircraft is the sea.
    String update(Object *jsb, bool over_water, double delta);
    Dictionary get_limits() const;
    double get_last_impact_sink_mps() const;

private:
    static constexpr double GRACE_S = 6.0;
    static constexpr double ULTIMATE_FACTOR = 1.5;       // ultimate load over the limit load
    static constexpr double G_HOLD_S = 0.3;
    static constexpr double OVERSPEED_FACTOR = 1.15;
    static constexpr double OVERSPEED_HOLD_S = 3.0;
    static constexpr double BOUNCE_FROM_MPS = 3.0;       // touchdowns harder than this lose their rebound
    static constexpr double BOUNCE_RESTITUTION = 0.3;
    static constexpr double BOUNCE_WINDOW_S = 1.5;

    double g_pos = 3.8, g_neg = -1.52;   // limit loads
    double vne_kt = 0.0;                 // 0: not checked
    double touchdown_sink_mps = 6.0;     // on the wheels
    double airframe_sink_mps = 3.0;      // on anything else

    double grace = GRACE_S;
    bool was_wow = false;
    double recent_sink = 0.0;
    double last_impact_sink = 0.0;
    double bounce_timer = 0.0, bounce_limit = 0.0;
    double g_timer = 0.0, speed_timer = 0.0, water_timer = 0.0;
};

} // namespace jsbgodot

#endif
