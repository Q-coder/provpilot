#include "cloud_layers.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "geo.h"
#include "geo_frame.h"

using namespace godot;

namespace jsbgodot
{

static const int STEPS_BY_QUALITY[4] = {0, 20, 32, 56}; // 0 = off

void CloudLayers::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &CloudLayers::set_layers);
	ClassDB::bind_method(D_METHOD("update", "cam", "sun_dir_world", "sun_color", "sun_energy", "ambient_color", "ambient_energy", "fog_density", "fog_color", "delta"), &CloudLayers::update);
	ClassDB::bind_method(D_METHOD("get_quality"), &CloudLayers::get_quality);
	ClassDB::bind_method(D_METHOD("set_quality", "quality"), &CloudLayers::set_quality);
	ClassDB::bind_method(D_METHOD("get_meshes"), &CloudLayers::get_meshes);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "quality"), "set_quality", "get_quality");
}

CloudLayers::CloudLayers()
{
}

int CloudLayers::get_quality() const { return quality; }

void CloudLayers::set_quality(int q)
{
	quality = Math::clamp(q, 0, 3);
	apply_quality();
}

Array CloudLayers::get_meshes() const
{
	Array out;
	for (int i = 0; i < meshes.size(); i++)
	{
		out.push_back(meshes[i]);
	}
	return out;
}

Ref<NoiseTexture3D> CloudLayers::make_noise3d(int size, int type, int octaves, int seed)
{
	Ref<NoiseTexture3D> t = memnew(NoiseTexture3D);
	t->set_width(size);
	t->set_height(size);
	t->set_depth(size);
	t->set_seamless(true);
	Ref<FastNoiseLite> n = memnew(FastNoiseLite);
	n->set_noise_type((FastNoiseLite::NoiseType)type);
	n->set_fractal_octaves(octaves);
	n->set_frequency(0.05);
	n->set_seed(seed);
	if (type == FastNoiseLite::TYPE_CELLULAR)
	{
		n->set_cellular_return_type(FastNoiseLite::RETURN_DISTANCE);
		t->set_invert(true); // worley "puffs": bright cells, dark edges
	}
	t->set_noise(n);
	return t;
}

void CloudLayers::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	volume_shader = ResourceLoader::get_singleton()->load("res://shaders/cloud_layer.gdshader");
	sheet_shader = ResourceLoader::get_singleton()->load("res://shaders/cloud_sheet.gdshader");
	shape_noise = make_noise3d(64, FastNoiseLite::TYPE_PERLIN, 3, 1);
	detail_noise = make_noise3d(32, FastNoiseLite::TYPE_CELLULAR, 2, 7);
	sheet_noise = memnew(NoiseTexture2D);
	sheet_noise->set_width(256);
	sheet_noise->set_height(256);
	sheet_noise->set_seamless(true);
	Ref<FastNoiseLite> n = memnew(FastNoiseLite);
	n->set_noise_type(FastNoiseLite::TYPE_PERLIN);
	n->set_fractal_octaves(4);
	n->set_frequency(0.02);
	n->set_seed(3);
	sheet_noise->set_noise(n);
}

void CloudLayers::set_layers(const Array &p_layers)
{
	layers = p_layers.duplicate(true);
	for (int i = 0; i < meshes.size(); i++)
	{
		meshes[i]->queue_free();
	}
	meshes.clear();
	wind.clear();
	offsets.clear();
	mids.clear();
	for (int i = 0; i < layers.size(); i++)
	{
		const Dictionary l = layers[i];
		const int ct = (int)(double)l.get("cloud_type", 0);
		const int oktas = (int)(double)l.get("coverage_oktas", 0);
		if (ct <= 0 || oktas <= 0)
		{
			continue;
		}
		const double base = l.get("base_m", 0.0);
		const double top = Math::max((double)l.get("top_m", base + 100.0), base + 100.0);
		MeshInstance3D *mi = memnew(MeshInstance3D);
		Ref<ShaderMaterial> mat = memnew(ShaderMaterial);
		if (ct <= CT_CUMULONIMBUS)
		{
			mat->set_shader(volume_shader);
			Ref<BoxMesh> box = memnew(BoxMesh);
			// Contains the curved shell out to BOX_HALF_M (sag ~ d^2 / 2R).
			const double sag = BOX_HALF_M * BOX_HALF_M / (2.0 * PLANET_RADIUS);
			box->set_size(Vector3(2.0 * BOX_HALF_M, (top - base) + sag + 200.0, 2.0 * BOX_HALF_M));
			mi->set_mesh(box);
			mi->set_position(Vector3(0.0, (base + top) * 0.5 - sag * 0.5 + 50.0, 0.0));
			mat->set_shader_parameter("mesh_offset", mi->get_position());
			mat->set_shader_parameter("shape_noise", shape_noise);
			mat->set_shader_parameter("detail_noise", detail_noise);
			mat->set_shader_parameter("top_m", top);
			mat->set_shader_parameter("steps", STEPS_BY_QUALITY[quality]);
			if (ct == CT_CUMULONIMBUS)
			{
				mat->set_shader_parameter("density_scale", 1.6);
			}
			else if (ct == CT_STRATUS)
			{
				mat->set_shader_parameter("density_scale", 0.8);
			}
		}
		else
		{
			mat->set_shader(sheet_shader);
			Ref<PlaneMesh> plane = memnew(PlaneMesh);
			plane->set_size(Vector2(2.0 * SHEET_HALF_M, 2.0 * SHEET_HALF_M));
			plane->set_subdivide_width(32);
			plane->set_subdivide_depth(32);
			mi->set_mesh(plane);
			mat->set_shader_parameter("sheet_noise", sheet_noise);
		}
		mat->set_shader_parameter("base_m", base);
		mat->set_shader_parameter("coverage", (double)oktas / 8.0);
		mat->set_shader_parameter("cloud_type", ct);
		mat->set_shader_parameter("planet_radius", PLANET_RADIUS);
		// Draw after every other transparent surface (water, glass); the
		// order among the layers is set per frame in apply_draw_order.
		mat->set_render_priority(100);
		mi->set_material_override(mat);
		mi->set_extra_cull_margin(16384.0);
		mi->set_custom_aabb(AABB(Vector3(-SHEET_HALF_M, -1000.0, -SHEET_HALF_M), Vector3(2.0 * SHEET_HALF_M, 25000.0, 2.0 * SHEET_HALF_M)));
		mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mi);
		meshes.push_back(mi);
		// Wind: meteorological "from" direction -> drift vector in the local
		// frame (x = west, z = north): blowing toward (dir + 180).
		const double dir = Math::deg_to_rad((double)l.get("wind_dir_deg", 0.0));
		const double spd = (double)l.get("wind_speed_kts", 0.0) * 0.514444;
		wind.push_back(Vector2(spd * std::sin(dir), -spd * std::cos(dir)));
		offsets.push_back(Vector2());
		mids.push_back((base + top) * 0.5);
	}
	apply_quality();
}

