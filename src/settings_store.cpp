#include "settings_store.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <utility>
#include <vector>

using namespace godot;

namespace jsbgodot
{

// ── Constants ───────────────────────────────────────────────────────────────

// Asset subdirs auto-detected inside each scenery area's path.
const char *SettingsStore::ASSET_SUBDIRS[] = { "terrain", "orthophoto", "buildings", "water", "roads", "vegetation", "trees", "airports", "objects" };
const int SettingsStore::ASSET_SUBDIR_COUNT = 9;

const double SettingsStore::TREE_DENSITY_FRACTION[5] = { 0.0, 0.25, 0.50, 0.75, 1.0 };

// Sections read back from provpilot.cfg at load(). Keys are routed to one of
// these by section_for_key() on save.
static const char *SETTINGS_SECTIONS[] = { "scenery", "graphics", "sound", "realism", "world", "map", "weather", "aircraft" };
static const int SETTINGS_SECTION_COUNT = 8;

// Library state that survives "Restore Defaults" (user data, not preferences).
static const char *LIBRARY_KEYS[] = { "scenery_areas", "scenery_scan_roots", "installed_scenery_packs" };
static const int LIBRARY_KEY_COUNT = 3;

// ── Binding ─────────────────────────────────────────────────────────────────

void SettingsStore::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("get_setting", "key"), &SettingsStore::get_setting);
    ClassDB::bind_method(D_METHOD("set_setting", "key", "value"), &SettingsStore::set_setting);
    ClassDB::bind_method(D_METHOD("has_setting", "key"), &SettingsStore::has_setting);
    ClassDB::bind_method(D_METHOD("get_settings"), &SettingsStore::get_settings);
    ClassDB::bind_method(D_METHOD("weather_profiles_dir"), &SettingsStore::weather_profiles_dir);
    ClassDB::bind_method(D_METHOD("list_weather_profiles"), &SettingsStore::list_weather_profiles);
    ClassDB::bind_method(D_METHOD("save_weather_profile", "name"), &SettingsStore::save_weather_profile);
    ClassDB::bind_method(D_METHOD("load_weather_profile", "name"), &SettingsStore::load_weather_profile);
    ClassDB::bind_method(D_METHOD("delete_weather_profile", "name"), &SettingsStore::delete_weather_profile);
    ClassDB::bind_method(D_METHOD("restore_defaults"), &SettingsStore::restore_defaults);
    ClassDB::bind_static_method("SettingsStore", D_METHOD("get_defaults"), &SettingsStore::get_defaults);
    ClassDB::bind_static_method("SettingsStore", D_METHOD("section_for_key", "key"), &SettingsStore::section_for_key);

    ClassDB::bind_method(D_METHOD("load"), &SettingsStore::load);
    ClassDB::bind_method(D_METHOD("save"), &SettingsStore::save);

    ClassDB::bind_method(D_METHOD("get_scenery_areas"), &SettingsStore::get_scenery_areas);
    ClassDB::bind_method(D_METHOD("set_scenery_areas", "areas"), &SettingsStore::set_scenery_areas);
    ClassDB::bind_method(D_METHOD("get_enabled_scenery_areas"), &SettingsStore::get_enabled_scenery_areas);
    ClassDB::bind_method(D_METHOD("scan_scenery"), &SettingsStore::scan_scenery);
    ClassDB::bind_method(D_METHOD("add_scenery_area", "path"), &SettingsStore::add_scenery_area);
    ClassDB::bind_method(D_METHOD("remove_scenery_area", "idx"), &SettingsStore::remove_scenery_area);
    ClassDB::bind_method(D_METHOD("swap_scenery_priority", "a_idx", "b_idx"), &SettingsStore::swap_scenery_priority);
    ClassDB::bind_method(D_METHOD("set_area_enabled", "idx", "enabled"), &SettingsStore::set_area_enabled);
    ClassDB::bind_method(D_METHOD("set_area_priority", "idx", "priority"), &SettingsStore::set_area_priority);
    ClassDB::bind_method(D_METHOD("add_scan_root", "path"), &SettingsStore::add_scan_root);
    ClassDB::bind_method(D_METHOD("detect_assets", "path"), &SettingsStore::detect_assets);
    ClassDB::bind_method(D_METHOD("missing_scenery_reason"), &SettingsStore::missing_scenery_reason);
    ClassDB::bind_static_method("SettingsStore", D_METHOD("format_path_for_display", "path", "max_chars"), &SettingsStore::format_path_for_display);

    ClassDB::bind_method(D_METHOD("apply_scenery_sources", "scenery3d", "vegetation", "vfr_map"), &SettingsStore::apply_scenery_sources);
    ClassDB::bind_method(D_METHOD("apply_tree_density", "vegetation"), &SettingsStore::apply_tree_density);
    ClassDB::bind_static_method("SettingsStore", D_METHOD("format_hhmm", "minutes"), &SettingsStore::format_hhmm);
}

