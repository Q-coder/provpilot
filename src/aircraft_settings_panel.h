#ifndef AIRCRAFT_SETTINGS_PANEL_H
#define AIRCRAFT_SETTINGS_PANEL_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    class SettingsStore;
    class AircraftConfig;

    // Settings > Aircraft tab (formerly aircraft_settings_panel.gd): per-aircraft
    // settings and reference data for the aircraft selected on the welcome
    // page (or the one being flown).
    //
    // Settings live in the SettingsStore as `aircraft_<id>_<name>` (section
    // [aircraft] of provpilot.cfg): aircraft_<id>_fuel_pct = fuel load in % of
    // every tank's capacity, applied at flight start and, when that aircraft
    // is being flown, immediately on change. Reference data comes from
    // aircraft.cfg [performance]; the centre of gravity is read live from
    // JSBSim while the aircraft is flown. The host wires the providers.
    class AircraftSettingsPanel : public VBoxContainer
    {
        GDCLASS(AircraftSettingsPanel, VBoxContainer);

    public:
        static void _bind_methods();
        AircraftSettingsPanel();

        void _ready() override;

        // Re-read the selection and the stored settings (show, cancel, defaults).
        void refresh();
        // Fill every tank of a JSBGodot to pct of its capacity.
        static void apply_fuel(Object *jsb, double pct);
        String fuel_key() const;
        // Start conditions (per aircraft): height above the ground in ft and
        // airspeed in kt (0 / 0 = on the runway), heading in degrees or NaN for
        // the runway heading. Defaults from aircraft.cfg [start] agl_ft / speed_kt.
        String start_key(const char *name) const;
        static double start_agl_ft(SettingsStore *store, const Ref<AircraftConfig> &cfg, const String &id);
        static double start_speed_kt(SettingsStore *store, const Ref<AircraftConfig> &cfg, const String &id);
        static double start_heading_deg(SettingsStore *store, const String &id);
        // Controls: do the throttle inputs (keys, gamepad triggers, axis) work the
        // airbrakes (gliders)? Stored as aircraft_<id>_throttle_drives = "airbrakes" /
        // "throttle"; default from aircraft.cfg [controls] throttle_drives.
        static bool throttle_drives_airbrakes(SettingsStore *store, const Ref<AircraftConfig> &cfg, const String &id);
        // Liveries: aircraft.cfg [liveries] catalog names a model/liveries/<variant>.json
        // (scripts/fg_liveries.py): {defaults {prop: file}, liveries [{id, name, textures {prop: file}}]}.
        // The choice is stored as aircraft_<id>_livery = livery id ("" = the paint the
        // glb was built with); the rig applies it at flight start and, when that
        // aircraft is flown, on the livery_changed signal.
        static Dictionary livery_catalog(const Ref<AircraftConfig> &cfg, const String &id);
        static String livery_key(const String &id);
        OptionButton *get_livery_button() const;
        void _on_livery_selected(int idx);
        // Test access.
        String get_aircraft_id() const;
        Label *get_title_label() const;
        GridContainer *get_info_grid() const;
        Label *get_cg_label() const;
        HSlider *get_fuel_slider() const;

        Callable get_aircraft_provider() const; void set_aircraft_provider(const Callable &c); // -> aircraft id
        Callable get_fdm_provider() const; void set_fdm_provider(const Callable &c);           // -> JSBGodot or null
        Callable get_flown_provider() const; void set_flown_provider(const Callable &c);       // -> flown id ("" before the first flight)
        Callable get_config_provider() const; void set_config_provider(const Callable &c);     // id -> AircraftConfig or null
        Callable get_name_provider() const; void set_name_provider(const Callable &c);         // id -> display name

        void _on_fuel_changed(double v);
        void _on_start_changed(double v);
        void _on_runway_heading_toggled(bool on);
        void _on_throttle_drives_selected(int idx);
        void _update_live(bool force = false);

    protected:
        Callable aircraft_provider, fdm_provider, flown_provider, config_provider, name_provider;
        SettingsStore *store = nullptr;
        String aircraft_id;
        Label *title = nullptr;
        HSlider *fuel_slider = nullptr;
        Label *fuel_label = nullptr;
        Label *fuel_note = nullptr;
        SpinBox *start_agl = nullptr;
        SpinBox *start_speed = nullptr;
        SpinBox *start_heading = nullptr;
        CheckBox *runway_heading = nullptr;
        OptionButton *throttle_drives = nullptr;
        Label *livery_title = nullptr;
        HBoxContainer *livery_row = nullptr;
        HSeparator *livery_sep = nullptr;
        OptionButton *livery = nullptr;
        PackedStringArray livery_ids; // per option: "" (as built) or the livery id
        Label *controls_note = nullptr;
        Label *start_note = nullptr;
        GridContainer *info_grid = nullptr;
        Label *cg_label = nullptr;
        HSlider *cg_bar = nullptr;
        Label *cg_limits = nullptr;
        Timer *timer = nullptr;
        bool updating = false;

        SettingsStore *find_store() const;
        Object *fdm_if_flown() const;
        void update_fuel_label();
        void update_start_note();
        static String fmt(double v);
    };

} // namespace jsbgodot

#endif
