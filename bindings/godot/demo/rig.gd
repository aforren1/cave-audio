## The rig scene and room_origin: addons/bw_audio/bwa_rig.tscn instanced, with its RoomOrigin
## pre-placed by this scene at (2, 0, 1) and yawed +90 degrees, and a ground plane enabled so
## that something is baked through the registration at start.
##
##   godot --headless --path bindings/godot res://demo/rig.tscn
##
## Exits 0 on pass, 1 on fail. Pass `-- --stay` to let it keep running.
##
## Every expected value below is a LITERAL worked by hand, never the engine's own inverse
## applied a second time: a check that computes its expectation from the value under test
## cannot catch that value.
##
## The worked case. Origin at O = (2, 0, 1). The probe is P = (5, 0.5, 3), so d = P - O =
## (3, 0.5, 2). The origin's local axes are the room axes, so room = (d . X, d . Y, d . Z)
## with X, Y, Z the origin's axes in the world:
##
##   yaw   0:  X = (1,0,0)   Y = (0,1,0)  Z = (0,0,1)    room = ( 3, 0.5,  2)
##   yaw +90:  X = (0,0,-1)  Y = (0,1,0)  Z = (1,0,0)    room = (-2, 0.5,  3)
##   yaw 180:  X = (-1,0,0)  Y = (0,1,0)  Z = (0,0,-1)   room = (-3, 0.5, -2)
##   pitched:  X = (0,0,-1)  Y = (1,0,0)  Z = (0,-1,0)   room = (-2, 3, -0.5)
##
## The 90 degree row is the one that catches a missing inverse (rotating d by +90 instead
## gives (2, 0.5, -3)). At 180 the rotation is its own inverse, so that row alone cannot.
## The pitched row is there for the FACING: the facing seam is a 180 degree yaw, every yaw
## commutes with it, and only a non-yaw origin can tell "registration, then facing" from the
## reverse.
extends Node3D

const RIG := preload("res://addons/bw_audio/bwa_rig.tscn")
const O := Vector3(2, 0, 1)
const P := Vector3(5, 0.5, 3)

@onready var rig: BwaEngine = $Rig
@onready var origin: Node3D = $Rig/RoomOrigin
@onready var listener: Node3D = $Listener
@onready var view: BwaSpeakerView = $Offset/Speakers

var _fail := 0


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("rig: " + msg)
		_fail += 1


func _near(got: Vector3, want: Vector3, what: String) -> void:
	_check(got.distance_to(want) < 1e-4, "%s: got %v, want %v" % [what, got, want])


func _frames(n: int) -> void:
	for i in n:
		await get_tree().process_frame