SettingsStore::SettingsStore()
{
    settings = get_defaults();
}

// ── Values ──────────────────────────────────────────────────────────────────

Dictionary SettingsStore::get_defaults()
{
    Dictionary d;
    // Scenery library (see class comment); populated at runtime.
    d["scenery_areas"] = Array();
    d["scenery_scan_roots"] = PackedStringArray();
    // Per-pack install state for the in-app Scenery Downloader
    // (scenery_downloader.gd). Keyed by pack_id.
    d["installed_scenery_packs"] = Dictionary();
    d["skip_white_pixels"] = true;
    // Graphics
    d["render_distance"] = 8;     // terrain load radius in tiles
    d["building_distance"] = 3;   // building load radius in tiles
    d["shadow_quality"] = 2;      // 0=off, 1=low, 2=medium, 3=high
    d["cloud_quality"] = 2;       // 0=off, 1=low (24 steps), 2=medium (40), 3=high (64)
    d["tree_density"] = 2;        // index into TREE_DENSITY_FRACTION
    d["vsync"] = true;
    d["fullscreen"] = true;
    // World / environment
    d["use_system_time"] = false; // false = manual time slider drives the sun
    d["time_of_day_min"] = 720;   // manual local time, minutes past midnight
    // Map (VFR map frame — zoom_index is owned by the VfrMap module)
    d["map_enabled"] = true;
    d["map_frame_size"] = 1;      // 0=Small(240px) 1=Medium(320px) 2=Large(400px)
    // Sound
    d["master_volume"] = 80;
    d["engine_volume"] = 70;
    d["wind_volume"] = 50;
    d["ui_volume"] = 60;
    // Realism
    d["auto_mixture"] = true;
    d["auto_carb_heat"] = true;
    d["unlimited_fuel"] = true;
    d["crash_detection"] = false;
    // Weather (UI model only for now — see design doc; the JSBSim hookup
    // for wind/turbulence/QNH and the visual clouds/visibility come later).
    // Each layer spans base_m..top_m (wind and clouds alike); the gaps in
    // between are clear air with wind interpolated across them.
    {
        Array layers;
        Dictionary l;
        l["base_m"] = 0;
        l["top_m"] = 3000;
        l["wind_dir_deg"] = 0;     // direction the wind blows FROM
        l["wind_speed_kts"] = 0;
        l["turbulence"] = 0;       // 0=none 1=light 2=moderate 3=severe
        l["cloud_type"] = 0;       // index into WEATHER_CLOUD_TYPES (0 = none)
        l["coverage_oktas"] = 0;   // 0/8 .. 8/8
        layers.push_back(l);
        d["weather_layers"] = layers;
    }
    d["live_weather"] = false;    // real-world weather feed (not implemented yet)
    d["visibility_km"] = 80;      // 0 .. 200 km (80 km ~ the authored fog density)
    d["qnh_hpa"] = 1013;          // sea-level pressure, hPa (mb)
    return d;
}

String SettingsStore::section_for_key(const String &key)
{
    if (key == "scenery_areas" || key == "scenery_scan_roots" || key == "installed_scenery_packs" || key == "skip_white_pixels")
    {
        return "scenery";
    }
    if (key == "render_distance" || key == "building_distance" || key == "shadow_quality" || key == "cloud_quality" || key == "tree_density" || key == "vsync" || key == "fullscreen")
    {
        return "graphics";
    }
    if (key.contains("volume"))
    {
        return "sound";
    }
    if (key == "use_system_time" || key == "time_of_day_min")
    {
        return "world";
    }
    if (key == "map_enabled" || key == "map_frame_size")
    {
        return "map";
    }
    if (key == "weather_layers" || key == "visibility_km" || key == "qnh_hpa" || key == "live_weather")
    {
        return "weather";
    }
    // per-aircraft settings (Aircraft tab): aircraft_<id>_<name>
    if (key.begins_with("aircraft_"))
    {
        return "aircraft";
    }
    return "realism";
}

Variant SettingsStore::get_setting(const String &key) const
{
    return settings.get(key, Variant());
}

void SettingsStore::set_setting(const String &key, const Variant &value)
{
    settings[key] = value;
}

bool SettingsStore::has_setting(const String &key) const
{
    return settings.has(key);
}

Dictionary SettingsStore::get_settings() const
{
    return settings;
}

