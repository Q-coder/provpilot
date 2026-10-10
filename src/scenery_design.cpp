#include "scenery_design.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/geometry2d.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace godot;

namespace jsbgodot
{

static const char *const ROAD_CLASSES[] = {"motorway", "trunk", "primary", "secondary", "tertiary",
	"motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link"};
static const char *const RAIL_CLASSES[] = {"rail", "narrow_gauge", "light_rail", "tram"};
static const char *const AREA_TYPES[] = {"grass", "forest", "field", "water", "building"};
static const char *const EDGE_ATTRS[] = {"kind", "cls", "lanes", "oneway", "gauge", "tracks", "width"}; // width m: 0 = from the lanes

static Array strings(const char *const *list, int n)
{
	Array a;
	for (int i = 0; i < n; i++)
	{
		a.push_back(String(list[i]));
	}
	return a;
}

void SceneryDesign::_bind_methods()
{
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("road_attrs", "cls", "lanes", "oneway", "width"), &SceneryDesign::road_attrs, DEFVAL(0.0));
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("rail_attrs", "cls", "gauge_mm", "tracks"), &SceneryDesign::rail_attrs, DEFVAL(1));
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("get_road_classes"), &SceneryDesign::get_road_classes);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("get_rail_classes"), &SceneryDesign::get_rail_classes);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("get_area_types"), &SceneryDesign::get_area_types);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("get_edge_attrs"), &SceneryDesign::get_edge_attrs);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("get_switch_min_radius_m"), &SceneryDesign::get_switch_min_radius_m);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("simplify", "pts", "tol"), &SceneryDesign::simplify);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("circle_through", "a", "v", "b"), &SceneryDesign::circle_through);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("arc_sweep", "a", "v", "b"), &SceneryDesign::arc_sweep);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("arc_at", "sw", "t"), &SceneryDesign::arc_at);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("arc_points", "a", "v", "b"), &SceneryDesign::arc_points);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("tangent_arc_via", "p", "t", "q"), &SceneryDesign::tangent_arc_via);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("nearest_on_segment", "a", "b", "q"), &SceneryDesign::nearest_on_segment);
	ClassDB::bind_static_method("SceneryDesign", D_METHOD("file_sha256", "path"), &SceneryDesign::file_sha256);

	ClassDB::bind_method(D_METHOD("node_count"), &SceneryDesign::node_count);
	ClassDB::bind_method(D_METHOD("edge_count"), &SceneryDesign::edge_count);
	ClassDB::bind_method(D_METHOD("node_pos", "id"), &SceneryDesign::node_pos);
	ClassDB::bind_method(D_METHOD("add_node", "e", "n"), &SceneryDesign::add_node);
	ClassDB::bind_method(D_METHOD("move_node", "id", "e", "n"), &SceneryDesign::move_node);
	ClassDB::bind_method(D_METHOD("node_offset", "id"), &SceneryDesign::node_offset);
	ClassDB::bind_method(D_METHOD("set_node_offset", "id", "h"), &SceneryDesign::set_node_offset);
	ClassDB::bind_method(D_METHOD("import_way", "rec"), &SceneryDesign::import_way);
	ClassDB::bind_method(D_METHOD("area_near", "e", "n", "radius"), &SceneryDesign::area_near);
	ClassDB::bind_method(D_METHOD("flatten_near", "e", "n", "radius"), &SceneryDesign::flatten_near);
	ClassDB::bind_method(D_METHOD("flatten_inside", "e", "n"), &SceneryDesign::flatten_inside);
	ClassDB::bind_method(D_METHOD("replace_near", "e", "n", "radius"), &SceneryDesign::replace_near);
	ClassDB::bind_method(D_METHOD("add_edge", "a", "b", "attrs"), &SceneryDesign::add_edge);
	ClassDB::bind_method(D_METHOD("set_edge_via", "id", "via"), &SceneryDesign::set_edge_via);
	ClassDB::bind_method(D_METHOD("clear_edge_via", "id"), &SceneryDesign::clear_edge_via);
	ClassDB::bind_method(D_METHOD("is_arc", "id"), &SceneryDesign::is_arc);
	ClassDB::bind_method(D_METHOD("edge_points", "id"), &SceneryDesign::edge_points);
	ClassDB::bind_method(D_METHOD("edge_tangent_out", "id", "node_id"), &SceneryDesign::edge_tangent_out);
	ClassDB::bind_method(D_METHOD("node_tangent_out", "node_id"), &SceneryDesign::node_tangent_out);
	ClassDB::bind_method(D_METHOD("add_circle", "centre", "radius", "attrs", "segments"), &SceneryDesign::add_circle, DEFVAL(8));
	ClassDB::bind_method(D_METHOD("track_tangent_at", "edge_id", "e", "n", "towards"), &SceneryDesign::track_tangent_at);
	ClassDB::bind_method(D_METHOD("add_switch", "edge_id", "e", "n", "q"), &SceneryDesign::add_switch);
	ClassDB::bind_method(D_METHOD("reverse_chain", "edge_id"), &SceneryDesign::reverse_chain);
	ClassDB::bind_method(D_METHOD("remove_edge", "id"), &SceneryDesign::remove_edge);
	ClassDB::bind_method(D_METHOD("set_edge_attrs", "id", "attrs"), &SceneryDesign::set_edge_attrs);
	ClassDB::bind_method(D_METHOD("degree", "node_id"), &SceneryDesign::degree);
	ClassDB::bind_method(D_METHOD("edges_of", "node_id"), &SceneryDesign::edges_of);
	ClassDB::bind_method(D_METHOD("split_edge", "id", "e", "n"), &SceneryDesign::split_edge);
	ClassDB::bind_method(D_METHOD("nearest_on_edge", "id", "q"), &SceneryDesign::nearest_on_edge);
	ClassDB::bind_method(D_METHOD("node_near", "e", "n", "radius"), &SceneryDesign::node_near);
	ClassDB::bind_method(D_METHOD("edge_near", "e", "n", "radius"), &SceneryDesign::edge_near);
	ClassDB::bind_method(D_METHOD("chains"), &SceneryDesign::chains);
	ClassDB::bind_method(D_METHOD("snapshot"), &SceneryDesign::snapshot);
	ClassDB::bind_method(D_METHOD("can_undo"), &SceneryDesign::can_undo);
	ClassDB::bind_method(D_METHOD("undo"), &SceneryDesign::undo);
	ClassDB::bind_method(D_METHOD("to_dict"), &SceneryDesign::to_dict);
	ClassDB::bind_method(D_METHOD("from_dict", "d"), &SceneryDesign::from_dict);
	ClassDB::bind_method(D_METHOD("save", "path"), &SceneryDesign::save, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("load", "path"), &SceneryDesign::load);
