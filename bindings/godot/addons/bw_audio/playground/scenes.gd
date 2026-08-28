## The eight playground scenes, ported from examples/playground.cpp.
##
## Each scene owns one feature and answers one by-ear question. They share the app's single
## source and reflection voice rather than minting their own, exactly as the C++ version
## does — which is why playground.gd's switch_scene() resets every engine-wide knob between
## them: the knobs are global, so a scene that leaves one engaged changes what the NEXT scene
## appears to demonstrate.
extends RefCounted

const GREEN := Color(0.35, 0.86, 0.35)
const RED := Color(0.9, 0.25, 0.25)
const AMBER := Color(1.0, 0.7, 0.3)
const BLUE := Color(0.5, 0.7, 0.9)


static func build(app) -> Array:
	return [
		Localization.new(app), Occlusion.new(app), Directivity.new(app),
		ChannelWalk.new(app), Abx.new(app), AmbisonicBed.new(app), ReverbBed.new(app),
		Underwater.new(app),
	]


class Base extends RefCounted:
	var app
	var title := "?"
	var help := ""

	func _init(a) -> void:
		app = a

	func enter() -> void: pass
	func update(_dt: float) -> void: pass
	func draw(_d) -> void: pass
	func key(_code: int) -> void: pass
	## Lines the panel shows for this scene: [label, value] pairs.
	func status() -> Array: return []

	## Widgets the panel should build for this scene. Each entry is a Dictionary:
	##   {kind = "option", label, items: [String], get: -> int,   set: (int)}
	##   {kind = "toggle", label,                  get: -> bool,  set: (bool)}
	##   {kind = "slider", label, min, max, step,  get: -> float, set: (float)}
	##   {kind = "button", label, text, press: ()}
	## Declaring them rather than building them keeps every control wired to the SAME setter
	## the keyboard shortcut uses, so the two paths cannot drift apart.
	func controls() -> Array: return []


## ============================ 1. Localization (pure DBAP) ============================
## Move a source around a 26-speaker array and hear where it lands. SPACE auto-sweeps on
## three incommensurate periods so the source covers the whole space rather than repeating a
## short loop; X is a fast straight flyby, which with Doppler on is the race-car pitch sweep.
class Localization extends Base:
	const TRAIL := 96

	var auto := false
	var flyby := false
	var doppler := false
	var air := false
	var dual := false
	var spread := 0.0
	## The point-source panner, and under SPCAP its two live tuning exponents. 0 on either
	## exponent means "the default": focus derived from the array geometry, density 2.0.
	var panner := BwaEngine.PAN_DBAP
	var focus := 0.0
	var density := 0.0
	var _focus_def := 0.0   ## cached at enter(): the derivation is O(N^2), the layout is not live
	var _t := 0.0
	var _fly_t := 0.0
	var _trail: PackedVector3Array = PackedVector3Array()

	func _init(a) -> void:
		super(a)
		title = "Localization (DBAP)"
		help = "SPACE orbit  X flyby  V doppler  B air  C size  M dual-band"

	func enter() -> void:
		app.source.gain = app.SRC_GAIN
		_trail.clear()
		auto = false
		flyby = false
		# switch_scene() cleared these; the scene owns them, so re-apply what the user chose.
		app.source.doppler = doppler
		app.source.air_absorption = air
		app.source.spread = spread
		app.engine.dual_band = dual
		app.engine.panner = panner
		app.engine.spcap_focus = focus
		app.engine.spcap_density = density
		_focus_def = app.engine.get_spcap_focus_default()

	func key(code: int) -> void:
		match code:
			KEY_SPACE:
				auto = not auto
				if auto: flyby = false; _t = 0.0; _trail.clear()
			KEY_X:
				flyby = not flyby
				if flyby: auto = false; _fly_t = 0.0; _trail.clear()
			KEY_V:
				doppler = not doppler
				app.source.doppler = doppler
			KEY_B:
				air = not air
				app.source.air_absorption = air
			KEY_C:
				# point -> .4 -> .7 -> wide -> point
				spread = 0.4 if spread < 0.05 else (0.7 if spread < 0.5 else (1.0 if spread < 0.85 else 0.0))
				app.source.spread = spread
			KEY_M:
				dual = not dual
				app.engine.dual_band = dual

	func update(dt: float) -> void:
		if flyby:
			# A fast straight pass 0.8 m in front, there and back at about 7.8 m/s.
			_fly_t += dt
			var period := 3.6
			var u: float = fmod(_fly_t, period) / period
			var x := (-7.0 + 28.0 * u) if u < 0.5 else (7.0 - 28.0 * (u - 0.5))
			app.source_pos = Vector3(x, app.head.y, 0.8)
		elif auto:
			_t += dt
			var az := 0.62 * _t                      # circle the listener, ~10 s an orbit
			var r := 2.0 + 1.2 * sin(0.90 * _t)      # near <-> far, ~7 s
			var y := 1.2 * sin(1.14 * _t)            # low <-> high, ~5.5 s
			app.source_pos = Vector3(r * cos(az), app.head.y + y, r * sin(az))
		if auto or flyby:
			_trail.append(app.source_pos)
			if _trail.size() > TRAIL:
				_trail.remove_at(0)
		app.source.gain = app.SRC_GAIN

	func draw(d) -> void:
		if auto and _trail.size() > 1:
			for i in range(1, _trail.size()):
				var a := float(i) / _trail.size()
				d.line(_trail[i - 1], _trail[i], Color(0.35, 0.86, 0.35, 0.15 + 0.7 * a))
			d.line(app.source_pos, Vector3(app.source_pos.x, 0, app.source_pos.z),
				Color(0.35, 0.86, 0.35, 0.25))
		d.line(app.head, app.source_pos, GREEN)
		d.sphere(app.source_pos, 0.18, RED)

	func status() -> Array:
		return [
			["motion", "orbit" if auto else ("flyby" if flyby else "manual")],
			["size (spread)", "%.2f" % spread],
			["SPCAP focus", ("%.1f" % focus) if focus > 0.0 else "default %.1f" % _focus_def],
		]

	func controls() -> Array:
		return [
			{"kind": "option", "label": "motion", "items": ["manual", "orbit (SPACE)", "flyby (X)"],
				"get": func() -> int: return 1 if auto else (2 if flyby else 0),
				"set": func(i: int) -> void:
					auto = i == 1
					flyby = i == 2
					_t = 0.0
					_fly_t = 0.0
					_trail.clear()},
			{"kind": "toggle", "label": "doppler (V)",
				"get": func() -> bool: return doppler,
				"set": func(v: bool) -> void:
					doppler = v
					app.source.doppler = v},
			{"kind": "toggle", "label": "air absorption (B)",
				"get": func() -> bool: return air,
				"set": func(v: bool) -> void:
					air = v
					app.source.air_absorption = v},
			{"kind": "toggle", "label": "dual-band (M)",
				"get": func() -> bool: return dual,
				"set": func(v: bool) -> void:
					dual = v
					app.engine.dual_band = v},
			{"kind": "slider", "label": "source size (C)", "min": 0.0, "max": 1.0, "step": 0.05,
				"get": func() -> float: return spread,
				"set": func(v: float) -> void:
					spread = v
					app.source.spread = v},
			{"kind": "option", "label": "panner", "items": ["DBAP (moving)", "SPCAP (fixed)",
					"VBAP (fixed)"],
				"get": func() -> int: return panner,
				"set": func(i: int) -> void:
					panner = i
					app.engine.panner = i},
			# SPCAP only. 0 = revert to the default (derived focus, density 2.0); the status line
			# above shows what this array derives.
			{"kind": "slider", "label": "SPCAP focus (0 = derived)", "min": 0.0, "max": 48.0,
				"step": 0.5,
				"get": func() -> float: return focus,
				"set": func(v: float) -> void:
					focus = v
					app.engine.spcap_focus = v},
			{"kind": "slider", "label": "SPCAP density (0 = 2.0)", "min": 0.0, "max": 8.0,
				"step": 0.1,
				"get": func() -> float: return density,
				"set": func(v: float) -> void:
					density = v
					app.engine.spcap_density = v},
		]


