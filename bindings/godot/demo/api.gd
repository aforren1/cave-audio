## Phase-2 test: drives the whole bound surface and checks it does something.
##
##   godot --headless --path bindings/godot res://demo/api.tscn -- --sink=manual
##   godot --headless --path bindings/godot res://demo/api.tscn -- --sink=null
##
## It runs on BOTH sinks, because either one alone leaves a hole.
##
## MANUAL has no device and no audio thread: blocks advance only when this script pumps
## render_block(), so playheads are functions of how many blocks were asked for rather than
## of how fast the machine is - the difference between an assertion and a coin flip. But it
## also means nothing is concurrent. The SPSC command ring has one thread on it, the push
## ring's producer and consumer are the same thread, and no voice can retire underneath a
## running mixer. None of that resembles what ships.
##
## NULL restores the audio thread - the production topology minus the device - at the cost
## of exact timing, so the same calls run again there with assertions loosened to what a
## wall clock can honestly promise.
##
## The engine's own ctest suite proves the DSP is right. What this proves is narrower and
## is what a binding can actually get wrong: that every call reaches the core with its
## arguments intact, that handles survive, and that the readbacks come back shaped right.
extends Node3D

const Tone := preload("res://demo/tone.gd")

@onready var engine: BwaEngine = $BwaEngine
@onready var emitter: BwaEmitter = $Emitter
@onready var pusher: BwaPushSource = $Pusher
@onready var bed: BwaBed = $Bed

var _fail := 0
var _manual := true
var _block_seconds := 256.0 / 48000.0


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("api[%s]: %s" % ["manual" if _manual else "null", msg])
		_fail += 1


## _enter_tree runs top-down, before any child's _ready - the only window in which the
## engine node's create-time config can still be changed.
func _enter_tree() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--sink="):
			_manual = arg.substr(7) == "manual"
	var e: BwaEngine = $BwaEngine
	e.sink = BwaEngine.SINK_MANUAL if _manual else BwaEngine.SINK_NULL


func _ready() -> void:
	# Run after BwaEngine's push+commit so each pumped block sees this frame's state.
	process_priority = 2000

	if not engine.is_running():
		push_error("api: engine not running: %s" % engine.get_last_error())
		_finish()
		return
	_block_seconds = float(engine.get_resolved_block_size()) / engine.get_resolved_sample_rate()

	_test_statics()
	_test_engine_knobs()
	_test_materials_and_scene()
	_test_render_block()
	await _test_emitter()
	await _test_push_source()
	await _test_bed()
	await _test_clock()
	_finish()


func _test_statics() -> void:
	# Engine-free registry enumeration, for a device picker. Zero drivers is a legitimate
	# answer on a machine with no ASIO installed, so only the shape is assertable.
	var n := BwaEngine.get_asio_driver_count()
	_check(n >= 0, "driver count should not be negative")
	for i in n:
		_check(BwaEngine.get_asio_driver_name(i) != "", "driver %d has an empty name" % i)
	_check(BwaEngine.get_asio_driver_name(n + 100) == "", "out-of-range driver should be empty")
	# The one-call form must agree with the index loop it replaces.
	var drivers := BwaEngine.get_asio_drivers()
	_check(drivers.size() == n, "get_asio_drivers should hold every enumerated driver")
	for i in n:
		_check(drivers[i] == BwaEngine.get_asio_driver_name(i),
			"get_asio_drivers[%d] disagrees with get_asio_driver_name" % i)
	_check(BwaEngine.get_version() > 0, "version should be non-zero")


