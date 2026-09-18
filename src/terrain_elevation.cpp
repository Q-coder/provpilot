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
	ClassDB::bind_method(D_METHOD("set_ecef", "ecef"), &TerrainElevation::set_ecef);
	ClassDB::bind_method(D_METHOD("process_rebase", "jsb", "terrain_height_at_ac", "scenery_valid"), &TerrainElevation::process_rebase);
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

void TerrainElevation::set_ecef(bool p_ecef)
{
	ecef = p_ecef;
}

void TerrainElevation::start_rebase(const Vector3 &pin_pos)
{
	_teleport_pin_pos = pin_pos;
	_rebase_cooldown = rebase_cooldown_frames;
}

void TerrainElevation::process_rebase(Object *p_jsb, double terrain_height_at_ac, bool scenery_valid)
{
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
		}
		double waited = static_cast<double>(now - static_cast<int64_t>(_rebase_retry_start_msec));
		if (waited < terrain_init_fallback_msec)
		{
			_rebase_cooldown = 1;
			return;
		}
		UtilityFunctions::print("Teleport: terrain elevation unavailable at (" + String::num_real(_teleport_pin_pos.x, 0) + ", " + String::num_real(_teleport_pin_pos.z, 0) + ") after " + String::num_real(waited / 1000.0, 0) + "s — releasing JSBSim at fallback elevation 0 m ASL");
		h = 0.0;
	}
	_rebase_retry_start_msec = 0.0;

	if (h > 0.1)
	{
		if (ecef)
		{
			const geo::Geodetic g = geo::geodetic_of(_teleport_pin_pos);
			_teleport_pin_pos = geo::ecef_of(g.lat, g.lon, h + 2.1); // terrain + gear clearance, along local up
		}
		else
		{
			_teleport_pin_pos.y = h + 2.1; // terrain + gear clearance
		}
		ac_node->set_global_position(_teleport_pin_pos);
		UtilityFunctions::print("Teleport: updated terrain height to " + String::num_real(h, 1) + "m");
	}

	// Re-initialise JSBSim at the final pinned position and release slew mode.
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm != nullptr)
	{
		if (ecef)
		{
			fdm->reinitialize_from_pose(ac_node->get_global_transform(), 0.0, h);
		}
		else
		{
			Vector3 final_pos = ac_node->get_global_position();
			double heading = ac_node->get_rotation().y;
			// h is already NAN-resolved and was queried at the same XZ as the AC, so it
			// stands in for the historical separate re-query of the ground height here.
			fdm->reinitialize_from_godot_position(final_pos, heading, h);
		}
		update_terrain_elevation(p_jsb, terrain_height_at_ac, scenery_valid);
		fdm->set_slew_mode(false);
	}
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

	// Convert Godot Y coordinate to JSBSim ASL elevation and push it (in metres —
	// the method converts to feet).
	fdm->set_terrain_elevation(terrain_height_at_ac + elevation_offset);
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
