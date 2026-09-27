#include "scenery_objects.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

#include "geo.h"
#include "geo_frame.h"

using namespace godot;

namespace jsbgodot
{

void SceneryObjects::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("apply_scenery_areas", "areas", "scenery3d", "vegetation"), &SceneryObjects::apply_scenery_areas);
	ClassDB::bind_method(D_METHOD("reload"), &SceneryObjects::reload);
	ClassDB::bind_method(D_METHOD("get_object_count"), &SceneryObjects::get_object_count);
	ClassDB::bind_method(D_METHOD("get_loaded_count"), &SceneryObjects::get_loaded_count);
	ClassDB::bind_method(D_METHOD("get_pending_count"), &SceneryObjects::get_pending_count);
	ClassDB::bind_method(D_METHOD("get_excluded_object_count"), &SceneryObjects::get_excluded_object_count);
	ClassDB::bind_method(D_METHOD("get_exclusion_zones"), &SceneryObjects::get_exclusion_zones);
	ClassDB::bind_method(D_METHOD("get_object_info", "i"), &SceneryObjects::get_object_info);
	ClassDB::bind_method(D_METHOD("find_object_node", "name"), &SceneryObjects::find_object_node);
	ClassDB::bind_method(D_METHOD("get_object_label", "i"), &SceneryObjects::get_object_label);
	ClassDB::bind_method(D_METHOD("pick_ray", "origin", "dir"), &SceneryObjects::pick_ray);
	ClassDB::bind_method(D_METHOD("begin_edit", "i"), &SceneryObjects::begin_edit);
	ClassDB::bind_method(D_METHOD("begin_edit_group"), &SceneryObjects::begin_edit_group);
	ClassDB::bind_method(D_METHOD("end_edit_group"), &SceneryObjects::end_edit_group);
	ClassDB::bind_method(D_METHOD("get_object_pivot", "i"), &SceneryObjects::get_object_pivot);
	ClassDB::bind_method(D_METHOD("set_object_pivot", "i", "local", "record"), &SceneryObjects::set_object_pivot, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_object_dimensions", "i"), &SceneryObjects::get_object_dimensions);
	ClassDB::bind_method(D_METHOD("set_object_dimensions", "i", "dims", "record"), &SceneryObjects::set_object_dimensions, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("set_object_pose", "i", "e", "n", "agl_m", "heading_deg", "pitch_deg", "bank_deg", "scale", "record"), &SceneryObjects::set_object_pose, DEFVAL(true));
	ClassDB::bind_static_method("SceneryObjects", D_METHOD("euler_of", "rot"), &SceneryObjects::euler_of);
	ClassDB::bind_static_method("SceneryObjects", D_METHOD("basis_of", "heading_deg", "pitch_deg", "bank_deg"), &SceneryObjects::basis_of);
	ClassDB::bind_method(D_METHOD("drop_to_ground", "i"), &SceneryObjects::drop_to_ground);
	ClassDB::bind_method(D_METHOD("remove_object", "i"), &SceneryObjects::remove_object);
	ClassDB::bind_method(D_METHOD("set_object_highlight", "i", "on"), &SceneryObjects::set_object_highlight);
	ClassDB::bind_method(D_METHOD("clear_highlights"), &SceneryObjects::clear_highlights);
	ClassDB::bind_method(D_METHOD("undo_object_edit"), &SceneryObjects::undo_object_edit);
	ClassDB::bind_method(D_METHOD("has_unsaved_edits"), &SceneryObjects::has_unsaved_edits);
	ClassDB::bind_method(D_METHOD("save_packs"), &SceneryObjects::save_packs);
	ClassDB::bind_method(D_METHOD("get_elevation"), &SceneryObjects::get_elevation);
	ClassDB::bind_method(D_METHOD("set_elevation", "callable"), &SceneryObjects::set_elevation);
	ClassDB::bind_method(D_METHOD("get_load_radius_m"), &SceneryObjects::get_load_radius_m);
	ClassDB::bind_method(D_METHOD("set_load_radius_m", "v"), &SceneryObjects::set_load_radius_m);
	ClassDB::bind_method(D_METHOD("get_unload_radius_m"), &SceneryObjects::get_unload_radius_m);
	ClassDB::bind_method(D_METHOD("set_unload_radius_m", "v"), &SceneryObjects::set_unload_radius_m);
	ClassDB::bind_method(D_METHOD("get_enabled"), &SceneryObjects::get_enabled);
	ClassDB::bind_method(D_METHOD("set_enabled", "v"), &SceneryObjects::set_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "elevation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_elevation", "get_elevation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "load_radius_m"), "set_load_radius_m", "get_load_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "unload_radius_m"), "set_unload_radius_m", "get_unload_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
}

SceneryObjects::SceneryObjects() {}

SceneryObjects::~SceneryObjects()
{
	stop_worker();
}

// ── background parsing ────────────────────────────────────────────────────

