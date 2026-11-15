#include "city_lights.h"

#include "geo.h"
#include "geo_frame.h"
#include "s3d_proj.h"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace jsbgodot
{

void CityLights::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_paths", "dirs"), &CityLights::set_paths);
	ClassDB::bind_method(D_METHOD("get_paths"), &CityLights::get_paths);
	ClassDB::bind_method(D_METHOD("set_terrain_height", "height_geodetic"), &CityLights::set_terrain_height);
	ClassDB::bind_method(D_METHOD("update", "cam", "intensity", "fog_density", "delta"), &CityLights::update);
	ClassDB::bind_method(D_METHOD("set_far_radius_m", "m"), &CityLights::set_far_radius_m);
	ClassDB::bind_method(D_METHOD("get_far_radius_m"), &CityLights::get_far_radius_m);
	ClassDB::bind_method(D_METHOD("set_near_radius_m", "m"), &CityLights::set_near_radius_m);
	ClassDB::bind_method(D_METHOD("get_near_radius_m"), &CityLights::get_near_radius_m);
	ClassDB::bind_method(D_METHOD("set_enabled", "on"), &CityLights::set_enabled);
	ClassDB::bind_method(D_METHOD("get_enabled"), &CityLights::get_enabled);
	ClassDB::bind_method(D_METHOD("stats"), &CityLights::stats);
	ClassDB::bind_method(D_METHOD("clear"), &CityLights::clear);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "paths"), "set_paths", "get_paths");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_radius_m"), "set_far_radius_m", "get_far_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_radius_m"), "set_near_radius_m", "get_near_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");
}

CityLights::CityLights()
{
	set_as_top_level(true);
}

CityLights::~CityLights()
{
	quit = true;
	cv.notify_all();
	if (worker.joinable())
	{
		worker.join();
	}
}

void CityLights::set_paths(const PackedStringArray &dirs)
{
	if (dirs == paths)
	{
		return;
	}
	paths = dirs;
	clear();
}

PackedStringArray CityLights::get_paths() const { return paths; }
void CityLights::set_terrain_height(const Callable &height_geodetic) { terrain_cb = height_geodetic; }
void CityLights::set_far_radius_m(double m) { far_radius_m = Math::clamp(m, 2000.0, 120000.0); }
double CityLights::get_far_radius_m() const { return far_radius_m; }
void CityLights::set_near_radius_m(double m) { near_radius_m = Math::clamp(m, 1000.0, 40000.0); }
double CityLights::get_near_radius_m() const { return near_radius_m; }
bool CityLights::get_enabled() const { return enabled; }

void CityLights::set_enabled(bool on)
{
	enabled = on;
	if (!on)
	{
		clear();
	}
}

Dictionary CityLights::stats() const
{
	Dictionary d;
	int drawn = 0, near = 0, pending = 0, empty = 0;
	for (const auto &kv : tiles)
	{
		drawn += kv.second.node != nullptr ? 1 : 0;
		near += (kv.second.node != nullptr && (kv.first >> 62) == 0) ? 1 : 0;
		pending += kv.second.pending ? 1 : 0;
		empty += kv.second.empty ? 1 : 0;
	}
	d["entries"] = (int)tiles.size();
	d["meshes"] = drawn;
	d["near_squares"] = near;
	d["far_blocks"] = drawn - near;
	d["pending"] = pending;
	d["empty"] = empty;
	d["points"] = drawn_points;
	return d;
}

void CityLights::drop_tile(Tile &t)
{
	if (t.node != nullptr)
	{
		drawn_points -= t.points;
		t.node->queue_free();
		t.node = nullptr;
	}
	t.points = 0;
}

void CityLights::clear()
{
	for (auto &kv : tiles)
	{
		drop_tile(kv.second);
	}
	tiles.clear();
	drawn_points = 0;
	generation++;   // results of jobs still running are thrown away
	std::lock_guard<std::mutex> lock(mtx);
	jobs.clear();
	results.clear();
}

void CityLights::ensure_material()
{
	if (material.is_valid())
	{
		return;
	}
	shader = ResourceLoader::get_singleton()->load("res://shaders/city_lights.gdshader");
	material.instantiate();
	if (shader.is_valid())
	{
		material->set_shader(shader);
	}
}

// ---- worker: file -> lamps -------------------------------------------------

