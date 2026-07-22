## Phase-1 test: the coordinate seam, the frame loop, and a live emitter.
##
##   godot --headless --path bindings/godot res://demo/frame.tscn
##
## Exits 0 on pass, 1 on fail. Pass `-- --stay` to let it keep running.
extends Node3D

const Tone := preload("res://demo/tone.gd")

@onready var engine: BwaEngine = $BwaEngine
@onready var emitter: BwaEmitter = $Emitter
@onready var listener: Node3D = $Listener

var _fail := 0
var _frames := 0
var _finished_at := -1  # frame the `finished` signal landed on, -1 = never


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("frame: " + msg)
		_fail += 1


func _ready() -> void:
	# Ordering: BwaEngine must run its push+commit AFTER ordinary gameplay nodes, so the
	# transforms it samples are the ones this frame produced. Godot orders _process by
	# process_priority (lower runs first), which is what BwaEngine's default relies on.
	# Pin the semantics here rather than trusting the comment.
	_check(engine.process_priority > process_priority,
		"BwaEngine should default to a later process_priority than a plain node (%d vs %d)"
			% [engine.process_priority, process_priority])

	_check(engine.is_running(), "engine not running: %s" % engine.get_last_error())
	if not engine.is_running():
		_done()
		return

	_test_seam()
	_test_readbacks()

	emitter.finished.connect(func() -> void: _finished_at = _frames)
	emitter.play_clip(Tone.write_ping("ping", 440.0, 0.30))
	# bwa_source_play only enqueues, so the raw is_playing readback is false for a frame or
	# two. The emitter counts a just-issued play as playing so this reads the obvious way.
	_check(emitter.is_playing(), "emitter should report playing right after play_clip")


func _test_seam() -> void:
	# Identity registration: Godot and room space share handedness and up axis, so a
	# position must pass through completely unchanged. Any flip here is a bug, not a
	# convention — this is the assertion that separates Godot's seam from Unity's.
	engine.registration = Transform3D.IDENTITY
	var p := Vector3(1.5, 0.8, -2.25)
	_check(engine.to_room_position(p).is_equal_approx(p),
		"identity registration must not move a position (got %v)" % engine.to_room_position(p))

	# A default-oriented node faces Godot -Z, which is room -Z. Round-trip the orientation
	# through the room quaternion and back to a direction.
	var facing := engine.to_room_orientation(Basis.IDENTITY) * Vector3(0, 0, 1)
	_check(facing.is_equal_approx(Vector3(0, 0, -1)),
		"identity basis should face room -Z, got %v" % facing)

	# Yawed +90 degrees about +Y, a node faces room -X. If the seam reversed the sense of
	# rotation (as the Unity binding must, because of its mirror) this would read +X and
	# still look perfectly self-consistent.
	var yawed := Basis(Vector3.UP, PI / 2)
	var facing90 := engine.to_room_orientation(yawed) * Vector3(0, 0, 1)
	_check(facing90.is_equal_approx(Vector3(-1, 0, 0)),
		"+90 deg yaw should face room -X, got %v" % facing90)
	_check(is_equal_approx(engine.to_room_yaw(yawed), -PI / 2),
		"to_room_yaw should agree with the facing direction, got %f" % engine.to_room_yaw(yawed))

	# Registration is the only thing that may move a position — so a translation must show
	# up verbatim, with no axis surprises.
	engine.registration = Transform3D(Basis.IDENTITY, Vector3(10, 0, 5))
	_check(engine.to_room_position(Vector3.ZERO).is_equal_approx(Vector3(10, 0, 5)),
		"registration translation should pass straight through")
	# ...but a direction must ignore the translation.
	_check(engine.to_room_direction(Vector3.FORWARD).is_equal_approx(Vector3(0, 0, -1)),
		"to_room_direction must not pick up the registration translation")
	engine.registration = Transform3D.IDENTITY


func _test_readbacks() -> void:
	var n := engine.get_channel_count()
	_check(n >= 4, "implausible channel count %d" % n)
	# Never hard-code 26: both arrays must follow the layout's own count.
	_check(engine.get_bus_levels().size() == n,
		"bus levels sized %d, expected the channel count %d" % [engine.get_bus_levels().size(), n])
	_check(engine.get_speakers().size() == n,
		"speaker array sized %d, expected the channel count %d" % [engine.get_speakers().size(), n])


func _process(delta: float) -> void:
	_frames += 1

	# Orbit the emitter around the listener so the push path is doing real work.
	var t := float(_frames) * delta
	emitter.position = Vector3(cos(t) * 2.0, 1.2, sin(t) * 2.0)
	listener.position = Vector3.ZERO

	if _frames == 10:
		# The regression this guards: a two-state was-playing/is-playing edge detector sees
		# "not playing" in the enqueue window right after play and fires `finished` on frame
		# one, while the sound is about to start. Ten frames is well inside the 0.30 s ping.
		_check(_finished_at == -1,
			"`finished` fired spuriously on frame %d, before the ping could end" % _finished_at)
		_check(engine.get_active_voices() >= 1,
			"expected an active voice while the ping plays, got %d" % engine.get_active_voices())

	# The 0.30 s ping is long done by frame 120 even at a slow headless tick.
	if _frames >= 120:
		_check(not emitter.is_playing(), "one-shot should have ended by now")
		_check(_finished_at != -1, "the `finished` signal never fired")
		_done()


func _done() -> void:
	print("frame: ", "PASS" if _fail == 0 else "FAIL (%d)" % _fail)
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if _fail == 0 else 1)
