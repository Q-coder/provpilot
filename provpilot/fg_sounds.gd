extends Node
class_name FGSounds
## FlightGear sound set runtime (docs/A10_FLIGHTGEAR_PORT.md, subtask 8).
##
## Plays the sounds of a FlightGear <fx> configuration exported by
## scripts/fg_a10_sounds.py (sounds.json + .wav files) from the same property
## values the animator gets (FGBridge). Per sound:
##   mode looped     — plays while its <condition> holds
##   mode once       — plays once when the condition becomes true (single alike)
##   mode in-transit — plays while its top-level <property> keeps changing
##   <volume>        — product of entries: value = property (or 1), <type>
##                     linear / log / ln / inverse / square, × factor + offset,
##                     clamped to min / max; <internal>dt_play = seconds playing
##   <pitch>         — same, default 1
## Positional sounds are played as plain 2D sounds; the generic FlightGear
## files the package does not ship (rumble, gear, flaps, squeal) are skipped.

var user_volume := 0.7            # settings engine_volume / 100
## Listening from outside: gain x reference / max(distance, reference), silent
## beyond max distance (FlightGear's positional model; per-sound
## <reference-dist> / <max-dist> when given). Cockpit sounds are not attenuated.
const REFERENCE_DIST_M := 60.0
const MAX_DIST_M := 8000.0
var camera_distance_m := 0.0      # camera to aircraft; 0 while in the cockpit
var count := 0
var skipped: Array = []
var _sounds: Array = []           # {rec, player, stream, mode, cond, prop, last, play_t, was_true, moving_t}
var _props: Dictionary = {}


func load(json_path: String, sounds_dir: String) -> int:
	clear()
	var f := FileAccess.open(json_path, FileAccess.READ)
	if f == null:
		return 0
	var m: Dictionary = JSON.parse_string(f.get_as_text())
	var streams: Dictionary = {}
	for rec in m.get("sounds", []):
		var file: String = str(rec.get("file", ""))
		if file.is_empty():
			skipped.append(str(rec.get("name", "")))
			continue
		var looping: bool = rec.get("mode", "once") in ["looped", "in-transit"]
		var key := file + ("#loop" if looping else "")
		if not streams.has(key):
			var st = AudioStreamWAV.load_from_file(sounds_dir.path_join(file))
			if st == null:
				skipped.append(str(rec.get("name", "")))
				continue
			if looping:
				st.loop_mode = AudioStreamWAV.LOOP_FORWARD
				st.loop_begin = 0
				var bytes_per_frame: int = (2 if st.format == AudioStreamWAV.FORMAT_16_BITS else 1) * (2 if st.stereo else 1)
				st.loop_end = st.data.size() / maxi(bytes_per_frame, 1)
			streams[key] = st
		var p := AudioStreamPlayer.new()
		p.name = "snd_" + str(rec.get("name", "x")).validate_node_name()
		p.stream = streams[key]
		p.bus = "Master"
		p.volume_db = -80.0
		add_child(p)
		var raw: Dictionary = rec.get("raw", {})
		_sounds.append({"rec": rec, "player": p, "mode": str(rec.get("mode", "once")), "cond": raw.get("condition", null),
			"prop": _norm(str(raw.get("property", ""))) if raw.get("property", null) is String else "",
			"last": NAN, "play_t": 0.0, "was_true": false, "moving_t": 0.0,
			"vol": _as_list(raw.get("volume", [])), "pitch": _as_list(raw.get("pitch", []))})
		count += 1
	print("FGSounds: %d sounds loaded, %d skipped (no file)" % [count, skipped.size()])
	return count


func clear() -> void:
	for s in _sounds:
		if is_instance_valid(s.player):
			s.player.queue_free()
	_sounds.clear()
	skipped.clear()
	count = 0


static func _as_list(v) -> Array:
	if v is Array:
		return v
	if v == null or (v is String and str(v).is_empty()):
		return []
	return [v]


static func _norm(p: String) -> String:
	var s := p.strip_edges()
	while s.begins_with("/"):
		s = s.substr(1)
	return s.replace("[0]", "")


func _val(prop: String) -> float:
	var v = _props.get(_norm(prop), 0.0)
	if v is bool:
		return 1.0 if v else 0.0
	if v is String:
		return str(v).to_float() if str(v).is_valid_float() else (1.0 if str(v) == "true" else 0.0)
	return float(v)


## FlightGear condition (same syntax as the animations): dict of operators.
func _cond(c) -> bool:
	if c == null:
		return true
	if c is String:
		return _val(c) != 0.0
	if not (c is Dictionary):
		return true
	var d: Dictionary = c
	var result := true
	for key in d.keys():
		var items: Array = _as_list(d[key])
		for body in items:
			var r := true
			match str(key):
				"and":
					r = _cond(body)
				"or":
					r = false
					if body is Dictionary:
						for k2 in (body as Dictionary).keys():
							for b2 in _as_list((body as Dictionary)[k2]):
								var one := {}
								one[k2] = b2
								if _cond(one):
									r = true
					else:
						r = _cond(body)
				"not":
					r = not _cond(body)
				"property":
					r = _val(str(body)) != 0.0
				"equals", "not-equals", "less-than", "less-than-equals", "greater-than", "greater-than-equals":
					r = _compare(str(key), body)
				_:
					r = true
			result = result and r
	return result


