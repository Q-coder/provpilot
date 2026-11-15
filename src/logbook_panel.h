#ifndef LOGBOOK_PANEL_H
#define LOGBOOK_PANEL_H

// Settings > Logbook (docs/LOGBOOK.md): the pilot's flights from logbook.json
// as a table, totals, and the "black box" of the selected flight (or of the
// flight that is running): its figures and the altitude / speed profile.

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

namespace jsbgodot
{
using namespace godot;

// Altitude (and speed) over time of one flight.
class FlightPlot : public Control
{
    GDCLASS(FlightPlot, Control)

protected:
    static void _bind_methods() {}

public:
    void set_samples(const Array &p_samples);
    void _draw() override;

private:
    Array samples;
};

class LogbookPanel : public VBoxContainer
{
    GDCLASS(LogbookPanel, VBoxContainer)

protected:
    static void _bind_methods();

public:
    void _ready() override;
    void refresh();
    void _on_black_box();
    void _on_selected();
    void _on_back();

private:
    String directory() const;
    void show_black_box(const Dictionary &entry, const Array &samples, bool running);
    static String hms(double seconds);

    Label *totals = nullptr;
    Tree *table = nullptr;
    Button *box_button = nullptr;
    VBoxContainer *list_view = nullptr;
    VBoxContainer *box_view = nullptr;
    Label *box_title = nullptr;
    Label *box_text = nullptr;
    FlightPlot *plot = nullptr;
    Array entries;       // as shown: [running flight?] + stored, newest first
    bool first_is_running = false;
};

} // namespace jsbgodot

#endif
