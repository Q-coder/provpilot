#include "aircraft_config.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "install_config.h"

using namespace godot;

namespace jsbgodot
{

void AircraftConfig::_bind_methods()
{
	ClassDB::bind_static_method("AircraftConfig", D_METHOD("load_from_file", "path"), &AircraftConfig::load_from_file);
	ClassDB::bind_static_method("AircraftConfig", D_METHOD("resolve_path", "aircraft_id"), &AircraftConfig::resolve_path);
	ClassDB::bind_static_method("AircraftConfig", D_METHOD("resolve_aircraft_file", "aircraft_id", "rel"), &AircraftConfig::resolve_aircraft_file);
	ClassDB::bind_static_method("AircraftConfig", D_METHOD("load_for_aircraft", "aircraft_id"), &AircraftConfig::load_for_aircraft);
	ClassDB::bind_static_method("AircraftConfig", D_METHOD("parse_axis", "name", "fallback"), &AircraftConfig::parse_axis);

	ClassDB::bind_method(D_METHOD("parse_text", "text"), &AircraftConfig::parse_text);
	ClassDB::bind_method(D_METHOD("has_value", "section", "key"), &AircraftConfig::has_value);
	ClassDB::bind_method(D_METHOD("get_string", "section", "key", "default"), &AircraftConfig::get_string, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_float", "section", "key", "default"), &AircraftConfig::get_float, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_int", "section", "key", "default"), &AircraftConfig::get_int, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_bool", "section", "key", "default"), &AircraftConfig::get_bool, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_float_array", "section", "key", "default"), &AircraftConfig::get_float_array, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("get_string_array", "section", "key", "default"), &AircraftConfig::get_string_array, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("get_vector3", "section", "key", "default"), &AircraftConfig::get_vector3, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("get_axis", "section", "key", "fallback"), &AircraftConfig::get_axis);
	ClassDB::bind_method(D_METHOD("get_section", "section"), &AircraftConfig::get_section);

	ClassDB::bind_method(D_METHOD("is_loaded"), &AircraftConfig::is_loaded);
	ClassDB::bind_method(D_METHOD("set_loaded", "loaded"), &AircraftConfig::set_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &AircraftConfig::get_source_path);
	ClassDB::bind_method(D_METHOD("set_source_path", "path"), &AircraftConfig::set_source_path);
	ClassDB::bind_method(D_METHOD("get_sections"), &AircraftConfig::get_sections);
	ClassDB::bind_method(D_METHOD("set_sections", "sections"), &AircraftConfig::set_sections);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loaded"), "set_loaded", "is_loaded");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path"), "set_source_path", "get_source_path");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "sections"), "set_sections", "get_sections");
}

AircraftConfig::AircraftConfig()
{
}

// ── Loading ─────────────────────────────────────────────────────────────────

Ref<AircraftConfig> AircraftConfig::load_from_file(const String &path)
{
	Ref<AircraftConfig> cfg;
	cfg.instantiate();
	cfg->source_path = path;
	if (!FileAccess::file_exists(path))
	{
		UtilityFunctions::push_warning("AircraftConfig: file not found: ", path);
		return cfg;
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null())
	{
		UtilityFunctions::push_warning("AircraftConfig: cannot open ", path, " (err=", (int)FileAccess::get_open_error(), ")");
		return cfg;
	}
	cfg->parse_text(f->get_as_text());
	cfg->loaded = true;
	return cfg;
}

static InstallConfig *install_root_node()
{
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (tree == nullptr || tree->get_root() == nullptr)
	{
		return nullptr;
	}
	return Object::cast_to<InstallConfig>(tree->get_root()->get_node_or_null(NodePath("InstallRoot")));
}

String AircraftConfig::resolve_path(const String &aircraft_id)
{
	const String res_path = String("res://aircraft/") + aircraft_id + "/aircraft.cfg";
	if (OS::get_singleton()->has_feature("editor") && FileAccess::file_exists(res_path))
	{
		return res_path;
	}
	InstallConfig *ir = install_root_node();
	if (ir != nullptr)
	{
		const String p = ir->get_aircraft_dir(aircraft_id).path_join("aircraft.cfg");
		if (FileAccess::file_exists(p))
		{
			return p;
		}
	}
	return res_path;
}

String AircraftConfig::resolve_aircraft_file(const String &aircraft_id, const String &rel)
{
	InstallConfig *ir = install_root_node();
	if (ir != nullptr)
	{
		const String p = ir->get_aircraft_dir(aircraft_id).path_join(rel);
		if (FileAccess::file_exists(p))
		{
			return p;
		}
	}
	return ProjectSettings::get_singleton()->globalize_path(String("res://aircraft/") + aircraft_id + "/" + rel);
}

Ref<AircraftConfig> AircraftConfig::load_for_aircraft(const String &aircraft_id)
{
	return load_from_file(resolve_path(aircraft_id));
}

Vector3 AircraftConfig::parse_axis(const String &name, const Vector3 &fallback)
{
	String n = name.strip_edges().to_lower();
	double sgn = 1.0;
	if (n.begins_with("-"))
	{
		sgn = -1.0;
		n = n.substr(1);
	}
	else if (n.begins_with("+"))
	{
		n = n.substr(1);
	}
	Vector3 axis;
	if (n == "x")
	{
		axis = Vector3(1, 0, 0);
	}
	else if (n == "y")
	{
		axis = Vector3(0, 1, 0);
	}
	else if (n == "z")
	{
		axis = Vector3(0, 0, 1);
	}
	else
	{
		return fallback;
	}
	return axis * sgn;
}

