#include "cockpit_controls.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdio>

#include "control_surfaces.h"
#include "jsbgodot.h"

using namespace godot;

namespace jsbgodot
{

// Magneto switch labels (historical MAGNETO_NAMES).
static const char *const kMagnetoNames[5] = {"OFF", "R", "L", "BOTH", "START"};

void CockpitControls::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_mixture_key_step", "step"), &CockpitControls::set_mixture_key_step);
	ClassDB::bind_method(D_METHOD("set_throttle_key_step", "step"), &CockpitControls::set_throttle_key_step);
	ClassDB::bind_method(D_METHOD("set_mixture_lever_travel", "travel"), &CockpitControls::set_mixture_lever_travel);
	ClassDB::bind_method(D_METHOD("set_throttle_lever_travel", "travel"), &CockpitControls::set_throttle_lever_travel);
	ClassDB::bind_method(D_METHOD("set_flap_lever_travel", "travel"), &CockpitControls::set_flap_lever_travel);
	ClassDB::bind_method(D_METHOD("set_magneto_step_angle", "deg"), &CockpitControls::set_magneto_step_angle);
	ClassDB::bind_method(D_METHOD("set_magneto_default_position", "position"), &CockpitControls::set_magneto_default_position);
	ClassDB::bind_method(D_METHOD("set_gear_retractable", "retractable"), &CockpitControls::set_gear_retractable);
	ClassDB::bind_method(D_METHOD("set_flap_positions", "positions"), &CockpitControls::set_flap_positions);
	ClassDB::bind_method(D_METHOD("set_flap_names", "names"), &CockpitControls::set_flap_names);

	ClassDB::bind_method(D_METHOD("reset"), &CockpitControls::reset);
	ClassDB::bind_method(D_METHOD("register_levers", "mixture", "throttle", "magneto", "flap"), &CockpitControls::register_levers);
	ClassDB::bind_method(D_METHOD("register_debug_bodies", "mixture_body", "throttle_body"), &CockpitControls::register_debug_bodies);

	// Expose the Lever enum to GDScript (set_dragging arguments).
	ClassDB::bind_integer_constant("CockpitControls", "Lever", "LEVER_MIXTURE", LEVER_MIXTURE);
	ClassDB::bind_integer_constant("CockpitControls", "Lever", "LEVER_THROTTLE", LEVER_THROTTLE);
	ClassDB::bind_integer_constant("CockpitControls", "Lever", "LEVER_MAGNETO", LEVER_MAGNETO);
	ClassDB::bind_integer_constant("CockpitControls", "Lever", "LEVER_FLAP", LEVER_FLAP);

	ClassDB::bind_method(D_METHOD("update", "jsb", "surfaces"), &CockpitControls::update);

	ClassDB::bind_method(D_METHOD("get_mixture_value"), &CockpitControls::get_mixture_value);
	ClassDB::bind_method(D_METHOD("get_magneto_position"), &CockpitControls::get_magneto_position);
	ClassDB::bind_method(D_METHOD("get_flap_position"), &CockpitControls::get_flap_position);
	ClassDB::bind_method(D_METHOD("is_gear_down"), &CockpitControls::is_gear_down);
	ClassDB::bind_method(D_METHOD("set_gear_down", "down"), &CockpitControls::set_gear_down);
	ClassDB::bind_method(D_METHOD("set_dragging", "lever", "dragging"), &CockpitControls::set_dragging);

	ClassDB::bind_method(D_METHOD("set_mixture_value", "value", "jsb"), &CockpitControls::set_mixture_value);
	ClassDB::bind_method(D_METHOD("set_magneto_position", "position", "jsb"), &CockpitControls::set_magneto_position);
	ClassDB::bind_method(D_METHOD("set_flap_position", "position", "jsb"), &CockpitControls::set_flap_position);
	ClassDB::bind_method(D_METHOD("apply_throttle_visual", "throttle"), &CockpitControls::apply_throttle_visual);

	ClassDB::bind_method(D_METHOD("apply_mixture_value", "jsb"), &CockpitControls::apply_mixture_value);
	ClassDB::bind_method(D_METHOD("apply_magneto_position", "jsb"), &CockpitControls::apply_magneto_position);
	ClassDB::bind_method(D_METHOD("sync_flaps_from_jsb", "jsb"), &CockpitControls::sync_flaps_from_jsb);
}

CockpitControls::CockpitControls()
{
}

CockpitControls::~CockpitControls()
{
}

// ── Tunables ────────────────────────────────────────────────────────────────

