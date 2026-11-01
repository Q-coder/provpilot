#include "fg_controls.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/geometry3d.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/reg_ex.hpp>
#include <godot_cpp/classes/reg_ex_match.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <limits>

#include "fg_animator.h"

using namespace godot;

namespace jsbgodot
{

// Nasal script (prefix) -> what to do.
static const struct { const char *prefix; const char *kind; } NASAL_MAP[] = {
	{"electrical.land_lights_switcher", "land_lights"},
	{"electrical.nav_lights_switcher", "nav_lights"},
	{"canopy.canopy_switch", "canopy"},
	{"canopy.canopy_hold", "canopy_hold"},
	{"A10fuel.fuel_sel_knob_move", "fuel_sel"},
	{"A10fuel.fill_dis_toggle", "fill_dis"},
	{"apu.toggle_off_start_switch", "apu_switch"},
	{"nav_scripts.nav_mode_selector", "nav_mode"},
	{"provpilot.gear_toggle", "gear_toggle"}, // ProvPilot-defined pick on the gear handle
};

void FGControls::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("setup", "manifest_path", "model_root", "animator"), &FGControls::setup);
	ClassDB::bind_method(D_METHOD("set_debug", "on"), &FGControls::set_debug);
	ClassDB::bind_method(D_METHOD("get_debug"), &FGControls::get_debug);
	ClassDB::bind_method(D_METHOD("hit", "camera", "screen_pos"), &FGControls::hit);
	ClassDB::bind_method(D_METHOD("hover_name", "camera", "screen_pos"), &FGControls::hover_name);
	ClassDB::bind_method(D_METHOD("click", "camera", "screen_pos", "button", "pressed", "shift"), &FGControls::click);
	ClassDB::bind_method(D_METHOD("value_of", "prop"), &FGControls::value_of);
	ClassDB::bind_method(D_METHOD("load_shims", "path"), &FGControls::load_shims);
	ClassDB::bind_method(D_METHOD("set_knob_actions", "specs"), &FGControls::set_knob_actions);
	ClassDB::bind_method(D_METHOD("get_knob_actions"), &FGControls::get_knob_actions);
	ClassDB::bind_method(D_METHOD("get_shim_count"), &FGControls::get_shim_count);
	ClassDB::bind_method(D_METHOD("binding_properties"), &FGControls::binding_properties);
	ClassDB::bind_method(D_METHOD("tick", "delta"), &FGControls::tick);
	ClassDB::bind_method(D_METHOD("get_items"), &FGControls::get_items);
	ClassDB::bind_method(D_METHOD("debug_box_count"), &FGControls::debug_box_count);
	ClassDB::bind_method(D_METHOD("get_state"), &FGControls::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &FGControls::set_state);
	ClassDB::bind_method(D_METHOD("get_on_change"), &FGControls::get_on_change);
	ClassDB::bind_method(D_METHOD("set_on_change", "callable"), &FGControls::set_on_change);
	ClassDB::bind_method(D_METHOD("get_count"), &FGControls::get_count);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "state"), "set_state", "get_state");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "on_change"), "set_on_change", "get_on_change");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug"), "set_debug", "get_debug");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count"), "", "get_count");
}

FGControls::FGControls()
{
}

Dictionary FGControls::get_state() const { return state; }
void FGControls::set_state(const Dictionary &s) { state = s; }
Callable FGControls::get_on_change() const { return on_change; }
void FGControls::set_on_change(const Callable &c) { on_change = c; }
int FGControls::get_count() const { return count; }
bool FGControls::get_debug() const { return debug; }

MeshInstance3D *FGControls::item_node(const Item &it)
{
	if (it.node_id == 0 || !UtilityFunctions::is_instance_id_valid(it.node_id))
	{
		return nullptr;
	}
	return Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(it.node_id));
}

