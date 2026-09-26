#ifndef SCENERY_EDITOR_H
#define SCENERY_EDITOR_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "scenery_design.h"
#include "scenery_design_mesher.h"

using namespace godot;

namespace jsbgodot
{
    class SceneryEditor;
    class WorldScenery;
    class SkyAtmosphere;
    class SlewMode;
    class TerrainElevation;
    class JSBGodot;

    // Blender-style navigation gizmo of the scenery editor: the east (X,
    // red), north (Y, green) and up (Z, blue) axes as seen by the editor
    // camera, with a hollow circle for each opposite direction. Click a
    // circle for that view, drag anywhere on the gizmo to orbit.
    class SceneryViewGizmo : public Control
    {
        GDCLASS(SceneryViewGizmo, Control);

    public:
        static void _bind_methods();
        SceneryViewGizmo();

        void set_editor(SceneryEditor *ed);
        void _draw() override;
        void _gui_input(const Ref<InputEvent> &event) override;
        // Axis name under a local position, "" when none (the nearest knob wins).
        String pick(const Vector2 &local) const;
        // [[name, screen centre], ...] as drawn last.
        Array get_hit_boxes() const;

    protected:
        static constexpr double R = 42.0;
        static constexpr double KNOB = 9.0;
        SceneryEditor *editor = nullptr;
        bool dragging = false;
        double drag_moved = 0.0;
        Array hit_boxes;
    };

    // Scenery design / developer mode (docs/SCENERY_EDITOR.md; formerly
    // scenery_editor.gd). Independent of the aircraft: F8 freezes the flight
    // model (like slew), takes a free camera and a terrain cursor, and offers
    // tools that edit a SceneryDesign (roads, railways, areas). The design is
    // drawn live by a SceneryDesignMesher and saved as JSON in the scenery
    // library, from where it is also loaded at every flight start so designed
    // scenery shows in normal flight.
    //
    // Keys while active: WASD move, Q/E or PgUp/PgDn down/up, Shift fast,
    // right mouse held = look, wheel = forward/back; 1 select/move, 2 road,
    // 3 rail, 4 delete, 5 circle, 6 switch, 7 replace area, 8 flatten,
    // 9 area, 0 pick; [ ] lower / raise the selected node's height offset or
    // the flatten height by 0.5 m (Shift 5 m); C = curved segments; Space =
    // view straight down (Shift+Space: north up as well); the axis gizmo
    // (right edge): click X / Y / Z or their opposites, drag to orbit; left
    // click = place / pick, Enter or right click = finish a line, Escape =
    // cancel the line (or exit when idle), Ctrl+Z undo, Ctrl+S save, F8 exit.
    //
    // The host is the main scene (node_3d.gd); its modules are read through
    // its properties (_world, _sky, _clouds, _slew_mode, _terrain_elevation,
    // _runways_merged, jsb_node, active_camera, terrain_initialized,
    // _fly_loading, _intro_playing, _edit_session, _escape_held).
    class SceneryEditor : public Node
    {
        GDCLASS(SceneryEditor, Node);

    public:
        enum Tool { TOOL_SELECT, TOOL_ROAD, TOOL_RAIL, TOOL_DELETE, TOOL_CIRCLE, TOOL_SWITCH, TOOL_REPLACE, TOOL_FLATTEN, TOOL_AREA, TOOL_PICK };

        static void _bind_methods();
        SceneryEditor();