func _compare(op: String, body) -> bool:
	if not (body is Dictionary):
		return false
	var b: Dictionary = body
	var props: Array = _as_list(b.get("property", []))
	if props.is_empty():
		return false
	var a: float = _val(str(props[0]))
	var rhs: float = _val(str(props[1])) if props.size() > 1 else float(str(b.get("value", "0"))) if str(b.get("value", "0")).is_valid_float() else 0.0
	if b.has("value") and not str(b["value"]).is_valid_float():
		var sv := str(_props.get(_norm(str(props[0])), ""))
		return (sv == str(b["value"])) if op == "equals" else (sv != str(b["value"]))
	match op:
		"equals": return absf(a - rhs) < 1e-9
		"not-equals": return absf(a - rhs) >= 1e-9
		"less-than": return a < rhs
		"less-than-equals": return a <= rhs
		"greater-than": return a > rhs
		"greater-than-equals": return a >= rhs
	return false


## Product of <volume>/<pitch> entries.
func _factor(entries: Array, s: Dictionary, default_value: float) -> float:
	if entries.is_empty():
		return default_value
	var total := 1.0
	for e in entries:
		if not (e is Dictionary):
			continue
		var d: Dictionary = e
		var v := 1.0
		if d.has("property"):
			v = _val(str(d["property"]))
		elif str(d.get("internal", "")) == "dt_play":
			v = float(s.play_t)
		match str(d.get("type", "linear")):
			"log": v = log(maxf(v, 1e-6)) / log(10.0) if v > 0.0 else 0.0
			"ln": v = log(maxf(v, 1e-6)) if v > 0.0 else 0.0
			"inverse": v = 1.0 / v if absf(v) > 1e-9 else 0.0
			"square": v = v * v
		v = v * float(str(d.get("factor", "1"))) + float(str(d.get("offset", "0")))
		if d.has("min"):
			v = maxf(v, float(str(d["min"])))
		if d.has("max"):
			v = minf(v, float(str(d["max"])))
		total *= v
	return total


## Per frame with the FlightGear property values of this frame (keys are
## normalised like the animator does: no leading "/", no "[0]").
func update(props: Dictionary, delta: float) -> void:
	_props.clear()
	for k in props.keys():
		_props[_norm(str(k))] = props[k]
	for s in _sounds:
		var p: AudioStreamPlayer = s.player
		var want := false
		match s.mode:
			"looped":
				want = _cond(s.cond)
			"in-transit":
				var v: float = _val(s.prop) if s.prop != "" else 0.0
				if not is_nan(float(s.last)) and absf(v - float(s.last)) > 1e-5:
					s.moving_t = 0.25
				s.last = v
				s.moving_t = maxf(float(s.moving_t) - delta, 0.0)
				want = float(s.moving_t) > 0.0
			_:
				# once / single: rising edge of the condition
				var now: bool = _cond(s.cond)
				if now and not bool(s.was_true):
					p.play()
					s.play_t = 0.0
					s.plays = int(s.get("plays", 0)) + 1
				s.was_true = now
				want = p.playing
		if s.mode in ["looped", "in-transit"]:
			if want and not p.playing:
				p.play()
				s.play_t = 0.0
			elif not want and p.playing:
				p.stop()
		if p.playing:
			s.play_t = float(s.play_t) + delta
			var gain: float = clampf(_factor(s.vol, s, 1.0), 0.0, 4.0) * user_volume
			if camera_distance_m > 0.0:
				var raw: Dictionary = s.rec.get("raw", {})
				var ref: float = float(str(raw.get("reference-dist", REFERENCE_DIST_M)))
				var mx: float = float(str(raw.get("max-dist", MAX_DIST_M)))
				gain *= 0.0 if camera_distance_m >= mx else ref / maxf(camera_distance_m, ref)
			p.volume_db = linear_to_db(gain) if gain > 0.0005 else -80.0
			p.pitch_scale = clampf(_factor(s.pitch, s, 1.0), 0.05, 4.0)


## Names of the sounds playing now (tests).
func playing() -> Array:
	var out: Array = []
	for s in _sounds:
		if s.player.playing:
			out.append(str(s.rec.get("name", "")))
	return out


## How often a once-sound was triggered (tests).
func play_count(name: String) -> int:
	for s in _sounds:
		if str(s.rec.get("name", "")) == name:
			return int(s.get("plays", 0))
	return 0


func volume_db_of(name: String) -> float:
	for s in _sounds:
		if str(s.rec.get("name", "")) == name:
			return s.player.volume_db
	return -999.0
