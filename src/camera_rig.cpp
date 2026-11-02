#include "key_bindings.h"
#include "joystick_bindings.h"
#include "camera_rig.h"
#include "geo_frame.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>

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
	ClassDB::bind_method(D_METHOD("set_orbit_distance", "distance"), &CameraRig::set_orbit_distance);
	ClassDB::bind_method(D_METHOD("set_orbit_yaw", "deg"), &CameraRig::set_orbit_yaw);
	ClassDB::bind_method(D_METHOD("set_orbit_pitch_min", "deg"), &CameraRig::set_orbit_pitch_min);
	ClassDB::bind_method(D_METHOD("set_orbit_pitch_max", "deg"), &CameraRig::set_orbit_pitch_max);
	ClassDB::bind_method(D_METHOD("set_chase_smooth", "smooth"), &CameraRig::set_chase_smooth);

	ClassDB::bind_method(D_METHOD("register_cameras", "camera1", "camera2", "ac_node"), &CameraRig::register_cameras);
	ClassDB::bind_method(D_METHOD("capture_cockpit_base"), &CameraRig::capture_cockpit_base);

	ClassDB::bind_method(D_METHOD("update", "delta", "joy_device"), &CameraRig::update);
	ClassDB::bind_method(D_METHOD("flip"), &CameraRig::flip);
	ClassDB::bind_method(D_METHOD("flip_back"), &CameraRig::flip_back);
	ClassDB::bind_method(D_METHOD("set_mouse_drag", "held"), &CameraRig::set_mouse_drag);
	ClassDB::bind_method(D_METHOD("is_mouse_drag_active"), &CameraRig::is_mouse_drag_active);
	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &CameraRig::set_mode);
	ClassDB::bind_method(D_METHOD("mouse_look", "relative"), &CameraRig::mouse_look);
	ClassDB::bind_method(D_METHOD("mouse_zoom", "notches"), &CameraRig::mouse_zoom);
	ClassDB::bind_method(D_METHOD("is_mouse_look_active"), &CameraRig::is_mouse_look_active);
	ClassDB::bind_method(D_METHOD("recenter_head"), &CameraRig::recenter_head);
	ClassDB::bind_method(D_METHOD("set_head_look", "yaw_deg", "pitch_deg"), &CameraRig::set_head_look);
	ClassDB::bind_method(D_METHOD("set_eye_offset", "offset"), &CameraRig::set_eye_offset);
	ClassDB::bind_method(D_METHOD("get_eye_offset"), &CameraRig::get_eye_offset);
	ClassDB::bind_method(D_METHOD("set_cockpit_move_scale", "scale"), &CameraRig::set_cockpit_move_scale);
	ClassDB::bind_method(D_METHOD("take_eye_offset_settled"), &CameraRig::take_eye_offset_settled);
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
void CameraRig::set_orbit_yaw(double deg) { orbit_yaw = deg; }

void CameraRig::set_orbit_distance(double distance)
{
	orbit_distance = distance;
	chase_distance = distance;
}
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
		cockpit_fov_default = camera1->get_fov();
	}
}

void CameraRig::mouse_look(const Vector2 &relative)
{
	if ((space_held && mode == MODE_COCKPIT) || drag_held)
	{
		pending_mouse += relative;
	}
}

void CameraRig::set_mouse_drag(bool held)
{
	if (held == drag_held)
	{
		return;
	}
	drag_held = held;
	if (held && !space_held)
	{
		pending_mouse = Vector2();
	}
	apply_mouse_mode();
}

bool CameraRig::is_mouse_drag_active() const { return drag_held; }

void CameraRig::apply_mouse_mode()
{
	Input *input = Input::get_singleton();
	if (input == nullptr)
	{
		return;
	}
	const bool grab = drag_held || (space_held && mode == MODE_COCKPIT);
	input->set_mouse_mode(grab ? Input::MOUSE_MODE_CAPTURED : Input::MOUSE_MODE_VISIBLE);
}

void CameraRig::mouse_zoom(int notches)
{
	if (!is_mouse_look_active() || camera1 == nullptr || notches == 0)
	{
		return;
	}
	space_motion_px += tap_max_px; // a wheel notch is not a tap
	double fov = camera1->get_fov() - notches * mouse_zoom_deg_per_notch;
	camera1->set_fov(Math::clamp(fov, cockpit_fov_min, cockpit_fov_max));
}

