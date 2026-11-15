#include "flight_spawn.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdio>

#include "geo.h"
#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

void FlightSpawn::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "ac_node", "jsb", "world", "terrain", "surfaces", "cockpit", "slew"), &FlightSpawn::register_nodes);
	ClassDB::bind_method(D_METHOD("park", "e", "n", "h", "true_heading_deg"), &FlightSpawn::park);
	ClassDB::bind_method(D_METHOD("request_flight", "e", "n", "true_heading_deg", "ride_camera"), &FlightSpawn::request_flight);
	ClassDB::bind_method(D_METHOD("is_first_flight"), &FlightSpawn::is_first_flight);
	ClassDB::bind_method(D_METHOD("process_deferred_init"), &FlightSpawn::process_deferred_init);
	ClassDB::bind_method(D_METHOD("is_init_pending"), &FlightSpawn::is_init_pending);
	ClassDB::bind_method(D_METHOD("is_initialized"), &FlightSpawn::is_initialized);
	ClassDB::bind_method(D_METHOD("get_target"), &FlightSpawn::get_target);
	ClassDB::bind_method(D_METHOD("has_target"), &FlightSpawn::has_target);
	ClassDB::bind_method(D_METHOD("process_rebase"), &FlightSpawn::process_rebase);
	ClassDB::bind_method(D_METHOD("update_terrain_elevation"), &FlightSpawn::update_terrain_elevation);
	ClassDB::bind_method(D_METHOD("set_start", "agl_m", "airspeed_kts", "heading_deg"), &FlightSpawn::set_start);
	ClassDB::bind_method(D_METHOD("is_airborne_start"), &FlightSpawn::is_airborne_start);
	// first: true after the first-flight initialisation, false after a teleport.
	ADD_SIGNAL(MethodInfo("spawned", PropertyInfo(Variant::BOOL, "first")));
}

FlightSpawn::FlightSpawn()
{
}

void FlightSpawn::register_nodes(Node3D *p_ac_node, Object *p_jsb, WorldScenery *p_world, TerrainElevation *p_terrain,
		ControlSurfaces *p_surfaces, CockpitControls *p_cockpit, SlewMode *p_slew)
{
	ac_node = p_ac_node;
	jsb = p_jsb;
	world = p_world;
	terrain = p_terrain;
	surfaces = p_surfaces;
	cockpit = p_cockpit;
	slew = p_slew;
}

bool FlightSpawn::scenery_valid() const
{
	return world != nullptr && world->get_scenery3d() != nullptr;
}

void FlightSpawn::park(double e, double n, double h, double true_heading_deg)
{
	if (ac_node == nullptr)
	{
		return;
	}
	ac_node->set_global_transform(WorldScenery::ac_transform_at(e, n, h, true_heading_deg, ac_node->get_scale()));
}

bool FlightSpawn::is_first_flight() const { return first_flight; }
bool FlightSpawn::is_init_pending() const { return init_pending; }
bool FlightSpawn::is_initialized() const { return initialized; }
bool FlightSpawn::has_target() const { return has_target_v; }

Dictionary FlightSpawn::get_target() const
{
	Dictionary d;
	if (has_target_v)
	{
		d["swiss_e"] = target_e;
		d["swiss_n"] = target_n;
		d["heading_deg"] = target_heading;
	}
	return d;
}

void FlightSpawn::set_start(double agl_m, double airspeed_kts, double heading_deg)
{
	start_agl_m = Math::max(agl_m, 0.0);
	start_airspeed_kts = Math::max(airspeed_kts, 0.0);
	start_heading_deg = heading_deg;
}

bool FlightSpawn::is_airborne_start() const
{
	return start_agl_m > 0.5 && start_airspeed_kts > 1.0;
}

bool FlightSpawn::request_flight(double e, double n, double true_heading_deg, Camera3D *ride_camera)
{
	if (!std::isnan(start_heading_deg))
	{
		true_heading_deg = Math::fposmod(start_heading_deg, 360.0);
	}
	if (first_flight)
	{
		begin_first_flight(e, n, true_heading_deg);
		first_flight = false;
		return false;
	}
	return teleport(e, n, true_heading_deg, ride_camera);
}

// ── First flight ────────────────────────────────────────────────────────────

void FlightSpawn::begin_first_flight(double e, double n, double true_heading_deg)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "=== FIRST FLIGHT: loading scenery for E=%.0f N=%.0f ===", e, n);
	UtilityFunctions::print(buf);
	if (ac_node == nullptr)
	{
		UtilityFunctions::push_error("FlightSpawn: missing AC node");
		return;
	}
	// Place the aircraft over the target (the height is corrected once the
	// terrain has loaded): level ECEF pose at the LV95 target, true heading.
	park(e, n, 500.0, true_heading_deg);
	target_e = e;
	target_n = n;
	target_heading = true_heading_deg;
	has_target_v = true;

	// Now that the camera-tracking AC is at its real destination, turn the
	// scenery streaming on: the tile manager's first tick queues tiles
	// around the airport instead of around the origin.
	if (world != nullptr)
	{
		world->set_streaming(true);
	}
	// JSBSim is initialised in a later frame, once the terrain under the
	// target has streamed in.
	init_pending = true;
	init_start_msec = Time::get_singleton()->get_ticks_msec();
	init_last_busy_msec = 0;
	init_last_print_msec = 0;
	// Pin the AC here while the terrain settles and JSBSim stabilises.
	if (terrain != nullptr)
	{
		terrain->start_rebase(ac_node->get_global_position());
	}
}

