extends Node
## Leaving the editor with Esc and coming back with F8 must leave the keys
## working (gery 2026-09-23: "WASD and other keys don't work any more").
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_editor_reenter.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _key(code: int, pressed: bool) -> void:
	var k := InputEventKey.new()
	k.keycode = code
	k.physical_keycode = code
	k.pressed = pressed
	Input.parse_input_event(k)

func _moves(ed: SceneryEditor, label: String) -> void:
	var before: Vector3 = ed._pos
	_key(KEY_W, true)
	await get_tree().process_frame
	for i in range(5):
		ed.update(0.05)
	_key(KEY_W, false)
	await get_tree().process_frame
	var moved: float = ed._pos.distance_to(before)
	print("  [diag] %s: W moved the camera %.1f m" % [label, moved])
	_check(moved > 1.0, "%s: W moves the camera" % label)

func _ready() -> void:
	get_window().size = Vector2i(1400, 900)
	OS.set_environment("PVP_DESIGN_FILE", OS.get_user_data_dir().path_join("smoke_reenter.json"))
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	var ed: SceneryEditor = root._scenery_editor
	# Edit Mode session: enter, Esc out, Edit Mode again
	root._on_welcome_edit_requested(E, N, 155.0)
	await get_tree().create_timer(3.0).timeout
	_check(ed.is_active(), "edit session entered")
	await _moves(ed, "first edit session")
	_key(KEY_ESCAPE, true)
	await get_tree().process_frame
	await get_tree().process_frame
	_key(KEY_ESCAPE, false)
	await get_tree().process_frame
	_check(not ed.is_active(), "Esc leaves the editor")
	_check(root._welcome_screen.visible, "welcome screen is back after the edit session")
	root._on_welcome_edit_requested(E, N, 155.0)
	await get_tree().create_timer(2.0).timeout
	_check(ed.is_active(), "edit session entered again")
	await _moves(ed, "second edit session")
	_key(KEY_ESCAPE, true)
	await get_tree().process_frame
	await get_tree().process_frame
	_key(KEY_ESCAPE, false)
	await get_tree().process_frame
	# flight: F8 in, Esc out, F8 in again
	root._on_welcome_flight_requested("c172p", E, N, 155.0)
	root._welcome_screen.hide_welcome()   # the Fly! button does this itself
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	waited = 0.0
	while (root._fly_loading or root._intro_playing) and waited < 20.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	if root._fly_loading or root._intro_playing:
		root._cancel_intro_zoom()
		await get_tree().create_timer(1.0).timeout
		root._intro_playing = false
		root._fly_loading = false
	await get_tree().create_timer(1.0).timeout
	_key(KEY_F8, true)
	await get_tree().process_frame
	await get_tree().process_frame
	_key(KEY_F8, false)
	await get_tree().process_frame
	_check(ed.is_active(), "F8 enters the editor in flight")
	await _moves(ed, "first F8 session")
	_key(KEY_ESCAPE, true)
	await get_tree().process_frame
	await get_tree().process_frame
	_key(KEY_ESCAPE, false)
	await get_tree().process_frame
	await get_tree().process_frame
	print("  [diag] after Esc: editor active %s, welcome visible %s" % [ed.is_active(), root._welcome_screen.visible])
	_check(not ed.is_active(), "Esc leaves the editor in flight")
	_check(not root._welcome_screen.visible, "Esc in the editor does not open the welcome screen")
	if root._welcome_screen.visible:
		root._welcome_screen.hide_welcome()
		await get_tree().process_frame
	_key(KEY_F8, true)
	await get_tree().process_frame
	await get_tree().process_frame
	_key(KEY_F8, false)
	await get_tree().process_frame
	_check(ed.is_active(), "F8 enters the editor again")
	await _moves(ed, "second F8 session")
	if _fails.is_empty():
		print("smoke_editor_reenter: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_editor_reenter: %d failure(s)" % _fails.size())
		get_tree().quit(1)
