#include "navaids_lib.h"

#include "airports_lib.h"
#include "geo.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace jsbgodot
{

static const char *CATALOG_PATH = "res://navaids.json";
static const char *ILS_PATH = "res://ils.json";
static const double NM_M = 1852.0;

std::vector<Navaid> NavaidsLib::catalog;
bool NavaidsLib::loaded = false;
std::vector<Ils> NavaidsLib::ils_list;
bool NavaidsLib::ils_loaded = false;
int NavaidsLib::ils_gen = 0;
std::vector<Dictionary> NavaidsLib::airports;
bool NavaidsLib::airports_set = false;

void NavaidsLib::_bind_methods()
{
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("load_from", "path"), &NavaidsLib::load_from);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("count"), &NavaidsLib::count);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("kind_of", "type"), &NavaidsLib::kind_of);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("find_frequency", "freq_khz", "kind_mask"), &NavaidsLib::find_frequency);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("nearby", "lat_deg", "lon_deg", "radius_nm", "kind_mask"), &NavaidsLib::nearby);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("nearest", "lat_deg", "lon_deg", "kind_mask"), &NavaidsLib::nearest);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("distance_m", "lat1_deg", "lon1_deg", "lat2_deg", "lon2_deg"), &NavaidsLib::distance_m);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("bearing_deg", "lat1_deg", "lon1_deg", "lat2_deg", "lon2_deg"), &NavaidsLib::bearing_deg);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("angle_diff_deg", "a", "b"), &NavaidsLib::angle_diff_deg);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("set_airports", "airports"), &NavaidsLib::set_airports);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("find_ils", "freq_khz"), &NavaidsLib::find_ils);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("ils_count"), &NavaidsLib::ils_count);
	ClassDB::bind_static_method("NavaidsLib", D_METHOD("is_ils_channel", "freq_khz"), &NavaidsLib::is_ils_channel);
	ClassDB::bind_integer_constant(get_class_static(), "", "KIND_VOR", KIND_VOR);
	ClassDB::bind_integer_constant(get_class_static(), "", "KIND_DME", KIND_DME);
	ClassDB::bind_integer_constant(get_class_static(), "", "KIND_NDB", KIND_NDB);
	ClassDB::bind_integer_constant(get_class_static(), "", "KIND_TACAN", KIND_TACAN);
}

NavaidsLib::NavaidsLib()
{
}

int NavaidsLib::kind_of(const String &type)
{
	if (type == "VOR")
	{
		return KIND_VOR;
	}
	if (type == "VOR-DME")
	{
		return KIND_VOR | KIND_DME;
	}
	if (type == "VORTAC")
	{
		return KIND_VOR | KIND_DME | KIND_TACAN;
	}
	if (type == "DME")
	{
		return KIND_DME;
	}
	if (type == "TACAN")
	{
		return KIND_DME | KIND_TACAN;
	}
	if (type == "NDB")
	{
		return KIND_NDB;
	}
	if (type == "NDB-DME")
	{
		return KIND_NDB | KIND_DME;
	}
	return 0;
}

double NavaidsLib::nominal_range_nm(const Navaid &n)
{
	const bool ndb = (n.kind & KIND_NDB) != 0;
	if (n.power == "HIGH")
	{
		return ndb ? 75.0 : 130.0;
	}
	if (n.power == "MEDIUM")
	{
		return ndb ? 50.0 : 40.0;
	}
	if (n.power == "LOW")
	{
		return 25.0;
	}
	if (n.usage == "TERMINAL")
	{
		return 25.0;
	}
	if (n.usage == "HI")
	{
		return ndb ? 75.0 : 130.0;
	}
	return 40.0;
}