void SettingsStore::restore_defaults()
{
    Dictionary fresh = get_defaults();
    for (int i = 0; i < LIBRARY_KEY_COUNT; i++)
    {
        fresh[LIBRARY_KEYS[i]] = settings.get(LIBRARY_KEYS[i], fresh[LIBRARY_KEYS[i]]);
    }
    // Mutate in place so anyone holding get_settings() keeps a live view.
    settings.clear();
    settings.merge(fresh, true);
}

// ── Persistence ─────────────────────────────────────────────────────────────

// ── Weather profiles ────────────────────────────────────────────────────────

static String _weather_profile_safe_name(const String &name)
{
    String n = name.strip_edges();
    // Keep it a plain file stem: letters, digits, space, dash, underscore, dot.
    String out;
    for (int i = 0; i < n.length(); i++)
    {
        const char32_t c = n[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '_' || c == '.' || c > 127;
        out += ok ? String::chr(c) : String("_");
    }
    return out;
}

String SettingsStore::weather_profiles_dir() const
{
    InstallConfig *ir = _install_root();
    if (ir == nullptr)
    {
        return String();
    }
    return ir->get_install_root().path_join("weather");
}

PackedStringArray SettingsStore::list_weather_profiles() const
{
    PackedStringArray out;
    const String dir = weather_profiles_dir();
    if (dir.is_empty() || !DirAccess::dir_exists_absolute(dir))
    {
        return out;
    }
    Ref<DirAccess> d = DirAccess::open(dir);
    if (d.is_null())
    {
        return out;
    }
    PackedStringArray files = d->get_files();
    for (int i = 0; i < files.size(); i++)
    {
        if (files[i].to_lower().ends_with(".json"))
        {
            out.push_back(files[i].get_basename());
        }
    }
    out.sort();
    return out;
}

bool SettingsStore::save_weather_profile(const String &name)
{
    const String safe = _weather_profile_safe_name(name);
    const String dir = weather_profiles_dir();
    if (safe.is_empty() || dir.is_empty())
    {
        return false;
    }
    DirAccess::make_dir_recursive_absolute(dir);
    Dictionary p;
    p["name"] = safe;
    p["layers"] = settings.get("weather_layers", Array());
    p["visibility_km"] = settings.get("visibility_km", 80);
    p["qnh_hpa"] = settings.get("qnh_hpa", 1013);
    Ref<FileAccess> f = FileAccess::open(dir.path_join(safe + String(".json")), FileAccess::WRITE);
    if (f.is_null())
    {
        UtilityFunctions::push_warning("SettingsStore: cannot write weather profile ", safe);
        return false;
    }
    f->store_string(JSON::stringify(p, "  "));
    f->close();
    UtilityFunctions::print("SettingsStore: weather profile saved: ", safe);
    return true;
}

bool SettingsStore::load_weather_profile(const String &name)
{
    const String dir = weather_profiles_dir();
    const String path = dir.path_join(_weather_profile_safe_name(name) + String(".json"));
    if (dir.is_empty() || !FileAccess::file_exists(path))
    {
        return false;
    }
    Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(path));
    if (parsed.get_type() != Variant::DICTIONARY)
    {
        UtilityFunctions::push_warning("SettingsStore: weather profile is not valid JSON: ", path);
        return false;
    }
    Dictionary p = parsed;
    if (p.has("layers") && Variant(p["layers"]).get_type() == Variant::ARRAY)
    {
        settings["weather_layers"] = p["layers"];
    }
    if (p.has("visibility_km"))
    {
        settings["visibility_km"] = int(p["visibility_km"]);
    }
    if (p.has("qnh_hpa"))
    {
        settings["qnh_hpa"] = int(p["qnh_hpa"]);
    }
    UtilityFunctions::print("SettingsStore: weather profile loaded: ", name);
    return true;
}

bool SettingsStore::delete_weather_profile(const String &name)
{
    const String dir = weather_profiles_dir();
    const String path = dir.path_join(_weather_profile_safe_name(name) + String(".json"));
    if (dir.is_empty() || !FileAccess::file_exists(path))
    {
        return false;
    }
    return DirAccess::remove_absolute(path) == OK;
}

InstallConfig *SettingsStore::_install_root() const
{
    SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
    if (tree == nullptr || tree->get_root() == nullptr)
    {
        return nullptr;
    }
    return Object::cast_to<InstallConfig>(tree->get_root()->get_node_or_null(NodePath("InstallRoot")));
}

