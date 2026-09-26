#include "fg_sounds.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

using namespace godot;

namespace jsbgodot
{

void FGSounds::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("load", "json_path", "sounds_dir"), &FGSounds::load);
	ClassDB::bind_method(D_METHOD("clear"), &FGSounds::clear);
	ClassDB::bind_method(D_METHOD("update", "props", "delta"), &FGSounds::update);
	ClassDB::bind_method(D_METHOD("playing"), &FGSounds::playing);
	ClassDB::bind_method(D_METHOD("play_count", "name"), &FGSounds::play_count);
	ClassDB::bind_method(D_METHOD("volume_db_of", "name"), &FGSounds::volume_db_of);
	ClassDB::bind_method(D_METHOD("get_user_volume"), &FGSounds::get_user_volume);
	ClassDB::bind_method(D_METHOD("set_user_volume", "value"), &FGSounds::set_user_volume);
	ClassDB::bind_method(D_METHOD("get_camera_distance_m"), &FGSounds::get_camera_distance_m);
	ClassDB::bind_method(D_METHOD("set_camera_distance_m", "value"), &FGSounds::set_camera_distance_m);
	ClassDB::bind_method(D_METHOD("get_count"), &FGSounds::get_count);
	ClassDB::bind_method(D_METHOD("get_skipped"), &FGSounds::get_skipped);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "user_volume"), "set_user_volume", "get_user_volume");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "camera_distance_m"), "set_camera_distance_m", "get_camera_distance_m");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count"), "", "get_count");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "skipped"), "", "get_skipped");
}

FGSounds::FGSounds()
{
}

double FGSounds::get_user_volume() const { return user_volume; }
void FGSounds::set_user_volume(double v) { user_volume = v; }
double FGSounds::get_camera_distance_m() const { return camera_distance_m; }
void FGSounds::set_camera_distance_m(double v) { camera_distance_m = v; }
int FGSounds::get_count() const { return count; }
Array FGSounds::get_skipped() const { return skipped; }

Array FGSounds::as_list(const Variant &v)
{
	if (v.get_type() == Variant::ARRAY)
	{
		return v;
	}
	if (v.get_type() == Variant::NIL || (v.get_type() == Variant::STRING && String(v).is_empty()))
	{
		return Array();
	}
	Array a;
	a.push_back(v);
	return a;
}

String FGSounds::norm(const String &p)
{
	String s = p.strip_edges();
	while (s.begins_with("/"))
	{
		s = s.substr(1);
	}
	return s.replace("[0]", "");
}

double FGSounds::num(const Variant &v, double def)
{
	switch (v.get_type())
	{
		case Variant::FLOAT:
		case Variant::INT:
			return (double)v;
		case Variant::BOOL:
			return (bool)v ? 1.0 : 0.0;
		case Variant::STRING:
		{
			const String s = v;
			if (s.is_valid_float())
			{
				return s.to_float();
			}
			return s == "true" ? 1.0 : def;
		}
		default:
			return def;
	}
}

int FGSounds::load(const String &json_path, const String &sounds_dir)
{
	clear();
	Ref<FileAccess> f = FileAccess::open(json_path, FileAccess::READ);
	if (f.is_null())
	{
		return 0;
	}
	const Dictionary m = JSON::parse_string(f->get_as_text());
	Dictionary streams; // file (+ "#loop") -> AudioStreamWAV
	const Array recs = m.get("sounds", Array());
	for (int i = 0; i < recs.size(); i++)
	{
		const Dictionary rec = recs[i];
		const String name = rec.get("name", "");
		const String file = rec.get("file", "");
		if (file.is_empty())
		{
			skipped.push_back(name);
			continue;
		}
		const String mode = rec.get("mode", "once");
		const bool looping = mode == "looped" || mode == "in-transit";
		const String key = file + String(looping ? "#loop" : "");
		if (!streams.has(key))
		{
			Ref<AudioStreamWAV> st = AudioStreamWAV::load_from_file(sounds_dir.path_join(file));
			if (st.is_null())
			{
				skipped.push_back(name);
				continue;
			}
			if (looping)
			{
				st->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
				st->set_loop_begin(0);
				const int bytes_per_frame = (st->get_format() == AudioStreamWAV::FORMAT_16_BITS ? 2 : 1) * (st->is_stereo() ? 2 : 1);
				st->set_loop_end(st->get_data().size() / Math::max(bytes_per_frame, 1));
			}
			streams[key] = st;
		}
		AudioStreamPlayer *p = memnew(AudioStreamPlayer);
		p->set_name("snd_" + (name.is_empty() ? String("x") : name).validate_node_name());
		p->set_stream(streams[key]);
		p->set_bus("Master");
		p->set_volume_db(-80.0);
		add_child(p);
		Sound s;
		s.rec = rec;
		s.raw = rec.get("raw", Dictionary());
		s.player = p;
		s.name = name;
		s.mode = mode;
		s.cond = s.raw.get("condition", Variant());
		const Variant prop = s.raw.get("property", Variant());
		s.prop = prop.get_type() == Variant::STRING ? norm(prop) : String();
		s.vol = as_list(s.raw.get("volume", Array()));
		s.pitch = as_list(s.raw.get("pitch", Array()));
		sounds.push_back(s);
		count++;
	}
	UtilityFunctions::print("FGSounds: ", count, " sounds loaded, ", skipped.size(), " skipped (no file)");
	return count;
}

