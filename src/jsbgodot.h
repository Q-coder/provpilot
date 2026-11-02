#ifndef JSBGODOT_H
#define JSBGODOT_H

#include "FGFDMExec.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_joypad_motion.hpp>
#include <godot_cpp/classes/input_event_joypad_button.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <vector>

using namespace godot;

namespace jsbgodot
{

    class JSBGodot : public Node3D
    {
        GDCLASS(JSBGodot, Node3D);

    private:
        JSBSim::FGFDMExec *FDMExec;
        bool do_scripted;

        float input_pitch = 0.0;
        float input_roll = 0.0;
        float input_rudder = 0.0;
        float input_throttle = 0.0;
        float input_aileron = 0.0f;  // Aileron input (-1.0 to 1.0)
        float input_elevator = 0.0f; // Elevator input (-1.0 to 1.0)
        float elevator_trim = 0.0f;  // Pitch trim command (fcs/pitch-trim-cmd-norm, -1.0 to 1.0)
        float elevator_hold = 0.0f;  // elevator the airborne trim could not move into the pitch trim
        float default_pitch_trim = 0.0f; // [jsbsim] pitch_trim_default: the trim a fresh session starts with
        double mac_le_x_in = 0.0, mac_ref_in = 0.0; // [performance] mac_le_x_in / mac_in
        double gear_transit_s = 0.0;                 // [gear] fdm_position false: retract_seconds
        double last_joy_roll = NAN, last_joy_pitch = NAN, last_joy_yaw = NAN, last_joy_throttle = NAN;
        float brake_axis = 0.0f;   // flight_brakes axis, pulled half 0..1
        bool brake_button = false; // "Hold brakes" button
        float trigger_left_value = 0.0f;   // LT trigger value for throttle decrease
        float trigger_right_value = 0.0f;  // RT trigger value for throttle increase
        bool trim_up_held = false;   // trim_up button held (set_trim_buttons)
        bool trim_down_held = false; // trim_down button held
        double altitude_ft = 0.0;
        double heading_deg = 0.0;
        float flaps = 0.0f; // Flaps input (0.0 to 1.0)
        std::vector<float> flap_detents = {0.0f, 0.333f, 0.667f, 1.0f}; // lever notches ([flaps] positions)
        // A detent that only exists while an FDM property is at or above a
        // threshold ([flaps] gated_detent / gate_property / gate_min): the
        // MD-80's DIAL detent is slats-only until the thumbwheel is set, so
        // stepping skips it while the wheel is stowed.
        int flap_gate_index = -1;
        std::string flap_gate_property;
        double flap_gate_min = 0.0;
        float input_mixture = 1.0f; // Mixture input (0.0 = cutoff, 1.0 = full rich)
        int input_magnetos = 3; // Magneto switch: 0=OFF, 1=R, 2=L, 3=BOTH, 4=START
        int init_starter_frames = 0; // Grace period: keep starter engaged after init
        // Landing gear command: 1.0 = down (default, so the aircraft spawns on
        // its wheels), 0.0 = up. Pushed to JSBSim as fcs/gear-cmd-norm; the FDM
        // drives gear/gear-pos-norm (drag + WOW). Visual retraction is animated
        // separately in GDScript from get_gear_pos().
        float gear_cmd = 1.0f;
        // Existing control input variables...
        float input_brake = 0.0f; // Brake input (0.0 to 1.0)
        // Glider mode (Settings > Aircraft > Controls): the throttle inputs
        // (keys, triggers, axis) work the airbrake lever instead: throttle 1 =
        // brakes in, 0 = fully out. The engines (none) get no command.
        bool throttle_drives_airbrakes = false;

        double airspeed_knots;
        double vertical_speed_fpm;
        bool slew_mode = false;  // When true, pause JSBSim physics and position updates

        void copy_inputs_to_JSBSim();
        void copy_outputs_from_JSBSim();

    public:
        static void _bind_methods();

        JSBGodot();
        ~JSBGodot();

        void _ready() override;
        void _process(double delta) override;
        void _input(const Ref<InputEvent> event); // Using Ref<InputEvent> requires full inclusion
        void _physics_process(const real_t delta);
        void initialise();

