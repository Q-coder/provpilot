#ifndef WELCOME_SCREEN_H
#define WELCOME_SCREEN_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

namespace jsbgodot
{

    // Startup screen with aircraft / airport selection (formerly
    // welcome_screen.gd). The user picks an aircraft, an airport and a runway,
    // then Fly! (or Edit Mode, or Continue Flight); Settings opens the
    // settings page. Also runs the update check against the release feed and
    // the first-run scenery-folder wizard.
    //
    // Signals: flight_requested(aircraft_id, swiss_e, swiss_n, heading_deg),
    // edit_requested(swiss_e, swiss_n, heading_deg), settings_requested,
    // continue_requested, scenery_setup_requested(path).
    class WelcomeScreen : public CanvasLayer
    {
        GDCLASS(WelcomeScreen, CanvasLayer);

    public:
        static void _bind_methods();
        WelcomeScreen();

        void _ready() override;
        void _input(const Ref<InputEvent> &event) override;
        void _unhandled_input(const Ref<InputEvent> &event) override;

        void populate_airports(const TypedArray<Dictionary> &airports);
        // Regions whose base scenery pack is installed (AirportsLib helpers).
        void set_installed_regions(const Dictionary &regions);
        void show_welcome();
        void hide_welcome();
        // "Continue Flight" once a flight is in progress.
        void set_continue_enabled(bool enabled);
        String get_selected_aircraft_id() const;
        static String aircraft_label(const String &id);

        void _on_aircraft_selected(int index);
        void _on_airport_selected(int index);
        void _on_airport_activated(int index);
        void _on_search_changed(const String &text);
        void _on_filter_toggled(bool on);
        void _on_fly_pressed();
        void _on_edit_pressed();
        void _on_settings_pressed();
        void _on_about_pressed();
        void _on_continue_pressed();
        void _on_quit_pressed();
        void _on_update_request_completed(int result, int code, const PackedStringArray &headers, const PackedByteArray &body);
        void _on_update_meta_clicked(const Variant &meta);
        void _on_about_meta_clicked(const Variant &meta);
        void _maybe_show_first_run_wizard();
        void _on_first_run_browse_pressed();
        void _on_first_run_dir_selected(const String &path);
        void _on_first_run_confirmed();
        void _on_first_run_canceled();

    protected:
        TypedArray<Dictionary> airports;
        Vector<Dictionary> filtered_airports;
        int selected_aircraft_idx = 0;
        Dictionary selected_airport;
        Dictionary installed_regions;

        ColorRect *bg = nullptr;
        RichTextLabel *update_label = nullptr;
        HTTPRequest *update_request = nullptr;
        String latest_download_url;
        ItemList *aircraft_list = nullptr;
        Label *aircraft_desc = nullptr;
        LineEdit *search_input = nullptr;
        ItemList *airport_list = nullptr;
        RichTextLabel *airport_info = nullptr;
        OptionButton *runway_picker = nullptr;
        Button *fly_button = nullptr;
        Button *continue_button = nullptr;
        Label *status_label = nullptr;
        CheckBox *filter_international = nullptr;
        CheckBox *filter_regional = nullptr;
        CheckBox *filter_grass = nullptr;
        CheckBox *filter_heliport = nullptr;
        AcceptDialog *first_run_dialog = nullptr;
        LineEdit *first_run_path_edit = nullptr;

        void build_ui();
        void build_aircraft_panel(HBoxContainer *parent);
        void build_airport_panel(HBoxContainer *parent);
        void build_details_panel(HBoxContainer *parent);
        void build_bottom_bar(VBoxContainer *parent);
        CheckBox *make_filter_checkbox(const String &label, bool checked);
        void populate_aircraft_list();
        bool type_matches_filter(const String &ap_type) const;
        void apply_filters();
        void rebuild_airport_list();
        String availability_marker(const Dictionary &airport) const;
        void update_airport_info();
        void populate_runways();
        Dictionary get_selected_departure() const;
        void show_about_dialog();
        void check_for_updates();
        static bool is_remote_newer(const String &remote, const String &current);
        bool has_scenery_areas_configured() const;
        void show_first_run_dialog();
        String first_run_default_path() const;
    };

} // namespace jsbgodot

#endif
