#include "key_bindings.h"
#include "aircraft_rig.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_ogg_vorbis.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "aircraft_model_loader.h"
#include "flight_hud.h"
#include "settings_store.h"
#include "aircraft_settings_panel.h"
#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

static const Vector3 AXIS_X(1, 0, 0);
static const Vector3 AXIS_Y(0, 1, 0);
static const Vector3 AXIS_Z(0, 0, 1);

void AircraftRig::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("setup", "host", "ac_node", "cockpit_camera", "camera_rig"), &AircraftRig::setup);
	ClassDB::bind_method(D_METHOD("load_config", "aircraft_id"), &AircraftRig::load_config);
	ClassDB::bind_method(D_METHOD("get_config"), &AircraftRig::get_config);
	ClassDB::bind_method(D_METHOD("get_aircraft_id"), &AircraftRig::get_aircraft_id);
	ClassDB::bind_method(D_METHOD("get_model_root_path"), &AircraftRig::get_model_root_path);
	ClassDB::bind_method(D_METHOD("get_model_root"), &AircraftRig::get_model_root);
	ClassDB::bind_method(D_METHOD("is_gear_retractable"), &AircraftRig::is_gear_retractable);

	ClassDB::bind_method(D_METHOD("apply_selected_aircraft", "aircraft_id", "jsb"), &AircraftRig::apply_selected_aircraft);
	ClassDB::bind_method(D_METHOD("apply_model_normals", "root"), &AircraftRig::apply_model_normals);
	ClassDB::bind_method(D_METHOD("is_vehicle"), &AircraftRig::is_vehicle);
	ClassDB::bind_method(D_METHOD("get_vehicle_kind"), &AircraftRig::get_vehicle_kind);
	ClassDB::bind_method(D_METHOD("get_vehicle_params"), &AircraftRig::get_vehicle_params);
	ClassDB::bind_method(D_METHOD("get_fg_property", "key"), &AircraftRig::get_fg_property);
	ClassDB::bind_method(D_METHOD("resolve_nodes", "jsb"), &AircraftRig::resolve_nodes);
	ClassDB::bind_method(D_METHOD("apply_camera_config"), &AircraftRig::apply_camera_config);
	ClassDB::bind_method(D_METHOD("apply_nose_steer_range", "jsb"), &AircraftRig::apply_nose_steer_range);

	ClassDB::bind_method(D_METHOD("setup_engine_sound", "parent"), &AircraftRig::setup_engine_sound);
	ClassDB::bind_method(D_METHOD("get_engine_audio"), &AircraftRig::get_engine_audio);
	ClassDB::bind_method(D_METHOD("update", "jsb", "delta", "user_volume_pct"), &AircraftRig::update);
	ClassDB::bind_method(D_METHOD("update_systems", "jsb", "delta", "rendering_cam", "pick_camera", "night", "sim_time_seconds", "sounds_active", "user_volume_pct"), &AircraftRig::update_systems);
	ClassDB::bind_method(D_METHOD("poll_keys", "jsb"), &AircraftRig::poll_keys);
	ClassDB::bind_method(D_METHOD("toggle_reverse_thrust", "jsb"), &AircraftRig::toggle_reverse_thrust);
	ClassDB::bind_method(D_METHOD("is_reverse_engaged"), &AircraftRig::is_reverse_engaged);
	ClassDB::bind_method(D_METHOD("set_parking_brake_at_start", "jsb"), &AircraftRig::set_parking_brake_at_start);
	ClassDB::bind_method(D_METHOD("get_parking_brake"), &AircraftRig::get_parking_brake);
	ClassDB::bind_method(D_METHOD("set_parking_brake", "on"), &AircraftRig::set_parking_brake);
	ClassDB::bind_method(D_METHOD("get_fg_bridge"), &AircraftRig::get_fg_bridge);
	ClassDB::bind_method(D_METHOD("get_strut_shifts"), &AircraftRig::get_strut_shifts);
	ClassDB::bind_method(D_METHOD("get_exhaust_effects"), &AircraftRig::get_exhaust_effects);
	ClassDB::bind_method(D_METHOD("get_fg_controls"), &AircraftRig::get_fg_controls);
	ClassDB::bind_method(D_METHOD("get_fg_property_rules"), &AircraftRig::get_fg_property_rules);
	ClassDB::bind_method(D_METHOD("get_autoflight"), &AircraftRig::get_autoflight);
	ClassDB::bind_method(D_METHOD("get_mcdu"), &AircraftRig::get_mcdu);
	ClassDB::bind_method(D_METHOD("_on_autoflight_action", "kind", "arg"), &AircraftRig::_on_autoflight_action);
	ClassDB::bind_method(D_METHOD("get_fg_sounds"), &AircraftRig::get_fg_sounds);
	ClassDB::bind_method(D_METHOD("get_fg_animator"), &AircraftRig::get_fg_animator);
	ClassDB::bind_method(D_METHOD("get_aircraft_lights"), &AircraftRig::get_aircraft_lights);
	ClassDB::bind_method(D_METHOD("setup_aircraft_lights"), &AircraftRig::setup_aircraft_lights);
	ClassDB::bind_method(D_METHOD("setup_fg_animations"), &AircraftRig::setup_fg_animations);
	ClassDB::bind_method(D_METHOD("setup_fg_sounds"), &AircraftRig::setup_fg_sounds);
	ClassDB::bind_method(D_METHOD("set_livery", "livery_id"), &AircraftRig::set_livery);
	ClassDB::bind_method(D_METHOD("get_livery"), &AircraftRig::get_livery);
	ClassDB::bind_method(D_METHOD("get_liveries"), &AircraftRig::get_liveries);
	ClassDB::bind_method(D_METHOD("_on_fg_control", "prop", "value"), &AircraftRig::_on_fg_control);

	ClassDB::bind_method(D_METHOD("get_control_surfaces"), &AircraftRig::get_control_surfaces);
	ClassDB::bind_method(D_METHOD("get_cockpit_controls"), &AircraftRig::get_cockpit_controls);
	ClassDB::bind_method(D_METHOD("get_engine_sound"), &AircraftRig::get_engine_sound);

	ADD_SIGNAL(MethodInfo("nodes_resolved"));
}

AircraftRig::AircraftRig()
{
	control_surfaces.instantiate();
	cockpit_controls.instantiate();
	engine_sound.instantiate();
	fg_bridge.instantiate();
	fg_controls.instantiate();
}

void AircraftRig::setup(Node *p_host, Node3D *p_ac_node, Camera3D *p_cockpit_camera, CameraRig *p_camera_rig)
{
	host = p_host;
	ac_node = p_ac_node;
	cockpit_camera = p_cockpit_camera;
	camera_rig = p_camera_rig;
	if (cockpit_camera != nullptr)
	{
		cockpit_scene_transform = cockpit_camera->get_transform();
		cockpit_scene_fov = cockpit_camera->get_fov();
	}
}

// ── Configuration ───────────────────────────────────────────────────────────

bool AircraftRig::load_config(const String &p_aircraft_id)
{
	aircraft_id = p_aircraft_id.is_empty() ? String("c172p") : p_aircraft_id;
	const String cfg_path = AircraftConfig::resolve_path(aircraft_id);
	cfg = AircraftConfig::load_for_aircraft(aircraft_id); // [aircraft] base: the base's sections under the variant's
	if (!cfg->is_loaded())
	{
		UtilityFunctions::print("Aircraft config not found (", cfg_path, ") - using built-in defaults");
		return false;
	}
	UtilityFunctions::print("Aircraft config loaded: ", cfg_path);

	model_root_path = cfg->get_string("model", "root_node", model_root_path);

	// Control-surface deflection limits + hinge axes.
	ControlSurfaces *cs = control_surfaces.ptr();
	cs->set_rudder_max_angle(cfg->get_float("control_limits", "rudder_max_deg", 16.0));
	cs->set_rudder_axis(cfg->get_axis("control_limits", "rudder_hinge_axis", AXIS_Y));
	cs->set_aileron_max_angle(cfg->get_float("control_limits", "aileron_max_deg", 20.0));
	cs->set_aileron_axis(cfg->get_axis("control_limits", "aileron_hinge_axis", AXIS_X));
	cs->set_elevator_max_angle(cfg->get_float("control_limits", "elevator_max_deg", 25.0));
	cs->set_elevator_axis(cfg->get_axis("control_limits", "elevator_hinge_axis", AXIS_X));
	cs->set_flap_max_angle(cfg->get_float("control_limits", "flap_max_deg", 30.0));
	cs->set_flap_axis(cfg->get_axis("control_limits", "flap_hinge_axis", AXIS_X));
	cs->set_front_wheel_max_angle(cfg->get_float("control_limits", "front_wheel_max_deg", 30.0));

	// Retractable landing gear.
	gear_retractable = cfg->get_bool("gear", "retractable", false);
	cockpit_controls->set_gear_retractable(gear_retractable);
	cs->set_gear_retract_seconds(cfg->get_float("gear", "retract_seconds", 4.0));
	gear_anim_name = cfg->get_string("gear", "animation", "");

	// Thrust reversers (the MD-80's JSBSim systems: flags the throttle input is read through).
	reverse_engage_props = cfg->get_string_array("engines", "reverse_engage_props", Array());
	reverse_lever_prop = cfg->get_string("engines", "reverse_lever_max_prop", "");
	reverse_engaged = false;

	// Speed brake lever of an FDM with its own spoiler system (the MD-80).
	speedbrake_prop = cfg->get_string("controls", "speedbrake_prop", "");
	speedbrake_key_value = cfg->get_float("controls", "speedbrake_key_value", 1.0);
	speedbrake_step = cfg->get_float("controls", "speedbrake_step", 0.0);
	speedbrake_out = false;

	// Flap detents (cockpit lever notches).
	const Array positions = cfg->get_float_array("flaps", "positions", Array());
	if (!positions.is_empty())
	{
		cockpit_controls->set_flap_positions(positions);
	}
	const Array names = cfg->get_string_array("flaps", "names", Array());
	if (!names.is_empty())
	{
		PackedStringArray psa;
		for (int i = 0; i < names.size(); i++)
		{
			psa.push_back(names[i]);
		}
		cockpit_controls->set_flap_names(psa);
	}

	// Propeller visuals: blur thresholds + spin axis for the animator, disc
	// size/offset for the blur disc built here.
	cs->set_propeller_blur_start_rpm(cfg->get_float("propeller", "blur_start_rpm", 900.0));
	cs->set_propeller_hide_blades_rpm(cfg->get_float("propeller", "hide_blades_rpm", 1200.0));
	// Spinner cone: swap-in mesh shown only while the blades hide (PC-7), or a
	// separate part shown at every rpm; spins with the blades when its origin
	// sits on the axis (fg_panel_build.py's Spinner_Pivot).
	cs->set_propeller_cone_always_visible(cfg->get_bool("propeller", "cone_always_visible", false));
	cs->set_propeller_cone_spins(cfg->get_bool("propeller", "cone_spins", false));
	propeller_disc_radius = cfg->get_float("propeller", "disc_radius_m", propeller_disc_radius);
	propeller_spin_axis = cfg->get_axis("propeller", "spin_axis", AXIS_Z);
	cs->set_propeller_spin_axis(propeller_spin_axis);
	propeller_disc_offset = cfg->get_vector3("propeller", "disc_offset", Vector3());

	// Engine sound: the stream path stays here (the player is built here), the
	// RPM->pitch/volume tunables go to the EngineSound module, including
	// [propeller] max_rpm which only feeds its RPM normalisation.
	engine_sound_path = cfg->get_string("sound", "engine", engine_sound_path);
	engine_sound->set_pitch_range(cfg->get_float("sound", "pitch_min", 0.7), cfg->get_float("sound", "pitch_max", 1.6));
	engine_sound->set_vol_db_range(cfg->get_float("sound", "vol_db_min", -12.0), cfg->get_float("sound", "vol_db_max", 3.0));
	engine_sound->set_max_rpm(cfg->get_float("propeller", "max_rpm", 2700.0));
	return true;
}