## ============================ 2. Occlusion & Materials ============================
## Push the source behind the wall and the sim attenuates and spectrally tilts it by the
## wall's MATERIAL. Y A/Bs the same occluder as a STATIC mesh (full scene rebuild) against a
## DYNAMIC instance (a transform update) — they should sound identical, which is the point:
## the dynamic path is the cheap one, so it had better not cost anything audible.
class Occlusion extends Base:
	const MATS := ["concrete", "glass", "carpet", "wood", "metal"]
	const PRESETS := [
		BwaMaterial.PRESET_CONCRETE, BwaMaterial.PRESET_GLASS, BwaMaterial.PRESET_CARPET,
		BwaMaterial.PRESET_WOOD, BwaMaterial.PRESET_METAL,
	]
	const REFL_GAIN := 0.5

	var wall_c := Vector3(0, 1.4, -1.4)
	var wall_n := Vector3(0, 0, 1)
	var wall_hw := 1.6
	var wall_hh := 1.4
	var cur_mat := 0
	var dynamic := true
	var sweep := false
	var occ_audible := true
	var refl_audible := true
	var _anim_t := 0.0
	var _handle := -1
	var _mats: Array[BwaMaterial] = []
	var _image := Vector3.ZERO
	var _refl_pt := Vector3.ZERO
	var _refl_valid := false
	var _occluded := false
	var _factor := 1.0

	func _init(a) -> void:
		super(a)
		title = "Occlusion & Materials"
		help = "[ ] slide wall  SPACE sweep  M material  Y static/dynamic  G occlusion  T reflection"
		for p in PRESETS:
			var m := BwaMaterial.new()
			m.preset = p
			_mats.append(m)

	func _wall_basis() -> Array:
		var up := Vector3.RIGHT if absf(wall_n.y) > 0.9 else Vector3.UP
		var u := up.cross(wall_n).normalized()
		return [u, wall_n.cross(u)]

	## The wall as world triangles. Local corners for the dynamic path are the same offsets
	## from center, so a pure translation reproduces the static mesh's exact world triangles —
	## which is what makes the A/B a comparison of PATHS rather than of two different walls.
	func _corners(centered: bool) -> PackedVector3Array:
		var b := _wall_basis()
		var u: Vector3 = b[0] * wall_hw
		var v: Vector3 = b[1] * wall_hh
		var c: Vector3 = Vector3.ZERO if centered else wall_c
		return PackedVector3Array([c - u - v, c + u - v, c + u + v, c - u + v])

	func _apply_wall() -> void:
		var tok := _mats[cur_mat].get_token(app.engine)
		if _handle >= 0:
			app.engine.scene_remove_dynamic_mesh(_handle)
			_handle = -1
		var q := _corners(dynamic)
		var verts := PackedVector3Array([q[0], q[1], q[2], q[0], q[2], q[3]])
		var tris := PackedInt32Array([0, 1, 2, 3, 4, 5])
		if dynamic:
			# Park the static slot far away rather than leaving the previous static wall in
			# place: the core will not take an empty mesh, and two walls is not the test.
			var far := PackedVector3Array([
				Vector3(100, 100, 100), Vector3(100.02, 100, 100), Vector3(100, 100.02, 100)])
			app.engine.scene_set_mesh(far, PackedInt32Array([0, 1, 2]), PackedInt32Array([tok]))
			_handle = app.engine.scene_add_dynamic_mesh(verts, tris, tok)
			_move_wall()
		else:
			app.engine.scene_set_mesh(verts, tris, PackedInt32Array([tok, tok]))

	func _move_wall() -> void:
		if _handle >= 0:
			app.engine.scene_set_dynamic_transform(_handle, wall_c, Quaternion.IDENTITY)

	func enter() -> void:
		app.source.gain = app.SRC_GAIN
		app.source.occlusion = occ_audible
		_handle = -1        # any handle from a prior rig is stale
		_apply_wall()

	func key(code: int) -> void:
		match code:
			KEY_SPACE: sweep = not sweep
			KEY_Y:
				dynamic = not dynamic
				_apply_wall()
			KEY_M:
				cur_mat = (cur_mat + 1) % MATS.size()
				_apply_wall()
			KEY_T: refl_audible = not refl_audible
			KEY_G:
				occ_audible = not occ_audible
				app.source.occlusion = occ_audible

	func update(dt: float) -> void:
		var moved := false
		var mv := 2.5 * dt
		if Input.is_key_pressed(KEY_BRACKETLEFT):
			wall_c -= wall_n * mv
			moved = true
		if Input.is_key_pressed(KEY_BRACKETRIGHT):
			wall_c += wall_n * mv
			moved = true
		if sweep:
			_anim_t += dt
			wall_c.z = -2.2 + 1.6 * sin(_anim_t * 1.1)
			moved = true
		# The whole point of the A/B: sliding is an instance transform in dynamic mode and a
		# full scene rebuild in static mode, and they must sound identical.
		if moved:
			if dynamic: _move_wall()
			else: _apply_wall()

		# The mirrored source is a valid specular reflection when both source and listener sit
		# on the same side and the bounce actually lands on the panel.
		var d: float = (app.source_pos - wall_c).dot(wall_n)
		_image = app.source_pos - wall_n * (2.0 * d)
		var same_side: bool = (d > 0) == ((app.head - wall_c).dot(wall_n) > 0)
		_refl_valid = false
		if same_side:
			var ab: Vector3 = _image - app.head
			var denom := ab.dot(wall_n)
			if absf(denom) > 1e-6:
				var t: float = (wall_c - app.head).dot(wall_n) / denom
				var hit: Vector3 = app.head + ab * t
				var b := _wall_basis()
				if t > 0.0 and t < 1.0 and absf((hit - wall_c).dot(b[0])) <= wall_hw \
						and absf((hit - wall_c).dot(b[1])) <= wall_hh:
					_refl_valid = true
					_refl_pt = hit

		app.refl.position = _image
		app.refl.gain = app.SRC_GAIN * REFL_GAIN if (refl_audible and _refl_valid) else 0.0
		app.source.gain = app.SRC_GAIN
		_factor = app.source.get_occlusion_factor()
		_occluded = _factor < 0.85

	func draw(d) -> void:
		var b := _wall_basis()
		d.quad(wall_c, b[0] * wall_hw, b[1] * wall_hh, Color(0.6, 0.7, 0.86, 0.9))
		d.line(app.head, app.source_pos, RED if _occluded else GREEN)
		d.sphere(app.source_pos, 0.18, RED)
		if _refl_valid:
			d.line(app.source_pos, _refl_pt, AMBER)
			d.line(_refl_pt, app.head, AMBER)
			d.sphere(_refl_pt, 0.06, AMBER)
			d.sphere(_image, 0.14, Color(0.9, 0.63, 0.24, 0.5))

	func status() -> Array:
		return [
			["attached", "yes" if _handle >= 0 else "no (needs the SDK)"],
			["occlusion", "%.2f" % _factor],
			["reflection", "audible" if (refl_audible and _refl_valid) else "-"],
			["wall z", "%.2f" % wall_c.z],
		]

	func controls() -> Array:
		return [
			{"kind": "option", "label": "material (M)", "items": MATS,
				"get": func() -> int: return cur_mat,
				"set": func(i: int) -> void:
					cur_mat = i
					_apply_wall()},
			{"kind": "option", "label": "geometry (Y)",
				"items": ["static mesh (rebuild)", "dynamic instance"],
				"get": func() -> int: return 1 if dynamic else 0,
				"set": func(i: int) -> void:
					dynamic = i == 1
					_apply_wall()},
			{"kind": "toggle", "label": "occlusion (G)",
				"get": func() -> bool: return occ_audible,
				"set": func(v: bool) -> void:
					occ_audible = v
					app.source.occlusion = v},
			{"kind": "toggle", "label": "reflection (T)",
				"get": func() -> bool: return refl_audible,
				"set": func(v: bool) -> void: refl_audible = v},
			{"kind": "toggle", "label": "auto sweep (SPACE)",
				"get": func() -> bool: return sweep,
				"set": func(v: bool) -> void: sweep = v},
			{"kind": "slider", "label": "wall distance", "min": -3.5, "max": 1.5, "step": 0.05,
				"get": func() -> float: return wall_c.z,
				"set": func(v: float) -> void:
					wall_c.z = v
					if dynamic: _move_wall()
					else: _apply_wall()},
		]


