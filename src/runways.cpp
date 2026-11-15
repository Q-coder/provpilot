#include "runways.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/geometry2d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "geo.h"
#include "geo_frame.h"
#include "navaids_lib.h"
#include "runways_lib.h"

using namespace godot;

namespace jsbgodot
{

static const Color COL_WHITE(1.0, 0.97, 0.88);
static const Color COL_YELLOW(1.0, 0.75, 0.15);
static const Color COL_GREEN(0.15, 1.0, 0.35);
static const Color COL_RED(1.0, 0.12, 0.05);
static const Color COL_BLUE(0.25, 0.45, 1.0);
static const Color COL_OFF(0.0, 0.0, 0.0);

void Runways::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_merged", "merged", "terrain_height_geodetic"), &Runways::set_merged, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("update", "cam_pos", "delta"), &Runways::update);
	ClassDB::bind_method(D_METHOD("build", "merged", "terrain_height_geodetic"), &Runways::build, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("build_airport", "icao", "ap", "terrain_height_geodetic"), &Runways::build_airport);
	ClassDB::bind_method(D_METHOD("set_light_intensity", "v"), &Runways::set_light_intensity);
	ClassDB::bind_method(D_METHOD("get_material"), &Runways::get_material);
	ClassDB::bind_method(D_METHOD("get_light_material"), &Runways::get_light_material);
	ClassDB::bind_method(D_METHOD("built_count"), &Runways::built_count);
	ClassDB::bind_method(D_METHOD("get_airport_node", "icao"), &Runways::get_airport_node);
	ClassDB::bind_static_method("Runways", D_METHOD("papi_glide_deg", "icao", "end_ident"), &Runways::papi_glide_deg);
	ClassDB::bind_method(D_METHOD("surface_lift_at", "world_pos"), &Runways::surface_lift_at);
	ClassDB::bind_method(D_METHOD("get_runway_count"), &Runways::get_runway_count);
	ClassDB::bind_method(D_METHOD("get_model_count"), &Runways::get_model_count);
	ClassDB::bind_method(D_METHOD("get_taxiway_count"), &Runways::get_taxiway_count);
	ClassDB::bind_method(D_METHOD("get_apron_count"), &Runways::get_apron_count);
	ClassDB::bind_method(D_METHOD("get_light_count"), &Runways::get_light_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "runway_count"), "", "get_runway_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "model_count"), "", "get_model_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "taxiway_count"), "", "get_taxiway_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "apron_count"), "", "get_apron_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "light_count"), "", "get_light_count");
	ClassDB::bind_static_method("Runways", D_METHOD("get_surface_lift_m"), &Runways::get_surface_lift_m);
	ClassDB::bind_static_method("Runways", D_METHOD("get_taxi_lift_m"), &Runways::get_taxi_lift_m);
	ClassDB::bind_static_method("Runways", D_METHOD("get_apron_lift_m"), &Runways::get_apron_lift_m);
	BIND_ENUM_CONSTANT(KIND_ASPHALT);
	BIND_ENUM_CONSTANT(KIND_CONCRETE);
	BIND_ENUM_CONSTANT(KIND_GRASS);
	BIND_ENUM_CONSTANT(KIND_GRAVEL);
	BIND_ENUM_CONSTANT(KIND_TAXIWAY);
	BIND_ENUM_CONSTANT(KIND_APRON);
	BIND_ENUM_CONSTANT(KIND_TAXI_LINE);
}

Runways::Runways()
{
}

int Runways::get_runway_count() const { return runway_count; }
int Runways::get_model_count() const { return model_count; }
int Runways::get_taxiway_count() const { return taxiway_count; }
int Runways::get_apron_count() const { return apron_count; }
int Runways::get_light_count() const { return light_count; }
double Runways::get_surface_lift_m() { return SURFACE_LIFT_M; }
double Runways::get_taxi_lift_m() { return TAXI_LIFT_M; }
double Runways::get_apron_lift_m() { return APRON_LIFT_M; }

int Runways::Batch::add_vertex(const Vector3 &p, const Vector2 &uv, const float *c0, const float *c1)
{
	verts.push_back(p);
	normals.push_back(Vector3(0, 1, 0));
	uvs.push_back(uv);
	for (int i = 0; i < 4; i++)
	{
		custom0.push_back(c0[i]);
	}
	for (int i = 0; i < 4; i++)
	{
		custom1.push_back(c1[i]);
	}
	return verts.size() - 1;
}

void Runways::Lights::add(const Vector3 &p, const Color &a, const Color &b, const Vector2 &axis, int mode, double size, double papi_deg)
{
	verts.push_back(p);
	c0.push_back(a.r); c0.push_back(a.g); c0.push_back(a.b); c0.push_back((float)size);
	c1.push_back(b.r); c1.push_back(b.g); c1.push_back(b.b); c1.push_back((float)mode);
	c2.push_back(axis.x); c2.push_back(axis.y); c2.push_back((float)papi_deg); c2.push_back((float)mode);
}

Transform3D Runways::frame_at(double lat, double lon, double h)
{
	return Transform3D(geo::wun_basis(lat, lon), geo::ecef_of(lat, lon, h));
}

void Runways::ensure_material()
{
	if (shader.is_null())
	{
		shader = ResourceLoader::get_singleton()->load("res://shaders/runway.gdshader");
		light_shader = ResourceLoader::get_singleton()->load("res://shaders/airport_lights.gdshader");
	}
	if (material.is_null())
	{
		material.instantiate();
		material->set_shader(shader);
	}
	if (light_material.is_null())
	{
		light_material.instantiate();
		light_material->set_shader(light_shader);
		light_material->set_shader_parameter("intensity", 0.0);
	}
}

void Runways::set_light_intensity(double v)
{
	ensure_material();
	light_material->set_shader_parameter("intensity", Math::clamp(v, 0.0, 1.0));
}

Ref<ShaderMaterial> Runways::get_light_material()
{
	ensure_material();
	return light_material;
}

Ref<ShaderMaterial> Runways::get_material()
{
	ensure_material();
	return material;
}

