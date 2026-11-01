#include "terrain_elevation.h"
#include "geo_frame.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

void TerrainElevation::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "ac"), &TerrainElevation::register_nodes);
	ClassDB::bind_method(D_METHOD("start_rebase", "pin_pos"), &TerrainElevation::start_rebase);
	ClassDB::bind_method(D_METHOD("process_rebase", "jsb", "precise_h", "fallback_h", "scenery_valid", "streamer_busy"), &TerrainElevation::process_rebase);
	ClassDB::bind_method(D_METHOD("update_terrain_elevation", "jsb", "terrain_height_at_ac", "scenery_valid"), &TerrainElevation::update_terrain_elevation);
	ClassDB::bind_method(D_METHOD("get_rebase_cooldown"), &TerrainElevation::get_rebase_cooldown);
	ClassDB::bind_method(D_METHOD("get_rebase_cooldown_frames"), &TerrainElevation::get_rebase_cooldown_frames);
	ClassDB::bind_method(D_METHOD("get_teleport_pin_pos"), &TerrainElevation::get_teleport_pin_pos);
	ClassDB::bind_method(D_METHOD("set_rebase_cooldown_frames", "frames"), &TerrainElevation::set_rebase_cooldown_frames);
	ClassDB::bind_method(D_METHOD("set_terrain_init_fallback_msec", "msec"), &TerrainElevation::set_terrain_init_fallback_msec);
	ClassDB::bind_method(D_METHOD("set_terrain_init_delay_frames", "frames"), &TerrainElevation::set_terrain_init_delay_frames);
	ClassDB::bind_method(D_METHOD("set_elevation_offset", "offset"), &TerrainElevation::set_elevation_offset);
	ClassDB::bind_method(D_METHOD("get_elevation_offset"), &TerrainElevation::get_elevation_offset);
}

TerrainElevation::TerrainElevation()
{
}

void TerrainElevation::register_nodes(Node3D *p_ac)
{
	ac_node = p_ac;
}


void TerrainElevation::start_rebase(const Vector3 &pin_pos)
{
	_teleport_pin_pos = pin_pos;
	_rebase_cooldown = rebase_cooldown_frames;
}

void TerrainElevation::set_airborne_release(double agl_m, double airspeed_kts, double heading_deg)
{
	_airborne_agl_m = agl_m;
	_airborne_kts = airspeed_kts;
	_airborne_heading = heading_deg;
}

void TerrainElevation::process_rebase(Object *p_jsb, double precise_h, double fallback_h, bool scenery_valid, bool streamer_busy)
{
	const double terrain_height_at_ac = precise_h;
	if (ac_node == nullptr || _rebase_cooldown <= 0)
	{
		return;
	}

	// Pin the AC at the destination while the countdown runs.
	ac_node->set_global_position(_teleport_pin_pos);
	_rebase_cooldown -= 1;
	if (_rebase_cooldown != 0)
	{
		return;
	}

	// Countdown expired — terrain should be loaded by now. Use the scenery-stack
	// query at the pinned position (passed in; NAN when no data there).
	double h = terrain_height_at_ac;
	if (std::isnan(h))
	{
		// Tile still not loaded — keep pinning and retry next frame, but only up
		// to terrain_init_fallback_msec so we don't loop forever over nodata.
		int64_t now = Time::get_singleton()->get_ticks_msec();
		if (_rebase_retry_start_msec == 0.0)
		{
			_rebase_retry_start_msec = static_cast<double>(now);
			_rebase_last_busy_msec = _rebase_retry_start_msec;
		}
		if (streamer_busy)
		{
			_rebase_last_busy_msec = static_cast<double>(now);
		}
		double waited = static_cast<double>(now - static_cast<int64_t>(_rebase_retry_start_msec));
		const double idle = static_cast<double>(now - static_cast<int64_t>(_rebase_last_busy_msec));
		// Keep waiting while tiles are still streaming in (the tile under the
		// pin is usually among them); give up only after the streamer has been
		// idle for the fallback window, or at the hard cap.
		if (idle < terrain_init_fallback_msec && waited < REBASE_HARD_CAP_MSEC)
		{
			_rebase_cooldown = 1;
			return;
		}
		h = std::isnan(fallback_h) ? 0.0 : fallback_h;
		UtilityFunctions::print("Teleport: precise terrain unavailable at (" + String::num_real(_teleport_pin_pos.x, 0) + ", " + String::num_real(_teleport_pin_pos.z, 0) + ") after " + String::num_real(waited / 1000.0, 0) + "s — releasing JSBSim at the coarse globe elevation " + String::num_real(h, 1) + " m ASL");
	}
	_rebase_retry_start_msec = 0.0;
	_last_pushed_h = NAN;

	const bool airborne = _airborne_agl_m > 0.5 && _airborne_kts > 1.0;
	if (h > 0.1)
	{
		const geo::Geodetic g = geo::geodetic_of(_teleport_pin_pos);
		// a hair above the ground (as the first flight), or the airborne start height, along local up
		_teleport_pin_pos = geo::ecef_of(g.lat, g.lon, h + (airborne ? _airborne_agl_m : 0.5));
		ac_node->set_global_position(_teleport_pin_pos);
		UtilityFunctions::print("Teleport: updated terrain height to " + String::num_real(h, 1) + "m");
	}

	// Re-initialise JSBSim at the final pinned position and release slew mode.
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm != nullptr)
	{
		if (airborne)
		{
			fdm->initialize_airborne(h, _teleport_pin_pos, _airborne_heading, _airborne_agl_m, _airborne_kts);
		}
		else
		{
			fdm->reinitialize_from_pose(ac_node->get_global_transform(), 0.0, h);
		}
		update_terrain_elevation(p_jsb, h, scenery_valid);
		fdm->set_slew_mode(false);
	}
	_airborne_agl_m = 0.0; // one shot: the next pin (editor exit, teleport) releases on the ground unless told again
	UtilityFunctions::print("Teleport cooldown expired – JSBSim released at ", ac_node->get_global_position());
}

