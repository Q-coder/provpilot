#include "key_bindings.h"
#include "jsbgodot.h"
#include "geo_frame.h"
#include "models/FGFCS.h"
#include "models/FGAircraft.h"
#include "initialization/FGInitialCondition.h"
#include "models/FGPropulsion.h"
#include "models/propulsion/FGTank.h"
#include "models/propulsion/FGThruster.h"
#include "models/FGAtmosphere.h"
#include "models/atmosphere/FGWinds.h"
#include "models/FGInertial.h"
#include "input_output/FGGroundCallback.h"
#include "math/FGLocation.h"
#include <cmath>
#include <algorithm>

#include <godot_cpp/classes/input_event.hpp>     // Ensure InputEvent is fully included
#include <godot_cpp/classes/input_event_key.hpp> // If you need specific event handling
#include <godot_cpp/core/memory.hpp>             // For Godot logging
#include <godot_cpp/classes/scene_tree.hpp>      // Include SceneTree header
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
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





// Ground model backed by the scenery: height under each contact point from
// the host's terrain query (Scenery3D elevation + drawn-surface lift), the
// normal from finite differences 4 m north/east. Falls back to the flat
// terrain elevation JSBSim was last given where the query has no data.
static constexpr double FDM_TARGET_HZ = 240.0;

class GodotGroundCallback : public JSBSim::FGGroundCallback
{
public:
    GodotGroundCallback(const godot::Callable &q, double semimajor, double semiminor)
        : query(q), a(semimajor), b(semiminor) {}

    double GetAGLevel(double t, const JSBSim::FGLocation &location, JSBSim::FGLocation &contact,
                      JSBSim::FGColumnVector3 &normal, JSBSim::FGColumnVector3 &v,
                      JSBSim::FGColumnVector3 &w) const override
    {
        // Like FGDefaultGroundCallback: the contact-point location handed in
        // may carry no ellipsoid, so geodetic values are taken on a copy
        // with the planet's ellipse set (without this the geodetic altitude
        // was garbage and the gear model blew up, 2026-09-20).
        JSBSim::FGLocation loc = location;
        loc.SetEllipse(a, b);
        const double lat = loc.GetGeodLatitudeDeg();
        const double lon = loc.GetLongitudeDeg();
        double h_ft = terrain_ft;
        double dhdn = 0.0, dhde = 0.0;
        if (query.is_valid())
        {
            const double h = (double)query.call(lat, lon);
            if (std::isfinite(h))
            {
                h_ft = h * 3.28084;
                const double d = 4.0;
                const double dlat = d / 111320.0;
                const double dlon = d / (111320.0 * std::max(0.05, std::cos(lat * M_PI / 180.0)));
                const double hn = (double)query.call(lat + dlat, lon);
                const double he = (double)query.call(lat, lon + dlon);
                if (std::isfinite(hn) && std::isfinite(he))
                {
                    dhdn = (hn - h) / d;
                    dhde = (he - h) / d;
                }
            }
        }
        contact.SetEllipse(a, b);
        contact.SetPositionGeodetic(loc.GetLongitude(), loc.GetGeodLatitudeRad(), h_ft);
        JSBSim::FGColumnVector3 n_ned(-dhdn, -dhde, -1.0);   // NED: up is -z
        n_ned.Normalize();
        normal = loc.GetTl2ec() * n_ned;
        v.InitMatrix();
        w.InitMatrix();
        return loc.GetGeodAltitude() - h_ft;
    }

