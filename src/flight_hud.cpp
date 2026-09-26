#include "flight_hud.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/box_container.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdio>

#include "geo.h"
#include "geo_frame.h"
#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

static const char *const kCloudNames[] = {"", "Cumulus", "Stratus", "Stratocumulus", "Cumulonimbus", "Altostratus", "Altocumulus", "Cirrus"};
static const char *const kTurbNames[] = {"", "light turb", "moderate turb", "severe turb"};

static String cloud_name(int type)
{
	return (type >= 0 && type < 8) ? String(kCloudNames[type]) : String();
}

static String turb_name(int level)
{
	return (level >= 0 && level < 4) ? String(kTurbNames[level]) : String();
}

void FlightHud::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_flight_hud", "root"), &FlightHud::register_flight_hud);
	ClassDB::bind_method(D_METHOD("poll_keys"), &FlightHud::poll_keys);
	ClassDB::bind_method(D_METHOD("update", "jsb"), &FlightHud::update);
	ClassDB::bind_method(D_METHOD("set_time_text", "text"), &FlightHud::set_time_text);
	ClassDB::bind_method(D_METHOD("get_hud_mode"), &FlightHud::get_hud_mode);
	ClassDB::bind_method(D_METHOD("set_hud_mode", "mode"), &FlightHud::set_hud_mode);
	ClassDB::bind_method(D_METHOD("is_fps_visible"), &FlightHud::is_fps_visible);
	ClassDB::bind_method(D_METHOD("set_fps_visible", "visible"), &FlightHud::set_fps_visible);

	ClassDB::bind_method(D_METHOD("set_weather_overlay", "show"), &FlightHud::set_weather_overlay);
	ClassDB::bind_method(D_METHOD("is_weather_overlay_visible"), &FlightHud::is_weather_overlay_visible);
	ClassDB::bind_method(D_METHOD("set_weather_state", "live", "live_weather", "static_layers", "qnh_hpa", "visibility_km"), &FlightHud::set_weather_state);
	ClassDB::bind_method(D_METHOD("get_weather_label"), &FlightHud::get_weather_label);

	ClassDB::bind_method(D_METHOD("show_intro_hud", "show"), &FlightHud::show_intro_hud);
	ClassDB::bind_method(D_METHOD("is_intro_hud_visible"), &FlightHud::is_intro_hud_visible);
	ClassDB::bind_method(D_METHOD("set_intro_status", "text"), &FlightHud::set_intro_status);
	ClassDB::bind_method(D_METHOD("set_intro_bar", "frac"), &FlightHud::set_intro_bar);
	ClassDB::bind_method(D_METHOD("reset_intro_progress"), &FlightHud::reset_intro_progress);
	ClassDB::bind_method(D_METHOD("update_intro", "distance_m", "pending_count", "loading"), &FlightHud::update_intro);
	ClassDB::bind_method(D_METHOD("get_intro_hud"), &FlightHud::get_intro_hud);
	ClassDB::bind_method(D_METHOD("get_intro_dist_label"), &FlightHud::get_intro_dist_label);
	ClassDB::bind_method(D_METHOD("get_intro_status_label"), &FlightHud::get_intro_status_label);
	ClassDB::bind_method(D_METHOD("get_intro_bar"), &FlightHud::get_intro_bar);
	ClassDB::bind_method(D_METHOD("get_intro_bar_bg"), &FlightHud::get_intro_bar_bg);

	ClassDB::bind_static_method("FlightHud", D_METHOD("format_swiss_coord", "value"), &FlightHud::format_swiss_coord);
	ClassDB::bind_static_method("FlightHud", D_METHOD("format_pm_coord", "deg", "is_lat"), &FlightHud::format_pm_coord);
	// Ctrl+I toggled the overlay: the host pushes the current weather state.
	ADD_SIGNAL(MethodInfo("weather_overlay_toggled", PropertyInfo(Variant::BOOL, "show")));
}

FlightHud::FlightHud()
{
}

void FlightHud::_ready()
{
	set_layer(100);
	build_overlays();
}

// ── Construction ────────────────────────────────────────────────────────────

void FlightHud::outline_label(Label *label, int px)
{
	// Black outline so HUD text stays readable over bright or similar-coloured
	// terrain and sky.
	label->add_theme_constant_override("outline_size", px);
	label->add_theme_color_override("font_outline_color", Color(0.0, 0.0, 0.0, 0.85));
}

