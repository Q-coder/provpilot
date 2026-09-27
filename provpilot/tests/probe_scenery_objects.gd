extends Node
## Visual probe of the object packs (PROBE_NO_OBJECTS=1: same view without the models): the Schmerlat pack on top of the Swiss
## detail area, Edit Mode camera straight down over the airfield (north up)
## so the placed models can be compared with the orthophoto. Saves PROBE_PNG.
##   PROBE_PNG=/tmp/lspf.png PROBE_AGL=450 godot --path provpilot -s ... (non-headless)
const E := 2681754.0
const N := 1282705.0

func _ready() -> void:
	var area: String = ProjectSettings.globalize_path("res://").path_join("../scenery/custom/lspf-schmerlat").simplify_path()
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var st = root._settings
	var areas: Array = []
	for a in st.get_scenery_areas():
		if str(a.get("path", "")) != area and str(a.get("path", "")) != "/Users/gery/provpilot/scenery/Switzerland":
			areas.append(a)
	areas.push_front({"name": "Switzerland (probe)", "path": "/Users/gery/provpilot/scenery/Switzerland", "enabled": true, "priority": 1})
	areas.push_front({"name": "lspf-schmerlat (probe)", "path": area, "enabled": true, "priority": 0})
	st.set_scenery_areas(areas)
	root._apply_scenery_sources()
	if OS.get_environment("PROBE_NO_OBJECTS") == "1":
		root._scenery_objects.enabled = false   # the bare orthophoto for comparison
	if root._welcome_screen:
		root._welcome_screen.hide_welcome()
	root._on_welcome_edit_requested(E, N, 0.0)
	root._sky.set_time(false, 13 * 60)
	var ed: SceneryEditor = root._scenery_editor
	var agl := float(OS.get_environment("PROBE_AGL")) if OS.get_environment("PROBE_AGL") != "" else 450.0
	var yaw := float(OS.get_environment("PROBE_YAW")) if OS.get_environment("PROBE_YAW") != "" else 0.0
	var pitch := float(OS.get_environment("PROBE_PITCH")) if OS.get_environment("PROBE_PITCH") != "" else 90.0
	var waited := 0.0
	while waited < 25.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
		var h := ed.elevation_lv95_precise(E, N)
		if is_nan(h):
			h = 460.0
		# keep E/N in the middle of the view: step back along the heading by agl / tan(pitch)
		var back: float = agl / tan(deg_to_rad(maxf(pitch, 5.0))) if pitch < 89.0 else 0.0
		ed.camera_position = ed.world_of(E - sin(deg_to_rad(yaw)) * back, N - cos(deg_to_rad(yaw)) * back, h + agl)
		ed.yaw = deg_to_rad(yaw)
		ed.pitch = -deg_to_rad(pitch)
		ed.apply_camera()
	ed.get_gizmo().visible = false
	var so = root._scenery_objects
	# PROBE_GIZMO=move|rotate|scale: select the main building with the Object tool and show its gizmo
	var gm := OS.get_environment("PROBE_GIZMO")
	if gm != "":
		ed.set_tool(SceneryEditor.TOOL_OBJECT)
		ed.set_gizmo_mode({"move": SceneryEditor.GIZMO_MOVE, "rotate": SceneryEditor.GIZMO_ROTATE, "scale": SceneryEditor.GIZMO_SCALE}.get(gm, SceneryEditor.GIZMO_MOVE))
		var bld: Node3D = so.find_object_node("schmerlat_hauptgebaeude")
		if bld:
			ed.select_object_at(ed.get_camera().unproject_position(bld.global_position + bld.global_transform.basis.y.normalized() * 3.0))
		for i in range(3):
			ed.update(0.016)
			await get_tree().process_frame
	await get_tree().process_frame
	await get_tree().process_frame
	print("probe_scenery_objects: %d of %d objects loaded, %d buildings excluded" % [so.get_loaded_count(), so.get_object_count(),
		root.scenery3d_node.find_children("*", "S3DBuildingManager", true, false)[0].get_excluded_building_count()])
	var png := OS.get_environment("PROBE_PNG")
	if png != "":
		get_viewport().get_texture().get_image().save_png(png)
		print("probe_scenery_objects: screenshot ", png)
	get_tree().quit(0)
