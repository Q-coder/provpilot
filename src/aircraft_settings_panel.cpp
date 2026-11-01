#include "aircraft_settings_panel.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cmath>
#include <cstdio>

#include "aircraft_config.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include "jsbgodot.h"
#include "settings_store.h"

using namespace godot;

namespace jsbgodot
{

static const struct { const char *key; const char *label; const char *unit; } INFO_ROWS[] = {
	{"vne_kts", "Never exceed (Vne)", "kt"},
	{"vno_kts", "Max structural cruise (Vno)", "kt"},
	{"vs_kts", "Stall, clean (Vs)", "kt"},
	{"vs0_kts", "Stall, landing config (Vs0)", "kt"},
	{"vr_kts", "Rotation (Vr)", "kt"},
	{"vref_kts", "Approach (Vref)", "kt"},
	{"ceiling_ft", "Service ceiling", "ft"},
	{"max_takeoff_lbs", "Max take-off weight", "lb"},
	{"empty_lbs", "Empty weight", "lb"},
};

void AircraftSettingsPanel::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("refresh"), &AircraftSettingsPanel::refresh);
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("apply_fuel", "jsb", "pct"), &AircraftSettingsPanel::apply_fuel);
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("start_agl_ft", "store", "cfg", "id"), &AircraftSettingsPanel::start_agl_ft);
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("start_speed_kt", "store", "cfg", "id"), &AircraftSettingsPanel::start_speed_kt);
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("start_heading_deg", "store", "id"), &AircraftSettingsPanel::start_heading_deg);
	ClassDB::bind_method(D_METHOD("fuel_key"), &AircraftSettingsPanel::fuel_key);
	ClassDB::bind_method(D_METHOD("get_aircraft_id"), &AircraftSettingsPanel::get_aircraft_id);
	ClassDB::bind_method(D_METHOD("_on_start_changed", "value"), &AircraftSettingsPanel::_on_start_changed);
	ClassDB::bind_method(D_METHOD("_on_runway_heading_toggled", "on"), &AircraftSettingsPanel::_on_runway_heading_toggled);
	ClassDB::bind_method(D_METHOD("_on_throttle_drives_selected", "idx"), &AircraftSettingsPanel::_on_throttle_drives_selected);
	ClassDB::bind_method(D_METHOD("_on_livery_selected", "idx"), &AircraftSettingsPanel::_on_livery_selected);
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("livery_catalog", "cfg", "id"), &AircraftSettingsPanel::livery_catalog);
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("livery_key", "id"), &AircraftSettingsPanel::livery_key);
	ClassDB::bind_method(D_METHOD("get_livery_button"), &AircraftSettingsPanel::get_livery_button);
	ADD_SIGNAL(MethodInfo("livery_changed", PropertyInfo(Variant::STRING, "aircraft_id"), PropertyInfo(Variant::STRING, "livery_id")));
	ClassDB::bind_static_method("AircraftSettingsPanel", D_METHOD("throttle_drives_airbrakes", "store", "cfg", "id"), &AircraftSettingsPanel::throttle_drives_airbrakes);
	ClassDB::bind_method(D_METHOD("get_title_label"), &AircraftSettingsPanel::get_title_label);
	ClassDB::bind_method(D_METHOD("get_info_grid"), &AircraftSettingsPanel::get_info_grid);
	ClassDB::bind_method(D_METHOD("get_cg_label"), &AircraftSettingsPanel::get_cg_label);
	ClassDB::bind_method(D_METHOD("get_fuel_slider"), &AircraftSettingsPanel::get_fuel_slider);
	ClassDB::bind_method(D_METHOD("_on_fuel_changed", "value"), &AircraftSettingsPanel::_on_fuel_changed);
	ClassDB::bind_method(D_METHOD("_update_live", "force"), &AircraftSettingsPanel::_update_live, DEFVAL(false));
