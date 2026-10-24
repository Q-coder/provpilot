// scenery_editor_objects.cpp — the scenery editor's Object tool: selection of
// pack models (SceneryObjects), the Blender-style transform gizmo with Global /
// Local orientation and median / individual pivots, the transform panel
// (typed location, rotation, scale, dimensions, pivot) and the object list.
#include "key_bindings.h"
#include <godot_cpp/classes/input_event_key.hpp>
#include "scenery_editor.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

#include "geo.h"
#include "geo_frame.h"
#include "scenery_objects.h"

using namespace godot;

namespace jsbgodot
{

// ── selection ─────────────────────────────────────────────────────────────

int SceneryEditor::select_object_at(const Vector2 &screen, bool add)
{
	SceneryObjects *so = objects();
	if (so == nullptr || cam == nullptr)
		return -1;
	const int idx = so->pick_ray(cam->project_ray_origin(screen), cam->project_ray_normal(screen));
	if (idx >= 0)
		select_object(idx, add);
	else if (!add)
		clear_selection();
	return idx;
}

void SceneryEditor::select_object(int idx, bool add)
{
	if (!add)
		selection.clear();
	if (idx >= 0 && std::find(selection.begin(), selection.end(), idx) == selection.end())
		selection.push_back(idx);
	selected_object = idx;
	selection_changed();
}

void SceneryEditor::add_to_selection(int idx) { select_object(idx, true); }

void SceneryEditor::clear_selection()
{
	selection.clear();
	selected_object = -1;
	gizmo_axis = AXIS_NONE;
	selection_changed();
}

Array SceneryEditor::get_selection() const
{
	Array out;
	for (int i : selection)
		out.push_back(i);
	return out;
}

int SceneryEditor::get_selected_object() const { return selected_object; }

void SceneryEditor::set_selected_object(int v) { select_object(v, false); }

// Everything that shows the selection: gizmo, footprint rings, panel, list.
void SceneryEditor::selection_changed()
{
	// tint the selected models (the outline box marks the active one)
	if (SceneryObjects *so = objects())
	{
		for (int idx : highlighted)
			if (std::find(selection.begin(), selection.end(), idx) == selection.end())
				so->set_object_highlight(idx, false);
		for (int idx : selection)
			so->set_object_highlight(idx, true);
		highlighted = selection;
	}
	update_gizmo();
	update_preview();
	refresh_object_panel();
	refresh_object_list();
}

// The point the gizmo sits on and rotations / scalings happen about (ECEF).
Vector3 SceneryEditor::selection_pivot() const
{
	SceneryObjects *so = objects();
	if (so == nullptr || selection.empty())
		return Vector3();
	if (pivot_mode == PIVOT_INDIVIDUAL && selected_object >= 0)
	{
		const Dictionary d = so->get_object_info(selected_object);
		if (d.has("ecef"))
			return d["ecef"];
	}
	Vector3 sum;
	int n = 0;
	for (int idx : selection)
	{
		const Dictionary d = so->get_object_info(idx);
		if (d.has("ecef"))
		{
			sum += Vector3(d["ecef"]);
			n++;
		}
	}
	return n > 0 ? sum / double(n) : Vector3();
}

// ── gizmo ─────────────────────────────────────────────────────────────────

// Screen-space handles of the selection, recomputed every frame: constant
// size (GIZMO_PX at the pivot's depth), axes of the chosen orientation.
void SceneryEditor::update_gizmo()
{
	gizmo_valid = false;
	gizmo_handles.clear();
	SceneryObjects *so = objects();
	if (tool != TOOL_OBJECT || so == nullptr || selection.empty() || cam == nullptr)
	{
		if (gizmo_overlay)
			gizmo_overlay->queue_redraw();
		return;
	}
	const Dictionary d = selected_object >= 0 ? so->get_object_info(selected_object) : Dictionary();
	if (d.is_empty() || bool(d["removed"]) || !bool(d["loaded"]))
	{
		if (gizmo_overlay)
			gizmo_overlay->queue_redraw();
		return;
	}
	gizmo_origin = selection_pivot();
	if (cam->is_position_behind(gizmo_origin))
	{
		if (gizmo_overlay)
			gizmo_overlay->queue_redraw();
		return;
	}
	const geo::Geodetic g = geo::geodetic_of(gizmo_origin);
	const Basis wun = geo::wun_basis(g.lat, g.lon);
	gizmo_east = -wun.get_column(0);
	gizmo_up = wun.get_column(1);
	gizmo_north = wun.get_column(2);
	if (orientation == ORIENT_LOCAL)
	{
		const Basis model = wun * Basis(d["rot"]);
		const Basis rot = d["rot"];
		for (int i = 0; i < 3; i++)
		{
			gizmo_axes[i] = model.get_column(i).normalized();
			gizmo_axes_local[i] = rot.get_column(i).normalized();
		}
	}
	else
	{
		gizmo_axes[0] = gizmo_east;
		gizmo_axes[1] = gizmo_north;
		gizmo_axes[2] = gizmo_up;
		gizmo_axes_local[0] = Vector3(-1, 0, 0);
		gizmo_axes_local[1] = Vector3(0, 0, 1);
		gizmo_axes_local[2] = Vector3(0, 1, 0);
	}
	// world length that shows as GIZMO_PX pixels at the pivot
	const double vh = host->get_viewport()->get_visible_rect().size.y;
	const double focal_px = (vh * 0.5) / std::tan(Math::deg_to_rad(cam->get_fov()) * 0.5);
	gizmo_size_m = MAX(GIZMO_PX * pos.distance_to(gizmo_origin) / MAX(focal_px, 1.0), 0.05);
	gizmo_valid = true;
	const double L = gizmo_size_m;
	const Vector2 o = cam->unproject_position(gizmo_origin);
	const Color cols[3] = {Color(0.93, 0.28, 0.32), Color(0.48, 0.80, 0.22), Color(0.30, 0.55, 0.98)};
	auto add_stub = [&](int axis, int kind, const Vector3 &dir3, const Color &col) {
		const Vector3 tip3 = gizmo_origin + dir3 * L;
		if (cam->is_position_behind(tip3))
			return;
		GizmoHandle h;
		h.axis = axis;
		h.kind = kind;
		const Vector2 t = cam->unproject_position(tip3);
		h.pts.push_back(o + (t - o) * 0.18);
		h.pts.push_back(t);
		h.dir = (t - o).length() > 1e-3 ? (t - o).normalized() : Vector2(1, 0);
		h.col = col;
		gizmo_handles.push_back(h);
	};
	auto add_centre = [&](int axis) {
		GizmoHandle c;
		c.axis = axis;
		c.kind = 3;
		c.pts.push_back(o);
		c.col = Color(0.95, 0.95, 0.95);
		gizmo_handles.push_back(c);
	};
	if (gizmo_mode == GIZMO_MOVE)
	{
		for (int i = 0; i < 3; i++)
			add_stub(AXIS_EAST + i, 0, gizmo_axes[i], cols[i]);
		add_centre(AXIS_FREE);
	}
	else if (gizmo_mode == GIZMO_ROTATE)
	{
		for (int i = 0; i < 3; i++)
		{
			const Vector3 a = gizmo_axes[i];
			const Vector3 u = gizmo_axes[(i + 1) % 3], v = gizmo_axes[(i + 2) % 3];
			GizmoHandle h;
			h.axis = AXIS_ROT_EAST + i;
			h.kind = 1;
			h.col = cols[i];
			// a ring seen edge-on is a line through the centre: drawn faint, not pickable (Blender fades it too)
			h.facing = std::abs(a.dot((pos - gizmo_origin).normalized()));
			bool ok = true;
			for (int k = 0; k <= 64 && ok; k++)
			{
				const double ang = Math_TAU * k / 64.0;
				const Vector3 w = gizmo_origin + (u * std::cos(ang) + v * std::sin(ang)) * L;
				if (cam->is_position_behind(w))
					ok = false;
				else
					h.pts.push_back(cam->unproject_position(w));
			}
			if (ok)
				gizmo_handles.push_back(h);
		}
	}
	else
	{
		for (int i = 0; i < 3; i++)
			add_stub(AXIS_SCALE_X + i, 2, gizmo_axes[i], cols[i]);
		add_centre(AXIS_UNIFORM);
	}
	if (gizmo_overlay)
		gizmo_overlay->queue_redraw();
}

const std::vector<SceneryEditor::GizmoHandle> &SceneryEditor::get_gizmo_handles() const { return gizmo_handles; }
int SceneryEditor::get_gizmo_active() const { return gizmo_axis != AXIS_NONE ? gizmo_axis : gizmo_hover; }

// The handle under a screen point (AXIS_* of the handle), or AXIS_NONE.
int SceneryEditor::gizmo_hit(const Vector2 &screen) const
{
	if (!gizmo_valid)
		return AXIS_NONE;
	// the centre circle first: axis stubs seen end-on collapse onto it
	for (const GizmoHandle &h : gizmo_handles)
		if (h.kind == 3 && h.pts[0].distance_to(screen) <= 12.0)
			return h.axis;
	int best = AXIS_NONE;
	double best_d = 12.0;
	for (const GizmoHandle &h : gizmo_handles)
	{
		double d = 1e9;
		if (h.kind == 3)
			continue;
		if (h.kind == 1)
		{
			if (h.facing < 0.15)
				continue;
			for (int k = 1; k < h.pts.size(); k++)
				d = MIN(d, SceneryDesign::nearest_on_segment(h.pts[k - 1], h.pts[k], screen).distance_to(screen));
		}
		else
			d = MIN(SceneryDesign::nearest_on_segment(h.pts[0], h.pts[1], screen).distance_to(screen), h.pts[1].distance_to(screen) - 6.0);
		if (d < best_d)
		{
			best_d = d;
			best = h.axis;
		}
	}
	return best;
}

// Start a handle drag of the selection: one undo group, the start pose of
// every selected object, the pivot and the axes as they are now.
void SceneryEditor::gizmo_begin(int axis, const Vector2 &screen)
{
	SceneryObjects *so = objects();
	if (so == nullptr || selection.empty())
		return;
	update_gizmo();
	so->begin_edit_group();
	drag_indices = selection;
	drag_starts.clear();
	for (int idx : drag_indices)
	{
		so->begin_edit(idx);
		drag_starts.push_back(so->get_object_info(idx));
	}
	so->end_edit_group();
	gizmo_axis = axis;
	gizmo_start_mouse = screen;
	gizmo_last_mouse = screen;
	gizmo_eff_mouse = screen;
	gizmo_moved = false;
	gizmo_start = selected_object >= 0 ? so->get_object_info(selected_object) : Dictionary();
	drag_pivot = gizmo_origin;
	for (int i = 0; i < 3; i++)
	{
		drag_axes[i] = gizmo_axes[i];
		drag_axes_local[i] = gizmo_axes_local[i];
	}
	gizmo_plane_origin = gizmo_origin;
	gizmo_plane_up = gizmo_up;
	if (!ground_plane_hit(screen, gizmo_plane_start))
		gizmo_plane_start = gizmo_origin;
}

// Where the mouse ray meets the horizontal plane through the drag pivot.
bool SceneryEditor::ground_plane_hit(const Vector2 &screen, Vector3 &out) const
{
	if (cam == nullptr || gizmo_plane_up.length_squared() < 0.5)
		return false;
	const Vector3 o = cam->project_ray_origin(screen);
	const Vector3 d = cam->project_ray_normal(screen);
	const double denom = d.dot(gizmo_plane_up);
	if (std::abs(denom) < 1e-6)
		return false;
	const double t = (gizmo_plane_origin - o).dot(gizmo_plane_up) / denom;
	if (t <= 0.0)
		return false;
	out = o + d * t;
	return true;
}

// Object idx at its start pose moved by an ECEF vector: the new placement
// point is converted exactly (the LV95 grid is turned against true north by
// the meridian convergence), the height above ground follows the height
// change of the point, so the object keeps riding the terrain.
void SceneryEditor::place_from_start(int idx, const Dictionary &start, const Vector3 &delta_ecef, const Basis &rot, const Vector3 &scale)
{
	SceneryObjects *so = objects();
	if (so == nullptr)
		return;
	if (!start.has("ecef") || delta_ecef.length_squared() < 1e-12)
	{
		so->set_object_transform(idx, double(start["e"]), double(start["n"]), double(start["agl_m"]), rot, scale, false);
		return;
	}
	const Vector3 base = start["ecef"];
	const Vector3 target = base + delta_ecef;
	const geo::Geodetic g0 = geo::geodetic_of(base), g1 = geo::geodetic_of(target);
	const Vector2 en = Geo::wgs84_to_lv95(g1.lat, g1.lon);
	so->set_object_transform(idx, en.x, en.y, double(start["agl_m"]) + (g1.h - g0.h), rot, scale, false);
}

// Move every object of `idx` from its start pose by a world (ECEF) vector.
void SceneryEditor::move_selected(const Vector3 &delta_ecef, const std::vector<Dictionary> &starts, const std::vector<int> &idx)
{
	for (size_t k = 0; k < idx.size() && k < starts.size(); k++)
		place_from_start(idx[k], starts[k], delta_ecef, Basis(starts[k]["rot"]), Vector3(starts[k]["scale"]));
}

// Apply the (virtual) mouse position of a handle drag to the selection.
void SceneryEditor::gizmo_drag(const Vector2 &screen)
{
	SceneryObjects *so = objects();
	if (so == nullptr || drag_indices.empty() || !gizmo_valid)
		return;
	const Vector2 o = cam->unproject_position(drag_pivot);
	if (gizmo_axis == AXIS_EAST || gizmo_axis == AXIS_NORTH || gizmo_axis == AXIS_UP)
	{
		const Vector3 a = drag_axes[gizmo_axis - AXIS_EAST];
		const Vector2 t = cam->unproject_position(drag_pivot + a * gizmo_size_m) - o;
		const double len = t.length();
		if (len < 1e-3)
			return;
		const double metres = (screen - gizmo_start_mouse).dot(t / len) * gizmo_size_m / len;
		move_selected(a * metres, drag_starts, drag_indices);
	}
	else if (gizmo_axis == AXIS_FREE)
	{
		Vector3 hit;
		if (!ground_plane_hit(screen, hit))
			return;
		move_selected(hit - gizmo_plane_start, drag_starts, drag_indices);
	}
	else if (gizmo_axis >= AXIS_ROT_EAST && gizmo_axis <= AXIS_ROT_UP)
	{
		const int i = gizmo_axis - AXIS_ROT_EAST;
		const Vector2 v0 = gizmo_start_mouse - o, v1 = screen - o;
		if (v0.length() < 2.0 || v1.length() < 2.0)
			return;
		double da = std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x); // screen y down: clockwise positive
		// seen from the axis' positive side a clockwise screen turn is a negative (right-hand) rotation
		if (drag_axes[i].dot(pos - drag_pivot) > 0.0)
			da = -da;
		const Basis r_local(drag_axes_local[i], da); // for the objects' own rotation
		const Basis r_world(drag_axes[i], da);       // for their positions about the pivot
		for (size_t k = 0; k < drag_indices.size(); k++)
		{
			const Dictionary &st = drag_starts[k];
			Vector3 delta;
			if (pivot_mode == PIVOT_MEDIAN && st.has("ecef"))
			{
				const Vector3 v = Vector3(st["ecef"]) - drag_pivot;
				delta = r_world.xform(v) - v;
			}
			place_from_start(drag_indices[k], st, delta, r_local * Basis(st["rot"]), Vector3(st["scale"]));
		}
	}
	else if (gizmo_axis >= AXIS_SCALE_X && gizmo_axis <= AXIS_UNIFORM)
	{
		const double r0 = (gizmo_start_mouse - o).length(), r1 = (screen - o).length();
		if (r0 < 4.0)
			return;
		const double ratio = r1 / r0;
		Vector3 f(1, 1, 1);
		if (gizmo_axis == AXIS_UNIFORM)
			f = Vector3(ratio, ratio, ratio);
		else
			f[gizmo_axis - AXIS_SCALE_X] = ratio;
		for (size_t k = 0; k < drag_indices.size(); k++)
		{
			const Dictionary &st = drag_starts[k];
			Vector3 sc = st["scale"];
			if (orientation == ORIENT_LOCAL || gizmo_axis == AXIS_UNIFORM)
				sc *= f;
			else
			{
				// a world-axis scale on a turned model: the model axis most aligned with it
				const Basis model = Basis(st["rot"]);
				for (int a = 0; a < 3; a++)
				{
					if (std::abs(f[a] - 1.0) < 1e-9)
						continue;
					int best = 0;
					double best_dot = -1.0;
					for (int m = 0; m < 3; m++)
					{
						const double dt = std::abs(model.get_column(m).normalized().dot(drag_axes_local[a]));
						if (dt > best_dot)
						{
							best_dot = dt;
							best = m;
						}
					}
					sc[best] *= f[a];
				}
			}
			Vector3 delta;
			if (pivot_mode == PIVOT_MEDIAN && st.has("ecef"))
			{
				const Vector3 v = Vector3(st["ecef"]) - drag_pivot;
				Vector3 v2;
				for (int a = 0; a < 3; a++)
					v2 += drag_axes[a] * (v.dot(drag_axes[a]) * f[a]);
				delta = v2 - v;
			}
			place_from_start(drag_indices[k], st, delta, Basis(st["rot"]), sc);
		}
	}
	else
		return;
	tool_note = "";
	update_gizmo();
	update_preview();
}

