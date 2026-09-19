#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "install_config.h"

using namespace godot;

namespace jsbgodot
{

    // User settings model + scenery library logic (formerly settings_screen.gd)
    // so the UI (SettingsScreen) and node_3d.gd share one source of truth
    // without any GDScript in between.
    //
    // Values live in a flat Dictionary keyed by setting name; each key is routed
    // to a provpilot.cfg section by section_for_key(). Persistence goes through
    // the InstallRoot autoload (C++ InstallConfig) so the in-memory ConfigFile
    // it shares with JoystickConfig / AircraftInstaller never goes stale.
    //
    // Scenery library (FSX-style): `scenery_areas` is an Array of
    //   {name: String, path: String, enabled: bool, priority: int}
    // Lower priority number = checked first (1 = highest). Each path is a
    // region directory whose asset subdirs (terrain/, orthophoto/, ...) are
    // auto-detected. apply_scenery_sources() turns the enabled areas into the
    // per-asset path arrays Scenery3D / Vegetation / VfrMap consume.
    class SettingsStore : public RefCounted
    {
        GDCLASS(SettingsStore, RefCounted);

    public:
        static void _bind_methods();
        SettingsStore();

        // ── Values ──
        Variant get_setting(const String &key) const;
        void set_setting(const String &key, const Variant &value);
        bool has_setting(const String &key) const;
        // The live settings Dictionary (shared reference, not a copy).
        Dictionary get_settings() const;
        // Reset preferences to defaults. The scenery library (areas, scan roots,
        // installed packs) is user data, not a preference, and is kept.
        void restore_defaults();
        static Dictionary get_defaults();
        static String section_for_key(const String &key);

        // ── Persistence (InstallRoot -> provpilot.cfg) ──
        void load();
        Error save();

        // ── Scenery library ──
        Array get_scenery_areas() const;
        void set_scenery_areas(const Array &areas);
        // Enabled areas sorted by priority ascending.
        Array get_enabled_scenery_areas() const;
        // Scan every scan root (+ parents of known areas) for region dirs.
        // Returns the number of areas added.
        int scan_scenery();
        // Add a region dir, or scan a parent dir for regions. Returns count added.
        int add_scenery_area(const String &path);
        bool remove_scenery_area(int idx);
        // Swap two rows and renumber priorities to list order.
        bool swap_scenery_priority(int a_idx, int b_idx);
        void set_area_enabled(int idx, bool enabled);
        void set_area_priority(int idx, int priority);
        void add_scan_root(const String &path);
        // Asset subdirs present inside a region dir.
        PackedStringArray detect_assets(const String &path) const;
        // "" when at least one enabled area has terrain/orthophoto data on
        // disk; otherwise a human-readable reason for an error dialog.
        String missing_scenery_reason() const;
        // Compact path for tables: $HOME -> ~, middle-elided beyond max_chars.
        static String format_path_for_display(const String &path, int max_chars);

        // ── Apply to scene (node_3d.gd hands over the nodes) ──
        // Push per-asset path arrays + skip_white_pixels + load_radius into
        // Scenery3D, vegetation_paths into Vegetation, map/ dirs into VfrMap.
        void apply_scenery_sources(Node *scenery3d, Node *vegetation, Node *vfr_map);

        // ── Weather profiles ─────────────────────────────────────────────
        // Named files under <install_root>/weather/<name>.json holding the
        // static weather (layers, visibility, QNH) so a setup survives the
        // session and can be swapped in one click.
        String weather_profiles_dir() const;
        PackedStringArray list_weather_profiles() const;
        bool save_weather_profile(const String &name);   // from the store's current values
        bool load_weather_profile(const String &name);   // into the store's values
        bool delete_weather_profile(const String &name);
        // tree_density index -> Vegetation density_fraction / load_radius_m.
        void apply_tree_density(Node *vegetation);

        static String format_hhmm(int minutes);

    protected:
        static const char *ASSET_SUBDIRS[];
        static const int ASSET_SUBDIR_COUNT;
        // Off / Low / Medium / High / Very High
        static const double TREE_DENSITY_FRACTION[5];
        // Fixed render radius (m) when vegetation is enabled. Density controls
        // the number of trees within this radius, not the radius itself.
        static constexpr int TREE_LOAD_RADIUS_M = 1500;

        Dictionary settings;

        // The InstallRoot autoload (InstallConfig), or nullptr outside a tree.
        InstallConfig *_install_root() const;
        static bool _case_insensitive_fs();
        static bool _has_property(Object *obj, const String &name);
        bool _is_scenery_area_dir(const String &path) const;
        void _scan_dir_recursive(const String &path, int depth, int max_depth, PackedStringArray &found) const;
        static String _name_from_path(const String &path);
        static Dictionary _make_area(const String &name, const String &path, int priority);
        void _merge_scan_roots();
    };

} // namespace jsbgodot

#endif
