## The desk fallback: a rig scene (profile Cave, a named device) opened on a machine without
## that device runs a headphone profile instead, when the scene opted in.
##
##   godot --headless --path bindings/godot res://demo/fallback.tscn
##
## Exits 0 on pass, 1 on fail. Hermetic on purpose: the primary that is "missing" names a
## device no machine has, and every fallback lands on the offline sink, so nothing here depends
## on the audio hardware of the box that runs it.
##
## What the native engine was created with is read from get_audio_backend(), which names the
## decode: "null" for Cave, "null (... sim)" for CaveSim, "null (... direct)" for Binaural.
## get_active_profile() alone would only repeat what the binding decided.
##
## Not covered, and it cannot be from a scene: the export half of fallback_only_in_editor.
## This runs under the editor binary, so the gate is always open here. The push_warning and
## the "listed but did not open" push_error are not asserted either: GDScript cannot read the
## log back, and no machine can stage a device that is listed and does not open.
extends Node

const NO_DEVICE := "bwa_test_no_such_device"
const NO_LAYOUT := "res://demo/bwa_test_no_such_layout.json"
const BAD_LAYOUT := "user://bwa_test_bad_layout.json"

var _fail := 0


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("fallback: " + msg)
		_fail += 1


## One engine on the rig's settings (Cave on a named ASIO device that is not here), adjusted by
## `setup`, then added to the tree: its _ready has run on return.
func _engine(setup: Callable) -> BwaEngine:
	var e := BwaEngine.new()
	e.profile = BwaEngine.PROFILE_CAVE
	e.sink = BwaEngine.SINK_ASIO
	e.device = NO_DEVICE
	e.fallback_sink = BwaEngine.SINK_NULL
	setup.call(e)
	add_child(e)
	return e


func _drop(e: BwaEngine) -> void:
	remove_child(e)
	e.free()