void CockpitControls::set_mixture_key_step(double step) { mixture_key_step = step; }
void CockpitControls::set_throttle_key_step(double step) { throttle_key_step = step; }
void CockpitControls::set_mixture_lever_travel(double travel) { mixture_lever_travel = travel; }
void CockpitControls::set_throttle_lever_travel(double travel) { throttle_lever_travel = travel; }
void CockpitControls::set_flap_lever_travel(double travel) { flap_lever_travel = travel; }
void CockpitControls::set_magneto_step_angle(double deg) { magneto_step_angle = deg; }
void CockpitControls::set_magneto_default_position(int position) { magneto_default_position = position; }
void CockpitControls::set_gear_retractable(bool retractable) { gear_retractable = retractable; }

void CockpitControls::set_flap_positions(const Array &positions)
{
	flap_positions.clear();
	for (int i = 0; i < positions.size(); i++)
	{
		flap_positions.push_back(positions[i].operator double());
	}
}

void CockpitControls::set_flap_names(const PackedStringArray &names)
{
	if (names.size() > 0)
	{
		flap_names = names;
	}
}

// ── Registration ────────────────────────────────────────────────────────────

void CockpitControls::reset()
{
	mixture_lever_node = nullptr;
	throttle_lever_node = nullptr;
	magneto_switch_node = nullptr;
	flap_lever_node = nullptr;
	mixture_debug_body = nullptr;
	throttle_debug_body = nullptr;
	// Mirror the historical _resolve_gear_legs reset (gear fully down).
	gear_down = true;
}

void CockpitControls::register_levers(Node3D *p_mixture, Node3D *p_throttle, Node3D *p_magneto, Node3D *p_flap)
{
	if (p_mixture != nullptr)
	{
		mixture_lever_node = p_mixture;
		mixture_lever_base = p_mixture->get_transform();
	}
	if (p_throttle != nullptr)
	{
		throttle_lever_node = p_throttle;
		throttle_lever_base = p_throttle->get_transform();
	}
	if (p_magneto != nullptr)
	{
		magneto_switch_node = p_magneto;
		magneto_switch_base = p_magneto->get_transform();
	}
	if (p_flap != nullptr)
	{
		flap_lever_node = p_flap;
		flap_lever_base = p_flap->get_transform();
	}
}

void CockpitControls::register_debug_bodies(Node *p_mixture_body, Node *p_throttle_body)
{
	mixture_debug_body = p_mixture_body;
	throttle_debug_body = p_throttle_body;
}

// ── Per-frame update ────────────────────────────────────────────────────────

void CockpitControls::update(Object *p_jsb, ControlSurfaces *surfaces)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);

	handle_mixture_keys(fdm);
	handle_throttle_keys(fdm);
	handle_magneto_keys(fdm);
	handle_flap_keys(fdm);
	handle_gear_key(surfaces, fdm);

	sync_visuals(fdm);
}

// ── Keyboard handlers (historical _handle_*_keys) ───────────────────────────

void CockpitControls::handle_mixture_keys(JSBGodot *fdm)
{
	Input *input = Input::get_singleton();
	if (!input->is_key_pressed(KEY_M))
	{
		mixture_key_held = false;
		return;
	}

	// Debounce: only act on the frame the key is first pressed.
	if (mixture_key_held)
	{
		return; // Wait for key release before next step
	}
	mixture_key_held = true;

	// Shift+Ctrl+M = toggle collision debug visibility
	if (input->is_key_pressed(KEY_SHIFT) && input->is_key_pressed(KEY_CTRL))
	{
		toggle_mixture_debug();
		return;
	}

	double old_value = mixture_value;
	if (input->is_key_pressed(KEY_SHIFT))
	{
		// Shift+M = decrease mixture (leaner)
		mixture_value = Math::clamp(mixture_value - mixture_key_step, 0.0, 1.0);
	}
	else
	{
		// M = increase mixture (richer)
		mixture_value = Math::clamp(mixture_value + mixture_key_step, 0.0, 1.0);
	}

	if (mixture_value != old_value)
	{
		apply_mixture(fdm);
		char buf[64];
		snprintf(buf, sizeof(buf), "Mixture: %.0f%%", mixture_value * 100.0);
		UtilityFunctions::print(buf);
	}
}

