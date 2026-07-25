## bwa_playground, Godot edition — the by-ear harness, ported from examples/playground.cpp.
##
## Same eight scenes, same keys, same synthesized signals. What differs is the frame: the C++
## one is raylib + Dear ImGui, this one is Godot nodes driving the SAME public ABI through the
## GDExtension binding. Running both against one engine build is a way to catch a binding
## that quietly changes what you hear.
##
## Scenes (TAB to cycle):
##   1 Localization   - pure listener-relative DBAP; SPACE auto-orbits, X flies past
##   2 Occlusion      - a real occluder, static-mesh vs dynamic-instance A/B
##   3 Directivity    - weighted-dipole radiation, aimed with , / .
##   4 Channel walk   - one raw output channel at a time (speaker check)
##   5 Blind A/B/X    - double-blind over live knobs, with a binomial p-value
##   6 Ambisonic bed  - a world-locked 3rd-order field; spin, tilt, renderer, max-rE
##   7 Reverb bed     - a shoebox + reverb; REBUILDS the engine on entry/exit
##   8 Underwater     - the api.md "listener submerges" recipe: dive and the FDN retunes
##                      LIVE, the speed of sound glides, crossing sources muffle, and the
##                      pressure-release surface throws the Lloyd's-mirror bounce. Rebuilds
##                      like scene 7 (its FDN is load-time), phonon-free.
##
## Global keys: WASD/RF move the source, Q/E turn the head, 1-4 signal, TAB scene, ESC quit.
## Without an ASIO device the engine falls back to the null sink and everything still runs,
## just silent — visual-only mode is a supported state, not a failure.
extends Node3D

const Signals := preload("res://addons/bw_audio/playground/signals.gd")
const Scenes := preload("res://addons/bw_audio/playground/scenes.gd")

const SRC_GAIN := 0.8
const ROOM := Vector3(8.0, 4.0, 8.0)   # the reverb scene's shoebox, matching the C++ ROOM_*
const WATER_Y := 2.4                   # the underwater scene's surface height (the C++ WATER_Y)

# --- live rig (rebuilt across the reverb boundary) ---
var engine: BwaEngine
var source: BwaEmitter
var refl: BwaEmitter
var bed: BwaBed
var speaker_view: BwaSpeakerView
var rig: Node3D

# --- shared state the scenes read and write ---
var head := Vector3.ZERO          # the array centroid: the engine's own reference point
var head_yaw := 0.0
var source_pos := Vector3(0, 1.2, 2.0)
var source_yaw := 0.0
var speakers: PackedVector3Array = PackedVector3Array()
var nspk := 0
var highlight_spk := -1
var cur_sig := 1                  # pink bursts: broadband with sharp onsets, the best localiser
var sig_paths: Array[String] = []
var backend := "none"

var draw: Node3D
var panel: Control
var scenes: Array = []
var cur_scene := 0
# Which config the live rig was built in: 0 interactive, 1 reverb (Steam bed + shoebox),
# 2 underwater (FDN + pressure-release surface plane). Mirrors the C++ engine_mode.
var rig_mode := 0

# Reverb-scene settings that are LOAD-time, so changing one rebuilds the rig.
var rev_decoder := 0
var want_sink := BwaEngine.SINK_AUTO
# The panel's "render" picker (create-time, so switching rebuilds):
# 0 = CAVE_SIM, the array audition — the speaker meters show what lights the speakers;
# 1 = BINAURAL, the direct per-source render — point sources and beds bypass the speaker
# bus, so quiet meters there are CORRECT; 2 = CAVE, the array ITSELF over 26-ch ASIO (the
# by-ear harness on the rig machine; with no such device the null sink runs visual-only).
# Mirrors the C++ playground's picker.
var render_pick := 0

var _cam_yaw := deg_to_rad(195.0)
var _cam_pitch := deg_to_rad(25.0)
var _cam_dist := 8.4
var _camera: Camera3D
var _headless := false


