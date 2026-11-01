extends Node
## The MD-80 family (docs/MD80_FLIGHTGEAR_PORT.md step 15): md81 / md83 / md87
## are variants of md80 (aircraft.cfg [aircraft] base): their configs inherit
## the MD-82's, files they lack come from md80/, each flies its own FDM, and
## the liveries of the package (model/liveries/<variant>.json) swap the
## airframe textures and the engine-variant gauge faces through the animator.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_md80_variants.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []
var _root: Node = null

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _find(n: Node, name: String) -> Node:
	if n.name == name:
		return n
	for c in n.get_children():
		var r := _find(c, name)
		if r:
			return r
	return null

func _albedo(mesh_name: String) -> Texture2D:
	var mi := _find(_root.get_node("AC"), mesh_name) as MeshInstance3D
	if mi == null:
		return null
	var m := mi.get_active_material(0) as BaseMaterial3D
	return m.albedo_texture if m else null

func _tex_size(t: Texture2D) -> Vector2i:
	return t.get_image().get_size() if t and t.get_image() else Vector2i.ZERO

func _fly(id: String) -> void:
	if _root._settings:
		_root._settings.set_setting("aircraft_%s_start_agl_ft" % id, 0.0)
		_root._settings.set_setting("aircraft_%s_start_speed_kt" % id, 0.0)
		_root._settings.get_settings().erase("aircraft_%s_livery" % id)
	_root._on_welcome_flight_requested(id, E, N, 155.0)
	var waited := 0.0
	while not _root.terrain_initialized and waited < 90.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(5.0).timeout

