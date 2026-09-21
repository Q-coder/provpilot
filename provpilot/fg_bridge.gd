extends RefCounted
class_name FGBridge
## FlightGear property bridge (docs/A10_FLIGHTGEAR_PORT.md, D2).
##
## The FlightGear model animations read FlightGear property paths; this
## collects their values every frame from JSBSim (properties the FDM has),
## from the host (lights, view, canopy, time of day) and from simple derived
## rules, and hands them to the FGAnimator. Whatever is not here evaluates to
## 0 in the animator and shows up in FGAnimator.unknown_properties().
##
## Subtask 3 — exterior: control surfaces, gear position / oleo compression /
## wheel spin, engine fans, slats (alpha), canopy, nose-gear lights, view.
## Subtask 4 — cockpit: flight instruments, engine and fuel gauges, stick /
## pedals / throttle / levers, clock, AoA indexer, electrical outputs and the
## system values the caution panel reads (all healthy until subtask 6 models
## them).

## JSBSim oleo travel used to normalise compression-ft to FlightGear's 0..1.
const OLEO_TRAVEL_FT := {0: 1.0, 1: 1.3, 2: 1.3}
## JSBSim gear unit order in the A-10 FDM: 0 nose, 1 left main, 2 right main.
const GEAR_UNITS := 3
const ENGINES := 2
const TANKS := 3                    # the FDM has three identical 3450 lb tanks
const FUEL_LBS_PER_GAL := 6.4       # JP-8 as in the FlightGear fuel system
const TF34_MIL_THRUST_LBF := 9065.0 # engine/TF34.xml <milthrust>
const BUS_VOLTS := 28.0             # DC outputs while the battery is on
const AC_VOLTS := 115.0
## FlightGear A-10-base.xml <aoa-indexer> thresholds (deg alpha)
const AOA_TOO_SLOW := 9.5
const AOA_SLOW := 8.5
const AOA_FAST := 7.5
const AOA_TOO_FAST := 6.5

var canopy_norm := 0.0          # 0 closed .. 1 open (host toggle, later a cockpit control)
var landing_light := false      # from AircraftLights (L key)
var taxi_light := false
var cockpit_view := true        # camera in the cockpit: FlightGear hides the outer canopy glass
var dc_volts := 28.0            # subtask 6: DC bus from battery / generators (0 when everything is off)
var ac_volts := 115.0
var g_max := 1.0                # accelerometer memory needles
var g_min := 1.0
var sim_time_seconds := -1.0    # the simulated time of day (seconds, the HUD clock); < 0 = system UTC
var control_state: Dictionary = {}   # FGControls.state: switch / knob values, merged last
var canopy_target := 0.0        # canopy switch: 0 close, 1 hold, 2 open -> moves canopy_norm

## Defaults for properties only the cockpit switches / knobs write (seeded
## into FGControls.state once by the host): power is on, canopy switch held.
const SWITCH_DEFAULTS := {
	"controls/electric/battery-switch": 1.0,
	"controls/electric/engine/generator": 1.0,
	"controls/electric/engine[1]/generator": 1.0,
	"sim/model/A-10/controls/canopy/canopy-switch": 1.0,
	"controls/lighting/panel-int": 0.0,
	"controls/gear/brake-parking": 1.0,   # every flight starts with the parking brake set
}

static func switch_default(prop: String) -> float:
	return float(SWITCH_DEFAULTS.get(prop, 0.0))