Vector2 SceneryEditor::get_gizmo_handle(int axis) const
{
	for (const GizmoHandle &h : gizmo_handles)
		if (h.axis == axis)
			return h.kind == 1 ? h.pts[0] : h.pts[h.pts.size() - 1];
	return Vector2(-1, -1);
}

double SceneryEditor::get_gizmo_size_m() const { return gizmo_size_m; }
int SceneryEditor::gizmo_hit_test(const Vector2 &screen) const { return gizmo_hit(screen); }
int SceneryEditor::get_orientation() const { return orientation; }

void SceneryEditor::set_orientation(int o)
{
	orientation = o == ORIENT_LOCAL ? ORIENT_LOCAL : ORIENT_GLOBAL;
	if (orient_option && orient_option->get_selected() != orientation)
		orient_option->select(orientation);
	update_gizmo();
}

int SceneryEditor::get_pivot_mode() const { return pivot_mode; }

void SceneryEditor::set_pivot_mode(int m)
{
	pivot_mode = m == PIVOT_INDIVIDUAL ? PIVOT_INDIVIDUAL : PIVOT_MEDIAN;
	if (pivot_option && pivot_option->get_selected() != pivot_mode)
		pivot_option->select(pivot_mode);
	update_gizmo();
}

// ── keys, camera, status ──────────────────────────────────────────────────

