#include "scenery_design_mesher.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry2d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "geo.h"
#include "geo_frame.h"

using namespace godot;

namespace jsbgodot
{

static const Color CONCRETE(0.62, 0.61, 0.58);
static const Color BALLAST_COLOUR(0.5, 0.48, 0.45);
static const Color RAIL_COLOUR(120.0 / 255.0, 118.0 / 255.0, 110.0 / 255.0);

void SceneryDesignMesher::_bind_methods()
{
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("road_geometry", "cls", "lanes", "oneway", "width_m"), &SceneryDesignMesher::road_geometry, DEFVAL(0.0));
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("edge_width", "design", "eid"), &SceneryDesignMesher::edge_width);
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("chain_polyline", "design", "chain"), &SceneryDesignMesher::chain_polyline);
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("chain_polyline_offsets", "design", "chain"), &SceneryDesignMesher::chain_polyline_offsets);
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("densify", "pts"), &SceneryDesignMesher::densify);
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("densify_with", "pts", "vals"), &SceneryDesignMesher::densify_with);
	ClassDB::bind_static_method("SceneryDesignMesher", D_METHOD("get_ballast_width_m"), &SceneryDesignMesher::get_ballast_width_m);
	ClassDB::bind_method(D_METHOD("clear"), &SceneryDesignMesher::clear);
	ClassDB::bind_method(D_METHOD("rebuild", "design"), &SceneryDesignMesher::rebuild);
	ClassDB::bind_method(D_METHOD("get_cuts"), &SceneryDesignMesher::get_cuts);
	ClassDB::bind_method(D_METHOD("get_elevation"), &SceneryDesignMesher::get_elevation);
	ClassDB::bind_method(D_METHOD("set_elevation", "callable"), &SceneryDesignMesher::set_elevation);
	ClassDB::bind_method(D_METHOD("get_tint"), &SceneryDesignMesher::get_tint);
	ClassDB::bind_method(D_METHOD("set_tint", "tint"), &SceneryDesignMesher::set_tint);
	ClassDB::bind_method(D_METHOD("get_chain_count"), &SceneryDesignMesher::get_chain_count);
	ClassDB::bind_method(D_METHOD("get_junction_count"), &SceneryDesignMesher::get_junction_count);
	ClassDB::bind_method(D_METHOD("get_area_count"), &SceneryDesignMesher::get_area_count);
	ClassDB::bind_method(D_METHOD("get_pier_count"), &SceneryDesignMesher::get_pier_count);
	ClassDB::bind_method(D_METHOD("get_unknown_height_count"), &SceneryDesignMesher::get_unknown_height_count);
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "elevation"), "set_elevation", "get_elevation");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tint"), "set_tint", "get_tint");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "chain_count"), "", "get_chain_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "junction_count"), "", "get_junction_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "area_count"), "", "get_area_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pier_count"), "", "get_pier_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "unknown_height_count"), "", "get_unknown_height_count");
}

SceneryDesignMesher::SceneryDesignMesher()
{
}

Callable SceneryDesignMesher::get_elevation() const { return elevation; }
void SceneryDesignMesher::set_elevation(const Callable &c) { elevation = c; }
Color SceneryDesignMesher::get_tint() const { return tint; }
void SceneryDesignMesher::set_tint(const Color &c) { tint = c; }
int SceneryDesignMesher::get_chain_count() const { return chain_count; }
int SceneryDesignMesher::get_junction_count() const { return junction_count; }
int SceneryDesignMesher::get_area_count() const { return area_count; }
int SceneryDesignMesher::get_pier_count() const { return pier_count; }
int SceneryDesignMesher::get_unknown_height_count() const { return unknown_height_count; }
Dictionary SceneryDesignMesher::get_cuts() const { return cuts; }
double SceneryDesignMesher::get_ballast_width_m() { return BALLAST_WIDTH_M; }

void SceneryDesignMesher::Buf::push(const Vector3 &v, const Vector3 &n, const Color &c, const Vector2 &uv, const Vector2 &uv2)
{
	verts.push_back(v);
	normals.push_back(n);
	colors.push_back(c);
	uvs.push_back(uv);
	uv2s.push_back(uv2);
}

double SceneryDesignMesher::lane_m(const String &cls)
{
	if (cls == "motorway" || cls == "motorway_link") return 3.75;
	if (cls == "trunk" || cls == "trunk_link") return 3.5;
	if (cls == "primary" || cls == "primary_link") return 3.25;
	if (cls == "secondary" || cls == "secondary_link") return 3.0;
	if (cls == "tertiary" || cls == "tertiary_link") return 2.9;
	return 3.0;
}

Color SceneryDesignMesher::class_colour(const String &cls)
{
	if (cls == "motorway" || cls == "motorway_link") return Color(0.30, 0.30, 0.31);
	if (cls == "trunk" || cls == "trunk_link") return Color(0.32, 0.32, 0.33);
	if (cls == "primary" || cls == "primary_link") return Color(0.34, 0.34, 0.35);
	if (cls == "secondary" || cls == "secondary_link") return Color(0.36, 0.36, 0.37);
	if (cls == "tertiary" || cls == "tertiary_link") return Color(0.38, 0.38, 0.39);
	return Color(0.35, 0.35, 0.36);
}