void SceneryObjects::start_worker()
{
	if (worker.joinable())
		return;
	worker_stop = false;
	worker = std::thread([this]() { worker_main(); });
}

void SceneryObjects::stop_worker()
{
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		worker_stop = true;
	}
	queue_cv.notify_all();
	if (worker.joinable())
		worker.join();
}

// Parses one GLB at a time: buffers, images and meshes are built here (the
// rendering server is thread-safe); the nodes are made on the main thread.
void SceneryObjects::worker_main()
{
	while (true)
	{
		String path;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this]() { return worker_stop || !load_queue.empty(); });
			if (worker_stop)
				return;
			path = load_queue.front();
			load_queue.pop_front();
		}
		LoadResult r;
		r.path = path;
		if (FileAccess::file_exists(path))
		{
			Ref<GLTFDocument> doc;
			doc.instantiate();
			Ref<GLTFState> state;
			state.instantiate();
			if (doc->append_from_file(path, state) == OK)
				r.scene = doc->generate_scene(state);
		}
		std::lock_guard<std::mutex> lock(queue_mutex);
		results.push_back(r);
	}
}

void SceneryObjects::collect_results()
{
	std::deque<LoadResult> done;
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		done.swap(results);
	}
	for (LoadResult &r : done)
	{
		Node *scene = r.scene;
		if (scene == nullptr)
			UtilityFunctions::push_warning("SceneryObjects: cannot load ", r.path);
		templates[r.path] = scene; // null too: never retried this session
		pending.erase(r.path);
		if (scene)
		{
			AABB box;
			bool first = true;
			accumulate_bounds(scene, Transform3D(), box, first);
			if (!first)
				template_bounds[r.path] = box;
		}
	}
}

// ── library ───────────────────────────────────────────────────────────────

Array SceneryObjects::read_package(const String &path)
{
	Array empty;
	if (!FileAccess::file_exists(path))
		return empty;
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(path));
	if (parsed.get_type() != Variant::DICTIONARY)
	{
		UtilityFunctions::push_warning("SceneryObjects: ", path, " is not a JSON object");
		return empty;
	}
	Array out;
	out.push_back(Dictionary(parsed));
	return out;
}

Node *SceneryObjects::find_class(Node *root, const char *cls)
{
	if (root == nullptr)
		return nullptr;
	if (root->is_class(cls))
		return root;
	const TypedArray<Node> found = root->find_children("*", cls, true, false);
	return found.size() > 0 ? Object::cast_to<Node>(found[0]) : nullptr;
}

static bool zone_contains(const Dictionary &z, double e, double n)
{
	if (z.has("circle"))
	{
		const Array c = z["circle"];
		return c.size() >= 3 && Vector2(e, n).distance_to(Vector2(double(c[0]), double(c[1]))) <= double(c[2]);
	}
	if (!z.has("polygon"))
		return false;
	const Variant pv = z["polygon"];
	PackedVector2Array poly;
	if (pv.get_type() == Variant::PACKED_VECTOR2_ARRAY)
		poly = pv;
	else if (pv.get_type() == Variant::ARRAY)
	{
		const Array a = pv;
		for (int q = 0; q < a.size(); q++)
			poly.push_back(Vector2(a[q]));
	}
	bool inside = false;
	for (int q = 0, r = poly.size() - 1; q < poly.size(); r = q++)
	{
		const Vector2 a = poly[q], b = poly[r];
		if ((a.y > n) != (b.y > n) && e < (b.x - a.x) * (n - a.y) / (b.y - a.y + 1e-12) + a.x)
			inside = !inside;
	}
	return inside;
}