## ============================ 3. Directivity ============================
## A weighted-dipole radiation pattern. Aim it with , and . and the listener hears it fall
## away off-axis; the drawn lobe is the same weighting the engine applies.
class Directivity extends Base:
	const NAMES := ["omni", "cardioid", "figure-8"]

	var cur := 1        # cardioid by default, so the effect is audible on entry
	var _gain := 1.0

	func _init(a) -> void:
		super(a)
		title = "Directivity"
		help = "Z pattern  , / . aim"

	func enter() -> void:
		app.source.gain = app.SRC_GAIN
		app.source.set_directivity_preset(cur as BwaSource.Directivity)

	func key(code: int) -> void:
		if code == KEY_Z:
			cur = (cur + 1) % 3
			app.source.set_directivity_preset(cur as BwaSource.Directivity)

	func update(dt: float) -> void:
		var rt := 1.8 * dt
		if Input.is_key_pressed(KEY_COMMA): app.source_yaw += rt
		if Input.is_key_pressed(KEY_PERIOD): app.source_yaw -= rt
		# set_orientation takes a NODE-space orientation (the binding's facing seam converts
		# it): aim the node's own forward, -Z, along the same room direction the lobe below
		# is drawn on. looking_at is that statement made directly - no hand-written
		# half-turn to drift out of sync with the draw, in the one tool whose job is
		# ear-vs-eye agreement.
		var aim := Vector3(sin(app.source_yaw), 0.0, cos(app.source_yaw))
		app.source.set_orientation(Quaternion(Basis.looking_at(aim)))
		app.source.gain = app.SRC_GAIN
		_gain = app.source.get_directivity_gain()

	func draw(d) -> void:
		d.line(app.head, app.source_pos, Color(0.35, 0.86, 0.35, 0.7))
		d.sphere(app.source_pos, 0.18, RED)
		# The horizontal lobe: |(1-w) + w*cos(a)|, the weighted dipole the engine renders.
		var w := 0.5 if cur == 1 else (1.0 if cur == 2 else 0.0)
		var pts := PackedVector3Array()
		for i in 49:
			var a := float(i) / 48.0 * TAU
			var g: float = absf((1.0 - w) + w * cos(a))
			var r := 0.15 + 0.7 * g
			var wa: float = app.source_yaw + a
			pts.append(app.source_pos + Vector3(r * sin(wa), 0, r * cos(wa)))
		d.polyline(pts, Color(1.0, 0.7, 0.3, 0.8))

	func status() -> Array:
		return [["aim", "%.0f deg" % rad_to_deg(app.source_yaw)], ["gain", "%.2f" % _gain]]

	func controls() -> Array:
		return [
			{"kind": "option", "label": "pattern (Z)", "items": NAMES,
				"get": func() -> int: return cur,
				"set": func(i: int) -> void:
					cur = i
					app.source.set_directivity_preset(i as BwaSource.Directivity)},
			{"kind": "slider", "label": "aim (, / .)", "min": -PI, "max": PI, "step": 0.02,
				"get": func() -> float: return wrapf(app.source_yaw, -PI, PI),
				"set": func(v: float) -> void: app.source_yaw = v},
		]