Ref<AircraftConfig> AircraftRig::get_config() const { return cfg; }
String AircraftRig::get_aircraft_id() const { return aircraft_id; }
String AircraftRig::get_model_root_path() const { return model_root_path; }
bool AircraftRig::is_gear_retractable() const { return gear_retractable; }

Node *AircraftRig::get_model_root() const
{
	if (host == nullptr)
	{
		return nullptr;
	}
	return host->get_node_or_null(NodePath(model_root_path));
}

// ── Model ───────────────────────────────────────────────────────────────────

void AircraftRig::apply_nose_steer_range(Object *p_jsb)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr)
	{
		return;
	}
	double limited = 0.0, full = 0.0;
	if (cfg.is_valid())
	{
		limited = cfg->get_float("gear", "steer_limited_deg", 0.0);
		full = cfg->get_float("gear", "steer_full_deg", 0.0);
	}
	fdm->set_nose_steer_range(limited, full);
	fdm->set_nose_steer_full(false);
}

bool AircraftRig::apply_selected_aircraft(const String &p_aircraft_id, Object *p_jsb)
{
	const String id = p_aircraft_id.is_empty() ? String("c172p") : p_aircraft_id;
	// the previous flight's sounds must not carry into the next zoom-in
	silence_sounds();
	if (id != aircraft_id || cfg.is_null())
	{
		load_config(id);
	}
	String jsbsim_model = aircraft_id;
	if (cfg.is_valid())
	{
		jsbsim_model = cfg->get_string("jsbsim", "model", aircraft_id);
	}
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm != nullptr && !is_vehicle())
	{
		UtilityFunctions::print("Loading JSBSim model '", jsbsim_model, "' for aircraft '", aircraft_id, "'");
		fdm->load_aircraft(aircraft_id, jsbsim_model);
		apply_fdm_init_properties(fdm);
	}
	else if (is_vehicle())
	{
		// a ground vehicle keeps whatever JSBSim model is loaded: DriveMode
		// moves the node and holds JSBSim paused
		UtilityFunctions::print("Vehicle '", aircraft_id, "' (", cfg->get_string("vehicle", "kind", "?"), "): no JSBSim model");
	}
	apply_nose_steer_range(p_jsb);

	// Swap the visual GLB only when the model actually changes: re-running on
	// the loaded model would double-flip the normals (lighting it from below).
	bool changed = false;
	if (aircraft_id != model_visuals_id)
	{
		AircraftModelLoader *loader = Object::cast_to<AircraftModelLoader>(get_model_root());
		if (loader != nullptr)
		{
			if (loader->load_aircraft_model(aircraft_id))
			{
				apply_model_normals(loader);
				hide_model_nodes(loader);
				resolve_nodes(p_jsb);
				model_visuals_id = aircraft_id;
				changed = true;
			}
		}
	}
	apply_camera_config();
	return changed;
}

// [model] hide_nodes: a comma list of node names the model ships but must
// not draw (a Sketchfab collision proxy, a stand).
void AircraftRig::hide_model_nodes(Node *root)
{
	if (root == nullptr || cfg.is_null())
		return;
	const PackedStringArray names = cfg->get_string("model", "hide_nodes", "").split(",", false);
	for (int i = 0; i < names.size(); i++)
	{
		Node *n = root->find_child(names[i].strip_edges(), true, false);
		Node3D *n3 = Object::cast_to<Node3D>(n);
		if (n3 != nullptr)
			n3->set_visible(false);
		else
			UtilityFunctions::push_warning("AircraftRig: hide_nodes: no node '", names[i], "' in the model of ", aircraft_id);
	}
}

bool AircraftRig::is_vehicle() const
{
	return cfg.is_valid() && cfg->is_loaded() && !cfg->get_string("vehicle", "kind", "").is_empty();
}

String AircraftRig::get_vehicle_kind() const
{
	return is_vehicle() ? cfg->get_string("vehicle", "kind", "") : String();
}

// The [vehicle] section as DriveMode::configure wants it.
Variant AircraftRig::get_fg_property(const String &key) const
{
	const String k = key.begins_with("/") ? key.substr(1) : key;
	return last_fg_props.has(k) ? last_fg_props[k] : Variant();
}

Dictionary AircraftRig::get_vehicle_params() const
{
	Dictionary d;
	if (!is_vehicle())
		return d;
	d["kind"] = cfg->get_string("vehicle", "kind", "");
	static const char *KEYS[] = {"wheelbase_m", "track_m", "max_speed_kmh", "max_reverse_kmh", "accel_mps2", "brake_mps2", "max_steer_deg", "steer_rate_deg_s", "clearance_m"};
	for (const char *k : KEYS)
		if (!cfg->get_string("vehicle", k, "").is_empty())
			d[k] = cfg->get_float("vehicle", k, 0.0);
	return d;
}

void AircraftRig::apply_model_normals(Node *root)
{
	if (root == nullptr)
	{
		return;
	}
	apply_glass_materials(root);
	if (cfg.is_valid() && !cfg->get_bool("model", "flip_normals", true))
	{
		return;
	}
	String prefix = "Cessna_Exterior_";
	if (cfg.is_valid())
	{
		prefix = cfg->get_string("model", "flip_normals_prefix", prefix);
	}
	flip_normals_recursive(root, prefix);
}

void AircraftRig::apply_glass_materials(Node *root)
{
	if (root == nullptr || !cfg.is_valid())
	{
		return;
	}
	const Array names = cfg->get_string_array("model", "glass_materials", Array());
	if (names.is_empty())
	{
		return;
	}
	const Vector3 tint = cfg->get_vector3("model", "glass_tint", Vector3(0.35, 0.4, 0.45));
	const double alpha = cfg->get_float("model", "glass_alpha", 0.14);
	const double rough = cfg->get_float("model", "glass_roughness", 0.06);
	auto matches = [&](const String &mat_name) {
		for (int i = 0; i < names.size(); i++)
		{
			const String pat = String(names[i]).strip_edges();
			if (pat.ends_with("*") ? mat_name.begins_with(pat.left(pat.length() - 1)) : mat_name == pat)
			{
				return true;
			}
		}
		return false;
	};
	int done = 0;
	Vector<Node *> stack;
	stack.push_back(root);
	while (!stack.is_empty())
	{
		Node *n = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		for (int i = 0; i < n->get_child_count(); i++)
		{
			stack.push_back(n->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n);
		if (mi == nullptr || mi->get_mesh().is_null())
		{
			continue;
		}
		for (int s = 0; s < mi->get_mesh()->get_surface_count(); s++)
		{
			Ref<Material> m = mi->get_active_material(s);
			BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(m.ptr());
			if (bm == nullptr || !matches(bm->get_name()))
			{
				continue;
			}
			bm->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
			bm->set_albedo(Color(tint.x, tint.y, tint.z, alpha));
			bm->set_roughness(rough);
			bm->set_metallic(0.0);
			bm->set_specular(0.5);
			bm->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			done++;
		}
	}
	if (done > 0)
	{
		UtilityFunctions::print("Glass materials: ", done, " surface(s) made clear (alpha ", alpha, ", roughness ", rough, ")");
	}
}

// Only exterior surfaces are flipped (prefix): the interior cockpit meshes
// ship with correct normals and would be backface-culled from inside the
// cabin; the Dynamic_Switch_* subtree (re-rigged controls) is skipped too.
void AircraftRig::flip_normals_recursive(Node *node, const String &prefix)
{
	// the FlightGear instruments built into the panel (fg_panel_build.py's
	// Panel_Root) have correct normals; a model-wide flip must spare them
	if (node->get_name().begins_with("Dynamic_Switch_") || node->get_name() == StringName("Panel_Root"))
	{
		return;
	}
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(node);
	if (mi != nullptr && (prefix.is_empty() || node->get_name().begins_with(prefix)))
	{
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid())
		{
			Ref<ArrayMesh> new_mesh;
			new_mesh.instantiate();
			for (int s = 0; s < mesh->get_surface_count(); s++)
			{
				Array arrays = mesh->surface_get_arrays(s);
				const Variant normals_v = arrays[Mesh::ARRAY_NORMAL];
				if (normals_v.get_type() != Variant::NIL)
				{
					PackedVector3Array normals = normals_v;
					Vector3 *w = normals.ptrw();
					for (int i = 0; i < normals.size(); i++)
					{
						w[i] = -w[i];
					}
					arrays[Mesh::ARRAY_NORMAL] = normals;
				}
				Ref<ArrayMesh> src = mesh;
				const Mesh::PrimitiveType prim = src.is_valid() ? src->surface_get_primitive_type(s) : Mesh::PRIMITIVE_TRIANGLES;
				new_mesh->add_surface_from_arrays(prim, arrays);
				Ref<Material> mat = mesh->surface_get_material(s);
				if (mat.is_valid())
				{
					new_mesh->surface_set_material(s, mat);
				}
			}
			mi->set_mesh(new_mesh);
		}
	}
	const TypedArray<Node> children = node->get_children();
	for (int i = 0; i < children.size(); i++)
	{
		flip_normals_recursive(Object::cast_to<Node>(children[i]), prefix);
	}
}

Node3D *AircraftRig::resolve_model_node(const String &rel) const
{
	if (rel.is_empty() || host == nullptr)
	{
		return nullptr;
	}
	return Object::cast_to<Node3D>(host->get_node_or_null(NodePath(model_root_path.path_join(rel))));
}

Node3D *AircraftRig::resolve_surface(const String &key) const
{
	if (cfg.is_null())
	{
		return nullptr;
	}
	return resolve_model_node(cfg->get_string("animation", key, ""));
}

