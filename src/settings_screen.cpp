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
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
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
    ClassDB::bind_method(D_METHOD("get_aircraft_panel"), &SettingsScreen::get_aircraft_panel);
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
    ClassDB::bind_method(D_METHOD("_on_weather_add_layer"), &SettingsScreen::_on_weather_add_layer);
    ClassDB::bind_method(D_METHOD("_on_live_weather_toggled", "pressed"), &SettingsScreen::_on_live_weather_toggled);
    ClassDB::bind_method(D_METHOD("_on_weather_profile_selected", "idx"), &SettingsScreen::_on_weather_profile_selected);
    ClassDB::bind_method(D_METHOD("_on_weather_profile_load"), &SettingsScreen::_on_weather_profile_load);
    ClassDB::bind_method(D_METHOD("_on_weather_profile_save"), &SettingsScreen::_on_weather_profile_save);
    ClassDB::bind_method(D_METHOD("_on_weather_profile_delete"), &SettingsScreen::_on_weather_profile_delete);
    ClassDB::bind_method(D_METHOD("set_live_weather", "layers", "status"), &SettingsScreen::set_live_weather);
    ClassDB::bind_method(D_METHOD("clear_live_weather"), &SettingsScreen::clear_live_weather);
    ClassDB::bind_method(D_METHOD("_on_weather_remove_layer", "idx"), &SettingsScreen::_on_weather_remove_layer);
    ClassDB::bind_method(D_METHOD("_on_weather_spin_changed", "value", "idx"), &SettingsScreen::_on_weather_spin_changed);
    ClassDB::bind_method(D_METHOD("_on_weather_option_changed", "value", "idx"), &SettingsScreen::_on_weather_option_changed);
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
    _refresh_aircraft_panel();
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

    _build_aircraft_tab();
    _build_scenery_tab();
    _build_downloads_tab();
    _build_graphics_tab();
    _build_world_tab();
    _build_weather_tab();
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

Ref<ImageTexture> SettingsScreen::_check_icon(bool checked, bool disabled)
{
    const int S = 26;
    Ref<Image> img = Image::create(S, S, false, Image::FORMAT_RGBA8);
    const Color border = disabled ? Color(0.45, 0.47, 0.5) : Color(0.8, 0.82, 0.85);
    const Color fill = checked ? (disabled ? Color(0.3, 0.42, 0.55) : Color(0.24, 0.55, 0.9)) : Color(0.16, 0.18, 0.22);
    for (int y = 0; y < S; y++)
    {
        for (int x = 0; x < S; x++)
        {
            const bool edge = x < 2 || y < 2 || x >= S - 2 || y >= S - 2;
            img->set_pixel(x, y, edge ? border : fill);
        }
    }
    if (checked)
    {
        // Tick: two thick segments (6,13)->(11,18) and (11,18)->(20,8).
        const Color ink = Color(1, 1, 1);
        auto seg = [&](int x0, int y0, int x1, int y1) {
            const int n = MAX(abs(x1 - x0), abs(y1 - y0));
            for (int i = 0; i <= n; i++)
            {
                const int x = x0 + (x1 - x0) * i / n;
                const int y = y0 + (y1 - y0) * i / n;
                for (int dx = -1; dx <= 1; dx++)
                {
                    for (int dy = -1; dy <= 1; dy++)
                    {
                        const int px = x + dx, py = y + dy;
                        if (px >= 0 && py >= 0 && px < S && py < S)
                        {
                            img->set_pixel(px, py, ink);
                        }
                    }
                }
            }
        };
        seg(6, 13, 11, 18);
        seg(11, 18, 20, 8);
    }
    return ImageTexture::create_from_image(img);
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
    // The default theme's check icons are a small dark square that all but
    // vanishes on the dark panel: draw our own 26 px box + tick.
    check->add_theme_icon_override("unchecked", _check_icon(false, false));
    check->add_theme_icon_override("checked", _check_icon(true, false));
    check->add_theme_icon_override("unchecked_disabled", _check_icon(false, true));
    check->add_theme_icon_override("checked_disabled", _check_icon(true, true));
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
    _add_option_row(content, "cloud_quality", "Cloud Quality:", PackedStringArray{ "Off", "Low", "Medium", "High" });
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
    _add_hint(content, "Wind and turbulence moved to the Weather tab.");
}