namespace
{
uint32_t rd_u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
float rd_f32(const uint8_t *p) { float v; std::memcpy(&v, p, 4); return v; }

struct Poly
{
	int kind = 0;
	std::vector<std::vector<float>> rings;
	float min_x = 0, min_z = 0, max_x = 0, max_z = 0;
};
struct Line
{
	std::vector<float> xz;
	float half_width = 3.0f;
};

bool in_ring(const std::vector<float> &ring, float x, float z)
{
	bool in = false;
	const size_t n = ring.size() / 2;
	for (size_t i = 0, j = n - 1; i < n; j = i++)
	{
		const float xi = ring[i * 2], zi = ring[i * 2 + 1], xj = ring[j * 2], zj = ring[j * 2 + 1];
		if (((zi > z) != (zj > z)) && (x < (xj - xi) * (z - zi) / (zj - zi) + xi))
		{
			in = !in;
		}
	}
	return in;
}

bool in_poly(const Poly &p, float x, float z)
{
	if (x < p.min_x || x > p.max_x || z < p.min_z || z > p.max_z || p.rings.empty() || !in_ring(p.rings[0], x, z))
	{
		return false;
	}
	for (size_t r = 1; r < p.rings.size(); r++)
	{
		if (in_ring(p.rings[r], x, z))
		{
			return false;
		}
	}
	return true;
}

// position hash: the same lamp for the same spot of the world, every session
uint32_t hash3(int64_t a, int64_t b, uint32_t c)
{
	uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ull ^ ((uint64_t)b + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)c * 0x165667B19E3779F9ull;
	h ^= h >> 29;
	h *= 0xBF58476D1CE4E5B9ull;
	h ^= h >> 32;
	return (uint32_t)h;
}
float unit(uint32_t h) { return (float)(h & 0xFFFFFF) / 16777216.0f; }
} // namespace