void SceneryObjects::apply_scenery_areas(const Array &areas, Node *scenery3d, Node *vegetation)
{
	if (has_unsaved_edits())
		save_packs();
	clear_instances();
	objects.clear();
	packs.clear();
	undo_stack.clear();
	zones = Array();
	excluded_objects = 0;
	// Path indices of the managers: the k-th area with a buildings/ dir is
	// buildings path k (SettingsStore::apply_scenery_sources order).
	int building_dirs = 0, vegetation_dirs = 0;
	std::vector<int> min_building_dir, min_vegetation_dir;
	for (int i = 0; i < areas.size(); i++)
	{
		const Dictionary area = areas[i];
		const String base = area.get("path", "");
		if (base.is_empty() || !DirAccess::dir_exists_absolute(base))
			continue;
		const bool has_b = DirAccess::dir_exists_absolute(base.path_join("buildings"));
		const bool has_v = DirAccess::dir_exists_absolute(base.path_join("vegetation"));
		const String objects_dir = base.path_join("objects");
		const Array pkgs = read_package(objects_dir.path_join("package.json"));
		if (pkgs.size() > 0)
		{
			Pack p;
			p.dir = objects_dir;
			p.name = String(area.get("name", base.get_file()));
			p.json = pkgs[0];
			packs.push_back(p);
			// zones apply to packs below this one: their dirs come after ours
			min_building_dir.push_back(building_dirs + (has_b ? 1 : 0));
			min_vegetation_dir.push_back(vegetation_dirs + (has_v ? 1 : 0));
		}
		building_dirs += has_b ? 1 : 0;
		vegetation_dirs += has_v ? 1 : 0;
	}
	// zones (with their owner pack index, so lower packs' objects are filtered)
	std::vector<int> zone_owner;
	for (size_t pi = 0; pi < packs.size(); pi++)
	{
		const Array ex = packs[pi].json.get("exclusions", Array());
		for (int k = 0; k < ex.size(); k++)
		{
			if (ex[k].get_type() != Variant::DICTIONARY)
				continue;
			Dictionary z = Dictionary(ex[k]).duplicate();
			z["min_dir"] = min_building_dir[pi];
			z["min_vegetation_dir"] = min_vegetation_dir[pi];
			z["pack"] = packs[pi].name;
			zones.push_back(z);
			zone_owner.push_back((int)pi);
		}
	}
	// objects
	for (size_t pi = 0; pi < packs.size(); pi++)
	{
		const Pack &p = packs[pi];
		const Array objs = p.json.get("objects", Array());
		for (int k = 0; k < objs.size(); k++)
		{
			if (objs[k].get_type() != Variant::DICTIONARY)
				continue;
			const Dictionary o = objs[k];
			Placed pl;
			pl.orig = o;
			pl.pack = (int)pi;
			pl.pack_obj_index = k;
			pl.name = String(o.get("name", o.get("model", "object")));
			pl.model_path = p.dir.path_join(String(o.get("model", "")));
			if (o.has("lat") && o.has("lon"))
			{
				pl.lat = o["lat"];
				pl.lon = o["lon"];
				const Vector2 en = Geo::wgs84_to_lv95(pl.lat, pl.lon);
				pl.e = en.x;
				pl.n = en.y;
			}
			else if (o.has("position_e") && o.has("position_n"))
			{
				pl.e = o["position_e"];
				pl.n = o["position_n"];
				const Vector2 ll = Geo::lv95_to_wgs84(pl.e, pl.n);
				pl.lat = ll.x;
				pl.lon = ll.y;
			}
			else
				continue;
			const Variant agl = o.get("agl_m", Variant());
			const Variant elev = o.get("elevation_m", Variant());
			if (elev.get_type() != Variant::NIL)
			{
				pl.has_elevation = true;
				pl.elevation = elev;
			}
			if (agl.get_type() != Variant::NIL)
				pl.agl = agl;
			pl.rot = basis_of(double(o.get("heading_deg", 0.0)), double(o.get("pitch_deg", 0.0)), double(o.get("bank_deg", 0.0)));
			const Variant sv = o.get("scale", 1.0);
			if (sv.get_type() == Variant::ARRAY && Array(sv).size() >= 3)
				pl.scale3 = Vector3(double(Array(sv)[0]), double(Array(sv)[1]), double(Array(sv)[2]));
			else if (sv.get_type() == Variant::VECTOR3)
				pl.scale3 = sv;
			else
				pl.scale3 = Vector3(1, 1, 1) * double(sv);
			const Variant pv = o.get("pivot", Variant());
			if (pv.get_type() == Variant::ARRAY && Array(pv).size() >= 3)
				pl.pivot = Vector3(double(Array(pv)[0]), double(Array(pv)[1]), double(Array(pv)[2]));
			const double sxz = MAX(MAX(pl.scale3.x, pl.scale3.z), 1e-9);
			pl.radius_m = double(o.get("footprint_radius_m", 0.0)) / sxz;
			pl.ground_ecef = geo::ecef_of(pl.lat, pl.lon, 0.0);
			// hidden by a zone of a higher-priority pack ("objects" or "all")
			for (size_t zi = 0; zi < zone_owner.size() && !pl.excluded; zi++)
			{
				if (zone_owner[zi] >= (int)pi)
					continue;
				const Dictionary z = zones[(int)zi];
				const String kinds = z.get("kinds", "all");
				if ((kinds == "all" || kinds.contains("objects")) && zone_contains(z, pl.e, pl.n))
					pl.excluded = true;
			}
			if (pl.excluded)
				excluded_objects++;
			objects.push_back(pl);
		}
	}
	// managers: building zones use min_dir, vegetation zones min_vegetation_dir
	Array veg_zones;
	for (int i = 0; i < zones.size(); i++)
	{
		Dictionary z = Dictionary(zones[i]).duplicate();
		z["min_dir"] = z["min_vegetation_dir"];
		veg_zones.push_back(z);
	}
	if (Node *bm = find_class(scenery3d, "S3DBuildingManager"))
		if (bm->has_method("set_exclusion_zones"))
			bm->call("set_exclusion_zones", zones);
	if (Node *vm = find_class(vegetation, "S3DVegetationManager"))
		if (vm->has_method("set_exclusion_zones"))
			vm->call("set_exclusion_zones", veg_zones);
	UtilityFunctions::print(vformat("SceneryObjects: %d pack(s), %d objects (%d hidden by higher packs), %d exclusion zones",
		(int)packs.size(), (int)objects.size(), excluded_objects, zones.size()));
}