double SceneryDesignMesher::class_drop_m(const String &cls)
{
	static const struct { const char *c; double d; } drops[] = {
		{"motorway", 0.0}, {"trunk", 0.01}, {"primary", 0.02}, {"secondary", 0.03}, {"tertiary", 0.04},
		{"motorway_link", 0.05}, {"trunk_link", 0.06}, {"primary_link", 0.07}, {"secondary_link", 0.08}, {"tertiary_link", 0.09}};
	for (const auto &d : drops)
	{
		if (cls == d.c)
		{
			return d.d;
		}
	}
	return 0.0;
}

int SceneryDesignMesher::area_kind(const String &typ)
{
	if (typ == "grass") return 10;
	if (typ == "forest") return 11;
	if (typ == "field") return 12;
	if (typ == "water") return 13;
	return 10;
}

Array SceneryDesignMesher::road_geometry(const String &cls, int p_lanes, bool oneway, double width_m)
{
	const double lm = lane_m(cls);
	const bool link = cls.ends_with("_link");
	int lanes = Math::clamp(p_lanes > 0 ? p_lanes : (link ? 1 : 2), 1, 6);
	double width;
	int kind;
	if ((cls == "motorway" || cls == "trunk") && oneway)
	{
		width = lanes * lm + SHOULDER_M + 2 * VERGE_M;
		kind = KIND_ONEWAY;
	}
	else if (cls == "motorway")
	{
		const int per = std::max(2, lanes / 2);
		width = 2 * (per * lm + SHOULDER_M + VERGE_M) + 3.0;
		kind = KIND_DUAL;
		lanes = per;
	}
	else if (link || (oneway && lanes == 1))
	{
		width = lanes * lm + 2 * VERGE_M;
		kind = KIND_LINK;
	}
	else if (oneway)
	{
		width = lanes * lm + 2 * VERGE_M;
		kind = KIND_ONEWAY;
	}
	else
	{
		width = lanes * lm + 2 * VERGE_M;
		kind = (cls == "trunk" || cls == "primary") ? KIND_TWOWAY_LINED : KIND_TWOWAY;
	}
	if (width_m >= 3.0 && width_m <= 60.0)
	{
		width = width_m; // an explicit width (OSM width tag) wins, as in the bake
		if (width < 5.0 && kind != KIND_ONEWAY)
		{
			kind = KIND_LINK; // a narrow road has no centre line
		}
	}
	Array out;
	out.push_back(width);
	out.push_back(kind);
	out.push_back(lanes);
	return out;
}

void SceneryDesignMesher::ensure_material()
{
	if (material.is_null())
	{
		material.instantiate();
		material->set_shader(ResourceLoader::get_singleton()->load("res://addons/scenery3d/shaders/roads.gdshader"));
	}
}

void SceneryDesignMesher::clear()
{
	const TypedArray<Node> kids = get_children();
	for (int i = 0; i < kids.size(); i++)
	{
		Node *c = Object::cast_to<Node>(kids[i]);
		remove_child(c); // out of the tree now, freed at the frame end
		c->queue_free();
	}
	chain_count = 0;
}

void SceneryDesignMesher::rebuild(const Ref<SceneryDesign> &design)
{
	ensure_material();
	clear();
	unknown_height_count = 0;
	junction_count = 0;
	area_count = 0;
	pier_count = 0;
	cuts = Dictionary();
	if (design.is_null())
	{
		return;
	}
	const Array nids = design->get_nodes().keys();
	for (int i = 0; i < nids.size(); i++)
	{
		const int nid = nids[i];
		const Dictionary j = junction(design, nid);
		if (j.is_empty())
		{
			continue;
		}
		const Array legs = j["legs"];
		for (int k = 0; k < legs.size(); k++)
		{
			const Dictionary leg = legs[k];
			const int eid = leg["edge"];
			if (!cuts.has(eid))
			{
				cuts[eid] = Dictionary();
			}
			Dictionary per_node = cuts[eid];
			per_node[nid] = leg["cut"];
		}
		MeshInstance3D *mi = build_junction(design, nid, j);
		if (mi != nullptr)
		{
			add_child(mi);
			junction_count++;
		}
	}
	const Array chains = design->chains();
	for (int i = 0; i < chains.size(); i++)
	{
		MeshInstance3D *mi = build_chain(design, chains[i]);
		if (mi != nullptr)
		{
			add_child(mi);
			chain_count++;
		}
	}
	const Array areas = design->get_areas();
	for (int i = 0; i < areas.size(); i++)
	{
		MeshInstance3D *mi = build_area(areas[i], i);
		if (mi != nullptr)
		{
			add_child(mi);
			area_count++;
		}
	}
}

double SceneryDesignMesher::edge_width(const Ref<SceneryDesign> &design, int eid)
{
	const Dictionary ed = design->get_edges()[eid];
	if (String(ed["kind"]) != "road")
	{
		return 0.0;
	}
	return (double)road_geometry(ed["cls"], (int)ed["lanes"], (bool)ed["oneway"], (double)ed.get("width", 0.0))[0];
}

