#include "settings_screen.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/file_dialog.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/h_slider.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace jsbgodot
{

// ── Constants ───────────────────────────────────────────────────────────────

// Fixed slider width: wide enough to drag precisely, without stretching a
// 2..16 range across the whole panel.
static constexpr int SLIDER_W = 520;

const Color SettingsScreen::BG_COLOR = Color(0.08, 0.10, 0.14, 1.0);
const Color SettingsScreen::PANEL_COLOR = Color(0.12, 0.14, 0.18, 1.0);
const Color SettingsScreen::SECTION_COLOR = Color(0.25, 0.55, 0.85, 1.0);
const Color SettingsScreen::LABEL_COLOR = Color(0.75, 0.78, 0.82, 1.0);
const Color SettingsScreen::HINT_COLOR = Color(0.6, 0.6, 0.6, 1.0);

// ── Binding ─────────────────────────────────────────────────────────────────

void SettingsScreen::_bind_methods()
{
    ADD_SIGNAL(MethodInfo("closed"));

    ClassDB::bind_method(D_METHOD("show_settings"), &SettingsScreen::show_settings);
    ClassDB::bind_method(D_METHOD("hide_settings"), &SettingsScreen::hide_settings);
    ClassDB::bind_method(D_METHOD("get_store"), &SettingsScreen::get_store);
    ClassDB::bind_method(D_METHOD("get_setting", "key"), &SettingsScreen::get_setting);
    ClassDB::bind_method(D_METHOD("refresh"), &SettingsScreen::refresh);
    ClassDB::bind_method(D_METHOD("commit"), &SettingsScreen::commit);

    // Signal targets (must be bound to be reachable through Callable).
    ClassDB::bind_method(D_METHOD("_on_save"), &SettingsScreen::_on_save);
    ClassDB::bind_method(D_METHOD("_on_cancel"), &SettingsScreen::_on_cancel);
    ClassDB::bind_method(D_METHOD("_on_restore_defaults"), &SettingsScreen::_on_restore_defaults);
    ClassDB::bind_method(D_METHOD("_on_scan_scenery"), &SettingsScreen::_on_scan_scenery);
    ClassDB::bind_method(D_METHOD("_on_add_scenery_area"), &SettingsScreen::_on_add_scenery_area);
    ClassDB::bind_method(D_METHOD("_on_area_dir_selected", "path"), &SettingsScreen::_on_area_dir_selected);
    ClassDB::bind_method(D_METHOD("_on_area_dialog_closed"), &SettingsScreen::_on_area_dialog_closed);
    ClassDB::bind_method(D_METHOD("_on_move_selected_up"), &SettingsScreen::_on_move_selected_up);
    ClassDB::bind_method(D_METHOD("_on_move_selected_down"), &SettingsScreen::_on_move_selected_down);
    ClassDB::bind_method(D_METHOD("_on_remove_selected"), &SettingsScreen::_on_remove_selected);
    ClassDB::bind_method(D_METHOD("_on_area_toggled", "enabled", "idx"), &SettingsScreen::_on_area_toggled);
    ClassDB::bind_method(D_METHOD("_on_area_priority_changed", "value", "idx"), &SettingsScreen::_on_area_priority_changed);
    ClassDB::bind_method(D_METHOD("_on_area_row_gui_input", "event", "idx"), &SettingsScreen::_on_area_row_gui_input);
    ClassDB::bind_method(D_METHOD("_on_slider_value_changed", "value", "row"), &SettingsScreen::_on_slider_value_changed);
    ClassDB::bind_method(D_METHOD("_on_time_slider_value_changed", "value", "row"), &SettingsScreen::_on_time_slider_value_changed);
    ClassDB::bind_method(D_METHOD("_downloader_get_state"), &SettingsScreen::_downloader_get_state);
    ClassDB::bind_method(D_METHOD("_downloader_set_state", "state"), &SettingsScreen::_downloader_set_state);
    ClassDB::bind_method(D_METHOD("_downloader_area_installed", "path"), &SettingsScreen::_downloader_area_installed);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

SettingsScreen::SettingsScreen()
{
    store.instantiate();
    set_layer(101); // above the welcome screen
    set_visible(false);
}

void SettingsScreen::_ready()
{
    store->load();
    _build_ui();
    _apply_to_ui();
}

void SettingsScreen::_input(const Ref<InputEvent> &event)
{
    if (!is_visible())
    {
        return;
    }
    Ref<InputEventKey> key = event;
    if (key.is_valid() && key->is_pressed() && key->get_keycode() == KEY_ESCAPE)
    {
        _on_cancel();
        get_viewport()->set_input_as_handled();
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

void SettingsScreen::show_settings()
{
    _apply_to_ui();
    set_visible(true);
}

void SettingsScreen::hide_settings()
{
    set_visible(false);
}

Ref<SettingsStore> SettingsScreen::get_store() const
{
    return store;
}

Variant SettingsScreen::get_setting(const String &key) const
{
    return store->get_setting(key);
}

void SettingsScreen::refresh()
{
    _apply_to_ui();
}

void SettingsScreen::commit()
{
    _read_from_ui();
}

// ── UI construction ─────────────────────────────────────────────────────────

void SettingsScreen::_build_ui()
{
    ColorRect *bg = memnew(ColorRect);
    bg->set_color(BG_COLOR);
    bg->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    add_child(bg);

    MarginContainer *margin = memnew(MarginContainer);
    margin->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    margin->add_theme_constant_override("margin_left", 32);
    margin->add_theme_constant_override("margin_right", 32);
    margin->add_theme_constant_override("margin_top", 32);
    margin->add_theme_constant_override("margin_bottom", 32);
    bg->add_child(margin);

    PanelContainer *panel = memnew(PanelContainer);
    Ref<StyleBoxFlat> style;
    style.instantiate();
    style->set_bg_color(PANEL_COLOR);
    style->set_corner_radius_all(16);
    style->set_content_margin(SIDE_LEFT, 48);
    style->set_content_margin(SIDE_RIGHT, 48);
    style->set_content_margin(SIDE_TOP, 36);
    style->set_content_margin(SIDE_BOTTOM, 36);
    panel->add_theme_stylebox_override("panel", style);
    panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    margin->add_child(panel);

    VBoxContainer *main_vbox = memnew(VBoxContainer);
    main_vbox->add_theme_constant_override("separation", 16);
    panel->add_child(main_vbox);

    Label *title = _make_label("Settings", FONT_XL, SECTION_COLOR);
    title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    main_vbox->add_child(title);
    main_vbox->add_child(memnew(HSeparator));

    // One tab per subject.
    tabs = memnew(TabContainer);
    tabs->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    tabs->add_theme_font_size_override("font_size", FONT_MD);
    main_vbox->add_child(tabs);

    _build_scenery_tab();
    _build_downloads_tab();
    _build_graphics_tab();
    _build_world_tab();
    _build_map_tab();
    _build_sound_tab();
    _build_realism_tab();
    _build_joystick_tab();

    // ── Bottom buttons ──
    main_vbox->add_child(memnew(HSeparator));

    HBoxContainer *btn_hbox = memnew(HBoxContainer);
    btn_hbox->add_theme_constant_override("separation", 16);
    main_vbox->add_child(btn_hbox);

    btn_hbox->add_child(_make_button("Restore Defaults", 220, FONT_MD, Callable(this, "_on_restore_defaults")));

    Control *spacer = memnew(Control);
    spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    btn_hbox->add_child(spacer);

    btn_hbox->add_child(_make_button("Cancel", 160, FONT_MD, Callable(this, "_on_cancel")));

    Control *save_btn = _make_button("Save", 160, FONT_MD, Callable(this, "_on_save"));
    Ref<StyleBoxFlat> save_style;
    save_style.instantiate();
    save_style->set_bg_color(SECTION_COLOR);
    save_style->set_corner_radius_all(6);
    save_btn->add_theme_stylebox_override("normal", save_style);
    save_btn->add_theme_color_override("font_color", Color(1, 1, 1));
    btn_hbox->add_child(save_btn);
}

// A scrollable tab page; returns the VBox that receives the rows.
VBoxContainer *SettingsScreen::_add_tab(const String &title)
{
    ScrollContainer *scroll = memnew(ScrollContainer);
    scroll->set_name(title);
    scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
    tabs->add_child(scroll);

    MarginContainer *margin = memnew(MarginContainer);
    margin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    margin->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    margin->add_theme_constant_override("margin_left", 16);
    margin->add_theme_constant_override("margin_right", 16);
    margin->add_theme_constant_override("margin_top", 20);
    margin->add_theme_constant_override("margin_bottom", 16);
    scroll->add_child(margin);

    VBoxContainer *content = memnew(VBoxContainer);
    content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    content->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    content->add_theme_constant_override("separation", 10);
    margin->add_child(content);
    return content;
}

Label *SettingsScreen::_make_label(const String &text, int font_size, const Color &color, int min_w)
{
    Label *l = memnew(Label);
    l->set_text(text);
    l->add_theme_font_size_override("font_size", font_size);
    l->add_theme_color_override("font_color", color);
    if (min_w > 0)
    {
        l->set_custom_minimum_size(Vector2(min_w, 0));
    }
    return l;
}

void SettingsScreen::_add_section_header(VBoxContainer *parent, const String &text)
{
    parent->add_child(_make_label(text, FONT_LG, SECTION_COLOR));
}

void SettingsScreen::_add_hint(VBoxContainer *parent, const String &text)
{
    Label *l = _make_label(text, FONT_SM, HINT_COLOR);
    l->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
    parent->add_child(l);
}

Control *SettingsScreen::_make_button(const String &text, int min_w, int font_size, const Callable &on_pressed)
{
    Button *b = memnew(Button);
    b->set_text(text);
    b->set_custom_minimum_size(Vector2(min_w, font_size >= FONT_MD ? 48 : 36));
    b->add_theme_font_size_override("font_size", font_size);
    b->connect("pressed", on_pressed);
    return b;
}

void SettingsScreen::_add_slider_row(VBoxContainer *parent, const String &key, const String &label, double min_v, double max_v, double step, const String &suffix)
{
    HBoxContainer *hbox = memnew(HBoxContainer);
    hbox->add_theme_constant_override("separation", 12);
    hbox->add_child(_make_label(label, FONT_MD, LABEL_COLOR, LABEL_MIN_W));

    HSlider *slider = memnew(HSlider);
    slider->set_min(min_v);
    slider->set_max(max_v);
    slider->set_step(step);
    slider->set_custom_minimum_size(Vector2(SLIDER_W, 32));
    hbox->add_child(slider);

    Label *value_label = _make_label("", FONT_SM, LABEL_COLOR, 70);
    hbox->add_child(value_label);
    parent->add_child(hbox);

    Row row;
    row.key = key;
    row.kind = ROW_SLIDER;
    row.control = slider;
    row.value_label = value_label;
    row.suffix = suffix;
    rows.push_back(row);
    slider->connect("value_changed", Callable(this, "_on_slider_value_changed").bind(int(rows.size()) - 1));
}

// Time-of-day slider (minutes past midnight) with an HH:MM readout. Used
// only when "Use System Clock" is off.
void SettingsScreen::_add_time_slider_row(VBoxContainer *parent, const String &key, const String &label)
{
    HBoxContainer *hbox = memnew(HBoxContainer);
    hbox->add_theme_constant_override("separation", 12);
    hbox->add_child(_make_label(label, FONT_MD, LABEL_COLOR, LABEL_MIN_W));

    HSlider *slider = memnew(HSlider);
    slider->set_min(0);
    slider->set_max(1439);
    slider->set_step(15);
    slider->set_custom_minimum_size(Vector2(SLIDER_W, 32));
    hbox->add_child(slider);

    Label *value_label = _make_label("", FONT_SM, LABEL_COLOR, 70);
    hbox->add_child(value_label);
    parent->add_child(hbox);

    Row row;
    row.key = key;
    row.kind = ROW_TIME_SLIDER;
    row.control = slider;
    row.value_label = value_label;
    rows.push_back(row);
    slider->connect("value_changed", Callable(this, "_on_time_slider_value_changed").bind(int(rows.size()) - 1));
}

void SettingsScreen::_add_check_row(VBoxContainer *parent, const String &key, const String &label, const String &description)
{
    HBoxContainer *hbox = memnew(HBoxContainer);
    hbox->add_theme_constant_override("separation", 12);
    hbox->add_child(_make_label(label, FONT_MD, LABEL_COLOR, LABEL_MIN_W));

    CheckBox *check = memnew(CheckBox);
    check->add_theme_font_size_override("font_size", FONT_MD);
    hbox->add_child(check);

    if (!description.is_empty())
    {
        hbox->add_child(_make_label(description, FONT_SM, Color(0.5, 0.5, 0.5)));
    }
    parent->add_child(hbox);

    Row row;
    row.key = key;
    row.kind = ROW_CHECK;
    row.control = check;
    rows.push_back(row);
}

void SettingsScreen::_add_option_row(VBoxContainer *parent, const String &key, const String &label, const PackedStringArray &options)
{
    HBoxContainer *hbox = memnew(HBoxContainer);
    hbox->add_theme_constant_override("separation", 12);
    hbox->add_child(_make_label(label, FONT_MD, LABEL_COLOR, LABEL_MIN_W));

    OptionButton *option = memnew(OptionButton);
    option->add_theme_font_size_override("font_size", FONT_MD);
    option->set_custom_minimum_size(Vector2(200, 40));
    option->get_popup()->add_theme_font_size_override("font_size", FONT_MD);
    for (int i = 0; i < options.size(); i++)
    {
        option->add_item(options[i]);
    }
    hbox->add_child(option);
    parent->add_child(hbox);

    Row row;
    row.key = key;
    row.kind = ROW_OPTION;
    row.control = option;
    rows.push_back(row);
}

Node *SettingsScreen::_instantiate_script(const String &res_path)
{
    Ref<Script> script = ResourceLoader::get_singleton()->load(res_path);
    if (script.is_null())
    {
        UtilityFunctions::push_warning("SettingsScreen: cannot load ", res_path);
        return nullptr;
    }
    Object *obj = script->call("new");
    Node *node = Object::cast_to<Node>(obj);
    if (node == nullptr)
    {
        UtilityFunctions::push_warning("SettingsScreen: ", res_path, " did not produce a Node");
    }
    return node;
}

// ── Tab: Scenery (FSX-style library) ────────────────────────────────────────

// Column widths shared by the header row and the data rows so they line up
// (the Pri column matches the SpinBox's rendered width).
static constexpr int COL_ON_W = 50;
static constexpr int COL_PRI_W = 84;
static constexpr int COL_NAME_W = 300;
static constexpr int COL_ASSETS_W = 560;

void SettingsScreen::_build_scenery_tab()
{
    VBoxContainer *content = _add_tab("Scenery");
    _add_section_header(content, "Scenery Library");
    _add_hint(content, "Available scenery areas. Lower priority = checked first (1 = highest). Disabled areas are skipped. Multiple enabled areas with the same asset type act as fallbacks (e.g. BW orthophotos fill SWISSIMAGE white pixels at the CH/DE border). Click a row to select it.");

    ScrollContainer *table_scroll = memnew(ScrollContainer);
    table_scroll->set_custom_minimum_size(Vector2(0, 200));
    table_scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    table_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
    content->add_child(table_scroll);

    scenery_table = memnew(VBoxContainer);
    scenery_table->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    scenery_table->add_theme_constant_override("separation", 2);
    table_scroll->add_child(scenery_table);

    // Header row + separator stay at indices 0/1; data rows follow.
    HBoxContainer *header = memnew(HBoxContainer);
    header->add_theme_constant_override("separation", 8);
    header->add_child(_make_label("On", FONT_SM, LABEL_COLOR, COL_ON_W));
    header->add_child(_make_label("Pri", FONT_SM, LABEL_COLOR, COL_PRI_W));
    header->add_child(_make_label("Name", FONT_SM, LABEL_COLOR, COL_NAME_W));
    header->add_child(_make_label("Assets", FONT_SM, LABEL_COLOR, COL_ASSETS_W));
    header->add_child(_make_label("Path", FONT_SM, LABEL_COLOR, 0));
    scenery_table->add_child(header);
    scenery_table->add_child(memnew(HSeparator));

    HBoxContainer *btn_row = memnew(HBoxContainer);
    btn_row->add_theme_constant_override("separation", 12);
    btn_row->add_child(_make_button("Scan for Scenery", 200, FONT_SM, Callable(this, "_on_scan_scenery")));
    btn_row->add_child(_make_button("Add Area...", 140, FONT_SM, Callable(this, "_on_add_scenery_area")));
    btn_row->add_child(_make_button("Move Up", 120, FONT_SM, Callable(this, "_on_move_selected_up")));
    btn_row->add_child(_make_button("Move Down", 120, FONT_SM, Callable(this, "_on_move_selected_down")));
    btn_row->add_child(_make_button("Remove", 120, FONT_SM, Callable(this, "_on_remove_selected")));
    content->add_child(btn_row);

    content->add_child(memnew(HSeparator));
    _add_check_row(content, "skip_white_pixels", "Skip White Pixels:", "Treat pure white ortho pixels as transparent so lower-priority sources fill in (CH/DE border fix)");
}

void SettingsScreen::_refresh_scenery_table()
{
    if (scenery_table == nullptr)
    {
        return;
    }
    // Drop existing data rows (keep header at 0 + separator at 1).
    for (int i = scenery_table->get_child_count() - 1; i >= 2; i--)
    {
        Node *child = scenery_table->get_child(i);
        scenery_table->remove_child(child);
        child->queue_free();
    }

    Array areas = store->get_scenery_areas();
    for (int i = 0; i < areas.size(); i++)
    {
        Dictionary area = areas[i];
        const bool selected = i == selected_area_idx;

        // Row background doubles as the click-to-select target; labels
        // ignore the mouse so clicks fall through to it.
        PanelContainer *row_panel = memnew(PanelContainer);
        row_panel->set_mouse_filter(Control::MOUSE_FILTER_STOP);
        if (selected)
        {
            Ref<StyleBoxFlat> sel;
            sel.instantiate();
            sel->set_bg_color(Color(SECTION_COLOR.r, SECTION_COLOR.g, SECTION_COLOR.b, 0.25));
            sel->set_corner_radius_all(4);
            row_panel->add_theme_stylebox_override("panel", sel);
        }
        else
        {
            Ref<StyleBoxEmpty> none;
            none.instantiate();
            row_panel->add_theme_stylebox_override("panel", none);
        }
        row_panel->connect("gui_input", Callable(this, "_on_area_row_gui_input").bind(i));

        HBoxContainer *row = memnew(HBoxContainer);
        row->add_theme_constant_override("separation", 8);
        row_panel->add_child(row);

        CheckBox *cb = memnew(CheckBox);
        cb->set_pressed(bool(area.get("enabled", true)));
        cb->set_custom_minimum_size(Vector2(COL_ON_W, 28));
        cb->connect("toggled", Callable(this, "_on_area_toggled").bind(i));
        row->add_child(cb);

        SpinBox *pri = memnew(SpinBox);
        pri->set_min(1);
        pri->set_max(999);
        pri->set_step(1);
        pri->set_value(int(area.get("priority", i + 1)));
        pri->set_custom_minimum_size(Vector2(COL_PRI_W, 28));
        pri->connect("value_changed", Callable(this, "_on_area_priority_changed").bind(i));
        row->add_child(pri);

        // Fixed-width, clipped columns keep rows aligned with the header; the
        // full text stays available in the tooltip.
        String name = String(area.get("name", "?"));
        Label *name_lbl = _make_label(name, FONT_SM, Color(1, 1, 1), COL_NAME_W);
        name_lbl->set_tooltip_text(name);
        name_lbl->set_clip_text(true);
        name_lbl->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
        name_lbl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        row->add_child(name_lbl);

        String path = String(area.get("path", ""));
        String assets = String(", ").join(store->detect_assets(path));
        Label *assets_lbl = _make_label(assets, FONT_SM, Color(0.55, 0.78, 0.55), COL_ASSETS_W);
        assets_lbl->set_tooltip_text(assets);
        assets_lbl->set_clip_text(true);
        assets_lbl->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
        assets_lbl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        row->add_child(assets_lbl);

        // $HOME collapsed to ~ and middle-elided so the distinguishing tail is
        // always visible; the full path stays in the tooltip.
        Label *path_lbl = _make_label(SettingsStore::format_path_for_display(path, 60), FONT_SM, Color(0.55, 0.55, 0.55), 200);
        path_lbl->set_tooltip_text(path);
        path_lbl->set_clip_text(true);
        path_lbl->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
        path_lbl->set_h_size_flags(Control::SIZE_EXPAND_FILL);
        // Labels ignore the mouse by default, which would also swallow the
        // tooltip; PASS keeps both the tooltip and the row click.
        path_lbl->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        row->add_child(path_lbl);

        if (selected)
        {
            row->add_child(_make_label(String::utf8("●"), FONT_SM, SECTION_COLOR, 20));
        }
        scenery_table->add_child(row_panel);
    }
}

void SettingsScreen::_select_area(int idx)
{
    selected_area_idx = idx;
    _refresh_scenery_table();
}

void SettingsScreen::_on_area_row_gui_input(const Ref<InputEvent> &event, int idx)
{
    Ref<InputEventMouseButton> mb = event;
    if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT)
    {
        _select_area(idx);
    }
}

void SettingsScreen::_on_area_toggled(bool enabled, int idx)
{
    store->set_area_enabled(idx, enabled);
}

void SettingsScreen::_on_area_priority_changed(double value, int idx)
{
    store->set_area_priority(idx, int(value));
}

void SettingsScreen::_on_scan_scenery()
{
    store->scan_scenery();
    _refresh_scenery_table();
}

void SettingsScreen::_on_add_scenery_area()
{
    if (area_dialog != nullptr)
    {
        return; // already open
    }
    FileDialog *dialog = memnew(FileDialog);
    dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_DIR);
    dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
    dialog->set_use_native_dialog(true);
    dialog->set_title("Select Scenery Area Directory");
    add_child(dialog);
    dialog->connect("dir_selected", Callable(this, "_on_area_dir_selected"));
    dialog->connect("canceled", Callable(this, "_on_area_dialog_closed"));
    area_dialog = dialog;
    dialog->popup_centered();
}

void SettingsScreen::_on_area_dir_selected(const String &path)
{
    store->add_scenery_area(path);
    _refresh_scenery_table();
    _on_area_dialog_closed();
}

void SettingsScreen::_on_area_dialog_closed()
{
    if (area_dialog != nullptr)
    {
        area_dialog->queue_free();
        area_dialog = nullptr;
    }
}

void SettingsScreen::_on_move_selected_up()
{
    if (store->swap_scenery_priority(selected_area_idx, selected_area_idx - 1))
    {
        _select_area(selected_area_idx - 1);
    }
}

void SettingsScreen::_on_move_selected_down()
{
    if (store->swap_scenery_priority(selected_area_idx, selected_area_idx + 1))
    {
        _select_area(selected_area_idx + 1);
    }
}

void SettingsScreen::_on_remove_selected()
{
    if (store->remove_scenery_area(selected_area_idx))
    {
        _select_area(-1);
    }
}

// ── Tab: Downloads (scenery_downloader.gd) ──────────────────────────────────

void SettingsScreen::_build_downloads_tab()
{
    VBoxContainer *content = _add_tab("Downloads");
    downloader_panel = _instantiate_script("res://scenery_downloader.gd");
    if (downloader_panel == nullptr)
    {
        _add_hint(content, "Scenery downloader unavailable.");
        return;
    }
    Control *panel = Object::cast_to<Control>(downloader_panel);
    if (panel != nullptr)
    {
        panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    }
    // The downloader reads/writes installed_scenery_packs through these
    // callbacks instead of depending on this screen directly.
    downloader_panel->set("get_state", Callable(this, "_downloader_get_state"));
    downloader_panel->set("set_state", Callable(this, "_downloader_set_state"));
    downloader_panel->set("on_area_installed", Callable(this, "_downloader_area_installed"));
    content->add_child(downloader_panel);

    // Initial install-root suggestion: configured scenery_root, else
    // <install>/scenery.
    String root;
    InstallConfig *ir = get_node<InstallConfig>(NodePath("/root/InstallRoot"));
    if (ir != nullptr)
    {
        root = ir->get_scenery_root();
        if (root.is_empty())
        {
            root = ir->get_install_root().path_join("scenery");
        }
    }
    downloader_panel->call("bootstrap", root);
}

Dictionary SettingsScreen::_downloader_get_state() const
{
    Variant s = store->get_setting("installed_scenery_packs");
    return s.get_type() == Variant::DICTIONARY ? Dictionary(s) : Dictionary();
}

void SettingsScreen::_downloader_set_state(const Dictionary &state)
{
    store->set_setting("installed_scenery_packs", state);
}

void SettingsScreen::_downloader_area_installed(const String &path)
{
    store->add_scenery_area(path);
    _refresh_scenery_table();
}

// ── Tabs: plain rows ────────────────────────────────────────────────────────

void SettingsScreen::_build_graphics_tab()
{
    VBoxContainer *content = _add_tab("Graphics");
    _add_section_header(content, "Graphics");
    _add_slider_row(content, "render_distance", "Terrain Distance:", 2, 16, 1, " tiles");
    _add_slider_row(content, "building_distance", "Building Distance:", 1, 8, 1, " tiles");
    _add_option_row(content, "shadow_quality", "Shadow Quality:", PackedStringArray{ "Off", "Low", "Medium", "High" });
    _add_option_row(content, "tree_density", "Tree Density:", PackedStringArray{ "Off", "Low", "Medium", "High", "Very High" });
    _add_check_row(content, "vsync", "V-Sync:", "Synchronize with display refresh");
    _add_check_row(content, "fullscreen", "Fullscreen:", "Run in fullscreen mode");
}

void SettingsScreen::_build_world_tab()
{
    VBoxContainer *content = _add_tab("World");
    _add_section_header(content, "Time of Day");
    _add_check_row(content, "use_system_time", "Use System Clock:", "Drive the sun from the real time of day");
    _add_time_slider_row(content, "time_of_day_min", "Time of Day:");
    _add_hint(content, "Dragging the time slider switches off the system clock.");
}

void SettingsScreen::_build_map_tab()
{
    VBoxContainer *content = _add_tab("Map");
    _add_section_header(content, "VFR Map");
    _add_check_row(content, "map_enabled", "Show Map Frame:", "Heading-up VFR map inset (N toggles in flight)");
    _add_option_row(content, "map_frame_size", "Frame Size:", PackedStringArray{ "Small", "Medium", "Large" });
}

void SettingsScreen::_build_sound_tab()
{
    VBoxContainer *content = _add_tab("Sound");
    _add_section_header(content, "Sound");
    _add_slider_row(content, "master_volume", "Master Volume:", 0, 100, 5, "%");
    _add_slider_row(content, "engine_volume", "Engine Sound:", 0, 100, 5, "%");
    _add_slider_row(content, "wind_volume", "Wind Sound:", 0, 100, 5, "%");
    _add_slider_row(content, "ui_volume", "UI Sounds:", 0, 100, 5, "%");
}

void SettingsScreen::_build_realism_tab()
{
    VBoxContainer *content = _add_tab("Realism");
    _add_section_header(content, "Realism");
    _add_check_row(content, "auto_mixture", "Auto Mixture:", "Automatically manage mixture control");
    _add_check_row(content, "auto_carb_heat", "Auto Carb Heat:", "Automatically manage carburetor heat");
    _add_check_row(content, "unlimited_fuel", "Unlimited Fuel:", "Fuel never runs out");
    _add_check_row(content, "crash_detection", "Crash Detection:", "Enable crash on hard impact");
    _add_option_row(content, "turbulence", "Turbulence:", PackedStringArray{ "None", "Light", "Moderate", "Severe" });
    _add_slider_row(content, "wind_speed_kts", "Wind Speed:", 0, 50, 1, " kts");
    _add_slider_row(content, "wind_direction_deg", "Wind Direction:", 0, 360, 10, String::utf8("°"));
}

// ── Tab: Joystick (joystick_settings_panel.gd) ──────────────────────────────

void SettingsScreen::_build_joystick_tab()
{
    VBoxContainer *content = _add_tab("Joystick");
    joystick_panel = _instantiate_script("res://joystick_settings_panel.gd");
    if (joystick_panel == nullptr)
    {
        _add_hint(content, "Joystick panel unavailable.");
        return;
    }
    content->add_child(joystick_panel);
}

Node *SettingsScreen::_joystick_config() const
{
    return get_node_or_null(NodePath("/root/JoystickConfig"));
}

void SettingsScreen::_refresh_joystick_panel()
{
    if (joystick_panel != nullptr && joystick_panel->has_method("refresh"))
    {
        joystick_panel->call("refresh");
    }
}

// ── State <-> UI ────────────────────────────────────────────────────────────

SettingsScreen::Row *SettingsScreen::_find_row(const String &key)
{
    for (Row &r : rows)
    {
        if (r.key == key)
        {
            return &r;
        }
    }
    return nullptr;
}

void SettingsScreen::_apply_to_ui()
{
    applying_ui = true;
    for (Row &r : rows)
    {
        Variant val = store->get_setting(r.key);
        switch (r.kind)
        {
            case ROW_SLIDER:
            {
                HSlider *s = Object::cast_to<HSlider>(r.control);
                s->set_value(double(val));
                r.value_label->set_text(vformat("%d%s", int(val), r.suffix));
                break;
            }
            case ROW_TIME_SLIDER:
            {
                HSlider *s = Object::cast_to<HSlider>(r.control);
                s->set_value(double(val));
                r.value_label->set_text(SettingsStore::format_hhmm(int(val)));
                break;
            }
            case ROW_CHECK:
                Object::cast_to<CheckBox>(r.control)->set_pressed(bool(val));
                break;
            case ROW_OPTION:
                Object::cast_to<OptionButton>(r.control)->select(int(val));
                break;
        }
    }
    _refresh_scenery_table();
    applying_ui = false;
}

void SettingsScreen::_read_from_ui()
{
    for (const Row &r : rows)
    {
        switch (r.kind)
        {
            case ROW_SLIDER:
            case ROW_TIME_SLIDER:
                store->set_setting(r.key, int(Object::cast_to<HSlider>(r.control)->get_value()));
                break;
            case ROW_CHECK:
                store->set_setting(r.key, Object::cast_to<CheckBox>(r.control)->is_pressed());
                break;
            case ROW_OPTION:
                store->set_setting(r.key, Object::cast_to<OptionButton>(r.control)->get_selected());
                break;
        }
    }
}

void SettingsScreen::_on_slider_value_changed(double value, int row)
{
    if (row < 0 || row >= int(rows.size()))
    {
        return;
    }
    rows[row].value_label->set_text(vformat("%d%s", int(value), rows[row].suffix));
}

void SettingsScreen::_on_time_slider_value_changed(double value, int row)
{
    if (row < 0 || row >= int(rows.size()))
    {
        return;
    }
    rows[row].value_label->set_text(SettingsStore::format_hhmm(int(value)));
    // Dragging the time implies manual mode — turn off the system clock so
    // the change actually drives the sun.
    if (!applying_ui)
    {
        Row *sys = _find_row("use_system_time");
        if (sys != nullptr)
        {
            Object::cast_to<CheckBox>(sys->control)->set_pressed(false);
        }
    }
}

// ── Bottom-bar actions ──────────────────────────────────────────────────────

void SettingsScreen::_on_save()
{
    commit();
    store->save();
    Node *jc = _joystick_config();
    if (jc != nullptr)
    {
        jc->call("save");
    }
    hide_settings();
    emit_signal("closed");
}

void SettingsScreen::_on_cancel()
{
    // Revert to the last saved state.
    store->load();
    _apply_to_ui();
    // Reload joystick bindings from disk so unsaved changes are discarded.
    Node *jc = _joystick_config();
    if (jc != nullptr)
    {
        jc->call("reset_to_defaults");
        jc->call("_load");
        _refresh_joystick_panel();
    }
    hide_settings();
    emit_signal("closed");
}

void SettingsScreen::_on_restore_defaults()
{
    store->restore_defaults();
    _apply_to_ui();
    Node *jc = _joystick_config();
    if (jc != nullptr)
    {
        jc->call("reset_to_defaults");
        _refresh_joystick_panel();
    }
}

} // namespace jsbgodot