bool SceneryEditor::object_key(const Ref<InputEventKey> &k, char32_t unicode)
{
	const bool cmd = k->is_ctrl_pressed() || k->is_meta_pressed();
	auto is = [&](const char *action) {
		return KeyBindings::matches_key(action, k->get_keycode(), unicode, k->is_shift_pressed(), cmd, k->is_alt_pressed());
	};
	SceneryObjects *so = objects();
	if (is("gizmo_move"))
	{
		set_gizmo_mode(GIZMO_MOVE);
		return true;
	}
	if (is("gizmo_scale"))
	{
		set_gizmo_mode(GIZMO_SCALE);
		return true;
	}
	if (is("frame_selection"))
	{
		frame_selected();
		return true;
	}
	const bool drop = is("drop_to_ground");
	// heading: clockwise is -y about up
	const double heading_deg = is("heading_left") ? 1.0 : is("heading_right") ? -1.0 : is("heading_left_big") ? 10.0 : is("heading_right_big") ? -10.0 : 0.0;
	const double scale_f = is("scale_down") ? 1.0 / 1.05 : is("scale_down_big") ? 1.0 / 1.25 : is("scale_up") ? 1.05 : is("scale_up_big") ? 1.25 : 1.0;
	if (!drop && heading_deg == 0.0 && scale_f == 1.0)
		return false;
	if (so == nullptr || selection.empty())
		return false;
	so->begin_edit_group();
	for (int idx : selection)
	{
		if (drop)
		{
			so->drop_to_ground(idx);
			continue;
		}
		const Dictionary d = so->get_object_info(idx);
		Basis rot = d["rot"];
		Vector3 sc = d["scale"];
		if (heading_deg != 0.0)
			rot = Basis(Vector3(0, 1, 0), Math::deg_to_rad(heading_deg)) * rot;
		else
			sc *= scale_f;
		so->set_object_transform(idx, double(d["e"]), double(d["n"]), double(d["agl_m"]), rot, sc);
	}
	so->end_edit_group();
	selection_changed();
	return true;
}

