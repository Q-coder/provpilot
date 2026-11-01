#include "scenery_editor.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/geometry2d.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "cloud_layers.h"
#include "key_bindings.h"
#include "geo.h"
#include "geo_frame.h"
#include "jsbgodot.h"
#include "scenery_objects.h"
#include "sky_atmosphere.h"
#include "slew_mode.h"
#include "terrain_elevation.h"
#include "world_scenery.h"

using namespace godot;

namespace jsbgodot
{

// ── SceneryViewGizmo ───────────────────────────────────────────────────────

void SceneryViewGizmo::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("pick", "local"), &SceneryViewGizmo::pick);
	ClassDB::bind_method(D_METHOD("get_hit_boxes"), &SceneryViewGizmo::get_hit_boxes);
}

SceneryViewGizmo::SceneryViewGizmo() {}

void SceneryViewGizmo::set_editor(SceneryEditor *ed)
{
	editor = ed;
}

Array SceneryViewGizmo::get_hit_boxes() const
{
	return hit_boxes;
}

void SceneryViewGizmo::_draw()
{
	const Vector2 c = get_size() * 0.5;
	draw_circle(c, R + KNOB + 6.0, Color(0.10, 0.10, 0.12, 0.55));
	if (editor == nullptr || editor->get_camera() == nullptr)
		return;
	struct Item
	{
		String name;
		Vector2 pos;
		double depth;
		Color col;
		String label;
		bool positive;
	};
	struct Spec
	{
		const char *name;
		Vector3 axis;
		Color col;
		const char *label;
	};
	static const Spec AXES[3] = {
		{"x", Vector3(-1, 0, 0), Color(0.90, 0.30, 0.30), "X"},
		{"y", Vector3(0, 0, 1), Color(0.40, 0.80, 0.30), "Y"},
		{"z", Vector3(0, 1, 0), Color(0.35, 0.55, 0.95), "Z"}};
	std::vector<Item> items;
	for (const Spec &spec : AXES)
	{
		const Vector3 v = editor->axis_on_screen(spec.axis);
		items.push_back({String(spec.name), c + Vector2(v.x, -v.y) * R, (double)v.z, spec.col, String(spec.label), true});
		items.push_back({String("-") + spec.name, c - Vector2(v.x, -v.y) * R, -(double)v.z, spec.col, String(), false});
	}
	std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.depth < b.depth; }); // far first
	hit_boxes.clear();
	Ref<Font> font = ThemeDB::get_singleton()->get_fallback_font();
	for (const Item &it : items)
	{
		const double fade = 0.55 + 0.45 * CLAMP((it.depth + 1.0) * 0.5, 0.0, 1.0);
		const Color col(it.col.r, it.col.g, it.col.b, fade);
		if (it.positive)
		{
			draw_line(c, it.pos, col, 2.5, true);
			draw_circle(it.pos, KNOB, col);
			draw_string(font, it.pos + Vector2(-4, 5), it.label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.05, 0.05, 0.05));
		}
		else
		{
			draw_circle(it.pos, KNOB, Color(col.r, col.g, col.b, fade * 0.35));
			draw_arc(it.pos, KNOB, 0.0, Math_TAU, 24, col, 1.5, true);
		}
		Array hb;
		hb.push_back(it.name);
		hb.push_back(it.pos);
		hit_boxes.push_back(hb);
	}
}

void SceneryViewGizmo::_gui_input(const Ref<InputEvent> &event)
{
	Ref<InputEventMouseButton> mb = event;
	if (mb.is_valid())
	{
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT)
		{
			if (mb->is_pressed())
			{
				dragging = true;
				drag_moved = 0.0;
			}
			else
			{
				dragging = false;
				if (drag_moved < 4.0)
				{
					const String name = pick(mb->get_position());
					if (name != "" && editor)
						editor->view_axis(name);
				}
			}
			accept_event();
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = event;
	if (mm.is_valid() && dragging)
	{
		drag_moved += mm->get_relative().length();
		if (editor)
			editor->orbit(mm->get_relative().x, mm->get_relative().y);
		accept_event();
	}
}

String SceneryViewGizmo::pick(const Vector2 &local) const
{
	String best;
	double best_d = KNOB + 3.0;
	for (int i = 0; i < hit_boxes.size(); i++)
	{
		const Array hb = hit_boxes[i];
		const double d = Vector2(hb[1]).distance_to(local);
		if (d < best_d)
		{
			best_d = d;
			best = hb[0];
		}
	}
	return best;
}

// ── SceneryEditor ──────────────────────────────────────────────────────────

void SceneryEditor::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("setup", "host"), &SceneryEditor::setup);
	ClassDB::bind_method(D_METHOD("design_path"), &SceneryEditor::design_path);
	ClassDB::bind_method(D_METHOD("design_is_baked"), &SceneryEditor::design_is_baked);
	ClassDB::bind_method(D_METHOD("node_world", "nid"), &SceneryEditor::node_world);
	ClassDB::bind_method(D_METHOD("node_markers_count"), &SceneryEditor::node_markers_count);
	ClassDB::bind_method(D_METHOD("node_at_screen", "screen"), &SceneryEditor::node_at_screen);
	ClassDB::bind_method(D_METHOD("edge_at_screen", "screen"), &SceneryEditor::edge_at_screen);
	ClassDB::bind_method(D_METHOD("elevation_lv95", "e", "n"), &SceneryEditor::elevation_lv95);
	ClassDB::bind_method(D_METHOD("elevation_lv95_precise", "e", "n"), &SceneryEditor::elevation_lv95_precise);
	ClassDB::bind_method(D_METHOD("is_active"), &SceneryEditor::is_active);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &SceneryEditor::poll);
	ClassDB::bind_method(D_METHOD("toggle"), &SceneryEditor::toggle);
	ClassDB::bind_method(D_METHOD("enter"), &SceneryEditor::enter);
	ClassDB::bind_method(D_METHOD("enter_at", "e", "n", "heading_deg", "agl"), &SceneryEditor::enter_at, DEFVAL(300.0));
	ClassDB::bind_method(D_METHOD("enter_common", "placed"), &SceneryEditor::enter_common, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("exit"), &SceneryEditor::exit);
	ClassDB::bind_method(D_METHOD("update", "delta"), &SceneryEditor::update);
	ClassDB::bind_method(D_METHOD("apply_camera"), &SceneryEditor::apply_camera);
	ClassDB::bind_method(D_METHOD("focus"), &SceneryEditor::focus);
	ClassDB::bind_method(D_METHOD("set_view", "heading_rad", "pitch_rad"), &SceneryEditor::set_view);
	ClassDB::bind_method(D_METHOD("view_top", "north_up"), &SceneryEditor::view_top);
	ClassDB::bind_method(D_METHOD("view_axis", "axis"), &SceneryEditor::view_axis);
	ClassDB::bind_method(D_METHOD("orbit", "dx", "dy"), &SceneryEditor::orbit);
	ClassDB::bind_method(D_METHOD("axis_on_screen", "axis"), &SceneryEditor::axis_on_screen);
	ClassDB::bind_method(D_METHOD("cursor_hit_from_screen", "screen"), &SceneryEditor::cursor_hit_from_screen);
	ClassDB::bind_method(D_METHOD("world_of", "e", "n", "h"), &SceneryEditor::world_of);
	ClassDB::bind_method(D_METHOD("update_direction_arrows"), &SceneryEditor::update_direction_arrows);
	ClassDB::bind_static_method("SceneryEditor", D_METHOD("heading_at", "pts", "q"), &SceneryEditor::heading_at);
	ClassDB::bind_method(D_METHOD("way_near", "e", "n"), &SceneryEditor::way_near);
	ClassDB::bind_method(D_METHOD("hidden_way_set"), &SceneryEditor::hidden_way_set);
	ClassDB::bind_method(D_METHOD("apply_hidden_ways"), &SceneryEditor::apply_hidden_ways);
	ClassDB::bind_method(D_METHOD("adopt_baked_heights", "node_ids"), &SceneryEditor::adopt_baked_heights);
	ClassDB::bind_method(D_METHOD("terrain_patches"), &SceneryEditor::terrain_patches);
	ClassDB::bind_method(D_METHOD("airport_protection"), &SceneryEditor::airport_protection);
	ClassDB::bind_method(D_METHOD("apply_terrain_patches", "force"), &SceneryEditor::apply_terrain_patches, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("apply_startup_design"), &SceneryEditor::apply_startup_design);
	ClassDB::bind_method(D_METHOD("edge_label", "eid"), &SceneryEditor::edge_label);
	ClassDB::bind_method(D_METHOD("baked_endpoint_near", "e", "n", "radius"), &SceneryEditor::baked_endpoint_near);
	ClassDB::bind_method(D_METHOD("baked_join_near", "e", "n", "radius"), &SceneryEditor::baked_join_near);
	ClassDB::bind_static_method("SceneryEditor", D_METHOD("way_label", "rec"), &SceneryEditor::way_label);
	ClassDB::bind_method(D_METHOD("pick_way"), &SceneryEditor::pick_way);
	ClassDB::bind_method(D_METHOD("current_attrs"), &SceneryEditor::current_attrs);
	ClassDB::bind_method(D_METHOD("adopt_attrs", "attrs"), &SceneryEditor::adopt_attrs);
	ClassDB::bind_method(D_METHOD("place_point"), &SceneryEditor::place_point);
	ClassDB::bind_method(D_METHOD("place_switch_point"), &SceneryEditor::place_switch_point);
	ClassDB::bind_method(D_METHOD("place_polygon_point"), &SceneryEditor::place_polygon_point);
	ClassDB::bind_method(D_METHOD("finish_polygon"), &SceneryEditor::finish_polygon);
	ClassDB::bind_method(D_METHOD("nudge_height", "delta"), &SceneryEditor::nudge_height);
	ClassDB::bind_method(D_METHOD("select_flatten", "index"), &SceneryEditor::select_flatten);
	ClassDB::bind_method(D_METHOD("get_selected_flatten"), &SceneryEditor::get_selected_flatten);
	ClassDB::bind_method(D_METHOD("place_circle_point"), &SceneryEditor::place_circle_point);
	ClassDB::bind_method(D_METHOD("finish_line"), &SceneryEditor::finish_line);
	ClassDB::bind_method(D_METHOD("delete_at_cursor"), &SceneryEditor::delete_at_cursor);
	ClassDB::bind_method(D_METHOD("undo"), &SceneryEditor::undo);
	ClassDB::bind_method(D_METHOD("save"), &SceneryEditor::save);
	ClassDB::bind_method(D_METHOD("set_tool", "tool"), &SceneryEditor::set_tool);
	ClassDB::bind_method(D_METHOD("get_tool"), &SceneryEditor::get_tool);
	ClassDB::bind_method(D_METHOD("rebuild_live"), &SceneryEditor::rebuild_live);
	ClassDB::bind_method(D_METHOD("bake_command"), &SceneryEditor::bake_command);
	ClassDB::bind_method(D_METHOD("bake_running"), &SceneryEditor::bake_running);
	ClassDB::bind_method(D_METHOD("bake_area"), &SceneryEditor::bake_area);
	ClassDB::bind_method(D_METHOD("clear_way_cache"), &SceneryEditor::clear_way_cache);
	ClassDB::bind_method(D_METHOD("select_object_at", "screen", "add"), &SceneryEditor::select_object_at, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("select_object", "idx", "add"), &SceneryEditor::select_object);
	ClassDB::bind_method(D_METHOD("add_to_selection", "idx"), &SceneryEditor::add_to_selection);
	ClassDB::bind_method(D_METHOD("clear_selection"), &SceneryEditor::clear_selection);
	ClassDB::bind_method(D_METHOD("get_selection"), &SceneryEditor::get_selection);
	ClassDB::bind_method(D_METHOD("get_orientation"), &SceneryEditor::get_orientation);
	ClassDB::bind_method(D_METHOD("set_orientation", "o"), &SceneryEditor::set_orientation);
	ClassDB::bind_method(D_METHOD("get_pivot_mode"), &SceneryEditor::get_pivot_mode);
	ClassDB::bind_method(D_METHOD("set_pivot_mode", "m"), &SceneryEditor::set_pivot_mode);
	ClassDB::bind_method(D_METHOD("frame_selected"), &SceneryEditor::frame_selected);
	ClassDB::bind_method(D_METHOD("set_transform_field", "group", "axis", "value"), &SceneryEditor::set_transform_field);
	ClassDB::bind_method(D_METHOD("_on_field_changed", "value", "field"), &SceneryEditor::on_field_changed);
	ClassDB::bind_method(D_METHOD("_on_orientation_selected", "idx"), &SceneryEditor::on_orientation_selected);
	ClassDB::bind_method(D_METHOD("_on_pivot_selected", "idx"), &SceneryEditor::on_pivot_selected);
	ClassDB::bind_method(D_METHOD("_on_pivot_button", "which"), &SceneryEditor::on_pivot_button);
	ClassDB::bind_method(D_METHOD("_on_list_multi_selected", "idx", "selected"), &SceneryEditor::on_list_multi_selected);
	ClassDB::bind_method(D_METHOD("_scroll_object_list"), &SceneryEditor::scroll_object_list);
	ClassDB::bind_method(D_METHOD("_on_field_submitted", "text"), &SceneryEditor::on_field_submitted);
	ClassDB::bind_method(D_METHOD("_on_list_item_activated", "idx"), &SceneryEditor::on_list_item_activated);
	ClassDB::bind_method(D_METHOD("get_selected_object"), &SceneryEditor::get_selected_object);
	ClassDB::bind_method(D_METHOD("set_selected_object", "v"), &SceneryEditor::set_selected_object);
	ClassDB::bind_method(D_METHOD("get_gizmo_mode"), &SceneryEditor::get_gizmo_mode);
	ClassDB::bind_method(D_METHOD("set_gizmo_mode", "m"), &SceneryEditor::set_gizmo_mode);
	ClassDB::bind_method(D_METHOD("get_gizmo_handle", "axis"), &SceneryEditor::get_gizmo_handle);
	ClassDB::bind_method(D_METHOD("get_gizmo_size_m"), &SceneryEditor::get_gizmo_size_m);
	ClassDB::bind_method(D_METHOD("gizmo_hit_test", "screen"), &SceneryEditor::gizmo_hit_test);
	ClassDB::bind_method(D_METHOD("_on_mode_button", "mode"), &SceneryEditor::on_mode_button);
	ClassDB::bind_method(D_METHOD("handle_mouse", "event"), &SceneryEditor::handle_mouse);
	ClassDB::bind_method(D_METHOD("handle_key", "event"), &SceneryEditor::handle_key);
	ClassDB::bind_method(D_METHOD("get_host"), &SceneryEditor::get_host);
	ClassDB::bind_method(D_METHOD("get_design"), &SceneryEditor::get_design);
	ClassDB::bind_method(D_METHOD("set_design", "design"), &SceneryEditor::set_design);
	ClassDB::bind_method(D_METHOD("get_mesher"), &SceneryEditor::get_mesher);
	ClassDB::bind_method(D_METHOD("get_active"), &SceneryEditor::get_active);
	ClassDB::bind_method(D_METHOD("get_camera"), &SceneryEditor::get_camera);
	ClassDB::bind_method(D_METHOD("get_gizmo"), &SceneryEditor::get_gizmo);
	ClassDB::bind_method(D_METHOD("get_camera_position"), &SceneryEditor::get_camera_position);
	ClassDB::bind_method(D_METHOD("set_camera_position", "p"), &SceneryEditor::set_camera_position);
	ClassDB::bind_method(D_METHOD("get_yaw"), &SceneryEditor::get_yaw);
	ClassDB::bind_method(D_METHOD("set_yaw", "v"), &SceneryEditor::set_yaw);
	ClassDB::bind_method(D_METHOD("get_pitch"), &SceneryEditor::get_pitch);
	ClassDB::bind_method(D_METHOD("set_pitch", "v"), &SceneryEditor::set_pitch);
	ClassDB::bind_method(D_METHOD("get_mouse"), &SceneryEditor::get_mouse);
	ClassDB::bind_method(D_METHOD("set_mouse", "m"), &SceneryEditor::set_mouse);
	ClassDB::bind_method(D_METHOD("get_cursor_valid"), &SceneryEditor::get_cursor_valid);
	ClassDB::bind_method(D_METHOD("set_cursor_valid", "v"), &SceneryEditor::set_cursor_valid);
	ClassDB::bind_method(D_METHOD("get_cursor_e"), &SceneryEditor::get_cursor_e);
	ClassDB::bind_method(D_METHOD("set_cursor_e", "v"), &SceneryEditor::set_cursor_e);
	ClassDB::bind_method(D_METHOD("get_cursor_n"), &SceneryEditor::get_cursor_n);
	ClassDB::bind_method(D_METHOD("set_cursor_n", "v"), &SceneryEditor::set_cursor_n);
	ClassDB::bind_method(D_METHOD("get_selected_edge"), &SceneryEditor::get_selected_edge);
	ClassDB::bind_method(D_METHOD("set_selected_edge", "v"), &SceneryEditor::set_selected_edge);
	ClassDB::bind_method(D_METHOD("get_selected_node"), &SceneryEditor::get_selected_node);
	ClassDB::bind_method(D_METHOD("set_selected_node", "v"), &SceneryEditor::set_selected_node);
	ClassDB::bind_method(D_METHOD("get_modifier_down"), &SceneryEditor::get_modifier_down);
	ClassDB::bind_method(D_METHOD("set_modifier_down", "v"), &SceneryEditor::set_modifier_down);
	ClassDB::bind_method(D_METHOD("get_tool_note"), &SceneryEditor::get_tool_note);
	ClassDB::bind_method(D_METHOD("get_line_nodes"), &SceneryEditor::get_line_nodes);
	ClassDB::bind_method(D_METHOD("get_class_option"), &SceneryEditor::get_class_option);
	ClassDB::bind_method(D_METHOD("get_lanes_spin"), &SceneryEditor::get_lanes_spin);
	ClassDB::bind_method(D_METHOD("get_oneway_check"), &SceneryEditor::get_oneway_check);
	ClassDB::bind_method(D_METHOD("get_curved_check"), &SceneryEditor::get_curved_check);
	ClassDB::bind_method(D_METHOD("get_direction_overlay_mesh"), &SceneryEditor::get_direction_overlay_mesh);
	ClassDB::bind_method(D_METHOD("_on_tool_button", "tool"), &SceneryEditor::on_tool_button);
	ClassDB::bind_method(D_METHOD("_on_panel_mouse_exited"), &SceneryEditor::on_panel_mouse_exited);
	ClassDB::bind_method(D_METHOD("_on_save_button"), &SceneryEditor::on_save_button);
	ClassDB::bind_method(D_METHOD("_on_bake_button"), &SceneryEditor::on_bake_button);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "host", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "design", PROPERTY_HINT_RESOURCE_TYPE, "SceneryDesign", PROPERTY_USAGE_NONE), "set_design", "get_design");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesher", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_mesher");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tool", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_tool", "get_tool");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_active");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "camera_position", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_camera_position", "get_camera_position");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "yaw", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_yaw", "get_yaw");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_pitch", "get_pitch");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "mouse", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_mouse", "get_mouse");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cursor_valid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_cursor_valid", "get_cursor_valid");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cursor_e", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_cursor_e", "get_cursor_e");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cursor_n", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_cursor_n", "get_cursor_n");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "selected_edge", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_selected_edge", "get_selected_edge");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "selected_node", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_selected_node", "get_selected_node");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "selected_object", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_selected_object", "get_selected_object");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gizmo_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_gizmo_mode", "get_gizmo_mode");
	BIND_ENUM_CONSTANT(GIZMO_MOVE);
	BIND_ENUM_CONSTANT(GIZMO_ROTATE);
	BIND_ENUM_CONSTANT(GIZMO_SCALE);
	BIND_ENUM_CONSTANT(AXIS_EAST);
	BIND_ENUM_CONSTANT(AXIS_NORTH);
	BIND_ENUM_CONSTANT(AXIS_UP);
	BIND_ENUM_CONSTANT(AXIS_FREE);
	BIND_ENUM_CONSTANT(AXIS_ROT_EAST);
	BIND_ENUM_CONSTANT(AXIS_ROT_NORTH);
	BIND_ENUM_CONSTANT(AXIS_ROT_UP);
	BIND_ENUM_CONSTANT(AXIS_SCALE_X);
	BIND_ENUM_CONSTANT(AXIS_SCALE_Y);
	BIND_ENUM_CONSTANT(AXIS_SCALE_Z);
	BIND_ENUM_CONSTANT(AXIS_UNIFORM);
	BIND_ENUM_CONSTANT(ORIENT_GLOBAL);
	BIND_ENUM_CONSTANT(ORIENT_LOCAL);
	BIND_ENUM_CONSTANT(PIVOT_MEDIAN);
	BIND_ENUM_CONSTANT(PIVOT_INDIVIDUAL);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "modifier_down", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_modifier_down", "get_modifier_down");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tool_note", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_tool_note");

	ADD_SIGNAL(MethodInfo("exited"));

	BIND_ENUM_CONSTANT(TOOL_SELECT);
	BIND_ENUM_CONSTANT(TOOL_ROAD);
	BIND_ENUM_CONSTANT(TOOL_RAIL);
	BIND_ENUM_CONSTANT(TOOL_DELETE);
	BIND_ENUM_CONSTANT(TOOL_CIRCLE);
	BIND_ENUM_CONSTANT(TOOL_SWITCH);
	BIND_ENUM_CONSTANT(TOOL_REPLACE);
	BIND_ENUM_CONSTANT(TOOL_FLATTEN);
	BIND_ENUM_CONSTANT(TOOL_AREA);
	BIND_ENUM_CONSTANT(TOOL_PICK);
	BIND_ENUM_CONSTANT(TOOL_OBJECT);
}

