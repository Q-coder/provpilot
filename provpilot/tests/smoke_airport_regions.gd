extends SceneTree
## Smoke test: which scenery regions count as installed for the welcome
## screen's airport markers (design §11: base packs tag their region in
## terrain/manifest.json; older packs are recognised by directory name).
## Run: godot --headless --path provpilot -s tests/smoke_airport_regions.gd

var _fails := 0

func _check(cond: bool, msg: String) -> void:
	if cond:
		print("  ok   " + msg)
	else:
		_fails += 1
		printerr("  FAIL " + msg)

func _initialize() -> void:
	var tmp := OS.get_temp_dir().path_join("provpilot_smoke_airport_regions")
	DirAccess.make_dir_recursive_absolute(tmp.path_join("tagged/terrain"))
	DirAccess.make_dir_recursive_absolute(tmp.path_join("Norway/terrain"))
	DirAccess.make_dir_recursive_absolute(tmp.path_join("Germany/Baden-Wuertemberg/terrain"))
	DirAccess.make_dir_recursive_absolute(tmp.path_join("Nothing"))

	# A base-tier manifest: scalars first, then a long filler so the 64 KB
	# head read is exercised with a realistic layout.
	var f := FileAccess.open(tmp.path_join("tagged/terrain/manifest.json"), FileAccess.WRITE)
	f.store_string('{\n  "tile_size_m": 1024,\n  "pixels_per_tile": 34,\n  "crs": "EPSG:2056",\n  "region": "denmark",\n  "tiles": {')
	for i in range(20000):
		f.store_string('"%d": {"file": "tile_%d.raw"},' % [i, i])
	f.store_string('}}\n')
	f.close()
	# An old-style pack: manifest without a region tag.
	f = FileAccess.open(tmp.path_join("Norway/terrain/manifest.json"), FileAccess.WRITE)
	f.store_string('{"tile_size_m": 1024, "pixels_per_tile": 34}')
	f.close()

	_check(AirportsLib.manifest_region(tmp.path_join("tagged")) == "denmark", "region read from the manifest head")
	_check(AirportsLib.manifest_region(tmp.path_join("Norway")) == "", "untagged manifest gives no region")
	_check(AirportsLib.manifest_region(tmp.path_join("Nothing")) == "", "missing manifest gives no region")

	var areas := [
		{"path": tmp.path_join("tagged"), "enabled": true},
		{"path": tmp.path_join("Norway"), "enabled": false},
		{"path": tmp.path_join("Germany/Baden-Wuertemberg"), "enabled": true},
		{"path": tmp.path_join("Nothing"), "enabled": true},
		"not a dictionary",
	]
	var regions: Dictionary = AirportsLib.installed_regions_from_areas(areas)
	_check(regions.has("denmark"), "tagged pack -> denmark")
	_check(regions.has("norway"), "Norway directory name -> norway (legacy rule)")
	_check(regions.has("baden-wuerttemberg"), "Germany/Baden-Wuertemberg -> baden-wuerttemberg")
	_check(not regions.has("germany"), "the BW add-on path does not imply the Germany base pack")
	_check(not regions.has("italy"), "unrelated region not reported")

	_check(AirportsLib.availability_marker({"region": "denmark"}, regions) == "✓", "installed marker")
	_check(AirportsLib.availability_marker({"region": "italy"}, regions) == "✗", "missing marker")
	_check(AirportsLib.availability_marker({}, regions) == "", "untagged airport is neutral")

	if _fails == 0:
		print("smoke_airport_regions: all checks passed")
		quit(0)
	else:
		printerr("smoke_airport_regions: %d failure(s)" % _fails)
		quit(1)
