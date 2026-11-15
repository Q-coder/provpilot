#include "world_scenery.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

#include "geo.h"
#include "geo_frame.h"

using namespace godot;

namespace jsbgodot
{

void WorldScenery::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "host", "ac_node", "scenery3d", "globe", "vegetation"), &WorldScenery::register_nodes);
	ClassDB::bind_method(D_METHOD("set_runways", "runways"), &WorldScenery::set_runways);
	ClassDB::bind_method(D_METHOD("set_vfr_map", "vfr_map"), &WorldScenery::set_vfr_map);
	ClassDB::bind_method(D_METHOD("has_globe"), &WorldScenery::has_globe);
	ClassDB::bind_method(D_METHOD("get_globe"), &WorldScenery::get_globe);
	ClassDB::bind_method(D_METHOD("get_scenery3d"), &WorldScenery::get_scenery3d);
	ClassDB::bind_method(D_METHOD("get_vegetation"), &WorldScenery::get_vegetation);

	ClassDB::bind_static_method("WorldScenery", D_METHOD("geodetic_of", "world_pos"), &WorldScenery::geodetic_of);
	ClassDB::bind_static_method("WorldScenery", D_METHOD("latlon_of_lv95", "e", "n"), &WorldScenery::latlon_of_lv95);
	ClassDB::bind_static_method("WorldScenery", D_METHOD("ecef_of_lv95", "e", "n", "h"), &WorldScenery::ecef_of_lv95);
	ClassDB::bind_static_method("WorldScenery", D_METHOD("ac_transform_at", "e", "n", "h", "true_heading_deg", "scale"), &WorldScenery::ac_transform_at);
	ClassDB::bind_static_method("WorldScenery", D_METHOD("ac_true_heading", "ac"), &WorldScenery::ac_true_heading);
	ClassDB::bind_static_method("WorldScenery", D_METHOD("up_at", "world_pos"), &WorldScenery::up_at);
	ClassDB::bind_static_method("WorldScenery", D_METHOD("airfield_light_intensity", "cam_pos", "sun_dir", "visibility_km"), &WorldScenery::airfield_light_intensity);

	ClassDB::bind_method(D_METHOD("scenery_elevation_geodetic", "lat", "lon"), &WorldScenery::scenery_elevation_geodetic);
	ClassDB::bind_method(D_METHOD("globe_elevation_geodetic", "lat", "lon"), &WorldScenery::globe_elevation_geodetic);
	ClassDB::bind_method(D_METHOD("terrain_elevation_geodetic", "lat", "lon"), &WorldScenery::terrain_elevation_geodetic);
	ClassDB::bind_method(D_METHOD("terrain_elevation_below", "world_pos"), &WorldScenery::terrain_elevation_below);
	ClassDB::bind_method(D_METHOD("ground_height_geodetic", "lat", "lon"), &WorldScenery::ground_height_geodetic);
	ClassDB::bind_method(D_METHOD("ground_height_below", "world_pos"), &WorldScenery::ground_height_below);
	ClassDB::bind_method(D_METHOD("road_surface_at", "lat", "lon", "h_hint"), &WorldScenery::road_surface_at);
	ClassDB::bind_method(D_METHOD("max_terrain_around", "e", "n", "true_heading_deg"), &WorldScenery::max_terrain_around);
	ClassDB::bind_method(D_METHOD("count_loaded_ring", "e", "n", "radius_m"), &WorldScenery::count_loaded_ring);

	ClassDB::bind_method(D_METHOD("get_tile_manager"), &WorldScenery::get_tile_manager);
	ClassDB::bind_method(D_METHOD("pending_count"), &WorldScenery::pending_count);
	ClassDB::bind_method(D_METHOD("streamer_busy"), &WorldScenery::streamer_busy);
	ClassDB::bind_method(D_METHOD("set_streaming", "enabled"), &WorldScenery::set_streaming);

	ClassDB::bind_method(D_METHOD("mark_suppress_dirty"), &WorldScenery::mark_suppress_dirty);
	ClassDB::bind_method(D_METHOD("refresh_globe_suppress"), &WorldScenery::refresh_globe_suppress);
	ClassDB::bind_method(D_METHOD("push_suppress_from_coverage", "cells", "r_tiles", "cam_ei", "cam_ni", "tile_m"), &WorldScenery::push_suppress_from_coverage);

	ClassDB::bind_method(D_METHOD("feed_vfr_map", "world_pos", "true_heading_deg", "ui_up"), &WorldScenery::feed_vfr_map);
}

WorldScenery::WorldScenery()
{
}