// The camera in front of the selection, looking at it from the south-west, 35 deg down.
void SceneryEditor::frame_selected()
{
	SceneryObjects *so = objects();
	if (so == nullptr || selection.empty())
		return;
	double radius = 5.0;
	for (int idx : selection)
		radius = MAX(radius, double(so->get_object_info(idx)["radius_m"]));
	const Vector3 centre = selection_pivot();
	if (centre.length_squared() < 1.0)
		return;
	const geo::Geodetic g = geo::geodetic_of(centre);
	const Vector2 en = Geo::wgs84_to_lv95(g.lat, g.lon);
	const double dist = radius * 2.2 + 8.0;
	yaw = Math::deg_to_rad(45.0);
	pitch = Math::deg_to_rad(-35.0);
	pos = world_of(en.x - std::sin(yaw) * dist * std::cos(-pitch), en.y - std::cos(yaw) * dist * std::cos(-pitch), g.h + dist * std::sin(-pitch));
	apply_camera();
	update_gizmo();
}

String SceneryEditor::object_status() const
{
	SceneryObjects *so = objects();
	if (so == nullptr || so->get_object_count() == 0)
		return "\nno object packs in the enabled scenery areas (an area with objects/package.json)";
	if (selection.empty())
		return vformat("\n%d pack objects, %d loaded: click one to select it, Shift-click adds, F frames it", so->get_object_count(), so->get_loaded_count());
	String s = "\n" + so->get_object_label(selected_object);
	if (selection.size() > 1)
		s += vformat(" (+%d more selected)", (int)selection.size() - 1);
	return s + vformat(String::utf8("\n%s move, %s rotate, %s scale (drag the arrows / rings / cubes or the models, Shift = fine); %s %s height 0.5 m, %s %s heading 1°, %s %s scale 5%% (Shift x10 / x5), %s to the ground, %s removes, %s frames; %s writes the pack"),
		KeyBindings::label("gizmo_move"), KeyBindings::label("reverse_or_rotate"), KeyBindings::label("gizmo_scale"), KeyBindings::label("height_down"), KeyBindings::label("height_up"),
		KeyBindings::label("heading_left"), KeyBindings::label("heading_right"), KeyBindings::label("scale_down"), KeyBindings::label("scale_up"), KeyBindings::label("drop_to_ground"), KeyBindings::label("delete"), KeyBindings::label("frame_selection"), KeyBindings::label("save"));
}

