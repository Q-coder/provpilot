extends Node
## Headless smoke test for the keyboard mapping (src/key_bindings.cpp) and the
## Settings > Keyboard panel (src/keyboard_settings_panel.cpp). Hermetic: the
## KeyBindings node under test writes a scratch keyboard.cfg under /tmp and the
## KeyConfig autoload (if any) is left as it is. Exits 0 on success, 1 on failure.
##
##   godot --headless --path provpilot res://tests/smoke_keyboard.tscn

var fails: Array[String] = []

const TMP := "/tmp/provpilot_keyboard_smoke"


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


func _key(code: Key, unicode: int = 0, shift := false, ctrl := false, alt := false, meta := false) -> InputEventKey:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.physical_keycode = code
	ev.unicode = unicode
	ev.shift_pressed = shift
	ev.ctrl_pressed = ctrl
	ev.alt_pressed = alt
	ev.meta_pressed = meta
	ev.pressed = true
	return ev


func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(TMP)
	_test_chords()
	_test_catalog_and_matching()
	await _test_persistence()
	await _test_panel()
	if fails.is_empty():
		print("SMOKE KEYBOARD: ALL PASS")
	else:
		printerr("SMOKE KEYBOARD: %d failure(s)" % fails.size())
		for f in fails:
			printerr("   - ", f)
	get_tree().quit(0 if fails.is_empty() else 1)


func _test_chords() -> void:
	print("== chords")
	check(KeyBindings.chord_of(_key(KEY_F, 0, true, true)) == "Ctrl+Shift+F", "Ctrl+Shift+F reads back in canonical order")
	check(KeyBindings.chord_of(_key(KEY_Z, 0, false, false, false, true)) == "Ctrl+Z", "Cmd counts as Ctrl")
	check(KeyBindings.chord_of(_key(KEY_PAGEUP)) == "PageUp", "named key")
	check(KeyBindings.chord_of(_key(KEY_5, "[".unicode_at(0), false, false, true)) == "[", "Option+5 typing [ is the [ key")
	check(KeyBindings.chord_of(_key(KEY_8, "{".unicode_at(0), false, false, true)) == "Shift+[", "Option+8 typing { is Shift+[")
	check(KeyBindings.chord_of(_key(KEY_1, "+".unicode_at(0), true)) == "Shift+=", "Shift+1 typing + is Shift+=")
	check(KeyBindings.chord_of(_key(KEY_SHIFT)) == "", "a bare modifier press is no chord")


