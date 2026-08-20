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
	await _test_directivity_aim()   # BEFORE any scene exists - see the function comment
	_test_materials_and_scene()
	_test_render_block()
	await _test_emitter()
	await _test_push_source()
	await _test_bed()
	await _test_clock()
	await _test_assets_and_desc()
	await _test_teardown_order()   # LAST: it frees the engine node
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


## set_orientation must land the audible dipole on the NODE's facing (its -Z), through the
## same seam push_frame uses - not on the raw quaternion's room reading, which is a silent
## half-turn. A dedicated source keeps this away from _test_emitter's manual-occlusion
## publish, which takes over the directivity readback for that voice.
##
## Runs BEFORE _test_materials_and_scene so no authored geometry muddies the reading. The
## readback is POLLED, not sampled: in a Steam Audio build the sim owns directivity and
## publishes on its own wall-clock tick (which no fixed number of pumped blocks can wait
## out on the manual sink); without the SDK the rt dipole needs a rendered block. The
## polling loop covers both, and the timeout turns "never converges" into a failure.
func _test_directivity_aim() -> void:
	var clip := Tone.write_ping("api_dir", 330.0, 1.0)
	var dir_src := BwaEmitter.new()
	dir_src.autoplay = false
	dir_src.loop = true                    # outlive the polling below regardless of sink pace
	dir_src.position = Vector3(0, 0, -2)   # listener at the origin; source 2 m out on -Z
	add_child(dir_src)
	dir_src.play_clip(clip)
	dir_src.set_directivity_preset(BwaSource.DIR_CARDIOID)
	# TWO frame awaits: process_frame fires BEFORE node processing, so one await resumes
	# with the engine's push+commit not yet run this frame - fatal on the manual sink,
	# where nothing else advances the frame and the voice would render at the origin.
	await get_tree().process_frame
	await get_tree().process_frame         # the engine pushed the position and committed

	# Node identity faces Godot -Z: dead away from the listener, a cardioid's near-null.
	dir_src.set_orientation(Quaternion.IDENTITY)
	var away := await _wait_directivity(dir_src, 0.2, true)
	# The about-face aims the node's -Z at the listener: the cardioid's on-axis 1.
	dir_src.set_orientation(Quaternion(Vector3.UP, PI))
	var toward := await _wait_directivity(dir_src, 0.8, false)
	_check(away < 0.2, "a cardioid facing away from the listener should be near its null, got %f" % away)
	_check(toward > 0.8, "a cardioid facing the listener should be near on-axis, got %f" % toward)
	dir_src.free()


## Poll the directivity readback until it crosses `bound` (below it when `want_low`), or
## give up after 3 s and return the last reading for the assertion to report.
func _wait_directivity(src: BwaSource, bound: float, want_low: bool) -> float:
	var g: float = src.get_directivity_gain()
	var waited := 0.0
	while waited < 3.0:
		if (want_low and g < bound) or (not want_low and g > bound):
			break
		await get_tree().create_timer(0.05).timeout
		if _manual:
			for i in 2:
				engine.render_block()   # commands and ramps only advance with blocks here
		waited += 0.05
		g = src.get_directivity_gain()
	return g


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

	# Metadata answers for the asset as LOADED: the field is cached as an ambisonic bed, so
	# its real channel count comes back - not a hidden second MONO decode's 1, which is what
	# a load-as-side-effect getter produced. And a getter must not load: a path this engine
	# never touched reports 0, it does not silently decode a file.
	_check(engine.sound_get_channels(field) == 4,
		"an order-1 bed should report 4 channels, got %d" % engine.sound_get_channels(field))
	_check(engine.sound_get_frames(field) > 0, "a loaded bed should report its length")
	_check(engine.sound_get_channels("user://never_loaded.wav") == 0,
		"metadata getters must not decode uncached paths as a side effect")

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