void FlightHud::outline_labels_recursive(Node *node)
{
	Label *label = Object::cast_to<Label>(node);
	if (label != nullptr)
	{
		outline_label(label, 3);
	}
	const TypedArray<Node> children = node->get_children();
	for (int i = 0; i < children.size(); i++)
	{
		outline_labels_recursive(Object::cast_to<Node>(children[i]));
	}
}

void FlightHud::build_overlays()
{
	if (fps_label != nullptr)
	{
		return;
	}
	fps_label = memnew(Label);
	fps_label->set_text("FPS: 0");
	fps_label->add_theme_font_size_override("font_size", 18);
	fps_label->add_theme_color_override("font_color", Color(1, 1, 1));
	fps_label->add_theme_color_override("font_shadow_color", Color(0, 0, 0));
	fps_label->add_theme_constant_override("shadow_offset_x", 1);
	fps_label->add_theme_constant_override("shadow_offset_y", 1);
	fps_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	fps_label->set_anchors_preset(Control::PRESET_TOP_RIGHT);
	fps_label->set_offset(SIDE_LEFT, -120);
	fps_label->set_offset(SIDE_RIGHT, -10);
	fps_label->set_offset(SIDE_TOP, 10);
	fps_label->set_visible(fps_visible);
	add_child(fps_label);

	wx_label = memnew(Label);
	wx_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	wx_label->add_theme_font_size_override("font_size", 20);
	wx_label->add_theme_color_override("font_color", Color(0.95, 0.97, 1.0));
	outline_label(wx_label, 4);
	wx_label->set_anchors_preset(Control::PRESET_CENTER_TOP);
	wx_label->set_offset(SIDE_LEFT, -500);
	wx_label->set_offset(SIDE_RIGHT, 500);
	wx_label->set_offset(SIDE_TOP, 12);
	wx_label->set_visible(wx_visible);
	add_child(wx_label);
	refresh_weather_overlay();
}

Label *FlightHud::hud_label(const char *path) const
{
	if (hud_root == nullptr)
	{
		return nullptr;
	}
	return Object::cast_to<Label>(hud_root->get_node_or_null(NodePath(path)));
}

void FlightHud::register_flight_hud(Control *root)
{
	hud_root = root;
	airspeed_label = hud_label("BottomRow/Label");
	altitude_label = hud_label("BottomRow/Label2");
	vspeed_label = hud_label("BottomRow/Label3");
	throttle_label = hud_label("BottomRow/Label4");
	heading_label = hud_label("BottomRow/Label5");
	ecef_label = hud_label("Label6");
	swiss_label = hud_label("Label7");
	pm_label = hud_label("Label9");
	time_label = hud_label("TimeLabel");
	if (hud_root != nullptr)
	{
		outline_labels_recursive(hud_root);
	}
	apply_hud_mode();
}

// ── HUD mode / keys ─────────────────────────────────────────────────────────

// 0 nothing, 1 the bottom readouts (airspeed, altitude, vertical speed,
// throttle, heading), 2 also the upper info block (coordinates, PM, ECEF,
// time, slew heading). The VFR map, the weather overlay and the FPS counter
// have their own toggles.
void FlightHud::apply_hud_mode()
{
	if (hud_root == nullptr)
	{
		return;
	}
	Control *bottom = Object::cast_to<Control>(hud_root->get_node_or_null(NodePath("BottomRow")));
	if (bottom != nullptr)
	{
		bottom->set_visible(hud_mode >= 1);
	}
	static const char *const upper[] = {"Label6", "Label7", "Label8", "Label9", "TimeLabel"};
	for (const char *nm : upper)
	{
		Control *c = Object::cast_to<Control>(hud_root->get_node_or_null(NodePath(nm)));
		if (c != nullptr)
		{
			c->set_visible(hud_mode >= 2);
		}
	}
}

int FlightHud::get_hud_mode() const { return hud_mode; }

void FlightHud::set_hud_mode(int mode)
{
	hud_mode = ((mode % 3) + 3) % 3;
	apply_hud_mode();
}

bool FlightHud::is_fps_visible() const { return fps_visible; }

void FlightHud::set_fps_visible(bool visible)
{
	fps_visible = visible;
	if (fps_label != nullptr)
	{
		fps_label->set_visible(visible);
	}
}

