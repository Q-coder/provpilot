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
#include "drive_mode.h"
#include "intro_zoom.h"
#include "engine_sound.h"
#include "terrain_elevation.h"
#include "loading_progress.h"
#include "vfr_map.h"
#include "install_config.h"
#include "settings_store.h"
#include "settings_screen.h"
#include "fg_animator.h"
#include "aircraft_config.h"
#include "aircraft_rig.h"
#include "world_scenery.h"
#include "flight_hud.h"
#include "loading_screen.h"
#include "flight_spawn.h"
#include "aircraft_model_loader.h"
#include "fg_bridge.h"
#include "fg_controls.h"
#include "fg_sounds.h"
#include "fg_property_rules.h"
#include "it_autoflight.h"
#include "ht9100_mcdu.h"
#include "aircraft_lights.h"
#include "airports_lib.h"
#include "runways_lib.h"
#include "live_weather.h"
#include "joystick_bindings.h"
#include "key_bindings.h"
#include "aircraft_installer.h"
#include "sky_atmosphere.h"
#include "cloud_layers.h"
#include "exhaust_effects.h"
#include "aircraft_settings_panel.h"
#include "joystick_settings_panel.h"
#include "keyboard_settings_panel.h"
#include "welcome_screen.h"
#include "scenery_downloader_panel.h"
#include "runways.h"
#include "scenery_design.h"
#include "scenery_design_mesher.h"
#include "scenery_editor.h"
#include "scenery_objects.h"
#include "geo_magnetic.h"
#include "navaids_lib.h"
#include "radio_nav.h"

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
	ClassDB::register_class<GeoMagnetic>();
	ClassDB::register_class<NavaidsLib>();
	ClassDB::register_class<RadioNav>();
	ClassDB::register_class<ControlSurfaces>();
	ClassDB::register_class<CameraRig>();
	ClassDB::register_class<CockpitControls>();
	ClassDB::register_class<SlewMode>();
	ClassDB::register_class<DriveMode>();
	ClassDB::register_class<IntroZoom>();
	ClassDB::register_class<EngineSound>();
	ClassDB::register_class<TerrainElevation>();
	ClassDB::register_class<LoadingProgress>();
	ClassDB::register_class<VfrMapCanvas>();
	ClassDB::register_class<VfrMap>();
	ClassDB::register_class<InstallConfig>();
	ClassDB::register_class<FGAnimator>();
	ClassDB::register_class<SettingsStore>();
	ClassDB::register_class<SettingsScreen>();
	ClassDB::register_class<AircraftConfig>();
	ClassDB::register_class<AircraftRig>();
	ClassDB::register_class<WorldScenery>();
	ClassDB::register_class<FlightHud>();
	ClassDB::register_class<LoadingScreen>();
	ClassDB::register_class<FlightSpawn>();
	ClassDB::register_class<AircraftModelLoader>();
	ClassDB::register_class<FGBridge>();
	ClassDB::register_class<FGControls>();
	ClassDB::register_class<FGSounds>();
	ClassDB::register_class<FGPropertyRules>();
	ClassDB::register_class<ItAutoflight>();
	ClassDB::register_class<Ht9100Mcdu>();
	ClassDB::register_class<AircraftLights>();
	ClassDB::register_class<AirportsLib>();
	ClassDB::register_class<RunwaysLib>();
	ClassDB::register_class<LiveWeather>();
	ClassDB::register_class<JoystickBindings>();
	ClassDB::register_class<KeyBindings>();
	ClassDB::register_class<AircraftInstaller>();
	ClassDB::register_class<SkyAtmosphere>();
	ClassDB::register_class<CloudLayers>();
	ClassDB::register_class<ExhaustEffects>();
	ClassDB::register_class<AircraftSettingsPanel>();
	ClassDB::register_class<JoystickSettingsPanel>();
	ClassDB::register_class<KeyboardSettingsPanel>();
	ClassDB::register_class<WelcomeScreen>();
	ClassDB::register_class<SceneryDownloaderPanel>();
	ClassDB::register_class<Runways>();
	ClassDB::register_class<SceneryDesign>();
	ClassDB::register_class<SceneryDesignMesher>();
	ClassDB::register_class<SceneryViewGizmo>();
	ClassDB::register_class<SceneryGizmoOverlay>();
	ClassDB::register_class<SceneryEditor>();
	ClassDB::register_class<SceneryObjects>();
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