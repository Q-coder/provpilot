#include "ht9100_mcdu.h"

#include "navaids_lib.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

using namespace godot;

namespace jsbgodot
{

    namespace
    {
        const char *const WHITE = "#ffffff";
        const char *const GREEN = "#00ff00";
        const char *const AMBER = "#f07600";
        const char *const DASHES = "------------------------";
        const char *const PREFIX = "instrumentation/ht9100/";
        const double M_PER_NM = 1852.0;

        double numv(const Dictionary &d, const char *key, double def)
        {
            const Variant v = d.get(String(key), Variant());
            if (v.get_type() == Variant::NIL)
                return def;
            if (v.get_type() == Variant::STRING)
                return String(v).is_valid_float() ? String(v).to_float() : def;
            return (double)v;
        }

        String fmt(const char *f, ...)
        {
            char buf[128];
            va_list ap;
            va_start(ap, f);
            vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return String::utf8(buf);
        }

        // FORMAT.Position.formatNode: "N47g27.5 E008g32.9" ('g' is the font's degree sign)
        String format_position(double lat, double lon)
        {
            const double alat = std::fabs(lat), alon = std::fabs(lon);
            const int dlat = (int)alat, dlon = (int)alon;
            return fmt("%s%02dg%04.1f %s%03dg%04.1f", lat >= 0.0 ? "N" : "S", dlat, (alat - dlat) * 60.0,
                    lon >= 0.0 ? "E" : "W", dlon, (alon - dlon) * 60.0);
        }
    } // namespace

    void Ht9100Mcdu::_bind_methods()
    {
        ClassDB::bind_method(D_METHOD("set_option", "name", "value"), &Ht9100Mcdu::set_option);
        ClassDB::bind_method(D_METHOD("reset"), &Ht9100Mcdu::reset);
        ClassDB::bind_method(D_METHOD("update", "props", "dt", "lat_deg", "lon_deg"), &Ht9100Mcdu::update);
        ClassDB::bind_method(D_METHOD("get_published"), &Ht9100Mcdu::get_published);
        ClassDB::bind_method(D_METHOD("key", "kind", "arg"), &Ht9100Mcdu::key);
        ClassDB::bind_method(D_METHOD("get_page"), &Ht9100Mcdu::get_page);
        ClassDB::bind_method(D_METHOD("get_scratchpad"), &Ht9100Mcdu::get_scratchpad);
    }

    Ht9100Mcdu::Ht9100Mcdu()
    {
        flight_data_reset();
        publish();
    }

    void Ht9100Mcdu::set_option(const String &name, const String &value)
    {
        if (name == "type")
            opt_type = value;
        else if (name == "engines")
            opt_engines = value;
    }

    // MCDU.reset + EditFlightData.reset (CORE.init(1)): after the engines shut down
    void Ht9100Mcdu::reset()
    {
        flight_data_reset();
        del = false;
        exec = false;
        has_last_fmc_page = false;
        messages.clear();
        page = P_IDENT;
        database_selected = true;
        scratchpad = "";
        scratchpad_old = "";
    }

    void Ht9100Mcdu::flight_data_reset()
    {
        airport_from = "";
        airport_to = "";
        flight_number = "";
        co_rte = "";
        airport_from_alt = -2000.0;
        airport_to_alt = -2000.0;
        cruise_alt = 0;
        cruise_fl = 0;
        trans_alt = 18000;
    }

    Dictionary Ht9100Mcdu::get_published() const { return st; }

    String Ht9100Mcdu::get_page() const
    {
        static const char *const names[] = { "ident", "posRef", "menu", "dataIndex", "nearestIndex", "nearestAirports", "rte", "perfInit", "fallback" };
        return names[(int)page];
    }

    String Ht9100Mcdu::get_scratchpad() const { return del ? String("DELETE") : scratchpad; }

