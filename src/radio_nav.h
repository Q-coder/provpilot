#ifndef RADIO_NAV_H
#define RADIO_NAV_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "navaids_lib.h"

using namespace godot;

namespace jsbgodot
{

    // The aircraft's radio navigation receivers (docs/RADIO_NAVIGATION.md):
    // NAV 1 (VOR with its course deviation indicator and the paired DME) and
    // ADF 1 (NDB). Each frame the host sets the tuned frequencies, the OBS
    // course and the ADF card from the cockpit knobs and calls update() with
    // the aircraft's position; the receiver finds the station on the
    // frequency, applies the reception range (service volume and, for VHF,
    // the radio horizon) and derives what the instruments show, published
    // under FlightGear's property names (instrumentation/nav/..., adf/...,
    // dme/...) by write_properties() so fgdata's gauges and radios animate
    // unchanged.
    //
    // Conventions follow FlightGear's navradio / adf: the radial is the true
    // bearing from the station to the aircraft minus the station's slaved
    // declination (a VOR's radials are magnetic as the station was set up,
    // not as today's field is); heading-needle-deflection is the course error
    // in degrees, +-10 full scale, positive = fly right; to-flag when the
    // selected course points at the station (within 90 deg of the inbound
    // course); the ADF's indicated-bearing-deg is the relative bearing to
    // the station, 0 = straight ahead, 90 = right wing. Needles follow their
    // targets with a short lag.
    class RadioNav : public RefCounted
    {
        GDCLASS(RadioNav, RefCounted);

    public:
        static void _bind_methods();
        RadioNav();

        // Cockpit inputs (from the control state).
        void set_nav_frequency_mhz(double mhz);
        double get_nav_frequency_mhz() const;
        void set_obs_deg(double deg);
        double get_obs_deg() const;
        void set_adf_frequency_khz(double khz);
        double get_adf_frequency_khz() const;
        void set_adf_card_deg(double deg);
        double get_adf_card_deg() const;
        void set_powered(bool on);
        bool get_powered() const;
        // Which NAV receiver this is: 0 writes instrumentation/nav/... (with the
        // DME and the ADF), n > 0 writes instrumentation/nav[n]/... only.
        void set_receiver_index(int index);
        int get_receiver_index() const;

        // One step: aircraft position (WGS84, metres MSL), true heading.
        void update(double delta, double lat_deg, double lon_deg, double alt_m, double true_heading_deg);
        // FlightGear property values for the instruments and radios.
        void write_properties(Dictionary &p) const;
        Dictionary get_properties() const;
        // One line for the HUD: "NAV 114.85 KLO R087 TO 12.3 NM  ADF 335 BER".
        String status_text() const;

        // Receiver state (tests / HUD).
        bool nav_in_range() const;
        String nav_ident() const;
        double nav_radial_deg() const;        // magnetic radial the aircraft is on
        double nav_bearing_to_deg() const;    // true bearing aircraft -> station
        double nav_distance_nm() const;       // ground distance
        double cdi_deflection() const;        // +-10, positive = fly right (lagged)
        bool nav_to_flag() const;
        bool nav_from_flag() const;
        bool nav_is_loc() const;              // tuned to an ILS localizer
        String ils_runway() const;            // "LSZH 16" while on an ILS
        double gs_deflection() const;         // +-3.5, positive = fly up (lagged)
        bool gs_in_range() const;
        bool dme_in_range() const;
        double dme_distance_nm() const;       // slant range
        double dme_ground_speed_kt() const;
        bool adf_in_range() const;
        String adf_ident() const;
        double adf_relative_bearing_deg() const; // lagged needle, 0..360
        double adf_bearing_true_deg() const;

    protected:
        // inputs
        double nav_mhz = 114.85;
        double obs_deg = 0.0;
        double adf_khz = 335.0;
        double adf_card_deg = 0.0;
        bool powered = true;
        int receiver_index = 0;
        // aircraft
        double ac_lat = 0.0, ac_lon = 0.0, ac_alt_m = 0.0, ac_hdg = 0.0;
        // NAV 1
        const Navaid *nav_station = nullptr;
        const Ils *ils = nullptr;
        bool loc_mode = false;
        double gs = 0.0, gs_target = 0.0;     // glide slope needle (lagged)
        double gs_direct = 0.0, gs_path = 0.0; // the aircraft's elevation angle from the GS site, the published path angle
        bool gs_ok = false;
        int nav_khz_tuned = -1;
        double nav_rescan = 0.0;
        bool nav_ok = false;          // azimuth usable
        double nav_radial = 0.0;
        double nav_bearing_to = 0.0;
        double nav_dist_m = 0.0;
        double nav_signal = 0.0;      // 0..1
        double cdi = 0.0;             // lagged needle
        double cdi_target = 0.0;
        bool to_flag = false, from_flag = false;
        // DME
        bool dme_ok = false;
        double dme_nm = 0.0;
        double dme_gs_kt = 0.0;
        double dme_prev_nm = -1.0;
        double dme_rate_acc = 0.0;    // smoothing
        // ADF
        const Navaid *adf_station = nullptr;
        int adf_khz_tuned = -1;
        double adf_rescan = 0.0;
        bool adf_ok = false;
        double adf_bearing = 0.0;     // true bearing to the station
        double adf_rel_target = 90.0;
        double adf_needle = 90.0;     // lagged relative bearing
        double adf_dist_m = 0.0;
        // station declination fallback cache
        mutable double decimal_year = 0.0;

        static constexpr double NM_M = 1852.0;
        static constexpr double CDI_TAU_S = 0.4;
        static constexpr double ADF_TAU_S = 0.8;

        void update_nav(double delta);
        void update_adf(double delta);
        const Navaid *pick_station(int khz, int kind_mask, double &dist_out, double &margin_out) const;
        const Ils *pick_ils(int khz, double &margin_out) const;
        void update_ils(double delta);
        double station_declination(const Navaid &n) const;
        static double vhf_range_nm(const Navaid &n, double alt_m);
        static double lag(double current, double target, double delta, double tau);
    };

} // namespace jsbgodot

#endif
