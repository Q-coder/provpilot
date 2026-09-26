#ifndef AIRCRAFT_CONFIG_H
#define AIRCRAFT_CONFIG_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace jsbgodot
{

    // Parser for ProvPilot `aircraft.cfg` files (formerly aircraft_config.gd).
    //
    // The format is a lightweight INI-like dialect (NOT Godot's ConfigFile):
    //
    //     [section]
    //         key "quoted string value"
    //         key bare_value          # numbers, true/false
    //         key "a, b, c"           # comma list (read via get_*_array)
    //         # full-line or trailing comment
    //
    // Use load_from_file() to obtain an instance, then the typed getters
    // (get_string, get_float, ...) to read values. Every getter takes a default
    // so callers degrade gracefully when a key is missing. Values are stored as
    // raw strings; the typed getters convert on read.
    //
    // The class also owns the per-airframe path lookup shared by the model
    // loader, the aircraft rig and the settings panel:
    //   resolve_path(id)              -> the aircraft.cfg to read for <id>
    //   resolve_aircraft_file(id, rel) -> a data file next to it (fg manifests, sounds)
    class AircraftConfig : public RefCounted
    {
        GDCLASS(AircraftConfig, RefCounted);

    public:
        static void _bind_methods();
        AircraftConfig();

        // Loads and parses the file at `path` (absolute or res://). The returned
        // config's `loaded` flag reflects success; on failure the instance is
        // still usable (every getter returns its default).
        static Ref<AircraftConfig> load_from_file(const String &path);

        // Config path for an aircraft id. In the editor the res:// source tree
        // wins so live edits apply on relaunch; otherwise the writable install
        // tree (<install_root>/aircraft/<id>/aircraft.cfg) with the bundled
        // res:// copy as the last resort.
        static String resolve_path(const String &aircraft_id);
        // A data file shipped with the aircraft (relative to its directory):
        // the install tree first, else the bundled res:// copy (globalized).
        static String resolve_aircraft_file(const String &aircraft_id, const String &rel);
        // Loads the config for any aircraft id (null-free: an unloaded config
        // when the file is missing).
        static Ref<AircraftConfig> load_for_aircraft(const String &aircraft_id);

        // Parses an axis name (x/y/z, optionally prefixed with - to invert the
        // sweep direction) into a unit Vector3; `fallback` on bad input.
        static Vector3 parse_axis(const String &name, const Vector3 &fallback);

        // Parses raw config text into the section table (replaces any previous
        // content).
        void parse_text(const String &text);

        bool has_value(const String &section, const String &key) const;
        String get_string(const String &section, const String &key, const String &default_value = String()) const;
        double get_float(const String &section, const String &key, double default_value = 0.0) const;
        int get_int(const String &section, const String &key, int default_value = 0) const;
        // Accepts true/1/yes/on (case-insensitive) as true.
        bool get_bool(const String &section, const String &key, bool default_value = false) const;
        // Comma-separated list as floats.
        Array get_float_array(const String &section, const String &key, const Array &default_value = Array()) const;
        // Comma-separated list as trimmed strings.
        Array get_string_array(const String &section, const String &key, const Array &default_value = Array()) const;
        // A three-float list as a Vector3 (default when absent or malformed).
        Vector3 get_vector3(const String &section, const String &key, const Vector3 &default_value = Vector3()) const;
        // An axis key through parse_axis().
        Vector3 get_axis(const String &section, const String &key, const Vector3 &fallback) const;
        // The whole section as { key: String } (empty if absent).
        Dictionary get_section(const String &section) const;

        bool is_loaded() const;
        void set_loaded(bool loaded);
        String get_source_path() const;
        void set_source_path(const String &path);
        Dictionary get_sections() const;
        void set_sections(const Dictionary &sections);

    protected:
        // { section_name: { key: String } }
        Dictionary sections;
        String source_path;
        bool loaded = false;

        static String parse_value(const String &rest);
        static int first_whitespace(const String &s);
    };

} // namespace jsbgodot

#endif
