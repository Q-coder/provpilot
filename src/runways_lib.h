#ifndef RUNWAYS_LIB_H
#define RUNWAYS_LIB_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace jsbgodot
{

    // Per-pack runway data (design §12, D16; formerly runways_lib.gd).
    //
    // Every scenery pack may carry `airports/runways.json` (schema v1). The
    // enabled packs are merged PER AIRPORT: the highest-priority pack that
    // lists an ICAO defines that airport completely (runways, optional model,
    // whether the procedural runways are drawn); lower packs are ignored for
    // it. Also converts merged airports into the catalog shape the welcome
    // screen and the spawn code use (threshold_<ident> in LV95, true heading,
    // length).
    class RunwaysLib : public RefCounted
    {
        GDCLASS(RunwaysLib, RefCounted);

    public:
        static void _bind_methods();
        RunwaysLib();

        // Glyph codes for the runway shader: digits 0-9, L=10, R=11, C=12, none=-1.
        static PackedInt32Array glyph_codes(const String &ident);
        // One pack's runways.json (absolute pack directory), {} when absent.
        static Dictionary read_pack(const String &pack_path);
        // Enabled scenery areas (priority order) -> icao -> airport entry,
        // each with "pack_path" and "pack_region".
        static Dictionary load_merged(const Array &scenery_areas);
        // Catalog-shaped runway list for a merged airport entry.
        static Array catalog_runways(const Dictionary &ap);
        // Replace the runways of catalog airports a pack defines and append
        // pack airports the catalog lacks (in place; returned).
        static TypedArray<Dictionary> merge_into_catalog(const TypedArray<Dictionary> &catalog, const Dictionary &merged);

    protected:
        static double initial_bearing(double lat1, double lon1, double lat2, double lon2);
    };

} // namespace jsbgodot

#endif
