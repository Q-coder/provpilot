#include "live_weather.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace godot;

namespace jsbgodot
{

void LiveWeather::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("refresh", "force"), &LiveWeather::refresh, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("needs_refetch", "ll", "now_msec"), &LiveWeather::needs_refetch);
	ClassDB::bind_method(D_METHOD("set_last_fetch", "ll", "msec"), &LiveWeather::set_last_fetch);
	ClassDB::bind_static_method("LiveWeather", D_METHOD("distance_km", "a", "b"), &LiveWeather::distance_km);
	ClassDB::bind_static_method("LiveWeather", D_METHOD("build_url", "src", "lat", "lon"), &LiveWeather::build_url);
	ClassDB::bind_static_method("LiveWeather", D_METHOD("parse_response", "src", "body", "lat", "lon"), &LiveWeather::parse_response);
	ClassDB::bind_static_method("LiveWeather", D_METHOD("report_to_weather", "rep"), &LiveWeather::report_to_weather);
	ClassDB::bind_method(D_METHOD("get_source"), &LiveWeather::get_source);
	ClassDB::bind_method(D_METHOD("set_source", "source"), &LiveWeather::set_source);
	ClassDB::bind_method(D_METHOD("get_enabled"), &LiveWeather::get_enabled);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LiveWeather::set_enabled);
	ClassDB::bind_method(D_METHOD("get_latlon_provider"), &LiveWeather::get_latlon_provider);
	ClassDB::bind_method(D_METHOD("set_latlon_provider", "callable"), &LiveWeather::set_latlon_provider);
	ClassDB::bind_method(D_METHOD("get_last_weather"), &LiveWeather::get_last_weather);
	ClassDB::bind_method(D_METHOD("set_last_weather", "weather"), &LiveWeather::set_last_weather);
	ClassDB::bind_method(D_METHOD("_on_check_tick"), &LiveWeather::_on_check_tick);
	ClassDB::bind_method(D_METHOD("_on_request_completed", "result", "code", "headers", "body"), &LiveWeather::_on_request_completed);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source"), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "latlon_provider"), "set_latlon_provider", "get_latlon_provider");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "last_weather"), "set_last_weather", "get_last_weather");
	ADD_SIGNAL(MethodInfo("weather_updated", PropertyInfo(Variant::DICTIONARY, "wx")));
	ADD_SIGNAL(MethodInfo("weather_failed", PropertyInfo(Variant::STRING, "reason")));
	BIND_ENUM_CONSTANT(CT_NONE);
	BIND_ENUM_CONSTANT(CT_CUMULUS);
	BIND_ENUM_CONSTANT(CT_STRATUS);
	BIND_ENUM_CONSTANT(CT_STRATOCUMULUS);
	BIND_ENUM_CONSTANT(CT_CUMULONIMBUS);
	BIND_ENUM_CONSTANT(CT_ALTOSTRATUS);
	BIND_ENUM_CONSTANT(CT_ALTOCUMULUS);
	BIND_ENUM_CONSTANT(CT_CIRRUS);
	ClassDB::bind_integer_constant("LiveWeather", "", "UNLIMITED_VIS_KM", (int64_t)UNLIMITED_VIS_KM);
}

LiveWeather::LiveWeather()
{
	http = memnew(HTTPRequest);
	http->set_use_threads(true);
	http->set_timeout(20.0);
	http->connect("request_completed", Callable(this, "_on_request_completed"));
	add_child(http);
	timer = memnew(Timer);
	timer->set_wait_time(CHECK_SEC);
	timer->set_one_shot(false);
	timer->connect("timeout", Callable(this, "_on_check_tick"));
	add_child(timer);
}

