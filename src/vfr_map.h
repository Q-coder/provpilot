#ifndef VFR_MAP_H
#define VFR_MAP_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace godot;

namespace jsbgodot
{
    class VfrMap;

    // The map world of the VFR map frame: a child Control with clip_contents
    // that covers the map area below the title bar, so tiles, borders, ILS
    // feathers and labels are cut at the frame edge by the renderer. It
    // draws nothing of its own; VfrMap::draw_world() paints into it in frame
    // coordinates (a transform shifts them by the map area's origin).
    class VfrMapCanvas : public Control
    {
        GDCLASS(VfrMapCanvas, Control);

    public:
        static void _bind_methods() {}
        VfrMapCanvas();
        void _draw() override;
        VfrMap *owner = nullptr;
    };

    // Heading-up VFR map frame (MFD-style inset) for the in-game HUD.
    //
    // Lives as a Control child of the HUD Control (see node_3d.tscn). All
    // logic runs in C++: input (N = toggle, wheel over the frame = zoom,
    // title bar drag = move, bottom corner drags = resize), LV95
    // projection math (double precision), map-tile streaming with an LRU
    // cache, and every pixel via _draw(). GDScript only does a one-time
    // setup() + set_tile_paths() and calls update() once per frame.
    //
    // Imagery, in priority order:
    //  1. Cartographic hillshade tiles generated offline by
    //     scripts/generate_vfr_map_tiles.py from the 1 m .raw elevation
    //     tiles (the photoreal add-ons' map/ folder). Same LV95 1024 m grid
    //     + pixel(0,0)=SE-corner orientation as the orthophoto layer:
    //       L0    map_{e}_{n}.jpg        64x64 px  (16 m/px,  1024 m cells)
    //       L3    /mip3/map_{e}_{n}.jpg  64x64 px  (128 m/px, 8192 m cells)
    //       L5    /mip5/map_{e}_{n}.jpg  64x64 px  (512 m/px, 32768 m cells)
    //  2. The base tier's terrain tiles (terrain/tile_{e}_{n}.raw, 34 x 34
    //     float32 per 1024 m cell) rendered at runtime with the same
    //     hillshade x hypsometric tint, one texture per L0 cell and
    //     composites of 8 x 8 / 32 x 32 tiles for L3 / L5 — the ground map
    //     of every installed base pack without an offline bake.
    //
    // Overlays: aircraft symbol + course line, adaptive range rings with a km
    // scale bar, airport symbols + ICAO labels (res://airports.json), radio
    // navaids and ILS feathers, and canton borders (res://web/data/cantons.geojson).
    // The map world rotates with the aircraft heading; rings/labels/scale
    // stay screen-aligned.
    class VfrMap : public Control
    {
        GDCLASS(VfrMap, Control);

    public:
        static void _bind_methods();

        // One-time wiring from node_3d.gd (after TERRAIN_CENTER is locked):
        // Portable provpilot.cfg path ([map] section). Loads airports + cantons.
        void setup(const String &config_path);

        // Scenery area roots (priority order) that may contain a map/ subdir.
        // Tile lookup tries each root in order, like the orthophoto fallback.
        void set_tile_paths(const PackedStringArray &map_dirs);
        // Scenery area terrain/ dirs (priority order): the runtime ground map
        // where no pre-rendered map/ tile exists.
        void set_terrain_paths(const PackedStringArray &terrain_dirs);

        // Re-read [map] from provpilot.cfg after a settings save.
        void apply_config();

        // Per-frame drive: aircraft position in LV95 metres (the host converts
        // its ECEF position via geodetic), heading_deg = the GRID heading the
        // map is drawn in (true + grid convergence), and whether a full-screen
        // UI overlay is up (input pass-through + frame hidden while true).
        void update_lv95(double e, double n, double heading_deg, bool ui_visible);

