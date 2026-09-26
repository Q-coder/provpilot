#ifndef LIVE_WEATHER_H
#define LIVE_WEATHER_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

namespace jsbgodot
{

    // Live weather (formerly live_weather.gd): fetches the nearest METAR and
    // turns it into the layer model the Weather settings tab uses (design
    // §10). Sources are pluggable through `source`; only Aviation Weather
    // Center (aviationweather.gov) exists so far. The parsing / translation
    // is static so tests can feed canned responses without a network.
    class LiveWeather : public Node
    {
        GDCLASS(LiveWeather, Node);

    public:
        // Cloud type indices match SettingsScreen's WEATHER_CLOUD_TYPES.
        enum CloudType { CT_NONE = 0, CT_CUMULUS = 1, CT_STRATUS = 2, CT_STRATOCUMULUS = 3, CT_CUMULONIMBUS = 4, CT_ALTOSTRATUS = 5, CT_ALTOCUMULUS = 6, CT_CIRRUS = 7 };

        static void _bind_methods();
        LiveWeather();

        // Fetch (if enabled) and (re)start the periodic check. Without force
        // the fetch only happens when the last one is older than REFRESH_SEC
        // or the aircraft moved MOVE_REFETCH_KM since.
        void refresh(bool force = true);
        bool needs_refetch(const Vector2 &ll, int64_t now_msec) const;
        // Where / when the last fetch was made (tests).
        void set_last_fetch(const Vector2 &ll, int64_t msec);
        static double distance_km(const Vector2 &a, const Vector2 &b);
        static String build_url(const String &src, double lat, double lon);
        // A source response -> normalised report for the station nearest to
        // (lat, lon); {} if nothing usable.
        static Dictionary parse_response(const String &src, const String &body, double lat, double lon);
        // Normalised report -> {layers, visibility_km, qnh_hpa, status, ...}.
        static Dictionary report_to_weather(const Dictionary &rep);

        String get_source() const; void set_source(const String &s);
        bool get_enabled() const; void set_enabled(bool v);
        Callable get_latlon_provider() const; void set_latlon_provider(const Callable &c);
        Dictionary get_last_weather() const; void set_last_weather(const Dictionary &d);

        void _on_check_tick();
        void _on_request_completed(int result, int code, const PackedStringArray &headers, const PackedByteArray &body);

    protected:
        static constexpr double REFRESH_SEC = 600.0;     // METARs are half-hourly
        static constexpr double CHECK_SEC = 20.0;        // timer period for age + distance
        static constexpr double MOVE_REFETCH_KM = 50.0;  // refetch sooner after moving this far
        static constexpr double SEARCH_DLAT = 1.0;       // bbox half-size, degrees
        static constexpr double SEARCH_DLON = 1.5;
        static constexpr double UNLIMITED_VIS_KM = 40.0; // "9999" / "6+" -> a clear day
        static constexpr double FT_TO_M = 0.3048;
        static constexpr double SM_TO_KM = 1.609344;
        static constexpr int SURFACE_LAYER_TOP_M = 1500;

        String source = "awc";
        bool enabled = false;
        Callable latlon_provider;
        Dictionary last_weather;
        HTTPRequest *http = nullptr;
        Timer *timer = nullptr;
        bool busy = false;
        Vector2 last_fetch_latlon = Vector2(INFINITY, INFINITY);
        int64_t last_fetch_msec = -1;

        static Dictionary parse_awc(const String &body, double lat, double lon);
        static int cover_to_oktas(const String &cover);
        static int cloud_thickness_m(int type);
    };

} // namespace jsbgodot

VARIANT_ENUM_CAST(jsbgodot::LiveWeather::CloudType);

#endif
