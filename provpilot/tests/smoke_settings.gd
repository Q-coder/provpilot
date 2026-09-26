extends Node
## Headless smoke test for the C++ SettingsStore + SettingsScreen modules
## (src/settings_store.cpp, src/settings_screen.cpp). Hermetic: builds a tiny
## fake scenery library under /tmp, exercises the library ops + apply-to-scene
## paths against stub nodes, then mounts the tabbed screen and round-trips a
## few controls. Never writes provpilot.cfg. Exits 0 on success, 1 on failure.
##
##   godot --headless --path provpilot --res tests/smoke_settings.tscn

var fails: Array[String] = []
var closed_count := 0

const TMP := "/tmp/provpilot_settings_smoke"


## Stand-in for the Scenery3D node: exposes the properties the store pushes.
class FakeScenery extends Node:
	var data_paths := PackedStringArray()
	var orthophoto_paths := PackedStringArray()
	var buildings_paths := PackedStringArray()
	var water_paths := PackedStringArray()
	var road_paths := PackedStringArray()
	var skip_white_pixels := false
	var load_radius := 0


## Stand-in for the Vegetation node (Node3D so `visible` exists).
class FakeVegetation extends Node3D:
	var vegetation_paths := PackedStringArray()
	var density_fraction := -1.0
	var load_radius_m := 0


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


func _ready() -> void:
	_make_fake_library()
	_test_install_root()
	_test_store()
	_test_screen()
	# The screen builds its GDScript panels on add_child; give the tree a frame
	# before tearing down so deferred frees don't trip.
	await get_tree().process_frame
	_cleanup()
	if fails.is_empty():
		print("SMOKE SETTINGS: ALL PASS")
	else:
		printerr("SMOKE SETTINGS: %d failure(s)" % fails.size())
		for f in fails:
			printerr("   - ", f)
	get_tree().quit(0 if fails.is_empty() else 1)


func _make_fake_library() -> void:
	# RegionA is "complete" (terrain + orthophoto), RegionB only has terrain,
	# Junk has no asset subdirs at all.
	for sub in ["RegionA/terrain", "RegionA/orthophoto", "RegionB/terrain", "Junk/stuff"]:
		DirAccess.make_dir_recursive_absolute(TMP.path_join(sub))


func _cleanup() -> void:
	if DirAccess.dir_exists_absolute(TMP):
		OS.execute("rm", ["-rf", TMP])


## The InstallRoot autoload is a C++ InstallConfig (src/install_config.cpp)
## mounted through install_root.tscn. In-memory checks only — never saved.
func _test_install_root() -> void:
	print("== InstallRoot (InstallConfig) ==")
	var ir := get_node_or_null("/root/InstallRoot")
	check(ir != null and ir is InstallConfig, "InstallRoot autoload is an InstallConfig node")
	if ir == null:
		return
	check(not ir.get_install_root().is_empty(), "install_root discovered: %s" % ir.get_install_root())
	check(ir.get_config_path() == ir.get_install_root().path_join("provpilot.cfg"), "config_path = <install_root>/provpilot.cfg")
	check(ir.get_aircraft_dir("c172p") == ir.get_install_root().path_join("aircraft/c172p"), "get_aircraft_dir builds <root>/aircraft/<id>")
	var roots: PackedStringArray = ir.get_default_scan_roots()
	check(roots.has(ir.get_install_root().path_join("scenery")), "default scan roots include <root>/scenery")
	var lowered := {}
	var dup := false
	for r in roots:
		if lowered.has(r.to_lower()):
			dup = true
		lowered[r.to_lower()] = true
	check(not dup, "default scan roots are de-duplicated case-insensitively")
	# Generic config API round trip in memory (scratch section, erased after).
	check(not ir.has_section("smoke_scratch"), "scratch section absent before test")
	ir.set_value("smoke_scratch", "answer", 42)
	check(ir.has_section_key("smoke_scratch", "answer") and int(ir.get_value("smoke_scratch", "answer", 0)) == 42, "set_value/get_value round trip")
	check(ir.get_value("smoke_scratch", "missing", "dflt") == "dflt", "get_value returns the default for a missing key")
	check(ir.get_section_keys("smoke_scratch") == PackedStringArray(["answer"]), "get_section_keys lists the staged key")
	check(Array(ir.get_sections()).has("smoke_scratch"), "get_sections includes the scratch section")
	ir.erase_section("smoke_scratch")
	check(not ir.has_section("smoke_scratch"), "erase_section removes it")
	check(ir.get_section_keys("no_such_section").is_empty(), "get_section_keys on unknown section is empty")
	# SettingsStore reaches the same node.
	var s := SettingsStore.new()
	s.load()
	check(s.get_setting("scenery_scan_roots").has(ir.get_install_root().path_join("scenery")), "SettingsStore.load() seeds scan roots from InstallRoot")


