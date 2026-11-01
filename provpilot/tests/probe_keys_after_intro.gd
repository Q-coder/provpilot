extends Node
## Probe: the real Fly! flow (welcome hidden) for an aircraft; wait for the
## intro zoom to finish, then inject C (camera) and H (HUD) key presses and
## report whether they act. Run: godot --headless --path provpilot res://tests/probe_keys_after_intro.tscn -- <aircraft_id>
func _press(k: Key) -> void:
	for pressed in [true, false]:
		var ev := InputEventKey.new()
		ev.keycode = k
		ev.physical_keycode = k
		ev.unicode = k if pressed else 0
		ev.pressed = pressed
		Input.parse_input_event(ev)
		await get_tree().create_timer(0.25).timeout

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "md80"
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested(id, 2682767.0, 1258797.0, 155.0)
	root._welcome_screen.visible = false
	var t := 0.0
	while root._intro_playing and t < 90.0:
		await get_tree().create_timer(1.0).timeout
		t += 1.0
	print("intro finished after %.0f s: playing %s, fly_loading %s, camera_mode %d" % [t, str(root._intro_playing), str(root._fly_loading), root.camera_mode])
	await get_tree().create_timer(2.0).timeout
	var m0: int = root.camera_mode
	await _press(KEY_C)
	await get_tree().create_timer(0.5).timeout
	print("C key: camera_mode %d -> %d" % [m0, root.camera_mode])
	var h0: int = root._hud.get_hud_mode()
	await _press(KEY_H)
	await get_tree().create_timer(0.5).timeout
	print("H key: hud_mode %d -> %d" % [h0, root._hud.get_hud_mode()])
	print("fps %.1f" % Engine.get_frames_per_second())
	get_tree().quit(0)
