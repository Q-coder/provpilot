#include "jsbgodot.h"
#include "geo_frame.h"
#include "models/FGFCS.h"
#include "models/FGAircraft.h"
#include "initialization/FGInitialCondition.h"
#include "models/FGPropulsion.h"
#include "models/propulsion/FGThruster.h"

#include <godot_cpp/classes/input_event.hpp>     // Ensure InputEvent is fully included
#include <godot_cpp/classes/input_event_key.hpp> // If you need specific event handling
#include <godot_cpp/core/memory.hpp>             // For Godot logging
#include <godot_cpp/classes/scene_tree.hpp>      // Include SceneTree header
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/utility_functions.hpp> // For UtilityFunctions::print
#include <cmath>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <cmath>
#include <exception>
#include <locale>

using namespace godot;
using namespace jsbgodot;


const float EARTH_RADIUS = 6371000.0; // Radius of Earth in meters

// Forward declarations - defined later in file
Vector3 lat_lon_alt_to_local(float latitude, float longitude, float altitude);
void reset_lat_lon_reference(double new_ref_lat_deg, double new_ref_lon_deg);
void adjust_lat_lon_reference(double godot_shift_x, double godot_shift_z);

// Shared static state for lat/lon <-> local coordinate conversion.
// Declared here so reinitialize_from_godot_position can access them.
static bool   g_reference_set = false;
static double g_ref_lat_rad = 0.0;
static double g_ref_lon_rad = 0.0;
static double g_ref_alt = 0.0;

void JSBGodot::_bind_methods()
{
    // Don't bind _process - it's a virtual method that's automatically overridden
    ClassDB::bind_method(D_METHOD("set_input_pitch", "value"), &JSBGodot::set_input_pitch);
    ClassDB::bind_method(D_METHOD("get_input_pitch"), &JSBGodot::get_input_pitch);
    ClassDB::bind_method(D_METHOD("set_input_roll", "value"), &JSBGodot::set_input_roll);
    ClassDB::bind_method(D_METHOD("get_input_roll"), &JSBGodot::get_input_roll);
    ClassDB::bind_method(D_METHOD("set_input_rudder", "value"), &JSBGodot::set_input_rudder);
    ClassDB::bind_method(D_METHOD("get_input_rudder"), &JSBGodot::get_input_rudder);
    ClassDB::bind_method(D_METHOD("set_input_throttle", "value"), &JSBGodot::set_input_throttle);
    ClassDB::bind_method(D_METHOD("get_input_throttle"), &JSBGodot::get_input_throttle);
    ClassDB::bind_method(D_METHOD("get_airspeed_knots"), &JSBGodot::get_airspeed_knots);
    ClassDB::bind_method(D_METHOD("get_vertical_speed_fpm"), &JSBGodot::get_vertical_speed_fpm);
    ClassDB::bind_method(D_METHOD("get_altitude_ft"), &JSBGodot::get_altitude_ft);
    ClassDB::bind_method(D_METHOD("get_heading"), &JSBGodot::get_heading);
    ClassDB::bind_method(D_METHOD("get_ground_speed_fps"), &JSBGodot::get_ground_speed_fps);
    ClassDB::bind_method(D_METHOD("get_weight_on_wheels"), &JSBGodot::get_weight_on_wheels);
    ClassDB::bind_method(D_METHOD("set_input_brake", "value"), &JSBGodot::set_input_brake);
    ClassDB::bind_method(D_METHOD("get_input_brake"), &JSBGodot::get_input_brake);
    ClassDB::bind_method(D_METHOD("set_input_mixture", "value"), &JSBGodot::set_input_mixture);
    ClassDB::bind_method(D_METHOD("get_input_mixture"), &JSBGodot::get_input_mixture);
    ClassDB::bind_method(D_METHOD("get_input_aileron"), &JSBGodot::get_input_aileron);
    ClassDB::bind_method(D_METHOD("get_input_elevator"), &JSBGodot::get_input_elevator);
    ClassDB::bind_method(D_METHOD("increase_flaps"), &JSBGodot::increase_flaps);
    ClassDB::bind_method(D_METHOD("decrease_flaps"), &JSBGodot::decrease_flaps);
    ClassDB::bind_method(D_METHOD("set_flaps", "value"), &JSBGodot::set_flaps);
    ClassDB::bind_method(D_METHOD("get_flaps"), &JSBGodot::get_flaps);
    ClassDB::bind_method(D_METHOD("get_flap_cmd"), &JSBGodot::get_flap_cmd);
    ClassDB::bind_method(D_METHOD("set_gear_down", "down"), &JSBGodot::set_gear_down);
    ClassDB::bind_method(D_METHOD("get_gear_down"), &JSBGodot::get_gear_down);
    ClassDB::bind_method(D_METHOD("get_gear_pos"), &JSBGodot::get_gear_pos);
    ClassDB::bind_method(D_METHOD("get_gear_cmd"), &JSBGodot::get_gear_cmd);
    ClassDB::bind_method(D_METHOD("get_propeller_rpm"), &JSBGodot::get_propeller_rpm);
    ClassDB::bind_method(D_METHOD("get_engine_running"), &JSBGodot::get_engine_running);
    ClassDB::bind_method(D_METHOD("set_input_magnetos", "value"), &JSBGodot::set_input_magnetos);
    ClassDB::bind_method(D_METHOD("get_input_magnetos"), &JSBGodot::get_input_magnetos);
    ClassDB::bind_method(D_METHOD("set_terrain_elevation", "elevation_m"), &JSBGodot::set_terrain_elevation);
    ClassDB::bind_method(D_METHOD("initialize_at_terrain", "terrain_elevation_m", "godot_pos", "heading_deg"), &JSBGodot::initialize_at_terrain);
    ClassDB::bind_method(D_METHOD("load_aircraft", "aircraft_id", "jsbsim_model"), &JSBGodot::load_aircraft, DEFVAL("c172p"), DEFVAL("c172p"));
    ClassDB::bind_method(D_METHOD("is_initialized"), &JSBGodot::is_initialized);
    ClassDB::bind_method(D_METHOD("set_slew_mode", "enabled"), &JSBGodot::set_slew_mode);
    ClassDB::bind_method(D_METHOD("get_slew_mode"), &JSBGodot::get_slew_mode);
    ClassDB::bind_method(D_METHOD("reinitialize_from_godot_position", "godot_pos", "heading_rad", "terrain_elevation_m"), &JSBGodot::reinitialize_from_godot_position);
    ClassDB::bind_method(D_METHOD("reinitialize_from_godot_position_with_speed", "godot_pos", "heading_rad", "airspeed_kts", "terrain_elevation_m"), &JSBGodot::reinitialize_from_godot_position_with_speed);
    ClassDB::bind_method(D_METHOD("adjust_origin", "godot_shift"), &JSBGodot::adjust_origin);
    ClassDB::bind_method(D_METHOD("set_godot_terrain_y_offset", "offset_m"), &JSBGodot::set_godot_terrain_y_offset);
    ClassDB::bind_method(D_METHOD("get_godot_terrain_y_offset"), &JSBGodot::get_godot_terrain_y_offset);
    ClassDB::bind_method(D_METHOD("get_latitude_deg"), &JSBGodot::get_latitude_deg);
    ClassDB::bind_method(D_METHOD("get_longitude_deg"), &JSBGodot::get_longitude_deg);
    ClassDB::bind_method(D_METHOD("get_godot_position"), &JSBGodot::get_godot_position);
    ClassDB::bind_method(D_METHOD("set_ecef_frame", "ecef"), &JSBGodot::set_ecef_frame);
    ClassDB::bind_method(D_METHOD("get_pitch_deg"), &JSBGodot::get_pitch_deg);
    ClassDB::bind_method(D_METHOD("get_roll_deg"), &JSBGodot::get_roll_deg);
    ClassDB::bind_method(D_METHOD("get_ecef_frame"), &JSBGodot::get_ecef_frame);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ecef_frame"), "set_ecef_frame", "get_ecef_frame");
    ClassDB::bind_method(D_METHOD("reinitialize_from_pose", "ac_global", "airspeed_kts", "terrain_elevation_m"), &JSBGodot::reinitialize_from_pose);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "input_pitch"), "set_input_pitch", "get_input_pitch");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "input_roll"), "set_input_roll", "get_input_roll");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "input_rudder"), "set_input_rudder", "get_input_rudder");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "input_throttle"), "set_input_throttle", "get_input_throttle");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "airspeed_knots", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "", "get_airspeed_knots");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertical_speed_fpm", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "", "get_vertical_speed_fpm");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "altitude_ft", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "", "get_altitude_ft");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heading", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "", "get_heading");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "input_brake"), "set_input_brake", "get_input_brake");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "input_mixture"), "set_input_mixture", "get_input_mixture");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "propeller_rpm", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "", "get_propeller_rpm");
}