bool NavaidsLib::parse(const Variant &data)
{
	if (data.get_type() != Variant::DICTIONARY)
	{
		return false;
	}
	const Array list = Dictionary(data).get("navaids", Array());
	catalog.clear();
	catalog.reserve(list.size());
	for (int i = 0; i < list.size(); i++)
	{
		if (list[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary d = list[i];
		Navaid n;
		n.ident = String(d.get("ident", ""));
		n.name = String(d.get("name", ""));
		n.type = String(d.get("type", ""));
		n.kind = kind_of(n.type);
		n.freq_khz = (int)(double)d.get("freq_khz", 0);
		n.lat = (double)d.get("lat", 0.0);
		n.lon = (double)d.get("lon", 0.0);
		const Variant el = d.get("elev_m", Variant());
		n.elev_m = el.get_type() == Variant::NIL ? 0.0 : (double)el;
		const Variant dme = d.get("dme_khz", Variant());
		n.dme_khz = dme.get_type() == Variant::NIL ? 0 : (int)(double)dme;
		const Variant sv = d.get("station_var_deg", Variant());
		n.has_station_var = sv.get_type() != Variant::NIL;
		n.station_var_deg = n.has_station_var ? (double)sv : 0.0;
		const Variant pw = d.get("power", Variant());
		n.power = pw.get_type() == Variant::STRING ? String(pw) : String();
		const Variant us = d.get("usage", Variant());
		n.usage = us.get_type() == Variant::STRING ? String(us) : String();
		const Variant ap = d.get("airport", Variant());
		n.airport = ap.get_type() == Variant::STRING ? String(ap) : String();
		n.region = String(d.get("region", ""));
		if (n.kind != 0 && n.freq_khz > 0)
		{
			catalog.push_back(n);
		}
	}
	return true;
}

Variant NavaidsLib::read_json(const String &path)
{
	// Godot imports .json as a JSON resource whose .data holds the parsed
	// value; fall back to direct parsing for files outside res://.
	Ref<Resource> res = ResourceLoader::get_singleton()->load(path);
	Ref<JSON> json = res;
	if (json.is_valid())
	{
		return json->get_data();
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null())
	{
		UtilityFunctions::push_warning("NavaidsLib: cannot read ", path);
		return Variant();
	}
	return JSON::parse_string(f->get_as_text());
}

bool NavaidsLib::load_from(const String &path)
{
	const Variant data = read_json(path);
	if (data.get_type() == Variant::NIL)
	{
		return false;
	}
	const bool ok = parse(data);
	if (!ok)
	{
		UtilityFunctions::push_warning("NavaidsLib: no navaid list in ", path);
	}
	loaded = true;
	return ok;
}

void NavaidsLib::ensure_loaded()
{
	if (!loaded)
	{
		load_from(CATALOG_PATH);
	}
}

const std::vector<Navaid> &NavaidsLib::all()
{
	ensure_loaded();
	return catalog;
}

int NavaidsLib::count()
{
	return (int)all().size();
}

Dictionary NavaidsLib::to_dict(const Navaid &n)
{
	Dictionary d;
	d["ident"] = n.ident;
	d["name"] = n.name;
	d["type"] = n.type;
	d["kind"] = n.kind;
	d["freq_khz"] = n.freq_khz;
	d["dme_khz"] = n.dme_khz;
	d["lat"] = n.lat;
	d["lon"] = n.lon;
	d["elev_m"] = n.elev_m;
	d["station_var_deg"] = n.station_var_deg;
	d["has_station_var"] = n.has_station_var;
	d["power"] = n.power;
	d["usage"] = n.usage;
	d["airport"] = n.airport;
	d["region"] = n.region;
	d["range_nm"] = nominal_range_nm(n);
	return d;
}

std::vector<const Navaid *> NavaidsLib::on_frequency(int freq_khz, int kind_mask)
{
	std::vector<const Navaid *> out;
	for (const Navaid &n : all())
	{
		if ((n.kind & kind_mask) == 0)
		{
			continue;
		}
		// a DME paired with a VOR answers on the VOR's frequency; a plain
		// DME / TACAN lists the paired VHF frequency as its own
		if (n.freq_khz == freq_khz || ((kind_mask & KIND_DME) && n.dme_khz == freq_khz && (n.kind & KIND_DME)))
		{
			out.push_back(&n);
		}
	}
	return out;
}

TypedArray<Dictionary> NavaidsLib::find_frequency(int freq_khz, int kind_mask)
{
	TypedArray<Dictionary> out;
	for (const Navaid *n : on_frequency(freq_khz, kind_mask))
	{
		out.push_back(to_dict(*n));
	}
	return out;
}

TypedArray<Dictionary> NavaidsLib::nearby(double lat_deg, double lon_deg, double radius_nm, int kind_mask)
{
	struct Hit
	{
		const Navaid *n;
		double d;
	};
	std::vector<Hit> hits;
	const double rad_m = radius_nm * NM_M;
	for (const Navaid &n : all())
	{
		if ((n.kind & kind_mask) == 0)
		{
			continue;
		}
		// cheap latitude pre-filter (1 deg ~ 60 nm)
		if (std::fabs(n.lat - lat_deg) * 111000.0 > rad_m)
		{
			continue;
		}
		const double d = distance_m(lat_deg, lon_deg, n.lat, n.lon);
		if (d <= rad_m)
		{
			hits.push_back({&n, d});
		}
	}
	std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.d < b.d; });
	TypedArray<Dictionary> out;
	for (const Hit &h : hits)
	{
		Dictionary d = to_dict(*h.n);
		d["distance_nm"] = h.d / NM_M;
		d["bearing_deg"] = bearing_deg(lat_deg, lon_deg, h.n->lat, h.n->lon);
		out.push_back(d);
	}
	return out;
}