void Runways::set_merged(const Dictionary &p_merged, const Callable &terrain_height_geodetic)
{
	ensure_material();
	for (const KeyValue<String, Node3D *> &kv : airport_nodes)
	{
		kv.value->queue_free();
	}
	airport_nodes.clear();
	airport_ecef.clear();
	lookup.clear();
	merged = p_merged;
	terrain_cb = terrain_height_geodetic;
	runway_count = model_count = taxiway_count = apron_count = light_count = 0;
	const Array keys = merged.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const Dictionary ap = merged[keys[i]];
		airport_ecef[keys[i]] = geo::ecef_of(ap.get("lat", 0.0), ap.get("lon", 0.0), ap.get("elevation_m", 0.0));
	}
	since_update = UPDATE_INTERVAL_S;
}

void Runways::update(const Vector3 &cam_pos, double delta)
{
	since_update += delta;
	if (since_update < UPDATE_INTERVAL_S || merged.is_empty())
	{
		return;
	}
	since_update = 0.0;
	int built = 0;
	const Array keys = merged.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		const String icao = keys[i];
		const double d = cam_pos.distance_to(airport_ecef[icao]);
		if (airport_nodes.has(icao))
		{
			if (d > FREE_RADIUS_M)
			{
				airport_nodes[icao]->queue_free();
				airport_nodes.erase(icao);
				lookup.erase(icao);
			}
		}
		else if (d < BUILD_RADIUS_M && built < BUILDS_PER_UPDATE)
		{
			build_airport(icao, merged[icao], terrain_cb);
			built++;
		}
	}
}

void Runways::build(const Dictionary &p_merged, const Callable &terrain_height_geodetic)
{
	set_merged(p_merged, terrain_height_geodetic);
	const Array keys = merged.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		build_airport(keys[i], merged[keys[i]], terrain_height_geodetic);
	}
	UtilityFunctions::print("Runways: ", runway_count, " procedural runways, ", taxiway_count, " taxiways, ", apron_count, " aprons, ",
			model_count, " models from ", merged.size(), " airports (1 material)");
}

void Runways::build_airport(const String &icao, const Dictionary &ap, const Callable &terrain_height_geodetic)
{
	if (airport_nodes.has(icao))
	{
		return;
	}
	ensure_material();
	Node3D *root = memnew(Node3D);
	root->set_name(icao);
	add_child(root);
	airport_nodes[icao] = root;
	const double lat = ap.get("lat", 0.0);
	const double lon = ap.get("lon", 0.0);
	const Transform3D frame = frame_at(lat, lon, ap.get("elevation_m", 0.0));
	if ((bool)ap.get("procedural", true))
	{
		Batch batch;
		const Transform3D inv = frame.affine_inverse();
		const Array rws = ap.get("runways", Array());
		for (int i = 0; i < rws.size(); i++)
		{
			if (rws[i].get_type() == Variant::DICTIONARY && add_runway(batch, inv, rws[i], terrain_height_geodetic))
			{
				runway_count++;
			}
		}
		add_aprons(batch, inv, ap.get("aprons", Array()));
		add_taxiway_mesh(batch, inv, ap.get("taxiway_mesh", Dictionary()));
		add_taxiways(batch, inv, ap.get("taxiways", Array()));
		MeshInstance3D *mi = mesh_instance(batch, frame);
		if (mi != nullptr)
		{
			root->add_child(mi);
			register_lookup(icao, batch, frame);
		}
		MeshInstance3D *lm = build_lights(icao, ap, frame);
		if (lm != nullptr)
		{
			root->add_child(lm);
		}
	}
	const String model = ap.get("model", "");
	if (!model.is_empty())
	{
		Node3D *node = load_model(String(ap.get("pack_path", "")).path_join(model));
		if (node != nullptr)
		{
			double h = ap.get("elevation_m", 0.0);
			if (terrain_height_geodetic.is_valid())
			{
				const double th = terrain_height_geodetic.call(lat, lon);
				if (std::isfinite(th) && th > -1000.0)
				{
					h = th;
				}
			}
			node->set_transform(frame_at(lat, lon, h));
			root->add_child(node);
			model_count++;
		}
	}
}

int Runways::built_count() const
{
	return airport_nodes.size();
}

Node3D *Runways::get_airport_node(const String &icao) const
{
	return airport_nodes.has(icao) ? airport_nodes[icao] : nullptr;
}

int Runways::surface_kind(const String &surface)
{
	if (surface == "asphalt") return KIND_ASPHALT;
	if (surface == "concrete") return KIND_CONCRETE;
	if (surface == "grass") return KIND_GRASS;
	return KIND_GRAVEL;
}

int Runways::bars_for_width(double w)
{
	if (w < 20.0) return 4;
	if (w < 27.0) return 6;
	if (w < 38.0) return 8;
	if (w < 52.0) return 12;
	return 16;
}

// three codes in -1..12 -> base-14 digits (code+1), exact in a float
double Runways::pack_glyphs(const PackedInt32Array &g)
{
	return (double)((g[0] + 1) * 196 + (g[1] + 1) * 14 + (g[2] + 1));
}

// Runway plane heights (h0, h1) from the flatten stage, the OurAirports
// ends, or the terrain sampler.
Vector2 Runways::end_heights(const Dictionary &rw, const Vector2 &p0, const Vector2 &p1, const Callable &terrain)
{
	const Array ends = rw.get("ends", Array());
	double h0 = NAN, h1 = NAN;
	if (ends.size() == 2)
	{
		const Dictionary e0 = ends[0], e1 = ends[1];
		for (const char *key : {"plane_elev_m", "elev_m"})
		{
			if (std::isnan(h0) && e0.has(key)) h0 = e0[key];
			if (std::isnan(h1) && e1.has(key)) h1 = e1[key];
		}
	}
	if ((std::isnan(h0) || std::isnan(h1)) && terrain.is_valid())
	{
		const Vector2 ll0 = Geo::lv95_to_wgs84(p0.x, p0.y);
		const Vector2 ll1 = Geo::lv95_to_wgs84(p1.x, p1.y);
		const double t0 = terrain.call(ll0.x, ll0.y);
		const double t1 = terrain.call(ll1.x, ll1.y);
		if (std::isnan(h0) && std::isfinite(t0)) h0 = t0;
		if (std::isnan(h1) && std::isfinite(t1)) h1 = t1;
	}
	if (std::isnan(h0) && std::isnan(h1))
	{
		h0 = h1 = 0.0;
	}
	else if (std::isnan(h0))
	{
		h0 = h1;
	}
	else if (std::isnan(h1))
	{
		h1 = h0;
	}
	return Vector2(h0, h1);
}