## ============================ 4. Channel walk ============================
## One raw output channel at a time — a speaker check, not a spatial path. In the binaural
## profile each channel is HRTF'd as its virtual speaker, so the tone should walk around your
## head; if two adjacent channels sound like the same place, the wiring is worth a look.
class ChannelWalk extends Base:
	var active := 0
	var kind := BwaEngine.TEST_SINE
	var auto := false
	var _timer := 0.0
	var _applied := -1
	var _applied_kind := BwaEngine.TEST_SINE

	func _init(a) -> void:
		super(a)
		title = "Channel walk (speaker check)"
		help = "LEFT/RIGHT channel  SPACE auto-walk  N sine/noise"

	func enter() -> void:
		app.source.gain = 0.0      # silence the spatial voices; only the test tone sounds
		app.refl.gain = 0.0
		_timer = 0.0
		auto = false               # start manual on every visit
		_applied = -1
		if active >= app.nspk:
			active = 0

	func key(code: int) -> void:
		match code:
			KEY_RIGHT: active = (active + 1) % app.nspk
			KEY_LEFT: active = (active + app.nspk - 1) % app.nspk
			KEY_N: kind = BwaEngine.TEST_NOISE if kind == BwaEngine.TEST_SINE else BwaEngine.TEST_SINE
			KEY_SPACE: auto = not auto

	func update(dt: float) -> void:
		if auto:
			_timer += dt
			if _timer >= 0.7:
				_timer = 0.0
				active = (active + 1) % app.nspk
		# Keys and the panel both write `active`; applying here means both paths share one
		# off/on switch and cannot leave two channels driven.
		if active != _applied or kind != _applied_kind:
			if _applied >= 0:
				app.engine.set_test_signal(_applied, BwaEngine.TEST_OFF, 0.0)
			app.engine.set_test_signal(active, kind, 0.3)
			_applied = active
			_applied_kind = kind
		app.highlight_spk = active

	func draw(d) -> void:
		if active < app.speakers.size():
			d.line(app.head, app.speakers[active], Color(0.47, 0.92, 0.59, 0.8))
			d.sphere(app.speakers[active], 0.2, Color(0.47, 0.92, 0.59))

	func status() -> Array:
		return [["channel", "%d of %d" % [active, app.nspk]]]

	func controls() -> Array:
		# The channel list is built from the LAYOUT's speaker count, never a hard-coded 26 —
		# a 24-speaker rig must show 24 entries.
		var names: Array[String] = []
		for i in app.nspk:
			names.append("channel %d" % i)
		return [
			{"kind": "option", "label": "channel", "items": names,
				"get": func() -> int: return active,
				"set": func(i: int) -> void: active = i},
			{"kind": "option", "label": "signal (N)", "items": ["sine", "noise"],
				"get": func() -> int: return 0 if kind == BwaEngine.TEST_SINE else 1,
				"set": func(i: int) -> void:
					kind = BwaEngine.TEST_SINE if i == 0 else BwaEngine.TEST_NOISE},
			{"kind": "toggle", "label": "auto-walk (SPACE)",
				"get": func() -> bool: return auto,
				"set": func(v: bool) -> void: auto = v},
		]


