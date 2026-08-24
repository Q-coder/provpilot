#include "slew_mode.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdio>

#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

void SlewMode::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "ac_node", "airspeed_label", "altitude_label", "vspeed_label", "throttle_label", "heading_label", "godot_pos_label", "swiss_label", "heading_deg_label"), &SlewMode::register_nodes);
	ClassDB::bind_method(D_METHOD("set_swiss_center", "easting", "northand"), &SlewMode::set_swiss_center);
	ClassDB::bind_method(D_METHOD("update", "delta", "jsb_node", "axis_strafe", "axis_forward", "axis_yaw", "axis_vertical", "terrain_height_at_ac", "joy_device"), &SlewMode::update);
	ClassDB::bind_method(D_METHOD("is_enabled"), &SlewMode::is_enabled);
	ClassDB::bind_method(D_METHOD("get_preserved_airspeed_kts"), &SlewMode::get_preserved_airspeed_kts);
	ClassDB::bind_method(D_METHOD("note_external_exit"), &SlewMode::note_external_exit);
}

SlewMode::SlewMode()
{
}

void SlewMode::register_nodes(Node3D *p_ac_node,
                              Label *p_airspeed_label, Label *p_altitude_label,
                              Label *p_vspeed_label, Label *p_throttle_label,
                              Label *p_heading_label, Label *p_godot_pos_label,
                              Label *p_swiss_label, Label *p_heading_deg_label)
{
	ac_node = p_ac_node;
	airspeed_label = p_airspeed_label;
	altitude_label = p_altitude_label;
	vspeed_label = p_vspeed_label;
	throttle_label = p_throttle_label;
	heading_label = p_heading_label;
	godot_pos_label = p_godot_pos_label;
	swiss_label = p_swiss_label;
	heading_deg_label = p_heading_deg_label;
}

void SlewMode::set_swiss_center(double easting, double northing)
{
	swiss_center_e = easting;
	swiss_center_n = northing;
}

bool SlewMode::is_enabled() const
{
	return enabled;
}

double SlewMode::get_preserved_airspeed_kts() const
{
	return pre_slew_airspeed_kts;
}

void SlewMode::note_external_exit()
{
	enabled = false;
	key_held = false;
	if (heading_deg_label != nullptr)
	{
		heading_deg_label->set_text("");
	}
}

bool SlewMode::update(double delta, Object *p_jsb, double axis_strafe, double axis_forward,
                      double axis_yaw, double axis_vertical, double terrain_height_at_ac, int joy_device)
{
	last_terrain_height = terrain_height_at_ac;
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);

	// Toggle slew mode with Shift+Y (debounced against key repeat).
	Input *input = Input::get_singleton();
	if (input->is_key_pressed(KEY_SHIFT) && input->is_key_pressed(KEY_Y))
	{
		if (!key_held)
		{
			key_held = true;
			toggle(fdm, joy_device);
		}
	}
	else
	{
		key_held = false;
	}

	if (enabled)
	{
		handle_movement(delta, axis_strafe, axis_forward, axis_yaw, axis_vertical, terrain_height_at_ac);
	}
	return enabled;
}

void SlewMode::toggle(JSBGodot *fdm, int joy_device)
{
	enabled = !enabled;

	// Tell JSBGodot to pause position updates in slew mode.
	if (fdm != nullptr)
	{
		fdm->set_slew_mode(enabled);
	}

	if (enabled)
	{
		// Save current airspeed before entering slew.
		if (fdm != nullptr)
		{
			pre_slew_airspeed_kts = fdm->get_airspeed_knots();
		}
		UtilityFunctions::print("=== SLEW MODE ENABLED ===");
		// Debug joystick state so axis mapping issues are easy to spot in the log.
		TypedArray<int> pads = Input::get_singleton()->get_connected_joypads();
		String pad_list;
		for (int i = 0; i < (int)pads.size(); i++)
		{
			if (i > 0)
			{
				pad_list += ", ";
			}
			pad_list += String::num_int64(pads[i]);
		}
		UtilityFunctions::print(String("  Connected joypads: ") + String::num_int64((int)pads.size()) + " — indices: [" + pad_list + "]");
		UtilityFunctions::print(String("  JoystickConfig active device: ") + String::num_int64(joy_device));
		if (joy_device >= 0)
		{
			for (int ax = 0; ax < 8; ax++)
			{
				double v = Input::get_singleton()->get_joy_axis(joy_device, (JoyAxis)ax);
				if (std::abs(v) > 0.01)
				{
					char buf[64];
					snprintf(buf, sizeof(buf), "    axis %d = %.3f (non-zero)", ax, v);
					UtilityFunctions::print(buf);
				}
			}
		}
		char buf[96];
		snprintf(buf, sizeof(buf), "  Saved airspeed: %.1f kts", pre_slew_airspeed_kts);
		UtilityFunctions::print(buf);
		UtilityFunctions::print("  Controls:");
		UtilityFunctions::print("    Left stick / WASD: Move horizontally");
		UtilityFunctions::print("    Right stick Y / Q,E: Move up/down");
		UtilityFunctions::print("    Right stick X: Rotate (yaw)");
		UtilityFunctions::print("    Hold Shift: Move faster (5x)");
		UtilityFunctions::print("  Press Shift+Y to exit slew mode");
	}
	else
	{
		UtilityFunctions::print("=== SLEW MODE DISABLED ===");
		// Clear the slew-only heading readout.
		if (heading_deg_label != nullptr)
		{
			heading_deg_label->set_text("");
		}
		// Re-initialize JSBSim at the new position with the saved airspeed.
		if (ac_node != nullptr && fdm != nullptr)
		{
			Vector3 pos = ac_node->get_global_position();
			double heading = ac_node->get_rotation().y;
			// Lift above ground if the aircraft sank below the surface during
			// slew. Without this, ic->RunIC() fires gear-collision forces for
			// the first frame and the aircraft bounces.
			double terr_h = last_terrain_height;
			if (!std::isnan(terr_h))
			{
				double min_y = terr_h + 2.0; // 2 m gear clearance
				if (pos.y < min_y)
				{
					pos.y = min_y;
					ac_node->set_global_position(pos);
				}
			}
			UtilityFunctions::print(String("  Reinitializing JSBSim at Godot position: X=") + String::num_real(pos.x) + " Y=" + String::num_real(pos.y) + " Z=" + String::num_real(pos.z));
			char buf[96];
			snprintf(buf, sizeof(buf), "  Heading: %.1f degrees", Math::rad_to_deg(heading));
			UtilityFunctions::print(buf);
			snprintf(buf, sizeof(buf), "  Restoring airspeed: %.1f kts", pre_slew_airspeed_kts);
			UtilityFunctions::print(buf);

			// Reinitialize with preserved airspeed (JSBSim falls back to a
			// plain reinit when NAN terrain is passed).
			fdm->reinitialize_from_godot_position_with_speed(pos, heading, pre_slew_airspeed_kts, terr_h);
		}
	}
}