void CockpitControls::handle_throttle_keys(JSBGodot *fdm)
{
	Input *input = Input::get_singleton();
	if (!input->is_key_pressed(KEY_T))
	{
		throttle_key_held = false;
		return;
	}

	if (throttle_key_held)
	{
		return;
	}
	throttle_key_held = true;

	// Shift+Ctrl+T = toggle collision debug visibility
	if (input->is_key_pressed(KEY_SHIFT) && input->is_key_pressed(KEY_CTRL))
	{
		toggle_throttle_debug();
		return;
	}

	if (fdm == nullptr)
	{
		return;
	}

	double cur = fdm->get_input_throttle();
	if (input->is_key_pressed(KEY_SHIFT))
	{
		cur = Math::clamp(cur - throttle_key_step, 0.0, 1.0);
	}
	else
	{
		cur = Math::clamp(cur + throttle_key_step, 0.0, 1.0);
	}

	fdm->set_input_throttle(static_cast<float>(cur));
	char buf[64];
	snprintf(buf, sizeof(buf), "Throttle: %.0f%%", cur * 100.0);
	UtilityFunctions::print(buf);
}

void CockpitControls::handle_magneto_keys(JSBGodot *fdm)
{
	Input *input = Input::get_singleton();
	if (!input->is_key_pressed(KEY_I))
	{
		magneto_key_held = false;
		// If at START and I released, spring back to BOTH (but not during mouse drag)
		if (magneto_position == 4 && !dragging[LEVER_MAGNETO])
		{
			magneto_position = 3;
			apply_magneto(fdm);
		}
		return;
	}

	if (magneto_key_held)
	{
		return;
	}
	magneto_key_held = true;

	int old_pos = magneto_position;
	if (input->is_key_pressed(KEY_SHIFT))
	{
		magneto_position = std::max(magneto_position - 1, 0);
	}
	else
	{
		magneto_position = std::min(magneto_position + 1, 4); // Allow going to START (4)
	}

	if (magneto_position != old_pos)
	{
		apply_magneto(fdm);
	}
}

void CockpitControls::handle_flap_keys(JSBGodot *fdm)
{
	Input *input = Input::get_singleton();
	if (!input->is_key_pressed(KEY_F))
	{
		flap_key_held = false;
		return;
	}

	if (flap_key_held)
	{
		return;
	}
	flap_key_held = true;

	int old_pos = flap_position;
	if (input->is_key_pressed(KEY_SHIFT))
	{
		flap_position = std::max(flap_position - 1, 0); // Shift+F = flaps up
	}
	else
	{
		flap_position = std::min(flap_position + 1, 3); // F = flaps down
	}

	if (flap_position != old_pos)
	{
		apply_flap(fdm);
	}
}

void CockpitControls::handle_gear_key(ControlSurfaces *surfaces, JSBGodot *fdm)
{
	if (!gear_retractable)
	{
		return;
	}
	Input *input = Input::get_singleton();
	if (!input->is_key_pressed(KEY_G))
	{
		gear_key_held = false;
		return;
	}
	if (gear_key_held)
	{
		return;
	}
	gear_key_held = true;
	gear_down = !gear_down;
	if (surfaces != nullptr)
	{
		surfaces->set_gear_down(gear_down);
	}
	if (fdm != nullptr)
	{
		fdm->set_gear_down(gear_down);
	}
	UtilityFunctions::print(String("Gear: ") + (gear_down ? "DOWN" : "UP"));
}

// ── State access (GDScript mouse-drag code) ─────────────────────────────────

double CockpitControls::get_mixture_value() const { return mixture_value; }
int CockpitControls::get_magneto_position() const { return magneto_position; }
int CockpitControls::get_flap_position() const { return flap_position; }
bool CockpitControls::is_gear_down() const { return gear_down; }
void CockpitControls::set_gear_down(bool down) { gear_down = down; }

void CockpitControls::set_dragging(int lever, bool p_dragging)
{
	if (lever >= 0 && lever < 4)
	{
		dragging[lever] = p_dragging;
	}
}

void CockpitControls::set_mixture_value(double value, Object *p_jsb)
{
	mixture_value = Math::clamp(value, 0.0, 1.0);
	apply_mixture(Object::cast_to<JSBGodot>(p_jsb));
}

void CockpitControls::set_magneto_position(int position, Object *p_jsb)
{
	magneto_position = position;
	apply_magneto(Object::cast_to<JSBGodot>(p_jsb));
}

void CockpitControls::set_flap_position(int position, Object *p_jsb)
{
	flap_position = position;
	apply_flap(Object::cast_to<JSBGodot>(p_jsb));
}

// ── Initial syncs (historical _resolve_aircraft_nodes behaviour) ────────────

void CockpitControls::apply_mixture_value(Object *p_jsb)
{
	apply_mixture(Object::cast_to<JSBGodot>(p_jsb));
}

void CockpitControls::apply_magneto_position(Object *p_jsb)
{
	apply_magneto(Object::cast_to<JSBGodot>(p_jsb));
}

