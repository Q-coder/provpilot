#include "fg_bridge.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/time.hpp>

#include <cmath>

#include "jsbgodot.h"
#include "geo_magnetic.h"

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
	FGB_PROP(Variant::FLOAT, frame_delta, get_frame_delta, set_frame_delta)
#undef FGB_PROP
	ClassDB::bind_method(D_METHOD("get_radio_nav"), &FGBridge::get_radio_nav);
	ClassDB::bind_method(D_METHOD("get_radio_nav2"), &FGBridge::get_radio_nav2);
	ClassDB::bind_method(D_METHOD("get_magnetic_variation_deg"), &FGBridge::get_magnetic_variation_deg);
	ClassDB::bind_method(D_METHOD("nav_status_text"), &FGBridge::nav_status_text);
	ClassDB::bind_method(D_METHOD("set_fdm_mirror", "on"), &FGBridge::set_fdm_mirror);
	ClassDB::bind_method(D_METHOD("get_fdm_mirror"), &FGBridge::get_fdm_mirror);
	ClassDB::bind_method(D_METHOD("set_power_property", "prop"), &FGBridge::set_power_property);
	ClassDB::bind_method(D_METHOD("get_power_property"), &FGBridge::get_power_property);
	ClassDB::bind_method(D_METHOD("set_power_threshold", "value"), &FGBridge::set_power_threshold);
	ClassDB::bind_method(D_METHOD("get_power_threshold"), &FGBridge::get_power_threshold);
	ClassDB::bind_method(D_METHOD("set_mirror_pull_keys", "keys"), &FGBridge::set_mirror_pull_keys);
	ClassDB::bind_method(D_METHOD("set_init_defaults", "defaults"), &FGBridge::set_init_defaults);
	ClassDB::bind_method(D_METHOD("set_property_aliases", "aliases"), &FGBridge::set_property_aliases);
	ClassDB::bind_method(D_METHOD("default_for", "prop"), &FGBridge::default_for);
	ClassDB::bind_method(D_METHOD("reset_mirror_cache"), &FGBridge::reset_mirror_cache);
	ClassDB::bind_method(D_METHOD("get_mirror_pushed"), &FGBridge::get_mirror_pushed);
	ClassDB::bind_method(D_METHOD("get_mirror_pulled"), &FGBridge::get_mirror_pulled);
}

FGBridge::FGBridge()
{
	radio_nav.instantiate();
	radio_nav2.instantiate();
	radio_nav2->set_receiver_index(1);
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
Dictionary FGBridge::get_extra_properties() const { return extra_props; }
void FGBridge::set_extra_properties(const Dictionary &v) { extra_props = v; }
double FGBridge::get_canopy_target() const { return canopy_target; }
void FGBridge::set_canopy_target(double v) { canopy_target = v; }
double FGBridge::get_frame_delta() const { return frame_delta; }
void FGBridge::set_frame_delta(double v) { frame_delta = Math::clamp(v, 0.0, 0.5); }
Ref<RadioNav> FGBridge::get_radio_nav() const { return radio_nav; }
Ref<RadioNav> FGBridge::get_radio_nav2() const { return radio_nav2; }
double FGBridge::get_magnetic_variation_deg() const { return magvar_deg; }
String FGBridge::nav_status_text() const { return radio_nav.is_valid() ? radio_nav->status_text() : String(); }
bool FGBridge::get_fdm_mirror() const { return fdm_mirror; }
void FGBridge::set_fdm_mirror(bool on) { fdm_mirror = on; }
String FGBridge::get_power_property() const { return power_property; }
void FGBridge::set_power_property(const String &p) { power_property = p; }
void FGBridge::set_surface_norm_per_deg(double elevator, double rudder)
{
	elevator_norm_per_deg = elevator;
	rudder_norm_per_deg = rudder;
}
double FGBridge::get_nose_steer_max_deg() const { return nose_steer_max_deg; }
void FGBridge::set_nose_steer_max_deg(double v) { nose_steer_max_deg = v; }
double FGBridge::get_power_threshold() const { return power_threshold; }
void FGBridge::set_power_threshold(double v) { power_threshold = v; }
void FGBridge::set_mirror_pull_keys(const PackedStringArray &keys) { pull_keys = keys; }

void FGBridge::set_property_aliases(const Dictionary &d)
{
	property_aliases.clear();
	const Array keys = d.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		String k = String(keys[i]).strip_edges();
		while (k.begins_with("/"))
		{
			k = k.substr(1);
		}
		property_aliases[k.replace("[0]", "")] = d[keys[i]];
	}
}

void FGBridge::set_init_defaults(const Dictionary &d)
{
	init_defaults.clear();
	const Array keys = d.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		String k = String(keys[i]).strip_edges();
		while (k.begins_with("/"))
		{
			k = k.substr(1);
		}
		init_defaults[k.replace("[0]", "")] = d[keys[i]];
	}
}