func _ready() -> void:
	_test_packed_scene()

	_check(rig.is_running(), "engine not running: %s" % rig.get_last_error())
	if not rig.is_running():
		_done()
		return

	# --- at _ready: no frame has run. The engine must have read the origin in its own
	# _ready, because the ground plane is baked through the registration there.
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "yaw +90 at _ready")
	# A direction takes the rotation and not the translation: world +X is the origin's +Z.
	_near(rig.to_room_direction(Vector3(1, 0, 0)), Vector3(0, 0, 1), "yaw +90 direction")
	_near(rig.to_room_position(O), Vector3.ZERO, "the origin is the room origin")
	_near(rig.from_room_position(Vector3(-2, 0.5, 3)), P, "from_room_position round trip")
	_check(not rig.is_registration_stale(), "nothing changed after start yet")

	# While room_origin resolves it owns the registration: a write is refused, not half-kept.
	rig.registration = Transform3D(Basis.IDENTITY, Vector3(9, 9, 9))
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "set_registration under room_origin")
	_check(not rig.is_registration_stale(), "a refused write changed nothing")

	# --- clearing room_origin keeps the last registration. Nothing writes it here: the
	# ex-origin moves and the value must stay the +90 one.
	rig.room_origin = NodePath("")
	origin.global_transform = Transform3D(Basis(Vector3.UP, PI), O)
	await _frames(2)
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "cleared room_origin keeps the last value")
	_check(not rig.is_registration_stale(), "clearing room_origin changed nothing")

	# --- re-pointing after start. The setter reads the node at once (no frame), and because
	# the ground plane was baked through the old frame this is the change that must flag it.
	rig.room_origin = NodePath("RoomOrigin")
	_near(rig.to_room_position(P), Vector3(-3, 0.5, -2), "yaw 180 on re-set, same frame")
	_check(rig.is_registration_stale(),
		"room_origin re-pointed after start with a baked ground plane must read stale")

	# --- the registration follows a moved origin.
	origin.global_transform = Transform3D(Basis(Vector3.UP, PI / 2), O)
	await _frames(2)
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "yaw +90 after a move")

	# Scale is ignored: the same pose scaled 2x gives the same room point, not half of it.
	origin.global_transform = Transform3D(Basis(Vector3.UP, PI).scaled(Vector3(2, 2, 2)), O)
	await _frames(2)
	_near(rig.to_room_position(P), Vector3(-3, 0.5, -2), "yaw 180, scaled 2x")
	# ...and so is a tiny one. 0.0005 has a determinant of 1.25e-10, which is a small rig in
	# millimeters and not a collapsed axis.
	origin.global_transform = Transform3D(
		Basis(Vector3.UP, PI / 2).scaled(Vector3(0.0005, 0.0005, 0.0005)), O)
	await _frames(2)
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "yaw +90, scaled 0.0005x")

	# A MIRRORED origin is refused: the registration holds. Followed blindly it would put P
	# at (-3, 0.5, 2), the x of the yaw 0 row negated.
	origin.global_transform = Transform3D(
		Basis(Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)), O)
	await _frames(2)
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "a mirrored origin must be refused")

	# --- the speaker view follows, in WORLD space although it sits under a turned, offset
	# parent ($Offset).
	origin.global_transform = Transform3D(Basis.IDENTITY, O)
	await _frames(3)
	_near(rig.to_room_position(P), Vector3(3, 0.5, 2), "yaw 0 after a move")
	var spk := rig.get_speakers()
	_check(view.get_speaker_count() == spk.size() and spk.size() >= 4,
		"speaker view drew %d of %d" % [view.get_speaker_count(), spk.size()])
	if view.get_speaker_count() > 0:
		var gz := view.get_child(0) as Node3D
		_near(gz.global_position, spk[0] + O, "speaker 0 gizmo at yaw 0")
		origin.global_transform = Transform3D(Basis(Vector3.UP, PI), O)
		await _frames(3)
		# Room (x, y, z) sits at world O + (-x, y, -z) under a 180 degree origin.
		_near(gz.global_position, O + Vector3(-spk[0].x, spk[0].y, -spk[0].z),
			"speaker 0 gizmo at yaw 180")

	# --- identity that has to be earned. $Holder is yawed +90 at (1, 2, 3) and its child
	# $Holder/Cancel carries the exact inverse as its LOCAL transform, so the child's GLOBAL
	# transform is identity while neither local one is.
	rig.room_origin = rig.get_path_to($Holder/Cancel)
	_check(rig.registration.is_equal_approx(Transform3D.IDENTITY),
		"an origin whose global transform is identity gives identity, got %s" % rig.registration)
	_near(rig.to_room_position(P), P, "identity through a cancelling parent")

	# --- a pitched origin, and the listener push itself.
	rig.room_origin = NodePath("RoomOrigin")
	origin.global_transform = Transform3D(
		Basis(Vector3(0, 0, -1), Vector3(1, 0, 0), Vector3(0, -1, 0)), O)
	listener.global_transform = Transform3D(Basis.IDENTITY, P)
	rig.listener = rig.get_path_to(listener)
	await _frames(2)
	_near(rig.to_room_position(P), Vector3(-2, 3, -0.5), "pitched origin")
	# A default Godot node faces world -Z, which is this origin's +X.
	_near(rig.to_room_orientation(Basis.IDENTITY) * Vector3(0, 0, 1), Vector3(1, 0, 0),
		"pitched facing")
	# Let the audio thread render a block with that pose, then freeze the pushes and take the
	# registration out of the readback: under identity, get_listener_transform() is the room
	# pose the engine holds, with only the facing convention undone.
	await get_tree().create_timer(0.3).timeout
	rig.feed_listener = false
	rig.room_origin = NodePath("")
	rig.registration = Transform3D.IDENTITY
	var held := rig.get_listener_transform()
	_near(held.origin, Vector3(-2, 3, -0.5), "listener position the engine received")
	# The world axes seen from the pitched room: +X -> room +Y, +Y -> room -Z, +Z -> room -X.
	_near(held.basis.x, Vector3(0, 1, 0), "listener basis x the engine received")
	_near(held.basis.y, Vector3(0, 0, -1), "listener basis y the engine received")
	_near(held.basis.z, Vector3(-1, 0, 0), "listener basis z the engine received")
	rig.listener = NodePath("")
	rig.feed_listener = true

	# --- a second engine is refused: it stays inert, and the first survives its exit.
	origin.global_transform = Transform3D(Basis(Vector3.UP, PI / 2), O)
	rig.room_origin = NodePath("RoomOrigin")
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "yaw +90 before the duplicate")
	var dup := BwaEngine.new()
	dup.sink = BwaEngine.SINK_NULL
	add_child(dup)
	_check(not dup.is_running(), "a second BwaEngine must stay inert")
	dup.queue_free()
	await _frames(3)
	_check(rig.is_running(), "the first BwaEngine must keep running after the duplicate left")
	_near(rig.to_room_position(P), Vector3(-2, 0.5, 3), "registration after a refused duplicate")
	# The singleton is not bound to script, so ask a node that looks it up: a speaker view
	# added now draws only when the survivor is still the registered engine.
	var late := BwaSpeakerView.new()
	add_child(late)
	_check(late.get_speaker_count() == spk.size(),
		"a view added after the duplicate left found no engine (%d of %d speakers)"
			% [late.get_speaker_count(), spk.size()])

	_done()