    // ── data base: the bundled airport catalog ─────────────────────────
    void Ht9100Mcdu::load_airports()
    {
        if (airports_loaded)
            return;
        airports_loaded = true;
        Ref<FileAccess> f = FileAccess::open("res://airports.json", FileAccess::READ);
        if (f.is_null())
            return;
        const Variant parsed = JSON::parse_string(f->get_as_text());
        Array list;
        if (parsed.get_type() == Variant::ARRAY)
            list = parsed;
        else if (parsed.get_type() == Variant::DICTIONARY)
            list = Dictionary(parsed).get("airports", Array());
        airports.reserve(list.size());
        for (int i = 0; i < list.size(); i++)
        {
            if (list[i].get_type() != Variant::DICTIONARY)
                continue;
            const Dictionary a = list[i];
            Airport ap;
            ap.icao = String(a.get("icao", "")).to_upper();
            if (ap.icao.is_empty() || !a.has("lat") || !a.has("lon"))
                continue;
            ap.lat = a.get("lat", 0.0);
            ap.lon = a.get("lon", 0.0);
            ap.elevation_ft = (double)a.get("elevation_m", 0.0) / 0.3048;
            airports.push_back(ap);
        }
    }

    int Ht9100Mcdu::find_airport(const String &icao) const
    {
        for (size_t i = 0; i < airports.size(); i++)
            if (airports[i].icao == icao)
                return (int)i;
        return -1;
    }

    // NearestAirports.setup: the five nearest within 4 000 NM, when the page opens
    void Ht9100Mcdu::setup_nearest()
    {
        load_airports();
        std::vector<std::pair<double, int>> by_dist;
        by_dist.reserve(airports.size());
        for (size_t i = 0; i < airports.size(); i++)
        {
            const double d = NavaidsLib::distance_m(lat, lon, airports[i].lat, airports[i].lon);
            if (d <= 4000.0 * M_PER_NM)
                by_dist.push_back({ d, (int)i });
        }
        const size_t n = std::min<size_t>(5, by_dist.size());
        std::partial_sort(by_dist.begin(), by_dist.begin() + n, by_dist.end());
        nearest.clear();
        for (size_t i = 0; i < n; i++)
            nearest.push_back(by_dist[i].second);
    }

    // ── scratchpad and messages (HT9100-MCDU.nas) ──────────────────────
    void Ht9100Mcdu::alpha_num_key(const String &k)
    {
        if (!powered)
            return;
        if (k == "LONGCLR")
        {
            clear_message(0);
            scratchpad = "";
        }
        else if (k == "CLR")
        {
            if (!messages.empty())
            {
                del = false;
                clear_message(0);
            }
            else if (scratchpad.length() > 0)
            {
                del = false;
                scratchpad = scratchpad.left(scratchpad.length() - 1);
            }
            else
                del = !del;
        }
        else if (k == "+/-")
        {
            del = false;
            if (!messages.empty())
                clear_message(1);
            if (scratchpad.ends_with("-"))
                scratchpad = scratchpad.left(scratchpad.length() - 1) + "+";
            else if (scratchpad.ends_with("+"))
                scratchpad = scratchpad.left(scratchpad.length() - 1) + "-";
            else if (scratchpad.length() < 23)
                scratchpad += "-";
        }
        else
        {
            del = false;
            if (!messages.empty())
                clear_message(1);
            if (scratchpad.length() < 24)
                scratchpad += k;
        }
    }

    void Ht9100Mcdu::clear_message(int a)
    {
        del = false;
        if (a == 2)
        {
            messages.clear();
            scratchpad = scratchpad_old;
        }
        else if (a == 1)
        {
            messages.clear();
            scratchpad = "";
            scratchpad_old = "";
        }
        else if (messages.size() > 1)
        {
            messages.erase(messages.begin());
            scratchpad = messages[0];
        }
        else if (messages.size() == 1)
        {
            messages.clear();
            scratchpad = scratchpad_old;
        }
    }

    void Ht9100Mcdu::remove_message(const String &m)
    {
        del = false;
        const auto it = std::find(messages.begin(), messages.end(), m);
        if (it == messages.end())
            return;
        messages.erase(it);
        scratchpad = messages.empty() ? scratchpad_old : messages[0];
    }