func _ready() -> void:
	_test_rule()

	# The rig scene at a desk: the named ASIO device is not here, CaveSim on the offline sink is.
	var e := _engine(func(x): x.fallback_profile = BwaEngine.FALLBACK_CAVE_SIM)
	_check(e.is_running(), "the fallback did not start: %s" % e.get_last_error())
	_check(e.is_using_fallback(), "is_using_fallback() is false on the fallback")
	_check(e.get_active_profile() == BwaEngine.PROFILE_CAVE_SIM, "active profile is not CaveSim")
	_check(e.profile == BwaEngine.PROFILE_CAVE and e.sink == BwaEngine.SINK_ASIO,
			"the rig settings were edited")
	var b := e.get_audio_backend()
	_check(b.begins_with("null (") and b.ends_with("sim)"), "engine is not on CaveSim: '%s'" % b)
	_drop(e)

	# AUTO with a named device no backend lists: today that is the silent offline sink on Cave.
	# With the fallback it is Binaural, which also tolerates the layout file being absent.
	e = _engine(func(x):
		x.sink = BwaEngine.SINK_AUTO
		x.layout_path = NO_LAYOUT
		x.fallback_profile = BwaEngine.FALLBACK_BINAURAL)
	_check(e.is_running(), "a Binaural fallback refused a missing layout: %s" % e.get_last_error())
	_check(e.is_using_fallback(), "AUTO with a missing named device took no fallback")
	_check(e.get_active_profile() == BwaEngine.PROFILE_BINAURAL, "active profile is not Binaural")
	_check(e.get_audio_backend().ends_with("direct)"),
			"engine is not on Binaural: '%s'" % e.get_audio_backend())
	_drop(e)

	# CaveSim pans over the layout, so the same missing file refuses the start.
	e = _engine(func(x):
		x.layout_path = NO_LAYOUT
		x.fallback_profile = BwaEngine.FALLBACK_CAVE_SIM)
	_check(not e.is_running(), "CaveSim started over a layout file that does not exist")
	_check(not e.is_using_fallback(), "is_using_fallback() is true with no engine running")
	_check(e.get_active_profile() == BwaEngine.PROFILE_CAVE,
			"active profile still names a fallback that did not start")
	_drop(e)

	# A fallback that ran and then left the tree is not in use either.
	e = _engine(func(x): x.fallback_profile = BwaEngine.FALLBACK_CAVE_SIM)
	_check(e.get_active_profile() == BwaEngine.PROFILE_CAVE_SIM, "the fallback did not run")
	remove_child(e)
	_check(e.get_active_profile() == BwaEngine.PROFILE_CAVE,
			"active profile still names the fallback after _exit_tree")
	_check(not e.is_using_fallback(), "is_using_fallback() after _exit_tree")
	e.free()

	# fallback_profile NONE is the feature off: a named backend with a missing device fails.
	e = _engine(func(_x): pass)
	_check(not e.is_running(), "a missing ASIO device started with no fallback set")
	_check(not e.is_using_fallback(), "is_using_fallback() with the feature off")
	_drop(e)

	# The offline sink opens no device, so it is always present: the primary runs.
	e = _engine(func(x):
		x.sink = BwaEngine.SINK_NULL
		x.device = ""
		x.fallback_profile = BwaEngine.FALLBACK_BINAURAL)
	_check(e.is_running(), "the primary did not start: %s" % e.get_last_error())
	_check(not e.is_using_fallback(), "a present primary took the fallback")
	_check(e.get_active_profile() == BwaEngine.PROFILE_CAVE, "active profile moved off Cave")
	_check(e.get_audio_backend() == "null", "engine is not on Cave: '%s'" % e.get_audio_backend())
	_drop(e)

	# A primary that is present and fails to start must fail: no second try on the fallback.
	# The offline sink is present; Cave with a missing layout is refused by bwa_start. The
	# Binaural fallback would have started over that same file, so a retry shows as running.
	e = _engine(func(x):
		x.sink = BwaEngine.SINK_NULL
		x.device = ""
		x.layout_path = NO_LAYOUT
		x.fallback_profile = BwaEngine.FALLBACK_BINAURAL)
	_check(not e.is_running(), "a failed start was retried on the fallback")
	_check(not e.is_using_fallback(), "is_using_fallback() after a failed primary start")
	_drop(e)

	# Binaural as the PRIMARY profile tolerates a missing layout file too, as Unity's Engine
	# does: no speakers are driven, so the default grid serves the diffuse bed.
	e = _engine(func(x):
		x.profile = BwaEngine.PROFILE_BINAURAL
		x.sink = BwaEngine.SINK_NULL
		x.device = ""
		x.layout_path = NO_LAYOUT)
	_check(e.is_running(), "Binaural refused a missing layout file: %s" % e.get_last_error())
	_check(not e.is_using_fallback(), "Binaural on the offline sink took a fallback")
	_check(e.get_channel_count() == 26, "not the default grid: %d channels" % e.get_channel_count())
	_check(e.get_audio_backend().ends_with("direct)"),
			"engine is not on Binaural: '%s'" % e.get_audio_backend())
	_drop(e)

	# The tolerance is for a file that is ABSENT. One that exists still has to load.
	var bad := FileAccess.open(BAD_LAYOUT, FileAccess.WRITE)
	bad.store_string("{ this is not a layout")
	bad.close()
	e = _engine(func(x):
		x.profile = BwaEngine.PROFILE_BINAURAL
		x.sink = BwaEngine.SINK_NULL
		x.device = ""
		x.layout_path = BAD_LAYOUT)
	_check(not e.is_running(), "Binaural started over a layout file that exists and does not load")
	_drop(e)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(BAD_LAYOUT))

	# CaveSim as the primary profile still refuses the missing file.
	e = _engine(func(x):
		x.profile = BwaEngine.PROFILE_CAVE_SIM
		x.sink = BwaEngine.SINK_NULL
		x.device = ""
		x.layout_path = NO_LAYOUT)
	_check(not e.is_running(), "CaveSim as the profile started over a missing layout file")
	_drop(e)

	print("fallback: ", "PASS" if _fail == 0 else "FAIL (%d)" % _fail)
	get_tree().quit(0 if _fail == 0 else 1)