    void SetTerrainElevation(double h) override { terrain_ft = h; }
    void SetEllipse(double semimajor, double semiminor) override { a = semimajor; b = semiminor; }

private:
    godot::Callable query;
    double a, b;
    double terrain_ft = 0.0;
};

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
    ClassDB::bind_method(D_METHOD("set_input_aileron", "value"), &JSBGodot::set_input_aileron);
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
    ClassDB::bind_method(D_METHOD("set_ground_query", "query"), &JSBGodot::set_ground_query);
    ClassDB::bind_method(D_METHOD("set_nose_steer_range", "limited_deg", "full_deg"), &JSBGodot::set_nose_steer_range);
    ClassDB::bind_method(D_METHOD("set_nose_steer_full", "full"), &JSBGodot::set_nose_steer_full);
    ClassDB::bind_method(D_METHOD("get_nose_steer_full"), &JSBGodot::get_nose_steer_full);
    ClassDB::bind_method(D_METHOD("initialize_at_terrain", "terrain_elevation_m", "godot_pos", "heading_deg"), &JSBGodot::initialize_at_terrain);
    ClassDB::bind_method(D_METHOD("load_aircraft", "aircraft_id", "jsbsim_model"), &JSBGodot::load_aircraft, DEFVAL("c172p"), DEFVAL("c172p"));
    ClassDB::bind_method(D_METHOD("is_initialized"), &JSBGodot::is_initialized);
    ClassDB::bind_method(D_METHOD("set_slew_mode", "enabled"), &JSBGodot::set_slew_mode);
    ClassDB::bind_method(D_METHOD("is_slew_mode"), &JSBGodot::is_slew_mode);
    ClassDB::bind_method(D_METHOD("get_slew_mode"), &JSBGodot::get_slew_mode);
    ClassDB::bind_method(D_METHOD("set_godot_terrain_y_offset", "offset_m"), &JSBGodot::set_godot_terrain_y_offset);
    ClassDB::bind_method(D_METHOD("get_godot_terrain_y_offset"), &JSBGodot::get_godot_terrain_y_offset);
    ClassDB::bind_method(D_METHOD("get_latitude_deg"), &JSBGodot::get_latitude_deg);
    ClassDB::bind_method(D_METHOD("get_longitude_deg"), &JSBGodot::get_longitude_deg);
    ClassDB::bind_method(D_METHOD("get_godot_position"), &JSBGodot::get_godot_position);
    ClassDB::bind_method(D_METHOD("get_property", "name"), &JSBGodot::get_property);
    ClassDB::bind_method(D_METHOD("set_property", "name", "value"), &JSBGodot::set_property);
    ClassDB::bind_method(D_METHOD("get_tank_count"), &JSBGodot::get_tank_count);
    ClassDB::bind_method(D_METHOD("get_tank_capacity_lbs", "index"), &JSBGodot::get_tank_capacity_lbs);
    ClassDB::bind_method(D_METHOD("get_tank_contents_lbs", "index"), &JSBGodot::get_tank_contents_lbs);
    ClassDB::bind_method(D_METHOD("set_tank_contents_lbs", "index", "lbs"), &JSBGodot::set_tank_contents_lbs);
    ClassDB::bind_method(D_METHOD("get_cg_info"), &JSBGodot::get_cg_info);
    ClassDB::bind_method(D_METHOD("get_pitch_deg"), &JSBGodot::get_pitch_deg);
    ClassDB::bind_method(D_METHOD("get_roll_deg"), &JSBGodot::get_roll_deg);
    ClassDB::bind_method(D_METHOD("reinitialize_from_pose", "ac_global", "airspeed_kts", "terrain_elevation_m"), &JSBGodot::reinitialize_from_pose);
    ClassDB::bind_method(D_METHOD("reset_controls"), &JSBGodot::reset_controls);
    ClassDB::bind_method(D_METHOD("set_weather", "layers", "qnh_hpa"), &JSBGodot::set_weather);
    ClassDB::bind_method(D_METHOD("get_weather_wind_ned_kts", "alt_m"), &JSBGodot::get_weather_wind_ned_kts);
    ClassDB::bind_method(D_METHOD("get_weather_turbulence", "alt_m"), &JSBGodot::get_weather_turbulence);
    ClassDB::bind_method(D_METHOD("get_weather_qnh_hpa"), &JSBGodot::get_weather_qnh_hpa);

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

void JSBGodot::set_nose_steer_range(double limited_deg, double full_deg)
{
    nose_steer_limited_deg = limited_deg;
    nose_steer_full_deg = full_deg;
}

void JSBGodot::set_nose_steer_full(bool full)
{
    nose_steer_full = full;
}

bool JSBGodot::get_nose_steer_full() const
{
    return nose_steer_full;
}

void JSBGodot::set_ground_query(const godot::Callable &query)
{
    ground_query = query;
    install_ground_callback();
}