static Vector2 pavement_lv95(const Dictionary &end)
{
	const Array ll = end["pavement"];
	return Geo::wgs84_to_lv95(ll[0], ll[1]);
}

bool Runways::add_runway(Batch &batch, const Transform3D &inv, const Dictionary &rw, const Callable &terrain)
{
	const Array ends = rw.get("ends", Array());
	const Array desigs = rw.get("designators", Array());
	if (ends.size() != 2 || !Dictionary(ends[0]).has("pavement") || !Dictionary(ends[1]).has("pavement"))
	{
		return false;
	}
	const Dictionary e0 = ends[0], e1 = ends[1];
	const Vector2 p0 = pavement_lv95(e0);
	const Vector2 p1 = pavement_lv95(e1);
	const Vector2 axis = p1 - p0;
	const double length = axis.length();
	if (length < 30.0)
	{
		return false;
	}
	const Vector2 u = axis / length;
	const Vector2 r(u.y, -u.x); // right of the along direction
	const double half_w = 0.5 * (double)rw.get("width_m", 45.0);
	const Vector2 h = end_heights(rw, p0, p1, terrain);
	const PackedInt32Array g0 = RunwaysLib::glyph_codes(desigs.size() > 0 ? String(desigs[0]) : String());
	const PackedInt32Array g1 = RunwaysLib::glyph_codes(desigs.size() > 1 ? String(desigs[1]) : String());
	// "other" is a catalog row without a surface word: a lighted or long
	// runway is paved (Aalborg 08L/26R, 2647 m), the rest stays unmarked
	String surface = rw.get("surface", "asphalt");
	if (surface == "other" && ((bool)rw.get("lighted", false) || length >= 1200.0))
	{
		surface = "asphalt";
	}
	const int kind = surface_kind(surface);
	const int bars = bars_for_width(2.0 * half_w);
	const int aiming = length >= 1800.0 ? 1 : 0;
	const float c0[4] = {(float)half_w, (float)length, (float)(double)e0.get("displaced_m", 0.0), (float)(double)e1.get("displaced_m", 0.0)};
	float c1[4] = {(float)pack_glyphs(g0), (float)pack_glyphs(g1), (float)(kind * 64 + bars * 2 + aiming), 0.0f};

	// Vertex rows every SEGMENT_M plus a duplicated row at each crossing
	// boundary so the marking cut is crisp; rows inside a crossing carry
	// flag 1 in CUSTOM1.w and the shader draws no markings there.
	const Array crossings = rw.get("crossings", Array());
	std::vector<std::pair<double, double>> cross;
	std::vector<double> cuts;
	for (int i = 0; i < crossings.size(); i++)
	{
		const Array cr = crossings[i];
		if (cr.size() >= 2)
		{
			cross.push_back({(double)cr[0], (double)cr[1]});
			cuts.push_back((double)cr[0]);
			cuts.push_back((double)cr[1]);
		}
	}
	std::sort(cuts.begin(), cuts.end());
	const int segs = std::max(1, (int)std::ceil(length / SEGMENT_M));
	std::vector<double> alongs;
	for (int k = 0; k <= segs; k++)
	{
		alongs.push_back((double)k / (double)segs * length);
	}
	for (double c : cuts)
	{
		if (c > 0.0 && c < length)
		{
			alongs.push_back(c);
		}
	}
	std::sort(alongs.begin(), alongs.end());
	std::vector<std::pair<double, double>> rows; // (along, flag)
	for (double along : alongs)
	{
		double inside = 0.0;
		for (const auto &cr : cross)
		{
			if (along > cr.first + 0.01 && along < cr.second - 0.01)
			{
				inside = 1.0;
			}
		}
		bool is_cut = false;
		for (double c : cuts)
		{
			if (std::abs(c - along) < 0.005)
			{
				is_cut = true;
			}
		}
		if (is_cut)
		{
			// before the boundary: state of the previous row; after: new state
			const double prev = rows.empty() ? 0.0 : rows.back().second;
			double nxt = 0.0;
			for (const auto &cr : cross)
			{
				if (along >= cr.first - 0.01 && along < cr.second - 0.01)
				{
					nxt = 1.0;
				}
			}
			rows.push_back({along, prev});
			rows.push_back({along, nxt});
		}
		else
		{
			rows.push_back({along, inside});
		}
	}
	const int first = batch.verts.size();
	for (const auto &row : rows)
	{
		const double along = row.first;
		const double t = along / length;
		const double z = h.x + (h.y - h.x) * t + SURFACE_LIFT_M;
		c1[3] = (float)row.second;
		for (double side : {-1.0, 1.0})
		{
			const Vector2 g = p0 + u * along + r * (side * half_w);
			const Vector2 ll = Geo::lv95_to_wgs84(g.x, g.y);
			batch.add_vertex(inv.xform(geo::ecef_of(ll.x, ll.y, z)), Vector2(side * half_w, along), c0, c1);
		}
	}
	for (int k = 0; k + 1 < (int)rows.size(); k++)
	{
		const int a = first + k * 2;
		const int tri[6] = {a, a + 2, a + 1, a + 1, a + 2, a + 3};
		for (int t : tri) batch.idx.push_back(t);
	}
	PackedStringArray names;
	for (int i = 0; i < desigs.size(); i++)
	{
		names.push_back(String(desigs[i]));
	}
	Dictionary rec;
	rec["name"] = String("/").join(names);
	rec["first"] = first;
	rec["count"] = (int)rows.size() * 2;
	rec["h0"] = h.x;
	rec["h1"] = h.y;
	rec["length"] = length;
	batch.runways.push_back(rec);
	const Vector3 va = batch.verts[first], vb = batch.verts[first + 1];
	const Vector3 vc = batch.verts[batch.verts.size() - 2], vd = batch.verts[batch.verts.size() - 1];
	const Vector2 p0l((va.x + vb.x) * 0.5, (va.z + vb.z) * 0.5);
	const Vector2 p1l((vc.x + vd.x) * 0.5, (vc.z + vd.z) * 0.5);
	Batch::Rect rect;
	rect.p0 = p0l;
	rect.u = (p1l - p0l).normalized();
	rect.length = p0l.distance_to(p1l);
	rect.half_w = half_w;
	batch.rwy_rects.push_back(rect);
	return true;
}