#define ASP_PROP(name, getter, setter)                                                          \
	ClassDB::bind_method(D_METHOD(#getter), &AircraftSettingsPanel::getter);                 \
	ClassDB::bind_method(D_METHOD(#setter, "callable"), &AircraftSettingsPanel::setter);     \
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, #name), #setter, #getter);
	ASP_PROP(aircraft_provider, get_aircraft_provider, set_aircraft_provider)
	ASP_PROP(fdm_provider, get_fdm_provider, set_fdm_provider)
	ASP_PROP(flown_provider, get_flown_provider, set_flown_provider)
	ASP_PROP(config_provider, get_config_provider, set_config_provider)
	ASP_PROP(name_provider, get_name_provider, set_name_provider)
#undef ASP_PROP
}

AircraftSettingsPanel::AircraftSettingsPanel()
{
}

Callable AircraftSettingsPanel::get_aircraft_provider() const { return aircraft_provider; }
void AircraftSettingsPanel::set_aircraft_provider(const Callable &c) { aircraft_provider = c; }
Callable AircraftSettingsPanel::get_fdm_provider() const { return fdm_provider; }
void AircraftSettingsPanel::set_fdm_provider(const Callable &c) { fdm_provider = c; }
Callable AircraftSettingsPanel::get_flown_provider() const { return flown_provider; }
void AircraftSettingsPanel::set_flown_provider(const Callable &c) { flown_provider = c; }
Callable AircraftSettingsPanel::get_config_provider() const { return config_provider; }
void AircraftSettingsPanel::set_config_provider(const Callable &c) { config_provider = c; }
Callable AircraftSettingsPanel::get_name_provider() const { return name_provider; }
void AircraftSettingsPanel::set_name_provider(const Callable &c) { name_provider = c; }

static Label *make_label(const String &text, int font_size = 0)
{
	Label *l = memnew(Label);
	l->set_text(text);
	if (font_size > 0)
	{
		l->add_theme_font_size_override("font_size", font_size);
	}
	return l;
}

void AircraftSettingsPanel::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	add_theme_constant_override("separation", 10);
	title = make_label("", 20);
	add_child(title);
	// fuel
	add_child(make_label("Fuel load", 16));
	HBoxContainer *fuel_row = memnew(HBoxContainer);
	fuel_row->add_theme_constant_override("separation", 12);
	add_child(fuel_row);
	fuel_slider = memnew(HSlider);
	fuel_slider->set_min(0.0);
	fuel_slider->set_max(100.0);
	fuel_slider->set_step(1.0);
	fuel_slider->set_custom_minimum_size(Vector2(320, 0));
	fuel_slider->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	fuel_slider->connect("value_changed", Callable(this, "_on_fuel_changed"));
	fuel_row->add_child(fuel_slider);
	fuel_label = make_label("");
	fuel_label->set_custom_minimum_size(Vector2(220, 0));
	fuel_row->add_child(fuel_label);
	fuel_note = make_label("", 12);
	fuel_note->set_modulate(Color(0.8, 0.85, 0.9));
	add_child(fuel_note);
	add_child(memnew(HSeparator));
	// start conditions
	add_child(make_label("Start", 16));
	GridContainer *start_grid = memnew(GridContainer);
	start_grid->set_columns(2);
	start_grid->add_theme_constant_override("h_separation", 24);
	start_grid->add_theme_constant_override("v_separation", 6);
	add_child(start_grid);
	auto spin = [&](double min, double max, double step, const String &suffix) {
		SpinBox *sb = memnew(SpinBox);
		sb->set_min(min);
		sb->set_max(max);
		sb->set_step(step);
		sb->set_suffix(suffix);
		sb->set_custom_minimum_size(Vector2(160, 0));
		sb->connect("value_changed", Callable(this, "_on_start_changed"));
		return sb;
	};
	start_grid->add_child(make_label("Height above the ground"));
	start_agl = spin(0.0, 30000.0, 100.0, "ft");
	start_grid->add_child(start_agl);
	start_grid->add_child(make_label("Airspeed"));
	start_speed = spin(0.0, 400.0, 5.0, "kt");
	start_grid->add_child(start_speed);
	start_grid->add_child(make_label("Heading"));
	HBoxContainer *hdg_row = memnew(HBoxContainer);
	hdg_row->add_theme_constant_override("separation", 12);
	start_grid->add_child(hdg_row);
	runway_heading = memnew(CheckBox);
	runway_heading->set_text("Runway heading");
	runway_heading->connect("toggled", Callable(this, "_on_runway_heading_toggled"));
	hdg_row->add_child(runway_heading);
	start_heading = spin(0.0, 359.0, 1.0, String::utf8("\xC2\xB0"));
	hdg_row->add_child(start_heading);
	start_note = make_label("", 12);
	start_note->set_modulate(Color(0.8, 0.85, 0.9));
	start_note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	start_note->set_custom_minimum_size(Vector2(520, 0));
	add_child(start_note);
	add_child(memnew(HSeparator));
	// controls
	add_child(make_label("Controls", 16));
	HBoxContainer *ctl_row = memnew(HBoxContainer);
	ctl_row->add_theme_constant_override("separation", 24);
	add_child(ctl_row);
	ctl_row->add_child(make_label("Throttle keys, triggers and axis work the"));
	throttle_drives = memnew(OptionButton);
	throttle_drives->add_item("Throttle (engines)", 0);
	throttle_drives->add_item("Airbrakes (glider lever)", 1);
	throttle_drives->connect("item_selected", Callable(this, "_on_throttle_drives_selected"));
	ctl_row->add_child(throttle_drives);
	controls_note = make_label("", 12);
	controls_note->set_modulate(Color(0.8, 0.85, 0.9));
	controls_note->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	controls_note->set_custom_minimum_size(Vector2(520, 0));
	add_child(controls_note);
	add_child(memnew(HSeparator));
	// livery (only aircraft with a [liveries] catalog show the section)
	livery_title = make_label("Livery", 16);
	add_child(livery_title);
	livery_row = memnew(HBoxContainer);
	livery_row->add_theme_constant_override("separation", 24);
	add_child(livery_row);
	livery_row->add_child(make_label("Paint scheme"));
	livery = memnew(OptionButton);
	livery->set_custom_minimum_size(Vector2(320, 0));
	livery->connect("item_selected", Callable(this, "_on_livery_selected"));
	livery_row->add_child(livery);
	livery_sep = memnew(HSeparator);
	add_child(livery_sep);
	// reference data
	add_child(make_label("Reference data", 16));
	info_grid = memnew(GridContainer);
	info_grid->set_columns(2);
	info_grid->add_theme_constant_override("h_separation", 24);
	add_child(info_grid);
	add_child(memnew(HSeparator));
	// centre of gravity
	add_child(make_label("Centre of gravity (live while flying)", 16));
	cg_label = make_label("");
	add_child(cg_label);
	cg_bar = memnew(HSlider);
	cg_bar->set_min(0.0);
	cg_bar->set_max(60.0);
	cg_bar->set_step(0.1);
	cg_bar->set_editable(false);
	cg_bar->set_focus_mode(Control::FOCUS_NONE);
	cg_bar->set_custom_minimum_size(Vector2(320, 0));
	add_child(cg_bar);
	cg_limits = make_label("", 12);
	cg_limits->set_modulate(Color(0.8, 0.85, 0.9));
	add_child(cg_limits);
	timer = memnew(Timer);
	timer->set_wait_time(0.5);
	timer->connect("timeout", Callable(this, "_update_live").bind(false));
	add_child(timer);
	timer->start();
	call_deferred("refresh");
}

SettingsStore *AircraftSettingsPanel::find_store() const
{
	Node *n = get_parent();
	while (n != nullptr)
	{
		if (n->has_method("get_store"))
		{
			return Object::cast_to<SettingsStore>(n->call("get_store"));
		}
		n = n->get_parent();
	}
	return nullptr;
}

String AircraftSettingsPanel::get_aircraft_id() const { return aircraft_id; }
Label *AircraftSettingsPanel::get_title_label() const { return title; }
GridContainer *AircraftSettingsPanel::get_info_grid() const { return info_grid; }
Label *AircraftSettingsPanel::get_cg_label() const { return cg_label; }
HSlider *AircraftSettingsPanel::get_fuel_slider() const { return fuel_slider; }

String AircraftSettingsPanel::fuel_key() const
{
	return String("aircraft_") + aircraft_id + "_fuel_pct";
}

Object *AircraftSettingsPanel::fdm_if_flown() const
{
	if (!fdm_provider.is_valid() || !flown_provider.is_valid())
	{
		return nullptr;
	}
	if (String(flown_provider.call()) != aircraft_id)
	{
		return nullptr;
	}
	return fdm_provider.call();
}

String AircraftSettingsPanel::fmt(double v)
{
	char buf[32];
	if (std::abs(v - std::round(v)) < 0.05)
	{
		snprintf(buf, sizeof(buf), "%d", (int)std::lround(v));
	}
	else
	{
		snprintf(buf, sizeof(buf), "%.1f", v);
	}
	return String(buf);
}

void AircraftSettingsPanel::refresh()
{
	if (store == nullptr)
	{
		store = find_store();
	}
	aircraft_id = aircraft_provider.is_valid() ? String(aircraft_provider.call()) : String();
	const String nm = name_provider.is_valid() ? String(name_provider.call(aircraft_id)) : aircraft_id;
	title->set_text(String("Aircraft: ") + (nm.is_empty() ? String("none selected") : nm));
	// fuel: the stored value, else the FDM's current load when flown, else 50 %
	double pct = 50.0;
	if (store != nullptr && store->has_setting(fuel_key()))
	{
		pct = store->get_setting(fuel_key());
	}
	else
	{
		JSBGodot *jsb = Object::cast_to<JSBGodot>(fdm_if_flown());
		if (jsb != nullptr && jsb->get_tank_count() > 0)
		{
			double cap = 0.0, cont = 0.0;
			for (int i = 0; i < jsb->get_tank_count(); i++)
			{
				cap += jsb->get_tank_capacity_lbs(i);
				cont += jsb->get_tank_contents_lbs(i);
			}
			if (cap > 0.0)
			{
				pct = cont / cap * 100.0;
			}
		}
	}
	Ref<AircraftConfig> cfg;
	if (config_provider.is_valid() && !aircraft_id.is_empty())
	{
		cfg = config_provider.call(aircraft_id);
	}
	updating = true;
	fuel_slider->set_value(pct);
	// start conditions: the stored values, else the cfg defaults, else the runway
	start_agl->set_value(start_agl_ft(store, cfg, aircraft_id));
	start_speed->set_value(start_speed_kt(store, cfg, aircraft_id));
	const double hdg = start_heading_deg(store, aircraft_id);
	runway_heading->set_pressed(std::isnan(hdg));
	start_heading->set_editable(!std::isnan(hdg));
	start_heading->set_value(std::isnan(hdg) ? 0.0 : hdg);
	const bool brakes = throttle_drives_airbrakes(store, cfg, aircraft_id);
	throttle_drives->select(brakes ? 1 : 0);
	controls_note->set_text(brakes ? "Airbrakes: throttle full = brakes in, throttle idle = brakes fully out; B toggles between the two. Applied now when flying this aircraft."
								   : "Throttle: the usual engine control. Gliders default to the airbrakes (aircraft.cfg [controls] throttle_drives).");
	// livery: the catalog's names, the stored choice selected
	livery->clear();
	livery_ids = PackedStringArray();
	const Array liveries = livery_catalog(cfg, aircraft_id).get("liveries", Array());
	const bool has_liveries = !liveries.is_empty();
	livery_title->set_visible(has_liveries);
	livery_row->set_visible(has_liveries);
	livery_sep->set_visible(has_liveries);
	if (has_liveries)
	{
		const String lkey = livery_key(aircraft_id);
		const String cur = store != nullptr && store->has_setting(lkey) ? String(store->get_setting(lkey)) : String();
		livery->add_item("As built (the model's own paint)");
		livery_ids.push_back("");
		int sel = 0;
		for (int i = 0; i < liveries.size(); i++)
		{
			const Dictionary l = liveries[i];
			const String lid = l.get("id", "");
			livery->add_item(l.get("name", lid));
			livery_ids.push_back(lid);
			if (!lid.is_empty() && lid == cur)
			{
				sel = i + 1;
			}
		}
		livery->select(sel);
	}
	updating = false;
	update_fuel_label();
	update_start_note();
	// reference data
	const TypedArray<Node> kids = info_grid->get_children();
	for (int i = 0; i < kids.size(); i++)
	{
		Object::cast_to<Node>(kids[i])->queue_free();
	}
	for (const auto &row : INFO_ROWS)
	{
		info_grid->add_child(make_label(row.label));
		const double val = cfg.is_valid() ? cfg->get_float("performance", row.key, -1.0) : -1.0;
		info_grid->add_child(make_label(val >= 0.0 ? fmt(val) + " " + row.unit : String::utf8("\xE2\x80\x94")));
	}
	const String notes = cfg.is_valid() ? cfg->get_string("performance", "notes", "") : String();
	if (!notes.is_empty())
	{
		info_grid->add_child(make_label("Notes"));
		Label *nv = make_label(notes);
		nv->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		nv->set_custom_minimum_size(Vector2(360, 0));
		info_grid->add_child(nv);
	}
	if (cfg.is_valid() && cfg->has_value("performance", "cg_fwd_pct_mac"))
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "Limits: %.0f \xE2\x80\x93 %.0f %% MAC", cfg->get_float("performance", "cg_fwd_pct_mac", 0.0), cfg->get_float("performance", "cg_aft_pct_mac", 0.0));
		cg_limits->set_text(String::utf8(buf));
	}
	else
	{
		cg_limits->set_text("% MAC estimates the aerodynamic centre at 25 % of the mean chord; no limits given for this aircraft.");
	}
	_update_live(true);
}