func _test_engine_knobs() -> void:
	# The live A/B knobs. Each must round-trip through its property, which is what catches a
	# setter wired to the wrong ABI call or a getter reading the wrong field.
	engine.panner = BwaEngine.PAN_VBAP
	_check(engine.panner == BwaEngine.PAN_VBAP, "panner did not round-trip")
	engine.panner = BwaEngine.PAN_DBAP

	engine.spread_mode = BwaEngine.SPREAD_SPECTRAL
	_check(engine.spread_mode == BwaEngine.SPREAD_SPECTRAL, "spread_mode did not round-trip")
	engine.spread_mode = BwaEngine.SPREAD_LOBE

	engine.dual_band = true
	engine.dual_band_cap = true
	engine.decorrelation = true
	engine.near_spread = 1.0
	engine.hole_spread = 1.0
	engine.max_re = true
	engine.max_re_split = true
	engine.bed_renderer = BwaEngine.BED_PARAMETRIC
	engine.tracked_room_eq = false
	engine.tracked_align_dead_zone = 0.08
	engine.tracked_align_slew_frames_per_s = 128.0
	engine.tracked_align = true
	_check(engine.dual_band and engine.decorrelation and engine.max_re and engine.max_re_split,
		"a boolean A/B knob did not round-trip")
	_check(is_equal_approx(engine.near_spread, 1.0), "near_spread did not round-trip")
	_check(is_equal_approx(engine.hole_spread, 1.0), "hole_spread did not round-trip")
	_check(engine.dual_band_cap, "dual_band_cap did not round-trip")
	_check(engine.tracked_align, "tracked_align did not round-trip")
	_check(is_equal_approx(engine.tracked_align_dead_zone, 0.08), "tracked_align_dead_zone did not round-trip")
	_check(is_equal_approx(engine.tracked_align_slew_frames_per_s, 128.0), "tracked_align_slew_frames_per_s did not round-trip")

	# situation tuning: the Dictionary form is inspectable, apply_setup pushes it
	var seated := engine.get_setup_tuning(BwaEngine.SETUP_SEATED)
	var roaming := engine.get_setup_tuning(BwaEngine.SETUP_ROAMING)
	_check(seated.has("panner") and seated.size() >= 16, "setup tuning should be a full table")
	_check(seated["panner"] != roaming["panner"], "seated and roaming should differ on the panner")
	_check(bool(seated["max_re"]) and bool(roaming["max_re"]), "max-rE should be on in both setups")
	_check(engine.apply_setup(BwaEngine.SETUP_ROAMING), "apply_setup should succeed")
	_check(engine.panner == BwaEngine.PAN_DBAP, "apply_setup should mirror into the node properties")

	engine.spcap_focus = 20.0
	engine.spcap_density = 3.0
	_check(is_equal_approx(engine.spcap_focus, 20.0), "spcap_focus did not round-trip")
	_check(is_equal_approx(engine.spcap_density, 3.0), "spcap_density did not round-trip")
	engine.spcap_focus = 0.0     # 0 = back to the geometry-derived default
	engine.spcap_density = 0.0
	_check(is_equal_approx(engine.spcap_focus, 0.0), "spcap_focus default sentinel did not round-trip")

	engine.dual_band = false
	engine.decorrelation = false
	engine.max_re = false
	engine.max_re_split = false
	engine.bed_renderer = BwaEngine.BED_MATRIX
	engine.tracked_room_eq = true
	engine.tracked_align = false
	engine.tracked_align_dead_zone = 0.0
	engine.tracked_align_slew_frames_per_s = 0.0

	engine.master_gain = 0.8
	engine.limiter_ceiling = 0.7   # linear peak ceiling in (0..1]
	_check(is_equal_approx(engine.master_gain, 0.8), "master_gain did not round-trip")
	_check(is_equal_approx(engine.limiter_ceiling, 0.7), "limiter_ceiling did not round-trip")
	engine.master_gain = 1.0

	engine.group_set_gain(1, 0.5)
	engine.group_set_paused(1, false)
	engine.reverb_set_gain(0.7)
	engine.early_reflections_set_gain(0.9)
	_check(is_equal_approx(engine.reverb_gain, 0.7), "reverb_gain did not round-trip")

	# Diagnostics: a channel index inside the layout's own count, never a hard-coded 26.
	engine.set_test_signal(engine.get_channel_count() - 1, BwaEngine.TEST_SINE, 0.1)
	engine.set_test_signal(engine.get_channel_count() - 1, BwaEngine.TEST_OFF, 0.0)

	engine.set_pose_prediction(0.02)   # seconds (20 ms lead)
	engine.set_pose_prediction(0.0)
	engine.set_extra_listeners(PackedVector3Array([Vector3(1, 1.2, 0), Vector3(-1, 1.2, 0)]))
	engine.set_extra_listeners(PackedVector3Array())

	# No tracker is connected, and saying so is the only honest answer here.
	_check(engine.get_tracker_status() == BwaEngine.TRACKER_DISCONNECTED,
		"tracker should read DISCONNECTED with nothing connected")