        // Load (or reload) the JSBSim flight model for a given aircraft.
        // `aircraft_id` selects the data tree under
        // `<install_root>/aircraft/<id>/jsbsim/{aircraft,engine,systems}` (via
        // the InstallRoot autoload); `jsbsim_model` is the model name passed to
        // FGFDMExec::LoadModel (i.e. the .xml stem). Both default to "c172p"
        // so existing callers keep the historical behaviour. Returns true on
        // success. Safe to call again at flight time to switch airframes.
        bool load_aircraft(const String &aircraft_id = "c172p", const String &jsbsim_model = "c172p");

        void set_input_pitch(float value);
        float get_input_pitch() const;

        void set_input_roll(float value);
        float get_input_roll() const;

        void set_input_rudder(float value);
        float get_input_rudder() const;

        void set_input_throttle(float value);
        float get_input_throttle() const;
        void set_throttle_drives_airbrakes(bool on);
        bool get_throttle_drives_airbrakes() const;

        void set_input_elevator(float value);
        float get_input_elevator() const;
        void set_input_aileron(float value);
        float get_input_aileron() const;

        double get_airspeed_knots() const;
        double get_vertical_speed_fpm() const;
        double get_altitude_ft() const;
        double get_heading() const;
        // Ground roll feedback for rolling-wheel visuals.
        double get_ground_speed_fps() const;  // velocities/vg-fps
        bool get_weight_on_wheels() const;     // gear/wow (any gear in contact)
        // Setter for brake input
        void set_input_brake(float value);

        // Getter for brake input (if needed)
        float get_input_brake() const;

        // Mixture control (0.0 = cutoff, 1.0 = full rich)
        void set_input_mixture(float value);
        float get_input_mixture() const;

        // Flaps control (4 discrete positions: 0, 0.333, 0.667, 1.0)
        void increase_flaps();        // next cfg detent (set_flap_detents), default thirds
        void decrease_flaps();
        void set_flap_detents(const Array &positions);
        Array get_flap_detents() const;
        void set_flap_detent_gate(int index, const String &property, double min_value);
        bool is_flap_detent_available(int index) const;
        // The detent reached from `from` by one notch in `dir` (+1 down,
        // -1 up), skipping a gated detent that is unavailable; clamped.
        int step_flap_detent(int from, int dir) const;
        void set_flaps(float value);  // Direct set (the exact lever value)
        float get_flaps() const;      // Actual kinematic position (for wing animation)
        float get_flap_cmd() const;   // Commanded position (for lever indicator)

        // Landing gear (retractable). gear_down: true = extend (cmd 1.0),
        // false = retract (cmd 0.0). get_gear_pos returns the FDM's actual
        // 0..1 position (for drag + visual animation); get_gear_cmd the command.
        void set_gear_down(bool down);
        // Nose-wheel steering range (design: A-10 NWS engage, Ctrl+W).
        // JSBSim's <max_steer> is the LIMITED range (the value in the FDM
        // XML); FULL scales the steer command by full/limited (JSBSim does
        // not clamp fcs/steer-cmd-norm). full <= limited disables the toggle.
        void set_nose_steer_range(double limited_deg, double full_deg);
        void set_nose_steer_full(bool full);
        bool get_nose_steer_full() const;
        bool get_gear_down() const;
        float get_gear_pos() const;
        float get_gear_cmd() const;

        // Propeller RPM from JSBSim
        double get_propeller_rpm() const;

        // Engine running state (true if engine is running)
        bool get_engine_running() const;

        // Magneto switch (0=OFF, 1=R, 2=L, 3=BOTH, 4=START)
        void set_input_magnetos(int value);
        int get_input_magnetos() const;

        // Set terrain elevation for ground collision (meters)
        void set_terrain_elevation(double elevation_m);
        // Terrain height (metres ASL, NaN = unknown) at a geodetic position:
        // Callable(lat_deg, lon_deg) -> float. Installed as JSBSim's ground
        // callback so every gear contact point sees the terrain under IT,
        // with the local slope normal — instead of one flat plane at the
        // CG height (aircraft stayed level on slopes and hopped when that
        // height stepped, ENGM 2026-09-20). set_terrain_elevation() remains
        // the fallback where the query has no data.
        void set_ground_query(const godot::Callable &query);
        