// Painter's order between the layers: the layer farthest from the camera
// altitude draws first, the nearest last.
void CloudLayers::apply_draw_order(double cam_alt)
{
	std::vector<int> order(meshes.size());
	for (int i = 0; i < meshes.size(); i++)
	{
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
		return std::abs(cam_alt - mids[a]) > std::abs(cam_alt - mids[b]);
	});
	for (int rank = 0; rank < (int)order.size(); rank++)
	{
		Ref<ShaderMaterial> mat = meshes[order[rank]]->get_material_override();
		const int pri = 100 + rank;
		if (mat.is_valid() && mat->get_render_priority() != pri)
		{
			mat->set_render_priority(pri);
		}
	}
}

void CloudLayers::apply_quality()
{
	set_visible(quality > 0);
	for (int i = 0; i < meshes.size(); i++)
	{
		Ref<ShaderMaterial> mat = meshes[i]->get_material_override();
		if (mat.is_valid() && mat->get_shader() == volume_shader)
		{
			mat->set_shader_parameter("steps", STEPS_BY_QUALITY[Math::max(quality, 1)]);
		}
	}
}

void CloudLayers::update(Camera3D *cam, const Vector3 &sun_dir_world, const Color &sun_color, double sun_energy,
		const Color &ambient_color, double ambient_energy, double fog_density, const Color &fog_color, double delta)
{
	if (quality <= 0 || meshes.is_empty() || cam == nullptr)
	{
		return;
	}
	const Vector3 p = cam->get_global_position();
	if (!p.is_finite() || p.length() < 1.0e6)
	{
		return;
	}
	const geo::Geodetic g = geo::geodetic_of(p);
	set_global_transform(Transform3D(geo::wun_basis(g.lat, g.lon), geo::ecef_of(g.lat, g.lon, 0.0)));
	const Transform3D gt = get_global_transform();
	const Vector3 sun_local = gt.basis.inverse().xform(sun_dir_world);
	const Vector3 cam_local = gt.affine_inverse().xform(p);
	// World-fixed noise anchor: LV95 easting/northing of the frame origin,
	// mapped onto the frame's axes (x = west, z = north).
	const Vector2 lv = Geo::wgs84_to_lv95(g.lat, g.lon);
	const Vector2 world_offset(-lv.x, lv.y);
	apply_draw_order(cam_local.y);
	for (int i = 0; i < meshes.size(); i++)
	{
		offsets.write[i] += wind[i] * delta;
		Ref<ShaderMaterial> mat = meshes[i]->get_material_override();
		if (mat.is_null())
		{
			continue;
		}
		mat->set_shader_parameter("cam_local", cam_local);
		mat->set_shader_parameter("world_offset", world_offset);
		mat->set_shader_parameter("sun_dir", sun_local.normalized());
		mat->set_shader_parameter("sun_color", sun_color);
		mat->set_shader_parameter("sun_energy", sun_energy);
		mat->set_shader_parameter("ambient_color", ambient_color);
		mat->set_shader_parameter("ambient_energy", ambient_energy);
		mat->set_shader_parameter("fog_density", fog_density);
		mat->set_shader_parameter("fog_color", fog_color);
		mat->set_shader_parameter("wind_offset", offsets[i]);
	}
}

} // namespace jsbgodot