#define SD_PROP(type, name, getter, setter)                                                    \
	ClassDB::bind_method(D_METHOD(#getter), &SceneryDesign::getter);                          \
	ClassDB::bind_method(D_METHOD(#setter, "value"), &SceneryDesign::setter);                 \
	ADD_PROPERTY(PropertyInfo(type, #name), #setter, #getter);
	SD_PROP(Variant::ARRAY, replace, get_replace, set_replace)
	SD_PROP(Variant::ARRAY, replace_ids, get_replace_ids, set_replace_ids)
	SD_PROP(Variant::ARRAY, flatten, get_flatten, set_flatten)
	SD_PROP(Variant::ARRAY, areas, get_areas, set_areas)
	SD_PROP(Variant::DICTIONARY, patch_cache, get_patch_cache, set_patch_cache)
	SD_PROP(Variant::DICTIONARY, nodes, get_nodes, set_nodes)
	SD_PROP(Variant::DICTIONARY, edges, get_edges, set_edges)
	SD_PROP(Variant::INT, next_id, get_next_id, set_next_id)
	SD_PROP(Variant::STRING, path, get_path, set_path)
	SD_PROP(Variant::BOOL, dirty, get_dirty, set_dirty)
#undef SD_PROP
	ClassDB::bind_integer_constant("SceneryDesign", "", "SWITCH_MIN_RADIUS_M", (int64_t)SWITCH_MIN_RADIUS_M);
}

SceneryDesign::SceneryDesign()
{
}

Array SceneryDesign::get_replace() const { return replace; }
void SceneryDesign::set_replace(const Array &v) { replace = v; }
Array SceneryDesign::get_replace_ids() const { return replace_ids; }
void SceneryDesign::set_replace_ids(const Array &v) { replace_ids = v; }
Array SceneryDesign::get_flatten() const { return flatten; }
void SceneryDesign::set_flatten(const Array &v) { flatten = v; }
Array SceneryDesign::get_areas() const { return areas; }
void SceneryDesign::set_areas(const Array &v) { areas = v; }
Dictionary SceneryDesign::get_patch_cache() const { return patch_cache; }
void SceneryDesign::set_patch_cache(const Dictionary &v) { patch_cache = v; }
Dictionary SceneryDesign::get_nodes() const { return nodes; }
void SceneryDesign::set_nodes(const Dictionary &v) { nodes = v; }
Dictionary SceneryDesign::get_edges() const { return edges; }
void SceneryDesign::set_edges(const Dictionary &v) { edges = v; }
int SceneryDesign::get_next_id() const { return next_id; }
void SceneryDesign::set_next_id(int v) { next_id = v; }
String SceneryDesign::get_path() const { return path; }
void SceneryDesign::set_path(const String &v) { path = v; }
bool SceneryDesign::get_dirty() const { return dirty; }
void SceneryDesign::set_dirty(bool v) { dirty = v; }

Array SceneryDesign::get_road_classes() { return strings(ROAD_CLASSES, 10); }
Array SceneryDesign::get_rail_classes() { return strings(RAIL_CLASSES, 4); }
Array SceneryDesign::get_area_types() { return strings(AREA_TYPES, 5); }
Array SceneryDesign::get_edge_attrs() { return strings(EDGE_ATTRS, 7); }
double SceneryDesign::get_switch_min_radius_m() { return SWITCH_MIN_RADIUS_M; }

Dictionary SceneryDesign::road_attrs(const String &cls, int lanes, bool oneway, double width)
{
	Dictionary d;
	d["kind"] = "road";
	d["cls"] = cls;
	d["lanes"] = lanes;
	d["oneway"] = oneway;
	d["gauge"] = 0;
	d["tracks"] = 0;
	d["width"] = width;
	return d;
}

Dictionary SceneryDesign::rail_attrs(const String &cls, int gauge_mm, int tracks)
{
	Dictionary d;
	d["kind"] = "rail";
	d["cls"] = cls;
	d["lanes"] = 0;
	d["oneway"] = false;
	d["gauge"] = gauge_mm;
	d["tracks"] = tracks;
	d["width"] = 0.0;
	return d;
}

Variant SceneryDesign::attr_default(const String &k)
{
	if (k == "width") return 0.0;
	if (k == "lanes" || k == "gauge" || k == "tracks") return 0;
	if (k == "kind" || k == "cls") return String();
	return false;
}

int SceneryDesign::node_count() const { return nodes.size(); }
int SceneryDesign::edge_count() const { return edges.size(); }

Vector2 SceneryDesign::node_pos(int id) const
{
	const Dictionary nd = nodes.get(id, Dictionary());
	return Vector2(nd.get("e", 0.0), nd.get("n", 0.0));
}

int SceneryDesign::add_node(double e, double n)
{
	const int id = next_id++;
	Dictionary nd;
	nd["e"] = e;
	nd["n"] = n;
	nodes[id] = nd;
	dirty = true;
	return id;
}

void SceneryDesign::move_node(int id, double e, double n)
{
	if (nodes.has(id))
	{
		Dictionary nd = nodes[id];
		nd["e"] = e;
		nd["n"] = n;
		dirty = true;
	}
}

double SceneryDesign::node_offset(int id) const
{
	const Dictionary nd = nodes.get(id, Dictionary());
	return nd.get("h", 0.0);
}

void SceneryDesign::set_node_offset(int id, double h)
{
	if (nodes.has(id))
	{
		Dictionary nd = nodes[id];
		if (std::abs(h) < 0.005)
		{
			nd.erase("h");
		}
		else
		{
			nd["h"] = h;
		}
		dirty = true;
	}
}

Vector2 SceneryDesign::nearest_on_segment(const Vector2 &a, const Vector2 &b, const Vector2 &q)
{
	const Vector2 ab = b - a;
	const double l2 = ab.length_squared();
	if (l2 < 1e-9)
	{
		return a;
	}
	const double t = Math::clamp((double)(q - a).dot(ab) / l2, 0.0, 1.0);
	return a + ab * t;
}

Array SceneryDesign::simplify(const Array &pts, double tol)
{
	if (pts.size() <= 2)
	{
		return pts.duplicate();
	}
	const Vector2 a = pts[0];
	const Vector2 b = pts[pts.size() - 1];
	double best_d = -1.0;
	int best_i = -1;
	for (int i = 1; i < pts.size() - 1; i++)
	{
		const Vector2 p = pts[i];
		const double d = p.distance_to(nearest_on_segment(a, b, p));
		if (d > best_d)
		{
			best_d = d;
			best_i = i;
		}
	}
	if (best_d <= tol)
	{
		Array out;
		out.push_back(a);
		out.push_back(b);
		return out;
	}
	Array left = simplify(pts.slice(0, best_i + 1), tol);
	const Array right = simplify(pts.slice(best_i), tol);
	left.pop_back();
	left.append_array(right);
	return left;
}

Array SceneryDesign::import_way(const Dictionary &rec)
{
	Array pts;
	const Array raw = rec.get("pts", Array());
	for (int i = 0; i < raw.size(); i++)
	{
		const Array p = raw[i];
		pts.push_back(Vector2((double)p[0], (double)p[1]));
	}
	pts = simplify(pts, 0.4);
	if (pts.size() < 2)
	{
		return Array();
	}
	Dictionary attrs;
	if (String(rec.get("kind", "road")) == "rail")
	{
		attrs = rail_attrs(rec.get("cls", "rail"), (int)(double)rec.get("gauge", 1435), std::max(1, (int)(double)rec.get("tracks", 1)));
	}
	else
	{
		attrs = road_attrs(rec.get("cls", "secondary"), (int)(double)rec.get("lanes", 0), rec.get("oneway", false), rec.get("width", 0.0));
	}
	Array ids;
	for (int i = 0; i < pts.size(); i++)
	{
		const Vector2 p = pts[i];
		int nid = (i == 0 || i == pts.size() - 1) ? node_near(p.x, p.y, 1.0) : -1; // ends snap to existing nodes within 1 m
		if (nid < 0)
		{
			nid = add_node(p.x, p.y);
		}
		ids.push_back(nid);
	}
	for (int i = 0; i + 1 < ids.size(); i++)
	{
		add_edge(ids[i], ids[i + 1], attrs);
	}
	const int wid = (int)(double)rec.get("id", 0);
	if (wid > 0 && !replace_ids.has(wid))
	{
		replace_ids.push_back(wid);
	}
	dirty = true;
	return ids;
}

static int poly_list_near(const Array &list, const char *poly_key, double e, double n, double radius)
{
	const Vector2 q(e, n);
	for (int i = 0; i < list.size(); i++)
	{
		const Array poly = poly_key != nullptr ? Array(Dictionary(list[i])[poly_key]) : Array(list[i]);
		for (int k = 0; k < poly.size(); k++)
		{
			if (Vector2(poly[k]).distance_to(q) <= radius)
			{
				return i;
			}
		}
	}
	return -1;
}

int SceneryDesign::area_near(double e, double n, double radius) const { return poly_list_near(areas, "poly", e, n, radius); }
int SceneryDesign::flatten_near(double e, double n, double radius) const { return poly_list_near(flatten, "poly", e, n, radius); }
int SceneryDesign::replace_near(double e, double n, double radius) const { return poly_list_near(replace, nullptr, e, n, radius); }

int SceneryDesign::flatten_inside(double e, double n) const
{
	// overlapping plateaus: the smallest one around the point is the pick
	int best = -1;
	double best_area = 0.0;
	for (int i = 0; i < flatten.size(); i++)
	{
		const Array poly = Dictionary(flatten[i])["poly"];
		PackedVector2Array pv;
		for (int k = 0; k < poly.size(); k++)
			pv.push_back(Vector2(poly[k]));
		if (pv.size() < 3 || !Geometry2D::get_singleton()->is_point_in_polygon(Vector2(e, n), pv))
			continue;
		double area = 0.0;
		for (int k = 0; k < pv.size(); k++)
			area += pv[k].cross(pv[(k + 1) % pv.size()]);
		area = std::abs(area) * 0.5;
		if (best < 0 || area < best_area)
		{
			best = i;
			best_area = area;
		}
	}
	return best;
}

int SceneryDesign::add_edge(int a, int b, const Dictionary &attrs)
{
	if (a == b || !nodes.has(a) || !nodes.has(b))
	{
		return -1;
	}
	const Array keys = edges.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const Dictionary ed = edges[keys[i]];
		const int ea = ed["a"], eb = ed["b"];
		if ((ea == a && eb == b) || (ea == b && eb == a))
		{
			return -1;
		}
	}
	const int id = next_id++;
	Dictionary ed;
	ed["a"] = a;
	ed["b"] = b;
	for (const char *k : EDGE_ATTRS)
	{
		ed[k] = attrs.get(k, attr_default(k));
	}
	edges[id] = ed;
	dirty = true;
	return id;
}

void SceneryDesign::set_edge_via(int id, const Vector2 &via)
{
	if (!edges.has(id))
	{
		return;
	}
	Dictionary ed = edges[id];
	Array v;
	v.push_back(via.x);
	v.push_back(via.y);
	ed["via"] = v;
	dirty = true;
}

void SceneryDesign::clear_edge_via(int id)
{
	if (edges.has(id))
	{
		Dictionary ed = edges[id];
		if (ed.has("via"))
		{
			ed.erase("via");
			dirty = true;
		}
	}
}

bool SceneryDesign::is_arc(int id) const
{
	return edges.has(id) && Dictionary(edges[id]).has("via");
}

Array SceneryDesign::edge_points(int id) const
{
	const Dictionary ed = edges[id];
	const Vector2 a = node_pos(ed["a"]);
	const Vector2 b = node_pos(ed["b"]);
	if (!ed.has("via"))
	{
		Array out;
		out.push_back(a);
		out.push_back(b);
		return out;
	}
	const Array via = ed["via"];
	return arc_points(a, Vector2((double)via[0], (double)via[1]), b);
}

Dictionary SceneryDesign::circle_through(const Vector2 &a, const Vector2 &v, const Vector2 &b)
{
	const double d = 2.0 * (a.x * (v.y - b.y) + v.x * (b.y - a.y) + b.x * (a.y - v.y));
	if (std::abs(d) < 1e-6)
	{
		return Dictionary();
	}
	const double a2 = a.length_squared(), v2 = v.length_squared(), b2 = b.length_squared();
	const double ux = (a2 * (v.y - b.y) + v2 * (b.y - a.y) + b2 * (a.y - v.y)) / d;
	const double uy = (a2 * (b.x - v.x) + v2 * (a.x - b.x) + b2 * (v.x - a.x)) / d;
	const Vector2 c(ux, uy);
	Dictionary out;
	out["c"] = c;
	out["r"] = c.distance_to(a);
	return out;
}

Dictionary SceneryDesign::arc_sweep(const Vector2 &a, const Vector2 &v, const Vector2 &b)
{
	const Dictionary circ = circle_through(a, v, b);
	if (circ.is_empty() || (double)circ["r"] > 50000.0)
	{
		return Dictionary();
	}
	const Vector2 c = circ["c"];
	const double ang_a = (a - c).angle();
	const double ang_v = (v - c).angle();
	const double ang_b = (b - c).angle();
	const double sweep_ccw = Math::fposmod(ang_b - ang_a, Math_TAU);
	const double via_ccw = Math::fposmod(ang_v - ang_a, Math_TAU);
	const double sweep = via_ccw <= sweep_ccw ? sweep_ccw : sweep_ccw - Math_TAU;
	Dictionary out;
	out["c"] = c;
	out["r"] = (double)circ["r"];
	out["ang_a"] = ang_a;
	out["sweep"] = sweep;
	return out;
}

Vector2 SceneryDesign::arc_at(const Dictionary &sw, double t)
{
	const double ang = (double)sw["ang_a"] + (double)sw["sweep"] * t;
	return Vector2(sw["c"]) + Vector2(std::cos(ang), std::sin(ang)) * (double)sw["r"];
}

Array SceneryDesign::arc_points(const Vector2 &a, const Vector2 &v, const Vector2 &b)
{
	const Dictionary sw = arc_sweep(a, v, b);
	Array out;
	if (sw.is_empty())
	{
		out.push_back(a);
		out.push_back(b);
		return out;
	}
	const Vector2 c = sw["c"];
	const double r = sw["r"], ang_a = sw["ang_a"], sweep = sw["sweep"];
	const int n = std::max(ARC_MIN_SEGMENTS, (int)std::ceil(std::abs(sweep) * r / ARC_STEP_M));
	for (int i = 0; i <= n; i++)
	{
		if (i == 0)
		{
			out.push_back(a);
		}
		else if (i == n)
		{
			out.push_back(b);
		}
		else
		{
			const double ang = ang_a + sweep * (double)i / n;
			out.push_back(c + Vector2(std::cos(ang), std::sin(ang)) * r);
		}
	}
	return out;
}

Dictionary SceneryDesign::tangent_arc_via(const Vector2 &p, const Vector2 &t, const Vector2 &q)
{
	const Vector2 c = q - p;
	const Vector2 nrm(-t.y, t.x);
	const double dn = c.dot(nrm);
	if (std::abs(dn) < 0.01 || c.length() < 0.5)
	{
		return Dictionary();
	}
	const double r = c.length_squared() / (2.0 * dn); // signed: + = centre to the left
	const Vector2 centre = p + nrm * r;
	const double ang_p = (p - centre).angle();
	const double ang_q = (q - centre).angle();
	double sweep = ang_q - ang_p;
	// the arc leaves p along t: for a left-hand centre it turns counter-clockwise
	sweep = r > 0.0 ? Math::fposmod(sweep, Math_TAU) : Math::fposmod(sweep, Math_TAU) - Math_TAU;
	const double mid = ang_p + sweep * 0.5;
	Dictionary out;
	out["via"] = centre + Vector2(std::cos(mid), std::sin(mid)) * std::abs(r);
	out["radius"] = std::abs(r);
	return out;
}

Vector2 SceneryDesign::edge_tangent_out(int id, int node_id) const
{
	const Array pts = edge_points(id);
	if (pts.size() < 2)
	{
		return Vector2();
	}
	const Dictionary ed = edges[id];
	if ((int)ed["b"] == node_id)
	{
		return (Vector2(pts[pts.size() - 1]) - Vector2(pts[pts.size() - 2])).normalized();
	}
	return (Vector2(pts[0]) - Vector2(pts[1])).normalized();
}

Vector2 SceneryDesign::node_tangent_out(int node_id) const
{
	const Array adj = edges_of(node_id);
	if (adj.size() != 1)
	{
		return Vector2();
	}
	return edge_tangent_out(adj[0], node_id);
}

Array SceneryDesign::add_circle(const Vector2 &centre, double radius, const Dictionary &attrs, int segments)
{
	Array ids;
	for (int k = 0; k < segments; k++)
	{
		const double ang = Math_TAU * k / segments;
		ids.push_back(add_node(centre.x + std::cos(ang) * radius, centre.y + std::sin(ang) * radius));
	}
	for (int k = 0; k < segments; k++)
	{
		const int eid = add_edge(ids[k], ids[(k + 1) % segments], attrs);
		const double mid = Math_TAU * (k + 0.5) / segments;
		if (eid > 0)
		{
			set_edge_via(eid, centre + Vector2(std::cos(mid), std::sin(mid)) * radius);
		}
	}
	return ids;
}

Vector2 SceneryDesign::track_tangent_at(int edge_id, double e, double n, const Vector2 &towards) const
{
	const Dictionary near = nearest_on_edge(edge_id, Vector2(e, n));
	const Array pts = edge_points(edge_id);
	const int i = near["i"];
	Vector2 t = (Vector2(pts[i + 1]) - Vector2(pts[i])).normalized();
	if ((towards - Vector2(near["p"])).dot(t) < 0.0)
	{
		t = -t;
	}
	return t;
}

Dictionary SceneryDesign::add_switch(int edge_id, double e, double n, const Vector2 &q)
{
	Dictionary out;
	if (!edges.has(edge_id) || String(Dictionary(edges[edge_id])["kind"]) != "rail")
	{
		out["error"] = "not a track";
		return out;
	}
	const Vector2 t = track_tangent_at(edge_id, e, n, q);
	const Dictionary near = nearest_on_edge(edge_id, Vector2(e, n));
	const Vector2 toe = near["p"];
	const Dictionary via = tangent_arc_via(toe, t, q);
	if (via.is_empty())
	{
		out["error"] = "the end point lies on the track";
		return out;
	}
	if ((double)via["radius"] < SWITCH_MIN_RADIUS_M)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "radius %.0f m is below %.0f m", (double)via["radius"], SWITCH_MIN_RADIUS_M);
		out["error"] = String(buf);
		return out;
	}
	Dictionary attrs;
	const Dictionary ed = edges[edge_id];
	for (const char *k : EDGE_ATTRS)
	{
		attrs[k] = ed[k];
	}
	const int snode = split_edge(edge_id, toe.x, toe.y);
	const int end = add_node(q.x, q.y);
	const int div = add_edge(snode, end, attrs);
	set_edge_via(div, via["via"]);
	out["node"] = snode;
	out["edge"] = div;
	out["radius"] = (double)via["radius"];
	out["end"] = end;
	return out;
}

int SceneryDesign::reverse_chain(int edge_id)
{
	if (!edges.has(edge_id))
	{
		return 0;
	}
	const Array all = chains();
	for (int c = 0; c < all.size(); c++)
	{
		const Dictionary chain = all[c];
		const Array eids = chain["edges"];
		if (!eids.has(edge_id))
		{
			continue;
		}
		for (int i = 0; i < eids.size(); i++)
		{
			Dictionary ed = edges[eids[i]];
			const Variant a = ed["a"];
			ed["a"] = ed["b"];
			ed["b"] = a;
		}
		dirty = true;
		return eids.size();
	}
	return 0;
}

void SceneryDesign::remove_edge(int id)
{
	if (!edges.has(id))
	{
		return;
	}
	const Dictionary ed = edges[id];
	edges.erase(id);
	for (const char *k : {"a", "b"})
	{
		const int nid = ed[k];
		if (degree(nid) == 0)
		{
			nodes.erase(nid);
		}
	}
	dirty = true;
}

void SceneryDesign::set_edge_attrs(int id, const Dictionary &attrs)
{
	if (!edges.has(id))
	{
		return;
	}
	Dictionary ed = edges[id];
	for (const char *k : EDGE_ATTRS)
	{
		if (attrs.has(k))
		{
			ed[k] = attrs[k];
		}
	}
	dirty = true;
}

int SceneryDesign::degree(int node_id) const
{
	return edges_of(node_id).size();
}

Array SceneryDesign::edges_of(int node_id) const
{
	Array out;
	const Array keys = edges.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const Dictionary ed = edges[keys[i]];
		if ((int)ed["a"] == node_id || (int)ed["b"] == node_id)
		{
			out.push_back(keys[i]);
		}
	}
	return out;
}

int SceneryDesign::split_edge(int id, double e, double n)
{
	if (!edges.has(id))
	{
		return -1;
	}
	const Dictionary ed = Dictionary(edges[id]).duplicate(true);
	const Dictionary near = nearest_on_edge(id, Vector2(e, n));
	const Vector2 p = near["p"];
	const int nid = add_node(p.x, p.y);
	edges.erase(id);
	Dictionary attrs;
	for (const char *k : EDGE_ATTRS)
	{
		attrs[k] = ed[k];
	}
	const int e1 = add_edge(ed["a"], nid, attrs);
	const int e2 = add_edge(nid, ed["b"], attrs);
	if (ed.has("via"))
	{
		// each half is an arc of the same circle through its own midpoint
		const Vector2 a = node_pos(ed["a"]);
		const Vector2 b = node_pos(ed["b"]);
		const Array via = ed["via"];
		const Dictionary sw = arc_sweep(a, Vector2((double)via[0], (double)via[1]), b);
		if (!sw.is_empty())
		{
			const double ang_p = (p - Vector2(sw["c"])).angle();
			double tp = Math::fposmod(ang_p - (double)sw["ang_a"], Math_TAU) / Math_TAU;
			if ((double)sw["sweep"] < 0.0)
			{
				tp = 1.0 - tp;
			}
			tp = tp * Math_TAU / std::abs((double)sw["sweep"]);
			tp = Math::clamp(tp, 0.02, 0.98);
			if (e1 > 0)
			{
				set_edge_via(e1, arc_at(sw, tp * 0.5));
			}
			if (e2 > 0)
			{
				set_edge_via(e2, arc_at(sw, tp + (1.0 - tp) * 0.5));
			}
		}
	}
	return nid;
}

Dictionary SceneryDesign::nearest_on_edge(int id, const Vector2 &q) const
{
	const Array pts = edge_points(id);
	Dictionary best;
	best["p"] = pts[0];
	best["dist"] = Vector2(pts[0]).distance_to(q);
	best["i"] = 0;
	for (int i = 0; i + 1 < pts.size(); i++)
	{
		const Vector2 p = nearest_on_segment(pts[i], pts[i + 1], q);
		const double d = p.distance_to(q);
		if (d < (double)best["dist"])
		{
			best["p"] = p;
			best["dist"] = d;
			best["i"] = i;
		}
	}
	return best;
}

int SceneryDesign::node_near(double e, double n, double radius) const
{
	int best = -1;
	double best_d = radius;
	const Vector2 q(e, n);
	const Array keys = nodes.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const double d = node_pos(keys[i]).distance_to(q);
		if (d <= best_d)
		{
			best_d = d;
			best = keys[i];
		}
	}
	return best;
}

