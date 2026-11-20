## What follows the scene after start: static geometry and the bed's yaw.
##
##   godot --headless --path bindings/godot res://demo/follow.tscn
##
## Exits 0 on pass, 1 on fail. Everything is created in code, properties first and add_child
## second, which is the runtime-creation pattern the docs give.
##
## Geometry is measured where it matters, on the occlusion a source behind the wall gets, so
## that half needs the Steam Audio build (it says SKIPPED without it). The rebuild counter and
## the bed half do not.
##
## The bed is measured on the bus, not on the angle the binding computed: a plane wave from
## the field's front lights the speakers it points at. The default grid has 9 speakers at room
## z = +1.5, 9 at z = -1.5, and the same on x. Room +x is LEFT.
extends Node3D

const Tone := preload("res://demo/tone.gd")

var engine: BwaEngine
var origin: Node3D
var _fail := 0


func _check(cond: bool, msg: String) -> void:
	if not cond:
		push_error("follow: " + msg)
		_fail += 1


func _frames(n: int) -> void:
	for i in n:
		await get_tree().process_frame


func _wall_time(seconds: float) -> void:
	await get_tree().create_timer(seconds).timeout


## Polls until `cond` holds or `seconds` of wall time pass.
func _until(cond: Callable, seconds: float) -> bool:
	var end := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end:
		if cond.call():
			return true
		await get_tree().process_frame
	return cond.call()


func _ready() -> void:
	origin = Node3D.new()
	origin.name = "Origin"
	add_child(origin)
	var head := Node3D.new()
	head.name = "Head"
	head.position = Vector3(0, 1.5, 0)
	add_child(head)

	engine = BwaEngine.new()
	engine.profile = BwaEngine.PROFILE_CAVE
	engine.sink = BwaEngine.SINK_NULL
	engine.listener = NodePath("../Head")
	engine.room_origin = NodePath("../Origin")
	add_child(engine)
	if not engine.is_running():
		push_error("follow: engine not running: %s" % engine.get_last_error())
		_done()
		return

	await _test_geometry()
	await _test_bed_yaw()
	_done()


func _wall(label: String, pos: Vector3, size: Vector3) -> BwaAcousticGeometry:
	var box := BoxMesh.new()
	box.size = size
	var g := BwaAcousticGeometry.new()
	g.name = label
	g.mesh = box
	g.position = pos
	return g


func _has_steam_audio() -> bool:
	var probe := BwaDynamicGeometry.new()
	probe.mesh = BoxMesh.new()
	add_child(probe)
	var attached := probe.is_attached()
	remove_child(probe)
	probe.free()
	return attached


func _test_geometry() -> void:
	var steam := _has_steam_audio()
	# An absolute path is a contract, not an accident of globalize_path.
	var clip := ProjectSettings.globalize_path(Tone.write_ping("follow_tone", 440.0, 2.0))
	var em := BwaEmitter.new()
	em.clip = clip
	em.loop = true
	em.occlusion = true
	em.position = Vector3(0, 1.5, -3)   # 3 m in front of the head
	add_child(em)
	await _frames(5)
	_check(em.get_playhead_frames() > 0, "a clip at an absolute path does not play: %s" % clip)
	_check(em.get_occlusion_factor() > 0.9, "occluded with no geometry")
	_check(engine.get_static_rebuilds() == 0, "an empty scene pushed a static mesh")

	# Two walls in one frame: one rebuild. The wall sits under a plain Node3D, so a hidden
	# parent can reach it.
	var r := engine.get_static_rebuilds()
	var group := Node3D.new()
	group.name = "Group"
	var wall := _wall("Wall", Vector3(0, 1.5, -1.5), Vector3(4, 4, 0.2))
	group.add_child(wall)
	var spare := _wall("Spare", Vector3(5, 0.5, 5), Vector3(1, 1, 1))
	add_child(group)
	add_child(spare)
	await _frames(2)
	_check(engine.get_static_rebuilds() == r + 1,
		"two adds in one frame: want 1 rebuild, got %d" % (engine.get_static_rebuilds() - r))
	# A hide and its undo in one frame leave the set as baked: no rebuild.
	r = engine.get_static_rebuilds()
	wall.visible = false
	wall.visible = true
	await _frames(2)
	_check(engine.get_static_rebuilds() == r, "a hide and show in one frame rebuilt")
	# Idle frames with geometry up: no rebuild. With none up a rebuild has nothing to push, so
	# this must run here to be able to fail.
	await _frames(5)
	_check(engine.get_static_rebuilds() == r, "rebuilt with nothing changed")
	if not steam:
		print("follow: occlusion checks SKIPPED (no Steam Audio build)")
	else:
		_check(await _until(func(): return em.get_occlusion_factor() < 0.5, 3.0),
			"a wall added after start does not occlude: %f" % em.get_occlusion_factor())

	# A hide, the wall's own and then its parent's: one rebuild each, and no occlusion.
	for hidden: Node3D in [wall, group]:
		r = engine.get_static_rebuilds()
		hidden.visible = false
		await _frames(2)
		_check(engine.get_static_rebuilds() == r + 1,
			"hiding %s: want 1 rebuild, got %d" % [hidden.name, engine.get_static_rebuilds() - r])
		if steam:
			_check(await _until(func(): return em.get_occlusion_factor() > 0.9, 3.0),
				"a wall with %s hidden still occludes: %f" % [hidden.name, em.get_occlusion_factor()])
		hidden.visible = true
		await _frames(2)
		if steam:
			_check(await _until(func(): return em.get_occlusion_factor() < 0.5, 3.0),
				"a wall with %s shown does not occlude: %f" % [hidden.name, em.get_occlusion_factor()])

	# Turn the room under the scene. Head and source follow per frame; the wall must be
	# re-baked into the new frame or it ends up behind the head. The source starts behind the
	# head (a clear path), so "occluded" is a change the re-bake has to cause.
	await _turn_with_source_ahead(em, steam, "the wall")

	# The last ones out, freed in one frame: one rebuild, and it must clear the mesh.
	r = engine.get_static_rebuilds()
	group.queue_free()
	spare.queue_free()
	await _frames(3)
	_check(engine.get_static_rebuilds() == r + 1,
		"two frees in one frame: want 1 rebuild, got %d" % (engine.get_static_rebuilds() - r))
	if steam:
		_check(await _until(func(): return em.get_occlusion_factor() > 0.9, 3.0),
			"with no geometry left the old mesh still occludes: %f" % em.get_occlusion_factor())

		# A STILL dynamic door must follow a moved origin too.
		var box := BoxMesh.new()
		box.size = Vector3(4, 4, 0.2)
		var door := BwaDynamicGeometry.new()
		door.mesh = box
		door.position = Vector3(0, 1.5, -1.5)
		add_child(door)
		_check(await _until(func(): return em.get_occlusion_factor() < 0.5, 3.0),
			"a dynamic door does not occlude: %f" % em.get_occlusion_factor())
		await _turn_with_source_ahead(em, steam, "the dynamic door")
		door.queue_free()
	em.queue_free()
	await _frames(2)