## The shipped scene as a user gets it, before any override: never added to the tree, so it
## opens no device.
func _test_packed_scene() -> void:
	var fresh := RIG.instantiate()
	_check(fresh is BwaEngine, "bwa_rig.tscn's root must be a BwaEngine")
	if fresh is BwaEngine:
		var e := fresh as BwaEngine
		_check(e.room_origin == NodePath("RoomOrigin"),
			"bwa_rig.tscn must wire room_origin to its RoomOrigin child, got \"%s\"" % e.room_origin)
		_check(e.get_node_or_null(e.room_origin) is Node3D, "room_origin must resolve to a Node3D")
		_check(e.layout_path == "", "the rig ships with no layout path")
		_check(e.profile == BwaEngine.PROFILE_CAVE, "the rig ships on the Cave profile")
	fresh.free()
	# A fixed uid in the scene header is what keeps a user's reference alive across an addon
	# update. Read from the file's own header: ResourceLoader.get_resource_uid answers from
	# the project's uid cache and passed with the header uid deleted.
	var header := FileAccess.get_file_as_string("res://addons/bw_audio/bwa_rig.tscn").get_slice("\n", 0)
	_check(header.contains("uid=\"uid://k1dtwya5ed4\""),
		"bwa_rig.tscn lost its fixed uid, header is: %s" % header)
	# This scene's instance: same wiring, through the instance.
	_check(rig.room_origin == NodePath("RoomOrigin") and rig.get_node_or_null(rig.room_origin) == origin,
		"the instanced rig lost its room_origin wiring")


func _done() -> void:
	print("rig: ", "PASS" if _fail == 0 else "FAIL (%d)" % _fail)
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if _fail == 0 else 1)
