#include "control_surfaces.h"
#include "jsbgodot.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/shader_material.hpp>

#include <cmath>

using namespace godot;

namespace jsbgodot
{

void ControlSurfaces::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_rudder_max_angle", "deg"), &ControlSurfaces::set_rudder_max_angle);
	ClassDB::bind_method(D_METHOD("set_rudder_axis", "axis"), &ControlSurfaces::set_rudder_axis);
	ClassDB::bind_method(D_METHOD("set_aileron_max_angle", "deg"), &ControlSurfaces::set_aileron_max_angle);
	ClassDB::bind_method(D_METHOD("set_aileron_axis", "axis"), &ControlSurfaces::set_aileron_axis);
	ClassDB::bind_method(D_METHOD("set_elevator_max_angle", "deg"), &ControlSurfaces::set_elevator_max_angle);
	ClassDB::bind_method(D_METHOD("set_elevator_axis", "axis"), &ControlSurfaces::set_elevator_axis);
	ClassDB::bind_method(D_METHOD("set_flap_max_angle", "deg"), &ControlSurfaces::set_flap_max_angle);
	ClassDB::bind_method(D_METHOD("set_flap_axis", "axis"), &ControlSurfaces::set_flap_axis);
	ClassDB::bind_method(D_METHOD("set_front_wheel_max_angle", "deg"), &ControlSurfaces::set_front_wheel_max_angle);
	ClassDB::bind_method(D_METHOD("set_propeller_blur_start_rpm", "rpm"), &ControlSurfaces::set_propeller_blur_start_rpm);
	ClassDB::bind_method(D_METHOD("set_propeller_hide_blades_rpm", "rpm"), &ControlSurfaces::set_propeller_hide_blades_rpm);
	ClassDB::bind_method(D_METHOD("set_propeller_spin_axis", "axis"), &ControlSurfaces::set_propeller_spin_axis);
	ClassDB::bind_method(D_METHOD("set_gear_retract_seconds", "seconds"), &ControlSurfaces::set_gear_retract_seconds);

	ClassDB::bind_method(D_METHOD("reset"), &ControlSurfaces::reset);
	ClassDB::bind_method(D_METHOD("register_rudder", "node"), &ControlSurfaces::register_rudder);
	ClassDB::bind_method(D_METHOD("register_front_wheel", "node"), &ControlSurfaces::register_front_wheel);
	ClassDB::bind_method(D_METHOD("register_propeller", "node"), &ControlSurfaces::register_propeller);
	ClassDB::bind_method(D_METHOD("register_propeller_cone", "node"), &ControlSurfaces::register_propeller_cone);
	ClassDB::bind_method(D_METHOD("register_propeller_blur_disc", "disc", "material"), &ControlSurfaces::register_propeller_blur_disc);
	ClassDB::bind_method(D_METHOD("register_aileron_left", "node"), &ControlSurfaces::register_aileron_left);
	ClassDB::bind_method(D_METHOD("register_aileron_right", "node"), &ControlSurfaces::register_aileron_right);
	ClassDB::bind_method(D_METHOD("register_elevator_left", "node"), &ControlSurfaces::register_elevator_left);
	ClassDB::bind_method(D_METHOD("register_elevator_right", "node"), &ControlSurfaces::register_elevator_right);
	ClassDB::bind_method(D_METHOD("register_single_elevator", "node"), &ControlSurfaces::register_single_elevator);
	ClassDB::bind_method(D_METHOD("register_flap_left", "node"), &ControlSurfaces::register_flap_left);
	ClassDB::bind_method(D_METHOD("register_flap_right", "node"), &ControlSurfaces::register_flap_right);
	ClassDB::bind_method(D_METHOD("add_flap_node", "node", "sign"), &ControlSurfaces::add_flap_node);
	ClassDB::bind_method(D_METHOD("add_gear_leg", "wheel", "strut", "pivot", "axis", "retract_deg", "steer", "steer_axis", "steer_max_deg", "steer_pivot", "roll", "roll_axis", "radius_m"), &ControlSurfaces::add_gear_leg);
	ClassDB::bind_method(D_METHOD("set_gear_anim", "player", "length"), &ControlSurfaces::set_gear_anim);
	ClassDB::bind_method(D_METHOD("set_gear_down", "down"), &ControlSurfaces::set_gear_down);
	ClassDB::bind_method(D_METHOD("reset_gear", "down"), &ControlSurfaces::reset_gear);
	ClassDB::bind_method(D_METHOD("get_gear_visual_pos"), &ControlSurfaces::get_gear_visual_pos);
	ClassDB::bind_method(D_METHOD("update", "jsb", "delta"), &ControlSurfaces::update);
	ClassDB::bind_method(D_METHOD("get_visual_rpm"), &ControlSurfaces::get_visual_rpm);
}