## Values for this frame. [param jsb] is the JSBGodot node (may be null
## before the FDM runs), [param airborne] = no weight on wheels,
## [param night] 0..1 drives the panel and instrument lighting.
func collect(jsb: Node, airborne: bool, night: float = 0.0) -> Dictionary:
	var p: Dictionary = {}
	# renderer flags the model tests
	p["sim/rendering/rembrandt/enabled"] = 0
	p["A-10/done"] = 0
	p["sim/variant-id"] = 0
	# view
	p["sim/current-view/name"] = "Cockpit View" if cockpit_view else "Chase View"
	p["sim/current-view/view-number"] = 0 if cockpit_view else 1
	p["sim/current-view/internal"] = 1 if cockpit_view else 0
	# sound set (subtask 8): warning audio volume, no rain / weapons audio yet
	p["sim/model/A-10/instrumentation/warnings/audio-alt-volume"] = 1.0
	p["sim/model/A-10/instrumentation/warnings/audio-alt"] = 0.0
	p["environment/rain-norm"] = 0.0
	p["payload/armament/aim-9m/sound-volume"] = 0.0
	p["controls/armament/trigger-gun"] = 0.0
	# canopy and lights (host state)
	p["canopy/position-norm"] = canopy_norm
	p["systems/electrical/outputs/landing-light"] = 1.0 if landing_light else 0.0
	p["systems/electrical/outputs/taxi-light"] = 1.0 if taxi_light else 0.0
	# light switches: 0 taxi / 1 off / 2 landing; nav 0 off / 1 steady / 2 flash
	p["sim/model/A-10/controls/lighting/land-lights-switch"] = 2.0 if landing_light else 1.0
	p["sim/model/A-10/controls/lighting/nav-lights-switch"] = 1.0
	p["controls/lighting/strobe"] = 1.0
	p["sim/model/A-10/controls/lighting/formation"] = 0.0
	# FlightGear's panel / instrument lighting knobs (0..1): the material
	# emission at 1.0 floods the panel orange, so dusk sets 0.6 unless the
	# cockpit knobs were turned (control_state wins below)
	p["controls/lighting/panel-norm"] = clampf(night, 0.0, 1.0) * 0.6
	p["controls/lighting/instruments-norm"] = clampf(night, 0.0, 1.0) * 0.6
	p["controls/lighting/panel-int"] = clampf(night, 0.0, 1.0) * 0.6
	# chronometer: the simulated time of day (what the HUD shows), else system UTC
	if sim_time_seconds >= 0.0:
		var secs := int(fposmod(sim_time_seconds, 86400.0))
		p["sim/time/utc/hour"] = secs / 3600
		p["sim/time/utc/minute"] = (secs / 60) % 60
		p["sim/time/utc/second"] = secs % 60
	else:
		var t: Dictionary = Time.get_time_dict_from_system(true)
		p["sim/time/utc/hour"] = int(t.get("hour", 0))
		p["sim/time/utc/minute"] = int(t.get("minute", 0))
		p["sim/time/utc/second"] = int(t.get("second", 0))
	# AoA indexer thresholds
	p["sim/aoa-indexer/too-slow-deg"] = AOA_TOO_SLOW
	p["sim/aoa-indexer/slow-deg"] = AOA_SLOW
	p["sim/aoa-indexer/fast-deg"] = AOA_FAST
	p["sim/aoa-indexer/too-fast-deg"] = AOA_TOO_FAST
	_systems_healthy(p)
	_avionics_idle(p)
	_electrical(p, jsb)
	if jsb == null:
		return p
	_surfaces(jsb, p)
	_gear(jsb, p, airborne)
	_engines(jsb, p)
	_fuel(jsb, p)
	_flight_instruments(jsb, p, airborne)
	_controls(jsb, p)
	# cockpit switches and knobs (FGControls) override the computed values
	for k in control_state.keys():
		p[k] = control_state[k]
	if dc_volts <= 0.0:
		for k in ["controls/lighting/panel-norm", "controls/lighting/instruments-norm", "controls/lighting/panel-int"]:
			p[k] = 0.0
	# HSI heading-set knob offsets the compass card
	var hsi_off: float = float(control_state.get("instrumentation/heading-indicator-fg/offset-deg", 0.0))
	if hsi_off != 0.0:
		p["instrumentation/heading-indicator-fg/indicated-heading-deg"] = fposmod(float(p["instrumentation/heading-indicator-fg/indicated-heading-deg"]) + hsi_off, 360.0)
	return p


## Canopy motion from the sprung switch (2 open / 0 close / 1 hold), ~10 s full travel.
func advance_canopy(delta: float) -> void:
	if canopy_target > 1.5:
		canopy_norm = minf(canopy_norm + delta * 0.1, 1.0)
	elif canopy_target < 0.5:
		canopy_norm = maxf(canopy_norm - delta * 0.1, 0.0)