// Cockpit lever/switch by name (find_child) under `root`: the primary name
// from cfg [levers] <key>, else "<key>_fallback" (the original Sketchfab name).
Node3D *AircraftRig::resolve_lever(Node *root, const String &key) const
{
	if (cfg.is_null() || root == nullptr)
	{
		return nullptr;
	}
	Node *node = nullptr;
	const String primary = cfg->get_string("levers", key, "");
	if (!primary.is_empty())
	{
		node = root->find_child(primary, true, false);
	}
	if (node == nullptr)
	{
		const String fallback = cfg->get_string("levers", key + String("_fallback"), "");
		if (!fallback.is_empty())
		{
			node = root->find_child(fallback, true, false);
		}
	}
	return Object::cast_to<Node3D>(node);
}

void AircraftRig::resolve_nodes(Object *p_jsb)
{
	// The model loader frees the previous GLB subtree synchronously on an
	// airframe swap, so drop every registered pointer before re-resolving.
	ControlSurfaces *cs = control_surfaces.ptr();
	cs->reset();
	cockpit_controls->reset();
	gear_struts.reset();
	fg_bridge->clear_compression_norms();

	struct Surface
	{
		const char *key;
		void (ControlSurfaces::*reg)(Node3D *);
		bool required;
	};
	const Surface surfaces[] = {
		{"rudder", &ControlSurfaces::register_rudder, true},
		{"propeller_cone", &ControlSurfaces::register_propeller_cone, false},
		{"left_aileron", &ControlSurfaces::register_aileron_left, true},
		{"right_aileron", &ControlSurfaces::register_aileron_right, true},
		{"left_elevator", &ControlSurfaces::register_elevator_left, true},
		{"right_elevator", &ControlSurfaces::register_elevator_right, true},
		{"single_elevator", &ControlSurfaces::register_single_elevator, false},
		{"left_flap", &ControlSurfaces::register_flap_left, true},
		{"right_flap", &ControlSurfaces::register_flap_right, true},
		{"front_wheel", &ControlSurfaces::register_front_wheel, true},
	};
	for (const Surface &s : surfaces)
	{
		Node3D *node = resolve_surface(s.key);
		if (node != nullptr)
		{
			(cs->*s.reg)(node);
		}
		else if (s.required)
		{
			UtilityFunctions::print("WARNING: ", s.key, " node not found");
		}
	}
	Node3D *propeller = resolve_surface("propeller");
	if (propeller != nullptr)
	{
		cs->register_propeller(propeller);
		create_propeller_blur_disc(propeller);
	}
	else
	{
		UtilityFunctions::print("WARNING: propeller node not found");
	}

	// Generic multi-flap list (cfg [animation] flaps), e.g. the PC-7's four
	// split flaps, each with its per-flap sign (cfg flap_signs).
	if (cfg.is_valid())
	{
		const Array flap_paths = cfg->get_string_array("animation", "flaps", Array());
		const Array flap_signs = cfg->get_float_array("control_limits", "flap_signs", Array());
		for (int i = 0; i < flap_paths.size(); i++)
		{
			Node3D *fnode = resolve_model_node(flap_paths[i]);
			if (fnode != nullptr)
			{
				cs->add_flap_node(fnode, i < flap_signs.size() ? (double)flap_signs[i] : 1.0);
			}
			else
			{
				UtilityFunctions::print("WARNING: flap node not found: ", flap_paths[i]);
			}
		}
	}

	resolve_gear_legs(p_jsb);
	resolve_levers(p_jsb);
	// The systems built on the model; the hidden nodes are applied last so a
	// FlightGear select cannot re-show them.
	setup_aircraft_lights();
	setup_fg_animations();
	setup_gear_struts();
	if (exhaust == nullptr && host != nullptr)
	{
		exhaust = memnew(ExhaustEffects);
		exhaust->set_name("ExhaustEffects");
		host->add_child(exhaust);
	}
	if (exhaust != nullptr)
	{
		exhaust->setup(ac_node, p_jsb);
		exhaust->configure(cfg);
	}
	setup_fg_sounds();
	setup_fg_property_rules();
	setup_autoflight();
	if (fg_animator != nullptr)
	{
		PackedStringArray keys = fg_animator->property_names();
		if (fg_sounds != nullptr)
		{
			keys.append_array(fg_sounds->property_names());
		}
		if (fg_rules.is_valid())
		{
			keys.append_array(fg_rules->input_names());
		}
		if (autoflight.is_valid())
		{
			keys.append_array(autoflight->input_names());
		}
		fg_bridge->set_mirror_pull_keys(keys);
		fg_bridge->set_property_aliases(fg_animator->get_aliases());
	}
	emit_signal("nodes_resolved");
	apply_hidden_nodes();
}

// Retractable-gear legs from cfg [gear]. "legs" lists the leg names; each
// <name> reads <name>_wheel / _strut (paths), _pivot "x, y, z", _axis,
// _retract_deg, optional steering (_steer, _steer_axis, _steer_max_deg,
// _steer_pivot) and rolling (_roll, _roll_axis, _wheel_radius_m). Wheel and
// strut rotate rigidly about the shared pivot so the leg sweeps as one unit.
void AircraftRig::resolve_gear_legs(Object *p_jsb)
{
	if (cfg.is_null())
	{
		return;
	}
	const Array legs = cfg->get_string_array("gear", "legs", Array());
	// Fixed gear may list legs too (rolling tyres): they never retract.
	if (!gear_retractable && legs.is_empty())
	{
		return;
	}
	ControlSurfaces *cs = control_surfaces.ptr();
	const double default_steer_max = cfg->get_float("control_limits", "front_wheel_max_deg", 30.0);
	for (int i = 0; i < legs.size(); i++)
	{
		const String leg = legs[i];
		Node3D *wheel = resolve_model_node(cfg->get_string("gear", leg + String("_wheel"), ""));
		Node3D *strut = resolve_model_node(cfg->get_string("gear", leg + String("_strut"), ""));
		if (wheel == nullptr && strut == nullptr)
		{
			UtilityFunctions::print("WARNING: gear leg '", leg, "' has no resolvable nodes");
			continue;
		}
		const Vector3 pivot = cfg->get_vector3("gear", leg + String("_pivot"), Vector3());
		const Vector3 axis = cfg->get_axis("gear", leg + String("_axis"), AXIS_X);
		// Optional nose-wheel steering: the wheel yaws about its own steer axis
		// with rudder input, composed UNDER the retract rotation. The steer
		// pivot defaults to the retract pivot so they share one axis.
		const bool steer = cfg->get_bool("gear", leg + String("_steer"), false);
		const Vector3 steer_axis = cfg->get_axis("gear", leg + String("_steer_axis"), AXIS_Z);
		const double steer_max = cfg->get_float("gear", leg + String("_steer_max_deg"), default_steer_max);
		const Vector3 steer_pivot = cfg->get_vector3("gear", leg + String("_steer_pivot"), pivot);
		// Optional rolling wheel: spins about its axle on the ground, stops
		// once airborne (weight-off-wheels).
		const bool roll = cfg->get_bool("gear", leg + String("_roll"), false);
		const Vector3 roll_axis = cfg->get_axis("gear", leg + String("_roll_axis"), AXIS_Y);
		const double radius = cfg->get_float("gear", leg + String("_wheel_radius_m"), cfg->get_float("gear", "wheel_radius_m", 0.3));
		cs->add_gear_leg(wheel, strut, pivot, axis,
				cfg->get_float("gear", leg + String("_retract_deg"), 90.0),
				steer, steer_axis, steer_max, steer_pivot,
				roll, roll_axis, Math::max(radius, 0.01));
		UtilityFunctions::print("Gear leg '", leg, "' resolved (wheel=", wheel != nullptr, " strut=", strut != nullptr, ")");
	}
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm != nullptr)
	{
		fdm->set_gear_down(true);
	}

	// Optional imported retract/extend clip: hold it at the gear-DOWN pose;
	// the animator then drives it by seeking instead of rotating leg pivots.
	if (gear_anim_name.is_empty())
	{
		return;
	}
	Node *model_root = get_model_root();
	if (model_root == nullptr)
	{
		UtilityFunctions::print("WARNING: model root not found for gear animation lookup");
		return;
	}
	AnimationPlayer *ap = Object::cast_to<AnimationPlayer>(model_root->find_child("AnimationPlayer", true, false));
	if (ap == nullptr || !ap->has_animation(gear_anim_name))
	{
		UtilityFunctions::print("WARNING: gear animation '", gear_anim_name, "' not found in model");
		return;
	}
	Ref<Animation> clip = ap->get_animation(gear_anim_name);
	// Scrub-only: the animator seeks every frame, so the player must neither
	// auto-advance nor loop (it would run forward on its own and snap back).
	clip->set_loop_mode(Animation::LOOP_NONE);
	ap->set_current_animation(gear_anim_name);
	ap->seek(0.0, true); // 0 = first keyframe = gear down
	ap->pause();
	UtilityFunctions::print("Gear animation '", gear_anim_name, "' found (length=", clip->get_length(), "s)");
	cs->set_gear_anim(ap, clip->get_length());
}

// Strut compression from cfg [gear] (src/gear_struts.h), one entry per FDM
// gear unit in the FDM's order:
//   strut_tyres     the tyre meshes (node names, or paths under the model root)
//   strut_travel_m  FlightGear models: metres of wheel travel per unit of
//                   gear/gear[n]/compression-norm (the oleo translate's factor)
//   strut_nodes     other models: the nodes to move (default: the tyres)
//   strut_range_m   "down, up" limits from the drawn pose, metres
//   strut_units     FDM unit indices, when not 0, 1, 2 ...
void AircraftRig::setup_gear_struts()
{
	if (cfg.is_null())
	{
		return;
	}
	const Array tyres = cfg->get_string_array("gear", "strut_tyres", Array());
	if (tyres.is_empty())
	{
		return;
	}
	const Array nodes = cfg->get_string_array("gear", "strut_nodes", Array());
	const Array travel = cfg->get_string_array("gear", "strut_travel_m", Array());
	const Array range = cfg->get_string_array("gear", "strut_range_m", Array());
	const Array unit_idx = cfg->get_string_array("gear", "strut_units", Array());
	Node *root = get_model_root();
	auto find = [&](const String &ref) -> Node3D * {
		const String r = ref.strip_edges();
		if (r.is_empty())
		{
			return nullptr;
		}
		if (r.contains("/"))
		{
			return resolve_model_node(r);
		}
		return root != nullptr ? Object::cast_to<Node3D>(root->find_child(r, true, false)) : nullptr;
	};
	for (int i = 0; i < tyres.size(); i++)
	{
		GearStruts::Unit u;
		u.fdm_unit = i < unit_idx.size() ? String(unit_idx[i]).strip_edges().to_int() : i;
		u.tyre = Object::cast_to<MeshInstance3D>(find(tyres[i]));
		if (u.tyre == nullptr)
		{
			UtilityFunctions::print("WARNING: gear strut tyre mesh not found: ", tyres[i]);
			continue;
		}
		if (!travel.is_empty())
		{
			u.travel_m = String(travel[MIN(i, travel.size() - 1)]).strip_edges().to_float();
			u.min_m = -0.1;
			u.max_m = 0.6;
		}
		else
		{
			u.node = i < nodes.size() ? find(nodes[i]) : nullptr;
			if (u.node == nullptr)
			{
				u.node = u.tyre;
			}
		}
		if (range.size() >= 2)
		{
			u.min_m = String(range[0]).strip_edges().to_float();
			u.max_m = String(range[1]).strip_edges().to_float();
		}
		gear_struts.add_unit(u);
	}
	UtilityFunctions::print("Gear struts: ", gear_struts.unit_count(), " wheels follow the FDM's gear contacts (",
			travel.is_empty() ? "node shift" : "FlightGear compression-norm", ")");
}