static String fg_path(const String &key);

Variant FGBridge::initial_value(const String &prop, Object *p_jsb)
{
	if (init_defaults.has(prop))
	{
		return init_defaults[prop];
	}
	JSBGodot *jsb = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm_mirror && jsb != nullptr && fdm_has(jsb, prop))
	{
		return jsb->get_property(fg_path(prop));
	}
	return Variant();
}

double FGBridge::default_for(const String &prop) const
{
	if (init_defaults.has(prop))
	{
		return (double)init_defaults[prop];
	}
	return switch_default(prop);
}
void FGBridge::reset_mirror_cache() { node_cache.clear(); }
int FGBridge::get_mirror_pushed() const { return mirror_pushed; }
int FGBridge::get_mirror_pulled() const { return mirror_pulled; }

// FlightGear-side paths are absolute in the FDM's tree ("/controls/..."):
// standalone JSBSim keeps its own properties under fdm/jsbsim and resolves
// a leading slash from the root, which is where the systems' references go.
// FlightGear's own name for a JSBSim property, "fdm/jsbsim/<path>" (the
// DFGS rules read fdm/jsbsim/aero/beta-deg, the autoflight the flap angle),
// is the bare JSBSim-relative path here.
static String fg_path(const String &key)
{
	if (key.begins_with("/fdm/jsbsim/"))
	{
		return key.substr(12);
	}
	if (key.begins_with("fdm/jsbsim/"))
	{
		return key.substr(11);
	}
	return key.begins_with("/") ? key : String("/") + key;
}

bool FGBridge::fdm_has(JSBGodot *jsb, const String &key)
{
	if (node_cache.has(key))
	{
		return (bool)node_cache[key];
	}
	const bool has = jsb != nullptr && jsb->has_property(fg_path(key));
	node_cache[key] = has;
	return has;
}

// Push: the FlightGear-side inputs the FDM's systems read. Only the roots
// FlightGear itself would provide (controls, instruments, flight state,
// environment, engine interface, options) go in; everything under
// systems/ belongs to the FDM except the few hand-off inputs listed.
static bool mirror_push_key(const String &k)
{
	static const char *const roots[] = {"controls/", "instrumentation/", "orientation/", "position/", "velocities/",
			"accelerations/", "environment/", "sim/time/", "sim/model/", "gear/", "services/", "it-autoflight/", "options/",
			"systems/acconfig/", "systems/hydraulics/sys-", "systems/load-manager/", "systems/performance/",
			"systems/engines/limit/active-mode-int", "engines/engine"};
	for (const char *r : roots)
	{
		if (k.begins_with(r))
		{
			if (k.begins_with("engines/engine"))
			{
				// FlightGear's engine interface only: n1, n2, fuel flow, oil quantity input
				return k.ends_with("/n1") || k.ends_with("/n2") || k.ends_with("/fuel-flow_pph") || k.ends_with("/oil-qty-input") || k.ends_with("/running");
			}
			return true;
		}
	}
	return false;
}

// Properties newer JSBSim versions tie at the FDM root that this build does
// not (the MD-80 systems read them): mirrored from their current names.
static const struct { const char *dst; const char *src; } REL_ALIASES[] = {
	{"sim-time-sec", "simulation/sim-time-sec"},
};

void FGBridge::mirror_push(JSBGodot *jsb, Dictionary &p)
{
	mirror_pushed = 0;
	for (const auto &a : REL_ALIASES)
	{
		if (jsb->has_property(a.dst))
		{
			jsb->set_property(a.dst, jsb->get_property(a.src));
		}
	}
	const Array keys = p.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const String k = keys[i];
		const Variant v = p[keys[i]];
		if (v.get_type() == Variant::STRING || !mirror_push_key(k) || !fdm_has(jsb, k))
		{
			continue;
		}
		jsb->set_property(fg_path(k), (double)v);
		mirror_pushed++;
	}
	const Array ckeys = control_state.keys();
	for (int i = 0; i < ckeys.size(); i++)
	{
		const String k = ckeys[i];
		const Variant v = control_state[ckeys[i]];
		if (v.get_type() == Variant::STRING || p.has(k) || !fdm_has(jsb, k))
		{
			continue;
		}
		jsb->set_property(fg_path(k), (double)v);
		mirror_pushed++;
	}
}