Dictionary SceneryDesign::edge_near(double e, double n, double radius) const
{
	Dictionary best;
	double best_d = radius;
	const Vector2 q(e, n);
	const Array keys = edges.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const Dictionary near = nearest_on_edge(keys[i], q);
		const double d = near["dist"];
		if (d <= best_d)
		{
			best_d = d;
			const Vector2 p = near["p"];
			best = Dictionary();
			best["id"] = keys[i];
			best["e"] = p.x;
			best["n"] = p.y;
			best["dist"] = d;
		}
	}
	return best;
}

bool SceneryDesign::same_attrs(const Dictionary &x, const Dictionary &y)
{
	for (const char *k : EDGE_ATTRS)
	{
		if (x.get(k, Variant()) != y.get(k, Variant()))
		{
			return false;
		}
	}
	return true;
}

Array SceneryDesign::chains() const
{
	Dictionary used;
	Array out;
	Array ids = edges.keys();
	ids.sort();
	for (int idx = 0; idx < ids.size(); idx++)
	{
		const int eid = ids[idx];
		if (used.has(eid))
		{
			continue;
		}
		used[eid] = true;
		const Dictionary ed = edges[eid];
		Array node_list;
		node_list.push_back(ed["a"]);
		node_list.push_back(ed["b"]);
		Array edge_list;
		edge_list.push_back(eid);
		// extend forward from b, then backward from a
		for (int dir : {1, -1})
		{
			while (true)
			{
				const int end_node = dir == 1 ? (int)node_list[node_list.size() - 1] : (int)node_list[0];
				int nxt = -1;
				const Array adj = edges_of(end_node);
				if (adj.size() != 2)
				{
					break;
				}
				for (int c = 0; c < adj.size(); c++)
				{
					if (!used.has(adj[c]) && same_attrs(edges[adj[c]], ed))
					{
						nxt = adj[c];
					}
				}
				if (nxt < 0)
				{
					break;
				}
				used[nxt] = true;
				const Dictionary ned = edges[nxt];
				const int other = (int)ned["a"] == end_node ? (int)ned["b"] : (int)ned["a"];
				const bool closes = node_list.has(other); // a ring: close it and stop
				if (dir == 1)
				{
					node_list.push_back(other);
					edge_list.push_back(nxt);
				}
				else
				{
					node_list.push_front(other);
					edge_list.push_front(nxt);
				}
				if (closes)
				{
					break;
				}
			}
		}
		Dictionary attrs;
		for (const char *k : EDGE_ATTRS)
		{
			attrs[k] = ed[k];
		}
		Dictionary chain;
		chain["attrs"] = attrs;
		chain["nodes"] = node_list;
		chain["edges"] = edge_list;
		out.push_back(chain);
	}
	return out;
}