ExhaustEffects *AircraftRig::get_exhaust_effects() const { return exhaust; }

PackedFloat64Array AircraftRig::get_strut_shifts() const
{
	PackedFloat64Array out;
	for (int i = 0; i < gear_struts.unit_count(); i++)
	{
		out.push_back(gear_struts.shift_m(i));
	}
	return out;
}

void AircraftRig::resolve_levers(Object *p_jsb)
{
	Node *root = get_model_root();
	if (root == nullptr)
	{
		return;
	}
	Node3D *mixture = resolve_lever(root, "mixture");
	Node3D *throttle = resolve_lever(root, "throttle");
	Node3D *magneto = resolve_lever(root, "magneto");
	Node3D *flap = resolve_lever(root, "flap_lever");
	UtilityFunctions::print("Cockpit levers: mixture=", mixture != nullptr, " throttle=", throttle != nullptr,
			" magneto=", magneto != nullptr, " flaps=", flap != nullptr);
	CockpitControls *cc = cockpit_controls.ptr();
	if (JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb))
	{
		fdm->set_flap_detents(cc->get_flap_positions()); // the keys / joystick step the cfg detents
		if (cfg.is_valid())
		{
			// [flaps] gated_detent: a notch that only exists while gate_property >= gate_min
			// (the MD-80's DIAL detent is UP/MID again while the thumbwheel is stowed)
			const int gated = (int)cfg->get_float("flaps", "gated_detent", -1.0);
			if (gated >= 0)
			{
				fdm->set_flap_detent_gate(gated, cfg->get_string("flaps", "gate_property", ""), cfg->get_float("flaps", "gate_min", 1.0));
			}
		}
	}
	cc->register_levers(mixture, throttle, magneto, flap); // captures base transforms
	cc->create_pick_bodies();
	// degrees per magneto position about the key's local Z (sign = key sense)
	cc->set_magneto_step_angle(cfg.is_valid() ? cfg->get_float("levers", "magneto_step_deg", 30.0) : 30.0);
	// which position the mesh's rest pose shows (0 OFF .. 3 BOTH, 4 START)
	cc->set_magneto_default_position(cfg.is_valid() ? (int)cfg->get_float("levers", "magneto_rest_position", 3.0) : 3);
	if (mixture != nullptr)
	{
		cc->apply_mixture_value(p_jsb);
	}
	if (magneto != nullptr)
	{
		cc->apply_magneto_position(p_jsb);
	}
	if (flap != nullptr)
	{
		cc->sync_flaps_from_jsb(p_jsb);
	}
}

// Propeller blur disc: a thin unshaded cylinder in the swept plane, added as
// a sibling of the propeller; the animator fades it in/out by RPM.
void AircraftRig::create_propeller_blur_disc(Node3D *prop_node)
{
	MeshInstance3D *mesh_inst = Object::cast_to<MeshInstance3D>(prop_node);
	if (mesh_inst == nullptr)
	{
		UtilityFunctions::print("WARNING: propeller is not a MeshInstance3D");
		return;
	}
	Node *prop_parent = prop_node->get_parent();
	if (prop_parent == nullptr)
	{
		return;
	}
	const AABB aabb = mesh_inst->get_aabb();
	const Vector3 mesh_center = aabb.position + aabb.size / 2.0;
	const Vector3 spin_abs(Math::abs(propeller_spin_axis.x), Math::abs(propeller_spin_axis.y), Math::abs(propeller_spin_axis.z));

	// Disc radius: the configured value, else the largest mesh half-extent
	// PERPENDICULAR to the spin axis (the swept plane).
	double radius = propeller_disc_radius;
	if (radius <= 0.0)
	{
		const Vector3 extents(Math::abs(aabb.size.x), Math::abs(aabb.size.y), Math::abs(aabb.size.z));
		const Vector3 plane = extents - extents * spin_abs;
		radius = Math::max(plane.x, Math::max(plane.y, plane.z)) / 2.0;
	}

	Ref<StandardMaterial3D> blur_mat = memnew(StandardMaterial3D);
	blur_mat->set_albedo(Color(0.15, 0.15, 0.18, 0.1));
	blur_mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	blur_mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	blur_mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);

	Ref<CylinderMesh> disc_mesh = memnew(CylinderMesh);
	disc_mesh->set_top_radius(radius);
	disc_mesh->set_bottom_radius(radius);
	disc_mesh->set_height(0.02);
	disc_mesh->set_radial_segments(32);
	disc_mesh->set_rings(1);

	MeshInstance3D *blur_disc = memnew(MeshInstance3D);
	blur_disc->set_mesh(disc_mesh);
	blur_disc->set_material_override(blur_mat);
	blur_disc->set_visible(false);
	prop_parent->add_child(blur_disc);
	// The propeller pivot may sit at a blade tip (PC-7): map the mesh centre
	// into the parent frame, then apply the cfg tuning offset.
	blur_disc->set_transform(Transform3D(Basis(), prop_node->get_transform().xform(mesh_center) + propeller_disc_offset));
	// Orient the cylinder (long axis = local Y) so its flat faces are
	// perpendicular to the spin axis.
	if (spin_abs.x > 0.5)
	{
		blur_disc->set_rotation_degrees(Vector3(0, 0, 90));
	}
	else if (spin_abs.z > 0.5)
	{
		blur_disc->set_rotation_degrees(Vector3(90, 0, 0));
	}
	UtilityFunctions::print("Propeller blur disc created with radius ", radius, " m");
	control_surfaces->register_propeller_blur_disc(blur_disc, blur_mat.ptr());
}

// cfg [model] hide_nodes: model nodes (by name, all matches) never shown,
// e.g. placeholder display textures of a FlightGear model.
void AircraftRig::apply_hidden_nodes()
{
	Node *model_root = get_model_root();
	if (cfg.is_null() || model_root == nullptr)
	{
		return;
	}
	int n = 0;
	const Array names = cfg->get_string_array("model", "hide_nodes", Array());
	for (int i = 0; i < names.size(); i++)
	{
		const String nm = String(names[i]).strip_edges();
		if (nm.is_empty())
		{
			continue;
		}
		const TypedArray<Node> found = model_root->find_children(nm, "", true, false);
		for (int j = 0; j < found.size(); j++)
		{
			Node3D *n3 = Object::cast_to<Node3D>(found[j]);
			if (n3 != nullptr)
			{
				n3->set_visible(false);
				n++;
			}
		}
	}
	if (n > 0)
	{
		UtilityFunctions::print("Model: ", n, " nodes hidden by [model] hide_nodes");
	}
}

// cfg [camera]: cockpit_position "x, y, z" (AC-local origin), cockpit_rotation_deg
// "p, y, r" (extra rotation vs the scene basis), cockpit_fov. Always starts
// from the pristine scene transform so repeated swaps don't compound.
void AircraftRig::apply_camera_config()
{
	if (cockpit_camera == nullptr || cfg.is_null())
	{
		return;
	}
	const Array pos = cfg->get_float_array("camera", "cockpit_position", Array());
	const Array rot = cfg->get_float_array("camera", "cockpit_rotation_deg", Array());
	const double fov = cfg->get_float("camera", "cockpit_fov", 0.0);
	cockpit_camera->set_transform(cockpit_scene_transform);
	cockpit_camera->set_fov(cockpit_scene_fov);
	if (pos.size() == 3 || rot.size() == 3 || fov > 0.0)
	{
		Transform3D xform = cockpit_scene_transform;
		if (rot.size() == 3)
		{
			const Basis extra = Basis::from_euler(Vector3(
					Math::deg_to_rad((double)rot[0]), Math::deg_to_rad((double)rot[1]), Math::deg_to_rad((double)rot[2])));
			xform.basis = xform.basis * extra;
		}
		if (pos.size() == 3)
		{
			xform.origin = Vector3((double)pos[0], (double)pos[1], (double)pos[2]);
		}
		cockpit_camera->set_transform(xform);
		if (fov > 0.0)
		{
			cockpit_camera->set_fov(fov);
		}
		UtilityFunctions::print("Cockpit camera positioned for '", aircraft_id, "': ", xform.origin);
	}
	if (camera_rig != nullptr)
	{
		// Head look composes on top of this transform; the rig resets Camera1
		// to its captured base every frame.
		camera_rig->capture_cockpit_base();
		camera_rig->set_cockpit_move_scale(cfg->get_float("model", "scale", 1.0));
		Vector3 eye;
		if (SettingsStore *st = settings_store())
		{
			const Variant v = st->get_setting("aircraft_" + aircraft_id + "_eye_offset");
			if (v.get_type() == Variant::VECTOR3)
			{
				eye = v;
			}
		}
		camera_rig->set_eye_offset(eye);
		camera_rig->set_orbit_distance_min(cfg->get_float("camera", "orbit_distance_min", 1.5));
		camera_rig->set_orbit_distance_max(cfg->get_float("camera", "orbit_distance_max", 50.0));
		camera_rig->set_orbit_distance(cfg->get_float("camera", "orbit_distance", 6.0));
	}
}

SettingsStore *AircraftRig::settings_store() const
{
	return host != nullptr ? Object::cast_to<SettingsStore>(host->get("_settings")) : nullptr;
}

void AircraftRig::save_eye_offset()
{
	SettingsStore *st = settings_store();
	if (st == nullptr || camera_rig == nullptr)
	{
		return;
	}
	const Vector3 eye = camera_rig->get_eye_offset();
	const String key = "aircraft_" + aircraft_id + "_eye_offset";
	if (eye.length_squared() < 1e-12)
	{
		st->get_settings().erase(key); // back at the cfg default: no entry (save() drops the stale key)
	}
	else
	{
		st->set_setting(key, eye);
	}
	st->save();
}

// ── Engine sound ────────────────────────────────────────────────────────────