int FGControls::setup(const String &manifest_path, Node *model_root, FGAnimator *p_animator)
{
	items.clear();
	animator = p_animator;
	count = 0;
	Ref<FileAccess> f = FileAccess::open(manifest_path, FileAccess::READ);
	if (f.is_null() || model_root == nullptr)
	{
		return 0;
	}
	const Dictionary m = JSON::parse_string(f->get_as_text());
	// mesh instance id -> records
	Dictionary by_node;
	Array controls = Array(m.get("controls", Array())).duplicate();
	// cfg knob_actions: a rotate / knob animation on a listed property with no
	// binding becomes a knob with a property-adjust action (step, min, max)
	int synthesized = 0;
	if (!knob_actions.is_empty())
	{
		const Array anims = m.get("animations", Array());
		for (int s = 0; s < knob_actions.size(); s++)
		{
			const PackedStringArray parts = String(knob_actions[s]).split(":");
			if (parts.is_empty() || parts[0].strip_edges().is_empty())
			{
				continue;
			}
			const String prop = norm(parts[0]);
			const String step = parts.size() > 1 ? parts[1].strip_edges() : String("0.05");
			const String mn = parts.size() > 2 ? parts[2].strip_edges() : String("0");
			const String mx = parts.size() > 3 ? parts[3].strip_edges() : String("1");
			for (int a = 0; a < anims.size(); a++)
			{
				const Dictionary anim = anims[a];
				const String type = anim.get("type", "");
				const Dictionary raw = anim.get("raw", Dictionary());
				if ((type != "rotate" && type != "knob") || norm(raw.get("property", "")) != prop ||
						raw.has("action") || raw.has("increase") || raw.has("decrease"))
				{
					continue;
				}
				Dictionary rec = anim.duplicate(true);
				Dictionary r = rec["raw"];
				Dictionary b;
				b["command"] = "property-adjust";
				b["property"] = String(raw.get("property", ""));
				b["factor"] = step;
				b["min"] = mn;
				b["max"] = mx;
				b["wrap"] = "false";
				Dictionary act;
				act["binding"] = b;
				r["action"] = act;
				rec["raw"] = r;
				rec["type"] = "knob";
				controls.push_back(rec);
				synthesized++;
			}
		}
	}
	int inert = 0, hidden = 0;
	for (int r = 0; r < controls.size(); r++)
	{
		const Dictionary rec = controls[r];
		const bool actionable = record_actionable(rec);
		// FlightGear's <visible>false</visible>: a sense surface that is never
		// drawn (the halves of a three-position switch, a knob's click zones);
		// it leaves the camera's layers and keeps its mesh for the ray
		const bool invisible = String(Dictionary(rec.get("raw", Dictionary())).get("visible", "")).to_lower() == "false";
		if (!actionable)
		{
			inert++;
			if (!invisible)
			{
				continue;
			}
		}
		const Array objects = rec.get("objects", Array());
		// "exact": the record names this import's own part (a file used twice);
		// the Blender duplicate suffix must not pull in the other copies
		const bool exact = bool(rec.get("exact", false));
		for (int o = 0; o < objects.size(); o++)
		{
			const String name = objects[o];
			TypedArray<Node> found = model_root->find_children(name, "", true, false);
			if (!exact)
			{
				found.append_array(model_root->find_children(name + String("_???"), "", true, false));
			}
			for (int i = 0; i < found.size(); i++)
			{
				Node *n = Object::cast_to<Node>(found[i]);
				if (n == nullptr)
				{
					continue;
				}
				// a control may be a group (AC3D object with kids): every mesh below it is clickable
				Vector<MeshInstance3D *> meshes;
				MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n);
				if (mi != nullptr && mi->get_mesh().is_valid())
				{
					meshes.push_back(mi);
				}
				const TypedArray<Node> kids = n->find_children("*", "MeshInstance3D", true, false);
				for (int k = 0; k < kids.size(); k++)
				{
					MeshInstance3D *c = Object::cast_to<MeshInstance3D>(kids[k]);
					if (c != nullptr && c->get_mesh().is_valid())
					{
						meshes.push_back(c);
					}
				}
				for (int k = 0; k < meshes.size(); k++)
				{
					if (invisible)
					{
						if (meshes[k]->get_layer_mask() != 0)
						{
							hidden++;
						}
						meshes[k]->set_layer_mask(0);
						meshes[k]->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
					}
					if (!actionable)
					{
						continue;
					}
					const int64_t id = (int64_t)meshes[k]->get_instance_id();
					if (!by_node.has(id))
					{
						by_node[id] = Array();
					}
					Array recs = by_node[id];
					recs.push_back(rec);
					by_node[id] = recs;
				}
			}
		}
	}
	const Array ids = by_node.keys();
	for (int i = 0; i < ids.size(); i++)
	{
		Item it;
		it.node_id = (uint64_t)(int64_t)ids[i];
		MeshInstance3D *mi = item_node(it);
		if (mi == nullptr)
		{
			continue;
		}
		it.aabb = mi->get_aabb();
		it.recs = by_node[ids[i]];
		items.push_back(it);
		count++;
	}
	cycle_values.clear();
	for (int r = 0; r < controls.size(); r++)
	{
		collect_cycle_values(Dictionary(controls[r]).get("raw", Dictionary()));
	}
	if (inert > 0)
	{
		UtilityFunctions::print("FGControls: ", inert, " control records without an executable binding left out");
	}
	if (hidden > 0)
	{
		UtilityFunctions::print("FGControls: ", hidden, " invisible sense surfaces taken out of the camera's layers");
	}
	if (synthesized > 0)
	{
		UtilityFunctions::print("FGControls: ", synthesized, " knobs given a property-adjust action from the cfg (knob_actions)");
	}
	return count;
}

// Every value the cockpit's property-cycle bindings list for each property,
// gathered over all records: a three-position switch is usually two picks
// (its halves) cycling sub-ranges of the same property, and only a list
// covering all of a property's values is a toggle allowed to wrap
void FGControls::collect_cycle_values(const Variant &v)
{
	if (v.get_type() == Variant::ARRAY)
	{
		const Array a = v;
		for (int i = 0; i < a.size(); i++)
		{
			collect_cycle_values(a[i]);
		}
		return;
	}
	if (v.get_type() != Variant::DICTIONARY)
	{
		return;
	}
	const Dictionary d = v;
	if (String(d.get("command", "")) == "property-cycle" && d.has("property"))
	{
		const String key = norm(d["property"]);
		PackedFloat64Array vals = cycle_values.get(key, PackedFloat64Array());
		const Array lst = as_list(d.get("value", Array()));
		for (int i = 0; i < lst.size(); i++)
		{
			const double x = num(lst[i]);
			bool known = false;
			for (int k = 0; k < vals.size() && !known; k++)
			{
				known = std::abs(vals[k] - x) < 1e-6;
			}
			if (!known)
			{
				vals.push_back(x);
			}
		}
		cycle_values[key] = vals;
	}
	const Array keys = d.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		collect_cycle_values(d[keys[i]]);
	}
}

