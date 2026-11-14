#ifndef HT9100_MCDU_H
#define HT9100_MCDU_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

using namespace godot;

namespace jsbgodot
{

    // The MD-80's Honeywell Trimble HT9100 FMS and MCDU: a port of the
    // FlightGear package's Nasal/HT9100/*.nas (Josh Davidson, Octal450) and of
    // the text side of Nasal/Displays/HT9100.nas, to the extent upstream
    // implements it: the pages MENU, IDENT, POS REF, DATA INDEX, NEAREST INDEX,
    // NEAREST AIRPORTS, RTE 1 (origin, destination, flight number) and PERF
    // INIT (transition altitude, cruise level), the scratchpad with its
    // messages, CLR / long CLR / DELETE, +/-, and "PAGE NOT AVAIL" for every
    // other key. There is no route manager behind it (upstream hands the
    // origin / destination to FlightGear's), so LEGS, DEP ARR, HOLD, PROG and
    // the company routes stay unavailable.
    //
    // The cockpit keys reach key() through the Nasal shim action "mcdu"
    // (mcdu_ht9100.unit[0].softKey("l1") -> key("softKey", "l1")). Per frame
    // the rig calls update() with the bridge's property dictionary and the
    // aircraft position; get_published() holds the screen as string
    // properties for the screen texts (model/screen_texts.json):
    //   instrumentation/ht9100/d/<field>   text of a field (Title, PageNum,
    //                                      Scratchpad, L1..L6, L1L.., L1B..,
    //                                      C1.., C1L.., R1.., R1L.., R1B..)
    //   instrumentation/ht9100/c/<field>   its colour (#rrggbb)
    //   instrumentation/ht9100/s/<field>   1 = the small font
    //   instrumentation/ht9100/exec-light, msg-light
    //   autopilot/route-manager/departure/airport, destination/airport,
    //   cruise/altitude-ft (what upstream writes to the route manager)
    class Ht9100Mcdu : public RefCounted
    {
        GDCLASS(Ht9100Mcdu, RefCounted);

    public:
        static void _bind_methods();
        Ht9100Mcdu();

        // cfg options: the IDENT page's model and engine texts
        void set_option(const String &name, const String &value);
        void reset();
        void update(const Dictionary &props, double dt, double lat_deg, double lon_deg);
        Dictionary get_published() const;
        // A cockpit key: softKey (l1..r6), alphaNumKey (a character, CLR, +/-),
        // clearKey (down / up), pageKey (rte, menu, dataIndex, ...),
        // nextPageKey, prevPageKey, execKey. False for an unknown kind.
        bool key(const String &kind, const String &arg);
        // For tests: the current page's name and the scratchpad line as shown.
        String get_page() const;
        String get_scratchpad() const;

    private:
        enum Page { P_IDENT, P_POS_REF, P_MENU, P_DATA_INDEX, P_NEAREST_INDEX, P_NEAREST_AIRPORTS, P_RTE, P_PERF_INIT, P_FALLBACK };
        struct Airport
        {
            String icao;
            double lat = 0.0, lon = 0.0, elevation_ft = 0.0;
        };

        Dictionary st;
        Page page = P_IDENT;
        Page last_fmc_page = P_IDENT;
        bool has_last_fmc_page = false;
        bool del = false;
        bool exec = false;
        String scratchpad, scratchpad_old;
        std::vector<String> messages;
        bool clear_timer_active = false;
        double clear_timer_time = -5.0;
        double elapsed = 0.0;
        bool powered = true;
        bool annun_test = false;
        bool eng_on = false, reset_toggle = false;
        double lat = 0.0, lon = 0.0, magvar = 0.0, groundspeed_kt = 0.0;
        int utc_h = 0, utc_m = 0, utc_s = 0;
        bool database_selected = true;
        String opt_type = "DC-9-82", opt_engines = "JT8D-217A";

        // flight data (HT9100-flight-data.nas)
        String airport_from, airport_to, flight_number, co_rte;
        double airport_from_alt = -2000.0, airport_to_alt = -2000.0;
        int cruise_alt = 0, cruise_fl = 0, trans_alt = 18000;

        std::vector<Airport> airports;
        bool airports_loaded = false;
        std::vector<int> nearest; // indices into airports, nearest first (set when the page opens)

        void load_airports();
        int find_airport(const String &icao) const;
        void setup_nearest();
        void flight_data_reset();

        void alpha_num_key(const String &k);
        void clear_message(int a);
        void set_message(const String &m);
        void remove_message(const String &m);
        void scratchpad_clear();
        int scratchpad_state() const;
        void set_page(Page p);
        Page page_by_name(const String &name) const;
        void soft_key(const String &k);
        static bool is_fmc_page(Page p);
        static bool string_is_int(const String &s);
        static bool length_in_range(const String &s, int lo, int hi);

        void put(const String &field, const String &text, const char *color = nullptr, int small = -1);
        void publish();
    };

} // namespace jsbgodot

#endif
