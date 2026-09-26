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
	ClassDB::bind_method(D_METHOD("fuel_key"), &AircraftSettingsPanel::fuel_key);
	ClassDB::bind_method(D_METHOD("get_aircraft_id"), &AircraftSettingsPanel::get_aircraft_id);
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
	updating = true;
	fuel_slider->set_value(pct);
	updating = false;
	update_fuel_label();
	// reference data
	const TypedArray<Node> kids = info_grid->get_children();
	for (int i = 0; i < kids.size(); i++)
	{
		Object::cast_to<Node>(kids[i])->queue_free();
	}
	Ref<AircraftConfig> cfg;
	if (config_provider.is_valid() && !aircraft_id.is_empty())
	{
		cfg = config_provider.call(aircraft_id);
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
	for (int i = 0; i < jsb->get_tank_count(); i++)
	{
		jsb->set_tank_contents_lbs(i, jsb->get_tank_capacity_lbs(i) * Math::clamp(pct, 0.0, 100.0) / 100.0);
	}
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
}

} // namespace jsbgodot
