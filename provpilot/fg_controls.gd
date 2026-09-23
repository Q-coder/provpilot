extends RefCounted
class_name FGControls
## Clickable cockpit controls of a FlightGear model (docs/A10_FLIGHTGEAR_PORT.md,
## subtask 5, D4): the manifest's `pick`, `knob` and `slider` records.
##
## Picking: a ray from the cockpit camera through the mouse is tested against
## the bounding box of every control mesh (no physics bodies needed for ~100
## small parts); the nearest hit gets the click. FlightGear input mapping:
##   pick   — left button runs the action's bindings, release runs `mod-up`
##   knob   — left click / wheel up = increase, right click / wheel down =
##            decrease, Shift for the coarse variants; old-style knobs with
##            an `action` run it on click and `release` on release
##   slider — click runs the action (FlightGear drags it; a click toggles)
## Bindings: property-toggle, property-assign, property-adjust (step / min /
## max / wrap), property-cycle, property-multiply, and the handful of Nasal
## scripts the A-10 uses, mapped by name in NASAL_MAP.
##
## Values changed by the controls live in `state` (property -> value), which
## FGBridge merges over its computed values every frame so switch positions
## persist and their animations follow. The host is told of every change
## through `on_change` (property, value) for real effects (brakes, lights,
## canopy, HSI offsets, accelerometer reset).

var state: Dictionary = {}
var on_change: Callable = Callable()
var _items: Array = []            # {node, aabb, recs: Array}
var _animator = null              # FGAnimator: current value of a property when not in state
var _pressed: Dictionary = {}     # button -> record whose release bindings are pending
var count := 0

## Nasal script (prefix) -> what to do. Args: the parsed call arguments.
const NASAL_MAP := {
	"electrical.land_lights_switcher": "land_lights",
	"electrical.nav_lights_switcher": "nav_lights",
	"canopy.canopy_switch": "canopy",
	"canopy.canopy_hold": "canopy_hold",
	"A10fuel.fuel_sel_knob_move": "fuel_sel",
	"A10fuel.fill_dis_toggle": "fill_dis",
	"apu.toggle_off_start_switch": "apu_switch",
	"nav_scripts.nav_mode_selector": "nav_mode",
	"provpilot.gear_toggle": "gear_toggle",   # ProvPilot-defined pick on the gear handle (build script EXTRA_CONTROLS)
}


func setup(manifest_path: String, model_root: Node, animator) -> int:
	_items.clear()
	_animator = animator
	count = 0
	var f := FileAccess.open(manifest_path, FileAccess.READ)
	if f == null or model_root == null:
		return 0
	var m: Dictionary = JSON.parse_string(f.get_as_text())
	var by_node: Dictionary = {}
	for rec in m.get("controls", []):
		for name in rec.get("objects", []):
			for n in model_root.find_children(str(name), "", true, false) + model_root.find_children(str(name) + "_???", "", true, false):
				# a control may be a group (AC3D object with kids): every mesh below it is clickable
				var meshes: Array = []
				if n is MeshInstance3D and (n as MeshInstance3D).mesh != null:
					meshes.append(n)
				for c in n.find_children("*", "MeshInstance3D", true, false):
					if (c as MeshInstance3D).mesh != null:
						meshes.append(c)
				for mi in meshes:
					if not by_node.has(mi):
						by_node[mi] = []
					by_node[mi].append(rec)
	for n in by_node.keys():
		_items.append({"node": n, "aabb": (n as MeshInstance3D).get_aabb(), "recs": by_node[n]})
		count += 1
	return count


## The control under the screen position, or null.
## Shift+Ctrl+M: translucent boxes on every clickable part's sense area
## (its mesh bounds, following the part's animation).
var debug := false
var _debug_boxes: Array = []

func set_debug(on: bool) -> void:
	debug = on
	for b in _debug_boxes:
		if is_instance_valid(b):
			b.queue_free()
	_debug_boxes.clear()
	if not on:
		return
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.2, 1.0, 0.4, 0.35)
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.no_depth_test = true
	for it in _items:
		var n: MeshInstance3D = it.node
		if not is_instance_valid(n):
			continue
		var box := MeshInstance3D.new()
		var bm := BoxMesh.new()
		var aabb: AABB = it.aabb
		bm.size = Vector3(maxf(aabb.size.x, 0.002), maxf(aabb.size.y, 0.002), maxf(aabb.size.z, 0.002))
		box.mesh = bm
		box.material_override = mat
		box.position = aabb.get_center()
		box.name = "HotspotDebug"
		n.add_child(box)
		_debug_boxes.append(box)


