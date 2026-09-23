class_name SceneryEditor
extends Node
## Scenery design / developer mode (docs/SCENERY_EDITOR.md). Independent of
## the aircraft: F8 freezes the flight model (like slew), takes a free camera
## and a terrain cursor, and offers tools that edit a SceneryDesign (roads
## and railways for now). The design is drawn live by SceneryDesignMesher
## and saved as JSON in the scenery library, from where it is also loaded
## at every flight start so designed scenery shows in normal flight.
##
## Keys while active: WASD move, Q/E or PgUp/PgDn down/up, Shift fast,
## right mouse held = look, wheel = forward/back; 1 select/move, 2 road,
## 3 rail, 4 delete, 5 circle, 6 switch (click a track, then the end of
## the diverging track), 7 replace area (polygon: OSM roads inside are cut
## by the bake), 8 flatten (polygon + height the bake writes into the
## terrain), 9 area (grass / forest / field / water patch or a building
## with a height), 0 pick (click a baked OSM road or track: it becomes
## design nodes and edges and the bake drops the original);
## [ ] lower / raise the selected node's height offset or the
## flatten height by 0.5 m (Shift 5 m); C = curved segments (tangent arcs);
## Space = view straight down (Shift+Space: north up as well); the axis
## gizmo (right edge): click X / Y / Z or their opposites for a side or
## top view, drag it to orbit around the point in the middle of the screen;
## left click = place / pick, Enter or right click = finish a line, Escape
## = cancel the line (or exit when idle), Ctrl+Z undo, Ctrl+S save, F8 exit.

enum Tool { SELECT, ROAD, RAIL, DELETE, CIRCLE, SWITCH, REPLACE, FLATTEN, AREA, PICK }
const PICK_M := 8.0
const PICK_PX := 14.0            # screen-space radius for nodes and edges
const EDITOR_TINT := Color(0.72, 0.84, 1.18)
const TILE_M := 1024
const HEIGHT_STEP_M := 0.5

signal exited()

const DESIGN_SUBDIR := "Design/design"
const DESIGN_FILE := "scenery_design.json"
const SNAP_NODE_M := 6.0
const SNAP_EDGE_M := 4.0
const MOVE_SPEED := 40.0          # m/s at ground level, grows with height
const LOOK_SENS := 0.004
const MIN_AGL_M := 2.0
const RAY_MAX_M := 30000.0

var host: Node3D = null            # node_3d root
var design := SceneryDesign.new()
var mesher := SceneryDesignMesher.new()
var tool: int = Tool.ROAD
var active: bool = false

var _cam: Camera3D = null
var _pos: Vector3 = Vector3.ZERO   # ECEF
var _yaw := 0.0
var _pitch := -0.4
var _looking := false
var _cursor_valid := false
var _cursor_e := 0.0
var _cursor_n := 0.0
var _cursor_h := 0.0
var _cursor_world := Vector3.ZERO
var _marker: MeshInstance3D = null
var _preview: MeshInstance3D = null
var _preview_mesh := ImmediateMesh.new()
var _line_last_node := -1          # node the next segment continues from
var _drag_node := -1
var _built_once := false
var _save_note := ""
var _modifier_down := false        # Ctrl / Cmd seen on the last key event
var _rebuild_pending := false
var _streamer_was_busy := false
var _toggle_held := false
var _ui: CanvasLayer = null
var _status: Label = null
var _tool_buttons: Dictionary = {}
var _cls_option: OptionButton = null
var _lanes_spin: SpinBox = null
var _oneway_check: CheckBox = null
var _gauge_option: OptionButton = null
var _road_box: Control = null
var _rail_box: Control = null
var _curved_check: CheckBox = null
var _radius_spin: SpinBox = null
var _circle_box: Control = null
var _circle_centre := Vector2.ZERO
var _circle_pending := false
var _switch_toe: Dictionary = {}     # {"edge", "e", "n"} after the first Switch click
var _poly_points: Array = []         # replace / flatten polygon being drawn
var _flatten_h := NAN                # target height of the flatten polygon being drawn
var _selected_node := -1             # Select tool: the node whose height offset [ ] edit
var _area_type: OptionButton = null
var _area_h: SpinBox = null
var _area_box: Control = null
var _gizmo: ViewGizmo = null
var _ways: Dictionary = {}           # OSM way id -> sidecar record, from the road tiles around
var _ways_tiles: Dictionary = {}     # "dir|E_N" -> true once a sidecar was read (or found missing)
var _hover_way: Dictionary = {}      # the way under the cursor with the Pick tool
var _node_markers: MultiMeshInstance3D = null
var _bake_pid := -1
var _bake_log := ""
var _bake_started := 0
const BAKE_HALF_KM := 3          # the bake covers this many km each side of the camera
var _mouse := Vector2.ZERO
var _overlay: MeshInstance3D = null  # existing replace polygons (red)
var _overlay_mesh := ImmediateMesh.new()
var _flat_overlay: MeshInstance3D = null   # existing flatten polygons (blue)
var _flat_overlay_mesh := ImmediateMesh.new()
var _baked_sha := ""                 # sha256 of the design the loaded road tiles carry
var _tool_note := ""


func setup(p_host: Node3D) -> void:
	host = p_host
	mesher.name = "SceneryDesignMesher"
	mesher.elevation = Callable(self, "elevation_lv95_precise")   # never the coarse globe: no hovering meshes
	host.add_child(mesher)
	_cam = Camera3D.new()
	_cam.name = "SceneryEditorCamera"
	_cam.far = 200000.0
	_cam.near = 0.5
	_cam.fov = 70.0
	host.add_child(_cam)
	_marker = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.2
	sm.height = 2.4
	_marker.mesh = sm
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(1.0, 0.85, 0.1)
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_marker.material_override = mat
	_marker.visible = false
	host.add_child(_marker)
	_preview = MeshInstance3D.new()
	_preview.mesh = _preview_mesh
	var pm := StandardMaterial3D.new()
	pm.albedo_color = Color(1.0, 0.6, 0.1)
	pm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	pm.no_depth_test = true
	_preview.material_override = pm
	host.add_child(_preview)
	_overlay = MeshInstance3D.new()
	_overlay.mesh = _overlay_mesh
	var om := StandardMaterial3D.new()
	om.albedo_color = Color(1.0, 0.25, 0.2)
	om.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	om.no_depth_test = true
	_overlay.material_override = om
	_overlay.visible = false
	host.add_child(_overlay)
	_flat_overlay = MeshInstance3D.new()
	_flat_overlay.mesh = _flat_overlay_mesh
	var fm := StandardMaterial3D.new()
	fm.albedo_color = Color(0.3, 0.6, 1.0)
	fm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	fm.no_depth_test = true
	_flat_overlay.material_override = fm
	_flat_overlay.visible = false
	host.add_child(_flat_overlay)
	_node_markers = MultiMeshInstance3D.new()
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	var nm := SphereMesh.new()
	nm.radius = 0.9
	nm.height = 1.8
	var nmat := StandardMaterial3D.new()
	nmat.vertex_color_use_as_albedo = true
	nmat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	nmat.no_depth_test = true
	nm.material = nmat
	mm.mesh = nm
	_node_markers.multimesh = mm
	_node_markers.visible = false
	host.add_child(_node_markers)
	_build_ui()
	set_process_unhandled_input(false)
	set_process_input(false)
	design.load(design_path())


## Where the design file lives: an override for tests (PVP_DESIGN_FILE),
## else <install root>/scenery/Design/design/scenery_design.json (the
## install root of provpilot.cfg, the same place the weather profiles
## use: stable across sessions whatever the scenery library holds), else
## user://. Never derived from the scenery areas: their order changes.
func design_path() -> String:
	var env := OS.get_environment("PVP_DESIGN_FILE")
	if env != "":
		return env
	var ir: Node = host.get_node_or_null("/root/InstallRoot") if host else null
	var root := ""
	if ir and ir.has_method("get_install_root"):
		root = str(ir.get_install_root())
	if root == "":
		return OS.get_user_data_dir().path_join("scenery_design").path_join(DESIGN_FILE)
	return root.path_join("scenery").path_join(DESIGN_SUBDIR).path_join(DESIGN_FILE)


## (Re)load the design when its path is known and differs from the loaded one.
func _ensure_loaded() -> void:
	var p := design_path()
	if design.path != p:
		design.load(p)
		_built_once = false
		_baked_sha = ""