bool FlightSpawn::process_deferred_init()
{
	if (!init_pending)
	{
		return false;
	}
	JSBGodot *fdm = Object::cast_to<JSBGodot>(jsb);
	if (ac_node == nullptr || fdm == nullptr || world == nullptr)
	{
		init_pending = false;
		UtilityFunctions::print("Cannot initialize JSBSim on terrain - missing AC, JSBGodot or scenery");
		return false;
	}
	double e = target_e, n = target_n, heading = target_heading;
	if (!has_target_v)
	{
		UtilityFunctions::push_error("FlightSpawn: no flight target - using the current AC position");
		const Vector3 g0 = WorldScenery::geodetic_of(ac_node->get_global_position());
		const Vector2 lv0 = Geo::wgs84_to_lv95(g0.x, g0.y);
		e = lv0.x;
		n = lv0.y;
		heading = WorldScenery::ac_true_heading(ac_node);
	}
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	const int64_t now = Time::get_singleton()->get_ticks_msec();
	char buf[200];

	// Spawn on the PRECISE scenery patch when available: wait for the tile
	// to stream in rather than initialising on the coarse globe and jumping
	// ~100+ m when the fine tile loads.
	double api_height = world->scenery_elevation_geodetic(ll.x, ll.y);
	if (std::isnan(api_height))
	{
		if (world->streamer_busy() || init_last_busy_msec == 0)
		{
			init_last_busy_msec = now;
		}
		const int64_t waited = now - init_start_msec;
		// Wait while tiles are still streaming (the fallback window counts
		// from the last busy moment), never longer than the hard cap.
		if (now - init_last_busy_msec < TERRAIN_INIT_FALLBACK_MSEC && waited < TERRAIN_INIT_HARD_CAP_MSEC)
		{
			if (now - init_last_print_msec >= 1000)
			{
				init_last_print_msec = now;
				snprintf(buf, sizeof(buf), "Waiting for terrain at E=%.0f N=%.0f hdg=%.1f (%.0f s, streamer %s)",
						e, n, heading, waited / 1000.0, world->streamer_busy() ? "busy" : "idle");
				UtilityFunctions::print(buf);
			}
			return false;
		}
		// No detailed patch here (outside the installed regions): fall back
		// to the coarse globe so we still spawn instead of hanging.
		api_height = world->globe_elevation_geodetic(ll.x, ll.y);
		if (std::isnan(api_height) || api_height < 0.0)
		{
			api_height = 0.0;   // the globe carries the sea floor: spawn on the water
		}
		snprintf(buf, sizeof(buf), "Scenery patch elevation unavailable at E=%.0f N=%.0f after %.0fs - spawning on globe/fallback elevation %.0f m ASL",
				e, n, waited / 1000.0, api_height);
		UtilityFunctions::push_warning(buf);
	}
	else
	{
		snprintf(buf, sizeof(buf), "=== Scenery3D elevation at E=%.0f N=%.0f = %.1f m ===", e, n, api_height);
		UtilityFunctions::print(buf);
	}

	// Place the aircraft a hair above the terrain (ECEF pose, level, true
	// heading) so JSBSim's gear springs barely have to settle; JSBSim takes
	// over position control after initialisation.
	const bool airborne = is_airborne_start();
	const double start_h = api_height + (airborne ? start_agl_m : SPAWN_GEAR_HEIGHT);
	ac_node->set_global_transform(WorldScenery::ac_transform_at(e, n, start_h, heading, ac_node->get_scale()));
	// Scenery3D Y is already ASL: no Godot Y -> ASL offset.
	double jsb_elevation = api_height;
	if (terrain != nullptr)
	{
		terrain->set_elevation_offset(0.0);
		jsb_elevation += terrain->get_elevation_offset();
	}
	fdm->set_godot_terrain_y_offset(WHEEL_GROUND_CLEARANCE);
	const Vector3 start_pos = ac_node->get_global_position();
	if (airborne)
	{
		fdm->initialize_airborne(jsb_elevation, start_pos, heading, start_agl_m, start_airspeed_kts);
	}
	else
	{
		fdm->initialize_at_terrain(jsb_elevation, start_pos, heading);
	}
	snprintf(buf, sizeof(buf), "=== JSBSim initialized at %.1f m ASL (terrain %.1f m), heading %.1f%s ===", start_h, api_height, heading, airborne ? ", airborne" : "");
	UtilityFunctions::print(buf);
	if (terrain != nullptr)
	{
		terrain->set_airborne_release(airborne ? start_agl_m : 0.0, start_airspeed_kts, heading);
	}
	init_pending = false;
	initialized = true;
	emit_signal("spawned", true);
	return true;
}

