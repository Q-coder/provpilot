#include "engine_sound.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>

using namespace godot;

namespace jsbgodot
{

void EngineSound::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "audio"), &EngineSound::register_nodes);
	ClassDB::bind_method(D_METHOD("set_pitch_range", "min", "max"), &EngineSound::set_pitch_range);
	ClassDB::bind_method(D_METHOD("set_vol_db_range", "min", "max"), &EngineSound::set_vol_db_range);
	ClassDB::bind_method(D_METHOD("set_max_rpm", "max_rpm"), &EngineSound::set_max_rpm);
	ClassDB::bind_method(D_METHOD("update", "rpm", "user_volume_pct"), &EngineSound::update);
}

EngineSound::EngineSound()
{
}

void EngineSound::register_nodes(AudioStreamPlayer *p_audio)
{
	audio = p_audio;
}

void EngineSound::set_pitch_range(double p_min, double p_max)
{
	pitch_min = p_min;
	pitch_max = p_max;
}

void EngineSound::set_vol_db_range(double p_min, double p_max)
{
	vol_db_min = p_min;
	vol_db_max = p_max;
}

void EngineSound::set_max_rpm(double p_max_rpm)
{
	max_rpm = p_max_rpm;
}

void EngineSound::update(double rpm, double user_volume_pct)
{
	if (audio == nullptr)
	{
		return;
	}

	// Fade to silence below the low-RPM threshold.
	if (rpm < fade_rpm)
	{
		audio->set_volume_db(-80.0);
		return;
	}

	// Normalise RPM to 0-1 (idle ~700 = 0.26, max 2700 = 1.0). A non-positive
	// max_rpm yields inf here, which clamp() resolves to 1.0 — matching the
	// historical GDScript behaviour.
	double rpm_norm = Math::clamp(rpm / max_rpm, 0.0, 1.0);

	// Pitch: linear interpolation.
	audio->set_pitch_scale(Math::lerp(pitch_min, pitch_max, rpm_norm));

	// Volume: linear interpolation in dB, then apply the user engine-volume
	// setting as a dB offset (0% -> -80 dB silent, 100% -> 0 dB no attenuation).
	double base_db = Math::lerp(vol_db_min, vol_db_max, rpm_norm);
	double user_db_offset = Math::lerp(-80.0, 0.0, user_volume_pct / 100.0);
	audio->set_volume_db(base_db + user_db_offset);
}

} // namespace jsbgodot