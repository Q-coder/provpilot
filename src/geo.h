#ifndef GEO_H
#define GEO_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

namespace jsbgodot
{
    // Stateless geographic conversions between Swiss LV95 (CH1903+, EPSG:2056),
    // UTM and WGS84 lat/lon. Thin GDScript-facing wrapper over the rigorous
    // projection library shared with Scenery3D (addons/scenery3d/src/s3d_proj.h):
    // Swiss oblique Mercator on Bessel 1841 + CH1903+/WGS84 datum shift, which
    // matches pyproj (the bake pipeline) to the millimetre anywhere in Europe.
    // Design §9 D7. Kept as a plain RefCounted so GDScript can call the statics
    // without any node-tree wiring.
    class Geo : public RefCounted
    {
        GDCLASS(Geo, RefCounted);

    public:
        static void _bind_methods();

        // WGS84 -> Swiss LV95 (CH1903+). Returns Vector2(easting, northing) in metres.
        static Vector2 wgs84_to_lv95(double lat_deg, double lon_deg);

        // Swiss LV95 (CH1903+) -> WGS84. Returns Vector2(latitude, longitude) in degrees.
        static Vector2 lv95_to_wgs84(double easting, double northing);

        // Bearing of TRUE north measured clockwise from LV95 grid north at a
        // WGS84 point, in degrees. grid_bearing = true_bearing + convergence.
        // ~0° in Switzerland, -2.7° at Oslo, -18° at Kirkenes.
        static double lv95_grid_convergence_deg(double lat_deg, double lon_deg);

        // WGS84 <-> UTM (WGS84 datum) for regions that declare a UTM CRS.
        static Vector2 wgs84_to_utm(double lat_deg, double lon_deg, int zone, bool north);
        static Vector2 utm_to_wgs84(double easting, double northing, int zone, bool north);
        static int utm_zone_for(double lon_deg);

    protected:
        Geo();
        ~Geo();
    };

} // namespace jsbgodot

#endif
