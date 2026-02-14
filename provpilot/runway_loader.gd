extends Node3D
## RunwayLoader — Procedural runway rendering from airports.json
##
## Reads airport/runway data from POILoader and creates flat textured quads
## for each runway surface.  Runways are loaded/unloaded based on aircraft
## proximity, and support floating-origin rebasing.

class_name RunwayLoader

# ── Configuration ──────────────────────────────────────────────────────

## The node to track for position (usually the aircraft)
@export var aircraft_node: Node3D

## Distance (m) at which to spawn runway meshes
@export var load_distance: float = 25000.0

## Distance (m) at which to despawn runway meshes
@export var unload_distance: float = 30000.0

## Height offset above terrain to avoid z-fighting (meters)
@export var y_offset: float = 0.1

## Enable debug output
@export var debug_mode: bool = false

## Enable/disable runway loading
@export var enabled: bool = true


# ── Internal State ─────────────────────────────────────────────────────

var _main_node: Node = null
var _poi_loader: Node = null
var _terrain3d_node: Node = null

# airport_key → Node3D (parent holding all runway meshes for that airport)
var _loaded_airports: Dictionary = {}

# Pre-built materials (lazily created once)
var _mat_asphalt: StandardMaterial3D = null
var _mat_grass: StandardMaterial3D = null
var _mat_helipad: StandardMaterial3D = null

var airports_loaded: int = 0


# ── Lifecycle ──────────────────────────────────────────────────────────

func _ready() -> void:
	if aircraft_node == null:
		aircraft_node = get_parent().get_node_or_null("AC")
		if aircraft_node == null:
			push_warning("RunwayLoader: No aircraft node set.")

	_main_node = get_parent()
	_poi_loader = get_parent().get_node_or_null("POILoader")
	_terrain3d_node = get_parent().get_node_or_null("Terrain3D")

	if _poi_loader == null or _poi_loader.airports.size() == 0:
		push_warning("RunwayLoader: No POILoader or no airports — disabled.")
		enabled = false
		return

	_create_materials()
	print("RunwayLoader: Ready — %d airports available" % _poi_loader.airports.size())


func _process(_delta: float) -> void:
	if not enabled or aircraft_node == null or _poi_loader == null:
		return

	var ac_pos := aircraft_node.global_position
	var load_sq := load_distance * load_distance
	var unload_sq := unload_distance * unload_distance

	for ap in _poi_loader.airports:
		var key := _airport_key(ap)
		var center_godot := _swiss_to_godot(
			float(ap.get("center_e", 0)),
			float(ap.get("center_n", 0))
		)

		var delta := ac_pos - center_godot
		delta.y = 0.0
		var dist_sq := delta.length_squared()

		var is_loaded := _loaded_airports.has(key)

		if dist_sq < load_sq and not is_loaded:
			_spawn_airport(ap, key)
		elif dist_sq > unload_sq and is_loaded:
			_despawn_airport(key)


# ── Spawning ───────────────────────────────────────────────────────────

func _spawn_airport(ap: Dictionary, key: String) -> void:
	var parent := Node3D.new()
	parent.name = "Airport_" + key
	add_child(parent)

	var ap_type: String = ap.get("type", "")

	if ap_type == "heliport":
		_spawn_helipad(ap, parent)
	else:
		var runways: Array = ap.get("runways", [])
		for rwy in runways:
			_spawn_runway(rwy, parent)

	_loaded_airports[key] = parent
	airports_loaded += 1

	if debug_mode:
		print("RunwayLoader: Spawned %s (%s)" % [ap.get("name", key), ap_type])