// ── Tab: Weather ─────────────────────────────────────────────────────────────
//
// A list of weather layers, each spanning Base..Top (wind and clouds alike;
// gaps between layers are clear air), plus global visibility and QNH. Model only for now: the
// values are persisted and editable, the JSBSim/visual hookup follows
// (design doc §10 in WORLD_SCENERY_DESIGN.md).

static const char *WEATHER_CLOUD_TYPES[] = { "None", "Cumulus", "Stratus", "Stratocumulus", "Cumulonimbus", "Altostratus", "Altocumulus", "Cirrus" };
static const int WEATHER_CLOUD_TYPE_COUNT = 8;
static const char *WEATHER_TURBULENCE[] = { "None", "Light", "Moderate", "Severe" };

void SettingsScreen::_build_weather_tab()
{
    VBoxContainer *tab = _add_tab("Weather");
    // Two columns: the weather itself on the left, profile files on the right.
    HBoxContainer *split = memnew(HBoxContainer);
    split->add_theme_constant_override("separation", 24);
    split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    tab->add_child(split);
    VBoxContainer *content = memnew(VBoxContainer);
    content->add_theme_constant_override("separation", 8);
    content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    split->add_child(content);
    VBoxContainer *side = memnew(VBoxContainer);
    side->add_theme_constant_override("separation", 8);
    side->set_custom_minimum_size(Vector2(300, 0));
    split->add_child(side);

    // ── Right column: weather profile files (<install_root>/weather/*.json) ──
    _add_section_header(side, "Weather Profiles");
    _add_hint(side, "Saved in the weather folder of the ProVPilot data directory.");
    weather_profile_list = memnew(ItemList);
    weather_profile_list->set_custom_minimum_size(Vector2(0, 260));
    weather_profile_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
    weather_profile_list->add_theme_font_size_override("font_size", FONT_SM);
    weather_profile_list->connect("item_selected", Callable(this, "_on_weather_profile_selected"));
    side->add_child(weather_profile_list);
    HBoxContainer *name_row = memnew(HBoxContainer);
    name_row->add_theme_constant_override("separation", 8);
    name_row->add_child(_make_label("Name:", FONT_SM, LABEL_COLOR, 60));
    weather_profile_name = memnew(LineEdit);
    weather_profile_name->set_placeholder("profile name");
    weather_profile_name->add_theme_font_size_override("font_size", FONT_SM);
    weather_profile_name->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    name_row->add_child(weather_profile_name);
    side->add_child(name_row);
    HBoxContainer *btns = memnew(HBoxContainer);
    btns->add_theme_constant_override("separation", 8);
    btns->add_child(_make_button("Load", 90, FONT_SM, Callable(this, "_on_weather_profile_load")));
    btns->add_child(_make_button("Save", 90, FONT_SM, Callable(this, "_on_weather_profile_save")));
    btns->add_child(_make_button("Delete", 90, FONT_SM, Callable(this, "_on_weather_profile_delete")));
    side->add_child(btns);
    weather_profile_status = _make_label("", FONT_SM, HINT_COLOR);
    weather_profile_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    side->add_child(weather_profile_status);
    _refresh_weather_profiles();

    // ── Left column ──
    _add_section_header(content, "Source");
    _add_check_row(content, "live_weather", "Live Weather:", "Nearest METAR (aviationweather.gov), refreshed every 10 min");
    live_weather_check = Object::cast_to<CheckBox>(rows.back().control);
    live_weather_check->connect("toggled", Callable(this, "_on_live_weather_toggled"));
    weather_status = _make_label("", FONT_SM, HINT_COLOR);
    weather_status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    weather_status->set_h_size_flags(Control::SIZE_EXPAND_FILL);
    content->add_child(weather_status);
    _add_section_header(content, "Weather Layers");
    _add_hint(content, "Each layer spans Base to Top (wind and clouds); the air between layers is clear.");

    // Column headers.
    HBoxContainer *head = memnew(HBoxContainer);
    head->add_theme_constant_override("separation", 8);
    const char *cols[] = { "Layer", "Base (m)", "Top (m)", "Wind from", "Speed (kt)", "Turbulence", "Clouds", "Coverage", "" };
    const int widths[] = { 60, 110, 110, 110, 110, 150, 170, 100, 110 };
    for (int i = 0; i < 9; i++)
    {
        head->add_child(_make_label(cols[i], FONT_SM, HINT_COLOR, widths[i]));
    }
    content->add_child(head);

    weather_table = memnew(VBoxContainer);
    weather_table->add_theme_constant_override("separation", 6);
    content->add_child(weather_table);

    HBoxContainer *btn_row = memnew(HBoxContainer);
    btn_row->add_theme_constant_override("separation", 12);
    weather_add_button = Object::cast_to<Button>(_make_button("Add Layer", 160, FONT_SM, Callable(this, "_on_weather_add_layer")));
    btn_row->add_child(weather_add_button);
    content->add_child(btn_row);

    _add_section_header(content, "Global");
    _add_slider_row(content, "visibility_km", "Visibility:", 0, 200, 1, " km");
    _add_slider_row(content, "qnh_hpa", "Pressure (QNH):", 900, 1100, 1, " hPa");
    _add_hint(content, "Standard atmosphere: 1013 hPa. Wind, turbulence and QNH feed the flight model; visibility sets the haze. Cloud detail: Graphics > Cloud Quality.");
    _refresh_weather_table();
}