// Pavement geometry of a node with three or more road edges: legs sorted by
// bearing with the corner where the edge lines of neighbouring legs meet,
// the polygon (LV95, counter-clockwise, fanned from the node) and how far
// each leg's ribbon is cut back. Empty when not a junction.
Dictionary SceneryDesignMesher::junction(const Ref<SceneryDesign> &design, int nid) const
{
	struct Leg { int edge; Vector2 dir; double ang, half, cut; String cls; };
	std::vector<Leg> legs;
	const Array adj = design->edges_of(nid);
	for (int i = 0; i < adj.size(); i++)
	{
		const int eid = adj[i];
		const Dictionary ed = design->get_edges()[eid];
		if (String(ed["kind"]) != "road")
		{
			continue;
		}
		Array pts = design->edge_points(eid);
		if ((int)ed["b"] == nid)
		{
			pts.reverse();
		}
		const Vector2 d = (Vector2(pts[1]) - Vector2(pts[0])).normalized();
		legs.push_back({eid, d, d.angle(), edge_width(design, eid) * 0.5, 0.0, ed["cls"]});
	}
	if (legs.size() < 3)
	{
		return Dictionary();
	}
	std::sort(legs.begin(), legs.end(), [](const Leg &x, const Leg &y) { return x.ang < y.ang; });
	const Vector2 centre = design->node_pos(nid);
	const int k = legs.size();
	std::vector<Vector2> corners;
	for (int i = 0; i < k; i++)
	{
		Leg &a = legs[i];
		Leg &b = legs[(i + 1) % k];
		const Vector2 da = a.dir, db = b.dir;
		const Vector2 na(-da.y, da.x); // left of leg a (counter-clockwise side)
		const Vector2 nb(-db.y, db.x);
		const Vector2 pa = centre + na * a.half; // a point on a's left edge line
		const Vector2 pb = centre - nb * b.half; // a point on b's right edge line
		Vector2 corner;
		const double den = da.cross(db);
		if (std::abs(den) < 0.05)
		{
			// nearly parallel legs (a fork): the corner sits between the edge lines
			corner = (pa + pb) * 0.5 + (da + db).normalized() * std::max(a.half, b.half);
		}
		else
		{
			const double t = (pb - pa).cross(db) / den;
			corner = pa + da * t;
			// a corner behind the node (very obtuse pair) is pulled forward
			if ((corner - centre).dot(da) < 0.0 && (corner - centre).dot(db) < 0.0)
			{
				corner = (pa + pb) * 0.5;
			}
		}
		corners.push_back(corner);
		a.cut = std::max(a.cut, (double)(corner - centre).dot(da));
		b.cut = std::max(b.cut, (double)(corner - centre).dot(db));
	}
	Array leg_dicts, poly;
	for (int i = 0; i < k; i++)
	{
		Leg &leg = legs[i];
		leg.cut = std::max(leg.cut, JUNCTION_MIN_CUT_M);
		const Vector2 d = leg.dir;
		const Vector2 n(-d.y, d.x);
		const Vector2 base = centre + d * leg.cut;
		poly.push_back(base - n * leg.half);
		poly.push_back(base + n * leg.half);
		poly.push_back(corners[i]);
		Dictionary ld;
		ld["edge"] = leg.edge;
		ld["dir"] = leg.dir;
		ld["ang"] = leg.ang;
		ld["half"] = leg.half;
		ld["cls"] = leg.cls;
		ld["cut"] = leg.cut;
		leg_dicts.push_back(ld);
	}
	Dictionary out;
	out["legs"] = leg_dicts;
	out["poly"] = poly;
	out["centre"] = centre;
	return out;
}

double SceneryDesignMesher::ground(double e, double n) const
{
	if (!elevation.is_valid())
	{
		return NAN;
	}
	return elevation.call(e, n);
}

Transform3D SceneryDesignMesher::frame_at_lv95(const Vector2 &p, double h)
{
	const Vector2 ll = Geo::lv95_to_wgs84(p.x, p.y);
	return Transform3D(geo::wun_basis(ll.x, ll.y), geo::ecef_of(ll.x, ll.y, h));
}

Vector3 SceneryDesignMesher::local_of(const Transform3D &inv, const Vector2 &p, double h)
{
	const Vector2 ll = Geo::lv95_to_wgs84(p.x, p.y);
	return inv.xform(geo::ecef_of(ll.x, ll.y, h));
}