## Subtask 6 — light electrical model. Battery switch and the two engine
## generators (cockpit switches, FGControls.state) feed one DC bus; an
## engine's generator is on line when its switch is on and N2 is above 50 %.
## The inverter makes AC from DC. Every powered output the model tests
## (`> 24`) follows the bus; the generator caution lights and the master
## caution come from generators off line.
func _electrical(p: Dictionary, jsb: Node) -> void:
	var battery: bool = float(control_state.get("controls/electric/battery-switch", 1.0)) > 0.5
	var gen_on := false
	var gen_caution: Array = [0.0, 0.0]
	for i in range(ENGINES):
		var sw: bool = float(control_state.get("controls/electric/engine%s/generator" % ("" if i == 0 else "[%d]" % i), 1.0)) > 0.5
		var n2: float = jsb.get_property("propulsion/engine[%d]/n2" % i) if jsb != null else 100.0
		var online: bool = sw and n2 > 50.0
		gen_on = gen_on or online
		gen_caution[i] = 0.0 if online else 1.0
	dc_volts = 28.0 if (gen_on or battery) else 0.0
	ac_volts = 115.0 if dc_volts > 0.0 else 0.0
	for out in ["adi", "hsi", "aoa-indexer", "caution-panel", "hud", "ldg-warning-system",
			"nav-mode", "rwr", "gau-8", "fuel-gauge-sel"]:
		p["systems/electrical/outputs/" + out] = dc_volts
	p["systems/electrical/R-AC-volts"] = ac_volts
	p["systems/A-10-electrical/l-generator-caution-light"] = gen_caution[0]
	p["systems/A-10-electrical/r-generator-caution-light"] = gen_caution[1]
	# the model compares the master caution with the string "true"
	p["sim/model/A-10/instrumentation/warnings/master-caution"] = dc_volts > 0.0 and (gen_caution[0] > 0.5 or gen_caution[1] > 0.5)
	# nothing lights without power: exterior lights, panel lighting
	if dc_volts <= 0.0:
		p["systems/electrical/outputs/landing-light"] = 0.0
		p["systems/electrical/outputs/taxi-light"] = 0.0
		p["controls/lighting/panel-norm"] = 0.0
		p["controls/lighting/instruments-norm"] = 0.0
		p["controls/lighting/panel-int"] = 0.0


## System values the caution panel and gauges read: everything serviceable.
func _systems_healthy(p: Dictionary) -> void:
	for i in range(2):
		p["systems/A-10-hydraulics/hyd-psi[%d]" % i] = 3000.0
		p["systems/A-10-hydraulics/hyd-res[%d]" % i] = 100.0
		p["systems/A-10-fuel/feed-line-press[%d]" % i] = 30.0
		p["systems/bleed-air/ats-valve[%d]" % i] = 0.0
	for i in range(4):
		p["systems/A-10-fuel/tank[%d]/boost-pump" % i] = 1.0
		p["controls/fuel/tank[%d]/fill-dis" % i] = 0.0
	p["systems/A-10-oxygen/oxy-level"] = 5.0
	for warn in ["systems/A-10-alpha-mach-computer/stall-sys-caution-light", "systems/A-10-bleed-air/warn-light",
			"systems/A-10-cadc/cadc-sys-caution-light", "systems/A-10-ejectable-seat/armed",
			"systems/A-10-electrical/apu-gen-caution-light", "systems/A-10-electrical/instrument-inverter-caution-light",
			"systems/A-10-electrical/l-converter-failure",
			"systems/A-10-electrical/r-converter-failure",
			"systems/A-10-iff/iff-warn-light", "systems/A-10-inertial-navigation-unit/inu-caution-light",
			"systems/A-10-inertial-navigation-unit/inu-hot-warn-light", "systems/A-10-service-air/service-air-warn-light",
			"sim/model/A-10/A-10-nav/hars/hars-warn-light",
			"systems/refuel/state", "sim/model/A-10/systems/apu/rpm-norm", "sim/model/A-10/systems/apu/temp",
			"controls/APU/off-start-switch", "sim/model/A-10/weapons/gun/gun-unsafe"]:
		p[warn] = 0.0
	p["controls/gear/antiskid"] = 1.0


