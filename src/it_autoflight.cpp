#include "it_autoflight.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

namespace jsbgodot
{

// property keys (normalised: no leading slash, no [0])
#define IN(x) "it-autoflight/input/" x
#define INT(x) "it-autoflight/internal/" x
#define OUT(x) "it-autoflight/output/" x
#define FMA(x) "instrumentation/fma/" x
static const char *const K_ACTIVE_MODE_INT = "systems/engines/limit/active-mode-int";
static const char *const K_ACTIVE_MODE = "systems/engines/limit/active-mode";

static double roundto(double v, double step)
{
	return step > 0.0 ? std::round(v / step) * step : std::round(v);
}

static String fmt3(double v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%3.0f", v);
	return String(buf);
}

void ItAutoflight::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("reset", "type"), &ItAutoflight::reset);
	ClassDB::bind_method(D_METHOD("panel", "action", "arg"), &ItAutoflight::panel);
	ClassDB::bind_method(D_METHOD("get_value", "key"), &ItAutoflight::get_value);
	ClassDB::bind_method(D_METHOD("get_published"), &ItAutoflight::get_published);
	ClassDB::bind_method(D_METHOD("input_names"), &ItAutoflight::input_names);
	ClassDB::bind_method(D_METHOD("is_engaged"), &ItAutoflight::is_engaged);
	ClassDB::bind_method(D_METHOD("is_athr_driving"), &ItAutoflight::is_athr_driving);
	ClassDB::bind_method(D_METHOD("get_lat_mode"), &ItAutoflight::get_lat_mode);
	ClassDB::bind_method(D_METHOD("get_vert_mode"), &ItAutoflight::get_vert_mode);
	ClassDB::bind_method(D_METHOD("get_thr_mode"), &ItAutoflight::get_thr_mode);
	ClassDB::bind_method(D_METHOD("set_option", "name", "value"), &ItAutoflight::set_option);
}

ItAutoflight::ItAutoflight() {}

void ItAutoflight::set_rules(const Ref<FGPropertyRules> &r) { rules = r; }

void ItAutoflight::set_option(const String &name, double value)
{
	if (name == "auto_arm_alt") opt_auto_arm_alt = value;
	else if (name == "nav") opt_nav = value;
	else if (name == "debug") debug_modes = value > 0.5;
}

// ── state access ────────────────────────────────────────────────────────

static double numv(const Variant &v)
{
	switch (v.get_type())
	{
		case Variant::NIL: return 0.0;
		case Variant::BOOL: return (bool)v ? 1.0 : 0.0;
		case Variant::INT: return (double)(int64_t)v;
		case Variant::FLOAT: return (double)v;
		case Variant::STRING: { const String s = v; return s.is_valid_float() ? s.to_float() : 0.0; }
		default: return 0.0;
	}
}

double ItAutoflight::g(const char *k) const { return st.has(k) ? numv(st[k]) : 0.0; }
bool ItAutoflight::b(const char *k) const { return g(k) > 0.5; }
String ItAutoflight::gs(const char *k) const { return st.has(k) ? String(st[k]) : String(); }
void ItAutoflight::s(const char *k, double v) { st[k] = v; }
void ItAutoflight::sb(const char *k, bool v) { st[k] = v ? 1.0 : 0.0; }
void ItAutoflight::ss(const char *k, const String &v) { st[k] = v; }

double ItAutoflight::in(const char *k) const
{
	const String key(k);
	if (cur.has(key))
	{
		return numv(cur[key]);
	}
	if (rules.is_valid())
	{
		return rules->get_value(key);
	}
	return 0.0;
}
bool ItAutoflight::inb(const char *k) const { return in(k) > 0.5; }
double ItAutoflight::elapsed() const { return in("sim/time/elapsed-sec"); }
bool ItAutoflight::fgcp_powered() const { return in("systems/electrical/outputs/fgcp") >= 24.0; }
bool ItAutoflight::wow_any() const { return inb("gear/gear/wow") || inb("gear/gear[1]/wow") || inb("gear/gear[2]/wow"); }
int ItAutoflight::radio_sel() const { return (int)g(IN("active-ap")) == 2 ? 1 : 0; }
bool ItAutoflight::nav_loc(int idx) const { return idx == 1 ? inb("instrumentation/nav[1]/nav-loc") : inb("instrumentation/nav/nav-loc"); }

Variant ItAutoflight::get_value(const String &key) const
{
	String k = key;
	while (k.begins_with("/")) k = k.substr(1);
	k = k.replace("[0]", "");
	return st.has(k) ? st[k] : Variant();
}

bool ItAutoflight::is_engaged() const { return b(OUT("ap1")) || b(OUT("ap2")); }
bool ItAutoflight::is_athr_driving() const { return b(OUT("athr")) && (int)g(OUT("thr-mode")) != 3; }
int ItAutoflight::get_lat_mode() const { return (int)g(OUT("lat")); }
int ItAutoflight::get_vert_mode() const { return (int)g(OUT("vert")); }
int ItAutoflight::get_thr_mode() const { return (int)g(OUT("thr-mode")); }

PackedStringArray ItAutoflight::input_names() const
{
	static const char *const names[] = {
		"controls/flight/aileron", "controls/flight/elevator", "engines/engine/reverser-pos-norm", "engines/engine[1]/reverser-pos-norm",
		"gear/gear/wow", "gear/gear[1]/wow", "gear/gear[2]/wow", "sim/time/elapsed-sec", "fdm/jsbsim/fcs/flap-pos-deg",
		"orientation/pitch-deg", "orientation/roll-deg", "position/gear-agl-ft", "instrumentation/altimeter/indicated-altitude-ft",
		"instrumentation/nav/gs-needle-deflection-norm", "instrumentation/nav[1]/gs-needle-deflection-norm",
		"instrumentation/nav/in-range", "instrumentation/nav[1]/in-range", "instrumentation/nav/heading-needle-deflection-norm",
		"instrumentation/nav[1]/heading-needle-deflection-norm", "instrumentation/nav/signal-quality-norm",
		"instrumentation/nav[1]/signal-quality-norm", "instrumentation/nav/nav-loc", "instrumentation/nav[1]/nav-loc",
		"velocities/groundspeed-kt", "instrumentation/airspeed-indicator/indicated-speed-kt", "instrumentation/airspeed-indicator/indicated-mach",
		"systems/dfgs/atr/cmd", "systems/dfgs/bit-1/active", "systems/dfgs/bit-2/active", "systems/dfgs/nlg-timer-20/wow-timer",
		"systems/dfgs/power-avail", "systems/dfgs/stall-alpha-deg", "systems/dfgs/stick-pusher/active", "systems/dfgs/speeds/vmax-type",
		"systems/dfgs/speeds/vmax", "systems/dfgs/speeds/vmax-mach", "systems/dfgs/speeds/vmin", "systems/dfgs/speeds/vmin-90-percent",
		"systems/dfgs/speeds/vmin-mach", "fdm/jsbsim/aero/alpha-deg-damped", "systems/electrical/outputs/fgcp",
		"systems/engines/limit/active-mode-int", "systems/engines/limit/idle-norm", "systems/engines/control-1/ats-max",
		"systems/engines/control-2/ats-max", "systems/acconfig/autoconfig-running", "systems/caws/apoff/horn-silence-enabled",
		"systems/caws/logic/altitude-alert", "position/wow", "it-autoflight/input/fd1", "it-autoflight/input/fd2", "it-autoflight/input/athr",
		"it-autoflight/input/bank-limit-sw", "it-autoflight/input/ap1-avail", "it-autoflight/input/ap2-avail", "it-autoflight/input/athr-avail",
		"systems/engines/control-1/throttle-pos", "systems/engines/control-2/throttle-pos",
	};
	PackedStringArray out;
	for (const char *n : names) out.push_back(n);
	return out;
}

// ── ITAF.init ───────────────────────────────────────────────────────────

