#ifndef IT_AUTOFLIGHT_H
#define IT_AUTOFLIGHT_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "fg_property_rules.h"

using namespace godot;

namespace jsbgodot
{

    // The MD-80 DFGS mode logic (docs/MD80_FLIGHTGEAR_PORT.md step 18): a port
    // of the FlightGear package's Nasal/DFGS/DFGS.nas (IT-AUTOFLIGHT System
    // Controller V4.1, Josh Davidson) and DFGS-b.nas (autothrottle modes, FMA)
    // plus the FGCP handlers of Nasal/cockpit.nas (ApPanel) and the thrust
    // limit text of Nasal/Systems/thrlim.nas. The control laws stay in the
    // package's property rules (Systems/dfgs-*.xml, run by FGPropertyRules);
    // this class owns the it-autoflight/* inputs, outputs and texts, the
    // instrumentation/fma/* strings and systems/engines/limit/active-mode.
    //
    // Per frame the rig gives collect()'s dictionary to update() (the module
    // reads the flight state, the radios, the FDM's dfgs outputs and the
    // rules' last outputs), publishes get_published() through the bridge
    // (FDM mirror + animator) and, after the rules ran, calls absorb() so the
    // inputs the driver rules sync while a mode is inactive (vs, kts-flch,
    // mach-flch) follow them. Cockpit-owned inputs (the FGCP's direct
    // bindings: fd1, fd2, athr, bank-limit-sw) are read from the cockpit
    // state and written back through take_control_writes(); the altitude
    // alert flags and the thrust limit mode go the same ways.
    class ItAutoflight : public RefCounted
    {
        GDCLASS(ItAutoflight, RefCounted);

    public:
        static void _bind_methods();
        ItAutoflight();

        void set_rules(const Ref<FGPropertyRules> &r);
        // acconfig options: auto_arm_alt (default on), nav (0 = analog)
        void set_option(const String &name, double value);
        // ITAF.init: type 0 = everything (a new flight), 1 = after a landing / AP kill in ROLLOUT
        void reset(int type);
        void update(const Dictionary &props, double dt);
        void absorb(const Dictionary &props);
        Dictionary get_published() const;
        Dictionary take_control_writes();
        Dictionary take_fdm_writes();
        // Rule outputs the module seeds (the rig writes them with FGPropertyRules::set_value).
        Dictionary take_rule_writes();
        // cockpit.ApPanel.<action>(arg): apSwitch apSelector apDisc atDisc spdPush
        // spdAdjust spdSel machSel eprLim hdgPush hdgPull hdgAdjust nav vorLoc ils
        // autoLand vsAdjust altPush altPull altAdjust vertSpd iasMach altHold turb toga
        bool panel(const String &action, double arg);
        Variant get_value(const String &key) const;
        PackedStringArray input_names() const;
        bool is_engaged() const;
        bool is_athr_driving() const; // A/T on and not in clamp: the levers follow the system command
        int get_lat_mode() const;
        int get_vert_mode() const;
        int get_thr_mode() const;

    protected:
        Ref<FGPropertyRules> rules;
        Dictionary st;        // published state (normalised paths)
        Dictionary cur;       // this frame's input dictionary
        Dictionary ctl_writes, fdm_writes, rule_writes;
        bool vs_alt_seeded = false;
        double opt_auto_arm_alt = 1.0;
        double opt_nav = 0.0;
        bool debug_modes = false; // set_option("debug", 1): print the mode changes