        void setup(Node3D *p_host);
        String design_path() const;
        bool design_is_baked();
        Vector3 node_world(int nid) const;
        int node_markers_count() const;
        int node_at_screen(const Vector2 &screen) const;
        Dictionary edge_at_screen(const Vector2 &screen) const;
        double elevation_lv95(double e, double n) const;
        double elevation_lv95_precise(double e, double n) const;
        bool is_active() const;
        void poll(double delta);
        void toggle();
        void enter();
        void enter_at(double e, double n, double heading_deg, double agl = 300.0);
        void enter_common(bool placed = false);
        void exit();
        void update(double delta);
        void apply_camera();
        Dictionary focus() const;
        void set_view(double heading_rad, double pitch_rad);
        void view_top(bool north_up);
        void view_axis(const String &axis);
        void orbit(double dx, double dy);
        Vector3 axis_on_screen(const Vector3 &axis) const;
        Dictionary cursor_hit_from_screen(const Vector2 &screen) const;
        Vector3 world_of(double e, double n, double h) const;
        void update_direction_arrows();
        static double heading_at(const Array &pts, const Vector2 &q);
        Dictionary way_near(double e, double n);
        PackedInt64Array hidden_way_set();
        void apply_hidden_ways();
        Dictionary terrain_patches();
        Dictionary airport_protection() const;
        void apply_terrain_patches(bool force = false);
        void apply_startup_design();
        String edge_label(int eid) const;
        Vector2 baked_endpoint_near(double e, double n, double radius);
        Dictionary baked_join_near(double e, double n, double radius);
        static String way_label(const Dictionary &rec);
        bool pick_way();
        Dictionary current_attrs();
        void adopt_attrs(const Dictionary &attrs);
        bool place_point();
        bool place_switch_point();
        bool place_polygon_point();
        void finish_polygon();
        void nudge_height(double delta);
        bool place_circle_point();
        void finish_line();
        bool delete_at_cursor();
        void undo();
        bool save();
        void set_tool(int t);
        int get_tool() const;
        void rebuild_live();
        Array bake_command() const;
        bool bake_running() const;
        bool bake_area();
        void clear_way_cache();
        // The mouse / key handlers behind _unhandled_input and _input, callable from tests.
        void handle_mouse(const Ref<InputEvent> &event);
        void handle_key(const Ref<InputEvent> &event);

        void _unhandled_input(const Ref<InputEvent> &event) override;
        void _input(const Ref<InputEvent> &event) override;
        void _notification(int what);

        Node3D *get_host() const;
        Ref<SceneryDesign> get_design() const; void set_design(const Ref<SceneryDesign> &d);
        SceneryDesignMesher *get_mesher() const;
        bool get_active() const;
        Camera3D *get_camera() const;
        SceneryViewGizmo *get_gizmo() const;
        Vector3 get_camera_position() const; void set_camera_position(const Vector3 &p);
        double get_yaw() const; void set_yaw(double v);
        double get_pitch() const; void set_pitch(double v);
        Vector2 get_mouse() const; void set_mouse(const Vector2 &m);
        bool get_cursor_valid() const; void set_cursor_valid(bool v);
        double get_cursor_e() const; void set_cursor_e(double v);
        double get_cursor_n() const; void set_cursor_n(double v);
        int get_selected_edge() const; void set_selected_edge(int v);
        int get_selected_node() const; void set_selected_node(int v);
        bool get_modifier_down() const; void set_modifier_down(bool v);
        String get_tool_note() const;
        Array get_line_nodes() const;
        OptionButton *get_class_option() const;
        SpinBox *get_lanes_spin() const;
        CheckBox *get_oneway_check() const;
        CheckBox *get_curved_check() const;
        Ref<ImmediateMesh> get_direction_overlay_mesh() const;

    protected:
        static constexpr double PICK_M = 8.0;
        static constexpr double PICK_PX = 14.0; // screen-space radius for nodes and edges
        static constexpr int TILE_M = 1024;
        static constexpr double HEIGHT_STEP_M = 0.5;
        static constexpr double SNAP_NODE_M = 6.0;
        static constexpr double SNAP_EDGE_M = 4.0;
        static constexpr double MOVE_SPEED = 40.0; // m/s at ground level, grows with height
        static constexpr double LOOK_SENS = 0.004;
        static constexpr double MIN_AGL_M = 2.0;
        static constexpr double RAY_MAX_M = 30000.0;
        static constexpr double CORRIDOR_MARGIN_M = 45.0; // as the bake: half width + the 31 m pixel diagonal
        static constexpr int BAKE_HALF_KM = 3;            // the bake covers this many km each side of the camera
        static constexpr double ARROW_EVERY_M = 40.0;
        static constexpr double ARROW_RADIUS_M = 1500.0;

        Node3D *host = nullptr;
        Ref<SceneryDesign> design;
        SceneryDesignMesher *mesher = nullptr;
        int tool = TOOL_ROAD;
        bool active = false;

