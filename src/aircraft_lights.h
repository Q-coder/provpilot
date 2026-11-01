#ifndef AIRCRAFT_LIGHTS_H
#define AIRCRAFT_LIGHTS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/templates/vector.hpp>

#include "aircraft_config.h"

using namespace godot;

namespace jsbgodot
{

    // Exterior lights of the flown aircraft (docs/AIRCRAFT_CONFIG.md, [lights];
    // formerly aircraft_lights.gd).
    //
    // Every fixture is a point sprite (shaders/aircraft_light.gdshader)
    // parented to the model mesh node the config names, at that mesh's
    // bounding-box centre pushed OUT_OFFSET outward, so it follows the
    // airframe and its animations. Landing and taxi lights also get a
    // SpotLight3D that lights the ground; it lives under this node (unscaled)
    // and follows the fixture every frame. A fixture node "<x>-on" with a
    // sibling "<x>-off" is a lit / unlit mesh pair whose visibility follows
    // the light (unless cfg pair_meshes is false).
    //
    // States: navigation lights and the beacon are on whenever the bus is
    // powered; strobes come on when airborne; landing and taxi lights are
    // toggled with L by the host and go out while the gear is retracted.
    class AircraftLights : public Node3D
    {
        GDCLASS(AircraftLights, Node3D);

    public:
        static void _bind_methods();
        AircraftLights();

        // Builds the fixtures for the model under model_root from the cfg
        // [lights] section; ac is the aircraft frame node (nose +Z, +X left,
        // +Y up) used for the outward offsets. Safe to call again after a swap.
        void setup(Node *model_root, const Ref<AircraftConfig> &cfg, Node3D *ac);
        void clear();
        void set_landing_lights(bool on);
        // A fixture's level from the aircraft's own systems (the MD-80's
        // exterior-light and cockpit flood properties): the i-th fixture of a kind.
        void set_fixture_level(const String &kind, int index, double level);
        double get_fixture_level(const String &kind, int index) const;
        int fixture_count_of(const String &kind) const;
        bool landing_lights_on() const;
        bool toggle_landing_lights();
        int fixture_count(const String &kind = String()) const;
        // Whether a fixture class is currently lit (strobes / beacon: now).
        bool is_lit(const String &kind) const;
        // The fixtures as {kind, sprite, anchor, spot} dictionaries (tests).
        Array get_fixtures() const;
        // Per frame: airborne = no weight on wheels, gear_down = fully or
        // partly down, night 0..1, forward / down = aircraft axes in world
        // space for aiming the landing spot.
        void update(double delta, bool airborne, bool gear_down, double night, const Vector3 &forward, const Vector3 &down);

        bool get_nav_enabled() const; void set_nav_enabled(bool v);
        bool get_strobe_enabled() const; void set_strobe_enabled(bool v);
        bool get_beacon_enabled() const; void set_beacon_enabled(bool v);
        bool get_taxi_enabled() const; void set_taxi_enabled(bool v);
        bool get_powered() const; void set_powered(bool v);
        bool get_debug_all_on() const; void set_debug_all_on(bool v);

    protected:
        struct Fixture
        {
            String kind;
            MeshInstance3D *sprite = nullptr;
            Ref<ShaderMaterial> mat;
            Node3D *on_mesh = nullptr;
            Node3D *off_mesh = nullptr;
            SpotLight3D *spot = nullptr;
            OmniLight3D *omni = nullptr; // flood: a cockpit light
            double level = -1.0;         // set_fixture_level: 0..1 from an aircraft property, -1 = the built-in logic
            Node3D *anchor = nullptr;
            uint64_t anchor_id = 0;
            Vector3 centre; // sprite vertex in anchor space
            double energy = 0.0;
        };
        static constexpr double STROBE_PERIOD_S = 1.2; // double flash every 1.2 s
        static constexpr double STROBE_FLASH_S = 0.06;
        static constexpr double BEACON_PERIOD_S = 1.0;
        static constexpr double BEACON_FLASH_S = 0.18;
        static constexpr double NIGHT_FLOOR = 0.45; // sprite brightness by day
        static constexpr double OUT_OFFSET = 0.10;  // outward offset in model units
        static constexpr double PULL_FACTOR = 0.12; // shader pull toward the camera x model scale

        Ref<Shader> shader;
        Vector<Fixture> fixtures;
        double time = 0.0;
        bool landing_on = false;
        double night = 0.0;
        bool airborne = false;
        bool gear_down = true;
        double landing_pitch_deg = 5.0;
        double landing_range_m = 300.0;
        double taxi_range_m = 90.0;
        Vector3 forward = Vector3(0, 0, -1);
        Vector3 down = Vector3(0, -1, 0);
        double spot_energy = 12.0;
        double flood_range_m = 2.0;  // [lights] flood_range_m / flood_energy: cockpit floods
        double flood_energy = 1.2;
        Node3D *ac = nullptr;
        bool debug_all_on = false;
        bool nav_enabled = true;
        bool strobe_enabled = true;
        bool beacon_enabled = true;
        bool taxi_enabled = true;
        bool powered = true;
        bool pair_meshes = true;

        static Color colour_of(const String &kind);
        static double size_of(const String &kind);
        void ride_on(Node3D *lens, Node3D *part, Node *model_root);
        static double aabb_distance(const AABB &box, const Vector3 &p);
        void add_fixture(const String &kind, Node3D *anchor);
        void apply_states();
        static Node3D *off_sibling(Node3D *anchor);
    };

} // namespace jsbgodot

#endif