JSBGodot::JSBGodot()
{
    FDMExec = nullptr;
}

JSBGodot::~JSBGodot()
{
    if (FDMExec)
    {
        delete FDMExec;
    }
}

void JSBGodot::set_input_pitch(float value)
{
    if (value < -1.0f)
    {
        input_pitch = -1.0f;
    }
    else if (value > 1.0f)
    {
        input_pitch = 1.0f;
    }
    else
    {
        input_pitch = value;
    }
}

float JSBGodot::get_input_pitch() const
{
    return input_pitch;
}

float JSBGodot::get_input_roll() const
{
    return input_roll;
}

void JSBGodot::set_input_roll(float value)
{
    if (value < -1.0f)
    {
        input_roll = -1.0f;
    }
    else if (value > 1.0f)
    {
        input_roll = 1.0f;
    }
    else
    {
        input_roll = value;
    }
}

float JSBGodot::get_input_rudder() const
{
    return input_rudder;
}

void JSBGodot::set_input_rudder(float value)
{
    if (value < -1.0f)
    {
        input_rudder = -1.0f;
    }
    else if (value > 1.0f)
    {
        input_rudder = 1.0f;
    }
    else
    {
        input_rudder = value;
    }
}

float JSBGodot::get_input_throttle() const
{
    return input_throttle;
}

void JSBGodot::set_input_throttle(float value)
{
    if (value < 0.0f)
    {
        input_throttle = 0.0f;
    }
    else if (value > 1.0f)
    {
        input_throttle = 1.0f;
    }
    else
    {
        input_throttle = value;
    }
}

void JSBGodot::set_input_aileron(float value)
{
    // input_aileron = CLAMP(value, -1.0f, 1.0f);
    if (value < -1.0f)
    {
        input_aileron = -1.0f;
    }
    else if (value > 1.0f)
    {
        input_aileron = 1.0f;
    }
    else
    {
        input_aileron = value;
    }
}

float JSBGodot::get_input_aileron() const
{
    return input_aileron;
}

void JSBGodot::set_input_elevator(float value)
{
    // input_elevator = CLAMP(value, -1.0f, 1.0f);
    if (value < -1.0f)
    {
        input_elevator = -1.0f;
    }
    else if (value > 1.0f)
    {
        input_elevator = 1.0f;
    }
    else
    {
        input_elevator = value;
    }
}

float JSBGodot::get_input_elevator() const
{
    return input_elevator;
}

void JSBGodot::increase_flaps()
{
    // C172p has 4 flap positions: 0, 10, 20, 30 degrees
    // Kinematic input: 0.0, 0.333, 0.667, 1.0
    if (flaps < 0.1f) {
        flaps = 0.333f;  // 10 degrees
    } else if (flaps < 0.4f) {
        flaps = 0.667f;  // 20 degrees
    } else if (flaps < 0.7f) {
        flaps = 1.0f;    // 30 degrees
    }
    // Already at max, do nothing
}

void JSBGodot::decrease_flaps()
{
    // C172p has 4 flap positions: 0, 10, 20, 30 degrees
    if (flaps > 0.9f) {
        flaps = 0.667f;  // 20 degrees
    } else if (flaps > 0.5f) {
        flaps = 0.333f;  // 10 degrees
    } else if (flaps > 0.1f) {
        flaps = 0.0f;    // 0 degrees
    }
    // Already at min, do nothing
}

void JSBGodot::set_flaps(float value)
{
    // Snap to nearest valid notch: 0, 0.333, 0.667, 1.0
    if (value < 0.167f) {
        flaps = 0.0f;
    } else if (value < 0.5f) {
        flaps = 0.333f;
    } else if (value < 0.833f) {
        flaps = 0.667f;
    } else {
        flaps = 1.0f;
    }
}

float JSBGodot::get_flaps() const
{
    // Return actual flap position from JSBSim (not command) for wing animation
    if (FDMExec) {
        return FDMExec->GetPropertyValue("fcs/flap-pos-norm");
    }
    return flaps;
}

float JSBGodot::get_flap_cmd() const
{
    // Return commanded flap position (instant, for lever indicator)
    return flaps;
}

void JSBGodot::set_gear_down(bool down)
{
    gear_cmd = down ? 1.0f : 0.0f;
}

bool JSBGodot::get_gear_down() const
{
    return gear_cmd > 0.5f;
}

float JSBGodot::get_gear_pos() const
{
    // Actual gear position from the FDM (0=up, 1=down). Reflects retract dynamics
    // and is what the drag function uses. Falls back to the command if no FDM.
    if (FDMExec) {
        return FDMExec->GetPropertyValue("gear/gear-pos-norm");
    }
    return gear_cmd;
}

float JSBGodot::get_gear_cmd() const
{
    return gear_cmd;
}

double JSBGodot::get_propeller_rpm() const
{
    // Return propeller RPM from JSBSim
    if (FDMExec) {
        return FDMExec->GetPropertyValue("propulsion/engine[0]/propeller-rpm");
    }
    return 0.0;
}

bool JSBGodot::get_engine_running() const
{
    if (FDMExec) {
        return FDMExec->GetPropertyValue("propulsion/engine[0]/set-running") > 0.5;
    }
    return false;
}

void JSBGodot::set_input_magnetos(int value)
{
    input_magnetos = value < 0 ? 0 : (value > 4 ? 4 : value);
}

int JSBGodot::get_input_magnetos() const
{
    return input_magnetos;
}