void SettingsStore::load()
{
    InstallConfig *ir = _install_root();
    bool loaded = false;
    if (ir != nullptr)
    {
        for (int s = 0; s < SETTINGS_SECTION_COUNT; s++)
        {
            String section = SETTINGS_SECTIONS[s];
            PackedStringArray keys = ir->get_section_keys(section);
            for (int k = 0; k < keys.size(); k++)
            {
                // Per-aircraft settings (aircraft_<id>_*) have no defaults: any
                // such key in the [aircraft] section is loaded as is. The section
                // also holds InstallConfig's own keys (initialized, version),
                // which are not settings.
                const bool per_aircraft = section == "aircraft" && keys[k].begins_with("aircraft_");
                if (per_aircraft || settings.has(keys[k]))
                {
                    settings[keys[k]] = ir->get_value(section, keys[k], settings.get(keys[k], Variant()));
                    loaded = true;
                }
            }
        }
    }
    UtilityFunctions::print(loaded ? "Settings loaded from provpilot.cfg" : "SettingsStore: no saved settings, using defaults");

    _merge_scan_roots();

    // Honor a portable provpilot.cfg living next to the .app: if it specifies
    // a scenery_root and we have no library yet, scan it once and adopt the
    // result.
    String portable_root = ir != nullptr ? ir->get_scenery_root() : String();
    if (get_scenery_areas().is_empty() && !portable_root.is_empty() && DirAccess::dir_exists_absolute(portable_root))
    {
        UtilityFunctions::print("SettingsStore: bootstrapping library from provpilot.cfg scenery_root=", portable_root);
        add_scan_root(portable_root);
        scan_scenery();
    }
}

// Scan roots are always re-seeded from InstallRoot — even if a stale saved
// config has obsolete dev-machine paths, the user still sees sensible defaults
// when they click "Scan for Scenery". Saved roots are appended after them.
void SettingsStore::_merge_scan_roots()
{
    InstallConfig *ir = _install_root();
    PackedStringArray default_roots = ir != nullptr ? ir->get_default_scan_roots() : PackedStringArray();
    PackedStringArray saved_roots = settings.get("scenery_scan_roots", PackedStringArray());
    PackedStringArray merged;
    Dictionary seen;
    for (int i = 0; i < default_roots.size(); i++)
    {
        if (!seen.has(default_roots[i]))
        {
            seen[default_roots[i]] = true;
            merged.append(default_roots[i]);
        }
    }
    for (int i = 0; i < saved_roots.size(); i++)
    {
        if (!seen.has(saved_roots[i]))
        {
            seen[saved_roots[i]] = true;
            merged.append(saved_roots[i]);
        }
    }
    settings["scenery_scan_roots"] = merged;
}

Error SettingsStore::save()
{
    InstallConfig *ir = _install_root();
    if (ir == nullptr)
    {
        UtilityFunctions::push_warning("SettingsStore: InstallRoot not available, save skipped");
        return ERR_UNAVAILABLE;
    }
    // Per-aircraft keys no longer in the store (Restore Defaults) must not
    // come back from the file on the next load.
    PackedStringArray stale = ir->get_section_keys("aircraft");
    for (int i = 0; i < stale.size(); i++)
    {
        if (stale[i].begins_with("aircraft_") && !settings.has(stale[i]))
        {
            ir->erase_section_key("aircraft", stale[i]);
        }
    }
    Array keys = settings.keys();
    for (int i = 0; i < keys.size(); i++)
    {
        String key = keys[i];
        ir->set_value(section_for_key(key), key, settings[key]);
    }
    Error err = ir->save_config();
    if (err == OK)
    {
        UtilityFunctions::print("Settings saved to provpilot.cfg");
    }
    else
    {
        UtilityFunctions::push_warning(vformat("SettingsStore: save failed (err %d)", int(err)));
    }
    return err;
}

// ── Scenery library ─────────────────────────────────────────────────────────

Array SettingsStore::get_scenery_areas() const
{
    return settings.get("scenery_areas", Array());
}

void SettingsStore::set_scenery_areas(const Array &areas)
{
    settings["scenery_areas"] = areas;
}

static bool _priority_less(const Dictionary &a, const Dictionary &b)
{
    return int(a.get("priority", 999)) < int(b.get("priority", 999));
}

Array SettingsStore::get_enabled_scenery_areas() const
{
    Array areas = get_scenery_areas();
    std::vector<Dictionary> sorted;
    sorted.reserve(areas.size());
    for (int i = 0; i < areas.size(); i++)
    {
        sorted.push_back(areas[i]);
    }
    std::stable_sort(sorted.begin(), sorted.end(), _priority_less);
    Array out;
    for (const Dictionary &a : sorted)
    {
        if (bool(a.get("enabled", true)))
        {
            out.append(a);
        }
    }
    return out;
}

