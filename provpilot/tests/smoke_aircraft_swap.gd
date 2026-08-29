extends Node
## Regression test: switching to a non-default aircraft (pc7) must not crash.
##
## Guards the "stale lever physics body" bug. c172p (the default) has interactive
## levers, so _setup_*_lever_collision() creates StaticBody3D children of its model
## nodes and stores them in *_lever_body. When you swap to an aircraft without
## levers (pc7), that model subtree is freed on the spot but the *_lever_body vars
## were never reset, so register_debug_bodies() dereferenced freed memory ->
## uncaught std::system_error "mutex lock failed: Invalid argument" -> SIGABRT.
## If that regresses, this scene aborts (non-zero exit) during the flight request.
var _root: Node = null


func _ready() -> void:
	var ps := load("res://node_3d.tscn") as PackedScene
	_root = ps.instantiate()
	add_child(_root)
	print("SMOKE aircraft-swap: main scene up, waiting for setup...")
	await get_tree().create_timer(4.0).timeout
	print("SMOKE aircraft-swap: requesting pc7 flight (triggers the c172p -> pc7 model swap)")
	_root._on_welcome_flight_requested("pc7", 2682163.0, 1282948.0, 250.0)
	await get_tree().create_timer(2.0).timeout
	print("SMOKE aircraft-swap: PASS (survived the c172p -> pc7 model swap)")
	get_tree().quit(0)