    void Ht9100Mcdu::set_message(const String &m)
    {
        del = false;
        if (!messages.empty())
        {
            if (messages[0] != m)
            {
                remove_message(m);
                messages.insert(messages.begin(), m);
                scratchpad = m;
            }
        }
        else
        {
            messages.push_back(m);
            scratchpad_old = scratchpad;
            scratchpad = m;
        }
    }

    void Ht9100Mcdu::scratchpad_clear() { clear_message(1); }

    // 0 = the DELETE character, 2 = an entry, 1 = empty or a message
    int Ht9100Mcdu::scratchpad_state() const
    {
        if (del)
            return 0;
        if (scratchpad.length() > 0 && messages.empty())
            return 2;
        return 1;
    }

    bool Ht9100Mcdu::string_is_int(const String &s)
    {
        return s.is_valid_int();
    }

    bool Ht9100Mcdu::length_in_range(const String &s, int lo, int hi)
    {
        const int n = s.replace("-", "").length();
        return n >= lo && n <= hi;
    }

    // ── pages ──────────────────────────────────────────────────────────
    bool Ht9100Mcdu::is_fmc_page(Page p) { return p != P_MENU && p != P_FALLBACK; }

    Ht9100Mcdu::Page Ht9100Mcdu::page_by_name(const String &name) const
    {
        if (name == "ident") return P_IDENT;
        if (name == "posRef") return P_POS_REF;
        if (name == "menu") return P_MENU;
        if (name == "dataIndex") return P_DATA_INDEX;
        if (name == "nearestIndex") return P_NEAREST_INDEX;
        if (name == "nearestAirports") return P_NEAREST_AIRPORTS;
        if (name == "rte") return P_RTE;
        if (name == "perfInit") return P_PERF_INIT;
        return P_FALLBACK; // legs, depArr, hold, prog, refIndex, rte2, nearestVorDme ...
    }

    void Ht9100Mcdu::set_page(Page p)
    {
        if (is_fmc_page(page))
        {
            last_fmc_page = page;
            has_last_fmc_page = true;
        }
        page = p;
        if (page == P_NEAREST_AIRPORTS)
            setup_nearest();
        publish();
    }

