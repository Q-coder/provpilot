#include "key_bindings.h"
#include "vfr_map.h"
#include "geo.h"
#include "navaids_lib.h"
#include "geo_magnetic.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace godot;

namespace jsbgodot
{

// ── Constants ───────────────────────────────────────────────────────────────

const double VfrMap::VIEW_WIDTHS_M[VfrMap::N_ZOOM] = { 5000.0, 10000.0, 20000.0, 40000.0, 80000.0, 160000.0, 250000.0 };
const int VfrMap::FRAME_SIZES_PX[3] = { 240, 320, 400 };

// Map tile pyramid (scripts/generate_vfr_map_tiles.py): same LV95 grid family
// as the orthophoto layer — file names encode the SW corner of each cell
// (min easting / min northing).
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
    const Color C_VOR(0.55, 0.78, 1.0, 1.0);          // VOR / DME symbols and labels
    const Color C_NDB(1.0, 0.6, 0.9, 1.0);            // NDB symbols and labels
    const Color C_ILS(0.65, 1.0, 0.75, 0.9);          // ILS feathers and labels
    const Color C_ILS_FILL(0.65, 1.0, 0.75, 0.25);    // shaded half of a feather
    const Color C_NO_TILE(0.13, 0.17, 0.14, 1.0);     // missing-tile fill

    constexpr int TITLE_H = 26;                        // title bar height (px)
    constexpr double BG_BOX_X = 80.0;                  // title bar: the terrain checkbox
    constexpr double BG_LABEL_MIN_W = 320.0;           // ... gets its label from this frame width
    constexpr int MARGIN = 8;                          // frame padding (px)

    constexpr double TAU = 6.283185307179586;

    double norm_pi(double a)
    {
        while (a > M_PI) a -= 2.0 * M_PI;
        while (a < -M_PI) a += 2.0 * M_PI;
        return a;
    }

    // ── Map-area clipping ─────────────────────────────────────────────────
    // CanvasItem::_draw() has no built-in scissor, so tile quads and border
    // segments are clipped against the map rect manually to keep everything
    // inside the frame.

    // Heading-up rotation of an LV95 offset (meters) into local frame pixels.
    // +x screen = right of the aircraft, -y screen = forward (heading).
    void project_offset(double de, double dn, double ch, double sh, double px_per_m,
                        const Vector2 &center_px, Vector2 &out)
    {
        out.x = center_px.x + (de * ch - dn * sh) * px_per_m;
        out.y = center_px.y - (de * sh + dn * ch) * px_per_m;
    }

    // Sutherland-Hodgman clip of a convex polygon (pts/uvs parallel arrays)
    // against an axis-aligned rect. UVs interpolate linearly along clipped
    // edges — exact for canvas textured polygons, which rasterize per-vertex
    // UVs. Returns false when nothing is left.
    bool clip_polygon_to_rect(const Vector2 *pts_in, const Vector2 *uvs_in, int n_in,
                              const Rect2 &rect,
                              std::vector<Vector2> &out_pts, std::vector<Vector2> &out_uvs)
    {
        struct Edge { double limit; bool keep_min; bool is_x; };
        const Edge edges[4] = {
            { rect.position.x, true,  true  }, // left:   x >= min
            { rect.get_end().x,      false, true  }, // right:  x <= max
            { rect.position.y, true,  false }, // top:    y >= min
            { rect.get_end().y,      false, false }, // bottom: y <= max
        };
        std::vector<Vector2> cur_pts;
        std::vector<Vector2> cur_uvs;
        for (int i = 0; i < n_in; i++)
        {
            cur_pts.push_back(pts_in[i]);
            cur_uvs.push_back(uvs_in[i]);
        }
        for (const Edge &edge : edges)
        {
            std::vector<Vector2> nxt_pts;
            std::vector<Vector2> nxt_uvs;
            const size_t n = cur_pts.size();
            for (size_t i = 0; i < n; i++)
            {
                const Vector2 &a = cur_pts[i];
                const Vector2 &b = cur_pts[(i + 1) % n];
                double av = edge.is_x ? a.x : a.y;
                double bv = edge.is_x ? b.x : b.y;
                bool a_in = edge.keep_min ? (av >= edge.limit) : (av <= edge.limit);
                bool b_in = edge.keep_min ? (bv >= edge.limit) : (bv <= edge.limit);
                if (a_in)
                {
                    nxt_pts.push_back(a);
                    nxt_uvs.push_back(cur_uvs[i]);
                }
                if (a_in != b_in)
                {
                    double t = (edge.limit - av) / (bv - av);
                    nxt_pts.push_back(a.lerp(b, t));
                    nxt_uvs.push_back(cur_uvs[i].lerp(cur_uvs[(i + 1) % n], t));
                }
            }
            cur_pts.swap(nxt_pts);
            cur_uvs.swap(nxt_uvs);
            if (cur_pts.empty())
            {
                return false;
            }
        }
        if (cur_pts.size() < 3)
        {
            return false; // degenerate sliver — skip
        }
        out_pts = std::move(cur_pts);
        out_uvs = std::move(cur_uvs);
        return true;
    }

    // Liang-Barsky clip of a segment against an axis-aligned rect. Returns
    // false when the segment is fully outside.
    bool clip_segment_to_rect(const Vector2 &a, const Vector2 &b, const Rect2 &rect,
                              Vector2 &out_a, Vector2 &out_b)
    {
        double t0 = 0.0;
        double t1 = 1.0;
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        struct Clip { double p; double q; };
        const Clip clips[4] = {
            { -dx,      a.x - rect.position.x }, // left:   x >= min
            { dx,       rect.get_end().x - a.x },      // right:  x <= max
            { -dy,      a.y - rect.position.y }, // top:    y >= min
            { dy,       rect.get_end().y - a.y },      // bottom: y <= max
        };
        for (const Clip &c : clips)
        {
            if (c.p == 0.0)
            {
                if (c.q < 0.0)
                {
                    return false; // parallel and outside
                }
            }
            else
            {
                double t = c.q / c.p;
                if (c.p < 0.0)
                {
                    if (t > t1)
                    {
                        return false;
                    }
                    if (t > t0)
                    {
                        t0 = t;
                    }
                }
                else
                {
                    if (t < t0)
                    {
                        return false;
                    }
                    if (t < t1)
                    {
                        t1 = t;
                    }
                }
            }
        }
        out_a = a.lerp(b, t0);
        out_b = a.lerp(b, t1);
        return true;
    }
} // namespace

// ── Binding ─────────────────────────────────────────────────────────────────

void VfrMap::_bind_methods()
{
    ClassDB::bind_method(D_METHOD("setup", "config_path"), &VfrMap::setup);
    ClassDB::bind_method(D_METHOD("set_tile_paths", "map_dirs"), &VfrMap::set_tile_paths);
    ClassDB::bind_method(D_METHOD("apply_config"), &VfrMap::apply_config);
    ClassDB::bind_method(D_METHOD("update_lv95", "e", "n", "heading_deg", "ui_visible"), &VfrMap::update_lv95);
    ClassDB::bind_method(D_METHOD("get_cached_tile_count"), &VfrMap::get_cached_tile_count);
    ClassDB::bind_method(D_METHOD("get_visible_airport_count"), &VfrMap::get_visible_airport_count);
    ClassDB::bind_method(D_METHOD("get_visible_navaid_count"), &VfrMap::get_visible_navaid_count);
    ClassDB::bind_method(D_METHOD("get_zoom_index"), &VfrMap::get_zoom_index);
    ClassDB::bind_method(D_METHOD("select_at", "local_px"), &VfrMap::select_at);
    ClassDB::bind_method(D_METHOD("select_airport", "icao"), &VfrMap::select_airport);
    ClassDB::bind_method(D_METHOD("close_info"), &VfrMap::close_info);
    ClassDB::bind_method(D_METHOD("get_info_text"), &VfrMap::get_info_text);
    ClassDB::bind_method(D_METHOD("_on_info_close"), &VfrMap::_on_info_close);
    ClassDB::bind_method(D_METHOD("get_view_width_m"), &VfrMap::get_view_width_m);
    ClassDB::bind_method(D_METHOD("toggle_visible"), &VfrMap::toggle_visible);
    ClassDB::bind_method(D_METHOD("set_background_enabled", "on"), &VfrMap::set_background_enabled);
    ClassDB::bind_method(D_METHOD("is_background_enabled"), &VfrMap::is_background_enabled);
    ClassDB::bind_method(D_METHOD("get_pending_tile_count"), &VfrMap::get_pending_tile_count);
    ClassDB::bind_method(D_METHOD("pan_by", "delta_px"), &VfrMap::pan_by);
    ClassDB::bind_method(D_METHOD("reset_pan"), &VfrMap::reset_pan);
    ClassDB::bind_method(D_METHOD("set_north_up", "on"), &VfrMap::set_north_up);
    ClassDB::bind_method(D_METHOD("is_north_up"), &VfrMap::is_north_up);
    ClassDB::bind_method(D_METHOD("is_panned"), &VfrMap::is_panned);
    // The map key (N) flipped the frame: the host mirrors it into the
    // SettingsStore so Settings > Map and the next Save show the same state.
    ADD_SIGNAL(MethodInfo("enabled_changed", PropertyInfo(Variant::BOOL, "enabled")));
    ClassDB::bind_method(D_METHOD("zoom_in"), &VfrMap::zoom_in);
    ClassDB::bind_method(D_METHOD("zoom_out"), &VfrMap::zoom_out);
    ClassDB::bind_method(D_METHOD("get_heading_rad"), &VfrMap::get_heading_rad);
    ClassDB::bind_method(D_METHOD("get_projected_px", "e", "n"), &VfrMap::get_projected_px);
    ClassDB::bind_method(D_METHOD("is_map_enabled"), &VfrMap::is_map_enabled);
    ClassDB::bind_method(D_METHOD("set_frame_size", "size_px"), &VfrMap::set_frame_size);
    ClassDB::bind_method(D_METHOD("set_terrain_paths", "terrain_dirs"), &VfrMap::set_terrain_paths);
    ClassDB::bind_method(D_METHOD("move_frame_by", "delta_px"), &VfrMap::move_frame_by);
    ClassDB::bind_method(D_METHOD("resize_frame_by", "delta_px"), &VfrMap::resize_frame_by);
    ClassDB::bind_method(D_METHOD("resize_frame_left_by", "delta_px"), &VfrMap::resize_frame_left_by);
    ClassDB::bind_method(D_METHOD("get_frame_size"), &VfrMap::get_frame_size);
    ClassDB::bind_method(D_METHOD("get_frame_offset"), &VfrMap::get_frame_offset);
    ClassDB::bind_method(D_METHOD("_on_viewport_resized"), &VfrMap::_on_viewport_resized);
}

// ── Clipped map world ───────────────────────────────────────────────────────

