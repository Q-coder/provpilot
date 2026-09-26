extends Node
## Headless smoke test for per-path pixels_per_tile terrain streaming (P0,
## docs/WORLD_SCENERY_DESIGN.md §3.3). Instantiates the full app — which
## applies the scenery library from provpilot.cfg, including the Norway build
## at /Volumes/Data1/provpilot/build/norway/Norway (34 px/tile ≈ 31 m/px)
## alongside CH/BW (512 px/tile) — then drives the Scenery3D tile manager
## with a camera:
##   Phase 1 (Oslo):  Norway's 34 px tiles must stream (active tiles > 0).
##   Phase 2 (LSZH):  CH/BW 512 px tiles must still stream (regression, R3).
## Watch the log for "S3DTileManager: per-path tile_px=34" — that line proves
## the manifest override was picked up from Norway/terrain/manifest.json.
## Run:
##   godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot \
##       res://tests/smoke_norway_terrain.tscn
## Exits 0 on success, 1 on any failure.

const TIMEOUT_FRAMES := 9000  # ~150 s at 60 fps (headless loading can be slow)
const PHASE_FRAMES := 700     # frames per phase (worker loads are budget-limited)

# LV95 positions (WGS84 → EPSG:2056 via pyproj, 2026-09-16).
const OSLO_E := 2805578.0  # near ENGM, inside the Norway bake bbox
const OSLO_N := 2666593.0
const LSZH_E := 2683732.0
const LSZH_N := 1257695.0

var app: Node = null
var tm: Node = null  # S3DTileManager
var edb = null      # S3DElevationDB (RefCounted — functional ground-truth probe)
var cam: Camera3D = null
var frame := 0
var phase := "boot"
var phase_frame := 0
var fails: Array[String] = []


func _ready() -> void:
	app = load("res://node_3d.tscn").instantiate()
	add_child(app)
	# The manager's _process needs a current camera to start its worker
	# threads and stream; the welcome screen also starts with streaming
	# disabled, so both are handled here.
	cam = Camera3D.new()
	add_child(cam)
	cam.make_current()


func _process(_delta: float) -> void:
	frame += 1
	if frame == 2:
		_setup()
	elif phase != "done":
		_step()
	if frame >= TIMEOUT_FRAMES and phase != "done":
		fails.append("timeout in phase %s" % phase)
		_finish(1)


func _setup() -> void:
	var s3d: Node = app.get_node_or_null("Scenery3D")
	if s3d == null:
		fails.append("Scenery3D node not found")
		_finish(1)
		return
	for c in s3d.get_children():
		if c.has_method("update_tiles"):
			tm = c
			break
	if tm == null:
		fails.append("S3DTileManager child not found under Scenery3D")
		_finish(1)
		return
	if s3d.has_method("get_elevation_db"):
		edb = s3d.get_elevation_db()

	var paths: PackedStringArray = tm.get_data_paths()
	print("[smoke] terrain data_paths (", paths.size(), "):")
	for p in paths:
		print("   ", p)
	if not _any_contains(paths, "/norway/norway/terrain"):
		fails.append("Norway terrain path missing from data_paths — check [scenery] scenery_areas in provpilot.cfg")

	app._world.set_streaming(true)
	phase = "oslo"
	phase_frame = 0
	_move_to(OSLO_E, OSLO_N)


func _any_contains(paths: PackedStringArray, needle: String) -> bool:
	for p in paths:
		if str(p).to_lower().contains(needle):
			return true
	return false


func _move_to(e: float, n: float) -> void:
	# Godot world ↔ LV95 (origin from the manager; +X = West).
	var oe: float = tm.get_origin_east()
	var on: float = tm.get_origin_north()
	cam.global_position = Vector3(oe - e, 500.0, n - on)


## Functional probe: the elevation DB must hold real ground under the camera
## (proves a tile with correct content was loaded at this position — a file
## that exists but is mis-named/mis-baked would leave this NaN or wrong).
func _probe_elevation(label: String, min_m: float, max_m: float) -> void:
	if edb == null:
		fails.append("%s: elevation DB unavailable" % label)
		return
	var elev: float = edb.get_elevation(cam.global_position.x, cam.global_position.z)
	print("[smoke] %s ground elevation under camera: %s m (expected %s…%s m)" % [
		label, "NaN" if is_nan(elev) else "%.1f" % elev, min_m, max_m])
	if is_nan(elev):
		fails.append("%s: no terrain data under camera (elevation NaN)" % label)
	elif elev < min_m or elev > max_m:
		fails.append("%s: ground elevation %.1f m outside plausible range" % [label, elev])


func _step() -> void:
	phase_frame += 1
	if phase == "oslo" and phase_frame >= PHASE_FRAMES:
		var oslo_tiles: int = tm.get_active_tile_count()
		print("[smoke] Oslo phase done: active tiles = ", oslo_tiles)
		if oslo_tiles <= 0:
			fails.append("no active tiles over Oslo — Norway 34 px terrain not streaming")
		# Oslo plateau ~20–80 m; allow fjord-side low ground with margin.
		_probe_elevation("Oslo", -100.0, 300.0)
		phase = "lszh"
		phase_frame = 0
		_move_to(LSZH_E, LSZH_N)
	elif phase == "lszh" and phase_frame >= PHASE_FRAMES:
		var lszh_tiles: int = tm.get_active_tile_count()
		print("[smoke] LSZH phase done: active tiles = ", lszh_tiles)
		if lszh_tiles <= 0:
			fails.append("no active tiles over LSZH — CH/BW regression (R3)")
		# Zurich plain ~400–470 m.
		_probe_elevation("LSZH", 250.0, 650.0)
		_finish(1 if fails.size() > 0 else 0)


func _finish(code: int) -> void:
	phase = "done"
	print("[smoke] ", "PASS" if code == 0 else "FAIL")
	for f in fails:
		print("   - ", f)
	get_tree().quit(code)