void SettingsScreen::_refresh_weather_table()
{
    if (weather_table == nullptr)
    {
        return;
    }
    for (int i = weather_table->get_child_count() - 1; i >= 0; i--)
    {
        Node *c = weather_table->get_child(i);
        weather_table->remove_child(c);
        c->queue_free();
    }
    weather_rows.clear();

    const bool live = _live_weather_shown();
    if (weather_status != nullptr)
    {
        if (live_weather_check != nullptr && live_weather_check->is_pressed())
        {
            weather_status->set_text(live_weather_status.is_empty() ? String("Live weather: waiting for a report... (static layers shown until it arrives)") : "Live: " + live_weather_status);
        }
        else
        {
            weather_status->set_text("Static weather: edit the layers below.");
        }
    }
    if (weather_add_button != nullptr)
    {
        weather_add_button->set_disabled(live);
    }

    Array layers = live ? live_weather_layers : Array(store->get_setting("weather_layers"));
    for (int i = 0; i < layers.size(); i++)
    {
        Dictionary l = layers[i];
        HBoxContainer *row = memnew(HBoxContainer);
        row->add_theme_constant_override("separation", 8);
        row->add_child(_make_label(vformat("%d", i + 1), FONT_SM, LABEL_COLOR, 60));

        WeatherRow wr;
        auto make_spin = [&](double min_v, double max_v, double step, double value, int min_w) -> SpinBox * {
            SpinBox *sb = memnew(SpinBox);
            sb->set_min(min_v);
            sb->set_max(max_v);
            sb->set_step(step);
            sb->set_value(value);
            sb->set_custom_minimum_size(Vector2(min_w, 36));
            sb->add_theme_font_size_override("font_size", FONT_SM);
            sb->connect("value_changed", Callable(this, "_on_weather_spin_changed").bind(i));
            sb->set_editable(!live);
            row->add_child(sb);
            return sb;
        };
        auto make_option = [&](const char *const *names, int count, int selected, int min_w) -> OptionButton * {
            OptionButton *ob = memnew(OptionButton);
            ob->add_theme_font_size_override("font_size", FONT_SM);
            ob->get_popup()->add_theme_font_size_override("font_size", FONT_SM);
            ob->set_custom_minimum_size(Vector2(min_w, 36));
            for (int k = 0; k < count; k++)
            {
                ob->add_item(names[k]);
            }
            ob->select(CLAMP(selected, 0, count - 1));
            ob->connect("item_selected", Callable(this, "_on_weather_option_changed").bind(i));
            ob->set_disabled(live);
            row->add_child(ob);
            return ob;
        };
        wr.base = make_spin(0, 20000, 100, double(l.get("base_m", 0)), 110);
        wr.top = make_spin(100, 20000, 100, double(l.get("top_m", 3000)), 110);
        wr.wind_dir = make_spin(0, 359, 5, double(l.get("wind_dir_deg", 0)), 110);
        wr.wind_speed = make_spin(0, 150, 1, double(l.get("wind_speed_kts", 0)), 110);
        wr.turbulence = make_option(WEATHER_TURBULENCE, 4, int(l.get("turbulence", 0)), 150);
        wr.cloud_type = make_option(WEATHER_CLOUD_TYPES, WEATHER_CLOUD_TYPE_COUNT, int(l.get("cloud_type", 0)), 170);
        static const char *oktas[] = { "0/8", "1/8", "2/8", "3/8", "4/8", "5/8", "6/8", "7/8", "8/8" };
        wr.coverage = make_option(oktas, 9, int(l.get("coverage_oktas", 0)), 100);
        Button *remove = Object::cast_to<Button>(_make_button("Remove", 110, FONT_SM, Callable(this, "_on_weather_remove_layer").bind(i)));
        remove->set_disabled(live);
        row->add_child(remove);
        weather_table->add_child(row);
        weather_rows.push_back(wr);
    }
}