func _ready() -> void:
	_headless = "--selftest" in OS.get_cmdline_user_args()
	if _headless:
		want_sink = BwaEngine.SINK_NULL   # hermetic: never touch a device under test

	var gen := Signals.new()
	sig_paths = gen.generate_all()

	draw = preload("res://addons/bw_audio/playground/draw3d.gd").new()
	add_child(draw)

	_camera = Camera3D.new()
	_camera.current = true
	add_child(_camera)
	_update_camera()

	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.06, 0.07, 0.09)
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	e.ambient_light_color = Color(0.5, 0.5, 0.55)
	env.environment = e
	add_child(env)

	scenes = Scenes.build(self)
	_build_rig(0)
	switch_scene(0)

	panel = preload("res://addons/bw_audio/playground/panel.gd").new()
	add_child(panel)

	if _headless:
		call_deferred("_run_selftest")   # let the panel's own _ready build its labels first


## Tear the rig down and stand a new one up. The reverb bed, the room geometry, and the FDN
## are load-time, so crossing a config boundary is a create/start cycle — the same brief
## audio gap the C++ playground takes, for the same reason.
## mode: 0 interactive, 1 reverb (Steam bed + shoebox), 2 underwater (FDN + surface plane).
func _build_rig(mode: int) -> void:
	if rig:
		remove_child(rig)
		rig.free()          # free(), not queue_free(): the new engine must not find the old
		rig = null           # singleton still installed, or bwa_create races its own teardown

	rig = Node3D.new()
	rig.name = "Rig"

	engine = BwaEngine.new()
	engine.name = "Engine"
	engine.profile = BwaEngine.PROFILE_CAVE if render_pick == 2 \
		else BwaEngine.PROFILE_BINAURAL if render_pick == 1 \
		else BwaEngine.PROFILE_CAVE_SIM   # default: the array audition (the meters read the bus)
	engine.sink = want_sink
	engine.sample_rate = Signals.SR
	engine.block_size = 256
	engine.feed_listener = true
	engine.bed_decoder = BwaEngine.DECODE_EPAD if rev_decoder == 1 else BwaEngine.DECODE_ALLRAD
	if mode == 1:
		engine.enable_reflections = true
		engine.reflections_ir_seconds = 1.0
		engine.reflections_order = 1
		engine.reflections_rays = 4096
		engine.reflections_bounces = 16
	elif mode == 2:
		# The underwater rig, phonon-free: the FDN renders whichever medium's tail (the scene
		# retunes it LIVE on a dive — fdn_set_decay is the demo), and the water surface is a
		# pressure-release mirror plane, so the submerged bounce inverts (the Lloyd's mirror).
		engine.enable_fdn = true
		var surface := BwaMaterial.new()
		surface.preset = BwaMaterial.PRESET_CUSTOM
		surface.absorption = Vector3(0.01, 0.01, 0.02)    # the surface reflects nearly everything
		surface.transmission = Vector3(0.30, 0.06, 0.01)  # ...and transmits almost nothing
		surface.scattering = 0.05
		engine.ground_enabled = true
		engine.ground_height = WATER_Y
		engine.ground_material = surface
		engine.ground_pressure_release = true
	rig.add_child(engine)

	# The listener node the engine follows. Its transform is pushed every frame by BwaEngine,
	# so moving THIS is how the head moves.
	var listener := Node3D.new()
	listener.name = "Listener"
	rig.add_child(listener)
	engine.listener = NodePath("../Listener")

	if mode == 1:
		var box := BwaRoomBox.new()
		box.name = "Room"
		box.size = ROOM
		var plaster := BwaMaterial.new()
		plaster.preset = BwaMaterial.PRESET_PLASTER
		box.all_materials = plaster
		rig.add_child(box)

	source = BwaEmitter.new()
	source.name = "Source"
	source.autoplay = false
	source.gain = SRC_GAIN
	rig.add_child(source)

	refl = BwaEmitter.new()
	refl.name = "Refl"
	refl.autoplay = false
	refl.gain = 0.0
	rig.add_child(refl)

	bed = BwaBed.new()
	bed.name = "Bed"
	bed.autoplay = false
	rig.add_child(bed)

	speaker_view = BwaSpeakerView.new()
	speaker_view.name = "Speakers"
	rig.add_child(speaker_view)

	# Everything above is configured BEFORE the rig enters the tree, because BwaEngine creates
	# and starts in _ready and the room box has to have registered by then.
	add_child(rig)

	rig_mode = mode
	backend = engine.get_audio_backend()
	speakers = engine.get_speakers()
	nspk = speakers.size()

	# Ear point = the array centroid, which is the engine's own nominal listening point, not
	# the room origin. A survey that puts the origin elsewhere must not move the head.
	head = Vector3.ZERO
	for s in speakers:
		head += s
	if nspk > 0:
		head /= nspk
	listener.position = head

	source.play_clip(sig_paths[cur_sig])
	refl.play_clip(sig_paths[cur_sig])
	refl.gain = 0.0
	bed.play_clip(Signals.BED_FILE)
	bed.stop()

	print("playground: %s, %d speakers" % [backend, nspk])


