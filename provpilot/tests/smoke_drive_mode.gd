extends Node
## Headless smoke test for the ground vehicles: the welcome screen's
## "Vehicles" frame (src/welcome_screen.cpp), the vehicle configs
## (aircraft/car, aircraft/re44) and the kinematic car (src/drive_mode.cpp).
## No scenery: DriveMode runs without a WorldScenery, so the car keeps its
## height and the test checks the planar kinematics. Exits 0 on success.
##
##   godot --headless --path provpilot res://tests/smoke_drive_mode.tscn

var fails: Array[String] = []


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


func _collect(node: Node, cls: String, out: Array) -> void:
	if node.get_class() == cls:
		out.append(node)
	for c in node.get_children():
		_collect(c, cls, out)


func _ready() -> void:
	await _test_welcome()
	_test_configs()
	_test_kinematics()
	if fails.is_empty():
		print("SMOKE DRIVE MODE: ALL PASS")
	else:
		printerr("SMOKE DRIVE MODE: %d failure(s)" % fails.size())
		for f in fails:
			printerr("   - ", f)
	get_tree().quit(0 if fails.is_empty() else 1)


func _test_welcome() -> void:
	print("== welcome: Vehicles frame")
	var screen := WelcomeScreen.new()
	add_child(screen)
	await get_tree().process_frame
	var vl: ItemList = screen.find_child("VehicleList", true, false)
	check(vl != null, "a VehicleList sits in the welcome screen")
	if vl == null:
		return
	check(vl.item_count == 2 and vl.get_item_text(0).contains("car") and vl.get_item_text(1).contains("Re 4/4"), "it lists the car and the locomotive: %s" % [range(vl.item_count).map(func(i): return vl.get_item_text(i))])
	check(screen.get_selected_aircraft_id() == "c172p" and not screen.is_vehicle_selected(), "the default selection is still the first aircraft")
	vl.select(0)
	screen._on_vehicle_selected(0)
	check(screen.get_selected_aircraft_id() == "car" and screen.is_vehicle_selected(), "selecting the car makes it the Fly! selection")
	var lists: Array = []
	_collect(screen, "ItemList", lists)
	var aircraft_list: ItemList = null
	for l in lists:
		if l != vl and l.item_count >= 3 and l.get_item_text(0).contains("Cessna"):
			aircraft_list = l
	check(aircraft_list != null and aircraft_list.get_selected_items().is_empty(), "the aircraft list lost its selection")
	if aircraft_list:
		aircraft_list.select(2)
		screen._on_aircraft_selected(2)
		check(screen.get_selected_aircraft_id() == "a10" and vl.get_selected_items().is_empty(), "picking an aircraft again clears the vehicle")
	check(WelcomeScreen.aircraft_label("re44").contains("Re 4/4"), "aircraft_label knows the vehicles")
	screen.queue_free()
	await get_tree().process_frame


func _test_configs() -> void:
	print("== vehicle configs")
	for id in ["car", "re44"]:
		var cfg := AircraftConfig.load_for_aircraft(id)
		check(cfg.is_loaded(), "%s/aircraft.cfg loads" % id)
		check(cfg.get_string("vehicle", "kind", "") in ["car", "train"], "%s has a [vehicle] kind (%s)" % [id, cfg.get_string("vehicle", "kind", "")])
		var model_file: String = cfg.get_string("model", "file", "")
		var disk: String = InstallRoot.get_aircraft_dir(id).path_join(model_file)
		check(FileAccess.file_exists(disk), "%s model on disk: %s" % [id, disk])
	var rig := AircraftRig.new()
	check(rig.load_config("car") and rig.is_vehicle() and rig.get_vehicle_kind() == "car", "AircraftRig sees the car as a vehicle")
	var p: Dictionary = rig.get_vehicle_params()
	check(absf(float(p.get("wheelbase_m", 0.0)) - 2.4) < 0.01 and float(p.get("max_speed_kmh", 0.0)) == 120.0, "vehicle params come from the cfg: %s" % [p])
	check(rig.load_config("c172p") and not rig.is_vehicle(), "the Cessna is not a vehicle")