// ── undo ────────────────────────────────────────────────────────────────────

void SceneryDesign::snapshot()
{
	undo_stack.push_back(to_dict());
	if (undo_stack.size() > 100)
	{
		undo_stack.pop_front();
	}
}

bool SceneryDesign::can_undo() const { return !undo_stack.is_empty(); }

bool SceneryDesign::undo()
{
	if (undo_stack.is_empty())
	{
		return false;
	}
	from_dict(undo_stack.pop_back());
	dirty = true;
	return true;
}

// ── persistence ─────────────────────────────────────────────────────────────

Array SceneryDesign::poly_to_json(const Array &poly)
{
	Array pl;
	for (int i = 0; i < poly.size(); i++)
	{
		const Vector2 p = poly[i];
		Array pt;
		pt.push_back(p.x);
		pt.push_back(p.y);
		pl.push_back(pt);
	}
	return pl;
}

Array SceneryDesign::poly_from_json(const Array &poly)
{
	Array pl;
	for (int i = 0; i < poly.size(); i++)
	{
		const Array p = poly[i];
		pl.push_back(Vector2((double)p[0], (double)p[1]));
	}
	return pl;
}

Dictionary SceneryDesign::to_dict() const
{
	Array nl;
	Array nids = nodes.keys();
	nids.sort();
	for (int i = 0; i < nids.size(); i++)
	{
		const Dictionary nd = nodes[nids[i]];
		Array rec;
		rec.push_back(nids[i]);
		rec.push_back(nd["e"]);
		rec.push_back(nd["n"]);
		if (nd.has("h"))
		{
			rec.push_back(nd["h"]);
		}
		nl.push_back(rec);
	}
	Array el;
	Array eids = edges.keys();
	eids.sort();
	for (int i = 0; i < eids.size(); i++)
	{
		Dictionary d = Dictionary(edges[eids[i]]).duplicate(true);
		d["id"] = eids[i];
		el.push_back(d);
	}
	Array rl;
	for (int i = 0; i < replace.size(); i++)
	{
		rl.push_back(poly_to_json(replace[i]));
	}
	Array fl;
	for (int i = 0; i < flatten.size(); i++)
	{
		const Dictionary f = flatten[i];
		Dictionary o;
		o["poly"] = poly_to_json(f["poly"]);
		o["h"] = (double)f["h"];
		fl.push_back(o);
	}
	Array al;
	for (int i = 0; i < areas.size(); i++)
	{
		const Dictionary a = areas[i];
		Dictionary o;
		o["poly"] = poly_to_json(a["poly"]);
		o["type"] = String(a["type"]);
		o["h"] = (double)a.get("h", 0.0);
		al.push_back(o);
	}
	Dictionary out;
	out["schema"] = SCHEMA;
	out["crs"] = "EPSG:2056";
	out["next_id"] = next_id;
	out["nodes"] = nl;
	out["edges"] = el;
	out["replace"] = rl;
	out["replace_ids"] = replace_ids.duplicate();
	out["flatten"] = fl;
	out["areas"] = al;
	if (!patch_cache.is_empty())
	{
		out["patches"] = patches_to_json(patch_cache);
	}
	return out;
}