void ItAutoflight::reset(int t)
{
	initialised = true;
	if (t != 1)
	{
		s(IN("alt"), 10000);
		ctl_writes[IN("bank-limit-sw")] = 4.0; // 30 deg
		s(IN("hdg"), 360);
		sb(IN("kts-mach"), false);
		sb(IN("kts-mach-fgcp"), false);
		sb(IN("kts-mach-flch"), false);
		s(IN("kts"), 250);
		s(IN("kts-flch"), 250);
		s(IN("mach"), 0.76);
		s(IN("mach-flch"), 0.76);
		sb(IN("true-course"), false);
		s(IN("active-ap"), 1);
	}
	sb(INT("takeoff-lvl"), true);
	sb(IN("ap1"), false);
	sb(IN("ap2"), false);
	update_auto_land(false);
	ctl_writes[IN("athr")] = 0.0;
	if (t != 1)
	{
		ctl_writes[IN("fd1")] = 1.0;
		ctl_writes[IN("fd2")] = 1.0;
		prev_fd1 = prev_fd2 = 1.0;
	}
	s(IN("vs"), 0);
	s(IN("pitch"), 0);
	sb(IN("alt-armed"), false);
	s(IN("lat"), 3);
	s(IN("vert"), 1);
	sb(IN("toga"), false);
	sb(OUT("ap1"), false);
	sb(OUT("ap2"), false);
	sb(OUT("athr"), false);
	if (t != 1)
	{
		sb(OUT("fd1"), true);
		sb(OUT("fd2"), true);
	}
	sb(OUT("hdg-in-hld"), true);
	sb(OUT("lnav-arm"), false);
	sb(OUT("loc-arm"), false);
	sb(OUT("gs-arm"), false);
	s(OUT("lat"), 0);
	s(OUT("vert"), 1);
	if (!st.has(OUT("thr-mode"))) s(OUT("thr-mode"), 3);
	s(INT("min-vs"), -500);
	s(INT("max-vs"), 500);
	s(INT("alt"), 10000);
	s(INT("hdg-hld-target"), 360);
	s(INT("bank-limit"), 30);
	s(INT("takeoff-hdg"), 0);
	s(INT("toga-kts"), 150);
	s(INT("lnav-advance-nm"), 0);
	s(INT("alt-alert"), 0);
	sb(INT("alt-alert-aural"), false);
	alt_capture_active = false;
	fma_thr();
	fma_arm();
	update_lat_text("HDG");
	update_vert_text("V/S");
	sb(INT("parallel-rudder-land"), false);
	if (t != 1)
	{
		sb("it-autoflight/sound/apoff", false);
		sb("it-autoflight/warning/ap", false);
		sb("it-autoflight/warning/atsflash", false);
		sb("it-autoflight/warning/ats", false);
		enable_ap_off = false;
		enable_power_ap_off = false;
		enable_athr_off = false;
		ap_kill_on = false;
		ats_kill_on = false;
		for (int w = 0; w < 4; w++) stop_blink(w);
	}
	sb("it-autoflight/sound/apoff-single", false);
	fdm_writes["systems/caws/logic/altitude-alert"] = 0.0;
	alt_rearm_time = opt_auto_arm_alt > 0.5 ? elapsed() : -5.0;
	// FGCP text windows and the canvas ND settings
	input_alt(g(IN("alt")));
	s("autopilot/settings/heading-bug-deg", g(IN("hdg")));
	s("autopilot/settings/target-altitude-ft", g(INT("alt")));
	if (!st.has(K_ACTIVE_MODE)) ss(K_ACTIVE_MODE, "---");
}

// ── per frame ───────────────────────────────────────────────────────────

void ItAutoflight::update(const Dictionary &props, double dt)
{
	cur = props;
	if (!initialised)
	{
		reset(0);
	}
	// The rules' "V/S altitude hold" reference syncs to the altitude only while
	// the hold is inactive; with the wheel at 0 from the first frame it would
	// stay at 0 ft (a dive to sea level on engagement): seed it once.
	if (!vs_alt_seeded)
	{
		const double alt = in("instrumentation/altimeter/indicated-altitude-ft");
		if (std::abs(alt) > 1.0)
		{
			rule_writes[INT("vs-alt")] = Math::clamp(roundto(alt, 100.0), 0.0, 50000.0);
			vs_alt_seeded = true;
		}
	}
	// listeners on properties others write: the FGCP's direct bindings and the rules
	const double fd1 = in(IN("fd1"));
	if (fd1 != prev_fd1)
	{
		prev_fd1 = fd1;
		if ((fd1 > 0.5) != b(OUT("fd1"))) fd1_master(fd1 > 0.5);
	}
	const double fd2 = in(IN("fd2"));
	if (fd2 != prev_fd2)
	{
		prev_fd2 = fd2;
		if ((fd2 > 0.5) != b(OUT("fd2"))) fd2_master(fd2 > 0.5);
	}
	const double athr_in = in(IN("athr"));
	if (athr_in != prev_athr_in)
	{
		prev_athr_in = athr_in;
		if ((athr_in > 0.5) != b(OUT("athr"))) athr_master(athr_in > 0.5);
	}
	const double vsaa = in(INT("vs-alt-active"));
	if (vsaa != prev_vs_alt_active)
	{
		prev_vs_alt_active = vsaa;
		update_vs_alt();
	}
	for (int i = 0; i < 2; i++)
	{
		const double nl = nav_loc(i) ? 1.0 : 0.0;
		if (nl != prev_nav_loc[i])
		{
			prev_nav_loc[i] = nl;
			if (radio_sel() == i)
			{
				fma_arm();
				fma_lat();
			}
		}
	}
	const int tri = (int)std::lround(in(K_ACTIVE_MODE_INT));
	if (tri != thrlim_prev_int)
	{
		thrlim_prev_int = tri;
		thrlim_update_txt();
	}
	// timers
	loop_acc += dt;
	if (loop_acc >= 0.1)
	{
		loop_acc = std::fmod(loop_acc, 0.1);
		loop();
	}
	slow_acc += dt;
	if (slow_acc >= 1.0)
	{
		slow_acc = std::fmod(slow_acc, 1.0);
		slow_loop();
	}
	if (loc_timer_on)
	{
		loc_acc += dt;
		if (loc_acc >= 0.5) { loc_acc = 0.0; fma_loc_update(); }
	}
	if (gs_timer_on)
	{
		gs_acc += dt;
		if (gs_acc >= 0.5) { gs_acc = 0.0; fma_gs_update(); }
	}
	if (ap_kill_on)
	{
		ap_kill_acc += dt;
		if (ap_kill_acc >= 0.4)
		{
			ap_kill_acc = 0.0;
			if (!b("it-autoflight/sound/apoff")) { ap_kill_on = false; sb("it-autoflight/warning/ap", false); }
			else sb("it-autoflight/warning/ap", !b("it-autoflight/warning/ap"));
		}
	}
	if (ats_kill_on)
	{
		ats_kill_acc += dt;
		if (ats_kill_acc >= 0.4)
		{
			ats_kill_acc = 0.0;
			if (!b("it-autoflight/warning/atsflash")) { ats_kill_on = false; sb("it-autoflight/warning/ats", false); }
			else sb("it-autoflight/warning/ats", !b("it-autoflight/warning/ats"));
		}
	}
	// cur stays: the panel handlers between two frames read the last inputs
}

void ItAutoflight::absorb(const Dictionary &props)
{
	// inputs the driver rules sync while their mode is inactive
	if (rules.is_null()) return;
	for (const char *k : {IN("vs"), IN("kts-flch"), IN("mach-flch")})
	{
		if (rules->wrote(k) && props.has(k))
		{
			st[k] = numv(props[k]);
		}
	}
}

Dictionary ItAutoflight::get_published() const
{
	Dictionary d = st.duplicate();
	// FMA blink: the window's texts flash at 2 Hz while active
	const bool off = std::fmod(elapsed() < 0.0 ? 0.0 : elapsed(), 0.5) >= 0.25;
	if (off)
	{
		static const char *const wins[4][2] = {{FMA("thr-mode-a"), FMA("thr-mode-b")}, {FMA("arm-mode-a"), FMA("arm-mode-b")},
				{FMA("roll-mode-a"), FMA("roll-mode-b")}, {FMA("pitch-mode-a"), FMA("pitch-mode-b")}};
		for (int w = 0; w < 4; w++)
		{
			if (blink_active[w])
			{
				d[wins[w][0]] = String();
				d[wins[w][1]] = String();
			}
		}
	}
	for (int w = 0; w < 4; w++)
	{
		d[String(FMA("blink-")) + String::num_int64(w)] = blink_active[w] ? 1.0 : 0.0;
	}
	return d;
}

Dictionary ItAutoflight::take_control_writes()
{
	Dictionary d = ctl_writes;
	ctl_writes = Dictionary();
	return d;
}

Dictionary ItAutoflight::take_fdm_writes()
{
	Dictionary d = fdm_writes;
	fdm_writes = Dictionary();
	return d;
}

Dictionary ItAutoflight::take_rule_writes()
{
	Dictionary d = rule_writes;
	rule_writes = Dictionary();
	return d;
}

// ── ITAF.loop ───────────────────────────────────────────────────────────

