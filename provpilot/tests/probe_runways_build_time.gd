extends SceneTree
## Merge every base pack and build the airports around LSZH by distance
## (as the host does): reports how many get built and how long it takes.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot -s tests/probe_runways_build_time.gd
func _initialize() -> void:
	var areas: Array = []
	var i := 10
	for p in ["denmark/Denmark","germany/Germany","switzerland-base/Switzerland","austria/Austria","italy/Italy","france/France","norway/Norway","sweden/Sweden","finland/Finland"]:
		areas.append({"path": "/Volumes/Data1/provpilot/build/" + p, "enabled": true, "priority": i}); i += 1
	var t0 := Time.get_ticks_msec()
	var merged: Dictionary = load("res://runways_lib.gd").load_merged(areas)
	var t1 := Time.get_ticks_msec()
	var rws: Node3D = load("res://runways.gd").new()
	root.add_child(rws)
	rws.set_merged(merged)
	var cam := W3DCoords.geodetic_to_ecef(47.4647, 8.5492, 1500.0)
	var rounds := 0
	var last := -1
	while rws.built_count() != last and rounds < 500:
		last = rws.built_count()
		rws.update(cam, 1.0)
		rounds += 1
	var t2 := Time.get_ticks_msec()
	print("probe: merge %d ms (%d airports), built %d airports within 150 km of LSZH in %d ms over %d updates" % [t1 - t0, merged.size(), rws.built_count(), t2 - t1, rounds])
	quit(0)
