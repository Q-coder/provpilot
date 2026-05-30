extends SceneTree
## One-shot glTF → GLB converter. Usage:
##   godot --headless --script scripts/gltf_to_glb.gd -- <input.gltf> <output.glb>
## Loads the (multi-file) glTF, embedding its .bin + textures, and writes a
## single self-contained binary .glb.

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("Usage: gltf_to_glb.gd -- <input.gltf> <output.glb>")
		quit(1)
		return
	var in_path := args[0]
	var out_path := args[1]
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err := doc.append_from_file(in_path, state)
	if err != OK:
		push_error("append_from_file failed (err=%d) for %s" % [err, in_path])
		quit(1)
		return
	err = doc.write_to_filesystem(state, out_path)
	if err != OK:
		push_error("write_to_filesystem failed (err=%d) for %s" % [err, out_path])
		quit(1)
		return
	print("Wrote GLB: %s" % out_path)
	quit(0)