## Radios, navigation and weapons idle: needles centred, flags shown, nothing armed.
func _avionics_idle(p: Dictionary) -> void:
	for k in ["instrumentation/nav/from-flag", "instrumentation/nav/to-flag", "instrumentation/nav/in-range",
			"instrumentation/nav/gs-needle-deflection", "instrumentation/nav/heading-needle-deflection",
			"instrumentation/nav[1]/radials/selected-deg", "instrumentation/nav[2]/in-range", "instrumentation/nav[2]/nav-distance",
			"instrumentation/tacan/in-range", "instrumentation/tacan/indicated-distance-nm", "instrumentation/tacan/switch-position",
			"sim/model/A-10/instrumentation/tacan/indicated-bearing-mag-deg",
			"sim/model/A-10/A-10-nav/selector-hars", "sim/model/A-10/A-10-nav/selector-ils", "sim/model/A-10/A-10-nav/selector-man",
			"sim/model/A-10/A-10-nav/selector-nav_crs", "sim/model/A-10/A-10-nav/selector-ptr", "sim/model/A-10/A-10-nav/selector-tils",
			"sim/model/A-10/A-10-nav/selector-vlocs", "sim/model/A-10/instrumentation/cdi/from-flag",
			"sim/model/A-10/instrumentation/cdi/needle-deflection", "sim/model/A-10/instrumentation/cdi/to-flag",
			"sim/model/A-10/instrumentation/vhf/homing-deviation", "sim/model/A-10/instrumentation/vhf/mode",
			"sim/model/A-10/instrumentation/vhf/selector", "sim/model/A-10/instrumentation/nav/frequencies/freq-whole",
			"instrumentation/comm/frequencies/selected-mhz", "controls/armament/master-arm", "controls/armament/ripple",
			"controls/armament/ripple-delay", "A-10/avionics/ew-rwr-switch", "A-10/displays/cmsc/cmsc-disp-int",
			"A-10/stores/aim9-light", "A-10/stores/ecm-mounted-left", "A-10/stores/ecm-mounted-right",
			"A-10/stores/tgp-mounted-left", "A-10/stores/tgp-mounted-right", "sim/model/A-10/weapons/wingtankC",
			"sim/model/A-10/weapons/wingtankL", "sim/model/A-10/weapons/wingtankR", "ai/submodels/submodel[1]/count",
			"sim/model/A-10/controls/hud/depr", "sim/model/A-10/controls/hud/mode-selector",
			"sim/model/A-10/instrumentation/hud/ccip_dev_h_m", "sim/model/A-10/instrumentation/hud/ccip_dev_v_m",
			"sim/model/A-10/controls/sas/pitch-sas-switch", "sim/model/A-10/controls/fuel/fuel-test-ind",
			"sim/model/A-10/controls/fuel/fuel-dsp-sel"]:
		p[k] = 0.0
	for i in range(1, 5):
		p["instrumentation/tacan/frequencies/selected-channel[%d]" % i] = 0.0
	for i in range(12):
		p["A-10/stations/station[%d]/selected" % i] = 0.0
		p["payload/armament/station/id-%d-count" % i] = 0.0
		p["payload/armament/station/id-%d-type" % i] = ""
	p["sim/hud/color/alpha"] = 1.0
	p["sim/model/A-10/controls/hud/intens"] = 1.0


func _surfaces(jsb: Node, p: Dictionary) -> void:
	# control surfaces: JSBSim positions are already normalised
	var rudder: float = jsb.get_property("fcs/rudder-pos-norm")
	var elevator: float = jsb.get_property("fcs/elevator-pos-norm")
	var flap: float = jsb.get_property("fcs/flap-pos-norm")
	var speedbrake: float = jsb.get_property("fcs/speedbrake-pos-norm")
	# The model rotates the right aileron with a mirrored axis and a negated
	# factor, i.e. it expects right-aileron-pos-norm = -left (FlightGear's
	# convention); the A-10 FDM outputs the same sign for both, so derive the
	# right side from the left.
	var ail_l: float = jsb.get_property("fcs/left-aileron-pos-norm")
	if absf(ail_l) < 1e-9:
		ail_l = jsb.get_property("fcs/aileron-pos-norm")
	p["surface-positions/rudder-pos-norm"] = rudder
	p["surface-positions/left-elevator-pos-norm"] = elevator
	p["surface-positions/right-elevator-pos-norm"] = elevator
	p["surface-positions/left-aileron-pos-norm"] = ail_l
	p["surface-positions/right-aileron-pos-norm"] = -ail_l
	p["surface-positions/left-flap-pos-norm"] = flap
	p["surface-positions/right-flap-pos-norm"] = flap
	p["surface-positions/left-spoiler-pos-norm"] = speedbrake
	p["surface-positions/right-spoiler-pos-norm"] = speedbrake