        // Initialise JSBSim resting on the terrain (metres ASL) at the aircraft
        // node's ECEF position with a TRUE compass heading (degrees). Must be
        // called before the first physics step for proper ground collision.
        void initialize_at_terrain(double terrain_elevation_m, Vector3 godot_pos, double heading_deg);
        // Airborne start (gliders without a winch or tow, any aircraft from the
        // Aircraft tab): level at agl_m above the terrain under godot_pos, the
        // calibrated airspeed set and the longitudinal axes trimmed (alpha,
        // elevator) so the aircraft starts in a steady glide / cruise.
        void initialize_airborne(double terrain_elevation_m, Vector3 godot_pos, double heading_deg, double agl_m, double airspeed_kts);
        // Trim pitch axis (alpha / elevator / throttle) at the current initial
        // conditions; false (and an untrimmed start) when the trim fails.
        bool trim_longitudinal();
        
        // Set Godot world Y offset for terrain (transforms JSBSim altitude to Godot Y)
        // godot_y = jsbsim_altitude - terrain_elevation + godot_terrain_y
        void set_godot_terrain_y_offset(double offset_m);
        double get_godot_terrain_y_offset() const;
        
        // Check if JSBSim is fully initialized and ready
        bool is_initialized() const;
        
        // Slew mode - pauses physics and position updates
        void set_slew_mode(bool enabled);
        bool is_slew_mode() const { return slew_mode; }
        bool get_slew_mode() const;
        
        
        // Current geodetic position (degrees) and ECEF position (metres).
        double get_latitude_deg() const;
        double get_longitude_deg() const;
        Vector3 get_godot_position() const;
        // The Godot world is WGS84 ECEF (design §9.5): JSBSim runs at the true
        // geodetic position; each tick the aircraft node gets position = ECEF
        // location and basis = body-to-ECEF DCM.
        // Diagnostic: read any JSBSim property (0.0 if the FDM is not created).
        double get_property(const String &name) const;
        // Whether the FDM's property tree holds this node (systems create theirs at load).
        bool has_property(const String &name) const;
        // Fuel tanks (Aircraft settings tab): count, capacity and contents in lb, direct fill.
        int get_tank_count() const;
        double get_tank_capacity_lbs(int index) const;
        double get_tank_contents_lbs(int index) const;
        void set_tank_contents_lbs(int index, double lbs);
        // Fuel as a share of the total capacity: wing tanks first, then the centreline ones.
        void set_fuel_percent(double pct);
        // [performance] mac_le_x_in / mac_in (cfg): the leading edge of the mean
        // chord aft of the datum and its length, for an exact % MAC (0 = estimate).
        void set_mac_reference(double le_x_in, double mac_in);
        // [gear] fdm_position false: PVP drives gear/gear-pos-norm towards the
        // command over this many seconds (0 = the FDM's own kinematic does).
        void set_gear_transit(double seconds);
        // Settings > Joystick axes, once per frame from node_3d.gd (NAN = unbound axis).
        void apply_joystick(double roll, double pitch, double yaw, double throttle, bool throttle_absolute, double brakes_axis, bool brakes_button);
        // The held trim buttons (Settings > Joystick trim_up / trim_down), polled each frame.
        void set_trim_buttons(bool up, bool down);
        // Centre of gravity: {cg_x_in, cg_y_in, cg_z_in, weight_lbs, mac_ft, aero_rp_x_in, pct_mac}
        // pct_mac estimates the aerodynamic centre at 25 % MAC (LEMAC = rp - 0.25 * MAC).
        Dictionary get_cg_info() const;
        // Generic FDM property write (e.g. fcs/speedbrake-cmd-norm from a key).
        void set_property(const String &name, double value);
        // Attitude readouts (degrees): JSBSim theta / phi.
        double get_pitch_deg() const;
        double get_roll_deg() const;
        // Fresh session: zero every pilot input (throttle, stick, pedals, trim,
        // brakes, triggers), flaps up, gear down, magnetos BOTH. Used by Fly!
        // so a new flight never inherits the previous flight's controls.
        void reset_controls();
        // Re-initialise JSBSim from the aircraft node's global transform
        // (position -> geodetic, heading from the nose axis); used after slew
        // mode and teleports.
        // airspeed_kts <= 0 = stationary and level; otherwise the current
        // pitch/roll are kept and the calibrated airspeed is set.
        void reinitialize_from_pose(Transform3D ac_global, double airspeed_kts, double terrain_elevation_m);