SceneryEditor::SceneryEditor()
{
	design.instantiate();
	preview_mesh.instantiate();
	overlay_mesh.instantiate();
	flat_overlay_mesh.instantiate();
	dir_overlay_mesh.instantiate();
}

// ── host modules ──────────────────────────────────────────────────────────

WorldScenery *SceneryEditor::world() const
{
	return host ? Object::cast_to<WorldScenery>(host->get("_world")) : nullptr;
}

SceneryObjects *SceneryEditor::objects() const
{
	return host ? Object::cast_to<SceneryObjects>(host->get("_scenery_objects")) : nullptr;
}

SkyAtmosphere *SceneryEditor::sky() const
{
	return host ? Object::cast_to<SkyAtmosphere>(host->get("_sky")) : nullptr;
}

Node3D *SceneryEditor::clouds() const
{
	return host ? Object::cast_to<Node3D>(host->get("_clouds")) : nullptr;
}

SlewMode *SceneryEditor::slew() const
{
	return host ? Object::cast_to<SlewMode>(host->get("_slew_mode")) : nullptr;
}

TerrainElevation *SceneryEditor::terrain_elevation() const
{
	return host ? Object::cast_to<TerrainElevation>(host->get("_terrain_elevation")) : nullptr;
}

JSBGodot *SceneryEditor::jsb() const
{
	return host ? Object::cast_to<JSBGodot>(host->get("jsb_node")) : nullptr;
}

Camera3D *SceneryEditor::active_camera() const
{
	return host ? Object::cast_to<Camera3D>(host->get("active_camera")) : nullptr;
}

bool SceneryEditor::host_flag(const char *name) const
{
	return host ? bool(host->get(name)) : false;
}

Array SceneryEditor::road_managers() const
{
	Array out;
	if (host == nullptr || host->get_tree() == nullptr)
		return out;
	const TypedArray<Node> found = host->get_tree()->get_root()->find_children("*", "S3DRoadManager", true, false);
	for (int i = 0; i < found.size(); i++)
		out.push_back(found[i]);
	return out;
}

// ── setup ─────────────────────────────────────────────────────────────────

static MeshInstance3D *make_overlay(Node *parent, const Ref<Mesh> &mesh, const Color &col, bool vertex_colours = false)
{
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(mesh);
	Ref<StandardMaterial3D> m;
	m.instantiate();
	if (vertex_colours)
	{
		m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	}
	else
		m->set_albedo(col);
	m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	m->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	mi->set_material_override(m);
	mi->set_visible(false);
	parent->add_child(mi);
	return mi;
}

void SceneryEditor::setup(Node3D *p_host)
{
	host = p_host;
	mesher = memnew(SceneryDesignMesher);
	mesher->set_name("SceneryDesignMesher");
	mesher->set_elevation(Callable(this, "elevation_lv95_precise")); // never the coarse globe: no hovering meshes
	host->add_child(mesher);
	cam = memnew(Camera3D);
	cam->set_name("SceneryEditorCamera");
	cam->set_far(200000.0);
	cam->set_near(0.5);
	cam->set_fov(70.0);
	host->add_child(cam);
	marker = memnew(MeshInstance3D);
	Ref<SphereMesh> sm;
	sm.instantiate();
	sm->set_radius(1.2);
	sm->set_height(2.4);
	marker->set_mesh(sm);
	Ref<StandardMaterial3D> mat;
	mat.instantiate();
	mat->set_albedo(Color(1.0, 0.85, 0.1));
	mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA); // faded while an object is selected
	marker->set_material_override(mat);
	marker->set_visible(false);
	host->add_child(marker);
	preview = make_overlay(host, preview_mesh, Color(1.0, 0.6, 0.1));
	preview->set_visible(true);
	overlay = make_overlay(host, overlay_mesh, Color(1.0, 0.25, 0.2));
	flat_overlay = make_overlay(host, flat_overlay_mesh, Color(0.3, 0.6, 1.0));
	dir_overlay = make_overlay(host, dir_overlay_mesh, Color(1, 1, 1), true);
	node_markers = memnew(MultiMeshInstance3D);
	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_colors(true);
	Ref<SphereMesh> nm;
	nm.instantiate();
	nm->set_radius(0.9);
	nm->set_height(1.8);
	Ref<StandardMaterial3D> nmat;
	nmat.instantiate();
	nmat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	nmat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	nmat->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	nm->set_material(nmat);
	mm->set_mesh(nm);
	node_markers->set_multimesh(mm);
	node_markers->set_visible(false);
	host->add_child(node_markers);
	build_ui();
	set_process_unhandled_input(false);
	set_process_input(false);
	design->load(design_path());
}

// Where the design file lives: an override for tests (PVP_DESIGN_FILE),
// else <install root>/scenery/Design/design/scenery_design.json (the
// install root of provpilot.cfg, the same place the weather profiles use:
// stable across sessions whatever the scenery library holds), else user://.
// Never derived from the scenery areas: their order changes.
String SceneryEditor::design_path() const
{
	const String env = OS::get_singleton()->get_environment("PVP_DESIGN_FILE");
	if (env != "")
		return env;
	Node *ir = host ? host->get_node_or_null(NodePath("/root/InstallRoot")) : nullptr;
	String root;
	if (ir && ir->has_method("get_install_root"))
		root = String(ir->call("get_install_root"));
	if (root == "")
		return OS::get_singleton()->get_user_data_dir().path_join("scenery_design").path_join("scenery_design.json");
	return root.path_join("scenery").path_join("Design/design").path_join("scenery_design.json");
}

// (Re)load the design when its path is known and differs from the loaded one.
void SceneryEditor::ensure_loaded()
{
	const String p = design_path();
	if (design->get_path() != p)
	{
		design->load(p);
		built_once = false;
		baked_sha = "";
		patches_stale = true;
	}
}

// True when the road tiles of an enabled scenery area were baked from
// exactly this design file (its sha256 is in a roads manifest): the tiles
// carry it, so the live meshes stay off until it is edited.
bool SceneryEditor::design_is_baked()
{
	if (design->get_dirty() || design->get_path() == "" || !FileAccess::file_exists(design->get_path()))
		return false;
	const String sha = SceneryDesign::file_sha256(design->get_path());
	if (sha == "")
		return false;
	if (baked_sha == sha)
		return true;
	const Array rms = road_managers();
	for (int r = 0; r < rms.size(); r++)
	{
		Node *rm = Object::cast_to<Node>(rms[r]);
		const PackedStringArray dirs = rm->call("get_road_paths");
		for (int d = 0; d < dirs.size(); d++)
		{
			const String mp = dirs[d].path_join("manifest.json");
			if (!FileAccess::file_exists(mp))
				continue;
			const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(mp));
			if (parsed.get_type() != Variant::DICTIONARY)
				continue;
			const Array designs = Dictionary(parsed).get("designs", Array());
			for (int i = 0; i < designs.size(); i++)
			{
				const Dictionary rec = designs[i];
				if (String(rec.get("sha256", "")) == sha)
				{
					baked_sha = sha;
					return true;
				}
			}
		}
	}
	return false;
}

// Called by every tool before it changes the design (undo point + patches).
void SceneryEditor::edit_begins()
{
	design->snapshot();
	patches_stale = true;
}

// Live meshes: everything, or nothing while the tiles already carry the design.
void SceneryEditor::rebuild_live()
{
	if (design_is_baked())
		mesher->clear();
	else
		mesher->rebuild(design);
	apply_hidden_ways();
	apply_terrain_patches();
	update_overlay();
	update_markers();
}

// World position of a design node: the precise ground (or the globe) plus its offset.
Vector3 SceneryEditor::node_world(int nid) const
{
	const Vector2 p = design->node_pos(nid);
	double h = elevation_lv95_precise(p.x, p.y);
	if (std::isnan(h))
		h = elevation_lv95(p.x, p.y);
	if (std::isnan(h))
		h = cursor_h;
	return world_of(p.x, p.y, h + design->node_offset(nid) + 0.6);
}

// Node markers: yellow spheres (white for the selected node), scaled with distance.
void SceneryEditor::update_markers()
{
	if (node_markers == nullptr)
		return;
	Ref<MultiMesh> mm = node_markers->get_multimesh();
	const Array ids = design->get_nodes().keys();
	mm->set_instance_count(ids.size());
	const Vector3 anchor = pos; // instance transforms are float: keep them small
	node_markers->set_global_position(anchor);
	const Dictionary edges = design->get_edges();
	for (int i = 0; i < ids.size(); i++)
	{
		const int nid = ids[i];
		const Vector3 w = node_world(nid);
		const double sc = CLAMP(pos.distance_to(w) / 120.0, 0.6, 12.0);
		mm->set_instance_transform(i, Transform3D(Basis().scaled(Vector3(sc, sc, sc)), w - anchor));
		bool on_edge = false;
		if (selected_edge >= 0 && edges.has(selected_edge))
		{
			const Dictionary ed = edges[selected_edge];
			on_edge = int(ed["a"]) == nid || int(ed["b"]) == nid;
		}
		mm->set_instance_color(i, (nid == selected_node || on_edge) ? Color(1.0, 1.0, 1.0) : Color(1.0, 0.85, 0.1));
	}
}

int SceneryEditor::node_markers_count() const
{
	return node_markers && node_markers->get_multimesh().is_valid() ? node_markers->get_multimesh()->get_instance_count() : 0;
}

// Screen-space picking: the design node within PICK_PX of a screen point, or -1.
int SceneryEditor::node_at_screen(const Vector2 &screen) const
{
	int best = -1;
	double best_d = PICK_PX;
	const Array ids = design->get_nodes().keys();
	for (int i = 0; i < ids.size(); i++)
	{
		const int nid = ids[i];
		const Vector3 w = node_world(nid);
		if (cam->is_position_behind(w))
			continue;
		const double d = cam->unproject_position(w).distance_to(screen);
		if (d < best_d)
		{
			best_d = d;
			best = nid;
		}
	}
	return best;
}

// The design edge within PICK_PX of a screen point: {"id", "e", "n"} (the
// LV95 point of it under the mouse), or empty.
Dictionary SceneryEditor::edge_at_screen(const Vector2 &screen) const
{
	Dictionary best;
	double best_d = PICK_PX;
	const Dictionary edges = design->get_edges();
	const Array eids = edges.keys();
	for (int k = 0; k < eids.size(); k++)
	{
		const int eid = eids[k];
		const Dictionary ed = edges[eid];
		// short world segments (10 m): the screen-linear interpolation below
		// is then accurate enough for the LV95 point under the mouse
		const Array pts = SceneryDesignMesher::densify(design->edge_points(eid));
		const double h0 = design->node_offset(ed["a"]);
		const double h1 = design->node_offset(ed["b"]);
		Vector2 prev;
		bool prev_ok = false;
		const int n = pts.size();
		for (int i = 0; i < n; i++)
		{
			const Vector2 p = pts[i];
			double gh = elevation_lv95_precise(p.x, p.y);
			if (std::isnan(gh))
				gh = elevation_lv95(p.x, p.y);
			if (std::isnan(gh))
			{
				prev_ok = false;
				continue;
			}
			const Vector3 w = world_of(p.x, p.y, gh + Math::lerp(h0, h1, double(i) / MAX(n - 1, 1)) + 0.3);
			if (cam->is_position_behind(w))
			{
				prev_ok = false;
				continue;
			}
			const Vector2 sp = cam->unproject_position(w);
			if (prev_ok)
			{
				const Vector2 q = SceneryDesign::nearest_on_segment(prev, sp, screen);
				const double d = q.distance_to(screen);
				if (d < best_d)
				{
					best_d = d;
					const double t = (q - prev).length() / MAX((sp - prev).length(), 0.001);
					const Vector2 pp = Vector2(pts[i - 1]).lerp(p, t);
					best = Dictionary();
					best["id"] = eid;
					best["e"] = pp.x;
					best["n"] = pp.y;
				}
			}
			prev = sp;
			prev_ok = true;
		}
	}
	return best;
}

// Terrain height (m ASL) at an LV95 point, NAN when unknown (falls back to
// the coarse globe: for the cursor and the camera).
double SceneryEditor::elevation_lv95(double e, double n) const
{
	WorldScenery *w = world();
	if (w == nullptr)
		return NAN;
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	return w->terrain_elevation_geodetic(ll.x, ll.y);
}

// The precise scenery terrain only, NAN until its tile is streamed in:
// meshes built on it never hover over the globe and settle later.
double SceneryEditor::elevation_lv95_precise(double e, double n) const
{
	WorldScenery *w = world();
	if (w == nullptr)
		return NAN;
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	return w->scenery_elevation_geodetic(ll.x, ll.y);
}

bool SceneryEditor::is_active() const
{
	return active;
}

// Called every frame by the host: F8 toggles the mode; in normal flight the
// loaded design is built once the terrain around is streamed in.
void SceneryEditor::poll(double delta)
{
	ensure_loaded();
	if (KeyBindings::just_pressed("editor_toggle")) // Settings > Keyboard
		toggle();
	const bool terrain_ok = host_flag("terrain_initialized");
	if (active)
		update(delta);
	else if (terrain_ok && design->edge_count() == 0 && !built_once)
	{
		built_once = true;
		apply_hidden_ways(); // a design of deletions only: hide them in the tiles
		apply_terrain_patches();
	}
	else if (terrain_ok && design->edge_count() > 0)
	{
		// in flight: build once the streamer is idle, and again whenever new
		// tiles have landed (a chain without precise terrain is left out
		// until then, never drawn on the coarse globe)
		WorldScenery *w = world();
		const bool busy = w ? w->streamer_busy() : false;
		if ((!built_once && !busy) || (streamer_was_busy && !busy))
		{
			rebuild_live();
			built_once = true;
		}
		streamer_was_busy = busy;
	}
}

void SceneryEditor::toggle()
{
	if (active)
		exit();
	else
		enter();
}

void SceneryEditor::enter()
{
	// not before the flight is on the ground: the intro zoom / loading HUD
	// owns the camera until then
	if (!host_flag("terrain_initialized") || host_flag("_fly_loading") || host_flag("_intro_playing"))
		return;
	enter_common(false);
	// a selection kept from the last session gets its tint, panel and gizmo back
	if (!selection.empty())
		selection_changed();
}

