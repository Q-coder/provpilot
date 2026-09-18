extends Node
## Headless probe for the orbit / chase camera rig in the ECEF world: after a
## spawn at LSZH, drive CameraRig.update() directly in ORBIT then CHASE mode
## while JSBSim taxies, and check the camera keeps its distance to the
## aircraft, stays upright (no roll about the view axis) and looks at it.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_ecef_camera.tscn

const E := 2683732.0
const N := 1257695.0
var _root: Node = null
var _fails: Array[String] = []
var _phase := ""
var _frames := 0
var _dist0 := -1.0
var _worst_roll := 0.0
var _worst_look := 0.0
var _worst_dist := 0.0

func _fail(m: String) -> void:
	_fails.append(m); printerr("FAIL: " + m)

func _ready() -> void:
	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(4.0).timeout
	_root._on_welcome_flight_requested("c172p", E, N, 160.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(5.0).timeout  # rebase cooldown
	for mode in [[1, "orbit"], [2, "chase"]]:
		_phase = mode[1]
		_root._camera_rig.set_mode(mode[0])
		_frames = 0; _dist0 = -1.0; _worst_roll = 0.0; _worst_look = 0.0; _worst_dist = 0.0
		while _frames < 240:
			await get_tree().process_frame
			_root._camera_rig.update(1.0 / 60.0, -1)
			_frames += 1
			if _frames > 30:
				_measure()
		print("[cam] %s: worst roll %.2f°, worst look-off %.2f°, worst distance drift %.2f m (d0=%.2f)" % [_phase, _worst_roll, _worst_look, _worst_dist, _dist0])
		if _worst_roll > 2.0: _fail("%s camera rolls (%.1f°)" % [_phase, _worst_roll])
		if _worst_look > 5.0: _fail("%s camera does not look at the aircraft (%.1f°)" % [_phase, _worst_look])
		if _worst_dist > 3.0: _fail("%s camera distance drifts (%.1f m)" % [_phase, _worst_dist])
	if _fails.is_empty():
		print("smoke_ecef_camera: OK"); get_tree().quit(0)
	else:
		printerr("smoke_ecef_camera: %d failure(s)" % _fails.size()); get_tree().quit(1)

func _measure() -> void:
	var ac: Node3D = _root.get_node("AC")
	var cam: Camera3D = _root.get_node("AC/Camera2")
	var acp: Vector3 = ac.global_position
	var cp: Vector3 = cam.global_position
	var up: Vector3 = W3DCoords.geodetic_normal(W3DCoords.ecef_to_geodetic(acp).x, W3DCoords.ecef_to_geodetic(acp).y)
	var d: float = (cp - acp).length()
	if _dist0 < 0.0: _dist0 = d
	_worst_dist = maxf(_worst_dist, absf(d - _dist0))
	var b: Basis = cam.global_transform.basis.orthonormalized()
	var view: Vector3 = -b.z
	var to_ac: Vector3 = (acp - cp).normalized()
	_worst_look = maxf(_worst_look, rad_to_deg(acos(clampf(view.dot(to_ac), -1.0, 1.0))))
	# roll = tilt of the camera's right axis out of the local horizontal plane
	var roll := rad_to_deg(asin(clampf(b.x.dot(up), -1.0, 1.0)))
	_worst_roll = maxf(_worst_roll, absf(roll))
	if _frames % 60 == 0:
		print("[cam] %s f%d: dist %.2f m, look-off %.2f°, roll %.2f°" % [_phase, _frames, d, rad_to_deg(acos(clampf(view.dot(to_ac), -1.0, 1.0))), roll])
