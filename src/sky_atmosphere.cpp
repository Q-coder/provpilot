#include "sky_atmosphere.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdio>

#include "geo_frame.h"

using namespace godot;

namespace jsbgodot
{

static const Color AMBIENT_COLOR(0.52, 0.58, 0.68); // constant-colour ambient floor

void SkyAtmosphere::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("setup", "env", "sun", "fill", "globe", "lat_deg", "lon_deg"), &SkyAtmosphere::setup);
	ClassDB::bind_method(D_METHOD("set_visibility_km", "km"), &SkyAtmosphere::set_visibility_km);
	ClassDB::bind_method(D_METHOD("set_time", "use_system", "local_minutes"), &SkyAtmosphere::set_time);
	ClassDB::bind_method(D_METHOD("get_time_string"), &SkyAtmosphere::get_time_string);
	ClassDB::bind_method(D_METHOD("update", "delta", "viewport"), &SkyAtmosphere::update);
	ClassDB::bind_static_method("SkyAtmosphere", D_METHOD("solar_direction", "lat_deg", "lon_deg", "doy", "utc_hours"), &SkyAtmosphere::solar_direction);
	ClassDB::bind_method(D_METHOD("get_environment"), &SkyAtmosphere::get_environment);
	ClassDB::bind_method(D_METHOD("get_sun_light"), &SkyAtmosphere::get_sun_light);
	ClassDB::bind_method(D_METHOD("get_observer_lat"), &SkyAtmosphere::get_observer_lat);
	ClassDB::bind_method(D_METHOD("set_observer_lat", "value"), &SkyAtmosphere::set_observer_lat);
	ClassDB::bind_method(D_METHOD("get_observer_lon"), &SkyAtmosphere::get_observer_lon);
	ClassDB::bind_method(D_METHOD("set_observer_lon", "value"), &SkyAtmosphere::set_observer_lon);
	ClassDB::bind_method(D_METHOD("get_sun_dir_now"), &SkyAtmosphere::get_sun_dir_now);
	ClassDB::bind_method(D_METHOD("get_sun_color_now"), &SkyAtmosphere::get_sun_color_now);
	ClassDB::bind_method(D_METHOD("get_sun_energy_now"), &SkyAtmosphere::get_sun_energy_now);
	ClassDB::bind_method(D_METHOD("get_ambient_energy_now"), &SkyAtmosphere::get_ambient_energy_now);
	ClassDB::bind_method(D_METHOD("get_fog_color_now"), &SkyAtmosphere::get_fog_color_now);
	ClassDB::bind_method(D_METHOD("get_cloud_ambient_now"), &SkyAtmosphere::get_cloud_ambient_now);
	ClassDB::bind_method(D_METHOD("get_twilight_now"), &SkyAtmosphere::get_twilight_now);
	ClassDB::bind_method(D_METHOD("get_time_seconds"), &SkyAtmosphere::get_time_seconds);
	ClassDB::bind_method(D_METHOD("set_time_seconds", "value"), &SkyAtmosphere::set_time_seconds);
	ClassDB::bind_method(D_METHOD("get_clear_air"), &SkyAtmosphere::get_clear_air);
	ClassDB::bind_method(D_METHOD("set_clear_air", "value"), &SkyAtmosphere::set_clear_air);
	ClassDB::bind_method(D_METHOD("get_base_sun_energy"), &SkyAtmosphere::get_base_sun_energy);
	ClassDB::bind_method(D_METHOD("get_base_fog_density"), &SkyAtmosphere::get_base_fog_density);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment", PROPERTY_HINT_RESOURCE_TYPE, "Environment"), "", "get_environment");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sun_light", PROPERTY_HINT_NODE_TYPE, "DirectionalLight3D"), "", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "observer_lat"), "set_observer_lat", "get_observer_lat");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "observer_lon"), "set_observer_lon", "get_observer_lon");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "sun_dir_now"), "", "get_sun_dir_now");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sun_color_now"), "", "get_sun_color_now");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_energy_now"), "", "get_sun_energy_now");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_energy_now"), "", "get_ambient_energy_now");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "fog_color_now"), "", "get_fog_color_now");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_ambient_now"), "", "get_cloud_ambient_now");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "twilight_now"), "", "get_twilight_now");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_seconds"), "set_time_seconds", "get_time_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clear_air"), "set_clear_air", "get_clear_air");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_sun_energy"), "", "get_base_sun_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_fog_density"), "", "get_base_fog_density");
}

SkyAtmosphere::SkyAtmosphere()
{
}