func _spawn_runway(rwy: Dictionary, parent: Node3D) -> void:
	var designator: String = rwy.get("designator", "??")
	var heading_deg: float = float(rwy.get("heading_deg", 0))
	var length_m: float = float(rwy.get("length_m", 500))
	var width_m: float = float(rwy.get("width_m", 18))
	var surface: String = rwy.get("surface", "asphalt")

	# Get threshold positions in Godot space (with fallback computation)
	var thresholds := _get_or_compute_thresholds(rwy)
	var p0: Vector3 = thresholds[0]
	var p1: Vector3 = thresholds[1]

	# Direction vector along runway (horizontal)
	var dir := p1 - p0
	dir.y = 0.0
	dir = dir.normalized()

	# Perpendicular vector (for runway width)
	var perp := Vector3(dir.z, 0.0, -dir.x)

	# Segment count — ~50 m segments for terrain conformance
	var segment_length := 50.0
	var num_segments := maxi(int(ceil(length_m / segment_length)), 4)

	# Sample heights along the centreline
	var heights: Array = []
	for i in range(num_segments + 1):
		var t := float(i) / float(num_segments)
		var sample_pos := p0.lerp(p1, t)
		sample_pos.y = 0.0
		heights.append(_get_terrain_height(sample_pos) + y_offset)

	# Build terrain-following mesh with SurfaceTool
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var half_w := width_m * 0.5

	for i in range(num_segments + 1):
		var t := float(i) / float(num_segments)
		var center_xz := p0.lerp(p1, t)
		var h: float = heights[i]

		var left := center_xz + perp * half_w
		var right := center_xz - perp * half_w
		left.y = h
		right.y = h

		st.set_normal(Vector3.UP)
		st.set_uv(Vector2(0.0, t))
		st.add_vertex(left)

		st.set_normal(Vector3.UP)
		st.set_uv(Vector2(1.0, t))
		st.add_vertex(right)

	for i in range(num_segments):
		var base := i * 2
		st.add_index(base)
		st.add_index(base + 1)
		st.add_index(base + 2)
		st.add_index(base + 1)
		st.add_index(base + 3)
		st.add_index(base + 2)

	var mesh := st.commit()
	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = "Rwy_" + designator
	mesh_instance.mesh = mesh

	if surface == "grass":
		mesh_instance.material_override = _mat_grass
	else:
		mesh_instance.material_override = _mat_asphalt

	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	parent.add_child(mesh_instance)

	# Add terrain-following markings
	_add_centerline_terrain(parent, p0, p1, heights, num_segments, heading_deg, length_m)
	_add_threshold_marks_terrain(parent, p0, p1, perp, heights, num_segments, width_m, heading_deg)

	if debug_mode:
		print("  Runway %s: %.0fm × %.0fm, hdg %.0f°, surface=%s, %d segments" % [
			designator, length_m, width_m, heading_deg, surface, num_segments])


func _spawn_helipad(ap: Dictionary, parent: Node3D) -> void:
	var pad_data: Dictionary = ap.get("pad", {})
	var pad_e: float = float(pad_data.get("e", ap.get("center_e", 0)))
	var pad_n: float = float(pad_data.get("n", ap.get("center_n", 0)))
	var center_godot := _swiss_to_godot(pad_e, pad_n)

	# Helipad: 30m × 30m square
	var size := 30.0

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = "Helipad"

	var quad := PlaneMesh.new()
	quad.size = Vector2(size, size)
	mesh_instance.mesh = quad
	mesh_instance.material_override = _mat_helipad
	center_godot.y = _get_terrain_height(center_godot) + y_offset
	mesh_instance.position = center_godot
	mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

	parent.add_child(mesh_instance)

	# Add "H" marking
	_add_helipad_h(mesh_instance, size)


# ── Visual Markings ────────────────────────────────────────────────────

func _add_centerline_terrain(parent: Node3D, p0: Vector3, p1: Vector3,
		heights: Array, num_segments: int, heading_deg: float,
		length_m: float) -> void:
	## Dashed center line that follows terrain height
	var dash_length := 30.0
	var gap_length := 20.0
	var stripe_width := 0.5
	var total := dash_length + gap_length

	# Leave 10 % at each end for threshold area
	var start_t := 0.1
	var end_t := 0.9
	var usable_length := length_m * (end_t - start_t)
	var count := int(usable_length / total)

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(1.0, 1.0, 1.0, 0.9)
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.render_priority = 1

	for i in range(count):
		var dash_center_dist := start_t * length_m + i * total + dash_length * 0.5
		var t := dash_center_dist / length_m
		var pos := p0.lerp(p1, t)
		pos.y = _interpolate_height(heights, num_segments, t) + 0.02

		var mi := MeshInstance3D.new()
		mi.name = "CL_%d" % i
		var q := PlaneMesh.new()
		q.size = Vector2(stripe_width, dash_length)
		mi.mesh = q
		mi.material_override = mat
		mi.position = pos
		mi.rotation.y = -deg_to_rad(heading_deg)
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		parent.add_child(mi)


func _add_threshold_marks_terrain(parent: Node3D, p0: Vector3, p1: Vector3,
		perp: Vector3, heights: Array, num_segments: int,
		width_m: float, heading_deg: float) -> void:
	## Threshold bars at both ends, following terrain height
	var bar_length := 20.0
	var bar_width := 1.5
	var num_bars := clampi(int(width_m / 4.0), 3, 12)
	var spacing := width_m * 0.8 / float(num_bars)

	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(1.0, 1.0, 1.0, 0.85)
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.render_priority = 1

	# Place at both ends: t = 0.03 and t = 0.97
	var end_ts: Array = [0.03, 0.97]
	for end_idx in range(2):
		var t: float = end_ts[end_idx]
		var center := p0.lerp(p1, t)
		center.y = _interpolate_height(heights, num_segments, t) + 0.02

		var start_offset := -(num_bars - 1) * 0.5 * spacing
		for i in range(num_bars):
			var offset: float = start_offset + i * spacing
			var bar_pos := center + perp * offset
			bar_pos.y = center.y

			var mi := MeshInstance3D.new()
			mi.name = "Thr_%d_%d" % [end_idx, i]
			var q := PlaneMesh.new()
			q.size = Vector2(bar_width, bar_length)
			mi.mesh = q
			mi.material_override = mat
			mi.position = bar_pos
			mi.rotation.y = -deg_to_rad(heading_deg)
			mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			parent.add_child(mi)


