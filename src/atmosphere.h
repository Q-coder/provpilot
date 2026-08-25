#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/viewport.hpp>

using namespace godot;

namespace jsbgodot
{

    // Altitude-driven atmosphere for the world3d globe view, moved out of
    // node_3d.gd so the per-frame rendering ramps run in C++ (double
    // precision).
    //
    // The scene Environment is exponential-fog based, which saturates to the
    // fog colour at very large distances — turning the whole Earth into a
    // white disc when viewed from space. This module fades the fog density out
    // with the rendering camera's altitude (near-ground haze kept, oceans and
    // continents visible from orbit), switches the background to solid black
    // above the atmosphere, tapers ambient light and glow toward their space
    // values, and gates the coarse global globe's visibility so its ~2 km
    // ETOPO surface doesn't poke through the fine Swiss/BW scenery3d patch at
    // low altitude (the globe's terrain queries are unaffected).
    //
    // GDScript (node_3d.gd) registers the scene Environment + globe node once
    // the world3d globe is mounted (register captures the Environment's base
    // values then) and calls update(get_viewport()) from _process. With
    // nothing registered, update() is a no-op — mirroring the historical
    // USE_WORLD3D_GLOBE / null-Environment guards.
    class Atmosphere : public RefCounted
    {
        GDCLASS(Atmosphere, RefCounted);

    public:
        static void _bind_methods();

        // ── Node registration (session-stable; no reset needed) ──
        // world_env = the scene WorldEnvironment's Environment resource
        // (nullptr when the scene has none), globe_node = the W3DGlobe node
        // (a Node3D under GlobeMount). Captures the fog/background/ambient/
        // glow base values from world_env.
        void register_nodes(Environment *world_env, Node3D *globe_node);

        // ── Per-frame update ──
        // Reads the active 3D camera from the viewport and applies the
        // altitude ramps + globe visibility gate. No-op without a camera or
        // without any registered node.
        void update(Viewport *viewport);

    protected:
        Atmosphere();

        // Tunables (defaults = historical GDScript constants)
        double fog_full_alt_m = 4000.0;     // full fog at/below this camera altitude (m ASL)
        double fog_zero_alt_m = 60000.0;    // no fog at/above this camera altitude (m ASL)
        double sky_full_alt_m = 12000.0;    // full sky at/below this camera altitude (m ASL)
        double sky_black_alt_m = 100000.0;  // black background at/above this altitude (m ASL)
        double ambient_space_energy = 0.35; // ambient light energy in space (vs bright ground value)
        double globe_show_alt_m = 25000.0;  // show the coarse globe only above this altitude

        Environment *world_env = nullptr;
        Node3D *globe_node = nullptr;

        // Base values captured at registration (the scene's authored values).
        double fog_base_density = 0.0;
        double bg_base_energy = 1.0;
        double ambient_base_energy = 1.0;
        double glow_base_intensity = 0.0;
    };

} // namespace jsbgodot

#endif
