#include "vfr_map.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

using namespace godot;

namespace jsbgodot
{

// ── Constants ───────────────────────────────────────────────────────────────

const double VfrMap::VIEW_WIDTHS_M[5] = { 5000.0, 10000.0, 20000.0, 40000.0, 80000.0 };
const int VfrMap::FRAME_SIZES_PX[3] = { 240, 320, 400 };

// Map tile pyramid (scripts/generate_vfr_map_tiles.py): same LV95 grid family
// as the orthophoto layer — file names encode the SE corner of each cell.
const VfrMap::LevelDef VfrMap::LEVELS[3] = {
    { 1024, "" },      // L0: 64x64 px, 16 m/px
    { 8192, "mip3" },  // L3: 64x64 px, 128 m/px
    { 32768, "mip5" }, // L5: 64x64 px, 512 m/px
};

int VfrMap::level_cell_m(int level)
{
    return LEVELS[std::max(0, std::min(2, level))].cell_m;
}

namespace
{
    // Palette (VFR-sectional style)
    const Color C_BG(0.05, 0.07, 0.10, 1.0);          // frame background
    const Color C_BORDER(0.35, 0.45, 0.60, 0.9);      // frame border
    const Color C_TITLE_BG(0.10, 0.13, 0.18, 0.92);   // title bar
    const Color C_TITLE_TEXT(0.75, 0.85, 1.0, 1.0);   // title text
    const Color C_RINGS(1.0, 1.0, 1.0, 0.35);         // range rings
    const Color C_RING_LABEL(1.0, 1.0, 1.0, 0.75);    // ring distance labels
    const Color C_COURSE(1.0, 0.85, 0.2, 0.9);        // course line
    const Color C_AC_BODY(0.9, 1.0, 0.9, 1.0);        // aircraft symbol fill
    const Color C_AC_EDGE(0.1, 0.15, 0.1, 1.0);       // aircraft symbol edge
    const Color C_AIRPORT_INTL(1.0, 1.0, 1.0, 1.0);   // international airport
    const Color C_AIRPORT_GA(0.85, 0.9, 1.0, 1.0);    // regional / GA airport
    const Color C_LABEL_SHADOW(0.0, 0.0, 0.0, 0.8);   // text shadow
    const Color C_CANTON(1.0, 1.0, 1.0, 0.25);        // canton borders
    const Color C_NO_TILE(0.13, 0.17, 0.14, 1.0);     // missing-tile fill

    constexpr int TITLE_H = 26;                        // title bar height (px)
    constexpr int MARGIN = 8;                          // frame padding (px)

    constexpr double TAU = 6.283185307179586;

    double norm_pi(double a)
    {
        while (a > M_PI) a -= 2.0 * M_PI;
        while (a < -M_PI) a += 2.0 * M_PI;
        return a;
    }
} // namespace

// ── Binding ─────────────────────────────────────────────────────────────────

void VfrMap::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("setup", "swiss_e", "swiss_n", "config_path"), &VfrMap::setup);
    ClassDB::bind_method(D_METHOD("set_tile_paths", "map_dirs"), &VfrMap::set_tile_paths);
    ClassDB::bind_method(D_METHOD("apply_config"), &VfrMap::apply_config);
    ClassDB::bind_method(D_METHOD("update", "godot_pos", "heading_deg", "ui_visible"), &VfrMap::update);
    ClassDB::bind_method(D_METHOD("get_cached_tile_count"), &VfrMap::get_cached_tile_count);
    ClassDB::bind_method(D_METHOD("get_visible_airport_count"), &VfrMap::get_visible_airport_count);
    ClassDB::bind_method(D_METHOD("get_zoom_index"), &VfrMap::get_zoom_index);
    ClassDB::bind_method(D_METHOD("get_view_width_m"), &VfrMap::get_view_width_m);
    ClassDB::bind_method(D_METHOD("toggle_visible"), &VfrMap::toggle_visible);
    ClassDB::bind_method(D_METHOD("zoom_in"), &VfrMap::zoom_in);
    ClassDB::bind_method(D_METHOD("zoom_out"), &VfrMap::zoom_out);
    ClassDB::bind_method(D_METHOD("get_heading_rad"), &VfrMap::get_heading_rad);
    ClassDB::bind_method(D_METHOD("is_map_enabled"), &VfrMap::is_map_enabled);
    ClassDB::bind_method(D_METHOD("set_frame_size", "size_px"), &VfrMap::set_frame_size);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

