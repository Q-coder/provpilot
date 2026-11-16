extends Node
## Probe: sounds of an aircraft flown after another one in the same session.
##   godot --headless --path provpilot res://tests/probe_sound_swap.tscn -- a10 md80
## (one id = fresh start). Prints the audible sounds of the last aircraft.
const E := 2682767.0
const N := 1258797.0

func _fly(root: Node, id: String) -> void:
	root._on_welcome_flight_requested(id, E, N, 155.0)
	var j = root.jsb_node
	var tr := func(tag: String) -> void:
		print("TRACE %s %s: t=%.2f n2=%.2f run=%.0f starter=%.0f cutoff=%.0f pulse=%.0f starved=%s ff=%.3f wow=%.0f" % [id, tag, j.get_property("simulation/sim-time-sec"), j.get_property("propulsion/engine[0]/n2"), j.get_property("propulsion/engine[0]/set-running"), j.get_property("/controls/engines/engine[0]/starter"), j.get_property("/controls/engines/engine[0]/cutoff"), j.get_property("/systems/acconfig/autoconfig-running"), str(j.get_property("propulsion/tank[2]/contents-lbs")), j.get_property("propulsion/engine[0]/fuel-flow-rate-pps"), j.get_property("gear/unit[1]/WOW")])
	tr.call("requested")
	await get_tree().create_timer(1.0).timeout
	root._cancel_intro_zoom()
	var waited := 0.0
	while root._fly_loading and waited < 120.0:
		await get_tree().create_timer(0.25).timeout
		waited += 0.25
		tr.call("loading")
	for i in 12:
		await get_tree().create_timer(0.25).timeout
		tr.call("flying")
	root._welcome_screen.visible = false
	root._camera_rig.set_mode(0)
	root.camera_mode = 0
	root.camera1.current = true
	await get_tree().create_timer(6.0).timeout

func _ready() -> void:
	var ids: PackedStringArray = OS.get_cmdline_user_args()
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	for id in ids:
		await _fly(root, id)
		var snd: FGSounds = root._fg_sounds
		var audible: Array = []
		for n in snd.playing():
			if snd.volume_db_of(str(n)) > -60.0:
				audible.append("%s %.0fdB" % [n, snd.volume_db_of(str(n))])
		print("PROBE %s: count %d, playing %d, audible %d: %s" % [id, snd.count, snd.playing().size(), audible.size(), str(audible)])
		print("PROBE %s: sounds_active %s, fly_loading %s, cockpit_view %s, dist %.1f" % [id, str(root._aircraft.get("sounds_active")), str(root._fly_loading), str(root._fg_bridge.cockpit_view), snd.camera_distance_m])
		for k in ["sim/sound/internal-custom", "sim/sound/packs", "sim/sound/avionics-fan", "sim/sound/engine/MDA_WHN_1", "sim/current-view/internal", "sim/current-view/view-number", "engines/engine[0]/n1-actual", "engines/engine[0]/n2-actual", "controls/pneumatics/radio-rack", "gear/gear[0]/wow", "systems/electrical/bus/ac-l", "systems/pneumatics/total-psi", "engines/engine[0]/n1", "controls/engines/engine[0]/cutoff", "controls/engines/engine[0]/cutoff-switch", "controls/engines/engine[0]/starter", "consumables/fuel/total-fuel-lbs", "controls/fuel/tank[0]/boost-pump", "systems/fuel/feed-l", "controls/electric/battery-switch"]:
			print("PROBE   %s = %s" % [k, str(root._aircraft.get_fg_property(k))])
		for k in ["propulsion/engine[0]/n2", "propulsion/engine[0]/set-running", "propulsion/engine[0]/cutoff", "propulsion/engine[0]/starter", "propulsion/tank[0]/contents-lbs", "propulsion/total-fuel-lbs", "/controls/engines/engine[0]/cutoff", "/controls/engines/engine[0]/starter", "/controls/engines/engine[0]/cutoff-switch", "fcs/throttle-cmd-norm[0]", "simulation/sim-time-sec", "/systems/fuel/feed-l-psi", "propulsion/engine[0]/fuel-flow-rate-pps"]:
			print("PROBE   FDM %s = %s (has %s)" % [k, str(root.jsb_node.get_property(k)), str(root.jsb_node.has_property(k))])
		var rules = root._aircraft.get_fg_property_rules()
		print("PROBE   rules: %s" % (str(rules.get_count()) if rules and rules.has_method("get_count") else str(rules)))
		if ids.size() > 1 and id != ids[ids.size() - 1]:
			root._welcome_screen.show_welcome()
			await get_tree().create_timer(1.0).timeout
	get_tree().quit(0)