// Fan mesh of the junction polygon, at the highest class lift of its legs.
MeshInstance3D *SceneryDesignMesher::build_junction(const Ref<SceneryDesign> &design, int nid, const Dictionary &j)
{
	const Vector2 centre = j["centre"];
	const Array poly = j["poly"];
	Color colour(0.34, 0.34, 0.35);
	double best_drop = 1.0;
	const Array legs = j["legs"];
	for (int i = 0; i < legs.size(); i++)
	{
		const String cls = Dictionary(legs[i])["cls"];
		const double drop = class_drop_m(cls);
		if (drop < best_drop)
		{
			best_drop = drop;
			colour = class_colour(cls);
		}
	}
	const double lift = ROAD_LIFT_M - best_drop + 0.005;
	// one height for the whole patch: the highest ground under it
	double h = ground(centre.x, centre.y);
	for (int i = 0; i < poly.size(); i++)
	{
		const Vector2 p = poly[i];
		const double hp = ground(p.x, p.y);
		if (!std::isnan(hp) && (std::isnan(h) || hp > h))
		{
			h = hp;
		}
	}
	if (std::isnan(h))
	{
		unknown_height_count++;
		return nullptr;
	}
	h += design->node_offset(nid);
	const Transform3D frame = frame_at_lv95(centre, h + lift);
	const Transform3D inv = frame.affine_inverse();
	Buf buf;
	const Color col(colour.r, colour.g, colour.b, (double)KIND_JUNCTION / 255.0);
	buf.push(local_of(inv, centre, h + lift), Vector3(0, 1, 0), col, Vector2(), Vector2());
	for (int i = 0; i < poly.size(); i++)
	{
		const Vector2 p = poly[i];
		buf.push(local_of(inv, p, h + lift), Vector3(0, 1, 0), col, p - centre, Vector2());
	}
	const int n = poly.size();
	for (int i = 0; i < n; i++)
	{
		buf.idx.push_back(0);
		buf.idx.push_back(1 + i);
		buf.idx.push_back(1 + (i + 1) % n);
	}
	MeshInstance3D *mi = mesh_instance(buf, frame);
	mi->set_name(String("Junction_") + String::num_int64(nid));
	mi->set_meta("node", nid);
	return mi;
}

MeshInstance3D *SceneryDesignMesher::mesh_instance(const Buf &buf, const Transform3D &frame)
{
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = buf.verts;
	arrays[Mesh::ARRAY_NORMAL] = buf.normals;
	arrays[Mesh::ARRAY_COLOR] = buf.colors;
	arrays[Mesh::ARRAY_TEX_UV] = buf.uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = buf.uv2s;
	arrays[Mesh::ARRAY_INDEX] = buf.idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(mesh);
	mi->set_material_override(material);
	mi->set_transform(frame);
	mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	return mi;
}

// Cut `metres` off the start of a polyline (LV95), keeping the direction.
Array SceneryDesignMesher::trim_start(const Array &pts, double metres)
{
	double left = metres;
	Array out = pts.duplicate();
	while (out.size() >= 2 && left > 0.0)
	{
		const double seg = Vector2(out[0]).distance_to(out[1]);
		if (seg <= left)
		{
			left -= seg;
			out.pop_front();
		}
		else
		{
			out[0] = Vector2(out[0]).lerp(out[1], left / seg);
			left = 0.0;
		}
	}
	return out;
}

Array SceneryDesignMesher::trim_vals(const Array &pts, const Array &vals, double metres)
{
	double left = metres;
	Array out = vals.duplicate();
	Array p = pts.duplicate();
	while (p.size() >= 2 && left > 0.0)
	{
		const double seg = Vector2(p[0]).distance_to(p[1]);
		if (seg <= left)
		{
			left -= seg;
			p.pop_front();
			out.pop_front();
		}
		else
		{
			out[0] = Math::lerp((double)out[0], (double)out[1], left / seg);
			left = 0.0;
		}
	}
	return out;
}

// Half width of the road on the far side of a chain end, when the end node
// joins exactly one other road edge of a different width; -1 otherwise.
double SceneryDesignMesher::neighbour_half(const Ref<SceneryDesign> &design, const Dictionary &chain, bool at_start)
{
	const Array node_list = chain["nodes"];
	const Array edge_list = chain["edges"];
	const int node_id = at_start ? (int)node_list[0] : (int)node_list[node_list.size() - 1];
	const int own_edge = at_start ? (int)edge_list[0] : (int)edge_list[edge_list.size() - 1];
	const Array adj = design->edges_of(node_id);
	if (adj.size() != 2)
	{
		return -1.0;
	}
	const int other = (int)adj[1] == own_edge ? (int)adj[0] : (int)adj[1];
	const Dictionary edges = design->get_edges();
	if (String(Dictionary(edges[other])["kind"]) != "road" || String(Dictionary(edges[own_edge])["kind"]) != "road")
	{
		return -1.0;
	}
	const double w_other = edge_width(design, other);
	const double w_own = edge_width(design, own_edge);
	if (std::abs(w_other - w_own) < 0.05)
	{
		return -1.0;
	}
	return w_other * 0.5;
}

Array SceneryDesignMesher::chain_polyline(const Ref<SceneryDesign> &design, const Dictionary &chain)
{
	return chain_polyline_offsets(design, chain)["pts"];
}

Dictionary SceneryDesignMesher::chain_polyline_offsets(const Ref<SceneryDesign> &design, const Dictionary &chain)
{
	Array out, offs;
	const Array node_list = chain["nodes"];
	const Array edge_list = chain["edges"];
	const Dictionary edges = design->get_edges();
	for (int i = 0; i < edge_list.size(); i++)
	{
		const int eid = edge_list[i];
		Array pts = design->edge_points(eid);
		if ((int)Dictionary(edges[eid])["a"] != (int)node_list[i])
		{
			pts.reverse();
		}
		const double h0 = design->node_offset(node_list[i]);
		const double h1 = design->node_offset(node_list[i + 1]);
		double total = 0.0;
		for (int k = 1; k < pts.size(); k++)
		{
			total += Vector2(pts[k]).distance_to(pts[k - 1]);
		}
		double run = 0.0;
		for (int k = 0; k < pts.size(); k++)
		{
			if (k > 0)
			{
				run += Vector2(pts[k]).distance_to(pts[k - 1]);
			}
			if (i > 0 && k == 0)
			{
				continue;
			}
			out.push_back(pts[k]);
			offs.push_back(Math::lerp(h0, h1, total > 0.0 ? run / total : 0.0));
		}
	}
	Dictionary d;
	d["pts"] = out;
	d["offs"] = offs;
	return d;
}