func rebuild_rig(mode: int) -> void:
	_build_rig(mode)
	scenes[cur_scene].enter()


## Leave the current scene at a clean baseline, then enter the new one.
##
## The reset list is the load-bearing part: every scene's knobs are ENGINE-wide, so a scene
## that leaves SPCAP selected or a spread mode engaged silently changes what the next scene
## demonstrates. Ported verbatim from switch_scene() for exactly that reason.
func switch_scene(idx: int) -> void:
	var want_mode := 1 if idx == 6 else (2 if idx == 7 else 0)
	if want_mode != rig_mode:
		_build_rig(want_mode)

	for ch in nspk:
		engine.set_test_signal(ch, BwaEngine.TEST_OFF, 0.0)
	refl.gain = 0.0
	source.occlusion = false
	source.set_directivity_preset(BwaSource.DIR_OMNI)
	# Room identity, not Godot identity: set_orientation is the facing seam, so a Godot
	# identity would land the source facing room -Z. Inaudible while OMNI, but a reset should
	# reset - the half-turn is what maps back to the room's own identity.
	source.set_orientation(Quaternion(Vector3.UP, PI))
	source.doppler = false
	source.air_absorption = false
	source.spread = 0.0
	engine.dual_band = false
	engine.panner = BwaEngine.PAN_DBAP        # the A/B/X scene may have left SPCAP or VBAP on
	engine.spread_mode = BwaEngine.SPREAD_LOBE
	engine.decorrelation = false
	bed.stop()
	engine.bed_renderer = BwaEngine.BED_MATRIX
	engine.max_re = false
	engine.max_re_split = false
	source_yaw = 0.0
	source.gain = SRC_GAIN
	highlight_spk = -1

	cur_scene = idx
	scenes[idx].enter()


func set_signal(which: int) -> void:
	cur_sig = clampi(which, 0, sig_paths.size() - 1)
	source.play_clip(sig_paths[cur_sig])
	refl.play_clip(sig_paths[cur_sig])


## The panel's "render" picker: 0 cave_sim / 1 binaural / 2 cave. Create-time (like the
## reverb decoder), so an actual change rebuilds the rig and re-enters the current scene.
func set_render_pick(i: int) -> void:
	i = clampi(i, 0, 2)
	if i == render_pick:
		return
	render_pick = i
	rebuild_rig(rig_mode)


func head_quat() -> Quaternion:
	return Quaternion(Vector3.UP, head_yaw)


func _process(dt: float) -> void:
	if not engine or not engine.is_running():
		return
	_global_keys(dt)

	scenes[cur_scene].update(dt)

	# The scenes write source_pos/source_yaw; one place pushes them, so a scene that forgets
	# cannot leave the audible source somewhere the drawn one is not.
	source.position = source_pos
	var listener := rig.get_node("Listener") as Node3D
	listener.position = head
	listener.quaternion = head_quat()

	draw.begin()
	_draw_common()
	scenes[cur_scene].draw(draw)
	draw.end()
	_update_camera()