func _gear(jsb: Node, p: Dictionary, airborne: bool) -> void:
	var ground_speed_ms: float = jsb.get_property("velocities/vg-fps") * 0.3048
	# JSBSim's per-unit gear/unit[n]/pos-norm stays at 1; the retract kinematic
	# (Systems/Landing Gear.xml) drives gear/gear-pos-norm for all legs
	var gear_pos_all: float = clampf(jsb.get_property("gear/gear-pos-norm"), 0.0, 1.0)
	for i in range(GEAR_UNITS):
		p["gear/gear[%d]/position-norm" % i] = gear_pos_all
		var comp_ft: float = jsb.get_property("gear/unit[%d]/compression-ft" % i)
		p["gear/gear[%d]/compression-norm" % i] = clampf(comp_ft / float(OLEO_TRAVEL_FT.get(i, 1.0)), 0.0, 1.0)
		var wow: float = jsb.get_property("gear/unit[%d]/WOW" % i)
		var wheel_fps: float = jsb.get_property("gear/unit[%d]/wheel-speed-fps" % i)
		var roll_ms: float = wheel_fps * 0.3048 if absf(wheel_fps) > 1e-6 else (ground_speed_ms if wow > 0.5 else 0.0)
		p["gear/gear[%d]/rollspeed-ms" % i] = roll_ms
	p["gear/gear/rollspeed-ms"] = p["gear/gear[0]/rollspeed-ms"]
	for i in range(GEAR_UNITS):
		p["gear/gear[%d]/wow" % i] = 1.0 if jsb.get_property("gear/unit[%d]/WOW" % i) > 0.5 else 0.0
	p["velocities/groundspeed-kt"] = ground_speed_ms * 1.943844
	p["velocities/speed-down-fps"] = jsb.get_property("velocities/v-down-fps")
	# real nose-wheel steering angle (manifest override in fg_a10_build.py:
	# FlightGear steers the visual wheel from the rudder, 20 deg max)
	p["gear/gear[0]/steering-deg"] = jsb.get_property("fcs/steer-pos-deg[0]")
	# gear handle: the FlightGear Nasal (landing_gear_handle.nas) animates it
	# to 0 for UP and -1 for DOWN
	var cmd_down: bool = jsb.get_gear_cmd() > 0.5 if jsb.has_method("get_gear_cmd") else gear_pos_all > 0.5
	p["sim/model/A-10/controls/gear/ld-gear-handle-anim"] = -1.0 if cmd_down else 0.0
	p["orientation/alpha-deg"] = jsb.get_property("aero/alpha-deg") if airborne else 0.0


func _engines(jsb: Node, p: Dictionary) -> void:
	for i in range(ENGINES):
		var n1: float = jsb.get_property("propulsion/engine[%d]/n1" % i)
		var n2: float = jsb.get_property("propulsion/engine[%d]/n2" % i)
		var thrust: float = jsb.get_property("propulsion/engine[%d]/thrust-lbs" % i)
		var ff_pps: float = jsb.get_property("propulsion/engine[%d]/fuel-flow-rate-pps" % i)
		p["sim/model/A-10/engines/engine[%d]/n1" % i] = n1
		p["engines/engine[%d]/n1" % i] = n1
		p["engines/engine[%d]/n2" % i] = n2
		# the model's ITT gauge and hot light read a thrust fraction (its table
		# is keyed 0 .. 0.987 for "max thrust"), not degrees
		var thrust_norm: float = clampf(thrust / TF34_MIL_THRUST_LBF, 0.0, 1.0)
		p["engines/engine[%d]/egt-degf" % i] = thrust_norm
		p["engines/engine[%d]/itt-norm" % i] = thrust_norm * 0.9
		p["engines/engine[%d]/fuel-flow_pph" % i] = ff_pps * 3600.0
		p["engines/engine[%d]/thrust-lbs" % i] = thrust     # the sound set spells it both ways
		p["engines/engine[%d]/thrust_lb" % i] = thrust
		# oil pressure follows the core speed
		p["engines/engine[%d]/oil-pressure-psi" % i] = clampf(n2, 0.0, 100.0) * 0.95
		p["engines/engine[%d]/oilp-norm" % i] = clampf(n2 / 100.0, 0.0, 1.0)
		p["sim/model/A-10/controls/engines/engine[%d]/throttle" % i] = jsb.get_property("fcs/throttle-cmd-norm[%d]" % i)
		p["sim/model/A-10/controls/engines/engine[%d]/starter-switch-position" % i] = 0.0