Array SceneryDesignMesher::densify(const Array &pts)
{
	return densify_with(pts, Array())["pts"];
}

// Densify to DENSIFY_M stations, interpolating a parallel value array.
Dictionary SceneryDesignMesher::densify_with(const Array &pts, const Array &vals)
{
	Array out, ov;
	const bool has_vals = vals.size() == pts.size();
	for (int i = 0; i + 1 < pts.size(); i++)
	{
		const Vector2 a = pts[i], b = pts[i + 1];
		const int n = std::max(1, (int)std::ceil(a.distance_to(b) / DENSIFY_M));
		for (int k = 0; k < n; k++)
		{
			const double t = (double)k / n;
			out.push_back(a.lerp(b, t));
			if (has_vals)
			{
				ov.push_back(Math::lerp((double)vals[i], (double)vals[i + 1], t));
			}
		}
	}
	if (pts.size() > 0)
	{
		out.push_back(pts[pts.size() - 1]);
		if (has_vals)
		{
			ov.push_back((double)vals[vals.size() - 1]);
		}
	}
	Dictionary d;
	d["pts"] = out;
	d["vals"] = ov;
	return d;
}

MeshInstance3D *SceneryDesignMesher::build_chain(const Ref<SceneryDesign> &design, const Dictionary &chain)
{
	const Dictionary attrs = chain["attrs"];
	const Dictionary po = chain_polyline_offsets(design, chain);
	Array pts = po["pts"];
	Array offs = po["offs"];
	if (pts.size() < 2)
	{
		return nullptr;
	}
	// legs of a junction start at the pavement edge
	const Array edge_list = chain["edges"];
	const Array node_list = chain["nodes"];
	const int first_edge = edge_list[0], last_edge = edge_list[edge_list.size() - 1];
	const double cut0 = Dictionary(cuts.get(first_edge, Dictionary())).get(node_list[0], 0.0);
	const double cut1 = Dictionary(cuts.get(last_edge, Dictionary())).get(node_list[node_list.size() - 1], 0.0);
	if (cut0 > 0.0)
	{
		offs = trim_vals(pts, offs, cut0);
		pts = trim_start(pts, cut0);
	}
	if (cut1 > 0.0)
	{
		pts.reverse();
		offs.reverse();
		offs = trim_vals(pts, offs, cut1);
		pts = trim_start(pts, cut1);
		pts.reverse();
		offs.reverse();
	}
	if (pts.size() < 2 || Vector2(pts[0]).distance_to(pts[pts.size() - 1]) < 0.5)
	{
		return nullptr;
	}
	const Dictionary dens = densify_with(pts, offs);
	pts = dens["pts"];
	offs = dens["vals"];
	const bool is_road = String(attrs["kind"]) == "road";
	double width, count, gauge_m = 1.435;
	int kind;
	if (is_road)
	{
		const Array g = road_geometry(attrs["cls"], (int)attrs["lanes"], (bool)attrs["oneway"], (double)attrs.get("width", 0.0));
		width = g[0];
		kind = g[1];
		count = (double)(int)g[2];
	}
	else
	{
		const int tracks = Math::clamp((int)attrs.get("tracks", 1), 1, 4);
		width = BALLAST_WIDTH_M + TRACK_SPACING_M * (tracks - 1);
		kind = KIND_BALLAST;
		count = tracks;
		const int gauge = attrs["gauge"];
		gauge_m = (gauge > 0 ? (double)gauge : 1435.0) / 1000.0;
	}
	// half width per station: constant, except a lane change at either end,
	// where the wider road tapers to the narrower over 30 m per lane
	const double half = width * 0.5;
	const int n = pts.size();
	std::vector<double> along(n, 0.0);
	for (int i = 1; i < n; i++)
	{
		along[i] = along[i - 1] + Vector2(pts[i]).distance_to(pts[i - 1]);
	}
	const double total = along[n - 1];
	const double nb0 = is_road ? neighbour_half(design, chain, true) : -1.0;
	const double nb1 = is_road ? neighbour_half(design, chain, false) : -1.0;
	Vector<double> halves;
	for (int i = 0; i < n; i++)
	{
		double hw = half;
		if (nb0 >= 0.0 && nb0 < half)
		{
			const double L = TAPER_PER_LANE_M * std::max(1.0, (half - nb0) / 1.6);
			hw = std::min(hw, Math::lerp(nb0, half, Math::clamp(along[i] / L, 0.0, 1.0)));
		}
		if (nb1 >= 0.0 && nb1 < half)
		{
			const double L1 = TAPER_PER_LANE_M * std::max(1.0, (half - nb1) / 1.6);
			hw = std::min(hw, Math::lerp(nb1, half, Math::clamp((total - along[i]) / L1, 0.0, 1.0)));
		}
		halves.push_back(hw);
	}
	// ground: highest of centre and both edges at every station, no smoothing
	std::vector<double> heights(n, NAN);
	double last_h = NAN;
	for (int i = 0; i < n; i++)
	{
		const Vector2 p = pts[i];
		const Vector2 d = (Vector2(pts[std::min(i + 1, n - 1)]) - Vector2(pts[std::max(i - 1, 0)])).normalized();
		const Vector2 nrm(-d.y, d.x);
		double h = NAN;
		for (double f : {-1.0, 0.0, 1.0})
		{
			const Vector2 q = p + nrm * half * f;
			const double hh = ground(q.x, q.y);
			if (!std::isnan(hh) && (std::isnan(h) || hh > h))
			{
				h = hh;
			}
		}
		if (std::isnan(h))
		{
			unknown_height_count++;
			h = last_h;
		}
		heights[i] = h;
		last_h = h;
	}
	double first_known = NAN;
	for (double h : heights)
	{
		if (!std::isnan(h))
		{
			first_known = h;
			break;
		}
	}
	if (std::isnan(first_known))
	{
		return nullptr; // no terrain under this chain yet
	}
	Vector<double> offs_v;
	for (int i = 0; i < n; i++)
	{
		if (std::isnan(heights[i]))
		{
			heights[i] = first_known;
		}
		heights[i] += (double)offs[i]; // node height offsets (decks, cuts)
		offs_v.push_back((double)offs[i]);
	}
	// +3 cm over the baked lift: a picked road draws over its OSM original until the re-bake
	double lift = is_road ? (ROAD_LIFT_M + 0.03 - class_drop_m(attrs["cls"])) : BALLAST_LIFT_M + 0.03;
	if (!is_road)
	{
		// the diverging track of a switch shares the bed with the through
		// track for a few metres: a few millimetres per chain avoid z-fighting
		lift += 0.004 * (double)((int)edge_list[0] % 4);
	}
	// local frame at the first station
	const Transform3D frame = frame_at_lv95(pts[0], heights[0] + lift);
	const Transform3D inv = frame.affine_inverse();
	Vector<Vector3> local;
	for (int i = 0; i < n; i++)
	{
		local.push_back(local_of(inv, pts[i], heights[i] + lift));
	}
	Buf buf;
	Color colour = is_road ? class_colour(attrs["cls"]) : BALLAST_COLOUR;
	colour = Color(colour.r * tint.r, colour.g * tint.g, colour.b * tint.b);
	Section road;
	road.pts.push_back(Vector2(-half, 0.0));
	road.pts.push_back(Vector2(half, 0.0));
	extrude(buf, local, road, colour, kind, 0.0, half, count, halves);
	structures(buf, local, offs_v, halves.size() == local.size() ? halves : Vector<double>(), half);
	if (!is_road)
	{
		const int tracks = (int)count;
		Section rail;
		rail.pts.push_back(Vector2(-RAIL_FOOT_M / 2, 0.0));
		rail.pts.push_back(Vector2(-RAIL_HEAD_M / 2, RAIL_TOP_M));
		rail.pts.push_back(Vector2(RAIL_HEAD_M / 2, RAIL_TOP_M));
		rail.pts.push_back(Vector2(RAIL_FOOT_M / 2, 0.0));
		for (int tr = 0; tr < tracks; tr++)
		{
			const double centre = (tr - (tracks - 1) / 2.0) * TRACK_SPACING_M;
			for (double side : {-1.0, 1.0})
			{
				extrude(buf, local, rail, RAIL_COLOUR, KIND_RAIL, centre + side * gauge_m / 2.0, RAIL_FOOT_M / 2, 1.0);
			}
		}
	}
	MeshInstance3D *mi = mesh_instance(buf, frame);
	mi->set_name(String("Chain_") + String::num_int64((int)edge_list[0]));
	mi->set_meta("edges", edge_list);
	return mi;
}