// Edit Mode from the welcome screen: no flight, the camera starts `agl`
// metres over the LV95 point looking along `heading` and the scenery
// streams in around it.
void SceneryEditor::enter_at(double e, double n, double heading_deg, double p_agl)
{
	double h = elevation_lv95(e, n);
	if (std::isnan(h))
		h = 500.0;
	pos = world_of(e, n, h + p_agl);
	yaw = Math::deg_to_rad(heading_deg);
	pitch = -0.7;
	enter_common(true);
}

void SceneryEditor::_notification(int what)
{
	if (what == NOTIFICATION_APPLICATION_FOCUS_OUT || what == NOTIFICATION_WM_WINDOW_FOCUS_OUT)
	{
		modifier_down = false; // the release of Cmd / Ctrl went to another app
		looking = false;
	}
}

void SceneryEditor::enter_common(bool placed)
{
	ensure_loaded();
	active = true;
	modifier_down = false;
	looking = false;
	line_nodes.clear();
	line_join = Dictionary();
	streamer_was_busy = true; // re-mesh once the tiles around have streamed in
	Camera3D *cur = host->get_viewport()->get_camera_3d();
	Node3D *ac = Object::cast_to<Node3D>(host->get_node_or_null(NodePath("AC")));
	bool from_cam = false;
	if (placed)
	{
	}
	else if (cur)
	{
		pos = cur->get_global_position();
		const double a = agl();
		from_cam = !std::isnan(a) && a < 5000.0;
	}
	if (from_cam)
	{
		const Basis enu = enu_basis();
		const Vector3 f = enu.inverse().xform(-cur->get_global_transform().basis.get_column(2));
		yaw = std::atan2(-f.x, f.z); // heading: local +Z = north, -X = east
		pitch = std::asin(CLAMP(f.y, -1.0, 1.0));
		// start a little higher and looking down so the ground is in view
		pitch = MIN(pitch, -0.35);
	}
	else if (ac && !placed)
	{
		// the camera is in space (intro view) or unknown: 300 m over the
		// aircraft, looking down along its heading
		pos = ac->get_global_position();
		const geo::Geodetic g = geo::geodetic_of(pos);
		const Basis enu = geo::wun_basis(g.lat, g.lon);
		pos += enu.xform(Vector3(0, 300.0, 0));
		yaw = Math::deg_to_rad(WorldScenery::ac_true_heading(ac));
		pitch = -0.7;
	}
	apply_camera();
	cam->set_current(true);
	if (JSBGodot *j = jsb())
		j->set_slew_mode(true);
	Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	ui->set_visible(true);
	set_process_unhandled_input(true);
	set_process_input(true);
	line_last_node = -1;
	if (!built_once || rebuild_pending)
	{
		rebuild_live();
		built_once = true;
		rebuild_pending = false;
	}
	overlay->set_visible(true);
	flat_overlay->set_visible(true);
	dir_overlay->set_visible(true);
	update_overlay();
	set_tool(tool);
	mesher->set_tint(Color(0.72, 0.84, 1.18));
	rebuild_live();
	node_markers->set_visible(true);
	// clear air: no fog, no clouds while designing
	if (SkyAtmosphere *s = sky())
		s->set_clear_air(true);
	if (Node3D *c = clouds())
		c->set_visible(false);
	UtilityFunctions::print(vformat("=== SCENERY EDITOR ON === design: %s (%d nodes, %d edges)", design->get_path(), design->node_count(), design->edge_count()));
}

void SceneryEditor::exit()
{
	SceneryObjects *so_exit = objects();
	if (design->get_dirty() || (so_exit && so_exit->has_unsaved_edits()))
		save(); // never lose an edit on leaving the mode
	active = false;
	looking = false;
	line_last_node = -1;
	drag_node = -1;
	preview_mesh->clear_surfaces();
	marker->set_visible(false);
	overlay->set_visible(false);
	flat_overlay->set_visible(false);
	dir_overlay->set_visible(false);
	ui->set_visible(false);
	gizmo_handles.clear();
	gizmo_axis = AXIS_NONE;
	if (SkyAtmosphere *s = sky())
		s->set_clear_air(false);
	if (Node3D *c = clouds())
		c->set_visible(true);
	node_markers->set_visible(false);
	mesher->set_tint(Color(1, 1, 1));
	rebuild_pending = true; // the flight view rebuilds without the tint when the streamer settles
	streamer_was_busy = true;
	if (SceneryObjects *so = objects())
		so->clear_highlights(); // no editor tint in flight; re-entry re-applies it
	highlighted.clear();
	set_process_unhandled_input(false);
	set_process_input(false);
	JSBGodot *j = jsb();
	SlewMode *sl = slew();
	if (j && !(sl && sl->is_enabled()))
		j->set_slew_mode(false);
	if (Camera3D *ac = active_camera())
		ac->set_current(true);
	Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	UtilityFunctions::print(String("=== SCENERY EDITOR OFF ===") + (design->get_dirty() ? " (unsaved changes)" : ""));
	emit_signal("exited");
}

// ── camera ────────────────────────────────────────────────────────────────

Basis SceneryEditor::enu_basis() const
{
	const geo::Geodetic g = geo::geodetic_of(pos);
	return geo::wun_basis(g.lat, g.lon);
}

// The local frame has +Z = north and -X = east (the aircraft's nose is its
// +Z). yaw is a true heading in radians: a camera looks down its -Z, so
// heading 0 needs the extra half turn.
Basis SceneryEditor::heading_basis() const
{
	return Basis(Vector3(0, 1, 0), -yaw);
}

void SceneryEditor::apply_camera()
{
	const Basis enu = enu_basis();
	const Basis b = enu * heading_basis() * Basis(Vector3(0, 1, 0), Math_PI) * Basis(Vector3(1, 0, 0), pitch);
	cam->set_global_transform(Transform3D(b, pos));
}

double SceneryEditor::agl() const
{
	WorldScenery *w = world();
	if (w == nullptr)
		return NAN;
	const geo::Geodetic g = geo::geodetic_of(pos);
	const double h = w->terrain_elevation_geodetic(g.lat, g.lon);
	return std::isnan(h) ? NAN : g.h - h;
}

void SceneryEditor::update(double delta)
{
	// movement in the local ENU frame: forward along the view heading
	Input *input = Input::get_singleton();
	const Basis enu = enu_basis();
	const Basis yaw_b = heading_basis();
	const Vector3 fwd = yaw_b.xform(Vector3(0, 0, 1));    // north at heading 0
	const Vector3 right = yaw_b.xform(Vector3(-1, 0, 0)); // east at heading 0
	Vector3 mv;
	// a shortcut (Ctrl+S, Cmd+Z ...) is never a move
	if (!modifier_down) // from the last key event only: a missed release never sticks
	{
		if (KeyBindings::pressed("editor_forward"))
			mv += fwd;
		if (KeyBindings::pressed("editor_back"))
			mv -= fwd;
		if (KeyBindings::pressed("editor_right"))
			mv += right;
		if (KeyBindings::pressed("editor_left"))
			mv -= right;
		if (KeyBindings::pressed("editor_up"))
			mv += Vector3(0, 1, 0);
		if (KeyBindings::pressed("editor_down"))
			mv -= Vector3(0, 1, 0);
	}
	double a = agl();
	double speed = MOVE_SPEED + (std::isnan(a) ? 0.0 : MAX(a, 0.0) * 0.8);
	if (KeyBindings::pressed("editor_fast"))
		speed *= 4.0;
	if (mv.length_squared() > 0.0)
		pos += enu.xform(mv.normalized() * speed * delta);
	// stay above the ground
	a = agl();
	if (!std::isnan(a) && a < MIN_AGL_M)
		pos += enu.xform(Vector3(0, MIN_AGL_M - a, 0));
	apply_camera();
	mouse = host->get_viewport()->get_mouse_position();
	update_cursor();
	update_preview();
	update_gizmo();
	if (gizmo)
		gizmo->queue_redraw();
	const uint64_t frames = Engine::get_singleton()->get_process_frames();
	if (frames % 10 == 0)
		update_markers();
	if (frames % 30 == 0 && pos.distance_to(dir_overlay_pos) > 300.0)
		update_direction_arrows();
	if (Control *owner = host->get_viewport()->gui_get_focus_owner())
	{
		// a field keeps the keys only while the mouse stays over its panel
		// (the viewport's mouse position: the editor's own `mouse` only
		// follows events the GUI left unhandled)
		Control *pnl = (panel && panel->is_ancestor_of(owner)) ? panel : ((object_panel && object_panel->is_ancestor_of(owner)) ? object_panel : nullptr);
		if (pnl != nullptr && !pnl->get_global_rect().has_point(host->get_viewport()->get_mouse_position()))
		{
			commit_spins();
			host->get_viewport()->gui_release_focus();
		}
	}
	if (bake_pid > 0 && !OS::get_singleton()->is_process_running(bake_pid))
	{
		bake_pid = -1;
		bake_finished();
	}
	// rebuild once the streamer settles after new tiles arrived (heights)
	WorldScenery *w = world();
	const bool busy = w ? w->streamer_busy() : false;
	if (streamer_was_busy && !busy)
		rebuild_pending = true;
	streamer_was_busy = busy;
	if (rebuild_pending && !busy)
	{
		rebuild_live();
		rebuild_pending = false;
	}
	update_status();
}

// ── views: straight down, axis views, orbit around the focus ──────────────

// The terrain point in the middle of the screen and its distance, or empty.
Dictionary SceneryEditor::focus() const
{
	Dictionary out;
	const Dictionary hit = cursor_hit_from_screen(host->get_viewport()->get_visible_rect().size * 0.5);
	if (hit.is_empty())
		return out;
	const Vector3 w = hit["world"];
	out["world"] = w;
	out["dist"] = pos.distance_to(w);
	return out;
}

// Turn to a heading / pitch, keeping the point in the middle of the screen
// in the middle (the camera moves back along the new view line).
void SceneryEditor::set_view(double heading_rad, double pitch_rad)
{
	const Dictionary f = focus();
	yaw = Math::fposmod(heading_rad, Math_TAU);
	pitch = CLAMP(pitch_rad, -Math_PI * 0.5, Math_PI * 0.5);
	if (!f.is_empty())
	{
		apply_camera();
		const Vector3 fwd = -cam->get_global_transform().basis.get_column(2);
		pos = Vector3(f["world"]) - fwd * double(f["dist"]);
	}
	apply_camera();
	update_cursor();
}

// Space: straight down; Shift+Space: straight down and north up.
void SceneryEditor::view_top(bool north_up)
{
	set_view(north_up ? 0.0 : yaw, -Math_PI * 0.5);
}

// Gizmo click: "x" from the east looking west, "-x" from the west, "y" from
// the north looking south, "-y" from the south, "z" top view, "-z" a level
// view along the current heading.
void SceneryEditor::view_axis(const String &axis)
{
	const double side_pitch = (pitch > -1.2 && pitch < 0.0) ? pitch : -0.35;
	if (axis == "x")
		set_view(Math::deg_to_rad(270.0), side_pitch);
	else if (axis == "-x")
		set_view(Math::deg_to_rad(90.0), side_pitch);
	else if (axis == "y")
		set_view(Math::deg_to_rad(180.0), side_pitch);
	else if (axis == "-y")
		set_view(0.0, side_pitch);
	else if (axis == "z")
		set_view(yaw, -Math_PI * 0.5);
	else if (axis == "-z")
		set_view(yaw, 0.0);
}

// Gizmo drag: orbit around the focus (mouse right = view turns right).
void SceneryEditor::orbit(double dx, double dy)
{
	set_view(yaw + dx * LOOK_SENS * 1.5, pitch - dy * LOOK_SENS * 1.5);
}

// Direction of a local axis on screen for the gizmo: (screen offset, depth)
// in the camera frame, for east / north / up.
Vector3 SceneryEditor::axis_on_screen(const Vector3 &axis) const
{
	const Basis enu = enu_basis();
	const Basis cam_inv = cam->get_global_transform().basis.inverse();
	return cam_inv.xform(enu.xform(axis)); // x right, y up, z towards the viewer
}

// ── cursor: mouse ray against the terrain ─────────────────────────────────

// March the camera ray until it goes below the terrain, then bisect.
Dictionary SceneryEditor::cursor_hit_from_screen(const Vector2 &screen) const
{
	Dictionary out;
	WorldScenery *w = world();
	if (w == nullptr || cam == nullptr)
		return out;
	const Vector3 origin = cam->project_ray_origin(screen);
	const Vector3 dir = cam->project_ray_normal(screen);
	double t = 0.0;
	double step = 4.0;
	double prev_t = 0.0;
	bool prev_above = true;
	bool found = false;
	while (t < RAY_MAX_M)
	{
		const Vector3 p = origin + dir * t;
		const geo::Geodetic g = geo::geodetic_of(p);
		const double h = w->terrain_elevation_geodetic(g.lat, g.lon);
		if (!std::isnan(h) && g.h <= h)
		{
			if (prev_above && t > 0.0)
			{
				found = true;
				break;
			}
			prev_above = false;
		}
		else
			prev_above = true;
		prev_t = t;
		t += step;
		step = MIN(step * 1.08, 400.0);
	}
	if (!found)
		return out;
	double lo = prev_t;
	double hi = t;
	for (int i = 0; i < 14; i++)
	{
		const double mid = (lo + hi) * 0.5;
		const Vector3 p = origin + dir * mid;
		const geo::Geodetic g = geo::geodetic_of(p);
		const double h = w->terrain_elevation_geodetic(g.lat, g.lon);
		if (!std::isnan(h) && g.h <= h)
			hi = mid;
		else
			lo = mid;
	}
	const Vector3 p = origin + dir * hi;
	const geo::Geodetic g = geo::geodetic_of(p);
	const Vector2 en = Geo::wgs84_to_lv95(g.lat, g.lon);
	out["e"] = en.x;
	out["n"] = en.y;
	out["h"] = g.h;
	out["world"] = p;
	return out;
}

void SceneryEditor::update_cursor()
{
	const Vector2 m = host->get_viewport()->get_mouse_position();
	const Dictionary hit = cursor_hit_from_screen(m);
	cursor_valid = !hit.is_empty();
	if (cursor_valid)
	{
		cursor_e = hit["e"];
		cursor_n = hit["n"];
		cursor_h = hit["h"];
		cursor_world = hit["world"];
		// snap to a node (on screen, so elevated nodes and zoomed-out views
		// work too), never to the node being dragged; else to the end of a
		// baked road
		int nid = node_at_screen(m);
		if (nid == drag_node)
			nid = -1;
		if (nid < 0)
		{
			nid = design->node_near(cursor_e, cursor_n, SNAP_NODE_M);
			if (nid == drag_node)
				nid = -1;
		}
		if (nid >= 0)
		{
			const Vector2 np = design->node_pos(nid);
			cursor_e = np.x;
			cursor_n = np.y;
		}
		else if (tool == TOOL_ROAD || tool == TOOL_RAIL || tool == TOOL_SELECT)
		{
			const Vector2 bp = baked_endpoint_near(cursor_e, cursor_n, SNAP_NODE_M * 2.0);
			if (bp != Vector2(INFINITY, INFINITY))
			{
				cursor_e = bp.x;
				cursor_n = bp.y;
			}
		}
		marker->set_global_position(world_of(cursor_e, cursor_n, cursor_h + 0.5));
		const double a = agl();
		const double sc = CLAMP((std::isnan(a) ? 0.0 : a) / 150.0, 1.0, 12.0);
		marker->set_scale(Vector3(sc, sc, sc));
	}
	marker->set_visible(cursor_valid);
	// the terrain cursor steps back while an object is selected (gery 2026-09-27)
	if (Ref<StandardMaterial3D> mm = marker->get_material_override(); mm.is_valid())
	{
		const bool faded = tool == TOOL_OBJECT && !selection.empty();
		mm->set_albedo(Color(1.0, 0.85, 0.1, faded ? 0.3 : 1.0));
	}
	if (drag_node >= 0 && cursor_valid)
	{
		design->move_node(drag_node, cursor_e, cursor_n);
		update_markers();
	}
}

Vector3 SceneryEditor::world_of(double e, double n, double h) const
{
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	return geo::ecef_of(ll.x, ll.y, h);
}

void SceneryEditor::preview_polyline(const Array &pts)
{
	preview_mesh->clear_surfaces();
	preview_origin = pos;
	preview->set_global_position(preview_origin);
	if (pts.size() < 2)
		return;
	preview_mesh->surface_begin(Mesh::PRIMITIVE_LINE_STRIP);
	for (int i = 0; i < pts.size(); i++)
	{
		const Vector2 p = pts[i];
		double h = elevation_lv95(p.x, p.y);
		if (std::isnan(h))
			h = cursor_h;
		preview_mesh->surface_add_vertex(world_of(p.x, p.y, h + 1.0) - preview_origin);
	}
	preview_mesh->surface_end();
}

// The segment the next click would add: a tangent arc from the last node
// when curved drawing is on and the node has a tangent, else a straight.
Array SceneryEditor::next_segment_points() const
{
	const Vector2 a = design->node_pos(line_last_node);
	const Vector2 b(cursor_e, cursor_n);
	const Dictionary via = next_segment_via(a, b);
	if (via.is_empty())
	{
		Array out;
		out.push_back(a);
		out.push_back(b);
		return out;
	}
	return SceneryDesign::arc_points(a, via["via"], b);
}

Dictionary SceneryEditor::next_segment_via(const Vector2 &a, const Vector2 &b) const
{
	if (curved_check == nullptr || !curved_check->is_pressed())
		return Dictionary();
	const Vector2 t = design->node_tangent_out(line_last_node);
	if (t == Vector2())
		return Dictionary();
	return SceneryDesign::tangent_arc_via(a, t, b);
}

// The diverging arc the second Switch click would add.
Array SceneryEditor::switch_preview_points() const
{
	Array out;
	if (switch_toe.is_empty() || !cursor_valid || !design->get_edges().has(int(switch_toe["edge"])))
		return out;
	const Vector2 q(cursor_e, cursor_n);
	const int eid = switch_toe["edge"];
	const Vector2 t = design->track_tangent_at(eid, double(switch_toe["e"]), double(switch_toe["n"]), q);
	const Vector2 toe = design->nearest_on_edge(eid, Vector2(double(switch_toe["e"]), double(switch_toe["n"])))["p"];
	const Dictionary via = SceneryDesign::tangent_arc_via(toe, t, q);
	if (via.is_empty())
	{
		out.push_back(toe);
		out.push_back(q);
		return out;
	}
	return SceneryDesign::arc_points(toe, via["via"], q);
}

