#include "camera_rig.h"
#include "geo_frame.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/input.hpp>

#include <cmath>

using namespace godot;

namespace jsbgodot
{

void CameraRig::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_head_yaw_max", "deg"), &CameraRig::set_head_yaw_max);
	ClassDB::bind_method(D_METHOD("set_head_pitch_max", "deg"), &CameraRig::set_head_pitch_max);
	ClassDB::bind_method(D_METHOD("set_head_look_speed", "deg_per_sec"), &CameraRig::set_head_look_speed);
	ClassDB::bind_method(D_METHOD("set_orbit_speed", "deg_per_sec"), &CameraRig::set_orbit_speed);
	ClassDB::bind_method(D_METHOD("set_orbit_zoom_speed", "units_per_sec"), &CameraRig::set_orbit_zoom_speed);
	ClassDB::bind_method(D_METHOD("set_orbit_distance_min", "distance"), &CameraRig::set_orbit_distance_min);
	ClassDB::bind_method(D_METHOD("set_orbit_distance_max", "distance"), &CameraRig::set_orbit_distance_max);
	ClassDB::bind_method(D_METHOD("set_orbit_pitch_min", "deg"), &CameraRig::set_orbit_pitch_min);
	ClassDB::bind_method(D_METHOD("set_orbit_pitch_max", "deg"), &CameraRig::set_orbit_pitch_max);
	ClassDB::bind_method(D_METHOD("set_chase_smooth", "smooth"), &CameraRig::set_chase_smooth);

	ClassDB::bind_method(D_METHOD("register_cameras", "camera1", "camera2", "ac_node"), &CameraRig::register_cameras);
	ClassDB::bind_method(D_METHOD("capture_cockpit_base"), &CameraRig::capture_cockpit_base);

	ClassDB::bind_method(D_METHOD("update", "delta", "joy_device"), &CameraRig::update);
	ClassDB::bind_method(D_METHOD("flip"), &CameraRig::flip);
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &CameraRig::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &CameraRig::get_mode);
	ClassDB::bind_method(D_METHOD("set_cockpit_enabled", "enabled"), &CameraRig::set_cockpit_enabled);

	ClassDB::bind_method(D_METHOD("orbit_offset"), &CameraRig::orbit_offset);
	ClassDB::bind_method(D_METHOD("get_orbit_yaw"), &CameraRig::get_orbit_yaw);
	ClassDB::bind_method(D_METHOD("get_orbit_pitch"), &CameraRig::get_orbit_pitch);
	ClassDB::bind_method(D_METHOD("get_orbit_distance"), &CameraRig::get_orbit_distance);
}

CameraRig::CameraRig()
{
}

CameraRig::~CameraRig()
{
}

// ── Tunables ────────────────────────────────────────────────────────────────

void CameraRig::set_head_yaw_max(double deg) { head_yaw_max = deg; }
void CameraRig::set_head_pitch_max(double deg) { head_pitch_max = deg; }
void CameraRig::set_head_look_speed(double deg_per_sec) { head_look_speed = deg_per_sec; }
void CameraRig::set_orbit_speed(double deg_per_sec) { orbit_speed = deg_per_sec; }
void CameraRig::set_orbit_zoom_speed(double units_per_sec) { orbit_zoom_speed = units_per_sec; }
void CameraRig::set_orbit_distance_min(double distance) { orbit_distance_min = distance; }
void CameraRig::set_orbit_distance_max(double distance) { orbit_distance_max = distance; }
void CameraRig::set_orbit_pitch_min(double deg) { orbit_pitch_min = deg; }
void CameraRig::set_orbit_pitch_max(double deg) { orbit_pitch_max = deg; }
void CameraRig::set_chase_smooth(double smooth) { chase_smooth = smooth; }

// ── Registration ────────────────────────────────────────────────────────────

void CameraRig::register_cameras(Camera3D *p_camera1, Camera3D *p_camera2, Node3D *p_ac_node)
{
	camera1 = p_camera1;
	camera2 = p_camera2;
	ac_node = p_ac_node;
	capture_cockpit_base();
}

void CameraRig::capture_cockpit_base()
{
	if (camera1 != nullptr)
	{
		cockpit_base = camera1->get_transform();
	}
}

// ── Mode handling ───────────────────────────────────────────────────────────

void CameraRig::apply_current_flags()
{
	bool cockpit = (mode == MODE_COCKPIT);
	if (camera1 != nullptr)
	{
		camera1->set_current(cockpit);
	}
	if (camera2 != nullptr)
	{
		camera2->set_current(!cockpit);
	}
}