        // plain Nasal variables
        bool alt_capture_active = false, flch_active = false, synced_alt = false, can_autoland = false;
        int go_around = 0; // 0 off, 1 MAN G/A, 2 FD G/A, 3 AUT G/A
        bool enable_athr_off = false, enable_ap_off = false, enable_power_ap_off = false;
        double alt_rearm_time = -5.0, kts_mach_fgcp_time = -5.0, capt_vs = 0.0;
        double blink_time[4] = {-5.0, -5.0, -5.0, -5.0};
        bool blink_active[4] = {false, false, false, false};
        bool spd_low = false, retard = false;
        String alt_arm_str; // the ARM window's armed altitude ("060" for 6 000 ft), refreshed by fma_loop
        String fl_string() const;
        int tri_mode = 0;
        String thrlim_last = "T/O";
        int thrlim_prev_int = -99;
        bool loc_timer_on = false, gs_timer_on = false, ap_kill_on = false, ats_kill_on = false;
        double loop_acc = 0.0, slow_acc = 0.0, loc_acc = 0.0, gs_acc = 0.0, ap_kill_acc = 0.0, ats_kill_acc = 0.0;
        // previous values of the listened properties
        double prev_fd1 = 1.0, prev_fd2 = 1.0, prev_athr_in = 0.0, prev_vs_alt_active = 0.0, prev_nav_loc[2] = {0.0, 0.0};
        double prev_alt_in = 10000.0, prev_internal_alt = 10000.0, prev_hdg_in = 360.0;
        bool initialised = false;

        // state access
        double g(const char *k) const;
        bool b(const char *k) const;
        String gs(const char *k) const;
        void s(const char *k, double v);
        void sb(const char *k, bool v);
        void ss(const char *k, const String &v);
        double in(const char *k) const;
        bool inb(const char *k) const;
        double elapsed() const;
        bool fgcp_powered() const;
        bool wow_any() const;

        // ITAF
        void loop();
        void slow_loop();
        void ap1_master(bool on);
        void ap2_master(bool on);
        void ap_off_function();
        void athr_master(bool on);
        void kill_ap_silent();
        void kill_athr_silent();
        void fd1_master(bool on);
        void fd2_master(bool on);
        void set_basic_mode();
        void set_lat_mode(int n);
        void set_vert_mode(int n);
        void bank_limit();
        void activate_lnav();
        void activate_loc();
        void activate_gs();
        void check_lnav(int t);
        void check_loc(int t);
        void check_gs(int t);
        void check_radio_reversion(int l, int v);
        void takeoff_logic(int t);
        void set_climb_rate_lim();
        void reset_climb_rate_lim();
        void takeoff_go_around();
        void sync_kts_flch();
        void sync_mach_flch();
        void sync_hdg();
        void sync_alt();
        void sync_vs();
        void sync_pitch();
        void kts_mach_changed();
        void spd_push();
        void spd_adjust_check();
        void update_lat_text(const String &t);
        void update_vert_text(const String &t);
        void update_lnav_arm(bool n);
        void update_loc_arm(bool n);
        void update_gs_arm(bool n, int t = 0);
        void update_auto_land(bool n);
        void update_vs_alt();
        // listeners
        void input_lat(int n);
        void input_vert(int n);
        void input_toga();
        void input_kts_mach(bool v);
        void input_kts_mach_flch(bool v);
        void input_alt(double v);
        void input_alt_armed(bool v);
        void input_hdg(double v);
        void internal_alt(double v);
        void kill_ap_warn();
        void kill_ats_warn();
        // DFGS-b: FMA
        void fma_loop();
        void start_blink(int w);
        void start_long_blink(int w);
        void stop_blink(int w);
        void fma_cap_trk_recheck();
        void fma_lat();
        void fma_vert();
        void fma_arm();
        void fma_alt_arm();
        void fma_thr();
        void fma_loc_update();
        void fma_gs_update();
        // DFGS-b: autothrottle
        void athr_loop();
        void athr_mode_zero_check();
        void athr_set_vmax_check_fma();
        void athr_set_mode(int n);
        void athr_to_check();
        void athr_limit_changed(const String &l, const String &c);
        // thrlim.nas
        void thrlim_set_mode(int m);
        void thrlim_update_txt();
        bool nav_loc(int idx) const;
        int radio_sel() const;
    };

} // namespace jsbgodot

#endif
