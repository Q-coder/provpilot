#ifndef AIRPORTS_LIB_H
#define AIRPORTS_LIB_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace jsbgodot
{

    // The bundled airport catalog (res://airports.json) in the shape the
    // welcome screen and the spawn code use (formerly airports_lib.gd):
    //
    //     { icao, name, type, region, [quadrant], center_e, center_n,
    //       elevation_m, grid_convergence_deg,
    //       runways: [ { designator, heading_deg, grid_heading_deg, length_m,
    //                    surface, threshold_<low>: {e, n}, threshold_<high>: {e, n} } ] }
    //
    // WGS84 latitude / longitude are converted to Swiss LV95 with the rigorous
    // projection (Geo); entries may carry explicit `swiss_e` / `swiss_n`
    // overrides. `heading_deg` is the runway's TRUE bearing, `grid_heading_deg`
    // the same bearing in the LV95 grid frame (true + convergence).
    //
    // Also derives the installed scenery regions from the downloader's
    // installed_scenery_packs dict and from the scenery areas on disk.
    class AirportsLib : public RefCounted
    {
        GDCLASS(AirportsLib, RefCounted);

    public:
        static void _bind_methods();
        AirportsLib();

        static TypedArray<Dictionary> load_default();
        // One raw JSON entry -> the runtime shape (pure).
        static Dictionary resolve(const Dictionary &raw);
        static Vector2 wgs84_to_lv95(double lat_deg, double lon_deg);
        // Region ids whose base pack ("<region>-base-v<ver>") is installed.
        static Dictionary installed_regions(const Dictionary &installed_packs);
        // Region ids with scenery on disk, from the configured scenery areas
        // (tagged by terrain/manifest.json, else by the area's directory name).
        static Dictionary installed_regions_from_areas(const Array &scenery_areas);
        // The "region" scalar of <area>/terrain/manifest.json (first 64 KB), or "".
        static String manifest_region(const String &area_path);
        // Availability marker for the welcome list: "✓" installed, "✗" not, "" unknown region.
        static String availability_marker(const Dictionary &airport, const Dictionary &installed_regions_set);

    protected:
        static Variant read_json(const String &path);
        static String region_dir(const String &region_id);
    };

} // namespace jsbgodot

#endif