// Direction arrows every ARROW_EVERY_M along one-way roads within
// ARROW_RADIUS_M of the camera: cyan for design roads, green for baked ones.
void SceneryEditor::update_direction_arrows()
{
	dir_overlay_mesh->clear_surfaces();
	dir_origin = pos;
	dir_overlay->set_global_position(dir_origin);
	dir_overlay_pos = pos;
	const geo::Geodetic cam_g = geo::geodetic_of(pos);
	const Vector2 cam_en = Geo::wgs84_to_lv95(cam_g.lat, cam_g.lon);
	struct Line
	{
		Array pts;
		Color col;
	};
	std::vector<Line> lines;
	const Array chains = design->chains();
	for (int i = 0; i < chains.size(); i++)
	{
		const Dictionary chain = chains[i];
		if (!bool(Dictionary(chain["attrs"]).get("oneway", false)))
			continue;
		lines.push_back({SceneryDesignMesher::chain_polyline(design, chain), Color(0.3, 0.9, 1.0)});
	}
	load_ways_near(cam_en.x, cam_en.y);
	const Array replace_ids = design->get_replace_ids();
	const Array wids = ways.keys();
	for (int k = 0; k < wids.size(); k++)
	{
		if (replace_ids.has(wids[k]))
			continue;
		const Dictionary rec = ways[wids[k]];
		if (!bool(rec.get("oneway", false)))
			continue;
		Array pts;
		const Array raw = rec.get("pts", Array());
		for (int i = 0; i < raw.size(); i++)
		{
			const Array p = raw[i];
			pts.push_back(Vector2(double(p[0]), double(p[1])));
		}
		if (pts.size() >= 2 && Vector2(pts[0]).distance_to(cam_en) < ARROW_RADIUS_M * 2.0)
			lines.push_back({pts, Color(0.35, 1.0, 0.35)});
	}
	if (lines.empty())
		return;
	dir_overlay_mesh->surface_begin(Mesh::PRIMITIVE_TRIANGLES);
	int drawn = 0;
	for (const Line &item : lines)
	{
		const Array &pts = item.pts;
		double since = ARROW_EVERY_M * 0.5;
		for (int i = 0; i < pts.size() - 1; i++)
		{
			const Vector2 a = pts[i];
			const Vector2 b = pts[i + 1];
			const double seg = a.distance_to(b);
			const Vector2 d = (b - a) / MAX(seg, 0.001);
			double t = 0.0;
			while (t < seg)
			{
				const double step = MIN(ARROW_EVERY_M - since, seg - t);
				t += step;
				since += step;
				if (since >= ARROW_EVERY_M - 0.001 && t < seg + 0.001)
				{
					since = 0.0;
					const Vector2 c = a + d * t;
					if (c.distance_to(cam_en) > ARROW_RADIUS_M || drawn > 4000)
						continue;
					const double h = elevation_lv95(c.x, c.y);
					if (std::isnan(h))
						continue;
					const Vector2 n(-d.y, d.x);
					const Vector2 tip = c + d * 2.5;
					const Vector2 l = c - d * 1.5 + n * 1.5;
					const Vector2 r = c - d * 1.5 - n * 1.5;
					dir_overlay_mesh->surface_set_color(item.col);
					dir_overlay_mesh->surface_add_vertex(world_of(tip.x, tip.y, h + 0.9) - dir_origin);
					dir_overlay_mesh->surface_add_vertex(world_of(l.x, l.y, h + 0.9) - dir_origin);
					dir_overlay_mesh->surface_add_vertex(world_of(r.x, r.y, h + 0.9) - dir_origin);
					drawn++;
				}
			}
		}
	}
	if (drawn == 0)
		dir_overlay_mesh->clear_surfaces();
	else
		dir_overlay_mesh->surface_end();
}

// Flow heading (true, degrees) of a polyline at its point nearest to q.
double SceneryEditor::heading_at(const Array &pts, const Vector2 &q)
{
	double best_d = 1e12;
	double best = 0.0;
	for (int i = 0; i < pts.size() - 1; i++)
	{
		const Vector2 p = SceneryDesign::nearest_on_segment(pts[i], pts[i + 1], q);
		const double d = p.distance_to(q);
		if (d < best_d)
		{
			best_d = d;
			const Vector2 v = Vector2(pts[i + 1]) - Vector2(pts[i]);
			best = Math::fposmod(Math::rad_to_deg(std::atan2(v.x, v.y)), 360.0); // east = 90
		}
	}
	return best;
}

// Outlines of the replace polygons (red) and the flatten plateaus (blue:
// the outline at the target height plus a drop line at every corner).
void SceneryEditor::update_overlay()
{
	update_direction_arrows();
	overlay_mesh->clear_surfaces();
	overlay_origin = pos;
	overlay->set_global_position(overlay_origin);
	const Array replace = design->get_replace();
	for (int k = 0; k < replace.size(); k++)
	{
		const Array poly = replace[k];
		if (poly.is_empty())
			continue;
		overlay_mesh->surface_begin(Mesh::PRIMITIVE_LINE_STRIP);
		for (int i = 0; i < poly.size() + 1; i++)
		{
			const Vector2 p = poly[i % poly.size()];
			const double h = elevation_lv95(p.x, p.y);
			overlay_mesh->surface_add_vertex(world_of(p.x, p.y, (std::isnan(h) ? cursor_h : h) + 1.5) - overlay_origin);
		}
		overlay_mesh->surface_end();
	}
	flat_overlay_mesh->clear_surfaces();
	flat_origin = pos;
	flat_overlay->set_global_position(flat_origin);
	const Array flatten = design->get_flatten();
	for (int k = 0; k < flatten.size(); k++)
	{
		const Dictionary f = flatten[k];
		const Array poly = f["poly"];
		const double fh = f["h"];
		if (poly.is_empty())
			continue;
		// the selected plateau shows a double outline
		for (int ring = 0; ring < (k == selected_flatten ? 2 : 1); ring++)
		{
			flat_overlay_mesh->surface_begin(Mesh::PRIMITIVE_LINE_STRIP);
			for (int i = 0; i < poly.size() + 1; i++)
			{
				const Vector2 p = poly[i % poly.size()];
				flat_overlay_mesh->surface_add_vertex(world_of(p.x, p.y, fh + 0.5 + ring * 1.5) - flat_origin);
			}
			flat_overlay_mesh->surface_end();
		}
		flat_overlay_mesh->surface_begin(Mesh::PRIMITIVE_LINES);
		for (int i = 0; i < poly.size(); i++)
		{
			const Vector2 p = poly[i];
			const double h = elevation_lv95(p.x, p.y);
			flat_overlay_mesh->surface_add_vertex(world_of(p.x, p.y, (std::isnan(h) ? fh : h) + 0.5) - flat_origin);
			flat_overlay_mesh->surface_add_vertex(world_of(p.x, p.y, fh + 0.5) - flat_origin);
		}
		flat_overlay_mesh->surface_end();
	}
}

void SceneryEditor::update_preview()
{
	if (tool == TOOL_OBJECT)
	{
		// the selection outline: each selected model's bounding box in its own
		// frame (stable under the camera; the earlier terrain-hugging ring
		// wobbled with the tile LOD as the camera moved) plus a spoke out of
		// the front face (model +Z faces the heading)
		SceneryObjects *so = objects();
		preview_mesh->clear_surfaces();
		preview_origin = pos;
		preview->set_global_position(preview_origin);
		if (so == nullptr)
			return;
		for (int idx : selection)
		{
			if (idx != selected_object)
				continue; // the others show their tint only
			const Dictionary d = so->get_object_info(idx);
			if (d.is_empty() || bool(d["removed"]))
				continue;
			Node3D *node = Object::cast_to<Node3D>(d["node"]);
			if (node == nullptr)
				continue;
			const Transform3D xf = node->get_global_transform();
			Vector3 bmin = d["bounds_min"];
			Vector3 bsz = d["bounds_size"];
			if (bsz.length() < 1e-6)
			{
				bmin = Vector3(-0.5, 0.0, -0.5);
				bsz = Vector3(1.0, 1.0, 1.0);
			}
			const Vector3 bmax = bmin + bsz;
			Vector3 c[8];
			for (int k = 0; k < 8; k++)
				c[k] = xf.xform(Vector3((k & 1) ? bmax.x : bmin.x, (k & 2) ? bmax.y : bmin.y, (k & 4) ? bmax.z : bmin.z));
			static const int EDGES[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {3, 7}, {2, 6}};
			preview_mesh->surface_begin(Mesh::PRIMITIVE_LINES);
			for (const auto &e : EDGES)
			{
				preview_mesh->surface_add_vertex(c[e[0]] - preview_origin);
				preview_mesh->surface_add_vertex(c[e[1]] - preview_origin);
			}
			// heading spoke: base centre of the front face, 40 % of the depth further out
			const Vector3 front = Vector3((bmin.x + bmax.x) * 0.5, bmin.y, bmax.z);
			preview_mesh->surface_add_vertex(xf.xform(front) - preview_origin);
			preview_mesh->surface_add_vertex(xf.xform(front + Vector3(0.0, 0.0, MAX(bsz.z, 1.0) * 0.4)) - preview_origin);
			preview_mesh->surface_end();
		}
		return;
	}
	if (tool == TOOL_REPLACE || tool == TOOL_FLATTEN || tool == TOOL_AREA)
	{
		Array pts = poly_points.duplicate();
		if (cursor_valid)
			pts.push_back(Vector2(cursor_e, cursor_n));
		if (pts.size() >= 3)
			pts.push_back(pts[0]);
		preview_polyline(pts);
		return;
	}
	if (tool == TOOL_SWITCH)
	{
		preview_polyline(switch_preview_points());
		return;
	}
	if (tool == TOOL_CIRCLE && circle_pending && cursor_valid)
	{
		const double r = MAX(circle_centre.distance_to(Vector2(cursor_e, cursor_n)), 5.0);
		Array ring;
		for (int k = 0; k < 33; k++)
		{
			const double ang = Math_TAU * k / 32.0;
			ring.push_back(circle_centre + Vector2(std::cos(ang), std::sin(ang)) * r);
		}
		preview_polyline(ring);
		return;
	}
	if (line_last_node < 0 || !cursor_valid)
	{
		preview_mesh->clear_surfaces();
		return;
	}
	preview_polyline(next_segment_points());
}

// ── baked OSM ways around the cursor (roads_E_N.ways.json sidecars) ──────

// Read the sidecars of the tiles around (e, n) once, from every road path.
void SceneryEditor::load_ways_near(double e, double n)
{
	const int te0 = int(std::floor(e / TILE_M)) * TILE_M;
	const int tn0 = int(std::floor(n / TILE_M)) * TILE_M;
	const Array rms = road_managers();
	for (int r = 0; r < rms.size(); r++)
	{
		Node *rm = Object::cast_to<Node>(rms[r]);
		const PackedStringArray dirs = rm->call("get_road_paths");
		for (int d = 0; d < dirs.size(); d++)
		{
			for (int te = te0 - TILE_M; te < te0 + 2 * TILE_M; te += TILE_M)
			{
				for (int tn = tn0 - TILE_M; tn < tn0 + 2 * TILE_M; tn += TILE_M)
				{
					const String key = vformat("%s|%d_%d", dirs[d], te, tn);
					if (ways_tiles.has(key))
						continue;
					ways_tiles[key] = true;
					const String fp = dirs[d].path_join(vformat("roads_%d_%d.ways.json", te, tn));
					if (!FileAccess::file_exists(fp))
						continue;
					const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(fp));
					if (parsed.get_type() != Variant::ARRAY)
						continue;
					const Array recs = parsed;
					for (int i = 0; i < recs.size(); i++)
					{
						if (recs[i].get_type() != Variant::DICTIONARY)
							continue;
						const Dictionary rec = recs[i];
						if (rec.has("id"))
							ways[int64_t(rec["id"])] = rec;
					}
				}
			}
		}
	}
}

void SceneryEditor::clear_way_cache()
{
	ways_tiles.clear();
	ways.clear();
}

// The baked way nearest to (e, n) within PICK_M, or empty.
Dictionary SceneryEditor::way_near(double e, double n)
{
	load_ways_near(e, n);
	const Vector2 q(e, n);
	Dictionary best;
	double best_d = PICK_M;
	const Array replace_ids = design->get_replace_ids();
	const Array wids = ways.keys();
	for (int k = 0; k < wids.size(); k++)
	{
		if (replace_ids.has(wids[k]))
			continue; // already picked into the design
		const Dictionary rec = ways[wids[k]];
		const Array pts = rec.get("pts", Array());
		for (int i = 0; i < pts.size() - 1; i++)
		{
			const Array pa = pts[i], pb = pts[i + 1];
			const Vector2 a = Vector2(double(pa[0]), double(pa[1]));
			const Vector2 b = Vector2(double(pb[0]), double(pb[1]));
			const double d = SceneryDesign::nearest_on_segment(a, b, q).distance_to(q);
			if (d < best_d)
			{
				best_d = d;
				best = rec;
			}
		}
	}
	return best;
}

// The OSM ways the design replaces: the picked / deleted ids plus every
// sidecar way with a point inside a replace polygon (their tiles' sidecars
// are read for that).
PackedInt64Array SceneryEditor::hidden_way_set()
{
	Dictionary ids;
	const Array replace_ids = design->get_replace_ids();
	for (int i = 0; i < replace_ids.size(); i++)
		ids[int64_t(replace_ids[i])] = true;
	const Array replace = design->get_replace();
	for (int k = 0; k < replace.size(); k++)
	{
		const Array poly = replace[k];
		Vector2 lo(1e12, 1e12), hi(-1e12, -1e12);
		PackedVector2Array pv;
		for (int i = 0; i < poly.size(); i++)
		{
			const Vector2 p = poly[i];
			lo = lo.min(p);
			hi = hi.max(p);
			pv.push_back(p);
		}
		for (double e = lo.x; e <= hi.x + TILE_M; e += TILE_M)
			for (double n = lo.y; n <= hi.y + TILE_M; n += TILE_M)
				load_ways_near(e, n);
		const Array wids = ways.keys();
		for (int w = 0; w < wids.size(); w++)
		{
			const int64_t wid = wids[w];
			if (ids.has(wid))
				continue;
			const Array pts = Dictionary(ways[wids[w]]).get("pts", Array());
			for (int i = 0; i < pts.size(); i++)
			{
				const Array pt = pts[i];
				if (Geometry2D::get_singleton()->is_point_in_polygon(Vector2(double(pt[0]), double(pt[1])), pv))
				{
					ids[wid] = true;
					break;
				}
			}
		}
	}
	std::vector<int64_t> keys;
	const Array ka = ids.keys();
	for (int i = 0; i < ka.size(); i++)
		keys.push_back(int64_t(ka[i]));
	std::sort(keys.begin(), keys.end());
	PackedInt64Array out;
	for (int64_t k : keys)
		out.push_back(k);
	return out;
}

// Give the road managers the replaced ways: their triangles are dropped as
// the tiles (re)load, so a pick or a delete shows at once, without a bake.
void SceneryEditor::apply_hidden_ways()
{
	const PackedInt64Array ids = hidden_way_set();
	if (ids == hidden_applied)
		return;
	hidden_applied = ids;
	const Array rms = road_managers();
	for (int r = 0; r < rms.size(); r++)
	{
		Node *rm = Object::cast_to<Node>(rms[r]);
		if (rm->has_method("set_hidden_ways"))
			rm->call("set_hidden_ways", ids);
	}
	UtilityFunctions::print(vformat("SceneryEditor: %d replaced OSM ways hidden in the road tiles", ids.size()));
}

