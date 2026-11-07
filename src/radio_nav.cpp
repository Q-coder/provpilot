#include "radio_nav.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

#include <cmath>
#include <cstdio>

#include "geo_magnetic.h"

using namespace godot;

namespace jsbgodot
{

void RadioNav::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("update", "delta", "lat_deg", "lon_deg", "alt_m", "true_heading_deg"), &RadioNav::update);
	ClassDB::bind_method(D_METHOD("get_properties"), &RadioNav::get_properties);
	ClassDB::bind_method(D_METHOD("status_text"), &RadioNav::status_text);
	ClassDB::bind_method(D_METHOD("nav_in_range"), &RadioNav::nav_in_range);
	ClassDB::bind_method(D_METHOD("nav_ident"), &RadioNav::nav_ident);
	ClassDB::bind_method(D_METHOD("nav_radial_deg"), &RadioNav::nav_radial_deg);
	ClassDB::bind_method(D_METHOD("nav_bearing_to_deg"), &RadioNav::nav_bearing_to_deg);
	ClassDB::bind_method(D_METHOD("nav_distance_nm"), &RadioNav::nav_distance_nm);
	ClassDB::bind_method(D_METHOD("cdi_deflection"), &RadioNav::cdi_deflection);
	ClassDB::bind_method(D_METHOD("nav_to_flag"), &RadioNav::nav_to_flag);
	ClassDB::bind_method(D_METHOD("nav_from_flag"), &RadioNav::nav_from_flag);
	ClassDB::bind_method(D_METHOD("dme_in_range"), &RadioNav::dme_in_range);
	ClassDB::bind_method(D_METHOD("nav_is_loc"), &RadioNav::nav_is_loc);
	ClassDB::bind_method(D_METHOD("ils_runway"), &RadioNav::ils_runway);
	ClassDB::bind_method(D_METHOD("gs_deflection"), &RadioNav::gs_deflection);
	ClassDB::bind_method(D_METHOD("gs_in_range"), &RadioNav::gs_in_range);
	ClassDB::bind_method(D_METHOD("dme_distance_nm"), &RadioNav::dme_distance_nm);
	ClassDB::bind_method(D_METHOD("dme_ground_speed_kt"), &RadioNav::dme_ground_speed_kt);
	ClassDB::bind_method(D_METHOD("adf_in_range"), &RadioNav::adf_in_range);
	ClassDB::bind_method(D_METHOD("adf_ident"), &RadioNav::adf_ident);
	ClassDB::bind_method(D_METHOD("adf_relative_bearing_deg"), &RadioNav::adf_relative_bearing_deg);
	ClassDB::bind_method(D_METHOD("adf_bearing_true_deg"), &RadioNav::adf_bearing_true_deg);

