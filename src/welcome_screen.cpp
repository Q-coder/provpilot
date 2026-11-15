#include "navaids_lib.h"
#include "welcome_screen.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdio>

#include "install_config.h"
#include "settings_store.h"

using namespace godot;

namespace jsbgodot
{

static const int FONT_SM = 20, FONT_MD = 24, FONT_LG = 28, FONT_XL = 36, FONT_TITLE = 48;
static const char *UPDATE_LATEST_URL = "https://www.md80.ch/provpilot/releases/latest.json";
static const char *UPDATE_BASE_URL = "https://www.md80.ch/provpilot/releases/";
static const Color BG_COLOR(0.08, 0.10, 0.14, 1.0);
static const Color PANEL_COLOR(0.12, 0.14, 0.18, 1.0);
static const Color ACCENT_COLOR(0.25, 0.55, 0.85, 1.0);
static const Color ACCENT_HOVER(0.35, 0.65, 0.95, 1.0);

// id: JSBSim model name, label: display name, description
static const struct { const char *id; const char *label; const char *desc; } AIRCRAFT[] = {
	{"c172p", "Cessna 172P Skyhawk", "Single-engine piston, 4-seat trainer. 160 HP."},
	{"pc7", "Pilatus PC-7 Turbo Trainer", "Single-engine turboprop military trainer. PT6A, ~650 SHP."},
	{"a10", "Fairchild A-10A Thunderbolt II", "Twin-engine close air support jet. 2x GE TF34 turbofans."},
	// the MD-80 family: md80 is the MD-82; md81 / md83 / md87 are variants of it
	// (aircraft.cfg [aircraft] base "md80": own FDM, livery folder and, for the
	// MD-87, airframe); liveries in Settings > Aircraft
	{"md81", "McDonnell Douglas MD-81", "Twin-jet airliner. 2x P&W JT8D-217. FlightGear MD-80 (Octal450); liveries in Settings > Aircraft."},
	{"md80", "McDonnell Douglas MD-82", "Twin-jet airliner. 2x P&W JT8D-217A. FlightGear MD-80 (Octal450); liveries in Settings > Aircraft."},
	{"md83", "McDonnell Douglas MD-83", "Twin-jet airliner, longer range. 2x P&W JT8D-219, auxiliary tanks. FlightGear MD-80 (Octal450)."},
	{"md87", "McDonnell Douglas MD-87", "Short-fuselage MD-80. 2x P&W JT8D-217C. FlightGear MD-80 (Octal450)."},
	{"ask21", "Schleicher ASK 21", "Two-seat training glider. No winch or tow yet: set the start height and speed in Settings > Aircraft."},
};
static const int AIRCRAFT_COUNT = 8;
// Ground vehicles (aircraft/<id>/aircraft.cfg with a [vehicle] section): the
// "Vehicles" frame below the aircraft. Fly! places them on the runway;
// DriveMode drives a car, a train is placed only (for now).
static const struct { const char *id; const char *label; const char *desc; } VEHICLES[] = {
	{"car", "Low poly car", "Small car for road testing: W / S drive, A / D steer, B handbrake (Settings > Keyboard, Vehicle)."},
	{"re44", "SBB Re 4/4 locomotive", "Swiss electric locomotive (CC-BY-4.0, hotdoghans). Placed on the runway; no train motion yet."},
};
static const int VEHICLE_COUNT = 2;

void WelcomeScreen::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("populate_airports", "airports"), &WelcomeScreen::populate_airports);
	ClassDB::bind_method(D_METHOD("set_installed_regions", "regions"), &WelcomeScreen::set_installed_regions);
	ClassDB::bind_method(D_METHOD("show_welcome"), &WelcomeScreen::show_welcome);
	ClassDB::bind_method(D_METHOD("hide_welcome"), &WelcomeScreen::hide_welcome);
	ClassDB::bind_method(D_METHOD("set_continue_enabled", "enabled"), &WelcomeScreen::set_continue_enabled);
	ClassDB::bind_method(D_METHOD("get_selected_aircraft_id"), &WelcomeScreen::get_selected_aircraft_id);
	ClassDB::bind_method(D_METHOD("get_selected_departure"), &WelcomeScreen::get_selected_departure);
	ClassDB::bind_static_method("WelcomeScreen", D_METHOD("aircraft_label", "id"), &WelcomeScreen::aircraft_label);
	ClassDB::bind_method(D_METHOD("_on_aircraft_selected", "index"), &WelcomeScreen::_on_aircraft_selected);
	ClassDB::bind_method(D_METHOD("_on_vehicle_selected", "index"), &WelcomeScreen::_on_vehicle_selected);
	ClassDB::bind_method(D_METHOD("is_vehicle_selected"), &WelcomeScreen::is_vehicle_selected);
	ClassDB::bind_method(D_METHOD("_on_airport_selected", "index"), &WelcomeScreen::_on_airport_selected);
	ClassDB::bind_method(D_METHOD("_on_airport_activated", "index"), &WelcomeScreen::_on_airport_activated);
	ClassDB::bind_method(D_METHOD("_on_search_changed", "text"), &WelcomeScreen::_on_search_changed);
	ClassDB::bind_method(D_METHOD("_on_filter_toggled", "on"), &WelcomeScreen::_on_filter_toggled);
	ClassDB::bind_method(D_METHOD("_on_fly_pressed"), &WelcomeScreen::_on_fly_pressed);
	ClassDB::bind_method(D_METHOD("_on_edit_pressed"), &WelcomeScreen::_on_edit_pressed);
	ClassDB::bind_method(D_METHOD("_on_settings_pressed"), &WelcomeScreen::_on_settings_pressed);
	ClassDB::bind_method(D_METHOD("_on_about_pressed"), &WelcomeScreen::_on_about_pressed);
	ClassDB::bind_method(D_METHOD("_on_continue_pressed"), &WelcomeScreen::_on_continue_pressed);
	ClassDB::bind_method(D_METHOD("_on_quit_pressed"), &WelcomeScreen::_on_quit_pressed);
	ClassDB::bind_method(D_METHOD("_on_update_request_completed", "result", "code", "headers", "body"), &WelcomeScreen::_on_update_request_completed);
	ClassDB::bind_method(D_METHOD("_on_update_meta_clicked", "meta"), &WelcomeScreen::_on_update_meta_clicked);
	ClassDB::bind_method(D_METHOD("_on_about_meta_clicked", "meta"), &WelcomeScreen::_on_about_meta_clicked);
	ClassDB::bind_method(D_METHOD("_maybe_show_first_run_wizard"), &WelcomeScreen::_maybe_show_first_run_wizard);
	ClassDB::bind_method(D_METHOD("_on_first_run_browse_pressed"), &WelcomeScreen::_on_first_run_browse_pressed);
	ClassDB::bind_method(D_METHOD("_on_first_run_dir_selected", "path"), &WelcomeScreen::_on_first_run_dir_selected);
	ClassDB::bind_method(D_METHOD("_on_first_run_confirmed"), &WelcomeScreen::_on_first_run_confirmed);
	ClassDB::bind_method(D_METHOD("_on_first_run_canceled"), &WelcomeScreen::_on_first_run_canceled);
	ADD_SIGNAL(MethodInfo("flight_requested", PropertyInfo(Variant::STRING, "aircraft_id"), PropertyInfo(Variant::FLOAT, "swiss_e"),
			PropertyInfo(Variant::FLOAT, "swiss_n"), PropertyInfo(Variant::FLOAT, "heading_deg")));
	ADD_SIGNAL(MethodInfo("edit_requested", PropertyInfo(Variant::FLOAT, "swiss_e"), PropertyInfo(Variant::FLOAT, "swiss_n"),
			PropertyInfo(Variant::FLOAT, "heading_deg")));
	ADD_SIGNAL(MethodInfo("settings_requested"));
	ADD_SIGNAL(MethodInfo("continue_requested"));
	ADD_SIGNAL(MethodInfo("scenery_setup_requested", PropertyInfo(Variant::STRING, "path")));
}

