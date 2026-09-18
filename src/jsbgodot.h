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
        float elevator_trim = 0.0f;  // Elevator trim (-1.0 to 1.0)
        float trigger_left_value = 0.0f;   // LT trigger value for throttle decrease
        float trigger_right_value = 0.0f;  // RT trigger value for throttle increase
        bool trim_up_held = false;   // Y button held for trim up
        bool trim_down_held = false; // X button held for trim down
        double altitude_ft = 0.0;
        double heading_deg = 0.0;
        float flaps = 0.0f; // Flaps input (0.0 to 1.0)
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
        void increase_flaps();
        void decrease_flaps();
        void set_flaps(float value);  // Direct set (snaps to nearest notch)
        float get_flaps() const;      // Actual kinematic position (for wing animation)
        float get_flap_cmd() const;   // Commanded position (for lever indicator)

        // Landing gear (retractable). gear_down: true = extend (cmd 1.0),
        // false = retract (cmd 0.0). get_gear_pos returns the FDM's actual
        // 0..1 position (for drag + visual animation); get_gear_cmd the command.
        void set_gear_down(bool down);
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
        
        // Initialise JSBSim resting on the terrain (metres ASL) at the aircraft
        // node's ECEF position with a TRUE compass heading (degrees). Must be
        // called before the first physics step for proper ground collision.
        void initialize_at_terrain(double terrain_elevation_m, Vector3 godot_pos, double heading_deg);
        
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
        bool has_last_good_transform = false;
        double last_terrain_m = 0.0;
        int64_t last_nan_reset_msec = 0;

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
        std::vector<WeatherLayer> weather_layers;  // sorted by base_m
        double weather_qnh_hpa = 1013.25;
        WeatherSample sample_weather(double alt_m) const;
        void apply_weather_to_fdm();
    };

}

#endif