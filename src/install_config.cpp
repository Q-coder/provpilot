#include "install_config.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace jsbgodot
{

// ── Binding ─────────────────────────────────────────────────────────────────

void InstallConfig::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_install_root"), &InstallConfig::get_install_root);
    ClassDB::bind_method(D_METHOD("get_config_path"), &InstallConfig::get_config_path);
    ClassDB::bind_method(D_METHOD("has_config"), &InstallConfig::has_config);
    ClassDB::bind_method(D_METHOD("is_first_run"), &InstallConfig::is_first_run);

    ClassDB::bind_method(D_METHOD("get_scenery_root"), &InstallConfig::get_scenery_root);
    ClassDB::bind_method(D_METHOD("get_regions"), &InstallConfig::get_regions);
    ClassDB::bind_method(D_METHOD("get_default_scan_roots"), &InstallConfig::get_default_scan_roots);
    ClassDB::bind_method(D_METHOD("save_scenery_choice", "scenery_root", "regions"), &InstallConfig::save_scenery_choice, DEFVAL(PackedStringArray()));

    ClassDB::bind_method(D_METHOD("get_aircraft_root"), &InstallConfig::get_aircraft_root);
    ClassDB::bind_method(D_METHOD("get_aircraft_dir", "aircraft_id"), &InstallConfig::get_aircraft_dir);
    ClassDB::bind_method(D_METHOD("is_aircraft_initialized", "version"), &InstallConfig::is_aircraft_initialized);
    ClassDB::bind_method(D_METHOD("mark_aircraft_initialized", "version"), &InstallConfig::mark_aircraft_initialized);

    ClassDB::bind_method(D_METHOD("get_value", "section", "key", "default"), &InstallConfig::get_value, DEFVAL(Variant()));
    ClassDB::bind_method(D_METHOD("set_value", "section", "key", "value"), &InstallConfig::set_value);
    ClassDB::bind_method(D_METHOD("has_section", "section"), &InstallConfig::has_section);
    ClassDB::bind_method(D_METHOD("has_section_key", "section", "key"), &InstallConfig::has_section_key);
    ClassDB::bind_method(D_METHOD("get_sections"), &InstallConfig::get_sections);
    ClassDB::bind_method(D_METHOD("get_section_keys", "section"), &InstallConfig::get_section_keys);
    ClassDB::bind_method(D_METHOD("erase_section", "section"), &InstallConfig::erase_section);
    ClassDB::bind_method(D_METHOD("erase_section_key", "section", "key"), &InstallConfig::erase_section_key);
    ClassDB::bind_method(D_METHOD("save_config"), &InstallConfig::save_config);
}

InstallConfig::InstallConfig()
{
    config.instantiate();
}

void InstallConfig::_ready()
{
    _discover_and_load();
    _migrate_legacy_user_configs();
}

// ── Discovery ───────────────────────────────────────────────────────────────

void InstallConfig::_discover_and_load()
{
    PackedStringArray candidates = _candidate_roots();
    // Portable-install rule: in shipped (non-editor) builds the directory
    // next to the executable IS the install root, even if no provpilot.cfg
    // has been written there yet. This way the user can move the folder
    // (e.g. C:\Users\me\ProvPilot -> I:\ProvPilot) and the app immediately
    // follows, instead of latching onto a stale provpilot.cfg in the legacy
    // ~/ProvPilot location.
    OS *os = OS::get_singleton();
    if (!os->has_feature("editor") && !candidates.is_empty() && _is_writable(candidates[0]))
    {
        install_root = candidates[0];
        config_path = install_root.path_join(CONFIG_FILENAME);
    }
    else
    {
        // Editor / dev fallback: the first candidate that already has a
        // config file, otherwise the first writable candidate.
        for (int i = 0; i < candidates.size(); i++)
        {
            String p = candidates[i].path_join(CONFIG_FILENAME);
            if (FileAccess::file_exists(p))
            {
                install_root = candidates[i];
                config_path = p;
                break;
            }
        }
        if (install_root.is_empty())
        {
            for (int i = 0; i < candidates.size(); i++)
            {
                if (_is_writable(candidates[i]))
                {
                    install_root = candidates[i];
                    config_path = install_root.path_join(CONFIG_FILENAME);
                    break;
                }
            }
        }
    }
    // Last resort: user data dir (always writable).
    if (install_root.is_empty())
    {
        install_root = os->get_user_data_dir();
        config_path = install_root.path_join(CONFIG_FILENAME);
    }

    // One-time migration: if the chosen install root has no provpilot.cfg
    // yet but a legacy location does, copy that cfg in so the user keeps
    // their scenery root, joystick bindings, etc.
    _migrate_legacy_install_cfg(candidates);

    if (FileAccess::file_exists(config_path))
    {
        Error err = config->load(config_path);
        if (err == OK)
        {
            loaded = true;
            _take_snapshot();
            UtilityFunctions::print("InstallRoot: loaded config ", config_path);
        }
        else
        {
            UtilityFunctions::push_warning(vformat("InstallRoot: failed to load %s (err=%d)", config_path, int(err)));
        }
    }
    else
    {
        UtilityFunctions::print("InstallRoot: no config at ", config_path, " (first run)");
    }
    UtilityFunctions::print("InstallRoot: install_root=", install_root);
}