void AircraftRig::setup_engine_sound(Node *parent)
{
	Ref<Resource> res = ResourceLoader::get_singleton()->load(engine_sound_path);
	Ref<AudioStream> stream = res;
	if (stream.is_null() || parent == nullptr)
	{
		UtilityFunctions::print("WARNING: engine sound not found at ", engine_sound_path);
		return;
	}
	Ref<AudioStreamOggVorbis> ogg = stream;
	if (ogg.is_valid())
	{
		ogg->set_loop(true);
	}
	Ref<AudioStreamMP3> mp3 = stream;
	if (mp3.is_valid())
	{
		mp3->set_loop(true);
	}
	engine_audio = memnew(AudioStreamPlayer);
	engine_audio->set_stream(stream);
	engine_audio->set_bus("Master");
	engine_audio->set_volume_db(-80.0); // start silent
	parent->add_child(engine_audio);
	engine_audio->play();
	engine_sound->register_nodes(engine_audio);
	UtilityFunctions::print("Engine sound loaded and playing (looped)");
}

AudioStreamPlayer *AircraftRig::get_engine_audio() const { return engine_audio; }

// ── Per-frame ───────────────────────────────────────────────────────────────

void AircraftRig::update(Object *p_jsb, double delta, double user_volume_pct)
{
	cockpit_controls->update(p_jsb, control_surfaces.ptr());
	if (p_jsb == nullptr)
	{
		return;
	}
	// wheels on the FDM's gear contacts: the control surfaces rewrite the
	// wheel transforms from their drawn pose, the strut shift goes on top
	gear_struts.undo_node_shifts();
	control_surfaces->update(p_jsb, delta);
	if (!gear_struts.empty())
	{
		gear_struts.apply_node_shifts(ac_node, Object::cast_to<JSBGodot>(p_jsb), gear_retractable ? control_surfaces->get_gear_visual_pos() : 1.0);
	}
	// the engine loop is driven from update_systems() (it runs during the
	// intro zoom too, with the same sounds_active gate as the FlightGear set)
}

void AircraftRig::silence_sounds()
{
	if (engine_audio != nullptr)
	{
		engine_audio->set_volume_db(-80.0);
	}
	if (fg_sounds != nullptr)
	{
		fg_sounds->silence();
	}
}

// ── Aircraft systems ────────────────────────────────────────────────────────

Ref<FGBridge> AircraftRig::get_fg_bridge() const { return fg_bridge; }
Ref<FGControls> AircraftRig::get_fg_controls() const { return fg_controls; }
Ref<FGPropertyRules> AircraftRig::get_fg_property_rules() const { return fg_rules; }
Ref<ItAutoflight> AircraftRig::get_autoflight() const { return autoflight; }
Ref<Ht9100Mcdu> AircraftRig::get_mcdu() const { return mcdu; }

void AircraftRig::setup_autoflight()
{
	autoflight.unref();
	// cfg [mcdu] ht9100: the HT9100 FMS / MCDU module (type / engines: the IDENT page)
	mcdu.unref();
	if (cfg.is_valid() && cfg->get_bool("mcdu", "ht9100", false))
	{
		mcdu.instantiate();
		mcdu->set_option("type", cfg->get_string("mcdu", "type", "DC-9-82"));
		mcdu->set_option("engines", cfg->get_string("mcdu", "engines", "JT8D-217A"));
	}
	if (cfg.is_null() || !cfg->get_bool("autoflight", "it_autoflight", false))
	{
		return;
	}
	autoflight.instantiate();
	autoflight->set_rules(fg_rules);
	autoflight->set_option("auto_arm_alt", cfg->get_float("autoflight", "auto_arm_alt", 1.0));
	autoflight->set_option("nav", cfg->get_float("autoflight", "nav", 0.0));
	UtilityFunctions::print("Autoflight (IT-AUTOFLIGHT DFGS) enabled for '", aircraft_id, "'");
}

void AircraftRig::_on_autoflight_action(const String &kind, double arg)
{
	if (kind.begins_with("mcdu."))
	{
		// "mcdu.<handler>:<text argument>" from the shim action "mcdu"
		const int colon = kind.find(":");
		if (mcdu.is_valid() && colon > 5)
		{
			mcdu->key(kind.substr(5, colon - 5), kind.substr(colon + 1));
		}
		return;
	}
	if (autoflight.is_valid() && !autoflight->panel(kind, arg))
	{
		UtilityFunctions::push_warning("Autoflight: unknown panel action '", kind, "'");
	}
}
FGSounds *AircraftRig::get_fg_sounds() const { return fg_sounds; }
FGAnimator *AircraftRig::get_fg_animator() const { return fg_animator; }
AircraftLights *AircraftRig::get_aircraft_lights() const { return aircraft_lights; }
bool AircraftRig::get_parking_brake() const { return parking_brake; }
void AircraftRig::set_parking_brake(bool on) { parking_brake = on; }

bool AircraftRig::has_fg_sounds() const
{
	return fg_sounds != nullptr && fg_sounds->get_count() > 0;
}

// Exterior lights (cfg [lights]) on the loaded model; rebuilt on every
// airframe swap because the fixtures live inside the model subtree.
void AircraftRig::setup_aircraft_lights()
{
	if (host == nullptr)
	{
		return;
	}
	if (aircraft_lights == nullptr)
	{
		aircraft_lights = memnew(AircraftLights);
		aircraft_lights->set_name("AircraftLights");
		host->add_child(aircraft_lights);
	}
	aircraft_lights->setup(get_model_root(), cfg, ac_node);
	// lights driven by the aircraft's own properties ([lights] <kind>_props, FDM
	// paths; the MD-80's exterior-light levels and cockpit flood knobs) and
	// the switch properties the L key sets (landing_switch_props)
	landing_props = cfg->get_string_array("lights", "landing_props", Array());
	flood_props = cfg->get_string_array("lights", "flood_props", Array());
	landing_ext_props = cfg->get_string_array("lights", "landing_ext_props", Array());
	nav_prop = cfg->get_string("lights", "nav_prop", "").strip_edges();
	strobe_prop = cfg->get_string("lights", "strobe_prop", "").strip_edges();
	beacon_prop = cfg->get_string("lights", "beacon_prop", "").strip_edges();
	landing_switch_props = cfg->get_string_array("lights", "landing_switch_props", Array());
	landing_switch_on = cfg->get_float("lights", "landing_switch_on", 1.0);
	landing_switch_off = cfg->get_float("lights", "landing_switch_off", 0.0);
	if (aircraft_lights->fixture_count() > 0)
	{
		UtilityFunctions::print("Aircraft lights: ", aircraft_lights->fixture_count(), " fixtures (nav ",
				aircraft_lights->fixture_count("nav_left") + aircraft_lights->fixture_count("nav_right") + aircraft_lights->fixture_count("nav_tail"),
				", strobe ", aircraft_lights->fixture_count("strobe"), ", beacon ", aircraft_lights->fixture_count("beacon"),
				", landing ", aircraft_lights->fixture_count("landing"), ", taxi ", aircraft_lights->fixture_count("taxi"), ")");
	}
}

// FlightGear animation manifest (cfg [model] fg_animations) run by the
// FGAnimator on the loaded model; the bridge feeds it per frame and the
// listed cockpit parts become clickable (FGControls).
void AircraftRig::setup_fg_animations()
{
	if (host == nullptr)
	{
		return;
	}
	if (fg_animator == nullptr)
	{
		fg_animator = memnew(FGAnimator);
		fg_animator->set_name("FGAnimator");
		host->add_child(fg_animator);
	}
	const String rel = cfg.is_valid() ? cfg->get_string("model", "fg_animations", "") : String();
	if (rel.is_empty())
	{
		fg_animator->clear();
		fg_controls->setup("", nullptr, nullptr);
		return;
	}
	const String path = AircraftConfig::resolve_aircraft_file(aircraft_id, rel);
	Node *model_root = get_model_root();
	// texture-prop files (liveries, gauge faces): [model] textures_dir, the base
	// aircraft's folder for a variant without its own
	fg_animator->set_texture_root(AircraftConfig::resolve_aircraft_file(aircraft_id, cfg->get_string("model", "textures_dir", "model/liveries")));
	if (fg_animator->load(path, model_root))
	{
		UtilityFunctions::print("FlightGear animations loaded for '", aircraft_id, "': ", fg_animator->stats());
		const String screens_rel = cfg->get_string("model", "screen_texts", "");
		if (!screens_rel.is_empty())
		{
			const int made = fg_animator->load_screen_texts(AircraftConfig::resolve_aircraft_file(aircraft_id, screens_rel));
			UtilityFunctions::print("Screen texts for '", aircraft_id, "': ", made, " labels from ", screens_rel);
		}
		fg_controls->set_state(Dictionary());
		fg_seeded = false;
		fg_controls->set_on_change(Callable(this, "_on_fg_control"));
		fg_controls->set_on_module_action(Callable(this, "_on_autoflight_action"));
		// cfg [model] nasal_shims: the aircraft's Nasal call -> property action table.
		// Loaded before setup(): setup keeps only records with a binding it can
		// run, and a shim is what makes a Nasal binding runnable. An empty path
		// clears the previous aircraft's shims.
		fg_controls->set_knob_actions(cfg->get_string_array("model", "knob_actions", Array()));
		const String shims_rel = cfg->get_string("model", "nasal_shims", "");
		const int n = fg_controls->load_shims(shims_rel.is_empty() ? String() : AircraftConfig::resolve_aircraft_file(aircraft_id, shims_rel));
		if (!shims_rel.is_empty())
		{
			UtilityFunctions::print("Nasal shims for '", aircraft_id, "': ", n);
		}
		UtilityFunctions::print("FlightGear cockpit controls: ", fg_controls->setup(path, model_root, fg_animator), " clickable parts");
		setup_liveries();
	}
	else
	{
		UtilityFunctions::push_warning("FlightGear animations not loaded from ", path);
		fg_controls->setup("", nullptr, nullptr);
	}
}

// cfg [sound] fg_sounds: a FlightGear sound set played by FGSounds from the
// bridge's property values; the basic engine loop stays silent.
void AircraftRig::setup_fg_sounds()
{
	if (host == nullptr)
	{
		return;
	}
	if (fg_sounds == nullptr)
	{
		fg_sounds = memnew(FGSounds);
		fg_sounds->set_name("FGSounds");
		host->add_child(fg_sounds);
	}
	fg_sounds->clear();
	const String rel = cfg.is_valid() ? cfg->get_string("sound", "fg_sounds", "") : String();
	if (rel.is_empty())
	{
		return;
	}
	const String path = AircraftConfig::resolve_aircraft_file(aircraft_id, rel);
	const int n = fg_sounds->load(path, path.get_base_dir());
	// [sound] aliases "a = b, ...": a reads b's value when nothing writes a
	Dictionary aliases;
	const Array al = cfg->get_string_array("sound", "aliases", Array());
	for (int i = 0; i < al.size(); i++)
	{
		const PackedStringArray kv = String(al[i]).split("=");
		if (kv.size() == 2)
		{
			aliases[kv[0].strip_edges()] = kv[1].strip_edges();
		}
	}
	fg_sounds->set_aliases(aliases);
	UtilityFunctions::print("FlightGear sounds for '", aircraft_id, "': ", n, " loaded, ", fg_sounds->get_skipped().size(), " without a file");
}