void ItAutoflight::loop()
{
	bool wow1 = inb("gear/gear[1]/wow");
	bool wow2 = inb("gear/gear[2]/wow");
	bool ap1 = b(OUT("ap1"));
	bool ap2 = b(OUT("ap2"));
	int lat = (int)g(OUT("lat"));
	int vert = (int)g(OUT("vert"));

	// Trip system off
	if (ap1 || ap2)
	{
		if (std::abs(in("controls/flight/aileron")) >= 0.2 || std::abs(in("controls/flight/elevator")) >= 0.2 || inb("systems/dfgs/stick-pusher/active") ||
				in("fdm/jsbsim/aero/alpha-deg-damped") >= in("systems/dfgs/stall-alpha-deg"))
		{
			ap1_master(false);
			ap2_master(false);
		}
	}
	if (!inb(IN("ap1-avail")) && ap1) ap1_master(false);
	if (!inb(IN("ap2-avail")) && ap2) ap2_master(false);
	if (!inb(IN("athr-avail")) && b(OUT("athr")))
	{
		if (in("engines/engine/reverser-pos-norm") >= 0.01 || in("engines/engine[1]/reverser-pos-norm") >= 0.01) kill_athr_silent();
		else athr_master(false);
	}

	// AP power warning
	if (inb("systems/dfgs/power-avail"))
	{
		if (inb("systems/acconfig/autoconfig-running")) enable_power_ap_off = false;
		else if (enable_power_ap_off)
		{
			sb("it-autoflight/sound/apoff-single", true);
			enable_power_ap_off = false;
		}
	}
	else
	{
		enable_power_ap_off = true;
		sb("it-autoflight/sound/apoff-single", false);
	}

	// LNAV reversion (no flight plan in ProvPilot: LNAV always reverts)
	if ((int)g(OUT("lat")) == 1)
	{
		set_lat_mode(3);
		start_blink(2);
	}
	// VOR/ILS reversion
	if (lat == 2 || lat == 4 || vert == 2 || vert == 6) check_radio_reversion(lat, vert);
	// turbulence mode reversion
	if (lat != 6 && vert == 10)
	{
		set_vert_mode(1);
		start_blink(3);
	}
	// takeoff / go around lateral reversion
	if (lat == 5 && vert != 7 && vert != 8)
	{
		set_lat_mode(3);
		start_blink(2);
	}

	ap1 = b(OUT("ap1"));
	ap2 = b(OUT("ap2"));
	lat = (int)g(OUT("lat"));
	vert = (int)g(OUT("vert"));
	const double agl = in("position/gear-agl-ft");
	const double vs_now = in(INT("vert-speed-fpm"));
	const double ind_alt = in("instrumentation/altimeter/indicated-altitude-ft");

	// takeoff mode logic
	if (lat == 5 && (b(INT("takeoff-lvl")) || wow1 || wow2)) takeoff_logic(0);
	if (b(OUT("lnav-arm"))) check_lnav(1);
	if (b(OUT("loc-arm"))) check_loc(1);
	if (b(OUT("gs-arm"))) check_gs(1);

	// autoland logic
	const bool auto_land = b(IN("auto-land"));
	const bool wow0 = inb("gear/gear/wow");
	if (ap1 || ap2)
	{
		if ((vert == 2 || vert == 6) && auto_land)
		{
			const int r = radio_sel();
			const double loc_defl = in(r == 1 ? "instrumentation/nav[1]/heading-needle-deflection-norm" : "instrumentation/nav/heading-needle-deflection-norm");
			const double sq = in(r == 1 ? "instrumentation/nav[1]/signal-quality-norm" : "instrumentation/nav/signal-quality-norm");
			can_autoland = (std::abs(loc_defl) <= 0.1 && loc_defl != 0.0 && sq >= 0.99) || wow0;
		}
		else can_autoland = false;
	}
	else
	{
		can_autoland = false;
		if (auto_land) update_auto_land(false);
	}

	if (can_autoland)
	{
		sb(INT("parallel-rudder-land"), agl <= 1500.0);
		if (vert == 2)
		{
			if (agl <= 1500.0 && agl >= 5.0)
			{
				if (lat != 4 && gs("it-autoflight/text/lat") != "LAND") update_lat_text("LAND");
				if (gs("it-autoflight/text/vert") != "LAND") update_vert_text("LAND");
			}
			if (agl <= 50.0 && agl >= 5.0) set_vert_mode(6);
		}
		else if (vert == 6)
		{
			if (wow1 && wow2)
			{
				if (gs("it-autoflight/text/vert") != "ROLLOUT")
				{
					update_lat_text("ROLLOUT");
					update_vert_text("ROLLOUT");
				}
			}
			else if (gs("it-autoflight/text/vert") != "FLARE")
			{
				update_lat_text("ALIGN");
				update_vert_text("FLARE");
			}
		}
		if (lat == 2 && agl <= 150.0) set_lat_mode(4);
	}
	else
	{
		sb(INT("parallel-rudder-land"), false);
		if (vert == 2)
		{
			if (ap1 || ap2)
			{
				if (agl <= 100.0 && agl >= 5.0 && gs("it-autoflight/text/vert") != "NO FLARE")
				{
					update_vert_text("NO FLARE");
					start_long_blink(3);
				}
			}
			else if (gs("it-autoflight/text/vert") == "NO FLARE")
			{
				update_vert_text("G/S");
				stop_blink(3);
			}
		}
		if (lat == 4 || vert == 6)
		{
			activate_loc();
			activate_gs();
		}
	}

	// go around arm
	if (in("systems/dfgs/nlg-timer-20/wow-timer") < 1.0 && vert != 7 && vert != 8 && agl < 1500.0 && in("fdm/jsbsim/fcs/flap-pos-deg") >= 25.9)
	{
		if ((ap1 || ap2) && !wow0)
		{
			if (go_around != 3) { go_around = 3; fma_arm(); }
		}
		else if (go_around != 2) { go_around = 2; fma_arm(); }
	}
	else if (go_around != 0) { go_around = 0; fma_arm(); }

	// altitude capture / sync logic
	const double alt_in = g(IN("alt"));
	if (vert != 0) internal_alt(alt_in);
	const bool alt_armed = b(IN("alt-armed"));
	const double int_alt = g(INT("alt"));
	double alt_diff = (vert == 0 && alt_armed) ? alt_in - ind_alt : int_alt - ind_alt;
	if (alt_armed)
	{
		if (vert == 0)
		{
			capt_vs = Math::clamp(roundto(std::abs(vs_now) / 5.0, 100.0), 50.0, 2500.0);
			if (std::abs(alt_diff) <= capt_vs && !wow1 && !wow2)
			{
				if (alt_in >= ind_alt && vs_now >= -25.0)
				{
					internal_alt(alt_in);
					set_vert_mode(3);
				}
				else if (alt_in < ind_alt && vs_now <= 25.0)
				{
					internal_alt(alt_in);
					set_vert_mode(3);
				}
			}
		}
		else if (vert != 2 && vert != 6)
		{
			capt_vs = Math::clamp(roundto(std::abs(vs_now) / 5.0, 100.0), 50.0, 2500.0);
			if (std::abs(alt_diff) <= capt_vs && !wow1 && !wow2)
			{
				if (int_alt >= ind_alt && vs_now >= -25.0) set_vert_mode(3);
				else if (int_alt < ind_alt && vs_now <= 25.0) set_vert_mode(3);
			}
		}
		else input_alt_armed(false);
	}

	// altitude hold min/max reset
	if (alt_capture_active)
	{
		if (std::abs(alt_diff) <= 25.0 && gs("it-autoflight/text/vert") != "ALT HLD")
		{
			reset_climb_rate_lim();
			update_vert_text("ALT HLD");
		}
	}
	synced_alt = alt_in == g(INT("alt"));

	bank_limit();
	athr_loop();
	fma_loop();

	// FGCP temporary swap reset
	const double now = elapsed();
	if (kts_mach_fgcp_time != -5.0 && kts_mach_fgcp_time + 3.0 < now)
	{
		kts_mach_fgcp_time = -5.0;
		sb(IN("kts-mach-fgcp"), b(IN("kts-mach")));
	}
	// auto altitude arm
	if (alt_rearm_time != -5.0 && !b(IN("alt-armed")) && alt_rearm_time + 1.0 < now)
	{
		alt_rearm_time = -5.0;
		vert = (int)g(OUT("vert"));
		if (opt_auto_arm_alt > 0.5 && vert != 2 && vert != 6 && !(vert == 0 && alt_in == g(INT("alt")))) input_alt_armed(true);
	}
}

void ItAutoflight::slow_loop()
{
	const int lat = (int)g(OUT("lat"));
	const int vert = (int)g(OUT("vert"));
	if (!b(OUT("ap1")) && !b(OUT("ap2")) && inb("gear/gear/wow") && in("velocities/groundspeed-kt") < 80.0 && (lat == 2 || lat == 4 || vert == 2 || vert == 6))
	{
		reset(1);
		start_blink(2);
		start_blink(3);
	}
	// (the waypoint advance logic needs a flight plan: none in ProvPilot)
}

// ── masters ─────────────────────────────────────────────────────────────

void ItAutoflight::ap1_master(bool on)
{
	if (on)
	{
		if (inb(IN("ap1-avail")) && (int)g(OUT("vert")) != 6)
		{
			if (!b(OUT("fd1")) && !b(OUT("fd2")) && !b(OUT("ap1")) && !b(OUT("ap2"))) set_basic_mode();
			sb(OUT("ap1"), true);
			enable_ap_off = true;
			sb("it-autoflight/sound/apoff", false);
		}
	}
	else
	{
		sb(OUT("ap1"), false);
		ap_off_function();
	}
	sb(IN("ap1"), b(OUT("ap1")));
}

