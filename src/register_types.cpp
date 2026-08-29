#include "register_types.h"
#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include "jsbgodot.h"
#include "geo.h"
#include "control_surfaces.h"
#include "camera_rig.h"
#include "cockpit_controls.h"
#include "slew_mode.h"
#include "intro_zoom.h"
#include "atmosphere.h"
#include "engine_sound.h"
#include "terrain_elevation.h"
#include "loading_progress.h"

using namespace godot;
using namespace jsbgodot;

void initialize_gojsb_module(ModuleInitializationLevel p_level)
{
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE)
	{
		return;
	}
	ClassDB::register_class<JSBGodot>();
	ClassDB::register_class<Geo>();
	ClassDB::register_class<ControlSurfaces>();
	ClassDB::register_class<CameraRig>();
	ClassDB::register_class<CockpitControls>();
	ClassDB::register_class<SlewMode>();
	ClassDB::register_class<IntroZoom>();
	ClassDB::register_class<Atmosphere>();
	ClassDB::register_class<EngineSound>();
	ClassDB::register_class<TerrainElevation>();
	ClassDB::register_class<LoadingProgress>();
}

void uninitialize_gojsb_module(ModuleInitializationLevel p_level)
{
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE)
	{
		return;
	}
}

extern "C"
{
	// Initialization.
	GDExtensionBool GDE_EXPORT gojsb_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization)
	{
		godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

		init_obj.register_initializer(initialize_gojsb_module);
		init_obj.register_terminator(uninitialize_gojsb_module);
		init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

		return init_obj.init();
	}
}