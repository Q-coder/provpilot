#include "aircraft_lights.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <limits>

using namespace godot;

namespace jsbgodot
{

static const char *const KINDS[] = {"nav_left", "nav_right", "nav_tail", "strobe", "beacon", "landing", "taxi", "flood"};

void AircraftLights::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("setup", "model_root", "cfg", "ac"), &AircraftLights::setup);
	ClassDB::bind_method(D_METHOD("clear"), &AircraftLights::clear);
	ClassDB::bind_method(D_METHOD("set_landing_lights", "on"), &AircraftLights::set_landing_lights);
	ClassDB::bind_method(D_METHOD("set_fixture_level", "kind", "index", "level"), &AircraftLights::set_fixture_level);
	ClassDB::bind_method(D_METHOD("get_fixture_level", "kind", "index"), &AircraftLights::get_fixture_level);
	ClassDB::bind_method(D_METHOD("set_fixture_extension", "kind", "index", "ext"), &AircraftLights::set_fixture_extension);
	ClassDB::bind_method(D_METHOD("fixture_count_of", "kind"), &AircraftLights::fixture_count_of);
	ClassDB::bind_method(D_METHOD("landing_lights_on"), &AircraftLights::landing_lights_on);
	ClassDB::bind_method(D_METHOD("toggle_landing_lights"), &AircraftLights::toggle_landing_lights);
	ClassDB::bind_method(D_METHOD("fixture_count", "kind"), &AircraftLights::fixture_count, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("is_lit", "kind"), &AircraftLights::is_lit);
	ClassDB::bind_method(D_METHOD("get_fixtures"), &AircraftLights::get_fixtures);
	ClassDB::bind_method(D_METHOD("update", "delta", "airborne", "gear_down", "night", "forward", "down"), &AircraftLights::update);