void WorldScenery::register_nodes(Node *p_host, Node3D *p_ac_node, Node *p_scenery3d, Node *p_globe, Node *p_vegetation)
{
	host = p_host;
	ac_node = p_ac_node;
	scenery3d_id = p_scenery3d != nullptr ? p_scenery3d->get_instance_id() : 0;
	globe_id = p_globe != nullptr ? p_globe->get_instance_id() : 0;
	vegetation_id = p_vegetation != nullptr ? p_vegetation->get_instance_id() : 0;
}

void WorldScenery::set_runways(Node *p_runways) { runways_id = p_runways != nullptr ? p_runways->get_instance_id() : 0; }
void WorldScenery::set_vfr_map(VfrMap *p_vfr_map) { vfr_map = p_vfr_map; }
bool WorldScenery::has_globe() const { return globe_node() != nullptr; }
Node *WorldScenery::get_globe() const { return globe_node(); }
Node *WorldScenery::get_scenery3d() const { return scenery_node(); }
Node *WorldScenery::get_vegetation() const { return live_node(vegetation_id); }

Node *WorldScenery::live_node(uint64_t id)
{
	if (id == 0 || !UtilityFunctions::is_instance_id_valid(id))
	{
		return nullptr;
	}
	return Object::cast_to<Node>(ObjectDB::get_instance(id));
}

// ── Geodetic helpers ────────────────────────────────────────────────────────

Vector3 WorldScenery::geodetic_of(const Vector3 &world_pos)
{
	const geo::Geodetic g = geo::geodetic_of(world_pos);
	return Vector3(g.lat, g.lon, g.h);
}

Vector2 WorldScenery::latlon_of_lv95(double e, double n)
{
	return Geo::lv95_to_wgs84(e, n);
}

Vector3 WorldScenery::ecef_of_lv95(double e, double n, double h)
{
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	return geo::ecef_of(ll.x, ll.y, h);
}

Transform3D WorldScenery::ac_transform_at(double e, double n, double h, double true_heading_deg, const Vector3 &scale)
{
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	return Transform3D(geo::ac_basis(ll.x, ll.y, true_heading_deg, scale), geo::ecef_of(ll.x, ll.y, h));
}

double WorldScenery::ac_true_heading(Node3D *ac)
{
	if (ac == nullptr)
	{
		return 0.0;
	}
	const geo::Geodetic g = geo::geodetic_of(ac->get_global_position());
	return geo::true_heading_deg(ac->get_global_transform().basis, g.lat, g.lon);
}

Vector3 WorldScenery::up_at(const Vector3 &world_pos)
{
	return geo::up_at(world_pos);
}

// ── Elevation ───────────────────────────────────────────────────────────────

double WorldScenery::scenery_elevation_geodetic(double lat, double lon) const
{
	Node *scenery3d = scenery_node();
	if (scenery3d == nullptr || !scenery3d->has_method("get_elevation_geodetic"))
	{
		return NAN;
	}
	const double h = scenery3d->call("get_elevation_geodetic", lat, lon);
	// Swiss/BW terrain is never at exactly 0 m ASL; treat 0.0 as "no data".
	if (h == 0.0 || std::isnan(h))
	{
		return NAN;
	}
	return h;
}

double WorldScenery::globe_elevation_geodetic(double lat, double lon) const
{
	Node *globe = globe_node();
	if (globe == nullptr || !globe->has_method("get_elevation"))
	{
		return NAN;
	}
	return globe->call("get_elevation", lat, lon);
}

double WorldScenery::terrain_elevation_geodetic(double lat, double lon) const
{
	const double h = scenery_elevation_geodetic(lat, lon);
	if (!std::isnan(h))
	{
		return h;
	}
	return globe_elevation_geodetic(lat, lon);
}

double WorldScenery::terrain_elevation_below(const Vector3 &world_pos) const
{
	const geo::Geodetic g = geo::geodetic_of(world_pos);
	return terrain_elevation_geodetic(g.lat, g.lon);
}

double WorldScenery::runway_lift_at(const Vector3 &world_pos) const
{
	Node *runways = live_node(runways_id);
	if (runways == nullptr || !runways->has_method("surface_lift_at"))
	{
		return 0.0;
	}
	return runways->call("surface_lift_at", world_pos);
}

Node *WorldScenery::road_manager_node() const
{
	Node *rm = live_node(road_manager_id);
	if (rm != nullptr)
	{
		return rm;
	}
	Node *scenery = scenery_node();
	if (scenery == nullptr)
	{
		return nullptr;
	}
	// Scenery3D creates its S3DRoadManager child once it has road paths
	const TypedArray<Node> found = scenery->find_children("*", "S3DRoadManager", false, false);
	if (found.is_empty())
	{
		return nullptr;
	}
	rm = Object::cast_to<Node>(static_cast<Object *>(found[0]));
	road_manager_id = rm != nullptr ? rm->get_instance_id() : 0;
	return rm;
}

