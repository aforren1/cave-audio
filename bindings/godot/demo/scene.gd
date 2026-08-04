## Phase-3 test: scene-authored acoustics — materials, geometry, and the speaker view.
##
##   godot --headless --path bindings/godot res://demo/scene.tscn
##
## The load-bearing claim here is the geometry MERGE. bwa_scene_set_mesh_mat replaces the
## whole static mesh, and bwa_scene_set_box is a convenience that calls it, so a room box
## and a separate occluder would clobber each other with no error anywhere — last writer
## wins and the loser is simply inaudible. BwaEngine collects instead, committing one mesh
## before bwa_start. These checks are what stop that collection from silently regressing
## into per-node calls.
extends Node3D

const Tone := preload("res://demo/tone.gd")

@onready var engine: BwaEngine = $BwaEngine
@onready var room: BwaRoomBox = get_node_or_null("Room")  # absent under --no-room
@onready var pillar: BwaAcousticGeometry = $Pillar
@onready var mover: BwaDynamicGeometry = $Mover
@onready var speakers: BwaSpeakerView = $Speakers
@onready var emitter: BwaEmitter = $Emitter

var _fail := 0
var _with_room := true


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("scene: " + msg)
		_fail += 1


## `--no-room` is the negative control for _test_room_reached_core. Drop the box before it
## can register (children's _enter_tree has not run yet at this point, so it never reaches
## BwaEngine's collection) and the same assertion must then FAIL to find a room.
func _enter_tree() -> void:
	# _test_audible's observable is the BUS meters, so this fixture must run a profile whose
	# point sources actually land on the bus. The node default (BINAURAL) no longer does:
	# its direct render bypasses the speaker bus by design — correct for headphones, silent
	# meters here. CAVE_SIM keeps the whole render on the bus. (Set before the child's _ready
	# creates the engine; profile is create-time.)
	$BwaEngine.profile = BwaEngine.PROFILE_CAVE_SIM

	_with_room = not "--no-room" in OS.get_cmdline_user_args()
	if not _with_room:
		var r := $Room
		remove_child(r)
		r.queue_free()


func _ready() -> void:
	process_priority = 2000
	if not engine.is_running():
		push_error("scene: engine not running: %s" % engine.get_last_error())
		_finish()
		return

	_test_room_reached_core()
	if _with_room:
		_test_materials()
		_test_geometry_merge()
		_test_speaker_view()
		await _test_audible()
	_finish()


## Did bwa_scene_set_box actually land?
##
## There is no readback for the static mesh, so most of what a geometry binding does is
## unobservable from outside. This is the one crack of light: enabling image-source early
## reflections without a captured room makes the core record "no room - call
## bwa_scene_set_box first". Its ABSENCE is therefore real evidence the box arrived, and
## the --no-room run proves the check has teeth rather than passing vacuously.
func _test_room_reached_core() -> void:
	emitter.early_reflections = true
	var err := engine.get_last_error()
	var complained := "no room" in err
	if _with_room:
		_check(not complained,
			"the room box never reached the core: %s" % err)
	else:
		_check(complained,
			"without a room box the core should have refused early reflections, but said: '%s'"
				% err)


func _test_materials() -> void:
	var brick := BwaMaterial.new()
	brick.preset = BwaMaterial.PRESET_BRICK
	var t1 := brick.get_token(engine)
	_check(t1 != 0, "a preset material should mint a real token")
	# Cached, not re-minted: the core's material table is fixed-capacity, so a resource used
	# on fifty surfaces must not burn fifty slots.
	_check(brick.get_token(engine) == t1, "a material should cache its token")

	var generic := BwaMaterial.new()
	generic.preset = BwaMaterial.PRESET_GENERIC
	_check(generic.get_token(engine) == 0, "GENERIC is the canonical token 0")

	var custom := BwaMaterial.new()
	custom.preset = BwaMaterial.PRESET_CUSTOM
	custom.absorption = Vector3(0.4, 0.5, 0.6)
	custom.transmission = Vector3(0.02, 0.01, 0.005)
	custom.scattering = 0.3
	var tc := custom.get_token(engine)
	_check(tc != 0 and tc != t1, "a custom material should mint its own token")
	# Editing the coefficients must invalidate the cache, or the resource keeps handing back
	# a token describing the OLD material.
	custom.absorption = Vector3(0.1, 0.1, 0.1)
	_check(custom.get_token(engine) != tc, "editing a custom material should re-mint its token")