func hit(camera: Camera3D, screen_pos: Vector2) -> Dictionary:
	if camera == null:
		return {}
	var from: Vector3 = camera.project_ray_origin(screen_pos)
	var dir: Vector3 = camera.project_ray_normal(screen_pos)
	var best: Dictionary = {}
	var best_d := INF
	for it in _items:
		var n: MeshInstance3D = it.node
		if not is_instance_valid(n) or not n.is_visible_in_tree():
			continue
		var inv: Transform3D = n.global_transform.affine_inverse()
		var lf: Vector3 = inv * from
		var ld: Vector3 = (inv.basis * dir).normalized()
		var box: AABB = it.aabb
		var p = box.intersects_ray(lf, ld)
		if p == null:
			continue
		# the bounds only coarsely select: neighbouring parts (the ALT / BAT
		# halves of a rocker) overlap in depth, so take the nearest triangle
		# of the mesh itself; a mesh without faces keeps the box hit
		var hit_p = _nearest_face_hit(it, lf, ld)
		if hit_p != null:
			p = hit_p
		var d: float = (n.global_transform * p - from).length()
		if d < best_d and d < 4.0:
			best_d = d
			best = it
	return best


## Nearest ray / triangle hit on the part's mesh (local space), or null.
func _nearest_face_hit(it: Dictionary, lf: Vector3, ld: Vector3):
	if not it.has("faces"):
		var n: MeshInstance3D = it.node
		it["faces"] = n.mesh.get_faces() if is_instance_valid(n) and n.mesh != null else PackedVector3Array()
	var faces: PackedVector3Array = it["faces"]
	if faces.is_empty():
		return null
	var best = null
	var best_d := INF
	for i in range(0, faces.size() - 2, 3):
		var hp = Geometry3D.ray_intersects_triangle(lf, ld, faces[i], faces[i + 1], faces[i + 2])
		if hp == null:
			hp = Geometry3D.ray_intersects_triangle(lf, ld, faces[i], faces[i + 2], faces[i + 1])
		if hp != null:
			var d: float = (hp - lf).length()
			if d < best_d:
				best_d = d
				best = hp
	return best


func hover_name(camera: Camera3D, screen_pos: Vector2) -> String:
	var it := hit(camera, screen_pos)
	return str((it.node as Node).name) if not it.is_empty() else ""


## Mouse button on the cockpit; returns true when a control took it.
func click(camera: Camera3D, screen_pos: Vector2, button: int, pressed: bool, shift: bool) -> bool:
	if not pressed:
		if _pressed.has(button):
			var rec: Dictionary = _pressed[button]
			_pressed.erase(button)
			_release(rec, button)
			return true
		return false
	var it := hit(camera, screen_pos)
	if it.is_empty():
		return false
	var took := false
	for rec in it.recs:
		if _press(rec, button, shift):
			took = true
			_pressed[button] = rec
	return took


