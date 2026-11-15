#include "flight_log.h"

#include "jsbgodot.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

namespace jsbgodot
{

FlightLog *FlightLog::active = nullptr;

void FlightLog::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_directory", "dir"), &FlightLog::set_directory);
	ClassDB::bind_method(D_METHOD("get_directory"), &FlightLog::get_directory);
	ClassDB::bind_method(D_METHOD("begin", "aircraft_id", "aircraft_name", "lat", "lon", "place"), &FlightLog::begin);
	ClassDB::bind_method(D_METHOD("sample", "jsb", "lat", "lon", "delta"), &FlightLog::sample);
	ClassDB::bind_method(D_METHOD("note_crash", "reason"), &FlightLog::note_crash);
	ClassDB::bind_method(D_METHOD("end", "lat", "lon", "place"), &FlightLog::end);
	ClassDB::bind_method(D_METHOD("is_recording"), &FlightLog::is_recording);
	ClassDB::bind_method(D_METHOD("current_entry"), &FlightLog::current_entry);
	ClassDB::bind_method(D_METHOD("current_samples"), &FlightLog::current_samples);
	ClassDB::bind_static_method("FlightLog", D_METHOD("read_entries", "dir"), &FlightLog::read_entries);
	ClassDB::bind_static_method("FlightLog", D_METHOD("read_samples", "dir", "id"), &FlightLog::read_samples);
}

FlightLog::FlightLog() { active = this; }

FlightLog::~FlightLog()
{
	if (active == this)
	{
		active = nullptr;
	}
}

FlightLog *FlightLog::get_active() { return active; }
void FlightLog::set_directory(const String &dir) { directory = dir; }
String FlightLog::get_directory() const { return directory; }
bool FlightLog::is_recording() const { return recording; }
Array FlightLog::current_samples() const { return samples; }