ControlSurfaces::ControlSurfaces()
{
}

ControlSurfaces::~ControlSurfaces()
{
}

// ── Tunables ────────────────────────────────────────────────────────────────

void ControlSurfaces::set_rudder_max_angle(double deg) { rudder_max_angle = deg; }
void ControlSurfaces::set_rudder_axis(const Vector3 &axis) { rudder_axis = axis; }
void ControlSurfaces::set_aileron_max_angle(double deg) { aileron_max_angle = deg; }
void ControlSurfaces::set_aileron_axis(const Vector3 &axis) { aileron_axis = axis; }
void ControlSurfaces::set_elevator_max_angle(double deg) { elevator_max_angle = deg; }
void ControlSurfaces::set_elevator_axis(const Vector3 &axis) { elevator_axis = axis; }
void ControlSurfaces::set_flap_max_angle(double deg) { flap_max_angle = deg; }
void ControlSurfaces::set_flap_axis(const Vector3 &axis) { flap_axis = axis; }
void ControlSurfaces::set_front_wheel_max_angle(double deg) { front_wheel_max_angle = deg; }
void ControlSurfaces::set_propeller_blur_start_rpm(double rpm) { propeller_blur_start_rpm = rpm; }
void ControlSurfaces::set_propeller_hide_blades_rpm(double rpm) { propeller_hide_blades_rpm = rpm; }
void ControlSurfaces::set_propeller_spin_axis(const Vector3 &axis) { propeller_spin_axis = axis; }
void ControlSurfaces::set_gear_retract_seconds(double seconds) { gear_retract_seconds = seconds; }

double ControlSurfaces::get_visual_rpm() const { return visual_rpm; }

// ── Registration ────────────────────────────────────────────────────────────

void ControlSurfaces::reset()
{
	rudder_node = nullptr;
	front_wheel_node = nullptr;
	propeller_node = nullptr;
	propeller_cone_node = nullptr;
	propeller_blur_disc = nullptr;
	propeller_blur_material = nullptr;
	left_aileron_node = nullptr;
	right_aileron_node = nullptr;
	left_elevator_node = nullptr;
	right_elevator_node = nullptr;
	single_elevator_node = nullptr;
	left_flap_node = nullptr;
	right_flap_node = nullptr;
	flap_nodes.clear();

	gear_legs.clear();
	gear_anim_player = nullptr;
	gear_anim_length = 0.0;
	// _resolve_gear_legs() historically reset the gear to fully down on every
	// (re)resolution, so a model swap drops the gear visually as well.
	gear_down = true;
	gear_visual_pos = 1.0;
}

void ControlSurfaces::register_rudder(Node3D *node)
{
	rudder_node = node;
	if (node)
		rudder_base = node->get_transform();
}

void ControlSurfaces::register_front_wheel(Node3D *node)
{
	front_wheel_node = node;
	if (node)
		front_wheel_base = node->get_transform();
}

void ControlSurfaces::register_propeller(Node3D *node)
{
	propeller_node = node;
	if (node)
		propeller_base = node->get_transform();
}

void ControlSurfaces::register_propeller_cone(Node3D *node)
{
	propeller_cone_node = node;
	if (node)
		node->set_visible(false); // Start hidden, like the historical code
}

