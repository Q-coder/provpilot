extends Node
## Probe: MD-80 autoconfig pulse -> AHRS aligned -> ADI/HSI live; APU start through the control state; main engines keep running with the cutoff links.
const E := 2682767.0
const N := 1258797.0

func _p(jsb, fga, p: String) -> void:
	var v = jsb.get_property(p)
	var a = fga.get_property(p.trim_prefix("/")) if fga else null
	print("  [probe] %-56s fdm %-10s anim %s" % [p, str(v).left(10), str(a)])

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(4.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var jsb = root.jsb_node
	var fga: FGAnimator = root._aircraft.get_fg_animator()
	var ctl: FGControls = root._aircraft.get_fg_controls()
	for t in [2, 6, 10]:
		await get_tree().create_timer(4.0 if t > 2 else 2.0).timeout
		print("=== t+%d s" % t)
		for p in ["/systems/acconfig/autoconfig-running", "/systems/ahrs[0]/can-align", "/systems/ahrs[0]/align-timer", "/systems/ahrs[0]/aligned", "/systems/platform[0]/aligned", "/instrumentation/ai[0]/att-avail", "/instrumentation/ai[0]/pitch-deg", "/instrumentation/ai[0]/roll-deg", "/instrumentation/hsi[0]/heading-avail", "/instrumentation/hsi[0]/heading-linear", "/instrumentation/rmi[0]/heading-linear", "/systems/ignition/cutoff-1", "/systems/ignition/starter-jsb-1", "/controls/engines/engine[0]/cutoff", "/engines/engine[0]/n2-actual", "/engines/engine[1]/n2-actual"]:
			_p(jsb, fga, p)
	print("=== APU: RUN 4 s, START held 3 s, RUN")
	var st: Dictionary = ctl.get_state()
	st["controls/apu/master"] = 1.0
	ctl.set_state(st)
	await get_tree().create_timer(4.0).timeout
	st = ctl.get_state()
	st["controls/apu/master"] = 2.0
	ctl.set_state(st)
	await get_tree().create_timer(3.0).timeout
	for p in ["/controls/apu/master", "/systems/apu/starter-latch", "/systems/apu/starter", "/controls/engines/engine[2]/starter", "/controls/engines/engine[2]/cutoff", "/engines/engine[2]/n2-actual", "propulsion/engine[2]/n2"]:
		_p(jsb, fga, p)
	st = ctl.get_state()
	st["controls/apu/master"] = 1.0
	ctl.set_state(st)
	for i in range(4):
		await get_tree().create_timer(5.0).timeout
		print("  -- released, t+%d s" % ((i + 1) * 5))
		for p in ["/systems/apu/starter", "/systems/apu/cutoff", "/systems/apu/cutoff-cmd", "/systems/apu/state", "/systems/apu/n1-cmd", "/engines/engine[2]/n2", "/engines/engine[2]/n2-actual", "propulsion/engine[2]/n2", "propulsion/engine[2]/cutoff", "propulsion/engine[2]/starter", "propulsion/engine[2]/set-running", "propulsion/engine[2]/fuel-flow-rate-pps", "propulsion/tank[5]/contents-lbs", "/instrumentation/apu-n/n", "/systems/electrical/sources/apu-gen/output-volt"]:
			_p(jsb, fga, p)
	print("=== main engines after 60 s")
	for p in ["/engines/engine[0]/n2-actual", "/engines/engine[1]/n2-actual", "/systems/electrical/bus/ac-l", "/instrumentation/ai[0]/att-avail"]:
		_p(jsb, fga, p)
	get_tree().quit(0)