void ItAutoflight::ap2_master(bool on)
{
	if (on)
	{
		if (inb(IN("ap2-avail")) && (int)g(OUT("vert")) != 6)
		{
			if (!b(OUT("fd1")) && !b(OUT("fd2")) && !b(OUT("ap1")) && !b(OUT("ap2"))) set_basic_mode();
			sb(OUT("ap2"), true);
			enable_ap_off = true;
			sb("it-autoflight/sound/apoff", false);
		}
	}
	else
	{
		sb(OUT("ap2"), false);
		ap_off_function();
	}
	sb(IN("ap2"), b(OUT("ap2")));
}

void ItAutoflight::ap_off_function()
{
	if (!b(OUT("ap1")) && !b(OUT("ap2")))
	{
		if (enable_ap_off)
		{
			sb("it-autoflight/sound/apoff", true);
			enable_ap_off = false;
			ap_kill_on = true;
			ap_kill_acc = 0.0;
		}
		if (gs("it-autoflight/text/vert") == "ROLLOUT")
		{
			reset(1);
			start_blink(2);
			start_blink(3);
		}
	}
}

void ItAutoflight::athr_master(bool on)
{
	if (on)
	{
		if (inb(IN("athr-avail")))
		{
			if (gs("it-autoflight/text/vert") == "G/A CLB") athr_set_mode(2);
			else
			{
				const int tm = (int)g(OUT("thr-mode"));
				if (tm == 1 || tm == 2) athr_set_mode(3);
			}
			sb(OUT("athr"), true);
			if ((int)g(OUT("thr-mode")) == 3) start_blink(0);
			ats_kill_on = false;
			sb("it-autoflight/warning/ats", false);
			sb("it-autoflight/warning/atsflash", false);
			enable_athr_off = true;
		}
	}
	else
	{
		sb(OUT("athr"), false);
		if (enable_athr_off)
		{
			sb("it-autoflight/warning/atsflash", true);
			enable_athr_off = false;
			ats_kill_on = true;
			ats_kill_acc = 0.0;
		}
	}
	const bool out = b(OUT("athr"));
	if ((in(IN("athr")) > 0.5) != out)
	{
		ctl_writes[IN("athr")] = out ? 1.0 : 0.0;
		prev_athr_in = out ? 1.0 : 0.0;
	}
}

void ItAutoflight::kill_ap_silent()
{
	sb(OUT("ap1"), false);
	sb(OUT("ap2"), false);
	sb("it-autoflight/sound/apoff", false);
	enable_ap_off = false;
	sb(IN("ap1"), false);
	sb(IN("ap2"), false);
}

void ItAutoflight::kill_athr_silent()
{
	sb(OUT("athr"), false);
	sb("it-autoflight/warning/atsflash", false);
	enable_athr_off = false;
	ctl_writes[IN("athr")] = 0.0;
	prev_athr_in = 0.0;
}

void ItAutoflight::fd1_master(bool on)
{
	if (!b(OUT("fd1")) && !b(OUT("fd2")) && !b(OUT("ap1")) && !b(OUT("ap2"))) set_basic_mode();
	sb(OUT("fd1"), on);
	if ((in(IN("fd1")) > 0.5) != on)
	{
		ctl_writes[IN("fd1")] = on ? 1.0 : 0.0;
		prev_fd1 = on ? 1.0 : 0.0;
	}
}

void ItAutoflight::fd2_master(bool on)
{
	if (on && !b(OUT("fd1")) && !b(OUT("fd2")) && !b(OUT("ap1")) && !b(OUT("ap2"))) set_basic_mode();
	sb(OUT("fd2"), on);
	if ((in(IN("fd2")) > 0.5) != on)
	{
		ctl_writes[IN("fd2")] = on ? 1.0 : 0.0;
		prev_fd2 = on ? 1.0 : 0.0;
	}
}

void ItAutoflight::set_basic_mode()
{
	if (debug_modes) UtilityFunctions::print("ItAutoflight: set_basic_mode at ", elapsed(), " s (fd ", b(OUT("fd1")), "/", b(OUT("fd2")), " ap ", b(OUT("ap1")), "/", b(OUT("ap2")), ")");
	set_lat_mode(3);
	if (std::abs(in(INT("vert-speed-fpm"))) > 75.0) set_vert_mode(1);
	else set_vert_mode(0);
	start_blink(2);
	start_blink(3);
}

// ── modes ───────────────────────────────────────────────────────────────

void ItAutoflight::set_lat_mode(int n)
{
	const int vert = (int)g(OUT("vert"));
	if (n == 0)
	{ // HDG SEL
		update_lnav_arm(false);
		update_loc_arm(false);
		update_gs_arm(false);
		sb(OUT("hdg-in-hld"), false);
		s(OUT("lat"), 0);
		update_lat_text("HDG");
		if (vert == 2 || vert == 6) { set_vert_mode(1); start_blink(3); }
	}
	else if (n == 1)
	{ // LNAV
		update_loc_arm(false);
		update_gs_arm(false);
		check_lnav(0);
	}
	else if (n == 2)
	{ // VOR/LOC
		update_lnav_arm(false);
		check_loc(0);
	}
	else if (n == 3)
	{ // HDG HLD
		update_lnav_arm(false);
		update_loc_arm(false);
		update_gs_arm(false);
		s(INT("hdg-hld-target"), std::round(in(INT("heading-predicted"))));
		sb(OUT("hdg-in-hld"), true);
		s(OUT("lat"), 0);
		update_lat_text("HDG");
		if (vert == 2 || vert == 6) { set_vert_mode(1); start_blink(3); }
	}
	else if (n == 4)
	{ // ALIGN
		update_lnav_arm(false);
		update_loc_arm(false);
		update_gs_arm(false, 1);
		s(OUT("lat"), 4);
		update_lat_text("ALIGN");
	}
	else if (n == 5)
	{ // T/O or G/A
		update_lnav_arm(false);
		update_loc_arm(false);
		update_gs_arm(false);
		takeoff_logic(1);
		s(OUT("lat"), 5);
	}
	else if (n == 6)
	{ // LVL
		update_lnav_arm(false);
		update_loc_arm(false);
		update_gs_arm(false);
		s(OUT("lat"), 6);
		update_lat_text("LVL");
	}
	stop_blink(2);
}

void ItAutoflight::set_vert_mode(int n)
{
	if (debug_modes) UtilityFunctions::print("ItAutoflight: set_vert_mode(", n, ") at ", elapsed(), " s, vs ", in(INT("vert-speed-fpm")), " alt ", in("instrumentation/altimeter/indicated-altitude-ft"), " armed ", b(IN("alt-armed")));
	if (n == 0)
	{ // ALT HLD
		input_alt_armed(false);
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false);
		s(OUT("vert"), 0);
		reset_climb_rate_lim();
		update_vert_text("ALT HLD");
		sync_alt();
		athr_set_mode(0);
	}
	else if (n == 1)
	{ // V/S
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false);
		s(OUT("vert"), 1);
		update_vert_text("V/S");
		sync_vs();
		if ((int)g(OUT("thr-mode")) != 2) athr_set_mode(0);
	}
	else if (n == 2)
	{ // G/S
		update_lnav_arm(false);
		check_loc(0);
		stop_blink(2);
		check_gs(0);
	}
	else if (n == 3)
	{ // ALT CAP
		input_alt_armed(false);
		flch_active = false;
		s(OUT("vert"), 0);
		set_climb_rate_lim();
		alt_capture_active = true;
		update_vert_text("ALT CAP");
		athr_set_mode(0);
	}
	else if (n == 4)
	{ // FLCH
		update_gs_arm(false);
		alt_capture_active = false;
		s(OUT("vert"), 4);
		update_vert_text("FLCH");
		flch_active = true;
		if ((int)g(OUT("thr-mode")) != 2) athr_set_mode(3);
		if (b(IN("kts-mach-flch"))) sync_mach_flch();
		else sync_kts_flch();
	}
	else if (n == 6)
	{ // FLARE / ROLLOUT
		input_alt_armed(false);
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false, 1);
		s(OUT("vert"), 6);
		update_vert_text("FLARE");
	}
	else if (n == 7)
	{ // T/O CLB
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false);
		s(OUT("vert"), 7);
		update_vert_text("T/O CLB");
		athr_set_mode(2);
		input_kts_mach_flch(false);
	}
	else if (n == 8)
	{ // G/A CLB
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false);
		s(OUT("vert"), 8);
		update_vert_text("G/A CLB");
		athr_set_mode(2);
		input_kts_mach_flch(false);
	}
	else if (n == 10)
	{ // PITCH (TURB)
		input_alt_armed(false);
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false);
		sync_pitch();
		s(OUT("vert"), 10);
		update_vert_text("PITCH");
		athr_set_mode(0);
		athr_master(false);
	}
	stop_blink(3);
}

