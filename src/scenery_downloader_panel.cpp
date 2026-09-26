#include "scenery_downloader_panel.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <vector>

using namespace godot;

namespace jsbgodot
{

static const char *DEFAULT_MANIFEST_URL = "https://www.md80.ch/provpilot/scenery_manifest.json";
static const int FONT_SM = 20, FONT_MD = 24, FONT_LG = 28;
static const Color SECTION_COLOR(0.25, 0.55, 0.85, 1.0);
static const Color LABEL_COLOR(0.75, 0.78, 0.82);
static const Color OK_COLOR(0.45, 0.80, 0.45);
static const Color WARN_COLOR(0.95, 0.75, 0.30);
static const Color ERR_COLOR(0.95, 0.45, 0.45);

void SceneryDownloaderPanel::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("bootstrap", "initial_root", "manifest_url"), &SceneryDownloaderPanel::bootstrap, DEFVAL(String()));
#define SDP_PROP(name, getter, setter)                                                          \
	ClassDB::bind_method(D_METHOD(#getter), &SceneryDownloaderPanel::getter);                \
	ClassDB::bind_method(D_METHOD(#setter, "callable"), &SceneryDownloaderPanel::setter);    \
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, #name), #setter, #getter);
	SDP_PROP(get_state, get_get_state, set_get_state)
	SDP_PROP(set_state, get_set_state, set_set_state)
	SDP_PROP(get_install_root, get_get_install_root, set_get_install_root)
	SDP_PROP(on_area_installed, get_on_area_installed, set_on_area_installed)
#undef SDP_PROP
	ClassDB::bind_method(D_METHOD("_on_refresh_pressed"), &SceneryDownloaderPanel::_on_refresh_pressed);
	ClassDB::bind_method(D_METHOD("_on_manifest_completed", "result", "code", "headers", "body"), &SceneryDownloaderPanel::_on_manifest_completed);
	ClassDB::bind_method(D_METHOD("_on_download_completed", "result", "code", "headers", "body"), &SceneryDownloaderPanel::_on_download_completed);
	ClassDB::bind_method(D_METHOD("_on_progress_tick"), &SceneryDownloaderPanel::_on_progress_tick);
	ClassDB::bind_method(D_METHOD("_on_cancel_pressed"), &SceneryDownloaderPanel::_on_cancel_pressed);
	ClassDB::bind_method(D_METHOD("_on_browse_root"), &SceneryDownloaderPanel::_on_browse_root);
	ClassDB::bind_method(D_METHOD("_on_root_dir_selected", "path"), &SceneryDownloaderPanel::_on_root_dir_selected);
	ClassDB::bind_method(D_METHOD("_on_pack_pressed", "pack", "pack_id"), &SceneryDownloaderPanel::_on_pack_pressed);
	ClassDB::bind_method(D_METHOD("_on_forget_pressed", "pack_id"), &SceneryDownloaderPanel::_on_forget_pressed);
	ADD_SIGNAL(MethodInfo("install_complete", PropertyInfo(Variant::STRING, "pack_id"), PropertyInfo(Variant::STRING, "install_path")));
	ADD_SIGNAL(MethodInfo("install_failed", PropertyInfo(Variant::STRING, "pack_id"), PropertyInfo(Variant::STRING, "message")));
}

SceneryDownloaderPanel::SceneryDownloaderPanel()
{
}

Callable SceneryDownloaderPanel::get_get_state() const { return get_state; }
void SceneryDownloaderPanel::set_get_state(const Callable &c) { get_state = c; }
Callable SceneryDownloaderPanel::get_set_state() const { return set_state; }
void SceneryDownloaderPanel::set_set_state(const Callable &c) { set_state = c; }
Callable SceneryDownloaderPanel::get_get_install_root() const { return get_install_root; }
void SceneryDownloaderPanel::set_get_install_root(const Callable &c) { get_install_root = c; }
Callable SceneryDownloaderPanel::get_on_area_installed() const { return on_area_installed; }
void SceneryDownloaderPanel::set_on_area_installed(const Callable &c) { on_area_installed = c; }

void SceneryDownloaderPanel::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	add_theme_constant_override("separation", 8);
	build_ui();
	http = memnew(HTTPRequest);
	http->set_use_threads(true);
	http->set_body_size_limit(-1);
	http->set_timeout(0); // no timeout for the big downloads
	add_child(http);
	progress_timer = memnew(Timer);
	progress_timer->set_wait_time(0.5);
	progress_timer->connect("timeout", Callable(this, "_on_progress_tick"));
	add_child(progress_timer);
	set_process(true);
}

// ── UI ──────────────────────────────────────────────────────────────────────

static Label *text_label(const String &text, int size, const Color &color)
{
	Label *l = memnew(Label);
	l->set_text(text);
	l->add_theme_font_size_override("font_size", size);
	l->add_theme_color_override("font_color", color);
	return l;
}

static Button *sized_button(const String &text, const Vector2 &size, int font)
{
	Button *b = memnew(Button);
	b->set_text(text);
	b->set_custom_minimum_size(size);
	b->add_theme_font_size_override("font_size", font);
	return b;
}

void SceneryDownloaderPanel::build_ui()
{
	add_child(text_label("Download Scenery", FONT_LG, SECTION_COLOR));
	Label *info = text_label("Fetch the official manifest and install region packs. Each region has one Base pack (terrain + roads + water + airports + trees) and four Detail quadrants (buildings + orthophoto + vegetation).", FONT_SM, Color(0.6, 0.6, 0.6));
	info->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
	add_child(info);

	HBoxContainer *url_row = memnew(HBoxContainer);
	url_row->add_theme_constant_override("separation", 8);
	add_child(url_row);
	Label *url_lbl = text_label("Manifest URL:", FONT_MD, LABEL_COLOR);
	url_lbl->set_custom_minimum_size(Vector2(200, 0));
	url_row->add_child(url_lbl);
	url_edit = memnew(LineEdit);
	url_edit->set_text(DEFAULT_MANIFEST_URL);
	url_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	url_edit->add_theme_font_size_override("font_size", FONT_MD);
	url_edit->set_custom_minimum_size(Vector2(0, 40));
	url_row->add_child(url_edit);
	refresh_btn = sized_button("Refresh", Vector2(140, 40), FONT_SM);
	refresh_btn->connect("pressed", Callable(this, "_on_refresh_pressed"));
	url_row->add_child(refresh_btn);

	HBoxContainer *root_row = memnew(HBoxContainer);
	root_row->add_theme_constant_override("separation", 8);
	add_child(root_row);
	Label *root_lbl = text_label("Install Root:", FONT_MD, LABEL_COLOR);
	root_lbl->set_custom_minimum_size(Vector2(200, 0));
	root_row->add_child(root_lbl);
	root_edit = memnew(LineEdit);
	root_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	root_edit->add_theme_font_size_override("font_size", FONT_MD);
	root_edit->set_custom_minimum_size(Vector2(0, 40));
	root_edit->set_placeholder(String::utf8("Directory that will contain Switzerland/, Germany/, \xE2\x80\xA6"));
	root_row->add_child(root_edit);
	Button *root_btn = sized_button(String::utf8("Browse\xE2\x80\xA6"), Vector2(140, 40), FONT_SM);
	root_btn->connect("pressed", Callable(this, "_on_browse_root"));
	root_row->add_child(root_btn);

	status_lbl = text_label("Click Refresh to fetch the manifest.", FONT_SM, LABEL_COLOR);
	add_child(status_lbl);

	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_custom_minimum_size(Vector2(0, 320));
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(scroll);
	packs_box = memnew(VBoxContainer);
	packs_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	packs_box->add_theme_constant_override("separation", 4);
	scroll->add_child(packs_box);

	HBoxContainer *prog_row = memnew(HBoxContainer);
	prog_row->add_theme_constant_override("separation", 8);
	add_child(prog_row);
	progress = memnew(ProgressBar);
	progress->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	progress->set_custom_minimum_size(Vector2(0, 24));
	progress->set_max(100.0);
	progress->set_value(0.0);
	prog_row->add_child(progress);
	progress_lbl = text_label("", FONT_SM, LABEL_COLOR);
	progress_lbl->set_custom_minimum_size(Vector2(320, 0));
	prog_row->add_child(progress_lbl);
	cancel_btn = sized_button("Cancel", Vector2(120, 32), FONT_SM);
	cancel_btn->connect("pressed", Callable(this, "_on_cancel_pressed"));
	prog_row->add_child(cancel_btn);
	set_job_ui_visible(false);
}

void SceneryDownloaderPanel::bootstrap(const String &initial_root, const String &manifest_url)
{
	if (!manifest_url.is_empty())
	{
		url_edit->set_text(manifest_url);
	}
	root_edit->set_text(initial_root);
}

// ── Pack list ───────────────────────────────────────────────────────────────

void SceneryDownloaderPanel::refresh_packs_table()
{
	const TypedArray<Node> kids = packs_box->get_children();
	for (int i = 0; i < kids.size(); i++)
	{
		Object::cast_to<Node>(kids[i])->queue_free();
	}
	if (manifest.is_empty())
	{
		return;
	}
	const Dictionary installed = get_state.is_valid() ? Dictionary(get_state.call()) : Dictionary();
	const Array regions = manifest.get("regions", Array());
	for (int r = 0; r < regions.size(); r++)
	{
		const Dictionary region = regions[r];
		const String rid = region.get("id", "?");
		packs_box->add_child(text_label(region.get("name", rid), FONT_MD, SECTION_COLOR));
		if (region.has("base"))
		{
			const Dictionary base = region["base"];
			add_pack_row(base, "Base", rid + String("-base-v") + String(base.get("version", "1")), installed);
		}
		const Array details = region.get("detail_packs", Array());
		for (int i = 0; i < details.size(); i++)
		{
			const Dictionary pack = details[i];
			const String pid = rid + String("-detail-") + String(pack.get("id", "")) + String("-v") + String(pack.get("version", "1"));
			add_pack_row(pack, String::utf8("Detail \xC2\xB7 ") + String(pack.get("name", pack.get("id", ""))), pid, installed);
		}
		packs_box->add_child(memnew(HSeparator));
	}
}

void SceneryDownloaderPanel::add_pack_row(const Dictionary &pack, const String &label, const String &pack_id, const Dictionary &installed)
{
	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override("separation", 12);
	packs_box->add_child(row);
	Label *name_lbl = text_label("  " + label, FONT_SM, Color(1, 1, 1));
	name_lbl->set_custom_minimum_size(Vector2(320, 0));
	row->add_child(name_lbl);
	Label *size_lbl = text_label(fmt_size((int64_t)(double)pack.get("size", 0)), FONT_SM, Color(0.6, 0.6, 0.6));
	size_lbl->set_custom_minimum_size(Vector2(100, 0));
	row->add_child(size_lbl);
	String status = "Not installed";
	Color status_color = LABEL_COLOR;
	String btn_text = "Download";
	if (installed.has(pack_id))
	{
		const Dictionary entry = installed[pack_id];
		if (String(entry.get("sha256", "")) == String(pack.get("sha256", "")))
		{
			status = String("Installed (v") + String(entry.get("version", "?")) + ")";
			status_color = OK_COLOR;
			btn_text = "Reinstall";
		}
		else
		{
			status = "Update available";
			status_color = WARN_COLOR;
			btn_text = "Update";
		}
	}
	Label *status_l = text_label(status, FONT_SM, status_color);
	status_l->set_custom_minimum_size(Vector2(200, 0));
	row->add_child(status_l);
	Button *dl_btn = sized_button(btn_text, Vector2(140, 32), FONT_SM);
	dl_btn->set_disabled(!job_pack_id.is_empty());
	dl_btn->connect("pressed", Callable(this, "_on_pack_pressed").bind(pack, pack_id));
	row->add_child(dl_btn);
	if (installed.has(pack_id))
	{
		Button *rm_btn = sized_button("Forget", Vector2(110, 32), FONT_SM);
		rm_btn->set_tooltip_text("Remove the installed-marker for this pack. Files on disk are NOT deleted.");
		rm_btn->set_disabled(!job_pack_id.is_empty());
		rm_btn->connect("pressed", Callable(this, "_on_forget_pressed").bind(pack_id));
		row->add_child(rm_btn);
	}
}

void SceneryDownloaderPanel::_on_pack_pressed(const Dictionary &pack, const String &pack_id)
{
	start_install(pack, pack_id);
}

void SceneryDownloaderPanel::_on_forget_pressed(const String &pack_id)
{
	forget_pack(pack_id);
}

// ── Manifest fetch ──────────────────────────────────────────────────────────

void SceneryDownloaderPanel::reconnect_http(const char *method)
{
	const Callable manifest_cb(this, "_on_manifest_completed");
	const Callable download_cb(this, "_on_download_completed");
	if (http->is_connected("request_completed", manifest_cb))
	{
		http->disconnect("request_completed", manifest_cb);
	}
	if (http->is_connected("request_completed", download_cb))
	{
		http->disconnect("request_completed", download_cb);
	}
	http->connect("request_completed", Callable(this, method));
}

void SceneryDownloaderPanel::_on_refresh_pressed()
{
	if (!job_pack_id.is_empty())
	{
		return;
	}
	const String url = url_edit->get_text().strip_edges();
	if (url.is_empty())
	{
		set_status("Manifest URL is empty.", ERR_COLOR);
		return;
	}
	set_status(String("Fetching ") + url + String::utf8(" \xE2\x80\xA6"), LABEL_COLOR);
	refresh_btn->set_disabled(true);
	reconnect_http("_on_manifest_completed");
	const Error err = http->request(url);
	if (err != OK)
	{
		set_status(String("Request failed to start (err=") + String::num_int64((int)err) + ")", ERR_COLOR);
		refresh_btn->set_disabled(false);
	}
}

void SceneryDownloaderPanel::_on_manifest_completed(int result, int code, const PackedStringArray &, const PackedByteArray &body)
{
	refresh_btn->set_disabled(false);
	if (result != HTTPRequest::RESULT_SUCCESS || code < 200 || code >= 300)
	{
		set_status(String("Manifest fetch failed (result=") + String::num_int64(result) + " http=" + String::num_int64(code) + ")", ERR_COLOR);
		return;
	}
	const Variant parsed = JSON::parse_string(body.get_string_from_utf8());
	if (parsed.get_type() != Variant::DICTIONARY || !Dictionary(parsed).has("regions"))
	{
		set_status("Manifest is not valid JSON (no 'regions').", ERR_COLOR);
		return;
	}
	manifest = parsed;
	const Array regions = manifest.get("regions", Array());
	int n = 0;
	for (int i = 0; i < regions.size(); i++)
	{
		n += 1 + Array(Dictionary(regions[i]).get("detail_packs", Array())).size();
	}
	set_status(String::utf8("Manifest OK \xE2\x80\x94 ") + String::num_int64(regions.size()) + " region(s), " + String::num_int64(n) + " pack(s).", OK_COLOR);
	refresh_packs_table();
}

// ── Install pipeline ────────────────────────────────────────────────────────

void SceneryDownloaderPanel::start_install(const Dictionary &pack, const String &pack_id)
{
	if (!job_pack_id.is_empty())
	{
		return;
	}
	const String install_root = root_edit->get_text().strip_edges();
	if (install_root.is_empty())
	{
		set_status("Set the install root first.", ERR_COLOR);
		return;
	}
	if (DirAccess::make_dir_recursive_absolute(install_root) != OK && !DirAccess::dir_exists_absolute(install_root))
	{
		set_status("Install root is not writable: " + install_root, ERR_COLOR);
		return;
	}
	const String url = pack.get("url", "");
	if (url.is_empty())
	{
		set_status("Pack has no URL.", ERR_COLOR);
		return;
	}
	job_pack_id = pack_id;
	job_pack = pack;
	job_cancelled = false;
	set_job_ui_visible(true);
	progress->set_value(0.0);
	progress_lbl->set_text(String::utf8("Connecting\xE2\x80\xA6"));
	set_status(String("Downloading ") + pack_id + String::utf8(" \xE2\x80\xA6"), LABEL_COLOR);
	// The zip goes to install_root/.downloads (not user://, which on macOS
	// lives in ~/Library/Application Support).
	const String dl_dir = install_root.path_join(".downloads");
	DirAccess::make_dir_recursive_absolute(dl_dir);
	job_zip_path = dl_dir.path_join(pack_id + String(".zip"));
	if (FileAccess::file_exists(job_zip_path))
	{
		DirAccess::remove_absolute(job_zip_path); // leftover of a failed attempt
	}
	reconnect_http("_on_download_completed");
	http->set_download_file(job_zip_path);
	const Error err = http->request(url);
	if (err != OK)
	{
		finish_job_failed(String("Request failed to start (err=") + String::num_int64((int)err) + ")");
		return;
	}
	phase = PHASE_DOWNLOADING;
	progress_timer->start();
	refresh_packs_table(); // disable the buttons
}

void SceneryDownloaderPanel::_on_progress_tick()
{
	if (job_pack_id.is_empty() || phase != PHASE_DOWNLOADING)
	{
		return;
	}
	// HTTPRequest's byte counters are int32 and overflow at ~2 GiB; the
	// packs are tens of GB, so read the partial file size from disk and use
	// the manifest's size for the total.
	const int64_t downloaded = file_size(job_zip_path);
	int64_t total = (int64_t)(double)job_pack.get("size", 0);
	if (total <= 0)
	{
		total = http->get_body_size();
	}
	if (total > 0)
	{
		progress->set_value(Math::clamp(100.0 * (double)downloaded / (double)total, 0.0, 100.0));
		progress_lbl->set_text(fmt_size(downloaded) + " / " + fmt_size(total));
	}
	else
	{
		progress->set_value(0.0);
		progress_lbl->set_text(fmt_size(downloaded) + " downloaded");
	}
}

void SceneryDownloaderPanel::_on_download_completed(int result, int code, const PackedStringArray &, const PackedByteArray &)
{
	progress_timer->stop();
	http->set_download_file("");
	if (job_cancelled)
	{
		finish_job_failed("Cancelled.");
		return;
	}
	if (result != HTTPRequest::RESULT_SUCCESS || code < 200 || code >= 300)
	{
		finish_job_failed(String("Download failed (result=") + String::num_int64(result) + " http=" + String::num_int64(code) + ")");
		return;
	}
	const int64_t expected_size = (int64_t)(double)job_pack.get("size", 0);
	const int64_t on_disk = file_size(job_zip_path);
	if (expected_size > 0 && on_disk != expected_size)
	{
		finish_job_failed(String("Size mismatch: got ") + String::num_int64(on_disk) + ", expected " + String::num_int64(expected_size));
		return;
	}
	begin_hashing();
}

void SceneryDownloaderPanel::begin_hashing()
{
	const String expected = job_pack.get("sha256", "");
	if (expected.is_empty())
	{
		begin_extract(); // nothing to check against
		return;
	}
	hash_file = FileAccess::open(job_zip_path, FileAccess::READ);
	if (hash_file.is_null())
	{
		finish_job_failed("cannot open the download for verification");
		return;
	}
	hash_ctx.instantiate();
	hash_ctx->start(HashingContext::HASH_SHA256);
	hash_total = (double)hash_file->get_length();
	hash_done = 0.0;
	progress->set_value(0.0);
	progress_lbl->set_text(String::utf8("Verifying sha256\xE2\x80\xA6"));
	phase = PHASE_HASHING;
}

// One chunk per frame so the UI stays responsive.
void SceneryDownloaderPanel::step_hashing()
{
	if (job_cancelled)
	{
		hash_file.unref();
		finish_job_failed("Cancelled.");
		return;
	}
	PackedByteArray chunk;
	if (!hash_file->eof_reached())
	{
		chunk = hash_file->get_buffer(HASH_CHUNK);
	}
	if (chunk.size() > 0)
	{
		hash_ctx->update(chunk);
		hash_done += chunk.size();
		if (hash_total > 0)
		{
			progress->set_value(Math::clamp(100.0 * hash_done / hash_total, 0.0, 100.0));
			progress_lbl->set_text(String::utf8("Verifying\xE2\x80\xA6 ") + fmt_size((int64_t)hash_done) + " / " + fmt_size((int64_t)hash_total));
		}
		return;
	}
	hash_file->close();
	hash_file.unref();
	const String digest = hash_ctx->finish().hex_encode();
	hash_ctx.unref();
	if (digest.to_lower() != String(job_pack.get("sha256", "")).to_lower())
	{
		finish_job_failed(String::utf8("sha256 mismatch \xE2\x80\x94 file may be corrupt."));
		return;
	}
	begin_extract();
}

// On Windows the bundled tar.exe (libarchive) handles ZIP64 and long paths
// cleanly, which Godot's ZIPReader does not for the multi-GB bundles; the
// ZIPReader is the fallback everywhere else.
void SceneryDownloaderPanel::begin_extract()
{
	progress_lbl->set_text(String::utf8("Extracting\xE2\x80\xA6"));
	const String dest_root = root_edit->get_text().strip_edges();
	job_top_level = String();
	if (!DirAccess::dir_exists_absolute(dest_root) && DirAccess::make_dir_recursive_absolute(dest_root) != OK)
	{
		finish_job_failed("Extraction failed: cannot create install root " + dest_root);
		return;
	}
	// Probe the zip to record the top-level dir (the install state's "path").
	zip.instantiate();
	const Error open_err = zip->open(job_zip_path);
	if (open_err == OK)
	{
		const PackedStringArray files = zip->get_files();
		for (int i = 0; i < files.size(); i++)
		{
			const PackedStringArray parts = files[i].split("/", false);
			if (parts.size() > 0)
			{
				job_top_level = parts[0];
				break;
			}
		}
		zip->close();
	}
	if (OS::get_singleton()->get_name() == "Windows")
	{
		PackedStringArray args;
		args.push_back("-xf");
		args.push_back(job_zip_path);
		args.push_back("-C");
		args.push_back(dest_root);
		tar_pid = OS::get_singleton()->create_process("tar.exe", args);
		if (tar_pid > 0)
		{
			progress->set_value(0.0);
			progress_lbl->set_text(String::utf8("Extracting via tar.exe\xE2\x80\xA6"));
			tar_poll_t = 0.0;
			phase = PHASE_EXTRACT_TAR;
			return;
		}
		UtilityFunctions::push_warning("SceneryDownloader: OS.create_process('tar.exe') failed - falling back to ZIPReader.");
	}
	if (open_err != OK)
	{
		finish_job_failed(String("Extraction failed: ZIPReader.open() returned ") + String::num_int64((int)open_err));
		return;
	}
	if (zip->open(job_zip_path) != OK)
	{
		finish_job_failed("Extraction failed: ZIPReader.open() failed");
		return;
	}
	zip_files = zip->get_files();
	zip_index = 0;
	dirs_made = Dictionary();
	phase = PHASE_EXTRACT_ZIP;
}

void SceneryDownloaderPanel::step_extract_tar(double delta)
{
	OS *os = OS::get_singleton();
	if (job_cancelled)
	{
		os->kill(tar_pid);
		finish_job_failed("Extraction failed: cancelled");
		return;
	}
	if (os->is_process_running(tar_pid))
	{
		tar_poll_t += delta;
		if (tar_poll_t >= 0.5)
		{
			tar_poll_t = 0.0;
			progress_lbl->set_text(String::utf8("Extracting via tar.exe\xE2\x80\xA6 (") + String::num_int64(count_files_under(root_edit->get_text().strip_edges(), 4000)) + " files)");
		}
		return;
	}
	const int exit_code = os->get_process_exit_code(tar_pid);
	tar_pid = -1;
	if (exit_code != 0)
	{
		finish_job_failed(String("Extraction failed: tar.exe exited with code ") + String::num_int64(exit_code));
		return;
	}
	progress->set_value(100.0);
	progress_lbl->set_text("Extraction complete (tar.exe)");
	finish_extract_ok();
}

// A batch of entries per frame, streamed to disk.
void SceneryDownloaderPanel::step_extract_zip()
{
	const String dest_root = root_edit->get_text().strip_edges();
	const int total = zip_files.size();
	for (int n = 0; n < ZIP_ENTRIES_PER_FRAME && zip_index < total; n++, zip_index++)
	{
		if (job_cancelled)
		{
			zip->close();
			finish_job_failed("Extraction failed: cancelled");
			return;
		}
		const String name = zip_files[zip_index];
		if (name.ends_with("/"))
		{
			continue; // bare directory entry
		}
		const String out_path = dest_root.path_join(name);
		const String out_dir = out_path.get_base_dir();
		if (!dirs_made.has(out_dir))
		{
			if (DirAccess::make_dir_recursive_absolute(out_dir) != OK && !DirAccess::dir_exists_absolute(out_dir))
			{
				zip->close();
				finish_job_failed("Extraction failed: cannot create dir " + out_dir);
				return;
			}
			dirs_made[out_dir] = true;
		}
		const PackedByteArray data = zip->read_file(name);
		if (data.size() == 0)
		{
			// A genuinely empty file or a read failure past the ZIP64 boundary.
			UtilityFunctions::push_warning("SceneryDownloader: ZIPReader.read_file('", name, "') returned 0 bytes");
		}
		Ref<FileAccess> f = FileAccess::open(out_path, FileAccess::WRITE);
		if (f.is_null())
		{
			const Error oserr = FileAccess::get_open_error();
			zip->close();
			finish_job_failed(String("Extraction failed: cannot write ") + out_path + " (FileAccess err=" + String::num_int64((int)oserr) + ")");
			return;
		}
		f->store_buffer(data);
		f->close();
		if (job_top_level.is_empty())
		{
			const PackedStringArray parts = name.split("/", false);
			if (parts.size() > 0)
			{
				job_top_level = parts[0];
			}
		}
	}
	if (zip_index < total)
	{
		progress->set_value(Math::clamp(100.0 * (double)zip_index / (double)total, 0.0, 100.0));
		progress_lbl->set_text(String("Extracting ") + String::num_int64(zip_index) + " / " + String::num_int64(total));
		return;
	}
	zip->close();
	progress->set_value(100.0);
	progress_lbl->set_text(String("Extracted ") + String::num_int64(total) + " files");
	finish_extract_ok();
}

void SceneryDownloaderPanel::finish_extract_ok()
{
	DirAccess::remove_absolute(job_zip_path); // best-effort cleanup
	// The recorded "path" is the region directory the scenery library
	// scanner expects: the zip's top-level dir joined onto the install root.
	const String install_root = root_edit->get_text().strip_edges();
	const String top_dir = top_level_dir_for_pack(install_root);
	Dictionary state = get_state.is_valid() ? Dictionary(get_state.call()) : Dictionary();
	Dictionary entry;
	entry["version"] = job_pack.get("version", "1");
	entry["sha256"] = job_pack.get("sha256", "");
	entry["size"] = job_pack.get("size", 0);
	entry["path"] = top_dir;
	entry["installed_at"] = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	state[job_pack_id] = entry;
	if (set_state.is_valid())
	{
		set_state.call(state);
	}
	if (!top_dir.is_empty() && on_area_installed.is_valid())
	{
		on_area_installed.call(top_dir);
	}
	emit_signal("install_complete", job_pack_id, top_dir);
	set_status(String("Installed ") + job_pack_id + String::utf8(" \xE2\x86\x92 ") + top_dir, OK_COLOR);
	job_pack_id = String();
	job_pack = Dictionary();
	job_zip_path = String();
	phase = PHASE_IDLE;
	set_job_ui_visible(false);
	refresh_packs_table();
}

void SceneryDownloaderPanel::_on_cancel_pressed()
{
	if (job_pack_id.is_empty())
	{
		return;
	}
	job_cancelled = true;
	if (phase == PHASE_DOWNLOADING)
	{
		http->cancel_request();
	}
}

void SceneryDownloaderPanel::finish_job_failed(const String &msg)
{
	UtilityFunctions::push_warning("SceneryDownloader: ", msg, " (", job_pack_id, ")");
	emit_signal("install_failed", job_pack_id, msg);
	if (!job_zip_path.is_empty() && FileAccess::file_exists(job_zip_path))
	{
		DirAccess::remove_absolute(job_zip_path);
	}
	set_status(msg, ERR_COLOR);
	job_pack_id = String();
	job_pack = Dictionary();
	job_zip_path = String();
	phase = PHASE_IDLE;
	set_job_ui_visible(false);
	refresh_packs_table();
}

void SceneryDownloaderPanel::_process(double delta)
{
	switch (phase)
	{
		case PHASE_HASHING: step_hashing(); break;
		case PHASE_EXTRACT_TAR: step_extract_tar(delta); break;
		case PHASE_EXTRACT_ZIP: step_extract_zip(); break;
		default: break;
	}
}

// ── Paths ───────────────────────────────────────────────────────────────────

// Walk down the unique single-child chain (BW uses Germany/Baden-Wuertemberg)
// so the recorded path matches what the asset detection looks for.
String SceneryDownloaderPanel::top_level_dir_for_pack(const String &install_root) const
{
	if (job_top_level.is_empty())
	{
		return install_root;
	}
	String cur = install_root.path_join(job_top_level);
	while (true)
	{
		Ref<DirAccess> d = DirAccess::open(cur);
		if (d.is_null())
		{
			break;
		}
		if (has_asset_subdir(cur))
		{
			return cur;
		}
		PackedStringArray subs;
		d->list_dir_begin();
		String n = d->get_next();
		while (!n.is_empty())
		{
			if (!n.begins_with(".") && d->current_is_dir())
			{
				subs.push_back(n);
			}
			n = d->get_next();
		}
		d->list_dir_end();
		if (subs.size() == 1)
		{
			cur = cur.path_join(subs[0]);
		}
		else
		{
			break;
		}
	}
	return cur;
}

bool SceneryDownloaderPanel::has_asset_subdir(const String &path)
{
	static const char *const subs[] = {"terrain", "orthophoto", "buildings", "water", "roads", "vegetation", "trees", "airports"};
	for (const char *s : subs)
	{
		if (DirAccess::dir_exists_absolute(path.path_join(s)))
		{
			return true;
		}
	}
	return false;
}

// Bounded directory walk for the tar.exe progress label.
int SceneryDownloaderPanel::count_files_under(const String &path, int limit)
{
	std::vector<String> stack;
	stack.push_back(path);
	int count = 0;
	while (!stack.empty() && count < limit)
	{
		const String cur = stack.back();
		stack.pop_back();
		Ref<DirAccess> d = DirAccess::open(cur);
		if (d.is_null())
		{
			continue;
		}
		d->list_dir_begin();
		String n = d->get_next();
		while (!n.is_empty() && count < limit)
		{
			if (!n.begins_with("."))
			{
				if (d->current_is_dir())
				{
					stack.push_back(cur.path_join(n));
				}
				else
				{
					count++;
				}
			}
			n = d->get_next();
		}
		d->list_dir_end();
	}
	return count;
}

void SceneryDownloaderPanel::forget_pack(const String &pack_id)
{
	Dictionary state = get_state.is_valid() ? Dictionary(get_state.call()) : Dictionary();
	if (state.erase(pack_id))
	{
		if (set_state.is_valid())
		{
			set_state.call(state);
		}
		refresh_packs_table();
	}
}

void SceneryDownloaderPanel::_on_browse_root()
{
	FileDialog *dialog = memnew(FileDialog);
	dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_DIR);
	dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	dialog->set_use_native_dialog(true);
	dialog->set_title("Choose Scenery Install Root");
	add_child(dialog);
	dialog->connect("dir_selected", Callable(this, "_on_root_dir_selected"));
	dialog->connect("dir_selected", Callable(dialog, "queue_free").unbind(1));
	dialog->connect("canceled", Callable(dialog, "queue_free"));
	dialog->popup_centered();
}

void SceneryDownloaderPanel::_on_root_dir_selected(const String &path)
{
	root_edit->set_text(path);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

void SceneryDownloaderPanel::set_status(const String &msg, const Color &color)
{
	status_lbl->set_text(msg);
	status_lbl->add_theme_color_override("font_color", color);
}

void SceneryDownloaderPanel::set_job_ui_visible(bool v)
{
	progress->set_visible(v);
	progress_lbl->set_visible(v);
	cancel_btn->set_visible(v);
}

String SceneryDownloaderPanel::fmt_size(int64_t n)
{
	static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
	double f = (double)n;
	for (int i = 0; i < 5; i++)
	{
		if (f < 1024.0 || i == 4)
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "%.1f %s", f, units[i]);
			return String(buf);
		}
		f /= 1024.0;
	}
	return String::num_int64(n);
}

int64_t SceneryDownloaderPanel::file_size(const String &path)
{
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null())
	{
		return -1;
	}
	const int64_t s = f->get_length();
	f->close();
	return s;
}

} // namespace jsbgodot
