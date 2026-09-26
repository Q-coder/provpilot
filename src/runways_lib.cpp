#include "runways_lib.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "geo.h"

using namespace godot;

namespace jsbgodot
{

static const char *RUNWAYS_REL = "airports/runways.json";

void RunwaysLib::_bind_methods()
{
	ClassDB::bind_static_method("RunwaysLib", D_METHOD("glyph_codes", "ident"), &RunwaysLib::glyph_codes);
	ClassDB::bind_static_method("RunwaysLib", D_METHOD("read_pack", "pack_path"), &RunwaysLib::read_pack);
	ClassDB::bind_static_method("RunwaysLib", D_METHOD("load_merged", "scenery_areas"), &RunwaysLib::load_merged);
	ClassDB::bind_static_method("RunwaysLib", D_METHOD("catalog_runways", "ap"), &RunwaysLib::catalog_runways);
	ClassDB::bind_static_method("RunwaysLib", D_METHOD("merge_into_catalog", "catalog", "merged"), &RunwaysLib::merge_into_catalog);
}

RunwaysLib::RunwaysLib()
{
}

PackedInt32Array RunwaysLib::glyph_codes(const String &ident)
{
	PackedInt32Array out;
	out.push_back(-1);
	out.push_back(-1);
	out.push_back(-1);
	int i = 0;
	const String up = ident.to_upper();
	for (int k = 0; k < up.length() && i < 3; k++)
	{
		const char32_t ch = up[k];
		if (ch >= '0' && ch <= '9')
		{
			out[i] = (int)(ch - '0');
		}
		else if (ch == 'L')
		{
			out[i] = 10;
		}
		else if (ch == 'R')
		{
			out[i] = 11;
		}
		else if (ch == 'C')
		{
			out[i] = 12;
		}
		else
		{
			continue;
		}
		i++;
	}
	return out;
}

Dictionary RunwaysLib::read_pack(const String &pack_path)
{
	const String p = pack_path.path_join(RUNWAYS_REL);
	Ref<FileAccess> f = FileAccess::open(p, FileAccess::READ);
	if (f.is_null())
	{
		return Dictionary();
	}
	const Variant parsed = JSON::parse_string(f->get_as_text());
	f->close();
	if (parsed.get_type() == Variant::DICTIONARY && Dictionary(parsed).has("airports"))
	{
		return parsed;
	}
	UtilityFunctions::push_warning("RunwaysLib: ", p, " is not a runways.json (schema_version/airports missing)");
	return Dictionary();
}

Dictionary RunwaysLib::load_merged(const Array &scenery_areas)
{
	Dictionary merged;
	std::vector<Dictionary> areas;
	for (int i = 0; i < scenery_areas.size(); i++)
	{
		if (scenery_areas[i].get_type() == Variant::DICTIONARY && (bool)Dictionary(scenery_areas[i]).get("enabled", true))
		{
			areas.push_back(scenery_areas[i]);
		}
	}
	std::stable_sort(areas.begin(), areas.end(), [](const Dictionary &a, const Dictionary &b) {
		return (int)a.get("priority", 999) < (int)b.get("priority", 999);
	});
	for (const Dictionary &a : areas)
	{
		const String path = a.get("path", "");
		if (path.is_empty())
		{
			continue;
		}
		const Dictionary data = read_pack(path);
		if (data.is_empty())
		{
			continue;
		}
		const Array aps = data.get("airports", Array());
		for (int i = 0; i < aps.size(); i++)
		{
			if (aps[i].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary ap = aps[i];
			const String icao = ap.get("icao", "");
			if (icao.is_empty() || merged.has(icao))
			{
				continue; // a higher-priority pack already defines it
			}
			Dictionary entry = ap.duplicate(true);
			entry["pack_path"] = path;
			entry["pack_region"] = data.get("region", "");
			merged[icao] = entry;
		}
	}
	return merged;
}

double RunwaysLib::initial_bearing(double lat1, double lon1, double lat2, double lon2)
{
	const double p1 = Math::deg_to_rad(lat1), p2 = Math::deg_to_rad(lat2);
	const double dl = Math::deg_to_rad(lon2 - lon1);
	const double y = std::sin(dl) * std::cos(p2);
	const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
	return Math::fposmod(Math::rad_to_deg(std::atan2(y, x)), 360.0);
}

Array RunwaysLib::catalog_runways(const Dictionary &ap)
{
	Array out;
	const Array rws = ap.get("runways", Array());
	for (int i = 0; i < rws.size(); i++)
	{
		if (rws[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary rw = rws[i];
		const Array ends = rw.get("ends", Array());
		const Array desigs = rw.get("designators", Array());
		if (ends.size() != 2 || desigs.size() != 2)
		{
			continue;
		}
		const Dictionary e0 = ends[0], e1 = ends[1];
		if (!(e0.has("pavement") && e1.has("pavement")))
		{
			continue;
		}
		const Array pav0 = e0["pavement"], pav1 = e1["pavement"];
		const Vector2 p0 = Geo::wgs84_to_lv95(pav0[0], pav0[1]);
		const Vector2 p1 = Geo::wgs84_to_lv95(pav1[0], pav1[1]);
		const Vector2 axis = p1 - p0;
		const double length = axis.length();
		if (length < 1.0)
		{
			continue;
		}
		const Vector2 u = axis / length;
		const double d0 = e0.get("displaced_m", 0.0);
		const double d1 = e1.get("displaced_m", 0.0);
		const Vector2 t0 = p0 + u * d0;
		const Vector2 t1 = p1 - u * d1;
		// True heading of designators[0]: the OurAirports value, else the
		// initial bearing between the ends.
		const double hdg = e0.has("heading_deg") ? (double)e0["heading_deg"]
				: initial_bearing(pav0[0], pav0[1], pav1[0], pav1[1]);
		const Vector2 ll = Geo::lv95_to_wgs84((p0.x + p1.x) * 0.5, (p0.y + p1.y) * 0.5);
		const double conv = Geo::lv95_grid_convergence_deg(ll.x, ll.y);
		Dictionary rec;
		rec["designator"] = String(desigs[0]) + "/" + String(desigs[1]);
		rec["heading_deg"] = hdg;
		rec["grid_heading_deg"] = Math::fposmod(hdg + conv, 360.0);
		rec["length_m"] = (double)rw.get("length_m", length);
		rec["width_m"] = (double)rw.get("width_m", 45.0);
		rec["surface"] = String(rw.get("surface", "asphalt"));
		rec["geometry_source"] = String(rw.get("geometry_source", ""));
		Dictionary th0, th1;
		th0["e"] = t0.x;
		th0["n"] = t0.y;
		th1["e"] = t1.x;
		th1["n"] = t1.y;
		rec[String("threshold_") + String(desigs[0])] = th0;
		rec[String("threshold_") + String(desigs[1])] = th1;
		out.push_back(rec);
	}
	return out;
}

TypedArray<Dictionary> RunwaysLib::merge_into_catalog(const TypedArray<Dictionary> &p_catalog, const Dictionary &merged)
{
	TypedArray<Dictionary> catalog = p_catalog;
	Dictionary seen;
	for (int i = 0; i < catalog.size(); i++)
	{
		Dictionary ap = catalog[i];
		const String icao = ap.get("icao", "");
		seen[icao] = true;
		if (merged.has(icao))
		{
			const Dictionary m = merged[icao];
			ap["runways"] = catalog_runways(m);
			ap["runways_source"] = String("pack:") + String(m.get("pack_region", ""));
		}
	}
	const Array keys = merged.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const String icao = keys[i];
		if (seen.has(icao))
		{
			continue;
		}
		const Dictionary m = merged[icao];
		const Vector2 lv = Geo::wgs84_to_lv95(m.get("lat", 0.0), m.get("lon", 0.0));
		const Vector2 ll = Geo::lv95_to_wgs84(lv.x, lv.y);
		Dictionary ap;
		ap["icao"] = icao;
		ap["name"] = String(m.get("name", ""));
		ap["type"] = String(m.get("type", "regional_airport"));
		ap["region"] = String(m.get("pack_region", ""));
		ap["elevation_m"] = (int)(double)m.get("elevation_m", 0);
		ap["center_e"] = lv.x;
		ap["center_n"] = lv.y;
		ap["grid_convergence_deg"] = Geo::lv95_grid_convergence_deg(ll.x, ll.y);
		ap["runways"] = catalog_runways(m);
		ap["runways_source"] = String("pack:") + String(m.get("pack_region", ""));
		catalog.push_back(ap);
	}
	return catalog;
}

} // namespace jsbgodot