void FGSounds::clear()
{
	for (int i = 0; i < sounds.size(); i++)
	{
		if (sounds[i].player != nullptr)
		{
			sounds[i].player->queue_free();
		}
	}
	sounds.clear();
	skipped.clear();
	count = 0;
}

double FGSounds::val(const String &prop) const
{
	const String key = norm(prop);
	if (!props.has(key))
	{
		return 0.0;
	}
	return num(props[key]);
}

// FlightGear condition (same syntax as the animations): dict of operators.
bool FGSounds::cond(const Variant &c) const
{
	if (c.get_type() == Variant::NIL)
	{
		return true;
	}
	if (c.get_type() == Variant::STRING)
	{
		return val(c) != 0.0;
	}
	if (c.get_type() != Variant::DICTIONARY)
	{
		return true;
	}
	const Dictionary d = c;
	bool result = true;
	const Array keys = d.keys();
	for (int k = 0; k < keys.size(); k++)
	{
		const String key = keys[k];
		const Array items = as_list(d[keys[k]]);
		for (int i = 0; i < items.size(); i++)
		{
			const Variant body = items[i];
			bool r = true;
			if (key == "and")
			{
				r = cond(body);
			}
			else if (key == "or")
			{
				r = false;
				if (body.get_type() == Variant::DICTIONARY)
				{
					const Dictionary bd = body;
					const Array k2s = bd.keys();
					for (int a = 0; a < k2s.size(); a++)
					{
						const Array b2s = as_list(bd[k2s[a]]);
						for (int b = 0; b < b2s.size(); b++)
						{
							Dictionary one;
							one[k2s[a]] = b2s[b];
							if (cond(one))
							{
								r = true;
							}
						}
					}
				}
				else
				{
					r = cond(body);
				}
			}
			else if (key == "not")
			{
				r = !cond(body);
			}
			else if (key == "property")
			{
				r = val(body) != 0.0;
			}
			else if (key == "equals" || key == "not-equals" || key == "less-than" || key == "less-than-equals" ||
					key == "greater-than" || key == "greater-than-equals")
			{
				r = compare(key, body);
			}
			result = result && r;
		}
	}
	return result;
}

bool FGSounds::compare(const String &op, const Variant &body) const
{
	if (body.get_type() != Variant::DICTIONARY)
	{
		return false;
	}
	const Dictionary b = body;
	const Array ps = as_list(b.get("property", Array()));
	if (ps.is_empty())
	{
		return false;
	}
	const double a = val(ps[0]);
	if (b.has("value") && !String(b["value"]).is_valid_float())
	{
		const String key = norm(ps[0]);
		const String sv = props.has(key) ? String(props[key]) : String();
		return op == "equals" ? sv == String(b["value"]) : sv != String(b["value"]);
	}
	double rhs = 0.0;
	if (ps.size() > 1)
	{
		rhs = val(ps[1]);
	}
	else
	{
		const String sv = b.get("value", "0");
		rhs = sv.is_valid_float() ? sv.to_float() : 0.0;
	}
	if (op == "equals") return std::abs(a - rhs) < 1e-9;
	if (op == "not-equals") return std::abs(a - rhs) >= 1e-9;
	if (op == "less-than") return a < rhs;
	if (op == "less-than-equals") return a <= rhs;
	if (op == "greater-than") return a > rhs;
	if (op == "greater-than-equals") return a >= rhs;
	return false;
}

