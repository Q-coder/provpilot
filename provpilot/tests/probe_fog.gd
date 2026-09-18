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
		for alt in [500.0, 7600.0, 62000.0, 400000.0, 10000000.0]:
			ac.global_transform = _scene._ac_transform_at(2802245.0, 2686476.0, alt, 184.0, ac.scale)
			var cam: Camera3D = _scene.get_node("AC/Camera2")
			cam.make_current()
			_scene._sky.update(0.016, root)
			var env: Environment = _scene._sky.environment
			var g: Vector3 = W3DCoords.ecef_to_geodetic(cam.global_position)
			print("[fog] alt=%.0f fog=%s ambient=%.2f sun_energy=%.2f sun_visible=%s" % [alt, str(env.fog_enabled), env.ambient_light_energy, _scene._sky.sun_light.light_energy, str(_scene._sky.sun_light.visible)])
		quit(0)
		return true
	return false
