extends SceneTree
## Builds every enabled base pack's airport surfaces with the REAL renderer
## (not headless) and renders a few frames: catches shader compile errors
## and driver limits (the per-runway materials crashed Metal, 2026-09-20).
## Run: godot/bin/godot.macos.editor.double.arm64 --path provpilot --resolution 320x240 -s tests/probe_runway_shader.gd
var _frames := 0
func _initialize() -> void:
	var areas: Array = []
	var i := 10
	for p in ["denmark/Denmark","germany/Germany","switzerland-base/Switzerland","austria/Austria","italy/Italy","france/France","norway/Norway","sweden/Sweden","finland/Finland"]:
		areas.append({"path": "/Volumes/Data1/provpilot/build/" + p, "enabled": true, "priority": i}); i += 1
	var merged: Dictionary = load("res://runways_lib.gd").load_merged(areas)
	var rws: Node3D = load("res://runways.gd").new()
	root.add_child(rws)
	rws.build(merged)
	# PROBE_NIGHT=1: airfield lights at full intensity over a dark world
	if OS.get_environment("PROBE_NIGHT") != "":
		rws.set_light_intensity(1.0)
		var env := Environment.new()
		env.background_mode = Environment.BG_COLOR
		env.background_color = Color(0.02, 0.02, 0.04)
		var we := WorldEnvironment.new()
		we.environment = env
		root.add_child(we)
	print("probe: lights %d fixtures" % rws.light_count)
	# Top-down view of the point given by PROBE_LAT/PROBE_LON/PROBE_ALT
	# (defaults: Basel taxiway junctions), screenshot to PROBE_PNG if set.
	var lat := float(OS.get_environment("PROBE_LAT")) if OS.get_environment("PROBE_LAT") != "" else 47.6005
	var lon := float(OS.get_environment("PROBE_LON")) if OS.get_environment("PROBE_LON") != "" else 7.5255
	var alt := float(OS.get_environment("PROBE_ALT")) if OS.get_environment("PROBE_ALT") != "" else 500.0
	var cam := Camera3D.new()
	root.add_child(cam)
	var wun: Transform3D = W3DCoords.enu_to_ecef_transform(lat, lon, alt, true)
	# look straight down: camera -Z along local -Y (down), camera up = local +Z (north)
	# right-handed: X = east (= -west), Y = north (screen up), Z (back) = up
	var b := Basis(-wun.basis.x, wun.basis.z, wun.basis.y)
	# PROBE_HDG / PROBE_PITCH: look toward a true heading, pitched down by
	# PROBE_PITCH degrees (default: straight down).
	if OS.get_environment("PROBE_HDG") != "":
		var hdg := deg_to_rad(float(OS.get_environment("PROBE_HDG")))
		var pitch := deg_to_rad(float(OS.get_environment("PROBE_PITCH")) if OS.get_environment("PROBE_PITCH") != "" else 10.0)
		var east: Vector3 = -wun.basis.x
		var north: Vector3 = wun.basis.z
		var up: Vector3 = wun.basis.y
		var fwd: Vector3 = (north * cos(hdg) + east * sin(hdg)) * cos(pitch) - up * sin(pitch)
		var right: Vector3 = fwd.cross(up).normalized()
		var cup: Vector3 = right.cross(fwd).normalized()
		b = Basis(right, cup, -fwd)
	cam.global_transform = Transform3D(b, wun.origin)
	if OS.get_environment("PROBE_TARGET") != "":
		# diagnostic: where is PROBE_TARGET (lat,lon,alt) in camera space?
		var tt: PackedStringArray = OS.get_environment("PROBE_TARGET").split(",")
		var tw: Vector3 = W3DCoords.geodetic_to_ecef(float(tt[0]), float(tt[1]), float(tt[2]))
		var tc: Vector3 = cam.transform.affine_inverse() * tw
		print("probe: cam origin ", cam.transform.origin, " target ", tw, " dist %.1f" % (tw - cam.transform.origin).length())
		print("probe: target in camera space x(right)=%.1f y(up)=%.1f z(back)=%.1f  -> bearing off-centre %.1f deg" % [tc.x, tc.y, tc.z, rad_to_deg(atan2(tc.x, -tc.z))])
	if OS.get_environment("PROBE_FOV") != "":
		cam.fov = float(OS.get_environment("PROBE_FOV"))
	cam.far = 100000.0
	cam.make_current()
	var sun := DirectionalLight3D.new()
	root.add_child(sun)
	sun.global_transform = Transform3D(Basis(wun.basis.x, wun.basis.y, wun.basis.z).rotated(wun.basis.x, -1.0), wun.origin)
	sun.light_energy = 1.2
func _process(_d: float) -> bool:
	_frames += 1
	if _frames == 25 and OS.get_environment("PROBE_PNG") != "":
		var img: Image = root.get_viewport().get_texture().get_image()
		img.save_png(OS.get_environment("PROBE_PNG"))
		print("probe_runway_shader: screenshot %s" % OS.get_environment("PROBE_PNG"))
	if _frames > 30:
		print("probe_runway_shader: rendered %d frames" % _frames)
		quit(0)
	return false
