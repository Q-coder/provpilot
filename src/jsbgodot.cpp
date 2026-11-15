#include "key_bindings.h"
#include "jsbgodot.h"
#include "aircraft_config.h"
#include "geo_frame.h"
#include <chrono>
#include <functional>
#include "models/FGFCS.h"
#include "models/FGGroundReactions.h"
#include "models/FGLGear.h"
#include "models/FGMassBalance.h"
#include "models/FGAircraft.h"
#include "initialization/FGInitialCondition.h"
#include "initialization/FGTrim.h"
#include "models/FGPropulsion.h"
#include "models/propulsion/FGEngine.h"
#include "models/propulsion/FGTurbine.h"
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
    ClassDB::bind_method(D_METHOD("set_throttle_drives_airbrakes", "on"), &JSBGodot::set_throttle_drives_airbrakes);
    ClassDB::bind_method(D_METHOD("get_throttle_drives_airbrakes"), &JSBGodot::get_throttle_drives_airbrakes);
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
    ClassDB::bind_method(D_METHOD("set_input_elevator", "value"), &JSBGodot::set_input_elevator);
    ClassDB::bind_method(D_METHOD("get_input_aileron"), &JSBGodot::get_input_aileron);
    ClassDB::bind_method(D_METHOD("get_input_elevator"), &JSBGodot::get_input_elevator);
    ClassDB::bind_method(D_METHOD("increase_flaps"), &JSBGodot::increase_flaps);
    ClassDB::bind_method(D_METHOD("decrease_flaps"), &JSBGodot::decrease_flaps);
    ClassDB::bind_method(D_METHOD("set_flaps", "value"), &JSBGodot::set_flaps);
    ClassDB::bind_method(D_METHOD("set_flap_detents", "positions"), &JSBGodot::set_flap_detents);
    ClassDB::bind_method(D_METHOD("get_flap_detents"), &JSBGodot::get_flap_detents);
    ClassDB::bind_method(D_METHOD("set_flap_detent_gate", "index", "property", "min_value"), &JSBGodot::set_flap_detent_gate);
    ClassDB::bind_method(D_METHOD("is_flap_detent_available", "index"), &JSBGodot::is_flap_detent_available);
    ClassDB::bind_method(D_METHOD("step_flap_detent", "from", "dir"), &JSBGodot::step_flap_detent);
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
    ClassDB::bind_method(D_METHOD("initialize_airborne", "terrain_elevation_m", "godot_pos", "heading_deg", "agl_m", "airspeed_kts"), &JSBGodot::initialize_airborne);
    ClassDB::bind_method(D_METHOD("trim_longitudinal"), &JSBGodot::trim_longitudinal);
    ClassDB::bind_method(D_METHOD("load_aircraft", "aircraft_id", "jsbsim_model"), &JSBGodot::load_aircraft, DEFVAL("c172p"), DEFVAL("c172p"));
    ClassDB::bind_method(D_METHOD("is_initialized"), &JSBGodot::is_initialized);
    ClassDB::bind_method(D_METHOD("set_slew_mode", "enabled"), &JSBGodot::set_slew_mode);
    ClassDB::bind_method(D_METHOD("is_slew_mode"), &JSBGodot::is_slew_mode);
    ClassDB::bind_method(D_METHOD("get_slew_mode"), &JSBGodot::get_slew_mode);
    ClassDB::bind_method(D_METHOD("set_godot_terrain_y_offset", "offset_m"), &JSBGodot::set_godot_terrain_y_offset);
    ClassDB::bind_method(D_METHOD("get_godot_terrain_y_offset"), &JSBGodot::get_godot_terrain_y_offset);
    ClassDB::bind_method(D_METHOD("set_origin_forward_m", "m"), &JSBGodot::set_origin_forward_m);
    ClassDB::bind_method(D_METHOD("get_origin_forward_m"), &JSBGodot::get_origin_forward_m);
    ClassDB::bind_method(D_METHOD("get_latitude_deg"), &JSBGodot::get_latitude_deg);
    ClassDB::bind_method(D_METHOD("get_longitude_deg"), &JSBGodot::get_longitude_deg);
    ClassDB::bind_method(D_METHOD("get_godot_position"), &JSBGodot::get_godot_position);
    ClassDB::bind_method(D_METHOD("get_property", "name"), &JSBGodot::get_property);
    ClassDB::bind_method(D_METHOD("has_property", "name"), &JSBGodot::has_property);
    ClassDB::bind_method(D_METHOD("set_property", "name", "value"), &JSBGodot::set_property);
    ClassDB::bind_method(D_METHOD("get_tank_count"), &JSBGodot::get_tank_count);
    ClassDB::bind_method(D_METHOD("get_tank_capacity_lbs", "index"), &JSBGodot::get_tank_capacity_lbs);
    ClassDB::bind_method(D_METHOD("get_tank_contents_lbs", "index"), &JSBGodot::get_tank_contents_lbs);
    ClassDB::bind_method(D_METHOD("set_tank_contents_lbs", "index", "lbs"), &JSBGodot::set_tank_contents_lbs);
    ClassDB::bind_method(D_METHOD("set_fuel_percent", "pct"), &JSBGodot::set_fuel_percent);
    ClassDB::bind_method(D_METHOD("set_mac_reference", "le_x_in", "mac_in"), &JSBGodot::set_mac_reference);
    ClassDB::bind_method(D_METHOD("set_gear_transit", "seconds"), &JSBGodot::set_gear_transit);
    ClassDB::bind_method(D_METHOD("apply_joystick", "roll", "pitch", "yaw", "throttle", "throttle_absolute", "brakes_axis", "brakes_button"), &JSBGodot::apply_joystick);
    ClassDB::bind_method(D_METHOD("set_trim_buttons", "up", "down"), &JSBGodot::set_trim_buttons);
    ClassDB::bind_method(D_METHOD("get_cg_info"), &JSBGodot::get_cg_info);
    ClassDB::bind_method(D_METHOD("get_gear_contacts"), &JSBGodot::get_gear_contacts);
    ClassDB::bind_method(D_METHOD("get_pitch_deg"), &JSBGodot::get_pitch_deg);
    ClassDB::bind_method(D_METHOD("get_roll_deg"), &JSBGodot::get_roll_deg);
    ClassDB::bind_method(D_METHOD("reinitialize_from_pose", "ac_global", "airspeed_kts", "terrain_elevation_m"), &JSBGodot::reinitialize_from_pose);
    ClassDB::bind_method(D_METHOD("reset_controls"), &JSBGodot::reset_controls);
    ClassDB::bind_method(D_METHOD("set_weather", "layers", "qnh_hpa"), &JSBGodot::set_weather);
    ClassDB::bind_method(D_METHOD("get_engine_count"), &JSBGodot::get_engine_count);
    ClassDB::bind_method(D_METHOD("set_property_links", "links"), &JSBGodot::set_property_links);
    ClassDB::bind_method(D_METHOD("set_init_pulses", "pulses"), &JSBGodot::set_init_pulses);
    ClassDB::bind_method(D_METHOD("set_default_pitch_trim", "trim"), &JSBGodot::set_default_pitch_trim);
    ClassDB::bind_method(D_METHOD("get_default_pitch_trim"), &JSBGodot::get_default_pitch_trim);
    ClassDB::bind_method(D_METHOD("get_pitch_trim"), &JSBGodot::get_pitch_trim);
    ClassDB::bind_method(D_METHOD("set_pitch_trim", "trim"), &JSBGodot::set_pitch_trim);
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