void FlightHud::poll_keys()
{
	if (fps_label != nullptr && fps_visible)
	{
		fps_label->set_text("FPS: " + String::num_int64(Engine::get_singleton()->get_frames_per_second()));
	}
	Input *input = Input::get_singleton();
	const bool ctrl = input->is_key_pressed(KEY_CTRL);
	const bool shift = input->is_key_pressed(KEY_SHIFT);

	if (input->is_key_pressed(KEY_H) && !ctrl)
	{
		if (!hud_key_held)
		{
			hud_key_held = true;
			set_hud_mode(hud_mode + 1);
		}
	}
	else
	{
		hud_key_held = false;
	}

	if (ctrl && shift && input->is_key_pressed(KEY_F))
	{
		if (!fps_key_held)
		{
			fps_key_held = true;
			set_fps_visible(!fps_visible);
		}
	}
	else
	{
		fps_key_held = false;
	}

	if (ctrl && input->is_key_pressed(KEY_I))
	{
		if (!wx_key_held)
		{
			wx_key_held = true;
			set_weather_overlay(!wx_visible);
			emit_signal("weather_overlay_toggled", wx_visible);
		}
	}
	else
	{
		wx_key_held = false;
	}
}

// ── Readouts ────────────────────────────────────────────────────────────────

static String rounded(double v)
{
	return String::num_int64((int64_t)std::llround(v));
}

void FlightHud::update(Object *p_jsb)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm == nullptr)
	{
		return;
	}
	if (airspeed_label != nullptr)
	{
		airspeed_label->set_text("Airspeed: " + rounded(fdm->get_airspeed_knots()) + " knots");
	}
	if (altitude_label != nullptr)
	{
		altitude_label->set_text("Altitude: " + rounded(fdm->get_altitude_ft()) + " feet");
	}
	if (vspeed_label != nullptr)
	{
		vspeed_label->set_text("Vertical Speed: " + rounded(fdm->get_vertical_speed_fpm()) + " ft/m");
	}
	if (throttle_label != nullptr)
	{
		throttle_label->set_text("Throttle: " + rounded(fdm->get_input_throttle() * 100.0) + " %");
	}
	if (heading_label != nullptr)
	{
		// JSBSim runs at the true location: psi IS the true heading.
		heading_label->set_text("Heading: " + rounded(fdm->get_heading()));
	}
	const Vector3 pos = fdm->get_godot_position(); // ECEF metres
	if (ecef_label != nullptr)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "ECEF: X=%.0f Y=%.0f Z=%.0f", (double)pos.x, (double)pos.y, (double)pos.z);
		ecef_label->set_text(buf);
	}
	if (swiss_label != nullptr || pm_label != nullptr)
	{
		const geo::Geodetic g = geo::geodetic_of(pos);
		if (swiss_label != nullptr)
		{
			const Vector2 lv = Geo::wgs84_to_lv95(g.lat, g.lon);
			swiss_label->set_text("Swiss E: " + format_swiss_coord(lv.x) + ", N: " + format_swiss_coord(lv.y));
		}
		if (pm_label != nullptr)
		{
			// Prime Meridian (WGS84) display, world-scenery design §3.4
			pm_label->set_text("PM: " + format_pm_coord(g.lat, true) + "  " + format_pm_coord(g.lon, false));
		}
	}
}

void FlightHud::set_time_text(const String &text)
{
	if (time_label != nullptr)
	{
		time_label->set_text(text);
	}
}

String FlightHud::format_swiss_coord(double value)
{
	const String digits = String::num_int64((int64_t)std::llround(value));
	String result;
	int count = 0;
	for (int i = digits.length() - 1; i >= 0; i--)
	{
		if (count > 0 && count % 3 == 0)
		{
			result = String("'") + result;
		}
		result = digits.substr(i, 1) + result;
		count++;
	}
	return result;
}

String FlightHud::format_pm_coord(double deg, bool is_lat)
{
	const char *hemi = is_lat ? (deg >= 0.0 ? "N" : "S") : (deg >= 0.0 ? "E" : "W");
	const double a = std::abs(deg);
	int d = (int)a;
	// Round minutes to 2 decimals before formatting so 59.997 doesn't print as "60.00".
	double m = std::round((a - (double)d) * 60.0 * 100.0) / 100.0;
	if (m >= 60.0)
	{
		d += 1;
		m = 0.0;
	}
	char buf[48];
	// \xC2\xB0 = °, \xE2\x80\xB2 = ′ (UTF-8)
	if (is_lat)
	{
		snprintf(buf, sizeof(buf), "%s %d\xC2\xB0%05.2f\xE2\x80\xB2", hemi, d, m);
	}
	else
	{
		snprintf(buf, sizeof(buf), "%s %03d\xC2\xB0%05.2f\xE2\x80\xB2", hemi, d, m);
	}
	return String::utf8(buf);
}

// ── Weather overlay ─────────────────────────────────────────────────────────

void FlightHud::set_weather_overlay(bool show)
{
	wx_visible = show;
	if (wx_label != nullptr)
	{
		wx_label->set_visible(show);
		refresh_weather_overlay();
	}
}