void JSBGodot::set_terrain_elevation(double elevation_m)
{
    // Set the terrain elevation in JSBSim (convert meters to feet)
    if (FDMExec && FDMExec->GetPropagate() && FDMExec->GetPropertyManager()) {
        double elevation_ft = elevation_m * 3.28084;  // meters to feet
        // Check if property exists before setting
        if (FDMExec->GetPropertyManager()->HasNode("position/terrain-elevation-asl-ft")) {
            FDMExec->SetPropertyValue("position/terrain-elevation-asl-ft", elevation_ft);
        }
    }
}

void JSBGodot::initialize_at_terrain(double terrain_elevation_m, Vector3 godot_pos, double heading_deg_param)
{
    // Re-initialize JSBSim with correct terrain elevation and starting position
    // This must be called after terrain is known but before physics runs
    if (!FDMExec) {
        UtilityFunctions::print("Cannot initialize_at_terrain: FDMExec not created");
        return;
    }
    
    // CRITICAL: Force-initialize lat_lon_alt_to_local's static reference point
    // to (0,0,0) BEFORE copy_outputs_from_JSBSim ever calls it.
    // Without this, the first call sets the reference to the aircraft's starting
    // lat/lon, making d_lat=0, d_lon=0 → position always maps back to (0, Y, 0).
    lat_lon_alt_to_local(0.0f, 0.0f, 0.0f);
    
    double terrain_ft = terrain_elevation_m * 3.28084;  // meters to feet
    // C172P CG-above-wheels distance from c172p.xml: CG z=36.5", main gear
    // z=-15.5" → 52" = 4.33 ft. Spawn the aircraft right at its rest height
    // so the gear springs don't have to oscillate to settle.
    double gear_height_ft = 4.33;
    double aircraft_altitude_ft = terrain_ft + gear_height_ft;

    if (ecef_frame)
    {
        // ECEF world: godot_pos is the aircraft's ECEF position, heading is a
        // TRUE compass heading. JSBSim runs at the real geodetic location.
        const geo::Geodetic g = geo::geodetic_of(godot_pos);
        UtilityFunctions::print("Initializing JSBSim (ECEF) at lat=", g.lat, " lon=", g.lon,
            " terrain=", terrain_elevation_m, " m, heading=", heading_deg_param);
        std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
        ic->SetTerrainElevationFtIC(terrain_ft);
        ic->SetAltitudeASLFtIC(aircraft_altitude_ft);
        ic->SetGeodLatitudeDegIC(g.lat);
        ic->SetLongitudeDegIC(g.lon);
        ic->SetThetaDegIC(0.0);
        ic->SetPhiDegIC(0.0);
        ic->SetPsiDegIC(heading_deg_param);
        ic->SetUBodyFpsIC(0.0);
        ic->SetVBodyFpsIC(0.0);
        ic->SetWBodyFpsIC(0.0);
        FDMExec->RunIC();
        FDMExec->SetPropertyValue("propulsion/set-running", -1);
        FDMExec->SetPropertyValue("fcs/brake-cmd-norm", 0.0);
        FDMExec->SetPropertyValue("fcs/parking-brake-cmd-norm", 0.0);
        FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
        jsbsim_initialized = true;
        UtilityFunctions::print("JSBSim initialized (ECEF) successfully");
        return;
    }
    
    // Convert Godot XZ position to lat/lon offsets
    // lat_lon_alt_to_local uses ref at (0,0), so we reverse it:
    //   north = d_lat * R  =>  d_lat = Z / R   (Z = north)
    //   east  = d_lon * R  =>  d_lon = X / R   (X = east, cos(0)=1)
    const double R = 6378137.0;
    double d_lat_rad = godot_pos.z / R;
    double d_lon_rad = godot_pos.x / R;
    double lat_deg = Math::rad_to_deg(d_lat_rad);
    double lon_deg = Math::rad_to_deg(d_lon_rad);
    
    UtilityFunctions::print("Initializing JSBSim at terrain elevation:");
    UtilityFunctions::print("  Terrain: ", terrain_elevation_m, "m (", terrain_ft, " ft)");
    UtilityFunctions::print("  Aircraft altitude: ", aircraft_altitude_ft, " ft ASL");
    UtilityFunctions::print("  Godot pos: X=", godot_pos.x, " Y=", godot_pos.y, " Z=", godot_pos.z);
    UtilityFunctions::print("  Lat/Lon: ", lat_deg, ", ", lon_deg);
    UtilityFunctions::print("  Heading: ", heading_deg_param, " deg");
    
    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
    
    // Set terrain elevation FIRST
    ic->SetTerrainElevationFtIC(terrain_ft);
    
    // Set aircraft altitude above terrain
    ic->SetAltitudeASLFtIC(aircraft_altitude_ft);
    
    // Set initial position from Godot coordinates
    ic->SetLatitudeDegIC(lat_deg);
    ic->SetLongitudeDegIC(lon_deg);
    
    // Set initial orientation
    // heading_deg_param is the desired VISUAL heading (e.g. 250° = WSW)
    // Godot's rotation convention: visual_heading = (360 - psi) mod 360
    // So JSBSim psi = (360 - visual_heading) mod 360
    double jsbsim_psi = fmod(360.0 - heading_deg_param + 360.0, 360.0);
    ic->SetThetaDegIC(0.0);  // Pitch
    ic->SetPhiDegIC(0.0);    // Roll
    ic->SetPsiDegIC(jsbsim_psi);  // Yaw (converted from visual heading)
    
    // Set initial velocities (stationary on ground)
    ic->SetUBodyFpsIC(0.0);
    ic->SetVBodyFpsIC(0.0);
    ic->SetWBodyFpsIC(0.0);
    
    // Apply initial conditions
    FDMExec->RunIC();
    
    // Properly initialize engine(s): propulsion/set-running = -1 runs InitRunning
    // on EVERY engine (sets RPM, mixture, magnetos, Running=true). A value >= 0
    // would only start that one engine — wrong for multi-engine aircraft (A-10).
    // Note: propulsion/engine[0]/set-running only sets the Running flag without RPM init!
    FDMExec->SetPropertyValue("propulsion/set-running", -1);
    // Release brakes
    FDMExec->SetPropertyValue("fcs/brake-cmd-norm", 0.0);
    FDMExec->SetPropertyValue("fcs/parking-brake-cmd-norm", 0.0);
    // Override mixture to current user setting (InitRunning sets it to PressureRatio*1.3)
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
    
    jsbsim_initialized = true;
    
    UtilityFunctions::print("JSBSim re-initialized at terrain elevation successfully!");
}

bool JSBGodot::is_initialized() const
{
    return jsbsim_initialized;
}

void JSBGodot::set_slew_mode(bool enabled)
{
    slew_mode = enabled;
    printf("JSBGodot: Slew mode %s\n", enabled ? "ENABLED" : "DISABLED");
}

bool JSBGodot::get_slew_mode() const
{
    return slew_mode;
}

