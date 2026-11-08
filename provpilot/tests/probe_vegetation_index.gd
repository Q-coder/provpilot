extends SceneTree
## How long the vegetation manager takes to index the library's forest dirs
## (the Fly! stall of 2026-11-08) and what a frame costs afterwards.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot -s tests/probe_vegetation_index.gd
func _init() -> void:
	await process_frame
	var veg := S3DVegetationManager.new()
	veg.frame_mode = 0
	veg.origin_east = 2688500.0
	veg.origin_north = 1250500.0
	veg.card_mode = true
	veg.vegetation_paths = PackedStringArray([
		"/Volumes/Data1/provpilot/build/switzerland-base/Switzerland/vegetation",
		"/Volumes/Data1/provpilot/build/austria/Austria/vegetation",
		"/Volumes/Data1/provpilot/build/germany/Germany/vegetation",
		"/Volumes/Data1/provpilot/build/italy/Italy/vegetation",
		"/Users/gery/provpilot/online_scenery/Switzerland/vegetation"])
	root.add_child(veg)
	var cam := Camera3D.new()
	root.add_child(cam)
	cam.current = true
	var t0 := Time.get_ticks_msec()
	var frames_blocked := 0
	var worst := 0
	while not veg.is_manifest_loaded() and Time.get_ticks_msec() - t0 < 120000:
		var f0 := Time.get_ticks_usec()
		await process_frame
		worst = maxi(worst, Time.get_ticks_usec() - f0)
		frames_blocked += 1
	print("[veg-index] manifest loaded after %d ms wall, %d frames, worst frame %d ms, %d tiles indexed, scan thread %d ms" % [Time.get_ticks_msec() - t0, frames_blocked, worst / 1000, veg.get_manifest_entry_count(), int(veg.get_manifest_scan_ms())])
	var t1 := Time.get_ticks_usec()
	var n := 0
	var worst2 := 0
	while n < 120:
		var f0 := Time.get_ticks_usec()
		await process_frame
		worst2 = maxi(worst2, Time.get_ticks_usec() - f0)
		n += 1
	print("[veg-index] 120 frames with the index: mean %.2f ms, worst %.1f ms, %d tiles active" % [(Time.get_ticks_usec() - t1) / 1000.0 / n, worst2 / 1000.0, veg.get_active_tile_count()])
	quit(0)