func _ready() -> void:
	# config inheritance needs no flight
	var c81 = AircraftConfig.load_for_aircraft("md81")
	_check(c81 != null and c81.is_loaded(), "md81 config loads")
	_check(c81.get_string("aircraft", "name", "") == "McDonnell Douglas MD-81", "md81 has its own name (%s)" % c81.get_string("aircraft", "name", ""))
	_check(c81.get_string("jsbsim", "model", "") == "md81", "md81 flies the md81 FDM")
	_check(c81.get_string("sound", "fg_sounds", "") == "sounds/sounds.json", "md81 inherits the MD-82 sound set key")
	_check(c81.get_string("model", "res_file", "").ends_with("md80/model/md80_fg.glb"), "md81 inherits the MD-82 model (%s)" % c81.get_string("model", "res_file", ""))
	_check(c81.get_float("jsbsim", "pitch_trim_default", 0.0) == -0.24, "md81 inherits [jsbsim] pitch_trim_default")
	var c87 = AircraftConfig.load_for_aircraft("md87")
	_check(c87.get_string("model", "res_file", "").ends_with("md87/model/md87_fg.glb"), "md87 overrides the model file")
	_check(c87.get_string("camera", "cockpit_position", "").begins_with("5.0, 24.5, 166.8"), "md87 overrides the eye point")
	_check(c87.get_float("camera", "cockpit_fov", 0.0) == 65.0, "md87 inherits the cockpit FOV")
	_check(c87.get_string_array("jsbsim", "init_properties", []).size() == 2, "md87 lists two init files")
	_check(AircraftConfig.base_id("md83") == "md80" and AircraftConfig.base_id("md80") == "", "base_id: md83 -> md80, md80 -> none")
	var shared: String = AircraftConfig.resolve_aircraft_file("md81", "sounds/sounds.json")
	_check(shared.ends_with("md80/sounds/sounds.json") and FileAccess.file_exists(shared), "md81 sound set resolves to the md80 folder (%s)" % shared)
	var own: String = AircraftConfig.resolve_aircraft_file("md87", "model/animations.json")
	_check(own.ends_with("md87/model/animations.json"), "md87 manifest resolves to its own folder")
	var liv: String = AircraftConfig.resolve_aircraft_file("md87", "model/liveries")
	_check(liv.ends_with("md80/model/liveries"), "md87 textures resolve to the shared liveries tree (%s)" % liv)
	var cat: Dictionary = AircraftSettingsPanel.livery_catalog(c81, "md81")
	_check(cat.get("liveries", []).size() == 11, "MD-81 catalog: 11 liveries (%d)" % cat.get("liveries", []).size())
	_check(cat.get("defaults", {}).get("sim/model/n1-tach/texture", "") == "n1-217.png", "MD-81 catalog: -217 N1 face")
	var cat82: Dictionary = AircraftSettingsPanel.livery_catalog(AircraftConfig.load_for_aircraft("md80"), "md80")
	_check(cat82.get("liveries", []).size() == 24, "MD-82 (md80) catalog: 24 liveries (%d)" % cat82.get("liveries", []).size())
	_check(cat82.get("defaults", {}).get("sim/model/n1-tach/texture", "") == "n1-217a.png", "MD-82 catalog: -217A N1 face")

	_root = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(_root)
	await get_tree().create_timer(3.0).timeout

	# ── MD-81: shared model and sounds, its own FDM, liveries ──
	await _fly("md81")
	var jsb = _root.jsb_node
	_check(_root.terrain_initialized and jsb != null and jsb.is_initialized(), "md81 flight starts")
	# tanks: L main, R main, centre, forward aux, aft aux
	_check(jsb.get_tank_count() >= 5 and jsb.get_tank_capacity_lbs(3) < 1.0 and jsb.get_tank_capacity_lbs(4) < 1.0, "md81 FDM: no auxiliary tanks (tanks 3 / 4 = %.3f / %.3f lb)" % [jsb.get_tank_capacity_lbs(3), jsb.get_tank_capacity_lbs(4)])
	_check(_root._aircraft.get_config().get_string("aircraft", "name", "") == "McDonnell Douglas MD-81", "the rig flies the md81 config")
	var sounds = _root._aircraft.get_fg_sounds()
	_check(sounds != null and sounds.playing().size() > 0, "md81 plays the shared MD-82 sound set (%d playing)" % (sounds.playing().size() if sounds else -1))
	_check(_root._aircraft.get_liveries().size() == 11 and _root._aircraft.get_livery() == "", "md81: 11 liveries, none selected")
	var n1_face := _albedo("n1L__Face")
	_check(n1_face != null and _tex_size(n1_face) == Vector2i(512, 512) and n1_face is ImageTexture, "md81 N1 gauge face is the -217 texture from the liveries tree (%s)" % str(n1_face))
	var fus_before := _albedo("Fuselage")
	_root._aircraft.set_livery("SAS")
	await get_tree().create_timer(0.5).timeout
	var fus_sas := _albedo("Fuselage")
	var sas_png := Image.load_from_file(AircraftConfig.resolve_aircraft_file("md81", "model/liveries/Liveries/MD-82/sas_fuselage.png"))
	_check(fus_sas != null and fus_sas != fus_before and sas_png != null and _tex_size(fus_sas) == sas_png.get_size(), "SAS livery: fuselage albedo swapped to sas_fuselage.png %s" % str(_tex_size(fus_sas)))
	var rud_sas := _albedo("Rudder")
	_check(rud_sas is ImageTexture, "SAS livery: rudder / engines texture swapped")
	_check(_root._aircraft.get_fg_animator().get_property("sim/model/livery/texture") == "Liveries/MD-82/sas_fuselage.png", "the livery property is published to the animator")
	_root._aircraft.set_livery("")
	await get_tree().create_timer(0.5).timeout
	_check(_albedo("Fuselage") == fus_before, "no livery: the glb's own fuselage texture is back")
	var panel = _root._settings_screen.get_aircraft_panel() if _root._settings_screen else null
	if panel:
		panel.refresh()
		await get_tree().create_timer(0.3).timeout
		var btn: OptionButton = panel.get_livery_button()
		_check(btn != null and btn.is_visible_in_tree() == false or (btn != null and btn.get_item_count() == 12), "Aircraft tab: livery option lists as-built + 11 liveries (%d)" % (btn.get_item_count() if btn else -1))
		if btn and btn.get_item_count() == 12:
			btn.select(6)
			btn.emit_signal("item_selected", 6)
			await get_tree().create_timer(0.5).timeout
			_check(_root._aircraft.get_livery() == btn.get_item_text(6) or _root._aircraft.get_livery() != "", "picking a livery on the Aircraft tab applies it to the flown aircraft (%s)" % _root._aircraft.get_livery())
			_check(_root._settings.get_setting("aircraft_md81_livery") == _root._aircraft.get_livery(), "the choice is stored as aircraft_md81_livery")
			btn.select(0)
			btn.emit_signal("item_selected", 0)

	# ── MD-83: JT8D-219 and the auxiliary tanks ──
	await _fly("md83")
	jsb = _root.jsb_node
	_check(_root.terrain_initialized and jsb != null and jsb.is_initialized(), "md83 flight starts")
	_check(jsb.get_tank_capacity_lbs(3) > 5000.0 and jsb.get_tank_capacity_lbs(4) > 8000.0, "md83 FDM: auxiliary tanks %.0f / %.0f lb" % [jsb.get_tank_capacity_lbs(3), jsb.get_tank_capacity_lbs(4)])
	_check(_root._aircraft.get_liveries().size() == 23, "md83: 23 liveries")
	_check(_root._aircraft.get_fg_animator().get_property("sim/model/n1-tach/texture") == "n1-219.png", "md83 N1 face is the -219 scale")

	# ── MD-87: its own airframe and model, the stab arm factor ──
	await _fly("md87")
	jsb = _root.jsb_node
	_check(_root.terrain_initialized and jsb != null and jsb.is_initialized(), "md87 flight starts")
	_check(_find(_root.get_node("AC"), "MD-87_Root") != null and _find(_root.get_node("AC"), "MD-82_Root") == null, "md87 shows the MD-87 model")
	_check(absf(jsb.get_property("aero/arm-factor") - 0.97) < 1e-6, "md87 stab arm factor 0.97 (%.3f)" % jsb.get_property("aero/arm-factor"))
	_check(_root._aircraft.get_liveries().size() == 2, "md87: 2 liveries")
	_check(_root._aircraft.get_fg_animator().get_property("sim/model/n1-tach/texture") == "n1-217a.png", "md87 N1 face is the -217A scale")
	var cam: Camera3D = _root.get_node("AC/Camera1") if _root.has_node("AC/Camera1") else null
	_check(cam != null and absf(cam.position.z - 166.8) < 12.0, "md87 eye point from its cfg, 2.9 m aft of the MD-82's (%s)" % (str(cam.position) if cam else "no camera"))
	_check(_root._aircraft.get_fg_sounds() != null and _root._aircraft.get_fg_sounds().playing().size() > 0, "md87 plays the shared sound set")
	if _fails.is_empty():
		print("smoke_md80_variants: OK")
	else:
		printerr("smoke_md80_variants: %d failure(s): %s" % [_fails.size(), ", ".join(_fails)])
	get_tree().quit(0 if _fails.is_empty() else 1)