// ── transform panel + object list ─────────────────────────────────────────

void SceneryEditor::build_object_panel()
{
	PanelContainer *panel_c = memnew(PanelContainer);
	panel_c->set_anchor(SIDE_LEFT, 1.0);
	panel_c->set_anchor(SIDE_RIGHT, 1.0);
	panel_c->set_anchor(SIDE_TOP, 0.0);
	panel_c->set_anchor(SIDE_BOTTOM, 0.0);
	panel_c->set_offset(SIDE_LEFT, -570);
	panel_c->set_offset(SIDE_RIGHT, -12);
	panel_c->set_offset(SIDE_TOP, 350);
	panel_c->set_offset(SIDE_BOTTOM, 350);
	panel_c->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	panel_c->set_visible(false);
	ui->add_child(panel_c);
	object_panel = panel_c;
	VBoxContainer *box = memnew(VBoxContainer);
	panel_c->add_child(box);
	Label *title = memnew(Label);
	title->set_text("Transform");
	title->add_theme_font_size_override("font_size", 14);
	box->add_child(title);
	HBoxContainer *opts = memnew(HBoxContainer);
	box->add_child(opts);
	orient_option = memnew(OptionButton);
	orient_option->add_item("Global");
	orient_option->add_item("Local");
	orient_option->set_tooltip_text("Gizmo axes: world east / north / up, or the active model's own axes");
	orient_option->set_focus_mode(Control::FOCUS_NONE);
	orient_option->connect("item_selected", Callable(this, "_on_orientation_selected"));
	opts->add_child(orient_option);
	pivot_option = memnew(OptionButton);
	pivot_option->add_item("Median point");
	pivot_option->add_item("Individual origins");
	pivot_option->set_tooltip_text("Several objects turn and scale about their median point, or each about its own pivot");
	pivot_option->set_focus_mode(Control::FOCUS_NONE);
	pivot_option->connect("item_selected", Callable(this, "_on_pivot_selected"));
	opts->add_child(pivot_option);
	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(4);
	box->add_child(grid);
	struct Row
	{
		const char *title;
		const char *labels[3];
		const char *suffix;
		double step;
	};
	static const Row ROWS[4] = {
		{"Location", {"E", "N", "Up"}, "m", 0.01},
		{"Rotation", {"Hdg", "Pitch", "Bank"}, "°", 0.1},
		{"Scale", {"X", "Y", "Z"}, "", 0.001},
		{"Dimensions", {"X", "Y", "Z"}, "m", 0.01}};
	for (int g = 0; g < 4; g++)
	{
		Label *rl = memnew(Label);
		rl->set_text(ROWS[g].title);
		rl->set_custom_minimum_size(Vector2(84, 0));
		grid->add_child(rl);
		for (int a = 0; a < 3; a++)
		{
			SpinBox *sp = memnew(SpinBox);
			sp->set_min(g == 2 || g == 3 ? 0.001 : -1e9);
			sp->set_max(1e9);
			sp->set_step(ROWS[g].step);
			sp->set_prefix(ROWS[g].labels[a]);
			sp->set_suffix(String::utf8(ROWS[g].suffix));
			sp->set_custom_minimum_size(Vector2(150, 0));
			sp->set_update_on_text_changed(false);
			sp->connect("value_changed", Callable(this, "_on_field_changed").bind(g * 3 + a));
			if (LineEdit *le = sp->get_line_edit())
				le->connect("text_submitted", Callable(this, "_on_field_submitted")); // Enter: apply and hand the keys back
			grid->add_child(sp);
			field_spin[g][a] = sp;
		}
	}
	HBoxContainer *pivots = memnew(HBoxContainer);
	box->add_child(pivots);
	Label *pl = memnew(Label);
	pl->set_text("Pivot:");
	pivots->add_child(pl);
	const char *pivot_labels[4] = {"centre", "base", "origin", "cursor"};
	const char *pivot_tips[4] = {"the model's bounds centre", "the middle of the model's base", "the model's own origin",
		"the terrain cursor: put the mouse where the pivot should be, then click"};
	for (int i = 0; i < 4; i++)
	{
		Button *b = memnew(Button);
		b->set_text(pivot_labels[i]);
		b->set_tooltip_text(pivot_tips[i]);
		b->set_focus_mode(Control::FOCUS_NONE);
		b->connect("pressed", Callable(this, "_on_pivot_button").bind(i));
		pivots->add_child(b);
	}
	Label *lt = memnew(Label);
	lt->set_text("Objects  (click selects, Shift / Cmd adds, double-click frames)");
	lt->add_theme_font_size_override("font_size", 12);
	box->add_child(lt);
	ItemList *list = memnew(ItemList);
	list->set_select_mode(ItemList::SELECT_MULTI);
	list->set_allow_reselect(true);
	list->set_custom_minimum_size(Vector2(530, 240));
	list->set_focus_mode(Control::FOCUS_NONE);
	list->connect("multi_selected", Callable(this, "_on_list_multi_selected"));
	list->connect("item_activated", Callable(this, "_on_list_item_activated"));
	box->add_child(list);
	object_list = list;
}