// Pull: every property the model references that the FDM defines and the
// bridge did not compute itself.
void FGBridge::mirror_pull(JSBGodot *jsb, Dictionary &p)
{
	mirror_pulled = 0;
	for (int i = 0; i < pull_keys.size(); i++)
	{
		const String &k = pull_keys[i];
		if (p.has(k) || !fdm_has(jsb, k))
		{
			continue;
		}
		p[k] = jsb->get_property(fg_path(k));
		mirror_pulled++;
	}
}

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
	// radios (docs/RADIO_NAVIGATION.md): NAV 1 on KLO Kloten VOR-DME with WIL
	// Willisau in standby, the KR 87 ADF on BER Bern with GLA Gland in standby
	d["instrumentation/nav/frequencies/selected-mhz"] = 114.85;
	d["instrumentation/nav/frequencies/standby-mhz"] = 116.90;
	d["instrumentation/nav/radials/selected-deg"] = 0.0;
	d["instrumentation/nav[1]/radials/selected-deg"] = 0.0;
	d["instrumentation/nav/power-btn"] = 1.0;
	d["instrumentation/nav/volume"] = 0.5;
	d["instrumentation/nav/serviceable"] = 1.0;
	d["instrumentation/comm/frequencies/selected-mhz"] = 118.10;
	d["instrumentation/comm/frequencies/standby-mhz"] = 121.50;
	d["instrumentation/comm/power-btn"] = 1.0;
	d["instrumentation/comm/volume"] = 0.5;
	d["instrumentation/comm/serviceable"] = 1.0;
	d["instrumentation/adf/rotation-deg"] = 0.0;
	d["instrumentation/kr-87/outputs/selected-khz"] = 335.0;
	d["instrumentation/kr-87/outputs/standby-khz"] = 375.0;
	d["instrumentation/kr-87/inputs/power-btn"] = 1.0;
	d["instrumentation/kr-87/inputs/volume"] = 0.5;
	d["instrumentation/kr-87/inputs/adf-btn"] = 1.0;
	d["instrumentation/kr-87/inputs/bfo-btn"] = 0.0;
	return d;
}

double FGBridge::switch_default(const String &prop)
{
	const Dictionary d = switch_defaults();
	return d.has(prop) ? (double)d[prop] : 0.0;
}

