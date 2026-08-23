#include "geo.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace jsbgodot
{

void Geo::_bind_methods()
{
	ClassDB::bind_static_method("Geo", D_METHOD("wgs84_to_lv95", "lat_deg", "lon_deg"), &Geo::wgs84_to_lv95);
	ClassDB::bind_static_method("Geo", D_METHOD("lv95_to_wgs84", "easting", "northing"), &Geo::lv95_to_wgs84);
}

// WGS84 -> Swiss LV95 (CH1903+). Official swisstopo approximation, ~1 m.
Vector2 Geo::wgs84_to_lv95(double lat_deg, double lon_deg)
{
	double phi_sec = lat_deg * 3600.0;
	double lam_sec = lon_deg * 3600.0;
	double phi = (phi_sec - 169028.66) / 10000.0;
	double lam = (lam_sec - 26782.5) / 10000.0;
	double e = 2600072.37 + 211455.93 * lam - 10938.51 * lam * phi - 0.36 * lam * phi * phi - 44.54 * lam * lam * lam;
	double n = 1200147.07 + 308807.95 * phi + 3745.25 * lam * lam + 76.63 * phi * phi - 194.56 * lam * lam * phi + 119.79 * phi * phi * phi;
	return Vector2(e, n);
}

// Swiss LV95 (CH1903+) -> WGS84. Official swisstopo approximation, ~1 m.
Vector2 Geo::lv95_to_wgs84(double easting, double northing)
{
	double y = (easting - 2600000.0) / 1000000.0;
	double x = (northing - 1200000.0) / 1000000.0;
	double lon_aux = 2.6779094 + 4.728982 * y + 0.791484 * y * x + 0.1306 * y * x * x - 0.0436 * y * y * y;
	double lat_aux = 16.9023892 + 3.238272 * x - 0.270978 * y * y - 0.002528 * x * x - 0.0447 * y * y * x - 0.0140 * x * x * x;
	double lon = lon_aux * 100.0 / 36.0;
	double lat = lat_aux * 100.0 / 36.0;
	return Vector2(lat, lon);
}

Geo::Geo() {}
Geo::~Geo() {}

} // namespace jsbgodot