Environment *SkyAtmosphere::get_environment() const { return environment; }
DirectionalLight3D *SkyAtmosphere::get_sun_light() const { return sun_light; }
double SkyAtmosphere::get_observer_lat() const { return observer_lat; }
void SkyAtmosphere::set_observer_lat(double v) { observer_lat = v; }
double SkyAtmosphere::get_observer_lon() const { return observer_lon; }
void SkyAtmosphere::set_observer_lon(double v) { observer_lon = v; }
Vector3 SkyAtmosphere::get_sun_dir_now() const { return sun_dir_now; }
Color SkyAtmosphere::get_sun_color_now() const { return sun_color_now; }
double SkyAtmosphere::get_sun_energy_now() const { return sun_energy_now; }
Color SkyAtmosphere::get_fog_color_now() const { return fog_color_now; }
double SkyAtmosphere::get_cloud_ambient_now() const { return cloud_ambient_now; }
double SkyAtmosphere::get_twilight_now() const { return twilight_now; }
double SkyAtmosphere::get_ambient_energy_now() const { return ambient_energy_now; }
double SkyAtmosphere::get_time_seconds() const { return time_seconds; }
void SkyAtmosphere::set_time_seconds(double v) { time_seconds = v; }
bool SkyAtmosphere::get_clear_air() const { return clear_air; }
void SkyAtmosphere::set_clear_air(bool v) { clear_air = v; }
double SkyAtmosphere::get_base_sun_energy() const { return base_sun_energy; }
double SkyAtmosphere::get_base_fog_density() const { return base_fog_density; }

void SkyAtmosphere::setup(Environment *env, DirectionalLight3D *sun, DirectionalLight3D *fill, Node *globe, double lat_deg, double lon_deg)
{
	environment = env;
	sun_light = sun;
	fill_light = fill;
	globe_node = globe;
	observer_lat = lat_deg;
	observer_lon = lon_deg;
	if (environment != nullptr)
	{
		base_fog_density = environment->get_fog_density();
		base_fog_color = environment->get_fog_light_color();
		base_fog_light_energy = environment->get_fog_light_energy();
		base_ambient_energy = environment->get_ambient_light_energy();
		base_glow_intensity = environment->get_glow_intensity();
		install_sky_material();
	}
	if (sun_light != nullptr)
	{
		base_sun_energy = sun_light->get_param(Light3D::PARAM_ENERGY);
	}
	if (fill_light != nullptr)
	{
		base_fill_energy = fill_light->get_param(Light3D::PARAM_ENERGY);
	}
}

// Koschmieder: the 5 % contrast threshold sits at 3.912 / beta. Replaces the
// authored baseline; the altitude ramp still fades it out toward space.
void SkyAtmosphere::set_visibility_km(double km)
{
	base_fog_density = 3.912 / (Math::max(km, 0.1) * 1000.0);
}

void SkyAtmosphere::set_time(bool use_system, int local_minutes)
{
	use_system_time = use_system;
	manual_minutes = Math::clamp(local_minutes, 0, 1439);
	time_seconds = (double)manual_minutes * 60.0;
}

String SkyAtmosphere::get_time_string() const
{
	const int s = ((int)time_seconds % 86400 + 86400) % 86400;
	char buf[16];
	snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	return String(buf);
}

