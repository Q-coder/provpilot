#ifndef FG_BRIDGE_H
#define FG_BRIDGE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "radio_nav.h"

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
        // Properties another module owns (the autoflight's it-autoflight/*,
        // instrumentation/fma/*): merged like the cockpit state, so they reach
        // the FDM mirror and the animator.
        Dictionary get_extra_properties() const; void set_extra_properties(const Dictionary &v);
        double get_canopy_target() const; void set_canopy_target(double v);
        // The frame time the receivers integrate with (set by the rig before collect()).
        double get_frame_delta() const; void set_frame_delta(double v);
        // NAV 1 / DME / ADF receivers (docs/RADIO_NAVIGATION.md).
        Ref<RadioNav> get_radio_nav() const;
        // NAV 2 (instrumentation/nav[1]: the first officer's HSI, CRS 2).
        Ref<RadioNav> get_radio_nav2() const;
        // WMM declination at the aircraft (east positive), from the last collect().
        double get_magnetic_variation_deg() const;
        // One HUD line with the tuned stations.
        String nav_status_text() const;

        // FlightGear property mirror (docs/MD80_FLIGHTGEAR_PORT.md): aircraft
        // whose systems live in JSBSim (reading FlightGear-side paths such as
        // /controls/..., /instrumentation/..., /engines/engine[n]/n1) get the
        // bridge's values pushed into the FDM each frame, and every property
        // the model animations or sounds reference that the FDM defines is
        // pulled back. Off for the other aircraft.
        bool get_fdm_mirror() const; void set_fdm_mirror(bool on);
        // JSBSim property that powers the panel (e.g. systems/electrical/bus/dc-l), with
        // its threshold; replaces the built-in electrical model when set.
        String get_power_property() const; void set_power_property(const String &p);
        double get_power_threshold() const; void set_power_threshold(double v);
        // The nose gear's full steering angle ([jsbsim] nose_steer_max_deg): FlightGear's
        // gear/gear[0]/steering-norm is the steering angle over this maximum.
        double get_nose_steer_max_deg() const; void set_nose_steer_max_deg(double v);
        // Strut compression per gear unit from GearStruts (the wheel pinned to
        // the FDM's contact point); without it the FDM's compression over a
        // nominal oleo travel.
        void set_compression_norm(int unit, double norm);
        void clear_compression_norms();
        void set_surface_norm_per_deg(double elevator, double rudder);
        // The animator's / sound set's property names to pull from the FDM.
        void set_mirror_pull_keys(const PackedStringArray &keys);
        // The aircraft's start-up property values ([jsbsim] init_properties):
        // cockpit switches the model animates start in that state instead of
        // at the generic switch defaults.
        void set_init_defaults(const Dictionary &d);
        // FlightGear -set file aliases (path -> FDM-side target) resolved every frame.
        void set_property_aliases(const Dictionary &d);
        double default_for(const String &prop) const;
        // Start value for a cockpit control's property: the init list, else the
        // FDM's current value when mirrored, else NIL (the caller keeps its default).
        Variant initial_value(const String &prop, Object *jsb);
        void reset_mirror_cache();
        int get_mirror_pushed() const;
        int get_mirror_pulled() const;

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
        Dictionary extra_props;       // set_extra_properties: merged with the cockpit state
        double canopy_target = 0.0;   // canopy switch: 0 close, 1 hold, 2 open
        double kollsman_seeded = -1.0; // Kollsman value last taken from the weather
        Ref<RadioNav> radio_nav;      // NAV 1 + DME + ADF
        Ref<RadioNav> radio_nav2;     // NAV 2
        double frame_delta = 1.0 / 60.0;
        double magvar_deg = 0.0;      // WMM declination at the aircraft
        double magvar_lat = 999.0, magvar_lon = 999.0; // where it was computed
        double magvar_year = 0.0;
        double prev_frq_btn = 0.0;    // KR 87 FRQ button edge
        double prev_vtrue_mps = -1.0; // total-energy variometer: last true airspeed
        double te_filtered_mps = 0.0; // the variometer's own lag (about a second, as the real instrument)
        double gear_agl_offset_ft = 0.0;      // CG height above the wheels on the ground (set from the FDM at rest)
        double agl_history_t = 0.0;
        PackedVector2Array agl_history;       // (t, gear agl) over the last two seconds
        bool fdm_mirror = false;
        String power_property;
        double power_threshold = 24.0;
        double nose_steer_max_deg = 20.0;
        double compression_norm[3] = {0.0, 0.0, 0.0};
        bool compression_set[3] = {false, false, false};
        // [animation] elevator/rudder_pos_norm_per_deg: FlightGear models built
        // against YASim read surface-positions/*-pos-norm scaled past +-1
        // (the ASK 21: +-4 for full travel); 0 = the FDM's own norm
        double elevator_norm_per_deg = 0.0, rudder_norm_per_deg = 0.0;
        PackedStringArray pull_keys;
        Dictionary init_defaults;     // normalised path -> value
        Dictionary property_aliases;  // normalised path -> target path
        Dictionary node_cache;        // property path -> bool (FDM has the node)
        int mirror_pushed = 0, mirror_pulled = 0;
        bool fdm_has(JSBGodot *jsb, const String &key);
        void mirror_push(JSBGodot *jsb, Dictionary &p);
        void mirror_pull(JSBGodot *jsb, Dictionary &p);

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
        void radios(JSBGodot *jsb, Dictionary &p, double lat_deg, double lon_deg, double alt_m, double true_heading_deg);
        void update_magvar(double lat_deg, double lon_deg, double alt_m);
        double ctl(const char *key, double def) const;
    };

} // namespace jsbgodot

#endif