void CockpitControls::sync_flaps_from_jsb(Object *p_jsb)
{
	JSBGodot *fdm = Object::cast_to<JSBGodot>(p_jsb);
	if (fdm != nullptr)
	{
		flap_position = flaps_norm_to_position(fdm->get_flaps());
	}
	// The visual apply runs even without an FDM, matching the historical code.
	apply_flap(fdm);
}

// ── Visual apply (historical _apply_* / _update_lever_visuals) ──────────────

void CockpitControls::sync_visuals(JSBGodot *fdm)
{
	// Animate mixture lever position (reflects current mixture_value).
	// Base transform = Blender rest pose for mixture = 0.0 (cutoff, pulled out);
	// push the lever forward along local -Z as the pilot enriches it.
	if (mixture_lever_node != nullptr && !dragging[LEVER_MIXTURE])
	{
		mixture_lever_node->set_transform(mixture_lever_base);
		double travel_offset = -mixture_value * mixture_lever_travel;
		mixture_lever_node->translate_object_local(Vector3(0.0, 0.0, travel_offset));
	}

	// Animate throttle lever position (reads current throttle from JSBSim).
	if (throttle_lever_node != nullptr && !dragging[LEVER_THROTTLE] && fdm != nullptr)
	{
		apply_throttle_visual(fdm->get_input_throttle());
	}

	// Animate magneto switch rotation. Base transform = Blender rest pose for
	// the BOTH position (magneto_default_position).
	if (magneto_switch_node != nullptr && !dragging[LEVER_MAGNETO])
	{
		magneto_switch_node->set_transform(magneto_switch_base);
		double angle_offset = double(magneto_position - magneto_default_position) * magneto_step_angle;
		magneto_switch_node->rotate_object_local(Vector3(0.0, 0.0, 1.0), Math::deg_to_rad(angle_offset));
	}

	// Animate flap lever position (slides along Y). Sync flap_position from the
	// commanded value so gamepad changes move the lever.
	if (flap_lever_node != nullptr && !dragging[LEVER_FLAP])
	{
		if (fdm != nullptr)
		{
			int cmd_pos = flaps_norm_to_position(fdm->get_flap_cmd());
			if (cmd_pos != flap_position)
			{
				flap_position = cmd_pos;
				if (flap_position != flap_last_printed_pos)
				{
					flap_last_printed_pos = flap_position;
					UtilityFunctions::print(String("Flaps: ") + flap_name(flap_position));
				}
			}
		}
		flap_lever_node->set_transform(flap_lever_base);
		double y_offset = -double(flap_position) * flap_lever_travel;
		flap_lever_node->translate_object_local(Vector3(0.0, y_offset, 0.0));
	}
}

void CockpitControls::apply_throttle_visual(double throttle)
{
	if (throttle_lever_node == nullptr)
	{
		return;
	}
	throttle_lever_node->set_transform(throttle_lever_base);
	double t_offset = -throttle * throttle_lever_travel;
	throttle_lever_node->translate_object_local(Vector3(0.0, 0.0, t_offset));
}

void CockpitControls::apply_mixture(JSBGodot *fdm)
{
	if (mixture_lever_node != nullptr)
	{
		mixture_lever_node->set_transform(mixture_lever_base);
		double travel_offset = -mixture_value * mixture_lever_travel;
		mixture_lever_node->translate_object_local(Vector3(0.0, 0.0, travel_offset));
	}

	// Send to JSBSim
	if (fdm != nullptr)
	{
		fdm->set_input_mixture(static_cast<float>(mixture_value));
	}
}

void CockpitControls::apply_magneto(JSBGodot *fdm)
{
	// Print only on actual change
	if (magneto_position != magneto_last_printed_pos)
	{
		magneto_last_printed_pos = magneto_position;
		const char *label = (magneto_position >= 0 && magneto_position < 5) ? kMagnetoNames[magneto_position] : "?";
		UtilityFunctions::print(String("Magneto: ") + label);
	}

	if (magneto_switch_node != nullptr)
	{
		magneto_switch_node->set_transform(magneto_switch_base);
		double angle_offset = double(magneto_position - magneto_default_position) * magneto_step_angle;
		magneto_switch_node->rotate_object_local(Vector3(0.0, 0.0, 1.0), Math::deg_to_rad(angle_offset));
	}

	// Send to JSBSim
	if (fdm != nullptr)
	{
		fdm->set_input_magnetos(magneto_position);
	}
}