void ControlSurfaces::register_propeller_blur_disc(MeshInstance3D *disc, Material *material)
{
	propeller_blur_disc = disc;
	propeller_blur_material = material;
}

void ControlSurfaces::register_aileron_left(Node3D *node)
{
	left_aileron_node = node;
	if (node)
		left_aileron_base = node->get_transform();
}

void ControlSurfaces::register_aileron_right(Node3D *node)
{
	right_aileron_node = node;
	if (node)
		right_aileron_base = node->get_transform();
}

void ControlSurfaces::register_elevator_left(Node3D *node)
{
	left_elevator_node = node;
	if (node)
		left_elevator_base = node->get_transform();
}

void ControlSurfaces::register_elevator_right(Node3D *node)
{
	right_elevator_node = node;
	if (node)
		right_elevator_base = node->get_transform();
}

void ControlSurfaces::register_single_elevator(Node3D *node)
{
	single_elevator_node = node;
	if (node)
		single_elevator_base = node->get_transform();
}

void ControlSurfaces::register_flap_left(Node3D *node)
{
	left_flap_node = node;
	if (node)
		left_flap_base = node->get_transform();
}

void ControlSurfaces::register_flap_right(Node3D *node)
{
	right_flap_node = node;
	if (node)
		right_flap_base = node->get_transform();
}

void ControlSurfaces::add_flap_node(Node3D *node, double sign)
{
	if (!node)
		return;
	FlapNode fn;
	fn.node = node;
	fn.base = node->get_transform();
	fn.sign = sign;
	flap_nodes.push_back(fn);
}

void ControlSurfaces::add_gear_leg(Node3D *wheel, Node3D *strut, const Vector3 &pivot,
		const Vector3 &axis, double retract_deg, bool steer,
		const Vector3 &steer_axis, double steer_max_deg,
		const Vector3 &steer_pivot, bool roll, const Vector3 &roll_axis,
		double radius_m)
{
	GearLeg leg;
	leg.wheel = wheel;
	leg.strut = strut;
	leg.wheel_base = wheel ? wheel->get_transform() : Transform3D();
	leg.strut_base = strut ? strut->get_transform() : Transform3D();
	leg.pivot = pivot;
	leg.axis = axis;
	leg.retract_deg = retract_deg;
	leg.steer = steer;
	leg.steer_axis = steer_axis;
	leg.steer_max_deg = steer_max_deg;
	leg.steer_pivot = steer_pivot;
	leg.roll = roll;
	leg.roll_axis = roll_axis;
	leg.radius = radius_m;
	gear_legs.push_back(leg);
}

void ControlSurfaces::set_gear_anim(AnimationPlayer *player, double length)
{
	gear_anim_player = player;
	gear_anim_length = length;
}

// ── Gear command ────────────────────────────────────────────────────────────

void ControlSurfaces::reset_gear(bool down)
{
	gear_down = down;
	gear_visual_pos = down ? 1.0 : 0.0;
}

double ControlSurfaces::get_gear_visual_pos() const { return gear_visual_pos; }

void ControlSurfaces::set_gear_down(bool down)
{
	gear_down = down;
}


// ── Per-frame animation ─────────────────────────────────────────────────────

Transform3D ControlSurfaces::apply_steer(const Transform3D &base, const Basis &steer,
		const Vector3 &pivot, bool enabled)
{
	if (!enabled)
		return base;
	// Rigid rotation about `pivot`: rotate the wrapper frame's basis and carry
	// the origin with it (origin' = pivot + steer * (origin - pivot)).
	return Transform3D(steer * base.basis, pivot + steer.xform(base.origin - pivot));
}

double ControlSurfaces::wheel_roll_delta(const JSBGodot *fdm, double delta) const
{
	if (!fdm->get_weight_on_wheels())
		return 0.0;
	// Ground speed is reported in ft/s; convert to metres for the roll angle.
	return fdm->get_ground_speed_fps() * 0.3048 * delta;
}