bool FGControls::nasal_actionable(const String &script) const
{
	const String s = script.strip_edges();
	if (s.is_empty())
	{
		return false;
	}
	const PackedStringArray stmts = s.split(";", false);
	for (int i = 0; i < stmts.size(); i++)
	{
		const String st = stmts[i].strip_edges();
		for (int j = 0; j < shims.size(); j++)
		{
			if (shims[j].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const String match = Dictionary(shims[j]).get("match", "");
			if (!match.is_empty() && st.begins_with(match))
			{
				return true;
			}
		}
	}
	if (s.contains("setprop("))
	{
		return true;
	}
	for (const auto &m : NASAL_MAP)
	{
		if (s.begins_with(m.prefix))
		{
			return true;
		}
	}
	return false;
}

bool FGControls::record_actionable(const Dictionary &rec) const
{
	const Dictionary raw = rec.get("raw", Dictionary());
	// pick / slider: "action" (one or several); knob: increase / decrease, their
	// shift variants and release -- the same keys the click handlers read
	Array actions;
	for (const char *k : {"action", "increase", "decrease", "shift-increase", "shift-decrease", "release", "shift-action"})
	{
		const Variant av = raw.get(k, Variant());
		if (av.get_type() == Variant::ARRAY)
		{
			actions.append_array(av);
		}
		else if (av.get_type() == Variant::DICTIONARY)
		{
			actions.push_back(av);
		}
	}
	for (int i = 0; i < actions.size(); i++)
	{
		if (actions[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary a = actions[i];
		Array bindings;
		const Variant bv = a.get("binding", Variant());
		if (bv.get_type() == Variant::ARRAY)
		{
			bindings.append_array(bv);
		}
		else if (bv.get_type() == Variant::DICTIONARY)
		{
			bindings.push_back(bv);
		}
		const Variant mu = a.get("mod-up", Variant());
		if (mu.get_type() == Variant::DICTIONARY)
		{
			const Variant mb = Dictionary(mu).get("binding", Variant());
			if (mb.get_type() == Variant::ARRAY)
			{
				bindings.append_array(mb);
			}
			else if (mb.get_type() == Variant::DICTIONARY)
			{
				bindings.push_back(mb);
			}
		}
		for (int j = 0; j < bindings.size(); j++)
		{
			if (bindings[j].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary b = bindings[j];
			const String cmd = b.get("command", "");
			if (cmd.is_empty() || cmd == "set-tooltip")
			{
				continue;
			}
			if (cmd == "nasal")
			{
				if (nasal_actionable(b.get("script", "")))
				{
					return true;
				}
				continue;
			}
			return true;
		}
	}
	return false;
}

void FGControls::set_debug(bool on)
{
	debug = on;
	for (int i = 0; i < debug_boxes.size(); i++)
	{
		if (UtilityFunctions::is_instance_id_valid(debug_boxes[i]))
		{
			Node *b = Object::cast_to<Node>(ObjectDB::get_instance(debug_boxes[i]));
			if (b != nullptr)
			{
				b->queue_free();
			}
		}
	}
	debug_boxes.clear();
	if (!on)
	{
		return;
	}
	Ref<StandardMaterial3D> mat = memnew(StandardMaterial3D);
	mat->set_albedo(Color(0.2, 1.0, 0.4, 0.35));
	mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	mat->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	for (int i = 0; i < items.size(); i++)
	{
		MeshInstance3D *n = item_node(items[i]);
		if (n == nullptr || !n->is_visible_in_tree())
		{
			continue; // hidden parts (a sub-model switched off by its condition) take no clicks either
		}
		MeshInstance3D *box = memnew(MeshInstance3D);
		Ref<BoxMesh> bm = memnew(BoxMesh);
		const AABB aabb = items[i].aabb;
		bm->set_size(Vector3(Math::max((double)aabb.size.x, 0.002), Math::max((double)aabb.size.y, 0.002), Math::max((double)aabb.size.z, 0.002)));
		box->set_mesh(bm);
		box->set_material_override(mat);
		box->set_position(aabb.get_center());
		box->set_name("HotspotDebug");
		n->add_child(box);
		debug_boxes.push_back(box->get_instance_id());
	}
}

bool FGControls::nearest_face_hit(Item &it, const Vector3 &lf, const Vector3 &ld, Vector3 &out) const
{
	if (!it.faces_loaded)
	{
		it.faces_loaded = true;
		MeshInstance3D *n = item_node(it);
		if (n != nullptr && n->get_mesh().is_valid())
		{
			it.faces = n->get_mesh()->get_faces();
		}
	}
	if (it.faces.is_empty())
	{
		return false;
	}
	Geometry3D *geo = Geometry3D::get_singleton();
	double best_d = std::numeric_limits<double>::infinity();
	bool found = false;
	const Vector3 *f = it.faces.ptr();
	for (int i = 0; i + 2 < it.faces.size(); i += 3)
	{
		Variant hp = geo->ray_intersects_triangle(lf, ld, f[i], f[i + 1], f[i + 2]);
		if (hp.get_type() == Variant::NIL)
		{
			hp = geo->ray_intersects_triangle(lf, ld, f[i], f[i + 2], f[i + 1]);
		}
		if (hp.get_type() != Variant::NIL)
		{
			const Vector3 p = hp;
			const double d = (p - lf).length();
			if (d < best_d)
			{
				best_d = d;
				out = p;
				found = true;
			}
		}
	}
	return found;
}

int FGControls::hit_index(Camera3D *camera, const Vector2 &screen_pos)
{
	if (camera == nullptr)
	{
		return -1;
	}
	const Vector3 from = camera->project_ray_origin(screen_pos);
	const Vector3 dir = camera->project_ray_normal(screen_pos);
	int best = -1;
	double best_d = std::numeric_limits<double>::infinity();
	for (int i = 0; i < items.size(); i++)
	{
		Item &it = items.write[i];
		MeshInstance3D *n = item_node(it);
		if (n == nullptr || !n->is_visible_in_tree())
		{
			continue;
		}
		const Transform3D inv = n->get_global_transform().affine_inverse();
		const Vector3 lf = inv.xform(from);
		const Vector3 ld = (inv.basis.xform(dir)).normalized();
		bool inside = false;
		Vector3 p;
		if (!it.aabb.find_intersects_ray(lf, ld, inside, &p))
		{
			continue;
		}
		// the bounds only coarsely select: neighbouring parts (the ALT / BAT
		// halves of a rocker) overlap in depth, so take the nearest triangle
		// of the mesh itself; a mesh without faces keeps the box hit
		Vector3 hp;
		if (nearest_face_hit(it, lf, ld, hp))
		{
			p = hp;
		}
		const double d = (n->get_global_transform().xform(p) - from).length();
		if (d < best_d && d < 4.0)
		{
			best_d = d;
			best = i;
		}
	}
	return best;
}

Array FGControls::get_items() const
{
	Array out;
	for (int i = 0; i < items.size(); i++)
	{
		Dictionary d;
		d["node"] = item_node(items[i]);
		d["aabb"] = items[i].aabb;
		d["recs"] = items[i].recs;
		out.push_back(d);
	}
	return out;
}

int FGControls::debug_box_count() const
{
	return debug_boxes.size();
}

Dictionary FGControls::hit(Camera3D *camera, const Vector2 &screen_pos)
{
	Dictionary d;
	const int i = hit_index(camera, screen_pos);
	if (i >= 0)
	{
		d["node"] = item_node(items[i]);
		d["aabb"] = items[i].aabb;
		d["recs"] = items[i].recs;
	}
	return d;
}

String FGControls::hover_name(Camera3D *camera, const Vector2 &screen_pos)
{
	const int i = hit_index(camera, screen_pos);
	if (i < 0)
	{
		return String();
	}
	MeshInstance3D *n = item_node(items[i]);
	return n != nullptr ? String(n->get_name()) : String();
}

// The mouse buttons a pick action lists (FlightGear numbering: 0 left,
// 1 middle, 2 right, 3 wheel up, 4 wheel down); `button` is one value or a
// list, and absent means the left button
static PackedInt32Array buttons_of(const Dictionary &a)
{
	PackedInt32Array out;
	const Variant b = a.get("button", Variant());
	if (b.get_type() == Variant::ARRAY)
	{
		const Array arr = b;
		for (int i = 0; i < arr.size(); i++)
		{
			out.push_back((int)String(arr[i]).to_int());
		}
	}
	else if (b.get_type() != Variant::NIL)
	{
		out.push_back((int)String(b).to_int());
	}
	if (out.is_empty())
	{
		out.push_back(0);
	}
	return out;
}

// Which way a pick action moves its control: +1 up / clockwise, -1 down /
// counter-clockwise, 0 none (a push, a toggle, an assign). Read from what
// the bindings do - the sign of a property-adjust step, the order of a
// property-cycle list - and only when they say nothing (Nasal-only actions)
// from the wheel button the action is paired with (wheel up = up).
int FGControls::action_direction(const Dictionary &a)
{
	int sign = 0;
	const Array bs = bindings(a);
	for (int i = 0; i < bs.size(); i++)
	{
		if (bs[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary b = bs[i];
		const String cmd = b.get("command", "");
		int sg = 0;
		if (cmd == "property-adjust" && b.has("step"))
		{
			const double s = num(b["step"]);
			sg = s > 0 ? 1 : (s < 0 ? -1 : 0);
		}
		else if (cmd == "property-cycle")
		{
			const Array vals = as_list(b.get("value", Array()));
			bool asc = vals.size() >= 2, desc = vals.size() >= 2;
			for (int k = 0; k + 1 < vals.size(); k++)
			{
				if (!String(vals[k]).is_valid_float() || !String(vals[k + 1]).is_valid_float())
				{
					asc = desc = false;
					break;
				}
				const double v0 = num(vals[k]), v1 = num(vals[k + 1]);
				asc = asc && v1 > v0;
				desc = desc && v1 < v0;
			}
			sg = asc ? 1 : (desc ? -1 : 0);
		}
		if (sg == 0)
		{
			continue;
		}
		if (sign == 0)
		{
			sign = sg;
		}
		else if (sign != sg)
		{
			return 0;
		}
	}
	if (sign != 0)
	{
		return sign;
	}
	const PackedInt32Array btns = buttons_of(a);
	const bool up = btns.has(3), down = btns.has(4);
	if (up != down)
	{
		return up ? 1 : -1;
	}
	return 0;
}

bool FGControls::click(Camera3D *camera, const Vector2 &screen_pos, int button, bool pressed_now, bool shift)
{
	if (!pressed_now)
	{
		if (pressed.has(button))
		{
			const Array entries = pressed[button];
			pressed.erase(button);
			release(entries);
			return true;
		}
		return false;
	}
	const int i = hit_index(camera, screen_pos);
	if (i < 0)
	{
		return false;
	}
	Array ran;
	press_item(items[i].recs, button, shift, ran);
	if (ran.is_empty())
	{
		return false;
	}
	pressed[button] = ran;
	return true;
}

// One mouse button over the records of a clickable mesh. House rule (gery,
// 2026-10-31): on a control that moves both ways the left button and wheel
// down go down / counter-clockwise, the right button and wheel up go up /
// clockwise. FlightGear aircraft disagree with each other (and with
// themselves) about which of their buttons 0 / 1 / 3 / 4 is which way, so
// the direction is read from the actions (`action_direction`) and the
// listed buttons only break ties. Pushes, toggles and one-way halves keep
// FlightGear's physical numbering (0 left, 1 middle - also the right button
// when no action lists 2 - 2 right, 3 / 4 the wheel). `ran` collects
// {rec, action} for the release.
void FGControls::press_item(const Array &recs, int button, bool shift, Array &ran)
{
	bool item_has_wheel = false, has_right = false, has_up = false, has_down = false;
	for (int r = 0; r < recs.size(); r++)
	{
		const Dictionary rec = recs[r];
		if (String(rec.get("type", "")) == "knob")
		{
			continue;
		}
		const Array actions = as_list(Dictionary(rec.get("raw", Dictionary())).get("action", Array()));
		for (int k = 0; k < actions.size(); k++)
		{
			if (actions[k].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary a = actions[k];
			const PackedInt32Array btns = buttons_of(a);
			item_has_wheel = item_has_wheel || btns.has(3) || btns.has(4);
			has_right = has_right || btns.has(2);
			const int d = action_direction(a);
			has_up = has_up || d > 0;
			has_down = has_down || d < 0;
		}
	}
	const bool directional = has_up && has_down && button != MOUSE_BUTTON_MIDDLE;
	const int target = (button == MOUSE_BUTTON_LEFT || button == MOUSE_BUTTON_WHEEL_DOWN) ? -1 : 1;
	int fg_button = 0;
	switch (button)
	{
	case MOUSE_BUTTON_MIDDLE:
		fg_button = 1;
		break;
	case MOUSE_BUTTON_RIGHT:
		fg_button = has_right ? 2 : 1;
		break;
	case MOUSE_BUTTON_WHEEL_UP:
		fg_button = 3;
		break;
	case MOUSE_BUTTON_WHEEL_DOWN:
		fg_button = 4;
		break;
	default:
		fg_button = 0;
	}
	auto took = [&ran](const Dictionary &rec, const Variant &action)
	{
		Dictionary e;
		e["rec"] = rec;
		if (action.get_type() == Variant::DICTIONARY)
		{
			e["action"] = action;
		}
		ran.push_back(e);
	};
	bool explicit_ran = false;
	for (int r = 0; r < recs.size(); r++)
	{
		const Dictionary rec = recs[r];
		const Dictionary raw = rec.get("raw", Dictionary());
		const String type = rec.get("type", "");
		if (type == "knob")
		{
			if (press_knob(rec, button, shift))
			{
				took(rec, Variant());
			}
			continue;
		}
		if (type != "pick" && type != "slider")
		{
			continue;
		}
		// "shift-action" (sliders: the other direction) with Shift or the right button
		if (raw.has("shift-action") && (shift || button == MOUSE_BUTTON_RIGHT))
		{
			run(bindings(raw["shift-action"]), rec, Dictionary());
			took(rec, Variant());
			continue;
		}
		const Array actions = as_list(raw.get("action", Array()));
		// Wheel on a pick that adjusts a property but has no wheel actions
		// (Kollsman, heading bug...): up increases, down decreases by the
		// button-0 action's step, Shift five steps
		if (!directional && !item_has_wheel && (button == MOUSE_BUTTON_WHEEL_UP || button == MOUSE_BUTTON_WHEEL_DOWN))
		{
			Dictionary chosen;
			bool have = false;
			for (int i = 0; i < actions.size(); i++)
			{
				if (actions[i].get_type() == Variant::DICTIONARY && buttons_of(actions[i]).has(0))
				{
					chosen = actions[i];
					have = true;
				}
			}
			if (!have)
			{
				continue;
			}
			bool adjusts = false;
			const Array bs = bindings(chosen);
			for (int i = 0; i < bs.size(); i++)
			{
				if (bs[i].get_type() == Variant::DICTIONARY && String(Dictionary(bs[i]).get("command", "")) == "property-adjust")
				{
					adjusts = true;
				}
			}
			if (!adjusts)
			{
				continue;
			}
			Dictionary ctx;
			ctx["wheel"] = (button == MOUSE_BUTTON_WHEEL_UP ? 1.0 : -1.0) * (shift ? 5.0 : 1.0);
			run(bs, rec, ctx);
			took(rec, chosen);
			continue;
		}
		for (int i = 0; i < actions.size(); i++)
		{
			if (actions[i].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary a = actions[i];
			if (!buttons_of(a).has(fg_button))
			{
				continue;
			}
			// on a two-way control only its pushes / toggles answer by number
			if (directional && action_direction(a) != 0)
			{
				continue;
			}
			run(bindings(a), rec, Dictionary());
			took(rec, a);
			explicit_ran = true;
		}
	}
	if (!directional || explicit_ran)
	{
		return;
	}
	// the two-way control: the one action going the button's way, preferring
	// the action that lists this button (fine over coarse steps)
	Dictionary best_rec, best_action;
	bool have = false, best_lists = false;
	for (int r = 0; r < recs.size() && !best_lists; r++)
	{
		const Dictionary rec = recs[r];
		const String type = rec.get("type", "");
		if (type != "pick" && type != "slider")
		{
			continue;
		}
		const Dictionary raw = rec.get("raw", Dictionary());
		const Array actions = as_list(raw.get("action", Array()));
		for (int i = 0; i < actions.size(); i++)
		{
			if (actions[i].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary a = actions[i];
			if (action_direction(a) != target)
			{
				continue;
			}
			const bool lists = buttons_of(a).has(fg_button);
			if (!have || (lists && !best_lists))
			{
				best_rec = rec;
				best_action = a;
				have = true;
				best_lists = lists;
				if (lists)
				{
					break;
				}
			}
		}
	}
	if (have)
	{
		run(bindings(best_action), best_rec, Dictionary());
		took(best_rec, best_action);
	}
}

bool FGControls::press_knob(const Dictionary &rec, int button, bool shift)
{
	const Dictionary raw = rec.get("raw", Dictionary());
	// house rule: right click / wheel up turn the knob clockwise (FlightGear's
	// "increase"), left click / wheel down counter-clockwise
	const bool inc = button == MOUSE_BUTTON_RIGHT || button == MOUSE_BUTTON_WHEEL_UP;
	const bool dec = button == MOUSE_BUTTON_LEFT || button == MOUSE_BUTTON_WHEEL_DOWN || button == MOUSE_BUTTON_MIDDLE;
	if (raw.has("increase") || raw.has("decrease"))
	{
		String key;
		if (inc)
		{
			key = (shift && raw.has("shift-increase")) ? "shift-increase" : "increase";
		}
		else if (dec)
		{
			key = (shift && raw.has("shift-decrease")) ? "shift-decrease" : "decrease";
		}
		if (key.is_empty() || !raw.has(key))
		{
			return false;
		}
		run(bindings(raw[key]), rec, Dictionary());
		return true;
	}
	if (raw.has("action") && (inc || dec))
	{
		// Shift turns the knob <shift-repeat> steps at once (fgdata's OBS: 5)
		const double rep = shift ? Math::max(num(raw.get("shift-repeat", 1.0), 1.0), 1.0) : 1.0;
		Dictionary ctx;
		ctx["offset"] = (inc ? 1.0 : -1.0) * rep;
		run(bindings(raw["action"]), rec, ctx);
		return true;
	}
	return false;
}

// Button up: the records' `release` bindings once each, and `mod-up` of the
// actions that ran on the press (a held START position springs back)
void FGControls::release(const Array &entries)
{
	Array released;
	for (int i = 0; i < entries.size(); i++)
	{
		const Dictionary e = entries[i];
		const Dictionary rec = e.get("rec", Dictionary());
		const Dictionary raw = rec.get("raw", Dictionary());
		if (raw.has("release") && !released.has(rec))
		{
			run(bindings(raw["release"]), rec, Dictionary());
			released.push_back(rec);
		}
		if (e.has("action"))
		{
			const Dictionary a = e["action"];
			if (a.has("mod-up"))
			{
				run(bindings(a["mod-up"]), rec, Dictionary());
			}
		}
	}
}

Array FGControls::as_list(const Variant &v)
{
	if (v.get_type() == Variant::ARRAY)
	{
		return v;
	}
	if (v.get_type() == Variant::NIL || (v.get_type() == Variant::STRING && String(v).is_empty()))
	{
		return Array();
	}
	Array a;
	a.push_back(v);
	return a;
}

Array FGControls::bindings(const Variant &container)
{
	if (container.get_type() == Variant::DICTIONARY)
	{
		return as_list(Dictionary(container).get("binding", Array()));
	}
	return Array();
}

String FGControls::norm(const String &p)
{
	String s = p.strip_edges();
	while (s.begins_with("/"))
	{
		s = s.substr(1);
	}
	return s.replace("[0]", "");
}

double FGControls::num(const Variant &v, double def)
{
	switch (v.get_type())
	{
		case Variant::FLOAT:
		case Variant::INT:
		case Variant::BOOL:
			return (double)v;
		case Variant::STRING:
		{
			const String s = v;
			return s.is_valid_float() ? s.to_float() : def;
		}
		default:
			return def;
	}
}

double FGControls::value_of(const String &prop) const
{
	const String key = norm(prop);
	if (state.has(key))
	{
		return num(state[key]);
	}
	if (animator != nullptr)
	{
		const Variant v = animator->get_property(key);
		if (v.get_type() != Variant::NIL && v.get_type() != Variant::STRING)
		{
			return (double)v;
		}
		if (v.get_type() == Variant::STRING && String(v).is_valid_float())
		{
			return String(v).to_float();
		}
	}
	return 0.0;
}

void FGControls::assign(const String &prop, const Variant &v)
{
	const String key = norm(prop);
	state[key] = v;
	if (on_change.is_valid())
	{
		on_change.call(key, v);
	}
}

// A binding's <condition> (fgdata radios gate their knobs on the power
// switch): property / not / and / or / equals / not-equals / less-than /
// greater-than (and the -equals variants) against the current values.
bool FGControls::cond_ok(const Variant &c) const
{
	if (c.get_type() == Variant::STRING)
	{
		return value_of(String(c)) > 0.5;
	}
	if (c.get_type() != Variant::DICTIONARY)
	{
		return true;
	}
	const Dictionary d = c;
	const Array keys = d.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const String k = keys[i];
		const Variant body = d[keys[i]];
		bool ok = true;
		if (k == "property")
		{
			ok = value_of(String(body)) > 0.5;
		}
		else if (k == "not")
		{
			ok = !cond_ok(body);
		}
		else if (k == "and")
		{
			ok = cond_ok(body);
		}
		else if (k == "or")
		{
			ok = false;
			if (body.get_type() == Variant::DICTIONARY)
			{
				const Dictionary ob = body;
				const Array ok2 = ob.keys();
				for (int j = 0; j < ok2.size() && !ok; j++)
				{
					Dictionary one;
					one[ok2[j]] = ob[ok2[j]];
					ok = cond_ok(one);
				}
			}
		}
		else if (k == "equals" || k == "not-equals" || k == "less-than" || k == "greater-than" || k == "less-than-equals" || k == "greater-than-equals")
		{
			double lhs = 0.0, rhs = 0.0;
			if (body.get_type() == Variant::DICTIONARY)
			{
				const Dictionary cb = body;
				const Array props = as_list(cb.get("property", Array()));
				if (props.size() > 0)
				{
					lhs = value_of(String(props[0]));
				}
				rhs = props.size() > 1 ? value_of(String(props[1])) : num(cb.get("value", 0.0));
			}
			if (k == "equals")
				ok = std::abs(lhs - rhs) < 1e-9;
			else if (k == "not-equals")
				ok = std::abs(lhs - rhs) >= 1e-9;
			else if (k == "less-than")
				ok = lhs < rhs;
			else if (k == "greater-than")
				ok = lhs > rhs;
			else if (k == "less-than-equals")
				ok = lhs <= rhs;
			else
				ok = lhs >= rhs;
		}
		if (!ok)
		{
			return false;
		}
	}
	return true;
}

void FGControls::run(const Array &bs, const Dictionary &rec, const Dictionary &ctx)
{
	for (int i = 0; i < bs.size(); i++)
	{
		if (bs[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary b = bs[i];
		if (b.has("condition") && !cond_ok(b["condition"]))
		{
			continue;
		}
		const String cmd = b.get("command", "");
		const Array props = as_list(b.get("property", Array()));
		const String prop = props.size() > 0 ? String(props[0]) : String();
		if (cmd == "property-toggle")
		{
			assign(prop, value_of(prop) > 0.5 ? 0.0 : 1.0);
		}
		else if (cmd == "property-assign")
		{
			const Variant val = b.get("value", 0);
			if (val.get_type() == Variant::STRING && !String(val).is_valid_float())
			{
				assign(prop, String(val));
			}
			else
			{
				assign(prop, num(val));
			}
		}
		else if (cmd == "property-swap")
		{
			// the radios' flip-flop button: active <-> standby frequency
			if (props.size() >= 2)
			{
				const String other = props[1];
				const double va = value_of(prop), vb = value_of(other);
				assign(prop, vb);
				assign(other, va);
			}
		}
		else if (cmd == "property-adjust")
		{
			// FlightGear: `step` is a fixed increment; without it the knob's
			// click direction (offset +-1) times `factor` is the increment, so
			// old-style knobs (single `action`) turn both ways
			double inc = b.has("step") ? num(b["step"]) : num(ctx.get("offset", 1.0)) * num(b.get("factor", 1.0), 1.0);
			if (ctx.has("wheel"))
			{
				inc = std::abs(inc) * (double)ctx["wheel"];
			}
			// `mask`: "integer" turns only the whole part, "decimal" only
			// the fraction (the two concentric knobs of a frequency selector)
			const String mask = b.get("mask", "all");
			const double cur = value_of(prop);
			double keep = 0.0;
			double v = cur;
			if (mask == "integer")
			{
				keep = cur - std::floor(cur);
				v = std::floor(cur);
			}
			else if (mask == "decimal")
			{
				keep = std::floor(cur);
				v = cur - std::floor(cur);
			}
			v += inc;
			const bool has_min = b.has("min"), has_max = b.has("max");
			const double mn = has_min ? num(b["min"]) : 0.0;
			const double mx = has_max ? num(b["max"]) : 0.0;
			const String wrap = b.get("wrap", "false");
			if (has_min && has_max && (wrap == "true" || wrap == "1"))
			{
				v = mn + Math::fposmod(v - mn, mx - mn);
			}
			else
			{
				if (has_min)
				{
					v = Math::max(v, mn);
				}
				if (has_max)
				{
					v = Math::min(v, mx);
				}
			}
			const double res = num(b.get("resolution", 0.0));
			if (res > 0.0)
			{
				v = std::round(v / res) * res;
			}
			v += keep;
			if (mask != "all")
			{
				// the fraction can carry floating-point dust: snap to 1 kHz
				v = std::round(v * 1000.0) / 1000.0;
			}
			assign(prop, v);
		}
		else if (cmd == "property-multiply")
		{
			assign(prop, value_of(prop) * num(b.get("factor", 1.0), 1.0));
		}
		else if (cmd == "property-cycle")
		{
			const Array vals = as_list(b.get("value", Array()));
			if (vals.is_empty())
			{
				continue;
			}
			// FlightGear wraps by default, which sends a three-position switch
			// from one end to the other (gery 2026-10-31: the landing lights
			// jumped from ON to RETRACT). A lever has ends: the cycle stops at
			// the last value unless `wrap` says so or the list is a plain
			// toggle - two values that are all this property ever cycles over
			bool wrap = false;
			if (b.has("wrap"))
			{
				wrap = String(b["wrap"]).to_lower() == "true" || num(b["wrap"]) != 0.0;
			}
			else if (vals.size() == 2)
			{
				const PackedFloat64Array all = cycle_values.get(norm(prop), PackedFloat64Array());
				wrap = all.size() <= 2;
			}
			const double cur = value_of(prop);
			int idx = 0;
			for (int k = 0; k < vals.size(); k++)
			{
				if (std::abs(num(vals[k]) - cur) < 1e-6)
				{
					idx = wrap ? (k + 1) % vals.size() : Math::min(k + 1, (int)vals.size() - 1);
					break;
				}
			}
			assign(prop, num(vals[idx]));
		}
		else if (cmd == "nasal")
		{
			nasal(b.get("script", ""), ctx);
		}
	}
}

// The few Nasal scripts the A-10's controls call, by name.
// ── Nasal shims ─────────────────────────────────────────────────────────────

void FGControls::set_knob_actions(const Array &specs) { knob_actions = specs; }
Array FGControls::get_knob_actions() const { return knob_actions; }

int FGControls::load_shims(const String &path)
{
	shims.clear();
	pulses.clear();
	if (path.is_empty())
	{
		return 0;
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null())
	{
		UtilityFunctions::push_warning("FGControls: nasal shims not found at ", path);
		return 0;
	}
	const Variant data = JSON::parse_string(f->get_as_text());
	if (data.get_type() == Variant::DICTIONARY)
	{
		shims = Dictionary(data).get("shims", Array());
	}
	else if (data.get_type() == Variant::ARRAY)
	{
		shims = data;
	}
	return shims.size();
}

int FGControls::get_shim_count() const { return shims.size(); }

static void collect_binding_props(const Variant &v, PackedStringArray &out)
{
	if (v.get_type() == Variant::DICTIONARY)
	{
		const Dictionary d = v;
		const Array keys = d.keys();
		for (int i = 0; i < keys.size(); i++)
		{
			const String k = keys[i];
			const Variant c = d[keys[i]];
			if (k == "property")
			{
				const Array ps = c.get_type() == Variant::ARRAY ? (Array)c : Array::make(c);
				for (int j = 0; j < ps.size(); j++)
				{
					if (ps[j].get_type() == Variant::STRING)
					{
						const String p = FGControls::norm(ps[j]);
						if (!p.is_empty() && !out.has(p))
						{
							out.push_back(p);
						}
					}
				}
			}
			else
			{
				collect_binding_props(c, out);
			}
		}
	}
	else if (v.get_type() == Variant::ARRAY)
	{
		const Array a = v;
		for (int i = 0; i < a.size(); i++)
		{
			collect_binding_props(a[i], out);
		}
	}
}

PackedStringArray FGControls::binding_properties() const
{
	PackedStringArray out;
	for (int i = 0; i < items.size(); i++)
	{
		const Array recs = items[i].recs;
		for (int r = 0; r < recs.size(); r++)
		{
			const Dictionary rec = recs[r];
			const Dictionary raw = rec.get("raw", Dictionary());
			for (const char *k : {"action", "increase", "decrease", "shift-increase", "shift-decrease", "release", "shift-action"})
			{
				if (raw.has(k))
				{
					collect_binding_props(raw[k], out);
				}
			}
		}
	}
	return out;
}

void FGControls::tick(double delta)
{
	for (int i = pulses.size() - 1; i >= 0; i--)
	{
		Dictionary pu = pulses[i];
		const double t = (double)pu["t"] - delta;
		if (t <= 0.0)
		{
			assign(pu["prop"], 0.0);
			pulses.remove_at(i);
		}
		else
		{
			pu["t"] = t;
		}
	}
}

// The numeric / string arguments of "head(a, b)".
Array FGControls::shim_args(const String &statement)
{
	Array out;
	const int open = statement.find("(");
	const int close = statement.rfind(")");
	if (open < 0 || close <= open)
	{
		return out;
	}
	const PackedStringArray parts = statement.substr(open + 1, close - open - 1).split(",", false);
	for (int i = 0; i < parts.size(); i++)
	{
		const String a = parts[i].strip_edges();
		if (a.is_valid_float())
		{
			out.push_back(a.to_float());
		}
		else
		{
			out.push_back(a.trim_prefix("\"").trim_suffix("\""));
		}
	}
	return out;
}

// "{0}" .. "{3}" in a property template take the integer value of that argument.
String FGControls::shim_prop(const String &tmpl, const Array &args)
{
	String p = tmpl;
	for (int i = 0; i < 4; i++)
	{
		const String key = String("{") + String::num_int64(i) + "}";
		if (p.find(key) >= 0)
		{
			const int v = i < args.size() ? (int)num(args[i]) : 0;
			p = p.replace(key, String::num_int64(v));
		}
	}
	return p;
}

bool FGControls::run_shim(const String &statement, const Dictionary &ctx)
{
	for (int i = 0; i < shims.size(); i++)
	{
		if (shims[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary sh = shims[i];
		const String match = sh.get("match", "");
		if (match.is_empty() || !statement.begins_with(match))
		{
			continue;
		}
		const Array args = shim_args(statement);
		const String action = sh.get("action", "");
		const String prop = shim_prop(sh.get("property", ""), args);
		if (action == "pulse")
		{
			assign(prop, 1.0);
			Dictionary pu;
			pu["prop"] = prop;
			pu["t"] = num(sh.get("seconds", 0.2), 0.2);
			pulses.push_back(pu);
		}
		else if (action == "assign")
		{
			Variant value = sh.get("value", 0.0);
			if (sh.has("value_arg"))
			{
				const int ai = (int)num(sh["value_arg"]);
				value = ai < args.size() ? args[ai] : Variant(0.0);
			}
			if (sh.has("value_map") && sh["value_map"].get_type() == Variant::DICTIONARY)
			{
				const Dictionary vm = sh["value_map"];
				const String key = value.get_type() == Variant::STRING ? String(value) : String::num(num(value), 0);
				if (vm.has(key))
				{
					value = vm[key];
				}
			}
			assign(prop, value);
		}
		else if (action == "set")
		{
			// several properties at once: {"set": {"path": value, ...}}
			const Dictionary vals = sh.get("set", Dictionary());
			const Array keys = vals.keys();
			for (int k = 0; k < keys.size(); k++)
			{
				assign(shim_prop(keys[k], args), vals[keys[k]]);
			}
		}
		else if (action == "toggle")
		{
			assign(prop, value_of(prop) > 0.5 ? 0.0 : 1.0);
		}
		else if (action == "swap")
		{
			const String other = shim_prop(sh.get("property2", ""), args);
			const double va = value_of(prop), vb = value_of(other);
			assign(prop, vb);
			assign(other, va);
		}
		else if (action == "adjust")
		{
			// like property-adjust: step from the call's argument times a factor
			double step = num(sh.get("step", 0.0));
			if (sh.has("step_arg"))
			{
				const int ai = (int)num(sh["step_arg"]);
				step = (ai < args.size() ? num(args[ai]) : 1.0) * num(sh.get("factor", 1.0), 1.0);
			}
			Dictionary b;
			b["command"] = "property-adjust";
			b["property"] = prop;
			b["step"] = step;
			for (const char *k : {"min", "max", "wrap", "mask", "resolution"})
			{
				if (sh.has(k))
				{
					b[k] = sh[k].get_type() == Variant::STRING ? sh[k] : Variant(String::num(num(sh[k]), 6));
				}
			}
			Array bs;
			bs.push_back(b);
			run(bs, Dictionary(), Dictionary());
		}
		else if (action == "cycle")
		{
			// step through a value list by the sign of an argument
			const Array vals = sh.get("values", Array());
			if (!vals.is_empty())
			{
				const int ai = sh.has("dir_arg") ? (int)num(sh["dir_arg"]) : 0;
				const double dir = ai < args.size() ? num(args[ai]) : 1.0;
				const double cur = value_of(prop);
				int idx = 0;
				for (int k = 0; k < vals.size(); k++)
				{
					if (std::abs(num(vals[k]) - cur) < 1e-6)
					{
						idx = k;
					}
				}
				idx = Math::clamp(idx + (dir >= 0.0 ? 1 : -1), 0, (int)vals.size() - 1);
				assign(prop, num(vals[idx]));
			}
		}
		else if (action == "action")
		{
			// a ProvPilot action (gear_cmd, flaps_step, speedbrake, gear_toggle ...)
			Array a = args;
			if (sh.has("value"))
			{
				a.clear();
				a.push_back(sh["value"]);
			}
			nasal_action(sh.get("kind", ""), a, ctx);
		}
		return true;
	}
	return false;
}

void FGControls::nasal(const String &script, const Dictionary &ctx)
{
	const String s = script.strip_edges();
	// the aircraft's shim table first, statement by statement
	if (!shims.is_empty())
	{
		const PackedStringArray stmts = s.split(";", false);
		bool any = false;
		for (int i = 0; i < stmts.size(); i++)
		{
			if (run_shim(stmts[i].strip_edges(), ctx))
			{
				any = true;
			}
		}
		if (any)
		{
			return;
		}
	}
	// setprop("path", value) - used by the armament station buttons
	Ref<RegEx> re = memnew(RegEx);
	re->compile("setprop\\(\\s*\"([^\"]+)\"\\s*,\\s*([-0-9.]+)");
	Ref<RegExMatch> mm = re->search(s);
	if (mm.is_valid())
	{
		assign(mm->get_string(1), mm->get_string(2).to_float());
		return;
	}
	Array args;
	const int open = s.find("(");
	if (open >= 0)
	{
		const String inner = s.substr(open + 1, s.rfind(")") - open - 1);
		const PackedStringArray parts = inner.split(",");
		for (int i = 0; i < parts.size(); i++)
		{
			const String t = parts[i].strip_edges();
			if (t.is_valid_float())
			{
				args.push_back(t.to_float());
			}
		}
	}
	for (const auto &m : NASAL_MAP)
	{
		if (s.begins_with(m.prefix))
		{
			nasal_action(m.kind, args, ctx);
			return;
		}
	}
}

void FGControls::nasal_action(const String &kind, const Array &args, const Dictionary &ctx)
{
	const double a0 = args.size() > 0 ? (double)args[0] : 0.0;
	if (kind == "land_lights")
	{
		// 3-position switch 0 taxi .. 1 off .. 2 landing (Nasal: +1 / -1 steps)
		const String p = "sim/model/A-10/controls/lighting/land-lights-switch";
		assign(p, Math::clamp(value_of(p) + ((args.size() > 0 && a0 > 0) ? 1.0 : -1.0), 0.0, 2.0));
	}
	else if (kind == "nav_lights")
	{
		// 0 off, 1 steady, 2 flash
		const String p = "sim/model/A-10/controls/lighting/nav-lights-switch";
		assign(p, Math::clamp(value_of(p) + ((args.size() > 0 && a0 > 0) ? 1.0 : -1.0), 0.0, 2.0));
	}
	else if (kind == "canopy")
	{
		// the switch is sprung: click toward open / close, release holds
		const double dir = num(ctx.get("offset", 1.0), 1.0);
		assign("sim/model/A-10/controls/canopy/canopy-switch", dir > 0.0 ? 2.0 : 0.0);
	}
	else if (kind == "canopy_hold")
	{
		assign("sim/model/A-10/controls/canopy/canopy-switch", 1.0);
	}
	else if (kind == "fuel_sel")
	{
		const String p = "sim/model/A-10/controls/fuel/fuel-dsp-sel";
		assign(p, Math::clamp(value_of(p) + (args.size() > 0 ? a0 : 1.0), 0.0, 4.0));
	}
	else if (kind == "fill_dis")
	{
		if (args.size() > 0)
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "controls/fuel/tank[%d]/fill-dis", (int)a0);
			const String p(buf);
			assign(p, value_of(p) > 0.5 ? 0.0 : 1.0);
		}
	}
	else if (kind == "apu_switch")
	{
		assign("controls/APU/off-start-switch", value_of("controls/APU/off-start-switch") > 0.5 ? 0.0 : 1.0);
	}
	else if (kind == "gear_toggle")
	{
		assign("provpilot/gear-toggle", 1.0);
	}
	else if (kind == "gear_cmd")
	{
		// gear handle with a position: 1 = down, 0 = up (the rig moves the FDM gear)
		assign("provpilot/gear-cmd", a0 > 0.5 ? 1.0 : 0.0);
	}
	else if (kind == "flaps_step")
	{
		// flap lever one notch: +1 down, -1 up (the rig steps the cfg detents)
		assign("provpilot/flaps-step", a0 >= 0.0 ? 1.0 : -1.0);
	}
	else if (kind == "speedbrake")
	{
		assign("provpilot/speedbrake", a0 > 0.5 ? 1.0 : 0.0);
	}
	else if (kind == "nav_mode")
	{
		// FlightGear nav_scripts.nav_mode_selector(n): 5 = ILS, 6 = TACAN
		const int n = (int)a0;
		assign("sim/model/A-10/A-10-nav/selector-ils", n == 5 ? 1.0 : 0.0);
		assign("sim/model/A-10/A-10-nav/selector-tils", n == 6 ? 1.0 : 0.0);
	}
}

} // namespace jsbgodot