void JSBGodot::set_throttle_drives_airbrakes(bool on)
{
    if (on && !throttle_drives_airbrakes)
    {
        input_throttle = 1.0f; // lever forward = airbrakes in
    }
    else if (!on && throttle_drives_airbrakes)
    {
        input_throttle = 0.0f;
    }
    throttle_drives_airbrakes = on;
}

bool JSBGodot::get_throttle_drives_airbrakes() const { return throttle_drives_airbrakes; }

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

// The lever notches: aircraft.cfg [flaps] positions (the MD-80's 7 detents),
// the Cessna's thirds by default. The keys / joystick buttons step through
// them; stepping by thirds sent the MD-80 lever past its 11 detent (gery
// 2026-11-01: "0/slats, then straight to 15").
void JSBGodot::set_flap_detents(const Array &positions)
{
    std::vector<float> d;
    for (int i = 0; i < positions.size(); i++) {
        d.push_back((float)(double)positions[i]);
    }
    if (d.size() >= 2) {
        flap_detents = d;
    }
}

Array JSBGodot::get_flap_detents() const
{
    Array a;
    for (float v : flap_detents) {
        a.push_back(v);
    }
    return a;
}

static int nearest_detent(const std::vector<float> &d, float v)
{
    int best = 0;
    for (int i = 1; i < (int)d.size(); i++) {
        if (std::abs(d[i] - v) < std::abs(d[best] - v)) {
            best = i;
        }
    }
    return best;
}

void JSBGodot::set_flap_detent_gate(int index, const String &property, double min_value)
{
    flap_gate_index = index;
    flap_gate_property = std::string(property.utf8().get_data());
    flap_gate_min = min_value;
}

bool JSBGodot::is_flap_detent_available(int index) const
{
    if (index != flap_gate_index || flap_gate_property.empty()) {
        return true;
    }
    if (!FDMExec || !FDMExec->GetPropertyManager()->HasNode(flap_gate_property)) {
        return true; // no FDM / no such node: never hide a detent
    }
    return FDMExec->GetPropertyValue(flap_gate_property) >= flap_gate_min;
}

int JSBGodot::step_flap_detent(int from, int dir) const
{
    const int last = (int)flap_detents.size() - 1;
    const int step = dir >= 0 ? 1 : -1;
    int i = CLAMP(from, 0, last) + step;
    while (i >= 0 && i <= last && !is_flap_detent_available(i)) {
        i += step;
    }
    if (i < 0 || i > last) {
        return CLAMP(from, 0, last); // nothing further in that direction
    }
    return i;
}

void JSBGodot::increase_flaps()
{
    flaps = flap_detents[step_flap_detent(nearest_detent(flap_detents, flaps), +1)];
}

void JSBGodot::decrease_flaps()
{
    flaps = flap_detents[step_flap_detent(nearest_detent(flap_detents, flaps), -1)];
}

