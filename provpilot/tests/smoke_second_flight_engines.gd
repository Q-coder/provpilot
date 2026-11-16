extends Node
## Smoke: an aircraft flown as the SECOND aircraft of a session starts with its
## engines running and its sound set audible. 1.1.1: the MD-80 after any other
## aircraft came up cold and silent (its model stepped before the flight was
## set up, the empty feed pipes latched JSBSim's starvation flag and
## set-running could not light the engines; JSBGodot::light_engines).
## Run: godot --headless --path provpilot res://tests/smoke_second_flight_engines.tscn
const E := 2682767.0
const N := 1258797.0
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _fly(root: Node, id: String) -> void:
	root._on_welcome_flight_requested(id, E, N, 155.0)
	await get_tree().create_timer(1.0).timeout
	root._cancel_intro_zoom()
	var waited := 0.0
	while root._fly_loading and waited < 120.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	root._welcome_screen.visible = false
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	root.camera1.current = true
	await get_tree().create_timer(6.0).timeout

func _audible(root: Node) -> int:
	var snd: FGSounds = root._fg_sounds
	var n := 0
	for s in snd.playing():
		if snd.volume_db_of(str(s)) > -60.0:
			n += 1
	return n

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	await _fly(root, "a10")
	_check(root.jsb_node.get_property("propulsion/engine[0]/n2") > 40.0, "A-10 first: engines running (N2 %.1f)" % root.jsb_node.get_property("propulsion/engine[0]/n2"))
	root._welcome_screen.show_welcome()
	await get_tree().create_timer(1.0).timeout
	await _fly(root, "md80")
	var n2: float = root.jsb_node.get_property("propulsion/engine[0]/n2")
	_check(n2 > 40.0, "MD-80 after the A-10: engines running (N2 %.1f)" % n2)
	_check(float(root._aircraft.get_fg_property("systems/electrical/bus/ac-l")) > 100.0, "MD-80 after the A-10: AC bus powered")
	_check(_audible(root) >= 3, "MD-80 after the A-10: sound set audible (%d sounds)" % _audible(root))
	root._welcome_screen.show_welcome()
	await get_tree().create_timer(1.0).timeout
	await _fly(root, "a10")
	_check(root.jsb_node.get_property("propulsion/engine[0]/n2") > 40.0 and _audible(root) >= 2, "A-10 after the MD-80: engines running and audible")
	print("smoke_second_flight_engines: %s" % ("ALL PASS" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(1 if _fails else 0)