String LiveWeather::get_source() const { return source; }
void LiveWeather::set_source(const String &s) { source = s; }
bool LiveWeather::get_enabled() const { return enabled; }
void LiveWeather::set_enabled(bool v)
{
	enabled = v;
	if (!v && timer != nullptr)
	{
		timer->stop();
	}
}
Callable LiveWeather::get_latlon_provider() const { return latlon_provider; }
void LiveWeather::set_latlon_provider(const Callable &c) { latlon_provider = c; }
Dictionary LiveWeather::get_last_weather() const { return last_weather; }
void LiveWeather::set_last_weather(const Dictionary &d) { last_weather = d; }

void LiveWeather::refresh(bool force)
{
	if (!enabled)
	{
		return;
	}
	if (!is_inside_tree())
	{
		call_deferred("refresh", force); // HTTPRequest / Timer need the tree
		return;
	}
	if (timer->is_stopped())
	{
		timer->start();
	}
	if (busy || !latlon_provider.is_valid())
	{
		return;
	}
	const Vector2 ll = latlon_provider.call();
	if (!force && !needs_refetch(ll, Time::get_singleton()->get_ticks_msec()))
	{
		return;
	}
	const String url = build_url(source, ll.x, ll.y);
	if (url.is_empty())
	{
		emit_signal("weather_failed", String("unknown weather source '") + source + "'");
		return;
	}
	const Error err = http->request(url);
	if (err != OK)
	{
		emit_signal("weather_failed", String("request error ") + String::num_int64((int)err));
		return;
	}
	busy = true;
	last_fetch_latlon = ll;
	last_fetch_msec = Time::get_singleton()->get_ticks_msec();
	UtilityFunctions::print("LiveWeather: requesting ", url);
}

void LiveWeather::_on_check_tick()
{
	refresh(false);
}

void LiveWeather::set_last_fetch(const Vector2 &ll, int64_t msec)
{
	last_fetch_latlon = ll;
	last_fetch_msec = msec;
}

bool LiveWeather::needs_refetch(const Vector2 &ll, int64_t now_msec) const
{
	if (last_fetch_msec < 0 || !std::isfinite(last_fetch_latlon.x))
	{
		return true;
	}
	if (now_msec - last_fetch_msec >= (int64_t)(REFRESH_SEC * 1000.0))
	{
		return true;
	}
	return distance_km(ll, last_fetch_latlon) >= MOVE_REFETCH_KM;
}

double LiveWeather::distance_km(const Vector2 &a, const Vector2 &b)
{
	const double dlat = Math::deg_to_rad((double)(b.x - a.x));
	const double dlon = Math::deg_to_rad((double)(b.y - a.y)) * std::cos(Math::deg_to_rad((double)(a.x + b.x) * 0.5));
	return 6371.0 * std::sqrt(dlat * dlat + dlon * dlon);
}

void LiveWeather::_on_request_completed(int result, int code, const PackedStringArray &headers, const PackedByteArray &body)
{
	busy = false;
	if (result != HTTPRequest::RESULT_SUCCESS || code < 200 || code >= 300)
	{
		emit_signal("weather_failed", String("HTTP result ") + String::num_int64(result) + " code " + String::num_int64(code));
		return;
	}
	if (!latlon_provider.is_valid())
	{
		return;
	}
	const Vector2 ll = latlon_provider.call();
	const Dictionary report = parse_response(source, body.get_string_from_utf8(), ll.x, ll.y);
	if (report.is_empty())
	{
		emit_signal("weather_failed", "no METAR station within the search box");
		return;
	}
	last_weather = report_to_weather(report);
	UtilityFunctions::print("LiveWeather: ", String(last_weather.get("status", "")));
	emit_signal("weather_updated", last_weather);
}

// ── Source: Aviation Weather Center ─────────────────────────────────────────

String LiveWeather::build_url(const String &src, double lat, double lon)
{
	if (src == "awc")
	{
		char buf[200];
		snprintf(buf, sizeof(buf), "https://aviationweather.gov/api/data/metar?format=json&bbox=%.3f,%.3f,%.3f,%.3f",
				lat - SEARCH_DLAT, lon - SEARCH_DLON, lat + SEARCH_DLAT, lon + SEARCH_DLON);
		return String(buf);
	}
	return String();
}