VfrMap::VfrMap()
{
    // Control-based HUD widget: stop mouse events so wheel zoom works even
    // when nothing else would consume them; _reposition() pins the frame to
    // the top-right corner.
    set_mouse_filter(Control::MOUSE_FILTER_STOP);
}

void VfrMap::_ready()
{
    load_config();
    load_static_data();
    set_visible(enabled);
    _reposition();
    queue_redraw();
}

// Pin the frame to the top-right corner at its configured size.
void VfrMap::_reposition()
{
    set_anchors_preset(Control::PRESET_TOP_RIGHT);
    double s = frame_size_px();
    set_offset(Side::SIDE_LEFT, -s - 16);
    set_offset(Side::SIDE_TOP, 16);
    set_offset(Side::SIDE_RIGHT, -16);
    set_offset(Side::SIDE_BOTTOM, 16 + s);
}

void VfrMap::_notification(int what)
{
    if (what == Control::NOTIFICATION_RESIZED)
    {
        _reposition(); // stay pinned to the top-right corner on viewport resize
    }
}

void VfrMap::_unhandled_input(const Ref<InputEvent> &event)
{
    if (ui_visible || !enabled)
    {
        return; // full-screen UI owns the input
    }

    InputEventKey *key = Object::cast_to<InputEventKey>(event.ptr());
    if (key != nullptr && key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_N)
    {
        toggle_visible();
    }
}

void VfrMap::toggle_visible()
{
    enabled = !enabled;
    set_visible(enabled);
    queue_redraw();
}

void VfrMap::zoom_in()
{
    zoom_step(-1);
}

void VfrMap::zoom_out()
{
    zoom_step(1);
}

// GUI input: wheel zoom while the pointer is over the frame. Control's GUI
// path consumes mouse events before _unhandled_input, so this override is
// where wheel events are visible to us.
void VfrMap::_gui_input(const Ref<InputEvent> &event)
{
    if (ui_visible || !enabled)
    {
        return;
    }

    InputEventMouseButton *btn = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (btn == nullptr || !btn->is_pressed())
    {
        return;
    }
    // Only react when the pointer is over the frame.
    Rect2 r(get_position(), get_size());
    if (!r.has_point(btn->get_position()))
    {
        return;
    }
    int step = 0;
    if (btn->get_button_index() == MOUSE_BUTTON_WHEEL_UP)
    {
        step = -1; // wheel up = zoom in (narrower view)
    }
    else if (btn->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)
    {
        step = 1;
    }
    if (step != 0)
    {
        zoom_step(step);
    }
}

void VfrMap::zoom_step(int step)
{
    int next = std::max(0, std::min(4, zoom_index + step));
    if (next != zoom_index)
    {
        zoom_index = next;
        persist_zoom();
        queue_redraw();
    }
}

// ── Setup / config ──────────────────────────────────────────────────────────

void VfrMap::setup(double swiss_e, double swiss_n, const String &config_path_in)
{
    swiss_center_e = swiss_e;
    swiss_center_n = swiss_n;
    config_path = config_path_in;
    load_config();
    set_visible(enabled);
    _reposition();
    queue_redraw();
}

void VfrMap::set_tile_paths(const PackedStringArray &map_dirs)
{
    tile_paths = map_dirs;
    // A path change invalidates nothing (keys are grid-relative), but a new
    // area may hold tiles we previously failed to load.
    queue_redraw();
}

void VfrMap::apply_config()
{
    load_config();
    set_visible(enabled);
    _reposition();
    queue_redraw();
}

void VfrMap::load_config()
{
    if (config_path.is_empty())
    {
        return;
    }
    Ref<ConfigFile> cfg = memnew(ConfigFile);
    if (cfg->load(config_path) != OK || !cfg->has_section("map"))
    {
        return;
    }
    // Key names must match settings_screen.gd, which writes [map] map_enabled
    // / map_frame_size (zoom_index is owned by this module via persist_zoom).
    enabled = cfg->get_value("map", "map_enabled", true);
    frame_size_idx = std::max(0, std::min(2, int(cfg->get_value("map", "map_frame_size", 1))));
    zoom_index = std::max(0, std::min(4, int(cfg->get_value("map", "zoom_index", 2))));
}