## The convenience tier: the shared asset cache, async loading, and the source desc.
##
## The cache lives in the CORE now (bwa_sound_acquire, keyed on path plus load flags), so
## what is checked here is the seam the binding still owns: the record that turns a PATH
## back into a handle, which no ABI call can do. Every by-path call below rides it.
func _test_assets_and_desc() -> void:
	# bwa_source_preset is pure, so the table answers with nothing running. The three fields
	# asserted are the ones the C header ARGUES for, not incidental defaults.
	var default_preset := BwaSource.get_preset(BwaSource.KIND_DEFAULT)
	_check(is_equal_approx(default_preset["gain"], 1.0), "the default preset should be unity gain")
	_check(default_preset["priority"] == 128, "the default preset should hold the default priority")
	_check(is_equal_approx(BwaSource.get_preset(BwaSource.KIND_AMBIENCE)["spread"], 1.0),
		"an ambience source should be wide")
	_check(BwaSource.get_preset(BwaSource.KIND_VOICE)["priority"] == 255,
		"a voice must not be stolen")
	# rolloff 0 alone proves nothing - the DEFAULT preset reads 0 there too, because a desc with
	# no attenuation override zeroes the whole triple. What says "UI" is the override being ON,
	# so assert the ref distance that enables it and the rolloff it enables it FOR.
	var ui := BwaSource.get_preset(BwaSource.KIND_UI)
	_check(is_equal_approx(ui["atten_ref_dist"], 1.0),
		"a UI source should carry a distance-attenuation override, not the layout curve")
	_check(is_equal_approx(ui["atten_rolloff"], 0.0), "a UI source should not obey distance")
	_check(is_equal_approx(BwaSource.get_preset(BwaSource.KIND_DEFAULT)["atten_ref_dist"], 0.0),
		"the default preset must NOT override the distance curve")

	var src := BwaEmitter.new()
	src.autoplay = false
	add_child(src)

	# A round-trip must be a no-op: get_desc feeds apply_desc unchanged.
	src.set_gain(0.4)
	src.set_size(2.0)
	src.doppler = true
	var desc := src.get_desc()
	_check(is_equal_approx(desc["gain"], 0.4), "get_desc should report the gain that was set")
	_check(is_equal_approx(desc["size_m"], 2.0), "get_desc should report the metric size")
	_check(desc["doppler"], "get_desc should report the propagation switches")
	_check(src.apply_desc(desc), "a desc read back from the source must be accepted")
	_check(is_equal_approx(src.get_desc()["gain"], 0.4), "the round-trip changed the gain")

	# Overlay, not replace: an absent key keeps its value, which is what makes a one-field
	# edit from script possible at all.
	_check(src.apply_desc({"gain": 0.75}), "a partial desc should be accepted")
	_check(is_equal_approx(src.get_desc()["gain"], 0.75), "the overlay did not take")
	_check(is_equal_approx(src.get_desc()["size_m"], 2.0),
		"an absent key must not reset the field it names")
	_check(is_equal_approx(src.get_gain(), 0.75), "the node property must mirror an applied desc")

	# The reset the setters could not express.
	_check(src.reset_to_preset(BwaSource.KIND_AMBIENCE), "reset_to_preset should be accepted")
	_check(is_equal_approx(src.get_desc()["spread"], 1.0), "the preset reset did not take")
	_check(not src.get_desc()["doppler"], "the preset reset should have cleared doppler")

	# Assets by path. preload warms the core cache; the metadata getters read the record it
	# leaves behind, and unload_sound_path drops every form of the path.
	var clip := Tone.write_ping("api_preload", 550.0, 1.0)
	_check(engine.preload_sound(clip), "preload_sound should accept a real file")
	_check(engine.sound_is_ready(clip), "a synchronous preload is ready the moment it returns")
	_check(engine.sound_get_channels(clip) == 1, "a preloaded ping should be mono")
	_check(not engine.preload_sound("res://does_not_exist.wav"),
		"preload_sound must report a missing file")
	_check(not engine.sound_is_ready("res://does_not_exist.wav"),
		"a path that never loaded is not ready")
	engine.unload_sound_path(clip)
	_check(engine.sound_get_channels(clip) == 0, "unload_sound_path should drop the record")
	_check(not engine.sound_is_ready(clip), "an unloaded path is not ready")

	# The same file held twice under DIFFERENT flags is two assets, which is the whole reason
	# the key is (path, flags) - and one unload_sound_path still drops both.
	_check(engine.preload_sound(clip), "re-acquiring an unloaded path should work")
	_check(engine.preload_sound(clip, BwaEngine.LOAD_STREAM), "the streamed form is its own asset")
	# Both forms answer for themselves. This cannot see the core's KEY from here (no handle
	# reaches GDScript), only that neither acquire disturbed the other and both are playable.
	_check(engine.sound_is_ready(clip), "the in-memory form should survive acquiring the streamed one")
	_check(engine.sound_is_ready(clip, BwaEngine.LOAD_STREAM), "the streamed form should be resident")
	engine.unload_sound_path(clip)
	_check(not engine.sound_is_ready(clip, BwaEngine.LOAD_STREAM),
		"unload_sound_path must drop every flag variant of the path")
	# ...and ask the CORE, not this node's record: sound_get_channels goes through bwa_sound_find,
	# which reports every flag variant the engine still holds. 0 means both were really released.
	_check(engine.sound_get_channels(clip) == 0,
		"unload_sound_path must release both variants in the core, not just forget them")

	# Async: the handle is usable at once and the data lands later. A play against it is HELD,
	# so the emitter must not read the silent window as an end - that is the regression this
	# guards, and `finished` firing here would be the symptom.
	var late := Tone.write_ping("api_async", 660.0, 1.0)
	_check(engine.preload_sound_async(late), "preload_sound_async should accept a real file")
	src.async_load = true
	var ended := [false]
	src.finished.connect(func() -> void: ended[0] = true)
	src.play_clip(late)
	# HONEST LIMIT: whether this play landed in the HELD window depends on whether the decode beat
	# it, and the binding checks readiness on the way to playing, which itself adopts a finished
	# decode. So this fixture asserts the OUTCOME, which holds either way, and does not claim to
	# have exercised the hold. The held path is covered deterministically in test/assets_test.c,
	# which can order its calls so nothing adopts the decode before the play.
	for i in 60:
		if engine.sound_is_ready(late):
			break
		await get_tree().process_frame
	_check(engine.sound_is_ready(late), "the async load never landed")
	await _pump(20)
	_check(not ended[0], "a held async play must not announce a finish")
	_check(src.get_playhead_frames() > 0, "an async clip should play once its data lands")
	_check(not src.is_loading(), "is_loading must clear once the data has landed")

	# Scene transitions. Both stop every voice and neither may read as a natural end.
	src.set_group(3)
	engine.group_stop(3)
	await _pump(8)
	_check(not src.is_playing(), "group_stop should have stopped its member")
	src.play_clip(late)
	await _pump(4)
	engine.stop_all()
	await _pump(8)
	_check(not src.is_playing(), "stop_all should have stopped every voice")
	_check(not ended[0], "a stopped voice is not a finished one")
	src.free()