    void Ht9100Mcdu::soft_key(const String &k)
    {
        if (!powered)
            return;
        const int state = scratchpad_state();
        const String pad = scratchpad;
        switch (page)
        {
            case P_MENU:
                if (k == "l1")
                    set_page(has_last_fmc_page ? last_fmc_page : P_IDENT);
                break;
            case P_IDENT:
                if (k == "r6")
                    set_page(P_POS_REF);
                break;
            case P_POS_REF:
                if (k == "r6")
                    set_page(P_RTE);
                break;
            case P_DATA_INDEX:
                if (k == "l2")
                    set_page(P_POS_REF);
                else if (k == "l6")
                    set_page(P_IDENT);
                else if (k == "r3")
                    set_page(P_NEAREST_INDEX);
                break;
            case P_NEAREST_INDEX:
                if (k == "l1")
                    set_page(P_NEAREST_AIRPORTS);
                break;
            case P_NEAREST_AIRPORTS:
                if (k == "l6")
                    set_page(P_NEAREST_INDEX);
                break;
            case P_RTE:
                if (k == "l1" || k == "r1")
                {
                    if (state == 2)
                    {
                        if (length_in_range(pad, 3, 4))
                        {
                            load_airports();
                            const int ai = find_airport(pad);
                            if (ai >= 0)
                            {
                                // EditFlightData.insertFrom / insertTo: a new pair starts over
                                if (!airport_from.is_empty() && !airport_to.is_empty())
                                {
                                    if (k == "l1")
                                    {
                                        airport_to = "";
                                        airport_to_alt = -2000.0;
                                    }
                                    else
                                    {
                                        airport_from = "";
                                        airport_from_alt = -2000.0;
                                    }
                                }
                                if (k == "l1")
                                {
                                    airport_from = pad;
                                    airport_from_alt = std::round(airports[ai].elevation_ft);
                                }
                                else
                                {
                                    airport_to = pad;
                                    airport_to_alt = std::round(airports[ai].elevation_ft);
                                }
                                co_rte = "";
                                scratchpad_clear();
                            }
                            else
                                set_message("NOT IN DATA BASE");
                        }
                        else
                            set_message("INVALID ENTRY");
                    }
                    else if (state == 0)
                        set_message("INVALID CLEAR");
                }
                else if (k == "r2")
                {
                    if (state == 2)
                    {
                        if (length_in_range(pad, 1, 10))
                        {
                            flight_number = pad;
                            scratchpad_clear();
                        }
                        else
                            set_message("INVALID ENTRY");
                    }
                    else if (state == 0)
                    {
                        flight_number = "";
                        scratchpad_clear();
                    }
                }
                else if (k == "r3")
                {
                    // company routes are flight plan files of FlightGear's
                    // route manager: none here, so none is in the data base
                    if (state == 2)
                        set_message(length_in_range(pad, 1, 10) ? "NOT IN DATA BASE" : "INVALID ENTRY");
                    else if (state == 0)
                        set_message("INVALID DELETE");
                }
                else if (k == "r6")
                    set_page(P_PERF_INIT);
                break;
            case P_PERF_INIT:
                if (k == "l1" || k == "r1")
                {
                    const int limit = k == "l1" ? 18000 : 37000; // transition altitude; Internal.crzMax
                    if (state == 2)
                    {
                        if (!string_is_int(pad))
                            set_message("INVALID ENTRY");
                        else if (length_in_range(pad, 2, 3))
                        {
                            const int v = (int)pad.to_int();
                            if (v > limit / 100)
                            {
                                set_message("INVALID ENTRY");
                                return;
                            }
                            if (k == "l1")
                                trans_alt = v * 100;
                            else
                            {
                                cruise_fl = v;
                                cruise_alt = v * 100;
                            }
                            scratchpad_clear();
                        }
                        else if (length_in_range(pad, 4, 5))
                        {
                            const int v = (int)pad.to_int();
                            if (v > limit)
                            {
                                set_message("INVALID ENTRY");
                                return;
                            }
                            if (k == "l1")
                                trans_alt = v;
                            else
                            {
                                cruise_alt = (int)std::lround(v / 100.0) * 100;
                                cruise_fl = cruise_alt / 100;
                            }
                            scratchpad_clear();
                        }
                        else
                            set_message("INVALID ENTRY");
                    }
                    else if (state == 0)
                        set_message("INVALID CLEAR");
                }
                break;
            case P_FALLBACK:
                break;
        }
    }

    bool Ht9100Mcdu::key(const String &kind, const String &arg)
    {
        if (kind == "softKey")
            soft_key(arg.to_lower());
        else if (kind == "alphaNumKey")
            alpha_num_key(arg);
        else if (kind == "clearKey")
        {
            // held 1.9 s = clear everything (MCDU.clearKey)
            if (arg == "up")
            {
                clear_timer_active = false;
                clear_timer_time = -5.0;
            }
            else if (arg == "down")
            {
                clear_timer_active = true;
                clear_timer_time = elapsed + 1.9;
                alpha_num_key("CLR");
            }
        }
        else if (kind == "pageKey")
        {
            if (powered)
                set_page(page_by_name(arg));
        }
        else if (kind == "nextPageKey" || kind == "prevPageKey")
        {
            // RTE 1 <-> RTE 2 and NEAREST AIRPORTS -> VOR/DME: pages upstream names but does not have
            if (powered && (page == P_RTE || (page == P_NEAREST_AIRPORTS && kind == "nextPageKey")))
                set_page(P_FALLBACK);
        }
        else if (kind == "execKey")
        {
        }
        else
            return false;
        publish();
        return true;
    }

