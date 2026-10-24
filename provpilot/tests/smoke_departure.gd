extends Node
## Headless smoke test for the departure point of the welcome screen
## (src/welcome_screen.cpp get_selected_departure): the aircraft starts on the
## runway, SPAWN_PAST_THRESHOLD_M (20 m) past the selected threshold along the
## runway, on the threshold stripes, with the runway's true heading. Uses the
## built-in airports.json only. Exits 0 on success, 1 on failure.
##
##   godot --headless --path provpilot res://tests/smoke_departure.tscn

var fails: Array[String] = []

const PAST_M := 20.0


func check(cond: bool, msg: String) -> void:
	if cond:
		print("  PASS: ", msg)
	else:
		fails.append(msg)
		printerr("  FAIL: ", msg)


func _find_first(node: Node, cls: String) -> Node:
	if node.get_class() == cls:
		return node
	for c in node.get_children():
		var r := _find_first(c, cls)
		if r != null:
			return r
	return null


func _ready() -> void:
	var screen := WelcomeScreen.new()
	add_child(screen)
	await get_tree().process_frame
	var catalog: Array[Dictionary] = AirportsLib.load_default()
	screen.populate_airports(catalog)
	await get_tree().process_frame
	# the airport list is the one whose items name ICAO codes
	var lists: Array = []
	_collect(screen, "ItemList", lists)
	var airport_list: ItemList = null
	var idx := -1
	for l in lists:
		for i in l.item_count:
			if l.get_item_text(i).contains("LSZH"):
				airport_list = l
				idx = i
				break
		if airport_list:
			break
	check(airport_list != null and idx >= 0, "LSZH is in the airport list")
	if airport_list == null:
		_finish()
		return
	airport_list.select(idx)
	screen._on_airport_selected(idx)
	await get_tree().process_frame
	var ap: Dictionary = {}
	for a in catalog:
		if a.get("icao", "") == "LSZH":
			ap = a
	var rwy: Dictionary = ap["runways"][0]
	var parts: PackedStringArray = String(rwy["designator"]).split("/")
	var grid_hdg: float = float(rwy.get("grid_heading_deg", rwy["heading_deg"]))
	var picker: OptionButton = _find_first(screen, "OptionButton")
	check(picker != null and picker.item_count >= 2, "runway picker lists both ends of %s" % rwy["designator"])
	# end 0: the lower designator
	picker.select(0)
	var dep: Dictionary = screen.get_selected_departure()
	var t0: Dictionary = rwy["threshold_" + parts[0]]
	var de: float = float(dep["swiss_e"]) - float(t0["e"])
	var dn: float = float(dep["swiss_n"]) - float(t0["n"])
	var dist := sqrt(de * de + dn * dn)
	var along := fposmod(rad_to_deg(atan2(de, dn)), 360.0)
	check(absf(dist - PAST_M) < 0.01, "%s: the start is %.2f m from the threshold (expected %.0f)" % [parts[0], dist, PAST_M])
	check(absf(fposmod(along - grid_hdg + 180.0, 360.0) - 180.0) < 0.5, "%s: ... along the runway (bearing %.1f, runway %.1f)" % [parts[0], along, grid_hdg])
	check(absf(float(dep["heading_deg"]) - float(rwy["heading_deg"])) < 0.01, "%s: the heading is the runway's true bearing" % parts[0])
	# end 1: the reciprocal, 20 m the other way from the other threshold
	picker.select(1)
	dep = screen.get_selected_departure()
	var t1: Dictionary = rwy["threshold_" + parts[1]]
	de = float(dep["swiss_e"]) - float(t1["e"])
	dn = float(dep["swiss_n"]) - float(t1["n"])
	dist = sqrt(de * de + dn * dn)
	along = fposmod(rad_to_deg(atan2(de, dn)), 360.0)
	var back := fposmod(grid_hdg + 180.0, 360.0)
	check(absf(dist - PAST_M) < 0.01, "%s: the start is %.2f m from its threshold" % [parts[1], dist])
	check(absf(fposmod(along - back + 180.0, 360.0) - 180.0) < 0.5, "%s: ... into the runway the other way (bearing %.1f, expected %.1f)" % [parts[1], along, back])
	check(absf(fposmod(float(dep["heading_deg"]) - float(rwy["heading_deg"]) - 180.0 + 180.0, 360.0) - 180.0) < 0.01, "%s: the heading is the reciprocal" % parts[1])
	# the start lies inside the runway, not before it: closer to the far end than the threshold is
	var fe: float = float(t1["e"]) - float(dep["swiss_e"])
	var fn: float = float(t1["n"]) - float(dep["swiss_n"])
	var te: float = float(t1["e"]) - float(t0["e"])
	var tn: float = float(t1["n"]) - float(t0["n"])
	picker.select(0)
	dep = screen.get_selected_departure()
	fe = float(t1["e"]) - float(dep["swiss_e"])
	fn = float(t1["n"]) - float(dep["swiss_n"])
	check(sqrt(fe * fe + fn * fn) < sqrt(te * te + tn * tn), "the start is on the runway side of the threshold")
	_finish()


func _collect(node: Node, cls: String, out: Array) -> void:
	if node.get_class() == cls:
		out.append(node)
	for c in node.get_children():
		_collect(c, cls, out)


func _finish() -> void:
	if fails.is_empty():
		print("SMOKE DEPARTURE: ALL PASS")
	else:
		printerr("SMOKE DEPARTURE: %d failure(s)" % fails.size())
		for f in fails:
			printerr("   - ", f)
	get_tree().quit(0 if fails.is_empty() else 1)
