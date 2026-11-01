#ifndef NAVAIDS_LIB_H
#define NAVAIDS_LIB_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <vector>

using namespace godot;

namespace jsbgodot
{

    // One radio navaid of the bundled catalog (res://navaids.json, built from
    // OurAirports by scripts/build_world_navaids.py; docs/RADIO_NAVIGATION.md).
    struct Navaid
    {
        String ident;
        String name;
        String type;   // VOR, VOR-DME, VORTAC, DME, TACAN, NDB, NDB-DME
        String power;  // LOW / MEDIUM / HIGH / ""
        String usage;  // TERMINAL / LO / HI / BOTH / RNAV / ""
        String airport;
        String region;
        int kind = 0;          // NavaidsLib::KIND_* bits
        int freq_khz = 0;      // every type in kHz: 114850 = 114.85 MHz, 375 = 375 kHz
        int dme_khz = 0;       // paired DME frequency (0 = none)
        double lat = 0.0;
        double lon = 0.0;
        double elev_m = 0.0;
        double station_var_deg = 0.0; // declination the VOR's radials are slaved to (east +)
        bool has_station_var = false;
    };

    // One ILS of res://ils.json (scripts/build_world_ils.py): the runway end
    // it serves and, once resolved against the airport list, the localizer /
    // glide slope geometry derived from that runway (so the approach lands on
    // the runway the sim draws). WGS84 degrees, metres; LV95 for the map.
    struct Ils
    {
        String icao, designator, rwy, ident, cat, source;
        int freq_khz = 0;
        double gs_deg = 3.0;
        bool offset = false;      // offset localizer: nav.dat course / position are kept
        bool synthetic = false;
        bool has_dme = true;
        double ref_course_deg = 0.0;
        bool has_ref_loc = false;
        double ref_loc_lat = 0.0, ref_loc_lon = 0.0, ref_loc_elev_m = 0.0;
        // resolved geometry
        bool resolved = false;
        double thr_lat = 0.0, thr_lon = 0.0, thr_elev_m = 0.0;
        double far_lat = 0.0, far_lon = 0.0;
        double thr_e = 0.0, thr_n = 0.0, far_e = 0.0, far_n = 0.0;
        double loc_lat = 0.0, loc_lon = 0.0, loc_elev_m = 0.0;
        double gs_lat = 0.0, gs_lon = 0.0, gs_elev_m = 0.0;
        double course_deg = 0.0;      // true inbound course
        double half_width_deg = 2.0;  // full-scale localizer deflection
        double length_m = 0.0;
    };

    // The navaid catalog: loads res://navaids.json once, answers frequency
    // and proximity queries for the receivers (RadioNav) and the VFR map, and
    // holds the geodesic helpers the receivers need (Vincenty inverse on
    // WGS84: distance and the forward / back azimuths, metre-accurate at
    // every range a radio reaches). Plain RefCounted with static methods so
    // GDScript tests can call it without wiring.
    class NavaidsLib : public RefCounted
    {
        GDCLASS(NavaidsLib, RefCounted);

    public:
        enum Kind
        {
            KIND_VOR = 1,   // azimuth: VOR, VOR-DME, VORTAC
            KIND_DME = 2,   // distance: VOR-DME, VORTAC, DME, TACAN, NDB-DME
            KIND_NDB = 4,   // NDB, NDB-DME
            KIND_TACAN = 8, // TACAN, VORTAC
        };

        static void _bind_methods();
        NavaidsLib();

        // The catalog (lazily loaded from res://navaids.json).
        static const std::vector<Navaid> &all();
        // Replace the catalog from another JSON file (tests); true on success.
        static bool load_from(const String &path);
        static int count();
        static Dictionary to_dict(const Navaid &n);
        static int kind_of(const String &type);
        // Nominal reception range of a station in nautical miles from its
        // power class (FAA service volumes: terminal 25, low 40, high 130 nm;
        // NDBs 25 / 50 / 75 nm).
        static double nominal_range_nm(const Navaid &n);

        // Every station on exactly this frequency (kHz) matching the kind mask.
        static std::vector<const Navaid *> on_frequency(int freq_khz, int kind_mask);
        static TypedArray<Dictionary> find_frequency(int freq_khz, int kind_mask);
        // Stations within radius_nm of a position (sorted by distance).
        static TypedArray<Dictionary> nearby(double lat_deg, double lon_deg, double radius_nm, int kind_mask);
        // The nearest station of a kind (empty Dictionary when none).
        static Dictionary nearest(double lat_deg, double lon_deg, int kind_mask);

        // ── ILS (docs/RADIO_NAVIGATION.md §ILS) ──
        // The ILS list (res://ils.json), geometry resolved against the airport
        // list given by set_airports() (the welcome screen's catalog with the
        // scenery packs' runways merged in), else the bundled catalog.
        static const std::vector<Ils> &all_ils();
        static void set_airports(const TypedArray<Dictionary> &airports);
        static int ils_generation();     // bumps whenever the geometry was re-resolved
        static std::vector<const Ils *> ils_on_frequency(int freq_khz);
        static TypedArray<Dictionary> find_ils(int freq_khz);
        static Dictionary ils_to_dict(const Ils &i);
        static int ils_count();
        static bool is_ils_channel(int freq_khz);

        // ── geodesy (WGS84) ──
        // Vincenty inverse: distance in metres, initial bearing 1->2 and the
        // bearing 2->1, true degrees 0..360. Falls back to the great circle
        // on the mean sphere for the (irrelevant) near-antipodal case.
        static void inverse(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg,
                double &dist_m, double &bearing12_deg, double &bearing21_deg);
        static double distance_m(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg);
        static double bearing_deg(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg);
        // Angle a - b wrapped into (-180, 180], degrees.
        static double angle_diff_deg(double a, double b);

    protected:
        static std::vector<Navaid> catalog;
        static bool loaded;
        static std::vector<Ils> ils_list;
        static bool ils_loaded;
        static int ils_gen;
        static std::vector<Dictionary> airports;
        static bool airports_set;
        static void ensure_ils_loaded();
        static void resolve_ils();
        static Variant read_json(const String &path);
        static void ensure_loaded();
        static bool parse(const Variant &data);
    };

} // namespace jsbgodot

#endif