void AircraftSettingsPanel::update_fuel_label()
{
	const double pct = fuel_slider->get_value();
	JSBGodot *jsb = Object::cast_to<JSBGodot>(fdm_if_flown());
	double cap = 0.0;
	if (jsb != nullptr)
	{
		for (int i = 0; i < jsb->get_tank_count(); i++)
		{
			cap += jsb->get_tank_capacity_lbs(i);
		}
	}
	char buf[96];
	if (cap > 0.0)
	{
		snprintf(buf, sizeof(buf), "%.0f %%  (%.0f of %.0f lb, %d tanks)", pct, cap * pct / 100.0, cap, jsb->get_tank_count());
		fuel_label->set_text(buf);
		fuel_note->set_text("Applied now to every tank of the aircraft you are flying, and again at each flight start.");
	}
	else
	{
		snprintf(buf, sizeof(buf), "%.0f %% of every tank", pct);
		fuel_label->set_text(buf);
		fuel_note->set_text("Applied when the flight starts. The FlightGear A-10's external tanks are not in the flight model; its three internal tanks share this setting.");
	}
}

String AircraftSettingsPanel::start_key(const char *name) const
{
	return String("aircraft_") + aircraft_id + "_start_" + name;
}

double AircraftSettingsPanel::start_agl_ft(SettingsStore *store, const Ref<AircraftConfig> &cfg, const String &id)
{
	const String key = String("aircraft_") + id + "_start_agl_ft";
	if (store != nullptr && store->has_setting(key))
	{
		return store->get_setting(key);
	}
	return cfg.is_valid() ? cfg->get_float("start", "agl_ft", 0.0) : 0.0;
}