Dictionary NavaidsLib::nearest(double lat_deg, double lon_deg, int kind_mask)
{
	const Navaid *best = nullptr;
	double best_d = 0.0;
	for (const Navaid &n : all())
	{
		if ((n.kind & kind_mask) == 0)
		{
			continue;
		}
		const double d = distance_m(lat_deg, lon_deg, n.lat, n.lon);
		if (best == nullptr || d < best_d)
		{
			best = &n;
			best_d = d;
		}
	}
	if (best == nullptr)
	{
		return Dictionary();
	}
	Dictionary d = to_dict(*best);
	d["distance_nm"] = best_d / NM_M;
	d["bearing_deg"] = bearing_deg(lat_deg, lon_deg, best->lat, best->lon);
	return d;
}

// ── ILS ─────────────────────────────────────────────────────────────────────

bool NavaidsLib::is_ils_channel(int freq_khz)
{
	// 108.10 .. 111.95 MHz, odd tenths and their + 50 kHz
	if (freq_khz < 108100 || freq_khz > 111950)
	{
		return false;
	}
	return ((freq_khz / 100) % 2) == 1;
}

void NavaidsLib::ensure_ils_loaded()
{
	if (ils_loaded)
	{
		return;
	}
	ils_loaded = true;
	const Variant data = read_json(ILS_PATH);
	if (data.get_type() != Variant::DICTIONARY)
	{
		return;
	}
	const Array list = Dictionary(data).get("ils", Array());
	ils_list.clear();
	ils_list.reserve(list.size());
	for (int i = 0; i < list.size(); i++)
	{
		if (list[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary d = list[i];
		Ils s;
		s.icao = String(d.get("icao", ""));
		s.designator = String(d.get("designator", ""));
		s.rwy = String(d.get("rwy", ""));
		s.ident = String(d.get("ident", ""));
		s.cat = String(d.get("cat", ""));
		s.source = String(d.get("source", ""));
		s.synthetic = s.source == "synthetic";
		s.freq_khz = (int)(double)d.get("freq_khz", 0);
		const Variant gs = d.get("gs_deg", Variant());
		s.gs_deg = gs.get_type() == Variant::NIL ? 3.0 : (double)gs;
		if (s.gs_deg < 1.0 || s.gs_deg > 6.0)
		{
			s.gs_deg = 3.0;
		}
		s.offset = (bool)d.get("offset", false);
		s.ref_course_deg = (double)d.get("course_true_deg", 0.0);
		const Variant loc = d.get("loc", Variant());
		if (loc.get_type() == Variant::DICTIONARY)
		{
			const Dictionary l = loc;
			s.has_ref_loc = true;
			s.ref_loc_lat = (double)l.get("lat", 0.0);
			s.ref_loc_lon = (double)l.get("lon", 0.0);
			s.ref_loc_elev_m = (double)l.get("elev_m", 0.0);
		}
		// every real ILS without a nav.dat DME row still gets one (the
		// receiver needs a distance to show; most European ILS have a DME)
		s.has_dme = true;
		if (s.freq_khz > 0 && !s.icao.is_empty())
		{
			ils_list.push_back(s);
		}
	}
	resolve_ils();
}

void NavaidsLib::set_airports(const TypedArray<Dictionary> &p_airports)
{
	airports.clear();
	for (int i = 0; i < p_airports.size(); i++)
	{
		airports.push_back(p_airports[i]);
	}
	airports_set = true;
	ensure_ils_loaded();
	resolve_ils();
}

static bool runway_end_index(const Dictionary &rwy, const String &end, int &idx, PackedStringArray &ends)
{
	ends = String(rwy.get("designator", "")).split("/");
	for (int i = 0; i < ends.size(); i++)
	{
		if (ends[i].strip_edges() == end)
		{
			idx = i;
			return true;
		}
	}
	// tolerate a missing / extra L, R, C
	auto strip = [](const String &s) {
		String t = s.strip_edges();
		if (t.ends_with("L") || t.ends_with("R") || t.ends_with("C"))
		{
			t = t.substr(0, t.length() - 1);
		}
		return t;
	};
	for (int i = 0; i < ends.size(); i++)
	{
		if (strip(ends[i]) == strip(end))
		{
			idx = i;
			return true;
		}
	}
	return false;
}

// Geometry from the runway the aircraft spawns on: thresholds (LV95) ->
// WGS84; localizer antenna 300 m beyond the far end on the centreline,
// glide slope / DME 300 m past the landing threshold; course = bearing
// threshold -> far end; sector half width such that full scale is 107 m
// at the threshold (ICAO Annex 10), kept within 1.5 .. 3 deg.
void NavaidsLib::resolve_ils()
{
	std::vector<Dictionary> list = airports;
	if (!airports_set)
	{
		const TypedArray<Dictionary> def = AirportsLib::load_default();
		for (int i = 0; i < def.size(); i++)
		{
			list.push_back(def[i]);
		}
	}
	Dictionary by_icao;
	for (const Dictionary &a : list)
	{
		by_icao[String(a.get("icao", ""))] = a;
	}
	int n_ok = 0;
	for (Ils &s : ils_list)
	{
		s.resolved = false;
		if (!by_icao.has(s.icao))
		{
			continue;
		}
		const Dictionary ap = by_icao[s.icao];
		const Array rws = ap.get("runways", Array());
		Dictionary rwy;
		int idx = -1;
		PackedStringArray ends;
		for (int r = 0; r < rws.size(); r++)
		{
			if (rws[r].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary cand = rws[r];
			if (runway_end_index(cand, s.rwy, idx, ends) && ends.size() >= 2)
			{
				rwy = cand;
				break;
			}
		}
		if (idx < 0)
		{
			continue;
		}
		const String thr_key = String("threshold_") + ends[idx].strip_edges();
		const String far_key = String("threshold_") + ends[1 - idx].strip_edges();
		if (!rwy.has(thr_key) || !rwy.has(far_key))
		{
			continue;
		}
		const Dictionary t = rwy[thr_key], f = rwy[far_key];
		s.thr_e = (double)t.get("e", 0.0);
		s.thr_n = (double)t.get("n", 0.0);
		s.far_e = (double)f.get("e", 0.0);
		s.far_n = (double)f.get("n", 0.0);
		const double dx = s.far_e - s.thr_e, dy = s.far_n - s.thr_n;
		s.length_m = std::sqrt(dx * dx + dy * dy);
		if (s.length_m < 100.0)
		{
			continue;
		}
		const double ux = dx / s.length_m, uy = dy / s.length_m;
		const Vector2 thr_ll = Geo::lv95_to_wgs84(s.thr_e, s.thr_n);
		const Vector2 far_ll = Geo::lv95_to_wgs84(s.far_e, s.far_n);
		s.thr_lat = thr_ll.x;
		s.thr_lon = thr_ll.y;
		s.far_lat = far_ll.x;
		s.far_lon = far_ll.y;
		s.thr_elev_m = (double)ap.get("elevation_m", 0.0);
		const Vector2 loc_ll = Geo::lv95_to_wgs84(s.far_e + ux * 300.0, s.far_n + uy * 300.0);
		const Vector2 gs_ll = Geo::lv95_to_wgs84(s.thr_e + ux * 300.0, s.thr_n + uy * 300.0);
		s.loc_lat = loc_ll.x;
		s.loc_lon = loc_ll.y;
		s.loc_elev_m = s.thr_elev_m;
		s.gs_lat = gs_ll.x;
		s.gs_lon = gs_ll.y;
		s.gs_elev_m = s.thr_elev_m;
		s.course_deg = bearing_deg(s.thr_lat, s.thr_lon, s.far_lat, s.far_lon);
		if (s.offset && s.has_ref_loc)
		{
			s.course_deg = s.ref_course_deg;
			s.loc_lat = s.ref_loc_lat;
			s.loc_lon = s.ref_loc_lon;
			s.loc_elev_m = s.ref_loc_elev_m;
		}
		s.half_width_deg = Math::clamp(Math::rad_to_deg(std::atan(107.0 / (s.length_m + 300.0))), 1.5, 3.0);
		s.resolved = true;
		n_ok++;
	}
	ils_gen++;
	UtilityFunctions::print("NavaidsLib: ", n_ok, " of ", (int)ils_list.size(), " ILS resolved against ", (int)list.size(), " airports");
}

const std::vector<Ils> &NavaidsLib::all_ils()
{
	ensure_ils_loaded();
	return ils_list;
}

int NavaidsLib::ils_generation()
{
	return ils_gen;
}

int NavaidsLib::ils_count()
{
	return (int)all_ils().size();
}

std::vector<const Ils *> NavaidsLib::ils_on_frequency(int freq_khz)
{
	std::vector<const Ils *> out;
	for (const Ils &s : all_ils())
	{
		if (s.resolved && s.freq_khz == freq_khz)
		{
			out.push_back(&s);
		}
	}
	return out;
}

Dictionary NavaidsLib::ils_to_dict(const Ils &s)
{
	Dictionary d;
	d["icao"] = s.icao;
	d["designator"] = s.designator;
	d["rwy"] = s.rwy;
	d["ident"] = s.ident;
	d["cat"] = s.cat;
	d["source"] = s.source;
	d["synthetic"] = s.synthetic;
	d["freq_khz"] = s.freq_khz;
	d["gs_deg"] = s.gs_deg;
	d["offset"] = s.offset;
	d["resolved"] = s.resolved;
	d["course_deg"] = s.course_deg;
	d["half_width_deg"] = s.half_width_deg;
	d["length_m"] = s.length_m;
	d["thr_lat"] = s.thr_lat;
	d["thr_lon"] = s.thr_lon;
	d["thr_elev_m"] = s.thr_elev_m;
	d["far_lat"] = s.far_lat;
	d["far_lon"] = s.far_lon;
	d["loc_lat"] = s.loc_lat;
	d["loc_lon"] = s.loc_lon;
	d["gs_lat"] = s.gs_lat;
	d["gs_lon"] = s.gs_lon;
	d["gs_elev_m"] = s.gs_elev_m;
	return d;
}

TypedArray<Dictionary> NavaidsLib::find_ils(int freq_khz)
{
	TypedArray<Dictionary> out;
	for (const Ils &s : all_ils())
	{
		if (s.freq_khz == freq_khz)
		{
			out.push_back(ils_to_dict(s));
		}
	}
	return out;
}

double NavaidsLib::angle_diff_deg(double a, double b)
{
	double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
	if (d <= -180.0)
	{
		d += 360.0;
	}
	return d;
}

// Vincenty's inverse formula on the WGS84 ellipsoid (T. Vincenty, 1975).
void NavaidsLib::inverse(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg,
		double &dist_m, double &bearing12_deg, double &bearing21_deg)
{
	const double a = 6378137.0, f = 1.0 / 298.257223563, b = a * (1.0 - f);
	const double phi1 = Math::deg_to_rad(lat1_deg), phi2 = Math::deg_to_rad(lat2_deg);
	const double L = Math::deg_to_rad(lon2_deg - lon1_deg);
	const double U1 = std::atan((1.0 - f) * std::tan(phi1)), U2 = std::atan((1.0 - f) * std::tan(phi2));
	const double sinU1 = std::sin(U1), cosU1 = std::cos(U1), sinU2 = std::sin(U2), cosU2 = std::cos(U2);
	double lambda = L, lambda_prev;
	double sin_sigma = 0.0, cos_sigma = 1.0, sigma = 0.0, sin_alpha = 0.0, cos2_alpha = 1.0, cos_2sigma_m = 0.0;
	bool converged = false;
	for (int it = 0; it < 100; it++)
	{
		const double sin_l = std::sin(lambda), cos_l = std::cos(lambda);
		const double t1 = cosU2 * sin_l, t2 = cosU1 * sinU2 - sinU1 * cosU2 * cos_l;
		sin_sigma = std::sqrt(t1 * t1 + t2 * t2);
		if (sin_sigma < 1e-15)
		{
			dist_m = 0.0; // coincident points
			bearing12_deg = 0.0;
			bearing21_deg = 0.0;
			return;
		}
		cos_sigma = sinU1 * sinU2 + cosU1 * cosU2 * cos_l;
		sigma = std::atan2(sin_sigma, cos_sigma);
		sin_alpha = cosU1 * cosU2 * sin_l / sin_sigma;
		cos2_alpha = 1.0 - sin_alpha * sin_alpha;
		cos_2sigma_m = cos2_alpha > 1e-15 ? cos_sigma - 2.0 * sinU1 * sinU2 / cos2_alpha : 0.0; // equatorial line
		const double C = f / 16.0 * cos2_alpha * (4.0 + f * (4.0 - 3.0 * cos2_alpha));
		lambda_prev = lambda;
		lambda = L + (1.0 - C) * f * sin_alpha * (sigma + C * sin_sigma * (cos_2sigma_m + C * cos_sigma * (-1.0 + 2.0 * cos_2sigma_m * cos_2sigma_m)));
		if (std::fabs(lambda - lambda_prev) < 1e-12)
		{
			converged = true;
			break;
		}
	}
	if (!converged)
	{
		// near-antipodal: great circle on the mean sphere
		const double R = 6371008.8;
		const double dphi = phi2 - phi1;
		const double h = std::sin(dphi / 2) * std::sin(dphi / 2) + std::cos(phi1) * std::cos(phi2) * std::sin(L / 2) * std::sin(L / 2);
		dist_m = 2.0 * R * std::asin(std::sqrt(std::fmin(1.0, h)));
		const double y = std::sin(L) * std::cos(phi2);
		const double x = std::cos(phi1) * std::sin(phi2) - std::sin(phi1) * std::cos(phi2) * std::cos(L);
		bearing12_deg = Math::fposmod(Math::rad_to_deg(std::atan2(y, x)), 360.0);
		const double y2 = std::sin(-L) * std::cos(phi1);
		const double x2 = std::cos(phi2) * std::sin(phi1) - std::sin(phi2) * std::cos(phi1) * std::cos(L);
		bearing21_deg = Math::fposmod(Math::rad_to_deg(std::atan2(y2, x2)), 360.0);
		return;
	}
	const double u2 = cos2_alpha * (a * a - b * b) / (b * b);
	const double A = 1.0 + u2 / 16384.0 * (4096.0 + u2 * (-768.0 + u2 * (320.0 - 175.0 * u2)));
	const double B = u2 / 1024.0 * (256.0 + u2 * (-128.0 + u2 * (74.0 - 47.0 * u2)));
	const double delta_sigma = B * sin_sigma * (cos_2sigma_m + B / 4.0 * (cos_sigma * (-1.0 + 2.0 * cos_2sigma_m * cos_2sigma_m) - B / 6.0 * cos_2sigma_m * (-3.0 + 4.0 * sin_sigma * sin_sigma) * (-3.0 + 4.0 * cos_2sigma_m * cos_2sigma_m)));
	dist_m = b * A * (sigma - delta_sigma);
	const double sin_l = std::sin(lambda), cos_l = std::cos(lambda);
	bearing12_deg = Math::fposmod(Math::rad_to_deg(std::atan2(cosU2 * sin_l, cosU1 * sinU2 - sinU1 * cosU2 * cos_l)), 360.0);
	bearing21_deg = Math::fposmod(Math::rad_to_deg(std::atan2(cosU1 * sin_l, -sinU1 * cosU2 + cosU1 * sinU2 * cos_l)) + 180.0, 360.0);
}

double NavaidsLib::distance_m(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg)
{
	double d, b12, b21;
	inverse(lat1_deg, lon1_deg, lat2_deg, lon2_deg, d, b12, b21);
	return d;
}

double NavaidsLib::bearing_deg(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg)
{
	double d, b12, b21;
	inverse(lat1_deg, lon1_deg, lat2_deg, lon2_deg, d, b12, b21);
	return b12;
}

} // namespace jsbgodot
