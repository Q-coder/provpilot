extends SceneTree
## Land-cover probe (design §14.6): a bare Scenery3D over the Tösstal with the
## base Switzerland terrain and its forest files, no orthophotos. Lists how many
## regular tiles and far chunks carry a ground texture and saves one chunk's
## texture and one tile's texture as PNGs.
## Run: godot/bin/godot.macos.editor.double.arm64 --headless --path provpilot -s tests/probe_landcover_chunks.gd -- <out_dir> [terrain_dir] [veg_dir]
const E := 2707800.0
const N := 1246400.0

func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var out: String = args[0] if args.size() > 0 else "/tmp"
	var terrain: String = args[1] if args.size() > 1 else "/Volumes/Data1/provpilot/build/switzerland-base/Switzerland/terrain"
	var veg: String = args[2] if args.size() > 2 else "/Volumes/Data1/provpilot/build/switzerland-base/Switzerland/vegetation"
	DirAccess.make_dir_recursive_absolute(out)
	var sc := Scenery3D.new()
	sc.frame_mode = 0
	sc.origin_east = E
	sc.origin_north = N
	sc.data_paths = PackedStringArray([terrain])
	sc.landcover_paths = PackedStringArray([veg])
	sc.load_radius = 4
	root.add_child(sc)
	var cam := Camera3D.new()
	root.add_child(cam)
	cam.global_position = Vector3(0.0, 1500.0, 0.0)
	cam.far = 200000.0
	cam.current = true
	var t0 := Time.get_ticks_msec()
	var tm: Node = null
	while Time.get_ticks_msec() - t0 < 40000:
		await process_frame
		for c in sc.get_children():
			if c.get_class() == "S3DTileManager":
				tm = c
	var tiles := 0
	var tiles_tex := 0
	var chunks := 0
	var chunks_tex := 0
	var saved_tile := false
	var saved_chunk := false
	for t in tm.get_children():
		if not (t is S3DTile) or t.mesh == null:
			continue
		var mat: Material = t.mesh.surface_get_material(0)
		var tex: Texture2D = mat.get_texture(BaseMaterial3D.TEXTURE_ALBEDO) if mat is StandardMaterial3D else null
		var is_chunk: bool = t.tile_size > 1024
		if is_chunk:
			chunks += 1
			if tex != null:
				chunks_tex += 1
				if not saved_chunk and tex.get_image() != null and not tex.get_image().is_empty():
					tex.get_image().save_png(out.path_join("chunk_%d_%d.png" % [t.tile_x, t.tile_z]))
					saved_chunk = true
		else:
			tiles += 1
			if tex != null:
				tiles_tex += 1
				if not saved_tile and tex.get_image() != null and not tex.get_image().is_empty() and t.tile_x == int(E / 1024.0) and t.tile_z == int(N / 1024.0):
					tex.get_image().save_png(out.path_join("tile_%d_%d.png" % [t.tile_x, t.tile_z]))
					saved_tile = true
	print("[landcover] %.1f s: " % ((Time.get_ticks_msec() - t0) / 1000.0) + "%d regular tiles, %d with a ground texture; %d chunks, %d with a ground texture" % [tiles, tiles_tex, chunks, chunks_tex])
	quit(0)