// The lamps of one square, appended with an offset (a far block places its
// squares side by side); false when the square has no land-cover file.
bool CityLights::read_square(const std::vector<std::string> &dirs, int ei, int ni, int lod, float off_x, float off_z, std::vector<Point> &out)
{
	const std::string fname = "/forest_" + std::to_string((int64_t)ei * TILE_M) + "_" + std::to_string((int64_t)ni * TILE_M) + ".bin";
	std::vector<uint8_t> buf;
	for (const std::string &dir : dirs)
	{
		std::ifstream f(dir + fname, std::ios::binary | std::ios::ate);
		if (!f.is_open())
		{
			continue;
		}
		const std::streamsize size = f.tellg();
		if (size < 32 || size > (std::streamsize)(64 << 20))
		{
			continue;
		}
		f.seekg(0, std::ios::beg);
		buf.resize((size_t)size);
		f.read(reinterpret_cast<char *>(buf.data()), size);
		if ((std::streamsize)f.gcount() == size && std::memcmp(buf.data(), "FOR1", 4) == 0)
		{
			break;
		}
		buf.clear();
	}
	if (buf.empty())
	{
		return false;
	}
	std::vector<Poly> polys;
	std::vector<Line> lines;
	const uint32_t count = rd_u32(&buf[8]);
	size_t p = 32;
	auto need = [&](size_t n) { return p + n <= buf.size(); };
	for (uint32_t i = 0; i < count; i++)
	{
		if (!need(16)) break;
		const int kind = buf[p];
		const float density = rd_f32(&buf[p + 4]);
		const uint32_t rings = rd_u32(&buf[p + 12]);
		p += 16;
		Poly poly;
		poly.kind = kind;
		Line line;
		line.half_width = density;
		bool bad = false;
		for (uint32_t r = 0; r < rings; r++)
		{
			if (!need(4)) { bad = true; break; }
			const uint32_t n = rd_u32(&buf[p]);
			p += 4;
			if (!need((size_t)n * 8)) { bad = true; break; }
			if (kind == 7 || kind == 8 || kind == 3)
			{
				std::vector<float> ring((size_t)n * 2);
				std::memcpy(ring.data(), &buf[p], (size_t)n * 8);
				if (kind == 3) line.xz = std::move(ring);
				else poly.rings.push_back(std::move(ring));
			}
			p += (size_t)n * 8;
		}
		if (bad) break;
		if (kind == 3)
		{
			if (line.xz.size() >= 4) lines.push_back(std::move(line));
		}
		else if ((kind == 7 || kind == 8) && !poly.rings.empty() && poly.rings[0].size() >= 6)
		{
			const std::vector<float> &o = poly.rings[0];
			poly.min_x = poly.max_x = o[0];
			poly.min_z = poly.max_z = o[1];
			for (size_t k = 0; k + 1 < o.size(); k += 2)
			{
				poly.min_x = std::min(poly.min_x, o[k]); poly.max_x = std::max(poly.max_x, o[k]);
				poly.min_z = std::min(poly.min_z, o[k + 1]); poly.max_z = std::max(poly.max_z, o[k + 1]);
			}
			polys.push_back(std::move(poly));
		}
	}
	if (polys.empty())
	{
		return true;
	}
	const int64_t e0 = (int64_t)ei * TILE_M, n0 = (int64_t)ni * TILE_M;
	// Lamps of the built-up ground on a jittered world grid (windows, yards,
	// side streets): the near level every 44 m, the far level every 88 m
	// with twice the size, so a town keeps its brightness as it comes closer.
	const float cell = lod == 0 ? 44.0f : 88.0f;
	const float far_size = lod == 0 ? 1.0f : 1.9f;
	const int gx0 = (int)std::floor((double)e0 / cell), gz0 = (int)std::floor((double)n0 / cell);
	const int gn = (int)std::ceil((double)TILE_M / cell) + 1;
	for (int gz = gz0; gz < gz0 + gn; gz++)
	{
		for (int gx = gx0; gx < gx0 + gn; gx++)
		{
			const uint32_t h = hash3(gx, gz, lod == 0 ? 11u : 23u);
			const float x = (float)(((double)gx + 0.15 + 0.7 * unit(h)) * cell - (double)e0);
			const float z = (float)(((double)gz + 0.15 + 0.7 * unit(h >> 8 ^ h << 13)) * cell - (double)n0);
			if (x < 0.0f || x >= (float)TILE_M || z < 0.0f || z >= (float)TILE_M)
			{
				continue;
			}
			int kind = -1;
			for (const Poly &pl : polys)
			{
				if (in_poly(pl, x, z)) { kind = pl.kind; break; }
			}
			if (kind < 0)
			{
				continue;
			}
			const float u = unit(hash3(gx, gz, 37u));
			const float v = unit(hash3(gx, gz, 53u));
			if (u < (kind == 8 ? 0.35f : 0.22f))
			{
				continue;   // dark plots
			}
			Point pt;
			pt.x = x;
			pt.z = z;
			if (kind == 8)
			{
				// industry and commerce: cool floodlights, some sodium
				if (v < 0.6f) { pt.r = 0.86f; pt.g = 0.93f; pt.b = 1.0f; }
				else { pt.r = 1.0f; pt.g = 0.70f; pt.b = 0.36f; }
				pt.size = (1.1f + 0.5f * u) * far_size;
			}
			else
			{
				if (v < 0.62f) { pt.r = 1.0f; pt.g = 0.72f; pt.b = 0.40f; }        // sodium
				else if (v < 0.88f) { pt.r = 1.0f; pt.g = 0.87f; pt.b = 0.68f; }   // warm white
				else { pt.r = 0.84f; pt.g = 0.92f; pt.b = 1.0f; }                  // LED
				pt.size = (0.75f + 0.5f * u) * far_size;
			}
			pt.x += off_x;
			pt.z += off_z;
			out.push_back(pt);
		}
	}
	// Street lamps: along the roads that run through built-up ground, the
	// near level only (a far square is a patch of glow, not a street map).
	if (lod == 0)
	{
		const float spacing = 46.0f;
		for (const Line &ln : lines)
		{
			if (ln.half_width < 2.5f)
			{
				continue;   // paths and tracks
			}
			float carry = spacing * unit(hash3(e0 + (int64_t)ln.xz[0], n0 + (int64_t)ln.xz[1], 71u));
			for (size_t k = 0; k + 3 < ln.xz.size(); k += 2)
			{
				const float ax = ln.xz[k], az = ln.xz[k + 1], bx = ln.xz[k + 2], bz = ln.xz[k + 3];
				const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
				if (len < 0.01f)
				{
					continue;
				}
				float t = carry;
				while (t < len)
				{
					const float x = ax + (bx - ax) * t / len, z = az + (bz - az) * t / len;
					t += spacing;
					if (x < 0.0f || x >= (float)TILE_M || z < 0.0f || z >= (float)TILE_M)
					{
						continue;
					}
					bool inside = false;
					for (const Poly &pl : polys)
					{
						if (in_poly(pl, x, z)) { inside = true; break; }
					}
					if (!inside)
					{
						continue;
					}
					Point pt;
					pt.x = x;
					pt.z = z;
					const bool led = ln.half_width > 5.0f && unit(hash3((int64_t)(e0 + x) / 400, (int64_t)(n0 + z) / 400, 91u)) < 0.3f;
					if (led) { pt.r = 0.90f; pt.g = 0.95f; pt.b = 1.0f; }
					else { pt.r = 1.0f; pt.g = 0.66f; pt.b = 0.30f; }
					pt.size = ln.half_width > 5.0f ? 1.25f : 1.0f;
					pt.x += off_x;
			pt.z += off_z;
			out.push_back(pt);
				}
				carry = t - len;
			}
		}
	}
	return true;
}