    void Ht9100Mcdu::update(const Dictionary &props, double dt, double lat_deg, double lon_deg)
    {
        (void)dt;
        lat = lat_deg;
        lon = lon_deg;
        elapsed = numv(props, "sim/time/elapsed-sec", elapsed);
        powered = numv(props, "systems/electrical/bus/emer-dc", 28.0) >= 24.0;
        annun_test = numv(props, "controls/switches/annun-test", 0.0) >= 0.5;
        magvar = numv(props, "environment/magnetic-variation-deg", 0.0);
        groundspeed_kt = numv(props, "velocities/groundspeed-kt", 0.0);
        utc_h = (int)numv(props, "sim/time/utc/hour", 0.0);
        utc_m = (int)numv(props, "sim/time/utc/minute", 0.0);
        utc_s = (int)numv(props, "sim/time/utc/second", 0.0);

        // HT9100-FMS.nas CORE.loop: the system resets once the engines are shut down
        eng_on = numv(props, "engines/engine[0]/state", 0.0) == 3.0 || numv(props, "engines/engine[1]/state", 0.0) == 3.0;
        if (eng_on)
            reset_toggle = true;
        else if (reset_toggle)
        {
            reset_toggle = false;
            reset();
        }

        if (!powered && page != P_IDENT)
            page = P_IDENT;
        if (clear_timer_active && clear_timer_time < elapsed)
        {
            clear_timer_active = false;
            clear_timer_time = -5.0;
            alpha_num_key("LONGCLR");
        }
        publish();
    }

    // ── the screen ─────────────────────────────────────────────────────
    void Ht9100Mcdu::put(const String &field, const String &text, const char *color, int small)
    {
        st[String(PREFIX) + "d/" + field] = text;
        if (color != nullptr)
            st[String(PREFIX) + "c/" + field] = String(color);
        if (small >= 0)
            st[String(PREFIX) + "s/" + field] = small;
    }