WelcomeScreen::WelcomeScreen()
{
}

void WelcomeScreen::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	set_layer(100); // on top of everything
	build_ui();
	populate_aircraft_list();
	check_for_updates();
	// Defer one frame so the host has time to connect its signal handler.
	call_deferred("_maybe_show_first_run_wizard");
}

// ── Public API ──────────────────────────────────────────────────────────────

void WelcomeScreen::populate_airports(const TypedArray<Dictionary> &p_airports)
{
	airports = p_airports;
	// the ILS geometry follows the same runways the list offers (pack runways merged)
	NavaidsLib::set_airports(p_airports);
	apply_filters();
	if (!filtered_airports.is_empty())
	{
		airport_list->select(0);
		_on_airport_selected(0);
	}
}

void WelcomeScreen::set_installed_regions(const Dictionary &regions)
{
	installed_regions = regions;
	if (airport_list != nullptr)
	{
		rebuild_airport_list();
	}
}

void WelcomeScreen::show_welcome()
{
	set_visible(true);
	if (search_input != nullptr)
	{
		search_input->grab_focus();
	}
}

void WelcomeScreen::hide_welcome()
{
	set_visible(false);
}

void WelcomeScreen::set_continue_enabled(bool enabled)
{
	if (continue_button != nullptr)
	{
		continue_button->set_visible(enabled);
		continue_button->set_disabled(!enabled);
	}
}

String WelcomeScreen::get_selected_aircraft_id() const
{
	if (selected_vehicle_idx >= 0 && selected_vehicle_idx < VEHICLE_COUNT)
	{
		return String(VEHICLES[selected_vehicle_idx].id);
	}
	return (selected_aircraft_idx >= 0 && selected_aircraft_idx < AIRCRAFT_COUNT) ? String(AIRCRAFT[selected_aircraft_idx].id) : String("c172p");
}

bool WelcomeScreen::is_vehicle_selected() const
{
	return selected_vehicle_idx >= 0 && selected_vehicle_idx < VEHICLE_COUNT;
}

String WelcomeScreen::aircraft_label(const String &id)
{
	for (const auto &a : AIRCRAFT)
	{
		if (id == a.id)
		{
			return a.label;
		}
	}
	for (const auto &v : VEHICLES)
	{
		if (id == v.id)
		{
			return v.label;
		}
	}
	return id;
}

// ── UI construction ─────────────────────────────────────────────────────────

static Label *header_label(const String &text, int size, const Color &color)
{
	Label *l = memnew(Label);
	l->set_text(text);
	l->add_theme_font_size_override("font_size", size);
	l->add_theme_color_override("font_color", color);
	return l;
}

static Ref<StyleBoxFlat> rounded_box(const Color &color, int radius)
{
	Ref<StyleBoxFlat> sb = memnew(StyleBoxFlat);
	sb->set_bg_color(color);
	sb->set_corner_radius_all(radius);
	return sb;
}

static Button *big_button(const String &text, const Vector2 &min_size, int font_size)
{
	Button *b = memnew(Button);
	b->set_text(text);
	b->set_custom_minimum_size(min_size);
	b->add_theme_font_size_override("font_size", font_size);
	return b;
}

void WelcomeScreen::build_ui()
{
	bg = memnew(ColorRect);
	bg->set_color(BG_COLOR);
	bg->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(bg);

	// Panel anchored to fill the viewport with a uniform margin.
	MarginContainer *margin = memnew(MarginContainer);
	margin->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	margin->add_theme_constant_override("margin_left", 32);
	margin->add_theme_constant_override("margin_right", 32);
	margin->add_theme_constant_override("margin_top", 32);
	margin->add_theme_constant_override("margin_bottom", 32);
	bg->add_child(margin);

	PanelContainer *main_panel = memnew(PanelContainer);
	Ref<StyleBoxFlat> panel_style = rounded_box(PANEL_COLOR, 16);
	panel_style->set_content_margin(SIDE_LEFT, 48);
	panel_style->set_content_margin(SIDE_RIGHT, 48);
	panel_style->set_content_margin(SIDE_TOP, 40);
	panel_style->set_content_margin(SIDE_BOTTOM, 40);
	main_panel->add_theme_stylebox_override("panel", panel_style);
	main_panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	main_panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	margin->add_child(main_panel);

	VBoxContainer *main_vbox = memnew(VBoxContainer);
	main_vbox->add_theme_constant_override("separation", 20);
	main_panel->add_child(main_vbox);

	// Title
	HBoxContainer *title_hbox = memnew(HBoxContainer);
	title_hbox->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	main_vbox->add_child(title_hbox);
	Label *title = header_label("ProVPilot", FONT_TITLE, ACCENT_COLOR);
	title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	title_hbox->add_child(title);
	title_hbox->add_child(header_label(String::utf8("  \xE2\x80\x94  Swiss Flight Simulator"), FONT_LG, Color(0.6, 0.65, 0.7)));

	// Update-available banner (hidden until the version check resolves)
	update_label = memnew(RichTextLabel);
	update_label->set_use_bbcode(true);
	update_label->set_fit_content(true);
	update_label->set_scroll_active(false);
	update_label->add_theme_font_size_override("normal_font_size", FONT_SM);
	update_label->add_theme_color_override("default_color", Color(0.95, 0.8, 0.3));
	update_label->set_visible(false);
	update_label->connect("meta_clicked", Callable(this, "_on_update_meta_clicked"));
	main_vbox->add_child(update_label);
	main_vbox->add_child(memnew(HSeparator));

	// Three columns
	HBoxContainer *content_hbox = memnew(HBoxContainer);
	content_hbox->add_theme_constant_override("separation", 32);
	content_hbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_vbox->add_child(content_hbox);
	build_aircraft_panel(content_hbox);
	build_airport_panel(content_hbox);
	build_details_panel(content_hbox);

	main_vbox->add_child(memnew(HSeparator));
	build_bottom_bar(main_vbox);
}

