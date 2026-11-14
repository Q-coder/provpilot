extends Node
## Probe: the MD-80's HT9100 MCDU (src/ht9100_mcdu.cpp). Drives the key
## handlers the cockpit shims call, checks pages, scratchpad and messages, and
## saves close-ups of the screen (IDENT, RTE 1 filled in, NEAREST AIRPORTS).
## Run NON-headless: godot --path provpilot res://tests/probe_md80_mcdu.tscn -- <out_dir>
var _fails := 0

func _check(ok: bool, what: String) -> void:
	print(("  PASS: " if ok else "  FAIL: ") + what)
	if not ok:
		_fails += 1

func _find(n: Node, name: String) -> Node:
	if String(n.name) == name:
		return n
	for c in n.get_children():
		var r := _find(c, name)
		if r != null:
			return r
	return null

func _type(mcdu, text: String) -> void:
	for ch in text:
		mcdu.key("alphaNumKey", ch)

func _shot(cam: Camera3D, out: String, name: String) -> void:
	cam.current = true
	await get_tree().create_timer(1.0).timeout
	get_viewport().get_texture().get_image().save_png(out.path_join(name))

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	get_window().size = Vector2i(1200, 1000)
	var root: Node = (load("res://node_3d.tscn") as PackedScene).instantiate()
	add_child(root)
	await get_tree().create_timer(3.0).timeout
	root._on_welcome_flight_requested("md80", 2682767.0, 1258797.0, 155.0)
	var waited := 0.0
	while not root.terrain_initialized and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	await get_tree().create_timer(6.0).timeout
	root._cancel_intro_zoom()
	root._welcome_screen.visible = false
	var mcdu = root._aircraft.get_mcdu()
	_check(mcdu != null, "the MD-80 rig owns the HT9100 module")
	if mcdu == null:
		get_tree().quit(1)
		return
	var title: Label3D = _find(root, "ScreenText_HT9100__mcdu1_screen_t0") as Label3D
	_check(title != null, "the screen carries its text labels")
	_check(mcdu.get_page() == "ident", "power-up page is IDENT")
	await get_tree().create_timer(0.5).timeout
	if title != null:
		_check(title.text.strip_edges() == "IDENT" and title.visible, "the title label shows IDENT (%s, visible %s)" % [title.text, title.visible])

	# a camera square in front of the screen: the labels face +z out of it
	var cam := Camera3D.new()
	cam.near = 0.01
	cam.fov = 40.0
	add_child(cam)
	if title != null:
		var screen := _find(root, "HT9100__mcdu1_screen") as MeshInstance3D
		var gt: Transform3D = title.global_transform
		var centre: Vector3 = screen.global_transform * screen.mesh.get_aabb().get_center() if screen != null else gt.origin
		var b := gt.basis.orthonormalized()
		var size: float = (screen.global_transform.basis * screen.mesh.get_aabb().size).length() if screen != null else 0.15
		cam.global_transform = Transform3D(b, centre + b.z * size * 1.1)
	await _shot(cam, out, "mcdu_ident.png")

	# keys as the cockpit sends them
	mcdu.key("softKey", "r6")
	_check(mcdu.get_page() == "posRef", "IDENT R6 opens POS REF")
	mcdu.key("softKey", "r6")
	_check(mcdu.get_page() == "rte", "POS REF R6 opens RTE 1")
	_type(mcdu, "XXXX")
	mcdu.key("softKey", "l1")
	_check(mcdu.get_scratchpad() == "NOT IN DATA BASE", "an unknown origin gives NOT IN DATA BASE (%s)" % mcdu.get_scratchpad())
	var pub: Dictionary = mcdu.get_published()
	_check(int(pub["instrumentation/ht9100/msg-light"]) == 1, "the MSG light is on with a message")
	mcdu.key("clearKey", "down")
	mcdu.key("clearKey", "up")
	_check(mcdu.get_scratchpad() == "XXXX", "CLR removes the message and restores the entry (%s)" % mcdu.get_scratchpad())
	for i in range(4):
		mcdu.key("alphaNumKey", "CLR")
	_check(mcdu.get_scratchpad() == "", "CLR deletes character by character")
	mcdu.key("alphaNumKey", "CLR")
	_check(mcdu.get_scratchpad() == "DELETE", "CLR on an empty scratchpad arms DELETE")
	mcdu.key("softKey", "l1")
	_check(mcdu.get_scratchpad() == "INVALID CLEAR", "DELETE on the origin is INVALID CLEAR")
	mcdu.key("alphaNumKey", "CLR")
	_type(mcdu, "ENGM")
	mcdu.key("softKey", "l1")
	_type(mcdu, "LSZH")
	mcdu.key("softKey", "r1")
	_type(mcdu, "SAS1845")
	mcdu.key("softKey", "r2")
	pub = mcdu.get_published()
	_check(pub["instrumentation/ht9100/d/L1"] == "ENGM" and pub["instrumentation/ht9100/d/R1"] == "LSZH", "origin and destination are entered (%s -> %s)" % [pub["instrumentation/ht9100/d/L1"], pub["instrumentation/ht9100/d/R1"]])
	_check(pub["instrumentation/ht9100/d/R2"] == "SAS1845", "the flight number is entered")
	_check(pub["autopilot/route-manager/departure/airport"] == "ENGM", "the origin is published for a route manager")
	await _shot(cam, out, "mcdu_rte.png")
	mcdu.key("softKey", "r6")
	_check(mcdu.get_page() == "perfInit", "RTE R6 opens PERF INIT")
	_type(mcdu, "330")
	mcdu.key("softKey", "r1")
	pub = mcdu.get_published()
	_check(pub["instrumentation/ht9100/d/R1"] == "FL330" and int(pub["autopilot/route-manager/cruise/altitude-ft"]) == 33000, "cruise level 330 is accepted (%s)" % pub["instrumentation/ht9100/d/R1"])
	_type(mcdu, "390")
	mcdu.key("softKey", "r1")
	_check(mcdu.get_scratchpad() == "INVALID ENTRY", "FL390 is above the MD-80's ceiling")
	mcdu.key("clearKey", "down")
	await get_tree().create_timer(2.5).timeout
	_check(mcdu.get_scratchpad() == "", "holding CLR clears the scratchpad (%s)" % mcdu.get_scratchpad())
	mcdu.key("clearKey", "up")
	await _shot(cam, out, "mcdu_perf.png")
	mcdu.key("pageKey", "legs")
	_check(mcdu.get_page() == "fallback", "LEGS is PAGE NOT AVAIL, as upstream")
	mcdu.key("pageKey", "menu")
	mcdu.key("softKey", "l1")
	_check(mcdu.get_page() == "perfInit", "MENU <HT9100 returns to the last FMC page (%s)" % mcdu.get_page())
	mcdu.key("pageKey", "dataIndex")
	mcdu.key("softKey", "r3")
	mcdu.key("softKey", "l1")
	_check(mcdu.get_page() == "nearestAirports", "DATA INDEX > NEAREST > AIRPORT")
	pub = mcdu.get_published()
	_check(String(pub["instrumentation/ht9100/d/L1"]).length() == 4 and String(pub["instrumentation/ht9100/d/C1"]).contains("NM"), "the nearest airport is listed with course and distance (%s %s)" % [pub["instrumentation/ht9100/d/L1"], String(pub["instrumentation/ht9100/d/C1"]).strip_edges()])
	await _shot(cam, out, "mcdu_nearest.png")
	# a real click on the R6 line select key goes through the Nasal shim
	mcdu.key("pageKey", "ident")
	var fgc = root._aircraft.get_fg_controls()
	var vp: Vector2 = get_viewport().get_visible_rect().size
	var pos := Vector2(vp.x * 0.815, vp.y * 0.80)
	print("  hover at R6: ", fgc.hover_name(cam, pos))
	fgc.click(cam, pos, 0, true, false)
	fgc.click(cam, pos, 0, false, false)
	await get_tree().create_timer(0.3).timeout
	_check(mcdu.get_page() == "posRef", "clicking the R6 key in the cockpit opens POS REF (%s)" % mcdu.get_page())
	await _shot(cam, out, "mcdu_posref.png")
	print("probe_md80_mcdu: %s" % ("OK" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(0 if _fails == 0 else 1)
