#include "geo.h"
#include "s3d_proj.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace jsbgodot
{

void Geo::_bind_methods()
{
	ClassDB::bind_static_method("Geo", D_METHOD("wgs84_to_lv95", "lat_deg", "lon_deg"), &Geo::wgs84_to_lv95);
	ClassDB::bind_static_method("Geo", D_METHOD("lv95_to_wgs84", "easting", "northing"), &Geo::lv95_to_wgs84);
	ClassDB::bind_static_method("Geo", D_METHOD("lv95_grid_convergence_deg", "lat_deg", "lon_deg"), &Geo::lv95_grid_convergence_deg);
	ClassDB::bind_static_method("Geo", D_METHOD("wgs84_to_utm", "lat_deg", "lon_deg", "zone", "north"), &Geo::wgs84_to_utm);
	ClassDB::bind_static_method("Geo", D_METHOD("utm_to_wgs84", "easting", "northing", "zone", "north"), &Geo::utm_to_wgs84);
	ClassDB::bind_static_method("Geo", D_METHOD("utm_zone_for", "lon_deg"), &Geo::utm_zone_for);
}

Vector2 Geo::wgs84_to_lv95(double lat_deg, double lon_deg)
{
	const s3d::proj::EN en = s3d::proj::wgs84_to_lv95(lat_deg, lon_deg);
	return Vector2(en.e, en.n);
}

Vector2 Geo::lv95_to_wgs84(double easting, double northing)
{
	const s3d::proj::LatLon ll = s3d::proj::lv95_to_wgs84(easting, northing);
	return Vector2(ll.lat, ll.lon);
}

double Geo::lv95_grid_convergence_deg(double lat_deg, double lon_deg)
{
	return s3d::proj::lv95_convergence_deg(lat_deg, lon_deg);
}

Vector2 Geo::wgs84_to_utm(double lat_deg, double lon_deg, int zone, bool north)
{
	const s3d::proj::EN en = s3d::proj::wgs84_to_utm(lat_deg, lon_deg, zone, north);
	return Vector2(en.e, en.n);
}

Vector2 Geo::utm_to_wgs84(double easting, double northing, int zone, bool north)
{
	const s3d::proj::LatLon ll = s3d::proj::utm_to_wgs84(easting, northing, zone, north);
	return Vector2(ll.lat, ll.lon);
}

int Geo::utm_zone_for(double lon_deg)
{
	return s3d::proj::utm_zone_for(lon_deg);
}

Geo::Geo() {}
Geo::~Geo() {}

} // namespace jsbgodot