## True when the road tiles of an enabled scenery area were baked from
## exactly this design file (its sha256 is in a roads manifest): the tiles
## carry it, so the live meshes stay off until it is edited.
func design_is_baked() -> bool:
	if design.dirty or design.path == "" or not FileAccess.file_exists(design.path):
		return false
	var sha := SceneryDesign.file_sha256(design.path)
	if sha == "":
		return false
	if _baked_sha == sha:
		return true
	for rm in host.get_tree().root.find_children("*", "S3DRoadManager", true, false):
		for dir in rm.get_road_paths():
			var mp: String = str(dir).path_join("manifest.json")
			if not FileAccess.file_exists(mp):
				continue
			var parsed = JSON.parse_string(FileAccess.get_file_as_string(mp))
			if parsed is Dictionary:
				for rec in (parsed as Dictionary).get("designs", []):
					if str(rec.get("sha256", "")) == sha:
						_baked_sha = sha
						return true
	return false


## Live meshes: everything, or nothing while the tiles already carry the design.
func _rebuild_live() -> void:
	if design_is_baked():
		mesher.clear()
	else:
		mesher.rebuild(design)
	_update_overlay()
	_update_markers()


## World position of a design node: the precise ground (or the globe) plus its offset.
func node_world(nid: int) -> Vector3:
	var p := design.node_pos(nid)
	var h := elevation_lv95_precise(p.x, p.y)
	if is_nan(h):
		h = elevation_lv95(p.x, p.y)
	if is_nan(h):
		h = _cursor_h
	return _world_of(p.x, p.y, h + design.node_offset(nid) + 0.6)


## Node markers: yellow spheres (white for the selected node), scaled with distance.
func _update_markers() -> void:
	if _node_markers == null:
		return
	var mm: MultiMesh = _node_markers.multimesh
	var ids: Array = design.nodes.keys()
	mm.instance_count = ids.size()
	for i in range(ids.size()):
		var nid: int = ids[i]
		var w := node_world(nid)
		var sc: float = clampf(_pos.distance_to(w) / 120.0, 0.6, 12.0)
		mm.set_instance_transform(i, Transform3D(Basis().scaled(Vector3(sc, sc, sc)), w))
		mm.set_instance_color(i, Color(1.0, 1.0, 1.0) if nid == _selected_node else Color(1.0, 0.85, 0.1))


func node_markers_count() -> int:
	return _node_markers.multimesh.instance_count if _node_markers else 0


## Screen-space picking: the design node within PICK_PX of a screen point, or -1.
func node_at_screen(screen: Vector2) -> int:
	var best := -1
	var best_d := PICK_PX
	for nid in design.nodes:
		var w := node_world(nid)
		if _cam.is_position_behind(w):
			continue
		var d: float = _cam.unproject_position(w).distance_to(screen)
		if d < best_d:
			best_d = d
			best = nid
	return best


## The design edge within PICK_PX of a screen point: {"id", "e", "n"} (the
## LV95 point of it under the mouse), or empty.
func edge_at_screen(screen: Vector2) -> Dictionary:
	var best := {}
	var best_d := PICK_PX
	for eid in design.edges:
		var ed: Dictionary = design.edges[eid]
		# short world segments (10 m): the screen-linear interpolation below
		# is then accurate enough for the LV95 point under the mouse
		var pts: Array = SceneryDesignMesher._densify(design.edge_points(eid))
		var h0 := design.node_offset(ed["a"])
		var h1 := design.node_offset(ed["b"])
		var prev := Vector2.ZERO
		var prev_ok := false
		var n := pts.size()
		for i in range(n):
			var p: Vector2 = pts[i]
			var gh := elevation_lv95_precise(p.x, p.y)
			if is_nan(gh):
				gh = elevation_lv95(p.x, p.y)
			if is_nan(gh):
				prev_ok = false
				continue
			var w := _world_of(p.x, p.y, gh + lerpf(h0, h1, float(i) / maxf(n - 1, 1)) + 0.3)
			if _cam.is_position_behind(w):
				prev_ok = false
				continue
			var sp: Vector2 = _cam.unproject_position(w)
			if prev_ok:
				var q := SceneryDesign._nearest_on_segment(prev, sp, screen)
				var d: float = q.distance_to(screen)
				if d < best_d:
					best_d = d
					var t: float = (q - prev).length() / maxf((sp - prev).length(), 0.001)
					var pp: Vector2 = (pts[i - 1] as Vector2).lerp(p, t)
					best = {"id": eid, "e": pp.x, "n": pp.y}
			prev = sp
			prev_ok = true
	return best


## Terrain height (m ASL) at an LV95 point, NAN when unknown (falls back
## to the coarse globe: for the cursor and the camera).
func elevation_lv95(e: float, n: float) -> float:
	var ll: Vector2 = Geo.lv95_to_wgs84(e, n)
	return host._terrain_elevation_geodetic(ll.x, ll.y)


## The precise scenery terrain only, NAN until its tile is streamed in:
## meshes built on it never hover over the globe and settle later.
func elevation_lv95_precise(e: float, n: float) -> float:
	var ll: Vector2 = Geo.lv95_to_wgs84(e, n)
	return host._scenery_elevation_geodetic(ll.x, ll.y)


func is_active() -> bool:
	return active


## Called every frame by the host: F8 toggles the mode; in normal flight
## the loaded design is built once the terrain around is streamed in.
func poll(delta: float) -> void:
	_ensure_loaded()
	var pressed := Input.is_key_pressed(KEY_F8)
	if pressed and not _toggle_held:
		toggle()
	_toggle_held = pressed
	if active:
		update(delta)
	elif host.terrain_initialized and design.edge_count() > 0:
		# in flight: build once the streamer is idle, and again whenever new
		# tiles have landed (a chain without precise terrain is left out
		# until then, never drawn on the coarse globe)
		var busy: bool = host._scenery_streamer_busy()
		if (not _built_once and not busy) or (_streamer_was_busy and not busy):
			_rebuild_live()
			_built_once = true
		_streamer_was_busy = busy


func toggle() -> void:
	if active:
		exit()
	else:
		enter()


func enter() -> void:
	# not before the flight is on the ground: the intro zoom / loading HUD
	# owns the camera until then
	if not host.terrain_initialized or host._fly_loading or host._intro_playing:
		return
	_enter_common()


## Edit Mode from the welcome screen: no flight, the camera starts `agl`
## metres over the LV95 point looking along `heading` and the scenery
## streams in around it.
func enter_at(e: float, n: float, heading_deg: float, agl: float = 300.0) -> void:
	var h := elevation_lv95(e, n)
	if is_nan(h):
		h = 500.0
	_pos = _world_of(e, n, h + agl)
	_yaw = deg_to_rad(heading_deg)
	_pitch = -0.7
	_enter_common(true)


func _enter_common(placed: bool = false) -> void:
	_ensure_loaded()
	active = true
	_streamer_was_busy = true   # re-mesh once the tiles around have streamed in
	var cur: Camera3D = host.get_viewport().get_camera_3d()
	var ac: Node3D = host.get_node_or_null("AC")
	var from_cam := false
	if placed:
		pass
	elif cur:
		_pos = cur.global_position
		var agl := _agl()
		from_cam = not is_nan(agl) and agl < 5000.0
	if from_cam:
		var g: Vector3 = W3DCoords.ecef_to_geodetic(_pos)
		var enu: Basis = W3DCoords.enu_to_ecef_transform(g.x, g.y, g.z, true).basis
		var f: Vector3 = enu.inverse() * (-cur.global_transform.basis.z)
		_yaw = atan2(-f.x, f.z)   # heading: local +Z = north, -X = east
		_pitch = asin(clampf(f.y, -1.0, 1.0))
		# start a little higher and looking down so the ground is in view
		_pitch = minf(_pitch, -0.35)
	elif ac and not placed:
		# the camera is in space (intro view) or unknown: 300 m over the
		# aircraft, looking down along its heading
		var g: Vector3 = W3DCoords.ecef_to_geodetic(ac.global_position)
		_pos = ac.global_position
		var enu: Basis = W3DCoords.enu_to_ecef_transform(g.x, g.y, g.z, true).basis
		_pos += enu * Vector3(0, 300.0, 0)
		_yaw = deg_to_rad(host._ac_true_heading(ac))
		_pitch = -0.7
	_apply_camera()
	_cam.current = true
	if host.jsb_node:
		host.jsb_node.set_slew_mode(true)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_ui.visible = true
	set_process_unhandled_input(true)
	set_process_input(true)
	_line_last_node = -1
	if not _built_once or _rebuild_pending:
		_rebuild_live()
		_built_once = true
		_rebuild_pending = false
	_overlay.visible = true
	_flat_overlay.visible = true
	_update_overlay()
	_set_tool(tool)
	mesher.tint = EDITOR_TINT
	_rebuild_live()
	_node_markers.visible = true
	# clear air: no fog, no clouds while designing
	host._sky.clear_air = true
	if host._clouds:
		host._clouds.visible = false
	print("=== SCENERY EDITOR ON === design: %s (%d nodes, %d edges)" % [design.path, design.node_count(), design.edge_count()])


