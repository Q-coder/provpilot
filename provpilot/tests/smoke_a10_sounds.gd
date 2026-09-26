extends Node
## Subtask 8 (docs/A10_FLIGHTGEAR_PORT.md): the FlightGear sound set plays from
## the bridge values — looped engine sounds by view and N1, a once click on a
## switch, an in-transit sound while the canopy moves, generic files skipped.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_a10_sounds.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var snd: FGSounds = root._fg_sounds
	# the intro zoom starts with the request and is the loading view: no
	# cockpit camera renders while the destination loads
	_check(root._intro_playing and root._fly_loading and (root._loading_screen == null or not root._loading_screen.visible), "intro zoom starts with the Fly! request, no loading screen")
	var waited := 0.0
	var internal_while_loading := false
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
		for n in snd.playing():
			if "internal" in str(n) and snd.volume_db_of(str(n)) > -79.0:
				internal_while_loading = true
	_check(not internal_while_loading, "no audible cockpit sound while the destination loads")
	# while the zoom runs, the intro camera renders from far away -> outside
	# sounds, attenuated to silence
	var t := 0.0
	while not root._intro_playing and t < 10.0:
		await get_tree().create_timer(0.25).timeout
		t += 0.25
	await get_tree().create_timer(1.0).timeout
	var far: Array = snd.playing()
	print("  [diag] during the intro: cockpit_view %s, camera %.0f km, playing %s" % [str(root._fg_bridge.cockpit_view), snd.camera_distance_m / 1000.0, str(far.slice(0, 3))])
	# the zoom has been descending since the request: silent beyond 8 km, else audible
	var far_ok: bool = (snd.camera_distance_m >= 8000.0) == (snd.volume_db_of("jet-outside-0") <= -79.0)
	_check(root._intro_playing and not root._fg_bridge.cockpit_view and "fan-outside-0" in far and far_ok, "intro zoom: outside sounds selected, silent beyond 8 km (%.0f m, %.0f dB)" % [snd.camera_distance_m, snd.volume_db_of("jet-outside-0")])
	root._cancel_intro_zoom()
	waited = 0.0
	while root._fly_loading and waited < 90.0:   # the sim (and its sounds) waits for the destination
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	root._welcome_screen.visible = false   # the overlay stays up for script-requested flights and pauses the camera rig
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	root.camera1.current = true
	await get_tree().create_timer(2.0).timeout
	var rc: Camera3D = get_viewport().get_camera_3d()
	print("  [diag] rendering camera %s, camera1.current %s, camera2.current %s, intro %s, cockpit_view %s, dist %.1f" % [str(rc.get_path()) if rc else "none", str(root.camera1.current), str(root.camera2.current), str(root._intro_playing), str(root._fg_bridge.cockpit_view), (rc.global_position - root.get_node("AC").global_position).length() if rc else -1.0])
	print("  [diag] sounds.camera_distance_m %.1f, terrain_initialized %s, sounds playing %s" % [snd.camera_distance_m, str(root.terrain_initialized), str(snd.playing().slice(0, 4))])
	_check(snd != null and snd.count >= 120, "sound set loaded (%d sounds)" % (snd.count if snd else 0))
	_check(snd.skipped.size() == 19, "the 19 records on generic FlightGear files (rumble, gear, gear-lock, flaps, spoilers, squeal) are skipped (%d)" % snd.skipped.size())
	var playing: Array = snd.playing()
	print("  [diag] playing in the cockpit: ", playing)
	_check("Engine 1 sound 1 internal" in playing and not ("fan-outside-0" in playing), "cockpit view: internal engine sound plays, outside fan does not")
	_check(snd.volume_db_of("Engine 1 sound 1 internal") > -40.0, "engine sound has volume from N1 (%.1f dB)" % snd.volume_db_of("Engine 1 sound 1 internal"))
	# external view: outside sounds take over
	root._camera_rig.set_mode(1)
	root.camera_mode = 1
	root.camera2.current = true
	await get_tree().create_timer(0.5).timeout
	playing = snd.playing()
	print("  [diag] playing outside: ", playing)
	_check("fan-outside-0" in playing and "jet-outside-0" in playing and not ("Engine 1 sound 1 internal" in playing), "external view: outside engine sounds play")
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	root.camera1.current = true
	await get_tree().create_timer(0.5).timeout
	# a switch click: battery off then on -> "battery-cockpit-switch-up" once
	var clicks_before: int = snd.play_count("battery-cockpit-switch-up")
	root._fg_controls.state["controls/electric/battery-switch"] = 0.0
	await get_tree().create_timer(0.3).timeout
	root._fg_controls.state["controls/electric/battery-switch"] = 1.0
	await get_tree().create_timer(0.2).timeout
	_check(snd.play_count("battery-cockpit-switch-up") == clicks_before + 1, "switch click plays once when the battery switch goes up")
	# canopy in transit
	root._fg_bridge.canopy_target = 2.0
	await get_tree().create_timer(0.5).timeout
	_check("canopy" in snd.playing(), "canopy sound plays while the canopy moves")
	root._fg_bridge.canopy_target = 1.0
	await get_tree().create_timer(0.6).timeout
	_check(not ("canopy" in snd.playing()), "canopy sound stops when the canopy holds")
	# the left generator switch must not silence the engines (FlightGear quirk overridden at export)
	root._fg_controls.state["controls/electric/engine/generator"] = 0.0
	await get_tree().create_timer(0.3).timeout
	_check("Engine 1 sound 1 internal" in snd.playing(), "engine sounds keep playing with the left generator off")
	root._fg_controls.state["controls/electric/engine/generator"] = 1.0
	# "internal" follows the camera that renders, not the camera mode (intro zoom)
	root.camera2.current = true
	await get_tree().create_timer(0.3).timeout
	_check(not root._fg_bridge.cockpit_view and "fan-outside-0" in snd.playing(), "an external camera rendering while camera_mode is cockpit gives the outside sounds")
	# distance: 40 m loud, 5 km almost silent, 20 km silent
	root._camera_rig.set_mode(1)
	root.camera_mode = 1
	root.camera2.current = true
	root._camera_rig.set_orbit_distance_min(40.0)
	root._camera_rig.set_orbit_distance_max(40.0)
	await get_tree().create_timer(0.5).timeout
	var near_db: float = snd.volume_db_of("jet-outside-0")
	root._camera_rig.set_orbit_distance_min(5000.0)
	root._camera_rig.set_orbit_distance_max(5000.0)
	await get_tree().create_timer(0.5).timeout
	var far_db: float = snd.volume_db_of("jet-outside-0")
	root._camera_rig.set_orbit_distance_min(20000.0)
	root._camera_rig.set_orbit_distance_max(20000.0)
	await get_tree().create_timer(0.5).timeout
	var space_db: float = snd.volume_db_of("jet-outside-0")
	print("  [diag] jet-outside-0 at 40 m %.1f dB, 5 km %.1f dB, 20 km %.1f dB (camera %.0f m)" % [near_db, far_db, space_db, snd.camera_distance_m])
	_check(far_db < near_db - 30.0 and space_db <= -79.0, "outside sounds fall off with distance and vanish far away")
	root._camera_rig.set_orbit_distance_min(6.0)
	root._camera_rig.set_orbit_distance_max(300.0)
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	root.camera1.current = true
	# the basic engine loop is silent for this aircraft
	_check(root._aircraft.get_engine_audio() == null or root._aircraft.get_engine_audio().volume_db < -60.0, "basic engine loop silent for the A-10 (%.1f dB)" % (root._aircraft.get_engine_audio().volume_db if root._aircraft.get_engine_audio() else -80.0))
	if _fails.is_empty():
		print("smoke_a10_sounds: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_a10_sounds: %d failure(s)" % _fails.size())
		get_tree().quit(1)