void CityLights::generate(const Job &job, Result &res)
{
	const int span = job.lod == 0 ? 1 : FAR_SPAN;
	for (int j = 0; j < span; j++)
	{
		for (int i = 0; i < span; i++)
		{
			read_square(job.dirs, job.ei + i, job.ni + j, job.lod, (float)(i * TILE_M), (float)(j * TILE_M), res.points);
		}
	}
	if (!res.points.empty())
	{
		// corners for the bilinear placement (the grid is conformal: a few
		// km of it map to the ellipsoid within decimetres)
		const double e0 = (double)job.ei * TILE_M, n0 = (double)job.ni * TILE_M, side = (double)span * TILE_M;
		const double ce[4] = {e0, e0 + side, e0, e0 + side};
		const double cn[4] = {n0, n0, n0 + side, n0 + side};
		for (int c = 0; c < 4; c++)
		{
			const s3d::proj::LatLon ll = s3d::proj::lv95_to_wgs84(ce[c], cn[c]);
			res.corner_lat[c] = ll.lat;
			res.corner_lon[c] = ll.lon;
		}
	}
}

void CityLights::worker_loop()
{
	while (!quit)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait(lock, [&] { return quit || !jobs.empty(); });
			if (quit)
			{
				return;
			}
			job = std::move(jobs.front());
			jobs.pop_front();
		}
		Result res;
		res.key = job.key;
		res.ei = job.ei;
		res.ni = job.ni;
		res.lod = job.lod;
		res.generation = job.generation;
		generate(job, res);
		std::lock_guard<std::mutex> lock(mtx);
		results.push_back(std::move(res));
	}
}

// ---- main thread -----------------------------------------------------------