void JSBGodot::set_flaps(float value)
{
    // the exact lever value: the detents come from the aircraft's cfg
    // ([flaps] positions); snapping to thirds here sent the MD-80's 11 deg
    // detent (0.36) to its lever table as 0.333 = slats only (gery 2026-11-01)
    flaps = CLAMP(value, 0.0f, 1.0f);
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

void JSBGodot::set_origin_forward_m(double m) { origin_forward_m = m; }
double JSBGodot::get_origin_forward_m() const { return origin_forward_m; }

// Where the CG goes for an aircraft node placed level at node_pos.
Vector3 JSBGodot::cg_of_node(const Vector3 &node_pos, double true_heading_deg) const
{
    if (origin_forward_m == 0.0)
        return node_pos;
    const geo::Geodetic g = geo::geodetic_of(node_pos);
    const Basis w = geo::wun_basis(g.lat, g.lon); // west, up, north
    const double h = Math::deg_to_rad(true_heading_deg);
    const Vector3 fwd = w.get_column(2) * std::cos(h) - w.get_column(0) * std::sin(h);
    return node_pos - fwd * origin_forward_m;
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



bool JSBGodot::has_property(const String &name) const
{
    if (!FDMExec || !FDMExec->GetPropertyManager())
    {
        return false;
    }
    return FDMExec->GetPropertyManager()->HasNode(std::string(name.utf8().get_data()));
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
    // the settings screen pauses the FDM: recompute weight and CG now so the
    // panel's CG bar follows the fuel slider
    auto mb = FDMExec->GetMassBalance();
    auto prop = FDMExec->GetPropulsion();
    if (mb && prop)
    {
        // what FGFDMExec::LoadInputs(eMassBalance) does before the model runs
        mb->in.TanksWeight = prop->GetTanksWeight();
        mb->in.TanksMoment = prop->GetTanksMoment();
        mb->in.TankInertia = prop->CalculateTankInertias();
        mb->Run(false);
    }
}

// Calibrated joystick axes from Settings > Joystick, once per frame. A stick
// axis is applied when it moved since the last poll (an unbound axis arrives
// as NAN), so the keyboard still nudges the controls while the stick rests.
// The throttle is absolute for a lever (0..1 from -1..1); a trigger keeps the
// incremental RT / LT behaviour of the event handler. The brake axis is the
// pulled half of a centred lever (0..1) and the brakes button holds them.
void JSBGodot::apply_joystick(double roll, double pitch, double yaw, double throttle, bool throttle_absolute, double brakes_axis, bool brakes_button)
{
    auto moved = [](double v, double &last) {
        if (std::isnan(v))
        {
            return false;
        }
        const bool m = std::isnan(last) || std::fabs(v - last) > 1e-4;
        last = v;
        return m;
    };
    if (moved(roll, last_joy_roll))
    {
        set_input_aileron((float)roll);
    }
    if (moved(pitch, last_joy_pitch))
    {
        set_input_elevator((float)pitch);
    }
    if (moved(yaw, last_joy_yaw))
    {
        set_input_rudder((float)yaw);
    }
    if (throttle_absolute && moved(throttle, last_joy_throttle))
    {
        input_throttle = CLAMP((float)((throttle + 1.0) * 0.5), 0.0f, 1.0f);
    }
    brake_axis = std::isnan(brakes_axis) ? 0.0f : CLAMP((float)brakes_axis, 0.0f, 1.0f);
    brake_button = brakes_button;
}

void JSBGodot::set_gear_transit(double seconds) { gear_transit_s = Math::max(seconds, 0.0); }

void JSBGodot::set_default_pitch_trim(double trim)
{
    default_pitch_trim = (float)Math::clamp(trim, -1.0, 1.0);
    if (!jsbsim_initialized)
    {
        elevator_trim = default_pitch_trim;
    }
}
double JSBGodot::get_default_pitch_trim() const { return default_pitch_trim; }
double JSBGodot::get_pitch_trim() const { return elevator_trim; }
void JSBGodot::set_pitch_trim(double trim) { elevator_trim = CLAMP((float)trim, -1.0f, 1.0f); }

void JSBGodot::settle_fcs(int steps)
{
    if (!FDMExec)
    {
        return;
    }
    // the init pulses (the MD-80's autoconfig flag, which lets its stabilizer
    // run at 20 deg/s instead of 1/3) are on while the aircraft is set up
    const Array pk = init_pulses.keys();
    for (int i = 0; i < pk.size(); i++)
    {
        set_property(String(pk[i]), 1.0);
    }
    const bool was_holding = FDMExec->Holding();
    FDMExec->Resume();
    FDMExec->Setdt(1.0 / FDM_TARGET_HZ);
    for (int i = 0; i < steps; i++)
    {
        FDMExec->Run();
    }
    FDMExec->RunIC(); // back to the initial conditions; the actuators keep their state
    if (was_holding)
    {
        FDMExec->Hold();
    }
}

// Fuel load as a share of the total capacity, loaded the way an airliner is:
// the wing tanks (off the centreline) first, in proportion to their capacity,
// then the centreline tanks. Pipes and placeholders (under 50 lb) stay empty.
void JSBGodot::set_fuel_percent(double pct)
{
    if (!FDMExec)
    {
        return;
    }
    auto prop = FDMExec->GetPropulsion();
    const int n = (int)prop->GetNumTanks();
    double total = 0.0, wing_cap = 0.0, centre_cap = 0.0;
    for (int i = 0; i < n; i++)
    {
        auto t = prop->GetTank(i);
        if (t->GetCapacity() < 50.0)
        {
            continue;
        }
        total += t->GetCapacity();
        if (std::fabs(t->GetXYZ(2)) > 12.0) // y in inches: more than a foot off the centreline
        {
            wing_cap += t->GetCapacity();
        }
        else
        {
            centre_cap += t->GetCapacity();
        }
    }
    double load = total * Math::clamp(pct, 0.0, 100.0) / 100.0;
    const double in_wings = Math::min(load, wing_cap);
    const double in_centre = Math::clamp(load - in_wings, 0.0, centre_cap);
    for (int i = 0; i < n; i++)
    {
        auto t = prop->GetTank(i);
        double lbs = 0.0;
        if (t->GetCapacity() >= 50.0)
        {
            const bool wing = std::fabs(t->GetXYZ(2)) > 12.0;
            const double share = wing ? (wing_cap > 0.0 ? in_wings / wing_cap : 0.0) : (centre_cap > 0.0 ? in_centre / centre_cap : 0.0);
            lbs = t->GetCapacity() * share;
        }
        set_tank_contents_lbs(i, lbs);
    }
}

Array JSBGodot::get_gear_contacts() const
{
    Array out;
    if (!FDMExec)
        return out;
    auto gr = FDMExec->GetGroundReactions();
    for (int i = 0; i < gr->GetNumGearUnits(); i++)
    {
        auto g = gr->GetGearUnit(i);
        const JSBSim::FGColumnVector3 b = g->GetBodyLocation(); // feet, body axes from the CG
        Dictionary d;
        d["name"] = String(g->GetName().c_str());
        d["bogey"] = g->IsBogey();
        d["forward_m"] = b(1) * 0.3048;
        d["right_m"] = b(2) * 0.3048;
        d["down_m"] = b(3) * 0.3048;
        d["wow"] = g->GetWOW();
        d["compression_m"] = g->GetCompLen() * 0.3048;
        out.push_back(d);
    }
    return out;
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
    d["pct_mac_source"] = "estimate";
    // an FDM that computes its own (the MD-80's systems know the real MAC) wins,
    // and the cfg's MAC reference over that: it is live while the settings
    // screen holds the FDM (the FDM function only updates with a step)
    if (has_property("inertia/cg-percent-mac"))
    {
        d["pct_mac"] = get_property("inertia/cg-percent-mac");
        d["pct_mac_source"] = "fdm";
    }
    if (mac_ref_in > 1e-6)
    {
        d["pct_mac"] = (cg_x - mac_le_x_in) / mac_ref_in * 100.0;
        d["pct_mac_source"] = "cfg";
    }
    // weight-dependent limits an FDM may compute (the MD-80's load manager, from the FCOM)
    if (has_property("/systems/load-manager/cg-fwd-limit") && has_property("/systems/load-manager/cg-aft-limit"))
    {
        d["cg_fwd_limit_pct"] = get_property("/systems/load-manager/cg-fwd-limit");
        d["cg_aft_limit_pct"] = get_property("/systems/load-manager/cg-aft-limit");
    }
    return d;
}

void JSBGodot::set_mac_reference(double le_x_in, double mac_in)
{
    mac_le_x_in = le_x_in;
    mac_ref_in = mac_in;
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

int JSBGodot::get_engine_count() const
{
    if (!FDMExec || !FDMExec->GetPropulsion())
    {
        return 0;
    }
    return (int)FDMExec->GetPropulsion()->GetNumEngines();
}

void JSBGodot::set_property_links(const Dictionary &links)
{
    property_links = links.duplicate();
    links_applied_once = false;
    if (property_links.size() > 0)
    {
        UtilityFunctions::print("JSBGodot: ", property_links.size(), " property links");
    }
}

void JSBGodot::set_init_pulses(const Dictionary &pulses)
{
    init_pulses = pulses.duplicate();
    if (init_pulses.size() > 0)
    {
        UtilityFunctions::print("JSBGodot: ", init_pulses.size(), " init pulse(s) started");
    }
    if (FDMExec)
    {
        const Array keys = init_pulses.keys();
        for (int i = 0; i < keys.size(); i++)
        {
            set_property(String(keys[i]), (double)init_pulses[keys[i]] > 0.0 ? 1.0 : 0.0);
        }
    }
}

// Before each FDM step: the init pulses count down in FDM time (a flight may
// be held for many seconds while the scenery loads, so wall time would
// expire unseen), the property links copy src -> dst, and -- as FlightGear's
// JSBSim interface does every frame -- /controls/engines/engine[n]/starter
// and /cutoff reach the engines of an aircraft whose systems define them.
void JSBGodot::apply_property_links_and_pulses(double delta)
{
    if (!FDMExec)
    {
        return;
    }
    if (!links_applied_once && (property_links.size() > 0 || init_pulses.size() > 0))
    {
        links_applied_once = true;
        UtilityFunctions::print("JSBGodot: links/pulses active in the FDM step (", property_links.size(), " links, ", init_pulses.size(), " pulses)");
    }
    const Array pk = init_pulses.keys();
    for (int i = 0; i < pk.size(); i++)
    {
        double left = (double)init_pulses[pk[i]];
        if (left <= 0.0)
        {
            continue;
        }
        left -= delta;
        init_pulses[pk[i]] = left;
        set_property(String(pk[i]), left > 0.0 ? 1.0 : 0.0);
        if (left <= 0.0)
        {
            UtilityFunctions::print("JSBGodot: init pulse ", String(pk[i]), " ended");
        }
    }
    // "_links": {dst: src} copies; {dst: {src, factor, offset}} scales (the
    // MD-80's vertical speed indicator reads an autoflight property its Nasal
    // filled from h-dot: linked here as h-dot x 60)
    // an FDM without a gear kinematic channel leaves gear/gear-pos-norm where
    // it was (JSBSim never copies the command into it): [gear] fdm_position
    // false makes PVP move it towards the command over retract_seconds, so
    // the legs retract in the FDM (drag, WOW) and in the model
    if (gear_transit_s > 0.0)
    {
        const double pos = FDMExec->GetPropertyValue("gear/gear-pos-norm");
        const double step = delta / gear_transit_s;
        const double target = (double)gear_cmd;
        const double next = pos < target ? Math::min(pos + step, target) : Math::max(pos - step, target);
        if (next != pos)
        {
            FDMExec->SetPropertyValue("gear/gear-pos-norm", next);
        }
    }
    const Array lk = property_links.keys();
    for (int i = 0; i < lk.size(); i++)
    {
        const Variant spec = property_links[lk[i]];
        if (spec.get_type() == Variant::DICTIONARY)
        {
            const Dictionary d = spec;
            const double v = get_property(String(d.get("src", ""))) * (double)d.get("factor", 1.0) + (double)d.get("offset", 0.0);
            set_property(String(lk[i]), v);
        }
        else
        {
            set_property(String(lk[i]), get_property(String(spec)));
        }
    }
    auto propulsion = FDMExec->GetPropulsion();
    if (!propulsion)
    {
        return;
    }
    for (unsigned int i = 0; i < propulsion->GetNumEngines(); i++)
    {
        const String base = vformat("/controls/engines/engine[%d]/", (int)i);
        auto eng = propulsion->GetEngine(i);
        if (!eng)
        {
            continue;
        }
        if (has_property(base + String("starter")))
        {
            eng->SetStarter(get_property(base + String("starter")) > 0.5);
        }
        if (has_property(base + String("cutoff")))
        {
            if (JSBSim::FGTurbine *turbine = dynamic_cast<JSBSim::FGTurbine *>(eng.get()))
            {
                turbine->SetCutoff(get_property(base + String("cutoff")) > 0.5);
            }
        }
    }
}

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
    elevator_trim = default_pitch_trim;
    elevator_hold = 0.0f;
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
        FDMExec->SetPropertyValue("fcs/pitch-trim-cmd-norm", elevator_trim);
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
    // the node is origin_forward_m ahead of the CG along its nose
    const geo::Geodetic g = geo::geodetic_of(ac_global.origin - ac_global.basis.get_column(2).normalized() * origin_forward_m);
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
        
        // Stick axes come through the Settings > Joystick bindings each frame
        // (apply_joystick); the triggers' incremental throttle stays here.
        if (axis == JOY_AXIS_TRIGGER_RIGHT) // RT - Increase throttle
        {
            trigger_right_value = value;
        }
        else if (axis == JOY_AXIS_TRIGGER_LEFT) // LT - Decrease throttle  
        {
            trigger_left_value = value;
        }
    }
    // Pad buttons (flaps, trim, camera, brakes) go by Settings > Joystick:
    // node_3d.gd polls JoystickConfig each frame (set_trim_buttons, the
    // flap steps); nothing is hard-wired to a button index here any more.
}

void JSBGodot::set_trim_buttons(bool up, bool down)
{
    trim_up_held = up;
    trim_down_held = down;
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
        apply_property_links_and_pulses((double)delta);
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
    // aircraft.cfg [aircraft] base: a variant aircraft carries only what
    // differs (its FDM under jsbsim/aircraft/<model>, maybe a system or two);
    // the engine, systems and aircraft folders it lacks are the base's
    String aircraft_dir = chosen.path_join("aircraft");
    String engine_dir = chosen.path_join("engine");
    String systems_dir = chosen.path_join("systems");
    const String base_id = AircraftConfig::base_id(aircraft_id);
    if (!base_id.is_empty() && tree && tree->get_root()) {
        Node *install_root = tree->get_root()->get_node_or_null(NodePath("InstallRoot"));
        if (install_root) {
            const String base_jsb = String(install_root->call("get_aircraft_dir", base_id)).path_join("jsbsim");
            if (!DirAccess::dir_exists_absolute(aircraft_dir.path_join(jsbsim_model)))
                aircraft_dir = base_jsb.path_join("aircraft");
            if (!DirAccess::dir_exists_absolute(engine_dir))
                engine_dir = base_jsb.path_join("engine");
            if (!DirAccess::dir_exists_absolute(systems_dir))
                systems_dir = base_jsb.path_join("systems");
            UtilityFunctions::print("JSBSim variant of '", base_id, "': aircraft ", aircraft_dir, ", engine ", engine_dir, ", systems ", systems_dir);
        }
    }
    CharString aircraft_dir_cs = aircraft_dir.utf8();
    CharString engine_dir_cs = engine_dir.utf8();
    CharString systems_dir_cs = systems_dir.utf8();
    FDMExec->SetRootDir(SGPath(jsbsim_root));
    FDMExec->SetAircraftPath(SGPath(std::string(aircraft_dir_cs.get_data())));
    FDMExec->SetEnginePath(SGPath(std::string(engine_dir_cs.get_data())));
    FDMExec->SetSystemsPath(SGPath(std::string(systems_dir_cs.get_data())));
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
    // (the variant-aware folders computed above, as std::string via named CharStrings)
    // The step the FDM runs at, set BEFORE the load: every flight-control and
    // system component keeps the dt of its construction (FGFCSComponent), and
    // JSBSim's default is 1/120 s. Loaded at that and stepped at 240 Hz, all
    // actuators, rate limits, lags, timers and controller integrals ran at
    // twice their speed (a 7 s lamp in 3.5 s, the DFGS's 120 s timer in 60 s).
    FDMExec->Setdt(1.0 / FDM_TARGET_HZ);
    if (FDMExec->LoadModel(SGPath(std::string(aircraft_dir_cs.get_data())),
                           SGPath(std::string(engine_dir_cs.get_data())),
                           SGPath(std::string(systems_dir_cs.get_data())),
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
    // Pitch trim goes through JSBSim's own trim command: the conventional
    // aircraft sum it with the elevator at unity (their "Pitch Trim Sum"),
    // the MD-80 drives its stabilizer from it (gery 2026-11-01: the stab sat
    // at 0 deg and the column carried the whole trim). elevator_hold is the
    // residual the airborne trim could not transfer (normally 0).
    FDMExec->SetPropertyValue("fcs/elevator-cmd-norm", input_elevator + elevator_hold);
    FDMExec->SetPropertyValue("fcs/pitch-trim-cmd-norm", elevator_trim);
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
    if (throttle_drives_airbrakes)
    {
        // glider mode: the throttle inputs are the airbrake lever (1 = in, 0 = out)
        FDMExec->SetPropertyValue("fcs/speedbrake-cmd-norm", 1.0 - input_throttle);
        FCS->SetThrottleCmd(-1, 0.0);
    }
    else
    {
        FCS->SetThrottleCmd(-1, input_throttle);
    }


    // Wheel brakes: keys (on/off), the held button and the brake axis, whichever
    // asks for more. JSBSim's command, and the FlightGear pedal properties when
    // the FDM has them: an FDM whose flight controls compute the brake command
    // themselves (the MD-80: pedals, parking brake and autobrake combined)
    // reads those and overrides ours.
    const double brake = Math::max((double)input_brake, Math::max((double)brake_axis, brake_button ? 1.0 : 0.0));
    FDMExec->SetPropertyValue("fcs/left-brake-cmd-norm", brake);
    FDMExec->SetPropertyValue("fcs/right-brake-cmd-norm", brake);
    if (has_property("/controls/gear/brake-left"))
    {
        set_property("/controls/gear/brake-left", brake);
    }
    if (has_property("/controls/gear/brake-right"))
    {
        set_property("/controls/gear/brake-right", brake);
    }

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

    const geo::Geodetic g = geo::geodetic_of(cg_of_node(godot_pos, heading_deg_param));
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

void JSBGodot::initialize_airborne(double terrain_elevation_m, Vector3 godot_pos, double heading_deg_param, double agl_m, double airspeed_kts)
{
    if (!FDMExec) {
        UtilityFunctions::print("Cannot initialize_airborne: FDMExec not created");
        return;
    }
    const double terrain_ft = terrain_elevation_m * 3.28084;
    const geo::Geodetic g = geo::geodetic_of(cg_of_node(godot_pos, heading_deg_param));
    UtilityFunctions::print("Initializing JSBSim airborne at lat=", g.lat, " lon=", g.lon,
        " terrain=", terrain_elevation_m, " m, ", agl_m, " m above it, heading=", heading_deg_param, " airspeed=", airspeed_kts, " kts");
    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
    // The IC object remembers the previous flight (a teleport in a crosswind
    // left beta at -0.7 deg, and SetVcalibratedKtsIC keeps alpha/beta/wind);
    // the longitudinal trim does not touch them and the glider was released
    // sideslipping and rolled off. Start from a clean IC every time.
    ic->InitializeIC();
    ic->SetTerrainElevationFtIC(terrain_ft);
    ic->SetAltitudeASLFtIC(terrain_ft + agl_m * 3.28084);
    ic->SetGeodLatitudeDegIC(g.lat);
    ic->SetLongitudeDegIC(g.lon);
    ic->SetThetaDegIC(0.0);
    ic->SetPhiDegIC(0.0);
    ic->SetPsiDegIC(heading_deg_param);
    ic->SetVcalibratedKtsIC(Math::max(airspeed_kts, 1.0));
    const bool glider = FDMExec->GetPropulsion()->GetNumEngines() == 0;
    if (glider) {
        ic->SetFlightPathAngleDegIC(-2.0); // a glide: the trim balances alpha and elevator along it
    }
    // The wind model is not part of the IC: it keeps the weather's wind, and
    // its MIL-spec turbulence draws random gusts on every run -- also during
    // the trim iterations, which then converged on a random sideslip (beta
    // -0.4..-0.7 deg, a 33 deg bank 15 s after release). Trim in still air;
    // the next physics frame applies the weather again.
    if (std::shared_ptr<JSBSim::FGWinds> winds = FDMExec->GetWinds()) {
        winds->SetWindNED(0.0, 0.0, 0.0);
        winds->SetTurbType(JSBSim::FGWinds::ttNone);
        winds->SetProbabilityOfExceedence(0);
    }
    FDMExec->RunIC();
    FDMExec->SetPropertyValue("propulsion/set-running", -1);
    FDMExec->SetPropertyValue("fcs/brake-cmd-norm", 0.0);
    FDMExec->SetPropertyValue("fcs/parking-brake-cmd-norm", 0.0);
    FDMExec->SetPropertyValue("fcs/mixture-cmd-norm", input_mixture);
    trim_longitudinal();
    jsbsim_initialized = true;
    UtilityFunctions::print("JSBSim initialized airborne");
}

// An engine's feed tanks that are pipes (a few pounds, kept full by the
// aircraft's fuel system in flight) start full: JSBSim starves an engine
// whose feed tank is empty on its very first step, before the systems that
// fill the pipe have come alive, and a starved turbine is cut off for good.
void JSBGodot::top_up_feed_tanks()
{
    if (!FDMExec)
    {
        return;
    }
    auto prop = FDMExec->GetPropulsion();
    for (unsigned int e = 0; e < prop->GetNumEngines(); e++)
    {
        auto eng = prop->GetEngine(e);
        for (unsigned int k = 0; k < eng->GetNumSourceTanks(); k++)
        {
            const int ti = eng->GetSourceTank(k);
            if (ti < 0 || ti >= (int)prop->GetNumTanks())
            {
                continue;
            }
            auto tank = prop->GetTank(ti);
            if (tank->GetCapacity() <= 100.0 && tank->GetContents() < tank->GetCapacity())
            {
                tank->SetContents(tank->GetCapacity());
            }
        }
    }
}

// The FlightGear engine interface the aircraft systems read (/engines/engine[n]/n1, n2,
// running), from JSBSim's engines: what FGBridge publishes every frame, for the
// moments the FDM runs without the bridge (the airborne trim).
void JSBGodot::mirror_engines_to_fg()
{
    if (!FDMExec)
    {
        return;
    }
    const int n = (int)FDMExec->GetPropulsion()->GetNumEngines();
    for (int i = 0; i < n; i++)
    {
        const double n1 = FDMExec->GetPropertyValue(vformat("propulsion/engine[%d]/n1", i).utf8().get_data());
        const double n2 = FDMExec->GetPropertyValue(vformat("propulsion/engine[%d]/n2", i).utf8().get_data());
        const String base = vformat("/engines/engine[%d]/", i);
        if (has_property(base + String("n1")))
        {
            set_property(base + String("n1"), n1);
        }
        if (has_property(base + String("n2")))
        {
            set_property(base + String("n2"), n2);
        }
        if (has_property(base + String("running")))
        {
            set_property(base + String("running"), n2 > 10.0 ? 1.0 : 0.0);
        }
    }
}

bool JSBGodot::trim_longitudinal()
{
    if (!FDMExec) {
        return false;
    }
    // Dynamic trim (gery 2026-11-01). JSBSim's FGTrim evaluates every probe
    // with the time step at zero, so an aircraft whose column, stabilizer or
    // engines answer through filters, actuators and spool dynamics (the MD-80)
    // shows it no response and the solve diverges. Here the FDM runs at its
    // real rate with the vehicle state pinned to the initial conditions after
    // every step: the actuators and engines settle while the aircraft stays
    // put, and the accelerations read after a run are the steady answer to the
    // controls. A secant per axis then nulls them: alpha for the vertical
    // acceleration, the throttles for the longitudinal one, the pitch trim
    // command for the pitch acceleration (so the stick is centred afterwards;
    // the conventional aircraft sum trim and elevator at unity, the MD-80
    // moves its stabilizer). A residual the trim cannot take (its stabilizer
    // at the stop) stays on the elevator (elevator_hold).
    const auto t_start = std::chrono::steady_clock::now();
    // the FDM is held until the flight is released: held models skip their
    // work, so the hold is lifted for the solve and put back afterwards
    const bool was_holding = FDMExec->Holding();
    FDMExec->Resume();
    // trim status: JSBSim skips fuel consumption (no starvation while the
    // aircraft systems that feed the engines are still coming alive)
    FDMExec->SetTrimStatus(true);
    const bool glider = FDMExec->GetPropulsion()->GetNumEngines() == 0;
    std::shared_ptr<JSBSim::FGInitialCondition> ic = FDMExec->GetIC();
    auto propagate = FDMExec->GetPropagate();
    auto fcs = FDMExec->GetFCS();
    const double gamma = ic->GetFlightPathAngleDegIC();
    top_up_feed_tanks();
    // the init pulses (the MD-80's autoconfig flag: stabilizer at 20 deg/s
    // instead of 1/3) stay on while the aircraft is set up
    // (the same flag zeroes the MD-80's rated thrust, as FlightGear's setup
    // phase does: the throttle axis is evaluated with it off)
    const Array pk = init_pulses.keys();
    auto set_pulses = [&](double v) {
        for (int i = 0; i < pk.size(); i++) {
            set_property(String(pk[i]), v);
        }
    };
    set_pulses(1.0);
    FDMExec->Setdt(1.0 / FDM_TARGET_HZ);
    long steps_run = 0;
    // real time steps with the vehicle pinned to the IC: actuators, filters
    // and the aircraft systems advance (the engine interface the systems
    // read is mirrored by hand, the bridge is not running yet); then one
    // step with the time step at zero, in which JSBSim's turbines give
    // their static thrust for the throttle setting
    auto run_pinned = [&](int steps) {
        for (int i = 0; i < steps; i++) {
            FDMExec->Run();
            propagate->SetInitialState(ic.get());
            mirror_engines_to_fg();
        }
        FDMExec->SuspendIntegration();
        FDMExec->Run();
        FDMExec->ResumeIntegration();
        propagate->SetInitialState(ic.get());
        steps_run += steps + 1;
    };
    auto acc = [&](const char *name) { return FDMExec->GetPropertyValue(name); };
    double alpha = ic->GetAlphaDegIC();
    double throttle = glider ? 0.0 : Math::clamp((double)input_throttle, 0.0, 1.0);
    double trim = elevator_trim;
    double hold = 0.0;
    auto set_alpha = [&](double a) {
        ic->SetAlphaDegIC(a);
        ic->SetFlightPathAngleDegIC(gamma);
        propagate->SetInitialState(ic.get());
    };
    // the throttle: JSBSim's command for every engine, and the FlightGear-side
    // levers the mirror normally feeds (an FDM whose systems derive the engine
    // command from /controls/engines/engine[n]/throttle, the MD-80's thrust
    // limiter, would otherwise not see the trim's throttle at all)
    const int n_eng = (int)FDMExec->GetPropulsion()->GetNumEngines();
    auto set_throttle = [&](double t) {
        fcs->SetThrottleCmd(-1, t);
        for (int i = 0; i < n_eng; i++) {
            const String lever = vformat("/controls/engines/engine[%d]/throttle", i);
            if (has_property(lever)) {
                set_property(lever, t);
            }
        }
    };
    // the trim: JSBSim's command, and the FlightGear-side property the mirror
    // normally derives from it (the MD-80's stabilizer reads that one)
    auto set_trim = [&](double t) {
        FDMExec->SetPropertyValue("fcs/pitch-trim-cmd-norm", t);
        FDMExec->SetPropertyValue("fcs/elevator-cmd-norm", hold);
        if (has_property("/controls/flight/elevator-trim")) {
            set_property("/controls/flight/elevator-trim", t);
        }
    };
    auto set_hold = [&](double e) {
        hold = e;
        FDMExec->SetPropertyValue("fcs/elevator-cmd-norm", e);
    };
    // one control against one acceleration: secant from x and x + step0
    auto secant = [&](double &x, const std::function<void(double)> &setf, const char *accname, int steps, double tol, double lo, double hi, double step0) {
        setf(x);
        run_pinned(steps);
        double x0 = x, f0 = acc(accname);
        if (std::fabs(f0) < tol) {
            return f0;
        }
        double x1 = Math::clamp(x0 + step0, lo, hi);
        if (x1 == x0) {
            x1 = Math::clamp(x0 - step0, lo, hi);
        }
        setf(x1);
        run_pinned(steps);
        double f1 = acc(accname);
        for (int k = 0; k < 10 && std::fabs(f1) >= tol; k++) {
            const double d = f1 - f0;
            if (std::fabs(d) < 1e-9) {
                break;
            }
            const double x2 = Math::clamp(x1 - f1 * (x1 - x0) / d, lo, hi);
            if (x2 == x1) {
                break;
            }
            x0 = x1;
            f0 = f1;
            x1 = x2;
            setf(x1);
            run_pinned(steps);
            f1 = acc(accname);
        }
        x = x1;
        return f1;
    };
    // engines running (an earlier frame's cutoff link or a fresh FDM may have
    // them off). JSBSim's starvation flag may be latched from ground frames
    // with empty feed pipes and only clears while fuel is consumed, i.e. with
    // the trim status off: a few steps of that first, then the pipes are
    // refilled and the engines re-lit for the solve.
    set_trim(trim);
    if (!glider) {
        set_throttle(throttle);
        FDMExec->SetTrimStatus(false);
        FDMExec->SetPropertyValue("propulsion/set-running", -1);
        run_pinned(40);
        FDMExec->SetTrimStatus(true);
        top_up_feed_tanks();
        FDMExec->SetPropertyValue("propulsion/set-running", -1);
    }
    // a moment for the aircraft systems to settle around the running engines
    run_pinned(480);
    const double TOL_W = 0.05, TOL_U = 0.05, TOL_Q = 0.002; // ft/s2, ft/s2, rad/s2
    double fw = 1e9, fu = 0.0, fq = 1e9;
    bool ok = false;
    int outer = 0;
    for (; outer < 6 && !ok; outer++) {
        fw = secant(alpha, set_alpha, "accelerations/wdot-ft_sec2", 60, TOL_W, -5.0, 15.0, 1.0);
        if (!glider) {
            set_pulses(0.0);
            fu = secant(throttle, set_throttle, "accelerations/udot-ft_sec2", 480, TOL_U, 0.0, 1.0, 0.1);
            set_pulses(1.0);
        }
        fq = secant(trim, set_trim, "accelerations/qdot-rad_sec2", 150, TOL_Q, -1.0, 1.0, 0.05);
        // re-check all three after the last move, setup flag off as in flight
        set_pulses(0.0);
        run_pinned(60);
        set_pulses(1.0);
        fw = acc("accelerations/wdot-ft_sec2");
        fu = glider ? 0.0 : acc("accelerations/udot-ft_sec2");
        fq = acc("accelerations/qdot-rad_sec2");
        ok = std::fabs(fw) < TOL_W && std::fabs(fu) < TOL_U && std::fabs(fq) < TOL_Q;
        if (ok) {
            ok = false; // the refinement below judges in the flight condition
            outer++;
            UtilityFunctions::print("JSBSim trim round ", outer, ": coarse solution alpha ", alpha, " throttle ", throttle, " trim ", trim);
            break;
        }
        UtilityFunctions::print("JSBSim trim round ", outer + 1, ": alpha ", alpha, " throttle ", throttle, " trim ", trim, " -> w/u/q ", fw, "/", fu, "/", fq,
            " (thrust ", acc("propulsion/engine[0]/thrust-lbs"), " lb, n2 ", acc("propulsion/engine[0]/n2"), ", throttle pos ", acc("fcs/throttle-pos-norm[0]"),
            ", stab ", has_property("/systems/fcs/stabilizer/final-deg") ? get_property("/systems/fcs/stabilizer/final-deg") : 0.0, ")");
    }
    // refinement with the setup flag off throughout, as in flight: the thrust
    // line's pitching moment enters, the stabilizer runs at its normal rate
    // (longer runs for the small corrections left)
    set_pulses(0.0);
    for (int k = 0; k < 4 && !ok; k++, outer++) {
        fw = secant(alpha, set_alpha, "accelerations/wdot-ft_sec2", 60, TOL_W, -5.0, 15.0, 0.5);
        if (!glider) {
            fu = secant(throttle, set_throttle, "accelerations/udot-ft_sec2", 480, TOL_U, 0.0, 1.0, 0.05);
        }
        fq = secant(trim, set_trim, "accelerations/qdot-rad_sec2", 600, TOL_Q, -1.0, 1.0, 0.02);
        run_pinned(120);
        fw = acc("accelerations/wdot-ft_sec2");
        fu = glider ? 0.0 : acc("accelerations/udot-ft_sec2");
        fq = acc("accelerations/qdot-rad_sec2");
        ok = std::fabs(fw) < TOL_W && std::fabs(fu) < TOL_U && std::fabs(fq) < TOL_Q;
        UtilityFunctions::print("JSBSim trim refine ", k + 1, ": alpha ", alpha, " throttle ", throttle, " trim ", trim, " -> w/u/q ", fw, "/", fu, "/", fq,
            " (thrust ", acc("propulsion/engine[0]/thrust-lbs"), " lb, stab ", has_property("/systems/fcs/stabilizer/final-deg") ? get_property("/systems/fcs/stabilizer/final-deg") : 0.0, ")");
    }
    set_pulses(1.0);
    if (!ok && std::fabs(fq) >= TOL_Q) {
        // the trim command is at its stop (the MD-80's stabilizer stops at
        // 2.5 deg nose-down): the elevator takes the rest
        double e = 0.0;
        fq = secant(e, set_hold, "accelerations/qdot-rad_sec2", 150, TOL_Q, -1.0, 1.0, 0.1);
        set_pulses(0.0);
        run_pinned(60);
        set_pulses(1.0);
        fw = acc("accelerations/wdot-ft_sec2");
        fu = glider ? 0.0 : acc("accelerations/udot-ft_sec2");
        fq = acc("accelerations/qdot-rad_sec2");
        ok = std::fabs(fw) < TOL_W && std::fabs(fu) < TOL_U && std::fabs(fq) < TOL_Q;
    }
    FDMExec->RunIC(); // the IC with the final alpha; the actuators keep their state
    FDMExec->SetTrimStatus(false);
    mirror_engines_to_fg();
    // the setup pulses did their work during the solve (a minute of FDM
    // time): a short tail only, so the flight gets its thrust at once; a pulse
    // that had already ended is put back to 0 here (the per-frame step no
    // longer touches it)
    for (int i = 0; i < pk.size(); i++) {
        const double left = Math::min((double)init_pulses[pk[i]], 0.3);
        init_pulses[pk[i]] = left;
        set_property(String(pk[i]), left > 0.0 ? 1.0 : 0.0);
    }
    if (was_holding) {
        FDMExec->Hold();
    }
    elevator_trim = CLAMP((float)trim, -1.0f, 1.0f);
    elevator_hold = CLAMP((float)hold, -1.0f, 1.0f);
    if (!glider) {
        input_throttle = CLAMP((float)throttle, 0.0f, 1.0f);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
    UtilityFunctions::print("JSBSim trim: ", ok ? "steady" : "NOT converged", " after ", outer, " rounds, ", (int64_t)steps_run, " pinned steps, ", (int)ms, " ms (pitch trim ",
        elevator_trim, ", elevator held ", elevator_hold, ", throttle ", input_throttle, ", alpha ", alpha, " deg, residual w/u/q ", fw, "/", fu, "/", fq,
        ", stabilizer ", has_property("/systems/fcs/stabilizer/final-deg") ? get_property("/systems/fcs/stabilizer/final-deg") : 0.0,
        " deg, beta ", FDMExec->GetPropertyValue("aero/beta-deg"), " deg, phi ", FDMExec->GetPropertyValue("attitude/phi-deg"),
        " deg, wind N/E fps ", FDMExec->GetPropertyValue("atmosphere/wind-north-fps"), "/", FDMExec->GetPropertyValue("atmosphere/wind-east-fps"),
        ", turb ", FDMExec->GetPropertyValue("atmosphere/turb-type"), ", vc ", FDMExec->GetPropertyValue("velocities/vc-kts"), " kt)");
    return ok;
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
    pos += bx.normalized() * origin_forward_m; // the node is ahead of the CG along the nose
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