VfrMapCanvas::VfrMapCanvas()
{
    set_clip_contents(true);
    set_mouse_filter(Control::MOUSE_FILTER_IGNORE); // the frame handles the mouse
}

void VfrMapCanvas::_draw()
{
    if (owner != nullptr)
    {
        owner->draw_world(this);
    }
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
    if (canvas == nullptr)
    {
        canvas = memnew(VfrMapCanvas);
        canvas->owner = this;
        add_child(canvas);
    }
    if (info_label == nullptr)
    {
        info_label = memnew(RichTextLabel);
        info_label->set_use_bbcode(true);
        info_label->set_scroll_active(true);
        info_label->add_theme_font_size_override("normal_font_size", 12);
        info_label->add_theme_font_size_override("bold_font_size", 12);
        info_label->add_theme_color_override("default_color", Color(0.88, 0.92, 1.0));
        info_label->set_visible(false);
        add_child(info_label);
        info_close = memnew(Button);
        info_close->set_text("x");
        info_close->set_flat(true);
        info_close->set_focus_mode(Control::FOCUS_NONE);
        info_close->add_theme_font_size_override("font_size", 13);
        info_close->set_visible(false);
        info_close->connect("pressed", Callable(this, "_on_info_close"));
        add_child(info_close);
    }
    load_config();
    load_static_data();
    set_visible(enabled);
    _reposition();
    Viewport *vp = get_viewport();
    if (vp != nullptr && !vp->is_connected("size_changed", Callable(this, "_on_viewport_resized")))
    {
        vp->connect("size_changed", Callable(this, "_on_viewport_resized"));
    }
    request_redraw();
}

void VfrMap::_on_viewport_resized()
{
    _reposition(); // keep the frame inside the window
}

// Anchored to the parent's top-right corner: frame_right / frame_top are
// the gaps to that corner, frame_w / frame_h the size. Clamped so the whole
// frame stays inside the parent (the HUD covers the viewport).
void VfrMap::_reposition()
{
    // A parent smaller than the minimum frame (the 64 x 64 headless window
    // of the smoke tests, a window mid-resize) does not clamp anything.
    const Vector2 area = get_parent_area_size();
    frame_w = std::max(MIN_FRAME_PX, frame_w);
    frame_h = std::max(MIN_FRAME_PX, frame_h);
    if (area.x >= MIN_FRAME_PX && area.y >= MIN_FRAME_PX)
    {
        frame_w = std::min(frame_w, std::max(MIN_FRAME_PX, double(area.x) - info_w()));
        frame_h = std::min(frame_h, double(area.y));
        frame_right = std::max(0.0, std::min(frame_right, double(area.x) - frame_w - info_w()));
        frame_top = std::max(0.0, std::min(frame_top, double(area.y) - frame_h));
    }
    set_anchors_preset(Control::PRESET_TOP_RIGHT);
    set_offset(Side::SIDE_LEFT, -frame_right - frame_w - info_w()); // the details panel widens the frame to the left
    set_offset(Side::SIDE_TOP, frame_top);
    set_offset(Side::SIDE_RIGHT, -frame_right);
    set_offset(Side::SIDE_BOTTOM, frame_top + frame_h);
    place_canvas();
}

Rect2 VfrMap::compute_map_area() const
{
    return Rect2(MARGIN, TITLE_H + MARGIN / 2.0, frame_w - 2.0 * MARGIN, frame_h - TITLE_H - 1.5 * MARGIN);
}

void VfrMap::place_canvas()
{
    map_area_rect = compute_map_area();
    if (canvas != nullptr)
    {
        canvas->set_position(map_area_rect.position);
        canvas->set_size(map_area_rect.size);
    }
    place_info();
}

void VfrMap::request_redraw()
{
    drawn_valid = false;
    queue_redraw();
    if (canvas != nullptr)
    {
        canvas->queue_redraw();
    }
}

bool VfrMap::in_resize_grip(const Vector2 &local_px) const
{
    return local_px.x >= frame_w + info_w() - RESIZE_GRIP_PX && local_px.y >= frame_h - RESIZE_GRIP_PX;
}

Rect2 VfrMap::center_button_rect() const
{
    return Rect2(map_area_rect.position + map_area_rect.size - Vector2(30.0, 30.0), Vector2(24.0, 24.0));
}

Rect2 VfrMap::heading_button_rect() const
{
    Rect2 r = center_button_rect();
    r.position.x -= 28.0;
    return r;
}

Rect2 VfrMap::north_button_rect() const
{
    Rect2 r = center_button_rect();
    r.position.x -= 56.0;
    return r;
}

void VfrMap::set_north_up(bool on)
{
    if (north_up == on)
    {
        return;
    }
    north_up = on;
    persist_layout();
    request_redraw();
}

bool VfrMap::is_north_up() const { return north_up; }

void VfrMap::update_frame_center()
{
    frame_center_px = map_area_rect.get_center();
    if (panned && has_position)
    {
        // the view centre is the panned spot: the aircraft sits off-centre by its offset from it
        Vector2 off;
        project_offset(ac_e - pan_e, ac_n - pan_n, std::cos(map_rot()), std::sin(map_rot()), px_per_m, Vector2(), off);
        frame_center_px += off;
    }
}

void VfrMap::pan_by(const Vector2 &delta_px)
{
    if (!has_position || px_per_m <= 0.0)
    {
        return;
    }
    if (!panned)
    {
        panned = true;
        pan_e = ac_e;
        pan_n = ac_n;
    }
    // inverse of project_offset: the content follows the mouse, the centre goes the other way
    const double ch = std::cos(map_rot()), sh = std::sin(map_rot());
    const double x = -delta_px.x / px_per_m, y = delta_px.y / px_per_m;
    pan_e += x * ch + y * sh;
    pan_n += -x * sh + y * ch;
    request_redraw();
}

void VfrMap::reset_pan()
{
    if (panned)
    {
        panned = false;
        request_redraw();
    }
}

bool VfrMap::is_panned() const { return panned; }

bool VfrMap::in_background_box(const Vector2 &local_px) const
{
    const double w = frame_w >= BG_LABEL_MIN_W ? 70.0 : 20.0;
    return local_px.y < TITLE_H && local_px.x >= BG_BOX_X - 4.0 && local_px.x <= BG_BOX_X - 4.0 + w;
}

bool VfrMap::in_resize_grip_left(const Vector2 &local_px) const
{
    return local_px.x < RESIZE_GRIP_PX && local_px.y >= frame_h - RESIZE_GRIP_PX;
}

void VfrMap::move_frame_by(const Vector2 &delta_px)
{
    frame_right -= delta_px.x; // dragging right shrinks the gap to the right edge
    frame_top += delta_px.y;
    _reposition();
    persist_layout();
    request_redraw();
}

void VfrMap::resize_frame_by(const Vector2 &delta_px)
{
    // The bottom-right corner follows the mouse: the left and top edges stay.
    const double w0 = frame_w;
    frame_w = std::max(MIN_FRAME_PX, frame_w + delta_px.x);
    frame_h = std::max(MIN_FRAME_PX, frame_h + delta_px.y);
    frame_right -= frame_w - w0;
    _reposition();
    persist_layout();
    request_redraw();
}

void VfrMap::resize_frame_left_by(const Vector2 &delta_px)
{
    // The bottom-left corner follows the mouse: the right and top edges stay
    // (frame_right is the gap to the parent's right edge, so it is untouched).
    frame_w = std::max(MIN_FRAME_PX, frame_w - delta_px.x);
    frame_h = std::max(MIN_FRAME_PX, frame_h + delta_px.y);
    _reposition();
    persist_layout();
    request_redraw();
}

Vector2 VfrMap::get_frame_size() const { return Vector2(frame_w, frame_h); }
Vector2 VfrMap::get_frame_offset() const { return Vector2(frame_right, frame_top); }

void VfrMap::_unhandled_input(const Ref<InputEvent> &event)
{
    if (ui_visible)
    {
        return; // full-screen UI owns the input
    }

    InputEventKey *key = Object::cast_to<InputEventKey>(event.ptr());
    if (key == nullptr || !key->is_pressed() || key->is_echo())
    {
        return;
    }
    // Keys by the keyboard mapping (Settings > Keyboard). The map toggle
    // must work while hidden too — a hidden Control still receives
    // unhandled input, so this is the only way to re-show the frame.
    if (KeyBindings::matches("map_toggle", event))
    {
        toggle_visible();
        get_viewport()->set_input_as_handled();
    }
    else if (enabled && KeyBindings::matches("map_zoom_out", event))
    {
        zoom_out();
        get_viewport()->set_input_as_handled();
    }
    else if (enabled && KeyBindings::matches("map_zoom_in", event))
    {
        // "=" and "Shift+=": the + character of any layout (Shift+1 on a Swiss one)
        zoom_in();
        get_viewport()->set_input_as_handled();
    }
}