double AircraftSettingsPanel::start_speed_kt(SettingsStore *store, const Ref<AircraftConfig> &cfg, const String &id)
{
	const String key = String("aircraft_") + id + "_start_speed_kt";
	if (store != nullptr && store->has_setting(key))
	{
		return store->get_setting(key);
	}
	return cfg.is_valid() ? cfg->get_float("start", "speed_kt", 0.0) : 0.0;
}

double AircraftSettingsPanel::start_heading_deg(SettingsStore *store, const String &id)
{
	const String key = String("aircraft_") + id + "_start_heading_deg";
	if (store != nullptr && store->has_setting(key))
	{
		const Variant v = store->get_setting(key);
		if (v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT)
		{
			return (double)v;
		}
	}
	return NAN;
}

bool AircraftSettingsPanel::throttle_drives_airbrakes(SettingsStore *store, const Ref<AircraftConfig> &cfg, const String &id)
{
	const String key = String("aircraft_") + id + "_throttle_drives";
	String v;
	if (store != nullptr && store->has_setting(key))
	{
		v = String(store->get_setting(key));
	}
	else if (cfg.is_valid())
	{
		v = cfg->get_string("controls", "throttle_drives", "throttle");
	}
	return v.to_lower() == "airbrakes";
}

OptionButton *AircraftSettingsPanel::get_livery_button() const { return livery; }

