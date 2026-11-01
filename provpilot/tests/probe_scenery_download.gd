extends SceneTree
## Probe (not a smoke test): install one base scenery pack through the real
## SceneryDownloaderPanel pipeline (HTTP download, size + sha256 check, ZIP
## extraction, top-level region directory detection) from any manifest URL,
## e.g. a local `python3 -m http.server` over build/world-packs with a
## manifest whose urls point at it (docs/RELEASE.md §4).
##
##   PROBE_MANIFEST=http://127.0.0.1:8765/manifest_local.json PROBE_REGION=denmark \
##   PROBE_ROOT=/tmp/dl_root godot --headless --path provpilot -s tests/probe_scenery_download.gd
##
## Exit 0 when install_complete fires and the region's terrain/manifest.json
## exists below the install root; 1 otherwise (or after PROBE_TIMEOUT_S, 900).
var _t0 := 0.0
var _done := false
var _panel = null
var _root_dir := ""
var _region := ""
var _timeout := 900.0
var _manifest_url := ""
var _started := false

func _initialize() -> void:
	var manifest_url: String = OS.get_environment("PROBE_MANIFEST")
	_region = OS.get_environment("PROBE_REGION")
	_root_dir = OS.get_environment("PROBE_ROOT")
	if OS.get_environment("PROBE_TIMEOUT_S") != "":
		_timeout = float(OS.get_environment("PROBE_TIMEOUT_S"))
	if manifest_url.is_empty() or _region.is_empty() or _root_dir.is_empty():
		printerr("probe_scenery_download: set PROBE_MANIFEST, PROBE_REGION and PROBE_ROOT")
		quit(2)
		return
	_manifest_url = manifest_url
	_t0 = Time.get_ticks_msec() / 1000.0
	print("probe_scenery_download: manifest %s, region %s, root %s" % [manifest_url, _region, _root_dir])

## The panel builds its widgets when it enters the tree: create it on the
## first frame, not inside _initialize.
func _start() -> void:
	_panel = SceneryDownloaderPanel.new()
	root.add_child(_panel)
	_panel.bootstrap(_root_dir, _manifest_url)
	_panel.install_complete.connect(_on_complete)
	_panel.install_failed.connect(_on_failed)
	# Fetch the manifest ourselves (the panel's own fetch feeds its table; the
	# install takes the pack dictionary directly, as the row button does).
	var http := HTTPRequest.new()
	root.add_child(http)
	http.request_completed.connect(_on_manifest.bind(http))
	var err := http.request(_manifest_url)
	if err != OK:
		printerr("probe_scenery_download: manifest request failed: %d" % err)
		quit(1)

func _on_manifest(result: int, code: int, _headers: PackedStringArray, body: PackedByteArray, http: HTTPRequest) -> void:
	http.queue_free()
	if result != HTTPRequest.RESULT_SUCCESS or code != 200:
		printerr("probe_scenery_download: manifest fetch failed (result %d, http %d)" % [result, code])
		quit(1)
		return
	var manifest = JSON.parse_string(body.get_string_from_utf8())
	if typeof(manifest) != TYPE_DICTIONARY:
		printerr("probe_scenery_download: manifest is not JSON")
		quit(1)
		return
	for region in manifest.get("regions", []):
		if region.get("id", "") == _region and region.has("base"):
			var base: Dictionary = region["base"]
			var pack_id: String = _region + "-base-v" + str(base.get("version", "1"))
			print("probe_scenery_download: installing %s (%d bytes) from %s" % [pack_id, int(base.get("size", 0)), base.get("url", "")])
			_panel._on_pack_pressed(base, pack_id)
			return
	printerr("probe_scenery_download: region '%s' has no base pack in the manifest" % _region)
	quit(1)

func _on_complete(pack_id: String, install_path: String) -> void:
	_done = true
	var manifest_path := install_path.path_join("terrain").path_join("manifest.json")
	var ok := FileAccess.file_exists(manifest_path)
	var n := _count_files(install_path.path_join("terrain"))
	print("probe_scenery_download: %s installed at %s in %.0f s; terrain/manifest.json %s, %d terrain files" % [pack_id, install_path, Time.get_ticks_msec() / 1000.0 - _t0, "present" if ok else "MISSING", n])
	quit(0 if ok and n > 0 else 1)

func _on_failed(pack_id: String, message: String) -> void:
	_done = true
	printerr("probe_scenery_download: %s FAILED: %s" % [pack_id, message])
	quit(1)

func _count_files(dir: String) -> int:
	var d := DirAccess.open(dir)
	if d == null:
		return 0
	var n := 0
	d.list_dir_begin()
	var f := d.get_next()
	while f != "":
		if not d.current_is_dir():
			n += 1
		f = d.get_next()
	return n

func _process(_delta: float) -> bool:
	if not _started:
		_started = true
		_start()
	if not _done and Time.get_ticks_msec() / 1000.0 - _t0 > _timeout:
		printerr("probe_scenery_download: timed out after %.0f s" % _timeout)
		quit(1)
	return false
