extends Node
## Headless consistency probe for the globe suppression mask (design §8.1
## iteration 4b): every tile the tile manager reports as COVERED must be
## backed by a visible, meshed S3DTile/chunk node — otherwise the coarse
## globe is discarded over nothing and a hole to space appears. Streams the
## Norway build (see smoke_norway_terrain.gd) at the position of the
## 2026-09-18 hole report south of Oslo, then slews south while checking
## every frame. Reports phantom cells with what the node tree says about them.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot res://tests/smoke_mask_consistency.tscn

const START_E := 2789836.0
const START_N := 2596690.0
const ALT := 19558.0
const SETTLE_FRAMES := 900
const MOVE_FRAMES := 1500
const STEP_M := 150.0       # per frame while moving (~9 km/s at 60 fps, slew-like)
const CHECK_RADIUS := 45    # tiles around the camera to audit

var app: Node = null
var tm: Node = null
var cam: Camera3D = null
var frame := 0
var moving := false
var phantom_streak := {}    # "ei,ni" -> consecutive frames phantom
var worst_streak := 0
var reports := 0
var _printed_chunk_info := false

func _ready() -> void:
	app = load("res://node_3d.tscn").instantiate()
	add_child(app)
	cam = Camera3D.new()
	add_child(cam)
	cam.make_current()

func _process(_d: float) -> void:
	frame += 1
	if frame == 2:
		var s3d: Node = app.get_node_or_null("Scenery3D")
		for c in s3d.get_children():
			if c.has_method("update_tiles"):
				tm = c
		if tm == null:
			printerr("FAIL: no tile manager"); get_tree().quit(1); return
		app._world.set_streaming(true)
		print("[probe] load_radius=%d far_radius=%d chunk_size=%d unload_margin=%d" % [tm.get_load_radius(), tm.get_far_radius(), tm.get_chunk_size(), tm.get_unload_margin()])
		_move_to(START_E, START_N)
		return
	if tm == null:
		return
	if frame == SETTLE_FRAMES:
		print("[probe] settled: active=%d" % tm.get_active_tile_count())
		var hist := {}
		for c in tm.get_children():
			if c is MeshInstance3D and c.has_method("get_tile_x") and c.get_tile_size() == 1024:
				var d: int = maxi(absi(c.get_tile_x() - tm.get_cam_tile_ei()), absi(c.get_tile_z() - tm.get_cam_tile_ni()))
				hist[d] = int(hist.get(d, 0)) + 1
		var ks := hist.keys(); ks.sort()
		var parts := []
		for d in ks: parts.append("%d:%d" % [d, hist[d]])
		print("[probe] individual tiles by Chebyshev distance: " + " ".join(parts))
		moving = true
	if moving:
		cam.global_position.z -= STEP_M  # south
	if frame > 60:
		_audit()
	if frame >= SETTLE_FRAMES + MOVE_FRAMES:
		print("[probe] done: worst phantom streak %d frames, %d reports" % [worst_streak, reports])
		get_tree().quit(1 if worst_streak > 3 else 0)

func _move_to(e: float, n: float) -> void:
	cam.global_position = Vector3(tm.get_origin_east() - e, ALT, n - tm.get_origin_north())

func _audit() -> void:
	var r: int = tm.get_coverage_mask_radius()
	var n := 2 * r + 1
	var mask: PackedByteArray = tm.get_coverage_mask()
	if mask.size() != n * n:
		return
	var ce: int = tm.get_cam_tile_ei()
	var cn: int = tm.get_cam_tile_ni()
	# Geometry set from the node tree: tile index -> node state string.
	var geom := {}
	for c in tm.get_children():
		if not (c is MeshInstance3D) or not c.has_method("get_tile_x"):
			continue
		var span: int = maxi(1, int(round(float(c.get_tile_size()) / 1024.0)))
		# is_mesh_ready() is false for chunks by design (heightmap freed after
		# meshing), so readiness = a visible node with a mesh surface whose
		# AABB spans the node's full footprint.
		var full_fp: bool = c.mesh != null and c.mesh.get_surface_count() > 0 \
				and c.get_aabb().size.x >= float(c.get_tile_size()) - 1.0 and c.get_aabb().size.z >= float(c.get_tile_size()) - 1.0
		var ok: bool = c.visible and full_fp
		var state := "ok" if ok else ("hidden" if not c.visible else ("nomesh" if c.mesh == null else ("nosurf" if c.mesh.get_surface_count() == 0 else "partial_aabb=" + str(c.get_aabb()))))
		# Chunk nodes carry the CHUNK-GRID index in tile_x/tile_z (request.ei = cg_ei);
		# derive the tile footprint from the node's world position instead.
		var base_e: int = int(round((tm.get_origin_east() - c.position.x) / 1024.0)) - span
		var base_n: int = int(round((c.position.z + tm.get_origin_north()) / 1024.0))
		if span == 1:
			base_e = c.get_tile_x()
			base_n = c.get_tile_z()
		if not _printed_chunk_info and span > 1:
			_printed_chunk_info = true
			print("[probe] chunk node sample: tile_x=%d tile_z=%d pos=%s aabb=%s -> base tiles (%d,%d) span %d" % [c.get_tile_x(), c.get_tile_z(), str(c.position), str(c.get_aabb()), base_e, base_n, span])
		for dy in range(span):
			for dx in range(span):
				var k := "%d,%d" % [base_e + dx, base_n + dy]
				if not geom.has(k) or state == "ok":
					geom[k] = state
					if state != "ok":
						geom[k + "#"] = "%s vis=%s ready=%s surfaces=%d pos=%s" % [c.name, str(c.visible), str(c.is_mesh_ready()), (c.mesh.get_surface_count() if c.mesh != null else -1), str(c.position)]
	var seen := {}
	for dn in range(-CHECK_RADIUS, CHECK_RADIUS + 1):
		for de in range(-CHECK_RADIUS, CHECK_RADIUS + 1):
			var ie := de + r
			var inn := dn + r
			if mask[inn * n + ie] == 0:
				continue
			var k := "%d,%d" % [ce + de, cn + dn]
			var st: String = geom.get(k, "none")
			if st == "ok":
				continue
			seen[k] = true
			phantom_streak[k] = int(phantom_streak.get(k, 0)) + 1
			var s: int = phantom_streak[k]
			worst_streak = maxi(worst_streak, s)
			if s == 4 and reports < 40:
				reports += 1
				print("[probe] frame %d PHANTOM tile %s (%d tiles from cam) node=%s %s moving=%s" % [frame, k, maxi(absi(de), absi(dn)), st, str(geom.get(k + "#", "")), str(moving)])
	for k in phantom_streak.keys():
		if not seen.has(k):
			phantom_streak.erase(k)