void VfrMap::persist_zoom()
{
    if (config_path.is_empty())
    {
        return;
    }
    Ref<ConfigFile> cfg = memnew(ConfigFile);
    if (cfg->load(config_path) != OK)
    {
        return; // no config yet — zoom resets next launch, that's fine
    }
    cfg->set_value("map", "zoom_index", zoom_index);
    cfg->save(config_path);
}

void VfrMap::set_frame_size(int size_px)
{
    int best = 0;
    int best_dist = 1 << 30;
    for (int i = 0; i < 3; i++)
    {
        int d = std::abs(FRAME_SIZES_PX[i] - size_px);
        if (d < best_dist)
        {
            best_dist = d;
            best = i;
        }
    }
    frame_size_idx = best;
    _reposition();
    queue_redraw();
}

double VfrMap::frame_size_px() const
{
    return double(FRAME_SIZES_PX[frame_size_idx]);
}

int VfrMap::pick_level() const
{
    // Finest level whose tiles still render at >= MIN_TILE_SCREEN_PX on
    // screen. Wider views switch to coarser levels so the tile count (and
    // cache pressure) stays bounded instead of loading hundreds of
    // sub-pixel tiles.
    double ppm = frame_size_px() / VIEW_WIDTHS_M[zoom_index];
    for (int i = 0; i < 3; i++)
    {
        if (LEVELS[i].cell_m * ppm >= MIN_TILE_SCREEN_PX)
        {
            return i;
        }
    }
    return 2;
}

// ── Per-frame drive ─────────────────────────────────────────────────────────

void VfrMap::update(const Vector3 &godot_pos, double heading_deg, bool ui_visible_in)
{
    ui_visible = ui_visible_in;
    if (!enabled)
    {
        return;
    }
    // Godot -> Swiss LV95 (same convention as the HUD coordinate readout).
    ac_e = swiss_center_e - double(godot_pos.x);
    ac_n = swiss_center_n + double(godot_pos.z);
    heading_rad = Math::deg_to_rad(heading_deg);
    has_position = true;

    // Throttle redraws to ~10 Hz; the map only needs to track smoothly.
    double now = double(Time::get_singleton()->get_ticks_msec());
    if (now - last_redraw_ms >= REDRAW_INTERVAL * 1000.0)
    {
        last_redraw_ms = now;
        queue_redraw();
    }
}

// ── Debug / test accessors ──────────────────────────────────────────────────

int VfrMap::get_cached_tile_count() const
{
    return int(cache_lru.size());
}

int VfrMap::get_visible_airport_count() const
{
    return visible_airports;
}

int VfrMap::get_zoom_index() const
{
    return zoom_index;
}

double VfrMap::get_view_width_m() const
{
    return VIEW_WIDTHS_M[zoom_index];
}

double VfrMap::get_heading_rad() const
{
    return heading_rad;
}

bool VfrMap::is_map_enabled() const
{
    return enabled;
}

// ── Static data (airports + cantons) ────────────────────────────────────────

void VfrMap::load_static_data()
{
    parse_airports("res://airports.json");
    parse_cantons("res://web/data/cantons.geojson");
}

Vector2 VfrMap::wgs84_to_lv95(double lat_deg, double lon_deg)
{
    // swisstopo polynomial approximation (same as AirportsLib / Geo).
    double phi = (lat_deg * 3600.0 - 169028.66) / 10000.0;
    double lam = (lon_deg * 3600.0 - 26782.5) / 10000.0;
    double e = 2600072.37 + 211455.93 * lam - 10938.51 * lam * phi
               - 0.36 * lam * phi * phi - 44.54 * lam * lam * lam;
    double n = 1200147.07 + 308807.95 * phi + 3745.25 * lam * lam
               + 76.63 * phi * phi - 194.56 * lam * lam * phi
               + 119.79 * phi * phi * phi;
    return Vector2(e, n);
}