func exit() -> void:
	if design.dirty:
		save()   # never lose an edit on leaving the mode
	active = false
	_looking = false
	_line_last_node = -1
	_drag_node = -1
	_preview_mesh.clear_surfaces()
	_marker.visible = false
	_overlay.visible = false
	_flat_overlay.visible = false
	_ui.visible = false
	host._sky.clear_air = false
	if host._clouds:
		host._clouds.visible = true
	_node_markers.visible = false
	mesher.tint = Color(1, 1, 1)
	_rebuild_pending = true   # the flight view rebuilds without the tint when the streamer settles
	_streamer_was_busy = true
	set_process_unhandled_input(false)
	set_process_input(false)
	if host.jsb_node and not host._slew_mode.is_enabled():
		host.jsb_node.set_slew_mode(false)
	if host.active_camera:
		host.active_camera.current = true
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	print("=== SCENERY EDITOR OFF ===%s" % (" (unsaved changes)" if design.dirty else ""))
	exited.emit()


# ── camera ──────────────────────────────────────────────────────────────

func _enu_basis() -> Basis:
	var g: Vector3 = W3DCoords.ecef_to_geodetic(_pos)
	return W3DCoords.enu_to_ecef_transform(g.x, g.y, g.z, true).basis


## The local ENU frame of W3DCoords has +Z = north and -X = east (the
## aircraft's nose is its +Z). _yaw is a true heading in radians: a camera
## looks down its -Z, so heading 0 needs the extra half turn.
func _heading_basis() -> Basis:
	return Basis(Vector3.UP, -_yaw)


func _apply_camera() -> void:
	var enu := _enu_basis()
	var b: Basis = enu * _heading_basis() * Basis(Vector3.UP, PI) * Basis(Vector3.RIGHT, _pitch)
	_cam.global_transform = Transform3D(b, _pos)


func _agl() -> float:
	var g: Vector3 = W3DCoords.ecef_to_geodetic(_pos)
	var h: float = host._terrain_elevation_geodetic(g.x, g.y)
	return NAN if is_nan(h) else g.z - h


func update(delta: float) -> void:
	# movement in the local ENU frame: forward along the view heading
	var enu := _enu_basis()
	var yaw_b := _heading_basis()
	var fwd: Vector3 = yaw_b * Vector3(0, 0, 1)      # north at heading 0
	var right: Vector3 = yaw_b * Vector3(-1, 0, 0)   # east at heading 0
	var mv := Vector3.ZERO
	# a shortcut (Ctrl+S, Cmd+Z ...) is never a move
	var modifier: bool = _modifier_down or Input.is_key_pressed(KEY_CTRL) or Input.is_key_pressed(KEY_META)
	if not modifier:
		if Input.is_key_pressed(KEY_W):
			mv += fwd
		if Input.is_key_pressed(KEY_S):
			mv -= fwd
		if Input.is_key_pressed(KEY_D):
			mv += right
		if Input.is_key_pressed(KEY_A):
			mv -= right
		if Input.is_key_pressed(KEY_E) or Input.is_key_pressed(KEY_PAGEUP):
			mv += Vector3.UP
		if Input.is_key_pressed(KEY_Q) or Input.is_key_pressed(KEY_PAGEDOWN):
			mv -= Vector3.UP
	var agl := _agl()
	var speed: float = MOVE_SPEED + (0.0 if is_nan(agl) else maxf(agl, 0.0) * 0.8)
	if Input.is_key_pressed(KEY_SHIFT):
		speed *= 4.0
	if mv.length_squared() > 0.0:
		_pos += enu * (mv.normalized() * speed * delta)
	# stay above the ground
	agl = _agl()
	if not is_nan(agl) and agl < MIN_AGL_M:
		_pos += enu * Vector3(0, MIN_AGL_M - agl, 0)
	_apply_camera()
	_mouse = host.get_viewport().get_mouse_position()
	_update_cursor()
	_update_preview()
	if _gizmo:
		_gizmo.queue_redraw()
	if Engine.get_process_frames() % 10 == 0:
		_update_markers()
	if _bake_pid > 0 and not OS.is_process_running(_bake_pid):
		_bake_pid = -1
		_bake_finished()
	# rebuild once the streamer settles after new tiles arrived (heights)
	var busy: bool = host._scenery_streamer_busy()
	if _streamer_was_busy and not busy:
		_rebuild_pending = true
	_streamer_was_busy = busy
	if _rebuild_pending and not busy:
		_rebuild_live()
		_rebuild_pending = false
	_update_status()


# ── views: straight down, axis views, orbit around the focus ────────────

## The terrain point in the middle of the screen and its distance, or empty.
func _focus() -> Dictionary:
	var hit := cursor_hit_from_screen(host.get_viewport().get_visible_rect().size * 0.5)
	if hit.is_empty():
		return {}
	var w: Vector3 = hit["world"]
	return {"world": w, "dist": _pos.distance_to(w)}


## Turn to a heading / pitch, keeping the point in the middle of the
## screen in the middle (the camera moves back along the new view line).
func set_view(heading_rad: float, pitch_rad: float) -> void:
	var f := _focus()
	_yaw = fposmod(heading_rad, TAU)
	_pitch = clampf(pitch_rad, -PI * 0.5, PI * 0.5)
	if not f.is_empty():
		_apply_camera()
		var fwd: Vector3 = -_cam.global_transform.basis.z
		_pos = (f["world"] as Vector3) - fwd * float(f["dist"])
	_apply_camera()
	_update_cursor()


## Space: straight down; Shift+Space: straight down and north up.
func view_top(north_up: bool) -> void:
	set_view(0.0 if north_up else _yaw, -PI * 0.5)


## Gizmo click: "x" from the east looking west, "-x" from the west, "y"
## from the north looking south, "-y" from the south, "z" top view, "-z"
## a level view along the current heading.
func view_axis(axis: String) -> void:
	var side_pitch: float = _pitch if _pitch > -1.2 and _pitch < 0.0 else -0.35
	match axis:
		"x":
			set_view(deg_to_rad(270.0), side_pitch)
		"-x":
			set_view(deg_to_rad(90.0), side_pitch)
		"y":
			set_view(deg_to_rad(180.0), side_pitch)
		"-y":
			set_view(0.0, side_pitch)
		"z":
			set_view(_yaw, -PI * 0.5)
		"-z":
			set_view(_yaw, 0.0)


## Gizmo drag: orbit around the focus (mouse right = view turns right).
func orbit(dx: float, dy: float) -> void:
	set_view(_yaw + dx * LOOK_SENS * 1.5, _pitch - dy * LOOK_SENS * 1.5)


## Direction of a local axis on screen for the gizmo: (screen offset,
## depth) in the camera frame, for east / north / up.
func axis_on_screen(axis: Vector3) -> Vector3:
	var enu := _enu_basis()
	var cam_inv: Basis = _cam.global_transform.basis.inverse()
	var v: Vector3 = cam_inv * (enu * axis)
	return v   # x right, y up, z towards the viewer


# ── cursor: mouse ray against the terrain ───────────────────────────────