bool CameraRig::is_mouse_look_active() const
{
	return space_held && mode == MODE_COCKPIT;
}

void CameraRig::set_head_look(double yaw_deg, double pitch_deg)
{
	head_yaw = Math::clamp(yaw_deg, -head_yaw_max, head_yaw_max);
	head_pitch = Math::clamp(pitch_deg, -head_pitch_max, head_pitch_max);
}

void CameraRig::recenter_head()
{
	head_yaw = 0.0;
	head_pitch = 0.0;
	if (camera1 != nullptr)
	{
		camera1->set_fov(cockpit_fov_default);
	}
}

void CameraRig::set_eye_offset(const Vector3 &offset)
{
	eye_offset = offset;
	eye_moving = false;
	eye_settled = false;
}

Vector3 CameraRig::get_eye_offset() const { return eye_offset; }

void CameraRig::set_cockpit_move_scale(double scale)
{
	const double s = Math::max(scale, 1e-3);
	cockpit_move_speed = 0.5 * s;
	cockpit_move_limit = 1.5 * s;
}

bool CameraRig::take_eye_offset_settled()
{
	const bool r = eye_settled;
	eye_settled = false;
	return r;
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
			// Back to the cockpit with the head where it was left (the spot
			// and chase views keep their positions the same way).
			mode = MODE_COCKPIT;
			break;
	}
	apply_current_flags();
}

