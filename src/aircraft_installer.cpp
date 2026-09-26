#include "aircraft_installer.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "install_config.h"

using namespace godot;

namespace jsbgodot
{

// Asset files DirAccess cannot enumerate in an exported PCK (imported .glb
// sources): copied explicitly with FileAccess afterwards.
static const char *const REQUIRED_FILES[] = {
	"c172p/model/C172P_1.glb",
	"pc7/model/pc7.glb",
	"a10/model/a10.glb",
};
// Never copied: Godot / OS metadata, Blender sources, the split GLTF pair and
// loose textures already baked into the .glb.
static const char *const SKIP_EXTENSIONS[] = {"import", "uid", "blend", "blend1", "gltf", "bin", "png"};
static const char *const SKIP_NAMES[] = {".gdignore", ".gitkeep", ".DS_Store"};
static const char *const SKIP_DIR_NAMES[] = {"textures"};

void AircraftInstaller::_bind_methods()
{
	ClassDB::bind_static_method("AircraftInstaller", D_METHOD("get_bundle_version"), &AircraftInstaller::get_bundle_version);
}

AircraftInstaller::AircraftInstaller()
{
}

String AircraftInstaller::get_bundle_version()
{
	return BUNDLE_VERSION;
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

void AircraftInstaller::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	InstallConfig *ir = install_root_node();
	if (ir == nullptr)
	{
		UtilityFunctions::push_error("AircraftInstaller: InstallRoot autoload missing");
		return;
	}
	const String dest_root = ir->get_aircraft_root();
	// Self-heal: even if the install is "initialized" per provpilot.cfg, the
	// user might have deleted or moved the aircraft/ tree. Re-extract
	// whenever an expected sentinel file is missing (XML + 3D model).
	const String sentinel = dest_root.path_join("c172p").path_join("jsbsim").path_join("aircraft").path_join("c172p").path_join("c172p.xml");
	const bool have_data = FileAccess::file_exists(sentinel) && required_files_present(dest_root);
	if (ir->is_aircraft_initialized(BUNDLE_VERSION) && have_data)
	{
		UtilityFunctions::print("AircraftInstaller: aircraft data already extracted (version=", BUNDLE_VERSION, ")");
		return;
	}
	if (!have_data && ir->is_aircraft_initialized(BUNDLE_VERSION))
	{
		UtilityFunctions::print("AircraftInstaller: aircraft tree missing at ", dest_root, " - re-extracting.");
	}
	else
	{
		UtilityFunctions::print("AircraftInstaller: extracting bundled aircraft data -> ", dest_root);
	}
	const Error err = copy_tree(RES_AIRCRAFT_ROOT, dest_root);
	if (err != OK)
	{
		UtilityFunctions::push_error("AircraftInstaller: extraction failed (err=", (int)err, "). Files may be missing under ", dest_root);
		return;
	}
	copy_required_files(dest_root);
	ir->mark_aircraft_initialized(BUNDLE_VERSION);
	UtilityFunctions::print("AircraftInstaller: extraction complete.");
}

static bool in_list(const String &s, const char *const *list, int n)
{
	for (int i = 0; i < n; i++)
	{
		if (s == list[i])
		{
			return true;
		}
	}
	return false;
}

// Recursive res:// -> absolute copy; existing files are overwritten so a
// version bump can replace stale XML files.
Error AircraftInstaller::copy_tree(const String &src_res, const String &dest_abs)
{
	Ref<DirAccess> dir = DirAccess::open(src_res);
	if (dir.is_null())
	{
		UtilityFunctions::push_error("AircraftInstaller: cannot open ", src_res);
		return ERR_CANT_OPEN;
	}
	const Error mk = DirAccess::make_dir_recursive_absolute(dest_abs);
	if (mk != OK && mk != ERR_ALREADY_EXISTS)
	{
		UtilityFunctions::push_error("AircraftInstaller: cannot create ", dest_abs, " (err=", (int)mk, ")");
		return mk;
	}
	dir->list_dir_begin();
	String name = dir->get_next();
	while (!name.is_empty())
	{
		if (in_list(name, SKIP_NAMES, 3))
		{
			name = dir->get_next();
			continue;
		}
		const String src = src_res.path_join(name);
		const String dst = dest_abs.path_join(name);
		if (dir->current_is_dir())
		{
			if (!in_list(name, SKIP_DIR_NAMES, 1))
			{
				const Error err = copy_tree(src, dst);
				if (err != OK)
				{
					dir->list_dir_end();
					return err;
				}
			}
		}
		else if (!in_list(name.get_extension().to_lower(), SKIP_EXTENSIONS, 7))
		{
			const Error err = copy_file(src, dst);
			if (err != OK)
			{
				UtilityFunctions::push_error("AircraftInstaller: copy ", src, " -> ", dst, " failed (err=", (int)err, ")");
				dir->list_dir_end();
				return err;
			}
		}
		name = dir->get_next();
	}
	dir->list_dir_end();
	return OK;
}

Error AircraftInstaller::copy_file(const String &src_res, const String &dest_abs)
{
	Ref<FileAccess> fin = FileAccess::open(src_res, FileAccess::READ);
	if (fin.is_null())
	{
		return FileAccess::get_open_error();
	}
	const PackedByteArray buf = fin->get_buffer(fin->get_length());
	fin->close();
	Ref<FileAccess> fout = FileAccess::open(dest_abs, FileAccess::WRITE);
	if (fout.is_null())
	{
		return FileAccess::get_open_error();
	}
	fout->store_buffer(buf);
	fout->close();
	return OK;
}

void AircraftInstaller::copy_required_files(const String &dest_root)
{
	for (const char *rel : REQUIRED_FILES)
	{
		const String src = String(RES_AIRCRAFT_ROOT).path_join(rel);
		const String dst = dest_root.path_join(rel);
		if (FileAccess::file_exists(dst))
		{
			continue;
		}
		if (!FileAccess::file_exists(src))
		{
			// Not fatal: in exported builds the .glb source is replaced by the
			// imported .scn; the model loader falls back to the PackedScene.
			UtilityFunctions::push_warning("AircraftInstaller: source not in res:// (will use PCK resource): ", src);
			continue;
		}
		const String parent = dst.get_base_dir();
		const Error mk = DirAccess::make_dir_recursive_absolute(parent);
		if (mk != OK && mk != ERR_ALREADY_EXISTS)
		{
			UtilityFunctions::push_warning("AircraftInstaller: mkdir ", parent, " failed (err=", (int)mk, ")");
			continue;
		}
		const Error err = copy_file(src, dst);
		if (err != OK)
		{
			UtilityFunctions::push_warning("AircraftInstaller: copy ", src, " -> ", dst, " failed (err=", (int)err, ")");
			continue;
		}
		UtilityFunctions::print("AircraftInstaller: copied ", dst);
	}
}

// Only files whose source is reachable via res:// count, or exported builds
// would re-extract every launch.
bool AircraftInstaller::required_files_present(const String &dest_root)
{
	for (const char *rel : REQUIRED_FILES)
	{
		if (!FileAccess::file_exists(String(RES_AIRCRAFT_ROOT).path_join(rel)))
		{
			continue;
		}
		if (!FileAccess::file_exists(dest_root.path_join(rel)))
		{
			return false;
		}
	}
	return true;
}

} // namespace jsbgodot