// Terrain patches of the design for the tile loader (the bake's plateau and
// corridor rules): every plateau, and along every road / track chain the
// smoothed ground profile plus the node offsets, except elevated runs (a
// deck leaves the ground alone). Stations every 10 m with their height,
// reach = half width + 45 m.
Dictionary SceneryEditor::terrain_patches()
{
	Array plateaus;
	const Array flatten = design->get_flatten();
	for (int k = 0; k < flatten.size(); k++)
	{
		const Dictionary f = flatten[k];
		Dictionary p;
		const Array poly = f["poly"];
		PackedVector2Array pv;
		for (int i = 0; i < poly.size(); i++)
			pv.push_back(Vector2(poly[i]));
		p["poly"] = pv;
		p["h"] = double(f["h"]);
		plateaus.push_back(p);
	}
	Array corridors;
	const Array chains = design->chains();
	for (int c = 0; c < chains.size(); c++)
	{
		const Dictionary chain = chains[c];
		const Dictionary attrs = chain["attrs"];
		const Dictionary po = SceneryDesignMesher::chain_polyline_offsets(design, chain);
		const Dictionary dens = SceneryDesignMesher::densify_with(po["pts"], po["offs"]);
		const Array pts = dens["pts"];
		const Array offs = dens["vals"];
		if (pts.size() < 2)
			continue;
		const bool is_road = String(attrs["kind"]) == "road";
		const double width = is_road ? SceneryDesignMesher::edge_width(design, int(Array(chain["edges"])[0])) : SceneryDesignMesher::get_ballast_width_m();
		const double window = !is_road ? 250.0 : (String(attrs["cls"]).begins_with("motorway") ? 120.0 : 80.0);
		// ground along the chain, Gaussian-smoothed like the bake's profile
		const int n = pts.size();
		std::vector<double> ground(n), along(n);
		along[0] = 0.0;
		int known = 0;
		for (int i = 0; i < n; i++)
		{
			const Vector2 p = pts[i];
			if (i > 0)
				along[i] = along[i - 1] + p.distance_to(Vector2(pts[i - 1]));
			ground[i] = elevation_lv95_precise(p.x, p.y);
			if (!std::isnan(ground[i]))
				known++;
		}
		if (known < 2)
			continue;
		std::vector<double> smooth(n);
		const double sig = window * 0.5;
		for (int i = 0; i < n; i++)
		{
			double wsum = 0.0, hsum = 0.0;
			for (int j = 0; j < n; j++)
			{
				if (std::isnan(ground[j]))
					continue;
				const double d = (along[j] - along[i]) / sig;
				if (std::abs(d) > 3.0)
					continue;
				const double wgt = std::exp(-0.5 * d * d);
				wsum += wgt;
				hsum += wgt * ground[j];
			}
			smooth[i] = wsum > 0.0 ? hsum / wsum : NAN;
		}
		// runs on the ground (offset <= 0.3 m): one corridor each
		PackedVector3Array run;
		for (int i = 0; i < n + 1; i++)
		{
			const bool on_ground = i < n && double(offs[i]) <= 0.3 && !std::isnan(smooth[i]);
			if (on_ground)
			{
				const Vector2 p = pts[i];
				run.push_back(Vector3(p.x, p.y, smooth[i] + double(offs[i])));
			}
			if ((!on_ground || i == n) && run.size() >= 2)
			{
				Dictionary cd;
				cd["stations"] = run;
				cd["reach"] = width * 0.5 + CORRIDOR_MARGIN_M;
				cd["half"] = width * 0.5;
				corridors.push_back(cd);
				run = PackedVector3Array();
			}
			else if (!on_ground)
				run = PackedVector3Array();
		}
	}
	// protection only where it can matter: shapes within 3 km of a corridor
	const Dictionary prot = airport_protection();
	std::vector<Rect2> boxes;
	for (int c = 0; c < corridors.size(); c++)
	{
		const PackedVector3Array st = Dictionary(corridors[c])["stations"];
		Vector2 lo(1e12, 1e12), hi(-1e12, -1e12);
		for (int i = 0; i < st.size(); i++)
		{
			lo = lo.min(Vector2(st[i].x, st[i].y));
			hi = hi.max(Vector2(st[i].x, st[i].y));
		}
		boxes.push_back(Rect2(lo - Vector2(3000, 3000), hi - lo + Vector2(6000, 6000)));
	}
	Array near_polys, near_corr;
	if (!boxes.empty())
	{
		const Array polys = prot["polys"];
		for (int i = 0; i < polys.size(); i++)
		{
			const PackedVector2Array poly = polys[i];
			for (const Rect2 &b : boxes)
				if (b.has_point(poly[0]))
				{
					near_polys.push_back(poly);
					break;
				}
		}
		const Array pc = prot["corridors"];
		for (int i = 0; i < pc.size(); i++)
		{
			const Dictionary c = pc[i];
			const PackedVector2Array st = c["stations"];
			for (const Rect2 &b : boxes)
				if (b.has_point(st[0]))
				{
					near_corr.push_back(c);
					break;
				}
		}
	}
	Dictionary out;
	out["plateaus"] = plateaus;
	out["corridors"] = corridors;
	out["protect_polys"] = near_polys;
	out["protect_corridors"] = near_corr;
	return out;
}

// Runway strips, taxiways (corridors) and aprons (polygons) of every airport
// the host knows: a design corridor never changes the terrain inside them
// (their plateaus carry the aircraft, D18).
Dictionary SceneryEditor::airport_protection() const
{
	Array polys, corridors;
	const Dictionary merged = host ? Dictionary(host->get("_runways_merged")) : Dictionary();
	const Array icaos = merged.keys();
	for (int k = 0; k < icaos.size(); k++)
	{
		const Dictionary ap = merged[icaos[k]];
		const Array runways = ap.get("runways", Array());
		for (int r = 0; r < runways.size(); r++)
		{
			const Dictionary rw = runways[r];
			const Array ends = rw.get("ends", Array());
			if (ends.size() != 2 || !Dictionary(ends[0]).has("pavement") || !Dictionary(ends[1]).has("pavement"))
				continue;
			const Array pa = Dictionary(ends[0])["pavement"], pb = Dictionary(ends[1])["pavement"];
			const Vector2 a = Geo::wgs84_to_lv95(double(pa[0]), double(pa[1]));
			const Vector2 b = Geo::wgs84_to_lv95(double(pb[0]), double(pb[1]));
			PackedVector2Array st;
			st.push_back(a);
			st.push_back(b);
			Dictionary c;
			c["stations"] = st;
			c["reach"] = 0.5 * double(rw.get("width_m", 45.0)) + 60.0;
			corridors.push_back(c);
		}
		const Array aprons = ap.get("aprons", Array());
		for (int i = 0; i < aprons.size(); i++)
		{
			const Array pg = Dictionary(aprons[i]).get("polygon", Array());
			PackedVector2Array poly;
			for (int j = 0; j < pg.size(); j++)
			{
				if (pg[j].get_type() != Variant::ARRAY)
					continue;
				const Array pt = pg[j];
				if (pt.size() >= 2 && pt[0].get_type() != Variant::NIL)
					poly.push_back(Geo::wgs84_to_lv95(double(pt[0]), double(pt[1])));
			}
			if (poly.size() >= 3)
				polys.push_back(poly);
		}
		const Array taxiways = ap.get("taxiways", Array());
		for (int i = 0; i < taxiways.size(); i++)
		{
			const Dictionary tw = taxiways[i];
			const Array pth = tw.get("path", Array());
			PackedVector2Array path;
			for (int j = 0; j < pth.size(); j++)
			{
				if (pth[j].get_type() != Variant::ARRAY)
					continue;
				const Array pt = pth[j];
				if (pt.size() >= 2)
					path.push_back(Geo::wgs84_to_lv95(double(pt[0]), double(pt[1])));
			}
			if (path.size() >= 2)
			{
				Dictionary c;
				c["stations"] = path;
				c["reach"] = 0.5 * double(tw.get("width_m", 23.0)) + 25.0;
				corridors.push_back(c);
			}
		}
	}
	Dictionary out;
	out["polys"] = polys;
	out["corridors"] = corridors;
	return out;
}

// Hand the tile manager the patches when they changed: the close tiles
// reload with the plateaus and corridors in their heights.
void SceneryEditor::apply_terrain_patches(bool force)
{
	// only after an edit: the corridor heights are read from the terrain,
	// which the patches themselves change, so recomputing on every tile
	// reload would chase its own tail
	if (!(force || patches_stale))
		return;
	WorldScenery *w = world();
	Object *tm = w ? w->get_tile_manager() : nullptr;
	if (tm == nullptr || !tm->has_method("set_terrain_patches"))
		return;
	patches_stale = false;
	Dictionary patches;
	if (active)
	{
		// the editor has the terrain around the edits: compute, and keep the
		// result in the design so a flight can apply it before any tile loads
		patches = terrain_patches();
		design->set_patch_cache(patches);
	}
	else
		patches = design->get_patch_cache().is_empty() ? terrain_patches() : design->get_patch_cache();
	const String sig = JSON::stringify(patches);
	if (sig == patches_applied)
		return;
	patches_applied = sig;
	if (host_flag("terrain_initialized") && !host_flag("_edit_session") && host->has_node(NodePath("AC")))
	{
		// tiles under a placed aircraft are about to reload: pin it until the
		// terrain and the flight model have settled (the teleport mechanism)
		Node3D *ac = Object::cast_to<Node3D>(host->get_node<Node>(NodePath("AC")));
		if (TerrainElevation *te = terrain_elevation())
			if (ac)
				te->start_rebase(ac->get_global_position());
	}
	tm->call("set_terrain_patches", patches);
	streamer_was_busy = true; // re-mesh once the patched tiles are back
	UtilityFunctions::print(vformat("SceneryEditor: terrain patches applied: %d plateaus, %d corridors%s", Array(patches["plateaus"]).size(), Array(patches["corridors"]).size(), active ? String() : String(" (from the design's cache)")));
}

// Before a flight or an edit session streams its first tile: the design's
// replaced ways and cached terrain patches go to the managers now, so the
// tiles load already patched and never reload under the aircraft.
void SceneryEditor::apply_startup_design()
{
	ensure_loaded();
	apply_hidden_ways();
	apply_terrain_patches(true);
}

// The chain (as design.chains() builds it) containing an edge.
Dictionary SceneryEditor::chain_of(int eid) const
{
	const Array chains = design->chains();
	for (int i = 0; i < chains.size(); i++)
	{
		const Dictionary chain = chains[i];
		if (Array(chain["edges"]).has(eid))
			return chain;
	}
	const Dictionary edges = design->get_edges();
	Dictionary out;
	out["attrs"] = edges.get(eid, Dictionary());
	Array nodes;
	if (edges.has(eid))
	{
		nodes.push_back(Dictionary(edges[eid])["a"]);
		nodes.push_back(Dictionary(edges[eid])["b"]);
	}
	out["nodes"] = nodes;
	Array es;
	es.push_back(eid);
	out["edges"] = es;
	return out;
}

// Short description of a design edge for the panel.
String SceneryEditor::edge_label(int eid) const
{
	const Dictionary edges = design->get_edges();
	if (!edges.has(eid))
		return "edge";
	const Dictionary ed = edges[eid];
	if (String(ed["kind"]) == "rail")
		return vformat("design track %s (edge %d)", String(ed["cls"]), eid);
	return vformat("design road %s, %d lanes%s (edge %d)", String(ed["cls"]), int(ed["lanes"]), bool(ed["oneway"]) ? String(", one-way") : String(), eid);
}

// The end point of a baked (not yet picked) way within `radius` m of
// (e, n), or Vector2(INF, INF): a design road can be joined to it exactly.
Vector2 SceneryEditor::baked_endpoint_near(double e, double n, double radius)
{
	const Dictionary j = baked_join_near(e, n, radius);
	return j.is_empty() ? Vector2(INFINITY, INFINITY) : Vector2(j["point"]);
}

// The same with the way: {"point", "rec", "at_start": the way begins here}.
Dictionary SceneryEditor::baked_join_near(double e, double n, double radius)
{
	load_ways_near(e, n);
	const Vector2 q(e, n);
	Dictionary best;
	double best_d = radius;
	const Array replace_ids = design->get_replace_ids();
	const Array wids = ways.keys();
	for (int k = 0; k < wids.size(); k++)
	{
		if (replace_ids.has(wids[k]))
			continue;
		const Dictionary rec = ways[wids[k]];
		const Array pts = rec.get("pts", Array());
		if (pts.size() < 2)
			continue;
		const int idxs[2] = {0, int(pts.size()) - 1};
		for (int idx : idxs)
		{
			const Array pt = pts[idx];
			const Vector2 p = Vector2(double(pt[0]), double(pt[1]));
			const double d = p.distance_to(q);
			if (d < best_d)
			{
				best_d = d;
				best = Dictionary();
				best["point"] = p;
				best["rec"] = rec;
				best["at_start"] = idx == 0;
			}
		}
	}
	return best;
}

String SceneryEditor::way_label(const Dictionary &rec)
{
	if (rec.is_empty())
		return vformat("no baked road within %.0f m", PICK_M);
	String extra;
	if (bool(rec.get("bridge", false)))
		extra += " bridge";
	if (bool(rec.get("tunnel", false)))
		extra += " tunnel";
	if (String(rec.get("kind", "")) == "rail")
		return vformat("track %s, gauge %d mm%s (OSM way %d)", String(rec.get("cls", "")), int(rec.get("gauge", 0)), extra, int64_t(rec.get("id", 0)));
	return vformat("%s, %d lanes%s%s (OSM way %d)", String(rec.get("cls", "")), int(rec.get("lanes", 0)), bool(rec.get("oneway", false)) ? String(", one-way") : String(), extra, int64_t(rec.get("id", 0)));
}

// The nodes of an imported way keep the baked height: each gets the offset
// between the drawn road surface and the terrain. On the terrain alone a
// roundabout beside the A11 cutting sank 4 m into the trench the bake's
// corridor had dug under it, and its terrain patch then buried the motorway
// (2026-10-25). The road lookup still holds a way just picked: hiding it
// takes effect at the next tile reload. Returns the nodes moved.
int SceneryEditor::adopt_baked_heights(const Array &ids)
{
	const Array rms = road_managers();
	int lifted = 0;
	for (int i = 0; i < ids.size(); i++)
	{
		const int nid = ids[i];
		const Vector2 p = design->node_pos(nid);
		const double ground = elevation_lv95(p.x, p.y);
		if (std::isnan(ground))
			continue;
		double road = NAN;
		for (int k = 0; k < rms.size() && std::isnan(road); k++)
		{
			Object *rm = rms[k];
			if (rm != nullptr && rm->has_method("surface_height_at"))
				road = (double)rm->call("surface_height_at", p.x, p.y, ground, 15.0);
		}
		if (std::isnan(road))
			continue;
		const double off = road - 0.20 /* SceneryDesignMesher::ROAD_LIFT_M, the live pavement over the ground */ - ground;
		if (std::abs(off) > 0.1)
		{
			design->set_node_offset(nid, off);
			lifted++;
		}
	}
	return lifted;
}

// Pick tool: the baked way under the cursor becomes design nodes / edges.
bool SceneryEditor::pick_way()
{
	if (!cursor_valid)
		return false;
	const Dictionary rec = way_near(cursor_e, cursor_n);
	if (rec.is_empty())
	{
		tool_note = way_label(rec);
		return false;
	}
	edit_begins();
	const Array ids = design->import_way(rec);
	const int lifted = adopt_baked_heights(ids);
	adopt_attrs(rec);
	tool_note = vformat("picked %s: %d nodes%s; the panel now has its settings, extend it with 2 Road from an end node", way_label(rec), ids.size(),
						lifted > 0 ? vformat(" (%d keep the baked height over the ground)", lifted) : String());
	mesher->rebuild(design);
	return true;
}

// ── tools ─────────────────────────────────────────────────────────────────

// Typed spin box values count even before Enter: apply the text.
void SceneryEditor::commit_spins()
{
	SpinBox *spins[4] = {lanes_spin, width_spin, radius_spin, area_h};
	for (SpinBox *sp : spins)
		if (sp)
			sp->apply();
	for (int g = 0; g < 4; g++)
		for (int a = 0; a < 3; a++)
			if (field_spin[g][a])
				field_spin[g][a]->apply();
}

Dictionary SceneryEditor::current_attrs()
{
	commit_spins();
	if (tool == TOOL_RAIL)
	{
		int gauge = 1435;
		if (gauge_option && gauge_option->get_selected() == 1)
			gauge = 1000;
		return SceneryDesign::rail_attrs(gauge == 1435 ? "rail" : "narrow_gauge", gauge);
	}
	const String cls = cls_option ? String(SceneryDesign::get_road_classes()[cls_option->get_selected()]) : String("secondary");
	const int lanes = lanes_spin ? int(lanes_spin->get_value()) : 2;
	const bool oneway = oneway_check ? oneway_check->is_pressed() : false;
	const double width = width_spin ? width_spin->get_value() : 0.0;
	return SceneryDesign::road_attrs(cls, lanes, oneway, width);
}

// Load a road's or track's attributes into the panel, so the next segment
// drawn continues it with the same class, lanes, direction and width.
void SceneryEditor::adopt_attrs(const Dictionary &attrs)
{
	if (String(attrs.get("kind", "")) == "rail")
	{
		if (gauge_option)
			gauge_option->select(int(attrs.get("gauge", 1435)) == 1000 ? 1 : 0);
		return;
	}
	const String cls = attrs.get("cls", "secondary");
	const Array classes = SceneryDesign::get_road_classes();
	if (cls_option && classes.has(cls))
		cls_option->select(classes.find(cls));
	if (lanes_spin)
		lanes_spin->set_value(MAX(1, int(attrs.get("lanes", 2))));
	if (oneway_check)
		oneway_check->set_pressed(bool(attrs.get("oneway", false)));
	if (width_spin)
		width_spin->set_value(double(attrs.get("width", 0.0)));
}

// Node at the cursor: an existing one when snapped, a split of a crossed
// edge, else a new node.
int SceneryEditor::node_at_cursor()
{
	int nid = node_at_screen(mouse);
	if (nid < 0)
		nid = design->node_near(cursor_e, cursor_n, SNAP_NODE_M);
	if (nid >= 0)
		return nid;
	Dictionary hit = edge_at_screen(mouse);
	if (hit.is_empty())
		hit = design->edge_near(cursor_e, cursor_n, SNAP_EDGE_M);
	if (!hit.is_empty())
		return design->split_edge(int(hit["id"]), double(hit["e"]), double(hit["n"]));
	return design->add_node(cursor_e, cursor_n);
}

// Left click with the road / rail tool: continue the line to the cursor.
bool SceneryEditor::place_point()
{
	if (!cursor_valid)
		return false;
	edit_begins();
	Dictionary via;
	if (line_last_node >= 0)
		via = next_segment_via(design->node_pos(line_last_node), Vector2(cursor_e, cursor_n));
	// joining a baked road's end: the line takes that road's settings
	const Dictionary join = baked_join_near(cursor_e, cursor_n, 0.5);
	if (!join.is_empty())
	{
		adopt_attrs(join["rec"]);
		line_join[line_last_node >= 0 ? "end" : "start"] = join;
	}
	const int nid = node_at_cursor();
	if (line_last_node >= 0 && nid != line_last_node)
	{
		const int eid = design->add_edge(line_last_node, nid, current_attrs());
		if (eid > 0 && !via.is_empty())
			design->set_edge_via(eid, via["via"]);
		mesher->rebuild(design);
	}
	if (line_last_node < 0)
		line_nodes.clear();
	line_nodes.push_back(nid);
	line_last_node = nid;
	return true;
}