void SceneryObjects::reload()
{
	clear_instances();
	clear_templates();
}

int SceneryObjects::get_pending_count() const { return pending.size(); }

// ── instances ─────────────────────────────────────────────────────────────

void SceneryObjects::clear_instances()
{
	for (Placed &p : objects)
		unplace(p);
}

void SceneryObjects::clear_templates()
{
	const Array keys = templates.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		Node *n = Object::cast_to<Node>(templates[keys[i]]);
		if (n)
			memdelete(n);
	}
	templates.clear();
	template_bounds.clear();
	mesh_triangles.clear();
}

void SceneryObjects::accumulate_bounds(Node *node, const Transform3D &xf, AABB &box, bool &first)
{
	Transform3D here = xf;
	if (Node3D *n3 = Object::cast_to<Node3D>(node))
		here = xf * n3->get_transform();
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(node))
	{
		const AABB b = here.xform(mi->get_aabb());
		box = first ? b : box.merge(b);
		first = false;
	}
	for (int i = 0; i < node->get_child_count(); i++)
		accumulate_bounds(node->get_child(i), here, box, first);
}

Node *SceneryObjects::load_template(const String &path)
{
	if (templates.has(path))
		return Object::cast_to<Node>(templates[path]);
	if (!pending.has(path))
	{
		pending[path] = true;
		start_worker();
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			load_queue.push_back(path);
		}
		queue_cv.notify_all();
	}
	return nullptr;
}

bool SceneryObjects::place(Placed &p)
{
	if (p.removed || p.excluded)
		return false;
	double h;
	if (p.has_elevation)
		h = p.elevation;
	else
	{
		if (!elevation.is_valid())
			return false;
		h = double(elevation.call(p.lat, p.lon));
		if (std::isnan(h))
			return false; // the precise terrain is not in yet
		h += p.agl;
	}
	Node *tpl = load_template(p.model_path);
	if (tpl == nullptr)
	{
		if (templates.has(p.model_path))
			p.excluded = true; // the file could not be loaded: do not retry
		return false;       // still parsing: the next pass places it
	}
	if (!p.bounds_known && template_bounds.has(p.model_path))
	{
		p.bounds = template_bounds[p.model_path];
		p.bounds_known = true;
	}
	Node3D *inst = Object::cast_to<Node3D>(tpl->duplicate());
	if (inst == nullptr)
		return false;
	inst->set_name(p.name);
	// The model's +Z faces the heading (checked against the orthophoto of
	// LSPF: the Schmerlat main building lands on its real roof). Local +Z is
	// north in the WUN frame, and the heading turns it clockwise
	// (rotation.y = -heading in the local frame, as for the aircraft).
	const Basis wun = geo::wun_basis(p.lat, p.lon);
	Basis b = wun * p.rot;
	b = b.scaled_local(p.scale3);
	add_child(inst);
	inst->set_global_transform(Transform3D(b, geo::ecef_of(p.lat, p.lon, h) - b.xform(p.pivot)));
	p.node = inst;
	if (p.highlighted)
		apply_highlight(p);
	return true;
}

void SceneryObjects::apply_highlight(Placed &p)
{
	if (p.node == nullptr)
		return;
	if (highlight_material.is_null())
	{
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(1.0, 0.6, 0.1, 0.25)); // strong enough to read, weak enough to keep the texture
		m->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		highlight_material = m;
	}
	const TypedArray<Node> meshes = p.node->find_children("*", "MeshInstance3D", true, false);
	for (int k = 0; k < meshes.size(); k++)
		if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(meshes[k]))
			mi->set_material_overlay(p.highlighted ? highlight_material : Ref<Material>());
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p.node))
		mi->set_material_overlay(p.highlighted ? highlight_material : Ref<Material>());
}

void SceneryObjects::set_object_highlight(int i, bool on)
{
	if (i < 0 || i >= (int)objects.size() || objects[i].highlighted == on)
		return;
	objects[i].highlighted = on;
	apply_highlight(objects[i]);
}

void SceneryObjects::clear_highlights()
{
	for (int i = 0; i < (int)objects.size(); i++)
		set_object_highlight(i, false);
}

void SceneryObjects::unplace(Placed &p)
{
	if (p.node)
	{
		p.node->queue_free();
		p.node = nullptr;
	}
}

void SceneryObjects::refresh(Placed &p)
{
	const bool was = p.node != nullptr;
	unplace(p);
	if (was)
		place(p);
}

double SceneryObjects::footprint_m(const Placed &p) const
{
	if (!p.bounds_known)
		return p.radius_m * MAX(p.scale3.x, p.scale3.z);
	double r = 0.0;
	for (int c = 0; c < 8; c++)
	{
		const Vector3 v = p.rot.xform(p.bounds.get_endpoint(c) * p.scale3);
		r = MAX(r, std::sqrt(v.x * v.x + v.z * v.z));
	}
	return r;
}