bool SettingsStore::_case_insensitive_fs()
{
    String os = OS::get_singleton()->get_name();
    return os == "macOS" || os == "Windows";
}

PackedStringArray SettingsStore::detect_assets(const String &path) const
{
    PackedStringArray out;
    if (path.is_empty())
    {
        return out;
    }
    Ref<DirAccess> dir = DirAccess::open(path);
    if (dir.is_null())
    {
        return out;
    }
    for (int i = 0; i < ASSET_SUBDIR_COUNT; i++)
    {
        if (dir->dir_exists(ASSET_SUBDIRS[i]))
        {
            out.append(ASSET_SUBDIRS[i]);
        }
    }
    return out;
}

bool SettingsStore::_is_scenery_area_dir(const String &path) const
{
    return detect_assets(path).size() > 0;
}

// If `path` IS a scenery area, record it and stop descending; otherwise
// recurse into subdirs up to max_depth.
void SettingsStore::_scan_dir_recursive(const String &path, int depth, int max_depth, PackedStringArray &found) const
{
    if (depth > max_depth)
    {
        return;
    }
    Ref<DirAccess> dir = DirAccess::open(path);
    if (dir.is_null())
    {
        return;
    }
    if (_is_scenery_area_dir(path))
    {
        found.append(path);
        return;
    }
    dir->list_dir_begin();
    String name = dir->get_next();
    while (!name.is_empty())
    {
        if (!name.begins_with(".") && dir->current_is_dir())
        {
            _scan_dir_recursive(path.path_join(name), depth + 1, max_depth, found);
        }
        name = dir->get_next();
    }
    dir->list_dir_end();
}

// Parent + leaf for disambiguation (e.g. "Germany/Baden-Wuertemberg").
String SettingsStore::_name_from_path(const String &path)
{
    String leaf = path.get_file();
    String parent = path.get_base_dir().get_file();
    if (parent.is_empty() || parent == "scenery")
    {
        return leaf;
    }
    return parent + "/" + leaf;
}

Dictionary SettingsStore::_make_area(const String &name, const String &path, int priority)
{
    Dictionary a;
    a["name"] = name;
    a["path"] = path;
    a["enabled"] = true;
    a["priority"] = priority;
    return a;
}

void SettingsStore::add_scan_root(const String &path)
{
    if (path.is_empty())
    {
        return;
    }
    PackedStringArray roots = settings.get("scenery_scan_roots", PackedStringArray());
    if (!roots.has(path))
    {
        roots.append(path);
        settings["scenery_scan_roots"] = roots;
    }
}

int SettingsStore::scan_scenery()
{
    // Effective scan root set: configured roots + parents of every known
    // scenery area (so adding one region automatically picks up its sibling
    // regions on the next scan), de-duplicated.
    PackedStringArray roots = settings.get("scenery_scan_roots", PackedStringArray());
    Array areas = get_scenery_areas();
    const bool ci = _case_insensitive_fs();
    PackedStringArray effective_roots;
    Dictionary seen_roots;
    auto push_root = [&](const String &p)
    {
        if (p.is_empty())
        {
            return;
        }
        String k = ci ? p.to_lower() : p;
        if (seen_roots.has(k))
        {
            return;
        }
        seen_roots[k] = true;
        effective_roots.append(p);
    };
    for (int i = 0; i < roots.size(); i++)
    {
        push_root(roots[i]);
    }
    for (int i = 0; i < areas.size(); i++)
    {
        String ap = String(Dictionary(areas[i]).get("path", ""));
        if (!ap.is_empty())
        {
            push_root(ap.get_base_dir());
        }
    }

    PackedStringArray found;
    for (int i = 0; i < effective_roots.size(); i++)
    {
        _scan_dir_recursive(effective_roots[i], 0, 3, found);
    }

    // Merge with existing entries (preserve enabled/priority for known paths).
    Array existing = areas.duplicate();
    Dictionary existing_paths;
    for (int i = 0; i < existing.size(); i++)
    {
        existing_paths[Dictionary(existing[i]).get("path", "")] = true;
    }

    // Sort newly found paths by asset count DESC so a "complete" region (e.g.
    // Switzerland with terrain+ortho+buildings+...) gets priority before a
    // partial region (e.g. BW with only a terrain dir). This matters because
    // the aircraft's spawn region must be in the priority-1 path or tile
    // loading silently fails.
    std::vector<std::pair<int, String>> new_paths;
    for (int i = 0; i < found.size(); i++)
    {
        if (existing_paths.has(found[i]))
        {
            continue;
        }
        new_paths.emplace_back(detect_assets(found[i]).size(), found[i]);
    }
    std::stable_sort(new_paths.begin(), new_paths.end(), [](const std::pair<int, String> &a, const std::pair<int, String> &b)
                     { return a.first > b.first; });

    int added = 0;
    int next_priority = existing.size() + 1;
    for (const auto &entry : new_paths)
    {
        existing.append(_make_area(_name_from_path(entry.second), entry.second, next_priority));
        next_priority++;
        added++;
    }

    // Sort by priority for display.
    std::vector<Dictionary> sorted;
    for (int i = 0; i < existing.size(); i++)
    {
        sorted.push_back(existing[i]);
    }
    std::stable_sort(sorted.begin(), sorted.end(), _priority_less);
    Array result;
    for (const Dictionary &a : sorted)
    {
        result.append(a);
    }
    settings["scenery_areas"] = result;
    UtilityFunctions::print(vformat("Scenery scan: %d new area(s) added (%d total)", added, result.size()));
    return added;
}