static Vector3 rec_local(const Transform3D &inv, const Array &rec, double lift)
{
	return inv.xform(geo::ecef_of(rec[0], rec[1], (double)rec[2] + lift));
}

void Runways::add_aprons(Batch &batch, const Transform3D &inv, const Array &aprons)
{
	const float c0[4] = {0, 0, 0, 0};
	const float c1[4] = {0, 0, (float)(KIND_APRON * 64), 0};
	for (int a = 0; a < aprons.size(); a++)
	{
		const Dictionary apn = aprons[a];
		const Array poly = apn.get("polygon", Array());
		if (poly.size() < 4 || Array(poly[0]).size() < 3)
		{
			continue;
		}
		PackedVector2Array pts2;
		const int base = batch.verts.size();
		const int n = (poly[0] == poly[poly.size() - 1]) ? poly.size() - 1 : poly.size();
		for (int i = 0; i < n; i++)
		{
			const Vector3 p = rec_local(inv, poly[i], APRON_LIFT_M);
			batch.add_vertex(p, Vector2(p.x, p.z), c0, c1);
			pts2.push_back(Vector2(p.x, p.z));
		}
		batch.apron_polys.push_back(pts2);
		PackedInt32Array tri = Geometry2D::get_singleton()->triangulate_polygon(pts2);
		if (tri.is_empty())
		{
			for (int i = 1; i + 1 < n; i++)
			{
				tri.push_back(0);
				tri.push_back(i);
				tri.push_back(i + 1);
			}
		}
		for (int k = 0; k + 2 < tri.size(); k += 3)
		{
			const int ia = base + tri[k], ib = base + tri[k + 1], ic = base + tri[k + 2];
			if (ia >= batch.verts.size() || ib >= batch.verts.size() || ic >= batch.verts.size())
			{
				continue;
			}
			const Vector3 va = batch.verts[ia], vb = batch.verts[ib], vc = batch.verts[ic];
			if ((vb - va).cross(vc - va).y >= 0.0)
			{
				batch.idx.push_back(ia); batch.idx.push_back(ic); batch.idx.push_back(ib);
			}
			else
			{
				batch.idx.push_back(ia); batch.idx.push_back(ib); batch.idx.push_back(ic);
			}
		}
		apron_count++;
	}
}

// Guidance lines only: the pavement comes from add_taxiway_mesh (the bake
// unions all ribbons of an airport). One 0.5 m strip per path, above every
// pavement so it reads through crossings and across runways.
void Runways::add_taxiways(Batch &batch, const Transform3D &inv, const Array &taxiways)
{
	const float c0[4] = {0, 0, 0, 0};
	const float c1l[4] = {0, 0, (float)(KIND_TAXI_LINE * 64), 0};
	for (int t = 0; t < taxiways.size(); t++)
	{
		const Dictionary tw = taxiways[t];
		const Array path = tw.get("path", Array());
		if (path.size() < 2 || Array(path[0]).size() < 3)
		{
			continue;
		}
		std::vector<Vector3> pts;
		for (int i = 0; i < path.size(); i++)
		{
			pts.push_back(rec_local(inv, path[i], LINE_LIFT_M));
		}
		for (int i = 0; i + 1 < (int)pts.size(); i++)
		{
			const Vector3 d = pts[i + 1] - pts[i];
			const Vector3 flat(d.x, 0.0, d.z);
			const double seg_len = flat.length();
			if (seg_len < 0.05)
			{
				continue;
			}
			const Vector3 dir = flat / seg_len;
			const Vector3 right = Vector3(-dir.z, 0.0, dir.x) * LINE_HALF_W;
			const int a = batch.add_vertex(pts[i] - right, Vector2(-LINE_HALF_W, 0.0), c0, c1l);
			batch.add_vertex(pts[i] + right, Vector2(LINE_HALF_W, 0.0), c0, c1l);
			batch.add_vertex(pts[i + 1] - right, Vector2(-LINE_HALF_W, seg_len), c0, c1l);
			batch.add_vertex(pts[i + 1] + right, Vector2(LINE_HALF_W, seg_len), c0, c1l);
			const int tri[6] = {a, a + 2, a + 1, a + 1, a + 2, a + 3};
			for (int k : tri) batch.idx.push_back(k);
			if (i + 1 < (int)pts.size() - 1)
			{
				add_disc(batch, pts[i + 1], LINE_HALF_W, 0.0, c0, c1l);
			}
		}
		taxiway_count++;
	}
}