double SceneryObjects::lowest_m(const Placed &p) const
{
	double lo = 1e300;
	for (int c = 0; c < 8; c++)
		lo = MIN(lo, p.rot.xform(p.bounds.get_endpoint(c) * p.scale3).y);
	return lo;
}

Vector3 SceneryObjects::euler_of(const Basis &rot)
{
	const Vector3 e = rot.get_euler(EulerOrder::EULER_ORDER_YXZ); // rot = Ry(y) * Rx(x) * Rz(z)
	return Vector3(Math::fposmod(Math::rad_to_deg(-e.y), 360.0), Math::rad_to_deg(e.x), Math::rad_to_deg(e.z));
}

Basis SceneryObjects::basis_of(double heading_deg, double pitch_deg, double bank_deg)
{
	return Basis::from_euler(Vector3(Math::deg_to_rad(pitch_deg), -Math::deg_to_rad(heading_deg), Math::deg_to_rad(bank_deg)), EulerOrder::EULER_ORDER_YXZ);
}

Vector3 SceneryObjects::camera_position() const
{
	Viewport *vp = get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	return cam ? cam->get_global_position() : Vector3();
}

void SceneryObjects::_process(double)
{
	if (!enabled || objects.empty() || Engine::get_singleton()->is_editor_hint())
		return;
	if (++frame_counter % 15 != 0)
		return;
	const Vector3 cam = camera_position();
	if (cam.length_squared() < 1.0)
		return;
	collect_results();
	for (Placed &p : objects)
	{
		if (p.excluded || p.removed)
			continue;
		const double d = p.ground_ecef.distance_to(cam);
		if (p.node == nullptr)
		{
			if (d <= load_radius_m)
				place(p); // queues the parse when the template is not in yet
		}
		else if (d > unload_radius_m)
			unplace(p);
	}
}

// ── editing ───────────────────────────────────────────────────────────────

// Triangles of a mesh in mesh space, extracted once per Mesh resource.
const PackedVector3Array &SceneryObjects::triangles_of(const Ref<Mesh> &mesh)
{
	const int64_t key = mesh->get_instance_id();
	auto it = mesh_triangles.find(key);
	if (it == mesh_triangles.end())
	{
		PackedVector3Array tris;
		for (int si = 0; si < mesh->get_surface_count(); si++)
		{
			const Array arrays = mesh->surface_get_arrays(si);
			if (arrays.size() <= Mesh::ARRAY_INDEX)
				continue;
			const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
			const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
			if (idx.size() > 0)
			{
				for (int i = 0; i + 2 < idx.size(); i += 3)
				{
					tris.push_back(verts[idx[i]]);
					tris.push_back(verts[idx[i + 1]]);
					tris.push_back(verts[idx[i + 2]]);
				}
			}
			else
				tris.append_array(verts);
		}
		it = mesh_triangles.emplace(key, tris).first;
	}
	return it->second;
}

// Möller–Trumbore: distance t along the ray to the nearest triangle of the
// node's meshes (in world space), true when something is hit.
bool SceneryObjects::ray_hits_node(Node *node, const Vector3 &origin, const Vector3 &dir, double &t, SceneryObjects *self)
{
	bool hit = false;
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(node))
	{
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid())
		{
			const Transform3D xf = mi->get_global_transform();
			const Transform3D inv = xf.affine_inverse();
			const Vector3 lo = inv.xform(origin);
			const Vector3 ld = inv.basis.xform(dir); // not normalised: t stays in world units
			// bounding box first
			const AABB box = mi->get_aabb();
			bool box_hit = box.has_point(lo);
			if (!box_hit)
			{
				double tmin = 0.0, tmax = 1e300;
				box_hit = true;
				for (int a = 0; a < 3 && box_hit; a++)
				{
					if (std::abs(ld[a]) < 1e-12)
					{
						if (lo[a] < box.position[a] || lo[a] > box.position[a] + box.size[a])
							box_hit = false;
						continue;
					}
					double t1 = (box.position[a] - lo[a]) / ld[a], t2 = (box.position[a] + box.size[a] - lo[a]) / ld[a];
					if (t1 > t2)
						std::swap(t1, t2);
					tmin = MAX(tmin, t1);
					tmax = MIN(tmax, t2);
					if (tmin > tmax)
						box_hit = false;
				}
			}
			if (box_hit)
			{
				const PackedVector3Array &tris = self->triangles_of(mesh);
				const int n = tris.size();
				for (int i = 0; i + 2 < n; i += 3)
				{
					const Vector3 v0 = tris[i], e1 = tris[i + 1] - v0, e2 = tris[i + 2] - v0;
					const Vector3 p = ld.cross(e2);
					const double det = e1.dot(p);
					if (std::abs(det) < 1e-12)
						continue;
					const double inv_det = 1.0 / det;
					const Vector3 s = lo - v0;
					const double u = s.dot(p) * inv_det;
					if (u < 0.0 || u > 1.0)
						continue;
					const Vector3 q = s.cross(e1);
					const double v = ld.dot(q) * inv_det;
					if (v < 0.0 || u + v > 1.0)
						continue;
					const double tt = e2.dot(q) * inv_det;
					if (tt > 1e-6 && tt < t)
					{
						t = tt;
						hit = true;
					}
				}
			}
		}
	}
	for (int i = 0; i < node->get_child_count(); i++)
		hit = ray_hits_node(node->get_child(i), origin, dir, t, self) || hit;
	return hit;
}

