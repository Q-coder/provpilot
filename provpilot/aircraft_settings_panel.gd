extends VBoxContainer
## Settings → Aircraft tab: per-aircraft settings and reference data for the
## aircraft selected on the welcome page (or the one being flown).
##
## Settings live in the SettingsStore as `aircraft_<id>_<name>` (section
## [aircraft] of provpilot.cfg, saved with the screen's Save button):
##   aircraft_<id>_fuel_pct   fuel load, % of every tank's capacity; applied at
##                            flight start and, when that aircraft is being
##                            flown, immediately on change
## Reference data comes from aircraft.cfg [performance] (see
## docs/AIRCRAFT_CONFIG.md); the centre of gravity is read live from JSBSim
## while the aircraft is flown. The host (node_3d.gd) wires the providers.

var aircraft_provider: Callable = Callable()   # -> aircraft id (String)
var fdm_provider: Callable = Callable()        # -> JSBGodot node of the flown aircraft, or null
var flown_provider: Callable = Callable()      # -> id of the flown aircraft ("" before the first flight)
var config_provider: Callable = Callable()     # id -> AircraftConfig (RefCounted) or null
var name_provider: Callable = Callable()       # id -> display name

var _store = null
var _aircraft_id := ""
var _title: Label
var _fuel_slider: HSlider
var _fuel_label: Label
var _fuel_note: Label
var _info_grid: GridContainer
var _cg_label: Label
var _cg_bar: HSlider
var _cg_limits: Label
var _timer: Timer
var _updating := false

const INFO_ROWS := [
	["vne_kts", "Never exceed (Vne)", "kt"],
	["vno_kts", "Max structural cruise (Vno)", "kt"],
	["vs_kts", "Stall, clean (Vs)", "kt"],
	["vs0_kts", "Stall, landing config (Vs0)", "kt"],
	["vr_kts", "Rotation (Vr)", "kt"],
	["vref_kts", "Approach (Vref)", "kt"],
	["ceiling_ft", "Service ceiling", "ft"],
	["max_takeoff_lbs", "Max take-off weight", "lb"],
	["empty_lbs", "Empty weight", "lb"],
]


func _ready() -> void:
	add_theme_constant_override("separation", 10)
	_title = Label.new()
	_title.add_theme_font_size_override("font_size", 20)
	add_child(_title)
	# fuel
	var fuel_head := Label.new()
	fuel_head.text = "Fuel load"
	fuel_head.add_theme_font_size_override("font_size", 16)
	add_child(fuel_head)
	var fuel_row := HBoxContainer.new()
	fuel_row.add_theme_constant_override("separation", 12)
	add_child(fuel_row)
	_fuel_slider = HSlider.new()
	_fuel_slider.min_value = 0.0
	_fuel_slider.max_value = 100.0
	_fuel_slider.step = 1.0
	_fuel_slider.custom_minimum_size = Vector2(320, 0)
	_fuel_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_fuel_slider.value_changed.connect(_on_fuel_changed)
	fuel_row.add_child(_fuel_slider)
	_fuel_label = Label.new()
	_fuel_label.custom_minimum_size = Vector2(220, 0)
	fuel_row.add_child(_fuel_label)
	_fuel_note = Label.new()
	_fuel_note.add_theme_font_size_override("font_size", 12)
	_fuel_note.modulate = Color(0.8, 0.85, 0.9)
	add_child(_fuel_note)
	add_child(HSeparator.new())
	# reference data
	var info_head := Label.new()
	info_head.text = "Reference data"
	info_head.add_theme_font_size_override("font_size", 16)
	add_child(info_head)
	_info_grid = GridContainer.new()
	_info_grid.columns = 2
	_info_grid.add_theme_constant_override("h_separation", 24)
	add_child(_info_grid)
	add_child(HSeparator.new())
	# centre of gravity
	var cg_head := Label.new()
	cg_head.text = "Centre of gravity (live while flying)"
	cg_head.add_theme_font_size_override("font_size", 16)
	add_child(cg_head)
	_cg_label = Label.new()
	add_child(_cg_label)
	_cg_bar = HSlider.new()
	_cg_bar.min_value = 0.0
	_cg_bar.max_value = 60.0
	_cg_bar.step = 0.1
	_cg_bar.editable = false
	_cg_bar.focus_mode = Control.FOCUS_NONE
	_cg_bar.custom_minimum_size = Vector2(320, 0)
	add_child(_cg_bar)
	_cg_limits = Label.new()
	_cg_limits.add_theme_font_size_override("font_size", 12)
	_cg_limits.modulate = Color(0.8, 0.85, 0.9)
	add_child(_cg_limits)
	_timer = Timer.new()
	_timer.wait_time = 0.5
	_timer.timeout.connect(_update_live)
	add_child(_timer)
	_timer.start()
	call_deferred("refresh")


func _find_store():
	var n: Node = get_parent()
	while n != null:
		if n.has_method("get_store"):
			return n.call("get_store")
		n = n.get_parent()
	return null


func _cfg():
	return config_provider.call(_aircraft_id) if config_provider.is_valid() and _aircraft_id != "" else null


func fuel_key() -> String:
	return "aircraft_%s_fuel_pct" % _aircraft_id


