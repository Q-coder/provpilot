#include "airports_lib.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/reg_ex.hpp>
#include <godot_cpp/classes/reg_ex_match.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

#include "geo.h"

using namespace godot;

namespace jsbgodot
{

static const char *CATALOG_PATH = "res://airports.json";

// region id -> on-disk folder name inside any scenery root (fallback for
// scenery not installed through the in-app downloader)
static const struct { const char *id; const char *dir; } REGION_DIRS[] = {
	{"switzerland", "Switzerland"},
	{"baden-wuerttemberg", "Baden-Wuertemberg"},
	{"norway", "Norway"},
	{"sweden", "Sweden"},
	{"finland", "Finland"},
	{"denmark", "Denmark"},
	{"germany", "Germany"},
	{"switzerland-base", "Switzerland"},
	{"austria", "Austria"},
	{"italy", "Italy"},
	{"france", "France"},
};

void AirportsLib::_bind_methods()
{
	ClassDB::bind_static_method("AirportsLib", D_METHOD("load_default"), &AirportsLib::load_default);
	ClassDB::bind_static_method("AirportsLib", D_METHOD("resolve", "raw"), &AirportsLib::resolve);
	ClassDB::bind_static_method("AirportsLib", D_METHOD("wgs84_to_lv95", "lat_deg", "lon_deg"), &AirportsLib::wgs84_to_lv95);
	ClassDB::bind_static_method("AirportsLib", D_METHOD("installed_regions", "installed_packs"), &AirportsLib::installed_regions);
	ClassDB::bind_static_method("AirportsLib", D_METHOD("installed_regions_from_areas", "scenery_areas"), &AirportsLib::installed_regions_from_areas);
	ClassDB::bind_static_method("AirportsLib", D_METHOD("manifest_region", "area_path"), &AirportsLib::manifest_region);
	ClassDB::bind_static_method("AirportsLib", D_METHOD("availability_marker", "airport", "installed_regions_set"), &AirportsLib::availability_marker);
}

AirportsLib::AirportsLib()
{
}

Variant AirportsLib::read_json(const String &path)
{
	// Godot imports .json as a JSON resource whose .data holds the parsed
	// value; fall back to direct parsing if the importer ever changes.
	Ref<Resource> res = ResourceLoader::get_singleton()->load(path);
	Ref<JSON> json = res;
	if (json.is_valid())
	{
		return json->get_data();
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null())
	{
		return Variant();
	}
	return JSON::parse_string(f->get_as_text());
}

TypedArray<Dictionary> AirportsLib::load_default()
{
	TypedArray<Dictionary> out;
	const Variant data = read_json(CATALOG_PATH);
	if (data.get_type() == Variant::NIL)
	{
		UtilityFunctions::push_warning("AirportsLib: failed to read ", CATALOG_PATH);
		return out;
	}
	Array entries;
	if (data.get_type() == Variant::DICTIONARY)
	{
		entries = Dictionary(data).get("airports", Array());
	}
	for (int i = 0; i < entries.size(); i++)
	{
		if (entries[i].get_type() == Variant::DICTIONARY)
		{
			out.push_back(resolve(entries[i]));
		}
	}
	return out;
}

Vector2 AirportsLib::wgs84_to_lv95(double lat_deg, double lon_deg)
{
	return Geo::wgs84_to_lv95(lat_deg, lon_deg);
}

Dictionary AirportsLib::resolve(const Dictionary &raw)
{
	Dictionary ap;
	ap["icao"] = raw.get("icao", "----");
	ap["name"] = raw.get("name", "");
	ap["type"] = raw.get("type", "small_airport");
	ap["region"] = raw.get("region", "");
	ap["elevation_m"] = (int)(double)raw.get("elevation_m", 0);
	if (raw.has("quadrant"))
	{
		ap["quadrant"] = raw["quadrant"];
	}
	// Centre: an explicit LV95 override wins, else convert from lat/lon.
	double center_e, center_n;
	if (raw.has("swiss_e") && raw.has("swiss_n"))
	{
		center_e = raw["swiss_e"];
		center_n = raw["swiss_n"];
	}
	else
	{
		const Vector2 lv = Geo::wgs84_to_lv95(raw.get("lat", 0.0), raw.get("lon", 0.0));
		center_e = lv.x;
		center_n = lv.y;
	}
	ap["center_e"] = center_e;
	ap["center_n"] = center_n;
	// Grid convergence at the airport: true bearings from the catalog become
	// LV95 grid bearings for everything placed in the flat scenery frame.
	const Vector2 ll = Geo::lv95_to_wgs84(center_e, center_n);
	const double convergence = Geo::lv95_grid_convergence_deg(ll.x, ll.y);
	ap["grid_convergence_deg"] = convergence;
	// Runway thresholds from heading + length (true bearing of the
	// lower-numbered designator, rotated into the grid frame).
	Array runways_out;
	const Array runways = raw.get("runways", Array());
	for (int i = 0; i < runways.size(); i++)
	{
		if (runways[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary r = runways[i];
		const String desig = r.get("designator", "");
		const double heading = r.get("heading_deg", 0.0);
		const double grid_heading = Math::fposmod(heading + convergence, 360.0);
		const double length = r.get("length_m", 0.0);
		Dictionary rwy;
		rwy["designator"] = desig;
		rwy["heading_deg"] = heading;
		rwy["grid_heading_deg"] = grid_heading;
		rwy["length_m"] = length;
		rwy["surface"] = r.get("surface", "asphalt");
		const PackedStringArray parts = desig.split("/");
		// "thresholds": [[lat, lon], [lat, lon]] of the two designators
		// (scripts/sync_catalog_runways.py, from the region's surveyed
		// runway ends); without them the runway is centred on the airport's
		// reference point, which is wrong for every airport with more than
		// one runway.
		const Array ths = r.get("thresholds", Array());
		if (parts.size() >= 2 && ths.size() == 2 && ths[0].get_type() == Variant::ARRAY && ths[1].get_type() == Variant::ARRAY
				&& Array(ths[0]).size() >= 2 && Array(ths[1]).size() >= 2)
		{
			const Array a0 = ths[0], a1 = ths[1];
			const Vector2 p0 = Geo::wgs84_to_lv95(a0[0], a0[1]);
			const Vector2 p1 = Geo::wgs84_to_lv95(a1[0], a1[1]);
			Dictionary t0, t1;
			t0["e"] = p0.x;
			t0["n"] = p0.y;
			t1["e"] = p1.x;
			t1["n"] = p1.y;
			rwy[String("threshold_") + parts[0]] = t0;
			rwy[String("threshold_") + parts[1]] = t1;
		}
		else if (parts.size() >= 2 && length > 0.0)
		{
			const double hdg_rad = Math::deg_to_rad(grid_heading);
			const double half = length * 0.5;
			const double dx_e = std::sin(hdg_rad) * half;
			const double dy_n = std::cos(hdg_rad) * half;
			Dictionary t0, t1;
			t0["e"] = center_e - dx_e;
			t0["n"] = center_n - dy_n;
			t1["e"] = center_e + dx_e;
			t1["n"] = center_n + dy_n;
			rwy[String("threshold_") + parts[0]] = t0;
			rwy[String("threshold_") + parts[1]] = t1;
		}
		runways_out.push_back(rwy);
	}
	ap["runways"] = runways_out;
	return ap;
}

Dictionary AirportsLib::installed_regions(const Dictionary &installed_packs)
{
	Dictionary regions;
	const Array keys = installed_packs.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const String pid = keys[i];
		const int idx = pid.find("-base-");
		if (idx > 0)
		{
			regions[pid.substr(0, idx)] = true;
		}
	}
	return regions;
}

Dictionary AirportsLib::installed_regions_from_areas(const Array &scenery_areas)
{
	Dictionary regions;
	for (int i = 0; i < scenery_areas.size(); i++)
	{
		if (scenery_areas[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const String path = Dictionary(scenery_areas[i]).get("path", "");
		if (path.is_empty())
		{
			continue;
		}
		// Base-tier packs name their region in terrain/manifest.json.
		const String tagged = manifest_region(path);
		if (!tagged.is_empty())
		{
			regions[tagged] = true;
			continue;
		}
		// Untagged packs: only the last path component counts, so
		// "Germany/Baden-Wuertemberg" is the BW add-on, not the Germany base.
		const String leaf = path.rstrip("/").get_file();
		for (const auto &rd : REGION_DIRS)
		{
			if (leaf == rd.dir)
			{
				regions[String(rd.id)] = true;
			}
		}
	}
	return regions;
}

String AirportsLib::manifest_region(const String &area_path)
{
	Ref<FileAccess> f = FileAccess::open(area_path.path_join("terrain/manifest.json"), FileAccess::READ);
	if (f.is_null())
	{
		return String();
	}
	// Only the first 64 KB: the Fennoscandia manifests weigh hundreds of MB
	// and the scalars sit at the top.
	const String head = f->get_buffer(65536).get_string_from_utf8();
	f->close();
	Ref<RegEx> re = memnew(RegEx);
	re->compile("\"region\"\\s*:\\s*\"([^\"]+)\"");
	Ref<RegExMatch> m = re->search(head);
	return m.is_valid() ? m->get_string(1) : String();
}

String AirportsLib::availability_marker(const Dictionary &airport, const Dictionary &installed_regions_set)
{
	const String region = airport.get("region", "");
	if (region.is_empty())
	{
		return String();
	}
	return installed_regions_set.has(region) ? String::utf8("\xE2\x9C\x93") : String::utf8("\xE2\x9C\x97");
}

} // namespace jsbgodot