// ── Parsing ─────────────────────────────────────────────────────────────────

void AircraftConfig::parse_text(const String &text)
{
	sections.clear();
	String current;
	const PackedStringArray lines = text.split("\n");
	for (int i = 0; i < lines.size(); i++)
	{
		const String line = lines[i].strip_edges();
		if (line.is_empty() || line.begins_with("#"))
		{
			continue;
		}
		if (line.begins_with("[") && line.ends_with("]"))
		{
			current = line.substr(1, line.length() - 2).strip_edges();
			if (!sections.has(current))
			{
				sections[current] = Dictionary();
			}
			continue;
		}
		if (current.is_empty())
		{
			continue; // key before any section header
		}
		const int sep = first_whitespace(line);
		if (sep < 0)
		{
			continue; // bare token with no value
		}
		const String key = line.substr(0, sep).strip_edges();
		const String value = parse_value(line.substr(sep).strip_edges());
		if (!key.is_empty())
		{
			Dictionary sec = sections[current];
			sec[key] = value;
			sections[current] = sec;
		}
	}
}

int AircraftConfig::first_whitespace(const String &s)
{
	const int sp = s.find(" ");
	const int tab = s.find("\t");
	if (sp < 0)
	{
		return tab;
	}
	if (tab < 0)
	{
		return sp;
	}
	return sp < tab ? sp : tab;
}

String AircraftConfig::parse_value(const String &p_rest)
{
	String rest = p_rest;
	if (rest.begins_with("\""))
	{
		const int end = rest.find("\"", 1);
		if (end > 0)
		{
			return rest.substr(1, end - 1);
		}
		return rest.substr(1); // unterminated quote: take the remainder
	}
	const int hash_idx = rest.find("#");
	if (hash_idx >= 0)
	{
		rest = rest.substr(0, hash_idx);
	}
	return rest.strip_edges();
}

// ── Getters ─────────────────────────────────────────────────────────────────

bool AircraftConfig::has_value(const String &section, const String &key) const
{
	if (!sections.has(section))
	{
		return false;
	}
	const Dictionary sec = sections[section];
	return sec.has(key);
}

String AircraftConfig::get_string(const String &section, const String &key, const String &default_value) const
{
	if (!has_value(section, key))
	{
		return default_value;
	}
	const Dictionary sec = sections[section];
	return sec[key];
}

double AircraftConfig::get_float(const String &section, const String &key, double default_value) const
{
	if (!has_value(section, key))
	{
		return default_value;
	}
	return get_string(section, key).to_float();
}

int AircraftConfig::get_int(const String &section, const String &key, int default_value) const
{
	if (!has_value(section, key))
	{
		return default_value;
	}
	return get_string(section, key).to_int();
}

bool AircraftConfig::get_bool(const String &section, const String &key, bool default_value) const
{
	if (!has_value(section, key))
	{
		return default_value;
	}
	const String v = get_string(section, key).strip_edges().to_lower();
	return v == "true" || v == "1" || v == "yes" || v == "on";
}

Array AircraftConfig::get_float_array(const String &section, const String &key, const Array &default_value) const
{
	if (!has_value(section, key))
	{
		return default_value;
	}
	Array out;
	const PackedStringArray parts = get_string(section, key).split(",", false);
	for (int i = 0; i < parts.size(); i++)
	{
		out.push_back(parts[i].strip_edges().to_float());
	}
	return out;
}

Array AircraftConfig::get_string_array(const String &section, const String &key, const Array &default_value) const
{
	if (!has_value(section, key))
	{
		return default_value;
	}
	Array out;
	const PackedStringArray parts = get_string(section, key).split(",", false);
	for (int i = 0; i < parts.size(); i++)
	{
		out.push_back(parts[i].strip_edges());
	}
	return out;
}

Vector3 AircraftConfig::get_vector3(const String &section, const String &key, const Vector3 &default_value) const
{
	const Array a = get_float_array(section, key, Array());
	if (a.size() != 3)
	{
		return default_value;
	}
	return Vector3((double)a[0], (double)a[1], (double)a[2]);
}

Vector3 AircraftConfig::get_axis(const String &section, const String &key, const Vector3 &fallback) const
{
	if (!has_value(section, key))
	{
		return fallback;
	}
	return parse_axis(get_string(section, key), fallback);
}

Dictionary AircraftConfig::get_section(const String &section) const
{
	if (!sections.has(section))
	{
		return Dictionary();
	}
	return sections[section];
}

bool AircraftConfig::is_loaded() const { return loaded; }
void AircraftConfig::set_loaded(bool p_loaded) { loaded = p_loaded; }
String AircraftConfig::get_source_path() const { return source_path; }
void AircraftConfig::set_source_path(const String &path) { source_path = path; }
Dictionary AircraftConfig::get_sections() const { return sections; }
void AircraftConfig::set_sections(const Dictionary &p_sections) { sections = p_sections; }

} // namespace jsbgodot