## March the camera ray until it goes below the terrain, then bisect.
func cursor_hit_from_screen(screen: Vector2) -> Dictionary:
	var origin: Vector3 = _cam.project_ray_origin(screen)
	var dir: Vector3 = _cam.project_ray_normal(screen)
	var t := 0.0
	var step := 4.0
	var prev_t := 0.0
	var prev_above := true
	var found := false
	while t < RAY_MAX_M:
		var p: Vector3 = origin + dir * t
		var g: Vector3 = W3DCoords.ecef_to_geodetic(p)
		var h: float = host._terrain_elevation_geodetic(g.x, g.y)
		if not is_nan(h) and g.z <= h:
			if prev_above and t > 0.0:
				found = true
				break
			prev_above = false
		else:
			prev_above = true
		prev_t = t
		t += step
		step = minf(step * 1.08, 400.0)
	if not found:
		return {}
	var lo := prev_t
	var hi := t
	for i in range(14):
		var mid := (lo + hi) * 0.5
		var p: Vector3 = origin + dir * mid
		var g: Vector3 = W3DCoords.ecef_to_geodetic(p)
		var h: float = host._terrain_elevation_geodetic(g.x, g.y)
		if not is_nan(h) and g.z <= h:
			hi = mid
		else:
			lo = mid
	var p: Vector3 = origin + dir * hi
	var g: Vector3 = W3DCoords.ecef_to_geodetic(p)
	var en: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
	return {"e": en.x, "n": en.y, "h": g.z, "world": p}


func _update_cursor() -> void:
	var mouse: Vector2 = host.get_viewport().get_mouse_position()
	var hit := cursor_hit_from_screen(mouse)
	_cursor_valid = not hit.is_empty()
	if _cursor_valid:
		_cursor_e = hit["e"]
		_cursor_n = hit["n"]
		_cursor_h = hit["h"]
		_cursor_world = hit["world"]
		# snap to a node (on screen, so elevated nodes and zoomed-out views work too)
		var nid := node_at_screen(mouse)
		if nid < 0:
			nid = design.node_near(_cursor_e, _cursor_n, SNAP_NODE_M)
		if nid >= 0:
			var np := design.node_pos(nid)
			_cursor_e = np.x
			_cursor_n = np.y
		_marker.global_position = _world_of(_cursor_e, _cursor_n, _cursor_h + 0.5)
		var agl := _agl()
		var sc: float = clampf((0.0 if is_nan(agl) else agl) / 150.0, 1.0, 12.0)
		_marker.scale = Vector3(sc, sc, sc)
	_marker.visible = _cursor_valid
	if _drag_node >= 0 and _cursor_valid:
		design.move_node(_drag_node, _cursor_e, _cursor_n)
		_update_markers()


func _world_of(e: float, n: float, h: float) -> Vector3:
	var ll: Vector2 = Geo.lv95_to_wgs84(e, n)
	return W3DCoords.geodetic_to_ecef(ll.x, ll.y, h)


func _preview_polyline(pts: Array) -> void:
	_preview_mesh.clear_surfaces()
	if pts.size() < 2:
		return
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for p in pts:
		var h := elevation_lv95(p.x, p.y)
		if is_nan(h):
			h = _cursor_h
		_preview_mesh.surface_add_vertex(_world_of(p.x, p.y, h + 1.0))
	_preview_mesh.surface_end()


## The segment the next click would add: a tangent arc from the last node
## when curved drawing is on and the node has a tangent, else a straight.
func _next_segment_points() -> Array:
	var a := design.node_pos(_line_last_node)
	var b := Vector2(_cursor_e, _cursor_n)
	var via := _next_segment_via(a, b)
	if via.is_empty():
		return [a, b]
	return SceneryDesign.arc_points(a, via["via"], b)


func _next_segment_via(a: Vector2, b: Vector2) -> Dictionary:
	if _curved_check == null or not _curved_check.button_pressed:
		return {}
	var t := design.node_tangent_out(_line_last_node)
	if t == Vector2.ZERO:
		return {}
	return SceneryDesign.tangent_arc_via(a, t, b)


## The diverging arc the second Switch click would add.
func _switch_preview_points() -> Array:
	if _switch_toe.is_empty() or not _cursor_valid or not design.edges.has(int(_switch_toe["edge"])):
		return []
	var q := Vector2(_cursor_e, _cursor_n)
	var eid := int(_switch_toe["edge"])
	var t := design.track_tangent_at(eid, float(_switch_toe["e"]), float(_switch_toe["n"]), q)
	var toe: Vector2 = design._nearest_on_edge(eid, Vector2(float(_switch_toe["e"]), float(_switch_toe["n"])))["p"]
	var via := SceneryDesign.tangent_arc_via(toe, t, q)
	if via.is_empty():
		return [toe, q]
	return SceneryDesign.arc_points(toe, via["via"], q)


## Outlines of the replace polygons (red, always on top while editing).
func _update_overlay() -> void:
	_overlay_mesh.clear_surfaces()
	for poly in design.replace:
		_overlay_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		for i in range(poly.size() + 1):
			var p: Vector2 = poly[i % poly.size()]
			var h := elevation_lv95(p.x, p.y)
			_overlay_mesh.surface_add_vertex(_world_of(p.x, p.y, (h if not is_nan(h) else _cursor_h) + 1.5))
		_overlay_mesh.surface_end()
	# flatten plateaus: the outline at the target height, plus a drop line at every corner
	_flat_overlay_mesh.clear_surfaces()
	for f in design.flatten:
		var poly: Array = f["poly"]
		var fh: float = float(f["h"])
		_flat_overlay_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		for i in range(poly.size() + 1):
			var p: Vector2 = poly[i % poly.size()]
			_flat_overlay_mesh.surface_add_vertex(_world_of(p.x, p.y, fh + 0.5))
		_flat_overlay_mesh.surface_end()
		_flat_overlay_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
		for p in poly:
			var h := elevation_lv95((p as Vector2).x, (p as Vector2).y)
			_flat_overlay_mesh.surface_add_vertex(_world_of((p as Vector2).x, (p as Vector2).y, (h if not is_nan(h) else fh) + 0.5))
			_flat_overlay_mesh.surface_add_vertex(_world_of((p as Vector2).x, (p as Vector2).y, fh + 0.5))
		_flat_overlay_mesh.surface_end()


func _update_preview() -> void:
	if tool == Tool.REPLACE or tool == Tool.FLATTEN or tool == Tool.AREA:
		var pts: Array = _poly_points.duplicate()
		if _cursor_valid:
			pts.append(Vector2(_cursor_e, _cursor_n))
		if pts.size() >= 3:
			pts.append(pts[0])
		_preview_polyline(pts)
		return
	if tool == Tool.SWITCH:
		_preview_polyline(_switch_preview_points())
		return
	if tool == Tool.CIRCLE and _circle_pending and _cursor_valid:
		var r: float = maxf(_circle_centre.distance_to(Vector2(_cursor_e, _cursor_n)), 5.0)
		var ring: Array = []
		for k in range(33):
			var ang: float = TAU * k / 32
			ring.append(_circle_centre + Vector2(cos(ang), sin(ang)) * r)
		_preview_polyline(ring)
		return
	if _line_last_node < 0 or not _cursor_valid:
		_preview_mesh.clear_surfaces()
		return
	_preview_polyline(_next_segment_points())


# ── baked OSM ways around the cursor (roads_E_N.ways.json sidecars) ──────

## Read the sidecars of the tiles around (e, n) once, from every road path.
func _load_ways_near(e: float, n: float) -> void:
	var te0: int = int(floor(e / TILE_M)) * TILE_M
	var tn0: int = int(floor(n / TILE_M)) * TILE_M
	for rm in host.get_tree().root.find_children("*", "S3DRoadManager", true, false):
		for dir in rm.get_road_paths():
			for te in range(te0 - TILE_M, te0 + 2 * TILE_M, TILE_M):
				for tn in range(tn0 - TILE_M, tn0 + 2 * TILE_M, TILE_M):
					var key := "%s|%d_%d" % [str(dir), te, tn]
					if _ways_tiles.has(key):
						continue
					_ways_tiles[key] = true
					var fp: String = str(dir).path_join("roads_%d_%d.ways.json" % [te, tn])
					if not FileAccess.file_exists(fp):
						continue
					var parsed = JSON.parse_string(FileAccess.get_file_as_string(fp))
					if parsed is Array:
						for rec in parsed:
							if rec is Dictionary and rec.has("id"):
								_ways[int(rec["id"])] = rec


