#ifndef VFR_MAP_H
#define VFR_MAP_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <list>
#include <string>
#include <vector>

using namespace godot;

namespace jsbgodot
{

    // Heading-up VFR map frame (MFD-style inset) for the in-game HUD.
    //
    // Lives as a Control child of the HUD Control (see node_3d.tscn). All
    // logic runs in C++: input (N = toggle, wheel over the frame = zoom),
    // LV95 projection math (double precision), map-tile streaming with an
    // LRU cache, and every pixel via _draw(). GDScript only does a one-time
    // setup() + set_tile_paths() and calls update() once per frame.
    //
    // Imagery: cartographic hillshade tiles generated offline by
    // scripts/generate_vfr_map_tiles.py from the swissALTI3D .raw elevation
    // tiles. Same LV95 1024 m grid + pixel(0,0)=SE-corner orientation as the
    // orthophoto layer, so tile indexing matches Scenery3D conventions:
    //   L0    map_{e}_{n}.jpg        64x64 px  (16 m/px,  1024 m cells)
    //   L3    /mip3/map_{e}_{n}.jpg  64x64 px  (128 m/px, 8192 m cells)
    //   L5    /mip5/map_{e}_{n}.jpg  64x64 px  (512 m/px, 32768 m cells)
    //
    // Overlays: aircraft symbol + course line, adaptive range rings with a km
    // scale bar, airport symbols + ICAO labels (res://airports.json), and
    // canton borders (res://web/data/cantons.geojson). The map world rotates
    // with the aircraft heading; rings/labels/scale stay screen-aligned.
    class VfrMap : public Control
    {
        GDCLASS(VfrMap, Control);

    public:
        static void _bind_methods();

        // One-time wiring from node_3d.gd (after TERRAIN_CENTER is locked):
        // Swiss LV95 origin for Godot<->LV95 conversion and the portable
        // provpilot.cfg path ([map] section). Loads airports + cantons.
        void setup(double swiss_e, double swiss_n, const String &config_path);

        // Scenery area roots (priority order) that may contain a map/ subdir.
        // Tile lookup tries each root in order, like the orthophoto fallback.
        void set_tile_paths(const PackedStringArray &map_dirs);

        // Re-read [map] from provpilot.cfg after a settings save.
        void apply_config();

        // Per-frame drive: Godot-space aircraft position (m), heading in
        // degrees true, and whether a full-screen UI overlay is up (input
        // pass-through + frame hidden while true).
        void update(const Vector3 &godot_pos, double heading_deg, bool ui_visible);
        // Same, with the aircraft position already in LV95 metres (ECEF
        // world hosts convert via geodetic). heading_deg is the GRID heading
        // the map is drawn in (true + grid convergence).
        void update_lv95(double e, double n, double heading_deg, bool ui_visible);

        // ── Public actions (also used by the smoke test) ──
        void toggle_visible();
        void zoom_in();
        void zoom_out();

        // ── Debug / test accessors ──
        double get_heading_rad() const;
        int get_cached_tile_count() const;
        int get_visible_airport_count() const;
        int get_zoom_index() const;
        double get_view_width_m() const;
        bool is_map_enabled() const;
        void set_frame_size(int size_px);

        // Local frame pixels for an LV95 point under the current heading-up
        // transform (frame center + px/m from the last _draw, live aircraft
        // state). Used by the smoke test to verify the map world tracks the
        // aircraft without capturing rendered pixels.
        Vector2 get_projected_px(double e, double n) const;

    public:
        VfrMap();

        // Godot virtuals (must be public for the GDCLASS binding).
        void _ready() override;
        void _notification(int what);           // NOTIFICATION_RESIZED -> re-anchor (not virtual in godot-cpp)
        void _unhandled_input(const Ref<InputEvent> &event) override;
        void _gui_input(const Ref<InputEvent> &event) override;  // wheel zoom over the frame
        void _draw() override;

    protected:
        // ── Tunables ──
        static constexpr int MAX_CACHE_TILES = 256;    // LRU texture cache cap
        static constexpr double MIN_TILE_SCREEN_PX = 40.0; // level-switch threshold
        static constexpr int MAX_LOADS_PER_FRAME = 2;  // decode budget per frame
        static constexpr double REDRAW_INTERVAL = 0.1; // ~10 Hz redraw throttle
        static constexpr int MAX_TILES_PER_DRAW = 4096; // safety valve for the tile loop

        struct LevelDef {
            int cell_m;          // LV95 grid cell size (m)
            const char *subdir;  // "" | "mip3" | "mip5"
        };
        static const LevelDef LEVELS[3];   // L0 base, L3 coarse, L5 overview
        static int level_cell_m(int level);

        static const double VIEW_WIDTHS_M[5];          // {5, 10, 20, 40, 80} km
        static const int FRAME_SIZES_PX[3];            // {240, 320, 400}

        // ── Config / state ──
        double swiss_center_e = 0.0;
        double swiss_center_n = 0.0;
        String config_path;
        PackedStringArray tile_paths;                  // map/ dirs, priority order

        bool enabled = true;
        int frame_size_idx = 1;                        // FRAME_SIZES_PX index
        int zoom_index = 2;                            // VIEW_WIDTHS_M index (20 km)

        double ac_e = 0.0;                             // aircraft LV95 position
        double ac_n = 0.0;
        double heading_rad = 0.0;
        bool ui_visible = false;
        bool has_position = false;
        double last_redraw_ms = -1e9;

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

        int pending_loads = 0;                         // decode budget used this frame

        // ── Static map data ──
        struct Airport {
            double e = 0.0;
            double n = 0.0;
            String icao;
            bool international = false;
        };
        std::vector<Airport> airports;
        // Canton border polylines in LV95 (m). Each polyline is a closed ring.
        std::vector<std::vector<Vector2>> canton_rings;

        // ── Per-frame projection results (filled by _draw) ──
        int visible_airports = 0;
        double px_per_m = 1.0;
        Vector2 frame_center_px;                       // map area center in local px

        // ── Internals ──
        void load_config();
        void _reposition();
        void zoom_step(int step);
        void persist_zoom();
        void load_static_data();
        void parse_airports(const String &path);
        void parse_cantons(const String &path);

        Ref<ImageTexture> get_tile(int ei, int ni, int level);
        bool try_load_tile(const TileKey &key);
        int pick_level() const;                        // 0/1/2 for current zoom
        double frame_size_px() const;
        void load_tile_into_cache(const TileKey &key, const Ref<ImageTexture> &tex);

        // Heading-up LV95 -> local frame pixels (shared by _draw and
        // get_projected_px).
        void project_px(double e, double n, Vector2 &out) const;

        static Vector2 wgs84_to_lv95(double lat_deg, double lon_deg);
    };

} // namespace jsbgodot

#endif