double WorldScenery::road_surface_at(double lat, double lon, double h_hint) const
{
	Node *rm = road_manager_node();
	if (rm == nullptr || !rm->has_method("surface_height_at"))
	{
		return NAN;
	}
	const Vector2 en = Geo::wgs84_to_lv95(lat, lon);
	return rm->call("surface_height_at", en.x, en.y, h_hint, ROAD_SURFACE_TOL_M);
}

double WorldScenery::ground_height_geodetic(double lat, double lon) const
{
	const double h = scenery_elevation_geodetic(lat, lon);
	if (std::isnan(h))
	{
		return NAN;
	}
	// the sea floor lies under water (scripts/build_world_sea.py): the
	// surface at 0 m is what carries a hull or stops a wheel
	const double ground = std::max(h + runway_lift_at(geo::ecef_of(lat, lon, h)), 0.0);
	const double road = road_surface_at(lat, lon, h);
	return std::isnan(road) ? ground : std::max(ground, road);
}

double WorldScenery::ground_height_below(const Vector3 &world_pos) const
{
	const geo::Geodetic g = geo::geodetic_of(world_pos);
	const double h = terrain_elevation_geodetic(g.lat, g.lon);
	if (std::isnan(h))
	{
		return NAN;
	}
	// the drawn ground is the top-most surface: pavement over terrain, a
	// road ribbon over both (a ribbon sunk into a terrain bump stays below)
	const double ground = std::max(h + runway_lift_at(world_pos), 0.0);   // the sea surface, not its floor
	const double road = road_surface_at(g.lat, g.lon, g.h);
	return std::isnan(road) ? ground : std::max(ground, road);
}

double WorldScenery::max_terrain_around(double e, double n, double true_heading_deg) const
{
	const Vector2 ll0 = Geo::lv95_to_wgs84(e, n);
	// Offsets along / across the runway in the LV95 grid (grid heading).
	const double hdg = Math::deg_to_rad(true_heading_deg + Geo::lv95_grid_convergence_deg(ll0.x, ll0.y));
	const double s = std::sin(hdg), c = std::cos(hdg);
	const Vector2 offsets[5] = {
		Vector2(0, 0),
		Vector2(s * 25.0, c * 25.0),
		Vector2(-s * 25.0, -c * 25.0),
		Vector2(c * 15.0, -s * 15.0),
		Vector2(-c * 15.0, s * 15.0),
	};
	double max_h = NAN;
	for (const Vector2 &off : offsets)
	{
		const Vector2 ll = Geo::lv95_to_wgs84(e + off.x, n + off.y);
		const double h = terrain_elevation_geodetic(ll.x, ll.y);
		if (!std::isnan(h) && (std::isnan(max_h) || h > max_h))
		{
			max_h = h;
		}
	}
	return max_h;
}

Vector2i WorldScenery::count_loaded_ring(double e, double n, double radius_m) const
{
	const Vector2 samples[5] = {
		Vector2(e, n),
		Vector2(e + radius_m, n),
		Vector2(e - radius_m, n),
		Vector2(e, n + radius_m),
		Vector2(e, n - radius_m),
	};
	int loaded = 0;
	for (const Vector2 &s : samples)
	{
		const Vector2 ll = Geo::lv95_to_wgs84(s.x, s.y);
		if (!std::isnan(terrain_elevation_geodetic(ll.x, ll.y)))
		{
			loaded++;
		}
	}
	return Vector2i(loaded, 5);
}

// ── Streaming ───────────────────────────────────────────────────────────────

Object *WorldScenery::get_tile_manager() const
{
	Node *scenery3d = scenery_node();
	if (scenery3d == nullptr || !scenery3d->has_method("get_tile_manager"))
	{
		return nullptr;
	}
	return scenery3d->call("get_tile_manager");
}

int WorldScenery::pending_count() const
{
	Object *tm = get_tile_manager();
	if (tm == nullptr || !tm->has_method("get_pending_count"))
	{
		return -1;
	}
	return tm->call("get_pending_count");
}

bool WorldScenery::streamer_busy() const
{
	return pending_count() > 0;
}

void WorldScenery::set_streaming(bool enabled)
{
	Node *scenery3d = scenery_node();
	if (scenery3d != nullptr)
	{
		const TypedArray<Node> children = scenery3d->get_children();
		for (int i = 0; i < children.size(); i++)
		{
			Node *child = Object::cast_to<Node>(children[i]);
			if (child != nullptr)
			{
				child->set_process(enabled);
			}
		}
	}
	Node *vegetation = live_node(vegetation_id);
	if (vegetation != nullptr)
	{
		vegetation->set_process(enabled);
	}
	UtilityFunctions::print("Scenery streaming ", enabled ? "ENABLED" : "DISABLED");
}