// A control's value: the cockpit state when the switch has been touched,
// else the aircraft's init list (jsbsim/init_properties.json: the MD-80's
// NAV 2 starts on 109.55), else the bridge's own default.
double FGBridge::ctl(const char *key, double def) const
{
	const String k(key);
	if (control_state.has(k))
	{
		return (double)control_state[k];
	}
	if (init_defaults.has(k))
	{
		return (double)init_defaults[k];
	}
	return def;
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
	// the KX 165 display glow and the KR 87 face lighting (28 V bus scaled)
	p["instrumentation/comm/dimming-norm"] = n * 0.6;
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
	p["systems/electrical/outputs/instrument-lights"] = dc_volts > 0.0 ? n * 0.6 * 28.0 : 0.0;
	if (dc_volts <= 0.0)
	{
		p["instrumentation/comm/dimming-norm"] = 0.0;
	}
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
	radios(jsb, p, jsb->get_latitude_deg(), jsb->get_longitude_deg(), jsb->get_property("position/h-sl-ft") * 0.3048,
			Math::fposmod((double)jsb->get_property("attitude/psi-deg"), 360.0));
	if (fdm_mirror)
	{
		// the cockpit switches first (they are inputs), then the FDM's own values
		const Array ck = control_state.keys();
		for (int i = 0; i < ck.size(); i++)
		{
			if (!p.has(ck[i]))
			{
				p[ck[i]] = control_state[ck[i]];
			}
		}
		const Array xk = extra_props.keys();
		for (int i = 0; i < xk.size(); i++)
		{
			p[xk[i]] = extra_props[xk[i]];
		}
		mirror_push(jsb, p);
		mirror_pull(jsb, p);
		// -set file aliases: rotors/main/blade[n]/flap-deg -> /systems/fcs/aileron-l/final-deg ...
		const Array ak = property_aliases.keys();
		for (int i = 0; i < ak.size(); i++)
		{
			String target = property_aliases[ak[i]];
			if (target.begins_with("/fdm/jsbsim/"))
			{
				p[ak[i]] = jsb->get_property(target.substr(12));
			}
			else if (fdm_has(jsb, target))
			{
				p[ak[i]] = jsb->get_property(fg_path(target));
			}
			else if (p.has(target.trim_prefix("/").replace("[0]", "")))
			{
				p[ak[i]] = p[target.trim_prefix("/").replace("[0]", "")];
			}
		}
	}
	// cockpit switches and knobs (FGControls) override the computed values —
	// except the levers the FDM / the flight keys own: those animate from the
	// FDM value and the mouse drives the FDM through the rig (_on_fg_control)
	static const char *FDM_OWNED[] = {"controls/flight/spoilers", "controls/flight/elevator-trim", "controls/engines/engine/throttle"};
	const Array keys = control_state.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const String key = keys[i];
		bool owned = false;
		for (const char *k : FDM_OWNED)
		{
			if (key == k && p.has(k))
			{
				owned = true;
			}
		}
		// every thrust lever (controls/engines/engine[n]/throttle): the levers
		// move together on the one throttle input, so a lever dragged once must
		// not pin its engine to that value afterwards
		if (key.begins_with("controls/engines/engine") && key.ends_with("/throttle") && p.has(key))
		{
			owned = true;
		}
		if (!owned)
		{
			p[keys[i]] = control_state[keys[i]];
		}
	}
	{
		const Array xk = extra_props.keys();
		for (int i = 0; i < xk.size(); i++)
		{
			p[xk[i]] = extra_props[xk[i]];
		}
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
	if (fdm_mirror && !power_property.is_empty())
	{
		// the FDM runs the electrical system: one bus property says whether the panel has power
		const double v = jsb != nullptr ? jsb->get_property(power_property) : 28.0;
		dc_volts = (jsb == nullptr || v >= power_threshold) ? 28.0 : 0.0;
		ac_volts = dc_volts > 0.0 ? 115.0 : 0.0;
		if (dc_volts <= 0.0)
		{
			p["systems/electrical/outputs/landing-light"] = 0.0;
			p["systems/electrical/outputs/taxi-light"] = 0.0;
			p["controls/lighting/panel-norm"] = 0.0;
			p["controls/lighting/instruments-norm"] = 0.0;
			p["controls/lighting/panel-int"] = 0.0;
		}
		return;
	}
	const bool battery = ctl("controls/electric/battery-switch", 1.0) > 0.5;
	bool gen_on = false;
	double gen_caution[ENGINES] = {0.0, 0.0};
	for (int i = 0; i < ENGINES; i++)
	{
		const bool sw = ctl(i == 0 ? "controls/electric/engine/generator" : "controls/electric/engine[1]/generator", 1.0) > 0.5;
		const double n2 = jsb != nullptr ? jsb->get_property(idx("propulsion/engine[%d]/n2", i)) : 100.0;
		const double n1 = jsb != nullptr ? jsb->get_property(idx("propulsion/engine[%d]/n1", i)) : 0.0; // a turboprop (PC-7) has N1 only
		const double rpm = jsb != nullptr ? jsb->get_property(idx("propulsion/engine[%d]/engine-rpm", i)) : 0.0;
		const bool online = sw && (n2 > 50.0 || n1 > 50.0 || rpm > 400.0);
		gen_on = gen_on || online;
		gen_caution[i] = online ? 0.0 : 1.0;
	}
	dc_volts = (gen_on || battery) ? 28.0 : 0.0;
	ac_volts = dc_volts > 0.0 ? 115.0 : 0.0;
	static const char *const outs[] = {"adi", "hsi", "aoa-indexer", "caution-panel", "hud", "ldg-warning-system",
			"nav-mode", "rwr", "gau-8", "fuel-gauge-sel", "comm", "nav", "adf", "dme"};
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
			"instrumentation/nav[2]/in-range", "instrumentation/nav[2]/nav-distance",
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
	double rudder = jsb->get_property("fcs/rudder-pos-norm");
	double elevator = jsb->get_property("fcs/elevator-pos-norm");
	if (rudder_norm_per_deg > 0.0)
	{
		rudder = (double)jsb->get_property("fcs/rudder-pos-deg") * rudder_norm_per_deg;
	}
	if (elevator_norm_per_deg > 0.0)
	{
		elevator = (double)jsb->get_property("fcs/elevator-pos-deg") * elevator_norm_per_deg;
	}
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
	p["surface-positions/elevator-pos-norm"] = elevator;
	p["surface-positions/left-aileron-pos-norm"] = ail_l;
	p["surface-positions/right-aileron-pos-norm"] = -ail_l;
	p["surface-positions/left-flap-pos-norm"] = flap;
	p["surface-positions/right-flap-pos-norm"] = flap;
	p["surface-positions/left-spoiler-pos-norm"] = speedbrake;
	p["surface-positions/speedbrake-pos-norm"] = speedbrake;
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
	p["gear/gear[0]/steering-norm"] = Math::clamp((double)jsb->get_property("fcs/steer-pos-deg[0]") / Math::max(nose_steer_max_deg, 1.0), -1.0, 1.0);
	p["gear/gear/steering-norm"] = p["gear/gear[0]/steering-norm"]; // the alias fallback looks the [0]-less key up
	// gear handle: the FlightGear Nasal animates it to 0 for UP and -1 for DOWN
	const bool cmd_down = jsb->get_gear_cmd() > 0.5;
	// airliner gear levers (the MD-80's JSBSim systems read /controls/gear/lever: 0 up, 3 down)
	if (fdm_mirror)
	{
		p["controls/gear/lever"] = cmd_down ? 3.0 : 0.0;
	}
	p["sim/model/A-10/controls/gear/ld-gear-handle-anim"] = cmd_down ? -1.0 : 0.0;
	p["orientation/alpha-deg"] = airborne ? (double)jsb->get_property("aero/alpha-deg") : 0.0;
}