void JSBGodot::reinitialize_from_godot_position(Vector3 godot_pos, double heading_rad, double terrain_elevation_m)
{
    if (!FDMExec)
    {
        UtilityFunctions::print("Cannot reinitialize: FDMExec not created");
        return;
    }
    
    // Godot Y is altitude in meters (since terrain is at real ASL elevations)
    double altitude_m = godot_pos.y;
    double altitude_ft = altitude_m * 3.28084;
    
    // Use the supplied terrain elevation (queried from Scenery3D in GDScript).
    // Fall back to (altitude - 2 m) only if caller passed NaN.
    double terrain_m = std::isnan(terrain_elevation_m) ? (altitude_m - 2.0) : terrain_elevation_m;
    double terrain_ft = terrain_m * 3.28084;
    
    UtilityFunctions::print("Reinitializing JSBSim from Godot position:");
    UtilityFunctions::print("  Godot pos: X=", godot_pos.x, " Y=", godot_pos.y, " Z=", godot_pos.z);
    UtilityFunctions::print("  Altitude: ", altitude_m, "m (", altitude_ft, " ft)");
    UtilityFunctions::print("  Heading: ", Math::rad_to_deg(heading_rad), " deg");
    
    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
    
    // Convert Godot XZ back to lat/lon using the SAME reference as
    // lat_lon_alt_to_local (exact inverse of that function).
    //   godot_x = (lon - ref_lon) * R * cos(ref_lat)   => lon = ref_lon + godot_x / (R * cos(ref_lat))
    //   godot_z = (lat - ref_lat) * R                   => lat = ref_lat + godot_z / R
    const double R = 6378137.0;
    double cos_ref = cos(g_ref_lat_rad);
    if (cos_ref < 1e-10) cos_ref = 1e-10;  // safety
    
    double new_lat_rad = g_ref_lat_rad + godot_pos.z / R;
    double new_lon_rad = g_ref_lon_rad + godot_pos.x / (R * cos_ref);
    
    double new_lat_deg = Math::rad_to_deg(new_lat_rad);
    double new_lon_deg = Math::rad_to_deg(new_lon_rad);
    
    UtilityFunctions::print("  New lat/lon: ", new_lat_deg, ", ", new_lon_deg);
    
    // Set terrain elevation
    ic->SetTerrainElevationFtIC(terrain_ft);
    
    // Set aircraft position
    ic->SetLatitudeDegIC(new_lat_deg);
    ic->SetLongitudeDegIC(new_lon_deg);
    ic->SetAltitudeASLFtIC(altitude_ft);
    
    // Set orientation - keep level, set heading from aircraft rotation
    // rotation.y = psi directly, so pass through
    ic->SetThetaDegIC(0.0);  // Pitch = 0
    ic->SetPhiDegIC(0.0);    // Roll = 0
    ic->SetPsiDegIC(Math::rad_to_deg(heading_rad));  // Yaw = heading
    
    // Set initial velocities (stationary)
    ic->SetUBodyFpsIC(0.0);
    ic->SetVBodyFpsIC(0.0);
    ic->SetWBodyFpsIC(0.0);
    
    // Apply initial conditions
    FDMExec->RunIC();
    
    // Start ALL engines (-1); >= 0 would start only that one engine.
    FDMExec->SetPropertyValue("propulsion/set-running", -1);
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
    
    UtilityFunctions::print("JSBSim reinitialized from Godot position successfully!");
}

void JSBGodot::reinitialize_from_godot_position_with_speed(Vector3 godot_pos, double heading_rad, double airspeed_kts, double terrain_elevation_m)
{
    if (!FDMExec)
    {
        UtilityFunctions::print("Cannot reinitialize: FDMExec not created");
        return;
    }
    
    double altitude_m = godot_pos.y;
    double altitude_ft = altitude_m * 3.28084;
    double terrain_m = std::isnan(terrain_elevation_m) ? (altitude_m - 2.0) : terrain_elevation_m;
    double terrain_ft = terrain_m * 3.28084;
    
    UtilityFunctions::print("Reinitializing JSBSim with speed:");
    UtilityFunctions::print("  Godot pos: X=", godot_pos.x, " Y=", godot_pos.y, " Z=", godot_pos.z);
    UtilityFunctions::print("  Heading: ", Math::rad_to_deg(heading_rad), " deg");
    UtilityFunctions::print("  Airspeed: ", airspeed_kts, " kts");
    
    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
    
    const double R = 6378137.0;
    double cos_ref = cos(g_ref_lat_rad);
    if (cos_ref < 1e-10) cos_ref = 1e-10;
    
    double new_lat_rad = g_ref_lat_rad + godot_pos.z / R;
    double new_lon_rad = g_ref_lon_rad + godot_pos.x / (R * cos_ref);
    
    ic->SetTerrainElevationFtIC(terrain_ft);
    ic->SetLatitudeDegIC(Math::rad_to_deg(new_lat_rad));
    ic->SetLongitudeDegIC(Math::rad_to_deg(new_lon_rad));
    ic->SetAltitudeASLFtIC(altitude_ft);
    
    // Preserve pitch/roll from current state, set heading
    double current_pitch = FDMExec->GetPropagate()->GetEuler(2);
    double current_roll = FDMExec->GetPropagate()->GetEuler(1);
    ic->SetThetaDegIC(Math::rad_to_deg(current_pitch));
    ic->SetPhiDegIC(Math::rad_to_deg(current_roll));
    ic->SetPsiDegIC(Math::rad_to_deg(heading_rad));
    
    // Set calibrated airspeed instead of zero body velocities
    ic->SetVcalibratedKtsIC(airspeed_kts);
    
    FDMExec->RunIC();
    
    // Start ALL engines (-1); >= 0 would start only that one engine.
    FDMExec->SetPropertyValue("propulsion/set-running", -1);
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
    
    UtilityFunctions::print("JSBSim reinitialized with speed successfully!");
}

void JSBGodot::set_godot_terrain_y_offset(double offset_m)
{
    godot_terrain_y_offset = offset_m;
    printf("JSBGodot: Godot terrain Y offset set to %f m\n", offset_m);
}

double JSBGodot::get_godot_terrain_y_offset() const
{
    return godot_terrain_y_offset;
}

void JSBGodot::set_input_brake(float value)
{
    input_brake = CLAMP(value, 0.0f, 1.0f);
}

float JSBGodot::get_input_brake() const
{
    return input_brake;
}

void JSBGodot::set_input_mixture(float value)
{
    input_mixture = CLAMP(value, 0.0f, 1.0f);
}

float JSBGodot::get_input_mixture() const
{
    return input_mixture;
}

double JSBGodot::get_airspeed_knots() const
{
    return airspeed_knots;
}

double JSBGodot::get_vertical_speed_fpm() const
{
    return vertical_speed_fpm;
}

double JSBGodot::get_altitude_ft() const
{
    return altitude_ft;
}

double JSBGodot::get_heading() const
{
    return heading_deg;
}

double JSBGodot::get_ground_speed_fps() const
{
    // Ground speed (horizontal track speed) in ft/s. Drives wheel spin rate;
    // unlike airspeed it stays sensible in wind and is ~0 when parked.
    if (FDMExec) {
        return FDMExec->GetPropertyValue("velocities/vg-fps");
    }
    return 0.0;
}

bool JSBGodot::get_weight_on_wheels() const
{
    // Aggregate gear/wow: nonzero when ANY gear unit is in ground contact.
    // Goes false once airborne, so the visuals stop spinning the wheels.
    if (FDMExec) {
        return FDMExec->GetPropertyValue("gear/wow") > 0.5;
    }
    return false;
}

