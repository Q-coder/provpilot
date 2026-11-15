#ifndef AIRCRAFT_RIG_H
#define AIRCRAFT_RIG_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "aircraft_config.h"
#include "gear_struts.h"
#include "exhaust_effects.h"
#include "aircraft_lights.h"
#include "camera_rig.h"
#include "cockpit_controls.h"
#include "control_surfaces.h"
#include "engine_sound.h"
#include "fg_animator.h"
#include "fg_bridge.h"
#include "fg_controls.h"
#include "it_autoflight.h"
#include "ht9100_mcdu.h"
#include "fg_property_rules.h"
#include "fg_sounds.h"

using namespace godot;

namespace jsbgodot
{
    class SettingsStore;

    // The flown airframe as described by its aircraft.cfg: loads the config,
    // pushes its tunables into the animation / cockpit / sound modules it
    // owns, resolves the model nodes those modules animate, positions the
    // cockpit camera and runs the basic engine loop. Formerly ~750 lines of
    // node_3d.gd (_load_aircraft_config, _apply_selected_aircraft,
    // _resolve_aircraft_nodes, _resolve_gear_legs, _apply_camera_config,
    // _create_propeller_blur_disc, the normals flip, the lever collision
    // bodies, the engine-sound player).
    //
    // It also owns the aircraft systems built on the model: the exterior
    // lights, the FlightGear property bridge / animations / clickable
    // controls / sound set, the parking brake and the flight keys that act on
    // them (L lights, B speed brakes, Shift+Ctrl+M hotspots).
    //
    // GDScript (node_3d.gd) calls setup() once with the session-stable nodes,
    // apply_selected_aircraft() on every Fly!, update_systems() every frame
    // and update() / poll_keys() while flying.
    class AircraftRig : public RefCounted
    {
        GDCLASS(AircraftRig, RefCounted);

    public:
        static void _bind_methods();
        AircraftRig();

        // host = the main scene node (model root path is relative to it),
        // ac_node = $AC, cockpit_camera = Camera1 (its scene transform is the
        // pristine base every [camera] override starts from), camera_rig = the
        // C++ rig whose cockpit base is re-captured after an override.
        void setup(Node *host, Node3D *ac_node, Camera3D *cockpit_camera, CameraRig *camera_rig);

        // ── Configuration ──
        // Reads the aircraft.cfg for `aircraft_id` (AircraftConfig::resolve_path)
        // and pushes every tunable into the modules. Returns the config's
        // loaded flag; an unloaded config keeps the built-in defaults.
        bool load_config(const String &aircraft_id);
        Ref<AircraftConfig> get_config() const;
        String get_aircraft_id() const;
        String get_model_root_path() const;
        Node *get_model_root() const;
        bool is_gear_retractable() const;

        // ── Model ──
        // Adopt the airframe picked on the welcome screen: (re)loads the config
        // when the id changes, asks JSBGodot for the matching flight model
        // (cfg [jsbsim] model), applies the nose-steer range, swaps the visual
        // GLB through the AircraftModelLoader at the model root path,
        // re-resolves the nodes and positions
        // the cockpit camera. Returns true when the visual model changed.
        bool apply_selected_aircraft(const String &aircraft_id, Object *jsb);
        // A ground vehicle (aircraft.cfg [vehicle] kind): no JSBSim model, DriveMode drives it.
        bool is_vehicle() const;
        String get_vehicle_kind() const;
        Dictionary get_vehicle_params() const;
        // The last frame's FlightGear-side property dictionary (bridge, receivers,
        // autoflight, rules), for probes and tests: a key like
        // "instrumentation/nav/gs-rate-of-climb-fpm", null when absent.
        Variant get_fg_property(const String &key) const;
        void hide_model_nodes(Node *root);
        // Flips mesh normals under `root` per cfg [model] flip_normals /
        // flip_normals_prefix (the C172P .blend import bakes them inverted).
        void apply_model_normals(Node *root);
        // cfg [model] glass_materials: the named materials become clear glass
        // (dark tint, low alpha, low roughness) instead of the milky grey the
        // AC3D / Sketchfab exports carry (gery 2026-11-08: PC-7 and ASK 21).
        void apply_glass_materials(Node *root);
        // Resolves every model-dependent node against the loaded GLB: control
        // surfaces, propeller (+ blur disc), gear legs, cockpit levers (+ pick
        // bodies), cfg [model] hide_nodes. Emits `nodes_resolved` at the end.
        void resolve_nodes(Object *jsb);
        // Cockpit camera per cfg [camera] (position / rotation / fov / orbit
        // distances); no keys = the scene's own Camera1 transform.
        void apply_camera_config();
        // Per-aircraft eye point (settings aircraft_<id>_eye_offset), saved when a move ends.
        void save_eye_offset();
        SettingsStore *settings_store() const;
        // cfg [gear] steer_limited_deg / steer_full_deg -> JSBGodot.
        void apply_nose_steer_range(Object *jsb);