// Bridge structure under elevated stations: parapets along both edges of
// every elevated run and a pier every PIER_SPACING_M from the deck's
// underside down to the ground (the station's offset). Concrete kind.
void SceneryDesignMesher::structures(Buf &buf, const Vector<Vector3> &local, const Vector<double> &offs, const Vector<double> &halves, double half)
{
	const int n = local.size();
	if (n < 2 || offs.size() != n)
	{
		return;
	}
	Section parapet;
	parapet.pts.push_back(Vector2(-PARAPET_W_M * 0.5, 0.0));
	parapet.pts.push_back(Vector2(-PARAPET_W_M * 0.5, PARAPET_H_M));
	parapet.pts.push_back(Vector2(PARAPET_W_M * 0.5, PARAPET_H_M));
	parapet.pts.push_back(Vector2(PARAPET_W_M * 0.5, 0.0));
	int run_start = -1;
	double since_pier = PIER_SPACING_M;
	for (int i = 0; i <= n; i++)
	{
		const bool elevated = i < n && offs[i] > ELEVATED_M;
		if (elevated && run_start < 0)
		{
			run_start = i;
			since_pier = PIER_SPACING_M;
		}
		if ((!elevated || i == n) && run_start >= 0)
		{
			Vector<Vector3> seg;
			for (int k = run_start; k < i; k++)
			{
				seg.push_back(local[k]);
			}
			if (seg.size() >= 2)
			{
				for (double side : {-1.0, 1.0})
				{
					const double u0 = side * (half - PARAPET_W_M * 0.5);
					extrude(buf, seg, parapet, CONCRETE, KIND_CONCRETE, u0, 0.0, 0.0);
				}
			}
			run_start = -1;
		}
		if (elevated)
		{
			if (i > 0)
			{
				since_pier += local[i].distance_to(local[i - 1]);
			}
			if (since_pier >= PIER_SPACING_M)
			{
				since_pier = 0.0;
				pier(buf, local, i, offs[i], halves.size() == n ? halves[i] : half);
				pier_count++;
			}
		}
	}
}