// A line that joins a one-way baked road must flow with it: into the road
// when it joins the road's start, out of it when it joins the road's end.
// The line's edges are reversed when they were drawn the other way, and
// every edge of the line takes the joined road's settings.
void SceneryEditor::orient_line_to_join()
{
	if (line_nodes.size() < 2)
		return;
	const Dictionary join = line_join.get("end", line_join.get("start", Dictionary()));
	if (join.is_empty())
		return;
	const Dictionary rec = join["rec"];
	Dictionary attrs;
	if (String(rec.get("kind", "road")) == "rail")
		attrs = SceneryDesign::rail_attrs(rec.get("cls", "rail"), int(rec.get("gauge", 1435)), MAX(1, int(rec.get("tracks", 1))));
	else
		attrs = SceneryDesign::road_attrs(rec.get("cls", "secondary"), int(rec.get("lanes", 0)), bool(rec.get("oneway", false)), double(rec.get("width", 0.0)));
	const Dictionary edges = design->get_edges();
	Array line_edges;
	for (int i = 0; i < line_nodes.size() - 1; i++)
	{
		const int na = line_nodes[i], nb = line_nodes[i + 1];
		const Array adj = design->edges_of(na);
		for (int k = 0; k < adj.size(); k++)
		{
			const Dictionary ed = edges[adj[k]];
			const int a = ed["a"], b = ed["b"];
			if ((a == na && b == nb) || (b == na && a == nb))
				line_edges.push_back(adj[k]);
		}
	}
	for (int i = 0; i < line_edges.size(); i++)
		design->set_edge_attrs(line_edges[i], attrs);
	if (!bool(attrs.get("oneway", false)))
		return;
	// drawn towards the join ("end") and the road starts there: flows in,
	// fine; drawn towards the road's end: must flow out of it -> reverse
	const bool joined_at_end = line_join.has("end");
	const bool road_starts_here = join["at_start"];
	const bool reverse = (joined_at_end && !road_starts_here) || (!joined_at_end && road_starts_here);
	if (reverse)
	{
		for (int i = 0; i < line_edges.size(); i++)
		{
			Dictionary ed = edges[line_edges[i]];
			const Variant a = ed["a"];
			ed["a"] = ed["b"];
			ed["b"] = a;
		}
		design->set_dirty(true);
		tool_note = "line reversed to flow with the one-way road it joins";
	}
}

// Switch tool: first click on a track = the toe of the turnout, second
// click = the end of the diverging track (a tangent arc, radius shown).
bool SceneryEditor::place_switch_point()
{
	if (!cursor_valid)
		return false;
	if (switch_toe.is_empty())
	{
		const Dictionary hit = design->edge_near(cursor_e, cursor_n, SNAP_NODE_M);
		if (hit.is_empty() || String(Dictionary(design->get_edges()[int(hit["id"])])["kind"]) != "rail")
		{
			tool_note = "click a track first";
			return false;
		}
		switch_toe = Dictionary();
		switch_toe["edge"] = int(hit["id"]);
		switch_toe["e"] = double(hit["e"]);
		switch_toe["n"] = double(hit["n"]);
		tool_note = "now click the end of the diverging track";
		return true;
	}
	edit_begins();
	const Dictionary res = design->add_switch(int(switch_toe["edge"]), double(switch_toe["e"]), double(switch_toe["n"]), Vector2(cursor_e, cursor_n));
	if (res.has("error"))
	{
		design->undo();
		tool_note = vformat("switch refused: %s", String(res["error"]));
		return false;
	}
	tool_note = vformat("switch placed, radius %.0f m", double(res["radius"]));
	switch_toe = Dictionary();
	preview_mesh->clear_surfaces();
	mesher->rebuild(design);
	return true;
}

// Replace / flatten / area tools: clicks add polygon corners, Enter closes
// it (three or more); the bake cuts every OSM road / track inside a replace
// polygon.
bool SceneryEditor::place_polygon_point()
{
	if (!cursor_valid)
		return false;
	if (tool == TOOL_FLATTEN && poly_points.is_empty())
	{
		// a click on a corner of a finished plateau selects it: [ ] then move
		// its height live, 4 Delete removes it
		const int fi = design->flatten_near(cursor_e, cursor_n, SNAP_NODE_M);
		if (fi >= 0)
		{
			select_flatten(fi);
			return true;
		}
		selected_flatten = -1;
		flatten_h = cursor_h; // the plateau starts at the ground of the first corner; [ ] adjust it
	}
	poly_points.push_back(Vector2(cursor_e, cursor_n));
	String what = "replace area";
	if (tool == TOOL_FLATTEN)
		what = "plateau";
	else if (tool == TOOL_AREA)
		what = area_type ? String(SceneryDesign::get_area_types()[area_type->get_selected()]) : String("area");
	tool_note = vformat("%d corners, Enter closes the %s", poly_points.size(), what);
	return true;
}

void SceneryEditor::finish_polygon()
{
	if (poly_points.size() >= 3)
	{
		edit_begins();
		if (tool == TOOL_FLATTEN)
		{
			Dictionary f;
			f["poly"] = poly_points.duplicate();
			f["h"] = std::isnan(flatten_h) ? cursor_h : flatten_h;
			design->get_flatten().push_back(f);
			tool_note = vformat("plateau added at %.1f m (%d), baked with --design --corridor", double(f["h"]), design->get_flatten().size());
		}
		else if (tool == TOOL_AREA)
		{
			const String typ = area_type ? String(SceneryDesign::get_area_types()[area_type->get_selected()]) : String("grass");
			Dictionary a;
			a["poly"] = poly_points.duplicate();
			a["type"] = typ;
			a["h"] = (area_h && typ == "building") ? area_h->get_value() : 0.0;
			design->get_areas().push_back(a);
			tool_note = vformat("%s area added (%d)", typ, design->get_areas().size());
			mesher->rebuild(design);
		}
		else
		{
			design->get_replace().push_back(poly_points.duplicate());
			tool_note = vformat("replace area added (%d), baked with --design", design->get_replace().size());
		}
		design->set_dirty(true);
		rebuild_live(); // a plateau reloads the close tiles at once
	}
	poly_points.clear();
	flatten_h = NAN;
	preview_mesh->clear_surfaces();
}

// [ and ]: the selected node's height offset, or the plateau height while a
// flatten polygon is being drawn.
void SceneryEditor::select_flatten(int fi)
{
	selected_flatten = fi;
	if (fi < 0 || fi >= design->get_flatten().size())
	{
		selected_flatten = -1;
		return;
	}
	const Dictionary f = design->get_flatten()[fi];
	tool_note = vformat("plateau %d selected, at %.1f m ([ ] { } move it 0.5 / 5 m, the terrain follows at once; 4 Delete removes it)", fi + 1, double(f["h"]));
	update_overlay();
}

double SceneryEditor::selected_flatten_height() const
{
	if (selected_flatten < 0 || selected_flatten >= design->get_flatten().size())
		return NAN;
	return double(Dictionary(design->get_flatten()[selected_flatten])["h"]);
}

void SceneryEditor::nudge_height(double delta)
{
	if (tool == TOOL_OBJECT)
	{
		SceneryObjects *so = objects();
		if (so && !selection.empty())
		{
			so->begin_edit_group();
			for (int idx : selection)
			{
				const Dictionary d = so->get_object_info(idx);
				so->set_object_transform(idx, double(d["e"]), double(d["n"]), double(d["agl_m"]) + delta, Basis(d["rot"]), Vector3(d["scale"]));
			}
			so->end_edit_group();
			selection_changed();
		}
		return;
	}
	if (tool == TOOL_FLATTEN && !poly_points.is_empty())
	{
		flatten_h = (std::isnan(flatten_h) ? cursor_h : flatten_h) + delta;
		tool_note = vformat("plateau height %.1f m", flatten_h);
		return;
	}
	if (selected_flatten >= 0 && selected_flatten < design->get_flatten().size() && (tool == TOOL_FLATTEN || (tool == TOOL_SELECT && selected_node < 0 && selected_edge < 0)))
	{
		// a finished plateau: its height changes in place, the close tiles
		// reload with it (the bake writes the same height)
		edit_begins();
		Dictionary f = design->get_flatten()[selected_flatten];
		f["h"] = double(f["h"]) + delta;
		design->get_flatten()[selected_flatten] = f;
		design->set_dirty(true);
		tool_note = vformat("plateau %d now at %.1f m: the tiles around it reload with the new height ([ ] { } to change, 4 Delete on a corner removes it)", selected_flatten + 1, double(f["h"]));
		rebuild_live();
		return;
	}
	if (tool == TOOL_SELECT && selected_node >= 0 && design->get_nodes().has(selected_node))
	{
		edit_begins();
		design->set_node_offset(selected_node, design->node_offset(selected_node) + delta);
		tool_note = vformat("node %d height offset %+.1f m", selected_node, design->node_offset(selected_node));
		rebuild_live();
		return;
	}
	if (tool == TOOL_SELECT && selected_edge >= 0 && design->get_edges().has(selected_edge))
	{
		edit_begins();
		const Dictionary ed = design->get_edges()[selected_edge];
		const int a = ed["a"], b = ed["b"];
		design->set_node_offset(a, design->node_offset(a) + delta);
		design->set_node_offset(b, design->node_offset(b) + delta);
		tool_note = vformat("%s: both ends now %+.1f / %+.1f m", edge_label(selected_edge), design->node_offset(a), design->node_offset(b));
		rebuild_live();
	}
}

// Circle tool: first click = centre, second click = a point on the ring (or
// the panel radius when the clicks coincide). A one-way ring of eight arcs,
// counter-clockwise: a roundabout for right-hand traffic.
bool SceneryEditor::place_circle_point()
{
	if (!cursor_valid)
		return false;
	const Vector2 p(cursor_e, cursor_n);
	if (!circle_pending)
	{
		circle_centre = p;
		circle_pending = true;
		return true;
	}
	double r = circle_centre.distance_to(p);
	if (r < 5.0)
		r = radius_spin ? radius_spin->get_value() : 20.0;
	edit_begins();
	const String cls = cls_option ? String(SceneryDesign::get_road_classes()[cls_option->get_selected()]) : String("secondary");
	const int lanes = lanes_spin ? int(lanes_spin->get_value()) : 1;
	design->add_circle(circle_centre, r, SceneryDesign::road_attrs(cls, lanes, true));
	if (radius_spin)
		radius_spin->set_value(r);
	circle_pending = false;
	preview_mesh->clear_surfaces();
	mesher->rebuild(design);
	return true;
}

void SceneryEditor::finish_line()
{
	if (line_last_node >= 0 && design->degree(line_last_node) == 0)
		design->get_nodes().erase(line_last_node); // a lone first click
	if (line_last_node >= 0)
	{
		orient_line_to_join();
		rebuild_live();
	}
	line_last_node = -1;
	line_nodes.clear();
	line_join = Dictionary();
	preview_mesh->clear_surfaces();
}

bool SceneryEditor::delete_at_cursor()
{
	if (tool == TOOL_OBJECT)
	{
		SceneryObjects *so = objects();
		if (so == nullptr || selection.empty())
			return false;
		so->begin_edit_group();
		for (int idx : selection)
			so->remove_object(idx);
		so->end_edit_group();
		tool_note = vformat("%d object(s) removed from their pack (Ctrl+Z restores them, Ctrl+S makes it final)", (int)selection.size());
		clear_selection();
		return true;
	}
	Dictionary hit = edge_at_screen(mouse);
	if (hit.is_empty() && cursor_valid)
		hit = design->edge_near(cursor_e, cursor_n, SNAP_NODE_M);
	if (hit.is_empty() && cursor_valid)
	{
		// a baked OSM road: it cannot leave the tiles before the next bake,
		// but its id goes to the design so the bake drops it
		const Dictionary rec = way_near(cursor_e, cursor_n);
		if (!rec.is_empty())
		{
			edit_begins();
			design->get_replace_ids().push_back(int64_t(rec["id"]));
			design->set_dirty(true);
			tool_note = vformat("%s removed (hidden in the tiles; the next bake drops it for good)", way_label(rec));
			apply_hidden_ways();
			return true;
		}
	}
	if (!cursor_valid)
		return false;
	if (hit.is_empty())
	{
		const int pi = design->replace_near(cursor_e, cursor_n, SNAP_NODE_M);
		const int ai = design->area_near(cursor_e, cursor_n, SNAP_NODE_M);
		int fi = design->flatten_near(cursor_e, cursor_n, SNAP_NODE_M);
		if (pi < 0 && fi < 0 && ai < 0)
			fi = design->flatten_inside(cursor_e, cursor_n); // a click inside a plateau removes it too
		if (pi < 0 && fi < 0 && ai < 0)
			return false;
		edit_begins();
		if (pi >= 0)
			design->get_replace().remove_at(pi);
		else if (fi >= 0)
		{
			design->get_flatten().remove_at(fi);
			selected_flatten = -1;
			tool_note = vformat("plateau %d removed, the tiles around it reload (%d left)", fi + 1, design->get_flatten().size());
		}
		else
		{
			design->get_areas().remove_at(ai);
			mesher->rebuild(design);
		}
		design->set_dirty(true);
		update_overlay();
		return true;
	}
	edit_begins();
	design->remove_edge(int(hit["id"]));
	tool_note = "edge removed";
	rebuild_live();
	return true;
}

void SceneryEditor::undo()
{
	if (tool == TOOL_OBJECT)
	{
		SceneryObjects *so = objects();
		if (so && so->undo_object_edit())
		{
			tool_note = "object edit undone";
			selection_changed();
			return;
		}
	}
	if (design->undo())
	{
		line_last_node = -1;
		patches_stale = true;
		rebuild_live();
	}
}

bool SceneryEditor::save()
{
	const bool ok = design->save(design_path());
	save_note = vformat("%s %s at %s", ok ? String("saved") : String("COULD NOT SAVE"), design->get_path(), Time::get_singleton()->get_time_string_from_system());
	if (SceneryObjects *so = objects())
	{
		if (so->has_unsaved_edits())
		{
			const int n = so->save_packs();
			save_note += vformat(", %d object pack(s) written", n);
		}
	}
	UtilityFunctions::print("SceneryEditor: " + save_note);
	update_status();
	return ok;
}

void SceneryEditor::set_tool(int t)
{
	tool = t;
	line_last_node = -1;
	drag_node = -1;
	circle_pending = false;
	switch_toe = Dictionary();
	poly_points.clear();
	flatten_h = NAN;
	selected_node = -1;
	selected_edge = -1;
	selected_flatten = -1;
	selected_object = -1;
	selection.clear();
	line_nodes.clear();
	line_join = Dictionary();
	tool_note = "";
	preview_mesh->clear_surfaces();
	const Array keys = tool_buttons.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		Button *b = Object::cast_to<Button>(tool_buttons[keys[i]]);
		if (b)
			b->set_pressed(int(keys[i]) == t);
	}
	if (road_box)
		road_box->set_visible(t == TOOL_ROAD || t == TOOL_CIRCLE);
	if (rail_box)
		rail_box->set_visible(t == TOOL_RAIL);
	if (circle_box)
		circle_box->set_visible(t == TOOL_CIRCLE);
	if (area_box)
		area_box->set_visible(t == TOOL_AREA);
	if (curved_check)
		curved_check->set_visible(t == TOOL_ROAD || t == TOOL_RAIL);
	if (mode_bar)
		mode_bar->set_visible(t == TOOL_OBJECT);
	if (object_panel)
		object_panel->set_visible(t == TOOL_OBJECT);
	gizmo_axis = AXIS_NONE;
	gizmo_handles.clear();
	if (gizmo_overlay)
		gizmo_overlay->queue_redraw();
	if (t == TOOL_OBJECT)
		selection_changed();
}

void SceneryEditor::on_mode_button(int mode)
{
	set_gizmo_mode(mode);
}

int SceneryEditor::get_gizmo_mode() const { return gizmo_mode; }

void SceneryEditor::set_gizmo_mode(int m)
{
	gizmo_mode = CLAMP(m, (int)GIZMO_MOVE, (int)GIZMO_SCALE);
	gizmo_axis = AXIS_NONE;
	const Array keys = mode_buttons.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		Button *b = Object::cast_to<Button>(mode_buttons[keys[i]]);
		if (b)
			b->set_pressed(int(keys[i]) == gizmo_mode);
	}
}

int SceneryEditor::get_tool() const
{
	return tool;
}

void SceneryEditor::on_tool_button(int t)
{
	set_tool(t);
}

void SceneryEditor::_unhandled_input(const Ref<InputEvent> &event)
{
	if (!active)
		return;
	handle_mouse(event);
}

// A Control under the mouse (other than the pass-through gizmo overlay)
// owns presses and wheel turns: the panels stop mouse events for the GUI, but
// an event no control accepts is not marked handled, so it would reach
// _unhandled_input as well and zoom the camera behind the Transform panel.
bool SceneryEditor::gui_owns_mouse(Viewport *vp) const
{
	Control *hov = vp ? vp->gui_get_hovered_control() : nullptr;
	return hov != nullptr && hov != gizmo_overlay;
}