func _press(rec: Dictionary, button: int, shift: bool) -> bool:
	var raw: Dictionary = rec.get("raw", {})
	match str(rec.get("type", "")):
		"knob":
			var inc: bool = button == MOUSE_BUTTON_LEFT or button == MOUSE_BUTTON_WHEEL_UP
			var dec: bool = button == MOUSE_BUTTON_RIGHT or button == MOUSE_BUTTON_WHEEL_DOWN or button == MOUSE_BUTTON_MIDDLE
			if raw.has("increase") or raw.has("decrease"):
				var key := ""
				if inc:
					key = "shift-increase" if shift and raw.has("shift-increase") else "increase"
				elif dec:
					key = "shift-decrease" if shift and raw.has("shift-decrease") else "decrease"
				if key == "" or not raw.has(key):
					return false
				_run(_bindings(raw[key]), rec)
				return true
			if raw.has("action") and (inc or dec):
				_run(_bindings(raw["action"]), rec, {"offset": 1.0 if inc else -1.0})
				return true
			return false
		"pick", "slider":
			# "shift-action" (sliders: the other direction) with Shift or the right button
			if raw.has("shift-action") and (shift or button == MOUSE_BUTTON_RIGHT):
				_run(_bindings(raw["shift-action"]), rec)
				return true
			var actions: Array = _as_list(raw.get("action", []))
			var has_right := false
			var has_wheel := false
			for a in actions:
				if a is Dictionary and int(str((a as Dictionary).get("button", "0"))) == 2:
					has_right = true
				if a is Dictionary and int(str((a as Dictionary).get("button", "0"))) in [3, 4]:
					has_wheel = true
			# Wheel on a pick that adjusts a property (Kollsman, heading bug...):
			# up increases, down decreases by the button-0 action's step, Shift
			# five steps (the fine and coarse picks are separate records, so the
			# coarse one is left alone)
			if not has_wheel and (button == MOUSE_BUTTON_WHEEL_UP or button == MOUSE_BUTTON_WHEEL_DOWN):
				var chosen = null
				for a in actions:
					if a is Dictionary and int(str((a as Dictionary).get("button", "0"))) == 0:
						chosen = a
				if chosen == null:
					return false
				var adjusts := false
				for b in _bindings(chosen):
					if b is Dictionary and str((b as Dictionary).get("command", "")) == "property-adjust":
						adjusts = true
				if not adjusts:
					return false
				_run(_bindings(chosen), rec, {"wheel": (1.0 if button == MOUSE_BUTTON_WHEEL_UP else -1.0) * (5.0 if shift else 1.0)})
				return true
			var ran := false
			for a in actions:
				if not (a is Dictionary):
					continue
				var b: int = int(str((a as Dictionary).get("button", "0")))
				var want := MOUSE_BUTTON_LEFT
				if b == 1:
					want = MOUSE_BUTTON_MIDDLE
				elif b == 2:
					want = MOUSE_BUTTON_RIGHT
				elif b == 3:
					want = MOUSE_BUTTON_WHEEL_UP
				elif b == 4:
					want = MOUSE_BUTTON_WHEEL_DOWN
				# FlightGear's middle button (the second switch direction) also answers to the right button
				var ok: bool = button == want or (b == 1 and button == MOUSE_BUTTON_RIGHT and not has_right)
				if not ok:
					continue
				_run(_bindings(a), rec)
				ran = true
			return ran
	return false


func _release(rec: Dictionary, button: int) -> void:
	var raw: Dictionary = rec.get("raw", {})
	if raw.has("release"):
		_run(_bindings(raw["release"]), rec)
	for a in _as_list(raw.get("action", [])):
		if a is Dictionary and (a as Dictionary).has("mod-up"):
			_run(_bindings((a as Dictionary)["mod-up"]), rec)


static func _as_list(v) -> Array:
	if v is Array:
		return v
	if v == null or (v is String and str(v).is_empty()):
		return []
	return [v]


static func _bindings(container) -> Array:
	if container is Dictionary:
		return _as_list((container as Dictionary).get("binding", []))
	return []


## Current value of a property: set by a control before, else the animator's.
func value_of(prop: String) -> float:
	var key := _norm(prop)
	if state.has(key):
		return float(state[key])
	if _animator != null:
		var v = _animator.get_property(key)
		if v != null and not (v is String):
			return float(v)
		if v is String and str(v).is_valid_float():
			return str(v).to_float()
	return 0.0


static func _norm(p: String) -> String:
	var s := p.strip_edges()
	while s.begins_with("/"):
		s = s.substr(1)
	return s.replace("[0]", "")


func _assign(prop: String, v: float) -> void:
	var key := _norm(prop)
	state[key] = v
	if on_change.is_valid():
		on_change.call(key, v)


func _run(bindings: Array, rec: Dictionary, ctx: Dictionary = {}) -> void:
	for b in bindings:
		if not (b is Dictionary):
			continue
		var cmd := str(b.get("command", ""))
		var prop := str(b.get("property", ""))
		match cmd:
			"property-toggle":
				_assign(prop, 0.0 if value_of(prop) > 0.5 else 1.0)
			"property-assign":
				var val = b.get("value", 0)
				if val is String and not str(val).is_valid_float():
					_assign_string(prop, str(val))
				else:
					_assign(prop, float(str(val)))
			"property-adjust":
				# FlightGear: `step` is a fixed increment; without it the knob's
				# click direction (offset ±1) times `factor` is the increment, so
				# old-style knobs (single `action`) turn both ways
				var inc: float = float(str(b.get("step"))) if b.has("step") else float(ctx.get("offset", 1.0)) * float(str(b.get("factor", "1")))
				if ctx.has("wheel"):
					inc = absf(inc) * float(ctx["wheel"])
				var v: float = value_of(prop) + inc
				var has_min: bool = b.has("min")
				var has_max: bool = b.has("max")
				var mn: float = float(str(b.get("min", "0"))) if has_min else 0.0
				var mx: float = float(str(b.get("max", "0"))) if has_max else 0.0
				if has_min and has_max and str(b.get("wrap", "false")) in ["true", "1"]:
					v = mn + fposmod(v - mn, mx - mn)
				else:
					if has_min:
						v = maxf(v, mn)
					if has_max:
						v = minf(v, mx)
				_assign(prop, v)
			"property-multiply":
				_assign(prop, value_of(prop) * float(str(b.get("factor", "1"))))
			"property-cycle":
				var vals: Array = _as_list(b.get("value", []))
				if vals.is_empty():
					continue
				var cur: float = value_of(prop)
				var idx := 0
				for i in range(vals.size()):
					if absf(float(str(vals[i])) - cur) < 1e-6:
						idx = (i + 1) % vals.size()
						break
				_assign(prop, float(str(vals[idx])))
			"nasal":
				_nasal(str(b.get("script", "")), ctx)
			_:
				pass