// Show the active object's pose in the fields (not while one is being typed into).
void SceneryEditor::refresh_object_panel()
{
	if (object_panel == nullptr)
		return;
	SceneryObjects *so = objects();
	const Dictionary d = so && selected_object >= 0 ? so->get_object_info(selected_object) : Dictionary();
	for (int g = 0; g < 4; g++)
		for (int a = 0; a < 3; a++)
		{
			LineEdit *le = field_spin[g][a]->get_line_edit();
			if (le && le->has_focus())
				return;
		}
	panel_updating = true;
	const Vector3 sc = d.is_empty() ? Vector3(1, 1, 1) : Vector3(d["scale"]);
	const Vector3 dims = d.is_empty() ? Vector3() : Vector3(d["dimensions"]);
	const double vals[4][3] = {
		{d.is_empty() ? 0.0 : double(d["e"]), d.is_empty() ? 0.0 : double(d["n"]), d.is_empty() ? 0.0 : double(d["agl_m"])},
		{d.is_empty() ? 0.0 : double(d["heading_deg"]), d.is_empty() ? 0.0 : double(d["pitch_deg"]), d.is_empty() ? 0.0 : double(d["bank_deg"])},
		{sc.x, sc.y, sc.z},
		{dims.x, dims.y, dims.z}};
	for (int g = 0; g < 4; g++)
		for (int a = 0; a < 3; a++)
		{
			field_spin[g][a]->set_editable(!d.is_empty() && (g != 3 || dims[a] > 1e-6));
			field_spin[g][a]->set_value(vals[g][a]);
		}
	panel_updating = false;
}

