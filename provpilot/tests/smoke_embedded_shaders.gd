extends SceneTree
## Smoke: the scenery3d shaders compiled into the library are the files.
## An exported game has no file below res://addons/scenery3d (.gdignore), so
## S3DShaders.load falls back to the embedded source; 1.1.0 shipped without
## it and drew untextured trees.
func _initialize() -> void:
	var fails := 0
	for n in ["tree", "roads", "chunk"]:
		var path := "res://addons/scenery3d/shaders/%s.gdshader" % n
		var f := FileAccess.open(path, FileAccess.READ)
		var code: String = ClassDB.class_call_static("S3DShaders", "embedded_code", n)
		var same := f != null and code.strip_edges() == f.get_as_text().strip_edges()
		print("%s  embedded %s matches the file (%d chars)" % ["PASS" if same else "FAIL", n, code.length()])
		if not same:
			fails += 1
		var sh: Shader = ClassDB.class_call_static("S3DShaders", "load", n)
		var ok := sh != null and not sh.code.is_empty()
		print("%s  S3DShaders.load(%s) returns a shader" % ["PASS" if ok else "FAIL", n])
		if not ok:
			fails += 1
	var none: Variant = ClassDB.class_call_static("S3DShaders", "embedded_code", "nope")
	print("%s  unknown name has no code" % ("PASS" if String(none).is_empty() else "FAIL"))
	if not String(none).is_empty():
		fails += 1
	print("smoke_embedded_shaders: %s" % ("ALL PASS" if fails == 0 else "%d FAILED" % fails))
	quit(1 if fails else 0)
