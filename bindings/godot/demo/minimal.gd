## The smallest realistic bw_audio client: orbit a click around the listener's head, then quit.
##
## Every binding has a copy of this demo and they all play the same stimulus (tone.gd's
## click_period), so you can A/B this one against the C reference, examples/minimal.c, by ear.
##
##   godot --path bindings/godot res://demo/minimal.tscn             # listen to it
##   godot --headless --path bindings/godot res://demo/minimal.tscn -- --tests
##
## The shape: a BwaEngine on the binaural profile, one looping BwaEmitter fed the click wav,
## a six-second horizontal lap at ear height two meters out, then quit. BwaEngine does the
## per-frame push and the single commit for you, so this script only moves a transform.
##
## Exits 0 on pass, 1 on fail. Pass `-- --stay` to keep it running.
extends Node3D

const Tone := preload("res://demo/tone.gd")

const RADIUS_M := 2.0
const EAR_HEIGHT_M := 1.5
const LAP_SECONDS := 6.0

@onready var engine: BwaEngine = $BwaEngine
@onready var emitter: BwaEmitter = $Emitter
@onready var listener: Node3D = $Listener

var _fail := 0
var _tests := false
var _t := 0.0
var _saw_voice := false
var _ended_early := false
var _quarters: Array[Vector3] = []      # where the click was at each quarter lap
var _next_quarter := 0


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("minimal: " + msg)
		_fail += 1


## _enter_tree runs top-down, before any child's _ready - the only window in which the engine
## node's create-time config can still be changed. The scene ships on AUTO because the demo is
## meant to be HEARD; ctest asks for the offline sink instead so it needs no device.
func _enter_tree() -> void:
	_tests = "--tests" in OS.get_cmdline_user_args()
	if _tests:
		var e: BwaEngine = $BwaEngine
		e.sink = BwaEngine.SINK_NULL


func _ready() -> void:
	# Child _ready() runs before the parent's, so the engine is already up (or already failed).
	if not engine.is_running():
		push_error("minimal: engine did not start: %s" % engine.get_last_error())
		_fail += 1
		_done()
		return

	print("backend   : ", engine.get_audio_backend())
	print("rate      : ", engine.get_resolved_sample_rate())
	print("block     : ", engine.get_resolved_block_size())

	# The stimulus. Check it here rather than trusting the generator: these numbers are the
	# contract every other binding's copy is held to.
	var period := Tone.click_period(48000)
	_check(period.size() == 12000, "the click period should be 250 ms, got %d" % period.size())
	var peak := 0.0
	var burst_crossings := 0
	for i in period.size():
		peak = maxf(peak, absf(period[i]))
		if i > 0 and i < 96 and signf(period[i]) != signf(period[i - 1]):
			burst_crossings += 1
	_check(absf(peak - Tone.CLICK_PEAK) < 1e-6, "the burst should peak at -12 dBFS, got %f" % peak)
	var tail_peak := 0.0
	for i in range(96, period.size()):
		tail_peak = maxf(tail_peak, absf(period[i]))
	_check(tail_peak == 0.0, "everything past the 2 ms burst should be silence, got %f" % tail_peak)
	# Broadband, not a tone: a 2 ms noise burst crosses zero dozens of times where an 880 Hz
	# sine crosses it three or four. No FFT needed to tell those apart.
	_check(burst_crossings > 20,
		"the burst should be broadband noise, only %d zero crossings" % burst_crossings)

	emitter.finished.connect(func() -> void: _ended_early = true)
	emitter.loop = true
	emitter.play_clip(Tone.write_click("bwa_minimal_click"))
	_check(emitter.is_playing(), "the emitter should report playing right after play_clip")


func _process(delta: float) -> void:
	if not engine.is_running():
		return

	# Orbit: horizontal, 2 m out, one lap in six seconds. Starts in front (+z) and passes the
	# LEFT ear first, because +x is left for an identity listener. Godot shares room space's
	# handedness and up axis, so no mirror: what is set here is what the engine gets.
	var a := TAU * _t / LAP_SECONDS
	emitter.position = Vector3(RADIUS_M * sin(a), EAR_HEIGHT_M, RADIUS_M * cos(a))
	listener.position = Vector3(0.0, EAR_HEIGHT_M, 0.0)

	if engine.get_active_voices() >= 1:
		_saw_voice = true

	# Record where the click was at each quarter lap. The demo's whole claim is that it MOVES,
	# so the four samples have to land in four different quadrants; a frozen orbit would put
	# them all in one, which is the failure a "did it play" check cannot see.
	if _next_quarter < 4 and _t >= _next_quarter * LAP_SECONDS / 4.0:
		_quarters.append(emitter.position)
		_next_quarter += 1

	_t += delta
	if _t >= LAP_SECONDS:
		_done()


func _done() -> void:
	if engine.is_running():
		# A looping voice never ends by itself, so `finished` firing means a spurious end.
		_check(not _ended_early, "`finished` fired on a looping voice")
		_check(_saw_voice, "no voice was ever active while the click played")
		_check(_quarters.size() == 4, "expected four quarter-lap samples, got %d" % _quarters.size())
		if _quarters.size() == 4:
			# Front, left, back, right. Demanded with a margin (half the radius) rather than by
			# sign alone: a frozen orbit parks every sample near one axis, where a bare sign
			# test on a near-zero coordinate is a coin flip.
			var m := RADIUS_M * 0.5
			_check(_quarters[0].z > m, "the lap should start in front, got %v" % _quarters[0])
			_check(_quarters[1].x > m, "a quarter lap on it should be to the left, got %v" % _quarters[1])
			_check(_quarters[2].z < -m, "half a lap on it should be behind, got %v" % _quarters[2])
			_check(_quarters[3].x < -m, "three quarters on it should be to the right, got %v" % _quarters[3])
			for q in _quarters:
				_check(is_equal_approx(q.y, EAR_HEIGHT_M), "the lap should stay at ear height, got %v" % q)
		emitter.stop()

	print("minimal: ", "PASS" if _fail == 0 else "FAIL")
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if _fail == 0 else 1)
	set_process(false)