func _fuel(jsb: Node, p: Dictionary) -> void:
	var lbs: Array = []
	var total := 0.0
	for i in range(TANKS):
		var l: float = jsb.get_property("propulsion/tank[%d]/contents-lbs" % i)
		lbs.append(l)
		total += l
	# FlightGear tanks: 0 left wing, 1 left main, 2 right main, 3 right wing;
	# the FDM's three tanks feed the left / right displays and the total drum
	var left: float = lbs[0] + lbs[1] * 0.5
	var right: float = lbs[2] + lbs[1] * 0.5
	p["sim/model/A-10/consumables/fuel/fuel-dsp-left"] = left
	p["sim/model/A-10/consumables/fuel/fuel-dsp-right"] = right
	p["sim/model/A-10/consumables/fuel/fuel-dsp-drum"] = total
	p["sim/model/A-10/consumables/fuel/diff-lbs"] = absf(left - right)
	p["consumables/fuel/tank[0]/level-gal_us"] = lbs[0] / FUEL_LBS_PER_GAL
	p["consumables/fuel/tank[1]/level-gal_us"] = lbs[1] * 0.5 / FUEL_LBS_PER_GAL
	p["consumables/fuel/tank[2]/level-gal_us"] = lbs[1] * 0.5 / FUEL_LBS_PER_GAL
	p["consumables/fuel/tank[3]/level-gal_us"] = lbs[2] / FUEL_LBS_PER_GAL


func _flight_instruments(jsb: Node, p: Dictionary, airborne: bool) -> void:
	var vc: float = jsb.get_property("velocities/vc-kts")
	p["instrumentation/airspeed-indicator/indicated-speed-kt"] = vc
	p["velocities/airspeed-kt"] = vc
	p["instrumentation/vertical-speed-indicator/indicated-speed-fpm"] = jsb.get_property("velocities/h-dot-fps") * 60.0
	# Altimeter: the cockpit knobs turn the Kollsman window (control state; set
	# to the weather's QNH at flight start, so the instrument reads the field
	# elevation on the ground); the reading is the ISA inversion of JSBSim's
	# static pressure against that setting, like FlightGear's altimeter.
	const PSF_PER_INHG := 70.7262
	if not control_state.has("instrumentation/altimeter/setting-inhg"):
		# the weather's QNH as the host set it (JSBSim applies it a little later)
		var qnh_hpa: float = float(jsb.get_weather_qnh_hpa()) if jsb.has_method("get_weather_qnh_hpa") else 1013.25
		control_state["instrumentation/altimeter/setting-inhg"] = snappedf(qnh_hpa / 33.8639, 0.01)
	var setting: float = float(control_state["instrumentation/altimeter/setting-inhg"])
	var p_psf: float = jsb.get_property("atmosphere/P-psf")
	if p_psf > 1.0 and setting > 1.0:
		p["instrumentation/altimeter/indicated-altitude-ft"] = 145366.45 * (1.0 - pow(p_psf / PSF_PER_INHG / setting, 0.190284))
	else:
		p["instrumentation/altimeter/indicated-altitude-ft"] = jsb.get_property("position/h-sl-ft")
	p["instrumentation/altimeter/setting-inhg"] = setting
	p["orientation/pitch-deg"] = jsb.get_property("attitude/theta-deg")
	p["orientation/roll-deg"] = jsb.get_property("attitude/phi-deg")
	var hdg: float = fposmod(jsb.get_property("attitude/psi-deg"), 360.0)
	p["instrumentation/heading-indicator-fg/indicated-heading-deg"] = hdg
	p["instrumentation/magnetic-compass/indicated-heading-deg"] = hdg
	p["instrumentation/slip-skid-ball/indicated-slip-skid"] = clampf(jsb.get_property("accelerations/Ny"), -1.0, 1.0)
	var nz: float = jsb.get_property("accelerations/Nz")
	if not airborne:
		nz = 1.0
	p["accelerations/pilot-g"] = nz
	g_max = maxf(g_max, nz)
	g_min = minf(g_min, nz)
	p["sim/model/A-10/instrumentation/g-meter/g-max"] = g_max
	p["sim/model/A-10/instrumentation/g-meter/g-min"] = g_min


func _controls(jsb: Node, p: Dictionary) -> void:
	p["controls/flight/aileron"] = jsb.get_property("fcs/aileron-cmd-norm")
	p["controls/flight/elevator"] = jsb.get_property("fcs/elevator-cmd-norm")
	p["controls/flight/rudder"] = jsb.get_property("fcs/rudder-cmd-norm")
	# flap lever detent: aircraft.cfg [flaps] positions 0 / 0.5 / 1 -> UP / MVR / DN
	var flap_cmd: float = jsb.get_property("fcs/flap-cmd-norm")
	p["sim/flaps/current-setting"] = 0 if flap_cmd < 0.25 else (1 if flap_cmd < 0.75 else 2)
