#ifndef INSTALL_CONFIG_H
#define INSTALL_CONFIG_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace jsbgodot
{

    // Discovers where ProvPilot is installed and owns the shared provpilot.cfg.
    // Mounted as the `InstallRoot` autoload (install_root.tscn, one node of
    // this type) so any script can call `InstallRoot.get_install_root()` etc.
    // Formerly install_root.gd.
    //
    // ProvPilot follows the X-Plane "portable install" model: the user copies
    // the .app into a folder of their choice, then on first launch a wizard
    // asks for the scenery location. The result is persisted to
    // `<install_root>/provpilot.cfg` so the install is self-contained and can
    // be moved to another machine simply by copying the folder.
    //
    // Discovery order (first hit wins):
    //   1. Directory containing the .app bundle (macOS) or the executable
    //      (Windows / Linux) — in shipped builds this IS the install root.
    //   2. $PWD — the working directory the user launched from.
    //   3. ~/ProvPilot — common user-chosen location.
    //   4. res:// — editor / dev fallback.
    // In the editor, the first candidate that contains a provpilot.cfg (or,
    // if none exists, the first writable candidate) becomes the install root.
    //
    // All app config (scenery library, aircraft extraction state, joystick
    // bindings, user settings) lives in this single ConfigFile; other modules
    // stage values with set_value() and flush with save_config().
    class InstallConfig : public Node
    {
        GDCLASS(InstallConfig, Node);

    public:
        static void _bind_methods();
        InstallConfig();

        void _ready() override;

        // ── Install location ──
        String get_install_root() const;
        String get_config_path() const;
        bool has_config() const;
        // True when there's no config, or first_run_done is unset AND no
        // scenery root is configured.
        bool is_first_run() const;

        // ── [scenery] ──
        // Configured scenery root (dir containing region subdirs), or "".
        String get_scenery_root() const;
        // Enabled region subdir names in priority order; empty = use all.
        PackedStringArray get_regions() const;
        // Plausible roots for "Scan for Scenery" (configured root, next to
        // the install, home candidates, /Volumes/*), de-duplicated.
        PackedStringArray get_default_scan_roots() const;
        // Persist the first-run choice and mark first_run_done.
        Error save_scenery_choice(const String &scenery_root, const PackedStringArray &regions);

        // ── [aircraft] ──
        // Writable per-airframe data tree: <install_root>/aircraft/<id>/...
        String get_aircraft_root() const;
        String get_aircraft_dir(const String &aircraft_id) const;
        // True once the bundled aircraft tree was extracted for `version`.
        bool is_aircraft_initialized(const String &version) const;
        Error mark_aircraft_initialized(const String &version);

        // ── Generic config API ──
        Variant get_value(const String &section, const String &key, const Variant &default_value) const;
        void set_value(const String &section, const String &key, const Variant &value);
        bool has_section(const String &section) const;
        bool has_section_key(const String &section, const String &key) const;
        PackedStringArray get_sections() const;
        PackedStringArray get_section_keys(const String &section) const;
        void erase_section(const String &section);
        void erase_section_key(const String &section, const String &key);
        Error save_config();

    protected:
        static constexpr const char *CONFIG_FILENAME = "provpilot.cfg";
        static constexpr const char *SEC_SCENERY = "scenery";
        static constexpr const char *KEY_SCENERY_ROOT = "scenery_root";
        static constexpr const char *KEY_REGIONS = "regions";
        static constexpr const char *KEY_FIRST_RUN_DONE = "first_run_done";
        static constexpr const char *SEC_AIRCRAFT = "aircraft";
        static constexpr const char *KEY_AIRCRAFT_INITIALIZED = "initialized";
        static constexpr const char *KEY_AIRCRAFT_VERSION = "version";
        static constexpr const char *AIRCRAFT_SUBDIR = "aircraft";

        String install_root;
        String config_path;
        Ref<ConfigFile> config;
        bool loaded = false;

        void _discover_and_load();
        PackedStringArray _candidate_roots() const;
        static bool _is_writable(const String &path);
        static bool _case_insensitive_fs();
        Error _save(const char *what);
        void _migrate_legacy_install_cfg(const PackedStringArray &candidates);
        void _migrate_legacy_user_configs();
        bool _has_any_section_with_prefix(const String &prefix) const;
        bool _import_legacy_cfg(const String &legacy_path);
    };

} // namespace jsbgodot

#endif
