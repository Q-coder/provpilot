#include "exhaust_effects.h"
#include "jsbgodot.h"
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace jsbgodot
{

// Contrail: puffs are made every EMIT_SPACING_M of flight path. A puff's
// diameter grows with the square root of its age; as the puffs grow, every
// second one fades out (by the trailing zero bits of its serial number), so
// the trail keeps about the same overlap from the nozzle to its far end.
static const double EMIT_SPACING_M = 1.0;
static const double PUFF_OVERLAP = 0.3;     // spacing kept at this share of the diameter
static const double TRAIL_LIFE_S = 55.0;
static const int MAX_PUFFS = 12000;
// outside air temperature (degC): no trail above, full trail below
static const double CONTRAIL_T_NONE = -36.0, CONTRAIL_T_FULL = -42.0;

static double puff_diameter(double age, double nozzle_radius)
{
	return 2.0 * nozzle_radius + 0.6 + 2.3 * std::sqrt(age);
}

static const char *HAZE_SHADER = R"(
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, shadows_disabled, fog_disabled;
// Heat shimmer: the picture behind the plume, bent by scrolling noise.
// COLOR.r = 1: the plume's cone (fades to its silhouette); 0: cross discs
// (seen along the jet axis, where the cone is edge-on). UV.y runs along the
// plume, UV.x round the cone / out from the axis on the discs.
uniform sampler2D screen_tex : hint_screen_texture, repeat_disable, filter_linear;
instance uniform float power = 1.0;
instance uniform float amplitude_m = 0.2;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
void fragment()
{
	float along = UV.y;
	float scroll = TIME * (3.0 + 5.0 * power);
	vec2 q;
	float shape;
	float facing = abs(dot(normalize(NORMAL), normalize(VIEW)));
	if (COLOR.r > 0.5)
	{
		float a = UV.x * 6.2831853;
		q = vec2(cos(a) * 1.6 + along * 7.0 - scroll, sin(a) * 1.6 + along * 3.0);
		shape = pow(facing, 1.6);
	}
	else
	{
		q = vec2(UV.x * 3.0 - scroll * 0.6, along * 9.0 + UV.x);
		shape = (1.0 - UV.x * UV.x) * facing * facing;
	}
	vec2 off = vec2(vnoise(q) + 0.5 * vnoise(q * 2.3 + 17.0), vnoise(q + 41.0) + 0.5 * vnoise(q * 2.3 + 73.0)) / 1.5 - 0.5;
	float fade = smoothstep(0.0, 0.05, along) * pow(1.0 - along, 1.4) * shape * sqrt(clamp(power, 0.0, 1.0));
	// amplitude_m of sideways shift at the plume's distance, as a share of the screen
	float dist = max(length(VERTEX), 1.0);
	vec2 to_uv = vec2(PROJECTION_MATRIX[0][0], PROJECTION_MATRIX[1][1]) * 0.5 / dist;
	vec2 uv = SCREEN_UV + off * 2.0 * amplitude_m * to_uv * fade;
	ALBEDO = textureLod(screen_tex, uv, 0.0).rgb;
	ALPHA = clamp(fade * 4.0, 0.0, 1.0);
}
)";

static const char *TRAIL_SHADER = R"(
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_disabled, shadows_disabled, specular_disabled;
// Contrail puffs: soft, ragged discs facing the camera. Lit by the sun and
// the sky without a facing term (a cloud of ice crystals has no surface).
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
void fragment()
{
	vec2 p = UV * 2.0 - 1.0;
	float seed = COLOR.r * 97.0;
	float r = length(p) + (vnoise(p * 2.2 + seed) - 0.5) * 0.45;
	ALBEDO = vec3(1.0);
	ROUGHNESS = 1.0;
	ALPHA = COLOR.a * smoothstep(1.0, 0.25, r);
}
void light()
{
	DIFFUSE_LIGHT += ATTENUATION * LIGHT_COLOR * 0.3;
}
)";

