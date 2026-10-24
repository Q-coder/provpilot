#include "drive_mode.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

#include "geo.h"
#include "geo_frame.h"
#include "jsbgodot.h"
#include "key_bindings.h"
#include "world_scenery.h"

using namespace godot;

namespace jsbgodot
{

void DriveMode::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "ac_node", "world", "speed_label", "altitude_label", "vspeed_label", "throttle_label", "heading_label", "heading_deg_label"), &DriveMode::register_nodes);
	ClassDB::bind_method(D_METHOD("configure", "params"), &DriveMode::configure);
	ClassDB::bind_method(D_METHOD("arm", "jsb"), &DriveMode::arm);
	ClassDB::bind_method(D_METHOD("disengage", "jsb"), &DriveMode::disengage);
	ClassDB::bind_method(D_METHOD("update", "delta", "jsb", "rebase_cooldown", "joy_steer", "joy_accelerate", "joy_brake", "joy_handbrake"), &DriveMode::update, DEFVAL(0.0), DEFVAL(0.0), DEFVAL(0.0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_active"), &DriveMode::is_active);
	ClassDB::bind_method(D_METHOD("is_armed"), &DriveMode::is_armed);
	ClassDB::bind_method(D_METHOD("step", "delta", "throttle", "brake", "steer", "handbrake"), &DriveMode::step);
	ClassDB::bind_method(D_METHOD("apply_pose"), &DriveMode::apply_pose);
	ClassDB::bind_method(D_METHOD("capture_pose"), &DriveMode::capture_pose);
	ClassDB::bind_method(D_METHOD("get_speed_kmh"), &DriveMode::get_speed_kmh);
	ClassDB::bind_method(D_METHOD("get_heading_deg"), &DriveMode::get_heading_deg);
	ClassDB::bind_method(D_METHOD("get_steer_deg"), &DriveMode::get_steer_deg);
	ClassDB::bind_method(D_METHOD("get_lv95"), &DriveMode::get_lv95);
	ClassDB::bind_method(D_METHOD("set_lv95", "e", "n"), &DriveMode::set_lv95);
	ClassDB::bind_method(D_METHOD("set_heading_deg", "heading_deg"), &DriveMode::set_heading_deg);
}

DriveMode::DriveMode()
{
}

void DriveMode::register_nodes(Node3D *p_ac_node, Object *p_world,
		Label *p_speed, Label *p_altitude, Label *p_vspeed, Label *p_throttle, Label *p_heading, Label *p_heading_deg)
{
	ac_node = p_ac_node;
	world = Object::cast_to<WorldScenery>(p_world);
	speed_label = p_speed;
	altitude_label = p_altitude;
	vspeed_label = p_vspeed;
	throttle_label = p_throttle;
	heading_label = p_heading;
	heading_deg_label = p_heading_deg;
}

void DriveMode::configure(const Dictionary &p)
{
	wheelbase_m = MAX(0.5, (double)p.get("wheelbase_m", wheelbase_m));
	track_m = MAX(0.3, (double)p.get("track_m", track_m));
	max_speed = MAX(1.0, (double)p.get("max_speed_kmh", max_speed * 3.6)) / 3.6;
	max_reverse = MAX(0.5, (double)p.get("max_reverse_kmh", max_reverse * 3.6)) / 3.6;
	accel = MAX(0.1, (double)p.get("accel_mps2", accel));
	brake_decel = MAX(0.1, (double)p.get("brake_mps2", brake_decel));
	max_steer_deg = CLAMP((double)p.get("max_steer_deg", max_steer_deg), 5.0, 60.0);
	steer_rate_deg_s = MAX(10.0, (double)p.get("steer_rate_deg_s", steer_rate_deg_s));
	clearance_m = (double)p.get("clearance_m", clearance_m);
	lat_accel_max = MAX(1.0, (double)p.get("lat_accel_mps2", lat_accel_max));
	// air drag such that full throttle tops out a little above the limit
	// (the limit itself is a clamp): terminal speed 1.1 x max_speed
	aero = MAX(0.0, accel - rolling) / ((1.1 * max_speed) * (1.1 * max_speed));
}

void DriveMode::arm(Object *p_jsb)
{
	armed = true;
	active = false;
	speed = 0.0;
	steer_deg = 0.0;
	pitch = roll = 0.0;
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm != nullptr)
		fdm->set_slew_mode(true);
}