// N in flight. The state persists with the layout ([map] map_enabled, the
// key Settings > Map writes) so apply_config() after the settings screen, or
// the next launch, brings the frame back as the pilot left it.
void VfrMap::toggle_visible()
{
    enabled = !enabled;
    set_visible(enabled);
    persist_layout();
    emit_signal("enabled_changed", enabled);
    request_redraw();
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

    // Title bar drag moves the frame, a drag from the bottom-right corner
    // resizes it; the pointer shape announces both. Event positions are in
    // this control's local space (the GUI router transformed them), deltas
    // use the global position so a moving frame does not feed back.
    InputEventMouseMotion *mm = Object::cast_to<InputEventMouseMotion>(event.ptr());
    if (mm != nullptr)
    {
        if (drag_mode != DRAG_NONE)
        {
            const Vector2 d = mm->get_global_position() - drag_start_mouse;
            if (drag_mode == DRAG_PAN)
            {
                if (!drag_pan_moved && d.length() < 4.0)
                {
                    return; // still a click
                }
                drag_pan_moved = true;
                panned = true;
                pan_e = drag_start_pan_e;
                pan_n = drag_start_pan_n;
                pan_by(d);
                set_default_cursor_shape(Control::CURSOR_DRAG);
                get_viewport()->set_input_as_handled();
                return;
            }
            if (drag_mode == DRAG_MOVE)
            {
                frame_right = drag_start_right - d.x;
                frame_top = drag_start_top + d.y;
            }
            else if (drag_mode == DRAG_RESIZE)
            {
                frame_w = std::max(MIN_FRAME_PX, drag_start_w + d.x);
                frame_h = std::max(MIN_FRAME_PX, drag_start_h + d.y);
                frame_right = drag_start_right - (frame_w - drag_start_w);
            }
            else // DRAG_RESIZE_LEFT: the right edge stays
            {
                frame_w = std::max(MIN_FRAME_PX, drag_start_w - d.x);
                frame_h = std::max(MIN_FRAME_PX, drag_start_h + d.y);
                frame_right = drag_start_right;
            }
            _reposition();
            request_redraw();
            get_viewport()->set_input_as_handled();
            return;
        }
        const Vector2 lp = mm->get_position();
        if (in_resize_grip(lp))
        {
            set_default_cursor_shape(Control::CURSOR_FDIAGSIZE);
        }
        else if (in_resize_grip_left(lp))
        {
            set_default_cursor_shape(Control::CURSOR_BDIAGSIZE);
        }
        else if (in_background_box(lp) || center_button_rect().has_point(lp) || north_button_rect().has_point(lp) || heading_button_rect().has_point(lp))
        {
            set_default_cursor_shape(Control::CURSOR_POINTING_HAND);
        }
        else if (lp.y < TITLE_H)
        {
            set_default_cursor_shape(Control::CURSOR_MOVE);
        }
        else
        {
            set_default_cursor_shape(Control::CURSOR_ARROW);
        }
        return;
    }

    InputEventMouseButton *btn = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (btn == nullptr)
    {
        return;
    }
    if (btn->get_button_index() == MOUSE_BUTTON_LEFT)
    {
        if (btn->is_pressed())
        {
            const Vector2 lp = btn->get_position();
            if (in_background_box(lp))
            {
                set_background_enabled(!background);
                get_viewport()->set_input_as_handled();
                return;
            }
            int mode = DRAG_NONE;
            if (in_resize_grip(lp))
            {
                mode = DRAG_RESIZE;
            }
            else if (in_resize_grip_left(lp))
            {
                mode = DRAG_RESIZE_LEFT;
            }
            else if (lp.y < TITLE_H)
            {
                mode = DRAG_MOVE;
            }
            if (mode == DRAG_NONE && center_button_rect().has_point(lp))
            {
                reset_pan();
                get_viewport()->set_input_as_handled();
                return;
            }
            if (mode == DRAG_NONE && (north_button_rect().has_point(lp) || heading_button_rect().has_point(lp)))
            {
                set_north_up(north_button_rect().has_point(lp));
                get_viewport()->set_input_as_handled();
                return;
            }
            if (mode == DRAG_NONE && map_area_rect.has_point(lp))
            {
                // a drag pans the map; a plain click (decided on release) selects a symbol
                mode = DRAG_PAN;
                drag_pan_moved = false;
                drag_start_local = lp;
                drag_start_pan_e = panned ? pan_e : ac_e;
                drag_start_pan_n = panned ? pan_n : ac_n;
            }
            if (mode != DRAG_NONE)
            {
                drag_mode = mode;
                drag_start_mouse = btn->get_global_position();
                drag_start_right = frame_right;
                drag_start_top = frame_top;
                drag_start_w = frame_w;
                drag_start_h = frame_h;
                get_viewport()->set_input_as_handled();
            }
        }
        else if (drag_mode == DRAG_PAN)
        {
            drag_mode = DRAG_NONE;
            if (!drag_pan_moved)
            {
                // a click on an airport or navaid symbol opens its details, one on the bare map closes them
                select_at(drag_start_local);
            }
            get_viewport()->set_input_as_handled();
        }
        else if (drag_mode != DRAG_NONE)
        {
            drag_mode = DRAG_NONE;
            persist_layout();
            get_viewport()->set_input_as_handled();
        }
        return;
    }
    if (!btn->is_pressed())
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
        get_viewport()->set_input_as_handled();
    }
}

void VfrMap::zoom_step(int step)
{
    int next = std::max(0, std::min(N_ZOOM - 1, zoom_index + step));
    if (next != zoom_index)
    {
        zoom_index = next;
        persist_layout();
        request_redraw();
    }
}

// ── Setup / config ──────────────────────────────────────────────────────────

void VfrMap::setup(const String &config_path_in)
{
    config_path = config_path_in;
    load_config();
    set_visible(enabled);
    _reposition();
    request_redraw();
}

void VfrMap::set_tile_paths(const PackedStringArray &map_dirs)
{
    tile_paths = map_dirs;
    missing_cells.clear();
    push_loader_sources();
    request_redraw();
}

void VfrMap::set_terrain_paths(const PackedStringArray &terrain_dirs)
{
    terrain_paths = terrain_dirs;
    missing_cells.clear();
    push_loader_sources();
    request_redraw();
}

void VfrMap::apply_config()
{
    load_config();
    set_visible(enabled);
    _reposition();
    request_redraw();
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
    // Key names must match SettingsStore (settings_store.cpp), which writes [map] map_enabled
    // / map_frame_size; zoom_index and the frame layout are owned by this module (persist_layout).
    enabled = cfg->get_value("map", "map_enabled", true);
    const int idx = std::max(0, std::min(2, int(cfg->get_value("map", "map_frame_size", 1))));
    // The Settings preset (Small / Medium / Large) wins when it changed since
    // the last read; otherwise the size the mouse left behind stays.
    const bool preset_changed = layout_loaded && idx != frame_size_idx;
    frame_size_idx = idx;
    const double w = double(cfg->get_value("map", "map_frame_w", 0.0));
    const double h = double(cfg->get_value("map", "map_frame_h", 0.0));
    if (!preset_changed && w >= MIN_FRAME_PX && h >= MIN_FRAME_PX)
    {
        frame_w = w;
        frame_h = h;
    }
    else
    {
        frame_w = frame_h = double(FRAME_SIZES_PX[frame_size_idx]);
    }
    frame_right = std::max(0.0, double(cfg->get_value("map", "map_frame_right", 16.0)));
    frame_top = std::max(0.0, double(cfg->get_value("map", "map_frame_top", 16.0)));
    layout_loaded = true;
    zoom_index = std::max(0, std::min(N_ZOOM - 1, int(cfg->get_value("map", "zoom_index", 2))));
    background = bool(cfg->get_value("map", "map_background", true));
    north_up = bool(cfg->get_value("map", "map_north_up", false));
}

// Zoom and frame layout go straight into provpilot.cfg [map]; the
// SettingsStore rewrites only the keys it owns on a save, so these survive.
void VfrMap::persist_layout()
{
    if (config_path.is_empty())
    {
        return;
    }
    Ref<ConfigFile> cfg = memnew(ConfigFile);
    if (cfg->load(config_path) != OK)
    {
        return; // no config yet — layout resets next launch, that's fine
    }
    cfg->set_value("map", "map_enabled", enabled);
    cfg->set_value("map", "zoom_index", zoom_index);
    cfg->set_value("map", "map_background", background);
    cfg->set_value("map", "map_north_up", north_up);
    cfg->set_value("map", "map_frame_w", frame_w);
    cfg->set_value("map", "map_frame_h", frame_h);
    cfg->set_value("map", "map_frame_right", frame_right);
    cfg->set_value("map", "map_frame_top", frame_top);
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
    frame_w = frame_h = double(size_px);
    _reposition();
    request_redraw();
}

double VfrMap::frame_size_px() const
{
    return frame_w - 2.0 * MARGIN; // the map area's width
}

// Distance from the aircraft to the farthest corner of the map area (m). The
// view width names the horizontal span only: a frame dragged tall shows three
// times as far up and down, and cells were loaded for the width alone.
double VfrMap::view_reach_m() const
{
    const double w = std::max(1.0, frame_size_px());
    const double h = std::max(w, double(map_area_rect.size.y));
    const double ppm = w / VIEW_WIDTHS_M[zoom_index];
    return 0.5 * std::sqrt(w * w + h * h) / ppm * 1.03;
}

int VfrMap::tiles_per_axis(int level) const
{
    // Mirrors the tile loop in draw_world: the view's half diagonal plus a
    // half-cell margin on each side, in cells; +2 for the partial cells at
    // both ends (independent of where the aircraft sits in its cell, so the
    // level choice does not flap while flying).
    const int cell = level_cell_m(level);
    const double extent = view_reach_m() + cell * 0.5;
    return int(2.0 * extent / cell) + 2;
}

int VfrMap::pick_level() const
{
    // Finest level whose tiles still render at >= MIN_TILE_SCREEN_PX on
    // screen AND whose grid over the view stays under MAX_TILES_PER_LEVEL
    // (a frame resized to half the screen at 20 km would otherwise walk a
    // thousand 1 km cells per redraw). Wider views and bigger frames switch
    // to coarser levels so the tile count (and cache pressure) stays bounded.
    double ppm = frame_size_px() / VIEW_WIDTHS_M[zoom_index];
    for (int i = 0; i < 3; i++)
    {
        const int n = tiles_per_axis(i);
        if (LEVELS[i].cell_m * ppm >= MIN_TILE_SCREEN_PX && n * n <= MAX_TILES_PER_LEVEL)
        {
            return i;
        }
    }
    return 2;
}

// ── Per-frame drive ─────────────────────────────────────────────────────────


void VfrMap::update_lv95(double e, double n, double heading_deg, bool ui_visible_in)
{
    ui_visible = ui_visible_in;
    if (!enabled)
    {
        return;
    }
    ac_e = e;
    ac_n = n;
    heading_rad = Math::deg_to_rad(heading_deg);
    has_position = true;
    pump_tile_results();
    if (sel_kind != SEL_NONE && double(Time::get_singleton()->get_ticks_msec()) - last_info_ms >= 1000.0)
    {
        refresh_info(); // bearing and distance from the aircraft
    }

    // Throttle redraws to ~10 Hz; the map only needs to track smoothly. A
    // parked aircraft (same position and heading as the last redraw, within
    // a tenth of a pixel / a tenth of a degree) redraws nothing: the canvas
    // keeps its last command list. Zoom, layout and data changes call
    // request_redraw() themselves, which also resets drawn_valid.
    double now = double(Time::get_singleton()->get_ticks_msec());
    if (now - last_redraw_ms >= REDRAW_INTERVAL * 1000.0)
    {
        const double eps_m = 0.1 / std::max(px_per_m, 1e-9);
        const bool same = drawn_valid && std::abs(e - drawn_e) < eps_m && std::abs(n - drawn_n) < eps_m &&
                          std::abs(heading_rad - drawn_heading_rad) < Math::deg_to_rad(0.1) &&
                          now - last_redraw_ms < PARKED_REDRAW_INTERVAL * 1000.0;
        if (!same)
        {
            last_redraw_ms = now;
            request_redraw();
        }
    }
}

// Heading-up projection of an LV95 point into local frame pixels, using the
// transform from the most recent _draw(). (frame_center_px/px_per_m are set
// there; ac_e/ac_n/heading_rad are live, so this tracks update() 1:1.)
void VfrMap::project_px(double e, double n, Vector2 &out) const
{
    Vector2 center = map_area_rect.get_center();
    if (panned)
    {
        Vector2 off;
        project_offset(ac_e - pan_e, ac_n - pan_n, std::cos(map_rot()), std::sin(map_rot()), px_per_m, Vector2(), off);
        center += off;
    }
    project_offset(e - ac_e, n - ac_n, std::cos(map_rot()), std::sin(map_rot()),
                   px_per_m, center, out);
}