bool SettingsScreen::_live_weather_shown() const
{
    return live_weather_check != nullptr && live_weather_check->is_pressed() && live_weather_layers.size() > 0;
}

void SettingsScreen::set_live_weather(const Array &layers, const String &status)
{
    live_weather_layers = layers;
    live_weather_status = status;
    _refresh_weather_table();
}

void SettingsScreen::clear_live_weather()
{
    live_weather_layers = Array();
    live_weather_status = String();
    _refresh_weather_table();
}

void SettingsScreen::_on_live_weather_toggled(bool)
{
    _refresh_weather_table();
}

void SettingsScreen::_refresh_weather_profiles()
{
    if (weather_profile_list == nullptr || store.is_null())
    {
        return;
    }
    weather_profile_list->clear();
    PackedStringArray names = store->list_weather_profiles();
    for (int i = 0; i < names.size(); i++)
    {
        weather_profile_list->add_item(names[i]);
    }
    if (weather_profile_status != nullptr && weather_profile_status->get_text().is_empty())
    {
        weather_profile_status->set_text(names.is_empty() ? String("No profiles yet: set up the layers and click Save.") : String());
    }
}

void SettingsScreen::_on_weather_profile_selected(int idx)
{
    if (weather_profile_list != nullptr && weather_profile_name != nullptr && idx >= 0 && idx < weather_profile_list->get_item_count())
    {
        weather_profile_name->set_text(weather_profile_list->get_item_text(idx));
    }
}

void SettingsScreen::_on_weather_profile_load()
{
    const String name = weather_profile_name != nullptr ? weather_profile_name->get_text().strip_edges() : String();
    if (name.is_empty())
    {
        weather_profile_status->set_text("Select a profile to load.");
        return;
    }
    // Keep the other tabs' edits, then swap the weather values in and
    // refresh the UI from the store.
    _read_from_ui();
    if (!store->load_weather_profile(name))
    {
        weather_profile_status->set_text("Could not load '" + name + "'.");
        return;
    }
    _apply_to_ui();
    weather_profile_status->set_text("Loaded '" + name + "'. Save the settings to apply.");
}

void SettingsScreen::_on_weather_profile_save()
{
    const String name = weather_profile_name != nullptr ? weather_profile_name->get_text().strip_edges() : String();
    if (name.is_empty())
    {
        weather_profile_status->set_text("Enter a name first.");
        return;
    }
    _read_from_ui();
    if (!store->save_weather_profile(name))
    {
        weather_profile_status->set_text("Could not save '" + name + "'.");
        return;
    }
    weather_profile_status->set_text("Saved '" + name + "'.");
    _refresh_weather_profiles();
    for (int i = 0; i < weather_profile_list->get_item_count(); i++)
    {
        if (weather_profile_list->get_item_text(i) == name)
        {
            weather_profile_list->select(i);
        }
    }
}

