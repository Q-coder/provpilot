extends Node3D
class_name AircraftLights
## Exterior lights of the flown aircraft (docs/AIRCRAFT_CONFIG.md, [lights]).
##
## Every fixture is a point sprite (shaders/aircraft_light.gdshader) parented
## to the model mesh node the config names, at that mesh's bounding-box
## centre, so it follows the airframe and its animations (the A-10's landing
## and taxi lights ride on the nose gear). Landing and taxi lights also get a
## SpotLight3D that lights the ground; it lives under this node (unscaled) and
## follows the fixture every frame, so the model's fitting scale cannot
## distort its range. A fixture node named "<x>-on" with a sibling "<x>-off"
## is a lit / unlit mesh pair: the pair's visibility follows the light.
##
## States: navigation lights and the beacon are on whenever an aircraft is
## loaded (battery on); strobes come on when airborne (no weight on wheels);
## landing and taxi lights are toggled with L by the host and go out while
## the gear is retracted.

const KINDS := ["nav_left", "nav_right", "nav_tail", "strobe", "beacon", "landing", "taxi"]
const COLOURS := {
	"nav_left": Color(1.0, 0.1, 0.05),
	"nav_right": Color(0.1, 1.0, 0.3),
	"nav_tail": Color(1.0, 0.97, 0.9),
	"strobe": Color(0.95, 0.97, 1.0),
	"beacon": Color(1.0, 0.15, 0.05),
	"landing": Color(1.0, 0.97, 0.9),
	"taxi": Color(1.0, 0.95, 0.85),
}
const SIZES := {
	"nav_left": 1.0, "nav_right": 1.0, "nav_tail": 1.0,
	"strobe": 1.6, "beacon": 1.2, "landing": 2.2, "taxi": 1.6,
}
const STROBE_PERIOD_S := 1.2      # double flash every 1.2 s
const STROBE_FLASH_S := 0.06
const BEACON_PERIOD_S := 1.0
const BEACON_FLASH_S := 0.18
const NIGHT_FLOOR := 0.45         # sprite brightness by day (1.0 at night)
const OUT_OFFSET := 0.10          # sprite offset outward from the lens centre, in the model's own units
const PULL_FACTOR := 0.12         # shader pull toward the camera = this x the model's world scale (metres)

var _shader: Shader = preload("res://shaders/aircraft_light.gdshader")
var _fixtures: Array = []         # {kind, sprite: MeshInstance3D, mat, on_mesh, off_mesh, spot: SpotLight3D, anchor: Node3D}
var _time := 0.0
var _landing_on := false
var _night := 0.0
var _airborne := false
var _gear_down := true
var _landing_pitch_deg := 5.0
var _landing_range_m := 300.0
var _taxi_range_m := 90.0
var _forward := Vector3.FORWARD  # aircraft forward in world space, set by the host each frame
var _down := Vector3.DOWN
var _spot_energy := 12.0
var _ac: Node3D = null
var debug_all_on := false         # probes: hold strobes and beacon lit for screenshots
var nav_enabled := true           # cockpit switches (FlightGear controls) gate these
var strobe_enabled := true
var beacon_enabled := true
var taxi_enabled := true          # taxi light switch (the L key / landing switch still gates it)
var powered := true               # electrical bus (FGBridge): nothing lights without it
var _pair_meshes := true


