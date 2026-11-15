#ifndef CITY_LIGHTS_H
#define CITY_LIGHTS_H

// Night lights of towns and cities (docs/CITY_LIGHTS.md): point sprites over
// the built-up and industrial polygons of the base tier's land-cover tiles
// (vegetation/forest_E_N.bin, FOR1: the same files that paint the ground and
// plant the forests), with street lamps along the roads inside them. One
// point mesh per 1024 m square, generated on a worker thread from the
// polygons (position-hashed, so a town always wears the same lights), in two
// levels of detail: every lamp near the aircraft, a quarter of them — larger
// — out to the horizon. Lit by the host's night factor, dimmed by the haze.

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jsbgodot
{
using namespace godot;

class CityLights : public Node3D
{
    GDCLASS(CityLights, Node3D)

protected:
    static void _bind_methods();

public:
    CityLights();
    ~CityLights();

    void set_paths(const PackedStringArray &dirs);
    PackedStringArray get_paths() const;
    void set_terrain_height(const Callable &height_geodetic);
    // intensity 0 (day) .. 1 (night); fog_density of the environment (1/m)
    void update(Camera3D *cam, double intensity, double fog_density, double delta);
    void set_far_radius_m(double m);
    double get_far_radius_m() const;
    void set_near_radius_m(double m);
    double get_near_radius_m() const;
    void set_enabled(bool on);
    bool get_enabled() const;
    Dictionary stats() const;
    void clear();

private:
    static constexpr int TILE_M = 1024;
    static constexpr double SCAN_INTERVAL_S = 0.5;
    static constexpr int FINISH_PER_FRAME = 4;
    static constexpr int FAR_SPAN = 4;    // a far block is 4 x 4 squares in one mesh (one draw call)
    static constexpr int GRID_NEAR = 9;   // terrain samples per side of a near square (128 m)
    static constexpr int GRID_FAR = 17;   // of a far block (256 m)
    static constexpr double LIFT_NEAR_M = 6.0;   // lamp height over the ground
    static constexpr double LIFT_FAR_M = 14.0;   // far squares stand on coarser terrain

    struct Point
    {
        float x, z;       // metres east / north of the square's SW corner
        float r, g, b;
        float size;
    };
    struct Job
    {
        int64_t key = 0;
        int ei = 0, ni = 0;
        int lod = 0;   // 0 near (one square), 1 far (FAR_SPAN x FAR_SPAN squares from ei, ni)
        uint64_t generation = 0;
        std::vector<std::string> dirs;
    };
    struct Result
    {
        int64_t key = 0;
        int ei = 0, ni = 0;
        int lod = 0;
        uint64_t generation = 0;
        std::vector<Point> points;
        double corner_lat[4] = {0, 0, 0, 0};   // SW, SE, NW, NE
        double corner_lon[4] = {0, 0, 0, 0};
    };
    struct Tile
    {
        bool pending = false;
        bool empty = false;       // no lamp in the square / block
        MeshInstance3D *node = nullptr;
        int points = 0;
    };

    static int64_t key_of(int ei, int ni, int lod) { return ((int64_t)lod << 62) | ((int64_t)(ei & 0x3FFFFFFF) << 31) | (int64_t)(ni & 0x7FFFFFFF); }
    static bool read_square(const std::vector<std::string> &dirs, int ei, int ni, int lod, float off_x, float off_z, std::vector<Point> &out);
    void worker_loop();
    static void generate(const Job &job, Result &res);
    void scan(const Vector2 &cam_en, double cam_alt);
    void finish(const Result &res);
    void drop_tile(Tile &t);
    void ensure_material();

    PackedStringArray paths;
    Callable terrain_cb;
    Ref<Shader> shader;
    Ref<ShaderMaterial> material;
    std::unordered_map<int64_t, Tile> tiles;
    double far_radius_m = 45000.0;
    double near_radius_m = 9000.0;
    bool enabled = true;
    double scan_timer = 0.0;
    uint64_t generation = 1;
    int drawn_points = 0;

    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Result> results;
    std::atomic<bool> quit{false};
};

} // namespace jsbgodot

#endif