## WASD/RF move the source along the ROOM basis, taken from the ABI rather than written out
## as signs. Room right is -X, which is the opposite of the reflex, and a hardcoded guess
## produces a scene that looks entirely sane with left and right swapped — audible, but only
## if you already know which way it should be.
func _global_keys(dt: float) -> void:
	if _headless:
		return
	var mv := 2.5 * dt
	var right := BwaEngine.room_right()
	var ahead := BwaEngine.room_ahead()
	var up := BwaEngine.room_up()
	if Input.is_key_pressed(KEY_D): source_pos += right * mv
	if Input.is_key_pressed(KEY_A): source_pos -= right * mv
	if Input.is_key_pressed(KEY_W): source_pos += ahead * mv
	if Input.is_key_pressed(KEY_S): source_pos -= ahead * mv
	if Input.is_key_pressed(KEY_R): source_pos += up * mv
	if Input.is_key_pressed(KEY_F): source_pos -= up * mv
	if Input.is_key_pressed(KEY_Q): head_yaw += 1.8 * dt
	if Input.is_key_pressed(KEY_E): head_yaw -= 1.8 * dt


func _unhandled_key_input(event: InputEvent) -> void:
	if _headless or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	var k := event as InputEventKey
	match k.keycode:
		KEY_TAB:
			switch_scene((cur_scene + 1) % scenes.size())
		KEY_ESCAPE:
			get_tree().quit(0)
		KEY_1, KEY_2, KEY_3, KEY_4:
			set_signal(k.keycode - KEY_1)
		_:
			scenes[cur_scene].key(k.keycode)


func _draw_common() -> void:
	draw.floor_grid(head, 12.0, 1.0)

	# The head: ear and nose axes taken from the ABI's own room-frame basis rather than
	# re-deriving the convention here, so a change to it cannot silently disagree.
	var q := head_quat()
	var right: Vector3 = q * BwaEngine.room_right()
	var fwd: Vector3 = q * BwaEngine.room_ahead()
	draw.sphere(head, 0.16, Color(0.4, 0.7, 0.95))
	draw.sphere(head + right * 0.17, 0.055, Color(0.9, 0.2, 0.2))    # right ear -> audio R
	draw.sphere(head - right * 0.17, 0.055, Color(0.95, 0.95, 0.95)) # left ear  -> audio L
	draw.line(head + fwd * 0.13, head + fwd * 0.32, Color(1.0, 0.6, 0.2))


func _update_camera() -> void:
	# Not while the cursor is over the panel, or dragging a slider would also swing the view.
	if not _headless and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) \
			and not (panel and panel.is_mouse_over()):
		var rel := Input.get_last_mouse_velocity() * 0.0006
		_cam_yaw -= rel.x
		_cam_pitch = clampf(_cam_pitch + rel.y, -1.4, 1.4)
	var o := Vector3(
		cos(_cam_pitch) * sin(_cam_yaw), sin(_cam_pitch), cos(_cam_pitch) * cos(_cam_yaw))
	_camera.position = head + o * _cam_dist
	_camera.look_at(head, Vector3.UP)


func _input(event: InputEvent) -> void:
	if panel and panel.is_mouse_over():
		return
	if event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_cam_dist = maxf(2.0, _cam_dist - 0.6)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_cam_dist = minf(30.0, _cam_dist + 0.6)