static VBoxContainer *column(HBoxContainer *parent, double ratio, int separation)
{
	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	vbox->set_stretch_ratio(ratio);
	vbox->add_theme_constant_override("separation", separation);
	parent->add_child(vbox);
	return vbox;
}

void WelcomeScreen::build_aircraft_panel(HBoxContainer *parent)
{
	VBoxContainer *vbox = column(parent, 0.7, 12);
	vbox->add_child(header_label("Aircraft", FONT_LG, ACCENT_COLOR));
	aircraft_list = memnew(ItemList);
	aircraft_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	aircraft_list->add_theme_font_size_override("font_size", FONT_MD);
	aircraft_list->connect("item_selected", Callable(this, "_on_aircraft_selected"));
	vbox->add_child(aircraft_list);
	aircraft_desc = memnew(Label);
	aircraft_desc->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	aircraft_desc->add_theme_font_size_override("font_size", FONT_SM);
	aircraft_desc->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	aircraft_desc->set_custom_minimum_size(Vector2(0, 60));
	vbox->add_child(aircraft_desc);
	// Ground vehicles: their own frame, one selection shared with the aircraft
	vbox->add_child(header_label("Vehicles", FONT_LG, ACCENT_COLOR));
	vehicle_list = memnew(ItemList);
	vehicle_list->set_name("VehicleList");
	vehicle_list->set_custom_minimum_size(Vector2(0, 40 * VEHICLE_COUNT + 12));
	vehicle_list->add_theme_font_size_override("font_size", FONT_MD);
	vehicle_list->connect("item_selected", Callable(this, "_on_vehicle_selected"));
	vbox->add_child(vehicle_list);
}

void WelcomeScreen::build_airport_panel(HBoxContainer *parent)
{
	VBoxContainer *vbox = column(parent, 1.3, 10);
	vbox->add_child(header_label("Starting Airport", FONT_LG, ACCENT_COLOR));
	// Search bar
	HBoxContainer *search_hbox = memnew(HBoxContainer);
	search_hbox->add_theme_constant_override("separation", 8);
	Label *search_label = memnew(Label);
	search_label->set_text("Search:");
	search_label->add_theme_font_size_override("font_size", FONT_MD);
	search_hbox->add_child(search_label);
	search_input = memnew(LineEdit);
	search_input->set_placeholder("ICAO or name...");
	search_input->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	search_input->add_theme_font_size_override("font_size", FONT_MD);
	search_input->set_custom_minimum_size(Vector2(0, 44));
	search_input->connect("text_changed", Callable(this, "_on_search_changed"));
	search_input->set_clear_button_enabled(true);
	search_hbox->add_child(search_input);
	vbox->add_child(search_hbox);
	// Type filters
	HBoxContainer *filter_hbox = memnew(HBoxContainer);
	filter_hbox->add_theme_constant_override("separation", 16);
	filter_international = make_filter_checkbox("Intl", true);
	filter_regional = make_filter_checkbox("Regional", true);
	filter_grass = make_filter_checkbox("Grass", true);
	filter_heliport = make_filter_checkbox("Heli", false);
	filter_hbox->add_child(filter_international);
	filter_hbox->add_child(filter_regional);
	filter_hbox->add_child(filter_grass);
	filter_hbox->add_child(filter_heliport);
	vbox->add_child(filter_hbox);
	// Airport list
	airport_list = memnew(ItemList);
	airport_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	airport_list->add_theme_font_size_override("font_size", FONT_SM);
	airport_list->connect("item_selected", Callable(this, "_on_airport_selected"));
	airport_list->connect("item_activated", Callable(this, "_on_airport_activated"));
	vbox->add_child(airport_list);
}

void WelcomeScreen::build_details_panel(HBoxContainer *parent)
{
	VBoxContainer *vbox = column(parent, 0.9, 12);
	vbox->add_child(header_label("Airport Details", FONT_LG, ACCENT_COLOR));
	airport_info = memnew(RichTextLabel);
	airport_info->set_use_bbcode(true);
	airport_info->set_fit_content(false);
	airport_info->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	airport_info->add_theme_font_size_override("normal_font_size", FONT_SM);
	airport_info->add_theme_font_size_override("bold_font_size", FONT_SM);
	vbox->add_child(airport_info);
	// Runway selector
	HBoxContainer *rwy_hbox = memnew(HBoxContainer);
	rwy_hbox->add_theme_constant_override("separation", 8);
	Label *rwy_label = memnew(Label);
	rwy_label->set_text("Runway:");
	rwy_label->add_theme_font_size_override("font_size", FONT_MD);
	rwy_label->set_custom_minimum_size(Vector2(100, 0));
	rwy_hbox->add_child(rwy_label);
	runway_picker = memnew(OptionButton);
	runway_picker->add_theme_font_size_override("font_size", FONT_MD);
	runway_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	runway_picker->set_custom_minimum_size(Vector2(0, 44));
	runway_picker->get_popup()->add_theme_font_size_override("font_size", FONT_MD);
	rwy_hbox->add_child(runway_picker);
	vbox->add_child(rwy_hbox);
	// Status
	status_label = memnew(Label);
	status_label->add_theme_color_override("font_color", Color(1.0, 0.27, 0.0));
	status_label->add_theme_font_size_override("font_size", FONT_SM);
	vbox->add_child(status_label);
}

