## The playground's control panel and HUD.
##
## Scenes DECLARE their controls (see each scene's controls()) and this builds the widgets —
## dropdowns, toggles, sliders — rather than every scene hand-rolling UI. The keyboard
## shortcuts still work and drive the same setters, so the two input paths cannot disagree
## about state the way two independent copies would; widgets read their value back from the
## scene each frame, so pressing a key visibly moves the corresponding control.
extends Control

const Signals := preload("res://addons/bw_audio/playground/signals.gd")

var app: Node3D

var _box: PanelContainer
var _scene_pick: OptionButton
var _signal_pick: OptionButton
var _render_pick: OptionButton
var _help: Label
var _controls_box: VBoxContainer
var _status: Label
var _meters: Control
var _widgets: Array = []      # [{spec, node}] for the current scene
var _built_for := -1


func _ready() -> void:
	app = get_parent()
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE   # the 3D view keeps the rest of the window

	_box = PanelContainer.new()
	_box.position = Vector2(16, 16)
	_box.custom_minimum_size = Vector2(380, 0)
	_box.mouse_filter = Control.MOUSE_FILTER_STOP   # ...but the panel itself takes clicks
	add_child(_box)

	var margin := MarginContainer.new()
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 10)
	_box.add_child(margin)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 6)
	margin.add_child(col)

	_scene_pick = OptionButton.new()
	for i in app.scenes.size():
		_scene_pick.add_item("%d. %s" % [i + 1, app.scenes[i].title])
	_scene_pick.item_selected.connect(func(i: int) -> void: app.switch_scene(i))
	col.add_child(_row("scene", _scene_pick))

	_signal_pick = OptionButton.new()
	for n in Signals.NAMES:
		_signal_pick.add_item(n)
	_signal_pick.item_selected.connect(func(i: int) -> void: app.set_signal(i))
	col.add_child(_row("signal", _signal_pick))

	_render_pick = OptionButton.new()
	_render_pick.add_item("cave_sim (array audition)")
	_render_pick.add_item("binaural (direct)")
	_render_pick.add_item("cave (the array itself)")
	_render_pick.item_selected.connect(func(i: int) -> void: app.set_render_pick(i))
	_render_pick.tooltip_text = (
		"What renders - the headphone pair A/Bs by ear on the same scene.\n"
		+ "cave_sim: the array render through virtual speakers, DBAP artifacts included;\n"
		+ "the meters show exactly what lights the speakers.\n"
		+ "binaural: the direct per-source render (per-voice HRTF with the Steam build);\n"
		+ "point sources and beds bypass the speaker bus, so quiet meters there are correct.\n"
		+ "cave: the ARRAY ITSELF over 26-ch ASIO - the by-ear harness on the rig machine\n"
		+ "(with no such device the null sink runs visual-only).\n"
		+ "Create-time: switching rebuilds the rig (brief gap).")
	col.add_child(_row("render", _render_pick))

	col.add_child(HSeparator.new())

	_help = Label.new()
	_help.add_theme_font_size_override("font_size", 11)
	_help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_help.modulate = Color(1, 1, 1, 0.6)
	col.add_child(_help)

	_controls_box = VBoxContainer.new()
	_controls_box.add_theme_constant_override("separation", 4)
	col.add_child(_controls_box)

	col.add_child(HSeparator.new())

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 12)
	col.add_child(_status)

	_meters = Control.new()
	_meters.custom_minimum_size = Vector2(360, 46)
	_meters.draw.connect(_draw_meters)
	col.add_child(_meters)


## True while the cursor is over the panel, so the camera does not orbit when you are
## dragging a slider.
func is_mouse_over() -> bool:
	return _box and _box.get_global_rect().has_point(get_global_mouse_position())


func _row(label: String, widget: Control) -> HBoxContainer:
	var h := HBoxContainer.new()
	var l := Label.new()
	l.text = label
	l.custom_minimum_size = Vector2(120, 0)
	l.add_theme_font_size_override("font_size", 12)
	h.add_child(l)
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	h.add_child(widget)
	return h


