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
	ClassDB::bind_method(D_METHOD("resolve_nodes", "jsb"), &AircraftRig::resolve_nodes);
	ClassDB::bind_method(D_METHOD("apply_camera_config"), &AircraftRig::apply_camera_config);
	ClassDB::bind_method(D_METHOD("apply_nose_steer_range", "jsb"), &AircraftRig::apply_nose_steer_range);

	ClassDB::bind_method(D_METHOD("setup_engine_sound", "parent"), &AircraftRig::setup_engine_sound);
	ClassDB::bind_method(D_METHOD("get_engine_audio"), &AircraftRig::get_engine_audio);
	ClassDB::bind_method(D_METHOD("update", "jsb", "delta", "user_volume_pct"), &AircraftRig::update);
	ClassDB::bind_method(D_METHOD("update_systems", "jsb", "delta", "rendering_cam", "pick_camera", "night", "sim_time_seconds", "sounds_active", "user_volume_pct"), &AircraftRig::update_systems);
	ClassDB::bind_method(D_METHOD("poll_keys", "jsb"), &AircraftRig::poll_keys);
	ClassDB::bind_method(D_METHOD("set_parking_brake_at_start", "jsb"), &AircraftRig::set_parking_brake_at_start);
	ClassDB::bind_method(D_METHOD("get_parking_brake"), &AircraftRig::get_parking_brake);
	ClassDB::bind_method(D_METHOD("set_parking_brake", "on"), &AircraftRig::set_parking_brake);
	ClassDB::bind_method(D_METHOD("get_fg_bridge"), &AircraftRig::get_fg_bridge);
	ClassDB::bind_method(D_METHOD("get_fg_controls"), &AircraftRig::get_fg_controls);
	ClassDB::bind_method(D_METHOD("get_fg_sounds"), &AircraftRig::get_fg_sounds);
	ClassDB::bind_method(D_METHOD("get_fg_animator"), &AircraftRig::get_fg_animator);
	ClassDB::bind_method(D_METHOD("get_aircraft_lights"), &AircraftRig::get_aircraft_lights);
	ClassDB::bind_method(D_METHOD("setup_aircraft_lights"), &AircraftRig::setup_aircraft_lights);
	ClassDB::bind_method(D_METHOD("setup_fg_animations"), &AircraftRig::setup_fg_animations);
	ClassDB::bind_method(D_METHOD("setup_fg_sounds"), &AircraftRig::setup_fg_sounds);
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
	cfg = AircraftConfig::load_from_file(cfg_path);
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
	if (fdm != nullptr)
	{
		UtilityFunctions::print("Loading JSBSim model '", jsbsim_model, "' for aircraft '", aircraft_id, "'");
		fdm->load_aircraft(aircraft_id, jsbsim_model);
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
				resolve_nodes(p_jsb);
				model_visuals_id = aircraft_id;
				changed = true;
			}
		}
	}
	apply_camera_config();
	return changed;
}

void AircraftRig::apply_model_normals(Node *root)
{
	if (root == nullptr)
	{
		return;
	}
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

// Only exterior surfaces are flipped (prefix): the interior cockpit meshes
// ship with correct normals and would be backface-culled from inside the
// cabin; the Dynamic_Switch_* subtree (re-rigged controls) is skipped too.
void AircraftRig::flip_normals_recursive(Node *node, const String &prefix)
{
	if (node->get_name().begins_with("Dynamic_Switch_"))
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
	setup_fg_sounds();
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
		camera_rig->set_orbit_distance_min(cfg->get_float("camera", "orbit_distance_min", 1.5));
		camera_rig->set_orbit_distance_max(cfg->get_float("camera", "orbit_distance_max", 50.0));
		camera_rig->set_orbit_distance(cfg->get_float("camera", "orbit_distance", 6.0));
	}
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
	control_surfaces->update(p_jsb, delta);
	if (engine_audio != nullptr)
	{
		// an aircraft with a FlightGear sound set plays its own engines
		engine_sound->update(control_surfaces->get_visual_rpm(), has_fg_sounds() ? 0.0 : user_volume_pct);
	}
}

// ── Aircraft systems ────────────────────────────────────────────────────────

Ref<FGBridge> AircraftRig::get_fg_bridge() const { return fg_bridge; }
Ref<FGControls> AircraftRig::get_fg_controls() const { return fg_controls; }
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
	if (fg_animator->load(path, model_root))
	{
		UtilityFunctions::print("FlightGear animations loaded for '", aircraft_id, "': ", fg_animator->stats());
		fg_controls->set_state(Dictionary());
		fg_seeded = false;
		fg_controls->set_on_change(Callable(this, "_on_fg_control"));
		UtilityFunctions::print("FlightGear cockpit controls: ", fg_controls->setup(path, model_root, fg_animator), " clickable parts");
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
	UtilityFunctions::print("FlightGear sounds for '", aircraft_id, "': ", n, " loaded, ", fg_sounds->get_skipped().size(), " without a file");
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
		return;
	}
	const bool landing_on = aircraft_lights != nullptr && aircraft_lights->landing_lights_on() && gear_down;
	fg_bridge->set_landing_light(landing_on);
	fg_bridge->set_taxi_light(landing_on);
	// "internal" follows the camera that renders (the intro zoom and the
	// external cameras are outside even while the mode says cockpit)
	fg_bridge->set_cockpit_view(rendering_cam != nullptr && rendering_cam == cockpit_camera);
	fg_bridge->set_sim_time_seconds(sim_time_seconds);
	sync_parking_brake(p_jsb);
	fg_bridge->set_control_state(fg_controls->get_state());
	fg_bridge->advance_canopy(delta);
	const Dictionary props = fg_bridge->collect(p_jsb, airborne, night);
	fg_animator->set_properties(props);
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
			state[unknown[i]] = FGBridge::switch_default(unknown[i]);
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
}

void AircraftRig::poll_keys(Object *p_jsb)
{
	Input *input = Input::get_singleton();
	const bool ctrl = input->is_key_pressed(KEY_CTRL);
	// Shift+Ctrl+M: show every cockpit mouse-sense area (FlightGear controls)
	if (input->is_key_pressed(KEY_M) && ctrl && input->is_key_pressed(KEY_SHIFT))
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
	if (input->is_key_pressed(KEY_B) && !ctrl)
	{
		if (!speedbrake_key_held)
		{
			speedbrake_key_held = true;
			if (fdm != nullptr)
			{
				speedbrake_out = !speedbrake_out;
				fdm->set_property("fcs/speedbrake-cmd-norm", speedbrake_out ? 1.0 : 0.0);
				UtilityFunctions::print("Speed brakes: ", speedbrake_out ? "OUT" : "IN");
			}
		}
	}
	else
	{
		speedbrake_key_held = false;
	}
	// Landing / taxi lights with L
	if (input->is_key_pressed(KEY_L) && !ctrl)
	{
		if (!landing_key_held)
		{
			landing_key_held = true;
			if (aircraft_lights != nullptr)
			{
				const bool on = aircraft_lights->toggle_landing_lights();
				UtilityFunctions::print("Landing lights: ", on ? "ON" : "OFF");
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
