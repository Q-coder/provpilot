extends VBoxContainer
class_name SceneryDownloaderPanel
## SceneryDownloaderPanel — Fetches the remote scenery manifest, lets the
## user download/install region "base" and "detail" packs, verifies sha256,
## extracts them into a configurable install root, and tracks installed
## pack state so future runs know what's already on disk.
##
## Embeddable in any screen — currently mounted as a section of
## `SettingsScreen`.
##
## State tracking (persisted via SettingsScreen.settings):
##   settings["installed_scenery_packs"] = {
##       "<pack_id>": {
##           "version": "1",
##           "sha256":  "…",
##           "size":    167314828,
##           "path":    "/abs/.../Switzerland",   # region root extracted to
##           "installed_at": <unix-ts>,
##       },
##       …
##   }
##
## A pack is considered "installed" when its sha256 in settings matches the
## manifest; an "update" is offered when the sha256 differs.

signal install_complete(pack_id: String, install_path: String)
signal install_failed(pack_id: String, message: String)

const DEFAULT_MANIFEST_URL := "https://www.md80.ch/provpilot/scenery_manifest.json"
const FONT_SM := 20
const FONT_MD := 24
const FONT_LG := 28
const SECTION_COLOR := Color(0.25, 0.55, 0.85, 1.0)
const LABEL_COLOR := Color(0.75, 0.78, 0.82)
const OK_COLOR := Color(0.45, 0.80, 0.45)
const WARN_COLOR := Color(0.95, 0.75, 0.30)
const ERR_COLOR := Color(0.95, 0.45, 0.45)

# Dictionary of {pack_id: dict} where dict has version/sha256/path/etc.
# Caller (SettingsScreen) supplies this and the helpers below mutate it.
var get_state: Callable = func() -> Dictionary: return {}
var set_state: Callable = func(_state: Dictionary) -> void: pass
var get_install_root: Callable = func() -> String: return ""
var on_area_installed: Callable = func(_path: String) -> void: pass

# UI refs.
var _url_edit: LineEdit
var _root_edit: LineEdit
var _root_btn: Button
var _refresh_btn: Button
var _status_lbl: Label
var _packs_box: VBoxContainer
var _progress: ProgressBar
var _progress_lbl: Label
var _cancel_btn: Button

var _manifest: Dictionary = {}
var _http: HTTPRequest

# Active job.
var _job_pack_id: String = ""
var _job_pack: Dictionary = {}
var _job_zip_path: String = ""
var _job_progress_timer: Timer
var _job_cancelled: bool = false


func _ready() -> void:
	add_theme_constant_override("separation", 8)
	_build_ui()
	_http = HTTPRequest.new()
	_http.use_threads = true
	_http.body_size_limit = -1
	_http.timeout = 0  # no timeout for the big downloads
	add_child(_http)
	_job_progress_timer = Timer.new()
	_job_progress_timer.wait_time = 0.5
	_job_progress_timer.timeout.connect(_on_progress_tick)
	add_child(_job_progress_timer)


# ── UI ────────────────────────────────────────────────────────────────