func _test_materials_and_scene() -> void:
	# GENERIC is the canonical token 0 and is minted without spending a table slot.
	_check(engine.material_preset(BwaEngine.MAT_GENERIC) == 0, "GENERIC should be token 0")
	var brick := engine.material_preset(BwaEngine.MAT_BRICK)
	_check(brick != 0, "a non-generic preset should mint a real token")

	var custom := engine.material_define(Vector3(0.2, 0.3, 0.4), 0.5, Vector3(0.1, 0.1, 0.05))
	_check(custom != 0, "material_define should mint a token")
	_check(custom != brick, "a defined material should not collide with a preset")
	engine.material_release(custom)

	# The shoebox feeds the ray-traced scene AND the image-source reflections, so it is the
	# one scene call that matters in a build without the Steam Audio SDK.
	engine.scene_set_box(6.0, 3.0, 8.0, PackedInt32Array([brick, brick, 0, 0, brick, brick]))

	# A dynamic mesh is an app-managed plain index, not a generation-checked handle - and it
	# is -1, not 0, when it fails (no SDK is a legitimate failure here).
	var quad := PackedVector3Array([
		Vector3(-1, 0, 0), Vector3(1, 0, 0), Vector3(1, 2, 0), Vector3(-1, 2, 0)])
	var tris := PackedInt32Array([0, 1, 2, 0, 2, 3])
	var mover := engine.scene_add_dynamic_mesh(quad, tris, brick)
	if mover >= 0:
		engine.scene_set_dynamic_transform(mover, Vector3(0, 0, -2), Quaternion.IDENTITY)
		engine.scene_remove_dynamic_mesh(mover)


func _test_render_block() -> void:
	var buf := engine.render_block()
	if not _manual:
		# Documented contract: render_block belongs to the MANUAL sink and returns nothing
		# anywhere else. Worth pinning - a binding that quietly returned a stale buffer here
		# would look fine until someone built an offline render on it.
		_check(buf.is_empty(), "render_block should be empty on a non-manual sink")
		return
	var block := engine.get_resolved_block_size()
	var chans := engine.get_channel_count()
	# Planar and sized by the LAYOUT's channel count, which is the contract that breaks
	# silently on a rig with fewer than 26 speakers.
	_check(buf.size() == chans * block,
		"render_block returned %d floats, expected %d x %d" % [buf.size(), chans, block])