// cfg [jsbsim] property_rules "a.xml, b.xml": FlightGear property-rule files
// (the <autopilot><path> entries of the -set file) run by FGPropertyRules on
// the bridge dictionary every frame, after collect() and before the animator
// and the sounds read it; outputs the FDM has a node for go into the FDM.
void AircraftRig::setup_liveries()
{
	livery_defaults.clear();
	livery_list.clear();
	livery_props = PackedStringArray();
	livery_id = String();
	if (cfg.is_null() || cfg->get_string("liveries", "catalog", "").is_empty())
	{
		return;
	}
	const Dictionary catalog = AircraftSettingsPanel::livery_catalog(cfg, aircraft_id);
	livery_defaults = catalog.get("defaults", Dictionary());
	livery_list = catalog.get("liveries", Array());
	for (int i = 0; i < livery_list.size(); i++)
	{
		const Dictionary tex = Dictionary(livery_list[i]).get("textures", Dictionary());
		const Array keys = tex.keys();
		for (int k = 0; k < keys.size(); k++)
		{
			if (!livery_props.has(keys[k]))
			{
				livery_props.push_back(keys[k]);
			}
		}
	}
	SettingsStore *store = settings_store();
	const String key = AircraftSettingsPanel::livery_key(aircraft_id);
	if (store != nullptr && store->has_setting(key))
	{
		livery_id = store->get_setting(key);
	}
	apply_livery_props();
	UtilityFunctions::print("Liveries for '", aircraft_id, "': ", livery_list.size(), " in the catalog, ", livery_defaults.size(),
			" variant textures, selected '", livery_id.is_empty() ? String("as built") : livery_id, "'");
}

// The texture properties as the state the bridge publishes every frame: the
// variant's defaults, then the chosen livery's files; a property no livery
// sets any more goes back to "" (FGAnimator restores the glb's texture).
void AircraftRig::apply_livery_props()
{
	if (fg_controls.is_null() || fg_animator == nullptr)
	{
		return;
	}
	Dictionary vals;
	Array keys = livery_defaults.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		vals[keys[i]] = livery_defaults[keys[i]];
	}
	for (int i = 0; i < livery_props.size(); i++)
	{
		if (!vals.has(livery_props[i]))
		{
			vals[livery_props[i]] = String();
		}
	}
	for (int i = 0; i < livery_list.size(); i++)
	{
		const Dictionary l = livery_list[i];
		if (!livery_id.is_empty() && String(l.get("id", "")) == livery_id)
		{
			const Dictionary tex = l.get("textures", Dictionary());
			const Array tk = tex.keys();
			for (int k = 0; k < tk.size(); k++)
			{
				vals[tk[k]] = tex[tk[k]];
			}
			break;
		}
	}
	if (vals.is_empty())
	{
		return;
	}
	Dictionary state = fg_controls->get_state();
	keys = vals.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		state[keys[i]] = vals[keys[i]];
	}
	fg_controls->set_state(state);
	fg_animator->set_properties(vals);
}

void AircraftRig::set_livery(const String &p_livery_id)
{
	livery_id = p_livery_id;
	apply_livery_props();
}

String AircraftRig::get_livery() const { return livery_id; }
Array AircraftRig::get_liveries() const { return livery_list; }

void AircraftRig::setup_fg_property_rules()
{
	if (fg_rules.is_null())
	{
		fg_rules.instantiate();
	}
	fg_rules->clear();
	rule_fdm_nodes.clear();
	const Array files = cfg.is_valid() ? cfg->get_string_array("jsbsim", "property_rules", Array()) : Array();
	for (int i = 0; i < files.size(); i++)
	{
		const String rel = String(files[i]).strip_edges();
		if (rel.is_empty())
		{
			continue;
		}
		const String path = AircraftConfig::resolve_aircraft_file(aircraft_id, rel);
		const int n = fg_rules->load(path);
		if (n < 0)
		{
			UtilityFunctions::push_warning("FlightGear property rules not loaded from ", path);
			continue;
		}
		UtilityFunctions::print("FlightGear property rules for '", aircraft_id, "': ", n, " components from ", rel);
	}
	if (!fg_rules->unsupported().is_empty())
	{
		UtilityFunctions::print("FlightGear property rules: components not run: ", fg_rules->unsupported());
	}
}

void AircraftRig::update_systems(Object *p_jsb, double delta, Camera3D *rendering_cam, Camera3D *pick_camera, double night,
		double sim_time_seconds, bool sounds_active, double user_volume_pct)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr || ac_node == nullptr)
	{
		return;
	}
	last_jsb = p_jsb;
	// Basic engine loop (aircraft without a FlightGear sound set): follows the
	// visual propeller rpm while sounds are active, silent otherwise and for
	// aircraft that play their own engines. Loading / welcome: everything silent.
	if (engine_audio != nullptr)
	{
		if (sounds_active && !has_fg_sounds())
		{
			engine_sound->update(control_surfaces->get_visual_rpm(), user_volume_pct);
		}
		else
		{
			engine_audio->set_volume_db(-80.0);
		}
	}
	if (!sounds_active && has_fg_sounds())
	{
		fg_sounds->silence();
	}
	const bool gear_down = !gear_retractable || fdm->get_gear_pos() > 0.05;
	const bool airborne = !fdm->get_weight_on_wheels();
	if (aircraft_lights != nullptr && aircraft_lights->fixture_count() > 0)
	{
		const Basis acb = ac_node->get_global_transform().basis;
		// AC frame: nose along +Z
		aircraft_lights->update(delta, airborne, gear_down, night, acb.get_column(2).normalized(), -acb.get_column(1).normalized());
	}
	if (fg_animator == nullptr || fg_animator->animation_count() == 0)
	{
		push_nav_text(String());
		return;
	}
	// fixture levels from the FDM (exterior-light extension / dimming, flood knobs)
	if (aircraft_lights != nullptr && fdm != nullptr)
	{
		for (int i = 0; i < landing_props.size(); i++)
		{
			const String pr = String(landing_props[i]).strip_edges();
			double level = fdm->has_property(pr) ? fdm->get_property(pr) : 0.0;
			// a retractable lamp: the beam follows its extension, and it burns while it
			// swings out with its switch at on (the FDM lights it only fully out, from dark)
			const String ep = i < landing_ext_props.size() ? String(landing_ext_props[i]).strip_edges() : String();
			if (!ep.is_empty() && ep != "-" && fdm->has_property(ep))
			{
				const double ext = fdm->get_property(ep);
				aircraft_lights->set_fixture_extension("landing", i, ext);
				const String sw = i < landing_switch_props.size() ? String("/") + String(landing_switch_props[i]).strip_edges().trim_prefix("/") : String();
				if (ext > 0.02 && !sw.is_empty() && fdm->has_property(sw) && fdm->get_property(sw) >= landing_switch_on - 1e-6)
				{
					level = Math::max(level, 1.0);
				}
			}
			aircraft_lights->set_fixture_level("landing", i, level);
		}
		for (int i = 0; i < flood_props.size(); i++)
		{
			const String pr = String(flood_props[i]).strip_edges();
			aircraft_lights->set_fixture_level("flood", i, fdm->has_property(pr) ? fdm->get_property(pr) : 0.0);
		}
		// position / strobe / beacon lights switched by the aircraft's own systems
		// (the MD-80's POS/STROBE switch, bus voltage and nose wheel logic)
		if (!nav_prop.is_empty())
		{
			aircraft_lights->set_nav_enabled(fdm->has_property(nav_prop) && fdm->get_property(nav_prop) > 0.5);
		}
		if (!strobe_prop.is_empty())
		{
			aircraft_lights->set_strobe_enabled(fdm->has_property(strobe_prop) && fdm->get_property(strobe_prop) > 0.5);
		}
		if (!beacon_prop.is_empty())
		{
			aircraft_lights->set_beacon_enabled(fdm->has_property(beacon_prop) && fdm->get_property(beacon_prop) > 0.5);
		}
	}
	const bool landing_on = aircraft_lights != nullptr && aircraft_lights->landing_lights_on() && gear_down;
	fg_bridge->set_landing_light(landing_on);
	fg_bridge->set_taxi_light(landing_on);
	// "internal" follows the camera that renders (the intro zoom and the
	// external cameras are outside even while the mode says cockpit)
	fg_bridge->set_cockpit_view(rendering_cam != nullptr && rendering_cam == cockpit_camera);
	if (camera_rig != nullptr && camera_rig->take_eye_offset_settled())
	{
		save_eye_offset();
	}
	fg_bridge->set_sim_time_seconds(sim_time_seconds);
	sync_parking_brake(p_jsb);
	fg_controls->tick(delta);
	fg_bridge->set_control_state(fg_controls->get_state());
	fg_bridge->advance_canopy(delta);
	fg_bridge->set_frame_delta(delta);
	if (autoflight.is_valid())
	{
		// last frame's autoflight state reaches the FDM mirror with this collect
		fg_bridge->set_extra_properties(autoflight->get_published());
	}
	if (!gear_struts.empty())
	{
		gear_struts.update_fg(ac_node, fdm, gear_retractable ? fdm->get_gear_pos() : 1.0);
		for (int i = 0; i < gear_struts.unit_count(); i++)
		{
			if (gear_struts.is_fg(i))
			{
				fg_bridge->set_compression_norm(gear_struts.fdm_unit(i), gear_struts.norm(i));
			}
		}
	}
	Dictionary props = fg_bridge->collect(p_jsb, airborne, night);
	if (mcdu.is_valid())
	{
		mcdu->update(props, delta, fdm->get_latitude_deg(), fdm->get_longitude_deg());
		const Dictionary pub = mcdu->get_published();
		const Array pk = pub.keys();
		for (int i = 0; i < pk.size(); i++)
		{
			props[pk[i]] = pub[pk[i]];
		}
	}
	if (autoflight.is_valid())
	{
		// the mode logic reads the flight state and writes its inputs / outputs
		// before the control laws (the rules) run on them
		autoflight->update(props, delta);
		const Dictionary pub = autoflight->get_published();
		const Array pk = pub.keys();
		for (int i = 0; i < pk.size(); i++)
		{
			props[pk[i]] = pub[pk[i]];
		}
		const Dictionary cw = autoflight->take_control_writes();
		const Array ck = cw.keys();
		for (int i = 0; i < ck.size(); i++)
		{
			fg_controls->assign_external(ck[i], cw[ck[i]]);
			props[ck[i]] = cw[ck[i]];
		}
		if (fg_rules.is_valid())
		{
			const Dictionary rw = autoflight->take_rule_writes();
			const Array rk = rw.keys();
			for (int i = 0; i < rk.size(); i++)
			{
				fg_rules->set_value(rk[i], (double)rw[rk[i]]);
			}
		}
		const Dictionary fw = autoflight->take_fdm_writes();
		const Array fk = fw.keys();
		for (int i = 0; i < fk.size(); i++)
		{
			if (fdm->has_property(String("/") + String(fk[i])))
			{
				fdm->set_property(String("/") + String(fk[i]), (double)fw[fk[i]]);
			}
		}
	}
	if (fg_rules.is_valid() && fg_rules->get_component_count() > 0)
	{
		fg_rules->apply(props, delta);
		const PackedStringArray outs = fg_rules->output_names();
		for (int i = 0; i < outs.size(); i++)
		{
			const String &k = outs[i];
			if (!rule_fdm_nodes.has(k))
			{
				rule_fdm_nodes[k] = fdm->has_property(String("/") + k);
			}
			if ((bool)rule_fdm_nodes[k] && props.has(k))
			{
				fdm->set_property(String("/") + k, (double)props[k]);
			}
		}
		if (autoflight.is_valid())
		{
			autoflight->absorb(props);
			// the servos' levers: the autotrim moves the pitch trim command, the
			// autothrottle backdrives the thrust levers (the FDM's engines already
			// follow systems/engines/ats-cmd-n while the A/T drives)
			if (autoflight->is_engaged() && fg_rules->wrote("controls/flight/elevator-trim"))
			{
				fdm->set_pitch_trim((double)props["controls/flight/elevator-trim"]);
			}
			if (autoflight->is_athr_driving() && fg_rules->wrote("controls/engines/engine/throttle"))
			{
				fdm->set_input_throttle((float)(double)props["controls/engines/engine/throttle"]);
			}
		}
	}
	fg_animator->set_properties(props);
	push_nav_text(fg_bridge->nav_status_text());
	if (aircraft_lights != nullptr)
	{
		aircraft_lights->set_powered(fg_bridge->get_dc_volts() > 0.0);
	}
	if (has_fg_sounds() && sounds_active)
	{
		fg_sounds->set_user_volume(user_volume_pct / 100.0);
		const bool inside = fg_bridge->get_cockpit_view() || rendering_cam == nullptr;
		fg_sounds->set_camera_distance_m(inside ? 0.0 : (rendering_cam->get_global_position() - ac_node->get_global_position()).length());
		fg_sounds->update(props, delta);
	}
	// properties only the cockpit switches write: seed their defaults once
	if (!fg_seeded)
	{
		fg_seeded = true;
		Dictionary state = fg_controls->get_state();
		const PackedStringArray unknown = fg_animator->unknown_properties();
		for (int i = 0; i < unknown.size(); i++)
		{
			state[unknown[i]] = fg_bridge->default_for(unknown[i]);
		}
		// properties only the clickable controls act on (a switch no
		// animation reads): start from the aircraft's init list or the FDM
		const PackedStringArray bound = fg_controls->binding_properties();
		for (int i = 0; i < bound.size(); i++)
		{
			// only properties nothing else provides: a value the bridge or
			// the FDM mirror already publishes (throttle, flap lever) must
			// not be frozen into the switch state
			if (!state.has(bound[i]) && fg_animator->get_property(bound[i]).get_type() == Variant::NIL)
			{
				const Variant v = fg_bridge->initial_value(bound[i], p_jsb);
				if (v.get_type() != Variant::NIL)
				{
					state[bound[i]] = v;
				}
			}
		}
		const Dictionary defaults = FGBridge::switch_defaults();
		const Array keys = defaults.keys();
		for (int i = 0; i < keys.size(); i++)
		{
			if (!state.has(keys[i]))
			{
				state[keys[i]] = defaults[keys[i]];
			}
		}
		fg_controls->set_state(state);
		fg_animator->set_properties(state);
	}
	fg_animator->update(delta);
	// pointing hand over a clickable cockpit part
	if (pick_camera != nullptr && fg_controls->get_count() > 0 && host != nullptr && host->is_inside_tree())
	{
		const String over = fg_controls->hover_name(pick_camera, host->get_viewport()->get_mouse_position());
		Input::get_singleton()->set_default_cursor_shape(over.is_empty() ? Input::CURSOR_ARROW : Input::CURSOR_POINTING_HAND);
	}
	last_fg_props = props;
}

