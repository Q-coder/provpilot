#ifndef EXHAUST_EFFECTS_H
#define EXHAUST_EFFECTS_H

#include <deque>
#include <vector>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include "aircraft_config.h"

namespace jsbgodot
{
    class JSBGodot;

    // Engine exhaust effects from aircraft.cfg [exhaust]:
    //   * heat shimmer: the picture behind each nozzle is refracted through a
    //     plume of hot gas (a cone of scrolling noise that bends the screen
    //     texture), stronger with the engine's power;
    //   * contrails: where the outside air is colder than about -40 degC the
    //     exhaust's water condenses. Each nozzle leaves puffs that stay where
    //     they were made (ECEF world, double positions), grow, thin out and
    //     fade over a minute.
    //
    //   [exhaust]
    //     nozzles   "right, up, forward; ..."  metres from the aircraft node
    //     radius_m  0.40                       nozzle radius
    //     engines   "0, 1"                     FDM engine per nozzle (default: in order)
    //     heat      true
    //     contrail  true
    //
    // The node processes late in the frame (after the FDM has moved the
    // aircraft), so the plumes sit on the nozzles without a frame of lag.
    class ExhaustEffects : public godot::Node3D
    {
        GDCLASS(ExhaustEffects, godot::Node3D)

    public:
        void setup(godot::Node3D *ac_node, godot::Object *jsb);
        void configure(const godot::Ref<AircraftConfig> &cfg);
        void clear();
        void _ready() override;
        void _process(double delta) override;

        // Tests / demos: contrails regardless of the air temperature.
        void set_force_contrail(bool on);
        bool get_force_contrail() const;
        void set_enabled(bool on);
        bool get_enabled() const;
        int get_nozzle_count() const;
        int get_puff_count() const;
        double get_contrail_factor() const; // 0..1 from the air temperature (and the override)
        double get_heat_power(int nozzle) const;

    protected:
        static void _bind_methods();

    private:
        struct Puff
        {
            godot::Vector3 pos; // ECEF
            double born = 0.0;
            uint32_t n = 0;
            float density = 1.0f;
            float seed = 0.0f;
        };
        struct Nozzle
        {
            godot::Vector3 pos_m; // right, up, forward from the aircraft node
            double radius = 0.4;
            int engine = 0;
            godot::MeshInstance3D *haze = nullptr;
            double power = 0.0;
            std::deque<Puff> trail;
            uint32_t counter = 0;
            godot::Vector3 last_pos;
            bool last_valid = false;
            double carry_m = 0.0;
        };
        godot::Node3D *ac = nullptr;
        godot::Object *jsb_obj = nullptr;
        std::vector<Nozzle> nozzles;
        bool heat = true, contrail = true, enabled = true, force_contrail = false;
        double clock = 0.0;
        double contrail_factor = 0.0;
        godot::MeshInstance3D *trail_node = nullptr;
        godot::Ref<godot::ArrayMesh> trail_mesh;
        godot::Ref<godot::ShaderMaterial> trail_material, haze_material;

        godot::Vector3 nozzle_world(const Nozzle &n) const;
        double engine_power(JSBGodot *fdm, int engine) const;
        godot::Ref<godot::ArrayMesh> build_haze_mesh(double radius, double length) const;
        void ensure_materials();
        void emit_puffs(Nozzle &n, const godot::Vector3 &now_pos, double density);
        void rebuild_trail();
    };
} // namespace jsbgodot

#endif