void SlewMode::handle_movement(double delta, double ax_strafe, double ax_forward,
                               double ax_yaw, double ax_vertical, double terrain_h)
{
	if (ac_node == nullptr)
	{
		return;
	}

	Input *input = Input::get_singleton();

	// Speed multiplier (hold shift for faster movement).
	double speed_mult = input->is_key_pressed(KEY_SHIFT) ? slew_fast_multiplier : 1.0;

	double move_x = ax_strafe;
	double move_z = ax_forward;
	double rotate_y = ax_yaw;
	double move_y = ax_vertical;

	// Keyboard support for vertical movement.
	if (input->is_key_pressed(KEY_Q) || input->is_key_pressed(KEY_PAGEUP))
	{
		move_y = 1.0;
	}
	else if (input->is_key_pressed(KEY_E) || input->is_key_pressed(KEY_PAGEDOWN))
	{
		move_y = -1.0;
	}

	// Keyboard WASD support.
	if (input->is_key_pressed(KEY_W))
	{
		move_z = -1.0;
	}
	else if (input->is_key_pressed(KEY_S))
	{
		move_z = 1.0;
	}
	if (input->is_key_pressed(KEY_A))
	{
		move_x = -1.0;
	}
	else if (input->is_key_pressed(KEY_D))
	{
		move_x = 1.0;
	}

	// Calculate movement in world space based on aircraft heading.
	double current_rotation = ac_node->get_rotation().y;
	Vector3 forward(std::sin(current_rotation), 0.0, std::cos(current_rotation));
	Vector3 right(std::cos(current_rotation), 0.0, -std::sin(current_rotation));

	// Calculate velocity (negate move_x to fix left/right direction).
	Vector3 velocity = forward * (-move_z) * slew_speed * speed_mult;
	velocity += right * (-move_x) * slew_speed * speed_mult;
	velocity.y = move_y * slew_vertical_speed * speed_mult;

	// Apply movement.
	Vector3 pos = ac_node->get_global_position();
	pos += velocity * delta;
	ac_node->set_global_position(pos);

	// Apply rotation (yaw).
	if (std::abs(rotate_y) > yaw_deadzone)
	{
		Vector3 eul = ac_node->get_rotation();
		eul.y -= rotate_y * yaw_rate * delta; // 2 rad/s rotation speed
		ac_node->set_rotation(eul);
	}

	// Update HUD with current position.
	double swiss_e = swiss_center_e - pos.x;
	double swiss_n = swiss_center_n + pos.z;
	double heading_deg = std::fmod(Math::rad_to_deg(-ac_node->get_rotation().y) + 360.0, 360.0);

	if (airspeed_label != nullptr)
	{
		airspeed_label->set_text("=== SLEW MODE ===");
	}
	if (altitude_label != nullptr)
	{
		altitude_label->set_text(String("Height AGL: ") + String::num_real(pos.y - terrain_h, 1) + " m");
	}
	if (vspeed_label != nullptr)
	{
		vspeed_label->set_text(String("Terrain: ") + String::num_real(terrain_h, 1) + " m ASL");
	}
	if (throttle_label != nullptr)
	{
		throttle_label->set_text(String("Aircraft: ") + String::num_real(pos.y, 1) + " m ASL");
	}
	if (heading_label != nullptr)
	{
		heading_label->set_text("Shift+Y to exit");
	}
	if (godot_pos_label != nullptr)
	{
		godot_pos_label->set_text(String("Godot: X=") + String::num_real(pos.x, 1) + " Y=" + String::num_real(pos.y, 1) + " Z=" + String::num_real(pos.z, 1));
	}
	if (swiss_label != nullptr)
	{
		swiss_label->set_text(String("Swiss E: ") + format_swiss_coord(swiss_e) + ", N: " + format_swiss_coord(swiss_n));
	}
	if (heading_deg_label != nullptr)
	{
		heading_deg_label->set_text(String("Heading: ") + String::num_real(heading_deg, 0) + "\u00B0");
	}
}

String SlewMode::format_swiss_coord(double value)
{
	// Mirror of the historical GDScript helper: round to whole metres and
	// insert a "'" thousands separator every three digits (Swiss LV95 style).
	String s = String::num_int64((int64_t)std::llround(value));
	String out;
	int count = 0;
	for (int i = (int)s.length() - 1; i >= 0; i--)
	{
		if (count > 0 && count % 3 == 0)
		{
			out = String("'") + out;
		}
		out = s.substr(i, 1) + out;
		count++;
	}
	return out;
}

} // namespace jsbgodot