func _rebuild_controls() -> void:
	for c in _controls_box.get_children():
		c.queue_free()
	_widgets.clear()

	var scene = app.scenes[app.cur_scene]
	for spec in scene.controls():
		var w: Control
		match spec.get("kind", ""):
			"option":
				var o := OptionButton.new()
				for item in spec["items"]:
					o.add_item(item)
				o.item_selected.connect(func(i: int) -> void: spec["set"].call(i))
				w = o
			"toggle":
				var b := CheckBox.new()
				b.toggled.connect(func(v: bool) -> void: spec["set"].call(v))
				w = b
			"slider":
				var s := HSlider.new()
				s.min_value = spec.get("min", 0.0)
				s.max_value = spec.get("max", 1.0)
				s.step = spec.get("step", 0.01)
				s.value_changed.connect(func(v: float) -> void: spec["set"].call(v))
				w = s
			"button":
				var b := Button.new()
				b.text = spec.get("text", spec["label"])
				b.pressed.connect(func() -> void: spec["press"].call())
				w = b
			_:
				continue
		_controls_box.add_child(_row(spec["label"], w))
		_widgets.append({"spec": spec, "node": w})

	_built_for = app.cur_scene
	_help.text = scene.help


func _process(_dt: float) -> void:
	if not app or not app.engine or not app.engine.is_running():
		return
	if _built_for != app.cur_scene:
		_rebuild_controls()

	if _scene_pick.selected != app.cur_scene:
		_scene_pick.selected = app.cur_scene       # TAB moved it
	if _signal_pick.selected != app.cur_sig:
		_signal_pick.selected = app.cur_sig
	if _render_pick.selected != app.render_pick:
		_render_pick.selected = app.render_pick    # the selftest (or a script) switched it

	# Pull each widget's value back from the scene, so a keyboard shortcut visibly moves the
	# matching control. Skipped while a widget has focus, or dragging a slider would fight
	# the value being written back underneath it.
	for e in _widgets:
		var node: Control = e["node"]
		if node.has_focus():
			continue
		var spec: Dictionary = e["spec"]
		match spec.get("kind", ""):
			"option": (node as OptionButton).selected = spec["get"].call()
			"toggle": (node as CheckBox).button_pressed = spec["get"].call()
			"slider": (node as HSlider).set_value_no_signal(spec["get"].call())

	var scene = app.scenes[app.cur_scene]
	var lines: Array[String] = []
	for row in scene.status():
		lines.append("%-16s %s" % [row[0], row[1]])
	lines.append("")
	lines.append("%-16s %s" % ["backend", app.backend])
	lines.append("%-16s %d" % ["voices", app.engine.get_active_voices()])
	# A silent backend is a legitimate state (visual-only mode), so say so rather than
	# leaving someone to wonder why a working scene makes no sound.
	if not app.backend.begins_with("asio"):
		lines.append("no ASIO device - rendering silently (visual-only)")
	_status.text = "\n".join(lines)

	_meters.queue_redraw()


func _draw_meters() -> void:
	var peaks: PackedFloat32Array = app.engine.get_bus_levels()
	if peaks.is_empty():
		return
	var w: float = _meters.size.x / peaks.size()
	for i in peaks.size():
		# Same 60 dB window the speaker gizmos use, so the strip and the 3D view agree.
		var db: float = 20.0 * log(maxf(peaks[i], 1e-6)) / log(10.0)
		var t := clampf(1.0 + db / 60.0, 0.0, 1.0)
		var h := 4.0 + t * 38.0
		var col := Color(0.25, 0.28, 0.35).lerp(Color(1.0, 0.75, 0.2), t)
		if i == app.highlight_spk:
			col = Color(0.47, 0.92, 0.59)
		_meters.draw_rect(Rect2(i * w, 44.0 - h, w - 1.0, h), col)