void DriveMode::disengage(Object *p_jsb)
{
	if (active)
	{
		UtilityFunctions::print("=== DRIVE MODE OFF ===");
		if (heading_deg_label != nullptr)
			heading_deg_label->set_text("");
	}
	armed = false;
	active = false;
	(void)p_jsb; // JSBSim is re-initialised by the next spawn
}

void DriveMode::capture_pose()
{
	if (ac_node == nullptr)
		return;
	const Transform3D xf = ac_node->get_global_transform();
	const geo::Geodetic g = geo::geodetic_of(xf.origin);
	const Vector2 lv = Geo::wgs84_to_lv95(g.lat, g.lon);
	e = lv.x;
	n = lv.y;
	h = g.h;
	heading_deg = geo::true_heading_deg(xf.basis, g.lat, g.lon);
	grid_offset_deg = Geo::lv95_grid_convergence_deg(g.lat, g.lon);
	speed = 0.0;
	steer_deg = 0.0;
	pitch = roll = 0.0;
}

double DriveMode::ground_at(double pe, double pn) const
{
	if (world == nullptr)
		return NAN;
	const Vector3 p = WorldScenery::ac_transform_at(pe, pn, h, heading_deg, Vector3(1, 1, 1)).origin;
	return world->ground_height_below(p);
}

void DriveMode::step(double delta, double throttle, double brake, double steer, bool handbrake)
{
	if (delta <= 0.0)
		return;
	throttle = CLAMP(throttle, 0.0, 1.0);
	brake = CLAMP(brake, 0.0, 1.0);
	steer = CLAMP(steer, -1.0, 1.0);

	// Steering: the lock shrinks with speed so the lateral acceleration
	// v^2 / R = v^2 tan(steer) / L stays under lat_accel_max (no skidding in
	// a kinematic car); the wheel turns at a finite rate toward the target
	// and centres itself when released.
	const double v2 = speed * speed;
	const double lock = v2 > 0.01 ? MIN(max_steer_deg, Math::rad_to_deg(std::atan(lat_accel_max * wheelbase_m / v2))) : max_steer_deg;
	const double target = steer * lock;
	const double max_turn = steer_rate_deg_s * delta;
	steer_deg += CLAMP(target - steer_deg, -max_turn, max_turn);

	// Longitudinal: throttle drives forward, the brake slows the car and,
	// once it stands, backs it up; a coasting car loses speed to rolling
	// resistance and air drag; the handbrake stops it fast.
	double a = 0.0;
	const bool stopped = std::abs(speed) < 0.05;
	if (throttle > 0.0 && speed > -0.05)
		a += throttle * accel;
	if (brake > 0.0)
	{
		if (speed > 0.05)
			a -= brake * brake_decel;
		else if (throttle <= 0.0)
			a -= brake * accel * 0.6; // reverse
	}
	if (!stopped)
	{
		const double s = speed > 0.0 ? 1.0 : -1.0;
		a -= s * (rolling + aero * speed * speed);
	}
	double next = speed + a * delta;
	if (handbrake)
	{
		const double d = brake_decel * 1.5 * delta;
		next = std::abs(next) <= d ? 0.0 : next - (next > 0.0 ? d : -d);
	}
	// resistance never reverses the direction of travel on its own
	if (throttle <= 0.0 && brake <= 0.0 && speed != 0.0 && (next > 0.0) != (speed > 0.0))
		next = 0.0;
	speed = CLAMP(next, -max_reverse, max_speed);
	if (std::abs(speed) < 0.06 && throttle <= 0.0 && brake <= 0.0)
		speed = 0.0; // the same band as "stopped": no resistance below it either

	// Bicycle model: yaw rate = v / L * tan(steer); then along the heading
	// in the LV95 grid (true heading + grid convergence).
	const double yaw_rate = speed / wheelbase_m * std::tan(Math::deg_to_rad(steer_deg));
	heading_deg = Math::fposmod(heading_deg + Math::rad_to_deg(yaw_rate * delta), 360.0);
	const double gh = Math::deg_to_rad(heading_deg + grid_offset_deg);
	e += std::sin(gh) * speed * delta;
	n += std::cos(gh) * speed * delta;
}