void TerrainElevation::update_terrain_elevation(Object *p_jsb, double terrain_height_at_ac, bool scenery_valid)
{
	// (The terrain_initialized gate is applied by GDScript before calling.)
	// Skip the first N frames to let JSBSim stabilise.
	terrain_init_frame_count += 1;
	if (terrain_init_frame_count < terrain_init_delay_frames)
	{
		return;
	}

	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr || !scenery_valid)
	{
		return;
	}

	// If the tile under the aircraft hasn't streamed in yet, do NOT push 0.0 to
	// JSBSim — that would make it think the ground is at sea level and produce a
	// multi-hundred-metre collision spike when the real tile loads. Leaving the
	// previous elevation in place keeps physics stable.
	if (std::isnan(terrain_height_at_ac))
	{
		return;
	}

	const double h = terrain_height_at_ac + elevation_offset;

	// A tile streaming in under the aircraft can raise the ground by tens of
	// metres in one frame (coarse globe -> precise tile). If that buries the
	// aircraft, JSBSim's gear springs would launch it (NaN pose, crash) — lift
	// it onto the new ground instead, keeping speed and heading. Only for a
	// terrain JUMP (not for flying into a mountain: crash detection owns that).
	const bool jumped = std::isnan(_last_pushed_h) || std::fabs(h - _last_pushed_h) > 1.0;
	_last_pushed_h = h;
	if (jumped && ac_node != nullptr && !fdm->is_slew_mode())
	{
		const double alt_m = fdm->get_altitude_ft() * 0.3048;
		const double buried = h - alt_m;  // > 0: CG below the new ground
		const int64_t now = Time::get_singleton()->get_ticks_msec();
		if (buried > 0.5 && now - _last_lift_msec > 500)
		{
			_last_lift_msec = now;
			Transform3D xf = ac_node->get_global_transform();
			const geo::Geodetic g = geo::geodetic_of(xf.origin);
			xf.origin = geo::ecef_of(g.lat, g.lon, h + 0.5);
			ac_node->set_global_transform(xf);
			const double kts = fdm->get_airspeed_knots();
			fdm->reinitialize_from_pose(xf, kts > 2.0 ? kts : 0.0, h);
			UtilityFunctions::print("Terrain: ground rose ", String::num_real(buried, 1), " m above the aircraft (tile loaded) — lifted onto it at ", String::num_real(h, 1), " m ASL");
		}
	}
	fdm->set_terrain_elevation(h);
}

int TerrainElevation::get_rebase_cooldown() const
{
	return _rebase_cooldown;
}

int TerrainElevation::get_rebase_cooldown_frames() const
{
	return rebase_cooldown_frames;
}

Vector3 TerrainElevation::get_teleport_pin_pos() const
{
	return _teleport_pin_pos;
}

void TerrainElevation::set_rebase_cooldown_frames(int frames)
{
	rebase_cooldown_frames = frames;
}

void TerrainElevation::set_terrain_init_fallback_msec(double msec)
{
	terrain_init_fallback_msec = msec;
}

void TerrainElevation::set_terrain_init_delay_frames(int frames)
{
	terrain_init_delay_frames = frames;
}

void TerrainElevation::set_elevation_offset(double offset)
{
	elevation_offset = offset;
}

double TerrainElevation::get_elevation_offset() const
{
	return elevation_offset;
}

} // namespace jsbgodot
