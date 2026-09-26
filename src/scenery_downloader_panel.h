#ifndef SCENERY_DOWNLOADER_PANEL_H
#define SCENERY_DOWNLOADER_PANEL_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/hashing_context.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/zip_reader.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    // Settings > Downloads tab (formerly scenery_downloader.gd): fetches the
    // remote scenery manifest, lets the user download / install region
    // "base" and "detail" packs, verifies sha256, extracts them into a
    // configurable install root and tracks the installed pack state.
    //
    // State (persisted through the get_state / set_state callables as
    // SettingsStore "installed_scenery_packs"):
    //   { "<pack_id>": {version, sha256, size, path, installed_at}, ... }
    // A pack is "installed" when its sha256 matches the manifest; an update
    // is offered when it differs.
    //
    // The long steps (hashing, extraction) run as a per-frame state machine
    // so the UI stays responsive.
    class SceneryDownloaderPanel : public VBoxContainer
    {
        GDCLASS(SceneryDownloaderPanel, VBoxContainer);

    public:
        static void _bind_methods();
        SceneryDownloaderPanel();

        void _ready() override;
        void _process(double delta) override;

        // Initial install-root text (and an optional manifest URL).
        void bootstrap(const String &initial_root, const String &manifest_url = String());

        Callable get_get_state() const; void set_get_state(const Callable &c);
        Callable get_set_state() const; void set_set_state(const Callable &c);
        Callable get_get_install_root() const; void set_get_install_root(const Callable &c);
        Callable get_on_area_installed() const; void set_on_area_installed(const Callable &c);

        void _on_refresh_pressed();
        void _on_manifest_completed(int result, int code, const PackedStringArray &headers, const PackedByteArray &body);
        void _on_download_completed(int result, int code, const PackedStringArray &headers, const PackedByteArray &body);
        void _on_progress_tick();
        void _on_cancel_pressed();
        void _on_browse_root();
        void _on_root_dir_selected(const String &path);
        void _on_pack_pressed(const Dictionary &pack, const String &pack_id);
        void _on_forget_pressed(const String &pack_id);

    protected:
        enum Phase { PHASE_IDLE, PHASE_DOWNLOADING, PHASE_HASHING, PHASE_EXTRACT_TAR, PHASE_EXTRACT_ZIP };
        static constexpr int64_t HASH_CHUNK = 4 * 1024 * 1024;
        static constexpr int ZIP_ENTRIES_PER_FRAME = 32;

        Callable get_state, set_state, get_install_root, on_area_installed;
        LineEdit *url_edit = nullptr;
        LineEdit *root_edit = nullptr;
        Button *refresh_btn = nullptr;
        Label *status_lbl = nullptr;
        VBoxContainer *packs_box = nullptr;
        ProgressBar *progress = nullptr;
        Label *progress_lbl = nullptr;
        Button *cancel_btn = nullptr;
        Dictionary manifest;
        HTTPRequest *http = nullptr;
        Timer *progress_timer = nullptr;

        // Active job
        Phase phase = PHASE_IDLE;
        String job_pack_id;
        Dictionary job_pack;
        String job_zip_path;
        String job_top_level;
        bool job_cancelled = false;
        // hashing
        Ref<FileAccess> hash_file;
        Ref<HashingContext> hash_ctx;
        double hash_total = 0.0, hash_done = 0.0;
        // tar.exe extraction
        int tar_pid = -1;
        double tar_poll_t = 0.0;
        // ZIPReader extraction
        Ref<ZIPReader> zip;
        PackedStringArray zip_files;
        int zip_index = 0;
        Dictionary dirs_made;

        void build_ui();
        void refresh_packs_table();
        void add_pack_row(const Dictionary &pack, const String &label, const String &pack_id, const Dictionary &installed);
        void start_install(const Dictionary &pack, const String &pack_id);
        void begin_hashing();
        void step_hashing();
        void begin_extract();
        void step_extract_tar(double delta);
        void step_extract_zip();
        void finish_extract_ok();
        void finish_job_failed(const String &msg);
        String top_level_dir_for_pack(const String &install_root) const;
        static bool has_asset_subdir(const String &path);
        static int count_files_under(const String &path, int limit);
        void forget_pack(const String &pack_id);
        void set_status(const String &msg, const Color &color);
        void set_job_ui_visible(bool v);
        static String fmt_size(int64_t n);
        static int64_t file_size(const String &path);
        void reconnect_http(const char *method);
    };

} // namespace jsbgodot

#endif