func _add_helipad_h(pad_mesh: MeshInstance3D, size: float) -> void:
	## Draw an "H" using three bars inside the helipad circle
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(1.0, 1.0, 1.0, 0.9)
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.render_priority = 1

	var bar_w := size * 0.08
	var leg_h := size * 0.4
	var cross_w := size * 0.2

	# Left vertical
	var ml := MeshInstance3D.new()
	ml.name = "H_L"
	var ql := PlaneMesh.new()
	ql.size = Vector2(bar_w, leg_h)
	ml.mesh = ql
	ml.material_override = mat
	ml.position = Vector3(-cross_w * 0.5, 0.02, 0.0)
	ml.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	pad_mesh.add_child(ml)

	# Right vertical
	var mr := MeshInstance3D.new()
	mr.name = "H_R"
	var qr := PlaneMesh.new()
	qr.size = Vector2(bar_w, leg_h)
	mr.mesh = qr
	mr.material_override = mat
	mr.position = Vector3(cross_w * 0.5, 0.02, 0.0)
	mr.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	pad_mesh.add_child(mr)

	# Cross bar
	var mc := MeshInstance3D.new()
	mc.name = "H_C"
	var qc := PlaneMesh.new()
	qc.size = Vector2(cross_w, bar_w)
	mc.mesh = qc
	mc.material_override = mat
	mc.position = Vector3(0.0, 0.02, 0.0)
	mc.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	pad_mesh.add_child(mc)

	# Circle outline around helipad (use a thin ring of quads)
	_add_helipad_circle(pad_mesh, size * 0.45, mat)


func _add_helipad_circle(pad_mesh: MeshInstance3D, radius: float, mat: Material) -> void:
	## Approximate a circle with thin rectangular segments
	var segments := 24
	var bar_w := 0.6
	var angle_step := TAU / float(segments)
	var seg_length := 2.0 * radius * sin(angle_step * 0.5)

	for i in range(segments):
		var angle := i * angle_step
		var cx := radius * cos(angle + angle_step * 0.5)
		var cz := radius * sin(angle + angle_step * 0.5)

		var mi := MeshInstance3D.new()
		mi.name = "Circle_%d" % i
		var q := PlaneMesh.new()
		q.size = Vector2(bar_w, seg_length)
		mi.mesh = q
		mi.material_override = mat
		mi.position = Vector3(cx, 0.02, cz)
		mi.rotation.y = -(angle + angle_step * 0.5) + PI * 0.5
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		pad_mesh.add_child(mi)


# ── Despawning ─────────────────────────────────────────────────────────

func _despawn_airport(key: String) -> void:
	var node = _loaded_airports.get(key)
	if node and is_instance_valid(node):
		node.queue_free()
	_loaded_airports.erase(key)
	airports_loaded -= 1

	if debug_mode:
		print("RunwayLoader: Despawned %s" % key)


# ── Coordinate Helpers ─────────────────────────────────────────────────

func _get_terrain_center() -> Vector2:
	if _main_node and "TERRAIN_CENTER_E" in _main_node:
		return Vector2(_main_node.TERRAIN_CENTER_E, _main_node.TERRAIN_CENTER_N)
	return Vector2(2682880.0, 1283072.0)


func _swiss_to_godot(swiss_e: float, swiss_n: float) -> Vector3:
	var center := _get_terrain_center()
	return Vector3(
		center.x - swiss_e,    # X = West
		0.0,
		swiss_n - center.y     # Z = North
	)


func _get_terrain_height(pos: Vector3) -> float:
	## Query Terrain3D for the ground height at the given XZ position.
	if _terrain3d_node:
		var terrain_data = _terrain3d_node.get("data")
		if terrain_data and terrain_data.has_method("get_height"):
			var h = terrain_data.get_height(Vector3(pos.x, 0.0, pos.z))
			if not is_nan(h):
				return h
	return 0.0