Vector2 VfrMap::get_projected_px(double e, double n) const
{
    Vector2 out;
    project_px(e, n, out);
    return out;
}

// ── Details panel ───────────────────────────────────────────────────────────

void VfrMap::place_info()
{
    if (info_label == nullptr)
    {
        return;
    }
    const bool on = sel_kind != SEL_NONE;
    info_label->set_visible(on);
    info_close->set_visible(on);
    if (on)
    {
        info_label->set_position(Vector2(frame_w + 6.0, TITLE_H + 4.0));
        info_label->set_size(Vector2(INFO_W - 12.0, frame_h - TITLE_H - 22.0));
        info_close->set_position(Vector2(frame_w + INFO_W - 26.0, 0.0));
        info_close->set_size(Vector2(22.0, TITLE_H));
    }
}

bool VfrMap::select_at(const Vector2 &local_px)
{
    int kind = SEL_NONE, index = -1;
    double best = 14.0;
    for (size_t i = 0; i < airports.size(); i++)
    {
        Vector2 sp;
        project_px(airports[i].e, airports[i].n, sp);
        const double d = sp.distance_to(local_px);
        if (d < best && map_area_rect.has_point(sp))
        {
            best = d;
            kind = SEL_AIRPORT;
            index = int(i);
        }
    }
    for (size_t i = 0; i < navaids.size(); i++)
    {
        Vector2 sp;
        project_px(navaids[i].e, navaids[i].n, sp);
        const double d = sp.distance_to(local_px);
        if (d < best && map_area_rect.has_point(sp))
        {
            best = d;
            kind = SEL_NAVAID;
            index = int(i);
        }
    }
    sel_kind = kind;
    sel_index = index;
    refresh_info();
    _reposition();
    request_redraw();
    return kind != SEL_NONE;
}

bool VfrMap::select_airport(const String &icao)
{
    for (size_t i = 0; i < airports.size(); i++)
    {
        if (airports[i].icao == icao)
        {
            sel_kind = SEL_AIRPORT;
            sel_index = int(i);
            refresh_info();
            _reposition();
            request_redraw();
            return true;
        }
    }
    return false;
}

void VfrMap::close_info()
{
    sel_kind = SEL_NONE;
    sel_index = -1;
    refresh_info();
    _reposition();
    request_redraw();
}

void VfrMap::_on_info_close() { close_info(); }

String VfrMap::get_info_text() const { return info_text; }

void VfrMap::refresh_info()
{
    last_info_ms = double(Time::get_singleton()->get_ticks_msec());
    const String text = build_info_text();
    if (text != info_text)
    {
        info_text = text;
        if (info_label != nullptr)
        {
            info_label->set_text(text); // only on a change: setting it resets the scroll position
        }
    }
}

namespace
{
    String km_nm(double m)
    {
        const double nm = m / 1852.0;
        return String::num(nm, nm < 100.0 ? 1 : 0) + " NM (" + String::num(m / 1000.0, m < 100000.0 ? 1 : 0) + " km)";
    }
    String deg3(double d)
    {
        int v = int(std::lround(Math::fposmod(d, 360.0))) % 360;
        if (v == 0)
        {
            v = 360;
        }
        return String::num_int64(v).pad_zeros(3);
    }
    String khz_text(int khz)
    {
        return khz >= 100000 ? String::num(khz / 1000.0, 2).pad_decimals(2) + " MHz" : String::num_int64(khz) + " kHz";
    }
    const char *HEAD = "[color=#8fb4e6]";
}

// BBCode for the selected airport (runways, its ILS, the navaids around it)
// or navaid (type, frequency), with the magnetic bearing and the distance
// from the aircraft.
String VfrMap::build_info_text() const
{
    if (sel_kind == SEL_NONE || sel_index < 0)
    {
        return String();
    }
    const Vector2 ac_ll = has_position ? Geo::lv95_to_wgs84(ac_e, ac_n) : Vector2();
    const double year = GeoMagnetic::decimal_year_now();
    auto from_aircraft = [&](double lat, double lon) -> String {
        if (!has_position)
        {
            return String();
        }
        double d, b12, b21;
        NavaidsLib::inverse(ac_ll.x, ac_ll.y, lat, lon, d, b12, b21);
        const double var = GeoMagnetic::declination_deg(ac_ll.x, ac_ll.y, 0.0, year);
        return String("From the aircraft: ") + deg3(b12 - var) + String::utf8("° M, ") + km_nm(d) + "\n";
    };
    String t;
    if (sel_kind == SEL_NAVAID && sel_index < int(navaids.size()))
    {
        const NavaidMark &nv = navaids[sel_index];
        t += String("[b]") + nv.ident + "[/b]  " + nv.name + "\n";
        const Navaid *src = nullptr;
        for (const Navaid &n : NavaidsLib::all())
        {
            if (n.ident == nv.ident && std::abs(n.lat - nv.lat) < 1e-6 && std::abs(n.lon - nv.lon) < 1e-6)
            {
                src = &n;
                break;
            }
        }
        t += String(nv.type) + String("  [b]") + (src != nullptr ? khz_text(src->freq_khz) : nv.freq) + "[/b]\n";
        if (src != nullptr && src->dme_khz > 0 && !(src->kind & NavaidsLib::KIND_VOR))
        {
            t += String("DME channel paired with ") + khz_text(src->dme_khz) + "\n";
        }
        t += String("Elevation ") + String::num_int64((int)std::lround(nv.elev_m / 0.3048)) + " ft\n";
        t += from_aircraft(nv.lat, nv.lon);
        if (has_position && (nv.kind & NavaidsLib::KIND_VOR))
        {
            double d, b12, b21;
            NavaidsLib::inverse(nv.lat, nv.lon, ac_ll.x, ac_ll.y, d, b12, b21);
            const double var = (src != nullptr && src->has_station_var) ? src->station_var_deg : GeoMagnetic::declination_deg(nv.lat, nv.lon, 0.0, year);
            t += String("The aircraft is on radial ") + deg3(b12 - var) + "\n";
        }
        return t;
    }
    if (sel_kind != SEL_AIRPORT || sel_index >= int(airports.size()))
    {
        return String();
    }
    const Airport &ap = airports[sel_index];
    t += String("[b]") + ap.icao + "[/b]  " + ap.name + "\n";
    t += ap.type.capitalize() + ", elevation " + String::num_int64((int)std::lround(ap.elev_m / 0.3048)) + " ft\n";
    t += from_aircraft(ap.lat, ap.lon);
    const double ap_var = GeoMagnetic::declination_deg(ap.lat, ap.lon, 0.0, year);
    if (!ap.runways.is_empty())
    {
        t += String("\n") + HEAD + "RUNWAYS[/color]\n";
        for (int i = 0; i < ap.runways.size(); i++)
        {
            const Dictionary r = ap.runways[i];
            t += String("[b]") + String(r.get("designator", "")) + "[/b]  " + String::num_int64((int)double(r.get("length_m", 0.0))) + " m  " + String(r.get("surface", "")) + "\n";
        }
    }
    String ils_txt;
    for (const Ils &s : NavaidsLib::all_ils())
    {
        if (s.icao != ap.icao)
        {
            continue;
        }
        ils_txt += String("[b]") + s.rwy + "  " + String::num(s.freq_khz / 1000.0, 2).pad_decimals(2) + "[/b]  " + s.ident + "  " + deg3(s.course_deg - ap_var) + String::utf8("°  GS ") + String::num(s.gs_deg, 1) +
                (s.synthetic ? String("  (no real ILS)") : String()) + "\n";
    }
    if (!ils_txt.is_empty())
    {
        t += String("\n") + HEAD + "ILS (runway, MHz, ident, course)[/color]\n" + ils_txt;
    }
    // navaids within 25 NM of the field, nearest first
    struct Near { double d; const NavaidMark *m; };
    std::vector<Near> near;
    for (const NavaidMark &nv : navaids)
    {
        if (std::abs(nv.lat - ap.lat) > 0.6)
        {
            continue;
        }
        const double d = NavaidsLib::distance_m(ap.lat, ap.lon, nv.lat, nv.lon);
        if (d <= 25.0 * 1852.0)
        {
            near.push_back({d, &nv});
        }
    }
    std::sort(near.begin(), near.end(), [](const Near &a, const Near &b) { return a.d < b.d; });
    if (!near.empty())
    {
        t += String("\n") + HEAD + "NAVAIDS within 25 NM[/color]\n";
        for (size_t i = 0; i < near.size() && i < 12; i++)
        {
            const NavaidMark &nv = *near[i].m;
            double d, b12, b21;
            NavaidsLib::inverse(ap.lat, ap.lon, nv.lat, nv.lon, d, b12, b21);
            t += String("[b]") + nv.ident + "  " + nv.freq + "[/b]  " + nv.type + "  " + deg3(b12 - ap_var) + String::utf8("° ") + String::num(d / 1852.0, 1) + " NM\n";
        }
    }
    return t;
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
    load_navaids();
    parse_cantons("res://web/data/cantons.geojson");
}

Vector2 VfrMap::wgs84_to_lv95(double lat_deg, double lon_deg)
{
    // Rigorous LV95 (shared s3d_proj.h via Geo) — the map tiles were baked
    // with pyproj, so airports and canton outlines now land on them exactly.
    return Geo::wgs84_to_lv95(lat_deg, lon_deg);
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
        ap.name = String(a.get("name", ""));
        ap.type = type.replace("_", " ");
        ap.lat = lat;
        ap.lon = lon;
        ap.elev_m = double(a.get("elevation_m", 0.0));
        ap.runways = a.get("runways", Array());
        airports.push_back(ap);
    }
}

void VfrMap::load_navaids()
{
    navaids.clear();
    for (const Navaid &n : NavaidsLib::all())
    {
        if (n.kind == 0)
        {
            continue;
        }
        Vector2 lv = wgs84_to_lv95(n.lat, n.lon);
        NavaidMark m;
        m.e = lv.x;
        m.n = lv.y;
        m.ident = n.ident;
        m.kind = n.kind;
        m.name = n.name;
        m.type = n.type;
        m.lat = n.lat;
        m.lon = n.lon;
        m.elev_m = n.elev_m;
        m.freq = n.freq_khz >= 100000 ? String::num(n.freq_khz / 1000.0, 2).pad_decimals(2) : String::num_int64(n.freq_khz);
        navaids.push_back(m);
    }
}