// ── Coarse-globe suppression ────────────────────────────────────────────────

void WorldScenery::mark_suppress_dirty()
{
	suppress_dirty = true;
}

Vector3 WorldScenery::stream_center() const
{
	// Same centre Scenery3D streams around (its update_tiles reads the
	// viewport's current camera); fall back to the AC when none is active.
	if (host != nullptr && host->is_inside_tree())
	{
		Viewport *vp = host->get_viewport();
		Camera3D *cam = vp != nullptr ? vp->get_camera_3d() : nullptr;
		if (cam != nullptr)
		{
			return cam->get_global_position();
		}
	}
	if (ac_node != nullptr)
	{
		return ac_node->get_global_position();
	}
	return Vector3(NAN, NAN, NAN);
}

void WorldScenery::refresh_globe_suppress()
{
	Node *globe = globe_node();
	if (globe == nullptr || !globe->has_method("set_coverage_mask"))
	{
		return;
	}
	const Vector3 p3 = stream_center();
	if (!p3.is_finite())
	{
		return;
	}
	// 2D proxy of the 3D position for the "moved ≥ 32 m" check.
	const Vector2 c(p3.x + p3.y, p3.z - p3.y);

	Object *tm = get_tile_manager();
	int64_t gen = -1;
	if (tm != nullptr)
	{
		gen = (int64_t)tm->call("get_coverage_generation");
	}
	if (!suppress_dirty && gen == suppress_cov_gen && suppress_last_center.distance_squared_to(c) < 1024.0)
	{
		return; // nothing moved, no tile/chunk appeared or disappeared
	}
	const int64_t now_ms = Time::get_singleton()->get_ticks_msec();
	if (!suppress_dirty && now_ms - suppress_last_ms < SUPPRESS_REFRESH_MS)
	{
		return; // throttle: the globe stays visible as fallback meanwhile
	}
	suppress_last_ms = now_ms;

	if (tm != nullptr)
	{
		push_suppress_from_coverage(tm->call("get_coverage_mask"), (int)tm->call("get_coverage_mask_radius"),
				(int)tm->call("get_cam_tile_ei"), (int)tm->call("get_cam_tile_ni"), (double)tm->call("get_tile_size"));
	}
	else
	{
		globe->call("clear_coverage_mask");
	}
	suppress_cov_gen = gen;
	suppress_last_center = c;
	suppress_dirty = false;
}

void WorldScenery::push_suppress_from_coverage(const PackedByteArray &cells, int r_tiles, int cam_ei, int cam_ni, double tile_m)
{
	Node *globe = globe_node();
	if (globe == nullptr)
	{
		return;
	}
	const int n = 2 * r_tiles + 1;
	if (cells.size() != (int64_t)n * n || tile_m <= 0.0)
	{
		globe->call("clear_coverage_mask");
		return;
	}
	// Mask plane = LV95 grid metres (mask frame 1): origin = the SW corner of
	// the grid, step +tile_m along E and N.
	const double e0 = (double)(cam_ei - r_tiles) * tile_m;
	const double n0 = (double)(cam_ni - r_tiles) * tile_m;
	globe->call("set_coverage_mask", cells, n, e0, n0, tile_m, tile_m);
}

// ── VFR map ─────────────────────────────────────────────────────────────────

void WorldScenery::feed_vfr_map(const Vector3 &world_pos, double true_heading_deg, bool ui_up)
{
	if (vfr_map == nullptr)
	{
		return;
	}
	// JSBSim's cached position is (0,0,0) until the first initialisation:
	// not a place on the planet; leave the map where it was.
	if (!world_pos.is_finite() || world_pos.length() < 1.0e6)
	{
		return;
	}
	const geo::Geodetic g = geo::geodetic_of(world_pos);
	const Vector2 lv = Geo::wgs84_to_lv95(g.lat, g.lon);
	const double grid_hdg = Math::fposmod(true_heading_deg + Geo::lv95_grid_convergence_deg(g.lat, g.lon), 360.0);
	vfr_map->update_lv95(lv.x, lv.y, grid_hdg, ui_up);
}

// ── Airfield lighting ───────────────────────────────────────────────────────

double WorldScenery::airfield_light_intensity(const Vector3 &cam_pos, const Vector3 &sun_dir, double visibility_km)
{
	if (visibility_km < 1.0)
	{
		return 1.0;
	}
	const Vector3 up = geo::up_at(cam_pos);
	const double sun_elev_deg = Math::rad_to_deg(std::asin(Math::clamp((double)sun_dir.dot(up), -1.0, 1.0)));
	return Math::clamp((4.0 - sun_elev_deg) / 8.0, 0.0, 1.0);
}

} // namespace jsbgodot