PackedStringArray InstallConfig::_candidate_roots() const
{
    PackedStringArray out;
    OS *os = OS::get_singleton();
    String exec = os->get_executable_path();
    if (exec.find(".app/Contents/MacOS/") != -1)
    {
        // 1a) macOS bundle parent: strip /<exe>, /MacOS, /Contents, /<bundle>.app
        String bundle_dir = exec.get_base_dir().get_base_dir().get_base_dir();
        String parent = bundle_dir.get_base_dir();
        if (!parent.is_empty())
        {
            out.append(parent);
        }
    }
    else if (!exec.is_empty() && !os->has_feature("editor"))
    {
        // 1b) Windows / Linux: the executable sits directly in the install root.
        String exec_dir = exec.get_base_dir();
        if (!exec_dir.is_empty() && !out.has(exec_dir))
        {
            out.append(exec_dir);
        }
    }
    // 2) $PWD (terminal launch dir — set on Linux/macOS but not on Windows CMD)
    String pwd = os->get_environment("PWD");
    if (!pwd.is_empty() && DirAccess::dir_exists_absolute(pwd) && !out.has(pwd))
    {
        out.append(pwd);
    }
    // 3) ~/ProvPilot — friendly default. Windows uses USERPROFILE instead of HOME.
    String home = os->get_environment("HOME");
    if (home.is_empty())
    {
        home = os->get_environment("USERPROFILE");
    }
    if (!home.is_empty())
    {
        String p = home.path_join("ProvPilot");
        if (!out.has(p))
        {
            out.append(p);
        }
    }
    // 4) res:// (editor / dev). globalize_path turns it into an OS path.
    if (os->has_feature("editor"))
    {
        String res_dir = ProjectSettings::get_singleton()->globalize_path("res://");
        if (!out.has(res_dir))
        {
            out.append(res_dir);
        }
    }
    return out;
}

bool InstallConfig::_is_writable(const String &path)
{
    if (!DirAccess::dir_exists_absolute(path))
    {
        // Being able to create the directory counts as writable.
        return DirAccess::make_dir_recursive_absolute(path) == OK;
    }
    // Probe with a small temp file.
    String probe = path.path_join(".provpilot_write_probe");
    Ref<FileAccess> f = FileAccess::open(probe, FileAccess::WRITE);
    if (f.is_null())
    {
        return false;
    }
    f->close();
    DirAccess::remove_absolute(probe);
    return true;
}

bool InstallConfig::_case_insensitive_fs()
{
    String os = OS::get_singleton()->get_name();
    return os == "macOS" || os == "Windows";
}

// ── Install location ────────────────────────────────────────────────────────

String InstallConfig::get_install_root() const
{
    return install_root;
}

String InstallConfig::get_config_path() const
{
    return config_path;
}

bool InstallConfig::has_config() const
{
    return loaded;
}