## Build the fixtures for the model under [param model_root] from the
## aircraft config's [lights] section. Safe to call again after a model swap.
## [param cfg] is the aircraft config object (node_3d preloads aircraft_config.gd).
## [param ac] is the aircraft frame node (nose along +Z, +X left, +Y up):
## each sprite is moved OUT_OFFSET along its fixture's outward direction
## (wingtip lights outboard, beacon up, nose lights forward, tail aft) so it
## sits just outside the skin: a point at the lens centre would be hidden by
## the lens from most angles, and pulling it toward the camera instead made
## wingtip lights show through the wing from above. Offsets are in the
## model's own units because the airframes are drawn at a fraction of world
## scale (the AC frame is 0.1, the A-10 model ends up at 0.2 m per unit).
func setup(model_root: Node, cfg, ac: Node3D = null) -> void:
	clear()
	_ac = ac
	if model_root == null or cfg == null:
		return
	_landing_pitch_deg = cfg.get_float("lights", "landing_pitch_deg", 5.0)
	_landing_range_m = cfg.get_float("lights", "landing_range_m", 300.0)
	_taxi_range_m = cfg.get_float("lights", "taxi_range_m", 90.0)
	_spot_energy = cfg.get_float("lights", "spot_energy", 12.0)
	# "pair_meshes false": leave "<x>-on"/"<x>-off" mesh visibility to another
	# system (the FlightGear select animations on the A-10)
	_pair_meshes = cfg.get_bool("lights", "pair_meshes", true)
	for kind in KINDS:
		for name in cfg.get_string_array("lights", kind, []):
			var node_name := str(name).strip_edges()
			if node_name.is_empty():
				continue
			var anchor: Node = model_root.find_child(node_name, true, false)
			if anchor == null or not (anchor is Node3D):
				push_warning("AircraftLights: node '%s' for %s not found in the model" % [node_name, kind])
				continue
			# "<kind>_anchor": an animated part the lens rides on (see _ride_on)
			var ride_name: String = cfg.get_string("lights", kind + "_anchor", "")
			if not ride_name.is_empty():
				var ride: Node = model_root.find_child(ride_name, true, false)
				if ride is Node3D:
					# the unlit twin (looked up before the lens moves) rides along
					var sib: Node = null
					if anchor.name.ends_with("-on") and anchor.get_parent():
						sib = anchor.get_parent().get_node_or_null(anchor.name.left(anchor.name.length() - 3) + "-off")
					_ride_on(anchor as Node3D, ride as Node3D, model_root)
					if sib is Node3D and sib.get_parent() != ride:
						_ride_on(sib as Node3D, ride as Node3D, model_root)
				else:
					push_warning("AircraftLights: anchor '%s' for %s not found in the model" % [ride_name, kind])
			_add_fixture(kind, anchor as Node3D)
		# "<kind>_pos": "x, y, z" in model-root units, for models without a lens mesh
		var pos: Array = cfg.get_float_array("lights", kind + "_pos", [])
		if pos.size() == 3 and model_root is Node3D:
			var n := Node3D.new()
			n.name = "Light_" + kind
			model_root.add_child(n)
			n.position = Vector3(float(pos[0]), float(pos[1]), float(pos[2]))
			_add_fixture(kind, n)
	_apply_states()


## Reparent a lens mesh under the animated part it belongs to. The A-10 glTF
## animates the gear per mesh and its lens meshes have no tracks, so they
## stay in the file pose (gear up, in the bay) while the strut swings down.
## The clip pose in which the lens lies on the part is the one it was
## modelled in: sample the part's tracks, take the pose with the lens closest
## to the part's bounds, and keep that offset as the lens's new local
## transform under the part.
func _ride_on(lens: Node3D, part: Node3D, model_root: Node) -> void:
	var lens_g: Transform3D = lens.global_transform
	var best: Transform3D = part.global_transform.affine_inverse() * lens_g
	var ap: AnimationPlayer = null
	for n in model_root.find_children("*", "AnimationPlayer", true, false):
		ap = n
		break
	var anim: Animation = null
	if ap != null:
		var clip: String = ap.current_animation if ap.current_animation != "" else ap.assigned_animation
		if clip == "" and ap.get_animation_list().size() > 0:
			clip = ap.get_animation_list()[0]
		if clip != "":
			anim = ap.get_animation(clip)
	if anim != null:
		var pos_t := -1
		var rot_t := -1
		var scl_t := -1
		for i in range(anim.get_track_count()):
			var path: NodePath = anim.track_get_path(i)
			if path.get_name_count() == 0 or path.get_name(path.get_name_count() - 1) != part.name:
				continue
			match anim.track_get_type(i):
				Animation.TYPE_POSITION_3D: pos_t = i
				Animation.TYPE_ROTATION_3D: rot_t = i
				Animation.TYPE_SCALE_3D: scl_t = i
		if pos_t >= 0 or rot_t >= 0:
			var parent_g: Transform3D = (part.get_parent() as Node3D).global_transform if part.get_parent() is Node3D else Transform3D.IDENTITY
			var bounds: AABB = (part as MeshInstance3D).get_aabb() if part is MeshInstance3D and (part as MeshInstance3D).mesh != null else AABB()
			var best_d := INF
			for k in range(21):
				var t: float = anim.length * float(k) / 20.0
				var p: Vector3 = anim.position_track_interpolate(pos_t, t) if pos_t >= 0 else part.position
				var q: Quaternion = anim.rotation_track_interpolate(rot_t, t) if rot_t >= 0 else part.quaternion
				var sc: Vector3 = anim.scale_track_interpolate(scl_t, t) if scl_t >= 0 else part.scale
				var local := Transform3D(Basis(q) * Basis.from_scale(sc), p)
				var offset: Transform3D = (parent_g * local).affine_inverse() * lens_g
				var d: float = _aabb_distance(bounds, offset.origin)
				if d < best_d:
					best_d = d
					best = offset
	lens.get_parent().remove_child(lens)
	part.add_child(lens)
	lens.transform = best