        // ── Engine sound ──
        // Creates the looping AudioStreamPlayer (cfg [sound] engine) under
        // `parent` and registers it with the EngineSound module.
        void setup_engine_sound(Node *parent);
        AudioStreamPlayer *get_engine_audio() const;

        // ── Per-frame ──
        // Keyboard lever input + lever visuals, control-surface / propeller /
        // gear animation and the engine loop (silent for an aircraft with its
        // own FlightGear sound set). jsb may be null: the levers still move,
        // the rest is skipped.
        void update(Object *jsb, double delta, double user_volume_pct);
        // Every frame (also while loading): exterior lights, the FlightGear
        // bridge -> animator / sounds, the electrical bus, and the pointing
        // hand over a clickable cockpit part. rendering_cam = the viewport
        // camera (the cockpit view flag follows it), pick_camera = the cockpit
        // camera while it is the active one (null otherwise), night 0..1,
        // sim_time_seconds = the simulated time of day, sounds_active = the
        // FlightGear sound set may play (flight live, not behind a loading
        // screen).
        void update_systems(Object *jsb, double delta, Camera3D *rendering_cam, Camera3D *pick_camera, double night,
                double sim_time_seconds, bool sounds_active, double user_volume_pct);
        // Flight keys while flying: Shift+Ctrl+M cockpit hotspots, B speed
        // brakes (fcs/speedbrake-cmd-norm), L landing / taxi lights.
        void poll_keys(Object *jsb);
        // Thrust reversers (cfg [engines] reverse_engage_props): FlightGear's
        // toggleRevThrust: on the ground with the levers at idle the flags flip
        // and the throttle inputs command reverse power; anywhere else the
        // call stows them and idles the levers. False when the aircraft has none.
        bool toggle_reverse_thrust(Object *jsb);
        bool is_reverse_engaged() const;
        // Every flight starts with the parking brake set (release with ",",
        // set again with "." or the cockpit handle).
        void set_parking_brake_at_start(Object *jsb);
        bool get_parking_brake() const;
        void set_parking_brake(bool on);

        // ── Owned modules ──
        Ref<ControlSurfaces> get_control_surfaces() const;
        Ref<CockpitControls> get_cockpit_controls() const;
        Ref<EngineSound> get_engine_sound() const;
        Ref<FGBridge> get_fg_bridge() const;
        // Strut shift of each configured gear unit, metres up from the drawn pose (tests).
        PackedFloat64Array get_strut_shifts() const;
        ExhaustEffects *get_exhaust_effects() const;
        Ref<FGControls> get_fg_controls() const;
        FGSounds *get_fg_sounds() const;
        Ref<FGPropertyRules> get_fg_property_rules() const;
        Ref<ItAutoflight> get_autoflight() const;
        Ref<Ht9100Mcdu> get_mcdu() const;
        FGAnimator *get_fg_animator() const;
        AircraftLights *get_aircraft_lights() const;
        // The systems built on the model (called from resolve_nodes; public
        // so a probe can rebuild them).
        void setup_aircraft_lights();
        void setup_fg_animations();
        void setup_fg_sounds();
        void setup_fg_property_rules();
        // cfg [autoflight] it_autoflight: the DFGS mode logic module (MD-80)
        void setup_autoflight();
        // cfg [liveries] catalog: the texture properties of the chosen livery
        // (Settings > Aircraft, aircraft_<id>_livery) and the variant's own
        // texture defaults go into the control state; FGAnimator swaps the
        // textures. set_livery applies a new choice while flying.
        void setup_liveries();
        void set_livery(const String &p_livery_id);
        String get_livery() const;
        Array get_liveries() const;
        // A cockpit switch or knob changed a FlightGear property: real effects.
        void _on_fg_control(const String &prop, const Variant &value);
        // a shim's "autoflight" action (the FGCP handlers) -> the module
        void _on_autoflight_action(const String &kind, double arg);