void ItAutoflight::bank_limit()
{
	static const double limits[5] = {10, 15, 20, 25, 30};
	const int lat = (int)g(OUT("lat"));
	const int sw = Math::clamp((int)std::lround(in(IN("bank-limit-sw"))), 0, 4);
	if (lat == 0 || (lat == 2 && !nav_loc(radio_sel()))) s(INT("bank-limit"), limits[sw]);
	else if (lat == 1) s(INT("bank-limit"), std::min(in(INT("bank-limit-auto")), opt_nav == 0.0 ? 27.0 : 25.0));
	else s(INT("bank-limit"), 30);
}

void ItAutoflight::activate_lnav()
{
	if ((int)g(OUT("lat")) != 1)
	{
		const int vert = (int)g(OUT("vert"));
		update_lnav_arm(false);
		update_loc_arm(false);
		update_gs_arm(false);
		s(OUT("lat"), 1);
		update_lat_text("LNAV");
		stop_blink(2);
		if (vert == 2 || vert == 6) { set_vert_mode(1); start_blink(3); }
	}
}

void ItAutoflight::activate_loc()
{
	if ((int)g(OUT("lat")) != 2)
	{
		update_lnav_arm(false);
		update_loc_arm(false);
		s(OUT("lat"), 2);
		update_lat_text("LOC");
		stop_blink(2);
	}
}

void ItAutoflight::activate_gs()
{
	if ((int)g(OUT("vert")) != 2)
	{
		input_alt_armed(false);
		flch_active = false;
		alt_capture_active = false;
		update_gs_arm(false, 1);
		s(OUT("vert"), 2);
		update_vert_text("G/S");
		stop_blink(3);
		athr_set_mode(0);
	}
}

void ItAutoflight::check_lnav(int)
{
	// no route manager: LNAV never engages or arms
	update_lnav_arm(false);
}

void ItAutoflight::check_loc(int t)
{
	const int r = radio_sel();
	if (inb(r == 1 ? "instrumentation/nav[1]/in-range" : "instrumentation/nav/in-range"))
	{
		const double hdg_err = in(r == 1 ? INT("nav2-heading-error-deg") : INT("nav1-heading-error-deg"));
		const double defl = in(r == 1 ? "instrumentation/nav[1]/heading-needle-deflection-norm" : "instrumentation/nav/heading-needle-deflection-norm");
		const double sq = in(r == 1 ? "instrumentation/nav[1]/signal-quality-norm" : "instrumentation/nav/signal-quality-norm");
		if (std::abs(defl) <= 0.95 && defl != 0.0 && sq >= 0.99)
		{
			if (std::abs(defl) <= 0.5) activate_loc();
			else if (defl >= 0.0 && hdg_err <= 0.0) activate_loc();
			else if (defl < 0.0 && hdg_err >= 0.0) activate_loc();
			else if (t != 1 && (int)g(OUT("lat")) != 2)
			{
				update_lnav_arm(false);
				update_loc_arm(true);
			}
		}
		else if (t != 1 && (int)g(OUT("lat")) != 2)
		{
			update_lnav_arm(false);
			update_loc_arm(true);
		}
	}
	else update_loc_arm(false);
}

void ItAutoflight::check_gs(int t)
{
	const int r = radio_sel();
	if (inb(r == 1 ? "instrumentation/nav[1]/in-range" : "instrumentation/nav/in-range"))
	{
		const double gsd = in(r == 1 ? "instrumentation/nav[1]/gs-needle-deflection-norm" : "instrumentation/nav/gs-needle-deflection-norm");
		if (std::abs(gsd) <= 0.2 && gsd != 0.0 && (int)g(OUT("lat")) == 2) activate_gs();
		else if (t != 1 && (int)g(OUT("vert")) != 2) update_gs_arm(true);
	}
	else update_gs_arm(false);
}

void ItAutoflight::check_radio_reversion(int l, int v)
{
	const int r = radio_sel();
	if (!inb(r == 1 ? "instrumentation/nav[1]/in-range" : "instrumentation/nav/in-range"))
	{
		if (l == 4 || v == 6)
		{
			ap1_master(false);
			ap2_master(false);
		}
		set_lat_mode(3);
		start_blink(2);
	}
}

void ItAutoflight::takeoff_logic(int t)
{
	if (!inb("gear/gear[1]/wow") && !inb("gear/gear[2]/wow"))
	{
		if (std::abs(in("orientation/roll-deg")) > 3.0)
		{
			s(INT("takeoff-hdg"), std::round(in(INT("heading-deg"))));
			sb(INT("takeoff-lvl"), true);
		}
		else
		{
			if (t == 1) s(INT("takeoff-hdg"), std::round(in(INT("heading-deg"))));
			sb(INT("takeoff-lvl"), false);
		}
	}
	else
	{
		s(INT("takeoff-hdg"), std::round(in(INT("heading-deg"))));
		sb(INT("takeoff-lvl"), true);
	}
}

void ItAutoflight::set_climb_rate_lim()
{
	const double vs = in(INT("vert-speed-fpm"));
	if (g(INT("alt")) >= in("instrumentation/altimeter/indicated-altitude-ft"))
	{
		s(INT("max-vs"), std::round(vs));
		s(INT("min-vs"), -500);
	}
	else
	{
		s(INT("max-vs"), 500);
		s(INT("min-vs"), std::round(vs));
	}
}

void ItAutoflight::reset_climb_rate_lim()
{
	s(INT("min-vs"), -500);
	s(INT("max-vs"), 500);
}

void ItAutoflight::takeoff_go_around()
{
	const int vert = (int)g(OUT("vert"));
	const double flap = in("fdm/jsbsim/fcs/flap-pos-deg");
	const bool wow1 = inb("gear/gear[1]/wow"), wow2 = inb("gear/gear[2]/wow");
	if (in("systems/dfgs/nlg-timer-20/wow-timer") < 1.0 && vert != 8 && in("position/gear-agl-ft") < 1500.0 && flap >= 25.9)
	{
		thrlim_set_mode(1); // G/A
		set_lat_mode(5);
		update_lat_text("G/A");
		set_vert_mode(8);
		if (wow1 || wow2)
		{
			ap1_master(false);
			ap2_master(false);
		}
	}
	else if ((wow1 || wow2) && flap >= 4.9)
	{
		if ((int)g(OUT("lat")) != 5) set_lat_mode(5);
		update_lat_text("T/O");
		set_vert_mode(7);
	}
}

void ItAutoflight::sync_kts_flch() { s(IN("kts-flch"), Math::clamp(std::round(in("instrumentation/airspeed-indicator/indicated-speed-kt")), 100.0, 340.0)); }
void ItAutoflight::sync_mach_flch()
{
	const double m = in("instrumentation/airspeed-indicator/indicated-mach");
	s(IN("mach-flch"), Math::clamp(roundto(m, 0.002), 0.5, 0.9));
}
void ItAutoflight::sync_hdg() { input_hdg(std::round(in(INT("heading-predicted")))); }
void ItAutoflight::sync_alt() { internal_alt(Math::clamp(roundto(in(INT("altitude-predicted")), 100.0), 0.0, 50000.0)); }
void ItAutoflight::sync_vs()
{
	const double vs = Math::clamp(roundto(in(INT("vert-speed-fpm")), 100.0), -6000.0, 6000.0);
	s(IN("vs"), vs);
}
void ItAutoflight::sync_pitch() { s(IN("pitch"), Math::clamp(std::round(in("orientation/pitch-deg")), -10.0, 25.0)); }

void ItAutoflight::kts_mach_changed()
{
	kts_mach_fgcp_time = -5.0;
	sb(IN("kts-mach-fgcp"), b(IN("kts-mach")));
}

void ItAutoflight::spd_push()
{
	if ((int)g(OUT("thr-mode")) == 0)
	{
		if (kts_mach_fgcp_time != -5.0)
		{
			kts_mach_fgcp_time = -5.0;
			sb(IN("kts-mach-fgcp"), b(IN("kts-mach")));
		}
		else
		{
			sb(IN("kts-mach-fgcp"), !b(IN("kts-mach")));
			kts_mach_fgcp_time = elapsed();
		}
	}
	else
	{
		input_kts_mach(!b(IN("kts-mach")));
		kts_mach_fgcp_time = -5.0;
		sb(IN("kts-mach-fgcp"), b(IN("kts-mach")));
	}
}

void ItAutoflight::spd_adjust_check()
{
	if (kts_mach_fgcp_time != -5.0) kts_mach_fgcp_time = elapsed();
}