int SceneryObjects::pick_ray(const Vector3 &origin, const Vector3 &dir)
{
	int best = -1;
	double best_t = 1e300;
	const Vector3 d = dir.normalized();
	for (size_t i = 0; i < objects.size(); i++)
	{
		const Placed &p = objects[i];
		if (p.node == nullptr)
			continue;
		double t = best_t;
		if (ray_hits_node(p.node, origin, d, t, this) && t < best_t)
		{
			best_t = t;
			best = (int)i;
		}
	}
	return best;
}

void SceneryObjects::begin_edit(int i)
{
	if (i < 0 || i >= (int)objects.size())
		return;
	const Placed &p = objects[i];
	if (!group_open)
		undo_group++; // a single edit is its own undo step
	undo_stack.push_back({i, p.e, p.n, p.agl, p.rot, p.scale3, p.pivot, p.removed, undo_group});
}

void SceneryObjects::begin_edit_group()
{
	undo_group++;
	group_open = true;
}

void SceneryObjects::end_edit_group()
{
	group_open = false;
}

Vector3 SceneryObjects::get_object_pivot(int i) const
{
	return (i >= 0 && i < (int)objects.size()) ? objects[i].pivot : Vector3();
}

void SceneryObjects::set_object_pivot(int i, const Vector3 &local, bool record)
{
	if (i < 0 || i >= (int)objects.size())
		return;
	Placed &p = objects[i];
	if (record)
		begin_edit(i);
	// the placement point follows the new pivot so the model does not move
	const Basis wun = geo::wun_basis(p.lat, p.lon);
	const Vector3 shift = (wun * p.rot).scaled_local(p.scale3).xform(local - p.pivot); // ECEF
	// the placement point moves by `shift` in ECEF; LV95 e/n come from the
	// exact conversion (a true-east offset is not a grid-east offset: the
	// meridian convergence is about a degree here) and the absolute height
	// is kept, so the model does not move at all
	double terr_old = NAN;
	if (elevation.is_valid() && !p.has_elevation)
		terr_old = double(elevation.call(p.lat, p.lon));
	const double h_old = std::isnan(terr_old) ? geo::geodetic_of(p.ground_ecef).h : terr_old + p.agl;
	const Vector3 new_pt = geo::ecef_of(p.lat, p.lon, h_old) + shift;
	const geo::Geodetic g = geo::geodetic_of(new_pt);
	const Vector2 en = Geo::wgs84_to_lv95(g.lat, g.lon);
	p.pivot = local;
	p.e = en.x;
	p.n = en.y;
	p.lat = g.lat;
	p.lon = g.lon;
	const double terr_new = (elevation.is_valid() && !p.has_elevation) ? double(elevation.call(p.lat, p.lon)) : NAN;
	if (!std::isnan(terr_new))
		p.agl = g.h - terr_new;
	else
		p.agl += shift.dot(wun.get_column(1));
	p.ground_ecef = geo::ecef_of(p.lat, p.lon, 0.0);
	packs[p.pack].dirty = true;
	refresh(p);
}

Vector3 SceneryObjects::get_object_dimensions(int i) const
{
	if (i < 0 || i >= (int)objects.size() || !objects[i].bounds_known)
		return Vector3();
	return objects[i].bounds.size * objects[i].scale3;
}

void SceneryObjects::set_object_dimensions(int i, const Vector3 &dims, bool record)
{
	if (i < 0 || i >= (int)objects.size() || !objects[i].bounds_known)
		return;
	const Placed &p = objects[i];
	Vector3 sc = p.scale3;
	for (int a = 0; a < 3; a++)
		if (p.bounds.size[a] > 1e-6 && dims[a] > 1e-6)
			sc[a] = dims[a] / p.bounds.size[a];
	set_object_transform(i, p.e, p.n, p.agl, p.rot, sc, record);
}

void SceneryObjects::set_object_pose(int i, double e, double n, double agl_m, double heading_deg, double pitch_deg, double bank_deg,
		const Vector3 &scale, bool record)
{
	set_object_transform(i, e, n, agl_m, basis_of(heading_deg, pitch_deg, bank_deg), scale, record);
}

