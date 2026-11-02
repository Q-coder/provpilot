extends Node
## Probe (not a smoke test): MD-82 airborne, engage the AP in HDG SEL + V/S
## and print the DFGS rule chain once a second.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/probe_md80_autoflight.tscn
const E := 2682767.0
const N := 1258797.0
const ROLL := ["it-autoflight/output/ap-on", "it-autoflight/output/apfd-on", "it-autoflight/internal/heading-deg", "it-autoflight/internal/hdg-target", "it-autoflight/internal/heading-error-deg", "it-autoflight/internal/target-roll-deg", "it-autoflight/internal/target-roll", "it-autoflight/internal/roll-rate", "it-autoflight/internal/aileron-cmd", "systems/dfgs/afs/roll-cmd"]
const PITCH := ["it-autoflight/internal/vs-alt-active", "it-autoflight/internal/vs-alt", "it-autoflight/internal/target-fpm-vs", "it-autoflight/internal/target-common-fpm", "it-autoflight/internal/target-fpm", "it-autoflight/internal/target-pitch-deg", "it-autoflight/internal/pitch-rate", "it-autoflight/internal/elevator-cmd", "systems/dfgs/afs/pitch-cmd", "it-autoflight/internal/out-of-trim", "controls/flight/elevator-trim", "it-autoflight/internal/vert-speed-fpm"]

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
	af.set_option("debug", 1.0)
	await get_tree().create_timer(6.0).timeout
	_dump("before", jsb, af, rules)
	af.panel("apSwitch", 0.0)
	for i in range(4):
		af.panel("hdgAdjust", 10.0)
	af.panel("hdgPull", 0.0)
	for i in range(12):
		af.panel("vsAdjust", 1.0)
	for t in range(25):
		await get_tree().create_timer(1.0).timeout
		_dump("t+%d" % (t + 1), jsb, af, rules)
	get_tree().quit(0)

func _dump(tag: String, jsb, af, rules) -> void:
	var r := ""
	for k in ROLL:
		r += " %s=%.2f" % [k.get_file(), rules.get_value(k)]
	var p := ""
	for k in PITCH:
		p += " %s=%.2f" % [k.get_file(), rules.get_value(k)]
	print("[%s] psi %.1f phi %.1f theta %.1f hdot %.0f fpm q %.2f | lat %d vert %d hdg-bug %s vs-bug %s | FDM roll-cmd %.3f pitch-cmd %.3f enabled %s yoke-sw %.3f elev-cmd %.3f trim %.3f" % [tag, fposmod(jsb.get_property("attitude/psi-deg"), 360.0), jsb.get_property("attitude/phi-deg"), jsb.get_property("attitude/theta-deg"), jsb.get_property("velocities/h-dot-fps") * 60.0, jsb.get_property("velocities/q-rad_sec") * 57.3, af.get_lat_mode(), af.get_vert_mode(), str(af.get_value("it-autoflight/input/hdg")), str(af.get_value("it-autoflight/input/vs")), jsb.get_property("/systems/dfgs/afs/roll-cmd"), jsb.get_property("/systems/dfgs/afs/pitch-cmd"), str(jsb.get_property("/systems/dfgs/afs/enabled")), jsb.get_property("/systems/fcs/elevators/yoke-switch"), jsb.get_property("fcs/elevator-cmd-norm"), jsb.get_pitch_trim()])
	print("   roll:" + r)
	print("   pitch:" + p)