bool FlightHud::is_weather_overlay_visible() const { return wx_visible; }

void FlightHud::set_weather_state(bool live, const Dictionary &live_weather, const Array &static_layers, double qnh_hpa, double visibility_km)
{
	wx_live = live;
	wx_live_weather = live_weather;
	wx_static_layers = static_layers;
	wx_qnh_hpa = qnh_hpa;
	wx_visibility_km = visibility_km;
	refresh_weather_overlay();
}

Label *FlightHud::get_weather_label() const { return wx_label; }

void FlightHud::refresh_weather_overlay()
{
	if (wx_label == nullptr || !wx_visible)
	{
		return;
	}
	wx_label->set_text(weather_overlay_text());
}

// The METAR (station, wind, QNH, visibility, cloud layers, raw report) when
// live weather has one, else the static layer stack from the settings.
String FlightHud::weather_overlay_text() const
{
	PackedStringArray lines;
	char buf[160];
	if (wx_live && !wx_live_weather.is_empty())
	{
		const Dictionary &wx = wx_live_weather;
		lines.push_back(String("METAR ") + String(wx.get("station", "?")) + " " + String(wx.get("when", "")) + "   " + String(wx.get("name", "")));
		snprintf(buf, sizeof(buf), "   QNH %.0f hPa   Visibility %.0f km", (double)wx.get("qnh_hpa", 0.0), (double)wx.get("visibility_km", 0.0));
		lines.push_back(String("Wind ") + String(wx.get("wind_txt", "")) + buf);
		PackedStringArray clouds;
		const Array layers = wx.get("layers", Array());
		for (int i = 0; i < layers.size(); i++)
		{
			const Dictionary l = layers[i];
			const int oktas = (int)l.get("coverage_oktas", 0);
			if (oktas > 0)
			{
				clouds.push_back(String::num_int64(oktas) + "/8 " + cloud_name((int)l.get("cloud_type", 0)) + " at " + String::num_int64((int64_t)(double)l.get("base_m", 0.0)) + " m");
			}
		}
		lines.push_back(String("Clouds: ") + (clouds.is_empty() ? String("sky clear") : String(", ").join(clouds)));
		const String raw = wx.get("raw", "");
		if (!raw.is_empty())
		{
			lines.push_back(raw);
		}
		return String("\n").join(lines);
	}
	lines.push_back(wx_live ? String::utf8("Live weather: waiting for a METAR\xE2\x80\xA6") : String("Static weather (Settings > Weather)"));
	snprintf(buf, sizeof(buf), "QNH %.0f hPa   Visibility %.0f km", wx_qnh_hpa, wx_visibility_km);
	lines.push_back(buf);
	for (int i = 0; i < wx_static_layers.size(); i++)
	{
		const Dictionary l = wx_static_layers[i];
		// \xE2\x80\x93 = – (en dash), \xC2\xB0 = °
		snprintf(buf, sizeof(buf), "%d\xE2\x80\x93%d m: wind %03d\xC2\xB0/%d kt",
				(int)l.get("base_m", 0), (int)l.get("top_m", 0), (int)l.get("wind_dir_deg", 0), (int)l.get("wind_speed_kts", 0));
		PackedStringArray parts;
		parts.push_back(String::utf8(buf));
		const int turb = (int)l.get("turbulence", 0);
		if (turb > 0)
		{
			parts.push_back(turb_name(turb));
		}
		const int oktas = (int)l.get("coverage_oktas", 0);
		if (oktas > 0)
		{
			parts.push_back(String::num_int64(oktas) + "/8 " + cloud_name((int)l.get("cloud_type", 0)));
		}
		lines.push_back(String(", ").join(parts));
	}
	return String("\n").join(lines);
}

// ── Intro HUD ───────────────────────────────────────────────────────────────

