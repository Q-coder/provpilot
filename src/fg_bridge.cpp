#include "fg_bridge.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/time.hpp>

#include <cmath>

#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

// FlightGear A-10-base.xml <aoa-indexer> thresholds (deg alpha)
static const double AOA_TOO_SLOW = 9.5, AOA_SLOW = 8.5, AOA_FAST = 7.5, AOA_TOO_FAST = 6.5;
// JSBSim oleo travel used to normalise compression-ft to FlightGear's 0..1.
static const double OLEO_TRAVEL_FT[3] = {1.0, 1.3, 1.3};

static String idx(const char *fmt, int i)
{
	char buf[128];
	snprintf(buf, sizeof(buf), fmt, i);
	return String(buf);
}

void FGBridge::_bind_methods()
{
	ClassDB::bind_static_method("FGBridge", D_METHOD("switch_defaults"), &FGBridge::switch_defaults);
	ClassDB::bind_static_method("FGBridge", D_METHOD("switch_default", "prop"), &FGBridge::switch_default);
	ClassDB::bind_method(D_METHOD("collect", "jsb", "airborne", "night"), &FGBridge::collect, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("advance_canopy", "delta"), &FGBridge::advance_canopy);

#define FGB_PROP(type, name, getter, setter)                                                   \
	ClassDB::bind_method(D_METHOD(#getter), &FGBridge::getter);                              \
	ClassDB::bind_method(D_METHOD(#setter, "value"), &FGBridge::setter);                     \
	ADD_PROPERTY(PropertyInfo(type, #name), #setter, #getter);
	FGB_PROP(Variant::FLOAT, canopy_norm, get_canopy_norm, set_canopy_norm)
	FGB_PROP(Variant::BOOL, landing_light, get_landing_light, set_landing_light)
	FGB_PROP(Variant::BOOL, taxi_light, get_taxi_light, set_taxi_light)
	FGB_PROP(Variant::BOOL, cockpit_view, get_cockpit_view, set_cockpit_view)
	FGB_PROP(Variant::FLOAT, dc_volts, get_dc_volts, set_dc_volts)
	FGB_PROP(Variant::FLOAT, ac_volts, get_ac_volts, set_ac_volts)
	FGB_PROP(Variant::FLOAT, g_max, get_g_max, set_g_max)
	FGB_PROP(Variant::FLOAT, g_min, get_g_min, set_g_min)
	FGB_PROP(Variant::FLOAT, sim_time_seconds, get_sim_time_seconds, set_sim_time_seconds)
	FGB_PROP(Variant::DICTIONARY, control_state, get_control_state, set_control_state)
	FGB_PROP(Variant::FLOAT, canopy_target, get_canopy_target, set_canopy_target)
#undef FGB_PROP
}

FGBridge::FGBridge()
{
}

double FGBridge::get_canopy_norm() const { return canopy_norm; }
void FGBridge::set_canopy_norm(double v) { canopy_norm = v; }
bool FGBridge::get_landing_light() const { return landing_light; }
void FGBridge::set_landing_light(bool v) { landing_light = v; }
bool FGBridge::get_taxi_light() const { return taxi_light; }
void FGBridge::set_taxi_light(bool v) { taxi_light = v; }
bool FGBridge::get_cockpit_view() const { return cockpit_view; }
void FGBridge::set_cockpit_view(bool v) { cockpit_view = v; }
double FGBridge::get_dc_volts() const { return dc_volts; }
void FGBridge::set_dc_volts(double v) { dc_volts = v; }
double FGBridge::get_ac_volts() const { return ac_volts; }
void FGBridge::set_ac_volts(double v) { ac_volts = v; }
double FGBridge::get_g_max() const { return g_max; }
void FGBridge::set_g_max(double v) { g_max = v; }
double FGBridge::get_g_min() const { return g_min; }
void FGBridge::set_g_min(double v) { g_min = v; }
double FGBridge::get_sim_time_seconds() const { return sim_time_seconds; }
void FGBridge::set_sim_time_seconds(double v) { sim_time_seconds = v; }
Dictionary FGBridge::get_control_state() const { return control_state; }
void FGBridge::set_control_state(const Dictionary &v) { control_state = v; }
double FGBridge::get_canopy_target() const { return canopy_target; }
void FGBridge::set_canopy_target(double v) { canopy_target = v; }

Dictionary FGBridge::switch_defaults()
{
	Dictionary d;
	d["controls/electric/battery-switch"] = 1.0;
	d["controls/electric/engine/generator"] = 1.0;
	d["controls/electric/engine[1]/generator"] = 1.0;
	d["sim/model/A-10/controls/canopy/canopy-switch"] = 1.0;
	d["controls/lighting/panel-int"] = 0.0;
	d["controls/gear/brake-parking"] = 1.0; // every flight starts with the parking brake set
	// generic light switches (Cessna panel) and the yoke visibility toggle
	d["controls/lighting/beacon"] = 1.0;
	d["controls/lighting/nav-lights"] = 1.0;
	d["controls/lighting/landing-lights"] = 0.0;
	d["controls/lighting/taxi-light"] = 0.0;
	d["sim/model/c172p/yokes-visible"] = 1.0;
	return d;
}

double FGBridge::switch_default(const String &prop)
{
	const Dictionary d = switch_defaults();
	return d.has(prop) ? (double)d[prop] : 0.0;
}

double FGBridge::ctl(const char *key, double def) const
{
	const String k(key);
	return control_state.has(k) ? (double)control_state[k] : def;
}

Dictionary FGBridge::collect(Object *p_jsb, bool airborne, double night)
{
	JSBGodot *jsb = Object::cast_to<JSBGodot>(p_jsb);
	const double n = Math::clamp(night, 0.0, 1.0);
	Dictionary p;
	// renderer flags the model tests
	p["sim/rendering/rembrandt/enabled"] = 0;
	p["A-10/done"] = 0;
	p["sim/variant-id"] = 0;
	// view
	p["sim/current-view/name"] = cockpit_view ? "Cockpit View" : "Chase View";
	p["sim/current-view/view-number"] = cockpit_view ? 0 : 1;
	p["sim/current-view/internal"] = cockpit_view ? 1 : 0;
	// sound set: warning audio volume, no rain / weapons audio yet
	p["sim/model/A-10/instrumentation/warnings/audio-alt-volume"] = 1.0;
	p["sim/model/A-10/instrumentation/warnings/audio-alt"] = 0.0;
	p["environment/rain-norm"] = 0.0;
	p["payload/armament/aim-9m/sound-volume"] = 0.0;
	p["controls/armament/trigger-gun"] = 0.0;
	// canopy and lights (host state)
	p["canopy/position-norm"] = canopy_norm;
	p["systems/electrical/outputs/landing-light"] = landing_light ? 1.0 : 0.0;
	p["systems/electrical/outputs/taxi-light"] = taxi_light ? 1.0 : 0.0;
	// light switches: 0 taxi / 1 off / 2 landing; nav 0 off / 1 steady / 2 flash
	p["sim/model/A-10/controls/lighting/land-lights-switch"] = landing_light ? 2.0 : 1.0;
	p["sim/model/A-10/controls/lighting/nav-lights-switch"] = 1.0;
	p["controls/lighting/strobe"] = 1.0;
	p["sim/model/A-10/controls/lighting/formation"] = 0.0;
	// FlightGear's panel / instrument lighting knobs (0..1): the material
	// emission at 1.0 floods the panel orange, so dusk sets 0.6 unless the
	// cockpit knobs were turned (control_state wins below)
	p["controls/lighting/panel-norm"] = n * 0.6;
	p["controls/lighting/instruments-norm"] = n * 0.6;
	p["controls/lighting/panel-int"] = n * 0.6;
	// fgdata generic instruments light their faces from this factor (ai.xml
	// spells the path with a double slash)
	p["sim/model/material/instruments/factor"] = n * 0.6;
	p["sim/model//material/instruments/factor"] = n * 0.6;
	// chronometer: the simulated time of day (what the HUD shows), else system UTC
	if (sim_time_seconds >= 0.0)
	{
		const int secs = (int)Math::fposmod(sim_time_seconds, 86400.0);
		p["sim/time/utc/hour"] = secs / 3600;
		p["sim/time/utc/minute"] = (secs / 60) % 60;
		p["sim/time/utc/second"] = secs % 60;
	}
	else
	{
		const Dictionary t = Time::get_singleton()->get_time_dict_from_system(true);
		p["sim/time/utc/hour"] = (int)t.get("hour", 0);
		p["sim/time/utc/minute"] = (int)t.get("minute", 0);
		p["sim/time/utc/second"] = (int)t.get("second", 0);
	}
	// AoA indexer thresholds
	p["sim/aoa-indexer/too-slow-deg"] = AOA_TOO_SLOW;
	p["sim/aoa-indexer/slow-deg"] = AOA_SLOW;
	p["sim/aoa-indexer/fast-deg"] = AOA_FAST;
	p["sim/aoa-indexer/too-fast-deg"] = AOA_TOO_FAST;
	systems_healthy(p);
	avionics_idle(p);
	electrical(p, jsb);
	if (jsb == nullptr)
	{
		return p;
	}
	surfaces(jsb, p);
	gear(jsb, p, airborne);
	engines(jsb, p);
	fuel(jsb, p);
	flight_instruments(jsb, p, airborne);
	controls(jsb, p);
	// cockpit switches and knobs (FGControls) override the computed values
	const Array keys = control_state.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		p[keys[i]] = control_state[keys[i]];
	}
	if (dc_volts <= 0.0)
	{
		p["controls/lighting/panel-norm"] = 0.0;
		p["controls/lighting/instruments-norm"] = 0.0;
		p["controls/lighting/panel-int"] = 0.0;
	}
	// HSI heading-set knob offsets the compass card
	const double hsi_off = ctl("instrumentation/heading-indicator-fg/offset-deg", 0.0);
	if (hsi_off != 0.0)
	{
		p["instrumentation/heading-indicator-fg/indicated-heading-deg"] =
				Math::fposmod((double)p["instrumentation/heading-indicator-fg/indicated-heading-deg"] + hsi_off, 360.0);
	}
	return p;
}

void FGBridge::advance_canopy(double delta)
{
	if (canopy_target > 1.5)
	{
		canopy_norm = Math::min(canopy_norm + delta * 0.1, 1.0);
	}
	else if (canopy_target < 0.5)
	{
		canopy_norm = Math::max(canopy_norm - delta * 0.1, 0.0);
	}
}

// Light electrical model: the battery switch and the two engine generators
// (cockpit switches) feed one DC bus; an engine's generator is on line when
// its switch is on and N2 is above 50 % (a piston engine: rpm above idle).
// The inverter makes AC from DC. Every powered output the model tests
// (`> 24`) follows the bus; the generator caution lights and the master
// caution come from generators off line.
void FGBridge::electrical(Dictionary &p, JSBGodot *jsb)
{
	const bool battery = ctl("controls/electric/battery-switch", 1.0) > 0.5;
	bool gen_on = false;
	double gen_caution[ENGINES] = {0.0, 0.0};
	for (int i = 0; i < ENGINES; i++)
	{
		const bool sw = ctl(i == 0 ? "controls/electric/engine/generator" : "controls/electric/engine[1]/generator", 1.0) > 0.5;
		const double n2 = jsb != nullptr ? jsb->get_property(idx("propulsion/engine[%d]/n2", i)) : 100.0;
		const double rpm = jsb != nullptr ? jsb->get_property(idx("propulsion/engine[%d]/engine-rpm", i)) : 0.0;
		const bool online = sw && (n2 > 50.0 || rpm > 400.0);
		gen_on = gen_on || online;
		gen_caution[i] = online ? 0.0 : 1.0;
	}
	dc_volts = (gen_on || battery) ? 28.0 : 0.0;
	ac_volts = dc_volts > 0.0 ? 115.0 : 0.0;
	static const char *const outs[] = {"adi", "hsi", "aoa-indexer", "caution-panel", "hud", "ldg-warning-system",
			"nav-mode", "rwr", "gau-8", "fuel-gauge-sel"};
	for (const char *out : outs)
	{
		p[String("systems/electrical/outputs/") + out] = dc_volts;
	}
	p["systems/electrical/R-AC-volts"] = ac_volts;
	p["systems/A-10-electrical/l-generator-caution-light"] = gen_caution[0];
	p["systems/A-10-electrical/r-generator-caution-light"] = gen_caution[1];
	// the model compares the master caution with the string "true"
	p["sim/model/A-10/instrumentation/warnings/master-caution"] = dc_volts > 0.0 && (gen_caution[0] > 0.5 || gen_caution[1] > 0.5);
	// nothing lights without power: exterior lights, panel lighting
	if (dc_volts <= 0.0)
	{
		p["systems/electrical/outputs/landing-light"] = 0.0;
		p["systems/electrical/outputs/taxi-light"] = 0.0;
		p["controls/lighting/panel-norm"] = 0.0;
		p["controls/lighting/instruments-norm"] = 0.0;
		p["controls/lighting/panel-int"] = 0.0;
	}
}

// System values the caution panel and gauges read: everything serviceable.
void FGBridge::systems_healthy(Dictionary &p)
{
	for (int i = 0; i < 2; i++)
	{
		p[idx("systems/A-10-hydraulics/hyd-psi[%d]", i)] = 3000.0;
		p[idx("systems/A-10-hydraulics/hyd-res[%d]", i)] = 100.0;
		p[idx("systems/A-10-fuel/feed-line-press[%d]", i)] = 30.0;
		p[idx("systems/bleed-air/ats-valve[%d]", i)] = 0.0;
	}
	for (int i = 0; i < 4; i++)
	{
		p[idx("systems/A-10-fuel/tank[%d]/boost-pump", i)] = 1.0;
		p[idx("controls/fuel/tank[%d]/fill-dis", i)] = 0.0;
	}
	p["systems/A-10-oxygen/oxy-level"] = 5.0;
	static const char *const warns[] = {"systems/A-10-alpha-mach-computer/stall-sys-caution-light", "systems/A-10-bleed-air/warn-light",
			"systems/A-10-cadc/cadc-sys-caution-light", "systems/A-10-ejectable-seat/armed",
			"systems/A-10-electrical/apu-gen-caution-light", "systems/A-10-electrical/instrument-inverter-caution-light",
			"systems/A-10-electrical/l-converter-failure",
			"systems/A-10-electrical/r-converter-failure",
			"systems/A-10-iff/iff-warn-light", "systems/A-10-inertial-navigation-unit/inu-caution-light",
			"systems/A-10-inertial-navigation-unit/inu-hot-warn-light", "systems/A-10-service-air/service-air-warn-light",
			"sim/model/A-10/A-10-nav/hars/hars-warn-light",
			"systems/refuel/state", "sim/model/A-10/systems/apu/rpm-norm", "sim/model/A-10/systems/apu/temp",
			"controls/APU/off-start-switch", "sim/model/A-10/weapons/gun/gun-unsafe"};
	for (const char *w : warns)
	{
		p[String(w)] = 0.0;
	}
	p["controls/gear/antiskid"] = 1.0;
}

// Radios, navigation and weapons idle: needles centred, flags shown, nothing armed.
void FGBridge::avionics_idle(Dictionary &p)
{
	static const char *const zeros[] = {"instrumentation/nav/from-flag", "instrumentation/nav/to-flag", "instrumentation/nav/in-range",
			"instrumentation/nav/gs-needle-deflection", "instrumentation/nav/heading-needle-deflection",
			"instrumentation/nav[1]/radials/selected-deg", "instrumentation/nav[2]/in-range", "instrumentation/nav[2]/nav-distance",
			"instrumentation/tacan/in-range", "instrumentation/tacan/indicated-distance-nm", "instrumentation/tacan/switch-position",
			"sim/model/A-10/instrumentation/tacan/indicated-bearing-mag-deg",
			"sim/model/A-10/A-10-nav/selector-hars", "sim/model/A-10/A-10-nav/selector-ils", "sim/model/A-10/A-10-nav/selector-man",
			"sim/model/A-10/A-10-nav/selector-nav_crs", "sim/model/A-10/A-10-nav/selector-ptr", "sim/model/A-10/A-10-nav/selector-tils",
			"sim/model/A-10/A-10-nav/selector-vlocs", "sim/model/A-10/instrumentation/cdi/from-flag",
			"sim/model/A-10/instrumentation/cdi/needle-deflection", "sim/model/A-10/instrumentation/cdi/to-flag",
			"sim/model/A-10/instrumentation/vhf/homing-deviation", "sim/model/A-10/instrumentation/vhf/mode",
			"sim/model/A-10/instrumentation/vhf/selector", "sim/model/A-10/instrumentation/nav/frequencies/freq-whole",
			"instrumentation/comm/frequencies/selected-mhz", "controls/armament/master-arm", "controls/armament/ripple",
			"controls/armament/ripple-delay", "A-10/avionics/ew-rwr-switch", "A-10/displays/cmsc/cmsc-disp-int",
			"A-10/stores/aim9-light", "A-10/stores/ecm-mounted-left", "A-10/stores/ecm-mounted-right",
			"A-10/stores/tgp-mounted-left", "A-10/stores/tgp-mounted-right", "sim/model/A-10/weapons/wingtankC",
			"sim/model/A-10/weapons/wingtankL", "sim/model/A-10/weapons/wingtankR", "ai/submodels/submodel[1]/count",
			"sim/model/A-10/controls/hud/depr", "sim/model/A-10/controls/hud/mode-selector",
			"sim/model/A-10/instrumentation/hud/ccip_dev_h_m", "sim/model/A-10/instrumentation/hud/ccip_dev_v_m",
			"sim/model/A-10/controls/sas/pitch-sas-switch", "sim/model/A-10/controls/fuel/fuel-test-ind",
			"sim/model/A-10/controls/fuel/fuel-dsp-sel"};
	for (const char *k : zeros)
	{
		p[String(k)] = 0.0;
	}
	for (int i = 1; i < 5; i++)
	{
		p[idx("instrumentation/tacan/frequencies/selected-channel[%d]", i)] = 0.0;
	}
	for (int i = 0; i < 12; i++)
	{
		p[idx("A-10/stations/station[%d]/selected", i)] = 0.0;
		p[idx("payload/armament/station/id-%d-count", i)] = 0.0;
		p[idx("payload/armament/station/id-%d-type", i)] = "";
	}
	p["sim/hud/color/alpha"] = 1.0;
	p["sim/model/A-10/controls/hud/intens"] = 1.0;
}

void FGBridge::surfaces(JSBGodot *jsb, Dictionary &p)
{
	// control surfaces: JSBSim positions are already normalised
	const double rudder = jsb->get_property("fcs/rudder-pos-norm");
	const double elevator = jsb->get_property("fcs/elevator-pos-norm");
	const double flap = jsb->get_property("fcs/flap-pos-norm");
	const double speedbrake = jsb->get_property("fcs/speedbrake-pos-norm");
	// The model rotates the right aileron with a mirrored axis and a negated
	// factor, i.e. it expects right-aileron-pos-norm = -left (FlightGear's
	// convention); the A-10 FDM outputs the same sign for both, so derive the
	// right side from the left.
	double ail_l = jsb->get_property("fcs/left-aileron-pos-norm");
	if (std::abs(ail_l) < 1e-9)
	{
		ail_l = jsb->get_property("fcs/aileron-pos-norm");
	}
	p["surface-positions/rudder-pos-norm"] = rudder;
	p["surface-positions/left-elevator-pos-norm"] = elevator;
	p["surface-positions/right-elevator-pos-norm"] = elevator;
	p["surface-positions/left-aileron-pos-norm"] = ail_l;
	p["surface-positions/right-aileron-pos-norm"] = -ail_l;
	p["surface-positions/left-flap-pos-norm"] = flap;
	p["surface-positions/right-flap-pos-norm"] = flap;
	p["surface-positions/left-spoiler-pos-norm"] = speedbrake;
	p["surface-positions/right-spoiler-pos-norm"] = speedbrake;
}

void FGBridge::gear(JSBGodot *jsb, Dictionary &p, bool airborne)
{
	const double ground_speed_ms = jsb->get_property("velocities/vg-fps") * 0.3048;
	// JSBSim's per-unit gear/unit[n]/pos-norm stays at 1; the retract
	// kinematic (Systems/Landing Gear.xml) drives gear/gear-pos-norm for all legs
	const double gear_pos_all = Math::clamp((double)jsb->get_property("gear/gear-pos-norm"), 0.0, 1.0);
	for (int i = 0; i < GEAR_UNITS; i++)
	{
		p[idx("gear/gear[%d]/position-norm", i)] = gear_pos_all;
		const double comp_ft = jsb->get_property(idx("gear/unit[%d]/compression-ft", i));
		p[idx("gear/gear[%d]/compression-norm", i)] = Math::clamp(comp_ft / OLEO_TRAVEL_FT[i], 0.0, 1.0);
		const double wow = jsb->get_property(idx("gear/unit[%d]/WOW", i));
		const double wheel_fps = jsb->get_property(idx("gear/unit[%d]/wheel-speed-fps", i));
		const double roll_ms = std::abs(wheel_fps) > 1e-6 ? wheel_fps * 0.3048 : (wow > 0.5 ? ground_speed_ms : 0.0);
		p[idx("gear/gear[%d]/rollspeed-ms", i)] = roll_ms;
		p[idx("gear/gear[%d]/wow", i)] = wow > 0.5 ? 1.0 : 0.0;
	}
	p["gear/gear/rollspeed-ms"] = p["gear/gear[0]/rollspeed-ms"];
	p["velocities/groundspeed-kt"] = ground_speed_ms * 1.943844;
	p["velocities/speed-down-fps"] = jsb->get_property("velocities/v-down-fps");
	// real nose-wheel steering angle (FlightGear steers the visual wheel from
	// the rudder, 20 deg max; the manifest override reads this)
	p["gear/gear[0]/steering-deg"] = jsb->get_property("fcs/steer-pos-deg[0]");
	// gear handle: the FlightGear Nasal animates it to 0 for UP and -1 for DOWN
	const bool cmd_down = jsb->get_gear_cmd() > 0.5;
	p["sim/model/A-10/controls/gear/ld-gear-handle-anim"] = cmd_down ? -1.0 : 0.0;
	p["orientation/alpha-deg"] = airborne ? (double)jsb->get_property("aero/alpha-deg") : 0.0;
}

void FGBridge::engines(JSBGodot *jsb, Dictionary &p)
{
	for (int i = 0; i < ENGINES; i++)
	{
		const double n1 = jsb->get_property(idx("propulsion/engine[%d]/n1", i));
		const double n2 = jsb->get_property(idx("propulsion/engine[%d]/n2", i));
		const double thrust = jsb->get_property(idx("propulsion/engine[%d]/thrust-lbs", i));
		const double ff_pps = jsb->get_property(idx("propulsion/engine[%d]/fuel-flow-rate-pps", i));
		p[idx("sim/model/A-10/engines/engine[%d]/n1", i)] = n1;
		p[idx("engines/engine[%d]/n1", i)] = n1;
		p[idx("engines/engine[%d]/n2", i)] = n2;
		// piston engines (tachometer): JSBSim engine-rpm, 0 for a turbine
		p[idx("engines/engine[%d]/rpm", i)] = jsb->get_property(idx("propulsion/engine[%d]/engine-rpm", i));
		// the model's ITT gauge and hot light read a thrust fraction (its table
		// is keyed 0 .. 0.987 for "max thrust"), not degrees
		const double thrust_norm = Math::clamp(thrust / TF34_MIL_THRUST_LBF, 0.0, 1.0);
		p[idx("engines/engine[%d]/egt-degf", i)] = thrust_norm;
		p[idx("engines/engine[%d]/itt-norm", i)] = thrust_norm * 0.9;
		p[idx("engines/engine[%d]/fuel-flow_pph", i)] = ff_pps * 3600.0;
		p[idx("engines/engine[%d]/thrust-lbs", i)] = thrust; // the sound set spells it both ways
		p[idx("engines/engine[%d]/thrust_lb", i)] = thrust;
		// oil pressure follows the core speed
		p[idx("engines/engine[%d]/oil-pressure-psi", i)] = Math::clamp(n2, 0.0, 100.0) * 0.95;
		p[idx("engines/engine[%d]/oilp-norm", i)] = Math::clamp(n2 / 100.0, 0.0, 1.0);
		p[idx("sim/model/A-10/controls/engines/engine[%d]/throttle", i)] = jsb->get_property(idx("fcs/throttle-cmd-norm[%d]", i));
		p[idx("sim/model/A-10/controls/engines/engine[%d]/starter-switch-position", i)] = 0.0;
	}
}

void FGBridge::fuel(JSBGodot *jsb, Dictionary &p)
{
	double lbs[TANKS];
	double total = 0.0;
	for (int i = 0; i < TANKS; i++)
	{
		lbs[i] = jsb->get_property(idx("propulsion/tank[%d]/contents-lbs", i));
		total += lbs[i];
	}
	// FlightGear tanks: 0 left wing, 1 left main, 2 right main, 3 right wing;
	// the FDM's three tanks feed the left / right displays and the total drum
	const double left = lbs[0] + lbs[1] * 0.5;
	const double right = lbs[2] + lbs[1] * 0.5;
	p["sim/model/A-10/consumables/fuel/fuel-dsp-left"] = left;
	p["sim/model/A-10/consumables/fuel/fuel-dsp-right"] = right;
	p["sim/model/A-10/consumables/fuel/fuel-dsp-drum"] = total;
	p["sim/model/A-10/consumables/fuel/diff-lbs"] = std::abs(left - right);
	p["consumables/fuel/tank[0]/level-gal_us"] = lbs[0] / FUEL_LBS_PER_GAL;
	p["consumables/fuel/tank[1]/level-gal_us"] = lbs[1] * 0.5 / FUEL_LBS_PER_GAL;
	p["consumables/fuel/tank[2]/level-gal_us"] = lbs[1] * 0.5 / FUEL_LBS_PER_GAL;
	p["consumables/fuel/tank[3]/level-gal_us"] = lbs[2] / FUEL_LBS_PER_GAL;
}

void FGBridge::flight_instruments(JSBGodot *jsb, Dictionary &p, bool airborne)
{
	const double vc = jsb->get_property("velocities/vc-kts");
	p["instrumentation/airspeed-indicator/indicated-speed-kt"] = vc;
	p["velocities/airspeed-kt"] = vc;
	p["instrumentation/vertical-speed-indicator/indicated-speed-fpm"] = jsb->get_property("velocities/h-dot-fps") * 60.0;
	// Altimeter: the cockpit knobs turn the Kollsman window (control state;
	// it follows the weather's QNH until the pilot turns the knob, so the
	// instrument reads the field elevation on the ground); the reading is the
	// ISA inversion of JSBSim's static pressure against that setting.
	const double PSF_PER_INHG = 70.7262;
	const String KOLLSMAN = "instrumentation/altimeter/setting-inhg";
	const double qnh_inhg = Math::snapped(jsb->get_weather_qnh_hpa() / 33.8639, 0.01);
	if (!control_state.has(KOLLSMAN) ||
			(std::abs((double)control_state[KOLLSMAN] - kollsman_seeded) < 1e-6 && std::abs(qnh_inhg - kollsman_seeded) > 1e-6))
	{
		control_state[KOLLSMAN] = qnh_inhg;
		kollsman_seeded = qnh_inhg;
	}
	const double setting = control_state[KOLLSMAN];
	const double p_psf = jsb->get_property("atmosphere/P-psf");
	if (p_psf > 1.0 && setting > 1.0)
	{
		p["instrumentation/altimeter/indicated-altitude-ft"] = 145366.45 * (1.0 - std::pow(p_psf / PSF_PER_INHG / setting, 0.190284));
	}
	else
	{
		p["instrumentation/altimeter/indicated-altitude-ft"] = jsb->get_property("position/h-sl-ft");
	}
	p[KOLLSMAN] = setting;
	const double theta = jsb->get_property("attitude/theta-deg");
	const double phi = jsb->get_property("attitude/phi-deg");
	p["orientation/pitch-deg"] = theta;
	p["orientation/roll-deg"] = phi;
	const double hdg = Math::fposmod((double)jsb->get_property("attitude/psi-deg"), 360.0);
	p["instrumentation/heading-indicator-fg/indicated-heading-deg"] = hdg;
	p["instrumentation/magnetic-compass/indicated-heading-deg"] = hdg;
	// fgdata generic instruments (docs/INSTRUMENTS.md): directional gyro with
	// its knob offset (control state), attitude indicator, turn coordinator
	// (1.0 = standard rate, 3 deg/s), gyro spin flags, clock hands
	const double hdg_offset = ctl("instrumentation/heading-indicator/offset-deg", 0.0);
	p["instrumentation/heading-indicator/indicated-heading-deg"] = Math::fposmod(hdg + hdg_offset, 360.0);
	p["instrumentation/heading-indicator/offset-deg"] = hdg_offset;
	p["instrumentation/attitude-indicator/indicated-pitch-deg"] = theta;
	p["instrumentation/attitude-indicator/indicated-roll-deg"] = phi;
	p["instrumentation/attitude-indicator/horizon-offset-deg"] = 0.0;
	p["instrumentation/turn-indicator/indicated-turn-rate"] = Math::clamp(Math::rad_to_deg((double)jsb->get_property("velocities/r-rad_sec")) / 3.0, -3.0, 3.0);
	p["instrumentation/turn-indicator/spin"] = 1.0;
	p["instrumentation/attitude-indicator/spin"] = 1.0;
	p["instrumentation/heading-indicator/spin"] = 1.0;
	if (sim_time_seconds >= 0.0)
	{
		p["sim/time/utc/day-seconds"] = Math::fposmod(sim_time_seconds, 86400.0);
	}
	else
	{
		const double unix = Time::get_singleton()->get_unix_time_from_system();
		p["sim/time/utc/day-seconds"] = unix - 86400.0 * std::floor(unix / 86400.0);
	}
	p["instrumentation/slip-skid-ball/indicated-slip-skid"] = Math::clamp((double)jsb->get_property("accelerations/Ny"), -1.0, 1.0);
	double nz = jsb->get_property("accelerations/Nz");
	if (!airborne)
	{
		nz = 1.0;
	}
	p["accelerations/pilot-g"] = nz;
	g_max = Math::max(g_max, nz);
	g_min = Math::min(g_min, nz);
	p["sim/model/A-10/instrumentation/g-meter/g-max"] = g_max;
	p["sim/model/A-10/instrumentation/g-meter/g-min"] = g_min;
}

void FGBridge::controls(JSBGodot *jsb, Dictionary &p)
{
	p["controls/flight/aileron"] = jsb->get_property("fcs/aileron-cmd-norm");
	p["controls/flight/elevator"] = jsb->get_property("fcs/elevator-cmd-norm");
	p["controls/flight/rudder"] = jsb->get_property("fcs/rudder-cmd-norm");
	// flap lever detent: aircraft.cfg [flaps] positions 0 / 0.5 / 1 -> UP / MVR / DN
	const double flap_cmd = jsb->get_property("fcs/flap-cmd-norm");
	p["sim/flaps/current-setting"] = flap_cmd < 0.25 ? 0 : (flap_cmd < 0.75 ? 1 : 2);
}

} // namespace jsbgodot