String AircraftSettingsPanel::livery_key(const String &id)
{
	return String("aircraft_") + id + "_livery";
}

Dictionary AircraftSettingsPanel::livery_catalog(const Ref<AircraftConfig> &cfg, const String &id)
{
	if (cfg.is_null() || !cfg->is_loaded())
	{
		return Dictionary();
	}
	const String rel = cfg->get_string("liveries", "catalog", "");
	if (rel.is_empty())
	{
		return Dictionary();
	}
	const String path = AircraftConfig::resolve_aircraft_file(id, rel);
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null())
	{
		UtilityFunctions::push_warning("AircraftSettingsPanel: livery catalog not found: ", path);
		return Dictionary();
	}
	const Variant v = JSON::parse_string(f->get_as_text());
	return v.get_type() == Variant::DICTIONARY ? Dictionary(v) : Dictionary();
}

void AircraftSettingsPanel::_on_livery_selected(int idx)
{
	if (updating || store == nullptr || idx < 0 || idx >= livery_ids.size())
	{
		return;
	}
	const String lid = livery_ids[idx];
	store->set_setting(livery_key(aircraft_id), lid);
	emit_signal("livery_changed", aircraft_id, lid);
}

void AircraftSettingsPanel::_on_throttle_drives_selected(int idx)
{
	if (updating || aircraft_id.is_empty() || store == nullptr)
	{
		return;
	}
	store->set_setting(String("aircraft_") + aircraft_id + "_throttle_drives", idx == 1 ? "airbrakes" : "throttle");
	controls_note->set_text(idx == 1 ? "Airbrakes: throttle full = brakes in, throttle idle = brakes fully out; B toggles between the two. Applied now when flying this aircraft."
									 : "Throttle: the usual engine control. Gliders default to the airbrakes (aircraft.cfg [controls] throttle_drives).");
	if (JSBGodot *jsb = Object::cast_to<JSBGodot>(fdm_if_flown()))
	{
		jsb->set_throttle_drives_airbrakes(idx == 1);
	}
}