// cfg [jsbsim] init_properties: a JSON file {"property": value, ...} of FDM
// property values set after the model loads and on every Fly! — the cockpit
// switch state an aircraft whose systems live in JSBSim starts with (the
// MD-80's "after start" panel). [jsbsim] fg_property_mirror turns on the
// bridge's two-way property mirror; [electrical] power_property names the
// bus that powers the panel.
void AircraftRig::apply_fdm_init_properties(JSBGodot *fdm)
{
	if (cfg.is_null() || fdm == nullptr)
	{
		return;
	}
	fg_bridge->set_fdm_mirror(cfg->get_bool("jsbsim", "fg_property_mirror", false));
	fg_bridge->set_power_property(cfg->get_string("electrical", "power_property", ""));
	fg_bridge->set_power_threshold(cfg->get_float("electrical", "power_threshold", 24.0));
	fg_bridge->set_nose_steer_max_deg(cfg->get_float("jsbsim", "nose_steer_max_deg", 20.0));
	fdm->set_default_pitch_trim(cfg->get_float("jsbsim", "pitch_trim_default", 0.0));
	fdm->set_origin_forward_m(cfg->get_float("jsbsim", "origin_forward_m", 0.0));
	fdm->set_mac_reference(cfg->get_float("performance", "mac_le_x_in", 0.0), cfg->get_float("performance", "mac_in", 0.0));
	// an FDM without a gear kinematic (the MD-80's gear ran in Nasal): PVP moves gear-pos-norm
	fdm->set_gear_transit(cfg->get_bool("gear", "fdm_position", true) ? 0.0 : cfg->get_float("gear", "retract_seconds", 10.0));
	fg_bridge->set_surface_norm_per_deg(cfg->get_float("animation", "elevator_pos_norm_per_deg", 0.0), cfg->get_float("animation", "rudder_pos_norm_per_deg", 0.0));
	fg_bridge->reset_mirror_cache();
	// per-aircraft control override (Settings > Aircraft > Controls; cfg [controls]
	// throttle_drives gives the default): a glider's throttle inputs work the airbrakes
	fdm->set_throttle_drives_airbrakes(AircraftSettingsPanel::throttle_drives_airbrakes(settings_store(), cfg, aircraft_id));
	// readout units ([instruments] units "metric": km/h, m, m/s)
	if (FlightHud *hud = Object::cast_to<FlightHud>(host != nullptr ? host->get("_hud") : Variant()))
	{
		hud->set_metric(cfg->get_string("instruments", "units", "aviation").to_lower() == "metric");
	}
	// one JSON file, or a comma-separated list merged in order (a variant
	// aircraft adds its few values to the base aircraft's list)
	const Array rels = cfg->get_string_array("jsbsim", "init_properties", Array());
	if (rels.is_empty())
	{
		return;
	}
	Dictionary d;
	for (int r = 0; r < rels.size(); r++)
	{
		const String path = AircraftConfig::resolve_aircraft_file(aircraft_id, rels[r]);
		Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
		if (f.is_null())
		{
			UtilityFunctions::print("WARNING: init_properties not found at ", path);
			continue;
		}
		const Variant data = JSON::parse_string(f->get_as_text());
		if (data.get_type() != Variant::DICTIONARY)
		{
			UtilityFunctions::print("WARNING: init_properties is not a JSON object: ", path);
			continue;
		}
		d.merge(Dictionary(data), true);
	}
	fg_bridge->set_init_defaults(d);
	// "_links" {dst: src} and "_pulse" {prop: seconds} are the FDM's per-step
	// helpers (JSBGodot::apply_property_links_and_pulses); cleared for an
	// aircraft that has none
	const Variant links = d.get("_links", Variant());
	fdm->set_property_links(links.get_type() == Variant::DICTIONARY ? Dictionary(links) : Dictionary());
	const Variant pulses = d.get("_pulse", Variant());
	const Array keys = d.keys();
	int n = 0;
	for (int i = 0; i < keys.size(); i++)
	{
		const Variant v = d[keys[i]];
		if (v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT || v.get_type() == Variant::BOOL)
		{
			// a leading slash is an absolute (FlightGear-side) path; JSBSim's own live under fdm/jsbsim
			fdm->set_property(String(keys[i]), (double)v);
			n++;
		}
	}
	// the pulses start after the plain values (they override some of them for a while)
	fdm->set_init_pulses(pulses.get_type() == Variant::DICTIONARY ? Dictionary(pulses) : Dictionary());
	UtilityFunctions::print("FDM init properties for '", aircraft_id, "': ", n, " set from ", String(", ").join(PackedStringArray(rels)));
}

// The HUD's radio navigation line (host $Control HUD, FlightHud).
void AircraftRig::push_nav_text(const String &text)
{
	if (host == nullptr)
	{
		return;
	}
	FlightHud *hud = Object::cast_to<FlightHud>(host->get("_hud"));
	if (hud != nullptr)
	{
		hud->set_nav_text(text);
	}
}

void AircraftRig::command_speedbrake(JSBGodot *fdm, bool out, bool notch)
{
	if (fdm == nullptr)
	{
		return;
	}
	if (!speedbrake_prop.is_empty())
	{
		// the FDM's own lever: the key goes all the way (the FDM limits the travel
		// in the air), a lever click moves one notch
		double v = out ? speedbrake_key_value : 0.0;
		if (notch && speedbrake_step > 0.0)
		{
			const double cur = fdm->get_property(speedbrake_prop);
			v = Math::clamp(cur + (out ? speedbrake_step : -speedbrake_step), 0.0, speedbrake_key_value);
		}
		fdm->set_property(speedbrake_prop, v);
		if (fg_controls.is_valid())
		{
			// the property mirror pushes controls/* from the cockpit state every frame
			fg_controls->assign_external(speedbrake_prop.trim_prefix("/"), v);
		}
		speedbrake_out = v > 0.01;
		return;
	}
	speedbrake_out = out;
	if (fdm->get_throttle_drives_airbrakes())
	{
		fdm->set_input_throttle(out ? 0.0f : 1.0f); // the lever the throttle inputs work
	}
	else
	{
		fdm->set_property("fcs/speedbrake-cmd-norm", out ? 1.0 : 0.0);
	}
}

