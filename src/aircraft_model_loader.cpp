#include "aircraft_model_loader.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "install_config.h"

using namespace godot;

namespace jsbgodot
{

static const char *DEFAULT_AIRCRAFT_ID = "c172p";

void AircraftModelLoader::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("load_aircraft_model", "aircraft_id"), &AircraftModelLoader::load_aircraft_model);
	ClassDB::bind_method(D_METHOD("get_loaded_aircraft_id"), &AircraftModelLoader::get_loaded_aircraft_id);
}

AircraftModelLoader::AircraftModelLoader()
{
}

void AircraftModelLoader::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	load_aircraft_model(DEFAULT_AIRCRAFT_ID);
}

String AircraftModelLoader::get_loaded_aircraft_id() const { return loaded_aircraft_id; }

bool AircraftModelLoader::load_aircraft_model(const String &p_aircraft_id)
{
	const String aircraft_id = p_aircraft_id.is_empty() ? String(DEFAULT_AIRCRAFT_ID) : p_aircraft_id;
	if (aircraft_id == loaded_aircraft_id)
	{
		return true;
	}
	// Generic per-aircraft defaults; the config may override them.
	String rel_path = String("model/") + aircraft_id + ".glb";
	String res_path = String("res://aircraft/") + aircraft_id + "/model/" + aircraft_id + ".glb";
	if (aircraft_id == DEFAULT_AIRCRAFT_ID)
	{
		rel_path = "model/C172P_1.glb";
		res_path = "res://aircraft/c172p/model/C172P_1.glb";
	}
	Ref<AircraftConfig> cfg = AircraftConfig::load_for_aircraft(aircraft_id);
	if (cfg->is_loaded())
	{
		rel_path = cfg->get_string("model", "file", rel_path);
		res_path = cfg->get_string("model", "res_file", res_path);
	}

	Node *scene = load_from_disk(aircraft_id, rel_path);
	if (scene == nullptr)
	{
		scene = load_from_resource(res_path);
	}
	if (scene == nullptr)
	{
		UtilityFunctions::push_error("AircraftModelLoader: failed to load the model for '", aircraft_id, "' (no on-disk .glb and no PCK fallback)");
		return false;
	}

	// Drop the previous model subtree synchronously: the modules that hold
	// pointers into it are reset by AircraftRig::resolve_nodes right after.
	TypedArray<Node> old = get_children();
	for (int i = 0; i < old.size(); i++)
	{
		Node *n = Object::cast_to<Node>(old[i]);
		remove_child(n);
		memdelete(n);
	}

	// Reparent the GLB's top-level children onto this node and drop the
	// now-empty wrapper, so this node is the parent of the model root exactly
	// like the former PackedScene instance was. The optional fitting
	// transform lets a model authored at another native scale be placed
	// without code changes (identity for the Cessna).
	const Transform3D fit = model_transform(cfg);
	TypedArray<Node> children = scene->get_children();
	for (int i = 0; i < children.size(); i++)
	{
		Node *c = Object::cast_to<Node>(children[i]);
		scene->remove_child(c);
		add_child(c);
		Node3D *c3 = Object::cast_to<Node3D>(c);
		if (c3 != nullptr && fit != Transform3D())
		{
			c3->set_transform(fit * c3->get_transform());
		}
	}
	scene->queue_free();
	loaded_aircraft_id = aircraft_id;
	return true;
}

// Preferred path: a real .glb from <install_root>/aircraft/... so users can
// swap the model file without rebuilding the PCK.
Node *AircraftModelLoader::load_from_disk(const String &aircraft_id, const String &rel_path) const
{
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	InstallConfig *ir = (tree != nullptr && tree->get_root() != nullptr)
			? Object::cast_to<InstallConfig>(tree->get_root()->get_node_or_null(NodePath("InstallRoot")))
			: nullptr;
	if (ir == nullptr)
	{
		return nullptr;
	}
	const String path = ir->get_aircraft_dir(aircraft_id).path_join(rel_path);
	if (!FileAccess::file_exists(path))
	{
		return nullptr;
	}
	Ref<GLTFDocument> doc;
	doc.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	const Error err = doc->append_from_file(path, state);
	if (err != OK)
	{
		UtilityFunctions::push_warning("AircraftModelLoader: append_from_file failed (err=", (int)err, ") for ", path);
		return nullptr;
	}
	Node *scene = doc->generate_scene(state);
	if (scene == nullptr)
	{
		UtilityFunctions::push_warning("AircraftModelLoader: generate_scene returned null for ", path);
		return nullptr;
	}
	UtilityFunctions::print("AircraftModelLoader: loaded model from disk: ", path);
	return scene;
}

// Fallback: the PackedScene Godot built when importing the .glb (works in
// both the editor and exported builds).
Node *AircraftModelLoader::load_from_resource(const String &res_path) const
{
	ResourceLoader *rl = ResourceLoader::get_singleton();
	if (!rl->exists(res_path))
	{
		UtilityFunctions::push_error("AircraftModelLoader: resource missing: ", res_path);
		return nullptr;
	}
	Ref<PackedScene> ps = rl->load(res_path);
	if (ps.is_null())
	{
		UtilityFunctions::push_error("AircraftModelLoader: ", res_path, " is not a PackedScene");
		return nullptr;
	}
	Node *inst = ps->instantiate();
	if (inst == nullptr)
	{
		UtilityFunctions::push_error("AircraftModelLoader: instantiate() returned null for ", res_path);
		return nullptr;
	}
	UtilityFunctions::print("AircraftModelLoader: loaded model from PCK resource: ", res_path);
	return inst;
}

// cfg [model]: scale (uniform), scale_xyz "x, y, z" (overrides), offset
// "x, y, z" (after scale), rotation_deg "x, y, z" (local euler).
Transform3D AircraftModelLoader::model_transform(const Ref<AircraftConfig> &cfg)
{
	if (cfg.is_null() || !cfg->is_loaded())
	{
		return Transform3D();
	}
	Basis basis;
	const Array rot = cfg->get_float_array("model", "rotation_deg", Array());
	if (rot.size() == 3)
	{
		basis = Basis::from_euler(Vector3(Math::deg_to_rad((double)rot[0]), Math::deg_to_rad((double)rot[1]), Math::deg_to_rad((double)rot[2])));
	}
	const Array scale_xyz = cfg->get_float_array("model", "scale_xyz", Array());
	if (scale_xyz.size() == 3)
	{
		basis = basis.scaled(Vector3((double)scale_xyz[0], (double)scale_xyz[1], (double)scale_xyz[2]));
	}
	else
	{
		const double s = cfg->get_float("model", "scale", 1.0);
		if (s != 1.0)
		{
			basis = basis.scaled(Vector3(s, s, s));
		}
	}
	return Transform3D(basis, cfg->get_vector3("model", "offset", Vector3()));
}

} // namespace jsbgodot
