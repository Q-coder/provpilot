#ifndef FLIGHT_HUD_H
#define FLIGHT_HUD_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    // The ProVPilot flight HUD overlays (formerly ~350 lines of node_3d.gd):
    //
    //   - the flight readouts in the scene's Control root (BottomRow labels,
    //     the coordinate / time labels) with the H key cycling their visibility
    //     (0 none, 1 bottom readouts, 2 + upper info),
    //   - the FPS counter (Ctrl+Shift+F) and the weather overlay (Ctrl+I:
    //     METAR station / wind / QNH / clouds, or the static layers),
    //   - the intro-zoom HUD (distance to the aircraft, loading status, a
    //     progress bar, the skip hint) that replaces the flight HUD while the
    //     cinematic plays.
    //
    // This node is a CanvasLayer (layer 100) added under the main scene; the
    // intro HUD is a nested CanvasLayer on layer 5.
    class FlightHud : public CanvasLayer
    {
        GDCLASS(FlightHud, CanvasLayer);

    public:
        static void _bind_methods();
        FlightHud();

        void _ready() override;

        // The scene's flight HUD root ($Control): resolves the readout labels,
        // outlines every label and applies the current HUD mode.
        void register_flight_hud(Control *root);

        // Per frame (also while slewing): the FPS text when shown, and the
        // edge-detected keys
        //   H (no Ctrl)     cycle the HUD mode
        //   Ctrl+Shift+F    toggle the FPS counter
        //   Ctrl+I          toggle the weather overlay
        void poll_keys();
        // Per frame while flying: the readouts from the flight model
        // (airspeed, altitude, vertical speed, throttle, heading, ECEF /
        // Swiss / prime-meridian coordinates). jsb may be null.
        void update(Object *jsb);
        void set_time_text(const String &text);
        // The radio navigation line above the readouts (empty hides it).
        void set_nav_text(const String &text);
        // Readout units: metric (km/h, m, m/s) for aircraft whose panel is
        // metric (aircraft.cfg [instruments] units "metric"), else kt / ft / fpm.
        void set_metric(bool on);
        bool is_metric() const;

        int get_hud_mode() const;
        void set_hud_mode(int mode);
        bool is_fps_visible() const;
        void set_fps_visible(bool visible);

        // ── Weather overlay ──
        void set_weather_overlay(bool show);
        bool is_weather_overlay_visible() const;
        // The state the overlay text is built from: whether live weather is
        // on, the last live report (empty while none arrived), the static
        // layer stack + QNH + visibility from the settings.
        void set_weather_state(bool live, const Dictionary &live_weather, const Array &static_layers, double qnh_hpa, double visibility_km);
        Label *get_weather_label() const;

        // ── Intro HUD ──
        // Builds it once; hides the flight HUD while shown.
        void show_intro_hud(bool show);
        bool is_intro_hud_visible() const;
        void set_intro_status(const String &text);
        void set_intro_bar(double frac);
        // Restart the tile-progress bar (a new loading phase / a new intro).
        void reset_intro_progress();
        // Per frame while the intro plays: the distance readout and, unless
        // the destination is still loading (the bar then shows the loading
        // phases), the monotonic tile-progress bar from the streamer's
        // pending count (-1 = unknown).
        void update_intro(double distance_m, int pending_count, bool loading);
        CanvasLayer *get_intro_hud() const;
        Label *get_intro_dist_label() const;
        Label *get_intro_status_label() const;
        ColorRect *get_intro_bar() const;
        ColorRect *get_intro_bar_bg() const;

        // "2'681'931" style thousands separators.
        static String format_swiss_coord(double value);
        // Degrees + decimal minutes with hemisphere, e.g. "N 60°11.63′" (design §3.4).
        static String format_pm_coord(double deg, bool is_lat);

    protected:
        // Flight HUD (scene nodes)
        Control *hud_root = nullptr;
        Label *airspeed_label = nullptr;
        bool metric = false;
        Label *altitude_label = nullptr;
        Label *vspeed_label = nullptr;
        Label *throttle_label = nullptr;
        Label *heading_label = nullptr;
        Label *ecef_label = nullptr;   // Control/Label6
        Label *swiss_label = nullptr;  // Control/Label7
        Label *pm_label = nullptr;     // Control/Label9
        Label *time_label = nullptr;   // Control/TimeLabel
        int hud_mode = 0;

        // Own overlays
        Label *fps_label = nullptr;
        Label *wx_label = nullptr;
        Label *nav_label = nullptr;
        String nav_text;
        bool fps_visible = false;
        bool wx_visible = false;
        bool hud_key_held = false;
        bool fps_key_held = false;
        bool wx_key_held = false;

        // Weather overlay state
        bool wx_live = false;
        Dictionary wx_live_weather;
        Array wx_static_layers;
        double wx_qnh_hpa = 1013.25;
        double wx_visibility_km = 40.0;

        // Intro HUD
        CanvasLayer *intro_hud = nullptr;
        Label *intro_dist_label = nullptr;
        Label *intro_status_label = nullptr;
        ColorRect *intro_bar_bg = nullptr;
        ColorRect *intro_bar = nullptr;
        int intro_pending_prev = -1; // pending count last frame (-1 = none yet)
        int intro_done = 0;          // completed loads since the intro started
        double intro_shown = 0.0;    // displayed bar fraction (monotonic)

        void build_overlays();
        void build_intro_hud();
        void apply_hud_mode();
        void refresh_weather_overlay();
        String weather_overlay_text() const;
        static void outline_label(Label *label, int px);
        static void outline_labels_recursive(Node *node);
        Label *hud_label(const char *path) const;
    };

} // namespace jsbgodot

#endif
