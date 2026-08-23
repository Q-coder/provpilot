#ifndef GEO_H
#define GEO_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

namespace jsbgodot
{
    // Stateless geographic conversions between Swiss LV95 (CH1903+) and
    // WGS84 lat/lon, using the official swisstopo polynomial approximation
    // (~1 m accuracy). Kept as a plain RefCounted so GDScript can instantiate
    // it with `Geo.new()` without any node-tree wiring.
    class Geo : public RefCounted
    {
        GDCLASS(Geo, RefCounted);

    public:
        static void _bind_methods();

        // WGS84 -> Swiss LV95 (CH1903+). Returns Vector2(easting, northing) in meters.
        static Vector2 wgs84_to_lv95(double lat_deg, double lon_deg);

        // Swiss LV95 (CH1903+) -> WGS84. Returns Vector2(latitude, longitude) in degrees.
        static Vector2 lv95_to_wgs84(double easting, double northing);

    protected:
        Geo();
        ~Geo();
    };

} // namespace jsbgodot

#endif