## The baked way nearest to (e, n) within PICK_M, or empty.
func way_near(e: float, n: float) -> Dictionary:
	_load_ways_near(e, n)
	var q := Vector2(e, n)
	var best := {}
	var best_d := PICK_M
	for wid in _ways:
		if wid in design.replace_ids:
			continue   # already picked into the design
		var rec: Dictionary = _ways[wid]
		var pts: Array = rec.get("pts", [])
		for i in range(pts.size() - 1):
			var a := Vector2(float(pts[i][0]), float(pts[i][1]))
			var b := Vector2(float(pts[i + 1][0]), float(pts[i + 1][1]))
			var d: float = SceneryDesign._nearest_on_segment(a, b, q).distance_to(q)
			if d < best_d:
				best_d = d
				best = rec
	return best


static func way_label(rec: Dictionary) -> String:
	if rec.is_empty():
		return "no baked road within %.0f m" % PICK_M
	var extra := ""
	if bool(rec.get("bridge", false)):
		extra += " bridge"
	if bool(rec.get("tunnel", false)):
		extra += " tunnel"
	if str(rec.get("kind", "")) == "rail":
		return "track %s, gauge %d mm%s (OSM way %d)" % [str(rec.get("cls", "")), int(rec.get("gauge", 0)), extra, int(rec.get("id", 0))]
	return "%s, %d lanes%s%s (OSM way %d)" % [str(rec.get("cls", "")), int(rec.get("lanes", 0)), ", one-way" if bool(rec.get("oneway", false)) else "", extra, int(rec.get("id", 0))]


## Pick tool: the baked way under the cursor becomes design nodes / edges.
func pick_way() -> bool:
	if not _cursor_valid:
		return false
	var rec := way_near(_cursor_e, _cursor_n)
	if rec.is_empty():
		_tool_note = way_label(rec)
		return false
	design.snapshot()
	var ids := design.import_way(rec)
	_tool_note = "picked %s: %d nodes; edit them with Select, [ ] for heights" % [way_label(rec), ids.size()]
	mesher.rebuild(design)
	return true


# ── tools ───────────────────────────────────────────────────────────────

func _current_attrs() -> Dictionary:
	if tool == Tool.RAIL:
		var gauge := 1435
		if _gauge_option and _gauge_option.selected == 1:
			gauge = 1000
		return SceneryDesign.rail_attrs("rail" if gauge == 1435 else "narrow_gauge", gauge)
	var cls: String = SceneryDesign.ROAD_CLASSES[_cls_option.selected] if _cls_option else "secondary"
	var lanes: int = int(_lanes_spin.value) if _lanes_spin else 2
	var oneway: bool = _oneway_check.button_pressed if _oneway_check else false
	return SceneryDesign.road_attrs(cls, lanes, oneway)


## Node at the cursor: an existing one when snapped, a split of a crossed
## edge, else a new node.
func _node_at_cursor() -> int:
	var nid := node_at_screen(_mouse)
	if nid < 0:
		nid = design.node_near(_cursor_e, _cursor_n, SNAP_NODE_M)
	if nid >= 0:
		return nid
	var hit := edge_at_screen(_mouse)
	if hit.is_empty():
		hit = design.edge_near(_cursor_e, _cursor_n, SNAP_EDGE_M)
	if not hit.is_empty():
		return design.split_edge(int(hit["id"]), float(hit["e"]), float(hit["n"]))
	return design.add_node(_cursor_e, _cursor_n)


## Left click with the road / rail tool: continue the line to the cursor.
func place_point() -> bool:
	if not _cursor_valid:
		return false
	design.snapshot()
	var via := {}
	if _line_last_node >= 0:
		via = _next_segment_via(design.node_pos(_line_last_node), Vector2(_cursor_e, _cursor_n))
	var nid := _node_at_cursor()
	if _line_last_node >= 0 and nid != _line_last_node:
		var eid := design.add_edge(_line_last_node, nid, _current_attrs())
		if eid > 0 and not via.is_empty():
			design.set_edge_via(eid, via["via"])
		mesher.rebuild(design)
	_line_last_node = nid
	return true


## Switch tool: first click on a track = the toe of the turnout, second
## click = the end of the diverging track (a tangent arc, radius shown).
func place_switch_point() -> bool:
	if not _cursor_valid:
		return false
	if _switch_toe.is_empty():
		var hit := design.edge_near(_cursor_e, _cursor_n, SNAP_NODE_M)
		if hit.is_empty() or design.edges[int(hit["id"])]["kind"] != SceneryDesign.KIND_RAIL:
			_tool_note = "click a track first"
			return false
		_switch_toe = {"edge": int(hit["id"]), "e": float(hit["e"]), "n": float(hit["n"])}
		_tool_note = "now click the end of the diverging track"
		return true
	design.snapshot()
	var res := design.add_switch(int(_switch_toe["edge"]), float(_switch_toe["e"]), float(_switch_toe["n"]), Vector2(_cursor_e, _cursor_n))
	if res.has("error"):
		design.undo()
		_tool_note = "switch refused: %s" % str(res["error"])
		return false
	_tool_note = "switch placed, radius %.0f m" % float(res["radius"])
	_switch_toe = {}
	_preview_mesh.clear_surfaces()
	mesher.rebuild(design)
	return true


## Replace tool: clicks add polygon corners, Enter closes it (three or
## more); the bake cuts every OSM road / track inside such a polygon.
func place_polygon_point() -> bool:
	if not _cursor_valid:
		return false
	if tool == Tool.FLATTEN and _poly_points.is_empty():
		_flatten_h = _cursor_h   # the plateau starts at the ground of the first corner; [ ] adjust it
	_poly_points.append(Vector2(_cursor_e, _cursor_n))
	var what := "replace area"
	if tool == Tool.FLATTEN:
		what = "plateau"
	elif tool == Tool.AREA:
		what = str(SceneryDesign.AREA_TYPES[_area_type.selected]) if _area_type else "area"
	_tool_note = "%d corners, Enter closes the %s" % [_poly_points.size(), what]
	return true


func finish_polygon() -> void:
	if _poly_points.size() >= 3:
		design.snapshot()
		if tool == Tool.FLATTEN:
			design.flatten.append({"poly": _poly_points.duplicate(), "h": _flatten_h if not is_nan(_flatten_h) else _cursor_h})
			_tool_note = "plateau added at %.1f m (%d), baked with --design --corridor" % [float(design.flatten[-1]["h"]), design.flatten.size()]
		elif tool == Tool.AREA:
			var typ: String = str(SceneryDesign.AREA_TYPES[_area_type.selected]) if _area_type else "grass"
			design.areas.append({"poly": _poly_points.duplicate(), "type": typ, "h": float(_area_h.value) if _area_h and typ == "building" else 0.0})
			_tool_note = "%s area added (%d)" % [typ, design.areas.size()]
			mesher.rebuild(design)
		else:
			design.replace.append(_poly_points.duplicate())
			_tool_note = "replace area added (%d), baked with --design" % design.replace.size()
		design.dirty = true
		_update_overlay()
	_poly_points.clear()
	_flatten_h = NAN
	_preview_mesh.clear_surfaces()


## [ and ]: the selected node's height offset, or the plateau height
## while a flatten polygon is being drawn.
func nudge_height(delta: float) -> void:
	if tool == Tool.FLATTEN and not _poly_points.is_empty():
		_flatten_h = (_flatten_h if not is_nan(_flatten_h) else _cursor_h) + delta
		_tool_note = "plateau height %.1f m" % _flatten_h
		return
	if tool == Tool.SELECT and _selected_node >= 0 and design.nodes.has(_selected_node):
		design.snapshot()
		design.set_node_offset(_selected_node, design.node_offset(_selected_node) + delta)
		_tool_note = "node %d height offset %+.1f m" % [_selected_node, design.node_offset(_selected_node)]
		mesher.rebuild(design)


## Circle tool: first click = centre, second click = a point on the ring
## (or the panel radius when the clicks coincide). A one-way ring of
## eight arcs, counter-clockwise: a roundabout for right-hand traffic.
func place_circle_point() -> bool:
	if not _cursor_valid:
		return false
	var p := Vector2(_cursor_e, _cursor_n)
	if not _circle_pending:
		_circle_centre = p
		_circle_pending = true
		return true
	var r: float = _circle_centre.distance_to(p)
	if r < 5.0:
		r = float(_radius_spin.value) if _radius_spin else 20.0
	design.snapshot()
	var cls: String = SceneryDesign.ROAD_CLASSES[_cls_option.selected] if _cls_option else "secondary"
	var lanes: int = int(_lanes_spin.value) if _lanes_spin else 1
	design.add_circle(_circle_centre, r, SceneryDesign.road_attrs(cls, lanes, true))
	if _radius_spin:
		_radius_spin.value = r
	_circle_pending = false
	_preview_mesh.clear_surfaces()
	mesher.rebuild(design)
	return true