void DriveMode::apply_pose()
{
	if (ac_node == nullptr)
		return;
	const double gh = Math::deg_to_rad(heading_deg + grid_offset_deg);
	const Vector2 along(std::sin(gh), std::cos(gh));
	const Vector2 left(-std::cos(gh), std::sin(gh));
	const double hc = ground_at(e, n);
	if (!std::isnan(hc))
	{
		h = hc;
		// pitch / roll from the wheel heights, smoothed (a 2 m posting steps)
		const Vector2 f = Vector2(e, n) + along * (wheelbase_m * 0.5);
		const Vector2 r = Vector2(e, n) - along * (wheelbase_m * 0.5);
		const Vector2 l = Vector2(e, n) + left * (track_m * 0.5);
		const Vector2 rt = Vector2(e, n) - left * (track_m * 0.5);
		const double hf = ground_at(f.x, f.y), hr = ground_at(r.x, r.y);
		const double hl = ground_at(l.x, l.y), hrt = ground_at(rt.x, rt.y);
		const double tp = (std::isnan(hf) || std::isnan(hr)) ? 0.0 : std::atan2(hf - hr, wheelbase_m);
		const double tr = (std::isnan(hl) || std::isnan(hrt)) ? 0.0 : std::atan2(hl - hrt, track_m);
		const double k = 0.25;
		pitch += (tp - pitch) * k;
		roll += (tr - roll) * k;
	}
	Transform3D xf = WorldScenery::ac_transform_at(e, n, h + clearance_m, heading_deg, Vector3(1, 1, 1));
	// AC frame: +Z nose, +X left, +Y up. Nose up = rotate about +X by -pitch,
	// left side up = rotate about +Z by +roll.
	xf.basis = xf.basis * Basis(Vector3(1, 0, 0), -pitch) * Basis(Vector3(0, 0, 1), roll);
	xf.basis = xf.basis * Basis::from_scale(ac_node->get_scale());
	ac_node->set_global_transform(xf);
}

bool DriveMode::update(double delta, Object *p_jsb, int rebase_cooldown, double joy_steer, double joy_accelerate, double joy_brake, bool joy_handbrake)
{
	if (!armed)
		return false;
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (!active)
	{
		if (rebase_cooldown > 0)
			return false; // the spawn still pins the node
		capture_pose();
		active = true;
		UtilityFunctions::print(vformat("=== DRIVE MODE ON === at E=%.0f N=%.0f heading %.0f (%s / %s drive, %s / %s steer, %s handbrake)",
				e, n, heading_deg, KeyBindings::label("drive_accelerate"), KeyBindings::label("drive_brake"),
				KeyBindings::label("drive_left"), KeyBindings::label("drive_right"), KeyBindings::label("drive_handbrake")));
	}
	// JSBSim never moves the node while we drive (the rebase release and a
	// slew exit both clear the flag).
	if (fdm != nullptr && !fdm->is_slew_mode())
		fdm->set_slew_mode(true);
	// keyboard and gamepad together: the larger input wins
	const double throttle = MAX(KeyBindings::pressed("drive_accelerate") ? 1.0 : 0.0, CLAMP(joy_accelerate, 0.0, 1.0));
	const double brake = MAX(KeyBindings::pressed("drive_brake") ? 1.0 : 0.0, CLAMP(joy_brake, 0.0, 1.0));
	const double key_steer = (KeyBindings::pressed("drive_right") ? 1.0 : 0.0) - (KeyBindings::pressed("drive_left") ? 1.0 : 0.0);
	const double steer = std::abs(joy_steer) > std::abs(key_steer) ? CLAMP(joy_steer, -1.0, 1.0) : key_steer;
	step(delta, throttle, brake, steer, KeyBindings::pressed("drive_handbrake") || joy_handbrake);
	apply_pose();
	hud();
	return true;
}

void DriveMode::hud()
{
	if (speed_label != nullptr)
		speed_label->set_text(vformat("Speed: %.0f km/h%s", std::abs(speed) * 3.6, speed < -0.05 ? " (reverse)" : ""));
	if (altitude_label != nullptr)
		altitude_label->set_text(vformat("Ground: %.1f m ASL", h));
	if (vspeed_label != nullptr)
		vspeed_label->set_text(vformat(String::utf8("Steer: %+.0f°"), steer_deg));
	if (throttle_label != nullptr)
		throttle_label->set_text(vformat("%s / %s drive, %s / %s steer, %s handbrake", KeyBindings::label("drive_accelerate"), KeyBindings::label("drive_brake"),
				KeyBindings::label("drive_left"), KeyBindings::label("drive_right"), KeyBindings::label("drive_handbrake")));
	if (heading_label != nullptr)
		heading_label->set_text(vformat(String::utf8("Heading: %03.0f°"), heading_deg));
	if (heading_deg_label != nullptr)
		heading_deg_label->set_text("=== DRIVING ===");
}

} // namespace jsbgodot