Dictionary SceneryDesign::patches_to_json(const Dictionary &pc)
{
	Dictionary out;
	Array plateaus, corridors, protect_polys, protect_corridors;
	const Array pls = pc.get("plateaus", Array());
	for (int i = 0; i < pls.size(); i++)
	{
		const Dictionary pl = pls[i];
		Dictionary o;
		o["poly"] = poly_to_json(pl["poly"]);
		o["h"] = (double)pl["h"];
		plateaus.push_back(o);
	}
	const Array cs = pc.get("corridors", Array());
	for (int i = 0; i < cs.size(); i++)
	{
		const Dictionary c = cs[i];
		Array st;
		const Array stations = c["stations"];
		for (int k = 0; k < stations.size(); k++)
		{
			const Vector3 v = stations[k];
			Array a;
			a.push_back(v.x);
			a.push_back(v.y);
			a.push_back(v.z);
			st.push_back(a);
		}
		Dictionary o;
		o["stations"] = st;
		o["reach"] = (double)c["reach"];
		corridors.push_back(o);
	}
	const Array pps = pc.get("protect_polys", Array());
	for (int i = 0; i < pps.size(); i++)
	{
		protect_polys.push_back(poly_to_json(pps[i]));
	}
	const Array pcs = pc.get("protect_corridors", Array());
	for (int i = 0; i < pcs.size(); i++)
	{
		const Dictionary c = pcs[i];
		Dictionary o;
		o["stations"] = poly_to_json(c["stations"]);
		o["reach"] = (double)c["reach"];
		protect_corridors.push_back(o);
	}
	out["plateaus"] = plateaus;
	out["corridors"] = corridors;
	out["protect_polys"] = protect_polys;
	out["protect_corridors"] = protect_corridors;
	return out;
}