double JSBGodot::get_latitude_deg() const
{
    return cached_latitude;
}

double JSBGodot::get_longitude_deg() const
{
    return cached_longitude;
}

Vector3 JSBGodot::get_godot_position() const
{
    return cached_godot_position;
}

void JSBGodot::set_ecef_frame(bool p_ecef)
{
    ecef_frame = p_ecef;
}

bool JSBGodot::get_ecef_frame() const
{
    return ecef_frame;
}

double JSBGodot::get_pitch_deg() const
{
    return FDMExec ? Math::rad_to_deg(FDMExec->GetPropagate()->GetEuler(2)) : 0.0;
}

double JSBGodot::get_roll_deg() const
{
    return FDMExec ? Math::rad_to_deg(FDMExec->GetPropagate()->GetEuler(1)) : 0.0;
}

void JSBGodot::reinitialize_from_pose(Transform3D ac_global, double airspeed_kts, double terrain_elevation_m)
{
    if (!FDMExec)
    {
        UtilityFunctions::print("Cannot reinitialize: FDMExec not created");
        return;
    }
    const geo::Geodetic g = geo::geodetic_of(ac_global.origin);
    const double heading = geo::true_heading_deg(ac_global.basis, g.lat, g.lon);
    const double altitude_ft = g.h * 3.28084;
    const double terrain_m = std::isnan(terrain_elevation_m) ? (g.h - 2.0) : terrain_elevation_m;

    UtilityFunctions::print("Reinitializing JSBSim (ECEF) at lat=", g.lat, " lon=", g.lon, " h=", g.h,
        " m, heading=", heading, " airspeed=", airspeed_kts, " kts, terrain=", terrain_m, " m");

    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
    ic->SetTerrainElevationFtIC(terrain_m * 3.28084);
    ic->SetGeodLatitudeDegIC(g.lat);
    ic->SetLongitudeDegIC(g.lon);
    ic->SetAltitudeASLFtIC(altitude_ft);
    if (airspeed_kts > 0.0)
    {
        const double current_pitch = FDMExec->GetPropagate()->GetEuler(2);
        const double current_roll = FDMExec->GetPropagate()->GetEuler(1);
        ic->SetThetaDegIC(Math::rad_to_deg(current_pitch));
        ic->SetPhiDegIC(Math::rad_to_deg(current_roll));
        ic->SetPsiDegIC(heading);
        ic->SetVcalibratedKtsIC(airspeed_kts);
    }
    else
    {
        ic->SetThetaDegIC(0.0);
        ic->SetPhiDegIC(0.0);
        ic->SetPsiDegIC(heading);
        ic->SetUBodyFpsIC(0.0);
        ic->SetVBodyFpsIC(0.0);
        ic->SetWBodyFpsIC(0.0);
    }
    FDMExec->RunIC();
    FDMExec->SetPropertyValue("propulsion/set-running", -1);
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
    jsbsim_initialized = true;
}

void JSBGodot::adjust_origin(Vector3 godot_shift)
{
    // Floating origin: shift the lat/lon→local reference so that all future
    // Godot positions shift by godot_shift.  This keeps C++ positions in sync
    // with the Swiss→Godot terrain mapping after a terrain-center change.
    
    adjust_lat_lon_reference(godot_shift.x, godot_shift.z);
    
    UtilityFunctions::print("adjust_origin: shifted ref by X=", godot_shift.x, " Z=", godot_shift.z);
}

void JSBGodot::_input(const Ref<InputEvent> event)
{
    if (event->is_class("InputEventKey"))
    {
        Ref<InputEventKey> key_event = event;
        if (key_event->is_pressed())
        {
            // Handle key press event
            int keycode = key_event->get_keycode();
            // Check if the 'O' or 'P' keys are pressed
            if (keycode == KEY_O)
            {
                // Decrease throttle
                set_input_throttle(input_throttle - 0.05f); // Decrease by 0.1
                copy_outputs_from_JSBSim();
                printf("Throttle decreased to %f\n", input_throttle);
            }
            else if (keycode == KEY_P)
            {
                // Increase throttle
                set_input_throttle(input_throttle + 0.05f); // Increase by 0.1
                copy_outputs_from_JSBSim();
                printf("Throttle increased to %f\n", input_throttle);
            }
            else if (keycode == KEY_W)
            {
                // Increase Elevator
                set_input_elevator(input_elevator + 0.05f); // Increase by 0.1
                copy_outputs_from_JSBSim();
                printf("Pitch increased to %f\n", input_pitch);
            }
            else if (keycode == KEY_S)
            {
                // Decrease Elevator
                set_input_elevator(input_elevator - 0.05f); // Decrease by 0.1
                copy_outputs_from_JSBSim();
                printf("Pitch decreased to %f\n", input_pitch);
            }
            else if (keycode == KEY_D)
            {
                // Increase Aileron
                set_input_aileron(input_aileron - 0.05f); // Decrease by 0.1
                copy_outputs_from_JSBSim();
                printf("Roll decreased to %f\n", input_roll);
            }
            else if (keycode == KEY_A)
            {
                // Decrease Aileron
                set_input_aileron(input_aileron + 0.05f); // Increase by 0.1
                copy_outputs_from_JSBSim();
                printf("Roll increased to %f\n", input_roll);
            }
            else if (keycode == KEY_X)
            {
                // Right Rudder
                set_input_rudder(input_rudder + 0.05f); // Increase by 0.1
            }
            else if (keycode == KEY_Y)
            {
                // Left Rudder
                set_input_rudder(input_rudder - 0.05f); // Decrease by 0.1
            }
            if (keycode == KEY_R)
            {
                // Start process
                set_process(true);
                printf("Process started\n");
            }
            if (keycode == KEY_E)
            {
                // Stop process
                set_process(false);
                printf("Process ended\n");
            }
            if (keycode == KEY_PERIOD)
            {
                set_input_brake(1.0f);
                printf("Brakes applied\n");
            }
            else if (keycode == KEY_COMMA)
            {
                set_input_brake(0.0f);
                printf("Brakes released\n");
            }
        }
    }
    // Gamepad input handling
    else if (event->is_class("InputEventJoypadMotion"))
    {
        Ref<InputEventJoypadMotion> joypad_event = event;
        int axis = joypad_event->get_axis();
        float value = joypad_event->get_axis_value();
        
        // Left stick: Aileron (horizontal) and Elevator (vertical)
        if (axis == JOY_AXIS_LEFT_X)
        {
            set_input_aileron(-value); // Left = positive, Right = negative
        }
        else if (axis == JOY_AXIS_LEFT_Y)
        {
            set_input_elevator(-value); // Up = positive, Down = negative
        }
        // Right stick: Rudder (horizontal only)
        else if (axis == JOY_AXIS_RIGHT_X)
        {
            set_input_rudder(value);
        }
        // Triggers for throttle control - store values for continuous processing
        else if (axis == JOY_AXIS_TRIGGER_RIGHT) // RT - Increase throttle
        {
            trigger_right_value = value;
        }
        else if (axis == JOY_AXIS_TRIGGER_LEFT) // LT - Decrease throttle  
        {
            trigger_left_value = value;
        }
    }
    else if (event->is_class("InputEventJoypadButton"))
    {
        Ref<InputEventJoypadButton> button_event = event;
        int button = button_event->get_button_index();
        bool pressed = button_event->is_pressed();
        
        // Track trim button held states
        if (button == JOY_BUTTON_Y) // Y button - Trim nose up
        {
            trim_up_held = pressed;
        }
        else if (button == JOY_BUTTON_X) // X button - Trim nose down
        {
            trim_down_held = pressed;
        }
        
        // Handle button press events (single action)
        if (pressed)
        {
            if (button == JOY_BUTTON_A) // A button
            {
                increase_flaps();
            }
            else if (button == JOY_BUTTON_B) // B button
            {
                decrease_flaps();
            }
            else if (button == JOY_BUTTON_START) // Start
            {
                set_process(true);
                printf("Process started (gamepad)\n");
            }
            else if (button == JOY_BUTTON_BACK) // Back/Select
            {
                set_process(false);
                printf("Process ended (gamepad)\n");
            }
        }
    }
}