void CityLights::scan(const Vector2 &cam_en, double cam_alt)
{
	const double block_m = (double)FAR_SPAN * TILE_M;
	const double alt = std::max(cam_alt, 0.0);
	// near squares matter where the camera is closer than near_radius_m (the
	// shader cross-fades the two levels by that distance): from high up, none
	const double near_load = near_radius_m + 1.5 * TILE_M;
	const double near_ground = near_load > alt ? std::sqrt(near_load * near_load - alt * alt) : 0.0;
	const double near_keep = near_ground + 3.0 * TILE_M;
	const double far_keep = far_radius_m + 2.0 * block_m;
	for (auto it = tiles.begin(); it != tiles.end();)
	{
		const int lod = (int)(it->first >> 62);
		const int ei = (int)((it->first >> 31) & 0x3FFFFFFF), ni = (int)(it->first & 0x7FFFFFFF);
		const double side = lod == 0 ? (double)TILE_M : block_m;
		const double dx = (double)ei * TILE_M + side * 0.5 - cam_en.x, dz = (double)ni * TILE_M + side * 0.5 - cam_en.y;
		const double keep = lod == 0 ? near_keep : far_keep;
		if (dx * dx + dz * dz > keep * keep && !it->second.pending)
		{
			drop_tile(it->second);
			it = tiles.erase(it);
		}
		else
		{
			++it;
		}
	}
	std::vector<std::string> dirs;
	for (int i = 0; i < paths.size(); i++)
	{
		if (!String(paths[i]).is_empty())
		{
			dirs.push_back(std::string(String(paths[i]).utf8().get_data()));
		}
	}
	struct Want { Job job; double d2; };
	std::vector<Want> wanted;
	auto want = [&](int ei, int ni, int lod, double d2) {
		Tile &t = tiles[key_of(ei, ni, lod)];
		if (t.empty || t.pending || t.node != nullptr)
		{
			return;
		}
		Want w;
		w.job.key = key_of(ei, ni, lod);
		w.job.ei = ei;
		w.job.ni = ni;
		w.job.lod = lod;
		w.job.generation = generation;
		w.job.dirs = dirs;
		// near squares before far blocks at the same distance
		w.d2 = d2 + (lod == 1 ? block_m * block_m : 0.0);
		t.pending = true;
		wanted.push_back(std::move(w));
	};
	// far blocks, aligned to FAR_SPAN squares
	const int rb = (int)std::ceil(far_radius_m / block_m) + 1;
	const int cbe = (int)std::floor(cam_en.x / block_m), cbn = (int)std::floor(cam_en.y / block_m);
	for (int dn = -rb; dn <= rb; dn++)
	{
		for (int de = -rb; de <= rb; de++)
		{
			const int ei = (cbe + de) * FAR_SPAN, ni = (cbn + dn) * FAR_SPAN;
			if (ei < 0 || ni < 0)
			{
				continue;
			}
			const double dx = (double)ei * TILE_M + block_m * 0.5 - cam_en.x, dz = (double)ni * TILE_M + block_m * 0.5 - cam_en.y;
			const double d2 = dx * dx + dz * dz;
			if (d2 <= (far_radius_m + block_m) * (far_radius_m + block_m))
			{
				want(ei, ni, 1, d2);
			}
		}
	}
	// near squares (a square of a block known to be dark is dark too)
	if (near_ground > 0.0)
	{
		const int r = (int)std::ceil(near_ground / TILE_M);
		const int ce = (int)std::floor(cam_en.x / TILE_M), cn = (int)std::floor(cam_en.y / TILE_M);
		for (int dn = -r; dn <= r; dn++)
		{
			for (int de = -r; de <= r; de++)
			{
				const int ei = ce + de, ni = cn + dn;
				if (ei < 0 || ni < 0)
				{
					continue;
				}
				const double dx = ((double)ei + 0.5) * TILE_M - cam_en.x, dz = ((double)ni + 0.5) * TILE_M - cam_en.y;
				const double d2 = dx * dx + dz * dz;
				if (d2 > near_ground * near_ground)
				{
					continue;
				}
				const auto blk = tiles.find(key_of(ei - ei % FAR_SPAN, ni - ni % FAR_SPAN, 1));
				if (blk != tiles.end() && blk->second.empty)
				{
					continue;
				}
				want(ei, ni, 0, d2);
			}
		}
	}
	if (wanted.empty())
	{
		return;
	}
	std::sort(wanted.begin(), wanted.end(), [](const Want &a, const Want &b) { return a.d2 < b.d2; });
	{
		std::lock_guard<std::mutex> lock(mtx);
		for (Want &w : wanted)
		{
			jobs.push_back(std::move(w.job));
		}
	}
	if (!worker.joinable())
	{
		worker = std::thread(&CityLights::worker_loop, this);
	}
	cv.notify_all();
}

