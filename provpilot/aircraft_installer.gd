extends Node
## AircraftInstaller — On first run, extracts the bundled per-airframe data
## (JSBSim XMLs and any other on-disk assets each aircraft needs) from
## `res://aircraft/` to a writable `<install_root>/aircraft/` tree.
##
## Why this exists:
##   JSBSim is a native library that loads its model/engine/system files from
##   the OS filesystem (not res://). To keep the deployment a single PCK and
##   still give JSBSim real paths, we ship the curated aircraft tree inside
##   res:// (so it lands in the PCK) and copy it out on first launch.
##
##   This also gives users a real folder under `<install_root>/aircraft/` they
##   can modify, version-control, or add new airframes to — X-Plane style.
##
## The extraction is keyed by `BUNDLE_VERSION`. Bump it when the bundled data
## changes and existing installs will re-extract on next launch.
##
## Registered as a project autoload (see project.godot).

# Bumping this string forces a re-extraction of res://aircraft/ on next run.
const BUNDLE_VERSION := "1.0.5"

# Subtree inside res:// that mirrors the on-disk layout we want to produce
# under <install_root>/aircraft/.
const RES_AIRCRAFT_ROOT := "res://aircraft"


func _ready() -> void:
	var dest_root: String = InstallRoot.get_aircraft_root()
	# Self-heal: even if the install is "initialized" per provpilot.cfg, the
	# user might have deleted or moved the aircraft/ tree. Re-extract whenever
	# the expected sentinel file is missing.
	var sentinel := dest_root.path_join("c172p").path_join("jsbsim") \
			.path_join("aircraft").path_join("c172p").path_join("c172p.xml")
	var have_data := FileAccess.file_exists(sentinel)
	if InstallRoot.is_aircraft_initialized(BUNDLE_VERSION) and have_data:
		print("AircraftInstaller: aircraft data already extracted (version=%s)"
				% BUNDLE_VERSION)
		return

	if not have_data and InstallRoot.is_aircraft_initialized(BUNDLE_VERSION):
		print("AircraftInstaller: aircraft tree missing at %s — re-extracting."
				% dest_root)
	else:
		print("AircraftInstaller: extracting bundled aircraft data → %s"
				% dest_root)

	var err := _copy_tree(RES_AIRCRAFT_ROOT, dest_root)
	if err != OK:
		push_error("AircraftInstaller: extraction failed (err=%d). Files may be missing under %s."
				% [err, dest_root])
		return

	InstallRoot.mark_aircraft_initialized(BUNDLE_VERSION)
	print("AircraftInstaller: extraction complete.")


# ── Recursive res:// → absolute path copy ─────────────────────────────
# Notes:
#   • Inside the exported PCK, `DirAccess.open("res://aircraft/")` enumerates
#     files Godot packed into res://. We skip Godot-internal sidecars
#     (.import, .uid, .gdignore, .gitkeep) because they're not relevant to
#     JSBSim and never reach the on-disk tree.
#   • Existing files at the destination are overwritten so a version bump
#     can replace stale XML files.

# Files we never want on disk:
#   • .import/.uid/.gdignore/.gitkeep/.DS_Store — Godot/OS metadata
#   • .blend/.blend1 — Blender source (editor-only; we ship .glb for runtime)
#   • .gltf/.bin — the .glb is self-contained, so skip the split GLTF pair
#   • .png — loose textures embedded in the .glb (no separate runtime use)
const SKIP_EXTENSIONS := ["import", "uid", "blend", "blend1", "gltf", "bin", "png"]
const SKIP_NAMES := [".gdignore", ".gitkeep", ".DS_Store"]
# Subdirectories to skip entirely. `textures/` holds the raw PNGs that are
# already baked into the per-airframe .glb.
const SKIP_DIR_NAMES := ["textures"]


func _copy_tree(src_res: String, dest_abs: String) -> Error:
	var dir := DirAccess.open(src_res)
	if dir == null:
		push_error("AircraftInstaller: cannot open %s" % src_res)
		return ERR_CANT_OPEN

	var mk := DirAccess.make_dir_recursive_absolute(dest_abs)
	if mk != OK and mk != ERR_ALREADY_EXISTS:
		push_error("AircraftInstaller: cannot create %s (err=%d)"
				% [dest_abs, mk])
		return mk

	dir.list_dir_begin()
	var name := dir.get_next()
	while name != "":
		if name in SKIP_NAMES:
			name = dir.get_next()
			continue
		var src := src_res.path_join(name)
		var dst := dest_abs.path_join(name)
		if dir.current_is_dir():
			if name in SKIP_DIR_NAMES:
				name = dir.get_next()
				continue
			var err := _copy_tree(src, dst)
			if err != OK:
				dir.list_dir_end()
				return err
		else:
			var ext := name.get_extension().to_lower()
			if ext in SKIP_EXTENSIONS:
				name = dir.get_next()
				continue
			var err := _copy_file(src, dst)
			if err != OK:
				push_error("AircraftInstaller: copy %s → %s failed (err=%d)"
						% [src, dst, err])
				dir.list_dir_end()
				return err
		name = dir.get_next()
	dir.list_dir_end()
	return OK


func _copy_file(src_res: String, dest_abs: String) -> Error:
	var fin := FileAccess.open(src_res, FileAccess.READ)
	if fin == null:
		return FileAccess.get_open_error()
	var buf := fin.get_buffer(fin.get_length())
	fin.close()

	var fout := FileAccess.open(dest_abs, FileAccess.WRITE)
	if fout == null:
		return FileAccess.get_open_error()
	fout.store_buffer(buf)
	fout.close()
	return OK