func finish_line() -> void:
	if _line_last_node >= 0 and design.degree(_line_last_node) == 0:
		design.nodes.erase(_line_last_node)   # a lone first click
	_line_last_node = -1
	_preview_mesh.clear_surfaces()


func delete_at_cursor() -> bool:
	var hit := edge_at_screen(_mouse)
	if hit.is_empty() and _cursor_valid:
		hit = design.edge_near(_cursor_e, _cursor_n, SNAP_NODE_M)
	if hit.is_empty() and _cursor_valid:
		# a baked OSM road: it cannot leave the tiles before the next bake,
		# but its id goes to the design so the bake drops it
		var rec := way_near(_cursor_e, _cursor_n)
		if not rec.is_empty():
			design.snapshot()
			design.replace_ids.append(int(rec["id"]))
			design.dirty = true
			_tool_note = "%s marked for removal: baked roads stay visible until the next bake" % way_label(rec)
			return true
	if not _cursor_valid:
		return false
	if hit.is_empty():
		var pi := design.replace_near(_cursor_e, _cursor_n, SNAP_NODE_M)
		var fi := design.flatten_near(_cursor_e, _cursor_n, SNAP_NODE_M)
		var ai := design.area_near(_cursor_e, _cursor_n, SNAP_NODE_M)
		if pi < 0 and fi < 0 and ai < 0:
			return false
		design.snapshot()
		if pi >= 0:
			design.replace.remove_at(pi)
		elif fi >= 0:
			design.flatten.remove_at(fi)
		else:
			design.areas.remove_at(ai)
			mesher.rebuild(design)
		design.dirty = true
		_update_overlay()
		return true
	design.snapshot()
	design.remove_edge(int(hit["id"]))
	_tool_note = "edge removed"
	_rebuild_live()
	return true


func undo() -> void:
	if design.undo():
		_line_last_node = -1
		mesher.rebuild(design)


func save() -> bool:
	var ok := design.save(design_path())
	_save_note = "%s %s at %s" % ["saved" if ok else "COULD NOT SAVE", design.path, Time.get_time_string_from_system()]
	print("SceneryEditor: " + _save_note)
	_update_status()
	return ok


func _set_tool(t: int) -> void:
	tool = t
	_line_last_node = -1
	_drag_node = -1
	_circle_pending = false
	_switch_toe = {}
	_poly_points.clear()
	_flatten_h = NAN
	_selected_node = -1
	_tool_note = ""
	_preview_mesh.clear_surfaces()
	for k in _tool_buttons:
		(_tool_buttons[k] as Button).button_pressed = (k == t)
	if _road_box:
		_road_box.visible = t == Tool.ROAD or t == Tool.CIRCLE
	if _rail_box:
		_rail_box.visible = t == Tool.RAIL
	if _circle_box:
		_circle_box.visible = t == Tool.CIRCLE
	if _area_box:
		_area_box.visible = t == Tool.AREA
	if _curved_check:
		_curved_check.visible = t == Tool.ROAD or t == Tool.RAIL


func _unhandled_input(event: InputEvent) -> void:
	if not active:
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_RIGHT:
			if mb.pressed:
				_looking = true
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			else:
				_looking = false
				Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
				if _line_last_node >= 0:
					finish_line()
			host.get_viewport().set_input_as_handled()
		elif mb.button_index == MOUSE_BUTTON_WHEEL_UP or mb.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			if mb.pressed:
				var agl := _agl()
				var stepm: float = maxf(5.0, (0.0 if is_nan(agl) else agl) * 0.15)
				var dir: float = 1.0 if mb.button_index == MOUSE_BUTTON_WHEEL_UP else -1.0
				_pos += (-_cam.global_transform.basis.z) * stepm * dir
				_apply_camera()
			host.get_viewport().set_input_as_handled()
		elif mb.button_index == MOUSE_BUTTON_LEFT:
			if mb.pressed:
				match tool:
					Tool.ROAD, Tool.RAIL:
						place_point()
					Tool.CIRCLE:
						place_circle_point()
					Tool.SWITCH:
						place_switch_point()
					Tool.REPLACE, Tool.FLATTEN, Tool.AREA:
						place_polygon_point()
					Tool.PICK:
						pick_way()
					Tool.DELETE:
						delete_at_cursor()
					Tool.SELECT:
						var nid := node_at_screen(mb.position)
						if nid < 0 and _cursor_valid:
							nid = design.node_near(_cursor_e, _cursor_n, SNAP_NODE_M)
						_selected_node = nid
						_update_markers()
						if nid >= 0:
							design.snapshot()
							_drag_node = nid
							_tool_note = "node %d selected, height offset %+.1f m ([ ] to change, drag to move)" % [nid, design.node_offset(nid)]
						else:
							_tool_note = "no node under the mouse (yellow spheres are nodes)"
			else:
				if _drag_node >= 0:
					_drag_node = -1
					_rebuild_live()
			host.get_viewport().set_input_as_handled()
	elif event is InputEventMouseMotion and _looking:
		var mm := event as InputEventMouseMotion
		_yaw += mm.relative.x * LOOK_SENS   # mouse right = turn right (heading up)
		_pitch = clampf(_pitch - mm.relative.y * LOOK_SENS, -1.5, 1.5)
		_apply_camera()
		host.get_viewport().set_input_as_handled()
	pass


## Keyboard shortcuts run ahead of the GUI (a focused spin box or option
## button would otherwise swallow Ctrl+S).
func _input(event: InputEvent) -> void:
	if not active:
		return
	if event is InputEventKey:
		_modifier_down = (event as InputEventKey).ctrl_pressed or (event as InputEventKey).meta_pressed
	if event is InputEventKey and (event as InputEventKey).pressed and not (event as InputEventKey).echo:
		var k := event as InputEventKey
		var handled := true
		match k.keycode:
			KEY_1:
				_set_tool(Tool.SELECT)
			KEY_2:
				_set_tool(Tool.ROAD)
			KEY_3:
				_set_tool(Tool.RAIL)
			KEY_4:
				_set_tool(Tool.DELETE)
			KEY_5:
				_set_tool(Tool.CIRCLE)
			KEY_6:
				_set_tool(Tool.SWITCH)
			KEY_7:
				_set_tool(Tool.REPLACE)
			KEY_8:
				_set_tool(Tool.FLATTEN)
			KEY_9:
				_set_tool(Tool.AREA)
			KEY_0:
				_set_tool(Tool.PICK)
			KEY_SPACE:
				view_top(k.shift_pressed)
			KEY_BRACKETLEFT:
				nudge_height(-(HEIGHT_STEP_M * (10.0 if k.shift_pressed else 1.0)))
			KEY_BRACKETRIGHT:
				nudge_height(HEIGHT_STEP_M * (10.0 if k.shift_pressed else 1.0))
			KEY_C:
				if _curved_check:
					_curved_check.button_pressed = not _curved_check.button_pressed
			KEY_ENTER, KEY_KP_ENTER:
				if tool == Tool.REPLACE or tool == Tool.FLATTEN or tool == Tool.AREA:
					finish_polygon()
				else:
					finish_line()
			KEY_ESCAPE:
				if _line_last_node >= 0:
					finish_line()
				elif _circle_pending or not _switch_toe.is_empty() or not _poly_points.is_empty():
					_circle_pending = false
					_switch_toe = {}
					_poly_points.clear()
					_preview_mesh.clear_surfaces()
				else:
					exit()
			KEY_Z:
				if k.ctrl_pressed or k.meta_pressed:
					undo()
				else:
					handled = false
			KEY_S:
				if k.ctrl_pressed or k.meta_pressed:
					save()
				else:
					handled = false
			KEY_B:
				if k.ctrl_pressed or k.meta_pressed:
					bake_area()
				else:
					handled = false
			KEY_DELETE, KEY_BACKSPACE:
				delete_at_cursor()
			_:
				handled = false
		if handled:
			host.get_viewport().set_input_as_handled()


