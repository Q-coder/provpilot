extends Node
## Probe: Fly! an aircraft and print the loading state machine every 5 s
## without cancelling the intro zoom (what gates the hand-over to flight).
## Run: godot --headless --path provpilot res://tests/probe_loading_state.tscn -- <aircraft_id>
func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var id: String = args[0] if args.size() > 0 else "md80"
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested(id, 2682767.0, 1258797.0, 155.0)
	for i in range(14):
		await get_tree().create_timer(5.0).timeout
		var ring: Vector2i = root._world.count_loaded_ring(2682767.0, 1258797.0, 200.0)
		print("[%3d s] fly_loading %s intro %s phase %d complete %s init_pending %s cooldown %d ring %s fdm_init %s terrain_init %s" % [
			(i + 1) * 5, str(root._fly_loading), str(root._intro_playing), root._loading_progress.get_phase(), str(root._loading_progress.is_complete()),
			str(root._spawn.is_init_pending()), root._terrain_elevation.get_rebase_cooldown(), str(ring), str(root.jsb_node.is_initialized()), str(root.terrain_initialized)])
		if not root._fly_loading and not root._intro_playing:
			print("hand-over to flight complete")
			break
	get_tree().quit(0)
