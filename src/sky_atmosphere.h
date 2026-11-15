#ifndef SKY_ATMOSPHERE_H
#define SKY_ATMOSPHERE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace jsbgodot
{

    // Day/night sun + atmospheric sky for the world3d globe (formerly
    // sky_atmosphere.gd). Installs the atmospheric-scattering sky shader so
    // the horizon follows the Earth's curvature at every altitude, computes
    // the sun from the simulated time of day + the observer position and
    // swings the scene DirectionalLight3D so shadows track it (off at night),
    // and applies the altitude ramps (fog fades toward space, ambient / glow
    // taper). The coarse globe stays visible at every altitude as the
    // fallback surface; WorldScenery's coverage mask culls it under loaded
    // tiles.
    //
    // setup() once the globe is mounted, set_time() when the time-of-day
    // settings change, update(viewport) every frame.
    class SkyAtmosphere : public RefCounted
    {
        GDCLASS(SkyAtmosphere, RefCounted);

    public:
        static void _bind_methods();
        SkyAtmosphere();

        // Registers the scene nodes, installs the sky shader and captures the
        // Environment's authored fog / ambient / glow values as baselines.
        void setup(Environment *env, DirectionalLight3D *sun, DirectionalLight3D *fill, Node *globe, double lat_deg, double lon_deg);
        // Meteorological visibility -> exponential fog density (Koschmieder).
        void set_visibility_km(double km);
        // Real UTC clock, or a manual local time-of-day (minutes past midnight).
        void set_time(bool use_system, int local_minutes);
        // Simulated local time-of-day as "HH:MM:SS".
        String get_time_string() const;
        // Per frame: sun, light + sky shader + shadows, altitude ramps.
        void update(double delta, Viewport *viewport);

        // Unit vector toward the sun in ECEF for the NOAA solar-position
        // approximation at the observer (static, for tests).
        static Vector3 solar_direction(double lat_deg, double lon_deg, double doy, double utc_hours);

        Environment *get_environment() const;
        DirectionalLight3D *get_sun_light() const;
        double get_observer_lat() const; void set_observer_lat(double v);
        double get_observer_lon() const; void set_observer_lon(double v);
        Vector3 get_sun_dir_now() const;
        Color get_sun_color_now() const;
        double get_sun_energy_now() const;
        double get_ambient_energy_now() const;
        Color get_fog_color_now() const;
        double get_cloud_ambient_now() const;
        double get_twilight_now() const;
        double get_time_seconds() const; void set_time_seconds(double v);
        bool get_clear_air() const; void set_clear_air(bool v);
        double get_base_sun_energy() const;
        double get_base_fog_density() const;

    protected:
        static constexpr double PLANET_RADIUS = 6371000.0;   // mean Earth radius (m)
        static constexpr double ATMOSPHERE_HEIGHT = 100000.0; // scattering shell (m)
        // Haze lives in the boundary layer: from a camera above it the light
        // path to the ground crosses only HAZE_LAYER_M of haze, so the fog
        // density is scaled by HAZE_LAYER_M / altitude.
        static constexpr double HAZE_LAYER_M = 2500.0;
        static constexpr double FOG_ZERO_ALT_M = 120000.0;  // fog off above
        static constexpr double SKY_FULL_ALT_M = 12000.0;   // full ambient/glow at/below
        static constexpr double SKY_TAPER_ALT_M = 100000.0; // fully tapered at/above
        static constexpr double AMBIENT_DAY_ENERGY = 0.7;
        static constexpr double AMBIENT_NIGHT_ENERGY = 0.04;
        static constexpr double FOG_NIGHT_LIGHT = 0.012;    // the haze's brightness at night, of its day value
        // Camera altitudes between which the local dusk fade gives way to
        // full-strength global sunlight (terminator on the globe).
        static constexpr double SUN_LOCAL_ALT_M = 30000.0;
        static constexpr double SUN_SPACE_ALT_M = 400000.0;

        Environment *environment = nullptr;
        DirectionalLight3D *sun_light = nullptr;
        DirectionalLight3D *fill_light = nullptr;
        Node *globe_node = nullptr;
        Ref<ShaderMaterial> sky_material;
        double observer_lat = 47.7;
        double observer_lon = 8.6;
        Vector3 observer_up = Vector3(0, 1, 0);
        Vector3 sun_dir_now = Vector3(0, 1, 0);
        Color sun_color_now = Color(1.0, 0.97, 0.92);
        double sun_energy_now = 1.0;
        double ambient_energy_now = 0.7;
        // Twilight: how bright the sky still is (0 night .. 1 day), the sun from
        // 8 deg under the horizon to 5 deg over it. Drives the fog's light and
        // the clouds' sky light (the sun's own light ends at the horizon).
        double twilight_now = 1.0;
        Color base_fog_color = Color(0.7, 0.75, 0.85);
        double base_fog_light_energy = 1.0;
        Color fog_color_now = Color(0.7, 0.75, 0.85);
        double cloud_ambient_now = 0.7;
        bool use_system_time = true;
        int manual_minutes = 720;
        double time_seconds = 43200.0; // local time-of-day (s)
        double base_fog_density = 0.00005;
        double base_ambient_energy = 2.2;
        double base_glow_intensity = 0.5;
        double base_sun_energy = 1.0;
        double base_fill_energy = 0.6;
        bool clear_air = false; // scenery editor: no fog at all

        void install_sky_material();
        void apply_altitude(double alt, double day, double space, double sin_elev);
        void orient_sun(const Vector3 &dir_to_sun);
        static Color sun_color(double sin_elev);
        Vector2 current_time(); // (day_of_year, utc_hours)
        static int day_of_year(int y, int m, int d);
    };

} // namespace jsbgodot

#endif