void AircraftRig::poll_keys(Object *p_jsb)
{
	// Keys by the keyboard mapping (Settings > Keyboard, hotspots_debug / speedbrake_toggle / landing_lights)
	// hotspots: show every cockpit mouse-sense area (FlightGear controls)
	if (KeyBindings::pressed("hotspots_debug"))
	{
		if (!hotspot_key_held)
		{
			hotspot_key_held = true;
			if (fg_controls->get_count() > 0)
			{
				fg_controls->set_debug(!fg_controls->get_debug());
				UtilityFunctions::print("Cockpit hotspots: ", fg_controls->get_debug() ? "SHOWN" : "HIDDEN");
			}
		}
	}
	else
	{
		hotspot_key_held = false;
	}
	// Speed brakes with B (fcs/speedbrake-cmd-norm 0/1; the A-10 splits its ailerons)
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (KeyBindings::pressed("speedbrake_toggle"))
	{
		if (!speedbrake_key_held)
		{
			speedbrake_key_held = true;
			if (fdm != nullptr)
			{
				command_speedbrake(fdm, !speedbrake_out, false);
				UtilityFunctions::print("Speed brakes: ", speedbrake_out ? "OUT" : "IN");
			}
		}
	}
	else
	{
		speedbrake_key_held = false;
	}
	// Reverse thrust (Settings > Keyboard reverse_toggle; the pad button goes the same way)
	if (KeyBindings::pressed("reverse_toggle"))
	{
		if (!reverse_key_held)
		{
			reverse_key_held = true;
			toggle_reverse_thrust(fdm);
		}
	}
	else
	{
		reverse_key_held = false;
	}
	// Landing / taxi lights
	if (KeyBindings::pressed("landing_lights"))
	{
		if (!landing_key_held)
		{
			landing_key_held = true;
			if (aircraft_lights != nullptr)
			{
				const bool on = aircraft_lights->toggle_landing_lights();
				UtilityFunctions::print("Landing lights: ", on ? "ON" : "OFF");
				if (fg_controls->get_count() > 0 && landing_switch_props.size() > 0)
				{
					// the cockpit switches (the FDM's systems light the fixtures from them)
					Dictionary st = fg_controls->get_state();
					for (int i = 0; i < landing_switch_props.size(); i++)
					{
						st[String(landing_switch_props[i]).strip_edges()] = on ? landing_switch_on : landing_switch_off;
					}
				}
				if (fg_controls->get_count() > 0)
				{
					Dictionary state = fg_controls->get_state();
					state["sim/model/A-10/controls/lighting/land-lights-switch"] = on ? 2.0 : 1.0;
					if (state.has("controls/lighting/landing-lights"))
					{
						state["controls/lighting/landing-lights"] = on ? 1.0 : 0.0;
					}
				}
			}
		}
	}
	else
	{
		landing_key_held = false;
	}
}

// FlightGear's systems.toggleRevThrust() (MD-80 engines.nas): with a main
// gear on the ground and both levers at idle the reversers toggle (stowing
// them also idles the levers); airborne or with power on, the call only
// stows them and idles the levers. The FDM's Reversers channel swings the
// buckets from the flags and turns the throttle input into reverse power,
// the DFGS's athr-avail drops when a bucket moves (the ATS lets go).
bool AircraftRig::toggle_reverse_thrust(Object *p_jsb)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr || reverse_engage_props.is_empty())
	{
		return false;
	}
	bool engaged = false;
	for (int i = 0; i < reverse_engage_props.size(); i++)
	{
		engaged = engaged || fdm->get_property(String(reverse_engage_props[i])) > 0.5;
	}
	const double lever = reverse_lever_prop.is_empty() ? fdm->get_input_throttle() : fdm->get_property(reverse_lever_prop);
	const bool may_toggle = fdm->get_weight_on_wheels() && lever <= 0.05;
	const bool next = may_toggle && !engaged;
	if (!next)
	{
		fdm->set_input_throttle(0.0f);
	}
	for (int i = 0; i < reverse_engage_props.size(); i++)
	{
		fdm->set_property(String(reverse_engage_props[i]), next ? 1.0 : 0.0);
	}
	if (next != engaged)
	{
		UtilityFunctions::print("Reverse thrust: ", next ? "ENGAGED (the throttle commands reverse power)" : "STOWED");
	}
	else if (!may_toggle && !engaged)
	{
		UtilityFunctions::print("Reverse thrust: only on the ground with the levers at idle");
	}
	reverse_engaged = next;
	return true;
}

bool AircraftRig::is_reverse_engaged() const { return reverse_engaged; }

void AircraftRig::set_parking_brake_at_start(Object *p_jsb)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr)
	{
		return;
	}
	fdm->set_input_brake(1.0);
	parking_brake = true;
	if (fg_controls->get_count() > 0)
	{
		Dictionary state = fg_controls->get_state();
		state["controls/gear/brake-parking"] = 1.0;
	}
	UtilityFunctions::print("Parking brake: SET at flight start (release with ,)");
}

// Keep the cockpit handle (FlightGear control) in step with the brake keys.
void AircraftRig::sync_parking_brake(Object *p_jsb)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr || fg_controls->get_count() == 0)
	{
		return;
	}
	const bool braked = fdm->get_input_brake() > 0.5;
	if (braked != parking_brake)
	{
		parking_brake = braked;
		Dictionary state = fg_controls->get_state();
		state["controls/gear/brake-parking"] = braked ? 1.0 : 0.0;
	}
}

void AircraftRig::_on_fg_control(const String &prop, const Variant &value)
{
	const double v = (double)value;
	const bool on = v > 0.5;
	JSBGodot *fdm = Object::cast_to<JSBGodot>(last_jsb);
	if (prop == "controls/gear/brake-parking")
	{
		if (fdm != nullptr)
		{
			parking_brake = on;
			fdm->set_input_brake(on ? 1.0 : 0.0);
			UtilityFunctions::print("Parking brake: ", on ? "SET" : "RELEASED");
		}
	}
	else if (prop == "controls/engines/engine/throttle" || (prop.begins_with("controls/engines/engine[") && prop.ends_with("/throttle")))
	{
		// a thrust lever dragged with the mouse (the glider's airbrake lever
		// in FlightGear: 1 = brakes in, 0 = fully out). Either lever of a twin
		// moves the one throttle input, so both engines follow; the lever's
		// state entry goes, the next drag starts from the FDM's live value
		if (fdm != nullptr)
		{
			fdm->set_input_throttle((float)Math::clamp(v, 0.0, 1.0));
		}
		Dictionary state = fg_controls->get_state();
		state.erase(prop);
	}
	else if (prop == "controls/flight/spoilers")
	{
		// the airbrake lever (FlightGear property-adjust 0..1): the FDM's speedbrake
		// command, through the throttle input when that drives the airbrakes
		if (fdm != nullptr)
		{
			const double sb = Math::clamp(v, 0.0, 1.0);
			if (fdm->get_throttle_drives_airbrakes())
			{
				fdm->set_input_throttle(1.0f - (float)sb);
			}
			else
			{
				fdm->set_property("fcs/speedbrake-cmd-norm", sb);
			}
			speedbrake_out = sb > 0.5;
		}
	}
	else if (prop == "sim/model/A-10/controls/lighting/land-lights-switch")
	{
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_landing_lights(v != 1.0); // 0 taxi / 1 off / 2 landing
		}
	}
	else if (prop == "sim/model/A-10/controls/lighting/nav-lights-switch" || prop == "controls/lighting/nav-lights")
	{
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_nav_enabled(on);
		}
	}
	else if (prop == "controls/lighting/strobe")
	{
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_strobe_enabled(on);
			// the A-10's one switch covers the beacon too; a model with its
			// own beacon switch (Cessna) keeps them apart
			if (fg_animator == nullptr || !fg_animator->property_names().has("controls/lighting/beacon"))
			{
				aircraft_lights->set_beacon_enabled(on);
			}
		}
	}
	else if (prop == "controls/lighting/beacon")
	{
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_beacon_enabled(on);
		}
	}
	else if (prop == "controls/lighting/landing-lights")
	{
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_landing_lights(on);
			UtilityFunctions::print("Landing lights: ", on ? "ON" : "OFF");
		}
	}
	else if (landing_switch_props.has(prop))
	{
		// one of the aircraft's landing light switches: lit when any is at its on value
		bool any = false;
		for (int i = 0; i < landing_switch_props.size(); i++)
		{
			any = any || fg_controls->value_of(String(landing_switch_props[i]).strip_edges()) >= landing_switch_on - 1e-6;
		}
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_landing_lights(any);
		}
	}
	else if (prop == "controls/lighting/taxi-light")
	{
		if (aircraft_lights != nullptr)
		{
			aircraft_lights->set_taxi_enabled(on);
		}
	}
	else if (prop == "sim/model/A-10/controls/canopy/canopy-switch")
	{
		fg_bridge->set_canopy_target(v);
	}
	else if (prop == "provpilot/gear-cmd")
	{
		Dictionary state = fg_controls->get_state();
		state.erase(prop);
		if (fdm != nullptr && gear_retractable)
		{
			cockpit_controls->set_gear_down(on);
			control_surfaces->set_gear_down(on);
			fdm->set_gear_down(on);
			UtilityFunctions::print("Gear (handle): ", on ? "DOWN" : "UP");
		}
	}
	else if (prop == "provpilot/flaps-step")
	{
		Dictionary state = fg_controls->get_state();
		state.erase(prop);
		if (fdm != nullptr && cockpit_controls.is_valid())
		{
			cockpit_controls->step_flaps(v >= 0.0 ? 1 : -1, fdm);
		}
	}
	else if (prop == "provpilot/speedbrake")
	{
		Dictionary state = fg_controls->get_state();
		state.erase(prop);
		if (fdm != nullptr)
		{
			command_speedbrake(fdm, on, true);
		}
	}
	else if (prop == "provpilot/gear-toggle")
	{
		// the gear handle (ProvPilot-defined pick): same path as the G key
		Dictionary state = fg_controls->get_state();
		state.erase(prop);
		if (fdm != nullptr && gear_retractable)
		{
			const bool down = fdm->get_gear_cmd() < 0.5;
			cockpit_controls->set_gear_down(down);
			control_surfaces->set_gear_down(down);
			fdm->set_gear_down(down);
			UtilityFunctions::print("Gear (handle): ", down ? "DOWN" : "UP");
		}
	}
	else if (prop == "sim/model/A-10/instrumentation/g-meter/g-max" || prop == "sim/model/A-10/instrumentation/g-meter/g-min")
	{
		fg_bridge->set_g_max(1.0);
		fg_bridge->set_g_min(1.0);
		Dictionary state = fg_controls->get_state();
		state.erase(prop);
	}
}

Ref<ControlSurfaces> AircraftRig::get_control_surfaces() const { return control_surfaces; }
Ref<CockpitControls> AircraftRig::get_cockpit_controls() const { return cockpit_controls; }
Ref<EngineSound> AircraftRig::get_engine_sound() const { return engine_sound; }

} // namespace jsbgodot