void AircraftSettingsPanel::update_start_note()
{
	const bool airborne = start_agl->get_value() > 0.0 && start_speed->get_value() > 0.0;
	if (airborne)
	{
		start_note->set_text("The flight starts in the air over the chosen airport, level and trimmed at this height and speed (a glider without a winch or tow starts this way). 0 ft or 0 kt = on the runway.");
	}
	else
	{
		start_note->set_text("On the runway. Give a height and a speed for an airborne start over the airport (a glider needs one until winch and tow exist).");
	}
}

void AircraftSettingsPanel::_on_start_changed(double)
{
	if (updating || aircraft_id.is_empty() || store == nullptr)
	{
		return;
	}
	store->set_setting(start_key("agl_ft"), start_agl->get_value());
	store->set_setting(start_key("speed_kt"), start_speed->get_value());
	if (!runway_heading->is_pressed())
	{
		store->set_setting(start_key("heading_deg"), start_heading->get_value());
	}
	update_start_note();
}

void AircraftSettingsPanel::_on_runway_heading_toggled(bool on)
{
	start_heading->set_editable(!on);
	if (updating || aircraft_id.is_empty() || store == nullptr)
	{
		return;
	}
	if (on)
	{
		store->get_settings().erase(start_key("heading_deg"));
	}
	else
	{
		store->set_setting(start_key("heading_deg"), start_heading->get_value());
	}
}