# ── bake from the editor ────────────────────────────────────────────────

## The road manager's paths, with the region id of each (the parent of the
## region directory: .../build/switzerland-base/Switzerland/roads).
func _road_regions() -> Array:
	var out: Array = []
	for rm in host.get_tree().root.find_children("*", "S3DRoadManager", true, false):
		for dir in rm.get_road_paths():
			var region_dir: String = str(dir).get_base_dir()          # .../Switzerland
			var region_id: String = region_dir.get_base_dir().get_file()   # switzerland-base
			if region_id != "" and FileAccess.file_exists(region_dir.path_join("terrain").path_join("manifest.json")):
				out.append({"id": region_id, "roads": str(dir), "region_dir": region_dir})
	return out


## The bake for the tiles around the camera: [script, args...]; empty when
## no region or script is available. Terrain writes (--corridor) only when
## the design has something that needs them (roads / tracks, plateaus).
func bake_command() -> Array:
	var ir: Node = host.get_node_or_null("/root/InstallRoot")
	var root: String = str(ir.get_install_root()) if ir and ir.has_method("get_install_root") else ""
	var script := root.path_join("scripts").path_join("build_world_roads.py")
	if root == "" or not FileAccess.file_exists(script):
		return []
	var regions := _road_regions()
	if regions.is_empty():
		return []
	var g: Vector3 = W3DCoords.ecef_to_geodetic(_pos)
	var en: Vector2 = Geo.wgs84_to_lv95(g.x, g.y)
	var e0: int = int(floor(en.x / 1000.0)) * 1000 - BAKE_HALF_KM * 1000
	var n0: int = int(floor(en.y / 1000.0)) * 1000 - BAKE_HALF_KM * 1000
	var args: Array = [script, "--region", str(regions[0]["id"]), "--bbox", str(e0), str(n0), str(e0 + 2 * BAKE_HALF_KM * 1000 + 1000), str(n0 + 2 * BAKE_HALF_KM * 1000 + 1000),
		"--design", design_path()]
	if design.edge_count() > 0 or design.flatten.size() > 0:
		args.append("--corridor")
	return args


func bake_running() -> bool:
	return _bake_pid > 0 and OS.is_process_running(_bake_pid)


## Save, then run the bake for the tiles around the camera in the
## background; the tiles reload when it finishes (see update()).
func bake_area() -> bool:
	if bake_running():
		_tool_note = "a bake is already running"
		return false
	save()
	var cmd := bake_command()
	if cmd.is_empty():
		_tool_note = "cannot bake here: no region with terrain among the road paths, or scripts/build_world_roads.py missing"
		return false
	_bake_log = OS.get_user_data_dir().path_join("bake_area.log")
	var quoted: Array = []
	for a in cmd:
		quoted.append("'" + str(a).replace("'", "'\\''") + "'")
	var shell := "python3 %s > '%s' 2>&1" % [" ".join(quoted), _bake_log]
	_bake_pid = OS.create_process("/bin/sh", ["-c", shell])
	_bake_started = Time.get_ticks_msec()
	_tool_note = "baking %d x %d km around the camera… (log: %s)" % [2 * BAKE_HALF_KM + 1, 2 * BAKE_HALF_KM + 1, _bake_log]
	print("SceneryEditor: bake started: " + shell)
	return _bake_pid > 0


## After the bake: reload the road tiles, forget the sidecar cache, and
## let design_is_baked() find the new manifest signature.
func _bake_finished() -> void:
	var secs: float = (Time.get_ticks_msec() - _bake_started) / 1000.0
	var tail := ""
	if FileAccess.file_exists(_bake_log):
		var lines := FileAccess.get_file_as_string(_bake_log).strip_edges().split("\n")
		tail = lines[-1] if lines.size() > 0 else ""
	for rm in host.get_tree().root.find_children("*", "S3DRoadManager", true, false):
		if rm.has_method("reload_tiles"):
			rm.reload_tiles()
	_ways_tiles.clear()
	_ways.clear()
	_baked_sha = ""
	_rebuild_pending = true
	_streamer_was_busy = true
	_tool_note = "bake finished in %.0f s: %s" % [secs, tail.substr(0, 160)]
	print("SceneryEditor: " + _tool_note)


# ── UI ──────────────────────────────────────────────────────────────────

func _build_ui() -> void:
	_ui = CanvasLayer.new()
	_ui.name = "SceneryEditorUI"
	_ui.layer = 20
	_ui.visible = false
	host.add_child(_ui)
	var panel := PanelContainer.new()
	panel.position = Vector2(12, 12)
	panel.mouse_filter = Control.MOUSE_FILTER_STOP
	_ui.add_child(panel)
	var box := VBoxContainer.new()
	panel.add_child(box)
	var title := Label.new()
	title.text = "Scenery editor  (F8 exit)"
	title.add_theme_font_size_override("font_size", 16)
	box.add_child(title)
	var tools := HBoxContainer.new()
	box.add_child(tools)
	for spec in [[Tool.SELECT, "1 Select"], [Tool.ROAD, "2 Road"], [Tool.RAIL, "3 Rail"], [Tool.DELETE, "4 Delete"], [Tool.CIRCLE, "5 Circle"], [Tool.SWITCH, "6 Switch"], [Tool.REPLACE, "7 Replace"], [Tool.FLATTEN, "8 Flatten"], [Tool.AREA, "9 Area"], [Tool.PICK, "0 Pick"]]:
		var b := Button.new()
		b.text = spec[1]
		b.toggle_mode = true
		b.focus_mode = Control.FOCUS_NONE
		var t: int = spec[0]
		b.pressed.connect(func() -> void: _set_tool(t))
		tools.add_child(b)
		_tool_buttons[t] = b
	_road_box = HBoxContainer.new()
	box.add_child(_road_box)
	_cls_option = OptionButton.new()
	_cls_option.focus_mode = Control.FOCUS_NONE
	for c in SceneryDesign.ROAD_CLASSES:
		_cls_option.add_item(c)
	_cls_option.selected = 3
	_road_box.add_child(_cls_option)
	var ll := Label.new()
	ll.text = " lanes"
	_road_box.add_child(ll)
	_lanes_spin = SpinBox.new()
	_lanes_spin.min_value = 1
	_lanes_spin.max_value = 6
	_lanes_spin.value = 2
	_road_box.add_child(_lanes_spin)
	_oneway_check = CheckBox.new()
	_oneway_check.text = "one-way"
	_oneway_check.focus_mode = Control.FOCUS_NONE
	_road_box.add_child(_oneway_check)
	_curved_check = CheckBox.new()
	_curved_check.text = "curved (C)"
	_curved_check.tooltip_text = "Each new segment continues the last one as a tangent arc"
	_curved_check.focus_mode = Control.FOCUS_NONE
	box.add_child(_curved_check)
	_area_box = HBoxContainer.new()
	box.add_child(_area_box)
	_area_type = OptionButton.new()
	_area_type.focus_mode = Control.FOCUS_NONE
	for t in SceneryDesign.AREA_TYPES:
		_area_type.add_item(t)
	_area_box.add_child(_area_type)
	var hl := Label.new()
	hl.text = " building height m"
	_area_box.add_child(hl)
	_area_h = SpinBox.new()
	_area_h.min_value = 2
	_area_h.max_value = 120
	_area_h.value = 9
	_area_box.add_child(_area_h)
	_circle_box = HBoxContainer.new()
	box.add_child(_circle_box)
	var rl := Label.new()
	rl.text = "radius m (click centre, then the ring) "
	_circle_box.add_child(rl)
	_radius_spin = SpinBox.new()
	_radius_spin.min_value = 5
	_radius_spin.max_value = 500
	_radius_spin.value = 20
	_circle_box.add_child(_radius_spin)
	_rail_box = HBoxContainer.new()
	box.add_child(_rail_box)
	var gl := Label.new()
	gl.text = "gauge "
	_rail_box.add_child(gl)
	_gauge_option = OptionButton.new()
	_gauge_option.focus_mode = Control.FOCUS_NONE
	_gauge_option.add_item("1435 mm standard")
	_gauge_option.add_item("1000 mm metre")
	_rail_box.add_child(_gauge_option)
	var actions := HBoxContainer.new()
	box.add_child(actions)
	var undo_b := Button.new()
	undo_b.text = "Undo (Ctrl+Z)"
	undo_b.focus_mode = Control.FOCUS_NONE
	undo_b.pressed.connect(undo)
	actions.add_child(undo_b)
	var save_b := Button.new()
	save_b.text = "Save (Ctrl+S)"
	save_b.focus_mode = Control.FOCUS_NONE
	save_b.pressed.connect(func() -> void: save())
	actions.add_child(save_b)
	var bake_b := Button.new()
	bake_b.text = "Bake area (Ctrl+B)"
	bake_b.tooltip_text = "Save, then bake the road tiles %d km around the camera with this design and reload them" % (2 * BAKE_HALF_KM + 1)
	bake_b.focus_mode = Control.FOCUS_NONE
	bake_b.pressed.connect(func() -> void: bake_area())
	actions.add_child(bake_b)
	_gizmo = ViewGizmo.new()
	_gizmo.editor = self
	_gizmo.custom_minimum_size = Vector2(120, 120)
	_gizmo.size = Vector2(120, 120)
	_gizmo.anchor_left = 1.0
	_gizmo.anchor_right = 1.0
	_gizmo.anchor_top = 0.0
	_gizmo.anchor_bottom = 0.0
	_gizmo.offset_left = -136
	_gizmo.offset_right = -16
	_gizmo.offset_top = 352
	_gizmo.offset_bottom = 472
	_gizmo.mouse_filter = Control.MOUSE_FILTER_STOP
	_ui.add_child(_gizmo)
	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 12)
	box.add_child(_status)
	var help := Label.new()
	help.add_theme_font_size_override("font_size", 11)
	help.modulate = Color(0.8, 0.8, 0.8)
	help.text = "WASD move, Q E / PgDn PgUp descend / climb, Shift fast, right mouse = look, wheel = closer\nleft click = place point (snaps to nodes, splits crossed lines), Enter / right click = finish line, Esc = cancel"
	box.add_child(help)