#define RN_PROP(type, name, getter, setter)                                                    \
	ClassDB::bind_method(D_METHOD(#getter), &RadioNav::getter);                               \
	ClassDB::bind_method(D_METHOD(#setter, "value"), &RadioNav::setter);                      \
	ADD_PROPERTY(PropertyInfo(type, #name), #setter, #getter);
	RN_PROP(Variant::FLOAT, nav_frequency_mhz, get_nav_frequency_mhz, set_nav_frequency_mhz)
	RN_PROP(Variant::FLOAT, obs_deg, get_obs_deg, set_obs_deg)
	RN_PROP(Variant::FLOAT, adf_frequency_khz, get_adf_frequency_khz, set_adf_frequency_khz)
	RN_PROP(Variant::FLOAT, adf_card_deg, get_adf_card_deg, set_adf_card_deg)
	RN_PROP(Variant::BOOL, powered, get_powered, set_powered)
	RN_PROP(Variant::INT, receiver_index, get_receiver_index, set_receiver_index)
#undef RN_PROP
}

RadioNav::RadioNav()
{
}

void RadioNav::set_nav_frequency_mhz(double mhz) { nav_mhz = mhz; }
double RadioNav::get_nav_frequency_mhz() const { return nav_mhz; }
void RadioNav::set_obs_deg(double deg) { obs_deg = deg; }
double RadioNav::get_obs_deg() const { return obs_deg; }
void RadioNav::set_adf_frequency_khz(double khz) { adf_khz = khz; }
double RadioNav::get_adf_frequency_khz() const { return adf_khz; }
void RadioNav::set_adf_card_deg(double deg) { adf_card_deg = deg; }
double RadioNav::get_adf_card_deg() const { return adf_card_deg; }
void RadioNav::set_powered(bool on) { powered = on; }
void RadioNav::set_receiver_index(int index) { receiver_index = std::max(index, 0); }
int RadioNav::get_receiver_index() const { return receiver_index; }
bool RadioNav::get_powered() const { return powered; }

bool RadioNav::nav_in_range() const { return nav_ok; }
String RadioNav::nav_ident() const
{
	if (loc_mode)
	{
		return ils != nullptr && (nav_ok || dme_ok) ? ils->ident : String();
	}
	return nav_station != nullptr && (nav_ok || dme_ok) ? nav_station->ident : String();
}
double RadioNav::nav_radial_deg() const { return nav_radial; }
double RadioNav::nav_bearing_to_deg() const { return nav_bearing_to; }
double RadioNav::nav_distance_nm() const { return nav_dist_m / NM_M; }
double RadioNav::cdi_deflection() const { return cdi; }
bool RadioNav::nav_to_flag() const { return to_flag; }
bool RadioNav::nav_from_flag() const { return from_flag; }
bool RadioNav::dme_in_range() const { return dme_ok; }
bool RadioNav::nav_is_loc() const { return loc_mode; }
String RadioNav::ils_runway() const { return loc_mode && ils != nullptr ? ils->icao + String(" ") + ils->rwy : String(); }
double RadioNav::gs_deflection() const { return gs; }
bool RadioNav::gs_in_range() const { return gs_ok; }
double RadioNav::dme_distance_nm() const { return dme_nm; }
double RadioNav::dme_ground_speed_kt() const { return dme_gs_kt; }
bool RadioNav::adf_in_range() const { return adf_ok; }
String RadioNav::adf_ident() const { return adf_station != nullptr && adf_ok ? adf_station->ident : String(); }
double RadioNav::adf_relative_bearing_deg() const { return adf_needle; }
double RadioNav::adf_bearing_true_deg() const { return adf_bearing; }

double RadioNav::lag(double current, double target, double delta, double tau)
{
	if (delta <= 0.0)
	{
		return target;
	}
	const double k = 1.0 - std::exp(-delta / tau);
	return current + (target - current) * k;
}

// VHF reception: the station's service volume, cut down by the radio
// horizon (1.23 * sqrt(height ft) nm for the aircraft over the station's
// site), never below a few miles of ground reception.
double RadioNav::vhf_range_nm(const Navaid &n, double alt_m)
{
	const double nominal = NavaidsLib::nominal_range_nm(n);
	const double h_ft = Math::max(alt_m - n.elev_m, 0.0) / 0.3048;
	const double horizon = 1.23 * std::sqrt(h_ft) + 1.23 * std::sqrt(30.0); // ~10 m antenna mast
	return Math::min(nominal, Math::max(horizon, 8.0));
}

double RadioNav::station_declination(const Navaid &n) const
{
	if (n.has_station_var)
	{
		return n.station_var_deg;
	}
	if (decimal_year <= 0.0)
	{
		decimal_year = GeoMagnetic::decimal_year_now();
	}
	return GeoMagnetic::declination_deg(n.lat, n.lon, n.elev_m, decimal_year);
}

// The nearest station on the frequency that the aircraft can receive
// (several stations share a frequency across Europe).
const Navaid *RadioNav::pick_station(int khz, int kind_mask, double &dist_out, double &margin_out) const
{
	const Navaid *best = nullptr;
	double best_margin = -1e9;
	dist_out = 0.0;
	margin_out = -1e9;
	for (const Navaid *n : NavaidsLib::on_frequency(khz, kind_mask))
	{
		const double d = NavaidsLib::distance_m(ac_lat, ac_lon, n->lat, n->lon);
		const double range_nm = (n->kind & NavaidsLib::KIND_NDB) ? NavaidsLib::nominal_range_nm(*n) : vhf_range_nm(*n, ac_alt_m);
		// prefer the station the aircraft is deepest inside the range of;
		// out of range everywhere, keep the nearest so the ident shows up
		// as soon as it comes in range
		const double margin = range_nm * NM_M - d;
		if (best == nullptr || margin > best_margin)
		{
			best = n;
			best_margin = margin;
			dist_out = d;
			margin_out = margin;
		}
	}
	return best;
}

// The ILS on the frequency: in range and on the approach side of its
// runway first (both ends of a runway may share the channel), then nearest.
const Ils *RadioNav::pick_ils(int khz, double &margin_out) const
{
	const Ils *best = nullptr;
	double best_score = -1e18;
	margin_out = -1e9;
	for (const Ils *s : NavaidsLib::ils_on_frequency(khz))
	{
		double d, b12, b21;
		NavaidsLib::inverse(ac_lat, ac_lon, s->loc_lat, s->loc_lon, d, b12, b21);
		const double range_m = Math::min(25.0, Math::max(1.23 * std::sqrt(Math::max(ac_alt_m - s->loc_elev_m, 0.0) / 0.3048) + 6.7, 8.0)) * NM_M;
		const double margin = range_m - d;
		const bool front = std::fabs(NavaidsLib::angle_diff_deg(s->course_deg, b21)) > 90.0;
		const double score = (margin > 0.0 ? 1e9 : 0.0) + (front ? 1e8 : 0.0) - d;
		if (best == nullptr || score > best_score)
		{
			best = s;
			best_score = score;
			margin_out = margin;
		}
	}
	return best;
}

void RadioNav::update(double delta, double lat_deg, double lon_deg, double alt_m, double true_heading_deg)
{
	ac_lat = lat_deg;
	ac_lon = lon_deg;
	ac_alt_m = alt_m;
	ac_hdg = Math::fposmod(true_heading_deg, 360.0);
	update_nav(delta);
	update_adf(delta);
}

void RadioNav::update_nav(double delta)
{
	const int khz = (int)std::llround(nav_mhz * 1000.0);
	nav_rescan -= delta;
	if (khz != nav_khz_tuned || nav_rescan <= 0.0 || (nav_station == nullptr && ils == nullptr))
	{
		double d, m_vor, m_ils;
		const Navaid *vor = pick_station(khz, NavaidsLib::KIND_VOR | NavaidsLib::KIND_DME, d, m_vor);
		const Ils *loc = pick_ils(khz, m_ils);
		// an ILS channel is an ILS unless only a VOR / DME is receivable there
		if (loc != nullptr && (vor == nullptr || m_ils >= 0.0 || m_vor < 0.0))
		{
			ils = loc;
			nav_station = nullptr;
		}
		else
		{
			ils = nullptr;
			nav_station = vor;
		}
		nav_khz_tuned = khz;
		nav_rescan = 2.0;
	}
	nav_ok = false;
	dme_ok = false;
	gs_ok = false;
	to_flag = from_flag = false;
	cdi_target = 0.0;
	gs_target = 0.0;
	loc_mode = ils != nullptr;
	if (loc_mode)
	{
		update_ils(delta);
		return;
	}
	gs = lag(gs, 0.0, delta, CDI_TAU_S);
	if (nav_station == nullptr || !powered)
	{
		cdi = lag(cdi, 0.0, delta, CDI_TAU_S);
		dme_prev_nm = -1.0;
		dme_gs_kt = 0.0;
		return;
	}
	const Navaid &st = *nav_station;
	double b12, b21;
	NavaidsLib::inverse(ac_lat, ac_lon, st.lat, st.lon, nav_dist_m, b12, b21);
	nav_bearing_to = b12;
	const double range_m = vhf_range_nm(st, ac_alt_m) * NM_M;
	const bool in_range = nav_dist_m <= range_m;
	nav_signal = in_range ? Math::clamp((range_m - nav_dist_m) / (0.1 * range_m + 1.0), 0.0, 1.0) : 0.0; // 1 within range, fading over the last tenth (the MD-80 flags below 0.99)
	// DME: slant range, ground speed from the range rate
	if ((st.kind & NavaidsLib::KIND_DME) && in_range)
	{
		dme_ok = true;
		const double dh = ac_alt_m - st.elev_m;
		dme_nm = std::sqrt(nav_dist_m * nav_dist_m + dh * dh) / NM_M;
		if (dme_prev_nm >= 0.0 && delta > 0.0)
		{
			const double rate_kt = -(dme_nm - dme_prev_nm) / delta * 3600.0;
			dme_gs_kt = lag(dme_gs_kt, rate_kt, delta, 3.0);
		}
		dme_prev_nm = dme_nm;
	}
	else
	{
		dme_prev_nm = -1.0;
		dme_gs_kt = 0.0;
	}
	// VOR azimuth: not over the station (cone of confusion, ~50 deg half angle)
	const bool over_station = nav_dist_m < 0.85 * Math::max(ac_alt_m - st.elev_m, 0.0);
	if ((st.kind & NavaidsLib::KIND_VOR) && in_range && !over_station)
	{
		nav_ok = true;
		nav_radial = Math::fposmod(b21 - station_declination(st), 360.0);
		// FlightGear navradio: course error r = selected - radial; beyond 90
		// deg the needle shows the inbound course and the TO flag
		double r = NavaidsLib::angle_diff_deg(obs_deg, nav_radial);
		if (std::fabs(r) > 90.0)
		{
			r = r < 0.0 ? -r - 180.0 : -r + 180.0;
			to_flag = true;
		}
		else
		{
			from_flag = true;
		}
		cdi_target = Math::clamp(r, -10.0, 10.0);
	}
	cdi = lag(cdi, cdi_target, delta, CDI_TAU_S);
}

// ILS: the localizer course error with the sector's sensitivity (full scale
// at half_width_deg, 107 m abeam the threshold), TO on the approach side
// and reverse sensing on the back course like a real localizer; the glide
// slope as the elevation angle error from the GS antenna, 0.7 deg full
// scale; the DME at the glide slope site reads about 0.2 nm over the
// threshold.
void RadioNav::update_ils(double delta)
{
	if (!powered || ils == nullptr)
	{
		cdi = lag(cdi, 0.0, delta, CDI_TAU_S);
		gs = lag(gs, 0.0, delta, CDI_TAU_S);
		dme_prev_nm = -1.0;
		dme_gs_kt = 0.0;
		return;
	}
	const Ils &s = *ils;
	double b12, b21;
	NavaidsLib::inverse(ac_lat, ac_lon, s.loc_lat, s.loc_lon, nav_dist_m, b12, b21);
	nav_bearing_to = b12;
	const double h_ft = Math::max(ac_alt_m - s.loc_elev_m, 0.0) / 0.3048;
	const double range_m = Math::min(25.0, Math::max(1.23 * std::sqrt(h_ft) + 6.7, 8.0)) * NM_M;
	const bool in_range = nav_dist_m <= range_m;
	nav_signal = in_range ? Math::clamp((range_m - nav_dist_m) / (0.1 * range_m + 1.0), 0.0, 1.0) : 0.0; // 1 within range, fading over the last tenth (the MD-80 flags below 0.99)
	nav_radial = Math::fposmod(b21, 360.0);
	if (in_range)
	{
		nav_ok = true;
		double r = NavaidsLib::angle_diff_deg(s.course_deg, nav_radial);
		if (std::fabs(r) > 90.0)
		{
			r = r < 0.0 ? -r - 180.0 : -r + 180.0;
			to_flag = true;
		}
		else
		{
			from_flag = true;
		}
		cdi_target = Math::clamp(r / s.half_width_deg * 10.0, -10.0, 10.0);
		// glide slope and DME from the GS site
		double d_gs, g12, g21;
		NavaidsLib::inverse(ac_lat, ac_lon, s.gs_lat, s.gs_lon, d_gs, g12, g21);
		const double dh = ac_alt_m - s.gs_elev_m;
		// glide slope usable to 15 nm: ICAO guarantees 10, real sites reach
		// further and crews intercept at 12-13 (gery, 2026-11-01)
		if (to_flag && d_gs < 15.0 * NM_M && std::fabs(r) < 3.0 * s.half_width_deg && d_gs > 50.0)
		{
			const double eps = Math::rad_to_deg(std::atan2(dh, d_gs));
			gs_ok = true;
			gs_direct = eps;
			gs_path = s.gs_deg;
			gs_target = Math::clamp((s.gs_deg - eps) * 5.0, -3.5, 3.5);
		}
		if (s.has_dme)
		{
			dme_ok = true;
			dme_nm = std::sqrt(d_gs * d_gs + dh * dh) / NM_M;
			if (dme_prev_nm >= 0.0 && delta > 0.0)
			{
				const double rate_kt = -(dme_nm - dme_prev_nm) / delta * 3600.0;
				dme_gs_kt = lag(dme_gs_kt, rate_kt, delta, 3.0);
			}
			dme_prev_nm = dme_nm;
		}
	}
	if (!dme_ok)
	{
		dme_prev_nm = -1.0;
		dme_gs_kt = 0.0;
	}
	cdi = lag(cdi, cdi_target, delta, CDI_TAU_S);
	gs = lag(gs, gs_target, delta, CDI_TAU_S);
}

void RadioNav::update_adf(double delta)
{
	const int khz = (int)std::llround(adf_khz);
	adf_rescan -= delta;
	if (khz != adf_khz_tuned || adf_rescan <= 0.0 || adf_station == nullptr)
	{
		double d;
		double m;
		adf_station = pick_station(khz, NavaidsLib::KIND_NDB, d, m);
		adf_khz_tuned = khz;
		adf_rescan = 2.0;
	}
	adf_ok = false;
	// no signal: the needle parks at 90 deg (ANT mode behaviour)
	adf_rel_target = 90.0;
	if (adf_station != nullptr && powered)
	{
		const Navaid &st = *adf_station;
		double b12, b21;
		NavaidsLib::inverse(ac_lat, ac_lon, st.lat, st.lon, adf_dist_m, b12, b21);
		adf_bearing = b12;
		if (adf_dist_m <= NavaidsLib::nominal_range_nm(st) * NM_M)
		{
			adf_ok = true;
			adf_rel_target = Math::fposmod(b12 - ac_hdg, 360.0);
		}
	}
	// shortest way round the card
	const double err = NavaidsLib::angle_diff_deg(adf_rel_target, adf_needle);
	adf_needle = Math::fposmod(adf_needle + (lag(0.0, err, delta, ADF_TAU_S)), 360.0);
}

void RadioNav::write_properties(Dictionary &p) const
{
	// NAV 1 (or NAV n: instrumentation/nav[n]/...)
	const String N = receiver_index > 0 ? String("instrumentation/nav[") + String::num_int64(receiver_index) + "]/" : String("instrumentation/nav/");
	p[N + String("frequencies/selected-mhz")] = nav_mhz;
	p[N + String("radials/selected-deg")] = obs_deg;
	p[N + String("radials/target-radial-deg")] = obs_deg;
	p[N + String("in-range")] = nav_ok ? 1.0 : 0.0;
	p[N + String("signal-quality-norm")] = nav_ok ? nav_signal : 0.0;
	p[N + String("to-flag")] = to_flag ? 1.0 : 0.0;
	p[N + String("from-flag")] = from_flag ? 1.0 : 0.0;
	p[N + String("heading-needle-deflection")] = cdi;
	p[N + String("heading-needle-deflection-norm")] = cdi / 10.0;
	p[N + String("gs-needle-deflection")] = gs_ok ? gs : 0.0;
	p[N + String("gs-needle-deflection-norm")] = gs_ok ? gs / 3.5 : 0.0;
	p[N + String("gs-in-range")] = gs_ok ? 1.0 : 0.0;
	p[N + String("gs-direct-deg")] = gs_ok ? gs_direct : 0.0;  // FlightGear navradio: the aircraft's angle above the GS site
	p[N + String("gs-target-deg")] = gs_ok ? gs_path : 0.0;    // the site's path angle (LSZH 28: 3.3)
	p[N + String("has-gs")] = loc_mode ? 1.0 : 0.0;
	p[N + String("nav-loc")] = loc_mode ? 1.0 : 0.0;
	p[N + String("ils-runway")] = ils_runway();
	p[N + String("radials/actual-deg")] = nav_ok ? nav_radial : 0.0;
	p[N + String("radials/reciprocal-radial-deg")] = nav_ok ? Math::fposmod(nav_radial + 180.0, 360.0) : 0.0;
	p[N + String("heading-deg")] = nav_ok ? nav_bearing_to : 0.0;
	p[N + String("nav-distance")] = (nav_ok || dme_ok) ? nav_dist_m : 0.0;
	p[N + String("nav-id")] = nav_ident();
	p[N + String("power-btn")] = powered ? 1.0 : 0.0;
	p[N + String("serviceable")] = 1.0;
	if (receiver_index > 0)
	{
		return;
	}
	// DME (slaved to NAV 1)
	p["instrumentation/dme/frequencies/selected-mhz"] = nav_mhz;
	p["instrumentation/dme/in-range"] = dme_ok ? 1.0 : 0.0;
	p["instrumentation/dme/indicated-distance-nm"] = dme_ok ? dme_nm : 0.0;
	p["instrumentation/dme/indicated-ground-speed-kt"] = dme_ok ? Math::max(dme_gs_kt, 0.0) : 0.0;
	p["instrumentation/dme/indicated-time-min"] = (dme_ok && dme_gs_kt > 1.0) ? dme_nm / dme_gs_kt * 60.0 : 0.0;
	// ADF 1
	p["instrumentation/adf/frequencies/selected-khz"] = adf_khz;
	p["instrumentation/adf/in-range"] = adf_ok ? 1.0 : 0.0;
	p["instrumentation/adf/indicated-bearing-deg"] = adf_needle;
	p["instrumentation/adf/rotation-deg"] = adf_card_deg;
	p["instrumentation/adf/ident"] = adf_ident();
	p["instrumentation/adf/nav-distance"] = adf_ok ? adf_dist_m : 0.0;
	p["instrumentation/kr-87/outputs/needle-deg"] = adf_needle;
	p["instrumentation/kr-87/inputs/rotation-deg"] = adf_card_deg;
}

Dictionary RadioNav::get_properties() const
{
	Dictionary p;
	write_properties(p);
	return p;
}

String RadioNav::status_text() const
{
	char buf[160];
	String s;
	if (loc_mode && ils != nullptr)
	{
		snprintf(buf, sizeof(buf), "ILS%s %.2f ", ils->synthetic ? "*" : "", nav_mhz);
		s += buf;
		if (nav_ok || dme_ok)
		{
			s += ils->ident + String(" RWY ") + ils->rwy;
			if (nav_ok)
			{
				snprintf(buf, sizeof(buf), " LOC %+.1f%s", cdi, gs_ok ? "" : " GS --");
				s += buf;
				if (gs_ok)
				{
					snprintf(buf, sizeof(buf), " GS %+.1f", gs);
					s += buf;
				}
			}
			if (dme_ok)
			{
				snprintf(buf, sizeof(buf), " %.1f NM", dme_nm);
				s += buf;
			}
		}
		else
		{
			s += ils->icao + String(" ") + ils->rwy + String(" ---");
		}
	}
	else
	{
		snprintf(buf, sizeof(buf), "NAV %.2f ", nav_mhz);
		s += buf;
	}
	if (!loc_mode && nav_station != nullptr && (nav_ok || dme_ok))
	{
		s += nav_station->ident;
		if (nav_ok)
		{
			snprintf(buf, sizeof(buf), " R%03d %s", (int)std::lround(nav_radial) % 360, to_flag ? "TO" : "FROM");
			s += buf;
		}
		if (dme_ok)
		{
			snprintf(buf, sizeof(buf), " %.1f NM", dme_nm);
			s += buf;
		}
	}
	else if (!loc_mode)
	{
		s += "---";
	}
	snprintf(buf, sizeof(buf), "   ADF %d ", (int)std::lround(adf_khz));
	s += buf;
	if (adf_station != nullptr && adf_ok)
	{
		snprintf(buf, sizeof(buf), "%s %03d", adf_station->ident.utf8().get_data(), (int)std::lround(adf_needle) % 360);
		s += buf;
	}
	else
	{
		s += "---";
	}
	return s;
}

} // namespace jsbgodot