func _build_ui() -> void:
	# Header
	var hdr := Label.new()
	hdr.text = "Download Scenery"
	hdr.add_theme_font_size_override("font_size", FONT_LG)
	hdr.add_theme_color_override("font_color", SECTION_COLOR)
	add_child(hdr)

	var info := Label.new()
	info.text = "Fetch the official manifest and install region packs. Each region has one Base pack (terrain + roads + water + airports + trees) and four Detail quadrants (buildings + orthophoto + vegetation)."
	info.add_theme_font_size_override("font_size", FONT_SM)
	info.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	info.autowrap_mode = TextServer.AUTOWRAP_WORD
	add_child(info)

	# Manifest URL
	var url_row := HBoxContainer.new()
	url_row.add_theme_constant_override("separation", 8)
	add_child(url_row)
	var url_lbl := Label.new()
	url_lbl.text = "Manifest URL:"
	url_lbl.custom_minimum_size.x = 200
	url_lbl.add_theme_font_size_override("font_size", FONT_MD)
	url_lbl.add_theme_color_override("font_color", LABEL_COLOR)
	url_row.add_child(url_lbl)
	_url_edit = LineEdit.new()
	_url_edit.text = DEFAULT_MANIFEST_URL
	_url_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_url_edit.add_theme_font_size_override("font_size", FONT_MD)
	_url_edit.custom_minimum_size.y = 40
	url_row.add_child(_url_edit)
	_refresh_btn = Button.new()
	_refresh_btn.text = "Refresh"
	_refresh_btn.custom_minimum_size = Vector2(140, 40)
	_refresh_btn.add_theme_font_size_override("font_size", FONT_SM)
	_refresh_btn.pressed.connect(_on_refresh_pressed)
	url_row.add_child(_refresh_btn)

	# Install root
	var root_row := HBoxContainer.new()
	root_row.add_theme_constant_override("separation", 8)
	add_child(root_row)
	var root_lbl := Label.new()
	root_lbl.text = "Install Root:"
	root_lbl.custom_minimum_size.x = 200
	root_lbl.add_theme_font_size_override("font_size", FONT_MD)
	root_lbl.add_theme_color_override("font_color", LABEL_COLOR)
	root_row.add_child(root_lbl)
	_root_edit = LineEdit.new()
	_root_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_root_edit.add_theme_font_size_override("font_size", FONT_MD)
	_root_edit.custom_minimum_size.y = 40
	_root_edit.placeholder_text = "Directory that will contain Switzerland/, Germany/, …"
	root_row.add_child(_root_edit)
	_root_btn = Button.new()
	_root_btn.text = "Browse…"
	_root_btn.custom_minimum_size = Vector2(140, 40)
	_root_btn.add_theme_font_size_override("font_size", FONT_SM)
	_root_btn.pressed.connect(_on_browse_root)
	root_row.add_child(_root_btn)

	# Status row
	_status_lbl = Label.new()
	_status_lbl.text = "Click Refresh to fetch the manifest."
	_status_lbl.add_theme_font_size_override("font_size", FONT_SM)
	_status_lbl.add_theme_color_override("font_color", LABEL_COLOR)
	add_child(_status_lbl)

	# Packs list (scrollable).
	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(0, 320)
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)
	_packs_box = VBoxContainer.new()
	_packs_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_packs_box.add_theme_constant_override("separation", 4)
	scroll.add_child(_packs_box)

	# Progress row (hidden until a job starts).
	var prog_row := HBoxContainer.new()
	prog_row.add_theme_constant_override("separation", 8)
	add_child(prog_row)
	_progress = ProgressBar.new()
	_progress.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_progress.custom_minimum_size.y = 24
	_progress.max_value = 100.0
	_progress.value = 0.0
	prog_row.add_child(_progress)
	_progress_lbl = Label.new()
	_progress_lbl.text = ""
	_progress_lbl.custom_minimum_size.x = 320
	_progress_lbl.add_theme_font_size_override("font_size", FONT_SM)
	_progress_lbl.add_theme_color_override("font_color", LABEL_COLOR)
	prog_row.add_child(_progress_lbl)
	_cancel_btn = Button.new()
	_cancel_btn.text = "Cancel"
	_cancel_btn.custom_minimum_size = Vector2(120, 32)
	_cancel_btn.add_theme_font_size_override("font_size", FONT_SM)
	_cancel_btn.pressed.connect(_on_cancel_pressed)
	prog_row.add_child(_cancel_btn)
	_set_job_ui_visible(false)


# ── Pack list rendering ───────────────────────────────────────────────