// The airport's unioned taxiway pavement: pre-triangulated by the bake
// (vertices [lat, lon, elev], triangle index list).
void Runways::add_taxiway_mesh(Batch &batch, const Transform3D &inv, const Dictionary &mesh)
{
	const Array vs = mesh.get("vertices", Array());
	const Array tris = mesh.get("triangles", Array());
	if (vs.is_empty() || tris.size() < 3)
	{
		return;
	}
	const float c0[4] = {0, 0, 0, 0};
	const float c1[4] = {0, 0, (float)(KIND_TAXIWAY * 64), 0};
	const int base = batch.verts.size();
	for (int i = 0; i < vs.size(); i++)
	{
		const Vector3 p = rec_local(inv, vs[i], TAXI_LIFT_M);
		batch.add_vertex(p, Vector2(p.x, p.z), c0, c1);
	}
	const int n = vs.size();
	for (int k = 0; k + 2 < tris.size(); k += 3)
	{
		const int t0 = tris[k], t1 = tris[k + 1], t2 = tris[k + 2];
		if (t0 >= n || t1 >= n || t2 >= n)
		{
			continue;
		}
		const int ia = base + t0, ib = base + t1, ic = base + t2;
		const Vector3 va = batch.verts[ia], vb = batch.verts[ib], vc = batch.verts[ic];
		batch.taxi_tri_first.push_back(batch.idx.size());
		if ((vb - va).cross(vc - va).y >= 0.0)
		{
			batch.idx.push_back(ia); batch.idx.push_back(ic); batch.idx.push_back(ib);
		}
		else
		{
			batch.idx.push_back(ia); batch.idx.push_back(ib); batch.idx.push_back(ic);
		}
	}
}

int64_t Runways::cell_key(int cx, int cz)
{
	return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cz;
}

double Runways::surface_lift_at(const Vector3 &world_pos) const
{
	double best = 0.0;
	Geometry2D *g2 = Geometry2D::get_singleton();
	for (const KeyValue<String, Lookup> &kv : lookup)
	{
		if (world_pos.distance_to(airport_ecef[kv.key]) > LOOKUP_RANGE_M)
		{
			continue;
		}
		const Lookup &lk = kv.value;
		const Vector3 local = lk.inv.xform(world_pos);
		const Vector2 p(local.x, local.z);
		for (int i = 0; i < lk.runways.size(); i++)
		{
			const Batch::Rect &rr = lk.runways[i];
			const Vector2 d = p - rr.p0;
			const double along = d.dot(rr.u);
			const double lateral = std::abs(d.cross(rr.u));
			if (along >= 0.0 && along <= rr.length && lateral <= rr.half_w)
			{
				best = std::max(best, SURFACE_LIFT_M);
			}
		}
		const int64_t cell = cell_key((int)std::floor(p.x / LOOKUP_CELL_M), (int)std::floor(p.y / LOOKUP_CELL_M));
		if (lk.grid.has(cell))
		{
			const Vector<int> &tris = lk.grid[cell];
			for (int i = 0; i < tris.size(); i++)
			{
				const int k = tris[i];
				const Vector3 a = lk.verts[lk.idx[k]], b = lk.verts[lk.idx[k + 1]], c = lk.verts[lk.idx[k + 2]];
				if (g2->point_is_inside_triangle(p, Vector2(a.x, a.z), Vector2(b.x, b.z), Vector2(c.x, c.z)))
				{
					best = std::max(best, TAXI_LIFT_M);
					break;
				}
			}
		}
		for (int i = 0; i < lk.aprons.size(); i++)
		{
			if (g2->is_point_in_polygon(p, lk.aprons[i]))
			{
				best = std::max(best, APRON_LIFT_M);
				break;
			}
		}
	}
	return best;
}

void Runways::register_lookup(const String &icao, const Batch &batch, const Transform3D &frame)
{
	Lookup lk;
	for (int i = 0; i < batch.taxi_tri_first.size(); i++)
	{
		const int k = batch.taxi_tri_first[i];
		const Vector3 a = batch.verts[batch.idx[k]], b = batch.verts[batch.idx[k + 1]], c = batch.verts[batch.idx[k + 2]];
		const double minx = std::min(a.x, std::min(b.x, c.x)), maxx = std::max(a.x, std::max(b.x, c.x));
		const double minz = std::min(a.z, std::min(b.z, c.z)), maxz = std::max(a.z, std::max(b.z, c.z));
		for (int cx = (int)std::floor(minx / LOOKUP_CELL_M); cx <= (int)std::floor(maxx / LOOKUP_CELL_M); cx++)
		{
			for (int cz = (int)std::floor(minz / LOOKUP_CELL_M); cz <= (int)std::floor(maxz / LOOKUP_CELL_M); cz++)
			{
				lk.grid[cell_key(cx, cz)].push_back(k);
			}
		}
	}
	lk.inv = frame.affine_inverse();
	lk.runways = batch.rwy_rects;
	lk.aprons = batch.apron_polys;
	lk.verts = batch.verts;
	lk.idx = batch.idx;
	lookup[icao] = lk;
}

void Runways::add_disc(Batch &batch, const Vector3 &centre, double radius, double along, const float *c0, const float *c1)
{
	// UV.x is kept off the ±0.15 m centreline band (the disc carries no
	// yellow line; the segment quads draw it through the node).
	const int n = 12;
	const int c = batch.add_vertex(centre, Vector2(0.5 * radius, along), c0, c1);
	for (int k = 0; k < n; k++)
	{
		const double ang = Math_TAU * (double)k / (double)n;
		batch.add_vertex(centre + Vector3(std::cos(ang) * radius, 0.0, std::sin(ang) * radius), Vector2(radius, along + std::sin(ang) * radius), c0, c1);
	}
	for (int k = 0; k < n; k++)
	{
		const int a = c + 1 + k;
		const int b = c + 1 + (k + 1) % n;
		// front-facing order (clockwise from +Y): c, a, b
		batch.idx.push_back(c); batch.idx.push_back(a); batch.idx.push_back(b);
	}
}