void SkyAtmosphere::update(double delta, Viewport *viewport)
{
	if (environment == nullptr)
	{
		return;
	}
	// Advance the manual clock in real time (system mode reads the OS clock).
	if (!use_system_time)
	{
		time_seconds = Math::fposmod(time_seconds + delta, 86400.0);
	}
	Camera3D *cam = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	// ECEF world: the observer is wherever the camera is.
	double alt = 0.0;
	Vector3 cam_normal(0, 1, 0);
	if (cam != nullptr && cam->get_global_position().is_finite() && cam->get_global_position().length() > 1.0e6)
	{
		const geo::Geodetic g = geo::geodetic_of(cam->get_global_position());
		observer_lat = g.lat;
		observer_lon = g.lon;
		alt = g.h;
		cam_normal = geo::wun_basis(g.lat, g.lon).get_column(1);
	}
	observer_up = cam_normal;

	const Vector2 when = current_time();
	const Vector3 dir_to_sun = solar_direction(observer_lat, observer_lon, when.x, when.y);
	const double sin_elev = dir_to_sun.dot(cam_normal);
	// The directional light fades to zero AS the sun reaches the horizon and
	// stays off below it (an upward light would lit the undersides).
	const double day = Math::clamp(sin_elev / 0.08, 0.0, 1.0);
	// From space the ECEF geometry already produces the terminator, so the
	// local fade must not dim the whole planet: blend the sun back to full
	// energy between SUN_LOCAL_ALT_M and SUN_SPACE_ALT_M and pull the ambient
	// floor down to night level.
	const double space = Math::clamp((alt - SUN_LOCAL_ALT_M) / (SUN_SPACE_ALT_M - SUN_LOCAL_ALT_M), 0.0, 1.0);
	const double sun_factor = Math::lerp(day, 1.0, space);
	// The sun disk stays visible down to the altitude-adjusted horizon (dip),
	// then fades over the last ~1 degree.
	const double dip = std::acos(Math::clamp(PLANET_RADIUS / (PLANET_RADIUS + Math::max(alt, 0.0)), 0.0, 1.0));
	const double horizon_sin = -std::sin(dip) - 0.010; // extra ~0.6 deg for refraction
	const double disk = Math::clamp((sin_elev - horizon_sin) / 0.02, 0.0, 1.0);

	sun_dir_now = dir_to_sun;
	sun_energy_now = base_sun_energy * sun_factor;
	sun_color_now = sun_color(sin_elev).lerp(Color(1.0, 0.97, 0.92), space);
	if (sun_light != nullptr)
	{
		orient_sun(dir_to_sun);
		sun_light->set_param(Light3D::PARAM_ENERGY, sun_energy_now);
		sun_light->set_color(sun_color_now);
		sun_light->set_visible(sun_factor > 0.001);
	}
	if (fill_light != nullptr)
	{
		fill_light->set_param(Light3D::PARAM_ENERGY, base_fill_energy * day * (1.0 - space));
	}
	if (sky_material.is_valid())
	{
		sky_material->set_shader_parameter("sun_direction", dir_to_sun);
		sky_material->set_shader_parameter("sun_disk_energy", disk);
		// The shader's sphere (mean radius) is kept tangent to the ellipsoid
		// under the camera: centre = camera - normal * (R + h), so dip and
		// horizon distance come out right at every latitude.
		if (cam != nullptr)
		{
			sky_material->set_shader_parameter("planet_center", cam->get_global_position() - cam_normal * (PLANET_RADIUS + alt));
		}
	}
	apply_altitude(alt, day, space, sin_elev);
}

void SkyAtmosphere::install_sky_material()
{
	Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://shaders/atmosphere_sky.gdshader");
	if (shader.is_null())
	{
		UtilityFunctions::push_warning("SkyAtmosphere: res://shaders/atmosphere_sky.gdshader missing - keeping the procedural sky");
		return;
	}
	Ref<ShaderMaterial> mat = memnew(ShaderMaterial);
	mat->set_shader(shader);
	mat->set_shader_parameter("planet_radius", PLANET_RADIUS);
	mat->set_shader_parameter("atmosphere_radius", PLANET_RADIUS + ATMOSPHERE_HEIGHT);
	mat->set_shader_parameter("planet_center", Vector3(0.0, -PLANET_RADIUS, 0.0));
	Ref<Sky> sky = environment->get_sky();
	if (sky.is_null())
	{
		sky = memnew(Sky);
		environment->set_sky(sky);
	}
	sky->set_material(mat);
	environment->set_background(Environment::BG_SKY);
	sky_material = mat;
}

// Fog fade with altitude + a day/night constant-colour ambient floor (the
// atmosphere sky gives little usable ambient, and a normal-independent floor
// keeps terrain with odd normals readable). The background stays on the sky
// shader, which renders space black + the atmospheric limb by itself.
void SkyAtmosphere::apply_altitude(double alt, double day, double space, double sin_elev)
{
	// The haze and the clouds are lit by the sky, and the sky fades through
	// twilight: full with the sun 5 deg up, dark with it 8 deg down. A
	// constant fog light left the ground haze and every distant cloud at
	// their daytime brightness all night. Warm near the horizon sun.
	double tw = Math::clamp((sin_elev + 0.14) / 0.23, 0.0, 1.0);
	tw = tw * tw * (3.0 - 2.0 * tw);
	twilight_now = Math::lerp(tw, 1.0, space);
	const double sky_light = Math::lerp(FOG_NIGHT_LIGHT, 1.0, twilight_now * twilight_now);
	const double dusk = Math::clamp(1.0 - std::fabs(sin_elev) / 0.12, 0.0, 1.0) * (1.0 - space);
	const Color fog_tint = base_fog_color.lerp(Color(0.80, 0.62, 0.55), 0.35 * dusk);
	environment->set_fog_light_color(fog_tint);
	environment->set_fog_light_energy(base_fog_light_energy * sky_light);
	fog_color_now = Color(fog_tint.r * sky_light, fog_tint.g * sky_light, fog_tint.b * sky_light);
	cloud_ambient_now = Math::lerp(AMBIENT_NIGHT_ENERGY, AMBIENT_DAY_ENERGY, twilight_now * twilight_now);
	double t = Math::clamp(HAZE_LAYER_M / Math::max(alt, HAZE_LAYER_M), 0.0, 1.0);
	if (alt >= FOG_ZERO_ALT_M || clear_air)
	{
		t = 0.0;
	}
	environment->set_fog_density(base_fog_density * t);
	environment->set_fog_enabled(t > 0.0);
	const double ts = Math::clamp((SKY_TAPER_ALT_M - alt) / (SKY_TAPER_ALT_M - SKY_FULL_ALT_M), 0.0, 1.0);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
	environment->set_ambient_light_sky_contribution(0.0);
	environment->set_ambient_light_color(AMBIENT_COLOR);
	const double ambient_local = Math::lerp(AMBIENT_NIGHT_ENERGY, AMBIENT_DAY_ENERGY, day) * Math::lerp(0.5, 1.0, ts);
	environment->set_ambient_light_energy(Math::lerp(ambient_local, AMBIENT_NIGHT_ENERGY, space));
	ambient_energy_now = environment->get_ambient_light_energy();
	environment->set_glow_intensity(base_glow_intensity * ts);
	environment->set_glow_enabled(ts > 0.02);
}