void WelcomeScreen::build_bottom_bar(VBoxContainer *parent)
{
	HBoxContainer *hbox = memnew(HBoxContainer);
	hbox->add_theme_constant_override("separation", 16);
	parent->add_child(hbox);

	Button *settings_btn = big_button("Settings", Vector2(180, 52), FONT_MD);
	settings_btn->connect("pressed", Callable(this, "_on_settings_pressed"));
	hbox->add_child(settings_btn);
	Button *about_btn = big_button("About", Vector2(140, 52), FONT_MD);
	about_btn->connect("pressed", Callable(this, "_on_about_pressed"));
	hbox->add_child(about_btn);
	Button *quit_btn = big_button("Quit", Vector2(140, 52), FONT_MD);
	quit_btn->connect("pressed", Callable(this, "_on_quit_pressed"));
	hbox->add_child(quit_btn);

	Label *version_label = memnew(Label);
	version_label->set_text("v" + String(ProjectSettings::get_singleton()->get_setting("application/config/version", "0.0.0")));
	version_label->add_theme_font_size_override("font_size", FONT_SM);
	version_label->add_theme_color_override("font_color", Color(0.4, 0.4, 0.4));
	version_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	version_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	hbox->add_child(version_label);

	// Continue (shown only when a flight is in progress): back to the cockpit
	// without re-teleporting.
	continue_button = big_button("Continue Flight", Vector2(220, 56), FONT_LG);
	continue_button->connect("pressed", Callable(this, "_on_continue_pressed"));
	continue_button->set_visible(false);
	continue_button->set_disabled(true);
	continue_button->add_theme_stylebox_override("normal", rounded_box(Color(0.20, 0.45, 0.30), 8));
	continue_button->add_theme_stylebox_override("hover", rounded_box(Color(0.28, 0.58, 0.40), 8));
	continue_button->add_theme_color_override("font_color", Color(1, 1, 1));
	hbox->add_child(continue_button);

	// Edit Mode: straight into the scenery editor at the selected airport
	Button *edit_btn = big_button("Edit Mode", Vector2(180, 56), FONT_MD);
	edit_btn->set_tooltip_text("Scenery editor over the selected airport (no aircraft)");
	edit_btn->connect("pressed", Callable(this, "_on_edit_pressed"));
	hbox->add_child(edit_btn);

	// Fly! (prominent): restarts the flight at the chosen airport.
	fly_button = big_button("Fly!", Vector2(220, 56), FONT_XL);
	fly_button->connect("pressed", Callable(this, "_on_fly_pressed"));
	fly_button->add_theme_stylebox_override("normal", rounded_box(ACCENT_COLOR, 8));
	fly_button->add_theme_stylebox_override("hover", rounded_box(ACCENT_HOVER, 8));
	fly_button->add_theme_color_override("font_color", Color(1, 1, 1));
	hbox->add_child(fly_button);
}

// ── Aircraft ────────────────────────────────────────────────────────────────

void WelcomeScreen::populate_aircraft_list()
{
	aircraft_list->clear();
	for (const auto &a : AIRCRAFT)
	{
		aircraft_list->add_item(a.label);
	}
	if (vehicle_list != nullptr)
	{
		vehicle_list->clear();
		for (const auto &v : VEHICLES)
		{
			vehicle_list->add_item(v.label);
		}
	}
	if (AIRCRAFT_COUNT > 0)
	{
		aircraft_list->select(0);
		_on_aircraft_selected(0);
	}
}

void WelcomeScreen::_on_aircraft_selected(int index)
{
	selected_aircraft_idx = index;
	selected_vehicle_idx = -1;
	if (vehicle_list != nullptr)
	{
		vehicle_list->deselect_all();
	}
	if (index >= 0 && index < AIRCRAFT_COUNT)
	{
		aircraft_desc->set_text(AIRCRAFT[index].desc);
	}
}

void WelcomeScreen::_on_vehicle_selected(int index)
{
	if (index < 0 || index >= VEHICLE_COUNT)
	{
		return;
	}
	selected_vehicle_idx = index;
	if (aircraft_list != nullptr)
	{
		aircraft_list->deselect_all();
	}
	aircraft_desc->set_text(VEHICLES[index].desc);
}

// ── Airport filter & search ─────────────────────────────────────────────────

CheckBox *WelcomeScreen::make_filter_checkbox(const String &label, bool checked)
{
	CheckBox *cb = memnew(CheckBox);
	cb->set_text(label);
	cb->set_pressed(checked);
	cb->add_theme_font_size_override("font_size", FONT_SM);
	cb->connect("toggled", Callable(this, "_on_filter_toggled"));
	return cb;
}

void WelcomeScreen::_on_filter_toggled(bool)
{
	apply_filters();
}

bool WelcomeScreen::type_matches_filter(const String &ap_type) const
{
	if (ap_type == "international_airport") return filter_international->is_pressed();
	if (ap_type == "regional_airport") return filter_regional->is_pressed();
	if (ap_type == "grass_strip") return filter_grass->is_pressed();
	if (ap_type == "heliport") return filter_heliport->is_pressed();
	return true;
}

void WelcomeScreen::apply_filters()
{
	const String search_text = search_input != nullptr ? search_input->get_text().strip_edges().to_lower() : String();
	filtered_airports.clear();
	for (int i = 0; i < airports.size(); i++)
	{
		const Dictionary ap = airports[i];
		if (!type_matches_filter(ap.get("type", "")))
		{
			continue;
		}
		if (!search_text.is_empty())
		{
			const String icao = String(ap.get("icao", "")).to_lower();
			const String name = String(ap.get("name", "")).to_lower();
			if (!icao.contains(search_text) && !name.contains(search_text))
			{
				continue;
			}
		}
		filtered_airports.push_back(ap);
	}
	// Sort alphabetically by ICAO
	std::stable_sort(filtered_airports.ptrw(), filtered_airports.ptrw() + filtered_airports.size(), [](const Dictionary &a, const Dictionary &b) {
		return String(a.get("icao", "")) < String(b.get("icao", ""));
	});
	rebuild_airport_list();
}

static const char *type_icon(const String &ap_type)
{
	if (ap_type == "international_airport") return "[INTL]";
	if (ap_type == "regional_airport") return "[REG ]";
	if (ap_type == "grass_strip") return "[GRAS]";
	if (ap_type == "heliport") return "[HELI]";
	return "[    ]";
}