#define AL_PROP(name, getter, setter)                                                          \
	ClassDB::bind_method(D_METHOD(#getter), &AircraftLights::getter);                        \
	ClassDB::bind_method(D_METHOD(#setter, "value"), &AircraftLights::setter);               \
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, #name), #setter, #getter);
	AL_PROP(nav_enabled, get_nav_enabled, set_nav_enabled)
	AL_PROP(strobe_enabled, get_strobe_enabled, set_strobe_enabled)
	AL_PROP(beacon_enabled, get_beacon_enabled, set_beacon_enabled)
	AL_PROP(taxi_enabled, get_taxi_enabled, set_taxi_enabled)
	AL_PROP(powered, get_powered, set_powered)
	AL_PROP(debug_all_on, get_debug_all_on, set_debug_all_on)
#undef AL_PROP
}

AircraftLights::AircraftLights()
{
}

bool AircraftLights::get_nav_enabled() const { return nav_enabled; }
void AircraftLights::set_nav_enabled(bool v) { nav_enabled = v; }
bool AircraftLights::get_strobe_enabled() const { return strobe_enabled; }
void AircraftLights::set_strobe_enabled(bool v) { strobe_enabled = v; }
bool AircraftLights::get_beacon_enabled() const { return beacon_enabled; }
void AircraftLights::set_beacon_enabled(bool v) { beacon_enabled = v; }
bool AircraftLights::get_taxi_enabled() const { return taxi_enabled; }
void AircraftLights::set_taxi_enabled(bool v) { taxi_enabled = v; }
bool AircraftLights::get_powered() const { return powered; }
void AircraftLights::set_powered(bool v) { powered = v; }
bool AircraftLights::get_debug_all_on() const { return debug_all_on; }
void AircraftLights::set_debug_all_on(bool v) { debug_all_on = v; }

Color AircraftLights::colour_of(const String &kind)
{
	if (kind == "nav_left") return Color(1.0, 0.1, 0.05);
	if (kind == "nav_right") return Color(0.1, 1.0, 0.3);
	if (kind == "nav_tail") return Color(1.0, 0.97, 0.9);
	if (kind == "strobe") return Color(0.95, 0.97, 1.0);
	if (kind == "beacon") return Color(1.0, 0.15, 0.05);
	if (kind == "landing") return Color(1.0, 0.97, 0.9);
	return Color(1.0, 0.95, 0.85); // taxi
}

double AircraftLights::size_of(const String &kind)
{
	if (kind == "strobe") return 1.6;
	if (kind == "beacon") return 1.2;
	if (kind == "landing") return 2.2;
	if (kind == "taxi") return 1.6;
	return 1.0;
}

Node3D *AircraftLights::off_sibling(Node3D *anchor)
{
	const String name = anchor->get_name();
	if (!name.ends_with("-on") || anchor->get_parent() == nullptr)
	{
		return nullptr;
	}
	return Object::cast_to<Node3D>(anchor->get_parent()->get_node_or_null(NodePath(name.left(name.length() - 3) + "-off")));
}

void AircraftLights::setup(Node *model_root, const Ref<AircraftConfig> &cfg, Node3D *p_ac)
{
	clear();
	ac = p_ac;
	if (model_root == nullptr || cfg.is_null())
	{
		return;
	}
	if (shader.is_null())
	{
		shader = ResourceLoader::get_singleton()->load("res://shaders/aircraft_light.gdshader");
	}
	landing_pitch_deg = cfg->get_float("lights", "landing_pitch_deg", 5.0);
	landing_range_m = cfg->get_float("lights", "landing_range_m", 300.0);
	taxi_range_m = cfg->get_float("lights", "taxi_range_m", 90.0);
	spot_energy = cfg->get_float("lights", "spot_energy", 12.0);
	flood_range_m = cfg->get_float("lights", "flood_range_m", 2.0);
	flood_energy = cfg->get_float("lights", "flood_energy", 1.2);
	// "pair_meshes false": leave "<x>-on"/"<x>-off" mesh visibility to another
	// system (the FlightGear select animations on the A-10)
	pair_meshes = cfg->get_bool("lights", "pair_meshes", true);
	// "landing_ext_props": one entry per landing fixture ("-" = a fixed lamp);
	// a named one is a retractable lamp that faces down when stowed
	ext_prop_names = cfg->get_string_array("lights", "landing_ext_props", Array());
	for (const char *k : KINDS)
	{
		const String kind(k);
		const Array names = cfg->get_string_array("lights", kind, Array());
		for (int i = 0; i < names.size(); i++)
		{
			const String node_name = String(names[i]).strip_edges();
			if (node_name.is_empty())
			{
				continue;
			}
			Node3D *anchor = Object::cast_to<Node3D>(model_root->find_child(node_name, true, false));
			if (anchor == nullptr)
			{
				UtilityFunctions::push_warning("AircraftLights: node '", node_name, "' for ", kind, " not found in the model");
				continue;
			}
			// "<kind>_anchor": an animated part the lens rides on (see ride_on)
			const String ride_name = cfg->get_string("lights", kind + String("_anchor"), "");
			if (!ride_name.is_empty())
			{
				Node3D *ride = Object::cast_to<Node3D>(model_root->find_child(ride_name, true, false));
				if (ride != nullptr)
				{
					// the unlit twin (looked up before the lens moves) rides along
					Node3D *sib = off_sibling(anchor);
					ride_on(anchor, ride, model_root);
					if (sib != nullptr && sib->get_parent() != ride)
					{
						ride_on(sib, ride, model_root);
					}
				}
				else
				{
					UtilityFunctions::push_warning("AircraftLights: anchor '", ride_name, "' for ", kind, " not found in the model");
				}
			}
			add_fixture(kind, anchor);
		}
		// "<kind>_pos": "x, y, z[, x, y, z ...]" in model-root units, one fixture
		// per triple, for models without a lens mesh (the MD-80's lights were
		// FlightGear effects the build leaves out)
		const Array pos = cfg->get_float_array("lights", kind + String("_pos"), Array());
		Node3D *root3d = Object::cast_to<Node3D>(model_root);
		for (int i = 0; root3d != nullptr && i + 2 < pos.size(); i += 3)
		{
			Node3D *n = memnew(Node3D);
			n->set_name(vformat("Light_%s_%d", kind, i / 3));
			root3d->add_child(n);
			n->set_position(Vector3((double)pos[i], (double)pos[i + 1], (double)pos[i + 2]));
			add_fixture(kind, n);
		}
	}
	apply_states();
}

// Reparent a lens mesh under the animated part it belongs to. The A-10 glTF
// animates the gear per mesh and its lens meshes have no tracks, so they stay
// in the file pose (gear up, in the bay) while the strut swings down. The
// clip pose in which the lens lies on the part is the one it was modelled
// in: sample the part's tracks, take the pose with the lens closest to the
// part's bounds, and keep that offset as the lens's new local transform.
void AircraftLights::ride_on(Node3D *lens, Node3D *part, Node *model_root)
{
	const Transform3D lens_g = lens->get_global_transform();
	Transform3D best = part->get_global_transform().affine_inverse() * lens_g;
	AnimationPlayer *ap = nullptr;
	const TypedArray<Node> players = model_root->find_children("*", "AnimationPlayer", true, false);
	if (players.size() > 0)
	{
		ap = Object::cast_to<AnimationPlayer>(players[0]);
	}
	Ref<Animation> anim;
	if (ap != nullptr)
	{
		String clip = ap->get_current_animation();
		if (clip.is_empty())
		{
			clip = ap->get_assigned_animation();
		}
		if (clip.is_empty() && ap->get_animation_list().size() > 0)
		{
			clip = ap->get_animation_list()[0];
		}
		if (!clip.is_empty())
		{
			anim = ap->get_animation(clip);
		}
	}
	if (anim.is_valid())
	{
		int pos_t = -1, rot_t = -1, scl_t = -1;
		for (int i = 0; i < anim->get_track_count(); i++)
		{
			const NodePath path = anim->track_get_path(i);
			if (path.get_name_count() == 0 || path.get_name(path.get_name_count() - 1) != part->get_name())
			{
				continue;
			}
			switch (anim->track_get_type(i))
			{
				case Animation::TYPE_POSITION_3D: pos_t = i; break;
				case Animation::TYPE_ROTATION_3D: rot_t = i; break;
				case Animation::TYPE_SCALE_3D: scl_t = i; break;
				default: break;
			}
		}
		if (pos_t >= 0 || rot_t >= 0)
		{
			Node3D *parent3d = Object::cast_to<Node3D>(part->get_parent());
			const Transform3D parent_g = parent3d != nullptr ? parent3d->get_global_transform() : Transform3D();
			MeshInstance3D *pm = Object::cast_to<MeshInstance3D>(part);
			const AABB bounds = (pm != nullptr && pm->get_mesh().is_valid()) ? pm->get_aabb() : AABB();
			double best_d = std::numeric_limits<double>::infinity();
			for (int k = 0; k <= 20; k++)
			{
				const double t = anim->get_length() * (double)k / 20.0;
				const Vector3 p = pos_t >= 0 ? anim->position_track_interpolate(pos_t, t) : part->get_position();
				const Quaternion q = rot_t >= 0 ? anim->rotation_track_interpolate(rot_t, t) : part->get_quaternion();
				const Vector3 sc = scl_t >= 0 ? anim->scale_track_interpolate(scl_t, t) : part->get_scale();
				const Transform3D local(Basis(q) * Basis::from_scale(sc), p);
				const Transform3D offset = (parent_g * local).affine_inverse() * lens_g;
				const double d = aabb_distance(bounds, offset.origin);
				if (d < best_d)
				{
					best_d = d;
					best = offset;
				}
			}
		}
	}
	lens->get_parent()->remove_child(lens);
	part->add_child(lens);
	lens->set_transform(best);
}

double AircraftLights::aabb_distance(const AABB &box, const Vector3 &p)
{
	const Vector3 lo = box.position;
	const Vector3 hi = box.get_end();
	const Vector3 c(Math::clamp((double)p.x, (double)lo.x, (double)hi.x), Math::clamp((double)p.y, (double)lo.y, (double)hi.y),
			Math::clamp((double)p.z, (double)lo.z, (double)hi.z));
	return c.distance_to(p);
}

void AircraftLights::clear()
{
	for (int i = 0; i < fixtures.size(); i++)
	{
		Fixture &f = fixtures.write[i];
		if (f.anchor_id != 0 && UtilityFunctions::is_instance_id_valid(f.anchor_id) && f.sprite != nullptr)
		{
			f.sprite->queue_free();
		}
		if (f.spot != nullptr)
		{
			f.spot->queue_free();
		}
	}
	fixtures.clear();
}

void AircraftLights::set_fixture_level(const String &kind, int index, double level)
{
	int k = 0;
	for (int i = 0; i < fixtures.size(); i++)
	{
		if (fixtures[i].kind == kind && k++ == index)
		{
			fixtures.write[i].level = level;
			return;
		}
	}
}

void AircraftLights::set_fixture_extension(const String &kind, int index, double ext)
{
	int k = 0;
	for (int i = 0; i < fixtures.size(); i++)
	{
		if (fixtures[i].kind == kind && k++ == index)
		{
			fixtures.write[i].ext = Math::clamp(ext, 0.0, 1.0);
			return;
		}
	}
}

double AircraftLights::get_fixture_level(const String &kind, int index) const
{
	int k = 0;
	for (int i = 0; i < fixtures.size(); i++)
	{
		if (fixtures[i].kind == kind && k++ == index)
		{
			return fixtures[i].level;
		}
	}
	return -1.0;
}

int AircraftLights::fixture_count_of(const String &kind) const
{
	int k = 0;
	for (int i = 0; i < fixtures.size(); i++)
	{
		k += fixtures[i].kind == kind ? 1 : 0;
	}
	return k;
}

void AircraftLights::add_fixture(const String &kind, Node3D *anchor)
{
	if (kind == "flood")
	{
		// a cockpit flood: an omni light at the anchor, its level from the
		// aircraft's flood knob (set_fixture_level); no lens sprite
		Fixture f;
		f.kind = kind;
		f.anchor = anchor;
		f.anchor_id = anchor->get_instance_id();
		OmniLight3D *omni = memnew(OmniLight3D);
		omni->set_name("Flood");
		omni->set_color(Color(1.0, 0.93, 0.8));
		omni->set_param(Light3D::PARAM_ENERGY, 0.0);
		omni->set_param(Light3D::PARAM_RANGE, flood_range_m);
		omni->set_param(Light3D::PARAM_ATTENUATION, 1.5);
		omni->set_shadow(false);
		omni->set_visible(false);
		anchor->add_child(omni);
		f.omni = omni;
		f.energy = flood_energy;
		fixtures.push_back(f);
		return;
	}
	bool retractable = false;
	if (kind == "landing")
	{
		const int idx = fixture_count_of(kind);
		const String en = idx < ext_prop_names.size() ? String(ext_prop_names[idx]).strip_edges() : String();
		retractable = !en.is_empty() && en != "-";
	}
	Vector3 centre;
	MeshInstance3D *am = Object::cast_to<MeshInstance3D>(anchor);
	if (am != nullptr && am->get_mesh().is_valid())
	{
		centre = am->get_aabb().get_center();
	}
	if (ac != nullptr && anchor->is_inside_tree())
	{
		// outward direction in the aircraft frame -> anchor-local offset
		const Transform3D ac_inv = ac->get_global_transform().affine_inverse();
		const Vector3 p_ac = ac_inv.xform(anchor->get_global_transform().xform(centre));
		Vector3 out;
		if (kind == "nav_left") out = Vector3(1, 0, 0);
		else if (kind == "nav_right") out = Vector3(-1, 0, 0);
		else if (kind == "nav_tail") out = Vector3(0, 0, -1);
		else if (kind == "beacon") out = Vector3(0, 1, 0);
		else if (retractable) out = Vector3(0, -1, 0); // stowed at setup: the lens faces down and swings forward with the lamp
		else if (kind == "landing" || kind == "taxi") out = Vector3(0, 0, 1);
		else if (kind == "strobe") out = std::abs(p_ac.x) > 1.0 ? Vector3(Math::sign((double)p_ac.x), 0, 0) : Vector3(0, 0, -1);
		const Vector3 world_dir = ac->get_global_transform().basis.xform(out);
		if (world_dir.length_squared() > 1e-9)
		{
			const Vector3 local_dir = anchor->get_global_transform().basis.inverse().xform(world_dir);
			if (local_dir.length_squared() > 1e-12)
			{
				centre += local_dir.normalized() * (retractable ? OUT_OFFSET * 0.6 : OUT_OFFSET);
			}
		}
	}
	Ref<ArrayMesh> mesh = memnew(ArrayMesh);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array verts;
	verts.push_back(centre);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_POINTS, arrays);
	Ref<ShaderMaterial> mat = memnew(ShaderMaterial);
	mat->set_shader(shader);
	const Color col = colour_of(kind);
	mat->set_shader_parameter("colour", Vector3(col.r, col.g, col.b));
	mat->set_shader_parameter("size_scale", size_of(kind));
	mat->set_shader_parameter("on", 0.0);
	mat->set_shader_parameter("pull_m", anchor->is_inside_tree() ? PULL_FACTOR * anchor->get_global_transform().basis.get_scale().x : 0.01);
	mesh->surface_set_material(0, mat);
	MeshInstance3D *sprite = memnew(MeshInstance3D);
	sprite->set_name("LightSprite_" + kind);
	sprite->set_mesh(mesh);
	sprite->set_custom_aabb(AABB(centre - Vector3(0.5, 0.5, 0.5), Vector3(1, 1, 1)));
	sprite->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	anchor->add_child(sprite);
	Fixture f;
	f.kind = kind;
	f.sprite = sprite;
	f.mat = mat;
	f.anchor = anchor;
	f.anchor_id = anchor->get_instance_id();
	f.centre = centre;
	f.retractable = retractable;
	// lit / unlit mesh pair
	if (pair_meshes)
	{
		Node3D *sib = off_sibling(anchor);
		if (sib != nullptr)
		{
			f.on_mesh = anchor;
			f.off_mesh = sib;
		}
	}
	if (kind == "landing" || kind == "taxi")
	{
		SpotLight3D *spot = memnew(SpotLight3D);
		spot->set_name("Spot_" + kind);
		spot->set_color(col);
		spot->set_param(Light3D::PARAM_ENERGY, kind == "landing" ? spot_energy : spot_energy * 0.5);
		spot->set_param(Light3D::PARAM_RANGE, kind == "landing" ? landing_range_m : taxi_range_m);
		spot->set_param(Light3D::PARAM_SPOT_ANGLE, kind == "landing" ? 14.0 : 32.0);
		spot->set_param(Light3D::PARAM_SPOT_ATTENUATION, 0.8);
		spot->set_shadow(false);
		spot->set_visible(false);
		add_child(spot);
		f.spot = spot;
		f.energy = spot->get_param(Light3D::PARAM_ENERGY);
	}
	fixtures.push_back(f);
}

void AircraftLights::set_landing_lights(bool on)
{
	landing_on = on;
	apply_states();
}

bool AircraftLights::landing_lights_on() const { return landing_on; }

bool AircraftLights::toggle_landing_lights()
{
	set_landing_lights(!landing_on);
	return landing_on;
}

int AircraftLights::fixture_count(const String &kind) const
{
	if (kind.is_empty())
	{
		return fixtures.size();
	}
	int n = 0;
	for (int i = 0; i < fixtures.size(); i++)
	{
		if (fixtures[i].kind == kind)
		{
			n++;
		}
	}
	return n;
}

bool AircraftLights::is_lit(const String &kind) const
{
	for (int i = 0; i < fixtures.size(); i++)
	{
		if (fixtures[i].kind == kind)
		{
			return (double)fixtures[i].mat->get_shader_parameter("on") > 0.01;
		}
	}
	return false;
}

Array AircraftLights::get_fixtures() const
{
	Array out;
	for (int i = 0; i < fixtures.size(); i++)
	{
		const Fixture &f = fixtures[i];
		Dictionary d;
		d["kind"] = f.kind;
		d["sprite"] = f.sprite;
		d["anchor"] = (f.anchor_id != 0 && UtilityFunctions::is_instance_id_valid(f.anchor_id)) ? f.anchor : nullptr;
		d["spot"] = f.spot;
		out.push_back(d);
	}
	return out;
}

void AircraftLights::update(double delta, bool p_airborne, bool p_gear_down, double p_night, const Vector3 &p_forward, const Vector3 &p_down)
{
	time += delta;
	airborne = p_airborne;
	gear_down = p_gear_down;
	night = Math::clamp(p_night, 0.0, 1.0);
	forward = p_forward;
	down = p_down;
	apply_states();
}

void AircraftLights::apply_states()
{
	const double bright = powered ? (NIGHT_FLOOR + (1.0 - NIGHT_FLOOR) * night) : 0.0;
	const double strobe_phase = std::fmod(time, STROBE_PERIOD_S);
	bool strobe_on = airborne && (strobe_phase < STROBE_FLASH_S || (strobe_phase > 0.16 && strobe_phase < 0.16 + STROBE_FLASH_S));
	bool beacon_on = std::fmod(time, BEACON_PERIOD_S) < BEACON_FLASH_S;
	if (debug_all_on)
	{
		strobe_on = true;
		beacon_on = true;
	}
	const bool landing_lit = landing_on && gear_down;
	// the landing spot is aimed along the aircraft, pitched down
	const double lp = Math::deg_to_rad(landing_pitch_deg);
	const double tp = Math::deg_to_rad(landing_pitch_deg + 6.0);
	const Vector3 aim = (forward * std::cos(lp) + down * std::sin(lp)).normalized();
	const Vector3 taxi_aim = (forward * std::cos(tp) + down * std::sin(tp)).normalized();
	for (int i = 0; i < fixtures.size(); i++)
	{
		Fixture &f = fixtures.write[i];
		double on = 0.0;
		if (f.kind == "nav_left" || f.kind == "nav_right" || f.kind == "nav_tail")
		{
			on = nav_enabled ? bright : 0.0;
		}
		else if (f.kind == "strobe")
		{
			on = (strobe_on && strobe_enabled && powered) ? 1.0 : 0.0;
		}
		else if (f.kind == "beacon")
		{
			on = (beacon_on && beacon_enabled) ? bright : 0.0;
		}
		else if (f.kind == "landing" || f.kind == "taxi")
		{
			on = (landing_lit && powered && (f.kind == "landing" || taxi_enabled)) ? 1.0 : 0.0;
		}
		// a level the aircraft's systems give (extension, dimming, power) replaces the logic above
		if (f.level >= 0.0)
		{
			on = Math::clamp(f.level, 0.0, 1.0);
		}
		if (f.kind == "flood")
		{
			// the knob is a display intent: linear light through the sRGB curve
			const double lin = std::pow(on, 2.2);
			if (f.omni != nullptr)
			{
				f.omni->set_visible(lin > 0.002);
				f.omni->set_param(Light3D::PARAM_ENERGY, f.energy * lin);
			}
			continue;
		}
		f.mat->set_shader_parameter("on", on);
		const bool anchor_alive = f.anchor_id != 0 && UtilityFunctions::is_instance_id_valid(f.anchor_id);
		if (f.on_mesh != nullptr && anchor_alive)
		{
			f.on_mesh->set_visible(on > 0.01);
			f.off_mesh->set_visible(on <= 0.01);
		}
		if (f.spot != nullptr)
		{
			f.spot->set_visible(on > 0.01);
			// a landing light barely shows on sunlit ground; the level the
			// aircraft's systems give (DIM 0.6 / BRT 1) is a display intent
			f.spot->set_param(Light3D::PARAM_ENERGY, f.energy * (0.08 + 0.92 * night) * std::pow(on, 2.2));
			if (f.spot->is_visible() && anchor_alive && f.anchor->is_inside_tree())
			{
				const Vector3 origin = f.anchor->get_global_transform().xform(f.centre);
				Vector3 dir = f.kind == "landing" ? aim : taxi_aim;
				if (f.retractable && f.ext < 1.0)
				{
					// the beam swings with the lamp: straight down when stowed
					const double a = (1.0 - f.ext) * Math_PI * 0.5;
					dir = (dir * std::cos(a) + down * std::sin(a)).normalized();
				}
				// spot lights shine along -Z
				const Vector3 z = -dir;
				Vector3 x = (-down).cross(z);
				if (x.length_squared() < 1e-6)
				{
					x = forward.cross(z);
				}
				x = x.normalized();
				const Vector3 y = z.cross(x).normalized();
				f.spot->set_global_transform(Transform3D(Basis(x, y, z), origin));
			}
		}
	}
}

} // namespace jsbgodot