void JSBGodot::_process(double delta)
{
    //    if (FDMExec) {
    //        copy_inputs_to_JSBSim();  // Apply inputs to JSBSim
    //        bool result = FDMExec->Run();  // Advance the simulation by one time step
    //        if (result) {
    //            double sim_time = FDMExec->GetSimTime();
    //            printf("Simulation time: %f\n", sim_time);
    //        } else {
    //            printf("FDMExec->Run() failed.\n");
    //        }
    //        copy_outputs_from_JSBSim();  // Update the position and rotation in Godot
    //    }
}

void JSBGodot::_physics_process(const real_t delta)
{
    //    Your physics logic
    if (!Engine::get_singleton()->is_editor_hint())
    {
        // Don't run physics until initialize_at_terrain() has been called
        if (!jsbsim_initialized) {
            return;
        }
        
        // Skip physics simulation entirely in slew mode
        if (slew_mode) {
            return;
        }
        
        // Apply continuous throttle from triggers
        if (trigger_right_value > 0.1f) {
            set_input_throttle(input_throttle + trigger_right_value * 0.005f);
        }
        if (trigger_left_value > 0.1f) {
            set_input_throttle(input_throttle - trigger_left_value * 0.005f);
        }
        
        // Apply continuous trim from held buttons
        if (trim_up_held) {
            elevator_trim += 0.001f;
            if (elevator_trim > 1.0f) elevator_trim = 1.0f;
        }
        if (trim_down_held) {
            elevator_trim -= 0.001f;
            if (elevator_trim < -1.0f) elevator_trim = -1.0f;
        }
        
        copy_inputs_to_JSBSim();
        FDMExec->Run();
        copy_outputs_from_JSBSim();
    }
}

void JSBGodot::_ready()
{
    UtilityFunctions::print("Initializing JSBGodot...");

    if (!FDMExec)
    {
        FDMExec = new JSBSim::FGFDMExec();
        if (FDMExec)
        {
            // Suppress JSBSim debug output to avoid per-frame console spam
            FDMExec->SetDebugLevel(0);
            UtilityFunctions::print("FGFDMExec instance created successfully.");
        }
        else
        {
            UtilityFunctions::print("Failed to create FGFDMExec instance!");
            return; // Prevent further execution if creation fails
        }
    }

    // Load the default airframe so the FDM is in a valid state immediately.
    // GDScript (node_3d.gd) may call load_aircraft() again at flight time to
    // switch to the airframe the user actually selected on the welcome screen.
    if (!load_aircraft("c172p", "c172p"))
    {
        return; // Early exit if model loading fails
    }

    // Initialize the simulation
    initialise();

    // Enable physics processing so _physics_process will be called
    set_physics_process(true);
    UtilityFunctions::print("JSBGodot ready complete - physics enabled!");
}

bool JSBGodot::load_aircraft(const String &aircraft_id, const String &jsbsim_model)
{
    if (!FDMExec)
    {
        UtilityFunctions::print("load_aircraft: FDMExec not created.");
        return false;
    }

    std::string model_name = jsbsim_model.utf8().get_data();

    // If the requested model is already loaded, do nothing. Re-running
    // LoadModel on an FDM that has already been initialised and stepped
    // leaves it in an inconsistent state and crashes on the next
    // initialize_at_terrain() call.
    if (!loaded_jsbsim_model.empty() && loaded_jsbsim_model == model_name)
    {
        UtilityFunctions::print("JSBSim model '", jsbsim_model, "' already loaded — skipping reload.");
        return true;
    }

    // Switching airframes: rebuild FDMExec from scratch. Calling LoadModel a
    // second time on an FGFDMExec that already holds a model does NOT fully
    // reset the propulsion/property state — stale FGPropertyValue pointers
    // from the previous model survive and crash inside RunIC() (observed as
    // a SIGSEGV in FGTurboProp::Calculate dereferencing a dangling property).
    // A fresh FGFDMExec guarantees a clean property tree for the new model.
    bool switching_models = !loaded_jsbsim_model.empty();
    if (switching_models)
    {
        delete FDMExec;
        FDMExec = new JSBSim::FGFDMExec();
        FDMExec->SetDebugLevel(0);
        loaded_jsbsim_model.clear();
    }

    // Compute JSBSim root via the InstallRoot autoload. Aircraft data lives
    // under `<install_root>/aircraft/<id>/jsbsim/{aircraft,engine,systems}`.
    // In the editor, install_root resolves to the project directory, so the
    // path points at the curated source-of-truth tree shipped in the repo.
    // In an exported build, the AircraftInstaller autoload has already
    // extracted the bundled PCK resources to that location on first run.
    std::string jsbsim_root;
    String chosen;
    SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
    if (tree && tree->get_root()) {
        Node *install_root = tree->get_root()->get_node_or_null(NodePath("InstallRoot"));
        if (install_root) {
            Variant v = install_root->call("get_aircraft_dir", aircraft_id);
            String dir = v;
            if (!dir.is_empty()) {
                chosen = dir.path_join("jsbsim");
            }
        }
    }
    if (chosen.is_empty()) {
        UtilityFunctions::print("JSBSim root: InstallRoot autoload not available");
    } else if (DirAccess::dir_exists_absolute(chosen)) {
        UtilityFunctions::print("JSBSim root: ", chosen);
    } else {
        UtilityFunctions::print("JSBSim root (missing, tried: ", chosen, ")");
    }
    // Capture the UTF-8 path through a NAMED CharString. Assigning directly
    // from `chosen.utf8().get_data()` relies on the temporary CharString
    // surviving the assignment; under godot-cpp 4.7 that produced an empty
    // std::string here (while the log above still printed `chosen`
    // correctly), leaving RootDir empty so JSBSim tried to open the relative
    // path "aircraft/c172p/c172p.xml" and failed.
    CharString jsbsim_root_cs = chosen.utf8();
    jsbsim_root = jsbsim_root_cs.get_data();
    UtilityFunctions::print("JSBSim resolved root path: '", String(jsbsim_root.c_str()), "'");
    FDMExec->SetRootDir(SGPath(jsbsim_root));
    FDMExec->SetAircraftPath(SGPath(jsbsim_root + "/aircraft"));
    FDMExec->SetEnginePath(SGPath(jsbsim_root + "/engine"));
    FDMExec->SetSystemsPath(SGPath(jsbsim_root + "/systems"));
    UtilityFunctions::print("JSBSim directories set.");

    // JSBSim parses the aircraft/engine/systems XML numbers through C++
    // iostreams, which honour the global C++ locale. Godot 4.7 sets the
    // process-wide locale to the user's system locale; on comma-decimal
    // locales (e.g. de_CH/de_DE) "0.5" parses as 0 and LoadModel silently
    // fails. Force the classic (C) locale so numeric parsing always uses a
    // '.' decimal separator. Godot's own number formatting is
    // locale-independent, so this is safe to leave set.
    std::locale::global(std::locale::classic());

    // Pass ABSOLUTE aircraft/engine/systems paths to LoadModel. JSBSim's
    // GetFullPath() only prepends RootDir to RELATIVE paths; absolute paths
    // are used verbatim, so this resolves correctly regardless of the
    // process working directory or RootDir state.
    bool was_loaded = switching_models;
    if (FDMExec->LoadModel(SGPath(jsbsim_root + "/aircraft"),
                           SGPath(jsbsim_root + "/engine"),
                           SGPath(jsbsim_root + "/systems"),
                           model_name))
    {
        UtilityFunctions::print("JSBSim model '", jsbsim_model, "' loaded successfully.");
        loaded_jsbsim_model = model_name;
        // When switching airframes after the initial _ready() load, re-seed
        // the initial conditions so the FDM is in a valid state before the
        // next initialize_at_terrain() call.
        if (was_loaded)
        {
            initialise();
        }
        return true;
    }

    UtilityFunctions::print("Failed to load JSBSim model '", jsbsim_model, "'.");
    return false;
}