void SceneryEditor::handle_mouse(const Ref<InputEvent> &event)
{
	Viewport *vp = host ? host->get_viewport() : nullptr;
	if (vp == nullptr)
		return;
	Ref<InputEventMouseButton> mb = event;
	if (mb.is_valid())
	{
		const MouseButton idx = mb->get_button_index();
		const bool wheel = idx == MOUSE_BUTTON_WHEEL_UP || idx == MOUSE_BUTTON_WHEEL_DOWN;
		if ((mb->is_pressed() || wheel) && gui_owns_mouse(vp))
			return;
		if (idx == MOUSE_BUTTON_RIGHT)
		{
			if (mb->is_pressed())
			{
				looking = true;
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
			}
			else
			{
				looking = false;
				Input::get_singleton()->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
				if (line_last_node >= 0)
					finish_line();
			}
			vp->set_input_as_handled();
		}
		else if (idx == MOUSE_BUTTON_WHEEL_UP || idx == MOUSE_BUTTON_WHEEL_DOWN)
		{
			if (mb->is_pressed())
			{
				const double a = agl();
				const double stepm = MAX(5.0, (std::isnan(a) ? 0.0 : a) * 0.15);
				const double dir = idx == MOUSE_BUTTON_WHEEL_UP ? 1.0 : -1.0;
				pos += (-cam->get_global_transform().basis.get_column(2)) * stepm * dir;
				apply_camera();
			}
			vp->set_input_as_handled();
		}
		else if (idx == MOUSE_BUTTON_LEFT)
		{
			if (mb->is_pressed())
			{
				switch (tool)
				{
					case TOOL_ROAD:
					case TOOL_RAIL:
						place_point();
						break;
					case TOOL_CIRCLE:
						place_circle_point();
						break;
					case TOOL_SWITCH:
						place_switch_point();
						break;
					case TOOL_REPLACE:
					case TOOL_FLATTEN:
					case TOOL_AREA:
						place_polygon_point();
						break;
					case TOOL_PICK:
						pick_way();
						break;
					case TOOL_DELETE:
						delete_at_cursor();
						break;
					case TOOL_OBJECT:
					{
						SceneryObjects *so = objects();
						const int handle = gizmo_hit(mb->get_position());
						if (handle != AXIS_NONE && so && !selection.empty())
						{
							gizmo_begin(handle, mb->get_position()); // a gizmo handle: constrained drag
							break;
						}
						// Shift-click adds to / removes from the selection; a click on a
						// selected object keeps the selection (so the whole group drags)
						const bool shift = mb->is_shift_pressed();
						const int under = so ? so->pick_ray(cam->project_ray_origin(mb->get_position()), cam->project_ray_normal(mb->get_position())) : -1;
						if (under >= 0)
						{
							const bool was = std::find(selection.begin(), selection.end(), under) != selection.end();
							if (shift)
							{
								if (was)
								{
									selection.erase(std::find(selection.begin(), selection.end(), under));
									selected_object = selection.empty() ? -1 : selection.back();
									selection_changed();
									break;
								}
								select_object(under, true);
							}
							else if (!was)
								select_object(under, false);
							else
							{
								selected_object = under;
								selection_changed();
							}
							// nothing moves until the mouse does (drag threshold); Move slides
							// the selection in its ground plane, Rotate turns it, Scale is uniform
							update_gizmo();
							gizmo_begin(gizmo_mode == GIZMO_MOVE ? (int)AXIS_FREE : gizmo_mode == GIZMO_ROTATE ? (int)AXIS_ROT_UP : (int)AXIS_UNIFORM, mb->get_position());
							tool_note = "";
						}
						else if (!shift)
						{
							clear_selection();
							tool_note = "no pack object under the mouse (models load within 8 km of the camera)";
						}
						break;
					}
					case TOOL_SELECT:
					{
						int nid = node_at_screen(mb->get_position());
						if (nid < 0 && cursor_valid)
							nid = design->node_near(cursor_e, cursor_n, SNAP_NODE_M);
						selected_node = nid;
						selected_edge = -1;
						if (nid >= 0)
						{
							edit_begins();
							drag_node = nid;
							const Array adj = design->edges_of(nid);
							if (adj.size() > 0)
								adopt_attrs(design->get_edges()[adj[0]]); // extending from here continues the same road
							tool_note = vformat("node %d selected, height offset %+.1f m ([ ] to change, drag to move)", nid, design->node_offset(nid));
						}
						else
						{
							const Dictionary eh = edge_at_screen(mb->get_position());
							if (!eh.is_empty())
							{
								selected_edge = int(eh["id"]);
								adopt_attrs(design->get_edges()[selected_edge]);
								tool_note = vformat("%s selected ([ ] raises / lowers both ends, 4 deletes); its settings are in the panel", edge_label(selected_edge));
							}
							else if (cursor_valid && (design->flatten_near(cursor_e, cursor_n, SNAP_NODE_M) >= 0 || design->flatten_inside(cursor_e, cursor_n) >= 0))
							{
								// a corner within reach, else the (smallest) plateau around the click
								const int fi = design->flatten_near(cursor_e, cursor_n, SNAP_NODE_M);
								select_flatten(fi >= 0 ? fi : design->flatten_inside(cursor_e, cursor_n));
							}
							else
							{
								selected_flatten = -1;
								tool_note = "nothing under the mouse (yellow spheres are nodes, blue roads are design roads)";
							}
						}
						update_markers();
						break;
					}
					default:
						break;
				}
			}
			else if (drag_node >= 0)
			{
				drag_node = -1;
				rebuild_live();
			}
			else if (gizmo_axis != AXIS_NONE)
			{
				if (!gizmo_moved && objects())
					objects()->undo_object_edit(); // a plain click: drop the undo point recorded at the press
				gizmo_axis = AXIS_NONE;
				gizmo_moved = false;
				refresh_object_panel();
			}
			vp->set_input_as_handled();
		}
		return;
	}
	Ref<InputEventMouseMotion> mm = event;
	if (mm.is_valid() && gizmo_axis != AXIS_NONE)
	{
		// precision: with Shift held the mouse counts a tenth (Blender's Shift drag)
		const Vector2 delta = mm->get_position() - gizmo_last_mouse;
		gizmo_last_mouse = mm->get_position();
		gizmo_eff_mouse += delta * (mm->is_shift_pressed() ? 0.1 : 1.0);
		if (!gizmo_moved && gizmo_eff_mouse.distance_to(gizmo_start_mouse) < 4.0)
		{
			vp->set_input_as_handled();
			return;
		}
		gizmo_moved = true;
		gizmo_drag(gizmo_eff_mouse);
		vp->set_input_as_handled();
		return;
	}
	if (mm.is_valid() && tool == TOOL_OBJECT)
		gizmo_hover = gizmo_hit(mm->get_position());
	if (mm.is_valid() && looking)
	{
		yaw += mm->get_relative().x * LOOK_SENS; // mouse right = turn right (heading up)
		pitch = CLAMP(pitch - mm->get_relative().y * LOOK_SENS, -1.5, 1.5);
		apply_camera();
		vp->set_input_as_handled();
	}
}

// Keyboard shortcuts run ahead of the GUI (a focused spin box or option
// button would otherwise swallow Ctrl+S).
void SceneryEditor::_input(const Ref<InputEvent> &event)
{
	if (!active)
		return;
	handle_key(event);
}

void SceneryEditor::handle_key(const Ref<InputEvent> &event)
{
	Ref<InputEventKey> k = event;
	if (k.is_null())
		return;
	modifier_down = k->is_ctrl_pressed() || k->is_meta_pressed();
	if (!k->is_pressed() || k->is_echo())
		return;
	// typing into a panel field: the field gets the keys (this runs in _input,
	// before the GUI); Escape leaves the field and returns the keys here
	if (Viewport *vp = host ? host->get_viewport() : nullptr)
	{
		Control *owner = vp->gui_get_focus_owner();
		const bool in_panel = owner != nullptr && ((panel && panel->is_ancestor_of(owner)) || (object_panel && object_panel->is_ancestor_of(owner)));
		if (in_panel && (Object::cast_to<LineEdit>(owner) != nullptr || Object::cast_to<TextEdit>(owner) != nullptr))
		{
			if (k->get_keycode() == KEY_ESCAPE)
			{
				commit_spins();
				vp->gui_release_focus();
				vp->set_input_as_handled();
			}
			return;
		}
	}
	bool handled = true;
	const bool cmd = k->is_ctrl_pressed() || k->is_meta_pressed();
	// Every shortcut goes by the keyboard mapping (Settings > Keyboard). The
	// mapping matches a printable key by the typed character too, so "[" is
	// Option+5 on a Swiss or German Mac and "Shift+[" is the { of any layout.
	// An Option press that macOS reports with the digit alone (no character):
	// Option+5 6 8 9 are [ ] { }.
	char32_t uni = char32_t(k->get_unicode());
	if (uni == 0 && k->is_alt_pressed() && !cmd)
	{
		const Key digit = (k->get_keycode() >= KEY_0 && k->get_keycode() <= KEY_9) ? k->get_keycode() : k->get_key_label();
		switch (digit)
		{
			case KEY_5: uni = '['; break;
			case KEY_6: uni = ']'; break;
			case KEY_8: uni = '{'; break;
			case KEY_9: uni = '}'; break;
			default: break;
		}
	}
	auto is = [&](const char *action) {
		return KeyBindings::matches_key(action, k->get_keycode(), uni, k->is_shift_pressed(), cmd, k->is_alt_pressed());
	};
	struct ToolKey
	{
		const char *action;
		int tool;
	};
	static const ToolKey TOOL_KEYS[] = {
		{"tool_select", TOOL_SELECT}, {"tool_road", TOOL_ROAD}, {"tool_rail", TOOL_RAIL}, {"tool_delete", TOOL_DELETE},
		{"tool_circle", TOOL_CIRCLE}, {"tool_switch", TOOL_SWITCH}, {"tool_replace", TOOL_REPLACE}, {"tool_flatten", TOOL_FLATTEN},
		{"tool_area", TOOL_AREA}, {"tool_pick", TOOL_PICK}, {"tool_object", TOOL_OBJECT}};
	int tool_hit = -1;
	for (const ToolKey &tk : TOOL_KEYS)
		if (is(tk.action))
		{
			tool_hit = tk.tool;
			break;
		}
	if (tool_hit >= 0)
		set_tool(tool_hit);
	else if (tool == TOOL_OBJECT && object_key(k, uni))
	{
		// gizmo mode, frame, heading, scale, drop (scenery_editor_objects.cpp)
	}
	else if (is("view_top_north"))
		view_top(true);
	else if (is("view_top"))
		view_top(false);
	else if (is("reverse_or_rotate"))
	{
		if (tool == TOOL_OBJECT)
			set_gizmo_mode(GIZMO_ROTATE);
		else if (selected_edge >= 0 && design->get_edges().has(selected_edge))
		{
			edit_begins();
			const int n = design->reverse_chain(selected_edge);
			tool_note = vformat(String::utf8("%s reversed (%d edges): it now flows %.0f° at the selected point"), edge_label(selected_edge), n, heading_at(SceneryDesignMesher::chain_polyline(design, chain_of(selected_edge)), Vector2(cursor_e, cursor_n)));
			rebuild_live();
		}
		else
			tool_note = vformat("%s reverses a road: select one with %s Select first", KeyBindings::label("reverse_or_rotate"), KeyBindings::label("tool_select"));
	}
	else if (is("height_down_big"))
		nudge_height(-HEIGHT_STEP_M * 10.0);
	else if (is("height_up_big"))
		nudge_height(HEIGHT_STEP_M * 10.0);
	else if (is("height_down"))
		nudge_height(-HEIGHT_STEP_M);
	else if (is("height_up"))
		nudge_height(HEIGHT_STEP_M);
	else if (is("curved_toggle"))
	{
		if (curved_check)
			curved_check->set_pressed(!curved_check->is_pressed());
	}
	else if (is("finish"))
	{
		if (tool == TOOL_REPLACE || tool == TOOL_FLATTEN || tool == TOOL_AREA)
			finish_polygon();
		else
			finish_line();
	}
	else if (is("cancel"))
	{
		if (line_last_node >= 0)
			finish_line();
		else if (circle_pending || !switch_toe.is_empty() || !poly_points.is_empty())
		{
			circle_pending = false;
			switch_toe = Dictionary();
			poly_points.clear();
			preview_mesh->clear_surfaces();
		}
		else if (selected_node >= 0 || selected_edge >= 0 || selected_flatten >= 0 || !selection.empty())
		{
			selected_node = -1;
			selected_edge = -1;
			selected_flatten = -1;
			clear_selection();
			preview_mesh->clear_surfaces();
			update_markers();
			update_overlay();
		}
		else
		{
			host->set("_escape_held", true); // the host must not open the welcome screen on this press
			exit();
		}
	}
	else if (is("undo"))
		undo();
	else if (is("save"))
		save();
	else if (is("bake"))
		bake_area();
	else if (is("delete"))
		delete_at_cursor();
	else
		handled = false;
	if (handled && host->get_viewport())
		host->get_viewport()->set_input_as_handled();
}

// ── bake from the editor ──────────────────────────────────────────────────

// The road manager's paths, with the region id of each (the parent of the
// region directory: .../build/switzerland-base/Switzerland/roads).
Array SceneryEditor::road_regions() const
{
	Array out;
	const Array rms = road_managers();
	for (int r = 0; r < rms.size(); r++)
	{
		Node *rm = Object::cast_to<Node>(rms[r]);
		const PackedStringArray dirs = rm->call("get_road_paths");
		for (int d = 0; d < dirs.size(); d++)
		{
			const String region_dir = dirs[d].get_base_dir();                // .../Switzerland
			const String region_id = region_dir.get_base_dir().get_file(); // switzerland-base
			if (region_id != "" && FileAccess::file_exists(region_dir.path_join("terrain").path_join("manifest.json")))
			{
				Dictionary rec;
				rec["id"] = region_id;
				rec["roads"] = dirs[d];
				rec["region_dir"] = region_dir;
				out.push_back(rec);
			}
		}
	}
	return out;
}

// pixels_per_tile of a terrain/ directory's manifest (0 = none).
static int terrain_px(const String &terrain_dir)
{
	const String mf = terrain_dir.path_join("manifest.json");
	if (!FileAccess::file_exists(mf))
		return 0;
	const Variant v = JSON::parse_string(FileAccess::get_file_as_string(mf));
	if (v.get_type() != Variant::DICTIONARY)
		return 0;
	return int(Dictionary(v).get("pixels_per_tile", 0));
}

PackedStringArray SceneryEditor::terrain_overlays(const String &region_dir) const
{
	PackedStringArray out;
	if (host == nullptr || host->get_tree() == nullptr)
		return out;
	const String base_dir = region_dir.path_join("terrain");
	const int base_px = terrain_px(base_dir);
	const TypedArray<Node> found = host->get_tree()->get_root()->find_children("*", "Scenery3D", true, false);
	for (int i = 0; i < found.size(); i++)
	{
		Node *s = Object::cast_to<Node>(static_cast<Object *>(found[i]));
		if (s == nullptr)
			continue;
		const PackedStringArray paths = s->get("data_paths");
		for (int p = 0; p < paths.size(); p++)
		{
			String dir = paths[p];
			if (!FileAccess::file_exists(dir.path_join("manifest.json")))
				dir = dir.path_join("terrain");
			if (dir == base_dir || out.has(dir))
				continue;
			if (terrain_px(dir) > base_px)
				out.push_back(dir);
		}
	}
	return out;
}

// The bake for the tiles around the camera: [script, args...]; empty when
// no region or script is available. Finer terrains drawn over the region's
// go along as --terrain-overlay, so the ribbons sit on the terrain one sees. Terrain writes (--corridor): always
// once the region has a pristine terrain_base (the bake derives every tile
// of the box from it, so a deleted plateau or way restores the ground);
// before that only when the design has something that needs them.
Array SceneryEditor::bake_command() const
{
	Array args;
	Node *ir = host->get_node_or_null(NodePath("/root/InstallRoot"));
	const String root = (ir && ir->has_method("get_install_root")) ? String(ir->call("get_install_root")) : String();
	const String script = root.path_join("scripts").path_join("build_world_roads.py");
	if (root == "" || !FileAccess::file_exists(script))
		return args;
	const Array regions = road_regions();
	if (regions.is_empty())
		return args;
	const geo::Geodetic g = geo::geodetic_of(pos);
	const Vector2 en = Geo::wgs84_to_lv95(g.lat, g.lon);
	const int e0 = int(std::floor(en.x / 1000.0)) * 1000 - BAKE_HALF_KM * 1000;
	const int n0 = int(std::floor(en.y / 1000.0)) * 1000 - BAKE_HALF_KM * 1000;
	args.push_back(script);
	args.push_back("--region");
	args.push_back(String(Dictionary(regions[0])["id"]));
	args.push_back("--bbox");
	args.push_back(String::num_int64(e0));
	args.push_back(String::num_int64(n0));
	args.push_back(String::num_int64(e0 + 2 * BAKE_HALF_KM * 1000 + 1000));
	args.push_back(String::num_int64(n0 + 2 * BAKE_HALF_KM * 1000 + 1000));
	args.push_back("--design");
	args.push_back(design_path());
	const String region_dir = Dictionary(regions[0])["region_dir"];
	const PackedStringArray overlays = terrain_overlays(region_dir);
	for (int i = 0; i < overlays.size(); i++)
	{
		args.push_back("--terrain-overlay");
		args.push_back(overlays[i]);
	}
	const bool has_base = DirAccess::dir_exists_absolute(region_dir.path_join("terrain_base"));
	if (has_base || design->edge_count() > 0 || design->get_flatten().size() > 0)
		args.push_back("--corridor");
	return args;
}

bool SceneryEditor::bake_running() const
{
	return bake_pid > 0 && OS::get_singleton()->is_process_running(bake_pid);
}

// Save, then run the bake for the tiles around the camera in the
// background; the tiles reload when it finishes (see update()).
bool SceneryEditor::bake_area()
{
	if (bake_running())
	{
		tool_note = "a bake is already running";
		return false;
	}
	save();
	const Array cmd = bake_command();
	Node *ir = host->get_node_or_null(NodePath("/root/InstallRoot"));
	const String root = (ir && ir->has_method("get_install_root")) ? String(ir->call("get_install_root")) : String();
	if (cmd.is_empty())
	{
		tool_note = "cannot bake here: no region with terrain among the road paths, or scripts/build_world_roads.py missing";
		return false;
	}
	bake_log = OS::get_singleton()->get_user_data_dir().path_join("bake_area.log");
	PackedStringArray quoted;
	for (int i = 0; i < cmd.size(); i++)
		quoted.push_back("'" + String(cmd[i]).replace("'", "'\\''") + "'");
	// The repo's virtualenv (Python 3.12, shapely 2.1 with constrained
	// Delaunay for the pavement patches, osmium) when it is there, else the
	// system python3 (exported builds).
	String python = "python3";
	const String venv = root.path_join(".venv").path_join("bin").path_join("python3");
	if (FileAccess::file_exists(venv))
		python = "'" + venv + "'";
	const String shell = vformat("%s %s > '%s' 2>&1", python, String(" ").join(quoted), bake_log);
	PackedStringArray sh_args;
	sh_args.push_back("-c");
	sh_args.push_back(shell);
	bake_pid = OS::get_singleton()->create_process("/bin/sh", sh_args);
	bake_started = Time::get_singleton()->get_ticks_msec();
	tool_note = vformat("baking %d x %d km around the camera… (log: %s)", 2 * BAKE_HALF_KM + 1, 2 * BAKE_HALF_KM + 1, bake_log);
	UtilityFunctions::print("SceneryEditor: bake started: " + shell);
	return bake_pid > 0;
}