func _refresh_packs_table() -> void:
	for c in _packs_box.get_children():
		c.queue_free()
	if _manifest.is_empty():
		return

	var installed: Dictionary = get_state.call()
	var regions: Array = _manifest.get("regions", [])
	for region in regions:
		var rname := str(region.get("name", region.get("id", "?")))
		var rhdr := Label.new()
		rhdr.text = rname
		rhdr.add_theme_font_size_override("font_size", FONT_MD)
		rhdr.add_theme_color_override("font_color", SECTION_COLOR)
		_packs_box.add_child(rhdr)

		if region.has("base"):
			_add_pack_row(region, region["base"], "Base", "%s-base-v%s" % [region.get("id"), region["base"].get("version", "1")], installed)
		for pack in region.get("detail_packs", []):
			var pid := "%s-detail-%s-v%s" % [region.get("id"), pack.get("id"), pack.get("version", "1")]
			_add_pack_row(region, pack, "Detail · %s" % pack.get("name", pack.get("id")), pid, installed)
		_packs_box.add_child(HSeparator.new())


func _add_pack_row(region: Dictionary, pack: Dictionary, label: String, pack_id: String, installed: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_packs_box.add_child(row)

	var name_lbl := Label.new()
	name_lbl.text = "  " + label
	name_lbl.custom_minimum_size.x = 320
	name_lbl.add_theme_font_size_override("font_size", FONT_SM)
	row.add_child(name_lbl)

	var size_lbl := Label.new()
	size_lbl.text = _fmt_size(int(pack.get("size", 0)))
	size_lbl.custom_minimum_size.x = 100
	size_lbl.add_theme_font_size_override("font_size", FONT_SM)
	size_lbl.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	row.add_child(size_lbl)

	var status := "Not installed"
	var status_color := LABEL_COLOR
	var btn_text := "Download"
	if installed.has(pack_id):
		var entry: Dictionary = installed[pack_id]
		if entry.get("sha256", "") == pack.get("sha256", ""):
			status = "Installed (v%s)" % entry.get("version", "?")
			status_color = OK_COLOR
			btn_text = "Reinstall"
		else:
			status = "Update available"
			status_color = WARN_COLOR
			btn_text = "Update"

	var status_lbl := Label.new()
	status_lbl.text = status
	status_lbl.custom_minimum_size.x = 200
	status_lbl.add_theme_font_size_override("font_size", FONT_SM)
	status_lbl.add_theme_color_override("font_color", status_color)
	row.add_child(status_lbl)

	var dl_btn := Button.new()
	dl_btn.text = btn_text
	dl_btn.custom_minimum_size = Vector2(140, 32)
	dl_btn.add_theme_font_size_override("font_size", FONT_SM)
	dl_btn.disabled = (_job_pack_id != "")
	var pid_capture := pack_id
	var pack_capture := pack
	var region_capture := region
	dl_btn.pressed.connect(func() -> void:
		_start_install(region_capture, pack_capture, pid_capture)
	)
	row.add_child(dl_btn)

	if installed.has(pack_id):
		var rm_btn := Button.new()
		rm_btn.text = "Forget"
		rm_btn.tooltip_text = "Remove the installed-marker for this pack. Files on disk are NOT deleted."
		rm_btn.custom_minimum_size = Vector2(110, 32)
		rm_btn.add_theme_font_size_override("font_size", FONT_SM)
		rm_btn.disabled = (_job_pack_id != "")
		rm_btn.pressed.connect(func() -> void: _forget_pack(pid_capture))
		row.add_child(rm_btn)


# ── Manifest fetch ────────────────────────────────────────────────────

func _on_refresh_pressed() -> void:
	if _job_pack_id != "":
		return
	var url := _url_edit.text.strip_edges()
	if url.is_empty():
		_set_status("Manifest URL is empty.", ERR_COLOR)
		return
	_set_status("Fetching %s …" % url, LABEL_COLOR)
	_refresh_btn.disabled = true
	# Disconnect any previous handler so signals don't double-fire.
	for cb in _http.request_completed.get_connections():
		_http.request_completed.disconnect(cb["callable"])
	_http.request_completed.connect(_on_manifest_completed)
	var err := _http.request(url)
	if err != OK:
		_set_status("Request failed to start (err=%d)" % err, ERR_COLOR)
		_refresh_btn.disabled = false


func _on_manifest_completed(result: int, code: int, _hdrs: PackedStringArray, body: PackedByteArray) -> void:
	_refresh_btn.disabled = false
	if result != HTTPRequest.RESULT_SUCCESS or code < 200 or code >= 300:
		_set_status("Manifest fetch failed (result=%d http=%d)" % [result, code], ERR_COLOR)
		return
	var parsed = JSON.parse_string(body.get_string_from_utf8())
	if not (parsed is Dictionary) or not parsed.has("regions"):
		_set_status("Manifest is not valid JSON (no 'regions').", ERR_COLOR)
		return
	_manifest = parsed
	var n := 0
	for r in _manifest.get("regions", []):
		n += 1
		n += int(r.get("detail_packs", []).size())
	_set_status("Manifest OK — %d region(s), %d pack(s)." % [_manifest.get("regions", []).size(), n], OK_COLOR)
	_refresh_packs_table()


# ── Install pipeline ─────────────────────────────────────────────────

func _start_install(_region: Dictionary, pack: Dictionary, pack_id: String) -> void:
	if _job_pack_id != "":
		return
	var install_root: String = _root_edit.text.strip_edges()
	if install_root.is_empty():
		_set_status("Set the install root first.", ERR_COLOR)
		return
	if DirAccess.make_dir_recursive_absolute(install_root) != OK and not DirAccess.dir_exists_absolute(install_root):
		_set_status("Install root is not writable: %s" % install_root, ERR_COLOR)
		return
	var url: String = str(pack.get("url", ""))
	if url.is_empty():
		_set_status("Pack has no URL.", ERR_COLOR)
		return
	_job_pack_id = pack_id
	_job_pack = pack
	_job_cancelled = false
	_set_job_ui_visible(true)
	_progress.value = 0.0
	_progress_lbl.text = "Connecting…"
	_set_status("Downloading %s …" % pack_id, LABEL_COLOR)

	# Write zip to install_root/.downloads so we don't accidentally fill
	# user:// (which on macOS lives in ~/Library/Application Support).
	var dl_dir := install_root.path_join(".downloads")
	DirAccess.make_dir_recursive_absolute(dl_dir)
	_job_zip_path = dl_dir.path_join(pack_id + ".zip")
	# Remove any leftover from a failed previous attempt.
	if FileAccess.file_exists(_job_zip_path):
		DirAccess.remove_absolute(_job_zip_path)

	for cb in _http.request_completed.get_connections():
		_http.request_completed.disconnect(cb["callable"])
	_http.request_completed.connect(_on_download_completed)
	_http.download_file = _job_zip_path
	var err := _http.request(url)
	if err != OK:
		_finish_job_failed("Request failed to start (err=%d)" % err)
		return
	_job_progress_timer.start()
	_refresh_packs_table()  # disable buttons


func _on_progress_tick() -> void:
	if _job_pack_id == "":
		return
	var downloaded := _http.get_downloaded_bytes()
	var total := _http.get_body_size()
	if total > 0:
		_progress.value = clamp(100.0 * float(downloaded) / float(total), 0.0, 100.0)
		_progress_lbl.text = "%s / %s" % [_fmt_size(downloaded), _fmt_size(total)]
	else:
		_progress.value = 0.0
		_progress_lbl.text = "%s downloaded" % _fmt_size(downloaded)


func _on_download_completed(result: int, code: int, _hdrs: PackedStringArray, _body: PackedByteArray) -> void:
	_job_progress_timer.stop()
	_http.download_file = ""
	if _job_cancelled:
		_finish_job_failed("Cancelled.")
		return
	if result != HTTPRequest.RESULT_SUCCESS or code < 200 or code >= 300:
		_finish_job_failed("Download failed (result=%d http=%d)" % [result, code])
		return
	# Verify size + sha256.
	var expected_size := int(_job_pack.get("size", 0))
	var on_disk := _file_size(_job_zip_path)
	if expected_size > 0 and on_disk != expected_size:
		_finish_job_failed("Size mismatch: got %d, expected %d" % [on_disk, expected_size])
		return
	_progress.value = 0.0
	_progress_lbl.text = "Verifying sha256…"
	var ok := await _verify_sha256(_job_zip_path, str(_job_pack.get("sha256", "")))
	if not ok:
		_finish_job_failed("sha256 mismatch — file may be corrupt.")
		return
	# Extract.
	_progress_lbl.text = "Extracting…"
	var install_root: String = _root_edit.text.strip_edges()
	var extract_ok := await _extract_zip(_job_zip_path, install_root)
	# Best-effort cleanup.
	DirAccess.remove_absolute(_job_zip_path)
	if not extract_ok:
		_finish_job_failed("Extraction failed.")
		return
	# Persist install state. The "path" we record is the first top-level
	# directory inside the zip joined onto install_root — that's the region
	# directory the SceneryLibrary scanner expects.
	var top_dir := _top_level_dir_for_pack(install_root)
	var state: Dictionary = get_state.call()
	state[_job_pack_id] = {
		"version":      _job_pack.get("version", "1"),
		"sha256":       _job_pack.get("sha256", ""),
		"size":         _job_pack.get("size", 0),
		"path":         top_dir,
		"installed_at": int(Time.get_unix_time_from_system()),
	}
	set_state.call(state)
	if not top_dir.is_empty():
		on_area_installed.call(top_dir)
	emit_signal("install_complete", _job_pack_id, top_dir)
	_set_status("Installed %s → %s" % [_job_pack_id, top_dir], OK_COLOR)
	_job_pack_id = ""
	_job_pack = {}
	_job_zip_path = ""
	_set_job_ui_visible(false)
	_refresh_packs_table()


func _on_cancel_pressed() -> void:
	if _job_pack_id == "":
		return
	_job_cancelled = true
	_http.cancel_request()


func _finish_job_failed(msg: String) -> void:
	push_warning("SceneryDownloader: %s (%s)" % [msg, _job_pack_id])
	emit_signal("install_failed", _job_pack_id, msg)
	if not _job_zip_path.is_empty() and FileAccess.file_exists(_job_zip_path):
		DirAccess.remove_absolute(_job_zip_path)
	_set_status(msg, ERR_COLOR)
	_job_pack_id = ""
	_job_pack = {}
	_job_zip_path = ""
	_set_job_ui_visible(false)
	_refresh_packs_table()


# ── sha256 (chunked, yields each step so the UI stays responsive) ─────

func _verify_sha256(path: String, expected: String) -> bool:
	if expected.is_empty():
		return true  # nothing to check against
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return false
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	var total := float(f.get_length())
	var done := 0.0
	const CHUNK := 4 * 1024 * 1024  # 4 MB
	while not f.eof_reached():
		var chunk := f.get_buffer(CHUNK)
		if chunk.size() == 0:
			break
		ctx.update(chunk)
		done += chunk.size()
		if total > 0:
			_progress.value = clamp(100.0 * done / total, 0.0, 100.0)
			_progress_lbl.text = "Verifying… %s / %s" % [_fmt_size(int(done)), _fmt_size(int(total))]
		# Yield to keep UI responsive between chunks.
		await get_tree().process_frame
	f.close()
	var digest := ctx.finish().hex_encode()
	return digest.to_lower() == expected.to_lower()


# ── ZIP extraction ────────────────────────────────────────────────────

# Track which top-level dirs the current zip writes into (for the install
# state's "path" field). Populated by _extract_zip().
var _job_top_level: String = ""


func _extract_zip(zip_path: String, dest_root: String) -> bool:
	var zr := ZIPReader.new()
	if zr.open(zip_path) != OK:
		return false
	var files := zr.get_files()
	var total := files.size()
	_job_top_level = ""
	var dirs_made := {}
	for i in range(total):
		if _job_cancelled:
			zr.close()
			return false
		var name: String = files[i]
		if name.ends_with("/"):
			continue  # bare directory entry
		var data := zr.read_file(name)
		var out_path := dest_root.path_join(name)
		var out_dir := out_path.get_base_dir()
		if not dirs_made.has(out_dir):
			DirAccess.make_dir_recursive_absolute(out_dir)
			dirs_made[out_dir] = true
		var f := FileAccess.open(out_path, FileAccess.WRITE)
		if f == null:
			push_warning("SceneryDownloader: cannot write %s" % out_path)
			zr.close()
			return false
		f.store_buffer(data)
		f.close()
		# Record first top-level component for install state.
		if _job_top_level.is_empty():
			var parts := name.split("/", false)
			if parts.size() > 0:
				_job_top_level = parts[0]
		if i % 32 == 0:
			_progress.value = clamp(100.0 * float(i) / float(total), 0.0, 100.0)
			_progress_lbl.text = "Extracting %d / %d" % [i, total]
			await get_tree().process_frame
	zr.close()
	_progress.value = 100.0
	_progress_lbl.text = "Extracted %d files" % total
	return true


func _top_level_dir_for_pack(install_root: String) -> String:
	if _job_top_level.is_empty():
		return install_root
	# Walk down the unique single-child chain (BW uses Germany/Baden-Wuertemberg)
	# so the recorded path matches what `_detect_assets()` looks for.
	var cur := install_root.path_join(_job_top_level)
	while true:
		var d := DirAccess.open(cur)
		if d == null:
			break
		if _has_asset_subdir(cur):
			return cur
		var subs := PackedStringArray()
		d.list_dir_begin()
		var n := d.get_next()
		while n != "":
			if not n.begins_with(".") and d.current_is_dir():
				subs.append(n)
			n = d.get_next()
		d.list_dir_end()
		if subs.size() == 1:
			cur = cur.path_join(subs[0])
		else:
			break
	return cur


func _has_asset_subdir(path: String) -> bool:
	for sub in ["terrain", "orthophoto", "buildings", "water", "roads", "vegetation", "trees", "airports"]:
		if DirAccess.dir_exists_absolute(path.path_join(sub)):
			return true
	return false


# ── Maintenance ───────────────────────────────────────────────────────

func _forget_pack(pack_id: String) -> void:
	var state: Dictionary = get_state.call()
	if state.erase(pack_id):
		set_state.call(state)
		_refresh_packs_table()


# ── Browse for install root ───────────────────────────────────────────

func _on_browse_root() -> void:
	var dialog := FileDialog.new()
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.use_native_dialog = true
	dialog.title = "Choose Scenery Install Root"
	add_child(dialog)
	dialog.dir_selected.connect(func(p: String) -> void:
		_root_edit.text = p
		dialog.queue_free()
	)
	dialog.canceled.connect(func() -> void: dialog.queue_free())
	dialog.popup_centered()


# ── Helpers ───────────────────────────────────────────────────────────

func _set_status(msg: String, color: Color) -> void:
	_status_lbl.text = msg
	_status_lbl.add_theme_color_override("font_color", color)


func _set_job_ui_visible(v: bool) -> void:
	_progress.visible = v
	_progress_lbl.visible = v
	_cancel_btn.visible = v


func _fmt_size(n: int) -> String:
	var f := float(n)
	for unit in ["B", "KB", "MB", "GB", "TB"]:
		if f < 1024.0 or unit == "TB":
			return "%.1f %s" % [f, unit]
		f /= 1024.0
	return str(n)


func _file_size(path: String) -> int:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return -1
	var s := f.get_length()
	f.close()
	return s


# ── Caller-facing helpers ─────────────────────────────────────────────

## Called by SettingsScreen after wiring up callbacks. Sets the initial
## install-root field text and tries to load a previously-fetched manifest.
func bootstrap(initial_root: String, manifest_url: String = "") -> void:
	if not manifest_url.is_empty():
		_url_edit.text = manifest_url
	_root_edit.text = initial_root
