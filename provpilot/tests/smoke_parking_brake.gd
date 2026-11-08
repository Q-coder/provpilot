extends Node
## The parking brake set at every flight start must release with the brakes_off
## key (default ",") on every aircraft, with or without a cockpit handle (gery
## 2026-11-08: "the ',' key doesn't release the parking brakes for the Cessna
## and the PC-7"). Spawns each aircraft on the ground, presses the key, and
## checks the FDM's brake command and that the aircraft rolls under power.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_parking_brake.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _key(code: Key, uni: int, pressed: bool) -> void:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.physical_keycode = code
	ev.unicode = uni
	ev.pressed = pressed
	Input.parse_input_event(ev)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	for id in ["c172p", "pc7"]:
		root._on_welcome_flight_requested(id, E, N, 155.0)
		var waited := 0.0
		while not root.terrain_initialized and waited < 60.0:
			await get_tree().create_timer(0.5).timeout
			waited += 0.5
		await get_tree().create_timer(5.0).timeout
		root._cancel_intro_zoom()
		root._welcome_screen.visible = false
		var jsb = root.jsb_node
		var rig = root._aircraft
		_check(jsb.get_input_brake() > 0.5 and rig.get_parking_brake(), "%s: parking brake set at flight start (input %.1f)" % [id, jsb.get_input_brake()])
		print("  [diag] %s left-brake-cmd %.2f, fg controls %d, has /controls/gear/brake-parking %s" % [id, jsb.get_property("fcs/left-brake-cmd-norm"), rig.get_fg_controls().get_count(), str(jsb.has_property("/controls/gear/brake-parking"))])
		_key(KEY_COMMA, ",".unicode_at(0), true)
		await get_tree().process_frame
		_key(KEY_COMMA, ",".unicode_at(0), false)
		await get_tree().create_timer(1.0).timeout
		_check(jsb.get_input_brake() < 0.01, "%s: the brakes_off key clears the brake input (%.2f)" % [id, jsb.get_input_brake()])
		_check(jsb.get_property("fcs/left-brake-cmd-norm") < 0.01 and jsb.get_property("fcs/right-brake-cmd-norm") < 0.01, "%s: the FDM brake commands are released (%.2f / %.2f)" % [id, jsb.get_property("fcs/left-brake-cmd-norm"), jsb.get_property("fcs/right-brake-cmd-norm")])
		_check(not rig.get_parking_brake(), "%s: the rig's parking brake flag follows the key" % id)
		var gs0: float = jsb.get_property("velocities/vg-fps") * 0.592484
		jsb.set_input_throttle(1.0)
		await get_tree().create_timer(10.0).timeout
		var gs1: float = jsb.get_property("velocities/vg-fps") * 0.592484
		print("  [diag] %s ground speed %.1f -> %.1f kt after 10 s at full throttle, rpm %.0f" % [id, gs0, gs1, jsb.get_property("propulsion/engine/engine-rpm")])
		_check(gs1 > gs0 + 15.0, "%s: rolls under full power once the brakes are off (%.1f -> %.1f kt)" % [id, gs0, gs1])
		jsb.set_input_throttle(0.0)
		jsb.set_input_brake(1.0)
	if _fails.is_empty():
		print("smoke_parking_brake: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_parking_brake: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
		get_tree().quit(1)