int SettingsStore::add_scenery_area(const String &path)
{
    if (path.is_empty())
    {
        return 0;
    }
    Array areas = get_scenery_areas().duplicate();
    for (int i = 0; i < areas.size(); i++)
    {
        if (String(Dictionary(areas[i]).get("path", "")) == path)
        {
            UtilityFunctions::print("Scenery area already in library: ", path);
            return 0;
        }
    }

    // If `path` is itself a region directory (contains terrain/, buildings/,
    // ...), add it directly. Otherwise treat it as a *parent* directory and
    // look for region subdirs (e.g. the user picks `I:/provpilot/scenery`
    // which contains `Switzerland/`, `Germany/`, ...). Also register the
    // parent as a scan root so future Scan presses keep finding new siblings.
    int added = 0;
    if (_is_scenery_area_dir(path))
    {
        areas.append(_make_area(_name_from_path(path), path, areas.size() + 1));
        added = 1;
    }
    else
    {
        PackedStringArray found;
        _scan_dir_recursive(path, 0, 3, found);
        Dictionary existing_paths;
        for (int i = 0; i < areas.size(); i++)
        {
            existing_paths[Dictionary(areas[i]).get("path", "")] = true;
        }
        // Sort by asset richness so the most complete region wins priority.
        std::vector<std::pair<int, String>> regions;
        for (int i = 0; i < found.size(); i++)
        {
            regions.emplace_back(detect_assets(found[i]).size(), found[i]);
        }
        std::stable_sort(regions.begin(), regions.end(), [](const std::pair<int, String> &a, const std::pair<int, String> &b)
                         { return a.first > b.first; });
        for (const auto &r : regions)
        {
            if (existing_paths.has(r.second))
            {
                continue;
            }
            areas.append(_make_area(_name_from_path(r.second), r.second, areas.size() + 1));
            added++;
        }
        add_scan_root(path);
        if (added == 0)
        {
            // Fall back to adding the dir itself so the user isn't left with nothing.
            UtilityFunctions::push_warning("Scenery area ", path, String::utf8(" does not contain any known asset subdirs — adding anyway"));
            areas.append(_make_area(_name_from_path(path), path, areas.size() + 1));
            added = 1;
        }
        else
        {
            UtilityFunctions::print(vformat("Add scenery: discovered %d region(s) under %s", added, path));
        }
    }
    settings["scenery_areas"] = areas;
    return added;
}

bool SettingsStore::remove_scenery_area(int idx)
{
    Array areas = get_scenery_areas().duplicate();
    if (idx < 0 || idx >= areas.size())
    {
        return false;
    }
    areas.remove_at(idx);
    settings["scenery_areas"] = areas;
    return true;
}

bool SettingsStore::swap_scenery_priority(int a_idx, int b_idx)
{
    Array areas = get_scenery_areas().duplicate();
    if (a_idx < 0 || a_idx >= areas.size() || b_idx < 0 || b_idx >= areas.size())
    {
        return false;
    }
    Variant tmp = areas[a_idx];
    areas[a_idx] = areas[b_idx];
    areas[b_idx] = tmp;
    // Renumber priorities to match list order.
    for (int i = 0; i < areas.size(); i++)
    {
        Dictionary a = areas[i];
        a["priority"] = i + 1;
    }
    settings["scenery_areas"] = areas;
    return true;
}

void SettingsStore::set_area_enabled(int idx, bool enabled)
{
    Array areas = get_scenery_areas();
    if (idx >= 0 && idx < areas.size())
    {
        Dictionary a = areas[idx];
        a["enabled"] = enabled;
    }
}

