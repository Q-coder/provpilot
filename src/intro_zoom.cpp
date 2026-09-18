#include "intro_zoom.h"
#include "geo_frame.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

#include "camera_rig.h"

using namespace godot;

namespace jsbgodot
{

void IntroZoom::_bind_methods()
{
	ClassDB::bind_integer_constant("IntroZoom", "Status", "STATUS_RUNNING", STATUS_RUNNING);
	ClassDB::bind_integer_constant("IntroZoom", "Status", "STATUS_FINISHED", STATUS_FINISHED);
	ClassDB::bind_method(D_METHOD("register_nodes", "ac_node", "intro_camera", "camera1", "camera2", "camera_rig"), &IntroZoom::register_nodes);
	ClassDB::bind_method(D_METHOD("start"), &IntroZoom::start);
	ClassDB::bind_method(D_METHOD("update", "delta"), &IntroZoom::update);
	ClassDB::bind_method(D_METHOD("is_playing"), &IntroZoom::is_playing);
	ClassDB::bind_method(D_METHOD("set_intro_duration", "seconds"), &IntroZoom::set_intro_duration);
	ClassDB::bind_method(D_METHOD("get_intro_duration"), &IntroZoom::get_intro_duration);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intro_duration"), "set_intro_duration", "get_intro_duration");
	ClassDB::bind_method(D_METHOD("set_intro_start_alt_m", "metres"), &IntroZoom::set_intro_start_alt_m);
	ClassDB::bind_method(D_METHOD("get_intro_start_alt_m"), &IntroZoom::get_intro_start_alt_m);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intro_start_alt_m"), "set_intro_start_alt_m", "get_intro_start_alt_m");
}

IntroZoom::IntroZoom()
{
}

void IntroZoom::register_nodes(Node3D *p_ac_node, Camera3D *p_intro_camera,
                               Camera3D *p_camera1, Camera3D *p_camera2,
                               CameraRig *p_camera_rig)
{
	ac_node = p_ac_node;
	intro_camera = p_intro_camera;
	camera1 = p_camera1;
	camera2 = p_camera2;
	camera_rig = p_camera_rig;
}

bool IntroZoom::is_playing() const
{
	return playing;
}

void IntroZoom::set_intro_duration(double p_seconds)
{
	intro_duration = MAX(0.1, p_seconds);
}

double IntroZoom::get_intro_duration() const
{
	return intro_duration;
}

void IntroZoom::set_intro_start_alt_m(double p_metres)
{
	intro_start_alt_m = MAX(1000.0, p_metres);
}

double IntroZoom::get_intro_start_alt_m() const
{
	return intro_start_alt_m;
}

Vector3 IntroZoom::spot_offset() const
{
	if (camera_rig != nullptr)
	{
		return camera_rig->orbit_offset_world();
	}
	// Defensive fallback (GDScript always registers the rig): 6 m behind.
	return Vector3(0.0, 0.0, -6.0);
}


// Up hint for the camera aim that is never parallel to the view direction:
// north while looking straight down, blending to local up as the view swings
// toward the horizon.
Vector3 IntroZoom::view_up_hint(const Vector3 &cam_pos, const Vector3 &target, double blend) const
{
	Vector3 fwd = (target - cam_pos).normalized();
	Vector3 up = up_dir;
	const geo::Geodetic g = geo::geodetic_of(target);
	const Vector3 north = geo::wun_basis(g.lat, g.lon).get_column(2);
	Vector3 hint = (north * (1.0 - blend) + up * blend).normalized();
	if (std::abs(fwd.dot(hint)) > 0.98)
	{
		hint = (std::abs(fwd.dot(up)) > 0.98) ? north : up;
	}
	return hint;
}

bool IntroZoom::start()
{
	if (ac_node == nullptr || intro_camera == nullptr)
	{
		return false;
	}

	// Start the camera straight up (local +Y) above the aircraft. Using the
	// true radial direction tilts it a few km horizontally (the aircraft sits
	// off the tangent origin), which makes the scenery managers stream tiles
	// around the wrong spot during the descent. Straight up keeps streaming
	// centred on the aircraft so detailed buildings load directly instead of
	// far-first.
	up_dir = geo::up_at(ac_node->get_global_position());
	start_pos = ac_node->get_global_position() + up_dir * intro_start_alt_m;
	elapsed = 0.0;
	playing = true;

	Vector3 ac_pos = ac_node->get_global_position();
	intro_camera->set_global_position(start_pos);
	geo::aim(intro_camera, ac_pos, view_up_hint(start_pos, ac_pos, 0.0));

	// Drive the dedicated intro camera; suppress the rig's cockpit branch
	// until the handoff back to camera2.
	if (camera1 != nullptr)
	{
		camera1->set_current(false);
	}
	if (camera2 != nullptr)
	{
		camera2->set_current(false);
	}
	intro_camera->set_current(true);
	if (camera_rig != nullptr)
	{
		camera_rig->set_cockpit_enabled(false);
	}

	UtilityFunctions::print("World3D intro zoom started (space → spot)");
	return true;
}

int IntroZoom::update(double delta)
{
	if (ac_node == nullptr || intro_camera == nullptr)
	{
		finish();
		return STATUS_FINISHED;
	}

	// Allow the user to skip the cinematic.
	Input *input = Input::get_singleton();
	if (input->is_key_pressed(KEY_ESCAPE) || input->is_key_pressed(KEY_SPACE))
	{
		finish();
		return STATUS_FINISHED;
	}

	elapsed += delta;
	double p = Math::clamp(elapsed / intro_duration, 0.0, 1.0);
	double pe = Math::smoothstep(0.0, 1.0, p); // ease in/out

	Vector3 target = ac_node->get_global_position();

	// Spot (orbit) end offset — from the C++ CameraRig, so the handoff to the
	// orbit camera is seamless.
	Vector3 spot = spot_offset();

	Vector3 start_dir = up_dir;
	Vector3 end_dir = spot.normalized();
	double start_dist = intro_start_alt_m;
	double end_dist = Math::max(spot.length(), 0.5);

	// Log distance interpolation feels linear in "orders of magnitude".
	double dist = std::exp(Math::lerp(std::log(start_dist), std::log(end_dist), pe));
	// Keep the view top-down while high, swing to the spot angle near the end.
	Vector3 dir = start_dir.slerp(end_dir, pe * pe);
	Vector3 cam_pos = target + dir * dist;
	intro_camera->set_global_position(cam_pos);

	// Robust up vector: avoid look_at degeneracy when peering straight down.
	geo::aim(intro_camera, target, view_up_hint(cam_pos, target, pe * pe));

	if (p >= 1.0)
	{
		finish();
		return STATUS_FINISHED;
	}
	return STATUS_RUNNING;
}

void IntroZoom::finish()
{
	if (!playing)
	{
		return;
	}
	playing = false;

	// Hand control to the normal orbit spot camera (it toggles the current
	// flags) and re-enable the rig's cockpit branch for later flips.
	if (camera_rig != nullptr)
	{
		camera_rig->set_mode(CameraRig::MODE_ORBIT);
		camera_rig->set_cockpit_enabled(true);
	}

	// Position camera2 at the spot immediately so the handoff has no 1-frame
	// gap.
	if (ac_node != nullptr && camera2 != nullptr)
	{
		Vector3 spot = spot_offset();
		camera2->set_global_position(ac_node->get_global_position() + spot);
		if (spot.length_squared() > 0.01)
		{
			geo::aim(camera2, ac_node->get_global_position(), geo::up_at(ac_node->get_global_position()));
		}
	}

	if (intro_camera != nullptr)
	{
		intro_camera->set_current(false);
	}

	UtilityFunctions::print("World3D intro zoom complete — spot view active");
}

} // namespace jsbgodot