void SettingsScreen::_on_weather_profile_delete()
{
    const String name = weather_profile_name != nullptr ? weather_profile_name->get_text().strip_edges() : String();
    if (name.is_empty() || !store->delete_weather_profile(name))
    {
        weather_profile_status->set_text("Nothing to delete.");
        return;
    }
    weather_profile_status->set_text("Deleted '" + name + "'.");
    weather_profile_name->set_text("");
    _refresh_weather_profiles();
}

void SettingsScreen::_read_weather_table()
{
    if (applying_ui || _live_weather_shown())
    {
        return;
    }
    Array layers;
    for (const WeatherRow &wr : weather_rows)
    {
        Dictionary l;
        int base = int(wr.base->get_value());
        int top = int(wr.top->get_value());
        l["base_m"] = base;
        l["top_m"] = MAX(top, base + 100);  // keep the slab non-empty
        l["wind_dir_deg"] = int(wr.wind_dir->get_value());
        l["wind_speed_kts"] = int(wr.wind_speed->get_value());
        l["turbulence"] = wr.turbulence->get_selected();
        l["cloud_type"] = wr.cloud_type->get_selected();
        l["coverage_oktas"] = wr.coverage->get_selected();
        layers.push_back(l);
    }
    store->set_setting("weather_layers", layers);
}

void SettingsScreen::_on_weather_add_layer()
{
    _read_weather_table();
    Array layers = store->get_setting("weather_layers");
    Dictionary l;
    int base = 0;
    if (layers.size() > 0)
    {
        Dictionary last = layers[layers.size() - 1];
        base = MIN(int(last.get("top_m", 3000)), 18000);
    }
    l["base_m"] = base;
    l["top_m"] = MIN(base + 2000, 20000);
    l["wind_dir_deg"] = 0;
    l["wind_speed_kts"] = 0;
    l["turbulence"] = 0;
    l["cloud_type"] = 0;
    l["coverage_oktas"] = 0;
    layers.push_back(l);
    store->set_setting("weather_layers", layers);
    _refresh_weather_table();
}

void SettingsScreen::_on_weather_remove_layer(int idx)
{
    _read_weather_table();
    Array layers = store->get_setting("weather_layers");
    if (idx >= 0 && idx < layers.size() && layers.size() > 1)
    {
        layers.remove_at(idx);
        store->set_setting("weather_layers", layers);
    }
    _refresh_weather_table();
}

void SettingsScreen::_on_weather_spin_changed(double, int)
{
    _read_weather_table();
}

void SettingsScreen::_on_weather_option_changed(int, int)
{
    _read_weather_table();
}

// ── Tab: Aircraft (aircraft_settings_panel.gd) ──────────────────────────────
// Per-aircraft settings (fuel) and reference data for the aircraft selected
// on the welcome page; the host wires the panel's providers (node_3d.gd).

void SettingsScreen::_build_aircraft_tab()
{
    VBoxContainer *content = _add_tab("Aircraft");
    aircraft_panel = _instantiate_script("res://aircraft_settings_panel.gd");
    if (aircraft_panel == nullptr)
    {
        _add_hint(content, "Aircraft panel unavailable.");
        return;
    }
    content->add_child(aircraft_panel);
}

Node *SettingsScreen::get_aircraft_panel() const
{
    return aircraft_panel;
}

void SettingsScreen::_refresh_aircraft_panel()
{
    if (aircraft_panel != nullptr && aircraft_panel->has_method("refresh"))
    {
        aircraft_panel->call("refresh");
    }
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
    _refresh_weather_table();
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
    _refresh_aircraft_panel();
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
    _refresh_aircraft_panel();
}

} // namespace jsbgodot