## Both teardown orders, for every class holding a BwaEngine back-pointer. A client freed
## BEFORE the engine must leave its registry (or the engine's own teardown walks a freed
## child); the engine freed FIRST must detach every survivor (or the survivor's next call
## - a bed setter, a speaker view's _process tick - is a heap use-after-free). Runs last:
## the engine node does not come back.
func _test_teardown_order() -> void:
	var view_first := BwaSpeakerView.new()
	add_child(view_first)
	view_first.free()          # child-first: the registry must forget it before the engine dies

	var view := BwaSpeakerView.new()
	add_child(view)
	var geo := BwaDynamicGeometry.new()
	geo.mesh = BoxMesh.new()
	add_child(geo)

	engine.free()              # engine-first: emitter, pusher, bed, view, geo all outlive it

	bed.set_gain(0.5)          # every survivor must take calls as a quiet no-op now
	bed.stop()
	_check(not bed.is_playing(), "a bed with no engine cannot be playing")
	emitter.set_gain(0.5)
	emitter.stop()
	# Let the tree tick the survivors' _process with the engine gone - the exact frame
	# that dereferenced the freed engine node before the detach protocol covered them.
	await get_tree().process_frame
	await get_tree().process_frame
	_check(view.get_speaker_count() >= 0, "a detached view still answers its counts")
	_check(not geo.is_attached(), "a mover with no engine cannot stay attached")
	view.free()
	geo.free()


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