static int64_t custom_flags(int n)
{
	int64_t flags = 0;
	flags |= (int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT;
	flags |= (int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT;
	if (n > 2)
	{
		flags |= (int64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT;
	}
	return flags;
}

MeshInstance3D *Runways::mesh_instance(Batch &batch, const Transform3D &frame)
{
	if (batch.idx.is_empty())
	{
		return nullptr;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = batch.verts;
	arrays[Mesh::ARRAY_NORMAL] = batch.normals;
	arrays[Mesh::ARRAY_TEX_UV] = batch.uvs;
	arrays[Mesh::ARRAY_CUSTOM0] = batch.custom0;
	arrays[Mesh::ARRAY_CUSTOM1] = batch.custom1;
	arrays[Mesh::ARRAY_INDEX] = batch.idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), custom_flags(2));
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name("Surfaces");
	mi->set_mesh(mesh);
	mi->set_material_override(material);
	mi->set_transform(frame);
	mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mi->set_visibility_range_end(VISIBLE_RANGE_M);
	mi->set_meta("runways", batch.runways);
	return mi;
}

// ── Lights ──────────────────────────────────────────────────────────────────

Vector3 Runways::lv95_local(const Transform3D &inv, const Vector2 &p, double h)
{
	const Vector2 ll = Geo::lv95_to_wgs84(p.x, p.y);
	return inv.xform(geo::ecef_of(ll.x, ll.y, h + LIGHT_LIFT_M));
}

Vector3 Runways::ll_local(const Transform3D &inv, double lat, double lon, double h)
{
	return inv.xform(geo::ecef_of(lat, lon, h + LIGHT_LIFT_M));
}

bool Runways::rwy_geo(const Dictionary &rw, const Transform3D &inv, RwyGeo &g)
{
	const Array ends = rw.get("ends", Array());
	if (ends.size() != 2 || !Dictionary(ends[0]).has("pavement") || !Dictionary(ends[1]).has("pavement"))
	{
		return false;
	}
	const Dictionary e0 = ends[0], e1 = ends[1];
	g.p0 = pavement_lv95(e0);
	const Vector2 p1 = pavement_lv95(e1);
	g.length = g.p0.distance_to(p1);
	if (g.length < 30.0)
	{
		return false;
	}
	g.u = (p1 - g.p0) / g.length;
	g.r = Vector2(g.u.y, -g.u.x);
	g.half_w = 0.5 * (double)rw.get("width_m", 45.0);
	const Vector2 h = end_heights(rw, g.p0, p1, Callable());
	g.h0 = h.x;
	g.h1 = h.y;
	g.disp0 = e0.get("displaced_m", 0.0);
	g.disp1 = e1.get("displaced_m", 0.0);
	g.lit = rw.get("lighted", false);
	const Vector3 a = lv95_local(inv, g.p0, g.h0);
	const Vector3 b = lv95_local(inv, p1, g.h1);
	g.axis_local = Vector2(b.x - a.x, b.z - a.z).normalized();
	return true;
}

// Fixtures of one lit runway: edges, thresholds / ends, centreline (long
// runways), PAPI, approach lights (long runways). Colour A is seen from
// the +axis side (looking back toward end0), B from the -axis side.
void Runways::gen_runway_lights(Lights &L, const RwyGeo &g, bool intl, const Dictionary &classes, const Transform3D &inv)
{
	const Vector2 ax = g.axis_local;
	// Edges: white, yellow in the last 600 m toward the end you are moving to.
	if (!classes.has("rwe"))
	{
		for (double t = 0.0; t <= g.length + 0.1; t += EDGE_SPACING_M)
		{
			const Color a_col = t < EDGE_CAUTION_M ? COL_YELLOW : COL_WHITE;
			const Color b_col = g.length - t < EDGE_CAUTION_M ? COL_YELLOW : COL_WHITE;
			for (double side : {-1.0, 1.0})
			{
				L.add(lv95_local(inv, g.p0 + g.u * t + g.r * (side * (g.half_w + 1.0)), g.z_at(t)), a_col, b_col, ax, 0, 1.0);
			}
		}
	}
	// Thresholds (green from the approach, red from the runway) and ends (red).
	if (!classes.has("rwt"))
	{
		for (int end_i = 0; end_i < 2; end_i++)
		{
			const double t_thr = end_i == 0 ? g.disp0 : g.length - g.disp1;
			const double t_end = end_i == 0 ? 0.0 : g.length;
			for (double lat = -g.half_w + 3.0; lat <= g.half_w - 3.0 + 0.1; lat += THRESHOLD_SPACING_M)
			{
				const Color a_col = end_i == 0 ? COL_RED : COL_GREEN;
				const Color b_col = end_i == 0 ? COL_GREEN : COL_RED;
				L.add(lv95_local(inv, g.p0 + g.u * t_thr + g.r * lat, g.z_at(t_thr)), a_col, b_col, ax, 0, 1.2);
				if (std::abs(t_thr - t_end) > 1.0)
				{
					const Color ea = end_i == 0 ? COL_RED : COL_OFF;
					const Color eb = end_i == 0 ? COL_OFF : COL_RED;
					L.add(lv95_local(inv, g.p0 + g.u * t_end + g.r * lat, g.z_at(t_end)), ea, eb, ax, 0, 1.0);
				}
			}
		}
	}
	// Centreline: white, red/white in the last 900 m, red in the last 300 m.
	if (g.length >= CENTRE_MIN_LENGTH_M && !classes.has("rwc"))
	{
		int k = 0;
		for (double t = 15.0; t < g.length - 15.0; t += CENTRE_SPACING_M, k++)
		{
			const double rem0 = t, rem1 = g.length - t;
			Color a_col = COL_WHITE, b_col = COL_WHITE;
			if (rem0 < 300.0) a_col = COL_RED;
			else if (rem0 < 900.0) a_col = k % 2 == 0 ? COL_RED : COL_WHITE;
			if (rem1 < 300.0) b_col = COL_RED;
			else if (rem1 < 900.0) b_col = k % 2 == 0 ? COL_RED : COL_WHITE;
			L.add(lv95_local(inv, g.p0 + g.u * t + g.r * 0.6, g.z_at(t)), a_col, b_col, ax, 0, 0.8);
		}
	}
	// PAPI: four units on the pilot's left, 300 m past the threshold, set
	// 30' and 10' either side of the end's approach slope (ICAO Annex 14:
	// the ILS glide path where the runway has one, else 3 deg): for 3 deg
	// 2.5 / 2.83 / 3.17 / 3.5 from the outermost unit inward, for LSZH 28's
	// 3.3 deg path 2.8 .. 3.8 (gery 2026-11-08: "on Rwy 28 the lights are
	// always white").
	if (!classes.has("papi"))
	{
		for (int end_i = 0; end_i < 2; end_i++)
		{
			const double t_thr = end_i == 0 ? g.disp0 : g.length - g.disp1;
			const double t_p = end_i == 0 ? t_thr + PAPI_DISTANCE_M : t_thr - PAPI_DISTANCE_M;
			const Vector2 left = end_i == 0 ? -g.r : g.r;
			const Vector2 axis_i = end_i == 0 ? ax : -ax;
			for (int unit = 0; unit < 4; unit++)
			{
				const double angle = g.glide_deg[end_i] + 0.5 - unit * (1.0 / 3.0);
				const double lat_off = g.half_w + PAPI_OFFSET_M + unit * PAPI_UNIT_SPACING_M;
				L.add(lv95_local(inv, g.p0 + g.u * t_p + left * lat_off, g.z_at(t_p)), COL_WHITE, COL_WHITE, axis_i, 2, PAPI_SIZE, angle);
			}
		}
	}
	// Approach lights on both ends of long lit runways.
	if (g.length >= CENTRE_MIN_LENGTH_M && !classes.has("als"))
	{
		const double total = intl ? APPROACH_LONG_M : APPROACH_SHORT_M;
		for (int end_i = 0; end_i < 2; end_i++)
		{
			const Vector2 axis_i = end_i == 0 ? ax : -ax; // directed toward -axis_i (the approach)
			for (double d = APPROACH_SPACING_M; d <= total + 0.1; d += APPROACH_SPACING_M)
			{
				const double t = end_i == 0 ? -d : g.length + d;
				const double z = end_i == 0 ? g.z_at(0.0) : g.z_at(g.length);
				L.add(lv95_local(inv, g.p0 + g.u * t, z), COL_WHITE, COL_WHITE, axis_i, 1, 1.2);
				// crossbars (Calvert-style: wider farther out) at 300 m, every 150 m on the long system
				double bar_half = 0.0;
				if (std::abs(d - 300.0) < 1.0)
				{
					bar_half = 15.0;
				}
				else if (intl && (int)std::lround(d) % 150 == 0)
				{
					bar_half = 10.0 + 5.0 * (d / 150.0 - 1.0);
				}
				if (bar_half > 0.0)
				{
					for (double lat = -bar_half; lat <= bar_half + 0.1; lat += 3.0)
					{
						if (std::abs(lat) > 1.0)
						{
							L.add(lv95_local(inv, g.p0 + g.u * t + g.r * lat, z), COL_WHITE, COL_WHITE, axis_i, 1, 1.0);
						}
					}
				}
			}
		}
	}
}

const Runways::RwyGeo *Runways::nearest_geo(const Vector2 &q, const Vector<RwyGeo> &geos)
{
	const RwyGeo *best = nullptr;
	double best_d = std::numeric_limits<double>::infinity();
	for (int i = 0; i < geos.size(); i++)
	{
		const RwyGeo &rg = geos[i];
		const double t = Math::clamp((double)(q - rg.p0).dot(rg.u), 0.0, rg.length);
		const double d = (q - (rg.p0 + rg.u * t)).length();
		if (d < best_d)
		{
			best_d = d;
			best = &rg;
		}
	}
	return best;
}

// Taxiway centreline (green) and edge (blue) lights from the paths; nodes
// flagged on a runway are skipped.
void Runways::gen_taxiway_lights(Lights &L, const Array &taxiways, const Transform3D &inv, const Dictionary &classes)
{
	const bool want_centre = !classes.has("txc");
	const bool want_edge = !classes.has("txe");
	if (!want_centre && !want_edge)
	{
		return;
	}
	for (int t = 0; t < taxiways.size(); t++)
	{
		const Dictionary tw = taxiways[t];
		const Array path = tw.get("path", Array());
		if (path.size() < 2 || Array(path[0]).size() < 3)
		{
			continue;
		}
		const double half_w = 0.5 * (double)tw.get("width_m", 23.0);
		std::vector<Vector3> pts;
		std::vector<double> flags;
		for (int i = 0; i < path.size(); i++)
		{
			const Array rec = path[i];
			pts.push_back(ll_local(inv, rec[0], rec[1], rec[2]));
			flags.push_back(rec.size() > 3 ? (double)rec[3] : 0.0);
		}
		double carry_c = 0.0, carry_e = 0.0;
		for (int i = 0; i + 1 < (int)pts.size(); i++)
		{
			if (flags[i] > 0.5 && flags[i + 1] > 0.5)
			{
				continue;
			}
			const Vector3 d = pts[i + 1] - pts[i];
			const double seg = Vector2(d.x, d.z).length();
			if (seg < 0.1)
			{
				continue;
			}
			const Vector2 dir = Vector2(d.x, d.z) / seg;
			const Vector2 right(-dir.y, dir.x);
			double s = carry_c;
			while (want_centre && s < seg)
			{
				L.add(pts[i] + d * (s / seg), COL_GREEN, COL_GREEN, dir, 0, 0.7);
				s += TAXI_CENTRE_SPACING_M;
			}
			carry_c = s - seg;
			s = carry_e;
			while (want_edge && s < seg)
			{
				const Vector3 base = pts[i] + d * (s / seg);
				for (double side : {-1.0, 1.0})
				{
					const Vector2 off = right * (side * (half_w + 1.5));
					L.add(base + Vector3(off.x, 0.0, off.y), COL_BLUE, COL_BLUE, dir, 0, 0.7);
				}
				s += TAXI_EDGE_SPACING_M;
			}
			carry_e = s - seg;
		}
	}
}

// Individually mapped OSM fixtures (bake: airport "lights"), replacing the
// generated class when at least OSM_CLASS_MIN of that class exist.
void Runways::add_osm_lights(Lights &L, const Dictionary &ap, const Transform3D &inv, const Dictionary &classes, const Vector<RwyGeo> &geos)
{
	const Array lights = ap.get("lights", Array());
	for (int i = 0; i < lights.size(); i++)
	{
		const Dictionary lt = lights[i];
		const String kind = lt.get("kind", "");
		if (!classes.has(kind))
		{
			continue;
		}
		const double lat = lt.get("lat", 0.0);
		const double lon = lt.get("lon", 0.0);
		const double h = lt.get("elev_m", ap.get("elevation_m", 0.0));
		const Vector3 p = ll_local(inv, lat, lon, h);
		// axis: the mapped direction (degrees true, toward the beam), else the
		// nearest runway's axis; an approach light without a direction beams
		// away from the runway end it belongs to.
		Vector2 axis(0.0, 1.0);
		if (lt.has("dir"))
		{
			const double dd = Math::deg_to_rad((double)lt["dir"]);
			axis = Vector2(-std::sin(dd), std::cos(dd)); // frame x = west: east = -x
		}
		else
		{
			const Vector2 q = Geo::wgs84_to_lv95(lat, lon);
			const RwyGeo *best = nearest_geo(q, geos);
			if (best != nullptr)
			{
				axis = best->axis_local;
				if (kind == "als")
				{
					const double t = (q - best->p0).dot(best->u);
					axis = t < 0.5 * best->length ? -best->axis_local : best->axis_local;
				}
			}
		}
		const String colour_tag = lt.get("colour", "");
		if (kind == "rwe")
		{
			L.add(p, COL_WHITE, COL_WHITE, axis, 0, 1.0);
		}
		else if (kind == "rwc" || kind == "tdz")
		{
			L.add(p, COL_WHITE, COL_WHITE, axis, 0, 0.8);
		}
		else if (kind == "rwt")
		{
			const Color c = colour_tag == "red" ? COL_RED : COL_GREEN;
			L.add(p, c, c, axis, 0, 1.2);
		}
		else if (kind == "txc")
		{
			L.add(p, COL_GREEN, COL_GREEN, axis, 0, 0.7);
		}
		else if (kind == "txe")
		{
			L.add(p, COL_BLUE, COL_BLUE, axis, 0, 0.7);
		}
		else if (kind == "als")
		{
			// beam toward `dir`: the shader's directed mode lights the -axis side
			L.add(p, COL_WHITE, COL_WHITE, -axis, 1, 1.2);
		}
		// papi / vasi: OSM maps a PAPI as one node per system and never the
		// angles, so mapped PAPIs never replace the generated ones.
	}
}

double Runways::papi_glide_deg(const String &icao, const String &end_ident)
{
	if (icao.is_empty() || end_ident.is_empty())
	{
		return 3.0;
	}
	for (const Ils &s : NavaidsLib::all_ils())
	{
		if (s.icao == icao && s.rwy == end_ident && s.gs_deg >= 1.0 && s.gs_deg <= 6.0)
		{
			return s.gs_deg;
		}
	}
	return 3.0;
}

MeshInstance3D *Runways::build_lights(const String &icao, const Dictionary &ap, const Transform3D &frame)
{
	const Transform3D inv = frame.affine_inverse();
	Lights L;
	// classes with enough mapped fixtures replace the generated ones
	Dictionary counts;
	const Array lights = ap.get("lights", Array());
	for (int i = 0; i < lights.size(); i++)
	{
		const String k = Dictionary(lights[i]).get("kind", "");
		counts[k] = (int)counts.get(k, 0) + 1;
	}
	Dictionary classes;
	const Array ckeys = counts.keys();
	for (int i = 0; i < ckeys.size(); i++)
	{
		const String k = ckeys[i];
		if ((int)counts[k] >= OSM_CLASS_MIN && k != "papi" && k != "vasi")
		{
			classes[k] = true;
		}
	}
	const bool intl = String(ap.get("type", "")) == "international_airport";
	Vector<RwyGeo> geos;
	const Array rws = ap.get("runways", Array());
	for (int i = 0; i < rws.size(); i++)
	{
		if (rws[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		RwyGeo g;
		if (!rwy_geo(rws[i], inv, g))
		{
			continue;
		}
		const Dictionary rw = rws[i];
		const Array ends = rw.get("ends", Array());
		const Array des = rw.get("designators", Array());
		for (int e = 0; e < 2; e++)
		{
			String ident = e < ends.size() ? String(Dictionary(ends[e]).get("ident", "")) : String();
			if (ident.is_empty() && e < des.size())
			{
				ident = String(des[e]);
			}
			g.glide_deg[e] = papi_glide_deg(icao, ident);
		}
		geos.push_back(g);
		if (g.lit)
		{
			gen_runway_lights(L, g, intl, classes, inv);
		}
	}
	gen_taxiway_lights(L, ap.get("taxiways", Array()), inv, classes);
	add_osm_lights(L, ap, inv, classes, geos);
	if (L.size() == 0)
	{
		return nullptr;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = L.verts;
	arrays[Mesh::ARRAY_CUSTOM0] = L.c0;
	arrays[Mesh::ARRAY_CUSTOM1] = L.c1;
	arrays[Mesh::ARRAY_CUSTOM2] = L.c2;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_POINTS, arrays, TypedArray<Array>(), Dictionary(), custom_flags(3));
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name("Lights");
	mi->set_mesh(mesh);
	mi->set_material_override(light_material);
	mi->set_transform(frame);
	mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mi->set_visibility_range_end(VISIBLE_RANGE_M);
	light_count += L.size();
	return mi;
}

Node3D *Runways::load_model(const String &path)
{
	if (!FileAccess::file_exists(path))
	{
		UtilityFunctions::push_warning("Runways: model not found: ", path);
		return nullptr;
	}
	Ref<GLTFDocument> doc;
	doc.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	if (doc->append_from_file(path, state) != OK)
	{
		UtilityFunctions::push_warning("Runways: failed to parse model ", path);
		return nullptr;
	}
	return Object::cast_to<Node3D>(doc->generate_scene(state));
}

} // namespace jsbgodot