static func _aabb_distance(box: AABB, p: Vector3) -> float:
	var lo: Vector3 = box.position
	var hi: Vector3 = box.end
	var c := Vector3(clampf(p.x, lo.x, hi.x), clampf(p.y, lo.y, hi.y), clampf(p.z, lo.z, hi.z))
	return c.distance_to(p)


func clear() -> void:
	for f in _fixtures:
		if is_instance_valid(f.sprite):
			f.sprite.queue_free()
		if f.spot != null and is_instance_valid(f.spot):
			f.spot.queue_free()
	_fixtures.clear()


func _add_fixture(kind: String, anchor: Node3D) -> void:
	var centre := Vector3.ZERO
	if anchor is MeshInstance3D and (anchor as MeshInstance3D).mesh != null:
		centre = (anchor as MeshInstance3D).get_aabb().get_center()
	if _ac != null and anchor.is_inside_tree():
		# outward direction in the aircraft frame -> anchor-local offset
		var ac_inv: Transform3D = _ac.global_transform.affine_inverse()
		var p_ac: Vector3 = ac_inv * (anchor.global_transform * centre)
		var out := Vector3.ZERO
		match kind:
			"nav_left": out = Vector3(1, 0, 0)
			"nav_right": out = Vector3(-1, 0, 0)
			"nav_tail": out = Vector3(0, 0, -1)
			"beacon": out = Vector3(0, 1, 0)
			"landing", "taxi": out = Vector3(0, 0, 1)
			"strobe": out = Vector3(signf(p_ac.x), 0, 0) if absf(p_ac.x) > 1.0 else Vector3(0, 0, -1)
		var world_dir: Vector3 = (_ac.global_transform.basis * out)
		if world_dir.length_squared() > 1e-9:
			var local_dir: Vector3 = anchor.global_transform.basis.inverse() * world_dir
			if local_dir.length_squared() > 1e-12:
				centre += local_dir.normalized() * OUT_OFFSET
	var mesh := ArrayMesh.new()
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([centre])
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_POINTS, arrays)
	var mat := ShaderMaterial.new()
	mat.shader = _shader
	mat.set_shader_parameter("colour", Vector3(COLOURS[kind].r, COLOURS[kind].g, COLOURS[kind].b))
	mat.set_shader_parameter("size_scale", float(SIZES[kind]))
	mat.set_shader_parameter("on", 0.0)
	mat.set_shader_parameter("pull_m", PULL_FACTOR * anchor.global_transform.basis.get_scale().x if anchor.is_inside_tree() else 0.01)
	mesh.surface_set_material(0, mat)
	var sprite := MeshInstance3D.new()
	sprite.name = "LightSprite_" + kind
	sprite.mesh = mesh
	sprite.custom_aabb = AABB(centre - Vector3.ONE * 0.5, Vector3.ONE)
	sprite.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	anchor.add_child(sprite)
	# lit / unlit mesh pair
	var on_mesh: Node3D = null
	var off_mesh: Node3D = null
	if _pair_meshes and anchor.name.ends_with("-on") and anchor.get_parent() != null:
		var sib := anchor.get_parent().get_node_or_null(anchor.name.left(anchor.name.length() - 3) + "-off")
		if sib is Node3D:
			on_mesh = anchor
			off_mesh = sib
	var spot: SpotLight3D = null
	if kind == "landing" or kind == "taxi":
		spot = SpotLight3D.new()
		spot.name = "Spot_" + kind
		spot.light_color = COLOURS[kind]
		spot.light_energy = _spot_energy if kind == "landing" else _spot_energy * 0.5
		spot.spot_range = _landing_range_m if kind == "landing" else _taxi_range_m
		spot.spot_angle = 14.0 if kind == "landing" else 32.0
		spot.spot_attenuation = 0.8
		spot.shadow_enabled = false
		spot.visible = false
		add_child(spot)
	_fixtures.append({"kind": kind, "sprite": sprite, "mat": mat, "on_mesh": on_mesh, "off_mesh": off_mesh, "spot": spot,
		"anchor": anchor, "energy": spot.light_energy if spot != null else 0.0})