void VfrMap::parse_airports(const String &path)
{
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    if (f.is_null())
    {
        return;
    }
    Variant parsed = JSON::parse_string(f->get_as_text());
    if (parsed.get_type() != Variant::DICTIONARY)
    {
        return;
    }
    Dictionary root = parsed;
    Array list = root.get("airports", Array());
    for (int i = 0; i < list.size(); i++)
    {
        Dictionary a = list[i];
        double lat = double(a.get("lat", 0.0));
        double lon = double(a.get("lon", 0.0));
        Vector2 lv = wgs84_to_lv95(lat, lon);
        Airport ap;
        ap.e = lv.x;
        ap.n = lv.y;
        ap.icao = String(a.get("icao", ""));
        String type = String(a.get("type", ""));
        ap.international = (type == "international_airport");
        airports.push_back(ap);
    }
}

void VfrMap::parse_cantons(const String &path)
{
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    if (f.is_null())
    {
        return;
    }
    Variant parsed = JSON::parse_string(f->get_as_text());
    if (parsed.get_type() != Variant::DICTIONARY)
    {
        return;
    }
    Dictionary root = parsed;
    Array features = root.get("features", Array());

    std::vector<Vector2> ring;
    auto flush_ring = [&]() {
        if (ring.size() > 1)
        {
            canton_rings.push_back(ring);
        }
        ring.clear();
    };

    for (int i = 0; i < features.size(); i++)
    {
        Dictionary feat = features[i];
        Dictionary geom = feat.get("geometry", Dictionary());
        String gtype = String(geom.get("type", ""));
        Array coords = geom.get("coordinates", Array());

        if (gtype == "Polygon")
        {
            // coordinates: [ring, hole, ...] — outer ring only.
            if (coords.size() > 0)
            {
                Array pts = coords[0];
                for (int p = 0; p < pts.size(); p++)
                {
                    Array xy = pts[p];
                    Vector2 lv = wgs84_to_lv95(double(xy[1]), double(xy[0]));
                    ring.push_back(lv);
                }
            }
            flush_ring();
        }
        else if (gtype == "MultiPolygon")
        {
            for (int poly = 0; poly < coords.size(); poly++)
            {
                Array rings = coords[poly];
                if (rings.size() > 0)
                {
                    Array pts = rings[0];
                    for (int p = 0; p < pts.size(); p++)
                    {
                        Array xy = pts[p];
                        Vector2 lv = wgs84_to_lv95(double(xy[1]), double(xy[0]));
                        ring.push_back(lv);
                    }
                }
                flush_ring();
            }
        }
    }
}

// ── Tile streaming ──────────────────────────────────────────────────────────

Ref<ImageTexture> VfrMap::get_tile(int ei, int ni, int level)
{
    for (size_t i = 0; i < cache_index.size(); i++)
    {
        if (cache_index[i].key.ei == ei && cache_index[i].key.ni == ni && cache_index[i].key.level == level)
        {
            TileEntry &entry = *cache_index[i].it;
            // Promote to most-recently-used.
            cache_lru.splice(cache_lru.begin(), cache_lru, cache_index[i].it);
            return entry.tex;
        }
    }
    return Ref<ImageTexture>();
}

void VfrMap::load_tile_into_cache(const TileKey &key, const Ref<ImageTexture> &tex)
{
    if (tex.is_null())
    {
        return;
    }
    // Evict least-recently-used entries over the cap. The back of the LRU
    // list is always the victim's index slot (iterators are stable until
    // pop), so match by iterator first, key as fallback.
    while (cache_lru.size() >= size_t(MAX_CACHE_TILES))
    {
        TileEntry victim = cache_lru.back();
        std::list<TileEntry>::iterator victim_it = std::prev(cache_lru.end());
        cache_lru.pop_back();
        for (size_t i = 0; i < cache_index.size(); i++)
        {
            if (cache_index[i].it == victim_it ||
                (cache_index[i].key.ei == victim.ei && cache_index[i].key.ni == victim.ni &&
                 cache_index[i].key.level == victim.level))
            {
                cache_index.erase(cache_index.begin() + int(i));
                break;
            }
        }
    }

    TileEntry entry;
    entry.ei = key.ei;
    entry.ni = key.ni;
    entry.level = key.level;
    entry.tex = tex;
    cache_lru.push_front(entry);
    TileSlot slot;
    slot.key = key;
    slot.it = cache_lru.begin();
    cache_index.push_back(slot);
}