void ControlSurfaces::update(Object *jsb, double delta)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(jsb);
	if (!fdm)
		return;

	// Rudder (the nose wheel follows the same input in the gear section).
	if (rudder_node)
	{
		double angle = -fdm->get_input_rudder() * rudder_max_angle;
		rudder_node->set_transform(rudder_base);
		rudder_node->rotate_object_local(rudder_axis, Math::deg_to_rad(angle));
	}

	// Legacy nose-wheel steering for airframes without gear-leg steer: the
	// wheel turns opposite the rudder about the local up axis.
	if (front_wheel_node)
	{
		double angle = fdm->get_input_rudder() * front_wheel_max_angle;
		front_wheel_node->set_transform(front_wheel_base);
		front_wheel_node->rotate_object_local(Vector3(0, 1, 0), Math::deg_to_rad(angle));
	}

	// Propeller: smooth the visual RPM (fast decay when the engine is off),
	// spin the blades and toggle the spinner cone / blur disc.
	if (propeller_node)
	{
		double rpm = fdm->get_propeller_rpm();
		if (fdm->get_engine_running())
			visual_rpm = rpm;
		else
			visual_rpm = Math::move_toward(visual_rpm, 0.0,
					visual_rpm * PROPELLER_DECAY_RATE * delta + 5.0);

		double degrees_per_second = visual_rpm * 6.0; // RPM -> deg/s
		propeller_rotation += degrees_per_second * delta;
		propeller_rotation = std::fmod(propeller_rotation, 360.0);

		propeller_node->set_transform(propeller_base);
		propeller_node->rotate_object_local(propeller_spin_axis,
				Math::deg_to_rad(propeller_rotation));

		if (propeller_cone_node)
		{
			bool blades_visible = visual_rpm < propeller_hide_blades_rpm;
			propeller_node->set_visible(blades_visible);
			propeller_cone_node->set_visible(!blades_visible);
		}
		else
		{
			propeller_node->set_visible(true);
		}

		if (propeller_blur_disc)
		{
			if (auto *shader = Object::cast_to<ShaderMaterial>(propeller_blur_material))
				shader->set_shader_parameter("rpm", visual_rpm);
			propeller_blur_disc->set_visible(visual_rpm >= propeller_blur_start_rpm);
		}
	}

	// Ailerons: opposite deflections, same hinge axis.
	double ai = fdm->get_input_aileron();
	if (left_aileron_node)
	{
		left_aileron_node->set_transform(left_aileron_base);
		left_aileron_node->rotate_object_local(aileron_axis,
				Math::deg_to_rad(ai * aileron_max_angle));
	}
	if (right_aileron_node)
	{
		right_aileron_node->set_transform(right_aileron_base);
		right_aileron_node->rotate_object_local(aileron_axis,
				Math::deg_to_rad(-ai * aileron_max_angle));
	}

	// Elevator: the L/R pair rotates about the X euler angle (not a local axis).
	double ei = fdm->get_input_elevator();
	if (left_elevator_node)
	{
		double angle = -ei * elevator_max_angle;
		Vector3 e = left_elevator_base.basis.get_euler();
		left_elevator_node->set_transform(left_elevator_base);
		left_elevator_node->set_rotation(
				Vector3(e.x + Math::deg_to_rad(angle), e.y, e.z));
	}
	if (right_elevator_node)
	{
		// Same signed angle as the left side (both rotate together in pitch;
		// the meshes are mirrored, so identical angles read as opposite motion).
		double angle = -ei * elevator_max_angle;
		Vector3 e = right_elevator_base.basis.get_euler();
		right_elevator_node->set_transform(right_elevator_base);
		right_elevator_node->set_rotation(
				Vector3(e.x + Math::deg_to_rad(angle), e.y, e.z));
	}
	if (single_elevator_node)
	{
		double angle = ei * elevator_max_angle;
		single_elevator_node->set_transform(single_elevator_base);
		single_elevator_node->rotate_object_local(elevator_axis,
				Math::deg_to_rad(angle));
	}

	// Flaps: symmetric L/R pair (X euler) plus the generic split-flap list.
	double fi = fdm->get_flaps();
	if (left_flap_node)
	{
		double angle = -fi * flap_max_angle;
		Vector3 e = left_flap_base.basis.get_euler();
		left_flap_node->set_transform(left_flap_base);
		left_flap_node->set_rotation(
				Vector3(e.x + Math::deg_to_rad(angle), e.y, e.z));
	}
	if (right_flap_node)
	{
		// Same signed angle as the left side (mirrored meshes, down-only).
		double angle = -fi * flap_max_angle;
		Vector3 e = right_flap_base.basis.get_euler();
		right_flap_node->set_transform(right_flap_base);
		right_flap_node->set_rotation(
				Vector3(e.x + Math::deg_to_rad(angle), e.y, e.z));
	}
	for (const FlapNode &fn : flap_nodes)
	{
		double angle = fn.sign * fi * flap_max_angle;
		fn.node->set_transform(fn.base);
		fn.node->rotate_object_local(flap_axis, Math::deg_to_rad(angle));
	}

	update_gear(fdm, delta);
}