void SceneryObjects::set_object_transform(int i, double e, double n, double agl_m, const Basis &rot, const Vector3 &scale, bool record)
{
	if (i < 0 || i >= (int)objects.size())
		return;
	if (record)
		begin_edit(i);
	Placed &p = objects[i];
	p.e = e;
	p.n = n;
	const Vector2 ll = Geo::lv95_to_wgs84(e, n);
	p.lat = ll.x;
	p.lon = ll.y;
	p.agl = agl_m;
	p.has_elevation = false;
	p.rot = rot.orthonormalized();
	p.scale3 = Vector3(MAX(scale.x, 0.01), MAX(scale.y, 0.01), MAX(scale.z, 0.01));
	p.ground_ecef = geo::ecef_of(p.lat, p.lon, 0.0);
	packs[p.pack].dirty = true;
	refresh(p);
	if (p.node == nullptr && !p.removed && !p.excluded)
		place(p);
}

void SceneryObjects::drop_to_ground(int i)
{
	if (i < 0 || i >= (int)objects.size())
		return;
	Placed &p = objects[i];
	if (!p.bounds_known)
	{
		if (!template_bounds.has(p.model_path))
			return;
		p.bounds = template_bounds[p.model_path];
		p.bounds_known = true;
	}
	set_object_transform(i, p.e, p.n, -lowest_m(p), p.rot, p.scale3, true);
}

void SceneryObjects::remove_object(int i)
{
	if (i < 0 || i >= (int)objects.size())
		return;
	begin_edit(i);
	Placed &p = objects[i];
	p.removed = true;
	packs[p.pack].dirty = true;
	unplace(p);
}

bool SceneryObjects::undo_object_edit()
{
	if (undo_stack.empty())
		return false;
	const int group = undo_stack.back().group;
	bool any = false;
	while (!undo_stack.empty() && undo_stack.back().group == group)
	{
		const Edit ed = undo_stack.back();
		undo_stack.pop_back();
		if (ed.index < 0 || ed.index >= (int)objects.size())
			continue;
		Placed &p = objects[ed.index];
		p.removed = ed.removed;
		p.pivot = ed.pivot;
		set_object_transform(ed.index, ed.e, ed.n, ed.agl, ed.rot, ed.scale3, false);
		any = true;
	}
	return any;
}

bool SceneryObjects::has_unsaved_edits() const
{
	for (const Pack &p : packs)
		if (p.dirty)
			return true;
	return false;
}

int SceneryObjects::save_packs()
{
	int written = 0;
	for (size_t pi = 0; pi < packs.size(); pi++)
	{
		Pack &pack = packs[pi];
		if (!pack.dirty)
			continue;
		Dictionary json = pack.json.duplicate(true);
		// exclusion margin as the converter wrote it (circle radius - footprint), default 3 m
		double margin = 3.0;
		const Array old_ex = pack.json.get("exclusions", Array());
		Array kept_ex; // zones not tied to an object (hand-drawn polygons etc.)
		for (int k = 0; k < old_ex.size(); k++)
		{
			const Dictionary z = old_ex[k];
			if (!z.has("object"))
				kept_ex.push_back(z);
		}
		Array objs, ex = kept_ex;
		for (const Placed &p : objects)
		{
			if (p.pack != (int)pi || p.removed)
				continue;
			Dictionary o = p.orig.duplicate(true);
			o["lat"] = Math::snapped(p.lat, 1e-7);
			o["lon"] = Math::snapped(p.lon, 1e-7);
			o["position_e"] = Math::snapped(p.e, 0.01);
			o["position_n"] = Math::snapped(p.n, 0.01);
			o["agl_m"] = Math::snapped(p.agl, 0.01);
			o["elevation_m"] = Variant();
			const Vector3 eul = euler_of(p.rot);
			o["heading_deg"] = Math::snapped(eul.x, 0.01);
			o["pitch_deg"] = Math::snapped(eul.y, 0.01);
			o["bank_deg"] = Math::snapped(eul.z, 0.01);
			if (std::abs(p.scale3.x - p.scale3.y) < 1e-6 && std::abs(p.scale3.x - p.scale3.z) < 1e-6)
				o["scale"] = Math::snapped(p.scale3.x, 0.0001);
			else
			{
				Array sc;
				sc.push_back(Math::snapped(p.scale3.x, 0.0001));
				sc.push_back(Math::snapped(p.scale3.y, 0.0001));
				sc.push_back(Math::snapped(p.scale3.z, 0.0001));
				o["scale"] = sc;
			}
			if (p.pivot.length_squared() > 1e-12)
			{
				Array pv;
				pv.push_back(Math::snapped(p.pivot.x, 0.001));
				pv.push_back(Math::snapped(p.pivot.y, 0.001));
				pv.push_back(Math::snapped(p.pivot.z, 0.001));
				o["pivot"] = pv;
			}
			else
				o.erase("pivot");
			const double footprint = footprint_m(p);
			o["footprint_radius_m"] = Math::snapped(footprint, 0.01);
			objs.push_back(o);
			if (footprint > 0.5)
			{
				Dictionary z;
				z["kinds"] = "buildings,vegetation,objects";
				Array c;
				c.push_back(Math::snapped(p.e, 0.01));
				c.push_back(Math::snapped(p.n, 0.01));
				c.push_back(Math::snapped(footprint + margin, 0.1));
				z["circle"] = c;
				z["object"] = p.name;
				ex.push_back(z);
			}
		}
		json["objects"] = objs;
		json["exclusions"] = ex;
		const String path = pack.dir.path_join("package.json");
		Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
		if (f.is_null())
		{
			UtilityFunctions::push_warning("SceneryObjects: cannot write ", path);
			continue;
		}
		f->store_string(JSON::stringify(json, "  ") + "\n");
		f->close();
		pack.json = json;
		pack.dirty = false;
		written++;
		UtilityFunctions::print("SceneryObjects: saved ", path, " (", objs.size(), " objects)");
	}
	return written;
}

