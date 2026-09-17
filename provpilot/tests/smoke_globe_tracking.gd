extends SceneTree
## Phase 2 tracking smoke: boot the real main scene, then jump the AC ~17 km
## away and confirm _track_globe_rebase() re-anchors the globe quietly.
## Run: godot --headless --path provpilot -s tests/smoke_globe_tracking.gd

var _frame := 0
var _scene: Node = null
var _anchor_before: Vector2 = Vector2.ZERO

func _initialize() -> void:
	_scene = load("res://node_3d.tscn").instantiate()
	root.add_child(_scene)

func _process(_delta: float) -> bool:
	_frame += 1
	match _frame:
		5:
			# By now the tracker has anchored at the parked AC (startup).
			if not _scene._globe_has_anchor:
				printerr("FAIL: no anchor after startup frames")
				quit(1)
				return true
			_anchor_before = _scene._globe_anchor_flat
			print("frame5: anchor=(%.0f, %.0f)" % [_anchor_before.x, _anchor_before.y])
			# Jump the AC ~17 km (flat X is West-positive: east = -x).
			var ac := _scene.get_node_or_null("AC") as Node3D
			if not ac:
				printerr("FAIL: no AC node")
				quit(1)
				return true
			ac.global_position += Vector3(-15000.0, 0.0, 8000.0)
			print("frame5: AC moved to (%.0f, %.0f)" % [ac.global_position.x, ac.global_position.z])
		20:
			var a: Vector2 = _scene._globe_anchor_flat
			if not _scene._globe_has_anchor or a.distance_to(_anchor_before) < 10000.0:
				printerr("FAIL: anchor did not follow the AC (before=(%.0f, %.0f) after=(%.0f, %.0f))" % [
					_anchor_before.x, _anchor_before.y, a.x, a.y])
				quit(1)
				return true
			print("frame20: anchor=(%.0f, %.0f) — followed the AC" % [a.x, a.y])
			print("smoke_globe_tracking: OK")
			quit(0)
			return true
	return false