Dictionary LiveWeather::parse_response(const String &src, const String &body, double lat, double lon)
{
	if (src == "awc")
	{
		return parse_awc(body, lat, lon);
	}
	return Dictionary();
}

static bool is_null(const Variant &v)
{
	return v.get_type() == Variant::NIL;
}

Dictionary LiveWeather::parse_awc(const String &body, double lat, double lon)
{
	const Variant data = JSON::parse_string(body);
	if (data.get_type() != Variant::ARRAY)
	{
		return Dictionary();
	}
	const Array items = data;
	Dictionary best;
	bool have = false;
	double best_d = std::numeric_limits<double>::infinity();
	for (int i = 0; i < items.size(); i++)
	{
		if (items[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary r = items[i];
		if (!r.has("lat") || !r.has("lon") || String(r.get("icaoId", "")).is_empty())
		{
			continue;
		}
		const double dlat = (double)r["lat"] - lat;
		const double dlon = ((double)r["lon"] - lon) * std::cos(Math::deg_to_rad(lat));
		const double d = dlat * dlat + dlon * dlon;
		if (d < best_d)
		{
			best_d = d;
			best = r;
			have = true;
		}
	}
	if (!have)
	{
		return Dictionary();
	}
	Dictionary rep;
	rep["station"] = String(best.get("icaoId", ""));
	rep["name"] = String(best.get("name", ""));
	rep["obs_time"] = is_null(best.get("obsTime", Variant())) ? 0 : (int64_t)(double)best.get("obsTime", 0);
	rep["lat"] = (double)best["lat"];
	rep["lon"] = (double)best["lon"];
	rep["elev_m"] = is_null(best.get("elev", Variant())) ? 0.0 : (double)best.get("elev", 0.0);
	rep["wdir_deg"] = -1;
	rep["wspd_kts"] = 0;
	rep["wgst_kts"] = 0;
	rep["vis_km"] = -1.0;
	rep["qnh_hpa"] = -1.0;
	rep["clouds"] = Array();
	rep["raw"] = String(best.get("rawOb", ""));
	const Variant wdir = best.get("wdir", Variant());
	if (!is_null(wdir) && wdir.get_type() != Variant::STRING)
	{
		rep["wdir_deg"] = (int)(double)wdir % 360;
	}
	if (!is_null(best.get("wspd", Variant())))
	{
		rep["wspd_kts"] = (int)(double)best["wspd"];
	}
	if (!is_null(best.get("wgst", Variant())))
	{
		rep["wgst_kts"] = (int)(double)best["wgst"];
	}
	const Variant visib = best.get("visib", Variant());
	if (is_null(visib))
	{
		rep["vis_km"] = -1.0; // AWC drops "9999"; treat as unlimited downstream
	}
	else if (visib.get_type() == Variant::STRING)
	{
		const String vs = visib;
		if (vs.ends_with("+"))
		{
			rep["vis_km"] = UNLIMITED_VIS_KM;
		}
		else if (vs.is_valid_float())
		{
			rep["vis_km"] = vs.to_float() * SM_TO_KM;
		}
	}
	else
	{
		rep["vis_km"] = (double)visib * SM_TO_KM;
	}
	if (!is_null(best.get("altim", Variant())))
	{
		rep["qnh_hpa"] = (double)best["altim"];
	}
	const String raw = rep["raw"];
	Array clouds;
	const Variant cl = best.get("clouds", Variant());
	if (cl.get_type() == Variant::ARRAY)
	{
		const Array cls = cl;
		for (int i = 0; i < cls.size(); i++)
		{
			if (cls[i].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary c = cls[i];
			const String cover = String(c.get("cover", "")).to_upper();
			const int oktas = cover_to_oktas(cover);
			if (oktas <= 0)
			{
				continue;
			}
			const double base_ft = is_null(c.get("base", Variant())) ? 0.0 : (double)c.get("base", 0.0);
			String special;
			char tok[32];
			snprintf(tok, sizeof(tok), "%s%03d", cover.utf8().get_data(), (int)std::lround(base_ft / 100.0));
			if (raw.contains(String(tok) + "CB"))
			{
				special = "CB";
			}
			else if (raw.contains(String(tok) + "TCU"))
			{
				special = "TCU";
			}
			Dictionary cd;
			cd["oktas"] = oktas;
			cd["base_agl_m"] = base_ft * FT_TO_M;
			cd["special"] = special;
			clouds.push_back(cd);
		}
	}
	rep["clouds"] = clouds;
	return rep;
}

int LiveWeather::cover_to_oktas(const String &cover)
{
	if (cover == "FEW") return 2;
	if (cover == "SCT") return 4;
	if (cover == "BKN") return 6;
	if (cover == "OVC" || cover == "OVX" || cover == "VV") return 8;
	return 0; // CLR SKC NSC NCD CAVOK
}

int LiveWeather::cloud_thickness_m(int type)
{
	switch (type)
	{
		case CT_CUMULUS: return 900;
		case CT_STRATUS: return 400;
		case CT_STRATOCUMULUS: return 600;
		case CT_CUMULONIMBUS: return 6000;
		case CT_ALTOSTRATUS: return 1200;
		case CT_ALTOCUMULUS: return 800;
		case CT_CIRRUS: return 1000;
		default: return 0;
	}
}

// Surface layer 0..1500 m (or up to the lowest cloud base) carries the wind;
// gusts set its turbulence. One layer per cloud group above, base = station
// elevation + reported AGL base, thickness by cloud type. METARs have no
// winds aloft, so every layer gets the surface wind.
Dictionary LiveWeather::report_to_weather(const Dictionary &rep)
{
	const int wdir = (int)(double)rep.get("wdir_deg", -1);
	const int wspd = (int)(double)rep.get("wspd_kts", 0);
	const int wgst = (int)(double)rep.get("wgst_kts", 0);
	const double elev = rep.get("elev_m", 0.0);
	int turb = 0;
	const int gust_delta = wgst > 0 ? wgst - wspd : 0;
	if (gust_delta >= 20)
	{
		turb = 3;
	}
	else if (gust_delta >= 10)
	{
		turb = 2;
	}
	else if (gust_delta >= 5 || wspd >= 20 || (wdir < 0 && wspd >= 5))
	{
		turb = 1;
	}
	const int dir_deg = wdir >= 0 ? wdir : 0;

	std::vector<Dictionary> cloud_layers;
	const Array clouds = rep.get("clouds", Array());
	for (int i = 0; i < clouds.size(); i++)
	{
		const Dictionary c = clouds[i];
		const int oktas = (int)(double)c.get("oktas", 0);
		const double base_agl = c.get("base_agl_m", 0.0);
		const String special = c.get("special", "");
		int ctype = CT_CUMULUS;
		if (special == "CB")
		{
			ctype = CT_CUMULONIMBUS;
		}
		else if (special == "TCU")
		{
			ctype = CT_CUMULUS;
		}
		else if (base_agl >= 6000.0)
		{
			ctype = CT_CIRRUS;
		}
		else if (base_agl >= 2000.0)
		{
			ctype = oktas >= 6 ? CT_ALTOSTRATUS : CT_ALTOCUMULUS;
		}
		else if (oktas >= 6)
		{
			ctype = base_agl < 600.0 ? CT_STRATUS : CT_STRATOCUMULUS;
		}
		const int base_m = (int)std::lround(elev + base_agl);
		Dictionary l;
		l["base_m"] = base_m;
		l["top_m"] = base_m + cloud_thickness_m(ctype);
		l["cloud_type"] = ctype;
		l["coverage_oktas"] = oktas;
		l["turbulence"] = ctype == CT_CUMULONIMBUS ? 3 : (special == "TCU" ? 2 : 0);
		cloud_layers.push_back(l);
	}
	std::stable_sort(cloud_layers.begin(), cloud_layers.end(), [](const Dictionary &a, const Dictionary &b) {
		return (int)a["base_m"] < (int)b["base_m"];
	});

	Array layers;
	int surface_top = (int)elev + SURFACE_LAYER_TOP_M;
	if (!cloud_layers.empty())
	{
		surface_top = std::min(surface_top, (int)cloud_layers[0]["base_m"] - 100);
	}
	surface_top = std::max(surface_top, 100);
	{
		Dictionary l;
		l["base_m"] = 0;
		l["top_m"] = surface_top;
		l["wind_dir_deg"] = dir_deg;
		l["wind_speed_kts"] = wspd;
		l["turbulence"] = turb;
		l["cloud_type"] = (int)CT_NONE;
		l["coverage_oktas"] = 0;
		layers.push_back(l);
	}
	int prev_top = surface_top;
	for (const Dictionary &cl : cloud_layers)
	{
		const int base_m = std::max((int)cl["base_m"], prev_top);
		const int top_m = base_m + std::max((int)cl["top_m"] - (int)cl["base_m"], 100); // keep the thickness when lifted
		Dictionary l;
		l["base_m"] = base_m;
		l["top_m"] = top_m;
		l["wind_dir_deg"] = dir_deg;
		l["wind_speed_kts"] = wspd;
		l["turbulence"] = std::max((int)cl["turbulence"], base_m < 3000 ? turb : 0);
		l["cloud_type"] = (int)cl["cloud_type"];
		l["coverage_oktas"] = (int)cl["coverage_oktas"];
		layers.push_back(l);
		prev_top = top_m;
	}

	double vis_km = rep.get("vis_km", -1.0);
	if (vis_km < 0.0)
	{
		vis_km = UNLIMITED_VIS_KM;
	}
	double qnh = rep.get("qnh_hpa", -1.0);
	if (qnh < 0.0)
	{
		qnh = 1013.25;
	}
	const int64_t obs = (int64_t)(double)rep.get("obs_time", 0);
	String when;
	if (obs > 0)
	{
		const Dictionary t = Time::get_singleton()->get_datetime_dict_from_unix_time(obs);
		char buf[16];
		snprintf(buf, sizeof(buf), "%02d%02d%02dZ", (int)t["day"], (int)t["hour"], (int)t["minute"]);
		when = buf;
	}
	char buf[64];
	String wind_txt;
	if (wdir < 0)
	{
		wind_txt = "VRB";
	}
	else
	{
		snprintf(buf, sizeof(buf), "%03d\xC2\xB0", wdir);
		wind_txt = String::utf8(buf);
	}
	snprintf(buf, sizeof(buf), "/%d kt", wspd);
	wind_txt += buf;
	if (wgst > 0)
	{
		snprintf(buf, sizeof(buf), "G%d", wgst);
		wind_txt += buf;
	}
	String cloud_txt = "sky clear";
	if (!cloud_layers.empty())
	{
		PackedStringArray parts;
		for (const Dictionary &cl : cloud_layers)
		{
			snprintf(buf, sizeof(buf), "%d/8 at %d m", (int)cl["coverage_oktas"], (int)cl["base_m"]);
			parts.push_back(buf);
		}
		cloud_txt = String(", ").join(parts);
	}
	char tail[96];
	snprintf(tail, sizeof(tail), ", vis %.0f km, QNH %.0f, ", vis_km, qnh);
	const String status = String(rep.get("station", "?")) + " " + when + ": wind " + wind_txt + tail + cloud_txt;
	Dictionary out;
	out["layers"] = layers;
	out["visibility_km"] = vis_km;
	out["qnh_hpa"] = qnh;
	out["status"] = status;
	out["station"] = String(rep.get("station", ""));
	out["name"] = String(rep.get("name", ""));
	out["obs_time"] = obs;
	out["when"] = when;
	out["wind_txt"] = wind_txt;
	out["raw"] = String(rep.get("raw", ""));
	return out;
}

} // namespace jsbgodot