func _update_status() -> void:
	if _status == null:
		return
	var cur := "cursor: —"
	if _cursor_valid:
		cur = "cursor: E %.0f  N %.0f  h %.0f m" % [_cursor_e, _cursor_n, _cursor_h]
	var line := ""
	if _line_last_node >= 0:
		var a := design.node_pos(_line_last_node)
		var via := _next_segment_via(a, Vector2(_cursor_e, _cursor_n)) if _cursor_valid else {}
		line = "\nline from node %d, %.0f m to cursor%s" % [_line_last_node, a.distance_to(Vector2(_cursor_e, _cursor_n)),
			(", arc radius %.0f m" % float(via["radius"])) if not via.is_empty() else ""]
	elif _circle_pending and _cursor_valid:
		line = "\ncircle radius %.0f m" % _circle_centre.distance_to(Vector2(_cursor_e, _cursor_n))
	elif tool == Tool.SWITCH and not _switch_toe.is_empty() and _cursor_valid:
		var eid := int(_switch_toe["edge"])
		if design.edges.has(eid):
			var q := Vector2(_cursor_e, _cursor_n)
			var t := design.track_tangent_at(eid, float(_switch_toe["e"]), float(_switch_toe["n"]), q)
			var toe: Vector2 = design._nearest_on_edge(eid, Vector2(float(_switch_toe["e"]), float(_switch_toe["n"])))["p"]
			var via := SceneryDesign.tangent_arc_via(toe, t, q)
			line = "\nturnout radius %s" % (("%.0f m%s" % [float(via["radius"]), "" if float(via["radius"]) >= SceneryDesign.SWITCH_MIN_RADIUS_M else "  (too tight, min %.0f)" % SceneryDesign.SWITCH_MIN_RADIUS_M]) if not via.is_empty() else "—")
	if tool == Tool.PICK and _cursor_valid:
		line += "\nunder the cursor: " + way_label(way_near(_cursor_e, _cursor_n))
	if bake_running():
		line += "\nbaking… %.0f s" % ((Time.get_ticks_msec() - _bake_started) / 1000.0)
	if _tool_note != "":
		line += "\n" + _tool_note
	var baked := "  (baked into the road tiles, live meshes off)" if _baked_sha != "" and not design.dirty else ""
	if tool == Tool.FLATTEN and not _poly_points.is_empty():
		line += "\nplateau height %.1f m ([ ] to change)" % (_flatten_h if not is_nan(_flatten_h) else _cursor_h)
	_status.text = "%s%s\n%d nodes, %d edges, %d replace areas, %d replaced OSM ways, %d plateaus, %d areas, %d meshes, %d piers%s%s\n%s%s%s" % [cur, line, design.node_count(), design.edge_count(), design.replace.size(), design.replace_ids.size(), design.flatten.size(), design.areas.size(), mesher.chain_count + mesher.area_count, mesher.pier_count, baked,
		("  (%d stations without terrain)" % mesher.unknown_height_count) if mesher.unknown_height_count > 0 else "",
		design.path, "  *unsaved*" if design.dirty else "", ("\n" + _save_note) if _save_note != "" else ""]


## Blender-style navigation gizmo: the east (X, red), north (Y, green) and
## up (Z, blue) axes as seen by the editor camera, with a hollow circle for
## each opposite direction. Click a circle for that view, drag anywhere on
## the gizmo to orbit.
class ViewGizmo extends Control:
	const R := 42.0
	const KNOB := 9.0
	const AXES := [["x", Vector3(-1, 0, 0), Color(0.90, 0.30, 0.30), "X"], ["y", Vector3(0, 0, 1), Color(0.40, 0.80, 0.30), "Y"], ["z", Vector3(0, 1, 0), Color(0.35, 0.55, 0.95), "Z"]]
	var editor = null
	var _dragging := false
	var _drag_moved := 0.0
	var _hit_boxes: Array = []   # [[name, screen centre], ...]

	func _draw() -> void:
		var c := size * 0.5
		draw_circle(c, R + KNOB + 6.0, Color(0.10, 0.10, 0.12, 0.55))
		if editor == null or editor._cam == null:
			return
		var items: Array = []
		for spec in AXES:
			var v: Vector3 = editor.axis_on_screen(spec[1])
			items.append({"name": spec[0], "pos": c + Vector2(v.x, -v.y) * R, "depth": v.z, "col": spec[2], "label": spec[3], "positive": true})
			items.append({"name": "-" + spec[0], "pos": c - Vector2(v.x, -v.y) * R, "depth": -v.z, "col": spec[2], "label": "", "positive": false})
		items.sort_custom(func(a, b): return a["depth"] < b["depth"])   # far first
		_hit_boxes.clear()
		for it in items:
			var p: Vector2 = it["pos"]
			var col: Color = it["col"]
			var fade: float = 0.55 + 0.45 * clampf((float(it["depth"]) + 1.0) * 0.5, 0.0, 1.0)
			col = Color(col.r, col.g, col.b, fade)
			if it["positive"]:
				draw_line(c, p, col, 2.5, true)
				draw_circle(p, KNOB, col)
				draw_string(ThemeDB.fallback_font, p + Vector2(-4, 5), it["label"], HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.05, 0.05, 0.05))
			else:
				draw_circle(p, KNOB, Color(col.r, col.g, col.b, fade * 0.35))
				draw_arc(p, KNOB, 0.0, TAU, 24, col, 1.5, true)
			_hit_boxes.append([it["name"], p])

	func _gui_input(event: InputEvent) -> void:
		if event is InputEventMouseButton:
			var mb := event as InputEventMouseButton
			if mb.button_index == MOUSE_BUTTON_LEFT:
				if mb.pressed:
					_dragging = true
					_drag_moved = 0.0
				else:
					_dragging = false
					if _drag_moved < 4.0:
						var name := pick(mb.position)
						if name != "" and editor:
							editor.view_axis(name)
				accept_event()
		elif event is InputEventMouseMotion and _dragging:
			var mm := event as InputEventMouseMotion
			_drag_moved += mm.relative.length()
			if editor:
				editor.orbit(mm.relative.x, mm.relative.y)
			accept_event()

	## Axis name under a local position, "" when none (the nearest knob wins).
	func pick(local: Vector2) -> String:
		var best := ""
		var best_d := KNOB + 3.0
		for hb in _hit_boxes:
			var d: float = (hb[1] as Vector2).distance_to(local)
			if d < best_d:
				best_d = d
				best = hb[0]
		return best