// After the bake: reload the road tiles, forget the sidecar cache, and let
// design_is_baked() find the new manifest signature.
void SceneryEditor::bake_finished()
{
	const double secs = (Time::get_singleton()->get_ticks_msec() - bake_started) / 1000.0;
	String tail;
	if (FileAccess::file_exists(bake_log))
	{
		const PackedStringArray lines = FileAccess::get_file_as_string(bake_log).strip_edges().split("\n");
		tail = lines.size() > 0 ? lines[lines.size() - 1] : String();
	}
	const Array rms = road_managers();
	for (int r = 0; r < rms.size(); r++)
	{
		Node *rm = Object::cast_to<Node>(rms[r]);
		if (rm->has_method("reload_tiles"))
			rm->call("reload_tiles");
	}
	ways_tiles.clear();
	ways.clear();
	baked_sha = "";
	rebuild_pending = true;
	streamer_was_busy = true;
	tool_note = vformat("bake finished in %.0f s: %s", secs, tail.substr(0, 160));
	UtilityFunctions::print("SceneryEditor: " + tool_note);
}

// ── UI ────────────────────────────────────────────────────────────────────

static Label *make_label(const String &text, int font_size = 0)
{
	Label *l = memnew(Label);
	l->set_text(text);
	if (font_size > 0)
		l->add_theme_font_size_override("font_size", font_size);
	return l;
}

static SpinBox *make_spin(double min, double max, double value, double step = 1.0)
{
	SpinBox *s = memnew(SpinBox);
	s->set_min(min);
	s->set_max(max);
	s->set_step(step);
	s->set_value(value);
	return s;
}

static Button *make_button(const String &text)
{
	Button *b = memnew(Button);
	b->set_text(text);
	b->set_focus_mode(Control::FOCUS_NONE);
	return b;
}

void SceneryEditor::on_panel_mouse_exited()
{
	// also fires while the tree is torn down: nothing to do then
	if (host == nullptr || !is_inside_tree() || !host->is_inside_tree())
		return;
	commit_spins();
	if (Viewport *vp = host->get_viewport())
		vp->gui_release_focus(); // keys go back to the editor
}

void SceneryEditor::on_save_button()
{
	save();
}

void SceneryEditor::on_bake_button()
{
	bake_area();
}

void SceneryEditor::build_ui()
{
	ui = memnew(CanvasLayer);
	ui->set_name("SceneryEditorUI");
	ui->set_layer(20);
	ui->set_visible(false);
	host->add_child(ui);
	panel = memnew(PanelContainer);
	panel->set_position(Vector2(12, 12));
	panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	panel->connect("mouse_exited", Callable(this, "_on_panel_mouse_exited"));
	ui->add_child(panel);
	VBoxContainer *box = memnew(VBoxContainer);
	panel->add_child(box);
	box->add_child(make_label(vformat("Scenery editor  (%s exit)", KeyBindings::label("editor_toggle")), 16));
	HBoxContainer *tools = memnew(HBoxContainer);
	box->add_child(tools);
	struct ToolSpec
	{
		int tool;
		const char *action; // the shortcut (Settings > Keyboard) in front of the name
		const char *name;
	};
	static const ToolSpec SPECS[11] = {
		{TOOL_SELECT, "tool_select", "Select"}, {TOOL_ROAD, "tool_road", "Road"}, {TOOL_RAIL, "tool_rail", "Rail"}, {TOOL_DELETE, "tool_delete", "Delete"}, {TOOL_CIRCLE, "tool_circle", "Circle"},
		{TOOL_SWITCH, "tool_switch", "Switch"}, {TOOL_REPLACE, "tool_replace", "Replace"}, {TOOL_FLATTEN, "tool_flatten", "Flatten"}, {TOOL_AREA, "tool_area", "Area"}, {TOOL_PICK, "tool_pick", "Pick"},
		{TOOL_OBJECT, "tool_object", "Object"}};
	for (const ToolSpec &spec : SPECS)
	{
		Button *b = make_button(KeyBindings::label(spec.action) + " " + spec.name);
		b->set_toggle_mode(true);
		b->connect("pressed", Callable(this, "_on_tool_button").bind(spec.tool));
		tools->add_child(b);
		tool_buttons[spec.tool] = b;
	}
	HBoxContainer *rb = memnew(HBoxContainer);
	road_box = rb;
	box->add_child(rb);
	cls_option = memnew(OptionButton);
	cls_option->set_focus_mode(Control::FOCUS_NONE);
	const Array classes = SceneryDesign::get_road_classes();
	for (int i = 0; i < classes.size(); i++)
		cls_option->add_item(classes[i]);
	cls_option->select(3);
	rb->add_child(cls_option);
	rb->add_child(make_label(" lanes"));
	lanes_spin = make_spin(1, 6, 2);
	rb->add_child(lanes_spin);
	oneway_check = memnew(CheckBox);
	oneway_check->set_text("one-way");
	oneway_check->set_focus_mode(Control::FOCUS_NONE);
	rb->add_child(oneway_check);
	rb->add_child(make_label(" width m (0 = from lanes)"));
	width_spin = make_spin(0, 60, 0, 0.5);
	rb->add_child(width_spin);
	curved_check = memnew(CheckBox);
	curved_check->set_text("curved (C)");
	curved_check->set_tooltip_text("Each new segment continues the last one as a tangent arc");
	curved_check->set_focus_mode(Control::FOCUS_NONE);
	box->add_child(curved_check);
	HBoxContainer *ab = memnew(HBoxContainer);
	area_box = ab;
	box->add_child(ab);
	area_type = memnew(OptionButton);
	area_type->set_focus_mode(Control::FOCUS_NONE);
	const Array types = SceneryDesign::get_area_types();
	for (int i = 0; i < types.size(); i++)
		area_type->add_item(types[i]);
	ab->add_child(area_type);
	ab->add_child(make_label(" building height m"));
	area_h = make_spin(2, 120, 9);
	ab->add_child(area_h);
	HBoxContainer *cb = memnew(HBoxContainer);
	circle_box = cb;
	box->add_child(cb);
	cb->add_child(make_label("radius m (click centre, then the ring) "));
	radius_spin = make_spin(5, 500, 20);
	cb->add_child(radius_spin);
	HBoxContainer *rlb = memnew(HBoxContainer);
	rail_box = rlb;
	box->add_child(rlb);
	rlb->add_child(make_label("gauge "));
	gauge_option = memnew(OptionButton);
	gauge_option->set_focus_mode(Control::FOCUS_NONE);
	gauge_option->add_item("1435 mm standard");
	gauge_option->add_item("1000 mm metre");
	rlb->add_child(gauge_option);
	HBoxContainer *actions = memnew(HBoxContainer);
	box->add_child(actions);
	Button *undo_b = make_button(vformat("Undo (%s)", KeyBindings::label("undo")));
	undo_b->connect("pressed", Callable(this, "undo"));
	actions->add_child(undo_b);
	Button *save_b = make_button(vformat("Save (%s)", KeyBindings::label("save")));
	save_b->connect("pressed", Callable(this, "_on_save_button"));
	actions->add_child(save_b);
	Button *bake_b = make_button(vformat("Bake area (%s)", KeyBindings::label("bake")));
	bake_b->set_tooltip_text(vformat("Save, then bake the road tiles %d km around the camera with this design and reload them", 2 * BAKE_HALF_KM + 1));
	bake_b->connect("pressed", Callable(this, "_on_bake_button"));
	actions->add_child(bake_b);
	gizmo = memnew(SceneryViewGizmo);
	gizmo->set_editor(this);
	gizmo->set_custom_minimum_size(Vector2(120, 120));
	gizmo->set_size(Vector2(120, 120));
	gizmo->set_anchor(SIDE_LEFT, 1.0);
	gizmo->set_anchor(SIDE_RIGHT, 1.0);
	gizmo->set_anchor(SIDE_TOP, 0.0);
	gizmo->set_anchor(SIDE_BOTTOM, 0.0);
	gizmo->set_offset(SIDE_LEFT, -136);
	gizmo->set_offset(SIDE_RIGHT, -16);
	gizmo->set_offset(SIDE_TOP, 352);
	gizmo->set_offset(SIDE_BOTTOM, 472);
	gizmo->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	ui->add_child(gizmo);
	// Blender-style tool column of the Object tool: Move / Rotate / Scale
	VBoxContainer *bar = memnew(VBoxContainer);
	bar->set_anchor(SIDE_LEFT, 0.0);
	bar->set_anchor(SIDE_TOP, 0.5);
	bar->set_anchor(SIDE_BOTTOM, 0.5);
	bar->set_offset(SIDE_LEFT, 12);
	bar->set_offset(SIDE_TOP, -70);
	bar->set_visible(false);
	ui->add_child(bar);
	mode_bar = bar;
	struct ModeSpec
	{
		int mode;
		const char *label;
		const char *tip;
	};
	static const ModeSpec MODES[3] = {
		{GIZMO_MOVE, "\u2725  Move  (G)", "Drag the red / green / blue arrows (east / north / up), or the model itself over the terrain"},
		{GIZMO_ROTATE, "\u21bb  Rotate  (R)", "Drag around the ring: true heading"},
		{GIZMO_SCALE, "\u2922  Scale  (Y)", "Drag the cubes or the model: uniform scale"}};
	for (const ModeSpec &m : MODES)
	{
		Button *b = make_button(String::utf8(m.label));
		b->set_toggle_mode(true);
		b->set_tooltip_text(m.tip);
		b->set_custom_minimum_size(Vector2(150, 34));
		b->connect("pressed", Callable(this, "_on_mode_button").bind(m.mode));
		bar->add_child(b);
		mode_buttons[m.mode] = b;
	}
	set_gizmo_mode(GIZMO_MOVE);
	gizmo_overlay = memnew(SceneryGizmoOverlay);
	gizmo_overlay->set_editor(this);
	gizmo_overlay->set_anchors_preset(Control::PRESET_FULL_RECT);
	gizmo_overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	ui->add_child(gizmo_overlay);
	build_object_panel();
	status = make_label("", 12);
	box->add_child(status);
	Label *help = make_label(vformat("%s %s %s %s move, %s / %s descend / climb, %s fast, right mouse = look, wheel = closer\nleft click = place point (snaps to nodes, splits crossed lines), %s / right click = finish line, %s = cancel (keys: Settings > Keyboard)",
		KeyBindings::label("editor_forward"), KeyBindings::label("editor_left"), KeyBindings::label("editor_back"), KeyBindings::label("editor_right"),
		KeyBindings::labels("editor_down"), KeyBindings::labels("editor_up"), KeyBindings::label("editor_fast"), KeyBindings::labels("finish"), KeyBindings::label("cancel")), 11);
	help->set_modulate(Color(0.8, 0.8, 0.8));
	box->add_child(help);
}

void SceneryEditor::update_status()
{
	if (status == nullptr)
		return;
	String cur = String::utf8("cursor: —");
	if (cursor_valid)
		cur = vformat("cursor: E %.0f  N %.0f  h %.0f m", cursor_e, cursor_n, cursor_h);
	String line;
	const Vector2 cq(cursor_e, cursor_n);
	if (line_last_node >= 0)
	{
		const Vector2 a = design->node_pos(line_last_node);
		const Dictionary via = cursor_valid ? next_segment_via(a, cq) : Dictionary();
		line = vformat("\nline from node %d, %.0f m to cursor%s", line_last_node, a.distance_to(cq), via.is_empty() ? String() : vformat(", arc radius %.0f m", double(via["radius"])));
	}
	else if (circle_pending && cursor_valid)
		line = vformat("\ncircle radius %.0f m", circle_centre.distance_to(cq));
	else if (tool == TOOL_SWITCH && !switch_toe.is_empty() && cursor_valid)
	{
		const int eid = switch_toe["edge"];
		if (design->get_edges().has(eid))
		{
			const Vector2 t = design->track_tangent_at(eid, double(switch_toe["e"]), double(switch_toe["n"]), cq);
			const Vector2 toe = design->nearest_on_edge(eid, Vector2(double(switch_toe["e"]), double(switch_toe["n"])))["p"];
			const Dictionary via = SceneryDesign::tangent_arc_via(toe, t, cq);
			String r = "—";
			if (!via.is_empty())
			{
				const double rad = via["radius"];
				const double min_r = SceneryDesign::get_switch_min_radius_m();
				r = vformat("%.0f m%s", rad, rad >= min_r ? String() : vformat("  (too tight, min %.0f)", min_r));
			}
			line = "\nturnout radius " + r;
		}
	}
	if (tool == TOOL_OBJECT)
		line += object_status();
	if ((tool == TOOL_PICK || tool == TOOL_SELECT) && cursor_valid)
	{
		const Dictionary deh = edge_at_screen(mouse);
		if (!deh.is_empty())
		{
			const int eid = deh["id"];
			const double flow = heading_at(SceneryDesignMesher::chain_polyline(design, chain_of(eid)), cq);
			const bool oneway = Dictionary(design->get_edges()[eid])["oneway"];
			line += vformat(String::utf8("\nunder the cursor: %s, flows %.0f°%s (R reverses it when selected)"), edge_label(eid), flow, oneway ? String() : String(" (two-way)"));
		}
		else
		{
			const Dictionary rec = way_near(cursor_e, cursor_n);
			if (rec.is_empty())
				line += "\nunder the cursor: " + way_label(rec);
			else
			{
				Array pts;
				const Array raw = rec.get("pts", Array());
				for (int i = 0; i < raw.size(); i++)
				{
					const Array p = raw[i];
					pts.push_back(Vector2(double(p[0]), double(p[1])));
				}
				line += vformat(String::utf8("\nunder the cursor: %s, flows %.0f°"), way_label(rec), heading_at(pts, cq));
			}
		}
	}
	if (line_last_node >= 0 && cursor_valid)
	{
		const Vector2 v = cq - design->node_pos(line_last_node);
		if (v.length() > 1.0)
			line += vformat(String::utf8("\ndrawing towards %.0f°: a one-way road flows this way (first click -> last click)"), Math::fposmod(Math::rad_to_deg(std::atan2(v.x, v.y)), 360.0));
	}
	if (bake_running())
		line += vformat("\nbaking… %.0f s", (Time::get_singleton()->get_ticks_msec() - bake_started) / 1000.0);
	if (tool_note != "")
		line += "\n" + tool_note;
	const String baked = (baked_sha != "" && !design->get_dirty()) ? String("  (baked into the road tiles, live meshes off)") : String();
	if (tool == TOOL_FLATTEN && !poly_points.is_empty())
		line += vformat("\nplateau height %.1f m ([ ] to change)", std::isnan(flatten_h) ? cursor_h : flatten_h);
	else if (!std::isnan(selected_flatten_height()) && (tool == TOOL_FLATTEN || (tool == TOOL_SELECT && selected_node < 0 && selected_edge < 0)))
		line += vformat("\nplateau %d at %.1f m ([ ] { } to change)", selected_flatten + 1, selected_flatten_height());
	const int unknown = mesher->get_unknown_height_count();
	status->set_text(vformat("%s%s\n%d nodes, %d edges, %d replace areas, %d replaced OSM ways, %d plateaus, %d areas, %d meshes, %d piers%s%s\n%s%s%s",
		cur, line, design->node_count(), design->edge_count(), design->get_replace().size(), design->get_replace_ids().size(), design->get_flatten().size(), design->get_areas().size(),
		mesher->get_chain_count() + mesher->get_area_count(), mesher->get_pier_count(), baked,
		unknown > 0 ? vformat("  (%d stations without terrain)", unknown) : String(),
		design->get_path(), design->get_dirty() ? String("  *unsaved*") : String(), save_note != "" ? "\n" + save_note : String()));
}

// ── accessors ─────────────────────────────────────────────────────────────

Node3D *SceneryEditor::get_host() const { return host; }
Ref<SceneryDesign> SceneryEditor::get_design() const { return design; }
void SceneryEditor::set_design(const Ref<SceneryDesign> &d) { if (d.is_valid()) design = d; }
SceneryDesignMesher *SceneryEditor::get_mesher() const { return mesher; }
bool SceneryEditor::get_active() const { return active; }
Camera3D *SceneryEditor::get_camera() const { return cam; }
SceneryViewGizmo *SceneryEditor::get_gizmo() const { return gizmo; }
Vector3 SceneryEditor::get_camera_position() const { return pos; }
void SceneryEditor::set_camera_position(const Vector3 &p) { pos = p; }
double SceneryEditor::get_yaw() const { return yaw; }
void SceneryEditor::set_yaw(double v) { yaw = v; }
double SceneryEditor::get_pitch() const { return pitch; }
void SceneryEditor::set_pitch(double v) { pitch = v; }
Vector2 SceneryEditor::get_mouse() const { return mouse; }
void SceneryEditor::set_mouse(const Vector2 &m) { mouse = m; }
bool SceneryEditor::get_cursor_valid() const { return cursor_valid; }
void SceneryEditor::set_cursor_valid(bool v) { cursor_valid = v; }
double SceneryEditor::get_cursor_e() const { return cursor_e; }
void SceneryEditor::set_cursor_e(double v) { cursor_e = v; }
double SceneryEditor::get_cursor_n() const { return cursor_n; }
void SceneryEditor::set_cursor_n(double v) { cursor_n = v; }
int SceneryEditor::get_selected_edge() const { return selected_edge; }
void SceneryEditor::set_selected_edge(int v) { selected_edge = v; }
int SceneryEditor::get_selected_node() const { return selected_node; }
void SceneryEditor::set_selected_node(int v) { selected_node = v; }
bool SceneryEditor::get_modifier_down() const { return modifier_down; }
void SceneryEditor::set_modifier_down(bool v) { modifier_down = v; }
String SceneryEditor::get_tool_note() const { return tool_note; }
Array SceneryEditor::get_line_nodes() const { return line_nodes.duplicate(); }
OptionButton *SceneryEditor::get_class_option() const { return cls_option; }
SpinBox *SceneryEditor::get_lanes_spin() const { return lanes_spin; }
CheckBox *SceneryEditor::get_oneway_check() const { return oneway_check; }
CheckBox *SceneryEditor::get_curved_check() const { return curved_check; }
Ref<ImmediateMesh> SceneryEditor::get_direction_overlay_mesh() const { return dir_overlay_mesh; }

} // namespace jsbgodot