void CameraRig::flip()
{
	switch (mode)
	{
		case MODE_COCKPIT:
			mode = MODE_ORBIT;
			break;
		case MODE_ORBIT:
			mode = MODE_CHASE;
			break;
		default:
			mode = MODE_COCKPIT;
			// Reset head position when switching back to cockpit view.
			head_yaw = 0.0;
			head_pitch = 0.0;
			break;
	}
	apply_current_flags();
}

void CameraRig::set_mode(int p_mode)
{
	if (p_mode < MODE_COCKPIT || p_mode > MODE_CHASE)
	{
		return;
	}
	mode = p_mode;
	apply_current_flags();
}

int CameraRig::get_mode() const { return mode; }

void CameraRig::set_cockpit_enabled(bool enabled) { cockpit_enabled = enabled; }

// ── Per-frame update ────────────────────────────────────────────────────────

void CameraRig::update(double delta, int joy_device)
{
	switch (mode)
	{
		case MODE_COCKPIT:
			update_head_look(delta, joy_device);
			break;
		case MODE_ORBIT:
			update_orbit(delta, joy_device);
			break;
		case MODE_CHASE:
			update_chase(delta, joy_device);
			break;
	}
}

// Pilot head look using hat switch or D-pad. Composes the head rotation on top
// of the captured cockpit base transform (reset every frame, then re-apply).
void CameraRig::update_head_look(double delta, int joy_device)
{
	// Only apply head look when using the cockpit camera (Camera1); a
	// dedicated camera (intro zoom) suppresses this branch.
	if (!cockpit_enabled || camera1 == nullptr)
	{
		return;
	}

	Input *input = Input::get_singleton();
	double look_x = 0.0;
	double look_y = 0.0;

	// Try hat switch axes first (DirectInput hat: axis 6 = X, axis 7 = Y on
	// most joysticks).
	if (joy_device >= 0)
	{
		double hx = input->get_joy_axis(joy_device, (JoyAxis)6);
		double hy = input->get_joy_axis(joy_device, (JoyAxis)7);
		if (std::abs(hx) >= 0.5)
		{
			look_x = (hx > 0.0) ? 1.0 : -1.0;
		}
		if (std::abs(hy) >= 0.5)
		{
			look_y = (hy > 0.0) ? 1.0 : -1.0;
		}
	}

	// If no hat axis input, check D-pad buttons.
	if (std::abs(look_x) < 0.1 && std::abs(look_y) < 0.1)
	{
		int d = (joy_device >= 0) ? joy_device : 0;
		if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_LEFT))
		{
			look_x = -1.0;
		}
		else if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_RIGHT))
		{
			look_x = 1.0;
		}
		if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_UP))
		{
			look_y = -1.0; // Up on D-pad = look up (negative pitch)
		}
		else if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_DOWN))
		{
			look_y = 1.0;  // Down on D-pad = look down (positive pitch)
		}
	}

	// Also support keyboard arrows for testing.
	if (input->is_key_pressed(KEY_LEFT))
	{
		look_x = -1.0;
	}
	else if (input->is_key_pressed(KEY_RIGHT))
	{
		look_x = 1.0;
	}
	if (input->is_key_pressed(KEY_UP))
	{
		look_y = -1.0;
	}
	else if (input->is_key_pressed(KEY_DOWN))
	{
		look_y = 1.0;
	}

	// Update head angles, clamped to limits.
	head_yaw += look_x * head_look_speed * delta;
	head_pitch += look_y * head_look_speed * delta;
	head_yaw = Math::clamp(head_yaw, -head_yaw_max, head_yaw_max);
	head_pitch = Math::clamp(head_pitch, -head_pitch_max, head_pitch_max);

	// Apply rotation to camera (relative to base transform).
	camera1->set_transform(cockpit_base);
	camera1->rotate_y(Math::deg_to_rad(-head_yaw));                           // Yaw (left/right)
	camera1->rotate_object_local(Vector3(1, 0, 0), Math::deg_to_rad(-head_pitch)); // Pitch (up/down)
}

