extends Node
## Measures JSBSim simulation time against wall-clock time while the FDM
## runs (after the ground-steering-style release from slew mode).
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_sim_rate.tscn
var _root: Node = null
func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", 2683732.0, 1257694.0, 160.0)   # LSZH
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	var jsb = _root.jsb_node
	jsb.set_slew_mode(false)
	await get_tree().create_timer(1.0).timeout
	var t0 := Time.get_ticks_msec()
	var s0: float = jsb.get_property("simulation/sim-time-sec")
	await get_tree().create_timer(4.0).timeout
	var t1 := Time.get_ticks_msec()
	var s1: float = jsb.get_property("simulation/sim-time-sec")
	print("probe_sim_rate: sim advanced %.2f s in %.2f s wall (ratio %.2f), physics %d Hz, jsb dt %.5f" % [s1 - s0, (t1 - t0) / 1000.0, (s1 - s0) / ((t1 - t0) / 1000.0), Engine.physics_ticks_per_second, jsb.get_property("simulation/dt")])
	get_tree().quit(0)