static double great_circle_m(double lat1, double lon1, double lat2, double lon2)
{
	const double p1 = Math::deg_to_rad(lat1), p2 = Math::deg_to_rad(lat2);
	const double dp = p2 - p1, dl = Math::deg_to_rad(lon2 - lon1);
	const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
	return 6371000.0 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

void FlightLog::begin(const String &aircraft_id, const String &aircraft_name, double lat, double lon, const String &place)
{
	recording = true;
	samples = Array();
	elapsed = air_time = sample_timer = distance_m = 0.0;
	sample_step = SAMPLE_S;
	have_last = false;
	was_ground = true;
	recent_sink = 0.0;
	max_alt = -1e9;
	max_ias = max_gs = max_climb = max_sink = hardest_landing = 0.0;
	max_g = min_g = 1.0;
	landings = takeoffs = 0;
	crash_reason = String();
	start_unix = Time::get_singleton()->get_unix_time_from_system();
	entry = Dictionary();
	entry["id"] = String::num_int64((int64_t)start_unix);
	entry["pilot"] = "Pilot";
	entry["aircraft"] = aircraft_id;
	entry["aircraft_name"] = aircraft_name;
	entry["start_time"] = Time::get_singleton()->get_datetime_string_from_system(false, true);
	entry["from_lat"] = lat;
	entry["from_lon"] = lon;
	entry["from"] = place;
}

void FlightLog::sample(Object *jsb_obj, double lat, double lon, double delta)
{
	JSBGodot *jsb = Object::cast_to<JSBGodot>(jsb_obj);
	if (!recording || jsb == nullptr || !jsb->is_initialized())
	{
		return;
	}
	elapsed += delta;
	const double alt_ft = jsb->get_property("position/h-sl-ft");
	const double ias = jsb->get_property("velocities/vc-kts");
	const double gs = jsb->get_property("velocities/vg-fps") * 0.592484;
	const double vs_fpm = jsb->get_property("velocities/h-dot-fps") * 60.0;
	const double g = jsb->get_property("accelerations/Nz");
	bool ground = false;
	const Array contacts = jsb->get_gear_contacts();
	for (int i = 0; i < contacts.size(); i++)
	{
		ground = ground || bool(Dictionary(contacts[i])["wow"]);
	}
	if (have_last)
	{
		const double d = great_circle_m(last_lat, last_lon, lat, lon);
		if (d < 2000.0)   // a teleport or slew jump is not flown distance
		{
			distance_m += d;
		}
	}
	last_lat = lat;
	last_lon = lon;
	have_last = true;
	if (!ground)
	{
		air_time += delta;
		recent_sink = Math::max(-vs_fpm, recent_sink * std::exp(-delta / 0.2));
		max_g = Math::max(max_g, g);
		min_g = Math::min(min_g, g);
		max_climb = Math::max(max_climb, vs_fpm);
		max_sink = Math::max(max_sink, -vs_fpm);
	}
	if (ground && !was_ground && elapsed > 3.0)
	{
		landings++;
		hardest_landing = Math::max(hardest_landing, recent_sink);
		recent_sink = 0.0;
	}
	if (!ground && was_ground && elapsed > 3.0 && gs > 20.0)
	{
		takeoffs++;
	}
	was_ground = ground;
	max_alt = Math::max(max_alt, alt_ft);
	max_ias = Math::max(max_ias, ias);
	max_gs = Math::max(max_gs, gs);
	sample_timer -= delta;
	if (sample_timer <= 0.0)
	{
		sample_timer = sample_step;
		Array s;
		s.push_back(Math::snapped(elapsed, 0.1));
		s.push_back(Math::snapped(lat, 0.00001));
		s.push_back(Math::snapped(lon, 0.00001));
		s.push_back(std::round(alt_ft));
		s.push_back(std::round(ias));
		s.push_back(std::round(vs_fpm));
		s.push_back(Math::snapped(g, 0.01));
		samples.push_back(s);
		if (samples.size() > MAX_SAMPLES)
		{
			// a long flight keeps its whole track at half the rate
			Array half;
			for (int i = 0; i < samples.size(); i += 2)
			{
				half.push_back(samples[i]);
			}
			samples = half;
			sample_step *= 2.0;
		}
	}
}

void FlightLog::note_crash(const String &reason)
{
	if (recording)
	{
		crash_reason = reason;
	}
}

Dictionary FlightLog::current_entry() const
{
	Dictionary e = entry.duplicate();
	if (!recording)
	{
		return e;
	}
	e["duration_s"] = std::round(elapsed);
	e["air_time_s"] = std::round(air_time);
	e["distance_nm"] = Math::snapped(distance_m / 1852.0, 0.1);
	e["max_alt_ft"] = max_alt > -1e8 ? std::round(max_alt) : 0.0;
	e["max_ias_kt"] = std::round(max_ias);
	e["max_gs_kt"] = std::round(max_gs);
	e["max_g"] = Math::snapped(max_g, 0.01);
	e["min_g"] = Math::snapped(min_g, 0.01);
	e["max_climb_fpm"] = std::round(max_climb);
	e["max_sink_fpm"] = std::round(max_sink);
	e["takeoffs"] = takeoffs;
	e["landings"] = landings;
	e["hardest_landing_fpm"] = std::round(hardest_landing);
	e["result"] = crash_reason.is_empty() ? String("Completed") : String("Crash: ") + crash_reason;
	return e;
}

Dictionary FlightLog::end(double lat, double lon, const String &place)
{
	if (!recording)
	{
		return Dictionary();
	}
	Dictionary e = current_entry();
	recording = false;
	e["end_time"] = Time::get_singleton()->get_datetime_string_from_system(false, true);
	e["to_lat"] = lat;
	e["to_lon"] = lon;
	e["to"] = place;
	if (elapsed < MIN_FLIGHT_S || directory.is_empty())
	{
		return e;   // a flight of a few seconds is not an entry
	}
	Array all = read_entries(directory);   // newest first
	all.push_front(e);
	while (all.size() > MAX_ENTRIES)
	{
		all.pop_back();
	}
	Dictionary doc;
	doc["schema"] = 1;
	doc["pilot"] = "Pilot";
	doc["flights"] = all;
	Ref<FileAccess> f = FileAccess::open(directory.path_join("logbook.json"), FileAccess::WRITE);
	if (f.is_valid())
	{
		f->store_string(JSON::stringify(doc, " "));
	}
	else
	{
		UtilityFunctions::push_warning("FlightLog: cannot write ", directory.path_join("logbook.json"));
	}
	DirAccess::make_dir_recursive_absolute(directory.path_join("logbook"));
	Ref<FileAccess> bf = FileAccess::open(directory.path_join("logbook").path_join(String(e["id"]) + String(".json")), FileAccess::WRITE);
	if (bf.is_valid())
	{
		Dictionary box;
		box["id"] = e["id"];
		box["columns"] = Array::make("t_s", "lat", "lon", "alt_ft", "ias_kt", "vs_fpm", "g");
		box["samples"] = samples;
		bf->store_string(JSON::stringify(box));
	}
	UtilityFunctions::print("Logbook: ", String(e["aircraft_name"]), ", ", String(e["from"]), " - ", String(e["to"]), ", ", String::num(elapsed / 60.0, 1), " min, ", String(e["result"]));
	return e;
}

Array FlightLog::read_entries(const String &dir)
{
	const String path = dir.path_join("logbook.json");
	if (!FileAccess::file_exists(path))
	{
		return Array();
	}
	const Variant v = JSON::parse_string(FileAccess::get_file_as_string(path));
	if (v.get_type() != Variant::DICTIONARY)
	{
		return Array();
	}
	const Variant fl = Dictionary(v).get("flights", Array());
	return fl.get_type() == Variant::ARRAY ? Array(fl) : Array();
}

Array FlightLog::read_samples(const String &dir, const String &id)
{
	const String path = dir.path_join("logbook").path_join(id + String(".json"));
	if (!FileAccess::file_exists(path))
	{
		return Array();
	}
	const Variant v = JSON::parse_string(FileAccess::get_file_as_string(path));
	if (v.get_type() != Variant::DICTIONARY)
	{
		return Array();
	}
	const Variant s = Dictionary(v).get("samples", Array());
	return s.get_type() == Variant::ARRAY ? Array(s) : Array();
}

} // namespace jsbgodot