void SettingsStore::set_area_priority(int idx, int priority)
{
    Array areas = get_scenery_areas();
    if (idx >= 0 && idx < areas.size())
    {
        Dictionary a = areas[idx];
        a["priority"] = priority;
    }
}

String SettingsStore::missing_scenery_reason() const
{
    Array areas = get_enabled_scenery_areas();
    if (areas.is_empty())
    {
        return String::utf8("No scenery areas are configured.\n\nOpen Settings → Scenery and add the directory that contains your scenery data (e.g. the folder with Switzerland/, Germany/, …).");
    }
    PackedStringArray missing_paths;
    bool has_any_data = false;
    for (int i = 0; i < areas.size(); i++)
    {
        String base = String(Dictionary(areas[i]).get("path", ""));
        if (base.is_empty())
        {
            continue;
        }
        if (!DirAccess::dir_exists_absolute(base))
        {
            missing_paths.append(base);
            continue;
        }
        // A region is usable if it has terrain or orthophoto tiles.
        if (DirAccess::dir_exists_absolute(base.path_join("terrain")) || DirAccess::dir_exists_absolute(base.path_join("orthophoto")))
        {
            has_any_data = true;
        }
    }
    if (has_any_data)
    {
        return "";
    }
    if (missing_paths.size() > 0)
    {
        return "The configured scenery folder(s) do not exist on disk:\n\n  " + String("\n  ").join(missing_paths) + String::utf8("\n\nOpen Settings → Scenery to update the paths.");
    }
    return String::utf8("The configured scenery folder(s) contain no terrain or orthophoto tiles.\n\nExtract the scenery ZIPs into the folder you registered in Settings → Scenery, then click Scan.");
}

// Collapse $HOME to "~" (keeping the original casing so ~/provpilot vs
// ~/ProvPilot stay distinguishable); beyond max_chars elide the middle,
// keeping the first 2 and last 3 components (~/.../scenery/Germany/BW).
String SettingsStore::format_path_for_display(const String &path, int max_chars)
{
    if (path.is_empty())
    {
        return "";
    }
    String p = path;
    String home = OS::get_singleton()->get_environment("HOME");
    if (!home.is_empty() && p.begins_with(home + "/"))
    {
        p = "~" + p.substr(home.length());
    }
    else if (!home.is_empty() && p == home)
    {
        p = "~";
    }
    if (p.length() <= max_chars)
    {
        return p;
    }
    PackedStringArray parts = p.split("/", false);
    if (parts.size() <= 5)
    {
        return p;
    }
    String head = parts[0] + "/" + parts[1];
    String tail = parts[parts.size() - 3] + "/" + parts[parts.size() - 2] + "/" + parts[parts.size() - 1];
    if (p.begins_with("/"))
    {
        head = "/" + head;
    }
    return head + String::utf8("/…/") + tail;
}

// ── Apply to scene ──────────────────────────────────────────────────────────

bool SettingsStore::_has_property(Object *obj, const String &name)
{
    TypedArray<Dictionary> props = obj->get_property_list();
    for (int i = 0; i < props.size(); i++)
    {
        if (String(Dictionary(props[i]).get("name", "")) == name)
        {
            return true;
        }
    }
    return false;
}

