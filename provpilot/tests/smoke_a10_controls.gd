extends Node
## Subtask 5 (docs/A10_FLIGHTGEAR_PORT.md): clickable FlightGear cockpit
## controls — synthetic clicks at the screen position of a switch mesh.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_a10_controls.tscn
const E := 2682767.0
const N := 1258797.0
var _fails: Array[String] = []

func _check(ok: bool, what: String) -> void:
	if ok:
		print("  PASS: " + what)
	else:
		_fails.append(what)
		printerr("  FAIL: " + what)

func _screen_of(cam: Camera3D, node: Node3D) -> Vector2:
	var mi: MeshInstance3D = node as MeshInstance3D
	if mi == null or mi.mesh == null:
		for c in node.find_children("*", "MeshInstance3D", true, false):
			if (c as MeshInstance3D).mesh != null:
				mi = c
				break
	return cam.unproject_position(mi.global_transform * mi.get_aabb().get_center())

func _ready() -> void:
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("a10", E, N, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(3.0).timeout
	var ctl = root._fg_controls
	var fga: FGAnimator = root._fg_animator
	var cam: Camera3D = root.camera1
	var mroot: Node = root.get_node(NodePath(root._model_root_path))
	_check(ctl.count > 80, "%d clickable cockpit parts resolved" % ctl.count)
	# aim the cockpit camera at the panel so the parts are in view
	cam.current = true
	# battery switch: property-toggle
	var batt: Node3D = mroot.find_child("A-10-elec-pwr-ctrl__battery-pwr-switch", true, false)
	var pos: Vector2 = _screen_of(cam, batt)
	var over: String = ctl.hover_name(cam, pos)
	print("  [diag] battery switch at %s, hover '%s'" % [str(pos), over])
	_check(over.begins_with(batt.name), "hover finds the battery switch under the cursor")
	var batt_before: float = float(ctl.state.get("controls/electric/battery-switch", 0.0))   # seeded ON
	_check(ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false), "click on the battery switch is taken")
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	var batt_after: float = float(ctl.state.get("controls/electric/battery-switch", 0.0))
	_check(absf(batt_after - batt_before) > 0.5, "battery switch toggled (%.0f -> %.0f)" % [batt_before, batt_after])
	await get_tree().process_frame
	await get_tree().process_frame
	_check(absf(float(fga.get_property("controls/electric/battery-switch")) - batt_after) < 1e-6, "the animator sees the switch state")
	# and back on, so the rest of the cockpit stays powered
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	await get_tree().process_frame
	# HSI course knob: left click +1, right click -1, shift +5
	var knob: Node3D = mroot.find_child("A-10-HSI__course-knob", true, false)
	pos = _screen_of(cam, knob)
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, true); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, true)
	ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, false, false)
	var crs: float = float(ctl.state.get("instrumentation/nav[1]/radials/selected-deg", -1.0))
	_check(absf(crs - 5.0) < 1e-6, "course knob: +1, +5 (shift), -1 -> 5 (got %.1f)" % crs)
	# wheel down 6 times wraps below 0
	for i in range(6):
		ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_DOWN, true, false)
	crs = float(ctl.state.get("instrumentation/nav[1]/radials/selected-deg", -1.0))
	_check(absf(crs - 359.0) < 1e-6, "course knob wraps to 359 (got %.1f)" % crs)
	# altimeter Kollsman knob (pick pair from fgdata): left zone decreases, right
	# increases; the wheel steps by the fine action, Shift five steps
	var kl: Node3D = mroot.find_child("altimeter__ingh-knob-left", true, false)
	var kr: Node3D = mroot.find_child("altimeter__ingh-knob-right", true, false)
	if kl and kr:
		var k0: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		pos = _screen_of(cam, kl)
		ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
		var k1: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		pos = _screen_of(cam, kr)
		ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, false, false)
		var k2: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_DOWN, true, false); ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_DOWN, false, false)
		var k3: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_UP, true, true); ctl.click(cam, pos, MOUSE_BUTTON_WHEEL_UP, false, true)
		var k4: float = float(ctl.state.get("instrumentation/altimeter/setting-inhg", 0.0))
		_check(absf(k1 - (k0 - 0.005)) < 1e-6 and absf(k2 - (k1 + 0.025)) < 1e-6, "Kollsman knob: left zone -0.005, right zone right-click +0.025 (%.3f -> %.3f -> %.3f)" % [k0, k1, k2])
		_check(absf(k3 - (k2 - 0.005)) < 1e-6 and absf(k4 - (k3 + 0.025)) < 1e-6, "wheel down -0.005, Shift wheel up +0.025 on the knob (%.3f -> %.3f)" % [k3, k4])
	else:
		_check(false, "altimeter knob zones present")
	# landing light switch (Nasal switcher): off (1) -> landing (2) turns the lights on
	var ll: Node3D = mroot.find_child("A-10-ld-gear-panel__light-land-switch", true, false)
	pos = _screen_of(cam, ll)
	_check(ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false), "landing light switch clickable")
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	await get_tree().process_frame
	_check(root._aircraft_lights.landing_lights_on(), "landing light switch turns the landing lights on")
	# parking brake handle (slider, property-toggle) sets the FDM brakes
	var pb: Node3D = mroot.find_child("A-10-emer-brake__emer-brake", true, false)
	pos = _screen_of(cam, pb)
	# every flight starts with the parking brake set: the first click releases it
	_check(float(ctl.state.get("controls/gear/brake-parking", 0.0)) > 0.5 and root.jsb_node.get_input_brake() > 0.9, "parking brake set at flight start (%.1f)" % root.jsb_node.get_input_brake())
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	_check(float(ctl.state.get("controls/gear/brake-parking", 1.0)) < 0.5 and root.jsb_node.get_input_brake() < 0.1, "parking brake handle releases the brakes (%.1f)" % root.jsb_node.get_input_brake())
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	_check(root.jsb_node.get_input_brake() > 0.9, "and sets them again (%.1f)" % root.jsb_node.get_input_brake())
	# the "," key releases them and the handle follows
	root.jsb_node.set_input_brake(0.0)
	await get_tree().process_frame
	await get_tree().process_frame
	_check(float(ctl.state.get("controls/gear/brake-parking", 1.0)) < 0.5, "handle follows the brake key")
	root.jsb_node.set_input_brake(1.0)
	# accelerometer reset button: pick with property-assign, consumed by the host
	root._fg_bridge.g_max = 3.5
	var acc: Node3D = mroot.find_child("A-10-accel__button", true, false)
	pos = _screen_of(cam, acc)
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	_check(absf(root._fg_bridge.g_max - 1.0) < 1e-6 and not ctl.state.has("sim/model/A-10/instrumentation/g-meter/g-max"), "accelerometer reset button resets the memory needles")
	# knobs and switches move: the battery switch (knob record) rotated after the toggle
	var batt_t: Transform3D = fga.animated_transform(batt.name if batt is MeshInstance3D else (batt.find_children("*", "MeshInstance3D", true, false)[0] as Node).name)
	var batt_deg: float = rad_to_deg(batt_t.basis.get_rotation_quaternion().get_angle())
	_check(batt_deg > 5.0, "battery switch lever rotates when toggled (%.1f deg)" % batt_deg)
	# 3-position light switch: Shift-click (or right click) moves it back down
	pos = _screen_of(cam, ll)
	ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, false, false)
	_check(absf(float(ctl.state.get("sim/model/A-10/controls/lighting/land-lights-switch", -1.0)) - 1.0) < 1e-6, "right click moves the light switch back to off (1)")
	ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, false, false)
	_check(absf(float(ctl.state.get("sim/model/A-10/controls/lighting/land-lights-switch", -1.0))) < 1e-6, "and again to taxi (0)")
	# gear handle: ProvPilot-defined pick toggles the gear command
	var gh: Node3D = mroot.find_child("A-10-ld-gear-panel__ld-gear-handle", true, false)
	pos = _screen_of(cam, gh)
	_check(ctl.hover_name(cam, pos) != "", "gear handle is clickable")
	var cmd_before: float = root.jsb_node.get_gear_cmd()
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	_check(absf(root.jsb_node.get_gear_cmd() - cmd_before) > 0.5, "gear handle click toggles the gear command (%.0f -> %.0f)" % [cmd_before, root.jsb_node.get_gear_cmd()])
	ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	# old-style knob (single action, property-adjust with factor): left = +1, right = -1, clamped 0..3
	var hm: Node3D = mroot.find_child("A-10-hud-panel__mode-selector", true, false)
	pos = _screen_of(cam, hm)
	for i in range(2):
		ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	_check(absf(float(ctl.state.get("controls/hud/m-sel", -1.0)) - 2.0) < 1e-6, "HUD mode selector: two left clicks -> 2")
	ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_RIGHT, false, false)
	_check(absf(float(ctl.state.get("controls/hud/m-sel", -1.0)) - 1.0) < 1e-6, "right click turns it back to 1")
	for i in range(5):
		ctl.click(cam, pos, MOUSE_BUTTON_LEFT, true, false); ctl.click(cam, pos, MOUSE_BUTTON_LEFT, false, false)
	_check(absf(float(ctl.state.get("controls/hud/m-sel", -1.0)) - 3.0) < 1e-6, "stops at its max of 3")
	# lit caution lamps keep their label: flat emissive materials got the albedo as emission texture
	var lamp: MeshInstance3D = mroot.find_child("cockpit__l-gen", true, false)
	var lm: BaseMaterial3D = lamp.get_active_material(0)
	_check(lm.shading_mode == BaseMaterial3D.SHADING_MODE_UNSHADED and not lm.emission_enabled, "lit caution lamp drawn unshaded with its label texture")
	# a click on empty panel is not taken
	_check(not ctl.click(cam, Vector2(5, 5), MOUSE_BUTTON_LEFT, true, false), "a click away from any control is not taken")
	if _fails.is_empty():
		print("smoke_a10_controls: OK")
		get_tree().quit(0)
	else:
		printerr("smoke_a10_controls: %d failure(s)" % _fails.size())
		get_tree().quit(1)