    void Ht9100Mcdu::publish()
    {
        // every field blank, white, large; the label lines and the page number are small
        static const char *const kinds[] = { "L", "LL", "LB", "C", "CL", "R", "RL", "RB" };
        for (int i = 1; i <= 6; i++)
        {
            for (const char *kd : kinds)
            {
                const String k(kd);
                const String name = k.left(1) + String::num_int64(i) + k.substr(1);
                put(name, "", WHITE, k.length() == 2 && k[1] == 'L' ? 1 : 0);
            }
        }
        put("PageNum", "", WHITE, 1);
        String title;
        // C fields start ten columns in; a page moves one by whole columns (CLTranslate)
        auto centre = [](const String &text, int shift = 0) { return String(" ").repeat(10 + shift) + text; };

        switch (page)
        {
            case P_MENU:
                title = "MENU";
                put("L1", "<HT9100", GREEN);
                break;
            case P_FALLBACK:
                title = "PAGE NOT AVAIL";
                break;
            case P_IDENT:
                title = "IDENT";
                put("L1L", " MODEL");
                put("L1", opt_type);
                put("L2L", " NAV DATA");
                put("L2", database_selected ? "MD19801002" : "MD19801001");
                put("L3", database_selected ? "MD19801001" : "MD19801002", WHITE, 1);
                put("L4L", " SOFTWARE");
                put("L4", "HT9100-007B");
                put("L6L", DASHES);
                put("R1L", "ENGINES");
                put("R1", opt_engines);
                put("R2L", "ACTIVE");
                put("R2", database_selected ? "JAN29FEB26/13" : "JAN01JAN28/13");
                put("R3", database_selected ? "JAN01JAN28/13" : "JAN29FEB26/13", WHITE, 1);
                put("R6", "POS REF>", GREEN);
                break;
            case P_POS_REF:
                title = "POS REF";
                put("L1L", " POS (GPS)");
                put("L1", format_position(lat, lon));
                put("L2L", " UTC (GPS)");
                put("L2", fmt("%02d%02d.%d", utc_h, utc_m, utc_s / 10));
                put("L2B", "      Z", WHITE, 1);
                put("L3L", " RNP/ACTUAL");
                put("L3", "1.00/0.05", WHITE, 1);
                put("L4", "<DR", GREEN);
                put("L5", "<ACT RTE", GREEN);
                put("L6L", DASHES);
                put("C5L", centre("INTEGRITY PREDICTION", -8));
                put("R2L", "GS");
                put("R2", fmt("%d", (int)std::lround(groundspeed_kt)) + "  ");
                put("R2B", "KT", WHITE, 1);
                put("R3L", "SV USED");
                put("R3", "7");
                put("R5", "DEST RAIM>", GREEN);
                put("R6", "ROUTE>", GREEN);
                break;
            case P_DATA_INDEX:
                title = "DATA INDEX";
                put("C1L", centre("FLIGHT PLAN XFER", -7));
                put("L1", "<TRANSMIT", GREEN);
                put("L2", "<POS REF", GREEN);
                put("L6", "<IDENT", GREEN);
                put("R1", "RECEIVE>", GREEN);
                put("R2", "NAV DATA>", GREEN);
                put("R3", "NEAREST>", GREEN);
                put("R4", "FIX>", GREEN);
                put("R6", "MAINTENANCE>", GREEN);
                break;
            case P_NEAREST_INDEX:
                title = "NEAREST INDEX";
                put("L1", "<AIRPORT", GREEN);
                put("L2", "<VOR/DME", GREEN);
                put("L3", "<ADF", GREEN);
                put("L4", "<WAYPOINT", GREEN);
                break;
            case P_NEAREST_AIRPORTS:
                title = "NEAREST AIRPORTS";
                put("PageNum", "1/4");
                put("C1L", centre("CRS /DIST"));
                put("L6", "<NEAREST INDEX", GREEN);
                if (nearest.empty())
                    put("C1", centre("NONE WITHIN 4000NM"));
                for (size_t i = 0; i < nearest.size() && i < 5; i++)
                {
                    const Airport &ap = airports[nearest[i]];
                    double d = 0.0, b12 = 0.0, b21 = 0.0;
                    NavaidsLib::inverse(lat, lon, ap.lat, ap.lon, d, b12, b21);
                    double crs = std::fmod(std::round(b12 - magvar), 360.0);
                    if (crs < 0.0)
                        crs += 360.0;
                    const String row = String::num_int64((int64_t)i + 1);
                    put(String("L") + row, ap.icao);
                    put(String("C") + row, centre(fmt("%03.0fg/%4.0fNM", crs, std::round(d / M_PER_NM))));
                }
                break;
            case P_RTE:
                title = "RTE 1";
                put("PageNum", "1/2");
                put("L1L", " ORIGIN");
                put("L1", airport_from.is_empty() ? String("____") : airport_from, airport_from.is_empty() ? AMBER : WHITE);
                put("L2L", " RUNWAY");
                put("L2", "-----");
                put("L5", "<RTE COPY", GREEN);
                put("L6", "<RTE 2", GREEN);
                put("R1L", "DEST");
                put("R1", airport_to.is_empty() ? String("____") : airport_to, airport_to.is_empty() ? AMBER : WHITE);
                put("R2L", "FLT NO");
                put("R2", flight_number.is_empty() ? String("----------") : flight_number);
                put("R3L", "CO ROUTE");
                put("R3", co_rte.is_empty() ? String("----------") : co_rte, WHITE, co_rte.is_empty() ? 1 : 0);
                put("R6", "PERF INIT>", GREEN);
                break;
            case P_PERF_INIT:
                title = "PERF INIT";
                put("L1L", " TRANS ALT");
                put("L1", fmt("%d", trans_alt));
                put("L6L", DASHES);
                put("R1L", "CRZ ALT");
                if (cruise_fl > 0)
                    put("R1", String("FL") + fmt("%2d", cruise_fl));
                else
                    put("R1", "_____", AMBER);
                break;
        }
        // an odd title keeps the character grid (Displays/HT9100.nas updateBase)
        if (title.length() % 2 != 0)
            title = String(" ") + title;
        put("Title", title, WHITE, 0);
        put("Scratchpad", del ? String("DELETE") : scratchpad, WHITE, 0);
        st[String(PREFIX) + "exec-light"] = powered && (exec || annun_test) ? 1 : 0;
        st[String(PREFIX) + "msg-light"] = powered && (!messages.empty() || annun_test) ? 1 : 0;
        // what upstream hands to FlightGear's route manager
        st["autopilot/route-manager/departure/airport"] = airport_from;
        st["autopilot/route-manager/destination/airport"] = airport_to;
        st["autopilot/route-manager/cruise/altitude-ft"] = cruise_alt;
    }

} // namespace jsbgodot