        // ── Weather (design doc §10) ───────────────────────────────────────
        // Static weather layers from the Settings store: an Array of
        // Dictionaries {base_m, top_m, wind_dir_deg (from), wind_speed_kts,
        // turbulence 0..3, ...}. Sampled at the aircraft's altitude every
        // physics tick and pushed into JSBSim's FGWinds (wind NED + MIL-F-8785C
        // turbulence); qnh_hpa goes to the atmosphere's sea-level pressure.
        void set_weather(const Array &layers, double qnh_hpa);
        // [jsbsim] init_properties "_links" {dst: src}: copied src -> dst every
        // physics step (the FlightGear property rules we do not run, e.g. the
        // MD-80's APU starter / cutoff into the engine controls); "_pulse"
        // {prop: seconds}: held at 1 for that much FDM time after a (re)start,
        // then 0 (FlightGear's acconfig "autoconfig-running" window).
        int get_engine_count() const;
        void set_property_links(const Dictionary &links);
        void set_init_pulses(const Dictionary &pulses);
        // [jsbsim] pitch_trim_default (cfg): the pitch trim command of a fresh
        // session (the MD-80 starts about 3 units nose-up like FlightGear's).
        void set_default_pitch_trim(double trim);
        double get_default_pitch_trim() const;
        double get_pitch_trim() const;
        // Pitch trim command set from outside (the autoflight's autotrim rule).
        void set_pitch_trim(double trim);
        // Run the FDM a number of steps at the nominal rate and put the vehicle
        // state back to the initial conditions: the control actuators (a rate
        // limited stabilizer) reach their commands while the aircraft stays put.
        void settle_fcs(int steps);
        void top_up_feed_tanks();
        void mirror_engines_to_fg();
        void apply_property_links_and_pulses(double delta);
        // Diagnostics: the sampled wind (north, east, down) in knots and the
        // turbulence level (0..3) at a geodetic altitude.
        Vector3 get_weather_wind_ned_kts(double alt_m) const;
        int get_weather_turbulence(double alt_m) const;
        double get_weather_qnh_hpa() const;
        
    private:
        // Cache position data for getters
        double cached_latitude = 0.0;
        double cached_longitude = 0.0;
        Vector3 cached_godot_position = Vector3(0, 0, 0);
        bool jsbsim_initialized = false;  // Track if JSBSim has been properly initialized
        double pending_terrain_elevation = 0.0;  // Terrain elevation to use at initialization
        bool has_pending_terrain = false;  // Whether we have a terrain elevation to set
        double godot_terrain_y_offset = 0.0;  // Y offset to apply (Godot terrain Y - JSBSim terrain elevation)
        std::string loaded_jsbsim_model;  // Model name currently loaded into FDMExec (empty = none)
        // Last finite pose handed to the node + terrain: a non-finite FDM state
        // (e.g. a gear-spring blow-up) is caught in copy_outputs_from_JSBSim and
        // the FDM re-initialised here instead of poisoning the scene.
        Transform3D last_good_transform;
        Vector3 last_good_scale = Vector3(1, 1, 1);
        bool has_last_good_transform = false;
        double last_terrain_m = 0.0;
        int64_t last_nan_reset_msec = 0;
        godot::Callable ground_query;
        double nose_steer_limited_deg = 0.0;
        double nose_steer_full_deg = 0.0;
        bool nose_steer_full = false;
        void install_ground_callback();

        struct WeatherLayer
        {
            double base_m = 0.0;
            double top_m = 0.0;
            double wind_n_kts = 0.0;  // wind vector the air moves TOWARD
            double wind_e_kts = 0.0;
            int turbulence = 0;
        };
        struct WeatherSample
        {
            double wind_n_kts = 0.0;
            double wind_e_kts = 0.0;
            int turbulence = 0;
        };
        Dictionary property_links; // dst -> src
        Dictionary init_pulses;    // prop -> seconds left (<= 0: done)
        bool links_applied_once = false;
        std::vector<WeatherLayer> weather_layers;  // sorted by base_m
        double weather_qnh_hpa = 1013.25;
        WeatherSample sample_weather(double alt_m) const;
        void apply_weather_to_fdm();
    };

}

#endif