bool InstallConfig::is_first_run() const
{
    if (!loaded)
    {
        return true;
    }
    if (bool(config->get_value(SEC_SCENERY, KEY_FIRST_RUN_DONE, false)))
    {
        return false;
    }
    return get_scenery_root().is_empty();
}

// ── [scenery] ───────────────────────────────────────────────────────────────

String InstallConfig::get_scenery_root() const
{
    return String(config->get_value(SEC_SCENERY, KEY_SCENERY_ROOT, ""));
}

PackedStringArray InstallConfig::get_regions() const
{
    Variant raw = config->get_value(SEC_SCENERY, KEY_REGIONS, PackedStringArray());
    if (raw.get_type() == Variant::PACKED_STRING_ARRAY)
    {
        return raw;
    }
    if (raw.get_type() == Variant::ARRAY)
    {
        Array arr = raw;
        PackedStringArray out;
        for (int i = 0; i < arr.size(); i++)
        {
            out.append(String(arr[i]));
        }
        return out;
    }
    return PackedStringArray();
}

PackedStringArray InstallConfig::get_default_scan_roots() const
{
    PackedStringArray roots;
    String configured = get_scenery_root();
    if (!configured.is_empty())
    {
        roots.append(configured);
    }
    // Next to the install
    roots.append(install_root.path_join("scenery"));
    // Home dir candidates
    String home = OS::get_singleton()->get_environment("HOME");
    if (!home.is_empty())
    {
        roots.append(home.path_join("ProvPilot/scenery"));
        roots.append(home.path_join("Documents/ProvPilot/scenery"));
    }
    // External volumes — best-effort /Volumes/*/scenery candidates (macOS).
    Ref<DirAccess> volumes = DirAccess::open("/Volumes");
    if (volumes.is_valid())
    {
        volumes->list_dir_begin();
        String name = volumes->get_next();
        while (!name.is_empty())
        {
            if (!name.begins_with(".") && volumes->current_is_dir())
            {
                roots.append(String("/Volumes/").path_join(name).path_join("scenery"));
            }
            name = volumes->get_next();
        }
        volumes->list_dir_end();
    }
    // De-duplicate while preserving order. macOS' default APFS volume is
    // case-insensitive, so `~/provpilot/scenery` and `~/ProvPilot/scenery`
    // refer to the same directory — collapse them to one entry to avoid the
    // scanner adding the same scenery area twice.
    const bool ci = _case_insensitive_fs();
    Dictionary seen;
    PackedStringArray unique;
    for (int i = 0; i < roots.size(); i++)
    {
        String key = ci ? roots[i].to_lower() : roots[i];
        if (!seen.has(key))
        {
            seen[key] = true;
            unique.append(roots[i]);
        }
    }
    return unique;
}

Error InstallConfig::save_scenery_choice(const String &scenery_root, const PackedStringArray &regions)
{
    config->set_value(SEC_SCENERY, KEY_SCENERY_ROOT, scenery_root);
    if (regions.size() > 0)
    {
        config->set_value(SEC_SCENERY, KEY_REGIONS, regions);
    }
    config->set_value(SEC_SCENERY, KEY_FIRST_RUN_DONE, true);
    return _save("saved");
}

// ── [aircraft] ──────────────────────────────────────────────────────────────

String InstallConfig::get_aircraft_root() const
{
    return install_root.path_join(AIRCRAFT_SUBDIR);
}

String InstallConfig::get_aircraft_dir(const String &aircraft_id) const
{
    return get_aircraft_root().path_join(aircraft_id);
}

bool InstallConfig::is_aircraft_initialized(const String &version) const
{
    if (!bool(config->get_value(SEC_AIRCRAFT, KEY_AIRCRAFT_INITIALIZED, false)))
    {
        return false;
    }
    return String(config->get_value(SEC_AIRCRAFT, KEY_AIRCRAFT_VERSION, "")) == version;
}

Error InstallConfig::mark_aircraft_initialized(const String &version)
{
    config->set_value(SEC_AIRCRAFT, KEY_AIRCRAFT_INITIALIZED, true);
    config->set_value(SEC_AIRCRAFT, KEY_AIRCRAFT_VERSION, version);
    Error err = _save(nullptr);
    if (err == OK)
    {
        UtilityFunctions::print("InstallRoot: marked aircraft initialized (version=", version, ")");
    }
    return err;
}

