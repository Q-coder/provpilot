extends SceneTree
## Diagnostic: fog/ambient state the sky script produces at a given camera
## altitude in the ECEF world (compare against the ramp constants).
## Run: godot --headless --path provpilot -s tests/probe_fog.gd
var _scene: Node = null
var _frame := 0
func _initialize() -> void:
	_scene = load("res://node_3d.tscn").instantiate()
	root.add_child(_scene)
func _process(_d: float) -> bool:
	_frame += 1
	if _frame == 10:
		var ac: Node3D = _scene.get_node("AC")
		for alt in [500.0, 7600.0, 30000.0, 62000.0]:
			ac.global_transform = _scene._ac_transform_at(2802245.0, 2686476.0, alt, 184.0, ac.scale)
			var cam: Camera3D = _scene.get_node("AC/Camera2")
			cam.make_current()
			_scene._sky.update(0.016, root)
			var env: Environment = _scene._sky.environment
			var g: Vector3 = W3DCoords.ecef_to_geodetic(cam.global_position)
			print("[fog] alt=%.0f cam_h=%.0f fog_enabled=%s density=%.8f ambient=%.2f glow=%.2f observer=%.2f/%.2f" % [alt, g.z, str(env.fog_enabled), env.fog_density, env.ambient_light_energy, env.glow_intensity, _scene._sky.observer_lat, _scene._sky.observer_lon])
		quit(0)
		return true
	return false