func _test_emitter() -> void:
	var clip := Tone.write_ping("api_ping", 440.0, 1.0)

	# Spatial surface. These are fire-and-forget in the core, so what is being checked is
	# that they reach it with a live handle and without tripping an error.
	emitter.spread = 0.5
	emitter.extent = Vector2(0.8, 0.2)   # anisotropic: wide but not tall
	emitter.size = 1.5
	emitter.doppler = true
	emitter.air_absorption = true
	emitter.loudness_comp = true
	emitter.reverb = true
	emitter.reverb_send = 0.6
	emitter.reverb_distance = true
	emitter.early_reflections = true
	emitter.group = 2
	emitter.priority = 200
	emitter.pitch = 1.5
	emitter.set_attenuation_override(1.0, 1.0, 0.05)
	emitter.set_occlusion_manual(0.5)
	emitter.set_occlusion_manual_bands(0.5, Vector3(0.9, 0.5, 0.2))
	emitter.set_directivity(0.5, 2.0)
	emitter.set_directivity_preset(BwaSource.DIR_CARDIOID)
	emitter.orientation_follows_node = true
	_check(is_equal_approx(emitter.pitch, 1.5), "pitch did not round-trip")
	_check(emitter.extent == Vector2(0.8, 0.2), "extent did not round-trip")

	# Occlusion/directivity readbacks are gains: out of range means the wrong thing is being
	# read, even where no SDK is present to move them off 1.
	_check(emitter.get_occlusion_factor() >= 0.0 and emitter.get_occlusion_factor() <= 1.0,
		"occlusion factor out of range: %f" % emitter.get_occlusion_factor())
	_check(emitter.get_directivity_gain() >= 0.0 and emitter.get_directivity_gain() <= 1.0,
		"directivity gain out of range: %f" % emitter.get_directivity_gain())

	# The playhead is the real test: it can only advance if the play command actually
	# reached the mixer with a valid sound bound.
	emitter.pitch = 1.0
	emitter.play_clip(clip)
	await _pump(20)
	var head := emitter.get_playhead_frames()
	_check(head > 0, "playhead should have advanced after 20 blocks, got %d" % head)
	_check(emitter.is_playing(), "emitter should still be playing a 1 s clip")

	# Seek lands where it was told, which a no-op binding could not fake. On the null sink
	# the voice keeps running while we look, so the landing is a floor, not a point.
	emitter.seek_frames(24000)
	await _pump(4)
	var after := emitter.get_playhead_frames()
	_check(after >= 24000, "seek to 24000 landed at %d" % after)

	# The seconds twin must land in the same place, and the frames-vs-seconds pair must be the
	# only spelling: a bare seek() would read as AudioStreamPlayer3D.seek(), which takes seconds.
	emitter.seek_seconds(0.75)
	await _pump(4)
	var after_s := emitter.get_playhead_frames()
	_check(after_s >= int(0.75 * engine.get_resolved_sample_rate()),
		"seek_seconds(0.75) landed at %d" % after_s)
	_check(not emitter.has_method("seek"), "the unit-ambiguous seek() must stay gone")

	# Pause freezes the playhead once the gate has ramped out. This one stays an equality on
	# both sinks: a frozen playhead is frozen regardless of how much wall time passes, so if
	# it moves here the pause did not take.
	emitter.paused = true
	await _pump(8)
	var frozen := emitter.get_playhead_frames()
	await _pump(8)
	_check(emitter.get_playhead_frames() == frozen,
		"paused playhead moved from %d to %d" % [frozen, emitter.get_playhead_frames()])
	emitter.paused = false

	# Gapless chaining, and the scheduled stop riding the dsp clock.
	emitter.queue(clip, false)
	emitter.clear_queue()
	emitter.stop_at(engine.get_dsp_time() + 48000)
	emitter.play_loop(clip, 1000, 20000)
	await _pump(4)
	emitter.stop()

	# One-shot: no handle to manage, and it must not disturb the named sources. The return is
	# the ONLY signal it gives, so assert it rather than assuming — a false here is the
	# difference between "played" and "clip missing" and "dropped under load".
	_check(engine.play_oneshot(clip, Vector3(1, 1, 1), 0.5), "the one-shot should be accepted")
	_check(not engine.play_oneshot("res://does_not_exist.wav", Vector3.ZERO, 1.0),
		"a one-shot on a missing clip must report the failure")
	await _pump(4)

	# Asset metadata comes back at the ENGINE rate, so a 1 s clip is one rate's worth.
	_check(engine.sound_get_channels(clip) == 1, "the ping should be mono")
	var frames := engine.sound_get_frames(clip)
	_check(absi(frames - engine.get_resolved_sample_rate()) < 2000,
		"a 1 s clip should be about one sample rate long, got %d" % frames)


func _test_push_source() -> void:
	var space := pusher.push_space()
	_check(space > 0, "a fresh push source should have ring space, got %d" % space)

	var chunk := PackedFloat32Array()
	chunk.resize(4096)
	for i in chunk.size():
		chunk[i] = sin(TAU * 440.0 * float(i) / 48000.0) * 0.4
	var accepted := pusher.push(chunk)
	_check(accepted == chunk.size(),
		"an empty ring should accept the whole chunk, took %d of %d" % [accepted, chunk.size()])

	# On the null sink the audio thread is draining the ring while we look, so the space can
	# only be checked as "less than a full ring", not against the pre-push figure.
	_check(pusher.push_space() < space, "push_space should shrink after a push")

	await _pump(10)
	_check(pusher.get_playhead_frames() > 0, "a push source should consume what it was given")

	pusher.push_end()
	_check(pusher.push(chunk) == 0, "pushes after push_end must be refused")
	await _pump(40)   # comfortably longer than the 4096 queued frames take to drain
	_check(not pusher.is_playing(), "a drained push source should have ended")


func _test_bed() -> void:
	# A bed needs a multichannel asset; handing it a mono one is a rejected mismatch, which
	# is exactly the sort of thing the wrong loader call would produce.
	var field := Tone.write_ambix("api_field", Vector3(1, 0, 0), 220.0, 1.0)
	bed.play_clip(field)
	await _pump(20)
	_check(bed.get_playhead_frames() > 0, "bed playhead should advance")
	_check(bed.is_playing(), "bed should be playing")

	# Orientation is pushed from _process, so it takes a frame to land - the point here is
	# that it round-trips and goes through the seam.
	bed.set_orientation(Vector3(PI / 2, 0.1, -0.1))
	_check(bed.get_orientation().is_equal_approx(Vector3(PI / 2, 0.1, -0.1)),
		"bed orientation did not round-trip")
	bed.set_yaw_from_basis(Basis(Vector3.UP, PI / 2))
	_check(is_equal_approx(bed.get_orientation().x, -PI / 2),
		"set_yaw_from_basis should go through the seam, got %f" % bed.get_orientation().x)

	bed.fade_to(0.5, 0.1)
	bed.seek_frames(1000)
	_check(not bed.has_method("seek"), "the bed's unit-ambiguous seek() must stay gone")
	await _pump(4)
	bed.fade_out(0.05)
	await _pump(10)


