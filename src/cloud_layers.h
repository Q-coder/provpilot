#ifndef CLOUD_LAYERS_H
#define CLOUD_LAYERS_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/noise_texture2d.hpp>
#include <godot_cpp/classes/noise_texture3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

namespace jsbgodot
{

    // Cloud layers from the weather model (design §10.2; formerly clouds.gd).
    // One child mesh per layer with clouds: thick types (cumulus, stratus,
    // stratocumulus, cumulonimbus) are raymarched inside a camera-centred box
    // between base and top; thin types (altostratus, altocumulus, cirrus)
    // are a curved textured sheet at the base altitude. This node's transform
    // is the West/Up/North frame at the camera's ground point, re-set every
    // frame (ECEF world).
    class CloudLayers : public Node3D
    {
        GDCLASS(CloudLayers, Node3D);

    public:
        static void _bind_methods();
        CloudLayers();

        void _ready() override;

        // Rebuild the meshes from the weather layer stack (base_m, top_m,
        // cloud_type, coverage_oktas, wind_dir_deg, wind_speed_kts).
        void set_layers(const Array &layers);
        // Per frame: re-anchor the local frame under the camera and push the
        // light state. sun_dir_world is the ECEF unit vector toward the sun.
        void update(Camera3D *cam, const Vector3 &sun_dir_world, const Color &sun_color, double sun_energy,
                const Color &ambient_color, double ambient_energy, double fog_density, const Color &fog_color, double delta);
        int get_quality() const;
        void set_quality(int q); // 0 off .. 3
        Array get_meshes() const;

    protected:
        enum { CT_CUMULONIMBUS = 4, CT_STRATUS = 2 };
        static constexpr double BOX_HALF_M = 60000.0;
        static constexpr double SHEET_HALF_M = 90000.0;
        static constexpr double PLANET_RADIUS = 6371000.0;

        int quality = 2;
        Ref<NoiseTexture3D> shape_noise;
        Ref<NoiseTexture3D> detail_noise;
        Ref<NoiseTexture2D> sheet_noise;
        Ref<Shader> volume_shader;
        Ref<Shader> sheet_shader;
        Array layers;
        Vector<MeshInstance3D *> meshes;
        Vector<Vector2> wind;    // drift (m/s) per mesh, (west, north)
        Vector<Vector2> offsets; // accumulated drift per mesh
        Vector<double> mids;     // layer mid altitude per mesh (draw order)

        static Ref<NoiseTexture3D> make_noise3d(int size, int type, int octaves, int seed);
        void apply_draw_order(double cam_alt);
        void apply_quality();
    };

} // namespace jsbgodot

#endif