func _test_catalog_and_matching() -> void:
	print("== catalog")
	var kb := KeyBindings.new()
	add_child(kb)
	var sections: Array = KeyBindings.get_sections()
	var ids := []
	for s in sections:
		ids.append(s["id"])
	check(ids == ["flight", "vehicle", "slew", "editor"], "four sections in order: %s" % [ids])
	var flight: Array = kb.get_actions("flight")
	var editor: Array = kb.get_actions("editor")
	check(flight.size() > 30 and editor.size() > 30, "catalog populated (%d flight, %d editor)" % [flight.size(), editor.size()])
	check(kb.get_keys("slew_up") == PackedStringArray(["Q", "PageUp"]), "two default keys for slew up: %s" % [kb.get_keys("slew_up")])
	check(KeyBindings.label("undo") == "Ctrl+Z" and KeyBindings.labels("finish") == "Enter / KpEnter", "labels: %s, %s" % [KeyBindings.label("undo"), KeyBindings.labels("finish")])
	print("== event matching (defaults)")
	check(KeyBindings.matches("undo", _key(KEY_Z, 0, false, true)), "Ctrl+Z is undo")
	check(KeyBindings.matches("undo", _key(KEY_Z, "z".unicode_at(0), false, false, false, true)), "Cmd+Z is undo")
	check(not KeyBindings.matches("undo", _key(KEY_Z)), "plain Z is not undo")
	check(KeyBindings.matches("height_down", _key(KEY_BRACKETLEFT, "[".unicode_at(0))), "[ lowers")
	check(KeyBindings.matches("height_down", _key(KEY_5, "[".unicode_at(0), false, false, true)), "Option+5 typing [ lowers (Swiss Mac)")
	check(not KeyBindings.matches("tool_circle", _key(KEY_5, "[".unicode_at(0), false, false, true)), "... and does not pick tool 5")
	check(KeyBindings.matches("height_down_big", _key(KEY_8, "{".unicode_at(0), false, false, true)), "Option+8 typing { lowers by 5 m")
	check(KeyBindings.matches("height_down_big", _key(KEY_BRACKETLEFT, "{".unicode_at(0), true)), "Shift+[ typing { lowers by 5 m")
	check(not KeyBindings.matches("height_down", _key(KEY_BRACKETLEFT, "{".unicode_at(0), true)), "{ is not the small step")
	check(KeyBindings.matches("map_zoom_in", _key(KEY_1, "+".unicode_at(0), true)), "Shift+1 typing + zooms the map in (Swiss layout)")
	check(KeyBindings.matches("map_zoom_in", _key(KEY_EQUAL, "=".unicode_at(0))), "= zooms the map in")
	check(KeyBindings.matches("finish", _key(KEY_KP_ENTER)), "keypad Enter finishes (second key)")
	check(KeyBindings.matches("flaps_up", _key(KEY_F, "F".unicode_at(0), true)) and not KeyBindings.matches("flaps_down", _key(KEY_F, "F".unicode_at(0), true)), "Shift+F is flaps up, not down")
	print("== editing")
	kb.set_keys("flaps_down", PackedStringArray(["V", ""]))
	check(kb.get_keys("flaps_down")[0] == "V" and not kb.is_default("flaps_down"), "rebinding flaps down to V")
	check(KeyBindings.matches("flaps_down", _key(KEY_V, "v".unicode_at(0))) == false, "a node that is not the autoload does not drive the static reads")
	kb.set_keys("flaps_up", PackedStringArray(["V"]))
	check(kb.conflicts("flaps_down") == PackedStringArray(["flaps_up"]), "the same key twice in a section is a conflict: %s" % [kb.conflicts("flaps_down")])
	kb.set_keys("slew_forward", PackedStringArray(["V"]))
	check(kb.conflicts("flaps_down") == PackedStringArray(["flaps_up"]), "another section never conflicts")
	kb.reset_action("flaps_down")
	check(kb.is_default("flaps_down") and kb.conflicts("flaps_up").is_empty(), "reset_action restores the default")
	kb.reset_to_defaults()
	check(kb.is_default("flaps_up") and kb.is_default("slew_forward"), "reset_to_defaults restores everything")
	kb.queue_free()


