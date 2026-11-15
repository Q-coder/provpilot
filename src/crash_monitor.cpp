#include "crash_monitor.h"

#include "jsbgodot.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

#include <cmath>

namespace jsbgodot
{

void CrashMonitor::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("configure", "cfg"), &CrashMonitor::configure);
	ClassDB::bind_method(D_METHOD("reset"), &CrashMonitor::reset);
	ClassDB::bind_method(D_METHOD("update", "jsb", "over_water", "delta"), &CrashMonitor::update);
	ClassDB::bind_method(D_METHOD("get_limits"), &CrashMonitor::get_limits);
	ClassDB::bind_method(D_METHOD("get_last_impact_sink_mps"), &CrashMonitor::get_last_impact_sink_mps);
}

// [limits] of aircraft.cfg: g_pos / g_neg (limit loads; the airframe fails at
// 1.5 times), vne_kt (indicated; 0 = not checked), touchdown_sink_mps (wheels
// first), airframe_sink_mps (anything else first).
void CrashMonitor::configure(const Ref<AircraftConfig> &cfg)
{
	g_pos = 3.8;
	g_neg = -1.52;
	vne_kt = 0.0;
	touchdown_sink_mps = 6.0;
	airframe_sink_mps = 3.0;
	if (cfg.is_valid())
	{
		g_pos = cfg->get_float("limits", "g_pos", g_pos);
		g_neg = cfg->get_float("limits", "g_neg", g_neg);
		vne_kt = cfg->get_float("limits", "vne_kt", vne_kt);
		touchdown_sink_mps = cfg->get_float("limits", "touchdown_sink_mps", touchdown_sink_mps);
		airframe_sink_mps = cfg->get_float("limits", "airframe_sink_mps", airframe_sink_mps);
	}
	reset();
}

void CrashMonitor::reset()
{
	grace = GRACE_S;
	was_wow = false;
	recent_sink = 0.0;
	bounce_timer = 0.0;
	g_timer = speed_timer = water_timer = 0.0;
}

Dictionary CrashMonitor::get_limits() const
{
	Dictionary d;
	d["g_pos"] = g_pos;
	d["g_neg"] = g_neg;
	d["vne_kt"] = vne_kt;
	d["touchdown_sink_mps"] = touchdown_sink_mps;
	d["airframe_sink_mps"] = airframe_sink_mps;
	return d;
}

double CrashMonitor::get_last_impact_sink_mps() const { return last_impact_sink; }

String CrashMonitor::update(Object *jsb_obj, bool over_water, double delta)
{
	JSBGodot *jsb = Object::cast_to<JSBGodot>(jsb_obj);
	if (jsb == nullptr || !jsb->is_initialized() || jsb->is_slew_mode())
	{
		return String();
	}
	if (grace > 0.0)
	{
		grace -= delta;
	}
	const bool armed = grace <= 0.0;
	const double sink = -jsb->get_property("velocities/h-dot-fps") * 0.3048;   // m/s, down positive
	bool any_wow = false, bogey_wow = false, airframe_wow = false;
	const Array contacts = jsb->get_gear_contacts();
	for (int i = 0; i < contacts.size(); i++)
	{
		const Dictionary c = contacts[i];
		if (bool(c["wow"]))
		{
			any_wow = true;
			if (bool(c["bogey"])) bogey_wow = true;
			else airframe_wow = true;
		}
	}
	String reason;
	if (!any_wow)
	{
		// the sink rate just before a touchdown (the contact frame itself is already braked)
		recent_sink = Math::max(sink, recent_sink * std::exp(-delta / 0.2));
	}
	if (any_wow && !was_wow)
	{
		last_impact_sink = Math::max(recent_sink, sink);
		if (last_impact_sink > BOUNCE_FROM_MPS)
		{
			bounce_timer = BOUNCE_WINDOW_S;
			bounce_limit = Math::max(BOUNCE_RESTITUTION * last_impact_sink, 0.8);
		}
		const double limit = (airframe_wow && !bogey_wow) ? airframe_sink_mps : touchdown_sink_mps;
		if (armed && last_impact_sink > limit)
		{
			reason = vformat("Hit the ground at %d ft/min", (int)std::lround(last_impact_sink * 196.85));
		}
		recent_sink = 0.0;
	}
	was_wow = any_wow;
	// A hard touchdown does not throw the aircraft back up: the rebound keeps
	// a share of the sink rate it arrived with (crash detection on or off).
	if (bounce_timer > 0.0)
	{
		bounce_timer -= delta;
		jsb->limit_climb_speed(bounce_limit);
	}
	if (armed && reason.is_empty() && any_wow)
	{
		const double roll = jsb->get_property("attitude/phi-deg");
		const double pitch = jsb->get_property("attitude/theta-deg");
		const double gs_kt = jsb->get_property("velocities/vg-fps") * 0.592484;
		if (std::fabs(roll) > 60.0 || pitch < -45.0 || pitch > 60.0)
		{
			reason = "Overturned on the ground";
		}
		else if (airframe_wow && gs_kt > 30.0 && (std::fabs(roll) > 15.0 || pitch < -3.0))
		{
			reason = std::fabs(roll) > 15.0 ? "Wing struck the ground" : "Nose struck the ground";
		}
		else if (over_water)
		{
			water_timer += delta;
			if (water_timer > 0.5)
			{
				reason = "Came down on water";
			}
		}
	}
	if (!any_wow || !over_water)
	{
		water_timer = 0.0;
	}
	if (armed && reason.is_empty() && !any_wow)
	{
		const double nz = jsb->get_property("accelerations/Nz");
		if (nz > g_pos * ULTIMATE_FACTOR || nz < g_neg * ULTIMATE_FACTOR)
		{
			g_timer += delta;
			if (g_timer > G_HOLD_S)
			{
				reason = vformat("Structural failure at %.1f g (limit %.1f / %.1f g)", nz, g_pos, g_neg);
			}
		}
		else
		{
			g_timer = 0.0;
		}
		if (vne_kt > 0.0 && jsb->get_property("velocities/vc-kts") > vne_kt * OVERSPEED_FACTOR)
		{
			speed_timer += delta;
			if (speed_timer > OVERSPEED_HOLD_S)
			{
				reason = vformat("Structural failure at %d kt (never exceed %d kt)", (int)std::lround(jsb->get_property("velocities/vc-kts")), (int)std::lround(vne_kt));
			}
		}
		else
		{
			speed_timer = 0.0;
		}
	}
	if (!reason.is_empty())
	{
		grace = 1.0e9;   // one report per flight; reset() arms the next
	}
	return reason;
}

} // namespace jsbgodot