func _test_kinematics() -> void:
	print("== kinematics (no scenery)")
	var dm := DriveMode.new()
	dm.configure({"wheelbase_m": 2.4, "track_m": 1.3, "max_speed_kmh": 120.0, "max_reverse_kmh": 15.0, "accel_mps2": 3.5, "brake_mps2": 8.0, "max_steer_deg": 32.0})
	dm.set_lv95(2683000.0, 1248000.0)
	dm.set_heading_deg(90.0)
	# full throttle, 5 s straight east
	for i in 300:
		dm.step(1.0 / 60.0, 1.0, 0.0, 0.0, false)
	var v := dm.get_speed_kmh()
	var pos: Vector2 = dm.get_lv95()
	check(v > 40.0 and v < 63.0, "5 s of full throttle: %.0f km/h (3.5 m/s2 minus drag)" % v)
	check(pos.x - 2683000.0 > 30.0 and absf(pos.y - 1248000.0) < 0.5 and absf(dm.get_heading_deg() - 90.0) < 0.01, "... moved east only (dE %.1f m, dN %.2f m)" % [pos.x - 2683000.0, pos.y - 1248000.0])
	# steer right at speed: heading grows, the lock shrinks with speed
	for i in 120:
		dm.step(1.0 / 60.0, 1.0, 0.0, 1.0, false)
	var hdg := dm.get_heading_deg()
	check(hdg > 110.0 and hdg < 170.0 and dm.get_steer_deg() > 2.0 and dm.get_steer_deg() < 10.0, "2 s steering right at ~55 km/h turns the heading to %.0f (steer %.1f deg: the lock is cut to 6 m/s2 lateral)" % [hdg, dm.get_steer_deg()])
	# release: the wheel centres
	for i in 60:
		dm.step(1.0 / 60.0, 0.0, 0.0, 0.0, false)
	check(absf(dm.get_steer_deg()) < 0.5, "released, the steering centres (%.1f deg)" % dm.get_steer_deg())
	# brake to a stop
	for i in 600:
		dm.step(1.0 / 60.0, 0.0, 1.0, 0.0, false)
		if dm.get_speed_kmh() <= 0.0:
			break
	check(dm.get_speed_kmh() <= 0.0, "braking stops the car")
	# brake held at standstill reverses, slowly, up to the reverse limit
	for i in 600:
		dm.step(1.0 / 60.0, 0.0, 1.0, 0.0, false)
	check(dm.get_speed_kmh() < -5.0 and dm.get_speed_kmh() >= -15.01, "brake held when stopped reverses: %.1f km/h (limit 15)" % dm.get_speed_kmh())
	# handbrake stops it fast, coasting never flips the direction
	for i in 120:
		dm.step(1.0 / 60.0, 0.0, 0.0, 0.0, true)
	check(dm.get_speed_kmh() == 0.0, "the handbrake stops it")
	for i in 60:
		dm.step(1.0 / 60.0, 1.0, 0.0, 0.0, false)
	for i in 600:
		dm.step(1.0 / 60.0, 0.0, 0.0, 0.0, false)
	check(dm.get_speed_kmh() == 0.0, "a coasting car rolls out to exactly 0, never backwards")
	# top speed is the configured limit
	for i in 60 * 60:
		dm.step(1.0 / 60.0, 1.0, 0.0, 0.0, false)
	check(dm.get_speed_kmh() <= 120.01 and dm.get_speed_kmh() > 100.0, "full throttle settles at the 120 km/h limit: %.1f" % dm.get_speed_kmh())
	check(not dm.is_active() and not dm.is_armed(), "no node registered: never active")
	# the key catalog has the Vehicle section
	var kb := KeyBindings.new()
	var ids := []
	for s in KeyBindings.get_sections():
		ids.append(s["id"])
	check(ids == ["flight", "vehicle", "slew", "editor"], "the keyboard mapping has a Vehicle section: %s" % [ids])
	check(kb.get_keys("drive_accelerate")[0] == "W" and kb.get_keys("drive_handbrake")[0] == "B", "default drive keys W and B")
	kb.free()
