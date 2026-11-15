#include "logbook_panel.h"

#include "flight_log.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

namespace jsbgodot
{

static const Color SECTION_COLOR(0.55, 0.78, 1.0);
static const Color TEXT_COLOR(0.88, 0.88, 0.88);
static const Color HINT_COLOR(0.6, 0.6, 0.6);

static Label *lb(const String &text, int size, const Color &color)
{
	Label *l = memnew(Label);
	l->set_text(text);
	l->add_theme_font_size_override("font_size", size);
	l->add_theme_color_override("font_color", color);
	return l;
}

// ── FlightPlot ───────────────────────────────────────────────────────────────

void FlightPlot::set_samples(const Array &p_samples)
{
	samples = p_samples;
	queue_redraw();
}

void FlightPlot::_draw()
{
	const Vector2 size = get_size();
	draw_rect(Rect2(Vector2(), size), Color(0.07, 0.08, 0.10));
	const Ref<Font> font = get_theme_default_font();
	if (samples.size() < 2)
	{
		if (font.is_valid())
		{
			draw_string(font, Vector2(16, size.y * 0.5), "No track recorded for this flight", HORIZONTAL_ALIGNMENT_LEFT, -1, 24, HINT_COLOR);
		}
		return;
	}
	double t1 = 1.0, alt_lo = 1e9, alt_hi = -1e9, ias_hi = 1.0;
	for (int i = 0; i < samples.size(); i++)
	{
		const Array s = samples[i];
		if (s.size() < 5) continue;
		t1 = Math::max(t1, (double)s[0]);
		alt_lo = Math::min(alt_lo, (double)s[3]);
		alt_hi = Math::max(alt_hi, (double)s[3]);
		ias_hi = Math::max(ias_hi, (double)s[4]);
	}
	if (alt_hi - alt_lo < 200.0)
	{
		alt_hi = alt_lo + 200.0;
	}
	const double left = 110.0, right = 90.0, top = 18.0, bottom = 40.0;
	const double w = size.x - left - right, h = size.y - top - bottom;
	if (w < 20.0 || h < 20.0)
	{
		return;
	}
	const Color grid(0.22, 0.24, 0.28), alt_col(0.45, 0.80, 1.0), ias_col(1.0, 0.72, 0.35);
	for (int g = 0; g <= 4; g++)
	{
		const double y = top + h * g / 4.0;
		draw_line(Vector2(left, y), Vector2(left + w, y), grid, 1.0);
		if (font.is_valid())
		{
			draw_string(font, Vector2(6, y + 7), String::num(alt_hi - (alt_hi - alt_lo) * g / 4.0, 0) + " ft", HORIZONTAL_ALIGNMENT_LEFT, -1, 20, alt_col);
			draw_string(font, Vector2(left + w + 8, y + 7), String::num(ias_hi * (1.0 - g / 4.0), 0) + " kt", HORIZONTAL_ALIGNMENT_LEFT, -1, 20, ias_col);
		}
	}
	for (int g = 0; g <= 4; g++)
	{
		const double x = left + w * g / 4.0;
		draw_line(Vector2(x, top), Vector2(x, top + h), grid, 1.0);
		if (font.is_valid())
		{
			draw_string(font, Vector2(x - 30, top + h + 28), String::num(t1 * g / 4.0 / 60.0, 1) + " min", HORIZONTAL_ALIGNMENT_LEFT, -1, 20, HINT_COLOR);
		}
	}
	PackedVector2Array alt_line, ias_line;
	for (int i = 0; i < samples.size(); i++)
	{
		const Array s = samples[i];
		if (s.size() < 5) continue;
		const double x = left + w * (double)s[0] / t1;
		alt_line.push_back(Vector2(x, top + h * (1.0 - ((double)s[3] - alt_lo) / (alt_hi - alt_lo))));
		ias_line.push_back(Vector2(x, top + h * (1.0 - (double)s[4] / ias_hi)));
	}
	if (alt_line.size() >= 2)
	{
		draw_polyline(ias_line, ias_col, 1.5, true);
		draw_polyline(alt_line, alt_col, 2.0, true);
	}
}

// ── LogbookPanel ─────────────────────────────────────────────────────────────

void LogbookPanel::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("refresh"), &LogbookPanel::refresh);
	ClassDB::bind_method(D_METHOD("_on_black_box"), &LogbookPanel::_on_black_box);
	ClassDB::bind_method(D_METHOD("_on_selected"), &LogbookPanel::_on_selected);
	ClassDB::bind_method(D_METHOD("_on_back"), &LogbookPanel::_on_back);
}