## Headless self-test: walk every scene, including the reverb boundary that rebuilds the rig,
## and assert the engine survives each transition. The point is not that it sounds right —
## nothing can hear it here — but that the scene machinery and the rebuild do not fall over,
## which is exactly what broke most often while porting.
func _run_selftest() -> void:
	var fail := 0
	# Engine generations must be unique per instance PROCESS-WIDE: BwaMaterial keys its token
	# cache on them, so two engines sharing a generation hands a stale token from a dead
	# engine to a live one - the core clamps it to the default material, silently. The
	# regression: a per-instance counter read 1 on every rebuilt rig.
	var gen_a: int = engine.get_generation()
	var mat := BwaMaterial.new()
	mat.preset = BwaMaterial.PRESET_BRICK
	var tok_a: int = mat.get_token(engine)
	rebuild_rig(rig_mode)
	if engine.get_generation() == gen_a:
		push_error("playground: rebuilt engine reused generation %d - material caches go stale" % gen_a)
		fail += 1
	# ...and the material must observe the change: a re-mint on the new engine, not the cache.
	if mat.get_token(engine) == 0 and tok_a != 0:
		push_error("playground: material failed to re-mint on the rebuilt engine")
		fail += 1

	for i in scenes.size():
		switch_scene(i)
		for f in 3:
			scenes[i].update(1.0 / 60.0)
			draw.begin()
			scenes[i].draw(draw)
			draw.end()
		if not engine.is_running():
			push_error("playground: engine died entering scene %d (%s)" % [i, scenes[i].title])
			fail += 1
			continue
		if engine.get_channel_count() < 4:
			push_error("playground: scene %d left a bad channel count" % i)
			fail += 1
		# Render the panel for this scene. status() rows are format strings evaluated only
		# when the HUD draws them, so without this a bad one crashes the first time someone
		# opens the scene rather than here.
		if scenes[i].status().is_empty():
			push_error("playground: scene %d reports no status" % i)
			fail += 1

		# Round-trip every declared control: read it, then write back what we read. The
		# widgets only call these when clicked, so without this a setter naming the wrong
		# property sits there looking fine until someone touches that one control. Writing
		# back the SAME value keeps it a no-op — notably for the reverb decoder, whose setter
		# rebuilds the engine when the value actually changes.
		for spec in scenes[i].controls():
			match spec.get("kind", ""):
				"option", "toggle", "slider":
					var v = spec["get"].call()
					spec["set"].call(v)
					if spec["get"].call() != v:
						push_error("playground: scene %d control '%s' did not round-trip"
							% [i, spec["label"]])
						fail += 1
				"button":
					pass   # pressing these mid-test would answer A/B/X trials or reset tallies
		panel._process(1.0 / 60.0)
	# Back across the config boundary the other way, which is the rebuild most likely to leak.
	switch_scene(0)
	if not engine.is_running():
		push_error("playground: engine died returning from the rebuild scenes")
		fail += 1

	# The underwater scene's cross-surface machinery, asserted where it is observable without
	# audio: manual occlusion publishes synchronously, so diving under a still-above-surface
	# source must read back heavily occluded, and surfacing must clear it. (The scene walk
	# above already proved the mode-2 rig builds and survives; this pins the recipe's logic.)
	switch_scene(7)
	var wat = scenes[7]
	wat.update(1.0 / 60.0)                 # source starts above the surface, listener in air
	if source.get_occlusion_factor() < 0.9:
		push_error("playground: underwater scene occluded a clear same-side path")
		fail += 1
	wat.set_submerged(true)
	wat.update(1.0 / 60.0)                 # the path now crosses the surface
	if source.get_occlusion_factor() > 0.1:
		push_error("playground: diving did not muffle the cross-surface source (factor %.2f)"
			% source.get_occlusion_factor())
		fail += 1
	wat.set_submerged(false)
	wat.update(1.0 / 60.0)
	if source.get_occlusion_factor() < 0.9:
		push_error("playground: surfacing did not clear the crossing occlusion")
		fail += 1
	switch_scene(0)
	if not engine.is_running():
		push_error("playground: engine died returning from the underwater scene")
		fail += 1

	# The render picker crosses a create/start boundary per pick: binaural (direct), cave (the
	# bare array device — no decode suffix), and back to cave_sim. The backend string is the
	# observable; the engine must survive every switch.
	set_render_pick(1)
	if not engine.is_running():
		push_error("playground: engine died switching to the binaural (direct) render")
		fail += 1
	if not backend.contains("direct"):
		push_error("playground: binaural render did not report a direct decode (%s)" % backend)
		fail += 1
	set_render_pick(2)
	if not engine.is_running():
		push_error("playground: engine died switching to the cave (array) render")
		fail += 1
	if backend.contains("("):
		push_error("playground: the cave render must report the bare device, no decode (%s)" % backend)
		fail += 1
	set_render_pick(0)
	if not engine.is_running():
		push_error("playground: engine died switching back to the cave_sim render")
		fail += 1
	if not backend.contains("sim"):
		push_error("playground: cave_sim render did not report a sim decode (%s)" % backend)
		fail += 1

	print("playground: ", "PASS" if fail == 0 else "FAIL (%d)" % fail)
	get_tree().quit(0 if fail == 0 else 1)