String WelcomeScreen::availability_marker(const Dictionary &airport) const
{
	const String region = airport.get("region", "");
	if (region.is_empty())
	{
		return "  ";
	}
	return installed_regions.has(region) ? String::utf8("\xE2\x9C\x93") : String::utf8("\xE2\x9C\x97");
}

void WelcomeScreen::rebuild_airport_list()
{
	airport_list->clear();
	for (int i = 0; i < filtered_airports.size(); i++)
	{
		const Dictionary &ap = filtered_airports[i];
		String icao = ap.get("icao", "----");
		if (icao.is_empty())
		{
			icao = "----";
		}
		const String marker = availability_marker(ap);
		const String sep = "  ";
		const String text = marker + sep + icao + sep + String(ap.get("name", "")) + sep + String(type_icon(ap.get("type", ""))) + sep +
				String::num_int64((int64_t)(double)ap.get("elevation_m", 0)) + String("m");
		const int idx = airport_list->add_item(text);
		if (marker == String::utf8("\xE2\x9C\x97"))
		{
			airport_list->set_item_custom_fg_color(idx, Color(0.55, 0.55, 0.58));
		}
	}
}

void WelcomeScreen::_on_search_changed(const String &)
{
	apply_filters();
}

// ── Airport selection ───────────────────────────────────────────────────────

void WelcomeScreen::_on_airport_selected(int index)
{
	if (index < 0 || index >= filtered_airports.size())
	{
		return;
	}
	selected_airport = filtered_airports[index];
	update_airport_info();
	populate_runways();
}

void WelcomeScreen::_on_airport_activated(int index)
{
	_on_airport_selected(index);
	_on_fly_pressed();
}

void WelcomeScreen::update_airport_info()
{
	const Dictionary &ap = selected_airport;
	if (ap.is_empty())
	{
		airport_info->set_text("");
		return;
	}
	String icao = ap.get("icao", "----");
	if (icao.is_empty())
	{
		icao = "----";
	}
	const String ap_type = String(ap.get("type", "")).replace("_", " ").capitalize();
	char buf[160];
	String info = "[b]" + icao + "[/b]  " + String(ap.get("name", "")) + "\n";
	info += "Type: " + ap_type + "\n";
	snprintf(buf, sizeof(buf), "Elevation: %dm MSL\n", (int)(double)ap.get("elevation_m", 0));
	info += buf;
	const double area = ap.get("area_ha", 0.0);
	if (area > 0)
	{
		snprintf(buf, sizeof(buf), "Area: %.1f ha\n", area);
		info += buf;
	}
	const Array runways = ap.get("runways", Array());
	if (runways.size() > 0)
	{
		info += "\nRunways:\n";
		for (int i = 0; i < runways.size(); i++)
		{
			const Dictionary rwy = runways[i];
			snprintf(buf, sizeof(buf), "  %s  %dm x %dm  %s\n", String(rwy.get("designator", "?")).utf8().get_data(),
					(int)(double)rwy.get("length_m", 0), (int)(double)rwy.get("width_m", 0), String(rwy.get("surface", "?")).utf8().get_data());
			info += String::utf8(buf);
		}
	}
	else if (ap.has("pad"))
	{
		info += "\nHelipad at center\n";
	}
	airport_info->set_text(info);
}

void WelcomeScreen::populate_runways()
{
	runway_picker->clear();
	const Dictionary &ap = selected_airport;
	if (ap.is_empty())
	{
		return;
	}
	const Array runways = ap.get("runways", Array());
	if (runways.size() == 0)
	{
		runway_picker->add_item("Pad center");
		return;
	}
	for (int i = 0; i < runways.size(); i++)
	{
		const Dictionary rwy = runways[i];
		const PackedStringArray parts = String(rwy.get("designator", "?")).split("/");
		for (int k = 0; k < parts.size(); k++)
		{
			const String p = parts[k];
			if (rwy.has(String("threshold_") + p))
			{
				int hdg = (int)(double)rwy.get("heading_deg", 0);
				if (parts.size() > 1 && k == 1)
				{
					hdg = (hdg + 180) % 360;
				}
				char buf[128];
				snprintf(buf, sizeof(buf), "Rwy %s  (%dm, %s, hdg %d\xC2\xB0)", p.utf8().get_data(), (int)(double)rwy.get("length_m", 0),
						String(rwy.get("surface", "?")).utf8().get_data(), hdg);
				runway_picker->add_item(String::utf8(buf));
			}
		}
	}
}

// {swiss_e, swiss_n, heading_deg} for the selected airport / runway. The
// spawn heading is the runway's TRUE bearing (ECEF world); the thresholds are
// computed in the LV95 grid (AirportsLib, or the pack's pavement end plus the
// displaced threshold via RunwaysLib). The aircraft stands SPAWN_PAST_THRESHOLD_M
// past the threshold, on the threshold stripes (the 30 m "piano keys" start
// 6 m in, the designator 12 m after them), not on the grass before it.
Dictionary WelcomeScreen::get_selected_departure() const
{
	const Dictionary &ap = selected_airport;
	if (ap.is_empty())
	{
		return Dictionary();
	}
	const Array runways = ap.get("runways", Array());
	const int rwy_idx = runway_picker->get_selected() >= 0 ? runway_picker->get_selected() : 0;
	if (runways.size() == 0)
	{
		const Dictionary pad = ap.get("pad", Dictionary());
		Dictionary d;
		d["swiss_e"] = pad.get("e", ap.get("center_e", 0));
		d["swiss_n"] = pad.get("n", ap.get("center_n", 0));
		d["heading_deg"] = 0.0;
		return d;
	}
	Vector<Dictionary> entries;
	for (int i = 0; i < runways.size(); i++)
	{
		const Dictionary rwy = runways[i];
		const PackedStringArray parts = String(rwy.get("designator", "?")).split("/");
		for (int k = 0; k < parts.size(); k++)
		{
			const String key = String("threshold_") + parts[k];
			if (!rwy.has(key))
			{
				continue;
			}
			const Dictionary thresh = rwy[key];
			double hdg = rwy.get("heading_deg", 0.0);
			double grid_hdg = rwy.get("grid_heading_deg", hdg);
			if (k == 1)
			{
				hdg = std::fmod(hdg + 180.0, 360.0);
				grid_hdg = std::fmod(grid_hdg + 180.0, 360.0);
			}
			// along the runway, in the grid the thresholds live in
			const double along = Math::deg_to_rad(grid_hdg);
			Dictionary d;
			d["swiss_e"] = (double)thresh.get("e", ap.get("center_e", 0)) + std::sin(along) * SPAWN_PAST_THRESHOLD_M;
			d["swiss_n"] = (double)thresh.get("n", ap.get("center_n", 0)) + std::cos(along) * SPAWN_PAST_THRESHOLD_M;
			d["heading_deg"] = hdg;
			entries.push_back(d);
		}
	}
	if (rwy_idx >= 0 && rwy_idx < entries.size())
	{
		return entries[rwy_idx];
	}
	if (!entries.is_empty())
	{
		return entries[0];
	}
	return Dictionary();
}