// ── accessors ─────────────────────────────────────────────────────────────

int SceneryObjects::get_object_count() const { return (int)objects.size(); }

int SceneryObjects::get_loaded_count() const
{
	int n = 0;
	for (const Placed &p : objects)
		n += p.node != nullptr;
	return n;
}

int SceneryObjects::get_excluded_object_count() const { return excluded_objects; }
Array SceneryObjects::get_exclusion_zones() const { return zones; }

Dictionary SceneryObjects::get_object_info(int i) const
{
	Dictionary d;
	if (i < 0 || i >= (int)objects.size())
		return d;
	const Placed &p = objects[i];
	d["name"] = p.name;
	d["model"] = p.model_path;
	d["pack"] = p.pack >= 0 ? packs[p.pack].name : String();
	d["lat"] = p.lat;
	d["lon"] = p.lon;
	d["e"] = p.e;
	d["n"] = p.n;
	d["agl_m"] = p.agl;
	const Vector3 eul = euler_of(p.rot);
	d["heading_deg"] = eul.x;
	d["pitch_deg"] = eul.y;
	d["bank_deg"] = eul.z;
	d["rot"] = p.rot;
	d["scale"] = p.scale3;
	d["scale_mean"] = (p.scale3.x + p.scale3.y + p.scale3.z) / 3.0;
	d["radius_m"] = footprint_m(p);
	d["lowest_m"] = p.bounds_known ? lowest_m(p) : 0.0;
	d["pivot"] = p.pivot;
	d["bounds_size"] = p.bounds_known ? p.bounds.size : Vector3();
	d["bounds_min"] = p.bounds_known ? p.bounds.position : Vector3();
	d["dimensions"] = p.bounds_known ? p.bounds.size * p.scale3 : Vector3();
	if (p.node)
	{
		const Basis b = (geo::wun_basis(p.lat, p.lon) * p.rot).scaled_local(p.scale3);
		d["ecef"] = p.node->get_global_position() + b.xform(p.pivot); // the placement point
	}
	d["excluded"] = p.excluded;
	d["removed"] = p.removed;
	d["loaded"] = p.node != nullptr;
	d["node"] = p.node;
	return d;
}

String SceneryObjects::get_object_label(int i) const
{
	if (i < 0 || i >= (int)objects.size())
		return "";
	const Placed &p = objects[i];
	const Vector3 eul = euler_of(p.rot);
	const bool uniform = std::abs(p.scale3.x - p.scale3.y) < 1e-6 && std::abs(p.scale3.x - p.scale3.z) < 1e-6;
	const String sc = uniform ? vformat("%.3f", p.scale3.x) : vformat("%.3f / %.3f / %.3f", p.scale3.x, p.scale3.y, p.scale3.z);
	const String tilt = (std::abs(eul.y) > 0.05 || std::abs(eul.z) > 0.05) ? vformat(String::utf8(", pitch %.1f° bank %.1f°"), eul.y, eul.z) : String();
	return vformat(String::utf8("%s (%s): E %.1f N %.1f, %.2f m above ground, heading %.1f°%s, scale %s%s"), p.name, packs[p.pack].name,
		p.e, p.n, p.agl, eul.x, tilt, sc, packs[p.pack].dirty ? String("  *unsaved*") : String());
}

Node3D *SceneryObjects::find_object_node(const String &name) const
{
	for (const Placed &p : objects)
		if (p.node && p.name == name)
			return p.node;
	return nullptr;
}

Callable SceneryObjects::get_elevation() const { return elevation; }
void SceneryObjects::set_elevation(const Callable &c) { elevation = c; }
double SceneryObjects::get_load_radius_m() const { return load_radius_m; }
void SceneryObjects::set_load_radius_m(double v) { load_radius_m = v; }
double SceneryObjects::get_unload_radius_m() const { return unload_radius_m; }
void SceneryObjects::set_unload_radius_m(double v) { unload_radius_m = v; }
bool SceneryObjects::get_enabled() const { return enabled; }
void SceneryObjects::set_enabled(bool v) { enabled = v; }

} // namespace jsbgodot