void ItAutoflight::update_lat_text(const String &t) { ss("it-autoflight/text/lat", t); fma_lat(); }
void ItAutoflight::update_vert_text(const String &t) { ss("it-autoflight/text/vert", t); fma_vert(); }
void ItAutoflight::update_lnav_arm(bool n) { sb(OUT("lnav-arm"), n); fma_arm(); }
void ItAutoflight::update_loc_arm(bool n) { sb(OUT("loc-arm"), n); fma_arm(); }
void ItAutoflight::update_gs_arm(bool n, int t)
{
	sb(OUT("gs-arm"), n);
	if (!n && t != 1) update_auto_land(false);
	else fma_arm();
}
void ItAutoflight::update_auto_land(bool n)
{
	sb(IN("auto-land"), n);
	if (!n)
	{
		if (gs("it-autoflight/text/lat") == "LAND") update_lat_text("LOC");
		if (gs("it-autoflight/text/vert") == "LAND") update_vert_text("G/S");
	}
	fma_arm();
}
void ItAutoflight::update_vs_alt()
{
	if ((int)g(OUT("vert")) == 1) fma_vert();
}

// ── listeners (Nasal setlistener) ───────────────────────────────────────

void ItAutoflight::input_lat(int n) { s(IN("lat"), n); set_lat_mode(n); }
void ItAutoflight::input_vert(int n) { s(IN("vert"), n); set_vert_mode(n); }
void ItAutoflight::input_toga()
{
	takeoff_go_around();
	sb(IN("toga"), false);
}
void ItAutoflight::input_kts_mach(bool v)
{
	if (b(IN("kts-mach")) == v) return;
	sb(IN("kts-mach"), v);
	kts_mach_changed();
	fma_thr();
}
void ItAutoflight::input_kts_mach_flch(bool v)
{
	if (b(IN("kts-mach-flch")) == v) return;
	sb(IN("kts-mach-flch"), v);
	const int vert = (int)g(OUT("vert"));
	if (vert == 7 || vert == 8)
	{
		if (b(IN("kts-mach-flch"))) sb(IN("kts-mach-flch"), false);
	}
	else if (b(IN("kts-mach-flch"))) sync_mach_flch();
	else sync_kts_flch();
	fma_vert();
}
void ItAutoflight::input_alt(double v)
{
	const bool changed = g(IN("alt")) != v || !st.has(IN("alt-hundreds"));
	s(IN("alt"), v);
	if (!changed) return;
	const int64_t a = (int64_t)std::llround(v);
	char buf[16];
	snprintf(buf, sizeof(buf), "%03lld", (long long)(a % 1000));
	ss(IN("alt-hundreds"), String(buf));
	if (a < 1000) ss(IN("alt-thousands"), "==");
	else if (a < 10000) ss(IN("alt-thousands"), String("=") + String::num_int64(a / 1000));
	else ss(IN("alt-thousands"), String::num_int64(a / 1000));
	alt_rearm_time = elapsed();
}
void ItAutoflight::input_alt_armed(bool v)
{
	if (b(IN("alt-armed")) == v) return;
	sb(IN("alt-armed"), v);
	fma_arm();
}
void ItAutoflight::input_hdg(double v)
{
	s(IN("hdg"), v);
	s("autopilot/settings/heading-bug-deg", v);
}
void ItAutoflight::internal_alt(double v)
{
	if (g(INT("alt")) == v) return;
	s(INT("alt"), v);
	s("autopilot/settings/target-altitude-ft", v);
	fdm_writes["systems/caws/logic/altitude-alert-captured"] = 0.0;
	if ((int)std::lround(in("systems/caws/logic/altitude-alert")) == 2) fdm_writes["systems/caws/logic/altitude-alert"] = 0.0;
}
void ItAutoflight::kill_ap_warn()
{
	if (b("it-autoflight/sound/apoff"))
	{
		ap_kill_on = false;
		sb("it-autoflight/warning/ap", false);
		sb("it-autoflight/sound/apoff", false);
	}
}
void ItAutoflight::kill_ats_warn()
{
	if (b("it-autoflight/warning/atsflash"))
	{
		ats_kill_on = false;
		sb("it-autoflight/warning/ats", false);
		sb("it-autoflight/warning/atsflash", false);
	}
}

// ── FGCP (cockpit.nas ApPanel) ──────────────────────────────────────────

bool ItAutoflight::panel(const String &action, double d)
{
	if (!initialised) reset(0);
	const bool pw = fgcp_powered();
	if (action == "apSwitch")
	{
		if (b(OUT("ap1")) || b(OUT("ap2")))
		{
			ap1_master(false);
			ap2_master(false);
		}
		else if ((int)g(IN("active-ap")) == 2)
		{
			ap2_master(true);
			ap1_master(false);
		}
		else
		{
			ap1_master(true);
			ap2_master(false);
		}
	}
	else if (action == "apSelector")
	{
		s(IN("active-ap"), (int)g(IN("active-ap")) == 1 ? 2 : 1);
		fma_cap_trk_recheck();
	}
	else if (action == "apDisc")
	{
		if (inb("systems/caws/apoff/horn-silence-enabled")) kill_ap_warn();
		if (b(OUT("ap1"))) ap1_master(false);
		if (b(OUT("ap2"))) ap2_master(false);
	}
	else if (action == "atDisc")
	{
		kill_ats_warn();
		if (b(OUT("athr"))) athr_master(false);
	}
	else if (action == "spdPush")
	{
		if (pw) spd_push();
	}
	else if (action == "spdAdjust")
	{
		if (pw)
		{
			spd_adjust_check();
			if (b(IN("kts-mach-fgcp")))
			{
				if (std::abs(d) > 1.0) d = d / 2.0;
				const double m = roundto(g(IN("mach")) + d * 0.002, std::abs(d * 0.002));
				s(IN("mach"), Math::clamp(m, 0.5, 0.9));
			}
			else s(IN("kts"), Math::clamp(g(IN("kts")) + d, 100.0, 380.0));
		}
	}
	else if (action == "spdSel")
	{
		if (pw)
		{
			const int vert = (int)g(OUT("vert"));
			input_kts_mach(false);
			athr_set_mode(0);
			if (vert == 4 || vert == 7) { input_vert(1); start_blink(3); }
		}
	}
	else if (action == "machSel")
	{
		if (pw && in("instrumentation/airspeed-indicator/indicated-mach") >= 0.4995)
		{
			const int vert = (int)g(OUT("vert"));
			input_kts_mach(true);
			athr_set_mode(0);
			if (vert == 4 || vert == 7) { input_vert(1); start_blink(3); }
		}
	}
	else if (action == "eprLim")
	{
		if (pw) athr_set_mode(2);
	}
	else if (action == "hdgPush")
	{
		if (pw) input_lat(3);
	}
	else if (action == "hdgPull")
	{
		if (pw) input_lat(0);
	}
	else if (action == "hdgAdjust")
	{
		if (pw)
		{
			double h = g(IN("hdg")) + d;
			if (h < 0.5) h += 360.0;
			else if (h >= 360.5) h -= 360.0;
			input_hdg(h);
		}
	}
	else if (action == "nav")
	{
		if (pw) input_lat(1);
	}
	else if (action == "vorLoc")
	{
		if (pw) input_lat(2);
	}
	else if (action == "ils")
	{
		if (pw)
		{
			update_auto_land(false);
			input_vert(2);
		}
	}
	else if (action == "autoLand")
	{
		if (pw && (b(OUT("ap1")) || b(OUT("ap2"))))
		{
			update_auto_land(true);
			input_vert(2);
		}
	}
	else if (action == "vsAdjust")
	{
		if (pw)
		{
			const int vert = (int)g(OUT("vert"));
			if (vert == 1) s(IN("vs"), Math::clamp(g(IN("vs")) + d * 100.0, -6000.0, 6000.0));
			else if (vert == 4)
			{
				if (b(IN("kts-mach-flch")))
				{
					if (std::abs(d) > 1.0) d = d / 2.0;
					const double m = roundto(g(IN("mach-flch")) + d * -0.002, std::abs(d * 0.002));
					s(IN("mach-flch"), Math::clamp(m, 0.5, 0.9));
				}
				else s(IN("kts-flch"), Math::clamp(g(IN("kts-flch")) + d * -1.0, 100.0, 380.0));
			}
			else if (vert == 10)
			{
				const double p = g(IN("pitch")) + (std::abs(d) == 10.0 ? d * 0.5 : d);
				s(IN("pitch"), Math::clamp(p, -10.0, 25.0));
			}
			else input_vert(1);
		}
	}
	else if (action == "altPush")
	{
		if (pw)
		{
			input_alt_armed(false);
			fdm_writes["systems/caws/logic/altitude-alert-captured"] = 0.0;
			if ((int)std::lround(in("systems/caws/logic/altitude-alert")) == 2) fdm_writes["systems/caws/logic/altitude-alert"] = 0.0;
		}
	}
	else if (action == "altPull")
	{
		if (pw)
		{
			const int vert = (int)g(OUT("vert"));
			if (vert != 2 && vert != 6) input_alt_armed(true);
		}
	}
	else if (action == "altAdjust")
	{
		if (pw)
		{
			input_alt_armed(false);
			fdm_writes["systems/caws/logic/altitude-alert-captured"] = 0.0;
			if ((int)std::lround(in("systems/caws/logic/altitude-alert")) == 2) fdm_writes["systems/caws/logic/altitude-alert"] = 0.0;
			double a = g(IN("alt"));
			if (d == 10.0) a = a >= 1000.0 ? roundto(a + 1000.0, 100.0) : roundto(a + 100.0, 100.0);
			else if (d == -10.0) a = a > 1000.0 ? roundto(a - 1000.0, 100.0) : roundto(a - 100.0, 100.0);
			else a = roundto(a + d * 100.0, 100.0);
			input_alt(Math::clamp(a, 0.0, 50000.0));
		}
	}
	else if (action == "vertSpd")
	{
		if (pw) input_vert(1);
	}
	else if (action == "iasMach")
	{
		if (pw)
		{
			const double mach = in("instrumentation/airspeed-indicator/indicated-mach");
			if ((int)g(OUT("vert")) == 4)
			{
				if (mach >= 0.4995) input_kts_mach_flch(!b(IN("kts-mach-flch")));
				else input_kts_mach_flch(false);
			}
			else
			{
				input_kts_mach_flch(in("instrumentation/altimeter/indicated-altitude-ft") >= 26995.0 && mach >= 0.4995);
				input_vert(4);
			}
		}
	}
	else if (action == "altHold")
	{
		if (pw) input_vert(0);
	}
	else if (action == "turb")
	{
		if (pw)
		{
			input_lat(6);
			input_vert(10);
		}
	}
	else if (action == "toga")
	{
		if (pw)
		{
			sb(IN("toga"), true);
			input_toga();
		}
	}
	else if (action == "fd1")
	{
		fd1_master(d > 0.5);
	}
	else if (action == "fd2")
	{
		fd2_master(d > 0.5);
	}
	else if (action == "athr")
	{
		athr_master(d > 0.5);
	}
	else
	{
		return false;
	}
	return true;
}