void ExhaustEffects::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("setup", "ac_node", "jsb"), &ExhaustEffects::setup);
	ClassDB::bind_method(D_METHOD("clear"), &ExhaustEffects::clear);
	ClassDB::bind_method(D_METHOD("set_force_contrail", "on"), &ExhaustEffects::set_force_contrail);
	ClassDB::bind_method(D_METHOD("get_force_contrail"), &ExhaustEffects::get_force_contrail);
	ClassDB::bind_method(D_METHOD("set_enabled", "on"), &ExhaustEffects::set_enabled);
	ClassDB::bind_method(D_METHOD("get_enabled"), &ExhaustEffects::get_enabled);
	ClassDB::bind_method(D_METHOD("get_nozzle_count"), &ExhaustEffects::get_nozzle_count);
	ClassDB::bind_method(D_METHOD("get_puff_count"), &ExhaustEffects::get_puff_count);
	ClassDB::bind_method(D_METHOD("get_contrail_factor"), &ExhaustEffects::get_contrail_factor);
	ClassDB::bind_method(D_METHOD("get_heat_power", "nozzle"), &ExhaustEffects::get_heat_power);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "force_contrail"), "set_force_contrail", "get_force_contrail");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
}

void ExhaustEffects::set_force_contrail(bool on) { force_contrail = on; }
bool ExhaustEffects::get_force_contrail() const { return force_contrail; }
void ExhaustEffects::set_enabled(bool on) { enabled = on; }
bool ExhaustEffects::get_enabled() const { return enabled; }
int ExhaustEffects::get_nozzle_count() const { return (int)nozzles.size(); }
double ExhaustEffects::get_contrail_factor() const { return contrail_factor; }
double ExhaustEffects::get_heat_power(int nozzle) const
{
	return nozzle >= 0 && nozzle < (int)nozzles.size() ? nozzles[nozzle].power : 0.0;
}
int ExhaustEffects::get_puff_count() const
{
	int c = 0;
	for (const Nozzle &n : nozzles)
	{
		c += (int)n.trail.size();
	}
	return c;
}

void ExhaustEffects::setup(Node3D *p_ac_node, Object *p_jsb)
{
	ac = p_ac_node;
	jsb_obj = p_jsb;
}

void ExhaustEffects::_ready()
{
	set_as_top_level(true);
	set_global_transform(Transform3D());
	set_process_priority(1000); // after the FDM has placed the aircraft
	set_process(!Engine::get_singleton()->is_editor_hint());
}

void ExhaustEffects::ensure_materials()
{
	if (haze_material.is_null())
	{
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(HAZE_SHADER);
		haze_material.instantiate();
		haze_material->set_shader(sh);
		haze_material->set_render_priority(10); // over the contrail and the clouds
	}
	if (trail_material.is_null())
	{
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(TRAIL_SHADER);
		trail_material.instantiate();
		trail_material->set_shader(sh);
	}
}

void ExhaustEffects::clear()
{
	for (Nozzle &n : nozzles)
	{
		if (n.haze != nullptr)
		{
			n.haze->queue_free();
		}
	}
	nozzles.clear();
	if (trail_node != nullptr)
	{
		trail_node->set_visible(false);
	}
}