void FGBridge::engines(JSBGodot *jsb, Dictionary &p)
{
	// every engine the FDM has (the MD-80's APU is engine[2]: its systems
	// read the FlightGear engine interface n2 of it to spool the APU), at
	// least the two the A-10 model expects
	const int count = Math::clamp(jsb->get_engine_count(), (int)ENGINES, 8);
	for (int i = 0; i < count; i++)
	{
		const double n1 = jsb->get_property(idx("propulsion/engine[%d]/n1", i));
		const double n2 = jsb->get_property(idx("propulsion/engine[%d]/n2", i));
		const double thrust = jsb->get_property(idx("propulsion/engine[%d]/thrust-lbs", i));
		const double ff_pps = jsb->get_property(idx("propulsion/engine[%d]/fuel-flow-rate-pps", i));
		p[idx("sim/model/A-10/engines/engine[%d]/n1", i)] = n1;
		p[idx("engines/engine[%d]/n1", i)] = n1;
		p[idx("engines/engine[%d]/n2", i)] = n2;
		if (fdm_mirror)
		{
			// FlightGear's engine interface as the FDM's systems expect it
			p[idx("engines/engine[%d]/egt-degf", i)] = jsb->get_property(idx("propulsion/engine[%d]/egt-degf", i));
			p[idx("engines/engine[%d]/fuel-flow_pph", i)] = ff_pps * 3600.0;
			p[idx("engines/engine[%d]/oil-pressure-psi", i)] = jsb->get_property(idx("propulsion/engine[%d]/oil-pressure-psi", i));
			p[idx("engines/engine[%d]/running", i)] = n2 > 10.0 ? 1.0 : 0.0;
			p[idx("engines/engine[%d]/thrust-lbs", i)] = thrust;
			p[idx("engines/engine[%d]/rpm", i)] = jsb->get_property(idx("propulsion/engine[%d]/engine-rpm", i));
			p[idx("controls/engines/engine[%d]/throttle", i)] = jsb->get_property(idx("fcs/throttle-cmd-norm[%d]", i));
			continue;
		}
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
	p["instrumentation/gps/indicated-vertical-speed"] = jsb->get_property("velocities/h-dot-fps") * 60.0;
	p["velocities/vertical-speed-fps"] = jsb->get_property("velocities/h-dot-fps");
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
	// Headings on the instruments are magnetic: true heading minus the WMM
	// declination at the aircraft (docs/RADIO_NAVIGATION.md)
	const double hdg_true = Math::fposmod((double)jsb->get_property("attitude/psi-deg"), 360.0);
	update_magvar(jsb->get_latitude_deg(), jsb->get_longitude_deg(), jsb->get_property("position/h-sl-ft") * 0.3048);
	const double hdg = Math::fposmod(hdg_true - magvar_deg, 360.0);
	p["orientation/heading-deg"] = hdg_true;
	p["orientation/heading-magnetic-deg"] = hdg;
	p["environment/magnetic-variation-deg"] = magvar_deg;
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
	// FlightGear flight-state paths (the FDM-mirrored aircraft read them as inputs)
	p["position/altitude-ft"] = jsb->get_property("position/h-sl-ft");
	p["position/altitude-agl-ft"] = jsb->get_property("position/h-agl-ft");
	// radio-altimeter style height of the gear and its value two seconds ago
	// (FlightGear's GPWS callouts fire only when that history says "descending")
	const double gear_agl = Math::max((double)jsb->get_property("position/h-agl-ft") - gear_agl_offset_ft, 0.0);
	p["position/gear-agl-ft"] = gear_agl;
	agl_history_t += frame_delta;
	agl_history.push_back(Vector2(agl_history_t, gear_agl));
	while (agl_history.size() > 1 && agl_history_t - agl_history[0].x > 2.0)
	{
		agl_history.remove_at(0);
	}
	// FlightGear's predict-simple: the height expected two seconds ahead (the
	// callouts fire only while it is below the current height = descending)
	const double agl_then = agl_history.size() > 0 ? agl_history[0].y : gear_agl;
	const double span = agl_history.size() > 0 ? Math::max(agl_history_t - agl_history[0].x, 0.1) : 2.0;
	const double agl_rate = (gear_agl - agl_then) / span; // ft/s, negative = descending
	// below 1.5 ft/s the "prediction" is the current height: gear-spring and
	// terrain-rebase jitter on the ground must not read as a descent (the
	// "ten" callout starts at 10 ft)
	p["position/gear-agl-ft-2-sec"] = std::abs(agl_rate) < 1.5 ? gear_agl : gear_agl + agl_rate * 2.0;
	p["orientation/pitch-rate-degps"] = Math::rad_to_deg((double)jsb->get_property("velocities/q-rad_sec"));
	p["orientation/roll-rate-degps"] = Math::rad_to_deg((double)jsb->get_property("velocities/p-rad_sec"));
	p["orientation/yaw-rate-degps"] = Math::rad_to_deg((double)jsb->get_property("velocities/r-rad_sec"));
	p["environment/pressure-inhg"] = p_psf / PSF_PER_INHG;
	p["environment/temperature-degc"] = ((double)jsb->get_property("atmosphere/T-R") - 491.67) * 5.0 / 9.0;
	p["sim/time/elapsed-sec"] = jsb->get_property("simulation/sim-time-sec");
	p["instrumentation/airspeed-indicator/indicated-mach"] = jsb->get_property("velocities/mach");
	p["controls/flight/flaps"] = jsb->get_property("fcs/flap-cmd-norm");
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
	p["accelerations/pilot-gmax"] = g_max;
	p["accelerations/pilot-gmin"] = g_min;
	// glider instruments (FlightGear ASK 21): total-energy variometer
	// (climb rate plus the kinetic energy change, m/s), yaw string, wind for
	// the wind compass, trim and the airbrake lever
	const double hdot_mps = (double)jsb->get_property("velocities/h-dot-fps") * 0.3048;
	const double vtrue_mps = (double)jsb->get_property("velocities/vtrue-kts") * 0.514444;
	double te_mps = hdot_mps;
	if (prev_vtrue_mps >= 0.0 && frame_delta > 1e-4)
	{
		te_mps += vtrue_mps * (vtrue_mps - prev_vtrue_mps) / frame_delta / 9.80665;
	}
	prev_vtrue_mps = vtrue_mps;
	// the frame-to-frame speed difference is noisy; a real variometer lags
	// about a second, which also steadies the needle
	te_filtered_mps += (Math::clamp(te_mps, -10.0, 10.0) - te_filtered_mps) * Math::clamp(frame_delta / 1.0, 0.0, 1.0);
	p["instrumentation/variometer/te-reading-mps"] = te_filtered_mps;
	p["instrumentation/ilec-sc7/te-reading-mps"] = te_filtered_mps;
	p["instrumentation/variometer/indicated-mps"] = Math::clamp(hdot_mps, -10.0, 10.0);
	p["instrumentation/yawstring"] = Math::clamp(-(double)jsb->get_property("aero/beta-deg") / 15.0, -1.0, 1.0);
	p["instrumentation/airspeed-indicator/indicated-corr-speed-kt"] = p["instrumentation/airspeed-indicator/indicated-speed-kt"];
	p["instrumentation/airspeed-indicator/true-speed-kt"] = jsb->get_property("velocities/vtrue-kts");
	p["instrumentation/altimeter/setting-hpa"] = (double)p.get("instrumentation/altimeter/setting-inhg", 29.92) * 33.8639;
	p["controls/flight/elevator-trim"] = jsb->get_property("fcs/pitch-trim-cmd-norm");
	p["controls/flight/spoilers"] = jsb->get_property("fcs/speedbrake-cmd-norm");
	// the throttle lever as the pilot set it (FlightGear's glider airbrake lever
	// is the throttle axis: 1 = brakes in, 0 = out)
	p["controls/engines/engine/throttle"] = jsb->get_input_throttle();
	p["controls/engines/engine[1]/throttle"] = jsb->get_input_throttle();
	const Vector3 wind = jsb->get_weather_wind_ned_kts(jsb->get_altitude_ft() * 0.3048); // the air moves toward (N, E)
	const double wind_kts = std::sqrt(wind.x * wind.x + wind.y * wind.y);
	p["environment/wind-speed-kt"] = wind_kts;
	p["environment/wind-from-heading-deg"] = wind_kts > 0.1 ? Math::fposmod(Math::rad_to_deg(std::atan2(-wind.y, -wind.x)), 360.0) : 0.0;
}

// WMM declination at the aircraft, recomputed after 0.05 deg of travel.
void FGBridge::update_magvar(double lat_deg, double lon_deg, double alt_m)
{
	if (std::abs(lat_deg - magvar_lat) < 0.05 && std::abs(lon_deg - magvar_lon) < 0.05)
	{
		return;
	}
	magvar_year = GeoMagnetic::decimal_year_now();
	magvar_deg = GeoMagnetic::declination_deg(lat_deg, lon_deg, alt_m, magvar_year);
	magvar_lat = lat_deg;
	magvar_lon = lon_deg;
}

// Radio navigation (docs/RADIO_NAVIGATION.md): the cockpit radios' tuned
// frequencies, the OBS and the ADF card come from the control state; the
// receivers compute what the gauges show. The KR 87's FRQ button swaps its
// active and standby frequency on the press edge, and the display logic
// FlightGear's kr_87 instrument provides (FRQ mode, ADF / ANT / BFO
// annunciators) is emulated here.
void FGBridge::radios(JSBGodot *jsb, Dictionary &p, double lat_deg, double lon_deg, double alt_m, double true_heading_deg)
{
	const String KR_SEL = "instrumentation/kr-87/outputs/selected-khz";
	const String KR_STBY = "instrumentation/kr-87/outputs/standby-khz";
	const double frq = ctl("instrumentation/kr-87/inputs/frq-btn", 0.0);
	if (frq > 0.5 && prev_frq_btn < 0.5 && control_state.has(KR_SEL) && control_state.has(KR_STBY))
	{
		const Variant a = control_state[KR_SEL];
		control_state[KR_SEL] = control_state[KR_STBY];
		control_state[KR_STBY] = a;
	}
	prev_frq_btn = frq;
	const double adf_khz = control_state.has(KR_SEL) ? (double)control_state[KR_SEL] : ctl("instrumentation/adf/frequencies/selected-khz", 335.0);
	const bool kr_on = dc_volts > 0.0 && ctl("instrumentation/kr-87/inputs/power-btn", 1.0) > 0.5;
	const bool nav_on = dc_volts > 0.0 && ctl("instrumentation/nav/power-btn", 1.0) > 0.5;
	radio_nav->set_powered(nav_on || kr_on);
	radio_nav->set_nav_frequency_mhz(ctl("instrumentation/nav/frequencies/selected-mhz", 114.85));
	radio_nav->set_obs_deg(ctl("instrumentation/nav/radials/selected-deg", 0.0));
	radio_nav->set_adf_frequency_khz(adf_khz);
	radio_nav->set_adf_card_deg(ctl("instrumentation/adf/rotation-deg", 0.0));
	radio_nav->update(frame_delta, lat_deg, lon_deg, alt_m, true_heading_deg);
	radio_nav->write_properties(p);
	// NAV 2: its own frequency and course (the MD-80's FGCP NAV 2 / CRS 2 knobs
	// through the Nasal shims), the first officer's HSI
	const bool nav2_on = dc_volts > 0.0 && ctl("instrumentation/nav[1]/power-btn", 1.0) > 0.5;
	radio_nav2->set_powered(nav2_on);
	radio_nav2->set_nav_frequency_mhz(ctl("instrumentation/nav[1]/frequencies/selected-mhz", 0.0));
	radio_nav2->set_obs_deg(ctl("instrumentation/nav[1]/radials/selected-deg", 0.0));
	radio_nav2->update(frame_delta, lat_deg, lon_deg, alt_m, true_heading_deg);
	radio_nav2->write_properties(p);
	// FlightGear navradio extras the DFGS reads: radials/target-auto-hdg-deg
	// (the course plus an intercept angle of 3 deg per degree of needle, at
	// most 30) and gs-rate-of-climb-fpm, navradio.cxx's rate for intercepting
	// the glideslope: the path angle steepened by ten times the angular error
	// (a fixed 3 deg descent here let a 3.3 deg beam and every gust walk away)
	const double vg_fpm = jsb->get_property("velocities/vg-fps") * 60.0;
	// Ground track (true and magnetic; the heading while the aircraft stands):
	// the DFGS drift angle, and FlightGear's cross-track heading error below
	const double hdg_true_now = Math::fposmod((double)jsb->get_property("attitude/psi-deg"), 360.0);
	const double track_true = vg_fpm > 100.0 ? Math::fposmod(Math::rad_to_deg((double)jsb->get_property("flight-path/psi-gt-rad")), 360.0) : hdg_true_now;
	p["orientation/track-deg"] = track_true;
	p["orientation/track-magnetic-deg"] = Math::fposmod(track_true - magvar_deg, 360.0);
	const double xtrack_hdg_err = Math::fposmod(track_true - hdg_true_now + 180.0, 360.0) - 180.0;
	for (const char *N : {"instrumentation/nav/", "instrumentation/nav[1]/"})
	{
		const String n(N);
		const double course = p.has(n + String("radials/selected-deg")) ? (double)p[n + String("radials/selected-deg")] : 0.0;
		const double cdi = p.has(n + String("heading-needle-deflection")) ? (double)p[n + String("heading-needle-deflection")] : 0.0;
		const double in_range = p.has(n + String("in-range")) ? (double)p[n + String("in-range")] : 0.0;
		const double from = p.has(n + String("from-flag")) ? (double)p[n + String("from-flag")] : 0.0;
		// navradio.cxx: target radial TRUE (the DFGS subtracts the true heading;
		// a magnetic base left the needle a third of the variation off centre),
		// plus 3 deg per unit of needle, minus the crab (track - heading), so a
		// crosswind holds the centreline with a centred needle
		const double course_true = Math::fposmod(course + magvar_deg, 360.0);
		const double base = from > 0.5 ? Math::fposmod(course_true + 180.0, 360.0) : course_true;
		p[n + String("radials/target-radial-true-deg")] = course_true;
		p[n + String("radials/target-auto-hdg-deg")] = in_range > 0.5 ? Math::fposmod(base + Math::clamp(cdi * 3.0, -30.0, 30.0) - xtrack_hdg_err, 360.0) : course_true;
		const double gs_ok = p.has(n + String("gs-in-range")) ? (double)p[n + String("gs-in-range")] : 0.0;
		double roc = 0.0;
		if (gs_ok > 0.5)
		{
			const double direct = p.has(n + String("gs-direct-deg")) ? (double)p[n + String("gs-direct-deg")] : 3.0;
			const double target = p.has(n + String("gs-target-deg")) ? (double)p[n + String("gs-target-deg")] : 3.0;
			const double gs_diff = target - direct;                 // positive: below the beam
			const double des_angle = Math::clamp(direct - 10.0 * gs_diff, -15.0, 15.0);
			roc = -std::sin(Math::deg_to_rad(des_angle)) * vg_fpm; // ft/min, negative = descend
		}
		p[n + String("gs-rate-of-climb-fpm")] = roc;
		p[n + String("gs-rate-of-climb")] = roc / 60.0;
	}
	if (!nav_on)
	{
		p["instrumentation/nav/in-range"] = 0.0;
		p["instrumentation/nav/to-flag"] = 0.0;
		p["instrumentation/nav/from-flag"] = 0.0;
		p["instrumentation/nav/heading-needle-deflection"] = 0.0;
		p["instrumentation/dme/in-range"] = 0.0;
	}
	if (!nav2_on)
	{
		p["instrumentation/nav[1]/in-range"] = 0.0;
		p["instrumentation/nav[1]/to-flag"] = 0.0;
		p["instrumentation/nav[1]/from-flag"] = 0.0;
		p["instrumentation/nav[1]/heading-needle-deflection"] = 0.0;
	}
	if (!kr_on)
	{
		p["instrumentation/adf/in-range"] = 0.0;
	}
	// KR 87 display: FRQ mode (standby shown on the right), ADF or ANT, BFO
	const bool adf_mode = ctl("instrumentation/kr-87/inputs/adf-btn", 1.0) > 0.5;
	p["instrumentation/kr-87/modes/stby"] = kr_on ? 1.0 : 0.0;
	p["instrumentation/kr-87/annunciators/frq"] = kr_on ? 1.0 : 0.0;
	p["instrumentation/kr-87/annunciators/adf"] = kr_on && adf_mode ? 1.0 : 0.0;
	p["instrumentation/kr-87/annunciators/ant"] = kr_on && !adf_mode ? 1.0 : 0.0;
	p["instrumentation/kr-87/annunciators/bfo"] = kr_on && ctl("instrumentation/kr-87/inputs/bfo-btn", 0.0) > 0.5 ? 1.0 : 0.0;
	p["instrumentation/kr-87/annunciators/flt"] = 0.0;
	p["instrumentation/kr-87/annunciators/et"] = 0.0;
	// the A-10's course deviation indicator follows NAV 1 too
	p["sim/model/A-10/instrumentation/cdi/needle-deflection"] = p["instrumentation/nav/heading-needle-deflection"];
	p["sim/model/A-10/instrumentation/cdi/to-flag"] = p["instrumentation/nav/to-flag"];
	p["sim/model/A-10/instrumentation/cdi/from-flag"] = p["instrumentation/nav/from-flag"];
}

void FGBridge::controls(JSBGodot *jsb, Dictionary &p)
{
	p["controls/flight/aileron"] = jsb->get_property("fcs/aileron-cmd-norm");
	p["controls/flight/elevator"] = jsb->get_property("fcs/elevator-cmd-norm");
	// FlightGear's /controls/flight/rudder is +1 for right pedal; its JSBSim
	// interface negates it into fcs/rudder-cmd-norm (JSBSim's rudder sign is
	// the other way), so the FlightGear-side value is the negated command
	p["controls/flight/rudder"] = -(double)jsb->get_property("fcs/rudder-cmd-norm");
	// flap lever detent: aircraft.cfg [flaps] positions 0 / 0.5 / 1 -> UP / MVR / DN
	const double flap_cmd = jsb->get_property("fcs/flap-cmd-norm");
	p["sim/flaps/current-setting"] = flap_cmd < 0.25 ? 0 : (flap_cmd < 0.75 ? 1 : 2);
}

} // namespace jsbgodot