## Re-read the selection and the stored settings (show, cancel, restore defaults).
func refresh() -> void:
	if _store == null:
		_store = _find_store()
	_aircraft_id = str(aircraft_provider.call()) if aircraft_provider.is_valid() else ""
	var nm: String = str(name_provider.call(_aircraft_id)) if name_provider.is_valid() else _aircraft_id
	_title.text = "Aircraft: %s" % (nm if nm != "" else "none selected")
	# fuel: stored value, else the FDM's current load when flown, else 50 %
	var pct := 50.0
	if _store != null and _store.has_setting(fuel_key()):
		pct = float(_store.get_setting(fuel_key()))
	else:
		var jsb = _fdm_if_flown()
		if jsb != null and jsb.get_tank_count() > 0:
			var cap := 0.0
			var cont := 0.0
			for i in range(jsb.get_tank_count()):
				cap += jsb.get_tank_capacity_lbs(i)
				cont += jsb.get_tank_contents_lbs(i)
			if cap > 0.0:
				pct = cont / cap * 100.0
	_updating = true
	_fuel_slider.value = pct
	_updating = false
	_update_fuel_label()
	# reference data
	for c in _info_grid.get_children():
		c.queue_free()
	var cfg = _cfg()
	for row in INFO_ROWS:
		var k := Label.new()
		k.text = row[1]
		_info_grid.add_child(k)
		var v := Label.new()
		var val: float = cfg.get_float("performance", row[0], -1.0) if cfg != null else -1.0
		v.text = ("%s %s" % [_fmt(val), row[2]]) if val >= 0.0 else "—"
		_info_grid.add_child(v)
	var notes: String = cfg.get_string("performance", "notes", "") if cfg != null else ""
	if notes != "":
		var nk := Label.new()
		nk.text = "Notes"
		_info_grid.add_child(nk)
		var nv := Label.new()
		nv.text = notes
		nv.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		nv.custom_minimum_size = Vector2(360, 0)
		_info_grid.add_child(nv)
	if cfg != null and cfg.has_value("performance", "cg_fwd_pct_mac"):
		_cg_limits.text = "Limits: %.0f – %.0f %% MAC" % [cfg.get_float("performance", "cg_fwd_pct_mac", 0.0), cfg.get_float("performance", "cg_aft_pct_mac", 0.0)]
	else:
		_cg_limits.text = "% MAC estimates the aerodynamic centre at 25 % of the mean chord; no limits given for this aircraft."
	_update_live(true)


static func _fmt(v: float) -> String:
	return str(int(round(v))) if absf(v - round(v)) < 0.05 else "%.1f" % v


func _fdm_if_flown():
	if not fdm_provider.is_valid() or not flown_provider.is_valid():
		return null
	if str(flown_provider.call()) != _aircraft_id:
		return null
	return fdm_provider.call()


func _update_fuel_label() -> void:
	var pct: float = _fuel_slider.value
	var jsb = _fdm_if_flown()
	var cap := 0.0
	if jsb != null:
		for i in range(jsb.get_tank_count()):
			cap += jsb.get_tank_capacity_lbs(i)
	if cap > 0.0:
		_fuel_label.text = "%.0f %%  (%.0f of %.0f lb, %d tanks)" % [pct, cap * pct / 100.0, cap, jsb.get_tank_count()]
		_fuel_note.text = "Applied now to every tank of the aircraft you are flying, and again at each flight start."
	else:
		_fuel_label.text = "%.0f %% of every tank" % pct
		_fuel_note.text = "Applied when the flight starts. The FlightGear A-10's external tanks are not in the flight model; its three internal tanks share this setting."


func _on_fuel_changed(v: float) -> void:
	if _updating or _aircraft_id == "":
		return
	if _store != null:
		_store.set_setting(fuel_key(), v)
	var jsb = _fdm_if_flown()
	if jsb != null:
		apply_fuel(jsb, v)
	_update_fuel_label()


## Fill every tank to [param pct] of its capacity.
static func apply_fuel(jsb, pct: float) -> void:
	if jsb == null or not jsb.has_method("get_tank_count"):
		return
	for i in range(jsb.get_tank_count()):
		jsb.set_tank_contents_lbs(i, jsb.get_tank_capacity_lbs(i) * clampf(pct, 0.0, 100.0) / 100.0)


func _update_live(force: bool = false) -> void:
	# the timer skips hidden tabs; a refresh (screen opening) always fills the label
	if not force and not is_visible_in_tree():
		return
	var jsb = _fdm_if_flown()
	if jsb == null or not jsb.has_method("get_cg_info"):
		_cg_label.text = "Not flying this aircraft."
		_cg_bar.value = 0.0
		return
	var cg: Dictionary = jsb.get_cg_info()
	if cg.is_empty():
		_cg_label.text = "Flight model not running."
		return
	_cg_label.text = "CG %.1f in aft of the datum, %.1f %% MAC — weight %.0f lb" % [float(cg.get("cg_x_in", 0.0)), float(cg.get("pct_mac", 0.0)), float(cg.get("weight_lbs", 0.0))]
	_cg_bar.value = clampf(float(cg.get("pct_mac", 0.0)), _cg_bar.min_value, _cg_bar.max_value)