// Product of <volume>/<pitch> entries.
double FGSounds::factor(const Array &entries, const Sound &s, double default_value) const
{
	if (entries.is_empty())
	{
		return default_value;
	}
	double total = 1.0;
	for (int i = 0; i < entries.size(); i++)
	{
		if (entries[i].get_type() != Variant::DICTIONARY)
		{
			continue;
		}
		const Dictionary d = entries[i];
		double v = 1.0;
		if (d.has("property"))
		{
			v = val(d["property"]);
		}
		else if (String(d.get("internal", "")) == "dt_play")
		{
			v = s.play_t;
		}
		const String type = d.get("type", "linear");
		if (type == "log")
		{
			v = v > 0.0 ? std::log10(Math::max(v, 1e-6)) : 0.0;
		}
		else if (type == "ln")
		{
			v = v > 0.0 ? std::log(Math::max(v, 1e-6)) : 0.0;
		}
		else if (type == "inverse")
		{
			v = std::abs(v) > 1e-9 ? 1.0 / v : 0.0;
		}
		else if (type == "square")
		{
			v = v * v;
		}
		v = v * num(d.get("factor", 1.0), 1.0) + num(d.get("offset", 0.0), 0.0);
		if (d.has("min"))
		{
			v = Math::max(v, num(d["min"]));
		}
		if (d.has("max"))
		{
			v = Math::min(v, num(d["max"]));
		}
		total *= v;
	}
	return total;
}

void FGSounds::update(const Dictionary &p_props, double delta)
{
	props.clear();
	const Array keys = p_props.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		props[norm(keys[i])] = p_props[keys[i]];
	}
	for (int i = 0; i < sounds.size(); i++)
	{
		Sound &s = sounds.write[i];
		AudioStreamPlayer *p = s.player;
		if (p == nullptr)
		{
			continue;
		}
		bool want = false;
		const bool continuous = s.mode == "looped" || s.mode == "in-transit";
		if (s.mode == "looped")
		{
			want = cond(s.cond);
		}
		else if (s.mode == "in-transit")
		{
			const double v = s.prop.is_empty() ? 0.0 : val(s.prop);
			if (!std::isnan(s.last) && std::abs(v - s.last) > 1e-5)
			{
				s.moving_t = 0.25;
			}
			s.last = v;
			s.moving_t = Math::max(s.moving_t - delta, 0.0);
			want = s.moving_t > 0.0;
		}
		else
		{
			// once / single: rising edge of the condition
			const bool now = cond(s.cond);
			if (now && !s.was_true)
			{
				p->play();
				s.play_t = 0.0;
				s.plays++;
			}
			s.was_true = now;
			want = p->is_playing();
		}
		if (continuous)
		{
			if (want && !p->is_playing())
			{
				p->play();
				s.play_t = 0.0;
			}
			else if (!want && p->is_playing())
			{
				p->stop();
			}
		}
		if (p->is_playing())
		{
			s.play_t += delta;
			double gain = Math::clamp(factor(s.vol, s, 1.0), 0.0, 4.0) * user_volume;
			if (camera_distance_m > 0.0)
			{
				const double ref = num(s.raw.get("reference-dist", REFERENCE_DIST_M), REFERENCE_DIST_M);
				const double mx = num(s.raw.get("max-dist", MAX_DIST_M), MAX_DIST_M);
				gain *= camera_distance_m >= mx ? 0.0 : ref / Math::max(camera_distance_m, ref);
			}
			p->set_volume_db(gain > 0.0005 ? UtilityFunctions::linear_to_db(gain) : -80.0);
			p->set_pitch_scale(Math::clamp(factor(s.pitch, s, 1.0), 0.05, 4.0));
		}
	}
}

Array FGSounds::playing() const
{
	Array out;
	for (int i = 0; i < sounds.size(); i++)
	{
		if (sounds[i].player != nullptr && sounds[i].player->is_playing())
		{
			out.push_back(sounds[i].name);
		}
	}
	return out;
}

int FGSounds::play_count(const String &name) const
{
	for (int i = 0; i < sounds.size(); i++)
	{
		if (sounds[i].name == name)
		{
			return sounds[i].plays;
		}
	}
	return 0;
}

double FGSounds::volume_db_of(const String &name) const
{
	for (int i = 0; i < sounds.size(); i++)
	{
		if (sounds[i].name == name && sounds[i].player != nullptr)
		{
			return sounds[i].player->get_volume_db();
		}
	}
	return -999.0;
}

} // namespace jsbgodot