void SceneryEditor::refresh_object_list()
{
	ItemList *list = Object::cast_to<ItemList>(object_list);
	SceneryObjects *so = objects();
	if (list == nullptr || so == nullptr)
		return;
	panel_updating = true;
	if (list->get_item_count() != so->get_object_count())
	{
		list->clear();
		for (int i = 0; i < so->get_object_count(); i++)
			list->add_item("");
	}
	for (int i = 0; i < so->get_object_count(); i++)
	{
		const Dictionary d = so->get_object_info(i);
		String text = String(d["name"]) + "  (" + String(d["pack"]) + ")";
		if (bool(d["removed"]))
			text += "  removed";
		else if (!bool(d["loaded"]))
			text += "  (not loaded)";
		if (list->get_item_text(i) != text)
			list->set_item_text(i, text);
		const bool sel = std::find(selection.begin(), selection.end(), i) != selection.end();
		if (list->is_selected(i) != sel)
		{
			if (sel)
				list->select(i, false);
			else
				list->deselect(i);
		}
	}
	if (selected_object >= 0 && selected_object < list->get_item_count())
		call_deferred("_scroll_object_list"); // after the list has laid out its items
	panel_updating = false;
}

// Scroll the active object into view (SELECT_MULTI keeps no "current" item,
// so ensure_current_is_visible() is of no use here).
void SceneryEditor::scroll_object_list()
{
	ItemList *list = Object::cast_to<ItemList>(object_list);
	if (!list || selected_object < 0 || selected_object >= list->get_item_count())
		return;
	const Rect2 r = list->get_item_rect(selected_object, true);
	VScrollBar *sb = list->get_v_scroll_bar();
	if (!sb)
		return;
	const double h = list->get_size().y;
	if (r.position.y < 0.0 || r.position.y + r.size.y > h)
		sb->set_value(sb->get_value() + r.position.y - h * 0.5 + r.size.y * 0.5);
}

void SceneryEditor::on_field_changed(double value, int field)
{
	if (panel_updating)
		return;
	set_transform_field(field / 3, field % 3, value);
}

void SceneryEditor::set_transform_field(int group, int axis, double value)
{
	SceneryObjects *so = objects();
	if (so == nullptr || selected_object < 0 || axis < 0 || axis > 2)
		return;
	const Dictionary d = so->get_object_info(selected_object);
	double e = d["e"], n = d["n"], agl = d["agl_m"];
	Vector3 eul = Vector3(double(d["heading_deg"]), double(d["pitch_deg"]), double(d["bank_deg"]));
	Vector3 sc = d["scale"];
	if (group == 0)
	{
		if (axis == 0)
			e = value;
		else if (axis == 1)
			n = value;
		else
			agl = value;
		so->set_object_transform(selected_object, e, n, agl, Basis(d["rot"]), sc);
	}
	else if (group == 1)
	{
		eul[axis] = value;
		so->set_object_pose(selected_object, e, n, agl, eul.x, eul.y, eul.z, sc);
	}
	else if (group == 2)
	{
		sc[axis] = MAX(value, 0.001);
		so->set_object_transform(selected_object, e, n, agl, Basis(d["rot"]), sc);
	}
	else if (group == 3)
	{
		Vector3 dims = d["dimensions"];
		dims[axis] = MAX(value, 0.001);
		so->set_object_dimensions(selected_object, dims);
	}
	update_gizmo();
	update_preview();
	refresh_object_panel();
}