// External/spot camera orbiting around the aircraft. The offset is computed in
// world space (not affected by aircraft rotation).
void CameraRig::update_orbit(double delta, int joy_device)
{
	if (camera2 == nullptr || ac_node == nullptr)
	{
		return;
	}

	Input *input = Input::get_singleton();
	double look_x = 0.0;
	double look_y = 0.0;

	// Hat switch axes (DirectInput: axis 6 = X, axis 7 = Y).
	if (joy_device >= 0)
	{
		double hx = input->get_joy_axis(joy_device, (JoyAxis)6);
		double hy = input->get_joy_axis(joy_device, (JoyAxis)7);
		if (std::abs(hx) >= 0.5)
		{
			look_x = (hx > 0.0) ? 1.0 : -1.0;
		}
		if (std::abs(hy) >= 0.5)
		{
			look_y = -((hy > 0.0) ? 1.0 : -1.0); // hat up = positive elevation
		}
	}

	// Check D-pad buttons (fallback / gamepads).
	if (std::abs(look_x) < 0.1 && std::abs(look_y) < 0.1)
	{
		int d = (joy_device >= 0) ? joy_device : 0;
		if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_LEFT))
		{
			look_x = -1.0;
		}
		else if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_RIGHT))
		{
			look_x = 1.0;
		}
		if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_UP))
		{
			look_y = 1.0; // Up = increase elevation
		}
		else if (input->is_joy_button_pressed(d, JOY_BUTTON_DPAD_DOWN))
		{
			look_y = -1.0;
		}
	}

	// Also support keyboard arrows for testing.
	if (input->is_key_pressed(KEY_LEFT))
	{
		look_x = -1.0;
	}
	else if (input->is_key_pressed(KEY_RIGHT))
	{
		look_x = 1.0;
	}
	if (input->is_key_pressed(KEY_UP))
	{
		look_y = 1.0;
	}
	else if (input->is_key_pressed(KEY_DOWN))
	{
		look_y = -1.0;
	}

	// Update orbit angles.
	orbit_yaw += look_x * orbit_speed * delta;
	orbit_pitch += look_y * orbit_speed * delta;

	// Wrap yaw (allow full 360 rotation).
	if (orbit_yaw > 360.0)
	{
		orbit_yaw -= 360.0;
	}
	else if (orbit_yaw < 0.0)
	{
		orbit_yaw += 360.0;
	}

	// Clamp pitch to limits.
	orbit_pitch = Math::clamp(orbit_pitch, orbit_pitch_min, orbit_pitch_max);

	// Handle zoom - L1/R1 (shoulder buttons) or Page Up/Page Down.
	double zoom_in = 0.0;
	double zoom_out = 0.0;
	int dz = (joy_device >= 0) ? joy_device : 0;
	if (input->is_joy_button_pressed(dz, JOY_BUTTON_LEFT_SHOULDER))
	{
		zoom_in = 1.0;
	}
	if (input->is_joy_button_pressed(dz, JOY_BUTTON_RIGHT_SHOULDER))
	{
		zoom_out = 1.0;
	}
	if (input->is_key_pressed(KEY_PAGEUP))
	{
		zoom_in = 1.0;
	}
	if (input->is_key_pressed(KEY_PAGEDOWN))
	{
		zoom_out = 1.0;
	}

	// Update distance.
	orbit_distance -= zoom_in * orbit_zoom_speed * delta;
	orbit_distance += zoom_out * orbit_zoom_speed * delta;
	orbit_distance = Math::clamp(orbit_distance, orbit_distance_min, orbit_distance_max);

	// Set camera to world position (aircraft position + world-aligned offset).
	Vector3 world_offset = orbit_offset_world();
	Vector3 aircraft_global_pos = ac_node->get_global_position();
	camera2->set_global_position(aircraft_global_pos + world_offset);

	// Make the camera look at the aircraft centre in world space. Guard against
	// the degenerate case (zero offset → same position).
	if (world_offset.length_squared() > 0.01)
	{
		geo::aim(camera2, aircraft_global_pos, geo::up_at(aircraft_global_pos));
	}
}
// Chase camera: follows behind the aircraft, rotating with its heading.
void CameraRig::update_chase(double delta, int joy_device)
{
	if (camera2 == nullptr || ac_node == nullptr)
	{
		return;
	}

	Input *input = Input::get_singleton();
	Vector3 aircraft_pos = ac_node->get_global_position();
	int d2 = (joy_device >= 0) ? joy_device : 0;

	// Get the aircraft's forward direction projected onto the horizontal plane.
	Vector3 up = geo::up_at(aircraft_pos);
	Vector3 forward = ac_node->get_global_transform().basis.get_column(2);
	forward -= up * forward.dot(up); // project onto the local horizontal plane
	if (forward.length_squared() > 0.001)
	{
		forward = forward.normalized();
	}
	else
	{
		const geo::Geodetic g = geo::geodetic_of(aircraft_pos);
		forward = geo::wun_basis(g.lat, g.lon).get_column(2); // north if degenerate
	}

	// Handle zoom - same controls as orbit camera.
	double zoom_in = 0.0;
	double zoom_out = 0.0;
	if (input->is_joy_button_pressed(d2, JOY_BUTTON_LEFT_SHOULDER))
	{
		zoom_in = 1.0;
	}
	if (input->is_joy_button_pressed(d2, JOY_BUTTON_RIGHT_SHOULDER))
	{
		zoom_out = 1.0;
	}
	if (input->is_key_pressed(KEY_PAGEUP))
	{
		zoom_in = 1.0;
	}
	if (input->is_key_pressed(KEY_PAGEDOWN))
	{
		zoom_out = 1.0;
	}
	chase_distance -= zoom_in * orbit_zoom_speed * delta;
	chase_distance += zoom_out * orbit_zoom_speed * delta;
	chase_distance = Math::clamp(chase_distance, orbit_distance_min, orbit_distance_max);

	// Hat switch axes for chase yaw (horizontal) and height (vertical).
	double hat_x = 0.0;
	double hat_y = 0.0;
	if (joy_device >= 0)
	{
		double hx = input->get_joy_axis(joy_device, (JoyAxis)6);
		double hy = input->get_joy_axis(joy_device, (JoyAxis)7);
		if (std::abs(hx) >= 0.5)
		{
			hat_x = (hx > 0.0) ? 1.0 : -1.0;
		}
		if (std::abs(hy) >= 0.5)
		{
			hat_y = -((hy > 0.0) ? 1.0 : -1.0); // hat up = height up
		}
	}

	// Handle height adjust with D-pad up/down or arrow keys.
	double height_delta = 0.0;
	if (hat_y != 0.0)
	{
		height_delta = hat_y;
	}
	else if (input->is_joy_button_pressed(d2, JOY_BUTTON_DPAD_UP) || input->is_key_pressed(KEY_UP))
	{
		height_delta = 1.0;
	}
	else if (input->is_joy_button_pressed(d2, JOY_BUTTON_DPAD_DOWN) || input->is_key_pressed(KEY_DOWN))
	{
		height_delta = -1.0;
	}
	chase_height += height_delta * orbit_zoom_speed * delta;
	chase_height = Math::clamp(chase_height, 0.5, 20.0);

	// Handle yaw offset with D-pad left/right or arrow keys.
	double yaw_delta = 0.0;
	if (hat_x != 0.0)
	{
		yaw_delta = hat_x;
	}
	else if (input->is_joy_button_pressed(d2, JOY_BUTTON_DPAD_LEFT) || input->is_key_pressed(KEY_LEFT))
	{
		yaw_delta = -1.0;
	}
	else if (input->is_joy_button_pressed(d2, JOY_BUTTON_DPAD_RIGHT) || input->is_key_pressed(KEY_RIGHT))
	{
		yaw_delta = 1.0;
	}
	chase_yaw += yaw_delta * orbit_speed * delta;
	if (chase_yaw > 360.0)
	{
		chase_yaw -= 360.0;
	}
	else if (chase_yaw < 0.0)
	{
		chase_yaw += 360.0;
	}

	// Rotate the "behind" direction by chase_yaw around the vertical axis.
	Vector3 rotated_forward = forward.rotated(up, Math::deg_to_rad(chase_yaw));

	// Target position: behind and above the aircraft.
	Vector3 target_pos = aircraft_pos - rotated_forward * chase_distance + up * chase_height;

	// Smooth follow using lerp.
	double t = Math::clamp(chase_smooth * delta, 0.0, 1.0);
	camera2->set_global_position(camera2->get_global_position().lerp(target_pos, t));

	// Look at the aircraft.
	Vector3 to_target = aircraft_pos - camera2->get_global_position();
	if (to_target.length_squared() > 0.01)
	{
		geo::aim(camera2, aircraft_pos, up);
	}
}

// ── Orbit state accessors (used by the GDScript intro-zoom handoff) ────────

Vector3 CameraRig::orbit_offset_world() const
{
	Vector3 off = orbit_offset();
	if (ac_node != nullptr)
	{
		const geo::Geodetic g = geo::geodetic_of(ac_node->get_global_position());
		return geo::wun_basis(g.lat, g.lon).xform(off);
	}
	return off;
}


Vector3 CameraRig::orbit_offset() const
{
	double yaw_rad = Math::deg_to_rad(orbit_yaw);
	double pitch_rad = Math::deg_to_rad(orbit_pitch);

	// Spherical to Cartesian conversion (world-aligned offset).
	return Vector3(
			orbit_distance * std::cos(pitch_rad) * std::sin(yaw_rad),
			orbit_distance * std::sin(pitch_rad),
			orbit_distance * std::cos(pitch_rad) * std::cos(yaw_rad));
}

double CameraRig::get_orbit_yaw() const { return orbit_yaw; }
double CameraRig::get_orbit_pitch() const { return orbit_pitch; }
double CameraRig::get_orbit_distance() const { return orbit_distance; }

} // namespace jsbgodot