func _test_persistence() -> void:
	print("== keyboard.cfg")
	var path := TMP.path_join("keyboard.cfg")
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(path)
	var kb := KeyBindings.new()
	kb.set_config_path(path)
	add_child(kb)
	kb.set_keys("flaps_down", PackedStringArray(["V", "Kp7"]))
	kb.set_keys("slew_fast", PackedStringArray(["Alt"]))
	kb.set_keys("undo", PackedStringArray([]))
	kb.save()
	check(FileAccess.file_exists(path), "save writes %s" % path)
	var cf := ConfigFile.new()
	check(cf.load(path) == OK, "the file is a ConfigFile")
	check(cf.get_value("flight", "flaps_down", "") == "V, Kp7", "[flight] flaps_down = \"V, Kp7\" (got %s)" % [cf.get_value("flight", "flaps_down", "")])
	check(cf.get_value("slew", "slew_fast", "") == "Alt", "[slew] slew_fast = Alt")
	check(cf.get_value("editor", "undo", "x") == "", "an unbound action is an empty value")
	check(cf.has_section_key("editor", "save") and cf.get_value("editor", "save") == "Ctrl+S", "untouched actions are written too (the file documents every key)")
	# a second node reads it back
	var kb2 := KeyBindings.new()
	kb2.set_config_path(path)
	add_child(kb2)   # _ready loads
	await get_tree().process_frame
	check(kb2.get_keys("flaps_down") == PackedStringArray(["V", "Kp7"]), "load restores two keys: %s" % [kb2.get_keys("flaps_down")])
	check(kb2.get_keys("slew_fast")[0] == "Alt", "load restores a bare modifier")
	check(kb2.get_keys("undo") == PackedStringArray(["", ""]) and KeyBindings.label("undo") != "", "load restores an unbound action")
	check(kb2.get_keys("save")[0] == "Ctrl+S", "defaults for the rest")
	# hand-written spellings
	cf.set_value("flight", "flaps_down", "shift+pgup, ctrl+alt+ESC")
	cf.set_value("flight", "gear_toggle", "nonsense key")
	cf.save(path)
	kb2.load()
	check(kb2.get_keys("flaps_down") == PackedStringArray(["Shift+PageUp", "Ctrl+Alt+Escape"]), "spellings are normalised: %s" % [kb2.get_keys("flaps_down")])
	check(kb2.get_keys("gear_toggle") == PackedStringArray(["", ""]), "an unreadable chord leaves the slot empty")
	# the comma key survives the file's ", " separator (brakes_off ",", Shift+, in the editor, a comma first of two)
	cf.set_value("flight", "brakes_off", ",")
	cf.set_value("editor", "heading_left_big", "Shift+,")
	cf.set_value("editor", "heading_left", ",, Kp4")
	cf.set_value("flight", "flaps_down", "V,Kp7")
	cf.save(path)
	kb2.load()
	check(kb2.get_keys("brakes_off")[0] == ",", "a lone comma loads as the comma key: %s" % [kb2.get_keys("brakes_off")])
	cf.set_value("flight", "brakes_off", "")
	cf.set_value("editor", "heading_left", "")
	cf.set_value("editor", "heading_left_big", "Shift")
	cf.set_value("editor", "undo", "")
	cf.save(path)
	kb2.load()
	check(kb2.get_keys("brakes_off")[0] == "," and kb2.get_keys("heading_left")[0] == ",", "an empty entry whose default is the comma key takes the default back (files saved by older builds): %s / %s" % [kb2.get_keys("brakes_off"), kb2.get_keys("heading_left")])
	check(kb2.get_keys("heading_left_big")[0] == "Shift+,", "a bare Shift stored for Shift+, takes the default back: %s" % [kb2.get_keys("heading_left_big")])
	check(kb2.get_keys("undo") == PackedStringArray(["", ""]), "an empty entry of any other key stays unbound")
	cf.set_value("flight", "brakes_off", ",")
	cf.set_value("editor", "heading_left", ",, Kp4")
	cf.set_value("editor", "heading_left_big", "Shift+,")
	cf.save(path)
	kb2.load()
	check(kb2.get_keys("heading_left_big")[0] == "Shift+,", "Shift+, loads: %s" % [kb2.get_keys("heading_left_big")])
	check(kb2.get_keys("heading_left") == PackedStringArray([",", "Kp4"]), "comma first of two keys: %s" % [kb2.get_keys("heading_left")])
	check(kb2.get_keys("flaps_down") == PackedStringArray(["V", "Kp7"]), "a hand-written pair without the space still reads: %s" % [kb2.get_keys("flaps_down")])
	# a file from before reverse_toggle existed still has fdm_start on R (its old default)
	cf.set_value("flight", "fdm_start", "R")
	cf.erase_section_key("flight", "reverse_toggle")
	cf.save(path)
	kb2.load()
	check(kb2.get_keys("reverse_toggle")[0] == "R" and kb2.get_keys("fdm_start")[0] == "Shift+R", "a new action takes its key from a stored action still on the stale default (fdm_start R -> Shift+R, reverse_toggle R): %s / %s" % [kb2.get_keys("reverse_toggle")[0], kb2.get_keys("fdm_start")[0]])
	cf.set_value("flight", "fdm_start", "J")
	cf.save(path)
	kb2.load()
	check(kb2.get_keys("fdm_start")[0] == "J" and kb2.get_keys("reverse_toggle")[0] == "R", "a deliberate rebinding stays (fdm_start J)")
	kb.queue_free()
	kb2.queue_free()
	await get_tree().process_frame


