#include "atmosphere.h"
#include "geo_frame.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/classes/camera3d.hpp>

using namespace godot;

namespace jsbgodot
{

void Atmosphere::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("register_nodes", "world_env", "globe_node"), &Atmosphere::register_nodes);
	ClassDB::bind_method(D_METHOD("set_ecef", "ecef"), &Atmosphere::set_ecef);
	ClassDB::bind_method(D_METHOD("update", "viewport"), &Atmosphere::update);
}

Atmosphere::Atmosphere()
{
}

void Atmosphere::register_nodes(Environment *p_world_env, Node3D *p_globe_node)
{
	world_env = p_world_env;
	globe_node = p_globe_node;
	if (world_env != nullptr)
	{
		fog_base_density = world_env->get_fog_density();
		bg_base_energy = world_env->get_bg_energy_multiplier();
		ambient_base_energy = world_env->get_ambient_light_energy();
		glow_base_intensity = world_env->get_glow_intensity();
	}
}

void Atmosphere::update(Viewport *viewport)
{
	if (world_env == nullptr && globe_node == nullptr)
	{
		return;
	}
	if (viewport == nullptr)
	{
		return;
	}
	Camera3D *cam = Object::cast_to<Camera3D>(viewport->get_camera_3d());
	if (cam == nullptr)
	{
		return;
	}

	// Flat-frame Y ≈ altitude ASL near the origin (good enough for a fog ramp).
	double alt = ecef ? geo::geodetic_of(cam->get_global_position()).h : cam->get_global_position().y;

	if (world_env != nullptr)
	{
		double t = Math::clamp((fog_zero_alt_m - alt) / (fog_zero_alt_m - fog_full_alt_m), 0.0, 1.0);
		world_env->set_fog_density(static_cast<float>(fog_base_density * t));
		// Disable fog ENTIRELY above the haze band: exponential fog still tints
		// the infinite-depth background to the pale fog colour via
		// fog_sky_affect / aerial-perspective even at density 0, which turns
		// "black space" pale blue.
		world_env->set_fog_enabled(t > 0.0);

		// Darken the sky background toward black as we climb out of the
		// atmosphere. Above the atmosphere switch to a solid black background
		// (energy fading alone leaves a lit sky that never reaches true
		// black); below, ramp the sky back in.
		double ts = Math::clamp((sky_black_alt_m - alt) / (sky_black_alt_m - sky_full_alt_m), 0.0, 1.0);
		if (ts <= 0.0)
		{
			if (world_env->get_background() != Environment::BG_COLOR)
			{
				world_env->set_background(Environment::BG_COLOR);
				world_env->set_bg_color(Color(0.0, 0.0, 0.0));
			}
		}
		else
		{
			if (world_env->get_background() != Environment::BG_SKY)
			{
				world_env->set_background(Environment::BG_SKY);
			}
			world_env->set_bg_energy_multiplier(static_cast<float>(bg_base_energy * ts));
		}

		// High ambient (tuned for ground) over-brightens the globe and blooms
		// into space; taper it toward a low value at altitude.
		world_env->set_ambient_light_energy(static_cast<float>(Math::lerp(ambient_space_energy, ambient_base_energy, ts)));

		// Glow blooms the sunlit globe across the (black) background, which
		// reads as a bright grey haze from space. Fade it out with altitude
		// and disable it fully above the atmosphere (the world3d demo uses no
		// glow for its clean black space).
		world_env->set_glow_intensity(static_cast<float>(glow_base_intensity * ts));
		world_env->set_glow_enabled(ts > 0.02);
	}

	if (globe_node != nullptr)
	{
		bool want_visible = alt > globe_show_alt_m;
		if (globe_node->is_visible() != want_visible)
		{
			globe_node->set_visible(want_visible);
		}
	}
}

void Atmosphere::set_ecef(bool p_ecef)
{
	ecef = p_ecef;
}

} // namespace jsbgodot