void JSBGodot::initialise()
{
    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();

    // Set initial position
    ic->SetLatitudeDegIC(0.0);
    ic->SetLongitudeDegIC(0.0);
    // Don't set altitude here - will be set by initialize_at_terrain() with terrain info

    // Set initial orientation
    ic->SetThetaDegIC(0.0); // Pitch
    ic->SetPhiDegIC(0.0);   // Roll
    ic->SetPsiDegIC(0.0);   // Yaw

    // Set initial velocities
    ic->SetUBodyFpsIC(0.0);
    ic->SetVBodyFpsIC(0.0);
    ic->SetWBodyFpsIC(0.0);
    
    // DON'T call RunIC() here - wait for initialize_at_terrain() to be called
    // This allows terrain elevation to be set before first physics frame
    jsbsim_initialized = false;
}

void JSBGodot::copy_inputs_to_JSBSim()
{
    if (!FDMExec)
    {
        printf("FDMExec is not initialized!\n");
        return;
    }

    // Retrieve the FCS and check for null before proceeding
    std::shared_ptr<JSBSim::FGFCS> FCS = FDMExec->GetFCS();
    if (!FCS)
    {
        printf("FCS object is null!\n");
        return;
    }

    // Apply input commands to the FCS
    //    FCS->SetDeCmd(input_pitch);
    //    FCS->SetDaCmd(input_roll);
    //    FCS->SetDrCmd(input_rudder);
    //    FCS->SetThrottleCmd(-1, input_throttle);
    //    printf("Inputs copied to JSBSim FCS. Throttle: %f\n", input_throttle);

    // Set control surface deflections via the property tree
    // The legacy flat frame ran JSBSim in a mirrored world (see
    // initialize_at_terrain), and every input path plus the control-surface
    // animations were tuned against that mirror. In the ECEF world the
    // aircraft is no longer mirrored, so the lateral commands are flipped
    // here, at the FDM boundary only, keeping input_* (and the animations
    // that read them) exactly as before. Pitch is unaffected by the mirror.
    const float lateral_sign = ecef_frame ? -1.0f : 1.0f;
    FDMExec->SetPropertyValue("fcs/aileron-cmd-norm", lateral_sign * input_aileron);
    FDMExec->SetPropertyValue("fcs/elevator-cmd-norm", input_elevator + elevator_trim);
    FDMExec->SetPropertyValue("fcs/rudder-cmd-norm", lateral_sign * input_rudder);
    // Throttle ALL engines (-1). Setting the bare "fcs/throttle-cmd-norm" property
    // only drives engine 0, leaving the other engine(s) at idle/zero on a twin
    // like the A-10 (→ asymmetric thrust + yaw). SetThrottleCmd(-1) fans out to
    // every engine; for single-engine aircraft it is identical to before.
    FCS->SetThrottleCmd(-1, input_throttle);

    // Set nose wheel steering (linked to rudder, but opposite direction for ground steering)
    FDMExec->SetPropertyValue("fcs/steer-cmd-norm", -input_rudder);

    // Set the brake command
    FDMExec->SetPropertyValue("fcs/left-brake-cmd-norm", input_brake);
    FDMExec->SetPropertyValue("fcs/right-brake-cmd-norm", input_brake);

    // Set flaps command
    FDMExec->SetPropertyValue("fcs/flap-cmd-norm", flaps);

    // Set landing gear command (1=down, 0=up). Aircraft without retractable
    // gear simply ignore this property; for retractable gear the FDM drives
    // gear/gear-pos-norm (drag + WOW) from it.
    FDMExec->SetPropertyValue("gear/gear-cmd-norm", gear_cmd);

    // Set mixture command
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);

    // Set magneto and starter commands from switch position
    // Switch: 0=OFF, 1=R, 2=L, 3=BOTH, 4=START
    // JSBSim magneto_cmd: 0=off, 1=left, 2=right, 3=both
    static const int magneto_map[] = {0, 2, 1, 3, 3};
    FDMExec->SetPropertyValue("propulsion/magneto_cmd", magneto_map[input_magnetos]);
    // Keep starter engaged during init grace period or when user selects START
    if (init_starter_frames > 0) {
        FDMExec->SetPropertyValue("propulsion/starter_cmd", 1.0);
        init_starter_frames--;
    } else {
        FDMExec->SetPropertyValue("propulsion/starter_cmd", input_magnetos == 4 ? 1.0 : 0.0);
    }


}

Vector3 lat_lon_alt_to_cartesian(float latitude, float longitude, float altitude)
{
    float lat_rad = Math::deg_to_rad(latitude);
    float lon_rad = Math::deg_to_rad(longitude);

    // Convert latitude, longitude, and altitude to Cartesian (ECEF)
    float x = (EARTH_RADIUS + altitude) * cos(lat_rad) * cos(lon_rad);
    float y = (EARTH_RADIUS + altitude) * cos(lat_rad) * sin(lon_rad);
    float z = (EARTH_RADIUS + altitude) * sin(lat_rad);

    return Vector3(x, y, z);
}