// A box pier under station i: from the deck underside to the ground.
void SceneryDesignMesher::pier(Buf &buf, const Vector<Vector3> &local, int i, double height, double hw)
{
	const Vector3 p = local[i];
	const Vector3 q = i < local.size() - 1 ? local[std::min(i + 1, (int)local.size() - 1)] : local[i - 1];
	const Vector2 d = Vector2(q.x - p.x, q.z - p.z).normalized();
	const Vector2 nrm(-d.y, d.x);
	const double w = std::max(hw * 2.0 - 2.0, PIER_W_M); // across the deck, 1 m in from each edge
	const double t = PIER_W_M * 0.5;
	const double top = -ROAD_LIFT_M;
	const double bottom = -height - ROAD_LIFT_M - 0.5;
	const int base = buf.verts.size();
	Vector3 corners[4];
	int c = 0;
	for (double sx : {-1.0, 1.0})
	{
		for (double sz : {-1.0, 1.0})
		{
			corners[c++] = Vector3(p.x + nrm.x * w * 0.5 * sx + d.x * t * sz, 0.0, p.z + nrm.y * w * 0.5 * sx + d.y * t * sz);
		}
	}
	const Color col(CONCRETE.r, CONCRETE.g, CONCRETE.b, (double)KIND_CONCRETE / 255.0);
	// four walls, each a quad with metres UVs (u across, v = height)
	const int order[4] = {0, 1, 3, 2};
	for (int k = 0; k < 4; k++)
	{
		const Vector3 a = corners[order[k]];
		const Vector3 b = corners[order[(k + 1) % 4]];
		const Vector3 nx = Vector3(b.z - a.z, 0.0, -(b.x - a.x)).normalized();
		const double len = a.distance_to(b);
		const struct { Vector3 v; double h; double u; } quad[4] = {{a, bottom, 0.0}, {b, bottom, len}, {b, top, len}, {a, top, 0.0}};
		for (const auto &v : quad)
		{
			buf.push(Vector3(v.v.x, p.y + v.h, v.v.z), nx, col, Vector2(v.u, v.h - bottom), Vector2());
		}
		const int s0 = base + k * 4;
		const int tri[6] = {s0, s0 + 1, s0 + 2, s0, s0 + 2, s0 + 3};
		for (int ix : tri) buf.idx.push_back(ix);
	}
}

// An area: a flat patch (grass, forest, field, water) on the highest ground
// under it, or a building (roof at ground + h, walls down to the ground).
MeshInstance3D *SceneryDesignMesher::build_area(const Dictionary &area, int index)
{
	const Array poly = area["poly"];
	if (poly.size() < 3)
	{
		return nullptr;
	}
	const String typ = area["type"];
	Vector2 centre;
	for (int i = 0; i < poly.size(); i++)
	{
		centre += Vector2(poly[i]);
	}
	centre /= poly.size();
	double h = ground(centre.x, centre.y);
	double lowest = h;
	for (int i = 0; i < poly.size(); i++)
	{
		const Vector2 p = poly[i];
		const double hp = ground(p.x, p.y);
		if (std::isnan(hp))
		{
			continue;
		}
		if (std::isnan(h) || hp > h) h = hp;
		if (std::isnan(lowest) || hp < lowest) lowest = hp;
	}
	if (std::isnan(h))
	{
		unknown_height_count++;
		return nullptr;
	}
	const bool building = typ == "building";
	double lift = building ? 0.0 : 0.06;
	if (typ == "water")
	{
		h = lowest; // water fills the low ground
		lift = 0.02;
	}
	const Transform3D frame = frame_at_lv95(centre, h + lift);
	const Transform3D inv = frame.affine_inverse();
	PackedVector2Array local2;
	Vector<Vector3> local3;
	for (int i = 0; i < poly.size(); i++)
	{
		const Vector3 v = local_of(inv, poly[i], h + lift);
		local3.push_back(v);
		local2.push_back(Vector2(v.x, v.z));
	}
	const PackedInt32Array tris = Geometry2D::get_singleton()->triangulate_polygon(local2);
	if (tris.is_empty())
	{
		return nullptr;
	}
	Buf buf;
	const double top_h = building ? (double)area.get("h", 0.0) : 0.0;
	const int kind = building ? KIND_ROOF : area_kind(typ);
	const Color col(0.5, 0.5, 0.5, (double)kind / 255.0);
	for (int i = 0; i < local3.size(); i++)
	{
		const Vector3 v = local3[i];
		buf.push(Vector3(v.x, top_h, v.z), Vector3(0, 1, 0), col, Vector2(v.x, v.z), Vector2());
	}
	buf.idx.append_array(tris);
	if (building)
	{
		const Color wcol(0.5, 0.5, 0.5, (double)KIND_WALL / 255.0);
		const double bottom = lowest - h - 0.5;
		const int n = local3.size();
		for (int i = 0; i < n; i++)
		{
			const Vector3 a = local3[i], b = local3[(i + 1) % n];
			const Vector3 nx = Vector3(b.z - a.z, 0.0, -(b.x - a.x)).normalized();
			const int s0 = buf.verts.size();
			const double len = a.distance_to(b);
			const struct { Vector3 v; double h; double u; } quad[4] = {{a, bottom, 0.0}, {b, bottom, len}, {b, top_h, len}, {a, top_h, 0.0}};
			for (const auto &v : quad)
			{
				buf.push(Vector3(v.v.x, v.h, v.v.z), nx, wcol, Vector2(v.u, v.h - bottom), Vector2());
			}
			const int tri[6] = {s0, s0 + 1, s0 + 2, s0, s0 + 2, s0 + 3};
			for (int ix : tri) buf.idx.push_back(ix);
		}
	}
	MeshInstance3D *mi = mesh_instance(buf, frame);
	mi->set_name(String("Area_") + String::num_int64(index));
	mi->set_meta("area", index);
	return mi;
}