void CameraRig::flip_back()
{
	switch (mode)
	{
		case MODE_COCKPIT:
			mode = MODE_CHASE;
			break;
		case MODE_CHASE:
			mode = MODE_ORBIT;
			break;
		default:
			mode = MODE_COCKPIT;
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
namespace
{
	// Settings > Joystick (the JoystickConfig autoload); null in a scene
	// without it (unit smokes).
	JoystickBindings *joy_cfg()
	{
		SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
		if (tree == nullptr || tree->get_root() == nullptr)
		{
			return nullptr;
		}
		return Object::cast_to<JoystickBindings>(tree->get_root()->get_node_or_null(NodePath("JoystickConfig")));
	}

	bool joy_button(const char *action)
	{
		JoystickBindings *cfg = joy_cfg();
		return cfg != nullptr && cfg->get_button(action);
	}

	// The pad's look input through the bindings: the look_x / look_y axes
	// when bound (stick right = x +1, stick forward = up), else the look_*
	// buttons as -1 / 0 / +1. up is +1 for "look / orbit / height up".
	void joy_look(double &x, double &up)
	{
		x = 0.0;
		up = 0.0;
		JoystickBindings *cfg = joy_cfg();
		if (cfg == nullptr)
		{
			return;
		}
		if (cfg->get_bound_axis("look_x") >= 0)
		{
			x = cfg->get_axis("look_x");
		}
		else
		{
			x = (cfg->get_button("look_right") ? 1.0 : 0.0) - (cfg->get_button("look_left") ? 1.0 : 0.0);
		}
		if (cfg->get_bound_axis("look_y") >= 0)
		{
			up = -cfg->get_axis("look_y"); // Godot: stick forward = negative
		}
		else
		{
			up = (cfg->get_button("look_up") ? 1.0 : 0.0) - (cfg->get_button("look_down") ? 1.0 : 0.0);
		}
	}
} // namespace

void CameraRig::update_head_look(double delta, int joy_device)
{
	// Only apply head look when using the cockpit camera (Camera1); a
	// dedicated camera (intro zoom) suppresses this branch.
	if (!cockpit_enabled || camera1 == nullptr)
	{
		return;
	}

	(void)joy_device; // the bindings know the active pad
	double look_x = 0.0;
	double look_up = 0.0;
	joy_look(look_x, look_up); // Settings > Joystick: look_x / look_y axes or the look_* buttons
	double look_y = -look_up;  // head pitch: negative = up

	// Keyboard arrows (Settings > Keyboard).
	if (KeyBindings::pressed("view_left"))
	{
		look_x = -1.0;
	}
	else if (KeyBindings::pressed("view_right"))
	{
		look_x = 1.0;
	}
	if (KeyBindings::pressed("view_up"))
	{
		look_y = -1.0;
	}
	else if (KeyBindings::pressed("view_down"))
	{
		look_y = 1.0;
	}

	// Space: held, the mouse looks around (right = look right, down = look
	// down); a short tap without movement recentres the view.
	const bool space = KeyBindings::pressed("view_mouse_look");
	if (space && !space_held)
	{
		space_held = true;
		space_time = 0.0;
		space_motion_px = 0.0;
		pending_mouse = Vector2();
		apply_mouse_mode();
	}
	// Space held, or the right mouse button dragged: the mouse turns the head.
	if (space_held || drag_held)
	{
		if (space_held)
		{
			space_time += delta;
			space_motion_px += pending_mouse.length();
		}
		look_x += pending_mouse.x * mouse_look_deg_per_px / (head_look_speed * std::max(delta, 1e-6));
		look_y += pending_mouse.y * mouse_look_deg_per_px / (head_look_speed * std::max(delta, 1e-6));
		pending_mouse = Vector2();
	}
	if (!space && space_held)
	{
		space_held = false;
		apply_mouse_mode();
		if (space_time < tap_max_s && space_motion_px < tap_max_px)
		{
			recenter_head();
		}
	}

	// Update head angles, clamped to limits.
	head_yaw += look_x * head_look_speed * delta;
	head_pitch += look_y * head_look_speed * delta;
	head_yaw = Math::clamp(head_yaw, -head_yaw_max, head_yaw_max);
	head_pitch = Math::clamp(head_pitch, -head_pitch_max, head_pitch_max);

	// Eye point: Ctrl+arrows / Ctrl+Shift+Up-Down move the seat, Ctrl+Space resets.
	Vector3 mv;
	if (KeyBindings::pressed("view_move_left")) mv.x -= 1.0;
	if (KeyBindings::pressed("view_move_right")) mv.x += 1.0;
	if (KeyBindings::pressed("view_move_up")) mv.y += 1.0;
	if (KeyBindings::pressed("view_move_down")) mv.y -= 1.0;
	if (KeyBindings::pressed("view_move_forward")) mv.z -= 1.0;
	if (KeyBindings::pressed("view_move_back")) mv.z += 1.0;
	if (mv.length_squared() > 0.0)
	{
		eye_offset += mv * cockpit_move_speed * delta;
		eye_offset.x = Math::clamp(eye_offset.x, -cockpit_move_limit, cockpit_move_limit);
		eye_offset.y = Math::clamp(eye_offset.y, -cockpit_move_limit, cockpit_move_limit);
		eye_offset.z = Math::clamp(eye_offset.z, -cockpit_move_limit, cockpit_move_limit);
		eye_moving = true;
	}
	else if (eye_moving)
	{
		eye_moving = false;
		eye_settled = true;
	}
	if (KeyBindings::pressed("view_move_reset") && eye_offset.length_squared() > 0.0)
	{
		eye_offset = Vector3();
		eye_moving = false;
		eye_settled = true;
	}

	// Apply the seat offset and the head rotation to the camera (relative to the base transform).
	camera1->set_transform(cockpit_base);
	camera1->translate_object_local(eye_offset);
	camera1->rotate_y(Math::deg_to_rad(-head_yaw));                           // Yaw (left/right)
	camera1->rotate_object_local(Vector3(1, 0, 0), Math::deg_to_rad(-head_pitch)); // Pitch (up/down)

	// Zoom: the same inputs as the external cameras (view_closer / view_farther
	// pad buttons, Page Up / Down) narrow / widen the field of view.
	double zoom_in = 0.0;
	double zoom_out = 0.0;
	if (joy_button("view_closer") || KeyBindings::pressed("view_closer"))
		zoom_in = 1.0;
	if (joy_button("view_farther") || KeyBindings::pressed("view_farther"))
		zoom_out = 1.0;
	if (zoom_in > 0.0 || zoom_out > 0.0)
	{
		double fov = camera1->get_fov();
		fov += (zoom_out - zoom_in) * cockpit_zoom_speed * delta;
		camera1->set_fov(Math::clamp(fov, cockpit_fov_min, cockpit_fov_max));
	}
}

// External/spot camera orbiting around the aircraft. The offset is computed in
// world space (not affected by aircraft rotation).
void CameraRig::update_orbit(double delta, int joy_device)
{
	if (camera2 == nullptr || ac_node == nullptr)
	{
		return;
	}

	(void)joy_device;
	double look_x = 0.0;
	double look_y = 0.0; // positive = elevation up
	joy_look(look_x, look_y); // Settings > Joystick

	// Keyboard arrows (Settings > Keyboard).
	if (KeyBindings::pressed("view_left"))
	{
		look_x = -1.0;
	}
	else if (KeyBindings::pressed("view_right"))
	{
		look_x = 1.0;
	}
	if (KeyBindings::pressed("view_up"))
	{
		look_y = 1.0;
	}
	else if (KeyBindings::pressed("view_down"))
	{
		look_y = -1.0;
	}

	// Update orbit angles. A right-button drag adds its pixels directly:
	// mouse right swings the camera right round the aircraft, mouse up raises it.
	orbit_yaw += look_x * orbit_speed * delta;
	orbit_pitch += look_y * orbit_speed * delta;
	if (drag_held)
	{
		orbit_yaw -= pending_mouse.x * mouse_orbit_deg_per_px;
		orbit_pitch += pending_mouse.y * mouse_orbit_deg_per_px;
		pending_mouse = Vector2();
	}

	// Wrap yaw (allow full 360 rotation).
	orbit_yaw = std::fmod(orbit_yaw, 360.0);
	if (orbit_yaw < 0.0)
	{
		orbit_yaw += 360.0;
	}

	// Clamp pitch to limits.
	orbit_pitch = Math::clamp(orbit_pitch, orbit_pitch_min, orbit_pitch_max);

	// Zoom: the view_closer / view_farther pad buttons or Page Up / Down.
	double zoom_in = 0.0;
	double zoom_out = 0.0;
	if (joy_button("view_closer"))
	{
		zoom_in = 1.0;
	}
	if (joy_button("view_farther"))
	{
		zoom_out = 1.0;
	}
	if (KeyBindings::pressed("view_closer"))
	{
		zoom_in = 1.0;
	}
	if (KeyBindings::pressed("view_farther"))
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

	(void)joy_device;
	Vector3 aircraft_pos = ac_node->get_global_position();

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

	// Zoom: the same controls as the orbit camera.
	double zoom_in = 0.0;
	double zoom_out = 0.0;
	if (joy_button("view_closer"))
	{
		zoom_in = 1.0;
	}
	if (joy_button("view_farther"))
	{
		zoom_out = 1.0;
	}
	if (KeyBindings::pressed("view_closer"))
	{
		zoom_in = 1.0;
	}
	if (KeyBindings::pressed("view_farther"))
	{
		zoom_out = 1.0;
	}
	chase_distance -= zoom_in * orbit_zoom_speed * delta;
	chase_distance += zoom_out * orbit_zoom_speed * delta;
	chase_distance = Math::clamp(chase_distance, orbit_distance_min, orbit_distance_max);

	// The pad's look input (Settings > Joystick) turns the chase camera
	// (horizontal) and raises / lowers it (vertical); arrow keys do the same.
	double hat_x = 0.0;
	double hat_y = 0.0;
	joy_look(hat_x, hat_y);

	double height_delta = 0.0;
	if (hat_y != 0.0)
	{
		height_delta = hat_y;
	}
	else if (KeyBindings::pressed("view_up"))
	{
		height_delta = 1.0;
	}
	else if (KeyBindings::pressed("view_down"))
	{
		height_delta = -1.0;
	}
	chase_height += height_delta * orbit_zoom_speed * delta;
	if (drag_held)
	{
		chase_height += pending_mouse.y * mouse_chase_height_per_px;
	}
	chase_height = Math::clamp(chase_height, 0.5, 20.0);

	double yaw_delta = 0.0;
	if (hat_x != 0.0)
	{
		yaw_delta = hat_x;
	}
	else if (KeyBindings::pressed("view_left"))
	{
		yaw_delta = -1.0;
	}
	else if (KeyBindings::pressed("view_right"))
	{
		yaw_delta = 1.0;
	}
	chase_yaw += yaw_delta * orbit_speed * delta;
	if (drag_held)
	{
		chase_yaw -= pending_mouse.x * mouse_orbit_deg_per_px;
		pending_mouse = Vector2();
	}
	chase_yaw = std::fmod(chase_yaw, 360.0);
	if (chase_yaw < 0.0)
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