void JSBGodot::install_ground_callback()
{
    if (FDMExec == nullptr || !ground_query.is_valid() || FDMExec->GetInertial() == nullptr)
    {
        return;
    }
    GodotGroundCallback *gc = new GodotGroundCallback(ground_query,
        FDMExec->GetInertial()->GetSemimajor(), FDMExec->GetInertial()->GetSemiminor());
    gc->SetTerrainElevation(last_terrain_m * 3.28084);
    FDMExec->GetInertial()->SetGroundCallback(gc);
    UtilityFunctions::print("JSBGodot: scenery ground callback installed (per-contact terrain + slope)");
}

void JSBGodot::set_terrain_elevation(double elevation_m)
{
    // Set the terrain elevation in JSBSim (convert meters to feet)
    if (FDMExec && FDMExec->GetPropagate() && FDMExec->GetPropertyManager()) {
        last_terrain_m = elevation_m;
        double elevation_ft = elevation_m * 3.28084;  // meters to feet
        // Check if property exists before setting
        if (FDMExec->GetPropertyManager()->HasNode("position/terrain-elevation-asl-ft")) {
            FDMExec->SetPropertyValue("position/terrain-elevation-asl-ft", elevation_ft);
        }
    }
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



double JSBGodot::get_property(const String &name) const
{
    return FDMExec ? FDMExec->GetPropertyValue(std::string(name.utf8().get_data())) : 0.0;
}

void JSBGodot::set_property(const String &name, double value)
{
    if (FDMExec)
        FDMExec->SetPropertyValue(std::string(name.utf8().get_data()), value);
}

int JSBGodot::get_tank_count() const
{
    return FDMExec ? (int)FDMExec->GetPropulsion()->GetNumTanks() : 0;
}

double JSBGodot::get_tank_capacity_lbs(int index) const
{
    if (!FDMExec || index < 0 || index >= (int)FDMExec->GetPropulsion()->GetNumTanks())
        return 0.0;
    return FDMExec->GetPropulsion()->GetTank(index)->GetCapacity();
}

double JSBGodot::get_tank_contents_lbs(int index) const
{
    if (!FDMExec || index < 0 || index >= (int)FDMExec->GetPropulsion()->GetNumTanks())
        return 0.0;
    return FDMExec->GetPropulsion()->GetTank(index)->GetContents();
}

void JSBGodot::set_tank_contents_lbs(int index, double lbs)
{
    if (!FDMExec || index < 0 || index >= (int)FDMExec->GetPropulsion()->GetNumTanks())
        return;
    auto tank = FDMExec->GetPropulsion()->GetTank(index);
    tank->SetContents(Math::clamp(lbs, 0.0, tank->GetCapacity()));
}

Dictionary JSBGodot::get_cg_info() const
{
    Dictionary d;
    if (!FDMExec)
        return d;
    double cg_x = FDMExec->GetPropertyValue("inertia/cg-x-in");
    double mac_ft = FDMExec->GetPropertyValue("metrics/cbarw-ft");
    double rp_x = FDMExec->GetPropertyValue("metrics/aero-rp-x-in");
    d["cg_x_in"] = cg_x;
    d["cg_y_in"] = FDMExec->GetPropertyValue("inertia/cg-y-in");
    d["cg_z_in"] = FDMExec->GetPropertyValue("inertia/cg-z-in");
    d["weight_lbs"] = FDMExec->GetPropertyValue("inertia/weight-lbs");
    d["mac_ft"] = mac_ft;
    d["aero_rp_x_in"] = rp_x;
    double mac_in = mac_ft * 12.0;
    d["pct_mac"] = mac_in > 1e-6 ? (cg_x - (rp_x - 0.25 * mac_in)) / mac_in * 100.0 : 0.0;
    return d;
}

double JSBGodot::get_pitch_deg() const
{
    return FDMExec ? Math::rad_to_deg(FDMExec->GetPropagate()->GetEuler(2)) : 0.0;
}

double JSBGodot::get_roll_deg() const
{
    return FDMExec ? Math::rad_to_deg(FDMExec->GetPropagate()->GetEuler(1)) : 0.0;
}

// ── Weather ──────────────────────────────────────────────────────────────────

void JSBGodot::set_weather(const Array &layers, double qnh_hpa)
{
    weather_layers.clear();
    for (int i = 0; i < layers.size(); i++)
    {
        Dictionary d = layers[i];
        WeatherLayer l;
        l.base_m = double(d.get("base_m", 0.0));
        l.top_m = std::max(double(d.get("top_m", 0.0)), l.base_m);
        const double dir = Math::deg_to_rad(double(d.get("wind_dir_deg", 0.0)));
        const double spd = std::max(0.0, double(d.get("wind_speed_kts", 0.0)));
        // Meteorological direction = where the wind blows FROM.
        l.wind_n_kts = -spd * std::cos(dir);
        l.wind_e_kts = -spd * std::sin(dir);
        l.turbulence = std::clamp(int(d.get("turbulence", 0)), 0, 3);
        weather_layers.push_back(l);
    }
    std::stable_sort(weather_layers.begin(), weather_layers.end(),
                     [](const WeatherLayer &a, const WeatherLayer &b) { return a.base_m < b.base_m; });
    weather_qnh_hpa = std::clamp(qnh_hpa, 800.0, 1100.0);
}

// Wind inside a layer is that layer's wind; between two layers it blends
// linearly (as a vector) across the gap; below the lowest layer the lowest
// applies, above the highest the highest. Turbulence is discrete: the nearer
// layer's level.
JSBGodot::WeatherSample JSBGodot::sample_weather(double alt_m) const
{
    WeatherSample s;
    const size_t n = weather_layers.size();
    if (n == 0)
    {
        return s;
    }
    auto from = [&](const WeatherLayer &l) {
        s.wind_n_kts = l.wind_n_kts;
        s.wind_e_kts = l.wind_e_kts;
        s.turbulence = l.turbulence;
    };
    if (alt_m <= weather_layers[0].base_m)
    {
        from(weather_layers[0]);
        return s;
    }
    for (size_t i = 0; i < n; i++)
    {
        const WeatherLayer &l = weather_layers[i];
        if (alt_m >= l.base_m && alt_m <= l.top_m)
        {
            from(l);
            return s;
        }
        if (i + 1 < n && alt_m > l.top_m && alt_m < weather_layers[i + 1].base_m)
        {
            const WeatherLayer &u = weather_layers[i + 1];
            const double f = (alt_m - l.top_m) / (u.base_m - l.top_m);
            s.wind_n_kts = l.wind_n_kts + (u.wind_n_kts - l.wind_n_kts) * f;
            s.wind_e_kts = l.wind_e_kts + (u.wind_e_kts - l.wind_e_kts) * f;
            s.turbulence = f < 0.5 ? l.turbulence : u.turbulence;
            return s;
        }
    }
    from(weather_layers[n - 1]);
    return s;
}

Vector3 JSBGodot::get_weather_wind_ned_kts(double alt_m) const
{
    const WeatherSample s = sample_weather(alt_m);
    return Vector3(s.wind_n_kts, s.wind_e_kts, 0.0);
}

int JSBGodot::get_weather_turbulence(double alt_m) const
{
    return sample_weather(alt_m).turbulence;
}

double JSBGodot::get_weather_qnh_hpa() const
{
    return weather_qnh_hpa;
}

void JSBGodot::apply_weather_to_fdm()
{
    if (!FDMExec)
    {
        return;
    }
    static const double KTS_TO_FPS = 1.6878098571;
    const double alt_m = FDMExec->GetPropagate()->GetAltitudeASL() * 0.3048;
    const WeatherSample s = sample_weather(alt_m);

    std::shared_ptr<JSBSim::FGWinds> winds = FDMExec->GetWinds();
    if (winds)
    {
        winds->SetWindNED(s.wind_n_kts * KTS_TO_FPS, s.wind_e_kts * KTS_TO_FPS, 0.0);
        if (s.turbulence <= 0)
        {
            winds->SetTurbType(JSBSim::FGWinds::ttNone);
            winds->SetProbabilityOfExceedence(0);
        }
        else
        {
            // MIL-F-8785C: severity = probability-of-exceedance index (1..7),
            // low-altitude intensity from the 20 ft AGL wind. Use the ground
            // layer's wind but at least the spec's light/moderate/severe
            // reference speeds (15/30/45 kt) so still air still shakes.
            static const int severity[4] = { 0, 2, 4, 6 };
            static const double floor_kts[4] = { 0.0, 15.0, 30.0, 45.0 };
            const WeatherLayer &g = weather_layers.front();
            const double ground_kts = std::hypot(g.wind_n_kts, g.wind_e_kts);
            winds->SetTurbType(JSBSim::FGWinds::ttMilspec);
            winds->SetProbabilityOfExceedence(severity[s.turbulence]);
            winds->SetWindspeed20ft(std::max(ground_kts, floor_kts[s.turbulence]) * KTS_TO_FPS);
        }
    }

    std::shared_ptr<JSBSim::FGAtmosphere> atm = FDMExec->GetAtmosphere();
    if (atm && std::fabs(atm->GetPressureSL(JSBSim::FGAtmosphere::eMillibars) - weather_qnh_hpa) > 0.01)
    {
        atm->SetPressureSL(JSBSim::FGAtmosphere::eMillibars, weather_qnh_hpa);
    }
}

void JSBGodot::reset_controls()
{
    input_pitch = 0.0f;
    input_roll = 0.0f;
    input_rudder = 0.0f;
    input_throttle = 0.0f;
    input_aileron = 0.0f;
    input_elevator = 0.0f;
    elevator_trim = 0.0f;
    trigger_left_value = 0.0f;
    trigger_right_value = 0.0f;
    trim_up_held = false;
    trim_down_held = false;
    flaps = 0.0f;
    gear_cmd = 1.0f;
    input_brake = 0.0f;
    input_magnetos = 3;
    if (FDMExec)
    {
        // Push the neutral commands now so RunIC() in the following
        // re-initialisation does not see the old throttle/flap settings.
        FDMExec->SetPropertyValue("fcs/throttle-cmd-norm", 0.0);
        FDMExec->SetPropertyValue("fcs/flap-cmd-norm", 0.0);
        FDMExec->SetPropertyValue("fcs/gear-cmd-norm", 1.0);
        // Snap the actuated POSITIONS too, not just the commands: a retractable
        // airframe's gear kinematic (A-10: 5 s) re-reads its output property
        // each step, and JSBSim computes no ground contact while the gear is
        // in transit — the aircraft fell through the runway and bounced on
        // a Fly! after a gear-up flight (Dübendorf 2026-09-18).
        static const char *snap_one[] = { "gear/gear-pos-norm", "gear/gear-cmd-norm" };
        static const char *snap_zero[] = { "fcs/flap-pos-norm", "fcs/flap-pos-deg", "fcs/speedbrake-pos-norm", "fcs/speedbrake-cmd-norm" };
        JSBSim::FGPropertyManager *pm = FDMExec->GetPropertyManager().get();
        for (const char *name : snap_one)
        {
            if (pm->HasNode(name)) FDMExec->SetPropertyValue(name, 1.0);
        }
        for (const char *name : snap_zero)
        {
            if (pm->HasNode(name)) FDMExec->SetPropertyValue(name, 0.0);
        }
        FDMExec->SetPropertyValue("fcs/left-brake-cmd-norm", 0.0);
        FDMExec->SetPropertyValue("fcs/right-brake-cmd-norm", 0.0);
        FDMExec->SetPropertyValue("fcs/pitch-trim-cmd-norm", 0.0);
        copy_inputs_to_JSBSim();
    }
    UtilityFunctions::print("JSBGodot: controls reset (fresh session)");
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
    // Diagnostic for a one-off "aircraft upside down after teleport" report
    // (2026-09-18): the node basis handed in should be roughly upright.
    {
        const Vector3 up = geo::wun_basis(g.lat, g.lon).get_column(1);
        const double updot = ac_global.basis.get_column(1).normalized().dot(up);
        if (updot < 0.5)
        {
            UtilityFunctions::push_warning("reinitialize_from_pose: aircraft basis is not upright (up.n=", updot,
                ") — JSBSim starts level anyway; if the model stays inverted, slew mode never released. basis=", ac_global.basis);
        }
    }
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


void JSBGodot::_input(const Ref<InputEvent> event)
{
    if (event->is_class("InputEventKey"))
    {
        Ref<InputEventKey> key_event = event;
        if (key_event->is_pressed())
        {
            // Every key goes by the keyboard mapping (Settings > Keyboard).
            // Nose-wheel steering range (A-10 NWS engage), checked first.
            if (KeyBindings::matches("nose_steer_toggle", event))
            {
                if (nose_steer_full_deg > nose_steer_limited_deg && nose_steer_limited_deg > 0.0)
                {
                    nose_steer_full = !nose_steer_full;
                    UtilityFunctions::print("Nose-wheel steering: ", nose_steer_full ? "FULL" : "LIMITED",
                        " (", nose_steer_full ? nose_steer_full_deg : nose_steer_limited_deg, " deg)");
                }
                else
                {
                    UtilityFunctions::print("Nose-wheel steering: this aircraft has a single range");
                }
                return;
            }
            if (KeyBindings::matches("throttle_down", event))
            {
                // Decrease throttle
                set_input_throttle(input_throttle - 0.05f); // Decrease by 0.1
                copy_outputs_from_JSBSim();
                printf("Throttle decreased to %f\n", input_throttle);
            }
            else if (KeyBindings::matches("throttle_up", event))
            {
                // Increase throttle
                set_input_throttle(input_throttle + 0.05f); // Increase by 0.1
                copy_outputs_from_JSBSim();
                printf("Throttle increased to %f\n", input_throttle);
            }
            else if (KeyBindings::matches("elevator_up", event))
            {
                // Increase Elevator
                set_input_elevator(input_elevator + 0.05f); // Increase by 0.1
                copy_outputs_from_JSBSim();
                printf("Pitch increased to %f\n", input_pitch);
            }
            else if (KeyBindings::matches("elevator_down", event))
            {
                // Decrease Elevator
                set_input_elevator(input_elevator - 0.05f); // Decrease by 0.1
                copy_outputs_from_JSBSim();
                printf("Pitch decreased to %f\n", input_pitch);
            }
            else if (KeyBindings::matches("aileron_right", event))
            {
                // Increase Aileron
                set_input_aileron(input_aileron - 0.05f); // Decrease by 0.1
                copy_outputs_from_JSBSim();
                printf("Roll decreased to %f\n", input_roll);
            }
            else if (KeyBindings::matches("aileron_left", event))
            {
                // Decrease Aileron
                set_input_aileron(input_aileron + 0.05f); // Increase by 0.1
                copy_outputs_from_JSBSim();
                printf("Roll increased to %f\n", input_roll);
            }
            else if (KeyBindings::matches("rudder_right", event))
            {
                // Right Rudder
                set_input_rudder(input_rudder + 0.05f); // Increase by 0.1
            }
            else if (KeyBindings::matches("rudder_left", event))
            {
                // Left Rudder
                set_input_rudder(input_rudder - 0.05f); // Decrease by 0.1
            }
            if (KeyBindings::matches("fdm_start", event))
            {
                // Start process
                set_process(true);
                printf("Process started\n");
            }
            if (KeyBindings::matches("fdm_stop", event))
            {
                // Stop process
                set_process(false);
                printf("Process ended\n");
            }
            if (KeyBindings::matches("brakes_on", event))
            {
                set_input_brake(1.0f);
                printf("Brakes applied\n");
            }
            else if (KeyBindings::matches("brakes_off", event))
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
        
        // Apply continuous throttle from triggers. Dead zone 0.3: pads that
        // share one axis between the two triggers (Logitech F310 in D mode)
        // rest a little off centre after a pull, and a raw > 0.1 test kept
        // running the throttle down to idle. The rate scales from the dead
        // zone up, so analog triggers still give a proportional rate.
        const float trigger_dead = 0.3f;
        if (trigger_right_value > trigger_dead) {
            set_input_throttle(input_throttle + (trigger_right_value - trigger_dead) / (1.0f - trigger_dead) * 0.005f);
        }
        if (trigger_left_value > trigger_dead) {
            set_input_throttle(input_throttle - (trigger_left_value - trigger_dead) / (1.0f - trigger_dead) * 0.005f);
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
        apply_weather_to_fdm();
        // Sub-step the FDM so it (a) keeps real time — JSBSim's default step
        // is 1/120 s but Godot's physics tick is 1/60 s, so one Run() per
        // tick ran the aircraft at half speed — and (b) integrates the gear
        // springs at FDM_TARGET_HZ, where the A-10's stiff, heavily damped
        // legs stop bouncing themselves up on downhill grass (ENGM,
        // 2026-09-20). Ground contacts (the scenery callback) are queried
        // per sub-step as well.
        const double target_dt = 1.0 / FDM_TARGET_HZ;
        int steps = (int)std::lround((double)delta / target_dt);
        if (steps < 1) steps = 1;
        if (steps > 16) steps = 16;
        FDMExec->Setdt((double)delta / (double)steps);
        for (int i = 0; i < steps; i++)
        {
            FDMExec->Run();
        }
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
        install_ground_callback();
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
    // The legacy flat frame ran JSBSim in a mirrored world, and every input
    // path plus the control-surface animations were tuned against that
    // mirror. The ECEF world is not mirrored, so the lateral commands are
    // flipped here, at the FDM boundary only, keeping input_* (and the
    // animations that read them) exactly as before. Pitch is unaffected.
    const float lateral_sign = -1.0f;
    FDMExec->SetPropertyValue("fcs/aileron-cmd-norm", lateral_sign * input_aileron);
    FDMExec->SetPropertyValue("fcs/elevator-cmd-norm", input_elevator + elevator_trim);
    FDMExec->SetPropertyValue("fcs/rudder-cmd-norm", lateral_sign * input_rudder);
    // Nose-wheel steering (fcs/steer-cmd-norm drives the gear legs' max_steer).
    // JSBSim's two commands have OPPOSITE sign conventions for the same pedal:
    // right pedal is rudder-cmd -1 (yaw right) but steer-cmd +1 (nose wheel
    // right) — exactly what FlightGear does (SetDrCmd(-rudder), SetDsCmd(rudder)).
    // The flat-frame build wrote -input_rudder here, which only looked right in
    // the mirrored world; in the ECEF world ground turns went the wrong way.
    // Verified headlessly: tests/smoke_ground_steering.tscn.
    // With full nose-wheel steering engaged (Ctrl+W) the command is scaled
    // by full/limited: JSBSim multiplies it by <max_steer> without clamping,
    // so the nose wheel reaches the full range (A-10: 5 deg -> 60 deg).
    double steer_gain = 1.0;
    if (nose_steer_full && nose_steer_limited_deg > 0.0 && nose_steer_full_deg > nose_steer_limited_deg)
    {
        steer_gain = nose_steer_full_deg / nose_steer_limited_deg;
    }
    FDMExec->SetPropertyValue("fcs/steer-cmd-norm", -lateral_sign * input_rudder * steer_gain);
    // Throttle ALL engines (-1). Setting the bare "fcs/throttle-cmd-norm" property
    // only drives engine 0, leaving the other engine(s) at idle/zero on a twin
    // like the A-10 (→ asymmetric thrust + yaw). SetThrottleCmd(-1) fans out to
    // every engine; for single-engine aircraft it is identical to before.
    FCS->SetThrottleCmd(-1, input_throttle);


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

void JSBGodot::initialize_at_terrain(double terrain_elevation_m, Vector3 godot_pos, double heading_deg_param)
{
    // Initialise JSBSim resting on the terrain at the aircraft node's ECEF
    // position with a TRUE compass heading. JSBSim runs at the real geodetic
    // location (design §9.5): gravity, Coriolis and the ellipsoid are real.
    if (!FDMExec) {
        UtilityFunctions::print("Cannot initialize_at_terrain: FDMExec not created");
        return;
    }

    double terrain_ft = terrain_elevation_m * 3.28084;  // meters to feet
    // C172P CG-above-wheels distance from c172p.xml: CG z=36.5", main gear
    // z=-15.5" → 52" = 4.33 ft. Spawn the aircraft right at its rest height
    // so the gear springs don't have to oscillate to settle.
    double gear_height_ft = 4.33;
    double aircraft_altitude_ft = terrain_ft + gear_height_ft;

    const geo::Geodetic g = geo::geodetic_of(godot_pos);
    UtilityFunctions::print("Initializing JSBSim at lat=", g.lat, " lon=", g.lon,
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
    // Properly initialize engine(s): propulsion/set-running = -1 runs InitRunning
    // on EVERY engine (RPM, mixture, magnetos, Running=true). A value >= 0 would
    // only start that one engine — wrong for multi-engine aircraft (A-10).
    FDMExec->SetPropertyValue("propulsion/set-running", -1);
    FDMExec->SetPropertyValue("fcs/brake-cmd-norm", 0.0);
    FDMExec->SetPropertyValue("fcs/parking-brake-cmd-norm", 0.0);
    // Override mixture to current user setting (InitRunning sets it to PressureRatio*1.3)
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
    jsbsim_initialized = true;
    UtilityFunctions::print("JSBSim initialized successfully");
}

void JSBGodot::copy_outputs_from_JSBSim()
{
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

    altitude_ft = Propagate->GetAltitudeASL();
    cached_latitude = Propagate->GetGeodLatitudeDeg();
    cached_longitude = Propagate->GetLongitudeDeg();

    // ECEF world (design §9.5): position straight from JSBSim's ECEF location,
    // basis from the body-to-ECEF DCM. Body axes (X fwd, Y right, Z down) map
    // onto the aircraft node's (X = -right, Y = up, Z = fwd); the node's scale
    // is preserved.
    const JSBSim::FGLocation &loc = Propagate->GetLocation();
    Vector3 pos((double)loc(1) * 0.3048, (double)loc(2) * 0.3048, (double)loc(3) * 0.3048);
    const JSBSim::FGMatrix33 &m = Propagate->GetTb2ec();
    const Vector3 bx((double)m(1, 1), (double)m(2, 1), (double)m(3, 1));
    const Vector3 by((double)m(1, 2), (double)m(2, 2), (double)m(3, 2));
    const Vector3 bz((double)m(1, 3), (double)m(2, 3), (double)m(3, 3));
    // Wheel/CG clearance offset along local up.
    pos += geo::wun_basis(cached_latitude, cached_longitude).get_column(1) * godot_terrain_y_offset;
    cached_godot_position = pos;

    Node3D *ac = Object::cast_to<Node3D>(get_parent());
    if (!ac)
    {
        printf("Parent node is null!\n");
        return;
    }
    // In slew mode GDScript/SlewMode own the node transform.
    if (!slew_mode)
    {
        // The node's scale is read back from its own basis; remember the last
        // sane value so one degenerate tick can never collapse the aircraft
        // (and every child camera with it) to scale 0 for good.
        Vector3 scale = ac->get_scale();
        if (scale.is_finite() && scale.x > 1e-6 && scale.y > 1e-6 && scale.z > 1e-6)
        {
            last_good_scale = scale;
        }
        else
        {
            scale = last_good_scale;
        }
        Basis b;
        b.set_column(0, -by * scale.x);
        b.set_column(1, -bz * scale.y);
        b.set_column(2, bx * scale.z);
        const Transform3D xf(b, pos);
        const bool dcm_ok = bx.is_finite() && by.is_finite() && bz.is_finite()
            && bx.length_squared() > 0.5 && by.length_squared() > 0.5 && bz.length_squared() > 0.5;
        if (!pos.is_finite() || !dcm_ok)
        {
            // The FDM blew up (huge ground-contact force, etc.). Never hand a
            // NaN pose to the scene (renderer asserts, camera/sky go NaN):
            // restart the FDM at the last good pose, stationary.
            const int64_t now = Time::get_singleton()->get_ticks_msec();
            if (now - last_nan_reset_msec > 200)
            {
                last_nan_reset_msec = now;
                UtilityFunctions::push_warning("JSBSim produced a non-finite state — resetting the flight model at the last good position");
                if (has_last_good_transform)
                {
                    reset_controls();
                    reinitialize_from_pose(last_good_transform, 0.0, last_terrain_m);
                }
            }
            return;
        }
        ac->set_global_transform(xf);
        last_good_transform = xf;
        has_last_good_transform = true;
    }

    vertical_speed_fpm = -FDMExec->GetPropertyValue("velocities/v-down-fps") * 60.0;
    airspeed_knots = FDMExec->GetPropertyValue("velocities/vc-kts");
    heading_deg = FDMExec->GetPropertyValue("attitude/psi-deg"); // true heading
}
