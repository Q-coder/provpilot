#ifndef SEA_SURFACE_H
#define SEA_SURFACE_H

// The sea (docs/SEA.md): one water surface at 0 m, a polar mesh centred under
// the camera and reaching the horizon. The coastline is not in this node: the
// base tier's terrain is cut below 0 m wherever OpenStreetMap has sea
// (scripts/build_world_sea.py), so the depth test draws the shore.

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>

namespace jsbgodot
{
using namespace godot;

class SeaSurface : public Node3D
{
    GDCLASS(SeaSurface, Node3D)

protected:
    static void _bind_methods();

public:
    SeaSurface();
    // sky_light: 0 night .. 1 day; wind in m/s toward (east, north)
    void update(Camera3D *cam, const Vector3 &sun_dir_world, const Color &sun_color, double sun_energy, double sky_light,
            const Vector2 &wind_en, double delta);
    void set_enabled(bool on);
    bool get_enabled() const;
    double get_radius_m() const;

private:
    static constexpr int SEGMENTS = 128;
    static constexpr int RINGS = 72;
    static constexpr double INNER_M = 12.0;
    static constexpr double MAX_ALT_M = 60000.0;   // above: the globe's own ocean is all one sees

    void build_mesh(double radius_m, double planet_radius);

    MeshInstance3D *mesh_instance = nullptr;
    Ref<ShaderMaterial> material;
    double built_radius = 0.0;
    double built_planet_radius = 0.0;
    Vector2 drift;
    bool enabled = true;
};

} // namespace jsbgodot

#endif
