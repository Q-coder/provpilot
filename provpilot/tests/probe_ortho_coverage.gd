extends SceneTree
## Probe: orthophoto no-data map for a LV95 window. For every tile: which
## sources exist and what fraction of the COMPOSITE (CH over BW, no-data =
## near-white or near-black filled from the next source) is still no-data.
## Run: godot --headless --path provpilot -s tests/probe_ortho_coverage.gd
## Env: E0 E1 N0 N1 (km, LV95), MIP ("" = full res, "mip3" = 16 px chunk tiles).
const CH := "/Users/gery/provpilot/scenery/Switzerland/orthophoto"
const BW := "/Users/gery/provpilot/scenery/Germany/Baden-Wuertemberg/orthophoto"
static func nodata(c: Color) -> bool:
	return (c.r > 0.98 and c.g > 0.98 and c.b > 0.98) or (c.r < 0.025 and c.g < 0.025 and c.b < 0.025)
func _initialize() -> void:
	var e0 := int(OS.get_environment("E0")) if OS.get_environment("E0") != "" else 2660
	var e1 := int(OS.get_environment("E1")) if OS.get_environment("E1") != "" else 2692
	var n0 := int(OS.get_environment("N0")) if OS.get_environment("N0") != "" else 1268
	var n1 := int(OS.get_environment("N1")) if OS.get_environment("N1") != "" else 1292
	var mip := OS.get_environment("MIP")
	var sub := "" if mip == "" else "/" + mip
	var ei0 := int(floor(e0 * 1000.0 / 1024.0))
	var ei1 := int(ceil(e1 * 1000.0 / 1024.0))
	var ni0 := int(floor(n0 * 1000.0 / 1024.0))
	var ni1 := int(ceil(n1 * 1000.0 / 1024.0))
	print("cell = sources (C=CH B=BW) + composite no-data %  (rows N desc, cols E asc, tile 1024 m, mip='%s')" % mip)
	var header := "        "
	for ei in range(ei0, ei1):
		header += "%6d" % ((ei * 1024) % 100000)
	print(header)
	var bad: Array = []
	for ni in range(ni1 - 1, ni0 - 1, -1):
		var line := "%7d " % (ni * 1024)
		for ei in range(ei0, ei1):
			var name := "/ortho_%d_%d.jpg" % [ei * 1024, ni * 1024]
			var imgs: Array = []
			var tags := ""
			if FileAccess.file_exists(CH + sub + name):
				imgs.append(Image.load_from_file(CH + sub + name)); tags += "C"
			if FileAccess.file_exists(BW + sub + name):
				imgs.append(Image.load_from_file(BW + sub + name)); tags += "B"
			if imgs.is_empty():
				line += "   -  "
				continue
			var w: int = imgs[0].get_width()
			var step: int = maxi(1, w / 64)
			var nd := 0
			var n := 0
			for y in range(0, w, step):
				for x in range(0, w, step):
					var still := true
					for im in imgs:
						if im.get_width() != w:
							continue
						if not nodata(im.get_pixel(x, y)):
							still = false
							break
					if still:
						nd += 1
					n += 1
			var pct := nd * 100 / maxi(n, 1)
			line += "%2s%3d%% " % [tags, pct]
			if pct > 0:
				bad.append("%s_%s %s %d%%" % [ei * 1024, ni * 1024, tags, pct])
		print(line)
	print("tiles with no-data left after compositing: %d" % bad.size())
	for b in bad:
		print("  " + b)
	quit(0)