// ── DFGS-b: FMA ─────────────────────────────────────────────────────────

void ItAutoflight::fma_loop()
{
	const double now = elapsed();
	if (now <= blink_time[0] + 5.0)
	{
		if (b(OUT("athr"))) blink_active[0] = true;
		else stop_blink(0);
	}
	else blink_active[0] = false;
	for (int w = 1; w < 4; w++) blink_active[w] = now <= blink_time[w] + 5.0;
	// the armed altitude follows the ALT knob while armed (the G/A arm texts own the window then)
	if (go_around == 0 && b(IN("alt-armed")) && !alt_arm_str.is_empty() && fl_string() != alt_arm_str) fma_alt_arm();
	const int vert = (int)g(OUT("vert"));
	if (vert != 4 && vert != 7 && !inb("position/wow")) spd_low = in("instrumentation/airspeed-indicator/indicated-speed-kt") <= in("systems/dfgs/speeds/vmin-90-percent");
	else spd_low = false;
}

void ItAutoflight::start_blink(int w)
{
	if (w == 0 && !b(OUT("athr"))) return;
	blink_time[w] = elapsed();
	blink_active[w] = true;
}
void ItAutoflight::start_long_blink(int w)
{
	if (w == 0 && !b(OUT("athr"))) return;
	blink_time[w] = 1e9;
	blink_active[w] = true;
}
void ItAutoflight::stop_blink(int w)
{
	blink_time[w] = -5.0;
	blink_active[w] = false;
}

void ItAutoflight::fma_cap_trk_recheck()
{
	fma_loc_update();
	loc_timer_on = true;
	fma_gs_update();
	gs_timer_on = true;
}

void ItAutoflight::fma_lat()
{
	const String t = gs("it-autoflight/text/lat");
	if (t == "HDG")
	{
		ss(FMA("roll-mode-a"), "HDG");
		ss(FMA("roll-mode-b"), b(OUT("hdg-in-hld")) ? "HLD" : "SEL");
	}
	else if (t == "LNAV") { ss(FMA("roll-mode-a"), "NAV"); ss(FMA("roll-mode-b"), "TRK"); }
	else if (t == "LOC")
	{
		ss(FMA("roll-mode-a"), nav_loc(radio_sel()) ? "LOC" : "VOR");
		fma_loc_update();
		loc_timer_on = true;
	}
	else if (t == "LAND") { ss(FMA("roll-mode-a"), "AUT"); ss(FMA("roll-mode-b"), "LND"); }
	else if (t == "ALIGN") { ss(FMA("roll-mode-a"), "ALN"); ss(FMA("roll-mode-b"), ""); }
	else if (t == "ROLLOUT") { ss(FMA("roll-mode-a"), "ROL"); ss(FMA("roll-mode-b"), "OUT"); }
	else if (t == "T/O") { ss(FMA("roll-mode-a"), "TAK"); ss(FMA("roll-mode-b"), "OFF"); }
	else if (t == "G/A") { ss(FMA("roll-mode-a"), "GO"); ss(FMA("roll-mode-b"), "RND"); }
	else if (t == "LVL") { ss(FMA("roll-mode-a"), "WNG"); ss(FMA("roll-mode-b"), "LVL"); }
}

void ItAutoflight::fma_vert()
{
	const String t = gs("it-autoflight/text/vert");
	if (t == "FLCH")
	{
		ss(FMA("pitch-mode-a"), b(IN("kts-mach-flch")) ? "MACH" : "IAS");
		ss(FMA("pitch-mode-b"), "");
	}
	else if (t == "T/O CLB") { ss(FMA("pitch-mode-a"), "TAK"); ss(FMA("pitch-mode-b"), "OFF"); }
	else if (t == "G/A CLB") { ss(FMA("pitch-mode-a"), "GO"); ss(FMA("pitch-mode-b"), "RND"); }
	else if (t == "ALT HLD") { ss(FMA("pitch-mode-a"), "ALT"); ss(FMA("pitch-mode-b"), "HLD"); }
	else if (t == "ALT CAP") { ss(FMA("pitch-mode-a"), "ALT"); ss(FMA("pitch-mode-b"), "CAP"); }
	else if (t == "V/S")
	{
		if (inb(INT("vs-alt-active"))) { ss(FMA("pitch-mode-a"), "ALT"); ss(FMA("pitch-mode-b"), "HLD"); }
		else { ss(FMA("pitch-mode-a"), "VERT"); ss(FMA("pitch-mode-b"), "SPD"); }
	}
	else if (t == "G/S")
	{
		ss(FMA("pitch-mode-a"), "G/S");
		fma_gs_update();
		gs_timer_on = true;
	}
	else if (t == "LAND")
	{
		ss(FMA("pitch-mode-a"), "AUT");
		ss(FMA("pitch-mode-b"), "LND");
		fma_arm();
	}
	else if (t == "NO FLARE") { ss(FMA("pitch-mode-a"), "NO"); ss(FMA("pitch-mode-b"), "FLR"); }
	else if (t == "FLARE") { ss(FMA("pitch-mode-a"), "FLAR"); ss(FMA("pitch-mode-b"), ""); }
	else if (t == "ROLLOUT") { ss(FMA("pitch-mode-a"), "ROL"); ss(FMA("pitch-mode-b"), "OUT"); }
	else if (t == "PITCH") { ss(FMA("pitch-mode-a"), "TURB"); ss(FMA("pitch-mode-b"), ""); }
}

void ItAutoflight::fma_arm()
{
	const String vt = gs("it-autoflight/text/vert");
	if (b(IN("auto-land")) && vt != "LAND" && vt != "ROLLOUT") { ss(FMA("arm-mode-a"), "LND"); fma_alt_arm(); }
	else if (b(OUT("gs-arm"))) { ss(FMA("arm-mode-a"), "ILS"); fma_alt_arm(); }
	else if (b(OUT("loc-arm"))) { ss(FMA("arm-mode-a"), nav_loc(radio_sel()) ? "LOC" : "VOR"); fma_alt_arm(); }
	else if (b(OUT("lnav-arm"))) { ss(FMA("arm-mode-a"), "NAV"); fma_alt_arm(); }
	else if (go_around == 3) { ss(FMA("arm-mode-a"), "AUT"); ss(FMA("arm-mode-b"), "G/A"); }
	else if (go_around == 2) { ss(FMA("arm-mode-a"), "F/D"); ss(FMA("arm-mode-b"), "G/A"); }
	else if (go_around == 1) { ss(FMA("arm-mode-a"), "MAN"); ss(FMA("arm-mode-b"), "G/A"); }
	else { ss(FMA("arm-mode-a"), ""); fma_alt_arm(); }
}