        Camera3D *cam = nullptr;
        Vector3 pos; // ECEF
        double yaw = 0.0;
        double pitch = -0.4;
        bool looking = false;
        bool cursor_valid = false;
        double cursor_e = 0.0, cursor_n = 0.0, cursor_h = 0.0;
        Vector3 cursor_world;
        MeshInstance3D *marker = nullptr;
        MeshInstance3D *preview = nullptr;
        Ref<ImmediateMesh> preview_mesh;
        int line_last_node = -1; // node the next segment continues from
        Array line_nodes;        // nodes placed in the line being drawn, in order
        Dictionary line_join;    // baked way joined by the line's first / last point
        int drag_node = -1;
        bool built_once = false;
        String save_note;
        bool modifier_down = false; // Ctrl / Cmd seen on the last key event
        bool rebuild_pending = false;
        bool streamer_was_busy = false;
        bool toggle_held = false;
        CanvasLayer *ui = nullptr;
        Label *status = nullptr;
        Dictionary tool_buttons;
        OptionButton *cls_option = nullptr;
        SpinBox *lanes_spin = nullptr;
        CheckBox *oneway_check = nullptr;
        OptionButton *gauge_option = nullptr;
        Control *road_box = nullptr;
        PanelContainer *panel = nullptr;
        SpinBox *width_spin = nullptr;
        Control *rail_box = nullptr;
        CheckBox *curved_check = nullptr;
        SpinBox *radius_spin = nullptr;
        Control *circle_box = nullptr;
        Vector2 circle_centre;
        bool circle_pending = false;
        Dictionary switch_toe; // {"edge", "e", "n"} after the first Switch click
        Array poly_points;     // replace / flatten polygon being drawn
        double flatten_h = NAN;
        int selected_node = -1;
        int selected_edge = -1;
        OptionButton *area_type = nullptr;
        SpinBox *area_h = nullptr;
        Control *area_box = nullptr;
        SceneryViewGizmo *gizmo = nullptr;
        Dictionary ways;       // OSM way id -> sidecar record, from the road tiles around
        Dictionary ways_tiles; // "dir|E_N" -> true once a sidecar was read (or found missing)
        MultiMeshInstance3D *node_markers = nullptr;
        int bake_pid = -1;
        PackedInt64Array hidden_applied; // last set given to the road managers
        String patches_applied;          // signature of the terrain patches given to the tile manager
        bool patches_stale = true;       // the design changed since the patches were computed
        String bake_log;
        int64_t bake_started = 0;
        Vector2 mouse;
        MeshInstance3D *overlay = nullptr; // existing replace polygons (red)
        Ref<ImmediateMesh> overlay_mesh;
        MeshInstance3D *flat_overlay = nullptr; // existing flatten polygons (blue)
        Ref<ImmediateMesh> flat_overlay_mesh;
        MeshInstance3D *dir_overlay = nullptr; // direction arrows on one-way roads
        Ref<ImmediateMesh> dir_overlay_mesh;
        Vector3 dir_overlay_pos;
        String baked_sha; // sha256 of the design the loaded road tiles carry
        String tool_note;

        // host modules
        WorldScenery *world() const;
        SkyAtmosphere *sky() const;
        Node3D *clouds() const;
        SlewMode *slew() const;
        TerrainElevation *terrain_elevation() const;
        JSBGodot *jsb() const;
        Camera3D *active_camera() const;
        bool host_flag(const char *name) const;
        Array road_managers() const;

        void ensure_loaded();
        void edit_begins();
        void update_markers();
        Basis enu_basis() const;
        Basis heading_basis() const;
        double agl() const;
        void update_cursor();
        void preview_polyline(const Array &pts);
        Array next_segment_points() const;
        Dictionary next_segment_via(const Vector2 &a, const Vector2 &b) const;
        Array switch_preview_points() const;
        void update_overlay();
        void update_preview();
        void load_ways_near(double e, double n);
        Dictionary chain_of(int eid) const;
        void commit_spins();
        int node_at_cursor();
        void orient_line_to_join();
        Array road_regions() const;
        void bake_finished();
        void build_ui();
        void update_status();
        void on_tool_button(int t);
        void on_panel_mouse_exited();
        void on_save_button();
        void on_bake_button();
    };

} // namespace jsbgodot

VARIANT_ENUM_CAST(jsbgodot::SceneryEditor::Tool);

#endif