## ============================ 5. Blind A/B/X ============================
## A and B are two settings of ONE live knob; X is secretly one of them. Listen freely, answer
## LEFT or RIGHT, and the one-sided binomial tail over your trials says whether you can
## actually hear the difference or are guessing. It turns "sounds different to me" into a
## measurement, which is the only honest way to settle these.
class Abx extends Base:
	var cmp_idx := 0
	var listen := 2        # 0 = A, 1 = B, 2 = X
	var x := 0             # X's hidden identity
	var trials := 0
	var correct := 0
	var last_x := 0
	var flash := 0.0
	var flash_ok := false
	var orbit := true
	var _orbit_t := 0.0
	var _rng := RandomNumberGenerator.new()

	# name, A label, B label, apply(app, v)
	var comparisons: Array = []

	func _init(a) -> void:
		super(a)
		title = "Blind A/B/X"
		help = "Z=A X=B C=X   LEFT/RIGHT answer   G knob   V reset   SPACE orbit"
		_rng.randomize()
		comparisons = [
			["dual-band panning", "single-band (power)", "dual-band (LF amplitude)",
				func(p, v): p.engine.dual_band = v],
			["panner: DBAP vs SPCAP", "DBAP", "SPCAP",
				func(p, v): p.engine.panner = BwaEngine.PAN_SPCAP if v else BwaEngine.PAN_DBAP],
			["panner: DBAP vs VBAP", "DBAP", "VBAP",
				func(p, v): p.engine.panner = BwaEngine.PAN_VBAP if v else BwaEngine.PAN_DBAP],
			["source spread", "point source", "spread 60%",
				func(p, v): p.source.spread = 0.6 if v else 0.0],
			["spread render: MDAP", "LOBE (reshape)", "MDAP (virtual ring)",
				func(p, v): p.source.spread = 0.6; p.engine.spread_mode = \
					BwaEngine.SPREAD_MDAP if v else BwaEngine.SPREAD_LOBE],
			["spread render: SPECTRAL", "LOBE (reshape)", "SPECTRAL (freq-dep pan)",
				func(p, v): p.source.spread = 0.6; p.engine.spread_mode = \
					BwaEngine.SPREAD_SPECTRAL if v else BwaEngine.SPREAD_LOBE],
			["decorrelation (wide src)", "coherent copies", "velvet-noise decorrelated",
				func(p, v): p.source.spread = 0.6; p.engine.decorrelation = v],
			["air absorption", "off", "on (distance LPF)",
				func(p, v): p.source.air_absorption = v],
		]

	## One-sided binomial tail: P(correct >= k | n trials, p = 1/2). Computed in log-gamma so
	## the factorials do not overflow at large n.
	static func pvalue(n: int, k: int) -> float:
		var p := 0.0
		for i in range(k, n + 1):
			p += exp(_lgamma(n + 1.0) - _lgamma(i + 1.0) - _lgamma(n - i + 1.0) - n * log(2.0))
		return minf(p, 1.0)

	## Lanczos log-gamma; GDScript has no lgamma and the p-value needs one.
	static func _lgamma(xx: float) -> float:
		const G := [
			76.18009172947146, -86.50532032941677, 24.01409824083091,
			-1.231739572450155, 0.1208650973866179e-2, -0.5395239384953e-5]
		var x := xx
		var y := xx
		var tmp := x + 5.5
		tmp -= (x + 0.5) * log(tmp)
		var ser := 1.000000000190015
		for j in 6:
			y += 1.0
			ser += G[j] / y
		return -tmp + log(2.5066282746310005 * ser / x)

	## Every knob back to baseline, THEN the tested one to whatever we are listening to.
	## Without the reset, switching comparisons leaves the previous knob stuck on its B
	## setting and quietly contaminates the next test.
	func _apply_listen() -> void:
		app.engine.dual_band = false
		app.engine.panner = BwaEngine.PAN_DBAP
		app.source.spread = 0.0
		app.engine.spread_mode = BwaEngine.SPREAD_LOBE
		app.engine.decorrelation = false
		app.source.air_absorption = false
		var v: int = x if listen == 2 else listen
		comparisons[cmp_idx][3].call(app, v == 1)

	func new_trial() -> void:
		x = _rng.randi_range(0, 1)
		listen = 2
		_apply_listen()

	func reset() -> void:
		trials = 0
		correct = 0
		flash = 0.0
		new_trial()

	func set_listen(which: int) -> void:
		listen = which
		_apply_listen()

	func answer(guess: int) -> void:
		last_x = x
		flash_ok = guess == x
		if flash_ok:
			correct += 1
		trials += 1
		flash = 1.6
		new_trial()      # reveal, and immediately deal the next X

	func enter() -> void:
		app.source.gain = app.SRC_GAIN
		orbit = true       # motion is what exposes panner differences
		_orbit_t = 0.0
		new_trial()        # keep the tally across visits; only X is redrawn

	func key(code: int) -> void:
		match code:
			KEY_Z: set_listen(0)
			KEY_X: set_listen(1)
			KEY_C: set_listen(2)
			KEY_LEFT: answer(0)
			KEY_RIGHT: answer(1)
			KEY_G:
				cmp_idx = (cmp_idx + 1) % comparisons.size()
				reset()        # a new knob gets a fresh tally
			KEY_V: reset()
			KEY_SPACE: orbit = not orbit

	func update(dt: float) -> void:
		if orbit:
			# Identical motion for A, B and X, so it can never cue the answer.
			_orbit_t += dt
			var az := 0.45 * _orbit_t
			app.source_pos = Vector3(
				2.2 * cos(az), app.head.y + 0.5 * sin(0.31 * _orbit_t), 2.2 * sin(az))
		flash = maxf(0.0, flash - dt)
		app.source.gain = app.SRC_GAIN

	func draw(d) -> void:
		d.line(app.head, app.source_pos, GREEN)
		d.sphere(app.source_pos, 0.18, RED)

	func status() -> Array:
		var c: Array = comparisons[cmp_idx]
		var p := pvalue(trials, correct) if trials > 0 else 1.0
		var rows := [
			["knob", c[0]],
			["A", c[1]],
			["B", c[2]],
			["listening", ["A", "B", "X (hidden)"][listen]],
			["score", "%d / %d" % [correct, trials]],
			["p-value", "%.4f%s" % [p, "  AUDIBLE" if (trials > 0 and p < 0.05) else ""]],
		]
		if flash > 0.0:
			rows.append(["last", "%s - X was %s" % [
				"correct" if flash_ok else "wrong", c[1 + last_x]]])
		return rows

	func controls() -> Array:
		var names: Array[String] = []
		for c in comparisons:
			names.append(c[0])
		var cmp_now: Array = comparisons[cmp_idx]
		return [
			{"kind": "option", "label": "knob (G)", "items": names,
				"get": func() -> int: return cmp_idx,
				"set": func(i: int) -> void:
					cmp_idx = i
					reset()},          # a new knob gets a fresh tally
			{"kind": "option", "label": "listening",
				"items": ["A: " + cmp_now[1], "B: " + cmp_now[2], "X (hidden)"],
				"get": func() -> int: return listen,
				"set": func(i: int) -> void: set_listen(i)},
			{"kind": "button", "label": "answer", "text": "X is A  (LEFT)",
				"press": func() -> void: answer(0)},
			{"kind": "button", "label": "", "text": "X is B  (RIGHT)",
				"press": func() -> void: answer(1)},
			{"kind": "toggle", "label": "orbit (SPACE)",
				"get": func() -> bool: return orbit,
				"set": func(v: bool) -> void: orbit = v},
			{"kind": "button", "label": "", "text": "reset tally (V)",
				"press": func() -> void: reset()},
		]