        // ── Public actions (also used by the smoke test) ──
        void toggle_visible();
        void zoom_in();
        void zoom_out();
        // Frame layout (what the title-bar / corner drags do): move by a
        // screen delta, grow by a delta (the bottom-right corner follows the
        // mouse, or the bottom-left one); all clamp to the parent area and
        // persist to provpilot.cfg [map].
        void move_frame_by(const Vector2 &delta_px);
        void resize_frame_by(const Vector2 &delta_px);
        void resize_frame_left_by(const Vector2 &delta_px);

        // ── Debug / test accessors ──
        double get_heading_rad() const;
        int get_cached_tile_count() const;
        int get_visible_airport_count() const;
        int get_visible_navaid_count() const;
        int get_zoom_index() const;
        double get_view_width_m() const;
        bool is_map_enabled() const;
        void set_frame_size(int size_px);
        Vector2 get_frame_size() const;     // (w, h) px
        Vector2 get_frame_offset() const;   // (right, top) px from the parent's top-right corner

        // Local frame pixels for an LV95 point under the current heading-up
        // transform (frame center + px/m from the last _draw, live aircraft
        // state). Used by the smoke test to verify the map world tracks the
        // aircraft without capturing rendered pixels.
        Vector2 get_projected_px(double e, double n) const;

        // Details panel: select the airport / navaid symbol nearest to a frame
        // pixel (within 14 px; false and the panel closes when there is none),
        // or an airport by ICAO; the panel's text (BBCode) for the tests.
        bool select_at(const Vector2 &local_px);
        bool select_airport(const String &icao);
        void close_info();
        String get_info_text() const;
        void _on_info_close();

        // The map world, painted by the clipped child canvas in frame coordinates.
        void draw_world(CanvasItem *ci);

    public:
        VfrMap();
        ~VfrMap();
        void pan_by(const Vector2 &delta_px);          // drag the map content by this many pixels
        void reset_pan();
        bool is_panned() const;
        void set_background_enabled(bool on);
        bool is_background_enabled() const;
        int get_pending_tile_count() const;            // cells waiting for or in the loader

        // Godot virtuals (must be public for the GDCLASS binding).
        void _ready() override;
        void _unhandled_input(const Ref<InputEvent> &event) override;
        void _gui_input(const Ref<InputEvent> &event) override;  // wheel zoom, move, resize
        void _draw() override;
        void _on_viewport_resized();

    protected:
        // ── Tunables ──
        static constexpr int MAX_CACHE_TILES = 256;    // LRU texture cache floor (the cap follows the view, see cache_cap)
        static constexpr int MAX_CACHE_TILES_HARD = 4096; // ... and its ceiling
        static constexpr int MAX_TILES_PER_LEVEL = 1200; // level choice: coarser when a level needs more tiles for the view
        static constexpr double MIN_TILE_SCREEN_PX = 40.0; // level-switch threshold
        static constexpr int MAX_UPLOADS_PER_FRAME = 24; // finished cell images turned into textures per frame
        static constexpr int LOADER_THREADS = 3;
        static constexpr double REDRAW_INTERVAL = 0.1; // ~10 Hz redraw throttle
        static constexpr double PARKED_REDRAW_INTERVAL = 1.0; // ... 1 Hz while the aircraft does not move (navaid / data refreshes)
        static constexpr int MAX_TILES_PER_DRAW = 4096; // safety valve for the tile loop
        static constexpr double MIN_FRAME_PX = 200.0;  // smallest frame the corner drag allows
        static constexpr double RESIZE_GRIP_PX = 16.0; // bottom corner hit zones
        static constexpr int BASE_TILE_PX = 34;        // base-tier terrain tile side (samples)

        struct LevelDef {
            int cell_m;          // LV95 grid cell size (m)
            const char *subdir;  // "" | "mip3" | "mip5"
        };
        static const LevelDef LEVELS[3];   // L0 base, L3 coarse, L5 overview
        static int level_cell_m(int level);

        static constexpr int N_ZOOM = 7;
        static const double VIEW_WIDTHS_M[N_ZOOM];     // {5, 10, 20, 40, 80, 160, 250} km
        static constexpr double INFO_W = 250.0;        // width of the details panel right of the map (px)
        static const int FRAME_SIZES_PX[3];            // {240, 320, 400}: the Settings presets