void CockpitControls::apply_flap(JSBGodot *fdm)
{
	// Print only on actual change
	if (flap_position != flap_last_printed_pos)
	{
		flap_last_printed_pos = flap_position;
		UtilityFunctions::print(String("Flaps: ") + flap_name(flap_position));
	}

	// Update lever visual
	if (flap_lever_node != nullptr)
	{
		flap_lever_node->set_transform(flap_lever_base);
		double y_offset = -double(flap_position) * flap_lever_travel;
		flap_lever_node->translate_object_local(Vector3(0.0, y_offset, 0.0));
	}

	// Send to JSBSim
	if (fdm != nullptr && flap_position >= 0 && flap_position < int(flap_positions.size()))
	{
		fdm->set_flaps(static_cast<float>(flap_positions[flap_position]));
	}
}



// ── Collision-box debug toggles (Shift+Ctrl+M / Shift+Ctrl+T) ───────────────

void CockpitControls::toggle_mixture_debug()
{
	mixture_debug_visible = !mixture_debug_visible;
	if (mixture_debug_body == nullptr)
	{
		UtilityFunctions::print("No mixture lever body to debug");
		return;
	}

	MeshInstance3D *debug_mesh = Object::cast_to<MeshInstance3D>(
			mixture_debug_body->get_node_or_null(NodePath("DebugMesh")));
	if (mixture_debug_visible)
	{
		if (debug_mesh == nullptr)
		{
			debug_mesh = memnew(MeshInstance3D);
			debug_mesh->set_name("DebugMesh");
			Ref<BoxMesh> box = memnew(BoxMesh);
			box->set_size(Vector3(0.02, 0.02, 0.04)); // Same as collision shape
			debug_mesh->set_mesh(box);
			Ref<StandardMaterial3D> mat = memnew(StandardMaterial3D);
			mat->set_albedo(Color(1.0, 0.0, 0.0, 0.3));
			mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
			mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
			debug_mesh->set_material_override(mat);
			mixture_debug_body->add_child(debug_mesh);
		}
		debug_mesh->set_visible(true);
		UtilityFunctions::print("Mixture lever collision box: VISIBLE");
		if (auto *body3d = Object::cast_to<Node3D>(mixture_debug_body))
		{
			UtilityFunctions::print("  Global position: ", body3d->get_global_position());
		}
		if (mixture_lever_node != nullptr)
		{
			UtilityFunctions::print("  Lever node global pos: ", mixture_lever_node->get_global_position());
		}
	}
	else
	{
		if (debug_mesh != nullptr)
		{
			debug_mesh->set_visible(false);
		}
		UtilityFunctions::print("Mixture lever collision box: HIDDEN");
	}
}

void CockpitControls::toggle_throttle_debug()
{
	throttle_debug_visible = !throttle_debug_visible;
	if (throttle_debug_body == nullptr)
	{
		UtilityFunctions::print("No throttle lever body to debug");
		return;
	}

	MeshInstance3D *debug_mesh = Object::cast_to<MeshInstance3D>(
			throttle_debug_body->get_node_or_null(NodePath("DebugMesh")));
	if (throttle_debug_visible)
	{
		if (debug_mesh == nullptr)
		{
			debug_mesh = memnew(MeshInstance3D);
			debug_mesh->set_name("DebugMesh");
			Ref<BoxMesh> box = memnew(BoxMesh);
			box->set_size(Vector3(0.02, 0.02, 0.04));
			debug_mesh->set_mesh(box);
			Ref<StandardMaterial3D> mat = memnew(StandardMaterial3D);
			mat->set_albedo(Color(0.0, 0.0, 1.0, 0.3));
			mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
			mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
			mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
			debug_mesh->set_material_override(mat);
			throttle_debug_body->add_child(debug_mesh);
		}
		debug_mesh->set_visible(true);
		UtilityFunctions::print("Throttle lever collision box: VISIBLE");
		if (auto *body3d = Object::cast_to<Node3D>(throttle_debug_body))
		{
			UtilityFunctions::print("  Global position: ", body3d->get_global_position());
		}
	}
	else
	{
		if (debug_mesh != nullptr)
		{
			debug_mesh->set_visible(false);
		}
		UtilityFunctions::print("Throttle lever collision box: HIDDEN");
	}
}

// ── Flap mapping ────────────────────────────────────────────────────────────

String CockpitControls::flap_name(int position) const
{
	if (position >= 0 && position < flap_names.size())
	{
		return flap_names[position];
	}
	return "?";
}

int CockpitControls::flaps_norm_to_position(double norm)
{
	if (norm < 0.167)
	{
		return 0;
	}
	else if (norm < 0.5)
	{
		return 1;
	}
	else if (norm < 0.833)
	{
		return 2;
	}
	else
	{
		return 3;
	}
}

} // namespace jsbgodot

