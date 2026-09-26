#ifndef AIRCRAFT_INSTALLER_H
#define AIRCRAFT_INSTALLER_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    // On first run, extracts the bundled per-airframe data (JSBSim XMLs and
    // the on-disk assets each aircraft needs) from res://aircraft/ to the
    // writable <install_root>/aircraft/ tree (formerly aircraft_installer.gd;
    // the `AircraftInstaller` autoload from aircraft_installer.tscn).
    //
    // JSBSim is a native library that loads its files from the OS filesystem,
    // not res://, so the curated tree ships inside the PCK and is copied out
    // on first launch, which also gives users a real folder they can modify or
    // add airframes to. The extraction is keyed by BUNDLE_VERSION: bump it
    // when the bundled data changes and existing installs re-extract.
    class AircraftInstaller : public Node
    {
        GDCLASS(AircraftInstaller, Node);

    public:
        static void _bind_methods();
        AircraftInstaller();

        void _ready() override;
        static String get_bundle_version();

    protected:
        static constexpr const char *BUNDLE_VERSION = "1.0.11";
        static constexpr const char *RES_AIRCRAFT_ROOT = "res://aircraft";

        Error copy_tree(const String &src_res, const String &dest_abs);
        static Error copy_file(const String &src_res, const String &dest_abs);
        void copy_required_files(const String &dest_root);
        static bool required_files_present(const String &dest_root);
    };

} // namespace jsbgodot

#endif