        // ── Config / state ──
        String config_path;
        PackedStringArray tile_paths;                  // map/ dirs, priority order
        PackedStringArray terrain_paths;               // terrain/ dirs, priority order

        bool enabled = true;
        int frame_size_idx = 1;                        // FRAME_SIZES_PX index (Settings preset)
        bool layout_loaded = false;                    // load_config() ran once
        double frame_w = 320.0;                        // frame size (px)
        double frame_h = 320.0;
        double frame_right = 16.0;                     // gap to the parent's right edge (px)
        double frame_top = 16.0;                       // gap to the parent's top edge (px)
        int zoom_index = 2;                            // VIEW_WIDTHS_M index (20 km)

        enum DragMode { DRAG_NONE = 0, DRAG_MOVE = 1, DRAG_RESIZE = 2, DRAG_RESIZE_LEFT = 3, DRAG_PAN = 4 };
        // Pan: a drag in the map area moves the view off the aircraft; the
        // view then stays on that spot of the world (pan_e / pan_n) until the
        // centre button puts it back on the aircraft.
        bool panned = false;
        double pan_e = 0.0, pan_n = 0.0;               // view centre (LV95) while panned
        double drag_start_pan_e = 0.0, drag_start_pan_n = 0.0;
        bool drag_pan_moved = false;                   // past the click threshold: a pan, not a click
        Vector2 drag_start_local;
        Rect2 center_button_rect() const;              // frame px, bottom-right of the map area
        void update_frame_center();                    // where the aircraft sits in the frame
        int drag_mode = DRAG_NONE;
        Vector2 drag_start_mouse;                      // global (viewport) px
        double drag_start_right = 0.0, drag_start_top = 0.0;
        double drag_start_w = 0.0, drag_start_h = 0.0;

        double ac_e = 0.0;                             // aircraft LV95 position
        double ac_n = 0.0;
        double heading_rad = 0.0;
        bool ui_visible = false;
        bool has_position = false;
        double last_redraw_ms = -1e9;
        double drawn_e = 0.0, drawn_n = 0.0, drawn_heading_rad = 0.0; // state of the last redraw (a parked aircraft redraws nothing)
        bool drawn_valid = false;

        VfrMapCanvas *canvas = nullptr;                // clipped map world
        Rect2 map_area_rect;                           // map area in frame px (below the title bar)

        // ── Tile cache (LRU) ──
        struct TileEntry {
            int ei = 0;
            int ni = 0;
            int level = 0;                             // 0 = base, 3 = mip3
            Ref<ImageTexture> tex;
        };
        std::list<TileEntry> cache_lru;                // front = most recently used
        struct TileKey {
            int ei = 0;
            int ni = 0;
            int level = 0;
            bool operator==(const TileKey &o) const
            {
                return ei == o.ei && ni == o.ni && level == o.level;
            }
        };
        struct TileSlot {
            TileKey key;
            std::list<TileEntry>::iterator it;         // LRU position
        };
        std::vector<TileSlot> cache_index;             // (ei,ni,level) -> slot
        // Cells with neither a map/ tile nor a terrain tile: never retried
        // until the source paths change (a miss costs up to 1024 opens at L5).
        std::unordered_set<uint64_t> missing_cells;
        static uint64_t cell_hash(const TileKey &key);

        int cache_cap = MAX_CACHE_TILES;               // LRU cap: at least twice the tiles the current view needs
        bool background = true;                        // terrain / map tiles under the symbols (title-bar checkbox)

