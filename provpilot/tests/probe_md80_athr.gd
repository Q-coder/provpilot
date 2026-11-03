extends Node
## Probe (not a smoke test): MD-82 airborne, A/T in SPD and the two thrust
## levers; prints both engines' autothrottle chain once a second.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_athr.tscn
const E := 2682767.0
const N := 1258797.0
const RULE := ["it-autoflight/output/athr-on", "it-autoflight/internal/throttle-cmd", "it-autoflight/internal/ats-max", "systems/engines/power-norm[0]", "systems/engines/power-norm[1]", "systems/engines/ats-power-cmd[0]", "systems/engines/ats-power-cmd[1]", "systems/engines/ats-cmd-1", "systems/engines/ats-cmd-2", "controls/engines/engine/throttle", "controls/engines/engine[1]/throttle"]
const FDM := ["/it-autoflight/output/athr", "/it-autoflight/output/thr-mode", "/systems/engines/control-1/ats-max", "/systems/engines/control-2/ats-max", "/controls/engines/engine[0]/man-epr-set", "/controls/engines/engine[1]/man-epr-set", "/systems/engines/limit/active-norm-no-art", "/systems/engines/ats-cmd-1", "/systems/engines/ats-cmd-2", "/systems/engines/control-1/throttle-pos", "/systems/engines/control-2/throttle-pos", "/controls/engines/engine[0]/throttle", "/controls/engines/engine[1]/throttle", "/controls/cockpit/throttle-lever[0]", "/controls/cockpit/throttle-lever[1]", "/engines/engine[0]/n1-actual", "/engines/engine[1]/n1-actual", "propulsion/engine[0]/n1", "propulsion/engine[1]/n1", "fcs/throttle-pos-norm[0]", "fcs/throttle-pos-norm[1]", "fcs/throttle-cmd-norm[0]", "fcs/throttle-cmd-norm[1]"]

func _ready() -> void:
	get_window().size = Vector2i(1200, 800)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._settings.get_settings()["aircraft_md80_start_agl_ft"] = 6000.0
	root._settings.get_settings()["aircraft_md80_start_speed_kt"] = 250.0
	root._settings.get_settings()["aircraft_md80_start_heading_deg"] = 155.0
	root._settings.get_settings()["weather_layers"] = []
	root._settings.get_settings()["live_weather"] = false
	root._push_weather_settings()
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var rig = root._aircraft
	var af = rig.get_autoflight()
	var rules = rig.get_fg_property_rules()
	var fgc = rig.get_fg_controls()
	af.set_option("debug", 1.0)
	await get_tree().create_timer(6.0).timeout
	var st: Dictionary = fgc.get_state()
	for k in st.keys():
		if "throttle" in String(k) or "engine" in String(k):
			print("  control state: %s = %s" % [k, str(st[k])])
	_dump("before", jsb, af, rules)
	af.panel("apSwitch", 0.0)
	var cs: Dictionary = fgc.get_state()
	cs["it-autoflight/input/athr"] = 1.0
	cs["systems/engines/limit/active-mode-int"] = 3.0
	fgc.set_state(cs)
	await get_tree().create_timer(0.5).timeout
	af.panel("spdSel", 0.0)
	for t in range(30):
		await get_tree().create_timer(1.0).timeout
		_dump("AT t+%d" % (t + 1), jsb, af, rules)
	# manual: A/T off, then the levers as the mouse moves them
	af.panel("atDisc", 0.0)
	for t in range(5):
		await get_tree().create_timer(1.0).timeout
		_dump("AT off t+%d" % (t + 1), jsb, af, rules)
	print("== right lever to 0.3")
	fgc.assign_external("controls/engines/engine[1]/throttle", 0.3)
	for t in range(4):
		await get_tree().create_timer(1.0).timeout
		_dump("R lever t+%d" % (t + 1), jsb, af, rules)
	print("== left lever to 0.8")
	fgc.assign_external("controls/engines/engine/throttle", 0.8)
	for t in range(4):
		await get_tree().create_timer(1.0).timeout
		_dump("L lever t+%d" % (t + 1), jsb, af, rules)
	print("== right lever to 0.5")
	fgc.assign_external("controls/engines/engine[1]/throttle", 0.5)
	for t in range(4):
		await get_tree().create_timer(1.0).timeout
		_dump("R lever again t+%d" % (t + 1), jsb, af, rules)
	print("== keyboard throttle to 0.6")
	jsb.set_input_throttle(0.6)
	for t in range(4):
		await get_tree().create_timer(1.0).timeout
		_dump("key t+%d" % (t + 1), jsb, af, rules)
	st = fgc.get_state()
	for k in st.keys():
		if "throttle" in String(k):
			print("  control state: %s = %s" % [k, str(st[k])])
	get_tree().quit(0)

func _dump(tag: String, jsb, af, rules) -> void:
	var r := ""
	for k in RULE:
		r += " %s=%.3f" % [k.get_file(), rules.get_value(k)]
	var f := ""
	for k in FDM:
		f += " %s=%.3f" % [k.get_file(), jsb.get_property(k)]
	print("[%s] kias %.0f thr-mode %d athr %s input_throttle %.3f" % [tag, jsb.get_property("velocities/vc-kts"), af.get_thr_mode(), str(af.get_value("it-autoflight/output/athr")), jsb.get_input_throttle()])
	print("   rules:" + r)
	print("   fdm:" + f)