// Per-station across unit vectors (x/z of the local frame, left of travel)
// scaled by the mitre at corners.
Vector<Vector2> SceneryDesignMesher::frames(const Vector<Vector3> &local)
{
	const int n = local.size();
	Vector<Vector2> out;
	for (int i = 0; i < n; i++)
	{
		const Vector3 p0 = local[std::max(i - 1, 0)], p1 = local[i], p2 = local[std::min(i + 1, n - 1)];
		Vector2 t0 = Vector2(p1.x - p0.x, p1.z - p0.z).normalized();
		Vector2 t1 = Vector2(p2.x - p1.x, p2.z - p1.z).normalized();
		if (i == 0) t0 = t1;
		if (i == n - 1) t1 = t0;
		const Vector2 n0(-t0.y, t0.x), n1(-t1.y, t1.x);
		const double den = 1.0 + t0.dot(t1);
		Vector2 m;
		if (den < 1e-4)
		{
			m = n0;
		}
		else
		{
			m = (n0 + n1) / den;
			if (m.length() > MITER_LIMIT)
			{
				m = m.normalized() * MITER_LIMIT;
			}
		}
		out.push_back(m);
	}
	return out;
}

// Extrude a cross-section [(u, h), ...] (left to right, metres) along the
// local stations; u0 shifts it sideways (rails), UV = (u, along), UV2 =
// (half width, count), colour alpha = kind / 255.
void SceneryDesignMesher::extrude(Buf &buf, const Vector<Vector3> &local, const Section &section, const Color &colour, int kind,
		double u0, double half_w, double count, const Vector<double> &halves)
{
	const int n = local.size();
	const int m = section.pts.size();
	if (n < 2)
	{
		return;
	}
	const bool scaled = halves.size() == n && half_w > 0.0;
	const int base = buf.verts.size();
	const Vector<Vector2> fr = frames(local);
	const Color col(colour.r, colour.g, colour.b, (double)kind / 255.0);
	std::vector<Vector2> sn;
	for (int j = 0; j < m; j++)
	{
		const int j0 = std::max(0, j - 1), j1 = std::min(m - 1, j + 1);
		const double du = section.pts[j1].x - section.pts[j0].x;
		const double dh = section.pts[j1].y - section.pts[j0].y;
		const double l = std::max(std::sqrt(du * du + dh * dh), 1e-6);
		sn.push_back(Vector2(-dh / l, du / l)); // (across, up)
	}
	double s = 0.0;
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
		{
			s += local[i].distance_to(local[i - 1]);
		}
		const Vector2 f = fr[i];
		const Vector3 p = local[i];
		const double hw = scaled ? halves[i] : half_w;
		const double k = scaled ? hw / half_w : 1.0;
		for (int j = 0; j < m; j++)
		{
			const double u = section.pts[j].x * k;
			const double h = section.pts[j].y;
			const double off = -(u0 + u); // + u = right of travel = -normal
			buf.push(Vector3(p.x + f.x * off, p.y + h, p.z + f.y * off),
					Vector3(-f.x * sn[j].x, sn[j].y, -f.y * sn[j].x).normalized(), col, Vector2(u, s), Vector2(hw, count));
		}
	}
	for (int i = 0; i + 1 < n; i++)
	{
		const int a = base + i * m;
		for (int j = 0; j + 1 < m; j++)
		{
			const int tri[6] = {a + j, a + m + j, a + j + 1, a + j + 1, a + m + j, a + m + j + 1};
			for (int ix : tri) buf.idx.push_back(ix);
		}
	}
}

} // namespace jsbgodot