func _find_first(node: Node, cls: String) -> Node:
	if node.get_class() == cls:
		return node
	for c in node.get_children():
		var r := _find_first(c, cls)
		if r != null:
			return r
	return null


func _test_panel() -> void:
	print("== panel")
	var kc := get_node_or_null("/root/KeyConfig")
	var own := false
	if kc == null:
		kc = KeyBindings.new()
		kc.name = "KeyConfig"
		kc.set_config_path(TMP.path_join("panel.cfg"))
		get_tree().root.add_child(kc)
		own = true
	else:
		kc.set_config_path(TMP.path_join("panel.cfg"))
	kc.reset_to_defaults()
	var panel := KeyboardSettingsPanel.new()
	add_child(panel)
	await get_tree().process_frame
	var row: Node = panel.find_child("flaps_down", true, false)
	check(row != null, "a row per action, named by the action id")
	if row == null:
		return
	var key0: Button = row.get_node("Key0")
	var key1: Button = row.get_node("Key1")
	var reset: Button = row.get_node("Reset")
	var conflict: Label = row.get_node("Conflict")
	check(key0.text == "F" and key1.text == "—" and reset.disabled, "row shows F, an empty second slot and no Reset while default")
	key0.pressed.emit()
	check(panel.get_capturing_action() == "flaps_down" and panel.get_capturing_slot() == 0 and key0.text.begins_with("press"), "clicking a key arms the capture")
	# Escape keeps the old key
	panel.handle_key_event(_key(KEY_ESCAPE))
	check(panel.get_capturing_action() == "" and key0.text == "F", "Escape cancels the capture")
	# a chord with modifiers
	key0.pressed.emit()
	panel.handle_key_event(_key(KEY_V, 0, false, true))
	check(kc.get_keys("flaps_down")[0] == "Ctrl+V" and key0.text == "Ctrl+V" and not reset.disabled, "Ctrl+V captured into slot 1 and Reset enabled")
	# a bare modifier: press and release Shift alone
	key1.pressed.emit()
	panel.handle_key_event(_key(KEY_SHIFT))
	check(panel.get_capturing_action() == "flaps_down", "a modifier press alone keeps waiting")
	var rel := _key(KEY_SHIFT)
	rel.pressed = false
	panel.handle_key_event(rel)
	check(kc.get_keys("flaps_down")[1] == "Shift" and key1.text == "Shift", "releasing it alone binds the bare modifier")
	# Backspace clears
	key1.pressed.emit()
	panel.handle_key_event(_key(KEY_BACKSPACE))
	check(kc.get_keys("flaps_down")[1] == "" and key1.text == "—", "Backspace clears the slot")
	# conflict mark
	key0.pressed.emit()
	panel.handle_key_event(_key(KEY_F, 0, true))
	check(conflict.text == "also: flaps_up", "a conflict with flaps_up is marked (got '%s')" % conflict.text)
	reset.pressed.emit()
	check(kc.is_default("flaps_down") and conflict.text == "" and key0.text == "F", "Reset restores the default and clears the mark")
	# refresh follows the config
	kc.set_keys("flaps_down", PackedStringArray(["K"]))
	panel.refresh()
	check(key0.text == "K", "refresh re-reads the rows")
	kc.reset_to_defaults()
	panel.queue_free()
	if own:
		kc.queue_free()
	await get_tree().process_frame
