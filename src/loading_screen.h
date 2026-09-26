#ifndef LOADING_SCREEN_H
#define LOADING_SCREEN_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

namespace jsbgodot
{

    // Lightweight full-screen overlay (title, progress bar, status text) shown
    // while a destination loads whenever the intro zoom cannot play. Stays on
    // top of everything (layer 200, above welcome 100 and settings 101) and
    // swallows input while visible. Formerly loading_screen.gd.
    class LoadingScreen : public CanvasLayer
    {
        GDCLASS(LoadingScreen, CanvasLayer);

    public:
        static void _bind_methods();
        LoadingScreen();

        void _ready() override;
        void _unhandled_input(const Ref<InputEvent> &event) override;

        void set_status(const String &text);
        void set_progress(double value); // 0..1
        void show_loading();
        // Hides the overlay and emits `loading_complete`.
        void hide_loading();

    protected:
        ColorRect *bg = nullptr;
        ColorRect *progress_bar = nullptr;
        ColorRect *progress_bg = nullptr;
        Label *status_label = nullptr;
        double progress = 0.0;

        void build_ui();
    };

} // namespace jsbgodot

#endif