func _threshold_godot_positions(rwy: Dictionary) -> Array:
	## Return an array of Godot-space Vector3 for both thresholds
	var result: Array = []
	var designator: String = rwy.get("designator", "")
	var parts := designator.split("/")
	if parts.size() == 2:
		for p in parts:
			var key := "threshold_" + p
			var thr: Dictionary = rwy.get(key, {})
			if thr.size() > 0:
				result.append(_swiss_to_godot(float(thr.get("e", 0)), float(thr.get("n", 0))))
	return result


func _runway_center_godot(rwy: Dictionary) -> Vector3:
	## Compute runway center from thresholds, falling back to midpoint estimate
	var designator: String = rwy.get("designator", "")
	var parts := designator.split("/")

	var has_thresholds := false
	var e1 := 0.0
	var n1 := 0.0
	var e2 := 0.0
	var n2 := 0.0

	if parts.size() == 2:
		var key1 := "threshold_" + parts[0]
		var key2 := "threshold_" + parts[1]
		var t1: Dictionary = rwy.get(key1, {})
		var t2: Dictionary = rwy.get(key2, {})
		if t1.size() > 0 and t2.size() > 0:
			e1 = float(t1.get("e", 0))
			n1 = float(t1.get("n", 0))
			e2 = float(t2.get("e", 0))
			n2 = float(t2.get("n", 0))
			has_thresholds = true

	if has_thresholds:
		var mid_e := (e1 + e2) * 0.5
		var mid_n := (n1 + n2) * 0.5
		return _swiss_to_godot(mid_e, mid_n)

	# Fallback: shouldn't happen with well-formed data, but just in case
	push_warning("RunwayLoader: No thresholds for runway %s" % designator)
	return Vector3.ZERO


func _get_or_compute_thresholds(rwy: Dictionary) -> Array:
	## Return [p0, p1] in Godot space.  Uses explicit data if present,
	## otherwise computes from center + heading + length.
	var thresholds := _threshold_godot_positions(rwy)
	if thresholds.size() == 2:
		return thresholds

	var center := _runway_center_godot(rwy)
	var heading_deg: float = float(rwy.get("heading_deg", 0))
	var length_m: float = float(rwy.get("length_m", 500))
	var half_len := length_m * 0.5
	var heading_rad := deg_to_rad(heading_deg)
	# Our coord system: +X = West, +Z = North
	var dir := Vector3(-sin(heading_rad), 0.0, cos(heading_rad))
	return [
		center - dir * half_len,
		center + dir * half_len
	]


func _interpolate_height(heights: Array, num_segments: int, t: float) -> float:
	## Linearly interpolate the sampled height array at parameter t ∈ [0, 1]
	var ft := t * float(num_segments)
	var idx := int(ft)
	if idx >= num_segments:
		return heights[num_segments]
	var frac := ft - float(idx)
	var h0: float = heights[idx]
	var h1: float = heights[idx + 1]
	return h0 * (1.0 - frac) + h1 * frac


# ── Materials ──────────────────────────────────────────────────────────

func _create_materials() -> void:
	# Concrete/asphalt runway — light concrete gray, unshaded so colour
	# is independent of scene lighting (avoids pitch-black look)
	_mat_asphalt = StandardMaterial3D.new()
	_mat_asphalt.albedo_color = Color(0.55, 0.53, 0.50)
	_mat_asphalt.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_mat_asphalt.cull_mode = BaseMaterial3D.CULL_DISABLED

	# Grass strip — muted green, unshaded
	_mat_grass = StandardMaterial3D.new()
	_mat_grass.albedo_color = Color(0.35, 0.50, 0.25)
	_mat_grass.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_mat_grass.cull_mode = BaseMaterial3D.CULL_DISABLED

	# Helipad — concrete gray, unshaded
	_mat_helipad = StandardMaterial3D.new()
	_mat_helipad.albedo_color = Color(0.50, 0.48, 0.46)
	_mat_helipad.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_mat_helipad.cull_mode = BaseMaterial3D.CULL_DISABLED


# ── Utility ────────────────────────────────────────────────────────────

func _airport_key(ap: Dictionary) -> String:
	var icao: String = ap.get("icao", "")
	if icao != "":
		return icao
	return ap.get("name", "unknown").to_lower().replace(" ", "_")


# ── Public API ─────────────────────────────────────────────────────────

func shift_all_runways(offset: Vector3) -> void:
	var count := 0
	for key in _loaded_airports:
		var instance = _loaded_airports[key]
		if instance != null and is_instance_valid(instance):
			instance.position += offset
			count += 1
	print("RunwayLoader: Shifted %d airports by offset (%.1f, %.1f)" % [count, offset.x, offset.z])


func unload_all() -> void:
	for key in _loaded_airports.keys():
		_despawn_airport(key)


func get_stats() -> Dictionary:
	return {
		"airports_loaded": airports_loaded,
		"total_available": _poi_loader.airports.size() if _poi_loader else 0
	}