// ILS feathers from the resolved ILS list (NavaidsLib), rebuilt when the
// geometry was re-resolved (the welcome screen hands over the runways).
void VfrMap::refresh_ils_marks()
{
    if (ils_marks_gen == NavaidsLib::ils_generation())
    {
        return;
    }
    ils_marks_gen = NavaidsLib::ils_generation();
    ils_marks.clear();
    for (const Ils &s : NavaidsLib::all_ils())
    {
        if (!s.resolved)
        {
            continue;
        }
        const double dx = s.far_e - s.thr_e, dy = s.far_n - s.thr_n;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0)
        {
            continue;
        }
        // outbound unit vector (away from the runway) and its left normal
        const double ux = -dx / len, uy = -dy / len;
        const double nx = -uy, ny = ux;
        const double L = 7000.0, W = 600.0;
        IlsMark m;
        m.apex = Vector2(s.thr_e, s.thr_n);
        m.base_c = Vector2(s.thr_e + ux * L, s.thr_n + uy * L);
        m.base_l = m.base_c + Vector2(nx, ny) * W;
        m.base_r = m.base_c - Vector2(nx, ny) * W;
        m.label = String(s.synthetic ? "ILS* " : "ILS ") + String::num(s.freq_khz / 1000.0, 2) + " " + s.rwy;
        m.synthetic = s.synthetic;
        ils_marks.push_back(m);
    }
}

int VfrMap::get_visible_navaid_count() const
{
    return visible_navaids;
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
    while (cache_lru.size() >= size_t(cache_cap) && !cache_lru.empty())
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

// ── Cell loader (worker threads) ────────────────────────────────────────────

struct VfrMap::TileLoader
{
    struct Result
    {
        TileKey key;
        uint64_t gen = 0;
        Ref<Image> img;                        // null = no data for this cell
    };
    std::mutex m;
    std::condition_variable cv;
    std::vector<TileKey> queue;                // farthest first: workers pop the back
    std::unordered_set<uint64_t> busy;         // taken by a worker, result not yet pumped
    std::vector<Result> results;
    PackedStringArray map_dirs;
    PackedStringArray terrain_dirs;
    String cache_dir;
    PackedStringArray purge_dirs;              // stale cache dirs to delete
    uint64_t gen = 0;
    bool stop = false;
    std::vector<std::thread> threads;
};

namespace
{
    void remove_dir_recursive(const String &dir)
    {
        const PackedStringArray files = DirAccess::get_files_at(dir);
        for (int i = 0; i < files.size(); i++)
        {
            DirAccess::remove_absolute(dir.path_join(files[i]));
        }
        const PackedStringArray dirs = DirAccess::get_directories_at(dir);
        for (int i = 0; i < dirs.size(); i++)
        {
            remove_dir_recursive(dir.path_join(dirs[i]));
        }
        DirAccess::remove_absolute(dir);
    }
}

void VfrMap::ensure_loader()
{
    if (loader)
    {
        return;
    }
    loader = std::make_shared<TileLoader>();
    std::shared_ptr<TileLoader> L = loader;
    for (int t = 0; t < LOADER_THREADS; t++)
    {
        L->threads.emplace_back([L]() {
            for (;;)
            {
                TileKey key;
                uint64_t gen = 0;
                PackedStringArray map_dirs, terrain_dirs, purge;
                String cdir;
                {
                    std::unique_lock<std::mutex> lock(L->m);
                    L->cv.wait(lock, [&]() { return L->stop || !L->queue.empty() || !L->purge_dirs.is_empty(); });
                    if (L->stop)
                    {
                        return;
                    }
                    if (!L->purge_dirs.is_empty())
                    {
                        purge = L->purge_dirs;
                        L->purge_dirs.clear();
                    }
                    else
                    {
                        key = L->queue.back();
                        L->queue.pop_back();
                        L->busy.insert(cell_hash(key));
                        gen = L->gen;
                        map_dirs = L->map_dirs;
                        terrain_dirs = L->terrain_dirs;
                        cdir = L->cache_dir;
                    }
                }
                if (!purge.is_empty())
                {
                    for (int i = 0; i < purge.size(); i++)
                    {
                        remove_dir_recursive(purge[i]);
                    }
                    continue;
                }
                TileLoader::Result r;
                r.key = key;
                r.gen = gen;
                r.img = load_cell_image(map_dirs, terrain_dirs, cdir, key);
                {
                    std::lock_guard<std::mutex> lock(L->m);
                    L->results.push_back(r);
                }
            }
        });
    }
    push_loader_sources();
}

VfrMap::~VfrMap()
{
    if (loader)
    {
        {
            std::lock_guard<std::mutex> lock(loader->m);
            loader->stop = true;
        }
        loader->cv.notify_all();
        for (std::thread &t : loader->threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }
}

// The disk cache is keyed by the terrain dirs and their modification times:
// installing, removing or re-baking a region starts a fresh cache instead of
// showing cells composed from the old set. Caches not used for 30 days go.
void VfrMap::push_loader_sources()
{
    cache_dir = String();
    PackedStringArray purge;
    if (!terrain_paths.is_empty())
    {
        String sig;
        for (int i = 0; i < terrain_paths.size(); i++)
        {
            const String ov = String(terrain_paths[i]).trim_suffix("/").get_base_dir().path_join("map_overview").path_join("L2");
            sig += String(terrain_paths[i]) + "|" + String::num_uint64(FileAccess::get_modified_time(terrain_paths[i])) + "|" +
                    String::num_uint64(FileAccess::get_modified_time(ov)) + "|" +
                    String::num_uint64(FileAccess::get_modified_time(ov.get_base_dir().path_join("L1"))) + ";";
        }
        const String root = OS::get_singleton()->get_user_data_dir().path_join("map_cache");
        const String name = String::num_uint64(uint64_t(sig.hash()), 16) + "_" + String::num_int64(sig.length());
        const String dir = root.path_join(name);
        if (DirAccess::make_dir_recursive_absolute(dir.path_join("L1")) == OK &&
            DirAccess::make_dir_recursive_absolute(dir.path_join("L2")) == OK)
        {
            cache_dir = dir;
            Ref<FileAccess> stamp = FileAccess::open(dir.path_join("stamp"), FileAccess::WRITE);
            if (stamp.is_valid())
            {
                stamp->store_string(sig);
            }
            const uint64_t now = uint64_t(Time::get_singleton()->get_unix_time_from_system());
            const PackedStringArray others = DirAccess::get_directories_at(root);
            for (int i = 0; i < others.size(); i++)
            {
                if (others[i] == name)
                {
                    continue;
                }
                const uint64_t used = FileAccess::get_modified_time(root.path_join(others[i]).path_join("stamp"));
                if (used + 30ull * 86400ull < now)
                {
                    purge.push_back(root.path_join(others[i]));
                }
            }
        }
    }
    if (!loader)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(loader->m);
        loader->gen++;
        loader->queue.clear();
        loader->map_dirs = tile_paths;
        loader->terrain_dirs = terrain_paths;
        loader->cache_dir = cache_dir;
        loader->purge_dirs.append_array(purge);
    }
    loader->cv.notify_all();
}

void VfrMap::request_tiles(std::vector<TileKey> &wanted)
{
    if (wanted.empty() && !loader)
    {
        return;
    }
    ensure_loader();
    // `wanted` is nearest first; the queue is popped from the back.
    std::reverse(wanted.begin(), wanted.end());
    {
        std::lock_guard<std::mutex> lock(loader->m);
        loader->queue.clear();
        for (const TileKey &k : wanted)
        {
            if (loader->busy.count(cell_hash(k)) == 0)
            {
                loader->queue.push_back(k);
            }
        }
    }
    loader->cv.notify_all();
}

void VfrMap::pump_tile_results()
{
    if (!loader)
    {
        return;
    }
    std::vector<TileLoader::Result> done;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(loader->m);
        if (loader->results.empty())
        {
            return;
        }
        const size_t n = std::min(loader->results.size(), size_t(MAX_UPLOADS_PER_FRAME));
        done.assign(loader->results.begin(), loader->results.begin() + n);
        loader->results.erase(loader->results.begin(), loader->results.begin() + n);
        for (const TileLoader::Result &r : done)
        {
            loader->busy.erase(cell_hash(r.key));
        }
        gen = loader->gen;
    }
    for (const TileLoader::Result &r : done)
    {
        if (r.gen != gen)
        {
            continue; // built from sources that changed meanwhile
        }
        if (r.img.is_null())
        {
            missing_cells.insert(cell_hash(r.key));
            continue;
        }
        if (get_tile(r.key.ei, r.key.ni, r.key.level).is_null())
        {
            load_tile_into_cache(r.key, ImageTexture::create_from_image(r.img));
        }
    }
    drawn_valid = false; // new cells: the next redraw tick shows them, also when parked
}

int VfrMap::get_pending_tile_count() const
{
    if (!loader)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(loader->m);
    return int(loader->queue.size() + loader->busy.size());
}

void VfrMap::set_background_enabled(bool on)
{
    if (background == on)
    {
        return;
    }
    background = on;
    persist_layout();
    request_redraw();
}

bool VfrMap::is_background_enabled() const { return background; }

// Worker thread. Sources in priority order: a pre-rendered map/ tile, the
// disk cache (composite levels), the base tier's terrain shaded on the fly.
Ref<Image> VfrMap::load_cell_image(const PackedStringArray &map_dirs, const PackedStringArray &terrain_dirs,
        const String &cdir, const TileKey &key)
{
    int cell = level_cell_m(key.level);
    String fname = String("map_") + String::num_int64((long long)(key.ei) * cell) + "_" + String::num_int64((long long)(key.ni) * cell) + ".jpg";
    const String subdir = LEVELS[std::max(0, std::min(2, key.level))].subdir;
    // is_empty(), not subdir[0]: indexing an empty String goes through the
    // engine's out-of-line operator[], which in this build resolves its
    // _null sentinel to address 0 and segfaults (hit at L0, whose subdir
    // is "").
    String suffix = subdir.is_empty() ? fname : (String("/") + subdir + "/" + fname);

    for (int i = 0; i < map_dirs.size(); i++)
    {
        String base = map_dirs[i];
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
            return Ref<Image>();
        }
        // Tile files store pixel(0,0) at the SE corner (top row = south,
        // left col = east). Godot textures need top-left = NW: flip both
        // axes so texture UV (0,0) samples the cell's NW corner.
        img->flip_y();
        img->flip_x();
        return img;
    }
    if (terrain_dirs.is_empty())
    {
        return Ref<Image>();
    }
    // Composite cells (64 / 1024 terrain files) come from the disk cache once
    // built; an empty file marks a cell without any terrain (open sea).
    String cache_file;
    if (key.level > 0 && !cdir.is_empty())
    {
        cache_file = cdir.path_join(String("L") + String::num_int64(key.level)).path_join(
                String("c_") + String::num_int64(key.ei) + "_" + String::num_int64(key.ni) + ".png");
        Ref<FileAccess> f = FileAccess::open(cache_file, FileAccess::READ);
        if (f.is_valid())
        {
            const uint64_t len = f->get_length();
            if (len == 0)
            {
                return Ref<Image>();
            }
            Ref<Image> img = memnew(Image);
            if (img->load_png_from_buffer(f->get_buffer(len)) == OK && !img->is_empty())
            {
                return img;
            }
            // truncated (the sim quit mid-write): build it again
        }
    }
    Ref<Image> img = render_terrain_image(terrain_dirs, key);
    if (!cache_file.is_empty())
    {
        const PackedByteArray png = img.is_valid() ? img->save_png_to_buffer() : PackedByteArray();
        Ref<FileAccess> f = FileAccess::open(cache_file, FileAccess::WRITE);
        if (f.is_valid() && !png.is_empty())
        {
            f->store_buffer(png);
        }
    }
    return img;
}