## ============================ 6. Ambisonic bed ============================
## A synthesized 3rd-order AmbiX field played WORLD-LOCKED through the bed decode: bursts from
## the front, clicks from the left and up, a diffuse floor. Spin it and the content should
## stay put in the ROOM while your head turns — which is the whole claim of a world-locked
## bed, and something a head-locked one cannot fake.
class AmbisonicBed extends Base:
	var spin := false
	var parametric := false
	var max_re := true   # mirrors the engine default (ON)
	var re_split := false
	var yaw := 0.0
	var pitch := 0.0

	func _init(a) -> void:
		super(a)
		title = "Ambisonic bed (world-locked)"
		help = "SPACE spin  G matrix/parametric  B max-rE  N band-split  R/F tilt"

	func _apply() -> void:
		app.engine.bed_renderer = \
			BwaEngine.BED_PARAMETRIC if parametric else BwaEngine.BED_MATRIX
		app.engine.max_re = max_re
		app.engine.max_re_split = re_split
		app.bed.set_orientation(Vector3(yaw, pitch, 0.0))

	func enter() -> void:
		app.source.gain = 0.0      # the bed IS the content here
		app.refl.gain = 0.0
		_apply()
		app.bed.set_gain(0.9)
		app.bed.play_clip(preload("res://addons/bw_audio/playground/signals.gd").BED_FILE)

	func key(code: int) -> void:
		match code:
			KEY_SPACE: spin = not spin
			KEY_G: parametric = not parametric
			KEY_B: max_re = not max_re
			KEY_N: re_split = not re_split

	func update(dt: float) -> void:
		if Input.is_key_pressed(KEY_R): pitch = clampf(pitch + 0.9 * dt, -1.5, 1.5)
		if Input.is_key_pressed(KEY_F): pitch = clampf(pitch - 0.9 * dt, -1.5, 1.5)
		# Yaw ACCUMULATES rather than wrapping: a target wrapped by 2*pi would make the
		# engine's ~1 turn/s glide take the long way round.
		if spin:
			yaw += 0.45 * dt
		_apply()

	func draw(d) -> void:
		# Where the field's content sits NOW: the encode bearings through the live
		# orientation, pitch about room right then yaw about room up (the engine's order).
		const COLS := [Color(0.92, 0.47, 0.47), Color(0.47, 0.78, 0.92)]
		var dirs = preload("res://addons/bw_audio/playground/signals.gd").BED_DIRS
		var cy := cos(yaw)
		var sy := sin(yaw)
		var cp := cos(pitch)
		var sp := sin(pitch)
		for i in 2:
			var v: Vector3 = (dirs[i] as Vector3).normalized()
			var t := Vector3(v.x, cp * v.y + sp * v.z, cp * v.z - sp * v.y)
			var r := Vector3(cy * t.x + sy * t.z, t.y, cy * t.z - sy * t.x)
			var p: Vector3 = app.head + r * 2.2
			d.sphere(p, 0.15, COLS[i])
			d.line(app.head, p, Color(COLS[i].r, COLS[i].g, COLS[i].b, 0.6))

	func status() -> Array:
		return [
			["yaw", "%.0f deg" % rad_to_deg(fmod(yaw, TAU))],
			["max-rE", ("band-split" if re_split else "broadband") if max_re else "off"],
		]

	func controls() -> Array:
		return [
			{"kind": "option", "label": "renderer (G)", "items": ["matrix", "parametric"],
				"get": func() -> int: return 1 if parametric else 0,
				"set": func(i: int) -> void: parametric = i == 1},
			{"kind": "option", "label": "max-rE (B / N)",
				"items": ["off", "broadband", "band-split (>700 Hz)"],
				"get": func() -> int: return (2 if re_split else 1) if max_re else 0,
				"set": func(i: int) -> void:
					max_re = i > 0
					re_split = i == 2},
			{"kind": "toggle", "label": "spin (SPACE)",
				"get": func() -> bool: return spin,
				"set": func(v: bool) -> void: spin = v},
			{"kind": "slider", "label": "tilt (R / F)", "min": -1.5, "max": 1.5, "step": 0.02,
				"get": func() -> float: return pitch,
				"set": func(v: float) -> void: pitch = v},
			{"kind": "slider", "label": "yaw", "min": 0.0, "max": TAU, "step": 0.02,
				"get": func() -> float: return fposmod(yaw, TAU),
				"set": func(v: float) -> void: yaw = v},
		]