Dictionary SceneryDesign::patches_from_json(const Dictionary &d)
{
	Dictionary out;
	Array plateaus, corridors, protect_polys, protect_corridors;
	const Array pls = d.get("plateaus", Array());
	for (int i = 0; i < pls.size(); i++)
	{
		const Dictionary pl = pls[i];
		PackedVector2Array poly;
		const Array pts = pl.get("poly", Array());
		for (int k = 0; k < pts.size(); k++)
		{
			const Array p = pts[k];
			poly.push_back(Vector2((double)p[0], (double)p[1]));
		}
		Dictionary o;
		o["poly"] = poly;
		o["h"] = (double)pl.get("h", 0.0);
		plateaus.push_back(o);
	}
	const Array cs = d.get("corridors", Array());
	for (int i = 0; i < cs.size(); i++)
	{
		const Dictionary c = cs[i];
		PackedVector3Array st;
		const Array stations = c.get("stations", Array());
		for (int k = 0; k < stations.size(); k++)
		{
			const Array v = stations[k];
			st.push_back(Vector3((double)v[0], (double)v[1], (double)v[2]));
		}
		Dictionary o;
		o["stations"] = st;
		o["reach"] = (double)c.get("reach", 20.0);
		corridors.push_back(o);
	}
	const Array pps = d.get("protect_polys", Array());
	for (int i = 0; i < pps.size(); i++)
	{
		PackedVector2Array pl;
		const Array pts = pps[i];
		for (int k = 0; k < pts.size(); k++)
		{
			const Array p = pts[k];
			pl.push_back(Vector2((double)p[0], (double)p[1]));
		}
		protect_polys.push_back(pl);
	}
	const Array pcs = d.get("protect_corridors", Array());
	for (int i = 0; i < pcs.size(); i++)
	{
		const Dictionary c = pcs[i];
		PackedVector2Array st;
		const Array stations = c.get("stations", Array());
		for (int k = 0; k < stations.size(); k++)
		{
			const Array v = stations[k];
			st.push_back(Vector2((double)v[0], (double)v[1]));
		}
		Dictionary o;
		o["stations"] = st;
		o["reach"] = (double)c.get("reach", 20.0);
		protect_corridors.push_back(o);
	}
	out["plateaus"] = plateaus;
	out["corridors"] = corridors;
	out["protect_polys"] = protect_polys;
	out["protect_corridors"] = protect_corridors;
	return out;
}