## The presence rule, against whatever this machine lists. The fixed cases hold everywhere; the
## per-device ones run for each device the box has.
func _test_rule() -> void:
	var cave := BwaEngine.PROFILE_CAVE
	var bin := BwaEngine.PROFILE_BINAURAL
	for p in [cave, bin, BwaEngine.PROFILE_CAVE_SIM, BwaEngine.PROFILE_CAVE_BOTH]:
		_check(BwaEngine.is_device_present(p, BwaEngine.SINK_NULL, NO_DEVICE), "NULL needs no device")
		_check(BwaEngine.is_device_present(p, BwaEngine.SINK_MANUAL, ""), "MANUAL needs no device")
		_check(not BwaEngine.is_device_present(p, BwaEngine.SINK_AUTO, NO_DEVICE),
				"AUTO found a device nobody has")

	var stereo_only := [BwaEngine.SINK_WASAPI, BwaEngine.SINK_AAUDIO, BwaEngine.SINK_WORKLET]
	var array_backends := [BwaEngine.SINK_ASIO, BwaEngine.SINK_COREAUDIO, BwaEngine.SINK_ALSA,
			BwaEngine.SINK_JACK]
	var array_names := {}
	for s in array_backends:
		for i in BwaEngine.get_device_count(s):
			array_names[BwaEngine.get_device_name(s, i)] = true
			array_names[BwaEngine.get_device_id(s, i)] = true
	var any_array := false
	# ASIO, WASAPI and CoreAudio open a device from their list, so an unlisted name is missing.
	# The other four open strings their list never held (a JACK port pattern, an ALSA PCM like
	# hw:0,0, an AAudio id), so a named device there is present: the rig's "hw:1,0" must never
	# read as missing and send the rig to headphones.
	var unlisted := [BwaEngine.SINK_JACK, BwaEngine.SINK_ALSA, BwaEngine.SINK_AAUDIO,
			BwaEngine.SINK_WORKLET]
	# ...but only where the engine library carries the backend. An ALSA scene on a Windows desk
	# is a missing device: that machine is not the rig, and the start would fail there.
	var carried: Array = {
		"Windows": [BwaEngine.SINK_ASIO, BwaEngine.SINK_WASAPI],
		"Linux": [BwaEngine.SINK_JACK, BwaEngine.SINK_ALSA],
		"macOS": [BwaEngine.SINK_COREAUDIO],
		"Android": [BwaEngine.SINK_AAUDIO],
	}.get(OS.get_name(), [])
	for s in array_backends + stereo_only:
		var want: bool = s in unlisted and s in carried
		for p in [cave, bin]:
			_check(BwaEngine.is_device_present(p, s, NO_DEVICE) == want,
					"sink %d with a device name its list does not hold" % s)
		if s in unlisted:
			_check(BwaEngine.is_device_present(cave, s, "hw:1,0") == (s in carried),
					"sink %d and hw:1,0" % s)
		var n := BwaEngine.get_device_count(s)
		_check(BwaEngine.is_device_present(cave, s, "") == (n > 0),
				"sink %d with no device name" % s)
		if n > 0 and not s in stereo_only:
			any_array = true
		for i in n:
			var dev_name := BwaEngine.get_device_name(s, i)
			var id := BwaEngine.get_device_id(s, i)
			if dev_name.is_empty():
				continue
			_check(BwaEngine.is_device_present(cave, s, dev_name),
					"sink %d misses its own '%s'" % [s, dev_name])
			if not id.is_empty():
				_check(BwaEngine.is_device_present(cave, s, id),
						"sink %d misses its own id '%s'" % [s, id])
			_check(BwaEngine.is_device_present(bin, BwaEngine.SINK_AUTO, dev_name),
					"AUTO does not offer a stereo profile '%s'" % dev_name)
			if not s in stereo_only:
				_check(BwaEngine.is_device_present(cave, BwaEngine.SINK_AUTO, dev_name),
						"AUTO does not offer the array '%s'" % dev_name)
			elif not array_names.has(dev_name):
				_check(not BwaEngine.is_device_present(cave, BwaEngine.SINK_AUTO, dev_name),
						"AUTO would hand the array to stereo-only '%s'" % dev_name)
			# Exact: a listed name with more text after it is another device.
			if not s in unlisted:
				_check(not BwaEngine.is_device_present(cave, s, dev_name + "_bwa_test_suffix"),
						"the device match is not exact")
	_check(BwaEngine.is_device_present(cave, BwaEngine.SINK_AUTO, "") == any_array,
			"AUTO with no device name on the array")
