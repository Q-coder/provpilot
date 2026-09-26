extends Node
## Settings > Aircraft tab: selection follows the welcome page, reference data
## from aircraft.cfg [performance], fuel setting stored per aircraft in the
## [aircraft] section and applied to the tanks at flight start, live CG.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_aircraft_settings.tscn
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
	var screen = root._settings_screen
	var panel = screen.get_aircraft_panel()
	_check(panel != null, "Aircraft tab panel exists")
	_check(SettingsStore.section_for_key("aircraft_a10_fuel_pct") == "aircraft", "aircraft_* keys route to the [aircraft] section")
	# welcome page selection -> tab, through the page's own Settings button
	# (which hides the page before the settings open)
	root._welcome_screen._on_aircraft_selected(2)   # A-10
	root._on_welcome_settings_requested()
	await get_tree().process_frame
	await get_tree().process_frame
	_check(panel.get_aircraft_id() == "a10" and "A-10" in panel.get_title_label().text, "tab shows the aircraft selected on the welcome page (%s)" % panel.get_title_label().text)
	var texts: Array = []
	for c in panel.get_info_grid().get_children():
		texts.append((c as Label).text)
	_check("450 kt" in texts and "45000 ft" in texts, "reference data from [performance] (Vne 450 kt, ceiling 45000 ft)")
	_check("Not flying" in panel.get_cg_label().text, "CG readout idle before the flight")
	# fuel slider stores the setting per aircraft
	panel.get_fuel_slider().value = 80.0
	await get_tree().process_frame
	var store = screen.get_store()
	_check(absf(float(store.get_setting("aircraft_a10_fuel_pct")) - 80.0) < 1e-6, "fuel slider stores aircraft_a10_fuel_pct = 80")
	store.set_setting("aircraft_c172p_fuel_pct", 55.0)
	root._welcome_screen._on_aircraft_selected(0)
	panel.refresh()
	_check(panel.get_aircraft_id() == "c172p" and absf(panel.get_fuel_slider().value - 55.0) < 1e-6, "another aircraft shows its own fuel value (%.0f)" % panel.get_fuel_slider().value)
	screen.hide_settings()
	# fly the A-10: tanks come up at 80 %
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(2.0).timeout
	var jsb = root.jsb_node
	var cap: float = jsb.get_tank_capacity_lbs(0)
	var cont: float = jsb.get_tank_contents_lbs(0)
	print("  [diag] tanks %d, tank0 %.0f of %.0f lb" % [jsb.get_tank_count(), cont, cap])
	_check(jsb.get_tank_count() == 3 and cap > 3000.0 and absf(cont / cap - 0.8) < 0.02, "flight starts with the tanks at 80 %% (%.0f of %.0f lb)" % [cont, cap])
	# live CG while flying (the welcome overlay stays up when the flight is
	# requested from a script; in the game it is hidden, so hide it here)
	root._welcome_screen.visible = false
	screen.show_settings()
	await get_tree().process_frame
	panel.refresh()
	panel._update_live()
	var cg: Dictionary = jsb.get_cg_info()
	print("  [diag] cg ", cg)
	_check(float(cg.get("weight_lbs", 0.0)) > 20000.0 and float(cg.get("mac_ft", 0.0)) > 1.0, "CG info from the FDM (weight %.0f lb, MAC %.2f ft)" % [float(cg.get("weight_lbs", 0.0)), float(cg.get("mac_ft", 0.0))])
	_check(panel.get_cg_label().text.begins_with("CG "), "live CG readout while flying (%s)" % panel.get_cg_label().text)
	# changing the fuel while flying applies immediately
	panel.get_fuel_slider().value = 30.0
	await get_tree().process_frame
	_check(absf(jsb.get_tank_contents_lbs(1) / jsb.get_tank_capacity_lbs(1) - 0.3) < 0.02, "fuel change while flying fills the tanks now (%.0f %%)" % (100.0 * jsb.get_tank_contents_lbs(1) / jsb.get_tank_capacity_lbs(1)))
	screen.hide_settings()
	# Saved values come back at the next start: aircraft_* keys have no default,
	# SettingsStore.load() takes them from [aircraft] as they are (InstallRoot's
	# in-memory config here; nothing is written to disk). The section also
	# carries InstallRoot's own keys, which are not settings.
	var ir = get_tree().root.get_node_or_null("InstallRoot")
	_check(ir != null, "InstallRoot node available")
	if ir:
		ir.set_value("aircraft", "aircraft_pc7_fuel_pct", 33.0)
		ir.set_value("aircraft", "initialized", true)
		store.load()
		_check(store.has_setting("aircraft_pc7_fuel_pct") and absf(float(store.get_setting("aircraft_pc7_fuel_pct")) - 33.0) < 1e-6, "SettingsStore.load() reads aircraft_* keys from [aircraft] without a default")
		_check(not store.has_setting("initialized"), "InstallRoot's own [aircraft] keys (initialized, version) are not loaded as settings")
		ir.erase_section_key("aircraft", "aircraft_pc7_fuel_pct")
		_check(not ir.has_section_key("aircraft", "aircraft_pc7_fuel_pct"), "InstallConfig.erase_section_key removes one key")
	if _fails.is_empty():
		print("smoke_aircraft_settings: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_aircraft_settings: %d failure(s)" % _fails.size())
		get_tree().quit(1)
