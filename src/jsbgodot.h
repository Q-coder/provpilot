#ifndef JSBGODOT_H
#define JSBGODOT_H

#include "FGFDMExec.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_joypad_motion.hpp>
#include <godot_cpp/classes/input_event_joypad_button.hpp>

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

        // Propeller RPM from JSBSim
        double get_propeller_rpm() const;

        // Engine running state (true if engine is running)
        bool get_engine_running() const;

        // Magneto switch (0=OFF, 1=R, 2=L, 3=BOTH, 4=START)
        void set_input_magnetos(int value);
        int get_input_magnetos() const;

        // Set terrain elevation for ground collision (meters)
        void set_terrain_elevation(double elevation_m);
        
        // Initialize aircraft at specific terrain elevation (meters) and Godot position
        // This must be called before the first physics step for proper ground collision
        void initialize_at_terrain(double terrain_elevation_m, Vector3 godot_pos, double heading_deg);
        
        // Set Godot world Y offset for terrain (transforms JSBSim altitude to Godot Y)
        // godot_y = jsbsim_altitude - terrain_elevation + godot_terrain_y
        void set_godot_terrain_y_offset(double offset_m);
        double get_godot_terrain_y_offset() const;
        
        // Check if JSBSim is fully initialized and ready
        bool is_initialized() const;
        
        // Slew mode - pauses physics and position updates
        void set_slew_mode(bool enabled);
        bool get_slew_mode() const;
        
        // Reinitialize JSBSim from current Godot position (used after slew mode).
        // terrain_elevation_m is the ground elevation under the aircraft, in meters ASL.
        // Pass NaN if unknown — the function will fall back to (altitude - 2 m).
        void reinitialize_from_godot_position(Vector3 godot_pos, double heading_rad, double terrain_elevation_m);
        
        // Reinitialize with airspeed preservation (used when exiting slew mode).
        // terrain_elevation_m is the ground elevation under the aircraft, in meters ASL.
        void reinitialize_from_godot_position_with_speed(Vector3 godot_pos, double heading_rad, double airspeed_kts, double terrain_elevation_m);
        
        // Floating origin: shift the lat_lon_alt_to_local reference by godot_shift
        // so all Godot positions shift by that amount.  Must match the terrain
        // center change to keep C++ and Swiss→Godot mappings in sync.
        void adjust_origin(Vector3 godot_shift);
        
        // Get current position data
        double get_latitude_deg() const;
        double get_longitude_deg() const;
        Vector3 get_godot_position() const;
        
    private:
        // Cache position data for getters
        double cached_latitude = 0.0;
        double cached_longitude = 0.0;
        Vector3 cached_godot_position = Vector3(0, 0, 0);
        bool jsbsim_initialized = false;  // Track if JSBSim has been properly initialized
        double pending_terrain_elevation = 0.0;  // Terrain elevation to use at initialization
        bool has_pending_terrain = false;  // Whether we have a terrain elevation to set
        double godot_terrain_y_offset = 0.0;  // Y offset to apply (Godot terrain Y - JSBSim terrain elevation)
    };

}

#endif