void AircraftSettingsPanel::_on_fuel_changed(double v)
{
	if (updating || aircraft_id.is_empty())
	{
		return;
	}
	if (store != nullptr)
	{
		store->set_setting(fuel_key(), v);
	}
	Object *jsb = fdm_if_flown();
	if (jsb != nullptr)
	{
		apply_fuel(jsb, v);
	}
	update_fuel_label();
}

void AircraftSettingsPanel::apply_fuel(Object *p_jsb, double pct)
{
	JSBGodot *jsb = Object::cast_to<JSBGodot>(p_jsb);
	if (jsb == nullptr)
	{
		return;
	}
	// wing tanks first, then the centreline ones (the CG moves with the fuel as on the real aircraft)
	jsb->set_fuel_percent(pct);
}

void AircraftSettingsPanel::_update_live(bool force)
{
	// the timer skips hidden tabs; a refresh (screen opening) always fills the label
	if (!force && !is_visible_in_tree())
	{
		return;
	}
	JSBGodot *jsb = Object::cast_to<JSBGodot>(fdm_if_flown());
	if (jsb == nullptr)
	{
		cg_label->set_text("Not flying this aircraft.");
		cg_bar->set_value(0.0);
		return;
	}
	const Dictionary cg = jsb->get_cg_info();
	if (cg.is_empty())
	{
		cg_label->set_text("Flight model not running.");
		return;
	}
	char buf[128];
	snprintf(buf, sizeof(buf), "CG %.1f in aft of the datum, %.1f %% MAC \xE2\x80\x94 weight %.0f lb",
			(double)cg.get("cg_x_in", 0.0), (double)cg.get("pct_mac", 0.0), (double)cg.get("weight_lbs", 0.0));
	cg_label->set_text(String::utf8(buf));
	cg_bar->set_value(Math::clamp((double)cg.get("pct_mac", 0.0), cg_bar->get_min(), cg_bar->get_max()));
	// the FDM's own limits (weight dependent) when it has them, the cfg's otherwise
	if (cg.has("cg_fwd_limit_pct"))
	{
		snprintf(buf, sizeof(buf), "Limits at this weight: %.1f \xE2\x80\x93 %.1f %% MAC (the aircraft's load manager)",
				(double)cg.get("cg_fwd_limit_pct", 0.0), (double)cg.get("cg_aft_limit_pct", 0.0));
		cg_limits->set_text(String::utf8(buf));
	}
	else if (String(cg.get("pct_mac_source", "estimate")) != "estimate" && cg_limits->get_text().begins_with("% MAC estimates"))
	{
		cg_limits->set_text("% MAC from the aircraft's own mean chord; no limits given for this aircraft.");
	}
}

} // namespace jsbgodot
