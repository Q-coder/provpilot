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
	const Array controls = m.get("controls", Array());
	for (int r = 0; r < controls.size(); r++)
	{
		const Dictionary rec = controls[r];
		const Array objects = rec.get("objects", Array());
		for (int o = 0; o < objects.size(); o++)
		{
			const String name = objects[o];
			TypedArray<Node> found = model_root->find_children(name, "", true, false);
			found.append_array(model_root->find_children(name + String("_???"), "", true, false));
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
	return count;
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
		if (n == nullptr)
		{
			continue;
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

bool FGControls::click(Camera3D *camera, const Vector2 &screen_pos, int button, bool pressed_now, bool shift)
{
	if (!pressed_now)
	{
		if (pressed.has(button))
		{
			const Dictionary rec = pressed[button];
			pressed.erase(button);
			release(rec, button);
			return true;
		}
		return false;
	}
	const int i = hit_index(camera, screen_pos);
	if (i < 0)
	{
		return false;
	}
	bool took = false;
	const Array recs = items[i].recs;
	for (int r = 0; r < recs.size(); r++)
	{
		const Dictionary rec = recs[r];
		if (press(rec, button, shift))
		{
			took = true;
			pressed[button] = rec;
		}
	}
	return took;
}

static int button_of(const Dictionary &a)
{
	return (int)String(a.get("button", "0")).to_int();
}

bool FGControls::press(const Dictionary &rec, int button, bool shift)
{
	const Dictionary raw = rec.get("raw", Dictionary());
	const String type = rec.get("type", "");
	if (type == "knob")
	{
		const bool inc = button == MOUSE_BUTTON_LEFT || button == MOUSE_BUTTON_WHEEL_UP;
		const bool dec = button == MOUSE_BUTTON_RIGHT || button == MOUSE_BUTTON_WHEEL_DOWN || button == MOUSE_BUTTON_MIDDLE;
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
			Dictionary ctx;
			ctx["offset"] = inc ? 1.0 : -1.0;
			run(bindings(raw["action"]), rec, ctx);
			return true;
		}
		return false;
	}
	if (type == "pick" || type == "slider")
	{
		// "shift-action" (sliders: the other direction) with Shift or the right button
		if (raw.has("shift-action") && (shift || button == MOUSE_BUTTON_RIGHT))
		{
			run(bindings(raw["shift-action"]), rec, Dictionary());
			return true;
		}
		const Array actions = as_list(raw.get("action", Array()));
		bool has_right = false, has_wheel = false;
		for (int i = 0; i < actions.size(); i++)
		{
			if (actions[i].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const int b = button_of(actions[i]);
			has_right = has_right || b == 2;
			has_wheel = has_wheel || b == 3 || b == 4;
		}
		// Wheel on a pick that adjusts a property (Kollsman, heading bug...):
		// up increases, down decreases by the button-0 action's step, Shift
		// five steps (the fine and coarse picks are separate records)
		if (!has_wheel && (button == MOUSE_BUTTON_WHEEL_UP || button == MOUSE_BUTTON_WHEEL_DOWN))
		{
			Dictionary chosen;
			bool have = false;
			for (int i = 0; i < actions.size(); i++)
			{
				if (actions[i].get_type() == Variant::DICTIONARY && button_of(actions[i]) == 0)
				{
					chosen = actions[i];
					have = true;
				}
			}
			if (!have)
			{
				return false;
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
				return false;
			}
			Dictionary ctx;
			ctx["wheel"] = (button == MOUSE_BUTTON_WHEEL_UP ? 1.0 : -1.0) * (shift ? 5.0 : 1.0);
			run(bs, rec, ctx);
			return true;
		}
		bool ran = false;
		for (int i = 0; i < actions.size(); i++)
		{
			if (actions[i].get_type() != Variant::DICTIONARY)
			{
				continue;
			}
			const Dictionary a = actions[i];
			const int b = button_of(a);
			int want = MOUSE_BUTTON_LEFT;
			if (b == 1)
			{
				want = MOUSE_BUTTON_MIDDLE;
			}
			else if (b == 2)
			{
				want = MOUSE_BUTTON_RIGHT;
			}
			else if (b == 3)
			{
				want = MOUSE_BUTTON_WHEEL_UP;
			}
			else if (b == 4)
			{
				want = MOUSE_BUTTON_WHEEL_DOWN;
			}
			// FlightGear's middle button (the second switch direction) also answers to the right button
			const bool ok = button == want || (b == 1 && button == MOUSE_BUTTON_RIGHT && !has_right);
			if (!ok)
			{
				continue;
			}
			run(bindings(a), rec, Dictionary());
			ran = true;
		}
		return ran;
	}
	return false;
}

void FGControls::release(const Dictionary &rec, int button)
{
	const Dictionary raw = rec.get("raw", Dictionary());
	if (raw.has("release"))
	{
		run(bindings(raw["release"]), rec, Dictionary());
	}
	const Array actions = as_list(raw.get("action", Array()));
	for (int i = 0; i < actions.size(); i++)
	{
		if (actions[i].get_type() == Variant::DICTIONARY && Dictionary(actions[i]).has("mod-up"))
		{
			run(bindings(Dictionary(actions[i])["mod-up"]), rec, Dictionary());
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

void FGControls::run(const Array &bs, const Dictionary &rec, const Dictionary &ctx)
{
	for (int i = 0; i < bs.size(); i++)
	{
		if (bs[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary b = bs[i];
		const String cmd = b.get("command", "");
		const String prop = b.get("property", "");
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
		else if (cmd == "property-adjust")
		{
			// FlightGear: `step` is a fixed increment; without it the knob's
			// click direction (offset ±1) times `factor` is the increment, so
			// old-style knobs (single `action`) turn both ways
			double inc = b.has("step") ? num(b["step"]) : num(ctx.get("offset", 1.0)) * num(b.get("factor", 1.0), 1.0);
			if (ctx.has("wheel"))
			{
				inc = std::abs(inc) * (double)ctx["wheel"];
			}
			double v = value_of(prop) + inc;
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
			const double cur = value_of(prop);
			int idx = 0;
			for (int k = 0; k < vals.size(); k++)
			{
				if (std::abs(num(vals[k]) - cur) < 1e-6)
				{
					idx = (k + 1) % vals.size();
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
void FGControls::nasal(const String &script, const Dictionary &ctx)
{
	const String s = script.strip_edges();
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
	else if (kind == "nav_mode")
	{
		// FlightGear nav_scripts.nav_mode_selector(n): 5 = ILS, 6 = TACAN
		const int n = (int)a0;
		assign("sim/model/A-10/A-10-nav/selector-ils", n == 5 ? 1.0 : 0.0);
		assign("sim/model/A-10/A-10-nav/selector-tils", n == 6 ? 1.0 : 0.0);
	}
}

} // namespace jsbgodot
