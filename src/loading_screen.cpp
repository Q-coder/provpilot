#include "loading_screen.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/box_container.hpp>
#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/viewport.hpp>

using namespace godot;

namespace jsbgodot
{

static const Color BG_COLOR(0.06, 0.08, 0.11, 1.0);
static const Color ACCENT_COLOR(0.25, 0.55, 0.85, 1.0);
static const Color BAR_BG_COLOR(0.15, 0.17, 0.21, 1.0);

void LoadingScreen::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("set_status", "text"), &LoadingScreen::set_status);
	ClassDB::bind_method(D_METHOD("set_progress", "value"), &LoadingScreen::set_progress);
	ClassDB::bind_method(D_METHOD("show_loading"), &LoadingScreen::show_loading);
	ClassDB::bind_method(D_METHOD("hide_loading"), &LoadingScreen::hide_loading);
	ADD_SIGNAL(MethodInfo("loading_complete"));
}

LoadingScreen::LoadingScreen()
{
}

void LoadingScreen::_ready()
{
	set_layer(200);
	build_ui();
}

void LoadingScreen::build_ui()
{
	if (bg != nullptr)
	{
		return;
	}
	bg = memnew(ColorRect);
	bg->set_color(BG_COLOR);
	bg->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(bg);

	CenterContainer *center = memnew(CenterContainer);
	center->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	bg->add_child(center);

	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	vbox->add_theme_constant_override("separation", 32);
	vbox->set_custom_minimum_size(Vector2(700, 300));
	center->add_child(vbox);

	Label *title = memnew(Label);
	title->set_text("ProVPilot");
	title->add_theme_font_size_override("font_size", 56);
	title->add_theme_color_override("font_color", ACCENT_COLOR);
	title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	vbox->add_child(title);

	Label *sub = memnew(Label);
	sub->set_text("Flight Simulator");
	sub->add_theme_font_size_override("font_size", 24);
	sub->add_theme_color_override("font_color", Color(0.5, 0.55, 0.6));
	sub->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	vbox->add_child(sub);

	Control *spacer = memnew(Control);
	spacer->set_custom_minimum_size(Vector2(0, 16));
	vbox->add_child(spacer);

	progress_bg = memnew(ColorRect);
	progress_bg->set_color(BAR_BG_COLOR);
	progress_bg->set_custom_minimum_size(Vector2(600, 12));
	vbox->add_child(progress_bg);

	progress_bar = memnew(ColorRect);
	progress_bar->set_color(ACCENT_COLOR);
	progress_bar->set_custom_minimum_size(Vector2(0, 12));
	progress_bar->set_size(Vector2(0, 12));
	progress_bg->add_child(progress_bar);

	status_label = memnew(Label);
	status_label->set_text("Initializing...");
	status_label->add_theme_font_size_override("font_size", 20);
	status_label->add_theme_color_override("font_color", Color(0.45, 0.48, 0.52));
	status_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	vbox->add_child(status_label);
}

void LoadingScreen::set_status(const String &text)
{
	if (status_label != nullptr)
	{
		status_label->set_text(text);
	}
}

void LoadingScreen::set_progress(double value)
{
	progress = Math::clamp(value, 0.0, 1.0);
	if (progress_bar != nullptr && progress_bg != nullptr)
	{
		progress_bar->set_custom_minimum_size(Vector2(progress_bg->get_custom_minimum_size().x * progress, 12));
		progress_bar->set_size(Vector2(progress_bg->get_size().x * progress, 12));
	}
}

void LoadingScreen::show_loading()
{
	set_visible(true);
}

void LoadingScreen::hide_loading()
{
	set_visible(false);
	emit_signal("loading_complete");
}

// Block all input while loading.
void LoadingScreen::_unhandled_input(const Ref<InputEvent> &event)
{
	if (is_visible() && is_inside_tree())
	{
		get_viewport()->set_input_as_handled();
	}
}

} // namespace jsbgodot