func _test_store() -> void:
	print("== SettingsStore ==")
	var s := SettingsStore.new()

	# Defaults + statics
	check(int(s.get_setting("render_distance")) == 8, "default render_distance is 8")
	check(bool(s.get_setting("map_enabled")) == true, "default map_enabled is true")
	check(s.get_setting("no_such_key") == null, "unknown key returns null")
	check(SettingsStore.section_for_key("engine_volume") == "sound", "volume keys route to [sound]")
	check(SettingsStore.section_for_key("scenery_areas") == "scenery", "scenery_areas routes to [scenery]")
	check(SettingsStore.section_for_key("time_of_day_min") == "world", "time_of_day_min routes to [world]")
	check(SettingsStore.section_for_key("crash_detection") == "realism", "crash_detection routes to [realism]")
	check(SettingsStore.section_for_key("weather_layers") == "weather", "weather_layers routes to [weather]")
	check(SettingsStore.section_for_key("qnh_hpa") == "weather", "qnh_hpa routes to [weather]")
	check(int(s.get_setting("qnh_hpa")) == 1013, "default QNH is 1013 hPa")
	check(int(s.get_setting("visibility_km")) == 80, "default visibility is 80 km")
	check(bool(s.get_setting("live_weather")) == false and SettingsStore.section_for_key("live_weather") == "weather", "live_weather defaults off in [weather]")
	var wl: Array = s.get_setting("weather_layers")
	check(wl.size() == 1 and int(wl[0].get("top_m")) == 3000, "default weather has one layer to 3000 m")
	check(SettingsStore.format_hhmm(725) == "12:05", "format_hhmm(725) == 12:05")
	var home := OS.get_environment("HOME")
	check(SettingsStore.format_path_for_display(home.path_join("x"), 60) == "~/x", "format_path collapses $HOME to ~")
	var long_path := "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z/aa/bb/cc/dd/ee/ff"
	var shown := SettingsStore.format_path_for_display(long_path, 20)
	check(shown.begins_with("/a/b/…/") and shown.ends_with("dd/ee/ff"), "format_path elides the middle: %s" % shown)

	# Library: add a parent dir -> discovers both regions, richest first.
	check(s.detect_assets(TMP.path_join("RegionA")) == PackedStringArray(["terrain", "orthophoto"]), "detect_assets(RegionA)")
	check(s.detect_assets(TMP.path_join("Junk")).is_empty(), "detect_assets(Junk) is empty")
	check(s.missing_scenery_reason() != "", "empty library reports missing scenery")
	var added := s.add_scenery_area(TMP)
	check(added == 2, "add_scenery_area(parent) discovers 2 regions (got %d)" % added)
	var areas: Array = s.get_scenery_areas()
	check(areas.size() == 2, "library has 2 areas")
	check(str(areas[0].get("path")) == TMP.path_join("RegionA"), "richest region (RegionA) gets priority 1")
	check(int(areas[0].get("priority")) == 1 and int(areas[1].get("priority")) == 2, "priorities numbered 1..2")
	check(s.get_setting("scenery_scan_roots").has(TMP), "parent dir registered as scan root")
	check(s.add_scenery_area(TMP.path_join("RegionA")) == 0, "re-adding a known region is a no-op")
	check(s.missing_scenery_reason() == "", "library with terrain reports no missing scenery")

	# Scan picks up nothing new (both known), keeps 2.
	check(s.scan_scenery() == 0 and s.get_scenery_areas().size() == 2, "scan_scenery finds nothing new")

	# Priority ops
	check(s.swap_scenery_priority(0, 1), "swap_scenery_priority(0,1)")
	areas = s.get_scenery_areas()
	check(str(areas[0].get("path")) == TMP.path_join("RegionB") and int(areas[0].get("priority")) == 1, "swap renumbers: RegionB now priority 1")
	check(not s.swap_scenery_priority(1, 2), "swap out of range rejected")
	s.set_area_enabled(0, false)
	var enabled: Array = s.get_enabled_scenery_areas()
	check(enabled.size() == 1 and str(enabled[0].get("path")) == TMP.path_join("RegionA"), "disabled area excluded from enabled list")
	s.set_area_enabled(0, true)
	s.set_area_priority(0, 5)
	enabled = s.get_enabled_scenery_areas()
	check(str(enabled[0].get("path")) == TMP.path_join("RegionA"), "enabled list sorted by priority (RegionA first after RegionB -> 5)")

	# apply_scenery_sources -> stub nodes
	var sc := FakeScenery.new()
	var veg := FakeVegetation.new()
	var map := VfrMap.new()
	s.set_setting("render_distance", 40)  # out of range -> clamped to 16
	s.set_setting("skip_white_pixels", true)
	s.apply_scenery_sources(sc, veg, map)
	check(sc.data_paths == PackedStringArray([TMP.path_join("RegionA/terrain"), TMP.path_join("RegionB/terrain")]), "terrain paths in priority order")
	check(sc.orthophoto_paths == PackedStringArray([TMP.path_join("RegionA/orthophoto")]), "orthophoto paths only from RegionA")
	check(sc.buildings_paths.is_empty() and sc.water_paths.is_empty() and sc.road_paths.is_empty(), "absent asset dirs give empty arrays")
	check(sc.skip_white_pixels == true, "skip_white_pixels pushed")
	check(sc.load_radius == 16, "render_distance clamped to 16 -> load_radius")
	check(veg.vegetation_paths.is_empty(), "vegetation paths empty")
	sc.free()
	map.free()

	# apply_tree_density
	s.set_setting("tree_density", 1)
	s.apply_tree_density(veg)
	check(is_equal_approx(veg.density_fraction, 0.25) and veg.visible and veg.load_radius_m == 1500, "tree_density=1 -> 0.25 / visible / 1500 m")
	s.set_setting("tree_density", 0)
	s.apply_tree_density(veg)
	check(is_equal_approx(veg.density_fraction, 0.0) and not veg.visible, "tree_density=0 hides vegetation")
	veg.free()

	# restore_defaults keeps the library, resets preferences.
	s.restore_defaults()
	check(int(s.get_setting("render_distance")) == 8 and int(s.get_setting("tree_density")) == 2, "restore_defaults resets preferences")
	check(s.get_scenery_areas().size() == 2, "restore_defaults keeps the scenery library")
	check(s.get_setting("scenery_scan_roots").has(TMP), "restore_defaults keeps scan roots")

	# remove
	check(s.remove_scenery_area(0) and s.get_scenery_areas().size() == 1, "remove_scenery_area(0)")
	check(not s.remove_scenery_area(7), "remove out of range rejected")

	# Settings dictionary is a live view.
	var live: Dictionary = s.get_settings()
	s.set_setting("master_volume", 33)
	check(int(live.get("master_volume")) == 33, "get_settings() is a live view")