func _test_clock() -> void:
	var t := engine.get_dsp_time()
	_check(t > 0, "dsp time should have advanced after all those blocks")
	await _pump(4)
	_check(engine.get_dsp_time() > t, "dsp time should be monotonic")

	var c := engine.get_clock()
	_check(c.has("valid") and c.has("dsp_sample") and c.has("host_time_ns"),
		"get_clock should return all three keys")
	_check(c["valid"], "a rendered block should have stamped the clock pair")

	if _manual:
		# The MANUAL sink stamps a NOMINAL time derived from the sample position rather than
		# a wall clock, so a fixed call sequence still renders identically. That makes this
		# the one sink where the pair has a predictable value instead of whatever the device
		# happened to say - so pin it.
		var expect_ns := int(float(c["dsp_sample"]) / engine.get_resolved_sample_rate() * 1e9)
		_check(absi(c["host_time_ns"] - expect_ns) < 1000,
			"manual host time should be sample/rate, got %d expected ~%d"
				% [c["host_time_ns"], expect_ns])
	else:
		# The null sink stamps real elapsed time from stream start, so only monotonicity is
		# assertable - but that much must hold, or the seqlock is handing back garbage.
		_check(c["host_time_ns"] > 0, "the null sink should stamp a real elapsed time")
		await _pump(10)
		var c2 := engine.get_clock()
		_check(c2["host_time_ns"] > c["host_time_ns"], "host time should advance")
		_check(c2["dsp_sample"] > c["dsp_sample"], "dsp sample should advance")

	# Both units, and the name that no longer collides with AudioServer.get_output_latency()
	# (which is SECONDS). A device-less sink reports 0, which is 0 in either unit — exactly why
	# the collision hid: the assertion below cannot tell the units apart, and neither could a
	# reader of the old name.
	_check(engine.get_output_latency_frames() == 0, "a sink with no device has no output latency")

	# Device health. The two sinks answer differently ON PURPOSE, and that difference is the
	# feature: the null sink has a thread on a real deadline so it MEASURES, while the manual sink
	# has no deadline and must say it cannot know rather than report a clean bill.
	var health := engine.get_health()
	_check(health["measured"] == not _manual,
		"health.measured should be %s on the %s sink" % [not _manual, "manual" if _manual else "null"])
	_check(health["xruns"] == 0, "no xruns should be reported off-hardware")
	_check(engine.get_xruns() == 0, "the one-line form should agree")
	# stream_starves is deliberately NOT asserted zero: _test_push_source drains its ring and
	# pumps past the pushed data, which is a real starve and is counted as one. That it shows up
	# here is the counter working - a push source that runs dry renders silence indistinguishable
	# from the end of a clip, which is the whole reason the two are counted separately.
	_check(health["stream_starves"] >= 0, "stream_starves should be a count")
	if not _manual:
		_check(health["blocks"] > 0, "the null sink should have counted blocks")
	_check(engine.get_output_latency_seconds() == 0.0, "no device means no latency in seconds either")
	_check(not engine.has_method("get_output_latency"),
		"the unit-ambiguous name must stay gone - it read as Godot's seconds-valued call")


## Advance the engine by at least n blocks.
##
## On MANUAL this is exact: nothing renders unless asked, so n blocks is n blocks and the
## assertions above can be equalities. On NULL the audio thread runs on its own, so the best
## available is to wait out the wall time those blocks take, with slack - hence the looser
## assertions on that side.
func _pump(n: int) -> void:
	if _manual:
		for i in n:
			engine.render_block()
		return
	await get_tree().create_timer(n * _block_seconds * 3.0 + 0.05).timeout


func _finish() -> void:
	print("api: ", "PASS" if _fail == 0 else "FAIL (%d)" % _fail)
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if _fail == 0 else 1)