void CityLights::finish(const Result &res)
{
	auto it = tiles.find(res.key);
	if (it == tiles.end())
	{
		return;
	}
	Tile &t = it->second;
	t.pending = false;
	if (res.points.empty())
	{
		drop_tile(t);
		t.empty = true;   // no file, or no built-up ground
		return;
	}
	const double side_m = res.lod == 0 ? (double)TILE_M : (double)FAR_SPAN * TILE_M;
	// terrain heights on a grid over the square, bilinear in between
	const int g = res.lod == 0 ? GRID_NEAR : GRID_FAR;
	std::vector<double> hgt((size_t)g * g, 0.0);
	auto corner_mix = [&](const double c[4], double u, double v) {
		return (c[0] * (1.0 - u) + c[1] * u) * (1.0 - v) + (c[2] * (1.0 - u) + c[3] * u) * v;
	};
	double last = 0.0;
	for (int j = 0; j < g; j++)
	{
		for (int i = 0; i < g; i++)
		{
			const double u = (double)i / (g - 1), v = (double)j / (g - 1);
			double h = NAN;
			if (terrain_cb.is_valid())
			{
				const Variant r = terrain_cb.call(corner_mix(res.corner_lat, u, v), corner_mix(res.corner_lon, u, v));
				if (r.get_type() == Variant::FLOAT || r.get_type() == Variant::INT)
				{
					h = (double)r;
				}
			}
			if (std::isnan(h))
			{
				h = last;
			}
			last = h;
			hgt[(size_t)j * g + i] = h;
		}
	}
	Vector3 corner[4];
	for (int c = 0; c < 4; c++)
	{
		corner[c] = geo::ecef_of(res.corner_lat[c], res.corner_lon[c], 0.0);
	}
	const Vector3 origin = corner[0];
	const Vector3 up = geo::wun_basis(corner_mix(res.corner_lat, 0.5, 0.5), corner_mix(res.corner_lon, 0.5, 0.5)).get_column(1);
	const double lift = res.lod == 0 ? LIFT_NEAR_M : LIFT_FAR_M;
	PackedVector3Array verts;
	PackedColorArray cols;
	PackedVector2Array uvs;   // x: 0 near level, 1 far level (the shader cross-fades them by distance)
	verts.resize((int64_t)res.points.size());
	cols.resize((int64_t)res.points.size());
	uvs.resize((int64_t)res.points.size());
	Vector3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
	for (size_t k = 0; k < res.points.size(); k++)
	{
		const Point &pt = res.points[k];
		const double u = Math::clamp((double)pt.x / side_m, 0.0, 1.0), v = Math::clamp((double)pt.z / side_m, 0.0, 1.0);
		const double gu = u * (g - 1), gv = v * (g - 1);
		const int i0 = std::min((int)gu, g - 2), j0 = std::min((int)gv, g - 2);
		const double fu = gu - i0, fv = gv - j0;
		const double h = (hgt[(size_t)j0 * g + i0] * (1.0 - fu) + hgt[(size_t)j0 * g + i0 + 1] * fu) * (1.0 - fv)
				+ (hgt[(size_t)(j0 + 1) * g + i0] * (1.0 - fu) + hgt[(size_t)(j0 + 1) * g + i0 + 1] * fu) * fv;
		const Vector3 flat = (corner[0] * (1.0 - u) + corner[1] * u) * (1.0 - v) + (corner[2] * (1.0 - u) + corner[3] * u) * v;
		const Vector3 local = flat + up * (h + lift) - origin;
		verts.set((int64_t)k, local);
		cols.set((int64_t)k, Color(pt.r, pt.g, pt.b, pt.size));
		uvs.set((int64_t)k, Vector2((double)res.lod, 0.0));
		lo = lo.min(local);
		hi = hi.max(local);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_COLOR] = cols;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_POINTS, arrays);
	ensure_material();
	mesh->surface_set_material(0, material);
	drop_tile(t);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(mesh);
	mi->set_custom_aabb(AABB(lo - Vector3(20, 20, 20), hi - lo + Vector3(40, 40, 40)));
	mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	add_child(mi);
	// world placement in double precision: the vertices are float offsets
	// from the square's SW corner on the ellipsoid
	mi->set_global_transform(Transform3D(Basis(), origin));
	t.node = mi;
	t.points = (int)res.points.size();
	drawn_points += t.points;
}

void CityLights::update(Camera3D *cam, double intensity, double fog_density, double delta)
{
	ensure_material();
	const bool lit = enabled && intensity > 0.003 && cam != nullptr && paths.size() > 0;
	set_visible(lit);
	if (!lit)
	{
		return;   // by day nothing is read or built
	}
	material->set_shader_parameter("intensity", Math::clamp(intensity, 0.0, 1.0));
	material->set_shader_parameter("fog_density", fog_density);
	material->set_shader_parameter("fade_start", far_radius_m * 0.82);
	material->set_shader_parameter("fade_end", far_radius_m);
	material->set_shader_parameter("near_radius", near_radius_m);
	scan_timer -= delta;
	if (scan_timer <= 0.0)
	{
		scan_timer = SCAN_INTERVAL_S;
		const Vector3 cp = cam->get_global_position();
		if (cp.is_finite() && cp.length() > 1.0e6)
		{
			const geo::Geodetic gd = geo::geodetic_of(cp);
			// height over the ground decides how much near detail is in reach
			double ground = 0.0;
			if (terrain_cb.is_valid())
			{
				const Variant r = terrain_cb.call(gd.lat, gd.lon);
				if ((r.get_type() == Variant::FLOAT || r.get_type() == Variant::INT) && !std::isnan((double)r))
				{
					ground = (double)r;
				}
			}
			scan(Geo::wgs84_to_lv95(gd.lat, gd.lon), gd.h - ground);
		}
	}
	// built squares come in a few per frame; empty ones cost nothing
	int built = 0, taken = 0;
	while (built < FINISH_PER_FRAME && taken < 600)
	{
		Result res;
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (results.empty())
			{
				break;
			}
			res = std::move(results.front());
			results.pop_front();
		}
		taken++;
		if (res.generation != generation)
		{
			continue;
		}
		built += res.points.empty() ? 0 : 1;
		finish(res);
	}
}

} // namespace jsbgodot
