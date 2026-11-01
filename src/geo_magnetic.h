#ifndef GEO_MAGNETIC_H
#define GEO_MAGNETIC_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

using namespace godot;

namespace jsbgodot
{
    // World Magnetic Model WMM2025 (NOAA NCEI / UK BGS, public domain
    // coefficients, valid 2025.0 - 2030.0): the magnetic declination at a
    // geodetic position, i.e. the angle from true north to magnetic north,
    // east positive. The magnetic compass, the directional gyro / HSI and the
    // ADF card read magnetic; VOR radials use the station's own slaved
    // declination instead (docs/RADIO_NAVIGATION.md). Spherical-harmonic
    // synthesis to degree 12 as in the WMM technical report; validated
    // against NOAA's WMM2025 test values in tests/smoke_radio_nav.gd.
    class GeoMagnetic : public RefCounted
    {
        GDCLASS(GeoMagnetic, RefCounted);

    public:
        static void _bind_methods();

        // Declination in degrees (east positive) at the WGS84 position and
        // height, for the decimal year (2025.0 .. 2030.0; clamped to the
        // model's span so the extrapolation stays sane).
        static double declination_deg(double lat_deg, double lon_deg, double alt_m, double decimal_year);
        // Inclination (dip) in degrees, positive down; total field in nT.
        static double inclination_deg(double lat_deg, double lon_deg, double alt_m, double decimal_year);
        static double total_intensity_nt(double lat_deg, double lon_deg, double alt_m, double decimal_year);
        // The system clock as a decimal year (2026.83 ...).
        static double decimal_year_now();

        // The field vector in the local geodetic frame: north, east, down (nT).
        static void field_ned(double lat_deg, double lon_deg, double alt_m, double decimal_year,
                double &x_nt, double &y_nt, double &z_nt);

    protected:
        GeoMagnetic();
        ~GeoMagnetic();
    };

} // namespace jsbgodot

#endif
