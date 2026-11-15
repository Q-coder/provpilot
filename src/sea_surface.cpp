#include "sea_surface.h"

#include "geo.h"
#include "geo_frame.h"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cmath>

namespace jsbgodot
{

void SeaSurface::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("update", "cam", "sun_dir_world", "sun_color", "sun_energy", "sky_light", "wind_en", "delta"), &SeaSurface::update);
	ClassDB::bind_method(D_METHOD("set_enabled", "on"), &SeaSurface::set_enabled);
	ClassDB::bind_method(D_METHOD("get_enabled"), &SeaSurface::get_enabled);
	ClassDB::bind_method(D_METHOD("get_radius_m"), &SeaSurface::get_radius_m);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
}

SeaSurface::SeaSurface()
{
	set_as_top_level(true);
}

void SeaSurface::set_enabled(bool on) { enabled = on; }
bool SeaSurface::get_enabled() const { return enabled; }
double SeaSurface::get_radius_m() const { return built_radius; }

// A disc on the sphere of the local radius of curvature: rings at geometric
// radii (fine under the camera, coarse at the horizon), vertices dropped by
// the curvature. X west, Y up, Z north, the centre at (0, 0, 0).
void SeaSurface::build_mesh(double radius_m, double planet_radius)
{
	PackedVector3Array verts;
	PackedInt32Array idx;
	verts.push_back(Vector3(0, 0, 0));
	const double ratio = std::pow(radius_m / INNER_M, 1.0 / (RINGS - 1));
	double r = INNER_M;
	for (int i = 0; i < RINGS; i++)
	{
		const double ang = r / planet_radius;   // arc length -> angle at the centre
		const double horiz = planet_radius * std::sin(ang);
		const double drop = planet_radius * (1.0 - std::cos(ang));
		for (int s = 0; s < SEGMENTS; s++)
		{
			const double th = Math_TAU * (double)s / SEGMENTS;
			verts.push_back(Vector3(horiz * std::sin(th), -drop, horiz * std::cos(th)));
		}
		r *= ratio;
	}
	for (int s = 0; s < SEGMENTS; s++)
	{
		idx.push_back(0);
		idx.push_back(1 + s);
		idx.push_back(1 + (s + 1) % SEGMENTS);
	}
	for (int i = 0; i + 1 < RINGS; i++)
	{
		const int a = 1 + i * SEGMENTS, b = a + SEGMENTS;
		for (int s = 0; s < SEGMENTS; s++)
		{
			const int s1 = (s + 1) % SEGMENTS;
			idx.push_back(a + s);
			idx.push_back(b + s);
			idx.push_back(b + s1);
			idx.push_back(a + s);
			idx.push_back(b + s1);
			idx.push_back(a + s1);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (mesh_instance == nullptr)
	{
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_name("SeaMesh");
		mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mesh_instance);
		Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://shaders/sea_surface.gdshader");
		material.instantiate();
		if (shader.is_valid())
		{
			material->set_shader(shader);
		}
		// before the clouds (priority 100 and up): a cloud deck covers the sea below it
		material->set_render_priority(-20);
	}
	mesh_instance->set_mesh(mesh);
	mesh_instance->set_material_override(material);
	mesh_instance->set_custom_aabb(AABB(Vector3(-radius_m, -radius_m, -radius_m), Vector3(radius_m, radius_m, radius_m) * 2.0));
	built_radius = radius_m;
	built_planet_radius = planet_radius;
}

void SeaSurface::update(Camera3D *cam, const Vector3 &sun_dir_world, const Color &sun_color, double sun_energy, double sky_light,
		const Vector2 &wind_en, double delta)
{
	if (cam == nullptr)
	{
		return;
	}
	const Vector3 p = cam->get_global_position();
	if (!p.is_finite() || p.length() < 1.0e6)
	{
		return;
	}
	const geo::Geodetic g = geo::geodetic_of(p);
	const bool show = enabled && g.h < MAX_ALT_M && g.h > -5.0;
	set_visible(show);
	if (!show)
	{
		return;
	}
	// Gaussian mean radius of curvature at this latitude: the disc follows
	// the ellipsoid within a metre or two over the first 100 km
	const double a = 6378137.0, e2 = 0.00669437999014;
	const double sl = std::sin(Math::deg_to_rad(g.lat));
	const double w = 1.0 - e2 * sl * sl;
	const double radius_c = a * std::sqrt(1.0 - e2) / w;
	// to the horizon and a little beyond, in steps so the mesh is rebuilt rarely
	const double alt = Math::max(g.h, 2.0);
	const double horizon = std::sqrt(2.0 * radius_c * alt) * 1.15 + 3000.0;
	double want = 6000.0;
	while (want < horizon)
	{
		want *= 1.6;
	}
	if (mesh_instance == nullptr || want > built_radius || want < built_radius / 3.0 || std::fabs(radius_c - built_planet_radius) > 4000.0)
	{
		build_mesh(want, radius_c);
	}
	set_global_transform(Transform3D(geo::wun_basis(g.lat, g.lon), geo::ecef_of(g.lat, g.lon, 0.0)));
	const Transform3D gt = get_global_transform();
	// ripples anchored to the world: the frame origin on the LV95 grid,
	// wrapped (float precision in the shader), x = west, z = north
	const Vector2 lv = Geo::wgs84_to_lv95(g.lat, g.lon);
	const double wrap = 4096.0;
	const Vector2 world_offset(-Math::fposmod((double)lv.x, wrap), Math::fposmod((double)lv.y, wrap));
	// the ripples drift downwind at a fraction of the wind speed
	drift.x = Math::fposmod((double)drift.x - wind_en.x * 0.06 * delta, wrap);
	drift.y = Math::fposmod((double)drift.y + wind_en.y * 0.06 * delta, wrap);
	const double wind = wind_en.length();
	material->set_shader_parameter("cam_local", gt.affine_inverse().xform(p));
	material->set_shader_parameter("planet_radius", radius_c);
	material->set_shader_parameter("sun_dir", gt.basis.inverse().xform(sun_dir_world).normalized());
	material->set_shader_parameter("sun_color", sun_color);
	material->set_shader_parameter("sun_energy", sun_energy);
	material->set_shader_parameter("sky_light", sky_light);
	material->set_shader_parameter("world_offset", world_offset);
	material->set_shader_parameter("wind_offset", drift);
	material->set_shader_parameter("ripple", Math::clamp(0.45 + wind / 12.0, 0.45, 1.5));
}

} // namespace jsbgodot
