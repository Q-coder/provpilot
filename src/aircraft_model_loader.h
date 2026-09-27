#ifndef AIRCRAFT_MODEL_LOADER_H
#define AIRCRAFT_MODEL_LOADER_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include "aircraft_config.h"

using namespace godot;

namespace jsbgodot
{

    // Runtime loader for the aircraft's 3D model (formerly c172p_loader.gd),
    // the `C172P_1` node of C172p.tscn under which node_3d.gd's model root
    // path resolves.
    //
    // The GLB is read from disk (X-Plane style, editable post-install):
    //     <install_root>/aircraft/<id>/<cfg [model] file>   (default model/<id>.glb)
    // which AircraftInstaller extracts from the PCK on first run; the bundled
    // PackedScene (cfg [model] res_file) is the fallback. The GLB's top-level
    // children are reparented under this node so the scene graph the config's
    // node paths refer to stays intact, with the optional cfg [model] fitting
    // transform (scale / scale_xyz / offset / rotation_deg) applied.
    class AircraftModelLoader : public Node3D
    {
        GDCLASS(AircraftModelLoader, Node3D);

    public:
        static void _bind_methods();
        AircraftModelLoader();

        // Loads the default aircraft; a child's _ready runs before the
        // parent's, so the main scene can resolve model paths in its own.
        void _ready() override;

        // (Re)loads the model for an aircraft id, replacing the attached
        // subtree; a swap to the loaded id is a no-op. Returns true on success.
        bool load_aircraft_model(const String &aircraft_id);
        String get_loaded_aircraft_id() const;

    protected:
        String loaded_aircraft_id;
        // The scene's pristine transform of this node; every load composes
        // the aircraft's fitting transform onto it.
        Transform3D base_transform;
        bool base_captured = false;

        Node *load_from_disk(const String &aircraft_id, const String &rel_path) const;
        Node *load_from_resource(const String &res_path) const;
        static Transform3D model_transform(const Ref<AircraftConfig> &cfg);
    };

} // namespace jsbgodot

#endif