func _assign_string(prop: String, v: String) -> void:
	var key := _norm(prop)
	state[key] = v
	if on_change.is_valid():
		on_change.call(key, v)


## The few Nasal scripts the A-10's controls call, by name.
func _nasal(script: String, ctx: Dictionary) -> void:
	var s := script.strip_edges()
	# setprop("path", value) — used by the armament station buttons
	var re := RegEx.new()
	re.compile("setprop\\(\\s*\"([^\"]+)\"\\s*,\\s*([-0-9.]+)")
	var mm := re.search(s)
	if mm:
		_assign(mm.get_string(1), float(mm.get_string(2)))
		return
	var args: Array = []
	var open := s.find("(")
	if open >= 0:
		var inner := s.substr(open + 1, s.rfind(")") - open - 1)
		for a in inner.split(","):
			var t := a.strip_edges()
			if t.is_valid_float():
				args.append(t.to_float())
	for key in NASAL_MAP.keys():
		if s.begins_with(key):
			_nasal_action(NASAL_MAP[key], args, ctx)
			return


func _nasal_action(kind: String, args: Array, ctx: Dictionary) -> void:
	match kind:
		"land_lights":
			# 3-position switch 0 taxi .. 1 off .. 2 landing (Nasal: +1 / -1 steps)
			var pos: float = value_of("sim/model/A-10/controls/lighting/land-lights-switch")
			_assign("sim/model/A-10/controls/lighting/land-lights-switch", clampf(pos + (1.0 if args.size() > 0 and args[0] > 0 else -1.0), 0.0, 2.0))
		"nav_lights":
			# 0 off, 1 steady, 2 flash
			var pos: float = value_of("sim/model/A-10/controls/lighting/nav-lights-switch")
			_assign("sim/model/A-10/controls/lighting/nav-lights-switch", clampf(pos + (1.0 if args.size() > 0 and args[0] > 0 else -1.0), 0.0, 2.0))
		"canopy":
			# the switch is sprung: click toward open / close, release holds
			var dir: float = float(ctx.get("offset", 1.0))
			_assign("sim/model/A-10/controls/canopy/canopy-switch", 2.0 if dir > 0.0 else 0.0)
		"canopy_hold":
			_assign("sim/model/A-10/controls/canopy/canopy-switch", 1.0)
		"fuel_sel":
			var pos: float = value_of("sim/model/A-10/controls/fuel/fuel-dsp-sel")
			_assign("sim/model/A-10/controls/fuel/fuel-dsp-sel", clampf(pos + (args[0] if args.size() > 0 else 1.0), 0.0, 4.0))
		"fill_dis":
			if args.size() > 0:
				var prop := "controls/fuel/tank[%d]/fill-dis" % int(args[0])
				_assign(prop, 0.0 if value_of(prop) > 0.5 else 1.0)
		"apu_switch":
			_assign("controls/APU/off-start-switch", 0.0 if value_of("controls/APU/off-start-switch") > 0.5 else 1.0)
		"gear_toggle":
			_assign("provpilot/gear-toggle", 1.0)
		"nav_mode":
			# FlightGear nav_scripts.nav_mode_selector(n): 5 = ILS, 6 = TACAN
			var n: int = int(args[0]) if args.size() > 0 else 0
			_assign("sim/model/A-10/A-10-nav/selector-ils", 1.0 if n == 5 else 0.0)
			_assign("sim/model/A-10/A-10-nav/selector-tils", 1.0 if n == 6 else 0.0)