// ── Generic config API ──────────────────────────────────────────────────────

Variant InstallConfig::get_value(const String &section, const String &key, const Variant &default_value) const
{
    return config->get_value(section, key, default_value);
}

void InstallConfig::set_value(const String &section, const String &key, const Variant &value)
{
    config->set_value(section, key, value);
}

bool InstallConfig::has_section(const String &section) const
{
    return config->has_section(section);
}

bool InstallConfig::has_section_key(const String &section, const String &key) const
{
    return config->has_section_key(section, key);
}

PackedStringArray InstallConfig::get_sections() const
{
    return config->get_sections();
}

PackedStringArray InstallConfig::get_section_keys(const String &section) const
{
    if (!config->has_section(section))
    {
        return PackedStringArray();
    }
    return config->get_section_keys(section);
}

void InstallConfig::erase_section(const String &section)
{
    if (config->has_section(section))
    {
        config->erase_section(section);
    }
}

void InstallConfig::erase_section_key(const String &section, const String &key)
{
    if (config->has_section_key(section, key))
    {
        config->erase_section_key(section, key);
    }
}

Error InstallConfig::save_config()
{
    return _save(nullptr);
}

// Flush the in-memory config to provpilot.cfg. `what` (optional) is echoed
// on success; failures are always reported.
Error InstallConfig::_save(const char *what)
{
    // Another instance (the editor next to the game, a test run next to a
    // flight) may have written the file since this one read it. Writing the
    // whole in-memory copy would put that instance's changes back to what this
    // one loaded - joystick bindings came back as their old values that way.
    // So: read the file again and apply only what THIS instance changed since
    // its last load / save.
    Ref<ConfigFile> fresh;
    fresh.instantiate();
    if (disk_snapshot.is_valid() && FileAccess::file_exists(config_path) && fresh->load(config_path) == OK)
    {
        PackedStringArray sections = config->get_sections();
        const PackedStringArray old_sections = disk_snapshot->get_sections();
        for (int i = 0; i < old_sections.size(); i++)
        {
            if (!sections.has(old_sections[i]))
            {
                sections.append(old_sections[i]);
            }
        }
        for (int i = 0; i < sections.size(); i++)
        {
            const String sn = sections[i];
            PackedStringArray keys = config->has_section(sn) ? config->get_section_keys(sn) : PackedStringArray();
            if (disk_snapshot->has_section(sn))
            {
                const PackedStringArray old_keys = disk_snapshot->get_section_keys(sn);
                for (int k = 0; k < old_keys.size(); k++)
                {
                    if (!keys.has(old_keys[k]))
                    {
                        keys.append(old_keys[k]);
                    }
                }
            }
            for (int k = 0; k < keys.size(); k++)
            {
                const bool now = config->has_section_key(sn, keys[k]);
                const bool before = disk_snapshot->has_section_key(sn, keys[k]);
                if (now && before && UtilityFunctions::var_to_str(config->get_value(sn, keys[k])) == UtilityFunctions::var_to_str(disk_snapshot->get_value(sn, keys[k])))
                {
                    continue; // untouched here: the file's value stands
                }
                if (now)
                {
                    fresh->set_value(sn, keys[k], config->get_value(sn, keys[k]));
                }
                else if (fresh->has_section_key(sn, keys[k]))
                {
                    fresh->erase_section_key(sn, keys[k]);
                }
            }
        }
        config = fresh;
    }
    Error err = config->save(config_path);
    if (err == OK)
    {
        loaded = true;
        _take_snapshot();
        if (what != nullptr)
        {
            UtilityFunctions::print("InstallRoot: ", what, " ", config_path);
        }
    }
    else
    {
        UtilityFunctions::push_error(vformat("InstallRoot: failed to save %s (err=%d)", config_path, int(err)));
    }
    return err;
}

// The file's content as this instance last read or wrote it (the baseline
// _save compares against).
void InstallConfig::_take_snapshot()
{
    disk_snapshot.instantiate();
    disk_snapshot->parse(config->encode_to_text());
}

