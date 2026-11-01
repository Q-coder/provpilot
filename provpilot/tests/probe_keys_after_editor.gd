extends Node
## Probe: do the flight keys (C camera, H HUD) still act after a scenery editor
## round trip? Scenarios (first user arg): "f8" = F8 in, F8 out during the
## flight; "esc" = F8 in, Escape out; "editmode" = welcome Edit Mode, Escape,
## then Fly!. Run: godot --headless --path provpilot res://tests/probe_keys_after_editor.tscn -- f8
func _press(k: Key) -> void:
	for pressed in [true, false]:
		var ev := InputEventKey.new()
		ev.keycode = k
		ev.physical_keycode = k
		ev.unicode = k if pressed else 0
		ev.pressed = pressed
		Input.parse_input_event(ev)
		await get_tree().create_timer(0.25).timeout

func _wait_intro(root: Node) -> void:
	var t := 0.0
	while root._intro_playing and t < 90.0:
		await get_tree().create_timer(1.0).timeout
		t += 1.0
	print("intro finished after %.0f s: fly_loading %s, camera_mode %d" % [t, str(root._fly_loading), root.camera_mode])
	await get_tree().create_timer(2.0).timeout

func _check_keys(root: Node, label: String) -> void:
	var m0: int = root.camera_mode
	await _press(KEY_C)
	await get_tree().create_timer(0.5).timeout
	var h0: int = root._hud.get_hud_mode()
	await _press(KEY_H)
	await get_tree().create_timer(0.5).timeout
	var cam: Camera3D = get_viewport().get_camera_3d()
	print("%s: C camera_mode %d -> %d, H hud_mode %d -> %d, current camera %s, editor active %s, slew %s" % [label, m0, root.camera_mode, h0, root._hud.get_hud_mode(), cam.name if cam else "none", str(root._scenery_editor.is_active()), str(root.jsb_node.is_slew_mode()) if root.jsb_node.has_method("is_slew_mode") else "?"])

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var scenario: String = args[0] if args.size() > 0 else "f8"
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	if scenario == "editmode":
		root._on_welcome_edit_requested(2682767.0, 1258797.0, 155.0)
		root._welcome_screen.visible = false
		await get_tree().create_timer(4.0).timeout
		print("edit mode active %s" % str(root._scenery_editor.is_active()))
		await _press(KEY_ESCAPE)
		await get_tree().create_timer(1.0).timeout
		print("after Escape: editor active %s, welcome visible %s" % [str(root._scenery_editor.is_active()), str(root._welcome_screen.visible)])
	root._on_welcome_flight_requested("c172p", 2682767.0, 1258797.0, 155.0)
	root._welcome_screen.visible = false
	await _wait_intro(root)
	await _check_keys(root, "before editor")
	if scenario != "editmode":
		await _press(KEY_F8)
		await get_tree().create_timer(3.0).timeout
		print("editor active %s" % str(root._scenery_editor.is_active()))
		await _press(KEY_ESCAPE if scenario == "esc" else KEY_F8)
		await get_tree().create_timer(1.0).timeout
		print("after leaving: editor active %s, welcome visible %s, intro %s, ui_up %s" % [str(root._scenery_editor.is_active()), str(root._welcome_screen.visible), str(root._intro_playing), str(root._ui_up())])
		await _check_keys(root, "after editor")
	get_tree().quit(0)