## Moves the source behind the head (clear), then in ONE frame turns the origin 180 degrees and
## puts the source back in front. Only an occluder that followed the turn blocks it.
func _turn_with_source_ahead(em: BwaEmitter, steam: bool, what: String) -> void:
	em.position = Vector3(0, 1.5, 3)
	if steam:
		_check(await _until(func(): return em.get_occlusion_factor() > 0.9, 3.0),
			"a source behind the head reads occluded: %f" % em.get_occlusion_factor())
	var r := engine.get_static_rebuilds()
	origin.rotation = Vector3(0, PI, 0)
	em.position = Vector3(0, 1.5, -3)
	await _frames(2)
	if what == "the wall":
		_check(engine.get_static_rebuilds() == r + 1,
			"a moved origin: want 1 rebuild, got %d" % (engine.get_static_rebuilds() - r))
	if steam:
		_check(await _until(func(): return em.get_occlusion_factor() < 0.5, 3.0),
			"%s did not follow a moved origin: %f" % [what, em.get_occlusion_factor()])
	origin.rotation = Vector3.ZERO
	await _frames(2)


## Summed bus levels over 20 frames for the speakers above +0.5 and below -0.5 on room axis
## `axis` (0 = x, where +x is room LEFT; 2 = z, where +z is room front).
func _sides(axis: int) -> Vector2:
	var spk := engine.get_speakers()
	var hi := 0.0
	var lo := 0.0
	for k in 20:
		await get_tree().process_frame
		var lv := engine.get_bus_levels()
		for i in lv.size():
			var c: float = spk[i][axis]
			if c > 0.5:
				hi += lv[i]
			elif c < -0.5:
				lo += lv[i]
	_check(hi + lo > 0.0, "the bed is silent, so this measures nothing")
	return Vector2(hi, lo)


func _test_bed_yaw() -> void:
	var bed := BwaBed.new()
	bed.clip = Tone.write_ambix("follow_front", Vector3(0, 0, 1), 300.0, 2.0)   # room front
	bed.loop = true
	add_child(bed)

	# Identity origin: scene and room agree, yaw 0 faces room +z.
	await _wall_time(0.5)
	var s := await _sides(2)
	_check(s.x > 1.5 * s.y, "identity origin, yaw 0: front %f, back %f" % [s.x, s.y])

	# Yaw +90 turns world +Z toward world +X, which is room +x (left) here. Pins the sign.
	bed.orientation = Vector3(PI / 2, 0, 0)
	await _wall_time(1.0)
	s = await _sides(0)
	_check(s.x > 1.5 * s.y, "identity origin, yaw +90: left %f, right %f" % [s.x, s.y])
	bed.orientation = Vector3.ZERO

	# Origin turned 180: world +Z is room -z. Yaw is scene terms, so the field faces room back.
	origin.rotation = Vector3(0, PI, 0)
	await _wall_time(1.5)
	s = await _sides(2)
	_check(s.y > 1.5 * s.x, "origin 180, yaw 0: front %f, back %f" % [s.x, s.y])

	# 0 and 180 survive a sign flip; a quarter turn does not. Origin +90: its +Z is world +X
	# and its +X is world -Z, so world +Z is room -x, the room's RIGHT.
	origin.rotation = Vector3(0, PI / 2, 0)
	await _wall_time(1.5)
	s = await _sides(0)
	_check(s.y > 1.5 * s.x, "origin +90, yaw 0: left %f, right %f" % [s.x, s.y])

	# set_yaw_from_basis is scene terms too. A node turned +90 faces world -X, which this
	# origin calls room -z: the back. Adding the registration twice would face room left.
	bed.set_yaw_from_basis(Basis(Vector3.UP, PI / 2))
	await _wall_time(1.5)
	s = await _sides(2)
	_check(s.y > 1.5 * s.x, "origin +90, set_yaw_from_basis(+90): front %f, back %f" % [s.x, s.y])
	bed.queue_free()


func _done() -> void:
	print("follow: ", "PASS" if _fail == 0 else "FAIL (%d)" % _fail)
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if _fail == 0 else 1)