        // ── Static map data ──
        struct Airport {
            double e = 0.0;
            double n = 0.0;
            String icao;
            bool international = false;
            String name, type;
            double lat = 0.0, lon = 0.0, elev_m = 0.0;
            Array runways;                             // airports.json records: designator, length_m, surface
        };
        std::vector<Airport> airports;
        // Radio navaids (res://navaids.json via NavaidsLib): LV95 position,
        // ident, frequency text and NavaidsLib::KIND_* bits.
        struct NavaidMark {
            double e = 0.0;
            double n = 0.0;
            String ident;
            String freq;
            int kind = 0;
            String name, type;
            double lat = 0.0, lon = 0.0, elev_m = 0.0;
        };
        std::vector<NavaidMark> navaids;
        // ILS localizer feathers (LV95): apex at the threshold, base 7 km out.
        struct IlsMark {
            Vector2 apex, base_l, base_c, base_r;
            String label;
            bool synthetic = false;
        };
        std::vector<IlsMark> ils_marks;
        int ils_marks_gen = -1;
        void refresh_ils_marks();
        // Canton border polylines in LV95 (m). Each polyline is a closed ring.
        std::vector<std::vector<Vector2>> canton_rings;

        // ── Per-frame projection results (filled by _draw) ──
        int visible_airports = 0;
        int visible_navaids = 0;
        double px_per_m = 1.0;
        Vector2 frame_center_px;                       // the aircraft in local px (the map area's centre unless panned)

        // ── Details panel (a click on an airport or navaid symbol) ──
        enum SelKind { SEL_NONE = 0, SEL_AIRPORT = 1, SEL_NAVAID = 2 };
        int sel_kind = SEL_NONE;
        int sel_index = -1;
        RichTextLabel *info_label = nullptr;
        Button *info_close = nullptr;
        String info_text;
        double last_info_ms = -1e9;
        double info_w() const { return sel_kind != SEL_NONE ? INFO_W : 0.0; } // the frame is this much wider while the panel shows
        void place_info();
        void refresh_info();
        String build_info_text() const;

        // ── Internals ──
        void load_config();
        void _reposition();
        void place_canvas();
        void request_redraw();
        Rect2 compute_map_area() const;
        bool in_resize_grip(const Vector2 &local_px) const;      // bottom-right
        bool in_resize_grip_left(const Vector2 &local_px) const; // bottom-left
        void zoom_step(int step);
        void persist_layout();
        void load_static_data();
        void parse_airports(const String &path);
        void load_navaids();
        void parse_cantons(const String &path);

        Ref<ImageTexture> get_tile(int ei, int ni, int level);
        // Cell images are built off the main thread (tile_loader): a wide
        // view needs up to 1024 terrain files per cell, which stalled the
        // frame and filled the map one cell per redraw. Composite cells
        // (L3 / L5) are also kept as PNG under user://map_cache, so a region
        // seen once comes back in milliseconds.
        static Ref<Image> render_terrain_image(const PackedStringArray &terrain_dirs, const TileKey &key);
        static Ref<Image> load_cell_image(const PackedStringArray &map_dirs, const PackedStringArray &terrain_dirs,
                const String &cache_dir, const TileKey &key);
        struct TileLoader;
        std::shared_ptr<TileLoader> loader;
        String cache_dir;                              // user://map_cache/<key of the terrain dirs>, "" = no disk cache
        bool in_background_box(const Vector2 &local_px) const;
        void ensure_loader();
        void push_loader_sources();                    // paths changed: new generation, queue dropped
        void pump_tile_results();                      // main thread: finished images -> textures
        void request_tiles(std::vector<TileKey> &wanted); // nearest first; replaces the waiting queue
        int pick_level() const;                        // 0/1/2 for current zoom and frame size
        double view_reach_m() const;
        int tiles_per_axis(int level) const;           // grid cells the tile loop walks per axis at a level
        double frame_size_px() const;                  // map area width (px)
        void load_tile_into_cache(const TileKey &key, const Ref<ImageTexture> &tex);

        // Heading-up LV95 -> local frame pixels (shared by _draw and
        // get_projected_px).
        void project_px(double e, double n, Vector2 &out) const;

        static Vector2 wgs84_to_lv95(double lat_deg, double lon_deg);
    };

} // namespace jsbgodot

#endif