bool VfrMap::try_load_tile(const TileKey &key)
{
    int cell = level_cell_m(key.level);
    String fname = String("map_") + String::num_int64((long long)(key.ei * cell)) + "_" + String::num_int64((long long)(key.ni * cell)) + ".jpg";
    String subdir = LEVELS[std::max(0, std::min(2, key.level))].subdir;
    String suffix = (subdir[0] == '\0') ? fname : (String("/") + subdir + "/" + fname);

    for (int i = 0; i < tile_paths.size(); i++)
    {
        String base = tile_paths[i];
        if (base.is_empty())
        {
            continue;
        }
        Ref<FileAccess> f = FileAccess::open(base.path_join(suffix), FileAccess::READ);
        if (f.is_null())
        {
            continue; // next priority source
        }
        PackedByteArray data = f->get_buffer(f->get_length());
        Ref<Image> img = memnew(Image);
        if (img->load_jpg_from_buffer(data) != OK)
        {
            return false;
        }
        // Tile files store pixel(0,0) at the SE corner with rows S->N and
        // cols E->W. Godot textures need top-left = NW: flip both axes.
        img->flip_y();
        img->flip_x();
        Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
        load_tile_into_cache(key, tex);
        return true;
    }
    return false;
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void VfrMap::_draw()
{
    double s = frame_size_px();
    Rect2 frame(0, 0, s, s);

    // Frame background + border.
    draw_rect(frame, C_BG);
    draw_rect(frame, C_BORDER, false, 2.0);

    Ref<Font> font = ThemeDB::get_singleton()->get_default_theme()->get_font("font", "Label");

    // Title bar: "VFR MAP" left, view width right.
    Rect2 title(0, 0, s, TITLE_H);
    draw_rect(title, C_TITLE_BG);
    if (font.is_valid())
    {
        draw_string(font, Vector2(8, 18), "VFR MAP", HORIZONTAL_ALIGNMENT_LEFT, -1, 14, C_TITLE_TEXT);
        String width_txt = String::num_int64((int)(VIEW_WIDTHS_M[zoom_index] / 1000.0)) + " km";
        double tw = double(width_txt.length()) * 7.5; // ~px per char at size 14
        draw_string(font, Vector2(s - 8 - tw, 18), width_txt, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, C_TITLE_TEXT);
    }

    // Map area (below the title bar).
    Rect2 map_area(MARGIN, TITLE_H + MARGIN / 2.0, s - 2.0 * MARGIN, s - TITLE_H - 1.5 * MARGIN);
    frame_center_px = map_area.get_center();
    double view_m = VIEW_WIDTHS_M[zoom_index];
    px_per_m = map_area.size.x / view_m;

    if (!has_position)
    {
        return; // no aircraft position yet — just the empty frame
    }

    draw_rect(map_area, C_NO_TILE); // fill under missing tiles

    // ── Rotated map world (heading-up) ────────────────────────────────────
    // Local screen axes: +x right, +y down. Map axes in meters:
    //   east  = (cosH, sinH)
    //   north = (-sinH, cosH)
    // A point at LV95 (e, n) projects to:
    //   px = center + ((e - ac_e) * cosH + (n - ac_n) * sinH) * ppm
    //   py = center + ((ac_n - n) * cosH - (e - ac_e) * sinH) * ppm
    double ch = std::cos(heading_rad);
    double sh = std::sin(heading_rad);

    auto project = [&](double e, double n, Vector2 &out) {
        double de = e - ac_e;
        double dn = n - ac_n;
        // right-handed heading-up rotation: +x screen = right of aircraft,
        // -y screen = forward (heading direction).
        out.x = frame_center_px.x + (de * ch - dn * sh) * px_per_m;
        out.y = frame_center_px.y - (de * sh + dn * ch) * px_per_m;
    };

    // Tiles: iterate the level's grid cells covering the view.
    int level = pick_level();
    int cell = level_cell_m(level);
    double half_view = view_m / 2.0;
    // Farthest visible corner is half_view*sqrt(2); add a half-cell margin
    // so rotation never exposes un-drawn edges.
    double extent = half_view * 1.5 + cell * 0.5;

    int ei_min = int(std::floor((ac_e - extent) / (double)cell));
    int ei_max = int(std::floor((ac_e + extent) / (double)cell));
    int ni_min = int(std::floor((ac_n - extent) / (double)cell));
    int ni_max = int(std::floor((ac_n + extent) / (double)cell));

    int tiles_drawn = 0;
    pending_loads = 0;
    for (int ni = ni_min; ni <= ni_max && tiles_drawn < MAX_TILES_PER_DRAW; ni++)
    {
        for (int ei = ei_min; ei <= ei_max && tiles_drawn < MAX_TILES_PER_DRAW; ei++)
        {
            TileKey key{ei, ni, level};
            Ref<ImageTexture> tex = get_tile(ei, ni, level);
            if (tex.is_null() && pending_loads < MAX_LOADS_PER_FRAME)
            {
                if (try_load_tile(key))
                {
                    tex = get_tile(ei, ni, level);
                    pending_loads++;
                }
            }
            if (tex.is_null())
            {
                continue; // no data here — background fill shows through
            }

            // Tile LV95 corners: file name encodes the SW corner of the cell
            // (same convention as orthophoto/terrain tiles). The cell spans
            // easting [ei*cell, (ei+1)*cell], northing [ni*cell, (ni+1)*cell].
            double e_west = ei * (double)cell;    // min E (file name)
            double n_south = ni * (double)cell;   // min N (file name)
            double e_east = e_west + cell;        // max E
            double n_north = n_south + cell;      // max N

            Vector2 p_se, p_sw, p_ne, p_nw;
            project(e_east, n_south, p_se);
            project(e_west, n_south, p_sw);
            project(e_east, n_north, p_ne);
            project(e_west, n_north, p_nw);

            // Texture corners (after the double flip): (0,0)=NW, (w,0)=NE,
            // (w,h)=SE, (0,h)=SW. Map them onto the projected quad with a
            // textured polygon: uv.x = E->W fraction, uv.y = N->S fraction.
            PackedVector2Array pts;
            pts.push_back(p_nw);
            pts.push_back(p_ne);
            pts.push_back(p_se);
            pts.push_back(p_sw);

            PackedVector2Array uvs;
            double w = double(tex->get_width());
            double h = double(tex->get_height());
            uvs.push_back(Vector2(0.0, 0.0));       // NW -> uv (0,0)
            uvs.push_back(Vector2(w, 0.0));         // NE -> uv (w,0)
            uvs.push_back(Vector2(w, h));           // SE -> uv (w,h)
            uvs.push_back(Vector2(0.0, h));         // SW -> uv (0,h)

            draw_colored_polygon(pts, Color(1, 1, 1, 1), uvs, tex);
            tiles_drawn++;
        }
    }

    // Canton borders (rotated with the map world).
    for (const std::vector<Vector2> &ring : canton_rings)
    {
        PackedVector2Array line;
        bool any_in_view = false;
        for (const Vector2 &pt : ring)
        {
            Vector2 sp;
            project(pt.x, pt.y, sp);
            if (map_area.grow(16.0).has_point(sp))
            {
                any_in_view = true;
            }
            line.push_back(sp);
        }
        if (any_in_view)
        {
            draw_polyline(line, C_CANTON, 1.0, false);
        }
    }

    // Airport symbols (rotated with the map world).
    visible_airports = 0;
    for (const Airport &ap : airports)
    {
        Vector2 sp;
        project(ap.e, ap.n, sp);
        if (!map_area.has_point(sp))
        {
            continue;
        }
        visible_airports++;
        Color c = ap.international ? C_AIRPORT_INTL : C_AIRPORT_GA;
        if (ap.international)
        {
            draw_rect(Rect2(sp.x - 4, sp.y - 3, 8, 6), c);
        }
        else
        {
            draw_arc(sp, 3.0, 0, TAU, 16, c, 1.5, false);
        }
    }

    // ── Screen-aligned overlays ───────────────────────────────────────────
    // Range rings: smallest step that keeps the ring count <= 4 (so the frame
    // shows 2-4 rings, not a dense stack of them).
    double max_ring_m = (map_area.size.x / 2.0) / px_per_m; // meters to edge
    const double ring_steps[] = { 500, 1000, 2000, 5000, 10000, 20000, 40000 };
    double ring_step = ring_steps[6];
    for (double st : ring_steps)
    {
        if (max_ring_m / st <= 4.0)
        {
            ring_step = st;
            break;
        }
    }
    for (double r = ring_step; r <= max_ring_m * 1.05; r += ring_step)
    {
        double rr = r * px_per_m;
        draw_arc(frame_center_px, rr, 0, TAU, 96, C_RINGS, 1.0, false);
        if (font.is_valid())
        {
            String lbl = (r >= 1000.0)
                ? (String::num_int64((int)(r / 1000.0)) + " km")
                : (String::num_int64((int)r) + " m");
            double lw = double(lbl.length()) * 6.5; // ~px per char at size 11
            draw_string(font, frame_center_px + Vector2(-lw / 2.0, -rr - 4), lbl,
                        HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_RING_LABEL);
        }
    }

    // Course line: dashed, from the aircraft symbol to ~85% of the map radius.
    {
        double len = (map_area.size.x / 2.0) * 0.85;
        Vector2 from(frame_center_px.x, frame_center_px.y - 14);
        Vector2 to(frame_center_px.x, frame_center_px.y - len);
        double seg = 10.0;
        double gap = 6.0;
        double d = 0.0;
        while (d < len - 14.0)
        {
            double d2 = std::min(d + seg, len - 14.0);
            draw_line(from + Vector2(0, -(d)), from + Vector2(0, -(d2)), C_COURSE, 1.5, false);
            d = d2 + gap;
        }
    }

    // Scale bar (bottom-left of the map area): a bar exactly ring_step long.
    {
        double bar_px = ring_step * px_per_m;
        Vector2 bl(map_area.position.x + 10, map_area.position.y + map_area.size.y - 12);
        draw_line(bl, bl + Vector2(bar_px, 0), Color(1, 1, 1, 0.9), 2.0, false);
        draw_line(bl, bl + Vector2(0, 5), Color(1, 1, 1, 0.9), 2.0, false);
        draw_line(bl + Vector2(bar_px, 0), bl + Vector2(bar_px, 5), Color(1, 1, 1, 0.9), 2.0, false);
        if (font.is_valid())
        {
            String lbl = (ring_step >= 1000.0)
                ? (String::num_int64((int)(ring_step / 1000.0)) + " km")
                : (String::num_int64((int)ring_step) + " m");
            double lw = double(lbl.length()) * 6.5;
            draw_string(font, bl + Vector2(bar_px / 2.0 - lw / 2.0, -4), lbl,
                        HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_RING_LABEL);
        }
    }

    // ICAO labels (screen-aligned, next to the projected airport positions).
    if (font.is_valid())
    {
        for (const Airport &ap : airports)
        {
            Vector2 sp;
            project(ap.e, ap.n, sp);
            if (!map_area.grow(8.0).has_point(sp))
            {
                continue;
            }
            double off = ap.international ? 7.0 : 6.0;
            Vector2 pos(sp.x + off, sp.y - off);
            // Shadow pass for legibility over bright terrain.
            draw_string(font, pos + Vector2(1, 1), ap.icao, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_LABEL_SHADOW);
            draw_string(font, pos, ap.icao, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_AIRPORT_INTL);
        }
    }

    // Aircraft symbol: centered triangle pointing up (heading-up).
    {
        PackedVector2Array tri;
        tri.push_back(Vector2(frame_center_px.x, frame_center_px.y - 9));
        tri.push_back(Vector2(frame_center_px.x + 6, frame_center_px.y + 8));
        tri.push_back(Vector2(frame_center_px.x - 6, frame_center_px.y + 8));
        draw_colored_polygon(tri, C_AC_BODY);
        PackedVector2Array edge;
        edge.push_back(tri[0]);
        edge.push_back(tri[1]);
        edge.push_back(tri[2]);
        edge.push_back(tri[0]);
        draw_polyline(edge, C_AC_EDGE, 1.5, false);
    }
}

} // namespace jsbgodot