## Landing / taxi lights master switch (L).
func set_landing_lights(on: bool) -> void:
	_landing_on = on
	_apply_states()


func landing_lights_on() -> bool:
	return _landing_on


func toggle_landing_lights() -> bool:
	set_landing_lights(not _landing_on)
	return _landing_on


func fixture_count(kind: String = "") -> int:
	if kind.is_empty():
		return _fixtures.size()
	var n := 0
	for f in _fixtures:
		if f.kind == kind:
			n += 1
	return n


## Whether a fixture class is currently lit (strobes and beacon: at this instant).
func is_lit(kind: String) -> bool:
	for f in _fixtures:
		if f.kind == kind:
			return float(f.mat.get_shader_parameter("on")) > 0.01
	return false


## Per frame. [param airborne]: no weight on wheels; [param gear_down]: gear
## fully or partly down; [param night] 0..1 (host rule: dusk or low
## visibility); [param forward]/[param down]: aircraft axes in world space
## for aiming the landing spot.
func update(delta: float, airborne: bool, gear_down: bool, night: float, forward: Vector3, down: Vector3) -> void:
	_time += delta
	_airborne = airborne
	_gear_down = gear_down
	_night = clampf(night, 0.0, 1.0)
	_forward = forward
	_down = down
	_apply_states()


func _apply_states() -> void:
	var bright: float = (NIGHT_FLOOR + (1.0 - NIGHT_FLOOR) * _night) if powered else 0.0
	var strobe_phase: float = fmod(_time, STROBE_PERIOD_S)
	var strobe_on: bool = _airborne and (strobe_phase < STROBE_FLASH_S or (strobe_phase > 0.16 and strobe_phase < 0.16 + STROBE_FLASH_S))
	var beacon_on: bool = fmod(_time, BEACON_PERIOD_S) < BEACON_FLASH_S
	if debug_all_on:
		strobe_on = true
		beacon_on = true
	var landing_on: bool = _landing_on and _gear_down
	# the landing spot is aimed along the aircraft, pitched down
	var aim: Vector3 = (_forward * cos(deg_to_rad(_landing_pitch_deg)) + _down * sin(deg_to_rad(_landing_pitch_deg))).normalized()
	var taxi_aim: Vector3 = (_forward * cos(deg_to_rad(_landing_pitch_deg + 6.0)) + _down * sin(deg_to_rad(_landing_pitch_deg + 6.0))).normalized()
	for f in _fixtures:
		var on := 0.0
		match f.kind:
			"nav_left", "nav_right", "nav_tail":
				on = bright if nav_enabled else 0.0
			"strobe":
				on = 1.0 if strobe_on and strobe_enabled and powered else 0.0
			"beacon":
				on = bright if beacon_on and beacon_enabled else 0.0
			"landing", "taxi":
				on = 1.0 if landing_on and powered and (f.kind == "landing" or taxi_enabled) else 0.0
		f.mat.set_shader_parameter("on", on)
		if f.on_mesh != null and is_instance_valid(f.on_mesh):
			f.on_mesh.visible = on > 0.01
			f.off_mesh.visible = on <= 0.01
		if f.spot != null and is_instance_valid(f.spot):
			f.spot.visible = on > 0.01
			# a landing light barely shows on sunlit ground
			f.spot.light_energy = float(f.energy) * (0.08 + 0.92 * _night)
			if f.spot.visible and is_instance_valid(f.anchor) and f.anchor.is_inside_tree():
				var origin: Vector3 = f.anchor.global_transform * f.sprite.mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0]
				var dir: Vector3 = aim if f.kind == "landing" else taxi_aim
				# spot lights shine along -Z
				var z: Vector3 = -dir
				var x: Vector3 = (-_down).cross(z)
				if x.length_squared() < 1e-6:
					x = _forward.cross(z)
				x = x.normalized()
				var y: Vector3 = z.cross(x).normalized()
				f.spot.global_transform = Transform3D(Basis(x, y, z), origin)