// The ARM window shows the armed altitude as a flight level (fma.nas with
// acconfig armed-alt-as-fl, the MD-80's default): 6 000 ft reads "060".
String ItAutoflight::fl_string() const
{
	return String::num_int64((int64_t)std::lround(g(IN("alt")) / 100.0)).pad_zeros(3);
}

void ItAutoflight::fma_alt_arm()
{
	alt_arm_str = b(IN("alt-armed")) ? fl_string() : String();
	ss(FMA("arm-mode-b"), alt_arm_str);
}

void ItAutoflight::fma_thr()
{
	const int tm = (int)g(OUT("thr-mode"));
	const int tri = (int)std::lround(in(K_ACTIVE_MODE_INT));
	if (tm == 3)
	{
		ss(FMA("thr-mode-a"), "CLMP");
		ss(FMA("thr-mode-b"), tri == 5 ? "FLX" : "");
	}
	else if (tm == 2)
	{
		ss(FMA("thr-mode-a"), "EPR");
		ss(FMA("thr-mode-b"), tri == 5 ? "FLX" : gs(K_ACTIVE_MODE));
	}
	else if (tm == 1) { ss(FMA("thr-mode-a"), "RETD"); ss(FMA("thr-mode-b"), ""); }
	else if (tm == 0) athr_mode_zero_check();
}

void ItAutoflight::fma_loc_update()
{
	if (gs("it-autoflight/text/lat") == "LOC")
	{
		const int r = radio_sel();
		if (std::abs(in(r == 1 ? "instrumentation/nav[1]/heading-needle-deflection-norm" : "instrumentation/nav/heading-needle-deflection-norm")) < 0.1)
		{
			loc_timer_on = false;
			ss(FMA("roll-mode-b"), "TRK");
		}
		else if (gs(FMA("roll-mode-b")) != "CAP") ss(FMA("roll-mode-b"), "CAP");
	}
	else loc_timer_on = false;
}

void ItAutoflight::fma_gs_update()
{
	if (gs("it-autoflight/text/vert") == "G/S")
	{
		const int r = radio_sel();
		if (std::abs(in(r == 1 ? "instrumentation/nav[1]/gs-needle-deflection-norm" : "instrumentation/nav/gs-needle-deflection-norm")) < 0.1)
		{
			gs_timer_on = false;
			ss(FMA("pitch-mode-b"), "TRK");
		}
		else if (gs(FMA("pitch-mode-b")) != "CAP") ss(FMA("pitch-mode-b"), "CAP");
	}
	else gs_timer_on = false;
}

// ── DFGS-b: autothrottle ────────────────────────────────────────────────

void ItAutoflight::athr_loop()
{
	tri_mode = (int)std::lround(in(K_ACTIVE_MODE_INT));
	const int tm = (int)g(OUT("thr-mode"));
	const int vert = (int)g(OUT("vert"));
	retard = b(OUT("athr")) && vert != 7 && vert != 8 && in("position/gear-agl-ft") <= 50.0 && in("fdm/jsbsim/fcs/flap-pos-deg") >= 27.9 && tri_mode != 0 && tri_mode != 5;
	if (tm == 0) athr_mode_zero_check();
	if (tri_mode == 0 || tri_mode == 5) athr_to_check();
	else if (retard && (int)g(OUT("thr-mode")) != 1)
	{
		s(OUT("thr-mode"), 1);
		fma_thr();
	}
	// Landed in RETD with the levers at idle: the ATS lets go (silently, as
	// FlightGear's model does when a reverser deploys) and the AUTO THROT
	// solenoid switch drops. FlightGear keeps it in RETD until reverse is
	// pulled; here the retard is the end of its job (gery, 2026-11-07), and
	// a reverser deploying (the R key) drops it through athr-avail as well.
	if (b(OUT("athr")) && tm == 1 && inb("gear/gear[1]/wow") && inb("gear/gear[2]/wow"))
	{
		const double idle = in("systems/engines/limit/idle-norm") + 0.02;
		if (in("systems/engines/control-1/throttle-pos") <= idle && in("systems/engines/control-2/throttle-pos") <= idle)
		{
			kill_athr_silent();
			fma_thr();
		}
	}
}

void ItAutoflight::athr_mode_zero_check()
{
	const bool km = b(IN("kts-mach"));
	const double pid = in(INT("throttle-cmd-raw-shape"));
	if (g(IN("mach")) < in("systems/dfgs/speeds/vmin-mach") - 1e-7 && km) { ss(FMA("thr-mode-a"), "ALFA"); ss(FMA("thr-mode-b"), "SPD"); }
	else if (g(IN("kts")) < in("systems/dfgs/speeds/vmin") && !km) { ss(FMA("thr-mode-a"), "ALFA"); ss(FMA("thr-mode-b"), "SPD"); }
	else if (g(IN("mach")) > in("systems/dfgs/speeds/vmax-mach") + 1e-7 && km) athr_set_vmax_check_fma();
	else if (g(IN("kts")) > in("systems/dfgs/speeds/vmax") && !km) athr_set_vmax_check_fma();
	else if (pid >= in("systems/engines/control-1/ats-max") - 0.005 || pid >= in("systems/engines/control-2/ats-max") - 0.005)
	{
		ss(FMA("thr-mode-a"), km ? "MACH" : "SPD");
		ss(FMA("thr-mode-b"), "ATL");
	}
	else if (pid <= in("systems/engines/limit/idle-norm") + 0.005) { ss(FMA("thr-mode-a"), "LOW"); ss(FMA("thr-mode-b"), "LIM"); }
	else if (km) { ss(FMA("thr-mode-a"), "MACH"); ss(FMA("thr-mode-b"), fmt3(g(IN("mach")) * 1000.0)); }
	else { ss(FMA("thr-mode-a"), "SPD"); ss(FMA("thr-mode-b"), fmt3(g(IN("kts")))); }
}

void ItAutoflight::athr_set_vmax_check_fma()
{
	const int t = (int)std::lround(in("systems/dfgs/speeds/vmax-type"));
	ss(FMA("thr-mode-a"), t == 0 ? "VMO" : (t == 1 ? "MMO" : (t == 2 ? "SLAT" : "FLAP")));
	ss(FMA("thr-mode-b"), "LIM");
}

void ItAutoflight::athr_set_mode(int n)
{
	tri_mode = (int)std::lround(in(K_ACTIVE_MODE_INT));
	const int vert = (int)g(OUT("vert"));
	if (tri_mode == 0 || tri_mode == 5) athr_to_check();
	else if (!retard || vert == 7 || vert == 8)
	{
		stop_blink(0);
		s(OUT("thr-mode"), n);
	}
	fma_thr();
}

void ItAutoflight::athr_to_check()
{
	if ((int)g(OUT("vert")) == 7)
	{
		if (in("instrumentation/airspeed-indicator/indicated-speed-kt") < 60.0 && inb("position/wow"))
		{
			if ((int)g(OUT("thr-mode")) != 2)
			{
				stop_blink(0);
				s(OUT("thr-mode"), 2);
				fma_thr();
			}
		}
		else if (inb("systems/dfgs/atr/cmd"))
		{
			thrlim_set_mode(1);
			if ((int)g(OUT("thr-mode")) != 2)
			{
				stop_blink(0);
				s(OUT("thr-mode"), 2);
				fma_thr();
			}
		}
		else if ((int)g(OUT("thr-mode")) != 3)
		{
			s(OUT("thr-mode"), 3);
			fma_thr();
			start_blink(0);
		}
	}
	else if ((int)g(OUT("thr-mode")) != 3)
	{
		s(OUT("thr-mode"), 3);
		fma_thr();
		start_blink(0);
	}
}

void ItAutoflight::athr_limit_changed(const String &l, const String &c)
{
	if ((l == "T/O" || l == "FLX") && (c != "T/O" && c != "FLX")) athr_set_mode(2);
	fma_thr();
}

// ── thrlim.nas ──────────────────────────────────────────────────────────

void ItAutoflight::thrlim_set_mode(int m)
{
	ctl_writes[K_ACTIVE_MODE_INT] = (double)m;
	cur[K_ACTIVE_MODE_INT] = (double)m;
	thrlim_prev_int = m;
	thrlim_update_txt();
}

void ItAutoflight::thrlim_update_txt()
{
	const int m = (int)std::lround(in(K_ACTIVE_MODE_INT));
	thrlim_last = gs(K_ACTIVE_MODE);
	String txt = "---";
	if (m == 0) txt = "T/O";
	else if (m == 1) txt = "G/A";
	else if (m == 2) txt = "MCT";
	else if (m == 3) txt = "CL ";
	else if (m == 4) txt = "CR ";
	else if (m == 5) txt = "FLX";
	ss(K_ACTIVE_MODE, txt);
	if (m == 0 || m == 5) athr_to_check();
	athr_limit_changed(thrlim_last, txt);
}

} // namespace jsbgodot