Vector3 lat_lon_alt_to_local(float latitude, float longitude, float altitude)
{
    double lat_rad = Math::deg_to_rad(latitude);
    double lon_rad = Math::deg_to_rad(longitude);

    if (!g_reference_set)
    {
        g_ref_lat_rad = lat_rad;
        g_ref_lon_rad = lon_rad;
        g_ref_alt = altitude;
        g_reference_set = true;
    }

    double d_lat = lat_rad - g_ref_lat_rad;
    double d_lon = lon_rad - g_ref_lon_rad;
    double d_alt = altitude - g_ref_alt;

    const double R = 6378137.0; // Earth's radius in meters

    double north = d_lat * R;
    double east = d_lon * R * cos(g_ref_lat_rad);
    double up = d_alt;

    // Map to Godot's coordinate system (X-East, Y-Up, Z-North)
    return Vector3(east, up, north);
}

/// Reset the lat/lon→local reference point to new coordinates (degrees).
/// After this call, lat_lon_alt_to_local(new_lat, new_lon, 0) will return (0, 0, 0).
void reset_lat_lon_reference(double new_ref_lat_deg, double new_ref_lon_deg)
{
    g_ref_lat_rad = Math::deg_to_rad(new_ref_lat_deg);
    g_ref_lon_rad = Math::deg_to_rad(new_ref_lon_deg);
    g_ref_alt = 0.0;
    g_reference_set = true;
}

/// Shift the lat/lon→local reference so that all Godot positions change by
/// (godot_shift_x, 0, godot_shift_z).  Derivation:
///   east_new  = (lon - ref_lon_new) * R * cos(ref_lat)
///             = east_old + shift_x
///   => ref_lon_new = ref_lon_old - shift_x / (R * cos(ref_lat))
///   north_new = (lat - ref_lat_new) * R = north_old + shift_z
///   => ref_lat_new = ref_lat_old - shift_z / R
void adjust_lat_lon_reference(double godot_shift_x, double godot_shift_z)
{
    const double R = 6378137.0;
    double cos_ref = cos(g_ref_lat_rad);
    if (cos_ref < 1e-10) cos_ref = 1e-10;  // safety
    g_ref_lon_rad -= godot_shift_x / (R * cos_ref);
    g_ref_lat_rad -= godot_shift_z / R;
}

void JSBGodot::copy_outputs_from_JSBSim()
{
    // Ensure FDMExec and Propagate are valid
    if (!FDMExec)
    {
        printf("FDMExec is null!\n");
        return;
    }
    auto Propagate = FDMExec->GetPropagate();
    if (!Propagate)
    {
        printf("Propagate is null!\n");
        return;
    }

    // Get aircraft position in geographic coordinates
    double latitude = Propagate->GetLatitudeDeg();
    double longitude = Propagate->GetLongitudeDeg();
    altitude_ft = Propagate->GetAltitudeASL();
    
    // Cache lat/lon for getter methods
    cached_latitude = latitude;
    cached_longitude = longitude;

    // Convert altitude from feet to meters
    double altitude_m = altitude_ft * 0.3048;

    // Print the geographic coordinates
    // printf("Latitude: %f, Longitude: %f, Altitude: %f ft\n", latitude, longitude, altitude_ft);

    if (ecef_frame)
    {
        // ECEF world: position straight from JSBSim's ECEF location, basis
        // from the body-to-ECEF DCM. Body axes (X fwd, Y right, Z down) map
        // onto the aircraft node's (X = -right, Y = up, Z = fwd); the node's
        // scale is preserved. No fake location, no heading mirror.
        const JSBSim::FGLocation &loc = Propagate->GetLocation();
        Vector3 pos((double)loc(1) * 0.3048, (double)loc(2) * 0.3048, (double)loc(3) * 0.3048);
        const JSBSim::FGMatrix33 &m = Propagate->GetTb2ec();
        const Vector3 bx((double)m(1, 1), (double)m(2, 1), (double)m(3, 1));
        const Vector3 by((double)m(1, 2), (double)m(2, 2), (double)m(3, 2));
        const Vector3 bz((double)m(1, 3), (double)m(2, 3), (double)m(3, 3));
        cached_latitude = Propagate->GetGeodLatitudeDeg();
        cached_longitude = longitude;
        // Wheel/CG clearance offset along local up.
        pos += (-bz).normalized() * 0.0 + geo::wun_basis(cached_latitude, cached_longitude).get_column(1) * godot_terrain_y_offset;
        cached_godot_position = pos;
        Node3D *ac = Object::cast_to<Node3D>(get_parent());
        if (!ac)
        {
            printf("Parent node is null!\n");
            return;
        }
        if (!slew_mode)
        {
            const Vector3 scale = ac->get_scale();
            Basis b;
            b.set_column(0, -by * scale.x);
            b.set_column(1, -bz * scale.y);
            b.set_column(2, bx * scale.z);
            ac->set_global_transform(Transform3D(b, pos));
        }
        vertical_speed_fpm = -FDMExec->GetPropertyValue("velocities/v-down-fps") * 60.0;
        airspeed_knots = FDMExec->GetPropertyValue("velocities/vc-kts");
        heading_deg = FDMExec->GetPropertyValue("attitude/psi-deg"); // true heading
        return;
    }

    // Get local position in meters
    Vector3 local_position = lat_lon_alt_to_local(latitude, longitude, altitude_m);
    
    // For terrain imported with offset 0, Godot Y = altitude in meters
    // godot_terrain_y_offset is a small adjustment (e.g. 0.5m) to lift wheels above ground
    local_position.y = altitude_m + godot_terrain_y_offset;
    
    // Cache Godot position for getter
    cached_godot_position = local_position;

    // Get Euler angles (in radians) from JSBSim
    double bank = Propagate->GetEuler(1);    // Roll
    double pitch = Propagate->GetEuler(2);   // Pitch
    double heading = Propagate->GetEuler(3); // Yaw

    // Map Euler angles to Godot's coordinate system
    // rotation.y = psi (heading CW from North=+Z), display = rad_to_deg(rotation.y)
    Vector3 newRot = Vector3(-pitch, heading, -bank);

    // Get the parent node
    Node3D *parent_node = Object::cast_to<Node3D>(get_parent());
    if (!parent_node)
    {
        printf("Parent node is null!\n");
        return;
    }

    // In slew mode, don't update position/rotation - let GDScript control it
    if (!slew_mode)
    {
        // Update the position and rotation of the parent node
        parent_node->set_position(local_position);
        parent_node->set_rotation(newRot);
    }

    // Access airspeed from the property tree
    double tas_knots = FDMExec->GetPropertyValue("aero/qbar-psf");
    double ias_knots = FDMExec->GetPropertyValue("velocities/vc-kts");

    // Access vertical speed (v-down-fps is positive downwards)
    double vertical_speed_fps = -FDMExec->GetPropertyValue("velocities/v-down-fps");
    vertical_speed_fpm = vertical_speed_fps * 60.0;

    double engine_thrust = FDMExec->GetPropulsion()->GetEngine(0)->GetThruster()->GetThrust();

    airspeed_knots = FDMExec->GetPropertyValue("velocities/vc-kts");

    // Get the heading in degrees from the property tree
    // Convert JSBSim psi to visual heading: visual = (360 - psi) mod 360
    heading_deg = FDMExec->GetPropertyValue("attitude/psi-deg");
    heading_deg = fmod(360.0 - heading_deg + 360.0, 360.0);
}