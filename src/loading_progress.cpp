#include "loading_progress.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/time.hpp>

#include <algorithm>

using namespace godot;

namespace jsbgodot
{

void LoadingProgress::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("begin_fly_loading"), &LoadingProgress::begin_fly_loading);
	ClassDB::bind_method(D_METHOD("update", "loaded", "total", "is_fly_loading", "terrain_init_pending", "rebase_cooldown", "rebase_cooldown_frames"), &LoadingProgress::update);
	ClassDB::bind_method(D_METHOD("get_status_text"), &LoadingProgress::get_status_text);
	ClassDB::bind_method(D_METHOD("is_complete"), &LoadingProgress::is_complete);
	ClassDB::bind_method(D_METHOD("get_phase_progress"), &LoadingProgress::get_phase_progress);
	ClassDB::bind_method(D_METHOD("get_phase"), &LoadingProgress::get_phase);
	ClassDB::bind_integer_constant("LoadingProgress", "Phase", "PHASE_SCENERY", PHASE_SCENERY);
	ClassDB::bind_integer_constant("LoadingProgress", "Phase", "PHASE_FDM_INIT", PHASE_FDM_INIT);
	ClassDB::bind_integer_constant("LoadingProgress", "Phase", "PHASE_STABILIZE", PHASE_STABILIZE);
	ClassDB::bind_integer_constant("LoadingProgress", "Phase", "PHASE_READY", PHASE_READY);
	ClassDB::bind_method(D_METHOD("set_scenery_weight", "weight"), &LoadingProgress::set_scenery_weight);
	ClassDB::bind_method(D_METHOD("set_cooldown_weight", "weight"), &LoadingProgress::set_cooldown_weight);
	ClassDB::bind_method(D_METHOD("set_fly_loading_max_msec", "msec"), &LoadingProgress::set_fly_loading_max_msec);
	ClassDB::bind_method(D_METHOD("set_fly_loading_nodata_msec", "msec"), &LoadingProgress::set_fly_loading_nodata_msec);
}

LoadingProgress::LoadingProgress()
{
}

void LoadingProgress::begin_fly_loading()
{
	_fly_loading_start_msec = static_cast<double>(Time::get_singleton()->get_ticks_msec());
	_phase = PHASE_SCENERY;
	_phase_start_msec = _fly_loading_start_msec;
	_phase_progress = 0.0;
}

double LoadingProgress::update(int loaded, int total, bool is_fly_loading, bool terrain_init_pending, int rebase_cooldown, int rebase_cooldown_frames)
{
	double p_scenery;
	bool scenery_ready;

	if (total > 0)
	{
		p_scenery = static_cast<double>(loaded) / static_cast<double>(total);
		scenery_ready = loaded >= total;
	}
	else
	{
		p_scenery = 1.0;
		scenery_ready = true;
	}

	// Hard timeout safety net - if scenery never reports ready (e.g. over nodata
	// territory), don't trap the user on the loading screen forever. Give up faster
	// when zero tiles have loaded.
	if (is_fly_loading && _fly_loading_start_msec > 0.0)
	{
		double elapsed = static_cast<double>(Time::get_singleton()->get_ticks_msec()) - _fly_loading_start_msec;
		double max_msec = (loaded == 0) ? fly_loading_nodata_msec : fly_loading_max_msec;
		if (elapsed > max_msec)
		{
			scenery_ready = true;
			p_scenery = 1.0;
		}
	}

	int cooldown_pending = is_fly_loading ? rebase_cooldown : 0;
	double p_cooldown = 1.0 - (static_cast<double>(cooldown_pending) / static_cast<double>(std::max(1, rebase_cooldown_frames)));

	double progress;
	if (is_fly_loading)
	{
		progress = p_scenery * scenery_weight + p_cooldown * cooldown_weight;
	}
	else
	{
		progress = p_scenery;
	}

	String status_text;
	int phase;
	double phase_progress;
	const double now_msec = static_cast<double>(Time::get_singleton()->get_ticks_msec());
	if (!scenery_ready)
	{
		status_text = "Loading scenery... (" + String::num_int64(loaded) + "/" + String::num_int64(total) + " tiles)";
		phase = PHASE_SCENERY;
		phase_progress = p_scenery;
	}
	else if (terrain_init_pending)
	{
		status_text = "Initialising flight model...";
		phase = PHASE_FDM_INIT;
		phase_progress = 0.0; // set below from the time in this phase
	}
	else if (cooldown_pending > 0)
	{
		status_text = "Stabilizing aircraft...";
		phase = PHASE_STABILIZE;
		phase_progress = p_cooldown;
	}
	else
	{
		status_text = "Ready";
		phase = PHASE_READY;
		phase_progress = 1.0;
	}
	if (phase != _phase)
	{
		_phase = phase;
		_phase_start_msec = now_msec;
	}
	if (phase == PHASE_FDM_INIT)
	{
		phase_progress = std::min((now_msec - _phase_start_msec) / 12000.0, 0.95);
	}

	bool complete = scenery_ready && !terrain_init_pending && cooldown_pending == 0;

	_progress = progress;
	_phase_progress = std::max(0.0, std::min(phase_progress, 1.0));
	_status_text = status_text;
	_complete = complete;

	return progress;
}

String LoadingProgress::get_status_text() const
{
	return _status_text;
}

bool LoadingProgress::is_complete() const
{
	return _complete;
}

double LoadingProgress::get_phase_progress() const
{
	return _phase_progress;
}

int LoadingProgress::get_phase() const
{
	return _phase;
}

void LoadingProgress::set_scenery_weight(double weight)
{
	scenery_weight = weight;
}

void LoadingProgress::set_cooldown_weight(double weight)
{
	cooldown_weight = weight;
}

void LoadingProgress::set_fly_loading_max_msec(double msec)
{
	fly_loading_max_msec = msec;
}

void LoadingProgress::set_fly_loading_nodata_msec(double msec)
{
	fly_loading_nodata_msec = msec;
}

} // namespace jsbgodot