// Plume mesh in nozzle space (x right, y up, z forward): a cone opening
// aft from the nozzle, and discs across it for the view along the jet.
Ref<ArrayMesh> ExhaustEffects::build_haze_mesh(double radius, double length) const
{
	PackedVector3Array verts, normals;
	PackedVector2Array uvs;
	PackedColorArray colors;
	PackedInt32Array idx;
	const int SEG = 20, RINGS = 10;
	for (int r = 0; r <= RINGS; r++)
	{
		const double t = (double)r / RINGS;
		const double rad = radius * (1.0 + 1.8 * t);
		for (int s = 0; s <= SEG; s++)
		{
			const double a = Math_TAU * s / SEG;
			verts.push_back(Vector3(std::cos(a) * rad, std::sin(a) * rad, -t * length));
			normals.push_back(Vector3(std::cos(a), std::sin(a), 0.0));
			uvs.push_back(Vector2((double)s / SEG, t));
			colors.push_back(Color(1, 1, 1, 1));
		}
	}
	for (int r = 0; r < RINGS; r++)
	{
		for (int s = 0; s < SEG; s++)
		{
			const int a = r * (SEG + 1) + s, b = a + SEG + 1;
			idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
			idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
		}
	}
	const int DISCS = 6;
	for (int d = 0; d < DISCS; d++)
	{
		const double t = (d + 0.5) / DISCS;
		const double rad = radius * (1.0 + 1.8 * t) * 1.1;
		const int centre = verts.size();
		verts.push_back(Vector3(0, 0, -t * length));
		normals.push_back(Vector3(0, 0, 1));
		uvs.push_back(Vector2(0.0, t));
		colors.push_back(Color(0, 1, 1, 1));
		for (int s = 0; s <= SEG; s++)
		{
			const double a = Math_TAU * s / SEG;
			verts.push_back(Vector3(std::cos(a) * rad, std::sin(a) * rad, -t * length));
			normals.push_back(Vector3(0, 0, 1));
			uvs.push_back(Vector2(1.0, t));
			colors.push_back(Color(0, 1, 1, 1));
		}
		for (int s = 0; s < SEG; s++)
		{
			idx.push_back(centre); idx.push_back(centre + 1 + s); idx.push_back(centre + 2 + s);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

void ExhaustEffects::configure(const Ref<AircraftConfig> &cfg)
{
	clear();
	if (cfg.is_null())
	{
		return;
	}
	const String spec = cfg->get_string("exhaust", "nozzles", "");
	if (spec.strip_edges().is_empty())
	{
		return;
	}
	ensure_materials();
	heat = cfg->get_bool("exhaust", "heat", true);
	contrail = cfg->get_bool("exhaust", "contrail", true);
	const double radius = cfg->get_float("exhaust", "radius_m", 0.4);
	const double length = cfg->get_float("exhaust", "heat_length_m", radius * 30.0);
	const Array engines = cfg->get_string_array("exhaust", "engines", Array());
	const PackedStringArray parts = spec.split(";", false);
	for (int i = 0; i < parts.size(); i++)
	{
		const PackedStringArray xyz = parts[i].split(",", false);
		if (xyz.size() < 3)
		{
			UtilityFunctions::print("WARNING: [exhaust] nozzle needs 'right, up, forward': ", parts[i]);
			continue;
		}
		Nozzle n;
		n.pos_m = Vector3(xyz[0].strip_edges().to_float(), xyz[1].strip_edges().to_float(), xyz[2].strip_edges().to_float());
		n.radius = radius;
		n.engine = i < engines.size() ? String(engines[i]).strip_edges().to_int() : i;
		if (heat)
		{
			n.haze = memnew(MeshInstance3D);
			n.haze->set_name(String("ExhaustHeat") + String::num_int64(i));
			n.haze->set_mesh(build_haze_mesh(radius, length));
			n.haze->set_material_override(haze_material);
			n.haze->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			n.haze->set_instance_shader_parameter("amplitude_m", Math::clamp(radius * 0.45, 0.04, 0.3));
			n.haze->set_instance_shader_parameter("power", 0.0);
			n.haze->set_visible(false);
			add_child(n.haze);
		}
		nozzles.push_back(n);
	}
	if (contrail && trail_node == nullptr)
	{
		trail_node = memnew(MeshInstance3D);
		trail_node->set_name("Contrail");
		trail_mesh.instantiate();
		trail_node->set_mesh(trail_mesh);
		trail_node->set_material_override(trail_material);
		trail_node->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		trail_node->set_visible(false);
		add_child(trail_node);
	}
	UtilityFunctions::print("Exhaust effects: ", (int)nozzles.size(), " nozzles (heat ", heat, ", contrail ", contrail, ")");
}

Vector3 ExhaustEffects::nozzle_world(const Nozzle &n) const
{
	// aircraft node axes: X = left, Y = up, Z = nose (the node may be scaled)
	const Transform3D ax = ac->get_global_transform();
	return ax.origin - ax.basis.get_column(0).normalized() * n.pos_m.x + ax.basis.get_column(1).normalized() * n.pos_m.y + ax.basis.get_column(2).normalized() * n.pos_m.z;
}

// 0 (engine off) .. 1 (full power), from N1 for a turbine, else from the throttle.
double ExhaustEffects::engine_power(JSBGodot *fdm, int engine) const
{
	char buf[96];
	snprintf(buf, sizeof(buf), "propulsion/engine[%d]/set-running", engine);
	if (fdm->has_property(buf) && fdm->get_property(buf) < 0.5)
	{
		return 0.0;
	}
	snprintf(buf, sizeof(buf), "propulsion/engine[%d]/n1", engine);
	if (fdm->has_property(buf))
	{
		const double n1 = fdm->get_property(buf);
		return n1 < 8.0 ? 0.0 : Math::clamp(0.3 + 0.7 * (n1 - 25.0) / 70.0, 0.15, 1.0);
	}
	snprintf(buf, sizeof(buf), "fcs/throttle-pos-norm[%d]", engine);
	const double thr = fdm->has_property(buf) ? (double)fdm->get_property(buf) : 0.5;
	return fdm->get_engine_running() ? 0.3 + 0.7 * Math::clamp(thr, 0.0, 1.0) : 0.0;
}

void ExhaustEffects::emit_puffs(Nozzle &n, const Vector3 &now_pos, double density)
{
	if (!n.last_valid || density <= 0.01)
	{
		n.last_pos = now_pos;
		n.last_valid = density > 0.01;
		n.carry_m = 0.0;
		return;
	}
	const double dist = n.last_pos.distance_to(now_pos);
	if (dist > 800.0)
	{
		// teleport / slew jump: no trail across it
		n.last_pos = now_pos;
		n.carry_m = 0.0;
		return;
	}
	double at = EMIT_SPACING_M - n.carry_m;
	while (at <= dist)
	{
		Puff p;
		const double f = at / dist;
		p.pos = n.last_pos.lerp(now_pos, f);
		// a puff made earlier in the frame is that much older
		p.born = clock - (1.0 - f) * get_process_delta_time();
		p.n = ++n.counter;
		p.density = (float)density;
		p.seed = (float)((p.n * 2654435761u) & 0xffff) / 65535.0f;
		n.trail.push_back(p);
		at += EMIT_SPACING_M;
	}
	n.carry_m = dist - (at - EMIT_SPACING_M);
	n.last_pos = now_pos;
}

void ExhaustEffects::rebuild_trail()
{
	if (trail_node == nullptr)
	{
		return;
	}
	Camera3D *cam = get_viewport() != nullptr ? get_viewport()->get_camera_3d() : nullptr;
	int total = 0;
	for (Nozzle &n : nozzles)
	{
		while (!n.trail.empty() && (clock - n.trail.front().born > TRAIL_LIFE_S || (int)n.trail.size() > MAX_PUFFS))
		{
			n.trail.pop_front();
		}
		total += (int)n.trail.size();
	}
	if (total == 0 || cam == nullptr || ac == nullptr)
	{
		trail_node->set_visible(false);
		return;
	}
	const Vector3 anchor = ac->get_global_transform().origin;
	const Basis cb = cam->get_global_transform().basis.orthonormalized();
	const Vector3 cx = cb.get_column(0), cy = cb.get_column(1), cz = cb.get_column(2);
	PackedVector3Array verts, normals;
	PackedVector2Array uvs;
	PackedColorArray colors;
	PackedInt32Array idx;
	verts.resize(total * 4);
	normals.resize(total * 4);
	uvs.resize(total * 4);
	colors.resize(total * 4);
	idx.resize(total * 6);
	Vector3 *vp = verts.ptrw(), *np = normals.ptrw();
	Vector2 *up = uvs.ptrw();
	Color *cp = colors.ptrw();
	int32_t *ip = idx.ptrw();
	int q = 0;
	for (Nozzle &n : nozzles)
	{
		for (auto it = n.trail.begin(); it != n.trail.end();)
		{
			const Puff &p = *it;
			const double age = clock - p.born;
			const double size = puff_diameter(age, n.radius);
			// thinning: a puff with serial number n = odd * 2^level lives until the
			// spacing its level provides is twice what the diameter asks for
			const double need = Math::max(1.0, PUFF_OVERLAP * size / EMIT_SPACING_M);
			int level = 0;
			for (uint32_t v = p.n; (v & 1u) == 0u && level < 8; v >>= 1)
			{
				level++;
			}
			const double keep = Math::clamp(2.0 - need / (double)(1 << level), 0.0, 1.0);
			if (keep <= 0.0)
			{
				it = n.trail.erase(it);
				continue;
			}
			// forms a little behind the nozzle, thins out at the end of its life
			const double form = Math::smoothstep(0.06, 0.35, age);
			const double die = 1.0 - Math::smoothstep(TRAIL_LIFE_S * 0.5, TRAIL_LIFE_S, age);
			const double alpha = 0.36 * form * die * keep * p.density;
			const Vector3 c = p.pos - anchor;
			const double ang = p.seed * Math_TAU;
			const double h = size * 0.5 * 1.25; // the disc's soft edge lies inside the quad
			const Vector3 rx = (cx * std::cos(ang) + cy * std::sin(ang)) * h;
			const Vector3 ry = (cy * std::cos(ang) - cx * std::sin(ang)) * h;
			const int v0 = q * 4;
			vp[v0] = c - rx - ry; vp[v0 + 1] = c + rx - ry; vp[v0 + 2] = c + rx + ry; vp[v0 + 3] = c - rx + ry;
			up[v0] = Vector2(0, 0); up[v0 + 1] = Vector2(1, 0); up[v0 + 2] = Vector2(1, 1); up[v0 + 3] = Vector2(0, 1);
			for (int k = 0; k < 4; k++)
			{
				np[v0 + k] = cz;
				cp[v0 + k] = Color(p.seed, 0, 0, (float)alpha);
			}
			const int i0 = q * 6;
			ip[i0] = v0; ip[i0 + 1] = v0 + 1; ip[i0 + 2] = v0 + 2; ip[i0 + 3] = v0; ip[i0 + 4] = v0 + 2; ip[i0 + 5] = v0 + 3;
			q++;
			++it;
		}
	}
	trail_mesh->clear_surfaces();
	if (q == 0)
	{
		trail_node->set_visible(false);
		return;
	}
	verts.resize(q * 4);
	normals.resize(q * 4);
	uvs.resize(q * 4);
	colors.resize(q * 4);
	idx.resize(q * 6);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = idx;
	trail_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	trail_node->set_global_transform(Transform3D(Basis(), anchor));
	trail_node->set_visible(true);
}

void ExhaustEffects::_process(double delta)
{
	if (ac == nullptr || nozzles.empty())
	{
		return;
	}
	JSBGodot *fdm = Object::cast_to<JSBGodot>(jsb_obj);
	if (fdm == nullptr || !fdm->is_initialized())
	{
		return;
	}
	clock += delta;
	// contrails form in air colder than about -40 degC
	double cold = 0.0;
	if (fdm->has_property("atmosphere/T-R"))
	{
		const double t_c = (double)fdm->get_property("atmosphere/T-R") * 5.0 / 9.0 - 273.15;
		cold = Math::clamp((CONTRAIL_T_NONE - t_c) / (CONTRAIL_T_NONE - CONTRAIL_T_FULL), 0.0, 1.0);
	}
	contrail_factor = force_contrail ? 1.0 : cold;
	const Basis ab = ac->get_global_transform().basis.orthonormalized();
	for (Nozzle &n : nozzles)
	{
		n.power = enabled ? engine_power(fdm, n.engine) : 0.0;
		const Vector3 pos = nozzle_world(n);
		if (n.haze != nullptr)
		{
			const bool on = heat && n.power > 0.01;
			n.haze->set_visible(on);
			if (on)
			{
				n.haze->set_global_transform(Transform3D(ab, pos)); // the plume is symmetric: the aircraft's axes will do
				n.haze->set_instance_shader_parameter("power", n.power);
			}
		}
		if (contrail)
		{
			emit_puffs(n, pos, enabled ? contrail_factor * Math::clamp(n.power * 1.6, 0.0, 1.0) : 0.0);
		}
	}
	if (contrail)
	{
		rebuild_trail();
	}
}

} // namespace jsbgodot