## ============================ 7. Reverb bed (static room) ============================
## The bed and the room geometry are LOAD-time, so entering or leaving this scene rebuilds
## the engine — a brief audio gap, by design. Transients show the tail best.
class ReverbBed extends Base:
	var wet := 1.0
	var on := true
	var dist := false
	var wall_on := false
	var sweep := false
	var _wall := -1
	var _wall_z := -1.6
	var _wall_t := 0.0
	var _concrete: BwaMaterial

	func _init(a) -> void:
		super(a)
		title = "Reverb bed (static room)"
		help = "G dry/wet  [ ] wet level  V distance->wet  B decoder (rebuilds)  N wall  SPACE sweep"
		_concrete = BwaMaterial.new()
		_concrete.preset = BwaMaterial.PRESET_CONCRETE

	func _place_wall() -> void:
		if _wall >= 0:
			app.engine.scene_set_dynamic_transform(
				_wall, Vector3(0, 1.5, _wall_z), Quaternion.IDENTITY)

	func _apply_wall() -> void:
		if _wall >= 0:
			app.engine.scene_remove_dynamic_mesh(_wall)
			_wall = -1
		if not wall_on:
			app.source.occlusion = false
			return
		var hw := 1.5
		var verts := PackedVector3Array([
			Vector3(-hw, -hw, 0), Vector3(hw, -hw, 0), Vector3(hw, hw, 0),
			Vector3(-hw, -hw, 0), Vector3(hw, hw, 0), Vector3(-hw, hw, 0)])
		_wall = app.engine.scene_add_dynamic_mesh(
			verts, PackedInt32Array([0, 1, 2, 3, 4, 5]), _concrete.get_token(app.engine))
		app.source.occlusion = true
		_place_wall()

	func enter() -> void:
		app.source.gain = app.SRC_GAIN
		app.source.reverb = on
		app.source.reverb_distance = dist
		app.engine.reverb_set_gain(wet)
		# Start behind where the wall sweeps, so the demo occludes out of the box.
		app.source_pos = Vector3(0, app.head.y, -3.0)
		_wall = -1                     # fresh rig; re-add if enabled
		_apply_wall()

	func key(code: int) -> void:
		match code:
			KEY_B:
				# The bed decoder is create-time, so A/Bing it means a rebuild.
				app.rev_decoder = 1 - app.rev_decoder
				app.rebuild_rig(true)
			KEY_G:
				on = not on
				app.source.reverb = on
			KEY_V:
				dist = not dist
				app.source.reverb_distance = dist
			KEY_N:
				wall_on = not wall_on
				_apply_wall()
			KEY_SPACE: sweep = not sweep

	func update(dt: float) -> void:
		if wall_on and sweep:
			_wall_t += dt
			_wall_z = -1.6 + 1.3 * sin(_wall_t * 0.6)
			_place_wall()
		if Input.is_key_pressed(KEY_BRACKETLEFT): wet = maxf(0.0, wet - 0.7 * dt)
		if Input.is_key_pressed(KEY_BRACKETRIGHT): wet = minf(2.0, wet + 0.7 * dt)
		app.engine.reverb_set_gain(wet)
		# Keep the source inside the room, which is floor-based: y runs 0..H.
		var r: Vector3 = app.ROOM
		app.source_pos.x = clampf(app.source_pos.x, -r.x * 0.5 + 0.5, r.x * 0.5 - 0.5)
		app.source_pos.y = clampf(app.source_pos.y, 0.5, r.y - 0.5)
		app.source_pos.z = clampf(app.source_pos.z, -r.z * 0.5 + 0.5, r.z * 0.5 - 0.5)
		app.source.gain = app.SRC_GAIN

	func draw(d) -> void:
		d.room_box(Vector3.ZERO, app.ROOM, Color(0.35, 0.43, 0.59, 0.5))
		d.line(app.head, app.source_pos, GREEN)
		d.sphere(app.source_pos, 0.18, RED)
		if wall_on:
			d.quad(Vector3(0, 1.5, _wall_z), Vector3(1.5, 0, 0), Vector3(0, 1.5, 0),
				Color(0.86, 0.7, 0.59, 0.9))

	func status() -> Array:
		return [
			["wet", "%.2f" % wet],
			["occluder", ("sweeping" if sweep else "static") if wall_on else "off"],
			["attached", "yes" if _wall >= 0 else "no"],
		]

	func controls() -> Array:
		return [
			{"kind": "toggle", "label": "reverb send (G)",
				"get": func() -> bool: return on,
				"set": func(v: bool) -> void:
					on = v
					app.source.reverb = v},
			{"kind": "slider", "label": "wet ([ / ])", "min": 0.0, "max": 2.0, "step": 0.02,
				"get": func() -> float: return wet,
				"set": func(v: float) -> void: wet = v},
			{"kind": "toggle", "label": "distance->wet (V)",
				"get": func() -> bool: return dist,
				"set": func(v: bool) -> void:
					dist = v
					app.source.reverb_distance = v},
			# The bed decoder is create-time, so picking one REBUILDS the engine — a brief
			# audio gap, and the reason this is a dropdown that warns rather than a toggle.
			{"kind": "option", "label": "decoder (B, rebuilds)", "items": ["AllRAD", "EPAD"],
				"get": func() -> int: return app.rev_decoder,
				"set": func(i: int) -> void:
					if i != app.rev_decoder:
						app.rev_decoder = i
						app.rebuild_rig(1)},
			{"kind": "toggle", "label": "occluder (N)",
				"get": func() -> bool: return wall_on,
				"set": func(v: bool) -> void:
					wall_on = v
					_apply_wall()},
			{"kind": "toggle", "label": "sweep it (SPACE)",
				"get": func() -> bool: return sweep,
				"set": func(v: bool) -> void: sweep = v},
		]