void SceneryEditor::on_orientation_selected(int idx) { set_orientation(idx); }
void SceneryEditor::on_pivot_selected(int idx) { set_pivot_mode(idx); }

// Pivot buttons: 0 bounds centre, 1 base centre, 2 origin, 3 the terrain cursor.
void SceneryEditor::on_pivot_button(int which)
{
	SceneryObjects *so = objects();
	if (so == nullptr || selection.empty())
		return;
	so->begin_edit_group();
	for (int idx : selection)
	{
		const Dictionary d = so->get_object_info(idx);
		if (!d.has("bounds_size"))
			continue;
		const Vector3 bmin = d["bounds_min"], bsize = d["bounds_size"];
		Vector3 local;
		if (which == 0)
			local = bmin + bsize * 0.5;
		else if (which == 1)
			local = Vector3(bmin.x + bsize.x * 0.5, bmin.y, bmin.z + bsize.z * 0.5);
		else if (which == 2)
			local = Vector3();
		else
		{
			if (!cursor_valid)
				continue;
			// the terrain cursor in the model's frame
			const Vector3 cursor_w = world_of(cursor_e, cursor_n, cursor_h);
			const Basis b = (geo::wun_basis(double(d["lat"]), double(d["lon"])) * Basis(d["rot"])).scaled_local(Vector3(d["scale"]));
			local = Vector3(d["pivot"]) + b.inverse().xform(cursor_w - Vector3(d["ecef"]));
			local.y = double(d["pivot"].operator Vector3().y); // keep the height of the pivot
		}
		so->set_object_pivot(idx, local, true);
	}
	so->end_edit_group();
	tool_note = "pivot moved; the model stays where it is";
	selection_changed();
}

void SceneryEditor::on_list_multi_selected(int idx, bool selected)
{
	if (panel_updating)
		return;
	if (selected)
	{
		if (std::find(selection.begin(), selection.end(), idx) == selection.end())
			selection.push_back(idx);
		selected_object = idx;
	}
	else
	{
		auto it = std::find(selection.begin(), selection.end(), idx);
		if (it != selection.end())
			selection.erase(it);
		if (selected_object == idx)
			selected_object = selection.empty() ? -1 : selection.back();
	}
	update_gizmo();
	update_preview();
	refresh_object_panel();
}

void SceneryEditor::on_list_item_activated(int idx)
{
	select_object(idx, false);
	frame_selected();
}

// ── SceneryGizmoOverlay ───────────────────────────────────────────────────

void SceneryGizmoOverlay::_bind_methods() {}

SceneryGizmoOverlay::SceneryGizmoOverlay() {}

void SceneryGizmoOverlay::set_editor(SceneryEditor *ed) { editor = ed; }

void SceneryGizmoOverlay::_draw()
{
	if (editor == nullptr)
		return;
	const int active = editor->get_gizmo_active();
	const Color hi(1.0, 0.85, 0.2);
	for (const SceneryEditor::GizmoHandle &h : editor->get_gizmo_handles())
	{
		const bool on = h.axis == active;
		const Color col = on ? hi : h.col;
		const Color shade(0.05, 0.05, 0.05, 0.55);
		if (h.kind == 3)
		{
			draw_circle(h.pts[0], 8.0, shade);
			draw_arc(h.pts[0], 8.0, 0.0, Math_TAU, 32, col, on ? 3.0 : 2.0, true);
			continue;
		}
		if (h.kind == 1)
		{
			const Color c = h.facing < 0.15 ? Color(col.r, col.g, col.b, 0.35) : col;
			draw_polyline(h.pts, shade, on ? 6.0 : 5.0, true);
			draw_polyline(h.pts, c, on ? 3.5 : 2.5, true);
			continue;
		}
		const Vector2 a = h.pts[0], t = h.pts[1];
		const Vector2 d = h.dir, p(-d.y, d.x);
		draw_line(a, t, shade, on ? 6.0 : 5.0, true);
		draw_line(a, t, col, on ? 3.5 : 2.5, true);
		if (h.kind == 0)
		{
			PackedVector2Array head;
			head.push_back(t + d * 12.0);
			head.push_back(t - p * 6.0);
			head.push_back(t + p * 6.0);
			draw_colored_polygon(head, col);
		}
		else
		{
			const double s = on ? 7.0 : 6.0;
			draw_rect(Rect2(t - Vector2(s, s), Vector2(2 * s, 2 * s)), col, true);
			draw_rect(Rect2(t - Vector2(s, s), Vector2(2 * s, 2 * s)), shade, false, 1.5, true);
		}
	}
}

// Enter in a transform field: the SpinBox has applied the value; drop the
// focus so the editor's keys work again without a click on the scene.
void SceneryEditor::on_field_submitted(const String &)
{
	if (Viewport *vp = host ? host->get_viewport() : nullptr)
		vp->call_deferred("gui_release_focus");
}

} // namespace jsbgodot