void SceneryDesign::from_dict(const Dictionary &d)
{
	nodes.clear();
	edges.clear();
	const Array nl = d.get("nodes", Array());
	for (int i = 0; i < nl.size(); i++)
	{
		const Array rec = nl[i];
		Dictionary nd;
		nd["e"] = (double)rec[1];
		nd["n"] = (double)rec[2];
		if (rec.size() > 3 && std::abs((double)rec[3]) >= 0.005)
		{
			nd["h"] = (double)rec[3];
		}
		nodes[(int)(double)rec[0]] = nd;
	}
	const Array el = d.get("edges", Array());
	for (int i = 0; i < el.size(); i++)
	{
		const Dictionary rec = el[i];
		Dictionary ed;
		ed["a"] = (int)(double)rec["a"];
		ed["b"] = (int)(double)rec["b"];
		for (const char *k : EDGE_ATTRS)
		{
			const Variant v = rec.get(k, attr_default(k));
			const String key(k);
			if (key == "lanes" || key == "gauge" || key == "tracks")
			{
				ed[k] = (int)(double)v;
			}
			else if (key == "width")
			{
				ed[k] = (double)v;
			}
			else
			{
				ed[k] = v;
			}
		}
		if (rec.has("via") && rec["via"].get_type() == Variant::ARRAY && Array(rec["via"]).size() == 2)
		{
			const Array v = rec["via"];
			Array via;
			via.push_back((double)v[0]);
			via.push_back((double)v[1]);
			ed["via"] = via;
		}
		edges[(int)(double)rec["id"]] = ed;
	}
	replace.clear();
	const Array rl = d.get("replace", Array());
	for (int i = 0; i < rl.size(); i++)
	{
		const Array pl = poly_from_json(rl[i]);
		if (pl.size() >= 3)
		{
			replace.push_back(pl);
		}
	}
	flatten.clear();
	const Array fl = d.get("flatten", Array());
	for (int i = 0; i < fl.size(); i++)
	{
		const Dictionary f = fl[i];
		const Array pl = poly_from_json(f.get("poly", Array()));
		if (pl.size() >= 3)
		{
			Dictionary o;
			o["poly"] = pl;
			o["h"] = (double)f.get("h", 0.0);
			flatten.push_back(o);
		}
	}
	replace_ids.clear();
	const Array ri = d.get("replace_ids", Array());
	for (int i = 0; i < ri.size(); i++)
	{
		replace_ids.push_back((int)(double)ri[i]);
	}
	areas.clear();
	const Array al = d.get("areas", Array());
	const Array types = get_area_types();
	for (int i = 0; i < al.size(); i++)
	{
		const Dictionary a = al[i];
		const Array pl = poly_from_json(a.get("poly", Array()));
		if (pl.size() >= 3 && types.has(String(a.get("type", ""))))
		{
			Dictionary o;
			o["poly"] = pl;
			o["type"] = String(a["type"]);
			o["h"] = (double)a.get("h", 0.0);
			areas.push_back(o);
		}
	}
	const Variant patches = d.get("patches", Variant());
	patch_cache = patches.get_type() == Variant::DICTIONARY ? patches_from_json(patches) : Dictionary();
	next_id = (int)(double)d.get("next_id", 1);
	const Array nkeys = nodes.keys();
	for (int i = 0; i < nkeys.size(); i++)
	{
		next_id = std::max(next_id, (int)nkeys[i] + 1);
	}
	const Array ekeys = edges.keys();
	for (int i = 0; i < ekeys.size(); i++)
	{
		next_id = std::max(next_id, (int)ekeys[i] + 1);
	}
}

// Whole-file signature, as the bake records it in the roads manifest.
String SceneryDesign::file_sha256(const String &p)
{
	if (!FileAccess::file_exists(p))
	{
		return String();
	}
	return FileAccess::get_sha256(p);
}

bool SceneryDesign::save(const String &p)
{
	if (!p.is_empty())
	{
		path = p;
	}
	if (path.is_empty())
	{
		return false;
	}
	const String dir = path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir))
	{
		DirAccess::make_dir_recursive_absolute(dir);
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null())
	{
		UtilityFunctions::push_warning("SceneryDesign: cannot write ", path);
		return false;
	}
	f->store_string(JSON::stringify(to_dict(), "  "));
	f->close();
	dirty = false;
	return true;
}

bool SceneryDesign::load(const String &p)
{
	path = p;
	if (!FileAccess::file_exists(p))
	{
		nodes.clear();
		edges.clear();
		next_id = 1;
		dirty = false;
		return false;
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(p));
	if (parsed.get_type() != Variant::DICTIONARY)
	{
		UtilityFunctions::push_warning("SceneryDesign: ", p, " is not a design file");
		return false;
	}
	from_dict(parsed);
	dirty = false;
	return true;
}

} // namespace jsbgodot