void FlightHud::build_intro_hud()
{
	if (intro_hud != nullptr)
	{
		return;
	}
	intro_hud = memnew(CanvasLayer);
	intro_hud->set_name("IntroHud");
	intro_hud->set_layer(5);
	add_child(intro_hud);
	VBoxContainer *box = memnew(VBoxContainer);
	box->set_anchors_and_offsets_preset(Control::PRESET_CENTER_BOTTOM);
	box->set_offset(SIDE_TOP, -150.0);
	box->set_offset(SIDE_BOTTOM, -40.0);
	box->set_offset(SIDE_LEFT, -320.0);
	box->set_offset(SIDE_RIGHT, 320.0);
	box->set_alignment(BoxContainer::ALIGNMENT_END);
	box->add_theme_constant_override("separation", 12);
	intro_hud->add_child(box);

	intro_dist_label = memnew(Label);
	intro_dist_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	intro_dist_label->add_theme_font_size_override("font_size", 24);
	intro_dist_label->add_theme_color_override("font_color", Color(0.35, 0.9, 1.0));
	outline_label(intro_dist_label, 4);
	box->add_child(intro_dist_label);

	intro_status_label = memnew(Label);
	intro_status_label->set_text("Loading scenery");
	intro_status_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	intro_status_label->add_theme_font_size_override("font_size", 18);
	intro_status_label->add_theme_color_override("font_color", Color(0.85, 0.9, 0.95));
	outline_label(intro_status_label, 4);
	box->add_child(intro_status_label);

	intro_bar_bg = memnew(ColorRect);
	intro_bar_bg->set_color(Color(0.12, 0.14, 0.18, 0.85));
	intro_bar_bg->set_custom_minimum_size(Vector2(600, 12));
	box->add_child(intro_bar_bg);
	intro_bar = memnew(ColorRect);
	intro_bar->set_color(Color(0.35, 0.9, 1.0));
	intro_bar->set_custom_minimum_size(Vector2(0, 12));
	intro_bar_bg->add_child(intro_bar);

	Label *hint = memnew(Label);
	hint->set_text("Press Space to skip animation");
	hint->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	hint->add_theme_font_size_override("font_size", 28);
	hint->add_theme_color_override("font_color", Color(0.95, 0.95, 0.9));
	outline_label(hint, 4);
	box->add_child(hint);
}

void FlightHud::show_intro_hud(bool show)
{
	build_intro_hud();
	intro_hud->set_visible(show);
	// The flight readouts mean nothing from space.
	if (hud_root != nullptr)
	{
		hud_root->set_visible(!show);
	}
}

bool FlightHud::is_intro_hud_visible() const
{
	return intro_hud != nullptr && intro_hud->is_visible();
}

void FlightHud::set_intro_status(const String &text)
{
	if (intro_status_label != nullptr)
	{
		intro_status_label->set_text(text);
	}
}

void FlightHud::set_intro_bar(double frac)
{
	if (intro_bar == nullptr || intro_bar_bg == nullptr)
	{
		return;
	}
	const double w = intro_bar_bg->get_size().x * Math::clamp(frac, 0.0, 1.0);
	intro_bar->set_custom_minimum_size(Vector2(w, intro_bar->get_custom_minimum_size().y));
	intro_bar->set_size(Vector2(w, intro_bar->get_size().y));
}

void FlightHud::reset_intro_progress()
{
	intro_pending_prev = -1;
	intro_done = 0;
	intro_shown = 0.0;
	set_intro_bar(0.0);
}

void FlightHud::update_intro(double distance_m, int pending_count, bool loading)
{
	if (intro_hud == nullptr || !intro_hud->is_visible())
	{
		return;
	}
	if (intro_dist_label != nullptr && std::isfinite(distance_m))
	{
		char buf[64];
		if (distance_m >= 10000.0)
		{
			snprintf(buf, sizeof(buf), "Distance to aircraft: %.0f km", distance_m / 1000.0);
		}
		else
		{
			snprintf(buf, sizeof(buf), "Distance to aircraft: %.0f m", distance_m);
		}
		intro_dist_label->set_text(buf);
	}
	if (loading || pending_count < 0)
	{
		return; // the bar shows the loading phases
	}
	// Completed work = decreases of the pending count; new requests (the
	// descending camera keeps asking for finer rings) only add to the
	// denominator. The shown fraction never goes back, it just pauses.
	if (intro_pending_prev >= 0 && pending_count < intro_pending_prev)
	{
		intro_done += intro_pending_prev - pending_count;
	}
	intro_pending_prev = pending_count;
	const int total = intro_done + pending_count;
	// nothing queued and nothing done yet: the streamer has not started, not "all done"
	const double frac = total == 0 ? (intro_done > 0 ? 1.0 : 0.0) : (double)intro_done / (double)total;
	intro_shown = Math::max(intro_shown, Math::clamp(frac, 0.0, 1.0));
	set_intro_bar(intro_shown);
}

CanvasLayer *FlightHud::get_intro_hud() const { return intro_hud; }
Label *FlightHud::get_intro_dist_label() const { return intro_dist_label; }
Label *FlightHud::get_intro_status_label() const { return intro_status_label; }
ColorRect *FlightHud::get_intro_bar() const { return intro_bar; }
ColorRect *FlightHud::get_intro_bar_bg() const { return intro_bar_bg; }

} // namespace jsbgodot