// Point the DirectionalLight3D so its light travels from the sun toward the
// ground (local -Z = -dir_to_sun), leaving its position untouched.
void SkyAtmosphere::orient_sun(const Vector3 &dir_to_sun)
{
	const Vector3 z = dir_to_sun.normalized(); // local +Z points at the sun
	Vector3 up_ref = observer_up;
	if (std::abs(z.dot(up_ref)) > 0.999)
	{
		up_ref = Vector3(0, 0, -1);
	}
	const Vector3 x = up_ref.cross(z).normalized();
	const Vector3 y = z.cross(x).normalized();
	sun_light->set_global_transform(Transform3D(Basis(x, y, z), sun_light->get_global_position()));
}

Color SkyAtmosphere::sun_color(double sin_elev)
{
	const Color warm(1.0, 0.55, 0.30);
	const Color white(1.0, 0.97, 0.92);
	return warm.lerp(white, Math::clamp(sin_elev / 0.25, 0.0, 1.0));
}

// (day_of_year, utc_hours) for the active time source; keeps time_seconds
// (local mean-solar time) in sync for the HUD.
Vector2 SkyAtmosphere::current_time()
{
	const Dictionary sys = Time::get_singleton()->get_datetime_dict_from_system(true); // UTC
	const double doy = (double)day_of_year((int)sys["year"], (int)sys["month"], (int)sys["day"]);
	double utc_hours;
	if (use_system_time)
	{
		utc_hours = (double)(int)sys["hour"] + (double)(int)sys["minute"] / 60.0 + (double)(int)sys["second"] / 3600.0;
		time_seconds = Math::fposmod((utc_hours + observer_lon / 15.0) * 3600.0, 86400.0);
	}
	else
	{
		// The manual value is local mean-solar time; convert to UTC by longitude.
		utc_hours = (time_seconds / 3600.0) - observer_lon / 15.0;
	}
	return Vector2(doy, utc_hours);
}

// NOAA solar-position approximation (equation of time + declination) in the
// observer's local frame, rotated into ECEF.
Vector3 SkyAtmosphere::solar_direction(double lat_deg, double lon_deg, double doy, double utc_hours)
{
	const double g = Math_TAU / 365.0 * (doy - 1.0 + (utc_hours - 12.0) / 24.0);
	const double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g)
			- 0.014615 * std::cos(2.0 * g) - 0.040849 * std::sin(2.0 * g)); // minutes
	const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g)
			- 0.006758 * std::cos(2.0 * g) + 0.000907 * std::sin(2.0 * g)
			- 0.002697 * std::cos(3.0 * g) + 0.00148 * std::sin(3.0 * g); // radians
	const double tst = utc_hours * 60.0 + eqtime + 4.0 * lon_deg;  // true solar time (min)
	const double ha = Math::deg_to_rad(tst / 4.0 - 180.0);         // hour angle
	const double lat = Math::deg_to_rad(lat_deg);
	const double sin_lat = std::sin(lat), cos_lat = std::cos(lat);
	const double sin_dec = std::sin(decl), cos_dec = std::cos(decl);
	const double sin_ha = std::sin(ha), cos_ha = std::cos(ha);
	// Equatorial -> local horizontal (ENU)
	const double east = -cos_dec * sin_ha;
	const double up = sin_dec * sin_lat + cos_dec * cos_ha * cos_lat;
	const double north = sin_dec * cos_lat - cos_dec * cos_ha * sin_lat;
	// ENU -> ECEF at the observer (WUN frame: +X West, +Y Up, +Z North)
	return geo::wun_basis(lat_deg, lon_deg).xform(Vector3(-east, up, north)).normalized();
}

int SkyAtmosphere::day_of_year(int y, int m, int d)
{
	int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
	{
		days[1] = 29;
	}
	int n = d;
	for (int i = 0; i < m - 1 && i < 12; i++)
	{
		n += days[i];
	}
	return n;
}

} // namespace jsbgodot