    protected:
        Node *host = nullptr;
        Node3D *ac_node = nullptr;
        Camera3D *cockpit_camera = nullptr;
        CameraRig *camera_rig = nullptr;
        Transform3D cockpit_scene_transform; // pristine Camera1 transform
        double cockpit_scene_fov = 0.0;

        Ref<AircraftConfig> cfg;
        String aircraft_id = "c172p";
        // Aircraft id whose visual model (normals flip + resolved nodes) is
        // currently applied; guards against double-flipping the normals.
        String model_visuals_id = "c172p";
        String model_root_path = "AC/Node3D2/C172P_1";

        Ref<ControlSurfaces> control_surfaces;
        Ref<CockpitControls> cockpit_controls;
        Ref<EngineSound> engine_sound;
        AudioStreamPlayer *engine_audio = nullptr;
        String engine_sound_path = "res://sounds/c172_engine_inflight.ogg";

        bool gear_retractable = false;
        String gear_anim_name;
        double propeller_disc_radius = 0.95; // 0 = derive from the mesh
        Vector3 propeller_spin_axis = Vector3(0, 0, 1);
        Vector3 propeller_disc_offset;

        // Aircraft systems on the model
        Ref<FGBridge> fg_bridge;
        Ref<FGControls> fg_controls;
        FGSounds *fg_sounds = nullptr;
        Ref<FGPropertyRules> fg_rules;    // cfg [jsbsim] property_rules
        Ref<Ht9100Mcdu> mcdu;             // cfg [mcdu] ht9100 (MD-80)
        Ref<ItAutoflight> autoflight;     // cfg [autoflight] it_autoflight
        Dictionary rule_fdm_nodes;
        Dictionary last_fg_props; // see get_fg_property()        // rule output -> the FDM has that node
        FGAnimator *fg_animator = nullptr;
        AircraftLights *aircraft_lights = nullptr;
        bool fg_seeded = false;
        Dictionary livery_defaults;      // catalog "defaults": prop -> file (the -set file's gauge faces)
        Array livery_list;               // catalog "liveries": [{id, name, textures}]
        PackedStringArray livery_props;  // every property a livery sets (cleared when none is chosen)
        String livery_id;
        void apply_livery_props();
        bool parking_brake = false;
        bool speedbrake_out = false;
        String speedbrake_prop;          // [controls] speedbrake_prop: the FDM's own lever property (MD-80), else fcs/speedbrake-cmd-norm
        double speedbrake_key_value = 1.0; // ... the lever value the B key deploys to
        double speedbrake_step = 0.0;    // ... one lever notch for the cockpit lever clicks (0 = in / out only)
        // The speed brake lever: out / in (B key), or one notch (the cockpit lever).
        void command_speedbrake(JSBGodot *fdm, bool out, bool notch);
        bool hotspot_key_held = false;
        bool speedbrake_key_held = false;
        bool reverse_key_held = false;
        Array reverse_engage_props;      // [engines] reverse_engage_props: the FDM flags, one per engine
        String reverse_lever_prop;       // [engines] reverse_lever_max_prop: the most forward lever (idle check)
        bool reverse_engaged = false;
        bool landing_key_held = false;
        Array landing_props, flood_props, landing_switch_props; // [lights] *_props (see setup)
        double landing_switch_on = 1.0, landing_switch_off = 0.0;
        Object *last_jsb = nullptr;
        GearStruts gear_struts;          // wheels pinned to the FDM's gear contacts ([gear] strut_*)
        void setup_gear_struts();
        ExhaustEffects *exhaust = nullptr; // heat shimmer and contrails ([exhaust]); a child of the host

        bool has_fg_sounds() const;
        void sync_parking_brake(Object *jsb);
        void push_nav_text(const String &text);
        void apply_fdm_init_properties(JSBGodot *fdm);
        // Engine loop and FlightGear sounds to silence (aircraft swap, loading).
        void silence_sounds();

        Node3D *resolve_surface(const String &key) const;
        Node3D *resolve_model_node(const String &rel) const;
        Node3D *resolve_lever(Node *root, const String &key) const;
        void resolve_gear_legs(Object *jsb);
        void resolve_levers(Object *jsb);
        void create_propeller_blur_disc(Node3D *prop_node);
        void apply_hidden_nodes();
        void flip_normals_recursive(Node *node, const String &prefix);
    };

} // namespace jsbgodot

#endif