uint64_t VfrMap::cell_hash(const TileKey &key)
{
    return (uint64_t(uint32_t(key.level)) << 60) ^ (uint64_t(uint32_t(key.ei)) << 30) ^ uint64_t(uint32_t(key.ni));
}

namespace
{
    // VFR-sectional hypsometric ramp (scripts/generate_vfr_map_tiles.py).
    struct TintStop { double elev; float r, g, b; };
    const TintStop TINT_STOPS[] = {
        { -50.0, 0.82f, 0.87f, 0.93f },   // below datum: pale blue
        { 0.0,   0.76f, 0.84f, 0.62f },   // lowland green
        { 500.0, 0.72f, 0.80f, 0.52f },   // forest green
        { 1000.0, 0.83f, 0.79f, 0.56f },  // alpine meadow tan
        { 1800.0, 0.80f, 0.72f, 0.55f },  // brown scree
        { 2500.0, 0.88f, 0.84f, 0.78f },  // rocky grey
        { 3200.0, 0.97f, 0.97f, 0.97f },  // snow white
    };
    constexpr int16_t OVERVIEW_NO_DATA = -32768;     // map_overview cells: no tile
    constexpr int16_t OVERVIEW_SEA = -32767;         // ... a sea level sample (the bake stores 0.01 m)
    const Color C_SEA(0.60f, 0.72f, 0.86f, 1.0f);   // sea level samples (the bake stores 0.01 m)

    Color tint_of(double elev)
    {
        if (elev < 0.05)
        {
            return C_SEA;
        }
        const int n = int(sizeof(TINT_STOPS) / sizeof(TINT_STOPS[0]));
        if (elev >= TINT_STOPS[n - 1].elev)
        {
            return Color(TINT_STOPS[n - 1].r, TINT_STOPS[n - 1].g, TINT_STOPS[n - 1].b);
        }
        for (int i = 1; i < n; i++)
        {
            if (elev <= TINT_STOPS[i].elev)
            {
                const TintStop &a = TINT_STOPS[i - 1];
                const TintStop &b = TINT_STOPS[i];
                const float t = float((elev - a.elev) / (b.elev - a.elev));
                return Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t);
            }
        }
        return Color(TINT_STOPS[0].r, TINT_STOPS[0].g, TINT_STOPS[0].b);
    }
}