## ============================ 8. Underwater (medium boundary) ============================
## The api.md "listener submerges" recipe, live and phonon-free. SPACE dives: a source across
## the surface gets the interface loss + the water's transmission EQ (manual occlusion) and
## goes diffuse (spread), the FDN retunes LIVE (the tail keeps ringing, only its slope
## changes) and the speed of sound glides to the medium's — Doppler is what makes that
## audible. With BOTH ends under, the surface bounce renders off the rig's PRESSURE-RELEASE
## plane: push the source up toward the surface and the inverted image thins it out (the
## Lloyd's-mirror comb; broadband signals show it best). The FDN is load-time (the plane
## itself is live-safe), so this scene runs its own rig config — crossing its boundary
## rebuilds, like scene 7.
class Underwater extends Base:
	var under := false
	var lloyd := true
	var doppler := true
	var _crossed := -1        # last pushed cross-surface state (-1 = force a re-push)
	var _er_on := -1          # last pushed early-reflection enable (-1 = force)

	func _init(a) -> void:
		super(a)
		title = "Underwater (medium boundary)"
		help = "SPACE dive/surface  L surface bounce  V doppler  R/F source through the surface"

	## The room-wide half: the medium the LISTENER is in. fdn_set_decay is live — the tail
	## keeps ringing through the retune, which is the point.
	func _apply_medium() -> void:
		if under:
			app.engine.fdn_set_decay(3.0, 0.3, 800.0)   # hard boundaries: long LF tail, dead HF
			app.engine.reverb_set_gain(1.5)
			app.engine.set_speed_of_sound(1480.0)       # every delay glides to the medium's c
		else:
			app.engine.fdn_set_decay(1.2, 0.7, 2000.0)  # the air defaults
			app.engine.reverb_set_gain(0.6)
			app.engine.set_speed_of_sound(343.0)
		_crossed = -1                                    # re-derive the per-source half
		_er_on = -1

	## The per-source half: does the source->listener path cross the surface?
	func _apply_source() -> void:
		var above: bool = app.source_pos.y > app.WATER_Y
		var crossed := 1 if above == under else 0        # source side != listener side
		if crossed != _crossed:
			_crossed = crossed
			if crossed == 1:                             # the -30 dB interface loss, tilted by the muffle
				# the tilt is RELATIVE to the level (the engine multiplies them), so its low band is
				# pinned at 1: the interface loss is charged once and the vector only says how much
				# MORE the mid and high bands lose. Net -30 / -44 / -60 dB.
				app.source.set_occlusion_manual_bands(0.03, Vector3(1.0, 0.2, 0.033))
				app.source.spread = 0.8                  # localization collapses across the boundary
			else:
				app.source.set_occlusion_manual(1.0)
				app.source.spread = 0.0
		# The surface bounce renders only with BOTH ends under it: the plane mirrors from
		# either side, but a cross-boundary image has no physical path.
		var er := 1 if (lloyd and under and not above) else 0
		if er != _er_on:
			_er_on = er
			app.source.early_reflections = er == 1

	## What the occlusion readback depends on, for a selftest failure message. A bare factor cannot
	## say WHY it was wrong: the engine returns a clear 1.0 both when nothing was pushed and when the
	## sim republished over the manual value, and those want opposite fixes.
	func debug_state() -> String:
		return "under=%s above=%s crossed=%d sim=%s" % [
			under, app.source_pos.y > app.WATER_Y, _crossed, app.source.occlusion]

	func set_submerged(v: bool) -> void:
		under = v
		_apply_medium()

	func enter() -> void:
		app.source.gain = app.SRC_GAIN
		app.source.reverb = true             # the FDN renders whichever medium's tail
		app.source.doppler = doppler
		app.source_pos = Vector3(0.0, app.WATER_Y + 0.8, -2.5)   # ABOVE: diving muffles it
		under = false
		_apply_medium()

	func key(code: int) -> void:
		match code:
			KEY_SPACE:
				set_submerged(not under)
			KEY_L:
				lloyd = not lloyd
				_er_on = -1
			KEY_V:
				doppler = not doppler
				app.source.doppler = doppler

	func update(_dt: float) -> void:
		app.source_pos.x = clampf(app.source_pos.x, -4.0, 4.0)
		app.source_pos.y = clampf(app.source_pos.y, 0.4, app.WATER_Y + 2.0)
		app.source_pos.z = clampf(app.source_pos.z, -4.0, 4.0)
		app.source.gain = app.SRC_GAIN
		_apply_source()

	func draw(d) -> void:
		# The surface: a translucent sheet, denser when the listener is under it.
		d.quad(Vector3(0, app.WATER_Y, 0), Vector3(4.5, 0, 0), Vector3(0, 0, 4.5),
			Color(0.3, 0.55, 0.8, 0.3 if under else 0.15))
		var above: bool = app.source_pos.y > app.WATER_Y
		d.sphere(app.source_pos, 0.18,
			Color(0.92, 0.78, 0.35) if above else Color(0.35, 0.7, 0.92))
		d.line(app.head, app.source_pos,
			Color(0.38, 0.5, 0.66) if _crossed == 1 else GREEN)
		if _er_on == 1:
			# The Lloyd's mirror: the inverted image above the surface.
			var img := Vector3(
				app.source_pos.x, 2.0 * app.WATER_Y - app.source_pos.y, app.source_pos.z)
			d.sphere(img, 0.13, Color(0.47, 0.74, 0.94, 0.5))
			d.line(img, app.head, Color(0.47, 0.74, 0.94, 0.35))

	func status() -> Array:
		var above: bool = app.source_pos.y > app.WATER_Y
		return [
			["listener", "UNDER the surface" if under else "in air"],
			["source", "above the surface" if above else "below"],
			["path", "CROSSES (muffled)" if _crossed == 1 else "clear"],
			["surface bounce", "on (Lloyd's mirror)" if _er_on == 1 else "off"],
		]

	func controls() -> Array:
		return [
			{"kind": "toggle", "label": "submerged (SPACE)",
				"get": func() -> bool: return under,
				"set": func(v: bool) -> void: set_submerged(v)},
			{"kind": "toggle", "label": "surface bounce (L)",
				"get": func() -> bool: return lloyd,
				"set": func(v: bool) -> void:
					lloyd = v
					_er_on = -1},
			{"kind": "toggle", "label": "doppler (V)",
				"get": func() -> bool: return doppler,
				"set": func(v: bool) -> void:
					doppler = v
					app.source.doppler = v},
			{"kind": "slider", "label": "source depth (R/F)",
				"min": 0.4, "max": app.WATER_Y + 2.0, "step": 0.05,
				"get": func() -> float: return app.source_pos.y,
				"set": func(v: float) -> void: app.source_pos.y = v},
		]
