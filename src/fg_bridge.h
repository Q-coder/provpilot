#ifndef FG_BRIDGE_H
#define FG_BRIDGE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    class JSBGodot;

    // FlightGear property bridge (docs/A10_FLIGHTGEAR_PORT.md, D2; formerly
    // fg_bridge.gd). The FlightGear model animations and sounds read
    // FlightGear property paths; collect() gathers their values every frame
    // from JSBSim (properties the FDM has), from the host (lights, view,
    // canopy, time of day) and from simple derived rules (electrical bus,
    // instruments, fuel display), and the FGAnimator / FGSounds consume the
    // dictionary. Whatever is not here evaluates to 0 in the animator and
    // shows up in FGAnimator.unknown_properties().
    //
    // Cockpit switch / knob values (FGControls.state) are merged last so the
    // positions the pilot set persist and their animations follow.
    class FGBridge : public RefCounted
    {
        GDCLASS(FGBridge, RefCounted);

    public:
        static void _bind_methods();
        FGBridge();

        // Defaults for properties only the cockpit switches / knobs write
        // (seeded into FGControls.state once by the rig).
        static Dictionary switch_defaults();
        static double switch_default(const String &prop);

        // Values for this frame. jsb may be null before the FDM runs;
        // airborne = no weight on wheels; night 0..1 drives the panel lighting.
        Dictionary collect(Object *jsb, bool airborne, double night = 0.0);
        // Canopy motion from the sprung switch (2 open / 0 close / 1 hold),
        // ~10 s full travel.
        void advance_canopy(double delta);

        double get_canopy_norm() const; void set_canopy_norm(double v);
        bool get_landing_light() const; void set_landing_light(bool v);
        bool get_taxi_light() const; void set_taxi_light(bool v);
        bool get_cockpit_view() const; void set_cockpit_view(bool v);
        double get_dc_volts() const; void set_dc_volts(double v);
        double get_ac_volts() const; void set_ac_volts(double v);
        double get_g_max() const; void set_g_max(double v);
        double get_g_min() const; void set_g_min(double v);
        double get_sim_time_seconds() const; void set_sim_time_seconds(double v);
        Dictionary get_control_state() const; void set_control_state(const Dictionary &v);
        double get_canopy_target() const; void set_canopy_target(double v);

    protected:
        double canopy_norm = 0.0;     // 0 closed .. 1 open
        bool landing_light = false;   // from AircraftLights (L key)
        bool taxi_light = false;
        bool cockpit_view = true;     // FlightGear hides the outer canopy glass inside
        double dc_volts = 28.0;       // DC bus from battery / generators (0 when everything is off)
        double ac_volts = 115.0;
        double g_max = 1.0;           // accelerometer memory needles
        double g_min = 1.0;
        double sim_time_seconds = -1.0; // simulated time of day (s); < 0 = system UTC
        Dictionary control_state;     // FGControls.state, merged last
        double canopy_target = 0.0;   // canopy switch: 0 close, 1 hold, 2 open
        double kollsman_seeded = -1.0; // Kollsman value last taken from the weather

        static constexpr int GEAR_UNITS = 3; // 0 nose, 1 left main, 2 right main
        static constexpr int ENGINES = 2;
        static constexpr int TANKS = 3;
        static constexpr double FUEL_LBS_PER_GAL = 6.4;
        static constexpr double TF34_MIL_THRUST_LBF = 9065.0;

        void electrical(Dictionary &p, JSBGodot *jsb);
        void systems_healthy(Dictionary &p);
        void avionics_idle(Dictionary &p);
        void surfaces(JSBGodot *jsb, Dictionary &p);
        void gear(JSBGodot *jsb, Dictionary &p, bool airborne);
        void engines(JSBGodot *jsb, Dictionary &p);
        void fuel(JSBGodot *jsb, Dictionary &p);
        void flight_instruments(JSBGodot *jsb, Dictionary &p, bool airborne);
        void controls(JSBGodot *jsb, Dictionary &p);
        double ctl(const char *key, double def) const;
    };

} // namespace jsbgodot

#endif