// Render one map cell from the base tier's 34 x 34 float32 terrain tiles:
// L0 = one tile at its native 34 samples, L3 = 8 x 8 tiles at 16 samples
// each (every 2nd), L5 = 32 x 32 tiles at 4 samples each (every 8th). Raw
// pixel (0,0) is the SE corner (rows S->N, cols E->W); the image is built
// with x eastward and y southward so the texture's top-left is the cell's
// NW corner like the pre-rendered tiles. Hillshade (light from the NW, 45°
// up) times the hypsometric tint; samples without a tile stay transparent
// so the frame's no-data fill shows through. 1 m add-on tiles (1024² floats)
// are skipped — those packs carry pre-rendered map/ tiles.
Ref<Image> VfrMap::render_terrain_image(const PackedStringArray &terrain_paths, const TileKey &key)
{
    const int cell = level_cell_m(key.level);
    const int K = std::max(1, cell / 1024);
    const int P = key.level == 0 ? BASE_TILE_PX : (key.level == 1 ? 16 : 4);
    const int stride = key.level == 0 ? 1 : (key.level == 1 ? 2 : 8);
    const int S = K * P;
    const float NO_DATA = -32768.0f;
    std::vector<float> h(size_t(S) * size_t(S), NO_DATA);
    const int64_t tile_bytes = int64_t(BASE_TILE_PX) * BASE_TILE_PX * 4;
    bool any = false;
    // Sources in priority order; the first one that has a tile wins it.
    // A region with a map_overview/ folder (scripts/build_map_overview.py)
    // delivers a whole overview cell in one file of 128 x 128 int16 metres
    // instead of 1024 tile files - what makes the wide views fill at once.
    std::vector<uint8_t> done(size_t(K) * size_t(K), 0);
    int todo = K * K;
    for (int i = 0; i < terrain_paths.size() && todo > 0; i++)
    {
        const String dir = String(terrain_paths[i]).trim_suffix("/");
        if (dir.is_empty())
        {
            continue;
        }
        if (key.level >= 1)
        {
            // L2: raw int16; L1 (64 m samples, many more cells): the same, deflated
            const bool packed = key.level == 1;
            const String ov_dir = dir.get_base_dir().path_join("map_overview").path_join(packed ? "L1" : "L2");
            if (DirAccess::dir_exists_absolute(ov_dir))
            {
                Ref<FileAccess> f = FileAccess::open(ov_dir.path_join(String("c_") + String::num_int64(key.ei) + "_" + String::num_int64(key.ni) + (packed ? ".i16z" : ".i16")), FileAccess::READ);
                if (f.is_null() || (!packed && f->get_length() != uint64_t(S) * S * 2))
                {
                    continue; // nothing of this region in the cell
                }
                PackedByteArray buf = f->get_buffer(int64_t(f->get_length()));
                if (packed)
                {
                    buf = buf.decompress(int64_t(S) * S * 2, FileAccess::COMPRESSION_DEFLATE);
                }
                if (buf.size() != int64_t(S) * S * 2)
                {
                    continue;
                }
                const int16_t *v16 = reinterpret_cast<const int16_t *>(buf.ptr());
                for (int ty = 0; ty < K; ty++)
                {
                    for (int tx = 0; tx < K; tx++)
                    {
                        if (done[size_t(ty) * K + tx])
                        {
                            continue;
                        }
                        const int y0 = (K - 1 - ty) * P;
                        const int x0 = tx * P;
                        if (v16[size_t(y0) * S + x0] == OVERVIEW_NO_DATA)
                        {
                            continue; // the region has no tile here
                        }
                        for (int y = y0; y < y0 + P; y++)
                        {
                            for (int x = x0; x < x0 + P; x++)
                            {
                                const int16_t q = v16[size_t(y) * S + x];
                                h[size_t(y) * S + x] = q == OVERVIEW_SEA ? 0.0f : (q == OVERVIEW_NO_DATA ? NO_DATA : std::max(0.1f, float(q)));
                            }
                        }
                        done[size_t(ty) * K + tx] = 1;
                        todo--;
                        any = true;
                    }
                }
                continue;
            }
        }
        for (int ty = 0; ty < K; ty++)
        {
            for (int tx = 0; tx < K; tx++)
            {
                if (done[size_t(ty) * K + tx])
                {
                    continue;
                }
                const long long e = (long long)key.ei * cell + (long long)tx * 1024;
                const long long n = (long long)key.ni * cell + (long long)ty * 1024;
                const String fname = String("tile_") + String::num_int64(e) + "_" + String::num_int64(n) + ".raw";
                Ref<FileAccess> f = FileAccess::open(dir.path_join(fname), FileAccess::READ);
                if (f.is_null())
                {
                    continue;
                }
                done[size_t(ty) * K + tx] = 1; // also a 1 m add-on tile: it shadows the base tile
                todo--;
                if (f->get_length() != uint64_t(tile_bytes))
                {
                    continue;
                }
                const PackedByteArray buf = f->get_buffer(tile_bytes);
                if (buf.size() != tile_bytes)
                {
                    continue;
                }
                const uint8_t *p = buf.ptr();
                for (int sy = 0; sy < P; sy++)
                {
                    const int row = std::min(BASE_TILE_PX - 1, sy * stride);          // from the south
                    for (int sx = 0; sx < P; sx++)
                    {
                        const int col = BASE_TILE_PX - 1 - std::min(BASE_TILE_PX - 1, sx * stride); // col 0 = east
                        float v;
                        std::memcpy(&v, p + (size_t(row) * BASE_TILE_PX + col) * 4, 4);
                        const int x = tx * P + sx;
                        const int y = (K - 1 - ty) * P + (P - 1 - sy);
                        h[size_t(y) * S + x] = v;
                    }
                }
                any = true;
            }
        }
    }
    if (!any)
    {
        return Ref<Image>();
    }
    const double m_per_px = double(cell) / double(S);
    const double az = Math::deg_to_rad(315.0);
    const double alt = Math::deg_to_rad(45.0);
    const double lx = std::cos(alt) * std::sin(az);
    const double ly = std::cos(alt) * std::cos(az);
    const double lz = std::sin(alt);
    PackedByteArray px;
    px.resize(size_t(S) * S * 4);
    uint8_t *o = px.ptrw();
    auto at = [&](int x, int y, float fallback) -> double {
        if (x < 0 || y < 0 || x >= S || y >= S)
        {
            return fallback;
        }
        const float v = h[size_t(y) * S + x];
        return v == NO_DATA ? fallback : v;
    };
    for (int y = 0; y < S; y++)
    {
        for (int x = 0; x < S; x++)
        {
            uint8_t *q = o + (size_t(y) * S + x) * 4;
            const float v = h[size_t(y) * S + x];
            if (v == NO_DATA)
            {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            const double dz_de = (at(x + 1, y, v) - at(x - 1, y, v)) / (2.0 * m_per_px);
            const double dz_dn = (at(x, y - 1, v) - at(x, y + 1, v)) / (2.0 * m_per_px);
            const double nx = -dz_de, ny = -dz_dn, nz = 1.0;
            const double norm = std::sqrt(nx * nx + ny * ny + nz * nz);
            const double shade = std::max(0.0, std::min(1.0, (nx * lx + ny * ly + nz * lz) / norm));
            const Color t = tint_of(v);
            const double k = 0.35 + 0.65 * shade;
            q[0] = uint8_t(std::min(255.0, t.r * k * 255.0));
            q[1] = uint8_t(std::min(255.0, t.g * k * 255.0));
            q[2] = uint8_t(std::min(255.0, t.b * k * 255.0));
            q[3] = 255;
        }
    }
    return Image::create_from_data(S, S, false, Image::FORMAT_RGBA8, px);
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void VfrMap::_draw()
{
    const double total_w = frame_w + info_w();
    const Rect2 frame(0, 0, total_w, frame_h);
    // Frame background + border.
    draw_rect(frame, C_BG);
    draw_rect(frame, C_BORDER, false, 2.0);

    Ref<Font> font = ThemeDB::get_singleton()->get_default_theme()->get_font("font", "Label");

    // Title bar: "VFR MAP" left, view width right.
    Rect2 title(0, 0, total_w, TITLE_H);
    draw_rect(title, C_TITLE_BG);
    if (font.is_valid())
    {
        draw_string(font, Vector2(8, 18), "VFR MAP", HORIZONTAL_ALIGNMENT_LEFT, -1, 14, C_TITLE_TEXT);
        // terrain background on / off: a checkbox, labelled where the bar has room
        const Rect2 box(BG_BOX_X, 7.0, 12.0, 12.0);
        draw_rect(box, C_TITLE_TEXT, false, 1.0);
        if (background)
        {
            draw_line(Vector2(BG_BOX_X + 2.5, 13.0), Vector2(BG_BOX_X + 5.0, 16.0), C_TITLE_TEXT, 1.5, true);
            draw_line(Vector2(BG_BOX_X + 5.0, 16.0), Vector2(BG_BOX_X + 9.5, 9.5), C_TITLE_TEXT, 1.5, true);
        }
        if (frame_w >= BG_LABEL_MIN_W)
        {
            draw_string(font, Vector2(BG_BOX_X + 17.0, 18), "Terrain", HORIZONTAL_ALIGNMENT_LEFT, -1, 13, C_TITLE_TEXT);
        }
        // the view width in both units (charts and DME read nautical miles)
        const String width_txt = String::num_int64((int)(VIEW_WIDTHS_M[zoom_index] / 1000.0)) + " km / " +
                String::num_int64((int)std::lround(VIEW_WIDTHS_M[zoom_index] / 1852.0)) + " NM";
        const double tw = font->get_string_size(width_txt, HORIZONTAL_ALIGNMENT_LEFT, -1, 14).x;
        draw_string(font, Vector2(frame_w - 8 - tw, 18), width_txt, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, C_TITLE_TEXT);
        if (sel_kind != SEL_NONE)
        {
            draw_line(Vector2(frame_w, TITLE_H), Vector2(frame_w, frame_h), C_BORDER, 1.0, false);
        }
    }

    // Map area (below the title bar): the clipped canvas child covers it and
    // draws the world (draw_world) right after this.
    place_canvas();
    px_per_m = map_area_rect.size.x / VIEW_WIDTHS_M[zoom_index];
    update_frame_center();

    // Resize grips: two short diagonals in each bottom corner.
    for (int i = 0; i < 2; i++)
    {
        const double d = 5.0 + 5.0 * i;
        draw_line(Vector2(total_w - 3.0, frame_h - 3.0 - d), Vector2(total_w - 3.0 - d, frame_h - 3.0), C_BORDER, 1.5, false);
        draw_line(Vector2(3.0, frame_h - 3.0 - d), Vector2(3.0 + d, frame_h - 3.0), C_BORDER, 1.5, false);
    }
}

// The map world, clipped by the canvas child to the map area. Everything is
// in frame coordinates (frame_center_px / px_per_m come from _draw, which
// Godot runs before the children's draw); the transform shifts them into
// the canvas.
void VfrMap::draw_world(CanvasItem *ci)
{
    if (ci == nullptr || !has_position)
    {
        return; // no aircraft position yet — just the empty frame
    }
    const Rect2 map_area = map_area_rect;
    ci->draw_set_transform(-map_area.position, 0.0, Vector2(1, 1));
    Ref<Font> font = ThemeDB::get_singleton()->get_default_theme()->get_font("font", "Label");
    double view_m = VIEW_WIDTHS_M[zoom_index];
    ci->draw_rect(map_area, C_NO_TILE); // fill under missing tiles

    // ── Rotated map world (heading-up) ────────────────────────────────────
    // Local screen axes: +x right, +y down. Map axes in meters:
    //   east  = (cosH, sinH)
    //   north = (-sinH, cosH)
    // A point at LV95 (e, n) projects to:
    //   px = center + ((e - ac_e) * cosH + (n - ac_n) * sinH) * ppm
    //   py = center + ((ac_n - n) * cosH - (e - ac_e) * sinH) * ppm
    double ch = std::cos(map_rot());
    double sh = std::sin(map_rot());

    auto project = [&](double e, double n, Vector2 &out) {
        // Heading-up rotation (see project_offset / project_px).
        project_offset(e - ac_e, n - ac_n, ch, sh, px_per_m, frame_center_px, out);
    };

    // Tiles: iterate the level's grid cells covering the view. Cells not in
    // the texture cache are requested from the loader threads (nearest
    // first) and appear on a later redraw; under a finer level the overview
    // level is drawn first, so the view is never empty while it fills.
    int level = pick_level();
    drawn_e = ac_e;
    drawn_n = ac_n;
    drawn_heading_rad = heading_rad;
    drawn_valid = true;

    std::vector<TileKey> wanted;
    std::vector<double> wanted_d2;
    int tiles_drawn = 0;
    int cells_in_view = 0;
    const double vc_e = panned ? pan_e : ac_e; // view centre
    const double vc_n = panned ? pan_n : ac_n;
    auto draw_level = [&](int lv) {
        const int cell = level_cell_m(lv);
        // The farthest visible corner plus a half-cell margin, so rotation
        // never exposes un-drawn edges.
        const double extent = view_reach_m() + cell * 0.5;
        const int ei_min = int(std::floor((vc_e - extent) / (double)cell));
        const int ei_max = int(std::floor((vc_e + extent) / (double)cell));
        const int ni_min = int(std::floor((vc_n - extent) / (double)cell));
        const int ni_max = int(std::floor((vc_n + extent) / (double)cell));
        cells_in_view += (ei_max - ei_min + 1) * (ni_max - ni_min + 1);
        for (int ni = ni_min; ni <= ni_max && tiles_drawn < MAX_TILES_PER_DRAW; ni++)
        {
            for (int ei = ei_min; ei <= ei_max && tiles_drawn < MAX_TILES_PER_DRAW; ei++)
            {
                TileKey key{ei, ni, lv};
                Ref<ImageTexture> tex = get_tile(ei, ni, lv);
                if (tex.is_null())
                {
                    if (missing_cells.count(cell_hash(key)) == 0)
                    {
                        const double de = (ei + 0.5) * cell - vc_e;
                        const double dn = (ni + 0.5) * cell - vc_n;
                        wanted.push_back(key);
                        // the overview level goes first: a few cells cover the whole view
                        wanted_d2.push_back(lv == level ? de * de + dn * dn : -1.0);
                    }
                    continue; // not there yet, or no data — what is underneath shows through
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

                // After the double flip in load_cell_image() the texture's top-left
                // is the cell's NW corner. Canvas UVs are normalized [0,1]
                // (draw_polygon remaps 0..1 for AtlasTexture and passes plain
                // textures straight to the rasterizer), so map the quad corners
                // onto unit-square UVs: NW->(0,0), NE->(1,0), SE->(1,1), SW->(0,1).
                const Vector2 quad_pts[4] = { p_nw, p_ne, p_se, p_sw };
                const Vector2 quad_uvs[4] = {
                    Vector2(0.0, 0.0), Vector2(1.0, 0.0),
                    Vector2(1.0, 1.0), Vector2(0.0, 1.0) };

                // Clip to the map area: the tile extent deliberately overshoots
                // so rotation never exposes un-drawn edges, but without clipping
                // tiles would bleed past the frame border. UVs interpolate
                // linearly along clipped edges (exact for canvas polygons).
                std::vector<Vector2> cp;
                std::vector<Vector2> cu;
                if (!clip_polygon_to_rect(quad_pts, quad_uvs, 4, map_area, cp, cu))
                {
                    continue;
                }

                PackedVector2Array pts;
                PackedVector2Array uvs;
                for (size_t i = 0; i < cp.size(); i++)
                {
                    pts.push_back(cp[i]);
                    uvs.push_back(cu[i]);
                }
                ci->draw_colored_polygon(pts, Color(1, 1, 1, 1), uvs, tex);
                tiles_drawn++;
            }
        }
    };
    if (background)
    {
        if (level < 2)
        {
            draw_level(2);
        }
        draw_level(level);
        // The LRU cap follows the view: a frame resized to half the screen at
        // 10 km needs ~300 L0 tiles at once; a fixed 256 made every redraw evict
        // tiles it was about to draw again (black cells flickering in and out,
        // steady holes, and a reload storm that cost frame rate).
        cache_cap = std::max(MAX_CACHE_TILES, std::min(MAX_CACHE_TILES_HARD, 2 * cells_in_view));
        std::vector<size_t> order(wanted.size());
        for (size_t i = 0; i < order.size(); i++)
        {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return wanted_d2[x] < wanted_d2[y]; });
        std::vector<TileKey> sorted;
        sorted.reserve(order.size());
        for (size_t i : order)
        {
            sorted.push_back(wanted[i]);
        }
        request_tiles(sorted);
    }
    else
    {
        request_tiles(wanted); // empty: drops what was still queued
    }

    // Canton borders (rotated with the map world). Every segment is clipped
    // to the map area so border lines stop at the frame edge instead of
    // bleeding past it. Rings whose projected bbox misses the view are
    // skipped before the per-segment clipping.
    for (const std::vector<Vector2> &ring : canton_rings)
    {
        if (ring.size() < 2)
        {
            continue;
        }
        PackedVector2Array screen;
        screen.resize(int(ring.size()));
        Rect2 bbox;
        bool bbox_valid = false;
        for (size_t i = 0; i < ring.size(); i++)
        {
            project(ring[i].x, ring[i].y, screen[i]);
            if (!bbox_valid)
            {
                bbox = Rect2(screen[i], Vector2());
                bbox_valid = true;
            }
            else
            {
                bbox = bbox.expand(screen[i]);
            }
        }
        if (!bbox.intersects(map_area.grow(2.0)))
        {
            continue; // whole ring outside the view (exact: segments can't
                      // cross the rect without their bbox touching it)
        }
        for (size_t i = 0; i < ring.size(); i++)
        {
            const Vector2 &a = screen[i];
            const Vector2 &b = screen[(i + 1) % ring.size()];
            Vector2 ca, cb;
            if (clip_segment_to_rect(a, b, map_area, ca, cb))
            {
                ci->draw_line(ca, cb, C_CANTON, 1.0, false);
            }
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
        if (sel_kind == SEL_AIRPORT && sel_index == int(&ap - airports.data()))
        {
            ci->draw_arc(sp, 9.0, 0, TAU, 24, C_COURSE, 2.0, false);
        }
        if (zoom_index >= 5 && !ap.international)
        {
            ci->draw_circle(sp, 1.5, C_AIRPORT_GA); // the wide views: small fields as dots
            continue;
        }
        Color c = ap.international ? C_AIRPORT_INTL : C_AIRPORT_GA;
        if (ap.international)
        {
            ci->draw_rect(Rect2(sp.x - 4, sp.y - 3, 8, 6), c);
        }
        else
        {
            ci->draw_arc(sp, 3.0, 0, TAU, 16, c, 1.5, false);
        }
    }

    // ── Screen-aligned overlays ───────────────────────────────────────────
    // Range rings: smallest step that keeps the ring count <= 4 (so the frame
    // shows 2-4 rings, not a dense stack of them).
    double max_ring_m = (map_area.size.x / 2.0) / px_per_m; // meters to edge
    const double ring_steps[] = { 500, 1000, 2000, 5000, 10000, 20000, 40000, 80000 };
    double ring_step = ring_steps[7];
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
        ci->draw_arc(frame_center_px, rr, 0, TAU, 96, C_RINGS, 1.0, false);
        if (font.is_valid())
        {
            String lbl = (r >= 1000.0)
                ? (String::num_int64((int)(r / 1000.0)) + " km")
                : (String::num_int64((int)r) + " m");
            double lw = double(lbl.length()) * 6.5; // ~px per char at size 11
            ci->draw_string(font, frame_center_px + Vector2(-lw / 2.0, -rr - 4), lbl,
                        HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_RING_LABEL);
        }
    }

    // Course line: dashed, from the aircraft symbol to ~85% of the map radius.
    {
        double len = (map_area.size.x / 2.0) * 0.85;
        // along the nose: straight up when heading-up, turned by the heading when north-up
        const double rel = heading_rad - map_rot();
        const Vector2 dir(std::sin(rel), -std::cos(rel));
        const Vector2 from = frame_center_px + dir * 14.0;
        double seg = 10.0;
        double gap = 6.0;
        double d = 0.0;
        while (d < len - 14.0)
        {
            double d2 = std::min(d + seg, len - 14.0);
            ci->draw_line(from + dir * d, from + dir * d2, C_COURSE, 1.5, false);
            d = d2 + gap;
        }
    }

    // Scale bar (bottom-left of the map area): a bar exactly ring_step long.
    {
        double bar_px = ring_step * px_per_m;
        Vector2 bl(map_area.position.x + 10, map_area.position.y + map_area.size.y - 12);
        ci->draw_line(bl, bl + Vector2(bar_px, 0), Color(1, 1, 1, 0.9), 2.0, false);
        ci->draw_line(bl, bl + Vector2(0, 5), Color(1, 1, 1, 0.9), 2.0, false);
        ci->draw_line(bl + Vector2(bar_px, 0), bl + Vector2(bar_px, 5), Color(1, 1, 1, 0.9), 2.0, false);
        if (font.is_valid())
        {
            String lbl = (ring_step >= 1000.0)
                ? (String::num_int64((int)(ring_step / 1000.0)) + " km")
                : (String::num_int64((int)ring_step) + " m");
            const double nm = ring_step / 1852.0;
            lbl += String(" / ") + String::num(nm, nm < 10.0 ? 1 : 0) + " NM";
            ci->draw_string(font, bl + Vector2(4, -4), lbl,
                        HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_RING_LABEL);
        }
    }

    // ILS feathers up to the 40 km view: outline, the left half shaded
    refresh_ils_marks();
    if (zoom_index <= 3)
    {
        for (const IlsMark &m : ils_marks)
        {
            Vector2 a, bl, bc, br;
            project(m.apex.x, m.apex.y, a);
            if (!map_area.grow(120.0).has_point(a))
            {
                continue;
            }
            project(m.base_l.x, m.base_l.y, bl);
            project(m.base_c.x, m.base_c.y, bc);
            project(m.base_r.x, m.base_r.y, br);
            PackedVector2Array fill;
            fill.push_back(a);
            fill.push_back(bc);
            fill.push_back(bl);
            ci->draw_colored_polygon(fill, m.synthetic ? Color(C_ILS_FILL, 0.12) : C_ILS_FILL);
            PackedVector2Array outline;
            outline.push_back(a);
            outline.push_back(bl);
            outline.push_back(br);
            outline.push_back(a);
            ci->draw_polyline(outline, m.synthetic ? Color(C_ILS, 0.5) : C_ILS, 1.0, false);
            ci->draw_line(a, bc, m.synthetic ? Color(C_ILS, 0.5) : C_ILS, 1.0, false);
            if (font.is_valid() && zoom_index <= 2 && map_area.has_point(bc))
            {
                ci->draw_string(font, bc + Vector2(5, 4), m.label, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, C_LABEL_SHADOW);
                ci->draw_string(font, bc + Vector2(4, 3), m.label, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, C_ILS);
            }
        }
    }

    // Radio navaids (docs/RADIO_NAVIGATION.md): VOR as a hexagon with a
    // centre dot, NDB as a dotted ring, DME / TACAN alone as a square.
    visible_navaids = 0;
    for (const NavaidMark &nv : navaids)
    {
        Vector2 sp;
        project(nv.e, nv.n, sp);
        if (!map_area.has_point(sp))
        {
            continue;
        }
        visible_navaids++;
        if (sel_kind == SEL_NAVAID && sel_index == int(&nv - navaids.data()))
        {
            ci->draw_arc(sp, 10.0, 0, TAU, 24, C_COURSE, 2.0, false);
        }
        if (nv.kind & NavaidsLib::KIND_VOR)
        {
            PackedVector2Array hex;
            for (int k = 0; k <= 6; k++)
            {
                const double a = Math_PI / 6.0 + k * Math_PI / 3.0;
                hex.push_back(sp + Vector2(std::cos(a), std::sin(a)) * 6.0);
            }
            ci->draw_polyline(hex, C_VOR, 1.5, false);
            ci->draw_circle(sp, 1.5, C_VOR);
        }
        else if (nv.kind & NavaidsLib::KIND_NDB)
        {
            ci->draw_circle(sp, 1.8, C_NDB);
            for (int k = 0; k < 12; k++)
            {
                const double a = k * Math_PI / 6.0;
                ci->draw_circle(sp + Vector2(std::cos(a), std::sin(a)) * 6.0, 0.9, C_NDB);
            }
        }
        else
        {
            ci->draw_rect(Rect2(sp.x - 4, sp.y - 4, 8, 8), C_VOR, false, 1.5);
        }
    }

    // ICAO labels (screen-aligned, next to the projected airport positions).
    if (font.is_valid())
    {
        for (const NavaidMark &nv : navaids)
        {
            Vector2 sp;
            project(nv.e, nv.n, sp);
            if (!map_area.grow(8.0).has_point(sp))
            {
                continue;
            }
            if (zoom_index >= 5 && !(nv.kind & NavaidsLib::KIND_VOR))
            {
                continue; // the wide views label the VORs only
            }
            // ident and frequency up to the 20 km view, ident alone beyond
            const String text = zoom_index <= 2 ? nv.ident + String(" ") + nv.freq : nv.ident;
            const Color c = (nv.kind & NavaidsLib::KIND_NDB) ? C_NDB : C_VOR;
            Vector2 pos(sp.x + 8, sp.y + 12);
            ci->draw_string(font, pos + Vector2(1, 1), text, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, C_LABEL_SHADOW);
            ci->draw_string(font, pos, text, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, c);
        }
        for (const Airport &ap : airports)
        {
            Vector2 sp;
            project(ap.e, ap.n, sp);
            if (!map_area.grow(8.0).has_point(sp))
            {
                continue;
            }
            if (zoom_index >= 5 && !ap.international)
            {
                continue;
            }
            double off = ap.international ? 7.0 : 6.0;
            Vector2 pos(sp.x + off, sp.y - off);
            // Shadow pass for legibility over bright terrain.
            ci->draw_string(font, pos + Vector2(1, 1), ap.icao, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_LABEL_SHADOW);
            ci->draw_string(font, pos, ap.icao, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, C_AIRPORT_INTL);
        }
    }

    // Aircraft symbol: centered triangle pointing up (heading-up).
    {
        PackedVector2Array tri;
        const double rel = heading_rad - map_rot(); // nose direction on the map
        tri.push_back(frame_center_px + Vector2(0, -9).rotated(rel));
        tri.push_back(frame_center_px + Vector2(6, 8).rotated(rel));
        tri.push_back(frame_center_px + Vector2(-6, 8).rotated(rel));
        ci->draw_colored_polygon(tri, C_AC_BODY);
        PackedVector2Array edge;
        edge.push_back(tri[0]);
        edge.push_back(tri[1]);
        edge.push_back(tri[2]);
        edge.push_back(tri[0]);
        ci->draw_polyline(edge, C_AC_EDGE, 1.5, false);
    }

    // Orientation buttons: aircraft heading up (an arrowhead) or north up (N
    // under a tick); the active one is lit.
    for (int b = 0; b < 2; b++)
    {
        const bool is_north = b == 1;
        const Rect2 r = is_north ? north_button_rect() : heading_button_rect();
        const bool active = north_up == is_north;
        const Color fg = active ? Color(1.0f, 0.85f, 0.3f) : Color(0.75f, 0.8f, 0.9f, 0.7f);
        ci->draw_rect(r, Color(0.05f, 0.07f, 0.1f, active ? 0.85f : 0.55f));
        ci->draw_rect(r, fg, false, 1.0);
        const Vector2 c = r.get_center();
        if (is_north)
        {
            PackedVector2Array tick;
            tick.push_back(c + Vector2(0, -10));
            tick.push_back(c + Vector2(4, -5));
            tick.push_back(c + Vector2(-4, -5));
            ci->draw_colored_polygon(tick, fg);
            if (font.is_valid())
            {
                ci->draw_string(font, c + Vector2(-5, 9), "N", HORIZONTAL_ALIGNMENT_LEFT, -1, 13, fg);
            }
        }
        else
        {
            PackedVector2Array arrow;
            arrow.push_back(c + Vector2(0, -9));
            arrow.push_back(c + Vector2(7, 8));
            arrow.push_back(c + Vector2(0, 4));
            arrow.push_back(c + Vector2(-7, 8));
            arrow.push_back(c + Vector2(0, -9));
            ci->draw_polyline(arrow, fg, 1.5, true);
        }
    }

    // Centre button: puts a panned view back on the aircraft (lit while panned).
    {
        const Rect2 r = center_button_rect();
        const Color fg = panned ? Color(1.0f, 0.85f, 0.3f) : Color(0.75f, 0.8f, 0.9f, 0.7f);
        ci->draw_rect(r, Color(0.05f, 0.07f, 0.1f, panned ? 0.85f : 0.55f));
        ci->draw_rect(r, fg, false, 1.0);
        const Vector2 c = r.get_center();
        ci->draw_arc(c, 6.0, 0, TAU, 24, fg, 1.5, true);
        ci->draw_circle(c, 1.8, fg);
        ci->draw_line(c + Vector2(0, -10), c + Vector2(0, -6), fg, 1.5, true);
        ci->draw_line(c + Vector2(0, 6), c + Vector2(0, 10), fg, 1.5, true);
        ci->draw_line(c + Vector2(-10, 0), c + Vector2(-6, 0), fg, 1.5, true);
        ci->draw_line(c + Vector2(6, 0), c + Vector2(10, 0), fg, 1.5, true);
    }
}

} // namespace jsbgodot