// For each enabled area (priority order) append every asset subdir that
// exists to the matching Scenery3D paths array. Example: "Switzerland"
// (priority 1) and "Germany/Baden-Wuertemberg" (priority 2), where BW only has
// orthophoto/, gives orthophoto_paths = [CH/orthophoto, BW/orthophoto] — Swiss
// tiles first, German tiles as fallback (with skip_white_pixels this fixes the
// CH/DE border).
void SettingsStore::apply_scenery_sources(Node *scenery3d, Node *vegetation, Node *vfr_map)
{
    if (scenery3d == nullptr)
    {
        return;
    }
    Array areas = get_enabled_scenery_areas();
    if (areas.is_empty())
    {
        UtilityFunctions::print(String::utf8("Scenery sources: no enabled scenery areas in library — clearing Scenery3D paths"));
    }

    PackedStringArray terrain_paths, ortho_paths, building_paths, water_paths, road_paths, vegetation_paths, map_paths;
    // macOS APFS / Windows are case-insensitive — two areas that differ only
    // in path case refer to the same data, so dedup case-insensitively to
    // avoid loading every tile twice.
    const bool ci = _case_insensitive_fs();
    Dictionary seen;
    auto append_unique = [&](PackedStringArray &arr, const String &p)
    {
        String k = ci ? p.to_lower() : p;
        if (!seen.has(k))
        {
            seen[k] = true;
            arr.append(p);
        }
    };
    struct AssetDir
    {
        const char *subdir;
        PackedStringArray *target;
    };
    AssetDir asset_dirs[] = {
        { "terrain", &terrain_paths },
        { "orthophoto", &ortho_paths },
        { "buildings", &building_paths },
        { "water", &water_paths },
        { "roads", &road_paths },
        { "vegetation", &vegetation_paths },
        { "map", &map_paths },
    };
    for (int i = 0; i < areas.size(); i++)
    {
        String base = String(Dictionary(areas[i]).get("path", ""));
        if (base.is_empty())
        {
            continue;
        }
        if (!DirAccess::dir_exists_absolute(base))
        {
            UtilityFunctions::push_warning("Scenery area path does not exist (skipped): ", base);
            continue;
        }
        for (const AssetDir &ad : asset_dirs)
        {
            String p = base.path_join(ad.subdir);
            if (DirAccess::dir_exists_absolute(p))
            {
                append_unique(*ad.target, p);
            }
        }
    }

    // Always assign — even empty arrays. Otherwise stale paths baked into the
    // scene file or ProjectSettings (scenery3d/*) would survive when the
    // runtime library has nothing. Do NOT also set the single-path properties
    // (data_path, orthophoto_path): their setters CLEAR the array first.
    // buildings_path stays as a legacy fallback for older Scenery3D builds.
    if (vfr_map != nullptr)
    {
        vfr_map->call("set_tile_paths", map_paths);
    }
    scenery3d->set("data_paths", terrain_paths);
    scenery3d->set("orthophoto_paths", ortho_paths);
    if (scenery3d->has_method("set_buildings_paths") || _has_property(scenery3d, "buildings_paths"))
    {
        scenery3d->set("buildings_paths", building_paths);
    }
    else if (building_paths.size() > 0)
    {
        scenery3d->set("buildings_path", building_paths[0]);
    }
    scenery3d->set("water_paths", water_paths);
    scenery3d->set("road_paths", road_paths);
    if (vegetation != nullptr)
    {
        vegetation->set("vegetation_paths", vegetation_paths);
    }

    UtilityFunctions::print(vformat("Scenery library applied: %d enabled area(s)", areas.size()));
    UtilityFunctions::print("  terrain_paths    = ", terrain_paths);
    UtilityFunctions::print("  orthophoto_paths = ", ortho_paths);
    UtilityFunctions::print("  buildings_paths  = ", building_paths);
    UtilityFunctions::print("  water_paths      = ", water_paths);
    UtilityFunctions::print("  road_paths       = ", road_paths);
    UtilityFunctions::print("  vegetation_paths = ", vegetation_paths);
    UtilityFunctions::print("  map_paths        = ", map_paths);

    // White-pixel skip (CH/DE border fix) — only effective once the C++ side ships it.
    bool skip_white = bool(settings.get("skip_white_pixels", true));
    if (scenery3d->has_method("set_skip_white_pixels"))
    {
        scenery3d->call("set_skip_white_pixels", skip_white);
        UtilityFunctions::print("  skip_white_pixels = ", skip_white);
    }
    else if (_has_property(scenery3d, "skip_white_pixels"))
    {
        scenery3d->set("skip_white_pixels", skip_white);
        UtilityFunctions::print("  skip_white_pixels = ", skip_white);
    }

    // Terrain Distance (render_distance, in tiles) -> Scenery3D.load_radius.
    // Keep the radius modest: the low-res far-terrain CHUNK panorama fills
    // everything beyond it out to far_radius, so a small value (default 8 =
    // ~8 km of full-detail tiles) keeps streaming fast and memory bounded.
    int render_distance = std::max(2, std::min(16, int(settings.get("render_distance", 8))));
    if (_has_property(scenery3d, "load_radius"))
    {
        scenery3d->set("load_radius", render_distance);
        UtilityFunctions::print(vformat("  load_radius (terrain distance) = %d tiles", render_distance));
    }
}

void SettingsStore::apply_tree_density(Node *vegetation)
{
    if (vegetation == nullptr)
    {
        return;
    }
    int density_idx = std::max(0, std::min(4, int(settings.get("tree_density", 2))));
    double fraction = TREE_DENSITY_FRACTION[density_idx];
    vegetation->set("density_fraction", fraction);
    vegetation->set("load_radius_m", TREE_LOAD_RADIUS_M);
    vegetation->set("visible", fraction > 0.0);
    UtilityFunctions::print(vformat("Tree density: index=%d fraction=%.2f radius=%d m", density_idx, fraction, TREE_LOAD_RADIUS_M));
}

String SettingsStore::format_hhmm(int minutes)
{
    return vformat("%02d:%02d", minutes / 60, minutes % 60);
}

} // namespace jsbgodot