// ── One-time migrations ─────────────────────────────────────────────────────

// When the user moves a portable install (e.g. C:\Users\me\ProvPilot ->
// I:\ProvPilot) the executable's new directory is empty and the old
// ~/ProvPilot folder still has a stale provpilot.cfg. We adopt the new
// exec_dir as the install root (see _discover_and_load) and, if no cfg exists
// there yet, copy the first legacy cfg in so the user keeps their scenery
// root, joystick bindings, etc.
void InstallConfig::_migrate_legacy_install_cfg(const PackedStringArray &candidates)
{
    if (FileAccess::file_exists(config_path))
    {
        return; // current install root already has its own cfg
    }
    for (int i = 0; i < candidates.size(); i++)
    {
        if (candidates[i] == install_root)
        {
            continue;
        }
        String legacy = candidates[i].path_join(CONFIG_FILENAME);
        if (!FileAccess::file_exists(legacy))
        {
            continue;
        }
        // Copy bytes verbatim — preserves comments, ordering, and any
        // sections we don't know about.
        Ref<FileAccess> src = FileAccess::open(legacy, FileAccess::READ);
        if (src.is_null())
        {
            continue;
        }
        PackedByteArray data = src->get_buffer(src->get_length());
        src->close();
        Ref<FileAccess> dst = FileAccess::open(config_path, FileAccess::WRITE);
        if (dst.is_null())
        {
            UtilityFunctions::push_warning("InstallRoot: cannot write ", config_path, " - legacy cfg ", legacy, " NOT migrated");
            return;
        }
        dst->store_buffer(data);
        dst->close();
        UtilityFunctions::print("InstallRoot: migrated legacy install cfg ", legacy, " -> ", config_path);
        return;
    }
}

// Earlier builds wrote joystick bindings and user settings to separate
// ConfigFiles under user:// (e.g. ~/Library/Application Support/Godot/...).
// On first launch with the consolidated layout, copy any pre-existing values
// into provpilot.cfg so the user doesn't lose their setup. The legacy files
// are never deleted — leaving them in place is harmless and reversible.
void InstallConfig::_migrate_legacy_user_configs()
{
    bool migrated = false;
    if (!config->has_section("device") && !_has_any_section_with_prefix("axis.") && !_has_any_section_with_prefix("button."))
    {
        migrated = _import_legacy_cfg("user://joystick.cfg") || migrated;
    }
    if (!config->has_section("graphics") && !config->has_section("sound") && !config->has_section("realism"))
    {
        migrated = _import_legacy_cfg("user://settings.cfg") || migrated;
    }
    if (migrated)
    {
        if (_save(nullptr) == OK)
        {
            UtilityFunctions::print("InstallRoot: migrated legacy user://*.cfg -> ", config_path);
        }
    }
}

bool InstallConfig::_has_any_section_with_prefix(const String &prefix) const
{
    PackedStringArray sections = config->get_sections();
    for (int i = 0; i < sections.size(); i++)
    {
        if (sections[i].begins_with(prefix))
        {
            return true;
        }
    }
    return false;
}

// Merge keys from a legacy ConfigFile into the shared config. Existing keys
// win (no overwrite). Returns true if anything was copied.
bool InstallConfig::_import_legacy_cfg(const String &legacy_path)
{
    if (!FileAccess::file_exists(legacy_path))
    {
        return false;
    }
    Ref<ConfigFile> cfg;
    cfg.instantiate();
    if (cfg->load(legacy_path) != OK)
    {
        return false;
    }
    bool any = false;
    PackedStringArray sections = cfg->get_sections();
    for (int s = 0; s < sections.size(); s++)
    {
        PackedStringArray keys = cfg->get_section_keys(sections[s]);
        for (int k = 0; k < keys.size(); k++)
        {
            if (config->has_section_key(sections[s], keys[k]))
            {
                continue;
            }
            config->set_value(sections[s], keys[k], cfg->get_value(sections[s], keys[k]));
            any = true;
        }
    }
    if (any)
    {
        UtilityFunctions::print("InstallRoot: imported ", legacy_path);
    }
    return any;
}

} // namespace jsbgodot