String LogbookPanel::directory() const
{
	FlightLog *fl = FlightLog::get_active();
	if (fl != nullptr && !fl->get_directory().is_empty())
	{
		return fl->get_directory();
	}
	if (is_inside_tree())
	{
		Node *ir = get_node_or_null(NodePath("/root/InstallRoot"));
		if (ir != nullptr && ir->has_method("get_install_root"))
		{
			return ir->call("get_install_root");
		}
	}
	return OS::get_singleton()->get_user_data_dir();
}

String LogbookPanel::hms(double seconds)
{
	const int s = (int)std::lround(seconds);
	return vformat("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}

void LogbookPanel::_ready()
{
	if (Engine::get_singleton()->is_editor_hint())
	{
		return;
	}
	set_h_size_flags(Control::SIZE_EXPAND_FILL);
	set_v_size_flags(Control::SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 8);

	list_view = memnew(VBoxContainer);
	list_view->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	list_view->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	list_view->add_theme_constant_override("separation", 8);
	add_child(list_view);
	list_view->add_child(lb("Logbook", 32, SECTION_COLOR));
	totals = lb("", 24, TEXT_COLOR);
	list_view->add_child(totals);
	table = memnew(Tree);
	table->set_columns(8);
	table->set_column_titles_visible(true);
	table->set_hide_root(true);
	table->set_select_mode(Tree::SELECT_ROW);
	static const char *const titles[8] = {"Start", "Aircraft", "From", "To", "Time", "Distance", "Landings", "Result"};
	static const int widths[8] = {300, 360, 220, 220, 140, 160, 130, 300};
	for (int c = 0; c < 8; c++)
	{
		table->set_column_title(c, titles[c]);
		table->set_column_custom_minimum_width(c, widths[c]);
		table->set_column_expand(c, c == 7);
	}
	table->add_theme_font_size_override("font_size", 22);
	table->add_theme_font_size_override("title_button_font_size", 22);
	table->set_custom_minimum_size(Vector2(0, 560));
	table->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	table->connect("item_selected", Callable(this, "_on_selected"));
	table->connect("item_activated", Callable(this, "_on_black_box"));
	list_view->add_child(table);
	HBoxContainer *row = memnew(HBoxContainer);
	box_button = memnew(Button);
	box_button->set_text("Black Box");
	box_button->set_custom_minimum_size(Vector2(240, 56));
	box_button->add_theme_font_size_override("font_size", 24);
	box_button->connect("pressed", Callable(this, "_on_black_box"));
	row->add_child(box_button);
	row->add_child(lb("  the figures and the altitude / speed profile of the selected flight", 20, HINT_COLOR));
	list_view->add_child(row);

	box_view = memnew(VBoxContainer);
	box_view->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	box_view->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	box_view->add_theme_constant_override("separation", 8);
	box_view->set_visible(false);
	add_child(box_view);
	box_title = lb("Black box", 32, SECTION_COLOR);
	box_view->add_child(box_title);
	box_text = lb("", 24, TEXT_COLOR);
	box_view->add_child(box_text);
	plot = memnew(FlightPlot);
	plot->set_custom_minimum_size(Vector2(0, 300));
	plot->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	plot->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	box_view->add_child(plot);
	Button *back = memnew(Button);
	back->set_text("Back to the logbook");
	back->set_custom_minimum_size(Vector2(320, 56));
	back->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	back->add_theme_font_size_override("font_size", 24);
	back->connect("pressed", Callable(this, "_on_back"));
	box_view->add_child(back);
	refresh();
}

void LogbookPanel::refresh()
{
	if (table == nullptr)
	{
		return;
	}
	list_view->set_visible(true);
	box_view->set_visible(false);
	const String dir = directory();
	const Array stored = FlightLog::read_entries(dir);
	entries = Array();
	first_is_running = false;
	FlightLog *fl = FlightLog::get_active();
	if (fl != nullptr && fl->is_recording())
	{
		Dictionary cur = fl->current_entry();
		cur["to"] = "(in flight)";
		entries.push_back(cur);
		first_is_running = true;
	}
	entries.append_array(stored);
	double total_s = 0.0, total_nm = 0.0;
	int total_landings = 0, crashes = 0;
	for (int i = 0; i < stored.size(); i++)
	{
		const Dictionary e = stored[i];
		total_s += (double)e.get("duration_s", 0.0);
		total_nm += (double)e.get("distance_nm", 0.0);
		total_landings += (int)(double)e.get("landings", 0);
		crashes += String(e.get("result", "")).begins_with("Crash") ? 1 : 0;
	}
	totals->set_text(vformat("Pilot: Pilot      Flights: %d      Time: %s      Distance: %d nm      Landings: %d      Crashes: %d", stored.size(), hms(total_s), (int)std::lround(total_nm), total_landings, crashes));
	table->clear();
	TreeItem *root = table->create_item();
	for (int i = 0; i < entries.size(); i++)
	{
		const Dictionary e = entries[i];
		TreeItem *it = table->create_item(root);
		it->set_text(0, String(e.get("start_time", "")).replace("T", " "));
		it->set_text(1, String(e.get("aircraft_name", e.get("aircraft", ""))));
		it->set_text(2, String(e.get("from", "")));
		it->set_text(3, String(e.get("to", "")));
		it->set_text(4, hms((double)e.get("duration_s", 0.0)));
		it->set_text(5, String::num((double)e.get("distance_nm", 0.0), 1) + " nm");
		it->set_text(6, String::num_int64((int64_t)(double)e.get("landings", 0)));
		const String result = (i == 0 && first_is_running) ? String("Running") : String(e.get("result", ""));
		it->set_text(7, result);
		if (result.begins_with("Crash"))
		{
			it->set_custom_color(7, Color(1.0, 0.55, 0.45));
		}
		it->set_metadata(0, i);
	}
	box_button->set_disabled(entries.is_empty());
	if (!entries.is_empty() && root->get_first_child() != nullptr)
	{
		root->get_first_child()->select(0);
	}
}

void LogbookPanel::_on_selected() {}

void LogbookPanel::_on_back()
{
	list_view->set_visible(true);
	box_view->set_visible(false);
}

void LogbookPanel::_on_black_box()
{
	TreeItem *sel = table != nullptr ? table->get_selected() : nullptr;
	if (sel == nullptr)
	{
		return;
	}
	const int i = (int)sel->get_metadata(0);
	if (i < 0 || i >= entries.size())
	{
		return;
	}
	const Dictionary e = entries[i];
	const bool running = i == 0 && first_is_running;
	FlightLog *fl = FlightLog::get_active();
	const Array samples = running && fl != nullptr ? fl->current_samples() : FlightLog::read_samples(directory(), String(e.get("id", "")));
	show_black_box(e, samples, running);
}

void LogbookPanel::show_black_box(const Dictionary &e, const Array &samples, bool running)
{
	list_view->set_visible(false);
	box_view->set_visible(true);
	box_title->set_text(vformat("Black box: %s, %s", String(e.get("aircraft_name", "")), String(e.get("start_time", "")).replace("T", " ")));
	auto num = [&](const char *k) { return (double)e.get(k, 0.0); };
	String t;
	t += vformat("Pilot: %s\n", String(e.get("pilot", "Pilot")));
	t += vformat("From %s (%.4f, %.4f)", String(e.get("from", "")), num("from_lat"), num("from_lon"));
	if (!running)
	{
		t += vformat("   to %s (%.4f, %.4f)", String(e.get("to", "")), num("to_lat"), num("to_lon"));
	}
	t += "\n";
	t += vformat("Start %s", String(e.get("start_time", "")).replace("T", " "));
	t += running ? String("   (still running)\n") : vformat("   end %s\n", String(e.get("end_time", "")).replace("T", " "));
	t += vformat("Time %s   in the air %s   distance %.1f nm\n", hms(num("duration_s")), hms(num("air_time_s")), num("distance_nm"));
	t += vformat("Highest %d ft   fastest %d kt indicated, %d kt over the ground\n", (int)num("max_alt_ft"), (int)num("max_ias_kt"), (int)num("max_gs_kt"));
	t += vformat("Climb up to %d ft/min   descent up to %d ft/min   load %.2f to %.2f g\n", (int)num("max_climb_fpm"), (int)num("max_sink_fpm"), num("min_g"), num("max_g"));
	t += vformat("Take-offs %d   landings %d   hardest touchdown %d ft/min\n", (int)num("takeoffs"), (int)num("landings"), (int)num("hardest_landing_fpm"));
	t += vformat("Result: %s", running ? String("Running") : String(e.get("result", "")));
	box_text->set_text(t);
	plot->set_samples(samples);
}

} // namespace jsbgodot
