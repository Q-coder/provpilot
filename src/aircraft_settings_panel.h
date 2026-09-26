#ifndef AIRCRAFT_SETTINGS_PANEL_H
#define AIRCRAFT_SETTINGS_PANEL_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    class SettingsStore;

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
        void _update_live(bool force = false);

    protected:
        Callable aircraft_provider, fdm_provider, flown_provider, config_provider, name_provider;
        SettingsStore *store = nullptr;
        String aircraft_id;
        Label *title = nullptr;
        HSlider *fuel_slider = nullptr;
        Label *fuel_label = nullptr;
        Label *fuel_note = nullptr;
        GridContainer *info_grid = nullptr;
        Label *cg_label = nullptr;
        HSlider *cg_bar = nullptr;
        Label *cg_limits = nullptr;
        Timer *timer = nullptr;
        bool updating = false;

        SettingsStore *find_store() const;
        Object *fdm_if_flown() const;
        void update_fuel_label();
        static String fmt(double v);
    };

} // namespace jsbgodot

#endif
