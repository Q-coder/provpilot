#include "geo_magnetic.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/time.hpp>

#include <cmath>

using namespace godot;

namespace jsbgodot
{

// WMM2025 Gauss coefficients: n, m, g (nT), h (nT), dg/dt, dh/dt (nT/yr),
// epoch 2025.0, from NOAA's WMM.COF (released 2024-11-13).
static const double WMM_EPOCH = 2025.0;
static const int WMM_DEGREE = 12;
static const double WMM_EARTH_RADIUS_KM = 6371.2;   // geomagnetic reference radius
static const double WGS84_A_KM = 6378.137;
static const double WGS84_F = 1.0 / 298.257223563;

struct WmmCoef
{
	int n, m;
	double g, h, gdot, hdot;
};

static const WmmCoef WMM2025[] = {
	{1, 0, -29351.8, 0.0, 12.0, 0.0},
	{1, 1, -1410.8, 4545.4, 9.7, -21.5},
	{2, 0, -2556.6, 0.0, -11.6, 0.0},
	{2, 1, 2951.1, -3133.6, -5.2, -27.7},
	{2, 2, 1649.3, -815.1, -8.0, -12.1},
	{3, 0, 1361.0, 0.0, -1.3, 0.0},
	{3, 1, -2404.1, -56.6, -4.2, 4.0},
	{3, 2, 1243.8, 237.5, 0.4, -0.3},
	{3, 3, 453.6, -549.5, -15.6, -4.1},
	{4, 0, 895.0, 0.0, -1.6, 0.0},
	{4, 1, 799.5, 278.6, -2.4, -1.1},
	{4, 2, 55.7, -133.9, -6.0, 4.1},
	{4, 3, -281.1, 212.0, 5.6, 1.6},
	{4, 4, 12.1, -375.6, -7.0, -4.4},
	{5, 0, -233.2, 0.0, 0.6, 0.0},
	{5, 1, 368.9, 45.4, 1.4, -0.5},
	{5, 2, 187.2, 220.2, 0.0, 2.2},
	{5, 3, -138.7, -122.9, 0.6, 0.4},
	{5, 4, -142.0, 43.0, 2.2, 1.7},
	{5, 5, 20.9, 106.1, 0.9, 1.9},
	{6, 0, 64.4, 0.0, -0.2, 0.0},
	{6, 1, 63.8, -18.4, -0.4, 0.3},
	{6, 2, 76.9, 16.8, 0.9, -1.6},
	{6, 3, -115.7, 48.8, 1.2, -0.4},
	{6, 4, -40.9, -59.8, -0.9, 0.9},
	{6, 5, 14.9, 10.9, 0.3, 0.7},
	{6, 6, -60.7, 72.7, 0.9, 0.9},
	{7, 0, 79.5, 0.0, -0.0, 0.0},
	{7, 1, -77.0, -48.9, -0.1, 0.6},
	{7, 2, -8.8, -14.4, -0.1, 0.5},
	{7, 3, 59.3, -1.0, 0.5, -0.8},
	{7, 4, 15.8, 23.4, -0.1, 0.0},
	{7, 5, 2.5, -7.4, -0.8, -1.0},
	{7, 6, -11.1, -25.1, -0.8, 0.6},
	{7, 7, 14.2, -2.3, 0.8, -0.2},
	{8, 0, 23.2, 0.0, -0.1, 0.0},
	{8, 1, 10.8, 7.1, 0.2, -0.2},
	{8, 2, -17.5, -12.6, 0.0, 0.5},
	{8, 3, 2.0, 11.4, 0.5, -0.4},
	{8, 4, -21.7, -9.7, -0.1, 0.4},
	{8, 5, 16.9, 12.7, 0.3, -0.5},
	{8, 6, 15.0, 0.7, 0.2, -0.6},
	{8, 7, -16.8, -5.2, -0.0, 0.3},
	{8, 8, 0.9, 3.9, 0.2, 0.2},
	{9, 0, 4.6, 0.0, -0.0, 0.0},
	{9, 1, 7.8, -24.8, -0.1, -0.3},
	{9, 2, 3.0, 12.2, 0.1, 0.3},
	{9, 3, -0.2, 8.3, 0.3, -0.3},
	{9, 4, -2.5, -3.3, -0.3, 0.3},
	{9, 5, -13.1, -5.2, 0.0, 0.2},
	{9, 6, 2.4, 7.2, 0.3, -0.1},
	{9, 7, 8.6, -0.6, -0.1, -0.2},
	{9, 8, -8.7, 0.8, 0.1, 0.4},
	{9, 9, -12.9, 10.0, -0.1, 0.1},
	{10, 0, -1.3, 0.0, 0.1, 0.0},
	{10, 1, -6.4, 3.3, 0.0, 0.0},
	{10, 2, 0.2, 0.0, 0.1, -0.0},
	{10, 3, 2.0, 2.4, 0.1, -0.2},
	{10, 4, -1.0, 5.3, -0.0, 0.1},
	{10, 5, -0.6, -9.1, -0.3, -0.1},
	{10, 6, -0.9, 0.4, 0.0, 0.1},
	{10, 7, 1.5, -4.2, -0.1, 0.0},
	{10, 8, 0.9, -3.8, -0.1, -0.1},
	{10, 9, -2.7, 0.9, -0.0, 0.2},
	{10, 10, -3.9, -9.1, -0.0, -0.0},
	{11, 0, 2.9, 0.0, 0.0, 0.0},
	{11, 1, -1.5, 0.0, -0.0, -0.0},
	{11, 2, -2.5, 2.9, 0.0, 0.1},
	{11, 3, 2.4, -0.6, 0.0, -0.0},
	{11, 4, -0.6, 0.2, 0.0, 0.1},
	{11, 5, -0.1, 0.5, -0.1, -0.0},
	{11, 6, -0.6, -0.3, 0.0, -0.0},
	{11, 7, -0.1, -1.2, -0.0, 0.1},
	{11, 8, 1.1, -1.7, -0.1, -0.0},
	{11, 9, -1.0, -2.9, -0.1, 0.0},
	{11, 10, -0.2, -1.8, -0.1, 0.0},
	{11, 11, 2.6, -2.3, -0.1, 0.0},
	{12, 0, -2.0, 0.0, 0.0, 0.0},
	{12, 1, -0.2, -1.3, 0.0, -0.0},
	{12, 2, 0.3, 0.7, -0.0, 0.0},
	{12, 3, 1.2, 1.0, -0.0, -0.1},
	{12, 4, -1.3, -1.4, -0.0, 0.1},
	{12, 5, 0.6, -0.0, -0.0, -0.0},
	{12, 6, 0.6, 0.6, 0.1, -0.0},
	{12, 7, 0.5, -0.1, -0.0, -0.0},
	{12, 8, -0.1, 0.8, 0.0, 0.0},
	{12, 9, -0.4, 0.1, 0.0, -0.0},
	{12, 10, -0.2, -1.0, -0.1, -0.0},
	{12, 11, -1.3, 0.1, -0.0, 0.0},
	{12, 12, -0.7, 0.2, -0.1, -0.1},
};

void GeoMagnetic::_bind_methods()
{
	ClassDB::bind_static_method("GeoMagnetic", D_METHOD("declination_deg", "lat_deg", "lon_deg", "alt_m", "decimal_year"), &GeoMagnetic::declination_deg);
	ClassDB::bind_static_method("GeoMagnetic", D_METHOD("inclination_deg", "lat_deg", "lon_deg", "alt_m", "decimal_year"), &GeoMagnetic::inclination_deg);
	ClassDB::bind_static_method("GeoMagnetic", D_METHOD("total_intensity_nt", "lat_deg", "lon_deg", "alt_m", "decimal_year"), &GeoMagnetic::total_intensity_nt);
	ClassDB::bind_static_method("GeoMagnetic", D_METHOD("decimal_year_now"), &GeoMagnetic::decimal_year_now);
}

GeoMagnetic::GeoMagnetic() {}
GeoMagnetic::~GeoMagnetic() {}

double GeoMagnetic::decimal_year_now()
{
	const Dictionary d = Time::get_singleton()->get_datetime_dict_from_system(true);
	const int year = (int)d.get("year", 2026);
	// fraction of the year from the unix time and the year's first day
	Dictionary jan1;
	jan1["year"] = year;
	jan1["month"] = 1;
	jan1["day"] = 1;
	jan1["hour"] = 0;
	jan1["minute"] = 0;
	jan1["second"] = 0;
	const double t0 = (double)Time::get_singleton()->get_unix_time_from_datetime_dict(jan1);
	const double now = Time::get_singleton()->get_unix_time_from_system();
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	const double year_s = (leap ? 366.0 : 365.0) * 86400.0;
	return year + std::fmin(std::fmax((now - t0) / year_s, 0.0), 1.0);
}

// Spherical-harmonic synthesis (WMM technical report, section 1): geodetic ->
// geocentric spherical, Schmidt semi-normalised associated Legendre functions
// by recursion, field components in the geocentric frame, rotated back into
// the geodetic north / east / down frame.
void GeoMagnetic::field_ned(double lat_deg, double lon_deg, double alt_m, double decimal_year,
		double &x_nt, double &y_nt, double &z_nt)
{
	const double dt = std::fmin(std::fmax(decimal_year, WMM_EPOCH), WMM_EPOCH + 5.0) - WMM_EPOCH;
	const double phi = lat_deg * M_PI / 180.0;
	const double lambda = lon_deg * M_PI / 180.0;
	const double h_km = alt_m / 1000.0;
	// geodetic -> geocentric
	const double e2 = WGS84_F * (2.0 - WGS84_F);
	const double sin_phi = std::sin(phi), cos_phi = std::cos(phi);
	const double rc = WGS84_A_KM / std::sqrt(1.0 - e2 * sin_phi * sin_phi);
	const double p = (rc + h_km) * cos_phi;
	const double zc = (rc * (1.0 - e2) + h_km) * sin_phi;
	const double r = std::sqrt(p * p + zc * zc);
	const double phi_c = std::asin(zc / r);
	const double s = std::sin(phi_c);
	double c = std::cos(phi_c);
	if (std::fabs(c) < 1e-9)
	{
		c = 1e-9; // the pole itself: avoid the 1/cos singularity in Y
	}
	// Schmidt semi-normalised P[n][m](sin phi_c) and dP/dphi_c
	const int N = WMM_DEGREE;
	double P[WMM_DEGREE + 1][WMM_DEGREE + 1];
	double dP[WMM_DEGREE + 1][WMM_DEGREE + 1];
	for (int n = 0; n <= N; n++)
	{
		for (int m = 0; m <= N; m++)
		{
			P[n][m] = 0.0;
			dP[n][m] = 0.0;
		}
	}
	P[0][0] = 1.0;
	dP[0][0] = 0.0;
	for (int n = 1; n <= N; n++)
	{
		for (int m = 0; m <= n; m++)
		{
			if (n == m)
			{
				// Schmidt semi-normalisation: the sqrt(2) of the m >= 1 terms
				// enters once, at P[1][1] = cos(phi_c)
				const double k = (n == 1) ? 1.0 : std::sqrt(1.0 - 1.0 / (2.0 * n));
				P[n][n] = k * c * P[n - 1][n - 1];
				dP[n][n] = k * (c * dP[n - 1][n - 1] - s * P[n - 1][n - 1]);
			}
			else
			{
				const double k1 = (2.0 * n - 1.0) / std::sqrt((double)(n * n - m * m));
				const double k2 = std::sqrt(std::fmax(0.0, (double)((n - 1) * (n - 1) - m * m)) / (double)(n * n - m * m));
				const double pm2 = (n >= 2) ? P[n - 2][m] : 0.0;
				const double dpm2 = (n >= 2) ? dP[n - 2][m] : 0.0;
				P[n][m] = k1 * s * P[n - 1][m] - k2 * pm2;
				dP[n][m] = k1 * (s * dP[n - 1][m] + c * P[n - 1][m]) - k2 * dpm2;
			}
		}
	}
	double cos_ml[WMM_DEGREE + 1], sin_ml[WMM_DEGREE + 1];
	for (int m = 0; m <= N; m++)
	{
		cos_ml[m] = std::cos(m * lambda);
		sin_ml[m] = std::sin(m * lambda);
	}
	double xp = 0.0, yp = 0.0, zp = 0.0;
	const double ar = WMM_EARTH_RADIUS_KM / r;
	double ar_n2 = ar * ar; // (a/r)^(n+2) for n = 0
	int ni = 0;
	for (int n = 1; n <= N; n++)
	{
		ar_n2 *= ar;
		double sx = 0.0, sy = 0.0, sz = 0.0;
		for (int m = 0; m <= n; m++)
		{
			const WmmCoef &cf = WMM2025[ni++];
			const double g = cf.g + dt * cf.gdot;
			const double hh = cf.h + dt * cf.hdot;
			const double a = g * cos_ml[m] + hh * sin_ml[m];
			const double b = g * sin_ml[m] - hh * cos_ml[m];
			sx += a * dP[n][m];
			sy += m * b * P[n][m];
			sz += a * P[n][m];
		}
		xp += -ar_n2 * sx;
		yp += ar_n2 * sy / c;
		zp += -(n + 1) * ar_n2 * sz;
	}
	// geocentric -> geodetic frame
	const double d = phi_c - phi;
	x_nt = xp * std::cos(d) - zp * std::sin(d);
	y_nt = yp;
	z_nt = xp * std::sin(d) + zp * std::cos(d);
}

double GeoMagnetic::declination_deg(double lat_deg, double lon_deg, double alt_m, double decimal_year)
{
	double x, y, z;
	field_ned(lat_deg, lon_deg, alt_m, decimal_year, x, y, z);
	return std::atan2(y, x) * 180.0 / M_PI;
}

double GeoMagnetic::inclination_deg(double lat_deg, double lon_deg, double alt_m, double decimal_year)
{
	double x, y, z;
	field_ned(lat_deg, lon_deg, alt_m, decimal_year, x, y, z);
	return std::atan2(z, std::sqrt(x * x + y * y)) * 180.0 / M_PI;
}

double GeoMagnetic::total_intensity_nt(double lat_deg, double lon_deg, double alt_m, double decimal_year)
{
	double x, y, z;
	field_ned(lat_deg, lon_deg, alt_m, decimal_year, x, y, z);
	return std::sqrt(x * x + y * y + z * z);
}

} // namespace jsbgodot
