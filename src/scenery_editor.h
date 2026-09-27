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
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <vector>

#include "scenery_design.h"
#include "scenery_design_mesher.h"

using namespace godot;

namespace jsbgodot
{
    class SceneryEditor;
    class SceneryGizmoOverlay;
    class SceneryObjects;
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

    // Screen-space drawing of the Object tool's transform gizmo (Blender
    // style, constant size): arrows, rings and cubes computed by the editor.
    class SceneryGizmoOverlay : public Control
    {
        GDCLASS(SceneryGizmoOverlay, Control);

    public:
        static void _bind_methods();
        SceneryGizmoOverlay();
        void set_editor(SceneryEditor *ed);
        void _draw() override;

    protected:
        SceneryEditor *editor = nullptr;
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
    // 9 area, 0 pick, O object (pick a pack model; a Blender-style gizmo with
    // Move / Rotate / Scale modes (toolbar or G / R / Y): drag its arrows,
    // ring or cubes, or the model itself; [ ] height, , . heading, - = scale,
    // Ctrl+G drops it to the ground, Delete removes it; Ctrl+S writes the
    // pack's package.json); [ ] lower / raise the selected node's height offset or
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
        enum Tool { TOOL_SELECT, TOOL_ROAD, TOOL_RAIL, TOOL_DELETE, TOOL_CIRCLE, TOOL_SWITCH, TOOL_REPLACE, TOOL_FLATTEN, TOOL_AREA, TOOL_PICK, TOOL_OBJECT };
        enum GizmoMode { GIZMO_MOVE, GIZMO_ROTATE, GIZMO_SCALE };
        enum Orientation { ORIENT_GLOBAL, ORIENT_LOCAL };   // gizmo axes: world east/north/up or the active model's own
        enum PivotMode { PIVOT_MEDIAN, PIVOT_INDIVIDUAL };  // several objects turn / scale about their median point or each about its own
        // Handles of the Object tool's gizmo: move arrows along the world axes,
        // the free-move centre, rings about the world axes, cubes along the
        // model's own axes, the uniform-scale centre.
        enum GizmoAxis { AXIS_NONE = -1, AXIS_EAST = 0, AXIS_NORTH = 1, AXIS_UP = 2, AXIS_FREE = 3,
            AXIS_ROT_EAST = 4, AXIS_ROT_NORTH = 5, AXIS_ROT_UP = 6,
            AXIS_SCALE_X = 7, AXIS_SCALE_Y = 8, AXIS_SCALE_Z = 9, AXIS_UNIFORM = 10 };
        // A handle as drawn on screen by SceneryGizmoOverlay.
        struct GizmoHandle
        {
            int axis = AXIS_NONE;
            int kind = 0;               // 0 arrow, 1 ring, 2 cube stub, 3 centre circle
            PackedVector2Array pts;     // arrow / stub: [base, tip]; ring: polyline; circle: [centre]
            Vector2 dir;                // arrow / cube: screen direction of the axis
            Color col;
            double facing = 1.0;        // ring: |axis . view direction|, small when seen edge-on
        };
        const std::vector<GizmoHandle> &get_gizmo_handles() const;
        int get_gizmo_active() const;

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
        // Object tool: select the pack object under a screen point (its index in
        // SceneryObjects, -1 none); add = keep the current selection (Shift-click).
        int select_object_at(const Vector2 &screen, bool add = false);
        void select_object(int idx, bool add);
        void add_to_selection(int idx);
        void clear_selection();
        Array get_selection() const;
        int get_selected_object() const; void set_selected_object(int v);
        int get_orientation() const; void set_orientation(int o);
        int get_pivot_mode() const; void set_pivot_mode(int m);
        // Put the camera in front of the selection.
        void frame_selected();
        // Transform panel entry: group 0 location (e, n, agl), 1 rotation (heading, pitch, bank),
        // 2 scale, 3 dimensions; axis 0..2; applied to the active object.
        void set_transform_field(int group, int axis, double value);
        int get_gizmo_mode() const; void set_gizmo_mode(int m);
        // Screen point of a gizmo handle (AXIS_* value) for the selected object, or (-1, -1).
        Vector2 get_gizmo_handle(int axis) const;
        double get_gizmo_size_m() const;
        int gizmo_hit_test(const Vector2 &screen) const;
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
        // Overlay meshes (ImmediateMesh / MultiMesh) store float vertices and
        // instance transforms: at ECEF magnitudes (6.4e6 m) a float step is
        // 0.5 m, so lines written in world coordinates wobble as the camera
        // moves. Each overlay is anchored at a double-precision origin (the
        // camera position when it was built) and holds vertices relative to it.
        Vector3 preview_origin, overlay_origin, flat_origin, dir_origin;
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
        int selected_object = -1;            // Object tool: the active object (index in SceneryObjects)
        std::vector<int> selection;
        std::vector<int> highlighted; // objects currently carrying the selection tint          // every selected object, the active one included
        int orientation = ORIENT_GLOBAL;
        int pivot_mode = PIVOT_MEDIAN;
        // Blender-style transform gizmo of the Object tool
        int gizmo_mode = GIZMO_MOVE;
        int gizmo_axis = AXIS_NONE;          // handle being dragged
        int gizmo_hover = AXIS_NONE;
        Vector2 gizmo_start_mouse;
        Vector2 gizmo_last_mouse;            // last motion event
        Vector2 gizmo_eff_mouse;             // virtual mouse: deltas count 1/10 while Shift is held (precision)
        bool gizmo_moved = false;            // the drag threshold was passed
        Vector3 gizmo_plane_start;           // AXIS_FREE: first hit of the mouse ray on the object's ground plane
        Vector3 gizmo_plane_origin, gizmo_plane_up;   // that plane, fixed for the whole drag
        Dictionary gizmo_start;              // active object's pose when the drag began
        std::vector<int> drag_indices;       // the selection at the drag start
        std::vector<Dictionary> drag_starts; // and their poses
        Vector3 drag_pivot;                  // pivot point (ECEF) at the drag start
        Vector3 drag_axes[3], drag_axes_local[3];       // orientation axes then (ECEF / local frame)
        SceneryGizmoOverlay *gizmo_overlay = nullptr;
        Control *mode_bar = nullptr;
        Dictionary mode_buttons;
        double gizmo_size_m = 1.0;           // world length that projects to GIZMO_PX at the object
        Vector3 gizmo_origin;                // ECEF: the pivot point of the selection
        Vector3 gizmo_east, gizmo_north, gizmo_up;      // world axes at the pivot
        Vector3 gizmo_axes[3], gizmo_axes_local[3];     // the orientation's axes (ECEF and local frame)
        bool gizmo_valid = false;
        std::vector<GizmoHandle> gizmo_handles;
        static constexpr double GIZMO_PX = 90.0;
        // transform panel + object list (right side)
        Control *object_panel = nullptr;
        SpinBox *field_spin[4][3] = {{nullptr}};
        OptionButton *orient_option = nullptr;
        OptionButton *pivot_option = nullptr;
        Control *object_list = nullptr;      // ItemList
        bool panel_updating = false;
        void update_gizmo();
        int gizmo_hit(const Vector2 &screen) const;
        void gizmo_drag(const Vector2 &screen);
        void gizmo_begin(int axis, const Vector2 &screen);
        bool ground_plane_hit(const Vector2 &screen, Vector3 &out) const;
        void on_mode_button(int mode);
        void selection_changed();
        Vector3 selection_pivot() const;
        void move_selected(const Vector3 &delta_ecef, const std::vector<Dictionary> &starts, const std::vector<int> &idx);
        // Place object idx at its start pose shifted by an ECEF vector (exact LV95 conversion), with a rotation / scale.
        void place_from_start(int idx, const Dictionary &start, const Vector3 &delta_ecef, const Basis &rot, const Vector3 &scale);
        void build_object_panel();
        void refresh_object_panel();
        void refresh_object_list();
        void scroll_object_list();
        void on_field_submitted(const String &text);
        bool gui_owns_mouse(Viewport *vp) const;
        void on_field_changed(double value, int field);
        void on_orientation_selected(int idx);
        void on_pivot_selected(int idx);
        void on_pivot_button(int which);
        void on_list_multi_selected(int idx, bool selected);
        void on_list_item_activated(int idx);
        String object_status() const;
        bool object_key(int keycode, bool shift, bool cmd);
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
        SceneryObjects *objects() const;
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
VARIANT_ENUM_CAST(jsbgodot::SceneryEditor::GizmoMode);
VARIANT_ENUM_CAST(jsbgodot::SceneryEditor::GizmoAxis);
VARIANT_ENUM_CAST(jsbgodot::SceneryEditor::Orientation);
VARIANT_ENUM_CAST(jsbgodot::SceneryEditor::PivotMode);

#endif
