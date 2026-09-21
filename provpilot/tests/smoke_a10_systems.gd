extends Node
## Subtask 6 (docs/A10_FLIGHTGEAR_PORT.md): the light electrical model — bus
## from battery / generators, powered outputs, generator cautions, master
## caution, lights and panel lighting dead without power.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_a10_systems.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	var fga: FGAnimator = root._fg_animator
	var ctl = root._fg_controls
	var br = root._fg_bridge
	var L: AircraftLights = root._aircraft_lights
	var n2: float = root.jsb_node.get_property("propulsion/engine[0]/n2")
	print("  [diag] engine N2 %.1f, dc %.0f V" % [n2, br.dc_volts])
	_check(br.dc_volts > 27.0 and float(fga.get_property("systems/electrical/outputs/hsi")) > 24.0, "bus powered with battery and generators on")
	_check(float(fga.get_property("systems/A-10-electrical/l-generator-caution-light")) < 0.5, "no generator caution with the engines running")
	_check(str(fga.get_property("sim/model/A-10/instrumentation/warnings/master-caution")) == "false", "master caution off")
	_check(L.is_lit("nav_left"), "nav lights lit with power")
	# generators off: battery keeps the bus, cautions come on, master caution too
	ctl.state["controls/electric/engine/generator"] = 0.0
	ctl.state["controls/electric/engine[1]/generator"] = 0.0
	await get_tree().process_frame
	await get_tree().process_frame
	_check(br.dc_volts > 27.0, "battery alone keeps the bus up")
	_check(float(fga.get_property("systems/A-10-electrical/l-generator-caution-light")) > 0.5 and float(fga.get_property("systems/A-10-electrical/r-generator-caution-light")) > 0.5, "generator caution lights with the generators off")
	_check(str(fga.get_property("sim/model/A-10/instrumentation/warnings/master-caution")) == "true", "master caution on")
	# battery off too: everything dead
	ctl.state["controls/electric/battery-switch"] = 0.0
	await get_tree().process_frame
	await get_tree().process_frame
	_check(br.dc_volts < 0.5 and float(fga.get_property("systems/electrical/outputs/adi")) < 0.5, "bus dead with battery and generators off")
	_check(not L.is_lit("nav_left"), "nav lights out without power")
	_check(float(fga.get_property("controls/lighting/panel-norm")) < 1e-6, "panel lighting off without power")
	# power back: generator switches on with the engines running -> bus up, cautions clear
	ctl.state["controls/electric/battery-switch"] = 1.0
	ctl.state["controls/electric/engine/generator"] = 1.0
	ctl.state["controls/electric/engine[1]/generator"] = 1.0
	await get_tree().process_frame
	await get_tree().process_frame
	_check(br.dc_volts > 27.0 and str(fga.get_property("sim/model/A-10/instrumentation/warnings/master-caution")) == "false", "power restored, master caution clear")
	if _fails.is_empty():
		print("smoke_a10_systems: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_a10_systems: %d failure(s)" % _fails.size())
		get_tree().quit(1)