// ── Teleport ────────────────────────────────────────────────────────────────

bool FlightSpawn::teleport(double e, double n, double true_heading_deg, Camera3D *ride_camera)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "=== TELEPORT: E=%.0f N=%.0f heading=%.0f ===", e, n, true_heading_deg);
	UtilityFunctions::print(buf);
	JSBGodot *fdm = Object::cast_to<JSBGodot>(jsb);
	if (ac_node == nullptr || fdm == nullptr || world == nullptr)
	{
		UtilityFunctions::push_error("FlightSpawn: cannot teleport - missing AC, JSBGodot or scenery");
		return false;
	}

	// Pause the physics position updates while repositioning.
	fdm->set_slew_mode(true);
	// Fly! is a fresh session: no throttle, stick, trim, flaps or brakes carry
	// over ("Continue Flight" keeps everything). reset_controls() puts
	// JSBSim's gear down; mirror that in the cockpit command and snap the
	// visual (legs / imported clip) to down as well.
	fdm->reset_controls();
	if (cockpit != nullptr)
	{
		cockpit->set_gear_down(true);
	}
	if (surfaces != nullptr)
	{
		surfaces->reset_gear(true);
	}

	// Terrain at the threshold: the highest of five samples along / across
	// the runway, matching the runway mesh approach.
	double terrain_height = world->max_terrain_around(e, n, true_heading_deg);
	if (std::isnan(terrain_height))
	{
		terrain_height = 0.0;
		UtilityFunctions::print("  WARNING: no terrain data at the target - using 0 m");
	}
	else
	{
		snprintf(buf, sizeof(buf), "  Terrain height at target (max of 5 samples): %.1f m", terrain_height);
		UtilityFunctions::print(buf);
	}

	// On the drawn runway surface with the same gear clearance as the first
	// flight.
	const bool airborne = is_airborne_start();
	ac_node->set_global_transform(WorldScenery::ac_transform_at(e, n,
			terrain_height + (airborne ? start_agl_m : RUNWAY_SURFACE_OFFSET + SPAWN_GEAR_HEIGHT), true_heading_deg, ac_node->get_scale()));

	// Reinitialise JSBSim at the new position but keep slew ON: physics stays
	// paused until the rebase cooldown expires (TerrainElevation releases it,
	// re-running the on-ground initialisation unless the start is airborne).
	if (airborne)
	{
		fdm->initialize_airborne(terrain_height, ac_node->get_global_position(), true_heading_deg, start_agl_m, start_airspeed_kts);
	}
	else
	{
		fdm->reinitialize_from_pose(ac_node->get_global_transform(), 0.0, terrain_height);
	}
	emit_signal("spawned", false);
	update_terrain_elevation();

	// The slew module's state + HUD are cleared so the slew UI is not shown.
	if (slew != nullptr)
	{
		slew->note_external_exit();
	}
	// Pin the AC here while JSBSim stabilises (released on the ground, or
	// re-initialised airborne for an airborne start).
	if (terrain != nullptr)
	{
		terrain->set_airborne_release(airborne ? start_agl_m : 0.0, start_airspeed_kts, true_heading_deg);
		terrain->start_rebase(ac_node->get_global_position());
	}

	// The tile streamer follows the viewport camera: make sure it is one that
	// rides with the pinned aircraft, or the destination tiles never load.
	bool switched = false;
	if (ac_node->is_inside_tree())
	{
		Viewport *vp = ac_node->get_viewport();
		Camera3D *cur = vp != nullptr ? vp->get_camera_3d() : nullptr;
		if ((cur == nullptr || !ac_node->is_ancestor_of(cur)) && ride_camera != nullptr)
		{
			ride_camera->set_current(true);
			switched = true;
		}
	}
	UtilityFunctions::print("=== TELEPORT COMPLETE ===");
	return switched;
}

// ── Per-frame pushes ────────────────────────────────────────────────────────

void FlightSpawn::process_rebase()
{
	if (!initialized || terrain == nullptr || world == nullptr)
	{
		return;
	}
	const Vector3 pin = terrain->get_teleport_pin_pos();
	const Vector3 g = WorldScenery::geodetic_of(pin);
	terrain->process_rebase(jsb, world->scenery_elevation_geodetic(g.x, g.y),
			world->globe_elevation_geodetic(g.x, g.y), scenery_valid(), world->streamer_busy());
}

void FlightSpawn::update_terrain_elevation()
{
	if (!initialized || terrain == nullptr || world == nullptr || ac_node == nullptr)
	{
		return;
	}
	// JSBSim's ground is the DRAWN surface: the runway / taxiway / apron
	// meshes sit a few centimetres above the flattened terrain (design §12.5).
	terrain->update_terrain_elevation(jsb, world->ground_height_below(ac_node->get_global_position()), scenery_valid());
}

} // namespace jsbgodot
