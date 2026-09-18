extends SceneTree
## Diagnostic: wall-clock cost of the main scene's _ready(), of the settings
## Save path (_on_settings_closed) and of SettingsStore.save().
## Run: godot --headless --path provpilot -s tests/probe_timing.gd
var _root: Node = null
var _frame := 0
var _t_ready := 0
func _initialize() -> void:
	var t0 := Time.get_ticks_msec()
	var ps: PackedScene = load("res://node_3d.tscn")
	var t1 := Time.get_ticks_msec()
	_root = ps.instantiate()
	var t2 := Time.get_ticks_msec()
	root.add_child(_root)
	_t_ready = Time.get_ticks_msec()
	print("[timing] load scene %d ms, instantiate %d ms, _ready (add_child) %d ms" % [t1 - t0, t2 - t1, _t_ready - t2])
func _process(_d: float) -> bool:
	_frame += 1
	if _frame == 30:
		var t0 := Time.get_ticks_msec()
		_root._settings.save()
		var t1 := Time.get_ticks_msec()
		_root._on_settings_closed()
		var t2 := Time.get_ticks_msec()
		_root._apply_scenery_sources()
		var t3 := Time.get_ticks_msec()
		_root._apply_tree_density()
		var t4 := Time.get_ticks_msec()
		_root._vfr_map.apply_config()
		var t5 := Time.get_ticks_msec()
		var n: int = _root._settings.scan_scenery()
		var t6 := Time.get_ticks_msec()
		print("[timing] settings.save %d ms | _on_settings_closed %d ms | apply_scenery_sources %d ms | tree_density %d ms | vfr apply_config %d ms | scan_scenery(%d) %d ms" % [t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, n, t6 - t5])
		# Managers reload their manifests lazily in _process: time the next frames.
		_root._apply_scenery_sources()
		_last = Time.get_ticks_msec()
		_watch = 40
	elif _watch > 0:
		var now := Time.get_ticks_msec()
		var dt := now - _last
		_last = now
		if dt > 20:
			print("[timing] frame %d after apply: %d ms" % [41 - _watch, dt])
		_watch -= 1
		if _watch == 0:
			quit(0)
			return true
	return false
var _last := 0
var _watch := 0