// ── Actions ─────────────────────────────────────────────────────────────────

void WelcomeScreen::_on_fly_pressed()
{
	const Dictionary dep = get_selected_departure();
	if (dep.is_empty())
	{
		status_label->set_text("Please select an airport.");
		return;
	}
	emit_signal("flight_requested", get_selected_aircraft_id(), (double)dep["swiss_e"], (double)dep["swiss_n"], (double)dep["heading_deg"]);
	hide_welcome();
}

void WelcomeScreen::_on_edit_pressed()
{
	const Dictionary dep = get_selected_departure();
	if (dep.is_empty())
	{
		status_label->set_text("Please select an airport.");
		return;
	}
	emit_signal("edit_requested", (double)dep["swiss_e"], (double)dep["swiss_n"], (double)dep["heading_deg"]);
	hide_welcome();
}

void WelcomeScreen::_on_settings_pressed()
{
	emit_signal("settings_requested");
}

void WelcomeScreen::_on_about_pressed()
{
	show_about_dialog();
}

void WelcomeScreen::_on_about_meta_clicked(const Variant &meta)
{
	OS::get_singleton()->shell_open(meta);
}

// The About dialog: third-party components, credits, license, disclaimer.
void WelcomeScreen::show_about_dialog()
{
	AcceptDialog *dlg = memnew(AcceptDialog);
	dlg->set_title("About ProVPilot");
	dlg->set_min_size(Vector2i(760, 640));
	dlg->set_hide_on_ok(true);
	dlg->connect("confirmed", Callable(dlg, "queue_free"));
	dlg->connect("close_requested", Callable(dlg, "queue_free"));
	RichTextLabel *rtl = memnew(RichTextLabel);
	rtl->set_use_bbcode(true);
	rtl->set_fit_content(false);
	rtl->set_scroll_active(true);
	rtl->set_custom_minimum_size(Vector2(720, 560));
	rtl->add_theme_font_size_override("normal_font_size", FONT_SM);
	rtl->add_theme_font_size_override("bold_font_size", FONT_SM);
	rtl->connect("meta_clicked", Callable(this, "_on_about_meta_clicked"));
	const String ver = ProjectSettings::get_singleton()->get_setting("application/config/version", "0.0.0");
	rtl->set_text(String::utf8(
			"[center][b]ProVPilot v") + ver + String::utf8("[/b]\n"
			"Swiss Flight Simulator \xE2\x80\x94 \xC2\xA9 2025 Gerhard Gubler[/center]\n\n"
			"[b]Built with[/b]\n"
			"\xE2\x80\xA2 [url=https://godotengine.org]Godot Engine[/url] 4.6 (custom [i]4.6-macdev[/i] fork, double precision) \xE2\x80\x94 MIT\n"
			"\xE2\x80\xA2 [url=https://jsbsim.sourceforge.net]JSBSim[/url] flight dynamics model \xE2\x80\x94 LGPL 2.1\n"
			"\xE2\x80\xA2 [url=https://gitlab.md80.ch/gery/scenery3d]Scenery3D[/url] GDExtension \xE2\x80\x94 MIT\n\n"
			"[b]Data sources[/b]\n"
			"\xE2\x80\xA2 Switzerland terrain, orthophotos, buildings, roads, water: [url=https://www.swisstopo.admin.ch]swisstopo[/url] (swissALTI3D, SWISSIMAGE, swissTLM3D) \xE2\x80\x94 [url=https://www.swisstopo.admin.ch/en/terms-of-use-free-geodata-and-geoservices]free geodata licence[/url]\n"
			"\xE2\x80\xA2 Baden-W\xC3\xBCrttemberg terrain & orthophotos: [url=https://www.lgl-bw.de]LGL Baden-W\xC3\xBCrttemberg[/url] \xE2\x80\x94 [url=https://www.govdata.de/dl-de/by-2-0]dl-de/by-2-0[/url] (Datenquelle: LGL, www.lgl-bw.de)\n"
			"\xE2\x80\xA2 Coastline, roads, forests, airports, runways, taxiways: \xC2\xA9 [url=https://www.openstreetmap.org/copyright]OpenStreetMap[/url] contributors \xE2\x80\x94 [url=https://opendatacommons.org/licenses/odbl/]ODbL[/url]\n"
			"\xE2\x80\xA2 World terrain: NASA SRTM; globe: NOAA ETOPO; country outlines: Natural Earth \xE2\x80\x94 public domain\n"
			"\xE2\x80\xA2 Airports and navaids: [url=https://ourairports.com]OurAirports[/url] \xE2\x80\x94 public domain; ILS data: X-Plane nav.dat by Robin Peel \xE2\x80\x94 GPL\n"
			"\xE2\x80\xA2 Magnetic variation: NOAA / NCEI World Magnetic Model 2025; weather: [url=https://aviationweather.gov]aviationweather.gov[/url] METAR \xE2\x80\x94 public domain\n\n"
			"[b]Aircraft and models[/b]\n"
			"\xE2\x80\xA2 MD-80 family: FlightGear MD-80 by Octal450 (Joshua Davidson), with its IT-AUTOFLIGHT system \xE2\x80\x94 GPL-2.0\n"
			"\xE2\x80\xA2 A-10: FlightGear A-10 by Lee Elliott, Alexis Bory, David Bastien and contributors \xE2\x80\x94 GPL-2.0\n"
			"\xE2\x80\xA2 ASK 21: FlightGear ASK21 by Heiko Schulz, Thomas Polzer, Viktor Radnai, Benedikt Wolf, Gilberto Agostinho and contributors \xE2\x80\x94 GPL-2.0 or later\n"
			"\xE2\x80\xA2 Cockpit instruments of the Cessna 172 and PC-7: [url=https://www.flightgear.org]FlightGear[/url] base package \xE2\x80\x94 GPL-2.0\n"
			"\xE2\x80\xA2 Cessna 172: [url=https://sketchfab.com/3d-models/cessna172-d1b15841c29c43d0862667300bad55a4]\"Cessna172\"[/url] by [url=https://sketchfab.com/ioai25312]KOG_THORNS[/url] on Sketchfab \xE2\x80\x94 [url=http://creativecommons.org/licenses/by/4.0/]CC-BY-4.0[/url]\n"
			"\xE2\x80\xA2 Locomotive: [url=https://sketchfab.com/3d-models/swiss-lok-re-44-ed93e661475549d3b53270f5b9246ab4]\"Swiss Lok Re 4/4\"[/url] by [url=https://sketchfab.com/hotdoghans]hotdoghans[/url] \xE2\x80\x94 CC-BY-4.0; car: [url=https://sketchfab.com/3d-models/low-poly-car-6-64ce11dd61af43c285d82cf0091e53be]\"Low poly car 6\"[/url] by assetfactory \xE2\x80\x94 Sketchfab Standard\n"
			"The licence texts and credits are in each aircraft's folder (LICENSE, CREDITS.txt). All product and company names are trademarks of their owners and are used to identify the aircraft only; ProVPilot is not affiliated with or endorsed by them.\n\n"
			"[b]ProVPilot license[/b]\n"
			"ProVPilot's own source code is released under the [url=https://opensource.org/license/mit]MIT License[/url]. The program as distributed contains the MD-80 autoflight logic, a port of GPL-2.0 code, and is therefore distributed as a whole under the GNU General Public License, version 2; the complete source is at [url=https://gitlab.md80.ch/gery/provpilot]gitlab.md80.ch/gery/provpilot[/url]. See [code]LICENSE[/code] and [code]NOTICE.md[/code] there.\n\n"
			"[b]Disclaimer[/b]\n"
			"ProVPilot is provided \"AS IS\", without warranty of any kind. It is intended for entertainment and educational purposes only. It is [b]not[/b] certified for flight training and must [b]not[/b] be used for real-world navigation, flight planning, or any safety-critical purpose. Use at your own risk."));
	dlg->add_child(rtl);
	add_child(dlg);
	dlg->popup_centered();
}