void ControlSurfaces::update_gear(const JSBGodot *fdm, double delta)
{
	if (gear_legs.empty() && !gear_anim_player)
		return;

	double target = gear_down ? 1.0 : 0.0;
	if (gear_visual_pos != target)
	{
		double step = delta / std::fmax(gear_retract_seconds, 0.01);
		gear_visual_pos = Math::move_toward(gear_visual_pos, target, step);
	}
	double retract_frac = 1.0 - gear_visual_pos;

	// Imported clip mode: seek the player instead of rotating the legs.
	if (gear_anim_player)
	{
		gear_anim_player->seek(retract_frac * gear_anim_length, true);
		double roll_delta = wheel_roll_delta(fdm, delta);
		for (GearLeg &leg : gear_legs)
		{
			if (!leg.wheel)
				continue;
			Basis basis = leg.wheel_base.basis;
			if (leg.roll)
			{
				leg.spin = Math::wrapf(leg.spin + roll_delta / leg.radius,
						-Math_TAU, Math_TAU);
				basis = basis * Basis(leg.roll_axis, (real_t)leg.spin);
			}
			if (leg.steer)
			{
				double steer_rad = Math::deg_to_rad(
						fdm->get_input_rudder() * leg.steer_max_deg);
				Basis steer_basis(leg.steer_axis, (real_t)steer_rad);
				Vector3 origin = leg.wheel_base.origin;
				leg.wheel->set_transform(Transform3D(steer_basis * basis,
						leg.steer_pivot + steer_basis.xform(origin - leg.steer_pivot)));
			}
			else
			{
				leg.wheel->set_transform(Transform3D(basis, leg.wheel_base.origin));
			}
		}
		return;
	}

	// Manual mode: rotate wheel + strut rigidly about the shared pivot.
	for (const GearLeg &leg : gear_legs)
	{
		Basis rot(leg.axis, Math::deg_to_rad(leg.retract_deg * retract_frac));
		Vector3 pivot = leg.pivot;

		Basis steer_basis; // identity unless this leg steers
		if (leg.steer)
			steer_basis = Basis(leg.steer_axis,
					Math::deg_to_rad(fdm->get_input_rudder() * leg.steer_max_deg));

		if (leg.wheel)
		{
			Transform3D st = apply_steer(leg.wheel_base, steer_basis,
					leg.steer_pivot, leg.steer);
			leg.wheel->set_transform(Transform3D(rot * st.basis,
					pivot + rot.xform(st.origin - pivot)));
		}
		if (leg.strut)
		{
			Transform3D st = apply_steer(leg.strut_base, steer_basis,
					leg.steer_pivot, leg.steer);
			leg.strut->set_transform(Transform3D(rot * st.basis,
					pivot + rot.xform(st.origin - pivot)));
		}
	}
}

} // namespace jsbgodot

