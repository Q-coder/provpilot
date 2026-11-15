#ifndef GEAR_STRUTS_H
#define GEAR_STRUTS_H

#include <vector>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>

namespace jsbgodot
{
    class JSBGodot;

    // Strut compression: every drawn wheel is pinned to the FDM's gear contact
    // point, so the tyres stay on the ground while the airframe sinks on its
    // struts (taxi loads, rotation, a hard landing) and hang at full
    // extension in the air.
    //
    // Per FDM gear unit the tyre's lowest point (its centre less its radius,
    // measured once from the mesh vertices) is compared with where the FDM has
    // its contact: unit location minus compression. The difference in height
    // is what the wheel has to move by, along the aircraft's up axis:
    //   * FlightGear models: through gear/gear[n]/compression-norm, the
    //     property their oleo animations (wheel, torque links, dampers) read;
    //     travel_m = metres of wheel travel per unit of that property.
    //   * other models: the configured node is moved (the wheel, or the
    //     steering pivot that carries fork and wheel).
    // No hand calibration: the reference is measured from the drawn model,
    // so an FDM gear change or a [model] offset change moves the wheels along.
    class GearStruts
    {
    public:
        struct Unit
        {
            int fdm_unit = 0;
            godot::MeshInstance3D *tyre = nullptr; // the tyre whose lowest point is pinned
            godot::Node3D *node = nullptr;         // node mode: what is moved
            double travel_m = 0.0;                 // FlightGear mode: metres per unit of compression-norm (0 = node mode)
            double min_m = -0.2, max_m = 0.35;     // travel limits from the drawn pose (up positive)
            // state
            bool measured = false;
            godot::Vector3 centre_local;           // tyre vertex-box centre, mesh space
            double radius = 0.0;                   // tyre radius, metres
            double rest_shift = 0.0;               // FlightGear mode: the shift asked for (before limits and the retract fade)
            double last_total = 0.0;               // the shift asked for last frame (for the one-frame lead)
            bool has_last = false;
            double applied_m = 0.0;                // shift applied last frame
            godot::Vector3 applied_local;          // node mode: the same in the node's parent space
            double norm = 0.0;                     // FlightGear mode: compression-norm to publish
        };

        void reset();
        void add_unit(const Unit &u);
        bool empty() const { return units.empty(); }
        int unit_count() const { return (int)units.size(); }
        bool is_fg(int i) const { return units[i].travel_m > 0.0; }
        int fdm_unit(int i) const { return units[i].fdm_unit; }
        double norm(int i) const { return units[i].norm; }
        double shift_m(int i) const { return units[i].applied_m; }

        // Node mode, around the control surfaces' update (which rewrites the
        // wheel transforms from their base pose): take last frame's shift off,
        // then put this frame's on.
        void undo_node_shifts();
        void apply_node_shifts(godot::Node3D *ac, const JSBGodot *fdm, double gear_pos);
        // FlightGear mode: compression-norm per unit for this frame (read with norm()).
        void update_fg(godot::Node3D *ac, const JSBGodot *fdm, double gear_pos);

    private:
        std::vector<Unit> units;
        void measure(Unit &u, godot::Node3D *ac);
        double lead(Unit &u, double total);
        bool shift_needed(const Unit &u, godot::Node3D *ac, const JSBGodot *fdm, double &out) const;
    };
} // namespace jsbgodot

#endif