func _test_screen() -> void:
	print("== SettingsScreen ==")
	var screen := SettingsScreen.new()
	screen.closed.connect(func() -> void: closed_count += 1)
	add_child(screen)  # _ready: loads provpilot.cfg (read-only), builds tabs
	var store: SettingsStore = screen.get_store()
	check(store != null, "get_store() returns the store")
	check(not screen.visible, "screen hidden after creation")
	check(screen.layer == 101, "screen on layer 101")

	var tabs: TabContainer = _find_first(screen, "TabContainer")
	check(tabs != null, "TabContainer present")
	if tabs == null:
		screen.queue_free()
		return
	var titles := PackedStringArray()
	for i in range(tabs.get_tab_count()):
		titles.append(tabs.get_tab_title(i))
	check(titles == PackedStringArray(["Aircraft", "Scenery", "Downloads", "Graphics", "World", "Weather", "Map", "Sound", "Realism", "Joystick"]), "10 subject tabs: %s" % [titles])
	check(_find_first(screen, "AircraftSettingsPanel") != null, "Aircraft tab hosts the AircraftSettingsPanel")

	# GDScript panels mounted as tabs.
	check(_find_first(screen, "SceneryDownloaderPanel") != null or _find_script(screen, "scenery_downloader.gd") != null, "Downloads tab hosts scenery_downloader.gd")
	check(_find_first(screen, "JoystickSettingsPanel") != null, "Joystick tab hosts the JoystickSettingsPanel")

	# store -> UI (refresh) -> store (commit) round trip on a slider.
	store.set_setting("master_volume", 45)
	screen.show_settings()
	check(screen.visible, "show_settings() makes it visible")
	var sound_sliders: Array = []
	_collect(tabs.get_child(7), "HSlider", sound_sliders)   # Sound tab
	var world_sliders: Array = []
	_collect(tabs.get_child(4), "HSlider", world_sliders)   # World tab
	check(sound_sliders.size() == 4, "Sound tab has 4 sliders (got %d)" % sound_sliders.size())
	check(world_sliders.size() == 1, "World tab has the time slider (got %d)" % world_sliders.size())
	if sound_sliders.size() == 4 and world_sliders.size() == 1:
		var master: HSlider = sound_sliders[0]
		check(int(master.value) == 45, "refresh pushed master_volume=45 into its slider")
		master.value = 60
		screen.commit()
		check(int(store.get_setting("master_volume")) == 60, "commit() reads slider back into the store")
		# Dragging the time slider switches the system clock off.
		var checks: Array = []
		_collect(tabs.get_child(4), "CheckBox", checks)
		var sys_clock: CheckBox = null
		for c in checks:
			if c.get_parent().get_child(0) is Label and c.get_parent().get_child(0).text == "Use System Clock:":
				sys_clock = c
		check(sys_clock != null, "found Use System Clock checkbox")
		if sys_clock:
			sys_clock.button_pressed = true
			var time_slider: HSlider = world_sliders[0]
			time_slider.value = 600
			check(not sys_clock.button_pressed, "moving the time slider unchecks Use System Clock")
			screen.commit()
			check(int(store.get_setting("time_of_day_min")) == 600 and not bool(store.get_setting("use_system_time")), "time slider + system clock committed")

	# Weather tab: layer table round trip (add / edit / remove).
	var weather_tab: Control = tabs.get_child(5)
	var w_spins: Array = []
	_collect(weather_tab, "SpinBox", w_spins)
	check(w_spins.size() == 4, "Weather tab shows one default layer (4 spin boxes, got %d)" % w_spins.size())
	var w_buttons: Array = []
	_collect(weather_tab, "Button", w_buttons)
	var add_btn: Button = null
	for b in w_buttons:
		if b.text == "Add Layer":
			add_btn = b
	check(add_btn != null, "Weather tab has an Add Layer button")
	if add_btn:
		add_btn.pressed.emit()
		w_spins.clear()
		_collect(weather_tab, "SpinBox", w_spins)
		check(w_spins.size() == 8, "Add Layer appends a second layer row")
		var layers: Array = store.get_setting("weather_layers")
		check(layers.size() == 2 and int(layers[1].get("base_m")) == 3000 and int(layers[1].get("top_m")) == 5000, "second layer starts at the first one's top and is 2000 m thick")
		if w_spins.size() == 8:
			w_spins[6].value = 25  # layer 2 wind direction spin (base, top, dir, speed per row)
			w_spins[7].value = 12  # layer 2 wind speed
			w_spins[4].value = 1500  # layer 2 cloud base
			w_spins[5].value = 2500
			layers = store.get_setting("weather_layers")
			check(int(layers[1].get("base_m")) == 1500 and int(layers[1].get("top_m")) == 2500, "base/top edits update the store")
			w_spins[5].value = 1200  # top below base -> clamped to a non-empty slab
			layers = store.get_setting("weather_layers")
			check(int(layers[1].get("wind_dir_deg")) == 25 and int(layers[1].get("wind_speed_kts")) == 12, "editing a layer spin updates the store")
			check(int(layers[1].get("top_m")) == 1600, "a top below the base is clamped to base + 100 m")
		w_buttons.clear()
		_collect(weather_tab, "Button", w_buttons)
		var removes: Array = []
		for b in w_buttons:
			if b.text == "Remove":
				removes.append(b)
		check(removes.size() == 2, "each layer has a Remove button")
		if removes.size() == 2:
			removes[0].pressed.emit()
			layers = store.get_setting("weather_layers")
			check(layers.size() == 1 and int(layers[0].get("wind_dir_deg")) == 25, "removing layer 1 keeps the edited layer 2")
			removes.clear()
			w_buttons.clear()
			_collect(weather_tab, "Button", w_buttons)
			for b in w_buttons:
				if b.text == "Remove":
					removes.append(b)
			if removes.size() == 1:
				removes[0].pressed.emit()
				layers = store.get_setting("weather_layers")
				check(layers.size() == 1, "the last layer cannot be removed")
	var w_sliders: Array = []
	_collect(weather_tab, "HSlider", w_sliders)
	check(w_sliders.size() == 2, "Weather tab has visibility + QNH sliders (got %d)" % w_sliders.size())
	if w_sliders.size() == 2:
		w_sliders[1].value = 1020
		screen.commit()
		check(int(store.get_setting("qnh_hpa")) == 1020, "QNH slider commits to the store")

	# Weather profiles: save / list / load / delete round trip through the store.
	var pname := "smoke_test_profile"
	store.delete_weather_profile(pname)
	store.set_setting("weather_layers", [{"base_m": 0, "top_m": 2500, "wind_dir_deg": 240, "wind_speed_kts": 18, "turbulence": 1, "cloud_type": 1, "coverage_oktas": 3}])
	store.set_setting("visibility_km", 35)
	store.set_setting("qnh_hpa", 1002)
	check(store.save_weather_profile(pname), "save_weather_profile writes a file")
	check(FileAccess.file_exists(store.weather_profiles_dir().path_join(pname + ".json")), "profile file exists under <install_root>/weather")
	check(store.list_weather_profiles().has(pname), "list_weather_profiles finds it")
	store.set_setting("weather_layers", [])
	store.set_setting("visibility_km", 80)
	store.set_setting("qnh_hpa", 1013)
	check(store.load_weather_profile(pname), "load_weather_profile succeeds")
	var pl: Array = store.get_setting("weather_layers")
	check(pl.size() == 1 and int(pl[0]["wind_dir_deg"]) == 240 and int(store.get_setting("visibility_km")) == 35 and int(store.get_setting("qnh_hpa")) == 1002, "loaded profile restores layers, visibility and QNH")
	check(not store.load_weather_profile("no_such_profile_xyz"), "loading a missing profile fails cleanly")
	check(store.delete_weather_profile(pname) and not store.list_weather_profiles().has(pname), "delete_weather_profile removes it")
	var plist: ItemList = _find_first(weather_tab, "ItemList")
	check(plist != null, "Weather tab has the profile list")
	var pbtns: Array = []
	_collect(weather_tab, "Button", pbtns)
	var have := {"Load": false, "Save": false, "Delete": false}
	for b in pbtns:
		if have.has(b.text):
			have[b.text] = true
	check(have["Load"] and have["Save"] and have["Delete"], "Weather tab has Load / Save / Delete profile buttons")

	# Restore Defaults resets the UI, keeps the library.
	var areas_before: int = store.get_scenery_areas().size()
	screen._on_restore_defaults()
	screen.commit()
	check(int(store.get_setting("master_volume")) == 80, "Restore Defaults resets master_volume to 80")
	check(store.get_scenery_areas().size() == areas_before, "Restore Defaults keeps the scenery library")

	# Cancel reloads from disk, hides, emits closed.
	screen._on_cancel()
	check(not screen.visible, "cancel hides the screen")
	check(closed_count == 1, "cancel emits closed once")

	screen.queue_free()


func _find_first(root: Node, cls: String) -> Node:
	if root.get_class() == cls:
		return root
	for c in root.get_children():
		var r := _find_first(c, cls)
		if r != null:
			return r
	return null


func _find_script(root: Node, script_file: String) -> Node:
	var scr = root.get_script()
	if scr != null and str(scr.resource_path).ends_with(script_file):
		return root
	for c in root.get_children():
		var r := _find_script(c, script_file)
		if r != null:
			return r
	return null


func _collect(root: Node, cls: String, out: Array) -> void:
	if root.get_class() == cls:
		out.append(root)
	for c in root.get_children():
		_collect(c, cls, out)