void WelcomeScreen::_on_continue_pressed()
{
	emit_signal("continue_requested");
	hide_welcome();
}

void WelcomeScreen::_on_quit_pressed()
{
	get_tree()->quit();
}

// Enter starts the flight (or resumes it when Continue is available); Escape
// resumes the flight when possible.
void WelcomeScreen::_input(const Ref<InputEvent> &event)
{
	if (!is_visible())
	{
		return;
	}
	Ref<InputEventKey> key = event;
	if (key.is_null() || !key->is_pressed())
	{
		return;
	}
	const bool can_continue = continue_button != nullptr && continue_button->is_visible() && !continue_button->is_disabled();
	if (key->get_keycode() == KEY_ENTER || key->get_keycode() == KEY_KP_ENTER)
	{
		if (can_continue)
		{
			_on_continue_pressed();
		}
		else
		{
			_on_fly_pressed();
		}
		get_viewport()->set_input_as_handled();
	}
	else if (key->get_keycode() == KEY_ESCAPE && can_continue)
	{
		_on_continue_pressed();
		get_viewport()->set_input_as_handled();
	}
}

// Block all unhandled input from reaching the sim behind the overlay.
void WelcomeScreen::_unhandled_input(const Ref<InputEvent> &)
{
	if (is_visible() && is_inside_tree())
	{
		get_viewport()->set_input_as_handled();
	}
}

// ── Update check ────────────────────────────────────────────────────────────
// GET the release feed; when its `version` is newer than the app's, show a
// clickable banner above the title. Failures are silent.

void WelcomeScreen::check_for_updates()
{
	if (update_request != nullptr)
	{
		return;
	}
	update_request = memnew(HTTPRequest);
	add_child(update_request);
	update_request->set_timeout(5.0);
	update_request->connect("request_completed", Callable(this, "_on_update_request_completed"));
	if (update_request->request(UPDATE_LATEST_URL) != OK)
	{
		update_request->queue_free();
		update_request = nullptr;
	}
}

void WelcomeScreen::_on_update_request_completed(int result, int code, const PackedStringArray &, const PackedByteArray &body)
{
	if (update_request != nullptr)
	{
		update_request->queue_free();
		update_request = nullptr;
	}
	if (result != HTTPRequest::RESULT_SUCCESS || code != 200)
	{
		return;
	}
	const Variant parsed = JSON::parse_string(body.get_string_from_utf8());
	if (parsed.get_type() != Variant::DICTIONARY)
	{
		return;
	}
	const Dictionary data = parsed;
	const String remote_version = data.get("version", "");
	if (remote_version.is_empty())
	{
		return;
	}
	const String current_version = ProjectSettings::get_singleton()->get_setting("application/config/version", "0.0.0");
	if (!is_remote_newer(remote_version, current_version))
	{
		return;
	}
	const Dictionary macos_arm64 = data.get("macos_arm64", Dictionary());
	const String rel_url = macos_arm64.get("url", "");
	if (rel_url.is_empty())
	{
		return;
	}
	latest_download_url = rel_url.begins_with("http") ? rel_url : String(UPDATE_BASE_URL) + rel_url;
	if (update_label != nullptr)
	{
		update_label->set_text(String::utf8("[center]Update available: ProVPilot ") + remote_version + String::utf8(" \xE2\x80\x94 [url=download]Download[/url][/center]"));
		update_label->set_visible(true);
	}
}

bool WelcomeScreen::is_remote_newer(const String &remote, const String &current)
{
	const PackedStringArray r = remote.split(".");
	const PackedStringArray c = current.split(".");
	const int n = std::max(r.size(), c.size());
	for (int i = 0; i < n; i++)
	{
		const int rv = i < r.size() ? (int)r[i].to_int() : 0;
		const int cv = i < c.size() ? (int)c[i].to_int() : 0;
		if (rv > cv) return true;
		if (rv < cv) return false;
	}
	return false;
}