func _test_geometry_merge() -> void:
	# The box's own walls: 6 faces, 2 triangles each, 3 vertices each.
	var walls := room.wall_faces()
	_check(walls.size() == 36, "a shoebox should be 12 triangles, got %d verts" % walls.size())

	# Floor-based and centered, matching the core's own box (x/z centered on the node, y from
	# 0 up). A binding that built a center-origin box would put the floor at -h/2 and every
	# reflection would arrive from the wrong place.
	var lo := walls[0]
	var hi := walls[0]
	for v in walls:
		lo = Vector3(minf(lo.x, v.x), minf(lo.y, v.y), minf(lo.z, v.z))
		hi = Vector3(maxf(hi.x, v.x), maxf(hi.y, v.y), maxf(hi.z, v.z))
	var size: Vector3 = room.size
	_check(is_equal_approx(lo.y, 0.0), "the box floor should sit at y = 0, got %f" % lo.y)
	_check(is_equal_approx(hi.y, size.y), "the box ceiling should be at y = height")
	_check(is_equal_approx(lo.x, -size.x / 2) and is_equal_approx(hi.x, size.x / 2),
		"the box should be centered in x")
	_check(is_equal_approx(lo.z, -size.z / 2) and is_equal_approx(hi.z, size.z / 2),
		"the box should be centered in z")

	# Inward-facing normals: the listener is INSIDE, so every triangle's normal must point
	# toward the room center. Get this backwards and the ray tracer sees an open box.
	var center := Vector3(0, size.y / 2, 0)
	var outward := 0
	for t in range(0, walls.size(), 3):
		var n := (walls[t + 1] - walls[t]).cross(walls[t + 2] - walls[t])
		if n.dot(center - walls[t]) < 0.0:
			outward += 1
	_check(outward == 0, "%d of 12 box triangles face outward" % outward)

	# The pillar is separate static geometry. If it produced no faces the merge had nothing
	# to protect and the rest of this test proves nothing.
	_check(pillar.world_faces().size() > 0, "the pillar should contribute triangles")

	# The mover attaches only with the Steam Audio build, so its handle is allowed to be
	# absent — but the node must survive either way rather than erroring out.
	if not mover.is_attached():
		print("scene: dynamic geometry inert (no Steam Audio build) - expected")


func _test_speaker_view() -> void:
	var n := speakers.get_speaker_count()
	# Sized from the layout, never from BWA_CHANNELS.
	_check(n == engine.get_channel_count(),
		"speaker view drew %d gizmos for %d channels" % [n, engine.get_channel_count()])
	_check(speakers.get_child_count() == n, "one gizmo node per speaker")

	# The nearest-speaker query is the channel-walk check's whole question. Ask it about a
	# speaker's own position and it must answer with that speaker.
	if n > 0:
		var probe: Vector3 = engine.from_room_position(engine.get_speakers()[n / 2])
		_check(speakers.nearest_speaker(probe) == n / 2,
			"nearest_speaker missed its own speaker: got %d, wanted %d"
				% [speakers.nearest_speaker(probe), n / 2])


func _test_audible() -> void:
	# Early reflections are already enabled by _test_room_reached_core. This is the
	# phonon-free path, so it must work in a build with no Steam Audio at all.
	emitter.play_clip(Tone.write_ping("scene_ping", 440.0, 0.5))
	await get_tree().create_timer(0.25).timeout
	_check(emitter.get_playhead_frames() > 0, "the emitter should be playing through the room")

	var levels := engine.get_bus_levels()
	_check(levels.size() == engine.get_channel_count(), "bus levels should follow the layout")
	var loudest := 0.0
	for v in levels:
		loudest = maxf(loudest, v)
	_check(loudest > 0.0, "something should have reached the bus")


func _finish() -> void:
	print("scene: ", "PASS" if _fail == 0 else "FAIL (%d)" % _fail)
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if _fail == 0 else 1)