void WelcomeScreen::_on_update_meta_clicked(const Variant &)
{
	if (!latest_download_url.is_empty())
	{
		OS::get_singleton()->shell_open(latest_download_url);
	}
}

// ── First-run wizard ────────────────────────────────────────────────────────
// Shown once, when InstallRoot reports the user has never configured a
// scenery location. Emits scenery_setup_requested with the chosen folder.

static InstallConfig *install_root(const Node *n)
{
	return Object::cast_to<InstallConfig>(n->get_node_or_null(NodePath("/root/InstallRoot")));
}

void WelcomeScreen::_maybe_show_first_run_wizard()
{
	InstallConfig *ir = install_root(this);
	if (ir == nullptr || !ir->is_first_run())
	{
		return;
	}
	if (has_scenery_areas_configured())
	{
		return;
	}
	show_first_run_dialog();
}

// The settings library takes precedence: best-effort via the parent's
// _settings (SettingsStore).
bool WelcomeScreen::has_scenery_areas_configured() const
{
	Node *parent = get_parent();
	if (parent == nullptr)
	{
		return false;
	}
	SettingsStore *s = Object::cast_to<SettingsStore>(parent->get("_settings"));
	return s != nullptr && s->get_scenery_areas().size() > 0;
}

void WelcomeScreen::show_first_run_dialog()
{
	if (first_run_dialog != nullptr)
	{
		first_run_dialog->popup_centered();
		return;
	}
	first_run_dialog = memnew(AcceptDialog);
	first_run_dialog->set_title("Welcome to ProVPilot");
	first_run_dialog->set_ok_button_text("Use This Folder");
	first_run_dialog->add_cancel_button("Skip for Now");
	first_run_dialog->set_flag(Window::FLAG_RESIZE_DISABLED, false);
	first_run_dialog->set_min_size(Vector2i(700, 280));
	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->add_theme_constant_override("separation", 12);
	first_run_dialog->add_child(vbox);
	Label *intro = memnew(Label);
	intro->set_text("ProVPilot needs to know where your scenery library is located.\n\nPick a folder that contains region subfolders such as 'Switzerland/' or 'Baden-Wuertemberg/'. You can change or add more folders later in Settings.");
	intro->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	intro->add_theme_font_size_override("font_size", FONT_MD);
	intro->set_custom_minimum_size(Vector2(660, 0));
	vbox->add_child(intro);
	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override("separation", 8);
	vbox->add_child(row);
	first_run_path_edit = memnew(LineEdit);
	first_run_path_edit->set_placeholder("/path/to/scenery");
	first_run_path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	first_run_path_edit->add_theme_font_size_override("font_size", FONT_MD);
	first_run_path_edit->set_custom_minimum_size(Vector2(0, 40));
	first_run_path_edit->set_text(first_run_default_path());
	row->add_child(first_run_path_edit);
	Button *browse_btn = memnew(Button);
	browse_btn->set_text(String::utf8("Browse\xE2\x80\xA6"));
	browse_btn->add_theme_font_size_override("font_size", FONT_MD);
	browse_btn->connect("pressed", Callable(this, "_on_first_run_browse_pressed"));
	row->add_child(browse_btn);
	Label *hint = memnew(Label);
	hint->set_text("Tip: Place the 'scenery/' folder next to ProVPilot.app for a portable install - the choice is saved to provpilot.cfg in the same folder.");
	hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint->add_theme_font_size_override("font_size", FONT_SM);
	hint->add_theme_color_override("font_color", Color(0.6, 0.65, 0.7));
	hint->set_custom_minimum_size(Vector2(660, 0));
	vbox->add_child(hint);
	first_run_dialog->connect("confirmed", Callable(this, "_on_first_run_confirmed"));
	first_run_dialog->connect("canceled", Callable(this, "_on_first_run_canceled"));
	add_child(first_run_dialog);
	first_run_dialog->popup_centered();
}

String WelcomeScreen::first_run_default_path() const
{
	InstallConfig *ir = install_root(this);
	if (ir == nullptr)
	{
		return String();
	}
	const PackedStringArray candidates = ir->get_default_scan_roots();
	for (int i = 0; i < candidates.size(); i++)
	{
		if (DirAccess::dir_exists_absolute(candidates[i]))
		{
			return candidates[i];
		}
	}
	// install_root/scenery even if it doesn't exist yet: the user can edit it.
	return ir->get_install_root().path_join("scenery");
}

void WelcomeScreen::_on_first_run_browse_pressed()
{
	FileDialog *dlg = memnew(FileDialog);
	dlg->set_file_mode(FileDialog::FILE_MODE_OPEN_DIR);
	dlg->set_access(FileDialog::ACCESS_FILESYSTEM);
	dlg->set_use_native_dialog(true);
	dlg->set_title("Select Scenery Folder");
	add_child(dlg);
	dlg->connect("dir_selected", Callable(this, "_on_first_run_dir_selected"));
	dlg->connect("dir_selected", Callable(dlg, "queue_free").unbind(1));
	dlg->connect("canceled", Callable(dlg, "queue_free"));
	dlg->popup_centered_ratio(0.7);
}

void WelcomeScreen::_on_first_run_dir_selected(const String &path)
{
	if (first_run_path_edit != nullptr)
	{
		first_run_path_edit->set_text(path);
	}
}

void WelcomeScreen::_on_first_run_confirmed()
{
	const String path = (first_run_path_edit != nullptr ? first_run_path_edit->get_text() : String()).strip_edges();
	if (path.is_empty())
	{
		return;
	}
	if (!DirAccess::dir_exists_absolute(path))
	{
		AcceptDialog *err = memnew(AcceptDialog);
		err->set_title("Folder Not Found");
		err->set_text("The folder does not exist:\n" + path);
		add_child(err);
		err->connect("confirmed", Callable(err, "queue_free"));
		err->connect("confirmed", Callable(this, "_maybe_show_first_run_wizard"));
		err->popup_centered();
		return;
	}
	InstallConfig *ir = install_root(this);
	if (ir != nullptr)
	{
		ir->save_scenery_choice(path, PackedStringArray()); // persisted next to the app
	}
	emit_signal("scenery_setup_requested", path); // the host fills the library + applies sources
}

void WelcomeScreen::_on_first_run_canceled()
{
	// Skipped: the user can still configure the scenery in Settings.
	UtilityFunctions::print("WelcomeScreen: first-run wizard skipped");
}

} // namespace jsbgodot
